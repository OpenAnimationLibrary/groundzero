

#include "toonz/preferences.h"
#include "toonz/toonzfolders.h"

#include "tiio_ffmpeg.h"
#include "tsystem.h"
#include "tsound.h"
#include "timageinfo.h"
#include "toonz/stage.h"
#include "trop.h"

#include <QAbstractEventDispatcher>
#include <QDebug>
#include <QEventLoop>
#include <QElapsedTimer>
#include <QImage>
#include <QProcess>
#include <QQueue>
#include <QRegularExpression>
#include <QThread>
#include <QTimer>
#include <QWaitCondition>
#include <QUuid>
#include <QDir>
#include <QFile>
#include <QSaveFile>
#include <QTemporaryFile>
#include <QTemporaryDir>
#include <QLockFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QDateTime>
#include <QCryptographicHash>
#include <algorithm>
#include <cmath>
#include "tmsgcore.h"
#include "thirdparty.h"

Ffmpeg::Ffmpeg() {
  int userTimeoutSec = ThirdParty::getFFmpegTimeout();
  m_ffmpegTimeoutMs  = (userTimeoutSec > 0) ? userTimeoutSec * 1000 : 30000;

  m_intermediateFormat = "png";
  m_startNumber        = std::numeric_limits<int>::max();
  m_frameCount         = 0;
  m_lx = m_ly = m_bpp = 0;
  m_frameRate         = 0.0;
  m_hasSoundTrack     = false;
  m_sampleRate = m_channelCount = m_bitsPerSample = 0;
}

Ffmpeg::~Ffmpeg() {
  // Owners also destroy this object while unwinding a failed reader/writer
  // constructor. Cleanup must not replace that exception or terminate the app.
  try {
    finishPipe();
  } catch (...) {
    if (!m_abortRequested.load())
      qWarning() << "FFmpeg pipe finalization failed during cleanup";
  }
  try {
    cleanUpFiles();
  } catch (...) {
    qWarning() << "FFmpeg temporary-file cleanup failed";
  }
}

bool Ffmpeg::checkFormat(std::string format) {
  // Cache with reload every hour (avoids becoming outdated if ffmpeg changes)
  static QString cachedFormats;
  static QDateTime lastCheck;

  QDateTime now = QDateTime::currentDateTime();
  if (cachedFormats.isEmpty() || lastCheck.secsTo(now) > 3600) {  // 1 hour
    QStringList args;
    args << "-formats";

    QProcess ffmpeg;
    ThirdParty::runFFmpeg(ffmpeg, args);
    ffmpeg.waitForFinished(8000);

    cachedFormats =
        ffmpeg.readAllStandardError() + ffmpeg.readAllStandardOutput();
    ffmpeg.close();

    lastCheck = now;
  }

  return cachedFormats.contains(QString::fromStdString(format),
                                Qt::CaseInsensitive);
}

TFilePath Ffmpeg::getFfmpegCache() const {
  QString cacheRoot = ToonzFolder::getCacheRootFolder().getQString();
  TFilePath cachePath(cacheRoot + "/ffmpeg");
  if (!TSystem::doesExistFileOrLevel(cachePath)) {
    try {
      TSystem::mkDir(cachePath);
    } catch (const TSystemException &e) {
      DVGui::warning(QObject::tr("Cannot create FFmpeg cache directory: %1")
                         .arg(QString::fromStdWString(e.getMessage())));
    }
  }
  return cachePath;
}

void Ffmpeg::setFrameRate(double fps) { m_frameRate = fps; }

void Ffmpeg::setPath(const TFilePath &path) {
  m_path = path;
  m_tempBaseName = "export_" + QUuid::createUuid().toString(QUuid::WithoutBraces);
  m_importCacheBase.clear();
}

QString Ffmpeg::exportOutputPath() {
  if (!m_exportOutputPath.isEmpty()) return m_exportOutputPath;
  const QFileInfo destination(m_path.getQString());
  QTemporaryFile output(destination.dir().filePath(
      ".groundzero-XXXXXX." + destination.suffix()));
  if (!output.open())
    throw TImageException(m_path, "Cannot create temporary export output");
  m_exportOutputPath = output.fileName();
  m_cleanUpList.push_back(m_exportOutputPath);
  output.setAutoRemove(false);
  return m_exportOutputPath;
}

void Ffmpeg::createIntermediateImage(const TImageP &img, int frameIndex) {
  if (m_finalizationAttempted || m_abortRequested.load())
    throw TImageException(m_path, "Cannot save a frame after finalization");
  int idx = frameIndex - 1;

  // Export-specific prefix + zero-padded 6-digit index (ffmpeg %06d)
  QString tempFileName = m_tempBaseName + "_in" +
                         QString("%1").arg(idx, 6, 10, QChar('0')) + "." +
                         m_intermediateFormat;

  QString tempPath =
      getFfmpegCache().getQString() + QDir::separator() + tempFileName;

  TRasterImageP tempImage(img);
  if (!tempImage)
    throw TImageException(m_path, "Cannot save non-raster image to FFmpeg");

  TRasterP raster = tempImage->getRaster();
  if (!raster || raster->getLx() <= 0 || raster->getLy() <= 0)
    throw TImageException(m_path, "Invalid or empty raster for FFmpeg");

  const int lx  = raster->getLx();
  const int ly  = raster->getLy();
  const int bpp = raster->getPixelSize();

  TRaster32P raster32;

  if (bpp == 4) {
    // Already 32-bit integer fast path, clone and mirror
    raster32 = raster->clone();
    if (!raster32)
      throw TImageException(m_path, "Failed to clone raster for FFmpeg");
    raster32->yMirror();  // Mirror only once here
  } else {
    // FP32 or 64-bit convert safely
    raster32 = TRaster32P(lx, ly);
    if (!raster32)
      throw TImageException(m_path, "Failed to allocate raster for FFmpeg");

    // float (16 bytes/pixel), do clamp
    if (bpp == 16) {
      TRasterPT<TPixelF> rasterFloat(raster);
      if (rasterFloat) {
        rasterFloat->lock();
        for (int y = 0; y < ly; ++y) {
          TPixelF *pix = rasterFloat->pixels(y);
          for (int x = 0; x < lx; ++x, ++pix) {
            pix->r = tcrop(pix->r, 0.0f, 1.0f);
            pix->g = tcrop(pix->g, 0.0f, 1.0f);
            pix->b = tcrop(pix->b, 0.0f, 1.0f);
            pix->m = tcrop(pix->m, 0.0f, 1.0f);
          }
        }
        rasterFloat->unlock();
      }
    }

    // Convert to 32-bit ARGB
    try {
      TRop::convert(raster32, raster);
    } catch (...) {
      throw TImageException(m_path, "Failed to convert raster for FFmpeg");
    }
    raster32->yMirror();  // Mirror only once here
  }

  QImage qi(raster32->getRawData(), lx, ly, raster32->getWrap() * 4,
            QImage::Format_ARGB32);

  if (qi.isNull())
    throw TImageException(m_path, "Failed to create intermediate image");
  // qi = qi.copy();
  // Currently redundant,  removed for performance & clarity.
  // If raster-to-QImage sharing is needed in the future, we can revisit.
  QByteArray formatBa = m_intermediateFormat.toUpper().toLatin1();
  const char *format  = formatBa.constData();

  if (!qi.save(tempPath, format, -1))
    throw TImageException(m_path, "Failed to save intermediate image");

  QMutexLocker locker(&m_intermediateMutex);
  ++m_frameCount;
  m_startNumber = std::min(m_startNumber, idx);
  m_lx = lx;
  m_ly = ly;
  m_bpp = bpp;
  m_cleanUpList.push_back(tempPath);
}

void Ffmpeg::runFfmpeg(const QStringList &preInputArgs,
                       const QStringList &postInputArgs, bool inputPathIncluded,
                       bool outputPathIncluded, bool overwrite,
                       bool asynchronous) {
  // Pattern with %06d to exactly match the generated names
  QString tempName = getFfmpegCache().getQString() + QDir::separator() +
                     m_tempBaseName + "_in%06d." + m_intermediateFormat;

  QStringList args = preInputArgs;

  if (!inputPathIncluded) {
    if (m_startNumber != 0 &&
        m_startNumber != std::numeric_limits<int>::max()) {
      args << "-start_number" << QString::number(m_startNumber);
    }
    args << "-i" << tempName;
  }

  if (m_hasSoundTrack) args.append(m_audioArgs);
  args.append(postInputArgs);

  if (overwrite && !outputPathIncluded) args << "-y";

  if (!outputPathIncluded) args << exportOutputPath();

  QProcess ffmpeg;
  ThirdParty::runFFmpeg(ffmpeg, args);

  bool success = waitFfmpeg(ffmpeg, asynchronous);
  if (!success) {
    const std::string details =
        ffmpeg.readAllStandardError().right(4096).toStdString();
    throw TImageException(m_path, "FFmpeg failed or timed out: " + details);
  }

  ffmpeg.close();
}

void Ffmpeg::runFfmpeg(const QStringList &preInputArgs,
                       const QStringList &postInputArgs,
                       const TFilePath &outputPath) {
  setPath(outputPath);
  runFfmpeg(preInputArgs, postInputArgs, false, false, true, true);
}

QString Ffmpeg::runFfprobe(const QStringList &args) const {
  QProcess proc;
  ThirdParty::runFFprobe(proc, args);

  if (!waitFfmpeg(proc, false)) {
    throw TImageException(m_path, "error accessing ffprobe.");
  }

  QString results = proc.readAllStandardError() + proc.readAllStandardOutput();
  int exitCode    = proc.exitCode();
  proc.close();

  if (exitCode > 0) {
    throw TImageException(m_path, "error reading info.");
  }

  return results;
}

bool Ffmpeg::waitFfmpeg(QProcess &process, bool async) const {
  QTimer cancelTimer;
  if (async) {
    QObject::connect(&cancelTimer, &QTimer::timeout, &process, [&] {
      if (m_abortRequested.load()) process.kill();
    });
    cancelTimer.start(100);
  }
  const bool finished = async
      ? ThirdParty::waitAsyncProcess(process, m_ffmpegTimeoutMs) == 0
      : process.waitForFinished(m_ffmpegTimeoutMs);
  if (!finished && process.state() != QProcess::NotRunning) {
    process.kill();
    process.waitForFinished(3000);
  }
  return finished && process.exitStatus() == QProcess::NormalExit &&
         process.exitCode() == 0;
}

//===========================================================
//
//  Raw video pipe
//
//===========================================================

namespace {

// Frames waiting for the pipe thread, besides the one it is writing
const int kPipeMaxQueuedFrames = 2;
// Bytes handed to QProcess at once: its write buffer holds at most twice this
const qint64 kPipeChunkBytes = 1 << 20;
// Time given to ffmpeg to exit by itself after a failed write
const int kPipeExitGraceMs = 3000;

// ffmpeg name of the in-memory TPixel32 channel order (see tpixel.h)
const char *pipePixelFormat() {
#if defined(TNZ_MACHINE_CHANNEL_ORDER_BGRM)
  return "bgra";
#elif defined(TNZ_MACHINE_CHANNEL_ORDER_MBGR)
  return "abgr";
#elif defined(TNZ_MACHINE_CHANNEL_ORDER_RGBM)
  return "rgba";
#else  // TNZ_MACHINE_CHANNEL_ORDER_MRGB
  return "argb";
#endif
}

// Converts a frame to the layout ffmpeg reads from the pipe: lx * ly pixels
// in TPixel32 memory order, rows from top to bottom (OpenToonz rasters are
// bottom-up). Pixel values are passed through: like the intermediate PNG
// path (a QImage::Format_ARGB32 over the same premultiplied pixels), ffmpeg
// receives them as straight alpha. A frame of another size is centered on a
// transparent frame of the stream size.
QByteArray makePipeFrame(const TRasterP &raster, int lx, int ly) {
  TRaster32P raster32 = raster;
  if (!raster32) {
    raster32 = TRaster32P(raster->getLx(), raster->getLy());
    // float (16 bytes/pixel), clamp like createIntermediateImage()
    if (raster->getPixelSize() == 16) {
      TRasterPT<TPixelF> rasterFloat(raster);
      if (rasterFloat) {
        rasterFloat->lock();
        for (int y = 0; y < rasterFloat->getLy(); ++y) {
          TPixelF *pix = rasterFloat->pixels(y);
          for (int x = 0; x < rasterFloat->getLx(); ++x, ++pix) {
            pix->r = tcrop(pix->r, 0.0f, 1.0f);
            pix->g = tcrop(pix->g, 0.0f, 1.0f);
            pix->b = tcrop(pix->b, 0.0f, 1.0f);
            pix->m = tcrop(pix->m, 0.0f, 1.0f);
          }
        }
        rasterFloat->unlock();
      }
    }
    TRop::convert(raster32, raster);
  }

  if (raster32->getLx() != lx || raster32->getLy() != ly) {
    qWarning() << "FFmpeg pipe: frame size" << raster32->getLx() << "x"
               << raster32->getLy() << "differs from the stream size" << lx
               << "x" << ly;
    TRaster32P canvas(lx, ly);
    canvas->clear();
    canvas->copy(raster32, TPoint((lx - raster32->getLx()) / 2,
                                  (ly - raster32->getLy()) / 2));
    raster32 = canvas;
  }

  const int rowBytes = lx * 4;
  QByteArray frame(rowBytes * ly, Qt::Uninitialized);
  char *dst = frame.data();
  raster32->lock();
  for (int y = ly - 1; y >= 0; --y, dst += rowBytes)
    memcpy(dst, raster32->pixels(y), rowBytes);
  raster32->unlock();
  return frame;
}

}  // namespace

// Runs ffmpeg for a raw video pipe on a thread of its own. QProcess must be
// driven by a single thread (on Windows its pipe I/O completes on the thread
// that issued it), while frames are saved from rendering threads: this thread
// starts ffmpeg, writes the queued frames to its standard input, closes it and
// waits for ffmpeg to exit. It needs no event loop: QProcess::waitFor*() work
// without one.
class FfmpegPipeWriter final : public QThread {
public:
  enum Result { Running, Succeeded, Failed, TimedOut };

  FfmpegPipeWriter(const QStringList &args, int timeoutMs,
                   const QString &logPath, const std::atomic<bool> &aborted)
      : m_args(args), m_timeoutMs(timeoutMs), m_logPath(logPath),
        m_abortRequested(aborted) {}

  // Queues a frame (QByteArray is shared, not copied). Blocks while the
  // backlog is full, so memory stays bounded when ffmpeg is slower than the
  // renderer. Returns false, dropping the frame, once ffmpeg has stopped.
  bool push(const QByteArray &frame) {
    QMutexLocker locker(&m_mutex);
    while (!m_stopped && m_queue.size() >= kPipeMaxQueuedFrames)
      m_frameTaken.wait(&m_mutex);
    if (m_stopped) return false;
    m_queue.enqueue(frame);
    m_frameQueued.wakeOne();
    return true;
  }

  // No more frames: standard input is closed once the backlog is written.
  void finish() {
    QMutexLocker locker(&m_mutex);
    m_finishing = true;
    m_frameQueued.wakeOne();
  }

  void cancel() {
    QMutexLocker locker(&m_mutex);
    m_canceled = m_stopped = m_finishing = true;
    m_queue.clear();
    m_frameQueued.wakeAll();
    m_frameTaken.wakeAll();
  }

  // Valid once the thread has finished
  Result result() const { return m_result; }
  int exitCode() const { return m_exitCode; }  // -1 if ffmpeg crashed

protected:
  void run() override {
    QProcess process;
    // No output pipes: nothing has to read them while frames are written, so
    // ffmpeg cannot block on a full stdout/stderr pipe. Its log goes to a file.
    process.setStandardOutputFile(QProcess::nullDevice());
    process.setStandardErrorFile(m_logPath);
    ThirdParty::runFFmpeg(process, m_args);

    Result result = process.waitForStarted(m_timeoutMs) ? Running : Failed;
    while (result == Running) {
      QByteArray frame;
      {
        QMutexLocker locker(&m_mutex);
        while (m_queue.isEmpty() && !m_finishing && !m_abortRequested.load())
          m_frameQueued.wait(&m_mutex, 100);
        if (m_canceled || m_abortRequested.load()) {
          result = Failed;
          break;
        }
        if (m_queue.isEmpty()) break;  // finishing, every frame written
        frame = m_queue.dequeue();
        m_frameTaken.wakeAll();
      }
      result = writeFrame(process, frame);
    }

    if (result == Running) result = drain(process, 0);
    if (result == Running) {
      // EOF makes ffmpeg flush its encoders and finalize the file
      process.closeWriteChannel();
      result = waitForExit(process);
    }
    if (canceled() && process.state() != QProcess::NotRunning) process.kill();
    if (process.state() != QProcess::NotRunning &&
        (result == TimedOut || !process.waitForFinished(kPipeExitGraceMs))) {
      process.kill();
      process.waitForFinished(kPipeExitGraceMs);
    }

    const bool normalExit = process.exitStatus() == QProcess::NormalExit;
    if (result == Running)
      result = (normalExit && process.exitCode() == 0) ? Succeeded : Failed;

    QMutexLocker locker(&m_mutex);
    m_result   = result;
    m_exitCode = normalExit ? process.exitCode() : -1;
    m_stopped  = true;
    m_queue.clear();
    m_frameTaken.wakeAll();
  }

private:
  bool canceled() {
    QMutexLocker locker(&m_mutex);
    return m_canceled || m_abortRequested.load();
  }

  Result waitForExit(QProcess &process) {
    QElapsedTimer timer;
    timer.start();
    while (process.state() != QProcess::NotRunning) {
      if (canceled()) return Failed;
      const qint64 remaining = m_timeoutMs - timer.elapsed();
      if (remaining <= 0) return TimedOut;
      if (process.waitForFinished(int(qMin<qint64>(100, remaining)))) break;
      if (process.state() == QProcess::NotRunning) break;
      if (process.error() != QProcess::Timedout) return Failed;
    }
    return Running;  // The caller checks the actual process exit status.
  }

  // Waits until at most maxPending bytes are left in QProcess' write buffer
  Result drain(QProcess &process, qint64 maxPending) {
    QElapsedTimer timer;
    timer.start();
    while (process.bytesToWrite() > maxPending) {
      if (canceled()) return Failed;
      const qint64 remaining = m_timeoutMs - timer.elapsed();
      if (remaining <= 0) return TimedOut;
      if (process.waitForBytesWritten(int(qMin<qint64>(100, remaining)))) {
        timer.restart();
      } else if (process.state() == QProcess::NotRunning ||
                 process.error() != QProcess::Timedout) {
        return Failed;
      }
    }
    return Running;
  }

  Result writeFrame(QProcess &process, const QByteArray &frame) {
    for (qint64 offset = 0; offset < frame.size();) {
      if (canceled()) return Failed;
      const qint64 size = qMin<qint64>(kPipeChunkBytes, frame.size() - offset);
      if (process.write(frame.constData() + offset, size) != size)
        return Failed;
      offset += size;
      // QProcess buffers whatever ffmpeg has not read yet: keep it bounded
      Result result = drain(process, kPipeChunkBytes);
      if (result != Running) return result;
    }
    return Running;
  }

  const QStringList m_args;
  const int m_timeoutMs;
  const QString m_logPath;
  const std::atomic<bool> &m_abortRequested;

  QMutex m_mutex;
  QWaitCondition m_frameQueued;
  QWaitCondition m_frameTaken;
  QQueue<QByteArray> m_queue;
  bool m_finishing = false;
  bool m_stopped   = false;
  bool m_canceled  = false;
  Result m_result  = Running;
  int m_exitCode   = 0;
};

void Ffmpeg::enablePipe(bool alwaysSequential) {
  // MovieRenderer saves movie frames one at a time and in order, unless the
  // "FFmpeg multithread" preference lets it save them concurrently (formats
  // that do not require sequential saving). OTDEV_FFMPEG_NO_PIPE=1 keeps the
  // intermediate images, e.g. to compare both paths.
  m_pipeEnabled =
      (alwaysSequential || !Preferences::instance()->getFfmpegMultiThread()) &&
      qEnvironmentVariableIntValue("OTDEV_FFMPEG_NO_PIPE") == 0;
}

void Ffmpeg::startPipe(int lx, int ly, const ArgsBuilder &buildArgs) {
  m_lx = lx;
  m_ly = ly;

  QStringList preInputArgs;
  QStringList postInputArgs;
  buildArgs(preInputArgs, postInputArgs);

  // Same layout as runFfmpeg(), with the video input read from stdin
  QStringList args;
  args << "-f"
       << "rawvideo"
       << "-pix_fmt" << pipePixelFormat() << "-video_size"
       << QString("%1x%2").arg(lx).arg(ly);
  args << preInputArgs << "-i"
       << "-";
  if (m_hasSoundTrack) args << m_audioArgs;
  args << postInputArgs << "-y" << exportOutputPath();

  m_pipeLogPath = getFfmpegCache().getQString() + QDir::separator() +
                  m_tempBaseName + "_pipe.log";
  m_pipe = new FfmpegPipeWriter(args, m_ffmpegTimeoutMs, m_pipeLogPath,
                               m_abortRequested);
  m_pipe->start();
}

void Ffmpeg::saveFrame(const TImageP &img, int frameIndex,
                       const ArgsBuilder &buildArgs) {
  // m_pipeEnabled only changes before the first streamed frame, and frames
  // are then saved one at a time
  if (!m_pipeEnabled) {
    createIntermediateImage(img, frameIndex);
    return;
  }

  QMutexLocker locker(&m_pipeMutex);
  if (m_finalizationAttempted || m_abortRequested.load())
    throw TImageException(m_path, "Cannot save a frame after finalization");

  TRasterImageP rasterImage(img);
  if (!rasterImage)
    throw TImageException(m_path, "Cannot save non-raster image to FFmpeg");
  TRasterP raster = rasterImage->getRaster();
  if (!raster || raster->getLx() <= 0 || raster->getLy() <= 0)
    throw TImageException(m_path, "Invalid or empty raster for FFmpeg");

  if (!m_pipe) {
    if (qint64(raster->getLx()) * raster->getLy() * 4 >
        std::numeric_limits<int>::max()) {
      // Too large for one QByteArray: use intermediate images
      m_pipeEnabled = false;
      locker.unlock();
      createIntermediateImage(img, frameIndex);
      return;
    }
    startPipe(raster->getLx(), raster->getLy(), buildArgs);
    m_pipeNextFrame = frameIndex;
  }

  // Frames already streamed cannot be revisited
  if (frameIndex < m_pipeNextFrame) {
    throw TImageException(m_path, "FFmpeg received an out-of-order frame");
  }

  QByteArray frame = makePipeFrame(raster, m_lx, m_ly);
  if (frame.isEmpty())
    throw TImageException(m_path, "Failed to convert frame for FFmpeg");

  // A gap (e.g. a render step) repeats the previous frame, keeping the timing.
  // (The image sequence read by the intermediate image path ends at the first
  // missing image instead.) push() returns false once ffmpeg failed, which
  // finishPipe() reports.
  for (; m_pipeNextFrame < frameIndex && !m_pipeLastFrame.isEmpty();
       ++m_pipeNextFrame)
    if (!m_pipe->push(m_pipeLastFrame))
      throw TImageException(m_path, "FFmpeg stopped accepting frames");
  m_pipeNextFrame = frameIndex + 1;
  m_pipeLastFrame = frame;
  if (!m_pipe->push(frame))
    throw TImageException(m_path, "FFmpeg stopped accepting frames");
  m_frameCount++;
}

bool Ffmpeg::finishPipe() {
  FfmpegPipeWriter *pipe = nullptr;
  {
    QMutexLocker locker(&m_pipeMutex);
    pipe   = m_pipe;
    m_pipe = nullptr;
    m_pipeLastFrame.clear();
  }
  if (!pipe) return false;

  pipe->finish();
  // Keep the event loop running while ffmpeg encodes the last frames, as
  // waitFfmpeg() does for the intermediate image path
  if (QAbstractEventDispatcher::instance()) {
    QEventLoop eloop;
    QObject::connect(pipe, &QThread::finished, &eloop, &QEventLoop::quit);
    if (!pipe->isFinished()) eloop.exec();
  }
  pipe->wait();

  const FfmpegPipeWriter::Result result = pipe->result();
  delete pipe;

  if (result == FfmpegPipeWriter::Succeeded) {
    m_cleanUpList.push_back(m_pipeLogPath);
    return true;
  }

  QFile log(m_pipeLogPath);
  std::string details;
  if (log.open(QIODevice::ReadOnly)) {
    if (log.size() > 4096) log.seek(log.size() - 4096);
    details = log.readAll().toStdString();
  }
  throw TImageException(m_path,
      (result == FfmpegPipeWriter::TimedOut ? "FFmpeg timed out: "
                                          : "FFmpeg encoding failed: ") +
      details);
}

void Ffmpeg::finalize(const ArgsBuilder &buildArgs) {
  if (m_abortRequested.load())
    throw TImageException(m_path, "FFmpeg export was aborted");
  if (m_finalizationAttempted) {
    if (m_finalizationError) std::rethrow_exception(m_finalizationError);
    return;
  }
  m_finalizationAttempted = true;
  try {
    // Opening a writer for capabilities alone must not launch an encoder.
    if (m_frameCount == 0 && !m_pipe) return;
    if (!finishPipe()) {
      QStringList preInputArgs, postInputArgs;
      buildArgs(preInputArgs, postInputArgs);
      runFfmpeg(preInputArgs, postInputArgs);
    }

    if (m_abortRequested.load())
      throw TImageException(m_path, "FFmpeg export was aborted");

    QFile encoded(m_exportOutputPath);
    if (!encoded.open(QIODevice::ReadOnly) || encoded.size() <= 0)
      throw TImageException(m_path, "FFmpeg produced no output");
    QSaveFile destination(m_path.getQString());
    destination.setDirectWriteFallback(false);
    if (!destination.open(QIODevice::WriteOnly))
      throw TImageException(m_path, "Cannot open export destination");
    while (!encoded.atEnd()) {
      if (m_abortRequested.load())
        throw TImageException(m_path, "FFmpeg export was aborted");
      const QByteArray chunk = encoded.read(1024 * 1024);
      if (chunk.isEmpty() || destination.write(chunk) != chunk.size())
        throw TImageException(m_path, "Cannot publish complete export output");
    }
    if (m_abortRequested.load())
      throw TImageException(m_path, "FFmpeg export was aborted");
    if (!destination.commit())
      throw TImageException(m_path, "Cannot commit export output");
    encoded.close();
    cleanUpFiles();
  } catch (...) {
    m_finalizationError = std::current_exception();
    throw;
  }
}

void Ffmpeg::abort() noexcept {
  if (m_aborted) return;
  m_aborted = true;
  requestAbort();
  m_finalizationAttempted = true;
  try {
    // The process belongs to the pipe thread. Request cancellation there,
    // then join before removing any of its output or audio files.
    {
      QMutexLocker locker(&m_pipeMutex);
      if (m_pipe) m_pipe->cancel();
    }
    finishPipe();
  } catch (...) {
    // Cancellation intentionally causes a non-successful encoder exit.
  }
  try {
    if (!m_pipeLogPath.isEmpty()) QFile::remove(m_pipeLogPath);
    cleanUpFiles();
  } catch (...) {
    qWarning() << "FFmpeg aborted export cleanup failed";
  }
}

void Ffmpeg::saveSoundTrack(TSoundTrack *soundTrack) {
  if (!soundTrack) return;

  {
    // The audio input is part of the command started on the first frame
    QMutexLocker locker(&m_pipeMutex);
    if (m_pipe || m_finalizationAttempted || m_abortRequested.load())
      throw TImageException(m_path, "Cannot add sound after encoding has started");
  }

  m_sampleRate        = soundTrack->getSampleRate();
  m_channelCount      = soundTrack->getChannelCount();
  m_bitsPerSample     = soundTrack->getBitPerSample();
  const qint64 bufSize = qint64(soundTrack->getSampleCount()) *
                         soundTrack->getSampleSize();
  const UCHAR *buffer = soundTrack->getRawData();
  if (bufSize == 0) return;
  if (bufSize < 0 || !buffer)
    throw TImageException(m_path, "Invalid sound track for FFmpeg");

  m_audioPath = getFfmpegCache().getQString() + QDir::separator() +
                m_tempBaseName + "_in.raw";

  m_audioFormat = ((soundTrack->getSampleType() == TSound::FLOAT) ? "f" : "s") +
                  QString::number(m_bitsPerSample);
  if (m_bitsPerSample > 8) m_audioFormat.append("le");

  // Write directly from the sound track: avoid a second full audio buffer and
  // preserve the 64-bit byte count for long clips.
  QSaveFile file(m_audioPath);
  if (!file.open(QIODevice::WriteOnly) ||
      file.write(reinterpret_cast<const char *>(buffer), bufSize) != bufSize ||
      !file.commit())
    throw TImageException(m_path, "Cannot save complete audio input for FFmpeg");
  m_hasSoundTrack = true;

  m_audioArgs = QStringList{"-f",  m_audioFormat,
                            "-ar", QString::number(m_sampleRate),
                            "-ac", QString::number(m_channelCount),
                            "-i",  m_audioPath};

  m_cleanUpList.push_back(m_audioPath);
}

QString Ffmpeg::importCacheBase() const {
  if (!m_importCacheBase.isEmpty()) return m_importCacheBase;
  QFile source(m_path.getQString());
  if (!source.open(QIODevice::ReadOnly))
    throw TImageException(m_path, "Cannot read video source for cache identity");
  QCryptographicHash hash(QCryptographicHash::Sha256);
  static const char version[] = "groundzero-ffmpeg-import-v2";
  hash.addData(version, sizeof(version));
  const QFileInfo info(source);
  const QString canonical = info.canonicalFilePath().isEmpty()
      ? info.absoluteFilePath() : info.canonicalFilePath();
  hash.addData(canonical.toUtf8());
  hash.addData("\0", 1);
  if (!hash.addData(&source))
    throw TImageException(m_path, "Cannot fingerprint video source");
  m_importCacheBase = getFfmpegCache().getQString() + "/import-v2-" +
                      QString::fromLatin1(hash.result().toHex());
  return m_importCacheBase;
}

bool Ffmpeg::hasCompleteImportCache() const {
  const QDir frames(importCacheBase() + ".frames");
  QFile manifest(frames.filePath("complete.json"));
  if (!manifest.open(QIODevice::ReadOnly)) return false;
  const QJsonObject object = QJsonDocument::fromJson(manifest.readAll()).object();
  const QJsonArray sizes = object.value("sizes").toArray();
  if (object.value("version").toInt() != 2 || sizes.size() != m_frameCount ||
      m_frameCount <= 0) return false;
  for (int frame = 1; frame <= m_frameCount; ++frame) {
    const QFileInfo image(frames.filePath(
        QString("frame%1.png").arg(frame, 6, 10, QChar('0'))));
    const qint64 expected = sizes[frame - 1].toString().toLongLong();
    if (expected <= 0 || !image.isFile() || image.size() != expected) return false;
  }
  return true;
}

bool Ffmpeg::checkFilesExist() const { return hasCompleteImportCache(); }

// ------------------------------------------------------------
// Cached getters
// ------------------------------------------------------------

ffmpegFileInfo Ffmpeg::getInfo() const {
  // If already cached, return immediately
  if (m_infoCached) {
    return ffmpegFileInfo{m_lx, m_ly, m_frameCount, m_frameRate};
  }

  const QString tempPath = importCacheBase() + ".txt";

  if (QFile::exists(tempPath)) {
    QFile infoText(tempPath);
    if (infoText.open(QIODevice::ReadOnly | QIODevice::Text)) {
      QByteArray ba = infoText.readAll();
      infoText.close();
      QString text         = QString::fromUtf8(ba.constData(), ba.size());
      QStringList textlist = text.split(" ", Qt::SkipEmptyParts);
      if (textlist.size() >= 4) {
        m_lx         = textlist[0].toInt();
        m_ly         = textlist[1].toInt();
        m_frameRate  = textlist[2].toDouble();
        m_frameCount = textlist[3].toInt();
        if (m_lx > 0 && m_ly > 0 && m_frameCount > 0 &&
            std::isfinite(m_frameRate) && m_frameRate > 0.0) {
          m_infoCached = true;
          return ffmpegFileInfo{m_lx, m_ly, m_frameCount, m_frameRate};
        }
        m_lx = m_ly = m_frameCount = 0;
        m_frameRate = 0.0;
      }
    }
  }

  // Cache file missing or invalid – query ffprobe directly
  QStringList failedOperations;
  QString errorDetails;

  try {
    (void)getSize();  // will update m_lx, m_ly
  } catch (const TImageException &e) {
    failedOperations.append("size");
    errorDetails += QObject::tr("Size error: %1\n")
                        .arg(QString::fromStdWString(e.getMessage()));
  }

  try {
    (void)getFrameRate();  // will update m_frameRate
  } catch (const TImageException &e) {
    failedOperations.append("frame rate");
    errorDetails += QObject::tr("Frame rate error: %1\n")
                        .arg(QString::fromStdWString(e.getMessage()));
  }

  try {
    (void)getFrameCount();  // will update m_frameCount
  } catch (const TImageException &e) {
    failedOperations.append("frame count");
    errorDetails += QObject::tr("Frame count error: %1\n")
                        .arg(QString::fromStdWString(e.getMessage()));
  }

  if (!failedOperations.isEmpty() || m_lx <= 0 || m_ly <= 0 ||
      m_frameCount <= 0 || !std::isfinite(m_frameRate) || m_frameRate <= 0.0)
    throw TImageException(m_path, "Cannot read valid video metadata: " +
                                    errorDetails.toStdString());

  // ffprobe opens the source independently. Do not attach metadata from a
  // replacement file to the content fingerprint captured before probing.
  Ffmpeg currentSource;
  currentSource.setPath(m_path);
  if (currentSource.importCacheBase() != m_importCacheBase) {
    m_lx = m_ly = m_frameCount = 0;
    m_frameRate = 0.0;
    m_importCacheBase.clear();
    throw TImageException(m_path, "Video source changed while reading metadata");
  }

  QSaveFile infoText(tempPath);
  if (infoText.open(QIODevice::WriteOnly | QIODevice::Text)) {
    const QByteArray bytes = QString("%1 %2 %3 %4\n")
        .arg(m_lx).arg(m_ly).arg(m_frameRate, 0, 'g', 17)
        .arg(m_frameCount).toUtf8();
    if (infoText.write(bytes) == bytes.size()) infoText.commit();
  }

  m_infoCached = true;
  return ffmpegFileInfo{m_lx, m_ly, m_frameCount, m_frameRate};
}

double Ffmpeg::getFrameRate() const {
  if (m_frameRate > 0.0) return m_frameRate;

  QStringList fpsArgs;
  fpsArgs << "-v" << "error" << "-select_streams" << "v:0" << "-show_entries"
          << "stream=r_frame_rate" << "-of"
          << "default=noprint_wrappers=1:nokey=1" << m_path.getQString();

  QString fpsResults;
  try {
    fpsResults = runFfprobe(fpsArgs);
  } catch (const TImageException &) {
    fpsResults = "";
  }

  int fpsNum = 0, fpsDen = 0;
  if (!fpsResults.isEmpty()) {
    QStringList fpsResultsList = fpsResults.split('/');
    if (fpsResultsList.size() > 1) {
      fpsNum = fpsResultsList[0].toInt();
      fpsDen = fpsResultsList[1].toInt();
    }
  }

  if (fpsNum <= 0 || fpsDen <= 0) {
    fpsArgs.clear();
    fpsArgs << "-v" << "error" << "-select_streams" << "v:0" << "-show_entries"
            << "stream=avg_frame_rate" << "-of"
            << "default=noprint_wrappers=1:nokey=1" << m_path.getQString();

    try {
      fpsResults = runFfprobe(fpsArgs);
    } catch (const TImageException &) {
      fpsResults = "";
    }

    if (!fpsResults.isEmpty()) {
      QStringList fpsResultsList = fpsResults.split('/');
      if (fpsResultsList.size() > 1) {
        fpsNum = fpsResultsList[0].toInt();
        fpsDen = fpsResultsList[1].toInt();
      }
    }
  }

  m_frameRate = (fpsDen > 0) ? static_cast<double>(fpsNum) / fpsDen : 0.0;
  return m_frameRate;
}

TDimension Ffmpeg::getSize() const {
  if (m_lx > 0 && m_ly > 0) return TDimension(m_lx, m_ly);

  QStringList sizeArgs;
  sizeArgs << "-v" << "error" << "-of" << "flat=s=_" << "-select_streams"
           << "v:0" << "-show_entries" << "stream=height,width"
           << m_path.getQString();

  QString sizeResults;
  try {
    sizeResults = runFfprobe(sizeArgs);
  } catch (const TImageException &) {
    sizeResults = "";
  }

  if (!sizeResults.isEmpty()) {
    QStringList lines = sizeResults.split('\n', Qt::SkipEmptyParts);
    for (const QString &line : lines) {
      if (line.contains("width=")) {
        m_lx = line.section('=', 1, 1).toInt();
      } else if (line.contains("height=")) {
        m_ly = line.section('=', 1, 1).toInt();
      }
    }
  }

  return TDimension(m_lx, m_ly);
}

int Ffmpeg::getFrameCount() const {
  if (m_frameCount > 0) return m_frameCount;
  // Containers such as MP4 provide an exact frame count without decoding.
  // Only count decoded frames when that field is absent; never estimate from
  // duration * rate, which can truncate or invent frames for variable rates.
  QStringList args = {"-v", "error", "-select_streams", "v:0", "-show_entries",
      "stream=nb_frames", "-of", "default=nokey=1:noprint_wrappers=1",
      m_path.getQString()};
  bool valid = false;
  int count = runFfprobe(args).trimmed().toInt(&valid);
  if (!valid || count <= 0) {
    args = QStringList{"-v", "error", "-count_frames", "-select_streams", "v:0",
        "-show_entries", "stream=nb_read_frames", "-of",
        "default=nokey=1:noprint_wrappers=1", m_path.getQString()};
    count = runFfprobe(args).trimmed().toInt(&valid);
  }
  if (!valid || count <= 0)
    throw TImageException(m_path, "Cannot count decoded video frames");
  m_frameCount = count;
  return m_frameCount;
}

// ------------------------------------------------------------
// Frame extraction and retrieval
// ------------------------------------------------------------

void Ffmpeg::getFramesFromMovie(int frame) {
  getInfo();
  if (frame != -1 && (frame < 1 || frame > m_frameCount))
    throw TImageException(m_path, "Video frame index is out of range");
  const QString base = importCacheBase();
  QLockFile lock(base + ".lock");
  // A live decoder may legitimately run longer than QLockFile's 30s default.
  // Disable age expiry; QLockFile still detects a dead owning process.
  lock.setStaleLockTime(0);
  if (!lock.tryLock(m_ffmpegTimeoutMs))
    throw TImageException(m_path, "Video cache is busy");
  if (hasCompleteImportCache()) return;

  // A failed extraction must never publish a reusable first frame. Build an
  // entire generation separately, then publish it with a completion manifest.
  QTemporaryDir pending(base + ".partial-XXXXXX");
  if (!pending.isValid())
    throw TImageException(m_path, "Cannot create video extraction directory");
  const QStringList probeArgs = {"-v", "error", "-select_streams", "v:0",
      "-show_entries", "stream=codec_name", "-of",
      "default=noprint_wrappers=1:nokey=1", m_path.getQString()};
  const QString codecName = runFfprobe(probeArgs).trimmed();
  QStringList pre;
  if (codecName == "vp9") pre << "-vcodec" << "libvpx-vp9";
  else if (codecName == "vp8") pre << "-vcodec" << "libvpx";
  else if (codecName == "av1") pre << "-vcodec" << "libaom-av1";
  pre << "-threads" << "auto" << "-i" << m_path.getQString();
  QStringList post = {"-y", "-map", "0:v:0"};
  post << ThirdParty::getFFmpegFrameSyncArgs() << "-f" << "image2"
       << pending.filePath("frame%06d.png");
  runFfmpeg(pre, post, true, true, true, false);

  QDir directory(pending.path());
  if (directory.entryList({"frame*.png"}, QDir::Files).size() != m_frameCount)
    throw TImageException(m_path, "Video extraction produced an incomplete sequence");
  QJsonArray sizes;
  for (int index = 1; index <= m_frameCount; ++index) {
    QFileInfo image(directory.filePath(
        QString("frame%1.png").arg(index, 6, 10, QChar('0'))));
    if (!image.isFile() || image.size() <= 0)
      throw TImageException(m_path, "Video extraction is missing a frame");
    sizes.append(QString::number(image.size()));
  }

  // Catch replacement while ffmpeg reopened/decoded the source. Existing
  // readers may keep their old immutable generation; new readers use a new key.
  Ffmpeg currentSource;
  currentSource.setPath(m_path);
  if (currentSource.importCacheBase() != base)
    throw TImageException(m_path, "Video source changed during extraction");
  QSaveFile manifest(directory.filePath("complete.json"));
  const QByteArray bytes = QJsonDocument(QJsonObject{
      {"version", 2}, {"sizes", sizes}}).toJson(QJsonDocument::Compact);
  if (!manifest.open(QIODevice::WriteOnly) ||
      manifest.write(bytes) != bytes.size() || !manifest.commit())
    throw TImageException(m_path, "Cannot complete the video cache manifest");
  const QString published = base + ".frames";
  if (QFileInfo::exists(published) && !QDir(published).removeRecursively())
    throw TImageException(m_path, "Cannot replace an incomplete video cache");
  if (!QDir().rename(pending.path(), published))
    throw TImageException(m_path, "Cannot publish the video frame cache");
  pending.setAutoRemove(false);
}

TRasterImageP Ffmpeg::getImage(int frameIndex) {
  if (frameIndex < 1 || frameIndex > getFrameCount())
    throw TImageException(m_path, "Video frame index is out of range");
  const QString tempName = QDir(importCacheBase() + ".frames").filePath(
      QString("frame%1.png").arg(frameIndex, 6, 10, QChar('0')));
  if (!QFileInfo::exists(tempName)) getFramesFromMovie(frameIndex);

  if (QFile::exists(tempName)) {
    QImage qi;
    if (qi.load(tempName, "PNG")) {
      if (qi.format() != QImage::Format_ARGB32) {
        qi = qi.convertToFormat(QImage::Format_ARGB32);
      }

      int lx = qi.width();
      int ly = qi.height();

      TRasterPT<TPixelRGBM32> ret;
      ret.create(lx, ly);

      ret->lock();
      memcpy(ret->getRawData(), qi.constBits(), lx * ly * 4);
      ret->unlock();

      ret->yMirror();
      return TRasterImageP(ret);
    }
  }
  return TRasterImageP();
}

QString Ffmpeg::cleanPathSymbols() const {
  // This function is kept for compatibility but no longer used for temporary
  // files. It may be used elsewhere; we keep the original implementation.
  QString name = QString::fromStdString(m_path.getName());
  name.replace(QRegularExpression("[^a-zA-Z0-9_\\-\\.]"), "_");
  return name;
}

int Ffmpeg::getGifFrameCount() { return getFrameCount(); }

void Ffmpeg::addToCleanUp(const QString &path) {
  if (TSystem::doesExistFileOrLevel(TFilePath(path))) {
    m_cleanUpList.push_back(path);
  }
}

void Ffmpeg::cleanUpFiles() {
  for (const QString &path : m_cleanUpList) {
    if (TSystem::doesExistFileOrLevel(TFilePath(path))) {
      try {
        TSystem::deleteFile(TFilePath(path));
      } catch (const TSystemException &e) {
        DVGui::warning(QObject::tr("Failed to delete file: %1\n%2")
                           .arg(path)
                           .arg(QString::fromStdWString(e.getMessage())));
      }
    }
  }
  m_cleanUpList.clear();
}

void Ffmpeg::disablePrecompute() {
  Preferences::instance()->setPrecompute(false);
}

//===========================================================
//
//  TImageReaderFFmpeg
//
//===========================================================

class TImageReaderFFmpeg final : public TImageReader {
public:
  int m_frameIndex;

  TImageReaderFFmpeg(const TFilePath &path, int index, TLevelReaderFFmpeg *lra,
                     TImageInfo *info)
      : TImageReader(path), m_lra(lra), m_frameIndex(index), m_info(info) {
    if (m_lra) m_lra->addRef();
  }

  ~TImageReaderFFmpeg() override {
    if (m_lra) m_lra->release();
  }

  TImageP load() override {
    return m_lra ? m_lra->load(m_frameIndex) : TImageP();
  }

  TDimension getSize() const { return m_lra ? m_lra->getSize() : TDimension(); }

  TRect getBBox() const { return TRect(); }

  const TImageInfo *getImageInfo() const override { return m_info; }

private:
  TLevelReaderFFmpeg *m_lra;
  TImageInfo *m_info;

  TImageReaderFFmpeg(const TImageReaderFFmpeg &)            = delete;
  TImageReaderFFmpeg &operator=(const TImageReaderFFmpeg &) = delete;
};

//===========================================================
//
//  TLevelReaderFFmpeg
//
//===========================================================

TLevelReaderFFmpeg::TLevelReaderFFmpeg(const TFilePath &path)
    : TLevelReader(path)
    , m_ffmpegReader(nullptr)
    , m_frameCount(-1)
    , m_lx(0)
    , m_ly(0)
    , m_framesExtracted(false)
    , m_imageInfo(nullptr) {
  m_ffmpegReader = std::make_unique<Ffmpeg>();

  m_ffmpegReader->setPath(path);
  m_ffmpegReader->disablePrecompute();

  try {
    ffmpegFileInfo tempInfo = m_ffmpegReader->getInfo();
    double fps              = tempInfo.m_frameRate;
    m_frameCount            = tempInfo.m_frameCount;
    m_size                  = TDimension(tempInfo.m_lx, tempInfo.m_ly);
    m_lx                    = m_size.lx;
    m_ly                    = m_size.ly;

    m_imageInfo                   = new TImageInfo();
    m_imageInfo->m_frameRate      = fps;
    m_imageInfo->m_lx             = m_lx;
    m_imageInfo->m_ly             = m_ly;
    m_imageInfo->m_bitsPerSample  = 8;
    m_imageInfo->m_samplePerPixel = 4;
    m_imageInfo->m_dpix           = Stage::standardDpi;
    m_imageInfo->m_dpiy           = Stage::standardDpi;
  } catch (const TImageException &e) {
    m_ffmpegReader.reset();
    throw;
  }
}

TLevelReaderFFmpeg::~TLevelReaderFFmpeg() {
  delete m_imageInfo;
}

TLevelP TLevelReaderFFmpeg::loadInfo() {
  if (m_frameCount <= 0) return TLevelP();

  TLevelP level;
  for (int i = 1; i <= m_frameCount; ++i) {
    level->setFrame(i, TImageP());
  }

  return level;
}

TImageReaderP TLevelReaderFFmpeg::getFrameReader(TFrameId fid) {
  if (!fid.getLetter().isEmpty()) return TImageReaderP(nullptr);
  int index = fid.getNumber();

  if (index < 1 || (m_frameCount > 0 && index > m_frameCount)) {
    return TImageReaderP(nullptr);
  }

  TImageReaderFFmpeg *irm =
      new TImageReaderFFmpeg(m_path, index, this, m_imageInfo);
  return TImageReaderP(irm);
}

TDimension TLevelReaderFFmpeg::getSize() { return m_size; }

TImageP TLevelReaderFFmpeg::load(int frameIndex) {
  if (!m_ffmpegReader) return TImageP();

  if (!m_framesExtracted) {
    try {
      m_ffmpegReader->getFramesFromMovie();
      // Use checkFilesExist() instead of accessing private member
      if (m_ffmpegReader->checkFilesExist()) {
        m_framesExtracted = true;
      } else {
        throw TImageException(m_path, "Frame extraction produced no files.");
      }
    } catch (const TImageException &e) {
      DVGui::warning(QObject::tr("Failed to extract frames from movie: %1")
                         .arg(QString::fromStdWString(e.getMessage())));
      return TImageP();
    }
  }

  return m_ffmpegReader->getImage(frameIndex);
}
