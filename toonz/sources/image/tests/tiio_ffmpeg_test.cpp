// SPDX-License-Identifier: BSD-3-Clause

// Exports frames through the FFmpeg-based APNG and MP4 writers, once streamed
// to ffmpeg's standard input and once through intermediate images
// (OTDEV_FFMPEG_NO_PIPE=1), and checks frame count, row and channel order,
// alpha and that both paths produce the same pictures.

#include "../ffmpeg/tiio_apng.h"
#include "../ffmpeg/tiio_mp4.h"

#include "tenv.h"
#include "thirdparty.h"
#include "tfilepath.h"
#include "tlevel_io.h"
#include "tpixel.h"
#include "traster.h"
#include "trasterimage.h"
#include "toonz/preferences.h"
#include "toonz/mediacache.h"
#include "toonz/toonzfolders.h"

#include <QByteArray>
#include <QCoreApplication>
#include <QDateTime>
#include <QFile>
#include <QDir>
#include <QFileInfo>
#include <QLockFile>
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

std::string saveDecodeDiagnostics(const QString &path, const QString &format,
                                  const QStringList &decodeArgs,
                                  const QByteArray &decoderLog) {
  const QDir output(QDir::current().filePath("ffmpeg-test-output"));
  QDir().mkpath(output.path());
  const QString name = QFileInfo(path).fileName();
  const QString saved = output.filePath(name);
  QFile::remove(saved);
  const bool copied = QFile::copy(path, saved);

  QProcess probe;
  QStringList args = {"-v", "error", "-f", format};
  if (format == "apng") args << "-ignore_loop" << "1";
  args << "-count_frames" << "-show_streams" << "-show_frames" << "-of"
       << "json" << "-i" << path;
  probe.start(QDir(QString::fromUtf8(TIIO_FFMPEG_TEST_DIR)).filePath("ffprobe"),
              args);
  if (!probe.waitForFinished(30000)) {
    probe.kill();
    probe.waitForFinished(3000);
  }
  QByteArray details = "Decoder arguments: " + decodeArgs.join(' ').toUtf8() +
      "\nDecoder output:\n" + decoderLog + "\nffprobe output:\n" +
      probe.readAllStandardOutput() + "\nffprobe errors:\n" +
      probe.readAllStandardError();
  QFile report(output.filePath(name + ".txt"));
  if (report.open(QIODevice::WriteOnly)) report.write(details);
  return "\nInput bytes: " + std::to_string(QFileInfo(path).size()) +
      "; preserved: " + (copied ? saved.toStdString() : "copy failed") +
      "\n" + details.right(12000).toStdString();
}

QByteArray decodeRgba(const QString &path, const QString &format,
                      int expectedFrames = kFrames) {
  QProcess process;
  QStringList args = {"-v", "warning", "-f", format};
  if (format == "apng") args << "-ignore_loop" << "1";
  // Validate the encoded frames themselves. Automatic timestamp resampling
  // may drop or duplicate frames, especially in very short animations.
  args << "-i" << path << ThirdParty::getFFmpegFrameSyncArgs() << "-f"
       << "rawvideo" << "-pix_fmt" << "rgba" << "-";
  process.start(ffmpegExecutable(), args);
  const bool finished = process.waitForFinished(120000);
  if (!finished) {
    process.kill();
    process.waitForFinished(3000);
  }
  const QByteArray frames = process.readAllStandardOutput();
  const QByteArray decoderLog = process.readAllStandardError();
  if (!finished || process.exitStatus() != QProcess::NormalExit ||
      process.exitCode() != 0 || frames.size() != expectedFrames * kFrameSize) {
    throw std::runtime_error(path.toStdString() + ": expected " +
        std::to_string(expectedFrames) + " frames, decoded " +
        std::to_string(frames.size()) + " bytes; decoder exit " +
        std::to_string(process.exitCode()) +
        saveDecodeDiagnostics(path, format, args, decoderLog));
  }
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
  const auto protectedReport = MediaCache::trim(ffmpegCache().absolutePath(), 0);
  require(protectedReport.errors.isEmpty() && protectedReport.activeGenerations > 0,
          "Media cleanup did not recognize real FFmpeg readers");
  first.reset();
  const auto stillProtected = MediaCache::trim(ffmpegCache().absolutePath(), 0);
  require(stillProtected.errors.isEmpty() && stillProtected.activeGenerations > 0,
          "Closing one FFmpeg reader released another reader's lease");
  TRasterImageP last = second->load(kFrames);
  require(last.getPointer() && last->getRaster()->getLx() == kLx &&
              last->getRaster()->getLy() == kLy,
          "Closing a reader removed another reader's cached frames");
  second.reset();
  const auto removed = MediaCache::trim(ffmpegCache().absolutePath(), 0);
  require(removed.errors.isEmpty() && removed.removedGenerations > 0,
          "Closed FFmpeg generation could not be cleaned");
  TLevelReaderFFmpeg regenerated(path);
  require(regenerated.load(kFrames).getPointer(),
          "Reading after cleanup did not recreate media from its original source");
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

void testFrameSyncCapabilities() {
  QTemporaryDir tools;
  require(tools.isValid(), "Cannot create capability fixture directory");
  const QString executable = tools.filePath("ffmpeg");
  writeFile(executable, "#!/bin/sh\nprintf '%s\\n' '-vsync mode'\n");
  require(QFile::setPermissions(executable, QFile::ReadOwner | QFile::WriteOwner |
                                          QFile::ExeOwner),
          "Cannot make capability fixture executable");
  Preferences::instance()->setValue(ffmpegPath, tools.path());
  require(ThirdParty::getFFmpegFrameSyncArgs() == QStringList({"-vsync", "0"}),
          "Older FFmpeg lost exact frame passthrough support");

  // Replace the executable in place. Its changed size invalidates cached help
  // even on filesystems whose modification timestamps have coarse resolution.
  writeFile(executable, "#!/bin/sh\nprintf '%s\\n' '-vsync mode' "
                        "'-fps_mode mode'\n");
  require(ThirdParty::getFFmpegFrameSyncArgs() ==
              QStringList({"-fps_mode", "passthrough"}),
          "Current FFmpeg did not prefer its supported frame passthrough option");
  writeFile(executable, "#!/bin/sh\nprintf '%s\\n' '-unrelated option'\n");
  expectFailure([] { ThirdParty::getFFmpegFrameSyncArgs(); },
                "Missing passthrough support silently enabled frame resampling");
  Preferences::instance()->setValue(ffmpegPath,
                                    QString::fromUtf8(TIIO_FFMPEG_TEST_DIR));
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

QString shellQuote(QString text) {
  text.replace("'", "'\\''");
  return "'" + text + "'";
}

void writeExecutable(const QString &path, const QByteArray &bytes) {
  writeFile(path, bytes);
  require(QFile::setPermissions(path, QFile::ReadOwner | QFile::WriteOwner |
                                    QFile::ExeOwner),
          "Cannot make probe fixture executable");
}

void expectImportedPixel(const TRasterImageP &image, int row,
                         const TPixel32 &expected) {
  require(image.getPointer(), "Missing imported image");
  TRaster32P raster(image->getRaster());
  require(raster.getPointer(), "Imported image is not RGBA");
  const TPixel32 actual = raster->pixels(row)[0];
  require(std::abs(int(actual.r) - expected.r) <= 12 &&
              std::abs(int(actual.g) - expected.g) <= 12 &&
              std::abs(int(actual.b) - expected.b) <= 12,
          "Imported image changed pixel channels or bottom-up row order");
}

void testMetadataProbeBatching(const QTemporaryDir &directory) {
  const QString source = directory.filePath("metadata-audio-first.mp4");
  QProcess mux;
  // Put audio first in the container: metadata must still select video v:0.
  mux.start(ffmpegExecutable(), {"-v", "error", "-y", "-f", "lavfi", "-i",
      "anullsrc=channel_layout=mono:sample_rate=48000", "-i",
      directory.filePath("pipe.mp4"), "-map", "0:a:0", "-map", "1:v:0",
      "-c:a", "aac", "-c:v", "copy", "-shortest", source});
  const bool finished = mux.waitForFinished(30000);
  if (!finished) {
    mux.kill();
    mux.waitForFinished(3000);
  }
  require(finished && mux.exitStatus() == QProcess::NormalExit && mux.exitCode() == 0,
          "Cannot create audio-first media fixture: " +
              mux.readAllStandardError().toStdString());

  QTemporaryDir tools;
  require(tools.isValid(), "Cannot create probe-count fixture directory");
  const QString calls = tools.filePath("probe-calls.txt");
  const QString realProbe = QDir(QString::fromUtf8(TIIO_FFMPEG_TEST_DIR))
                                .filePath("ffprobe");
  const QByteArray wrapper = ("#!/bin/sh\nprintf 'probe\\n' >> " +
      shellQuote(calls) + "\nexec " + shellQuote(realProbe) + " \"$@\"\n").toUtf8();
  writeExecutable(tools.filePath("ffprobe"), wrapper);
  require(QFile::link(ffmpegExecutable(), tools.filePath("ffmpeg")),
          "Cannot provide encoder beside probe-count fixture");
  Preferences::instance()->setValue(ffmpegPath, tools.path());
  const QStringList before = importGenerations();
  Ffmpeg reader;
  reader.setPath(TFilePath(source));
  const ffmpegFileInfo info = reader.getInfo();
  require(info.m_lx == kLx && info.m_ly == kLy && info.m_frameCount == kFrames &&
              std::abs(info.m_frameRate - 24.0) < 0.001,
          "Combined probe did not read the first video stream correctly");
  reader.getSize();
  reader.getFrameRate();
  reader.getFrameCount();
  require(readFile(calls).count('\n') == 1,
          "Complete container metadata needed more than one ffprobe process");
  reader.getFramesFromMovie();
  require(readFile(calls).count('\n') == 1,
          "Extraction probed a codec already returned with metadata");
  expectImportedPixel(reader.getImage(kFrames), kLy - 1, topPixel(kFrames - 1));
  expectImportedPixel(reader.getImage(kFrames), 0, bottomPixel());

  const QStringList added = newNames(before, importGenerations());
  require(added.size() == 1, "Probe fixture did not publish one cache generation");
  const QString base = ffmpegCache().filePath(added.front().chopped(7));
  QLockFile heldLock(base + ".lock");
  heldLock.setStaleLockTime(0);
  require(heldLock.tryLock(0), "Cannot hold extraction lock for cached-read test");
  Ffmpeg cached;
  cached.setPath(TFilePath(source));
  cached.getFramesFromMovie();
  require(cached.getImage(1).getPointer(),
          "Validated cached frames were blocked by the extraction lock");
  require(readFile(calls).count('\n') == 1,
          "A new reader ignored complete cached metadata");
  Preferences::instance()->setValue(ffmpegPath,
                                    QString::fromUtf8(TIIO_FFMPEG_TEST_DIR));
}

void testMissingContainerFrameCount(const QTemporaryDir &directory) {
  const QString source = directory.filePath("missing-container-count.mp4");
  require(QFile::copy(directory.filePath("pipe.mp4"), source),
          "Cannot create missing-count source");
  QTemporaryDir tools;
  require(tools.isValid(), "Cannot create missing-count fixture directory");
  const QString calls = tools.filePath("probe-calls.txt");
  const QString realProbe = QDir(QString::fromUtf8(TIIO_FFMPEG_TEST_DIR))
                                .filePath("ffprobe");
  const QString json = "{\"streams\":[{\"width\":320,\"height\":240,"
      "\"r_frame_rate\":\"0/0\",\"avg_frame_rate\":\"30000/1001\","
      "\"nb_frames\":\"N/A\",\"codec_name\":\"h264\"}]}";
  const QByteArray wrapper = ("#!/bin/sh\nprintf 'probe\\n' >> " +
      shellQuote(calls) + "\ncase \"$*\" in\n*nb_read_frames*) exec " +
      shellQuote(realProbe) + " \"$@\" ;;\n*) printf '%s\\n' " +
      shellQuote(json) + " ;;\nesac\n").toUtf8();
  writeExecutable(tools.filePath("ffprobe"), wrapper);
  require(QFile::link(ffmpegExecutable(), tools.filePath("ffmpeg")),
          "Cannot provide encoder for missing-count fixture");
  Preferences::instance()->setValue(ffmpegPath, tools.path());
  Ffmpeg reader;
  reader.setPath(TFilePath(source));
  const ffmpegFileInfo info = reader.getInfo();
  require(info.m_frameCount == kFrames &&
              std::abs(info.m_frameRate - 30000.0 / 1001.0) < 1e-10,
          "Decoded count or fractional average-rate fallback is incorrect");
  reader.getFramesFromMovie();
  require(reader.getImage(kFrames).getPointer(), "Missing-count source lost frames");
  require(readFile(calls).count('\n') == 2,
          "Missing count must need only metadata plus one decoded-count probe");
  Ffmpeg cached;
  cached.setPath(TFilePath(source));
  require(std::abs(cached.getInfo().m_frameRate - info.m_frameRate) < 1e-10,
          "Cached fractional rate lost precision");
  cached.getFramesFromMovie();
  require(readFile(calls).count('\n') == 2,
          "Cached missing-count metadata repeated decoding or codec probing");
  Preferences::instance()->setValue(ffmpegPath,
                                    QString::fromUtf8(TIIO_FFMPEG_TEST_DIR));
}

void testOwnedCleanup(const QTemporaryDir &directory) {
  const QString owned = directory.filePath("owned-frame.png");
  const QString removed = directory.filePath("already-removed.png");
  const QString sibling = directory.filePath("unrelated-frame.png");
  writeFile(owned, "partial PNG bytes");
  writeFile(removed, "complete PNG bytes");
  writeFile(sibling, "unrelated data");
  Ffmpeg writer;
  writer.addToCleanUp(owned);
  writer.addToCleanUp(owned);  // duplicate owned entries are harmless
  writer.addToCleanUp(removed);
  require(QFile::remove(removed), "Cannot remove owned cleanup fixture");
  writer.abort();
  writer.abort();
  require(!QFileInfo::exists(owned) && !QFileInfo::exists(removed),
          "Abort left an owned partial or missing file behind");
  require(readFile(sibling) == "unrelated data", "Cleanup removed an unrelated file");
}

void testLosslessImportPixels(const QTemporaryDir &directory) {
  Ffmpeg reader;
  reader.setPath(TFilePath(directory.filePath("pipe.apng")));
  reader.getFramesFromMovie();
  for (int frame = 0; frame < kFrames; ++frame) {
    const TRasterImageP image = reader.getImage(frame + 1);
    require(image.getPointer(), "Missing imported APNG frame");
    const TRaster32P raster(image->getRaster());
    require(raster.getPointer() &&
                raster->pixels(kLy - 1)[0] == topPixel(frame) &&
                raster->pixels(0)[0] == bottomPixel(),
            "Lossless import changed row order, channels or alpha");
  }
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
    replacement.setPath(TFilePath(directory.filePath("pipe.apng")));
    require(replacement.getInfo().m_frameCount == kFrames,
            "Cannot initialize path-switch reader");
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
  writeFile(executable, "#!/bin/sh\n"
      "if [ \"$1\" = \"-hide_banner\" ]; then\n"
      "  printf '%s\\n' '-fps_mode mode'\n  exit 0\nfi\nexit 23\n");
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
    testFrameSyncCapabilities();

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
    testMetadataProbeBatching(directory);
    testMissingContainerFrameCount(directory);
    testOwnedCleanup(directory);
    testLosslessImportPixels(directory);
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
