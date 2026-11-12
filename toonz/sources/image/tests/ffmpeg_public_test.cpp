// SPDX-License-Identifier: BSD-3-Clause

// Exercise the exported image-I/O API on every platform, including Windows
// DLL builds. Decoding with ffmpeg independently checks the saved pixels.
#include "tenv.h"
#include "thirdparty.h"
#include "texception.h"
#include "tlevel_io.h"
#include "tnzimage.h"
#include "trasterimage.h"
#include "toonz/preferences.h"
#include "toonz/toonzfolders.h"

#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QProcess>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QThread>

#include <array>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>

namespace {

constexpr int width = 96, height = 64, frameCount = 4;

void require(bool condition, const std::string &message) {
  if (!condition) throw std::runtime_error(message);
}

void verifyToolDirectory(const QString& configured, const QString& expected) {
  Preferences::instance()->setValue(ffmpegPath, configured);
  Preferences::instance()->setValue(rhubarbPath, configured);
  const struct {
    const char* name;
    void (*run)(QProcess&, const QStringList&);
  } tools[] = {{"ffmpeg", ThirdParty::runFFmpeg},
               {"ffprobe", ThirdParty::runFFprobe},
               {"rhubarb", ThirdParty::runRhubarb}};
  for (const auto& tool : tools) {
    QString executable = QString::fromLatin1(tool.name);
#ifdef _WIN32
    executable += ".exe";
#endif
    QProcess process;
    tool.run(process, {"-version"});
    const QString program = QDir::cleanPath(process.program());
    if (process.waitForStarted(10000)) {
      process.kill();
      process.waitForFinished(5000);
    }
    require(
        program == QDir(expected).filePath(executable),
        std::string(tool.name) + " resolved the helper directory incorrectly");
  }
}

QByteArray audioPcm() { return QByteArray::fromHex("00000100ff7f0080"); }

int runAudioFixture(const QString& mode) {
  const QByteArray pcm = audioPcm();
  std::cout.write(pcm.constData(), pcm.size()).flush();
  if (mode == "--audio-fixture-delay") QThread::msleep(2000);
  if (mode == "--audio-fixture-fail") {
    std::cerr << "Audio fixture rejected the input\n";
    return 7;
  }
  return 0;
}

void verifyAudioResult(QProcess& process, bool expectedSuccess,
                       const QByteArray& expectedData) {
  QByteArray data("stale audio from a previous import");
  const bool success = ThirdParty::readFFmpegAudio(process, data);
  require(success == expectedSuccess,
          "Audio decoder reported the wrong result");
  require(data == expectedData,
          "Audio decoder returned stale, partial or changed PCM");
  require(process.state() == QProcess::NotRunning,
          "Audio decoder left its child running");
  require(!process.isOpen(), "Audio decoder left its process channels open");
}

void verifyAudioLifecycle(const QDir& directory) {
  const int savedTimeout = ThirdParty::getFFmpegTimeout();
  ThirdParty::setFFmpegTimeout(5);
  const QString executable = QCoreApplication::applicationFilePath();
  QProcess process;
  verifyAudioResult(process, false, {});
  process.start(directory.filePath("missing audio helper with spaces"),
                QStringList());
  verifyAudioResult(process, false, {});

  process.start(executable, {"--audio-fixture-success"});
  verifyAudioResult(process, true, audioPcm());
  process.start(executable, {"--audio-fixture-success"});
  require(process.waitForFinished(10000),
          "Audio fixture did not finish before reading");
  verifyAudioResult(process, true, audioPcm());

  process.start(executable, {"--audio-fixture-fail"});
  verifyAudioResult(process, false, {});
  process.start(executable, {"--audio-fixture-delay"});
  require(process.waitForStarted(10000),
          "Cannot start the killed-audio fixture");
  process.kill();
  require(process.waitForFinished(10000),
          "Killed audio fixture was not reaped");
  verifyAudioResult(process, false, {});

  ThirdParty::setFFmpegTimeout(1);
  process.start(executable, {"--audio-fixture-delay"});
  QElapsedTimer timer;
  timer.start();
  verifyAudioResult(process, false, {});
  require(timer.elapsed() < 10000,
          "Audio decoder ignored the configured timeout");

  ThirdParty::setFFmpegTimeout(5);
  process.start(executable, {"--audio-fixture-delay"});
  verifyAudioResult(process, true, audioPcm());
  // A wrapped 32-bit product would be 704 ms instead of this timeout.
  ThirdParty::setFFmpegTimeout(4294968);
  process.start(executable, {"--audio-fixture-delay"});
  verifyAudioResult(process, true, audioPcm());
  ThirdParty::setFFmpegTimeout(std::numeric_limits<int>::max());
  process.start(executable, {"--audio-fixture-success"});
  verifyAudioResult(process, true, audioPcm());
  ThirdParty::setFFmpegTimeout(0);
  process.start(executable, {"--audio-fixture-delay"});
  verifyAudioResult(process, false, {});
  ThirdParty::setFFmpegTimeout(savedTimeout);
}

void verifyAudioDecode(const QDir& directory) {
  const QByteArray header = QByteArray::fromHex(
      "524946462c00000057415645666d7420100000000100020044ac0000"
      "10b10200040010006461746108000000");
  QFile source(directory.filePath("stereo audio with spaces.wav"));
  require(source.open(QIODevice::WriteOnly),
          "Cannot create the stereo WAV fixture");
  const QByteArray wav = header + audioPcm();
  require(source.write(wav) == wav.size(),
          "Cannot finish the stereo WAV fixture");
  source.close();
  QProcess process;
  ThirdParty::runFFmpegAudio(process, source.fileName(), "-");
  verifyAudioResult(process, true, audioPcm());
  ThirdParty::runFFmpegAudio(process, directory.filePath("missing audio.wav"),
                             "-");
  verifyAudioResult(process, false, {});
}

TPixel32 pixelAt(int frame, bool top) {
  return top ? TPixel32(200, 30 + frame * 30, 50, 255)
             : TPixel32(20, 60, 180 + frame * 10, 255);
}

TPixelF hdrPixelAt(int frame, bool top) {
  if (frame >= 2)
    return top ? TPixelF(2.0f, -0.25f, 0.25f, 1.0f)
               : TPixelF(0.5f, 0.25f, 2.0f, 0.5f);
  return top ? TPixelF(-0.25f, (30 + frame * 30) / 255.0f, 1.25f, 1.5f)
             : TPixelF(1.5f, -0.5f, (180 + frame * 10) / 255.0f, 1.25f);
}

TPixel32 hdrOutputPixelAt(int frame, bool top) {
  if (frame >= 2)
    return top ? TPixel32(255, 0, 64, 255) : TPixel32(128, 64, 255, 128);
  return top ? TPixel32(0, 30 + frame * 30, 255, 255)
             : TPixel32(255, 0, 180 + frame * 10, 255);
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

void verifyDecoded(const QString& ffmpeg, const QString& path,
                   TPixel32 (*expectedPixel)(int, bool) = pixelAt) {
  QProcess decoder;
  QStringList args = {"-v", "error", "-f", "apng", "-ignore_loop", "1",
                      "-i", path};
  args << ThirdParty::getFFmpegFrameSyncArgs() << "-f" << "rawvideo"
       << "-pix_fmt" << "rgba" << "-";
  decoder.start(ffmpeg, args);
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
        const TPixel32 expected = expectedPixel(frame, y < height / 2);
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

void exportHdr(const QDir& directory, const QString& ffmpeg, bool pipe) {
  if (pipe)
    qunsetenv("OTDEV_FFMPEG_NO_PIPE");
  else
    qputenv("OTDEV_FFMPEG_NO_PIPE", "1");
  const QString path =
      directory.filePath(pipe ? "hdr-pipe.apng" : "hdr-images.apng");
  std::array<TRasterFP, frameCount> sources;
  {
    TLevelWriterP writer{TFilePath(path)};
    require(!!writer, "Cannot open the public APNG writer for HDR frames");
    writer->setFrameRate(24);
    for (int frame = 0; frame < frameCount; ++frame) {
      if (frame == 3) {
        sources[frame] = sources[frame - 1];
      } else {
        sources[frame] = TRasterFP(width, height);
        for (int y = 0; y < height; ++y)
          for (int x = 0; x < width; ++x)
            sources[frame]->pixels(y)[x] = hdrPixelAt(frame, y >= height / 2);
      }
      TImageWriterP frameWriter = writer->getFrameWriter(TFrameId(frame + 1));
      require(!!frameWriter, "Public factory returned no HDR frame writer");
      frameWriter->save(TRasterImageP(sources[frame]));
    }
    writer->finalize();
  }
  for (int frame = 0; frame < frameCount; ++frame)
    for (int y = 0; y < height; ++y)
      for (int x = 0; x < width; ++x)
        require(
            sources[frame]->pixels(y)[x] == hdrPixelAt(frame, y >= height / 2),
            "APNG export modified the floating-point source pixels");
  verifyDecoded(ffmpeg, path, hdrOutputPixelAt);
  qunsetenv("OTDEV_FFMPEG_NO_PIPE");
}

}  // namespace

int main(int argc, char **argv) {
  QCoreApplication application(argc, argv);
  if (argc == 2 &&
      QString::fromLocal8Bit(argv[1]).startsWith("--audio-fixture-"))
    return runAudioFixture(QString::fromLocal8Bit(argv[1]));
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
    verifyToolDirectory(QString(), QCoreApplication::applicationDirPath());
    verifyToolDirectory(".", QCoreApplication::applicationDirPath());
    verifyToolDirectory(
        "./helpers with spaces",
        QCoreApplication::applicationDirPath() + "/helpers with spaces");
    verifyToolDirectory(directory.filePath("helpers with spaces"),
                        directory.filePath("helpers with spaces"));
    Preferences::instance()->setValue(ffmpegPath, tools.absolutePath());
    Preferences::instance()->setValue(ffmpegMultiThread, false);
    verifyAudioLifecycle(directory);
    verifyAudioDecode(directory);
    initImageIo();
    exportAndAbort(directory, executable.absoluteFilePath(), true);
    exportAndAbort(directory, executable.absoluteFilePath(), false);
    exportHdr(directory, executable.absoluteFilePath(), true);
    exportHdr(directory, executable.absoluteFilePath(), false);
    std::cout << "Public FFmpeg export, HDR preservation, decode and abort "
                 "checks passed\n";
    return 0;
  } catch (const TException &error) {
    std::cerr << QString::fromStdWString(error.getMessage()).toStdString() << '\n';
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
  }
  return 1;
}
