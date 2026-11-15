#include "texternfx.h"
#include "texception.h"
#include "ttile.h"
#include "tnzimage.h"
#include "tthread.h"
#include "trenderer.h"
#include "tstream.h"

#include <QColor>
#include <QDir>
#include <QFile>
#include <QGuiApplication>
#include <QImage>
#include <QTemporaryDir>

#include <iostream>
#include <stdexcept>
#include <string>
#include <utility>

#ifdef _WIN32
#include <windows.h>
#endif

namespace {
void require(bool condition, const char* message) {
  if (!condition) throw std::runtime_error(message);
}

void compute(const TFilePath& executable, const std::string& arguments) {
  TExternalProgramFx effect;
  effect.setExecutable(executable, arguments);
  effect.addPort("out", "png", false);
  TRaster32P raster(1, 1);
  TTile tile(raster);
  effect.doCompute(tile, 0, TRenderSettings());
}

QByteArray result(const QString& name) {
  QFile file(name);
  require(file.open(QIODevice::ReadOnly),
          "The external effect did not receive its command arguments");
  return file.readAll();
}

void expectFailure(const TFilePath& executable, const std::string& arguments,
                   const QString& explanation) {
  bool rejected = false;
  try {
    compute(executable, arguments);
  } catch (const TException& error) {
    rejected =
        QString::fromStdWString(error.getMessage()).contains(explanation);
  }
  require(rejected, "Failed external effect had no actionable error");
}

int writeGrayscaleTga(const QString& path) {
  QByteArray image(18, '\0');
  image[2]  = 3;
  image[12] = 3;
  image[14] = 2;
  image[16] = 8;
  image.append(6, char(127));
  QFile file(path);
  require(file.open(QIODevice::WriteOnly),
          "Cannot create grayscale TGA output fixture");
  require(file.write(image) == image.size(),
          "Cannot finish grayscale TGA output fixture");
  return 0;
}

QImage renderedImage(const QString& mode) {
  const int width = mode == "small" ? 2 : mode == "large" ? 4 : 3;
  QImage output(
      width, 2,
      mode == "gray" ? QImage::Format_Grayscale8 : QImage::Format_ARGB32);
  output.fill(mode == "gray" ? 127 : qRgba(255, 0, 0, 255));
  if (mode == "pixels") {
    output.setPixel(1, 0, qRgba(0, 255, 0, 128));
    output.setPixel(2, 0, qRgba(0, 0, 0, 0));
    output.setPixel(0, 1, qRgba(0, 255, 255, 255));
    output.setPixel(1, 1, qRgba(255, 255, 0, 255));
    output.setPixel(2, 1, qRgba(255, 0, 255, 255));
  }
  return output;
}

int renderWorker(const QStringList& arguments) {
  if (arguments.size() != 5) return 20;
  const QImage input(arguments[2]);
  require(input.size() == QSize(3, 2),
          "External effect input tile changed size");
  for (int y = 0; y < input.height(); ++y)
    for (int x = 0; x < input.width(); ++x)
      require(input.pixelColor(x, y) == QColor(Qt::blue),
              "External effect input pixels were not rendered");
  const QString mode = arguments[4];
  if (mode == "gray-tga") return writeGrayscaleTga(arguments[3]);
  const QImage output = renderedImage(mode);
  require(output.save(arguments[3], "PNG"),
          "Cannot save rendered output fixture");
  return 0;
}

class BlueSource final : public TRasterFx {
  int* m_destroyed;

public:
  explicit BlueSource(int* destroyed = nullptr) : m_destroyed(destroyed) {}
  ~BlueSource() override {
    if (m_destroyed) ++*m_destroyed;
  }
  const TPersistDeclaration* getDeclaration() const override { return nullptr; }
  std::string getPluginId() const override { return {}; }
  bool canHandle(const TRenderSettings&, double) override { return true; }
  std::string getAlias(double, const TRenderSettings&) const override {
    return "external-output-blue-source";
  }
  bool doGetBBox(double, TRectD& box, const TRenderSettings&) override {
    box = TRectD(11, -7, 14, -5);
    return true;
  }
  void doCompute(TTile& tile, double, const TRenderSettings&) override {
    TRaster32P raster = tile.getRaster();
    require(bool(raster),
            "External effect input requested an unsupported pixel type");
    raster->fill(TPixel32::Blue);
  }
};

void inputPortOwnership() {
  int destroyed = 0;
  {
    TRasterFxP source(new BlueSource(&destroyed));
    const int references = source->getRefCount();
    for (int cycle = 0; cycle < 16; ++cycle) {
      {
        TExternalProgramFx effect;
        effect.addPort("first", "png", true);
        effect.addPort("second", "png", true);
        effect.addPort("out", "png", false);
        effect.getInputPort(0)->setFx(source.getPointer());
        effect.getInputPort(1)->setFx(source.getPointer());
        require(source->getRefCount() == references + 2,
                "External effect input ports did not retain their source");
        effect.addPort("first", "png", true);
        require(effect.getInputPortCount() == 2,
                "Duplicate external input changed the registered port count");
        if (cycle % 2) effect.getInputPort(1)->setFx(nullptr);
      }
      require(source->getRefCount() == references && destroyed == 0,
              "Destroyed external effect retained its upstream source");
    }
  }
  require(destroyed == 1,
          "External input ownership leaked or destroyed its source twice");
}

void failedLoadPortOwnership(const QString& directory) {
  const TFilePath path(QDir(directory).filePath("failed-port-load.xml"));
  {
    TOStream stream(path);
    stream.openChild("ports");
    stream.openCloseChild("port", {{"name", "in"}, {"ext", "png"}});
    stream.closeChild();
    stream.child("unexpected") << std::string("invalid");
  }
  int destroyed = 0;
  for (int cycle = 0; cycle < 8; ++cycle) {
    TRasterFxP source(new BlueSource(&destroyed));
    const int references = source->getRefCount();
    {
      TExternalProgramFx effect;
      TIStream stream(path);
      bool failed = false;
      try {
        effect.loadData(stream);
      } catch (const TException&) {
        failed = true;
      }
      require(failed && effect.getInputPortCount() == 1,
              "Malformed external effect did not exercise a loaded input");
      effect.getInputPort(0)->setFx(source.getPointer());
    }
    require(source->getRefCount() == references,
            "Failed external effect load retained an input source");
  }
  require(destroyed == 8, "Failed external effect loads leaked sources");
}

void publicCompute(TRasterFx& effect, TTile& tile) {
  TRenderer renderer(1);
  renderer.enablePrecomputing(false);
  const auto id = TRenderer::buildRenderId();
  renderer.install(id);
  renderer.declareRenderStart(id);
  renderer.declareFrameStart(0);
  try {
    effect.compute(tile, 0, TRenderSettings());
  } catch (...) {
    renderer.declareFrameEnd(0);
    renderer.declareRenderEnd(id);
    renderer.uninstall();
    throw;
  }
  renderer.declareFrameEnd(0);
  renderer.declareRenderEnd(id);
  renderer.uninstall();
}

std::string pixelValue(const TPixel32& pixel) {
  return "(" + std::to_string(pixel.r) + "," + std::to_string(pixel.g) + "," +
         std::to_string(pixel.b) + "," + std::to_string(pixel.m) + ")";
}

void checkRenderedPixels(const TRaster32P& parent, const std::string& mode,
                         bool success) {
  const TPixel32 expected[2][3] = {
      {TPixel32(0, 255, 255), TPixel32(255, 255, 0), TPixel32(255, 0, 255)},
      {TPixel32::Red, TPixel32(0, 128, 0, 128), TPixel32::Transparent}};
  for (int y = 0; y < parent->getLy(); ++y) {
    for (int x = 0; x < parent->getLx(); ++x) {
      const bool inside = x >= 1 && x <= 3 && y >= 1 && y <= 2;
      TPixel32 pixel    = TPixel32::Green;
      if (inside) {
        pixel = success ? expected[y - 1][x - 1] : TPixel32::Blue;
        if (mode == "gray") pixel = TPixel32(127, 127, 127);
      }
      const TPixel32 actual = parent->pixels(y)[x];
      if (actual != pixel)
        throw std::runtime_error(
            "External output mode " + mode + " changed pixel (" +
            std::to_string(x) + "," + std::to_string(y) + "): expected RGBA " +
            pixelValue(pixel) + ", received " + pixelValue(actual));
    }
  }
}

void publicRenderingCase(const QString& executable, const std::string& mode) {
  auto* effect = new TExternalProgramFx();
  TRasterFxP owner(effect);
  effect->setExecutable(TFilePath(executable),
                        std::string("--render-fixture $in $out ") + mode);
  effect->addPort("in", "png", true);
  effect->addPort("out", mode == "gray-tga" ? "tga" : "png", false);
  effect->getInputPort(0)->setFx(new BlueSource());
  TRaster32P parent(5, 4);
  parent->fill(TPixel32::Green);
  TRaster32P destination = parent->extract(1, 1, 3, 2);
  TTile tile(destination, TPointD(11, -7));
  const std::string expectedError =
      mode == "gray-tga" ? "32-bit RGBA" : "different dimensions";
  const bool success = mode == "pixels" || mode == "gray";
  bool failed        = false;
  std::string failure;
  try {
    publicCompute(*effect, tile);
  } catch (const TException& error) {
    failed  = true;
    failure = QString::fromStdWString(error.getMessage()).toStdString();
  }
  if (success && failed)
    throw std::runtime_error("External output mode " + mode + ": " + failure);
  if (!success && failure.find(expectedError) == std::string::npos)
    throw std::runtime_error(
        "External output mode " + mode +
        " expected rejection containing: " + expectedError +
        "; received: " + (failure.empty() ? "no exception" : failure));
  require(tile.getRaster()->getRawData() == destination->getRawData() &&
              tile.getRaster()->getSize() == destination->getSize() &&
              tile.m_pos == TPointD(11, -7),
          "External output replaced destination storage or tile coordinates");
  checkRenderedPixels(parent, mode, success);
}

void publicRendering(const QString& executable) {
  bool passed = true;
  for (const std::string mode :
       {"pixels", "small", "large", "gray", "gray-tga"}) {
    try {
      publicRenderingCase(executable, mode);
    } catch (const std::exception& error) {
      passed = false;
      std::cerr << "External rendering case " << mode << ": " << error.what()
                << '\n';
    }
  }
  require(passed,
          "External effect rendering cases failed; see mode diagnostics");
}

int writeWorker(const QStringList& arguments, const QByteArray& executable) {
  const bool identity = arguments[1] == "--identity-fixture";
  if (arguments.size() != (identity ? 4 : 5) ||
      (!identity && arguments[1] != "--write-fixture"))
    return 20;
  QFile output(arguments[2]);
  const QByteArray bytes = identity ? executable : arguments[3].toUtf8();
  require(output.open(QIODevice::WriteOnly), "Cannot write worker fixture");
  require(output.write(bytes) == bytes.size(), "Cannot finish worker fixture");
  QImage image(1, 1, QImage::Format_ARGB32);
  image.fill(Qt::red);
  require(image.save(arguments[identity ? 3 : 4], "PNG"),
          "Cannot save external-effect output");
  return 0;
}

int worker(const QStringList& arguments, const QByteArray& executable) {
  if (arguments[1] == "--render-fixture") return renderWorker(arguments);
  if (arguments[1] == "--fail-fixture") return 7;
  if (arguments[1] == "--missing-fixture") return 0;
  if (arguments.size() == 3 && arguments[1] == "--corrupt-fixture") {
    QFile output(arguments[2]);
    require(output.open(QIODevice::WriteOnly), "Cannot write corrupt output");
    require(output.write("invalid PNG") == 11, "Cannot finish corrupt output");
    return 0;
  }
  return writeWorker(arguments, executable);
}

void executableIdentity(const QString& executable, const QString& directory) {
  QString selectedExecutable = executable;
#ifndef _WIN32
  selectedExecutable = QDir(directory).filePath("external effect's $tool");
  require(QFile::link(executable, selectedExecutable),
          "Cannot create the external executable path fixture");
#endif
  const QString resultPath =
      QDir(directory).filePath("executable identity.txt");
  compute(TFilePath(selectedExecutable),
          "--identity-fixture \"" + resultPath.toStdString() + "\" $out");
  require(result(resultPath) == QFile::encodeName(selectedExecutable),
          "External effect changed the selected executable argv[0]");
}

#ifdef _WIN32
void windowsProcesses(const QString& executable, const QString& directory) {
  const QString resultPath    = QDir(directory).filePath("argument result.txt");
  const std::string arguments = "--write-fixture \"" +
                                resultPath.toStdString() +
                                "\" \"argument with spaces\" $out";
  compute(TFilePath(executable), arguments);
  require(result(resultPath) == "argument with spaces",
          "The external effect changed quoted command arguments");

  const QByteArray expanded(4096, 'x');
  const QString expandedPath = QDir(directory).filePath("expanded result.txt");
  qputenv("GROUNDZERO_EXTERNFX_TEST_VALUE", expanded);
  compute(TFilePath(executable),
          "--write-fixture \"" + expandedPath.toStdString() +
              "\" \"%GROUNDZERO_EXTERNFX_TEST_VALUE%\" $out");
  require(result(expandedPath) == expanded,
          "Environment expansion truncated external-effect arguments");
  qunsetenv("GROUNDZERO_EXTERNFX_TEST_VALUE");

  const std::pair<TFilePath, std::string> failures[] = {
      {TFilePath(QDir(directory).filePath("missing-external-effect.exe")),
       arguments},
      {TFilePath(executable), std::string(40000, 'x')}};
  for (const auto& failure : failures)
    expectFailure(failure.first, failure.second, "Check the executable path");
  expectFailure(TFilePath(executable), "--fail-fixture", "exited with code 7");
  expectFailure(TFilePath(executable), "--missing-fixture", "output image");
  expectFailure(TFilePath(executable), "--corrupt-fixture $out",
                "output image");

  DWORD before = 0, after = 0;
  require(GetProcessHandleCount(GetCurrentProcess(), &before),
          "Cannot measure process handles before repeated external effects");
  for (int i = 0; i < 8; ++i) compute(TFilePath(executable), arguments);
  require(GetProcessHandleCount(GetCurrentProcess(), &after),
          "Cannot measure process handles after repeated external effects");
  require(after == before, "External-effect launches leaked process handles");
}
#endif
}  // namespace

int main(int argc, char** argv) {
  const QByteArray executableArgument = argc > 0 ? argv[0] : "";
  QGuiApplication app(argc, argv);
  try {
    const QStringList arguments = app.arguments();
    if (arguments.size() > 1) return worker(arguments, executableArgument);
    TThread::init();
    initImageIo();
    TRenderer::initialize();
    const QString executable = QCoreApplication::applicationFilePath();
    QTemporaryDir directory;
    require(directory.isValid(), "Cannot create external-effect fixture");
    inputPortOwnership();
    failedLoadPortOwnership(directory.path());
#ifndef _WIN32
    require(QDir::setCurrent(directory.path()),
            "Cannot enter external-effect fixture directory");
#endif

#ifdef _WIN32
    windowsProcesses(executable, directory.path());
#else
    QImage image(1, 1, QImage::Format_ARGB32);
    image.fill(Qt::red);
    require(image.save("fixture.png", "PNG"), "Cannot write source fixture");
    compute(TFilePath("/bin/sh"),
            "-c 'printf \"%s\" \"argument with spaces\" > "
            "\"argument result.txt\"; cp fixture.png $out'");
    require(result("argument result.txt") == "argument with spaces",
            "The external effect changed quoted command arguments");
    expectFailure(TFilePath("/bin/sh"), "-c 'exit 7'", "exited with code 7");
    expectFailure(TFilePath("/bin/sh"), "-c 'exit 0'", "output image");
    expectFailure(TFilePath("/bin/sh"), "-c 'printf invalid > $out'",
                  "output image");
#endif
    executableIdentity(executable, directory.path());
    publicRendering(executable);
    std::cout << "External effect process and output validation: PASS\n";
    return 0;
  } catch (const TException& error) {
    std::cerr << QString::fromStdWString(error.getMessage()).toStdString()
              << '\n';
    return 1;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
