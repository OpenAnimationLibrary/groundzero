// SPDX-License-Identifier: BSD-3-Clause

// Exports frames through the FFmpeg-based APNG and MP4 writers, once streamed
// to ffmpeg's standard input and once through intermediate images
// (OTDEV_FFMPEG_NO_PIPE=1), and checks frame count, row and channel order,
// alpha and that both paths produce the same pictures.

#include "../ffmpeg/tiio_apng.h"
#include "../ffmpeg/tiio_mp4.h"

#include "tenv.h"
#include "tfilepath.h"
#include "tlevel_io.h"
#include "tpixel.h"
#include "traster.h"
#include "trasterimage.h"
#include "toonz/preferences.h"
#include "toonz/toonzfolders.h"

#include <QByteArray>
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QFileInfo>
#include <QProcess>
#include <QSemaphore>
#include <QStandardPaths>
#include <QTemporaryDir>

#include <algorithm>
#include <cstdlib>
#include <exception>
#include <functional>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#ifndef TIIO_FFMPEG_TEST_DIR
#error "TIIO_FFMPEG_TEST_DIR must name the directory of the ffmpeg executable"
#endif

namespace {

// Even, so the writers keep the size; large enough for the MP4 bit rate
// (pixel count / 150 kbit/s) to keep flat colors accurate
constexpr int kLx        = 320;
constexpr int kLy        = 240;
constexpr int kFrames    = 6;
constexpr int kFrameSize = kLx * kLy * 4;

void require(bool condition, const std::string &message) {
  if (!condition) throw std::runtime_error(message);
}

QString ffmpegExecutable() {
  return QDir(QString::fromUtf8(TIIO_FFMPEG_TEST_DIR))
      .filePath(QStringLiteral("ffmpeg"));
}

// Premultiplied pixels; raster row 0 is the bottom of the picture
TPixel32 topPixel(int frame) { return TPixel32(200, 20 * frame, 50, 255); }
TPixel32 bottomPixel() { return TPixel32(64, 32, 16, 128); }

TRaster32P makeFrame(int frame) {
  TRaster32P raster(kLx, kLy);
  raster->lock();
  for (int y = 0; y < kLy; ++y) {
    TPixel32 *pix = raster->pixels(y);
    for (int x = 0; x < kLx; ++x)
      pix[x] = (y >= kLy / 2) ? topPixel(frame) : bottomPixel();
  }
  raster->unlock();
  return raster;
}

// Path of the first intermediate image Ffmpeg would write for path
QString firstIntermediateImage(const QString &path) {
  const QByteArray hash = QCryptographicHash::hash(
      QFileInfo(path).absoluteFilePath().toUtf8(), QCryptographicHash::Md5);
  return QStandardPaths::writableLocation(QStandardPaths::CacheLocation) +
         "/ffmpeg/" + QString::fromLatin1(hash.toHex().left(6)) +
         "_in000000.png";
}

typedef std::function<TLevelWriter *(const TFilePath &)> WriterMaker;

// Writes the frames; the writer's destructor runs ffmpeg
void exportFrames(const WriterMaker &makeWriter, const QString &path,
                  bool usePipe) {
  if (usePipe)
    qunsetenv("OTDEV_FFMPEG_NO_PIPE");
  else
    qputenv("OTDEV_FFMPEG_NO_PIPE", "1");

  {
    TSmartPointerT<TLevelWriter> writer(
        makeWriter(TFilePath(path.toStdWString())));
    writer->setFrameRate(24);
    for (int frame = 0; frame < kFrames; ++frame) {
      TImageWriterP frameWriter = writer->getFrameWriter(TFrameId(frame + 1));
      require(frameWriter.getPointer(), "No frame writer");
      frameWriter->save(TRasterImageP(makeFrame(frame)));
    }
    // ffmpeg is still running (pipe) or not started yet (intermediate images)
    require(QFileInfo::exists(firstIntermediateImage(path)) == !usePipe,
            usePipe ? "The pipe export wrote intermediate images"
                    : "The intermediate image export wrote no images");
  }
  qunsetenv("OTDEV_FFMPEG_NO_PIPE");
  require(QFileInfo(path).size() > 0,
          "No output written: " + path.toStdString());
}

QByteArray decodeRgba(const QString &path, const QString &format,
                      int expectedFrames = kFrames) {
  QProcess process;
  QStringList args;
  args << "-v"
       << "error"
       << "-f" << format << "-i" << path << "-f"
       << "rawvideo"
       << "-pix_fmt"
       << "rgba"
       << "-";
  process.start(ffmpegExecutable(), args);
  require(process.waitForFinished(120000), "ffmpeg decode did not finish");
  const QByteArray frames = process.readAllStandardOutput();
  require(
      process.exitStatus() == QProcess::NormalExit && process.exitCode() == 0,
      "ffmpeg cannot decode " + path.toStdString() + ": " +
          process.readAllStandardError().toStdString());
  require(frames.size() == expectedFrames * kFrameSize,
          path.toStdString() + ": expected " + std::to_string(expectedFrames) +
              " frames, decoded " + std::to_string(frames.size()) + " bytes");
  return frames;
}

// Decoded rgba pixel; row 0 is the top of the picture
void expectPixel(const QByteArray &frames, int frame, int row,
                 const TPixel32 &expected, int tolerance,
                 const std::string &what) {
  const unsigned char *p = reinterpret_cast<const unsigned char *>(
      frames.constData() + frame * kFrameSize + row * kLx * 4);
  const int actual[4] = {p[0], p[1], p[2], p[3]};
  const int wanted[4] = {expected.r, expected.g, expected.b, expected.m};
  const int channels  = tolerance ? 3 : 4;  // lossy video has no alpha
  for (int c = 0; c < channels; ++c)
    require(std::abs(actual[c] - wanted[c]) <= tolerance,
            what + ": frame " + std::to_string(frame) + " row " +
                std::to_string(row) + " channel " + std::to_string(c) + " is " +
                std::to_string(actual[c]) + ", expected " +
                std::to_string(wanted[c]));
}

void expectSamePictures(const QByteArray &a, const QByteArray &b, int tolerance,
                        const std::string &what) {
  require(a.size() == b.size(), what + ": different sizes");
  int maxDiff = 0;
  for (int i = 0; i < a.size(); ++i)
    maxDiff = std::max(
        maxDiff, std::abs(int((unsigned char)a[i]) - int((unsigned char)b[i])));
  require(maxDiff <= tolerance,
          what + ": pipe and intermediate image exports differ by " +
              std::to_string(maxDiff));
  if (maxDiff) std::cout << what << ": max difference " << maxDiff << "\n";
}

// tolerance: allowed error of the decoded colors; sameTolerance: allowed
// difference between the pipe and intermediate image exports
void testFormat(const QTemporaryDir &directory, const QString &type,
                const QString &decodeFormat, const WriterMaker &makeWriter,
                int tolerance, int sameTolerance) {
  const QString pipePath = directory.filePath("pipe." + type);
  const QString filePath = directory.filePath("files." + type);
  exportFrames(makeWriter, pipePath, true);
  exportFrames(makeWriter, filePath, false);

  const QByteArray piped = decodeRgba(pipePath, decodeFormat);
  const QByteArray files = decodeRgba(filePath, decodeFormat);
  const std::string what = type.toStdString();
  for (int frame = 0; frame < kFrames; ++frame) {
    expectPixel(piped, frame, 0, topPixel(frame), tolerance, what);
    expectPixel(piped, frame, kLy - 1, bottomPixel(), tolerance, what);
  }
  expectSamePictures(piped, files, sameTolerance, what);
}

// Exercise the shared writer exactly as concurrent render completions do.
// Conversion and PNG encoding may overlap, but frame order, shared metadata,
// and cleanup registration must remain valid.
void testConcurrentExport(const QTemporaryDir &directory) {
  constexpr int frameCount = 24;
  constexpr int workers = 4;
  const QString path = directory.filePath("concurrent.apng");
  Preferences::instance()->setValue(ffmpegMultiThread, true);
  qunsetenv("OTDEV_FFMPEG_NO_PIPE");
  {
    TSmartPointerT<TLevelWriter> writer(TLevelWriterAPng::create(
        TFilePath(path.toStdWString()), new Tiio::APngWriterProperties()));
    writer->setFrameRate(24);
    QSemaphore ready, start;
    std::mutex errorMutex;
    std::exception_ptr error;
    std::vector<std::thread> threads;
    for (int worker = 0; worker < workers; ++worker) {
      threads.emplace_back([&, worker] {
        ready.release();
        start.acquire();
        try {
          for (int frame = worker; frame < frameCount; frame += workers) {
            TImageWriterP frameWriter =
                writer->getFrameWriter(TFrameId(frame + 1));
            require(frameWriter.getPointer(), "No concurrent frame writer");
            frameWriter->save(TRasterImageP(makeFrame(frame % kFrames)));
          }
        } catch (...) {
          std::lock_guard<std::mutex> lock(errorMutex);
          if (!error) error = std::current_exception();
        }
      });
    }
    ready.acquire(workers);
    start.release(workers);
    for (std::thread &thread : threads) thread.join();
    if (error) std::rethrow_exception(error);
    require(QFileInfo::exists(firstIntermediateImage(path)),
            "Concurrent export did not use intermediate images");
  }
  Preferences::instance()->setValue(ffmpegMultiThread, false);
  require(!QFileInfo::exists(firstIntermediateImage(path)),
          "Concurrent export left owned temporary images behind");
  const QByteArray decoded = decodeRgba(path, "apng", frameCount);
  for (int frame = 0; frame < frameCount; ++frame) {
    expectPixel(decoded, frame, 0, topPixel(frame % kFrames), 0,
                "concurrent APNG");
    expectPixel(decoded, frame, kLy - 1, bottomPixel(), 0,
                "concurrent APNG");
  }
}

void testSharedReaderCache(const QTemporaryDir &directory) {
  const TFilePath path(directory.filePath("pipe.mp4").toStdWString());
  auto first = std::make_unique<TLevelReaderFFmpeg>(path);
  auto second = std::make_unique<TLevelReaderMp4>(path);
  require(first->load(1).getPointer(), "First reader did not extract frames");
  require(second->load(1).getPointer(), "Second reader could not share frames");
  first.reset();
  TRasterImageP last = second->load(kFrames);
  require(last.getPointer() && last->getRaster()->getLx() == kLx &&
              last->getRaster()->getLy() == kLy,
          "Closing a reader removed another reader's cached frames");
  second.reset();

  // The cache is shared, so the test owns cleanup of its unique input path.
  const QByteArray hash = QCryptographicHash::hash(
      QFileInfo(path.getQString()).absoluteFilePath().toUtf8(),
      QCryptographicHash::Md5);
  QDir cache(QStandardPaths::writableLocation(QStandardPaths::CacheLocation) +
             "/ffmpeg");
  const QString prefix = QString::fromLatin1(hash.toHex().left(6));
  for (const QString &name : cache.entryList({prefix + "_in*.png",
                                             prefix + ".txt"}, QDir::Files))
    require(cache.remove(name), "Cannot remove test import cache");
}

void setUpEnvironment(const QTemporaryDir &directory) {
  // Preferences need a stuff folder with at least one room
  const QString stuff = directory.filePath("stuff");
  require(QDir().mkpath(stuff + "/profiles/layouts/rooms/Default") &&
              QDir().mkpath(stuff + "/config"),
          "Cannot create the stuff folder");
  TEnv::setStuffDir(TFilePath(stuff.toStdWString()));
  require(QDir().mkpath(ToonzFolder::getMyModuleDir().getQString()),
          "Cannot create the settings folder");

  Preferences::instance()->setValue(ffmpegPath,
                                    QString::fromUtf8(TIIO_FFMPEG_TEST_DIR));
  Preferences::instance()->setValue(ffmpegMultiThread, false);
}

}  // namespace

int main(int argc, char *argv[]) {
  QCoreApplication application(argc, argv);
  QCoreApplication::setApplicationName("tiio_ffmpeg_test");
  QStandardPaths::setTestModeEnabled(true);  // FFmpeg cache in a test folder
  try {
    QTemporaryDir directory;
    require(directory.isValid(), "Cannot create the test directory");
    require(QFileInfo(ffmpegExecutable()).isExecutable(),
            "ffmpeg not found in " TIIO_FFMPEG_TEST_DIR);
    setUpEnvironment(directory);

    // APNG is lossless: exact pixels, alpha included, identical exports
    testFormat(
        directory, "apng", "apng",
        [](const TFilePath &fp) {
          return TLevelWriterAPng::create(fp, new Tiio::APngWriterProperties());
        },
        0, 0);
    // MP4 (yuv420p): colors within compression error, no alpha
    testFormat(
        directory, "mp4", "mp4",
        [](const TFilePath &fp) {
          return TLevelWriterMp4::create(fp, new Tiio::Mp4WriterProperties());
        },
        12, 2);

    testConcurrentExport(directory);
    testSharedReaderCache(directory);

    std::cout << "FFmpeg export tests passed\n";
    return 0;
  } catch (const std::exception &error) {
    std::cerr << "FFmpeg export test failed: " << error.what() << '\n';
    return 1;
  }
}
