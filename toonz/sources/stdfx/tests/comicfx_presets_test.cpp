// The presets shipped for the comic and watercolour FX: each one loads into
// its FX through the same path as the Add FX menu, names every parameter of
// the FX, changes something, and the generators draw with it.

#include "tdoubleparam.h"
#include "tfx.h"
#include "tnotanimatableparam.h"
#include "tparamcontainer.h"
#include "tparamset.h"
#include "tpixelutils.h"
#include "trasterfx.h"
#include "trenderer.h"
#include "tthread.h"
#include "tstream.h"

#include <QGuiApplication>
#include <QDir>
#include <QStringList>
#include <QFile>
#include <QXmlStreamReader>

#include <cmath>
#include <iostream>
#include <map>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

// Declared here, not through stdfx.h, which exports it to the library
DV_IMPORT_API void initStdFx();

namespace {
void require(bool value, const std::string &message) {
  if (!value) throw std::runtime_error(message);
}

// A preset file's parameters, in order, each with the text of its leaves:
// "default" for a number, "x/default" for a point, "red/default" for a
// colour, "" for a switch
using Leaves = std::map<std::string, QString>;
std::vector<std::pair<std::string, Leaves>> presetParams(const QString &path) {
  QFile file(path);
  require(file.open(QIODevice::ReadOnly), "Cannot read " + path.toStdString());
  QXmlStreamReader xml(&file);
  std::vector<std::pair<std::string, Leaves>> params;
  std::vector<std::string> stack;  // below dvpreset > params
  int depth = 0;
  while (!xml.atEnd()) {
    xml.readNext();
    if (xml.isStartElement()) {
      ++depth;
      if (depth == 3) params.push_back({xml.name().toString().toStdString(), {}});
      if (depth > 3) stack.push_back(xml.name().toString().toStdString());
    } else if (xml.isEndElement()) {
      if (depth > 3) stack.pop_back();
      --depth;
    } else if (xml.isCharacters() && !xml.isWhitespace() && depth >= 3) {
      std::string leaf;
      for (const std::string &part : stack) leaf += (leaf.empty() ? "" : "/") + part;
      params.back().second[leaf] = xml.text().toString().trimmed();
    }
  }
  require(!xml.hasError(), path.toStdString() + ": " +
                               xml.errorString().toStdString());
  return params;
}

double number(const Leaves &leaves, const std::string &leaf,
              const std::string &where) {
  const auto it = leaves.find(leaf);
  require(it != leaves.end(), where + " has no " + leaf);
  bool ok             = false;
  const double value  = it->second.toDouble(&ok);
  require(ok, where + ": " + leaf + " is not a number");
  return value;
}

// The loaded parameter holds the file's value, and the value is in range
void checkNumber(const TDoubleParamP &param, double expected,
                 const std::string &where) {
  require(std::abs(param->getDefaultValue() - expected) < 1e-9,
          where + " did not load its value");
  double low = 0, high = 0, step = 0;
  if (param->getValueRange(low, high, step))
    require(expected >= low - 1e-9 && expected <= high + 1e-9,
            where + " is out of range");
}

void checkLoaded(TFx *fx, const std::string &name, const Leaves &leaves,
                 const std::string &where) {
  TParamP param = fx->getParams()->getParam(name);
  if (TDoubleParamP d = param) {
    checkNumber(d, number(leaves, "default", where), where);
  } else if (TPointParamP p = param) {
    checkNumber(p->getX(), number(leaves, "x/default", where), where + " x");
    checkNumber(p->getY(), number(leaves, "y/default", where), where + " y");
  } else if (TPixelParamP c = param) {
    checkNumber(c->getRed(), number(leaves, "red/default", where), where);
    checkNumber(c->getGreen(), number(leaves, "green/default", where), where);
    checkNumber(c->getBlue(), number(leaves, "blue/default", where), where);
    checkNumber(c->getMatte(), number(leaves, "matte/default", where), where);
  } else if (TBoolParamP b = param) {
    const QStringList pair = leaves.count("") ? leaves.at("").split(' ', Qt::SkipEmptyParts)
                                              : QStringList();
    require(pair.size() == 2, where + " is not a default and value pair");
    require(b->getDefaultValue() == (pair[0] == "1") &&
                b->getValue() == (pair[1] == "1"),
            where + " did not load its value");
  } else {
    throw std::runtime_error(where + " has a parameter type this test "
                             "does not know");
  }
}

// Every pixel's alpha, summed, over a tile that covers +-800 scene units
double drawn(TFx *fx) {
  auto *raster = dynamic_cast<TRasterFx *>(fx);
  require(raster, "The FX is not a raster FX");
  TRenderer renderer(1);
  const unsigned long id = TRenderer::buildRenderId();
  renderer.install(id);
  renderer.declareRenderStart(id);
  renderer.declareFrameStart(0);
  TRenderSettings settings;
  settings.m_bpp       = 32;
  settings.m_cameraBox = TRectD(-800, -800, 800, 800);
  settings.m_affine = TScale(0.25);
  TRaster32P output(400, 400);
  output->clear();
  TTile tile(output, TPointD(-200, -200));
  raster->compute(tile, 0, settings);
  renderer.declareFrameEnd(0);
  renderer.declareRenderEnd(id);
  renderer.uninstall();
  double sum = 0;
  for (int y = 0; y < 400; ++y)
    for (int x = 0; x < 400; ++x) sum += output->pixels(y)[x].m / 255.0;
  return sum;
}

void checkFx(const QDir &root, const std::string &id, bool generator) {
  const QDir folder(root.filePath(QString::fromStdString("STD_" + id)));
  const QStringList files = folder.entryList({"*.fx"}, QDir::Files);
  require(files.size() >= 3, "STD_" + id + " has fewer than three presets");
  TFxP plain = TFx::create("STD_" + id);
  require(plain, "Cannot create STD_" + id);
  std::set<std::string> params;
  for (int i = 0; i < plain->getParams()->getParamCount(); ++i)
    params.insert(plain->getParams()->getParamName(i));
  const std::string plainAlias =
      dynamic_cast<TRasterFx *>(plain.getPointer())->getAlias(0, TRenderSettings());

  for (const QString &file : files) {
    const std::string name = "STD_" + id + "/" + file.toStdString();
    const auto listed = presetParams(folder.filePath(file));
    std::set<std::string> seen;
    for (const auto &param : listed) {
      require(params.count(param.first),
              name + " names an unknown parameter " + param.first);
      require(seen.insert(param.first).second,
              name + " names " + param.first + " twice");
    }
    for (const std::string &param : params)
      require(seen.count(param), name + " does not set " + param);

    TFxP fx = TFx::create("STD_" + id);
    TIStream stream(TFilePath(folder.filePath(file).toStdWString()));
    fx->loadPreset(stream);
    // Loading skips a parameter that fails and never clamps: check each
    for (const auto &param : listed)
      checkLoaded(fx.getPointer(), param.first, param.second,
                  name + " " + param.first);
    const std::string alias =
        dynamic_cast<TRasterFx *>(fx.getPointer())->getAlias(0, TRenderSettings());
    require(alias != plainAlias, name + " changes nothing");
    if (generator) require(drawn(fx.getPointer()) > 1, name + " draws nothing");
  }
}
}  // namespace

int main(int argc, char **argv) {
  // Rendering makes offscreen OpenGL surfaces, which need a GUI application
  QGuiApplication app(argc, argv);
  TThread::init();
  TRenderer::initialize();
  int result = 1;
  try {
    require(argc == 2, "Expected the preset folder");
    initStdFx();
    const QDir root(QString::fromLocal8Bit(argv[1]));
    checkFx(root, "motionLinesFx", true);
    checkFx(root, "burstLinesFx", true);
    checkFx(root, "dotScreenFx", true);
    checkFx(root, "watercolourFx", false);
    checkFx(root, "roughStrokeFx", false);
    std::cout << "PASS: every comic and watercolour preset loads, sets every "
                 "parameter, changes the FX and draws\n";
    result = 0;
  } catch (const TException &error) {
    std::cerr << QString::fromStdWString(error.getMessage()).toStdString()
              << '\n';
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
  }
  TThread::shutdown();
  return result;
}
