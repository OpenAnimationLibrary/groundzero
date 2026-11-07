#include "texternfx.h"
#include "texception.h"
#include "ttile.h"
#include "tnzimage.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QImage>
#include <QTemporaryDir>

#include <iostream>
#include <stdexcept>
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

int worker(const QStringList& arguments) {
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
  QCoreApplication app(argc, argv);
  try {
    const QStringList arguments = app.arguments();
    if (arguments.size() > 1) return worker(arguments);
    initImageIo();
#ifdef _WIN32
    const QString executable = QCoreApplication::applicationFilePath();
#endif
    QTemporaryDir directory;
    require(directory.isValid(), "Cannot create external-effect fixture");
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
