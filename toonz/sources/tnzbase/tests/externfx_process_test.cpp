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
  const int width    = mode == "small" ? 2 : mode == "large" ? 4 : 3;
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

void checkRenderedPixels(const TRaster32P& parent, bool success) {
  const TPixel32 expected[2][3] = {
      {TPixel32(0, 255, 255), TPixel32(255, 255, 0), TPixel32(255, 0, 255)},
      {TPixel32::Red, TPixel32(0, 128, 0, 128), TPixel32::Transparent}};
  for (int y = 0; y < parent->getLy(); ++y) {
    for (int x = 0; x < parent->getLx(); ++x) {
      const bool inside = x >= 1 && x <= 3 && y >= 1 && y <= 2;
      TPixel32 pixel    = TPixel32::Green;
      if (inside) pixel = success ? expected[y - 1][x - 1] : TPixel32::Blue;
      require(
          parent->pixels(y)[x] == pixel,
          "External output pixels, alpha, orientation or tile border changed");
    }
  }
}

void publicRendering(const QString& executable) {
  for (const char* mode : {"pixels", "small", "large", "gray"}) {
    auto* effect = new TExternalProgramFx();
    TRasterFxP owner(effect);
    effect->setExecutable(TFilePath(executable),
                          std::string("--render-fixture $in $out ") + mode);
    effect->addPort("in", "png", true);
    effect->addPort("out", "png", false);
    effect->getInputPort(0)->setFx(new BlueSource());
    TRaster32P parent(5, 4);
    parent->fill(TPixel32::Green);
    TRaster32P destination = parent->extract(1, 1, 3, 2);
    TTile tile(destination, TPointD(11, -7));
    const QString expectedError = QString::fromLatin1(mode) == "gray"
                                      ? "32-bit RGBA"
                                      : "different dimensions";
    const bool success          = QString::fromLatin1(mode) == "pixels";
    bool rejected               = false;
    try {
      publicCompute(*effect, tile);
    } catch (const TException& error) {
      rejected =
          QString::fromStdWString(error.getMessage()).contains(expectedError);
    }
    require(rejected != success,
            "External effect accepted an invalid output or rejected pixels");
    require(tile.getRaster()->getRawData() == destination->getRawData() &&
                tile.getRaster()->getSize() == destination->getSize() &&
                tile.m_pos == TPointD(11, -7),
            "External output replaced destination storage or tile coordinates");
    checkRenderedPixels(parent, success);
  }
}

int worker(const QStringList& arguments) {
  if (arguments[1] == "--render-fixture") return renderWorker(arguments);
  if (arguments[1] == "--fail-fixture") return 7;
  if (arguments[1] == "--missing-fixture") return 0;
  if (arguments.size() == 3 && arguments[1] == "--corrupt-fixture") {
    QFile output(arguments[2]);
    require(output.open(QIODevice::WriteOnly), "Cannot write corrupt output");
    require(output.write("invalid PNG") == 11, "Cannot finish corrupt output");
    return 0;
  }
  if (arguments.size() != 5 || arguments[1] != "--write-fixture") return 20;
  QFile output(arguments[2]);
  const QByteArray bytes = arguments[3].toUtf8();
  require(output.open(QIODevice::WriteOnly), "Cannot write worker fixture");
  require(output.write(bytes) == bytes.size(), "Cannot finish worker fixture");
  QImage image(1, 1, QImage::Format_ARGB32);
  image.fill(Qt::red);
  require(image.save(arguments[4], "PNG"),
          "Cannot save external-effect output");
  return 0;
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
  QGuiApplication app(argc, argv);
  try {
    const QStringList arguments = app.arguments();
    if (arguments.size() > 1) return worker(arguments);
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
