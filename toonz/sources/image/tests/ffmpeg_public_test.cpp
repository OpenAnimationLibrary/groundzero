// SPDX-License-Identifier: BSD-3-Clause

// Exercise the exported image-I/O API on every platform, including Windows
// DLL builds. Decoding with ffmpeg independently checks the saved pixels.
#include "tenv.h"
#include "texception.h"
#include "tlevel_io.h"
#include "tnzimage.h"
#include "trasterimage.h"
#include "toonz/preferences.h"
#include "toonz/toonzfolders.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QProcess>
#include <QStandardPaths>
#include <QTemporaryDir>

#include <iostream>
#include <stdexcept>
#include <string>

namespace {

constexpr int width = 96, height = 64, frameCount = 4;

void require(bool condition, const std::string &message) {
  if (!condition) throw std::runtime_error(message);
}

TPixel32 pixelAt(int frame, bool top) {
  return top ? TPixel32(200, 30 + frame * 30, 50, 255)
             : TPixel32(20, 60, 180 + frame * 10, 255);
}

void saveFrame(const TLevelWriterP &writer, int frame, int colorFrame) {
  TRaster32P raster(width, height);
  for (int y = 0; y < height; ++y)
    for (int x = 0; x < width; ++x)
      raster->pixels(y)[x] = pixelAt(colorFrame, y >= height / 2);
  TImageWriterP frameWriter = writer->getFrameWriter(TFrameId(frame));
  require(!!frameWriter, "Public factory returned no frame writer");
  frameWriter->save(TRasterImageP(raster));
}

QByteArray readFile(const QString &path) {
  QFile file(path);
  require(file.open(QIODevice::ReadOnly), "Cannot read the exported file");
  const QByteArray bytes = file.readAll();
  require(file.error() == QFile::NoError, "Could not read the complete export");
  return bytes;
}

void verifyDecoded(const QString &ffmpeg, const QString &path) {
  QProcess decoder;
  decoder.start(ffmpeg, {"-v", "error", "-f", "apng", "-ignore_loop", "1",
                        "-i", path, "-vsync", "0", "-f", "rawvideo",
                        "-pix_fmt", "rgba", "-"});
  require(decoder.waitForStarted(10000), "Cannot start the FFmpeg decoder");
  if (!decoder.waitForFinished(60000)) {
    decoder.kill();
    decoder.waitForFinished(5000);
    throw std::runtime_error("FFmpeg decoding timed out");
  }
  require(decoder.exitStatus() == QProcess::NormalExit && decoder.exitCode() == 0,
          "FFmpeg decode failed: " + decoder.readAllStandardError().toStdString());
  const QByteArray decoded = decoder.readAllStandardOutput();
  require(decoded.size() == frameCount * width * height * 4,
          "Export has the wrong frame count or dimensions");
  const auto *bytes = reinterpret_cast<const unsigned char *>(decoded.constData());
  for (int frame = 0; frame < frameCount; ++frame)
    for (int y = 0; y < height; ++y)
      for (int x = 0; x < width; ++x) {
        // FFmpeg raw RGBA starts at the top; native rasters start at the bottom.
        const TPixel32 expected = pixelAt(frame, y < height / 2);
        const int offset = ((frame * height + y) * width + x) * 4;
        require(bytes[offset] == expected.r && bytes[offset + 1] == expected.g &&
                    bytes[offset + 2] == expected.b && bytes[offset + 3] == expected.m,
                "Export changed frame order, pixel channels or row orientation");
      }
}

void exportAndAbort(const QDir &directory, const QString &ffmpeg, bool pipe) {
  if (pipe) qunsetenv("OTDEV_FFMPEG_NO_PIPE");
  else qputenv("OTDEV_FFMPEG_NO_PIPE", "1");
  const QString path = directory.filePath(pipe ? "pipe.apng" : "images.apng");
  {
    TLevelWriterP writer{TFilePath(path)};
    require(!!writer, "APNG writer was not registered by initImageIo");
    writer->setFrameRate(24);
    for (int frame = 0; frame < frameCount; ++frame) saveFrame(writer, frame + 1, frame);
    writer->finalize();
    writer->finalize();
  }
  verifyDecoded(ffmpeg, path);
  const QByteArray original = readFile(path);
  require(!original.isEmpty(), "Successful export was empty");
  {
    TLevelWriterP writer{TFilePath(path)};
    require(!!writer, "Cannot reopen the public APNG writer");
    writer->setFrameRate(24);
    saveFrame(writer, 1, 3);
    saveFrame(writer, 2, 2);
    writer->abort();
    writer->abort();
    require(readFile(path) == original, "Aborting replaced the previous export");
  }
  require(readFile(path) == original, "Destructor published an aborted export");
  qunsetenv("OTDEV_FFMPEG_NO_PIPE");
}

}  // namespace

int main(int argc, char **argv) {
  QCoreApplication application(argc, argv);
  QCoreApplication::setApplicationName("ffmpeg_public_test");
  QStandardPaths::setTestModeEnabled(true);
  try {
    require(argc == 2, "Usage: ffmpeg_public_test <ffmpeg-executable>");
    const QFileInfo executable(QString::fromLocal8Bit(argv[1]));
    require(executable.isExecutable(), "Configured FFmpeg executable is unavailable");
    const QDir tools(executable.absolutePath());
#ifdef _WIN32
    const QString probe = tools.filePath("ffprobe.exe");
#else
    const QString probe = tools.filePath("ffprobe");
#endif
    require(QFileInfo(probe).isExecutable(), "ffprobe must be beside ffmpeg");
    QTemporaryDir temporary;
    require(temporary.isValid(), "Cannot create test storage");
    const QDir directory(temporary.path());
    const QString stuff = directory.filePath("stuff");
    require(QDir().mkpath(stuff + "/profiles/layouts/rooms/Default") &&
                QDir().mkpath(stuff + "/config"),
            "Cannot create test preferences");
    TEnv::setStuffDir(TFilePath(stuff));
    TEnv::setApplicationFullName("ffmpeg_public_test");
    require(QDir().mkpath(ToonzFolder::getMyModuleDir().getQString()),
            "Cannot create the settings folder");
    Preferences::instance()->setValue(ffmpegPath, tools.absolutePath());
    Preferences::instance()->setValue(ffmpegMultiThread, false);
    initImageIo();
    exportAndAbort(directory, executable.absoluteFilePath(), true);
    exportAndAbort(directory, executable.absoluteFilePath(), false);
    std::cout << "Public FFmpeg export, decode and abort checks passed\n";
    return 0;
  } catch (const TException &error) {
    std::cerr << QString::fromStdWString(error.getMessage()).toStdString() << '\n';
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
  }
  return 1;
}
