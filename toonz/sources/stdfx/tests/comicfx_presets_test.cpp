// The presets shipped for the comic and watercolour FX: each one loads into
// its FX through the same path as the Add FX menu, names every parameter of
// the FX, changes something, and the generators draw with it.

#include "stdfx.h"
#include "tfx.h"
#include "tparamcontainer.h"
#include "tpixelutils.h"
#include "trasterfx.h"
#include "trenderer.h"
#include "tthread.h"
#include "tstream.h"

#include <QGuiApplication>
#include <QDir>
#include <QFile>
#include <QXmlStreamReader>

#include <iostream>
#include <map>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

DV_IMPORT_API void initStdFx();

namespace {
void require(bool value, const std::string &message) {
  if (!value) throw std::runtime_error(message);
}

// The parameter names in a preset file, in order
std::vector<std::string> presetParams(const QString &path) {
  QFile file(path);
  require(file.open(QIODevice::ReadOnly), "Cannot read " + path.toStdString());
  QXmlStreamReader xml(&file);
  std::vector<std::string> names;
  while (!xml.atEnd()) {
    xml.readNext();
    // dvpreset > params > name
    if (xml.isStartElement() && xml.name() != QLatin1String("dvpreset") &&
        xml.name() != QLatin1String("params")) {
      names.push_back(xml.name().toString().toStdString());
      xml.skipCurrentElement();
    }
  }
  require(!xml.hasError(), path.toStdString() + ": " +
                               xml.errorString().toStdString());
  return names;
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
    const std::vector<std::string> listed = presetParams(folder.filePath(file));
    std::set<std::string> seen;
    for (const std::string &param : listed) {
      require(params.count(param), name + " names an unknown parameter " + param);
      require(seen.insert(param).second, name + " names " + param + " twice");
    }
    for (const std::string &param : params)
      require(seen.count(param), name + " does not set " + param);

    TFxP fx = TFx::create("STD_" + id);
    TIStream stream(TFilePath(folder.filePath(file).toStdWString()));
    fx->loadPreset(stream);
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
