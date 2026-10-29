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
#include <QDateTime>
#include <QFile>
#include <QDir>
#include <QFileInfo>
#include <QProcess>
#include <QSemaphore>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTimer>

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

QDir ffmpegCache() {
  return QDir(QStandardPaths::writableLocation(QStandardPaths::CacheLocation) +
              "/ffmpeg");
}

QStringList intermediateImages() {
  return ffmpegCache().entryList({"export_*_in*.png"}, QDir::Files);
}

QStringList newNames(const QStringList &before, const QStringList &after) {
  QStringList result;
  for (const QString &name : after)
    if (!before.contains(name)) result.push_back(name);
  return result;
}

QStringList importGenerations() {
  return ffmpegCache().entryList({"import-v2-*.frames"}, QDir::Dirs);
}

QByteArray readFile(const QString &path) {
  QFile file(path);
  require(file.open(QIODevice::ReadOnly), "Cannot read test file");
  return file.readAll();
}

void writeFile(const QString &path, const QByteArray &bytes) {
  QFile file(path);
  require(file.open(QIODevice::WriteOnly), "Cannot create test file");
  require(file.write(bytes) == bytes.size(), "Cannot write test file");
}

typedef std::function<TLevelWriter *(const TFilePath &)> WriterMaker;

// Writes the frames and explicitly checks encoder completion.
void exportFrames(const WriterMaker &makeWriter, const QString &path,
                  bool usePipe, int frameOffset = 0, int frameCount = kFrames) {
  const QStringList previousImages = intermediateImages();
  if (usePipe)
    qunsetenv("OTDEV_FFMPEG_NO_PIPE");
  else
    qputenv("OTDEV_FFMPEG_NO_PIPE", "1");

  {
    TSmartPointerT<TLevelWriter> writer(
        makeWriter(TFilePath(path.toStdWString())));
    writer->setFrameRate(24);
    for (int frame = 0; frame < frameCount; ++frame) {
      TImageWriterP frameWriter = writer->getFrameWriter(TFrameId(frame + 1));
      require(frameWriter.getPointer(), "No frame writer");
      frameWriter->save(TRasterImageP(makeFrame((frame + frameOffset) % kFrames)));
    }
    // ffmpeg is still running (pipe) or not started yet (intermediate images)
    require(newNames(previousImages, intermediateImages()).isEmpty() == usePipe,
            usePipe ? "The pipe export wrote intermediate images"
                    : "The intermediate image export wrote no images");
    writer->finalize();
    writer->finalize();  // Completion is idempotent, including destructor fallback.
  }
  qunsetenv("OTDEV_FFMPEG_NO_PIPE");
  require(newNames(previousImages, intermediateImages()).isEmpty(),
          "Export leaked owned intermediate images");
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
  const QStringList previousImages = intermediateImages();
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
    require(!newNames(previousImages, intermediateImages()).isEmpty(),
            "Concurrent export did not use intermediate images");
    writer->finalize();
  }
  Preferences::instance()->setValue(ffmpegMultiThread, false);
  require(newNames(previousImages, intermediateImages()).isEmpty(),
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

}

void expectFailure(const std::function<void()> &operation,
                   const std::string &message) {
  bool failed = false;
  try {
    operation();
  } catch (const TException &) {
    failed = true;
  }
  require(failed, message);
}

void testFailedFinalization(const QTemporaryDir &directory, bool usePipe) {
  const QString path = directory.filePath(usePipe ? "failed-pipe.apng"
                                                 : "failed-files.apng");
  const QByteArray original("previous good output");
  writeFile(path, original);
  Ffmpeg writer;
  writer.setPath(TFilePath(path.toStdWString()));
  if (usePipe) writer.enablePipe();
  const Ffmpeg::ArgsBuilder invalidCodec = [](QStringList &pre,
                                             QStringList &post) {
    pre << "-framerate" << "24";
    post << "-c:v" << "groundzero_missing_encoder_for_regression";
  };
  // A failed pipe may be noticed either while enqueueing or while closing.
  try {
    writer.saveFrame(TRasterImageP(makeFrame(0)), 1, invalidCodec);
  } catch (const TException &) {
  }
  require(readFile(path) == original, "Encoding replaced output before success");
  expectFailure([&] { writer.finalize(invalidCodec); },
                "Encoder failure was reported as successful finalization");
  expectFailure([&] { writer.finalize(invalidCodec); },
                "Repeated finalization forgot the encoder failure");
  require(readFile(path) == original, "Failed encoding destroyed previous output");
}

void testAbortedExport(const QTemporaryDir &directory, bool usePipe,
                       bool failedFrame) {
  const QString path = directory.filePath(QString("aborted-%1-%2.apng")
                                              .arg(usePipe).arg(failedFrame));
  const QByteArray original("previous good output");
  writeFile(path, original);
  const QStringList previousImages = intermediateImages();
  if (usePipe) qunsetenv("OTDEV_FFMPEG_NO_PIPE");
  else qputenv("OTDEV_FFMPEG_NO_PIPE", "1");
  {
    TSmartPointerT<TLevelWriter> writer(TLevelWriterAPng::create(
        TFilePath(path.toStdWString()), new Tiio::APngWriterProperties()));
    writer->setFrameRate(24);
    writer->getFrameWriter(TFrameId(1))->save(TRasterImageP(makeFrame(0)));
    if (failedFrame)
      expectFailure([&] { writer->getFrameWriter(TFrameId(2))->save(TImageP()); },
                    "A rejected frame was reported as saved");
    else
      writer->getFrameWriter(TFrameId(2))->save(TRasterImageP(makeFrame(1)));
    writer->abort();
    writer->abort();
    expectFailure([&] { writer->finalize(); },
                  "An aborted export was reported as completed");
    require(readFile(path) == original, "Aborting replaced the old output");
  }
  require(readFile(path) == original,
          "Destructor finalized an aborted export");
  require(newNames(previousImages, intermediateImages()).isEmpty(),
          "Aborting leaked owned intermediate images");
  qunsetenv("OTDEV_FFMPEG_NO_PIPE");
}

void testCancellationDuringFinalization(const QTemporaryDir &directory) {
  const QString path = directory.filePath("cancel-finalize.apng");
  const QByteArray original("previous good output");
  writeFile(path, original);
  qputenv("OTDEV_FFMPEG_NO_PIPE", "1");
  TSmartPointerT<TLevelWriter> writer(TLevelWriterAPng::create(
      TFilePath(path.toStdWString()), new Tiio::APngWriterProperties()));
  writer->setFrameRate(24);
  for (int frame = 1; frame <= 2; ++frame)
    writer->getFrameWriter(TFrameId(frame))->save(TRasterImageP(makeFrame(frame)));
  // Intermediate encoding runs a nested event loop, just like the UI export.
  bool requested = false;
  QTimer cancellation;
  cancellation.setSingleShot(true);
  QObject::connect(&cancellation, &QTimer::timeout, &cancellation, [&] {
    requested = true;
    writer->requestAbort();
  });
  cancellation.start(0);
  expectFailure([&] { writer->finalize(); },
                "Cancellation during finalization published an export");
  require(requested, "Finalization did not dispatch the cancellation callback");
  writer->abort();
  require(readFile(path) == original,
          "Cancellation during finalization replaced the old output");
  qunsetenv("OTDEV_FFMPEG_NO_PIPE");
}

void testIndependentExports(const QTemporaryDir &directory) {
  const QString path = directory.filePath("same-target.apng");
  const QByteArray original("previous good output");
  writeFile(path, original);
  qputenv("OTDEV_FFMPEG_NO_PIPE", "1");
  TSmartPointerT<TLevelWriter> first(TLevelWriterAPng::create(
      TFilePath(path.toStdWString()), new Tiio::APngWriterProperties()));
  TSmartPointerT<TLevelWriter> second(TLevelWriterAPng::create(
      TFilePath(path.toStdWString()), new Tiio::APngWriterProperties()));
  require(readFile(path) == original,
          "Opening a writer removed the previous output");
  first->setFrameRate(24);
  second->setFrameRate(24);
  for (int frame = 1; frame <= 2; ++frame) {
    first->getFrameWriter(TFrameId(frame))->save(TRasterImageP(makeFrame(frame)));
    second->getFrameWriter(TFrameId(frame))->save(
        TRasterImageP(makeFrame(frame + 3)));
  }
  require(readFile(path) == original,
          "Unfinished encoding replaced the previous output");
  first->finalize();
  expectPixel(decodeRgba(path, "apng", 2), 0, 0, topPixel(1), 0,
              "first same-target export");
  second->finalize();
  expectPixel(decodeRgba(path, "apng", 2), 0, 0, topPixel(4), 0,
              "second same-target export");
  qunsetenv("OTDEV_FFMPEG_NO_PIPE");
}

void testSourceReplacementAndIncompleteCache(const QTemporaryDir &directory) {
  const WriterMaker makeWriter = [](const TFilePath &path) {
    return TLevelWriterMp4::create(path, new Tiio::Mp4WriterProperties());
  };
  const QString source = directory.filePath("replace-source.mp4");
  exportFrames(makeWriter, source, true);
  const QDateTime originalTime = QFileInfo(source).lastModified();
  auto first = std::make_unique<TLevelReaderMp4>(TFilePath(source.toStdWString()));
  require(first->load(kFrames).getPointer(), "Cannot read initial source");

  exportFrames(makeWriter, source, true, 3, 3);
  QFile timestamp(source);
  require(timestamp.open(QIODevice::ReadWrite), "Cannot open replaced source");
  require(timestamp.setFileTime(originalTime, QFileDevice::FileModificationTime),
          "Cannot restore source timestamp");
  timestamp.close();

  const QStringList before = importGenerations();
  {
    Ffmpeg replacement;
    replacement.setPath(TFilePath(source.toStdWString()));
    require(replacement.getInfo().m_frameCount == 3,
            "Replaced source reused old video metadata");
    replacement.getFramesFromMovie();
    require(replacement.getImage(3).getPointer(), "Cannot read replaced source");
  }
  require(first->load(kFrames).getPointer(),
          "Source replacement invalidated an existing reader's generation");
  first.reset();

  const QStringList added = newNames(before, importGenerations());
  require(added.size() == 1, "Replacement did not publish one new cache generation");
  QDir generation(ffmpegCache().filePath(added.front()));
  const QString missing = generation.filePath("frame000003.png");
  require(QFile::remove(missing), "Cannot remove a cached test frame");
  {
    Ffmpeg reader;
    reader.setPath(TFilePath(source.toStdWString()));
    reader.getFramesFromMovie();
    require(reader.getImage(3).getPointer(),
            "Cache reused an incomplete sequence because frame one existed");
  }
  writeFile(missing, "truncated");
  {
    Ffmpeg reader;
    reader.setPath(TFilePath(source.toStdWString()));
    reader.getFramesFromMovie();
    require(reader.getImage(3).getPointer(), "Truncated cached frame was reused");
  }
  require(QFile::remove(generation.filePath("complete.json")),
          "Cannot remove the test completion manifest");
  {
    Ffmpeg reader;
    reader.setPath(TFilePath(source.toStdWString()));
    reader.getFramesFromMovie();
    require(QFileInfo::exists(generation.filePath("complete.json")),
            "Uncommitted frame cache was reused");
  }
}

void testFailedProbeCanRetry(const QTemporaryDir &directory) {
  const QString source = directory.filePath("retry-probe.mp4");
  require(QFile::copy(directory.filePath("pipe.mp4"), source),
          "Cannot create probe retry source");
  Ffmpeg reader;
  reader.setPath(TFilePath(source.toStdWString()));
  QTemporaryDir failingTools;
  require(failingTools.isValid(), "Cannot create failing probe directory");
  const QString executable = failingTools.filePath("ffprobe");
  writeFile(executable, "#!/bin/sh\nexit 23\n");
  require(QFile::setPermissions(executable, QFile::ReadOwner | QFile::WriteOwner |
                                          QFile::ExeOwner),
          "Cannot make failing probe executable");
  Preferences::instance()->setValue(ffmpegPath, failingTools.path());
  expectFailure([&] { reader.getInfo(); }, "Failed probe was accepted");
  Preferences::instance()->setValue(ffmpegPath,
                                    QString::fromUtf8(TIIO_FFMPEG_TEST_DIR));
  const ffmpegFileInfo info = reader.getInfo();
  require(info.m_frameCount == kFrames && info.m_lx == kLx && info.m_ly == kLy,
          "Failed metadata was cached and prevented a retry");
  reader.getFramesFromMovie();
  require(reader.getImage(kFrames).getPointer(),
          "Cannot read frames after retrying metadata");
}

void testFailedExtractionCanRetry(const QTemporaryDir &directory) {
  const QString source = directory.filePath("retry-source.mp4");
  require(QFile::copy(directory.filePath("pipe.mp4"), source),
          "Cannot create retry source");
  Ffmpeg reader;
  reader.setPath(TFilePath(source.toStdWString()));
  reader.getInfo();
  const QStringList before = importGenerations();
  QTemporaryDir failingTools;
  require(failingTools.isValid(), "Cannot create failing encoder directory");
  const QString executable = failingTools.filePath("ffmpeg");
  writeFile(executable, "#!/bin/sh\nexit 23\n");
  require(QFile::setPermissions(executable, QFile::ReadOwner | QFile::WriteOwner |
                                          QFile::ExeOwner),
          "Cannot make failing encoder executable");
  // The metadata is already available; keep ffprobe available for codec lookup.
  require(QFile::link(QDir(QString::fromUtf8(TIIO_FFMPEG_TEST_DIR))
                         .filePath("ffprobe"), failingTools.filePath("ffprobe")),
          "Cannot provide ffprobe for failed extraction");
  Preferences::instance()->setValue(ffmpegPath, failingTools.path());
  expectFailure([&] { reader.getFramesFromMovie(); },
                "Failed extraction was accepted");
  Preferences::instance()->setValue(ffmpegPath,
                                    QString::fromUtf8(TIIO_FFMPEG_TEST_DIR));
  require(newNames(before, importGenerations()).isEmpty(),
          "Failed extraction published a reusable cache generation");
  reader.getFramesFromMovie();
  require(reader.getImage(kFrames).getPointer(),
          "Failed extraction could not recover on retry");
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
    const QStringList previousImports = importGenerations();

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
    testFailedFinalization(directory, false);
    testFailedFinalization(directory, true);
    testAbortedExport(directory, false, false);
    testAbortedExport(directory, true, false);
    testAbortedExport(directory, false, true);
    testAbortedExport(directory, true, true);
    testCancellationDuringFinalization(directory);
    testIndependentExports(directory);
    testSourceReplacementAndIncompleteCache(directory);
    testFailedProbeCanRetry(directory);
    testFailedExtractionCanRetry(directory);

    for (const QString &generation :
         newNames(previousImports, importGenerations())) {
      require(QDir(ffmpegCache().filePath(generation)).removeRecursively(),
              "Cannot remove test import generation");
      const QString metadata = generation.left(generation.size() - 7) + ".txt";
      QFile::remove(ffmpegCache().filePath(metadata));
    }

    std::cout << "FFmpeg export tests passed\n";
    return 0;
  } catch (const TException &error) {
    std::cerr << "FFmpeg export test failed: "
              << QString::fromStdWString(error.getMessage()).toStdString()
              << '\n';
    return 1;
  } catch (const std::exception &error) {
    std::cerr << "FFmpeg export test failed: " << error.what() << '\n';
    return 1;
  }
}
