#pragma once

#ifndef TTIO_FFMPEG_INCLUDED
#define TTIO_FFMPEG_INCLUDED

#include "tproperty.h"
#include "tlevel_io.h"
#include "trasterimage.h"
#include "tsound.h"
#include "tfilepath.h"
#include "tgeometry.h"

#include <QByteArray>
#include <QFile>
#include <QMutex>
#include <QString>
#include <QStringList>
#include <QVector>
#include <QProcess>
#include <functional>
#include <limits>
#include <memory>

// Struct to hold video file information.
// Note: Zero values in any field may indicate that ffprobe failed to retrieve
// that information.
struct ffmpegFileInfo {
  int m_lx           = 0;
  int m_ly           = 0;
  int m_frameCount   = 0;
  double m_frameRate = 0.0;
};

// RAII helper to safely manage temporary files
// (can be used within functions that create intermediate PNG files)
struct FfmpegTempFileGuard {
  QVector<QString> &cleanupList;
  QStringList addedThisScope;

  explicit FfmpegTempFileGuard(QVector<QString> &list) : cleanupList(list) {}
  ~FfmpegTempFileGuard();

  void add(const QString &path);
  void release();

  FfmpegTempFileGuard(const FfmpegTempFileGuard &)            = delete;
  FfmpegTempFileGuard &operator=(const FfmpegTempFileGuard &) = delete;
};

class FfmpegPipeWriter;

class Ffmpeg {
public:
  // Builds a writer's ffmpeg arguments (before and after the video input)
  typedef std::function<void(QStringList &preInputArgs,
                             QStringList &postInputArgs)>
      ArgsBuilder;

  Ffmpeg();
  ~Ffmpeg();

  // Creates optimized intermediate PNG image (used in the old flow)
  void createIntermediateImage(const TImageP &image, int frameIndex);

  // Streams the frames of an export to ffmpeg's standard input (raw video)
  // instead of writing intermediate images. Call it before the first frame.
  // It has no effect when frames may be saved concurrently or out of order.
  // alwaysSequential: the format is always rendered one frame at a time and
  // in order (isSequencialRequired()).
  void enablePipe(bool alwaysSequential = false);

  // Saves a frame of an export: when the pipe is enabled, starts ffmpeg on
  // the first frame (with the arguments from buildArgs) and streams the
  // frame to it; otherwise calls createIntermediateImage().
  void saveFrame(const TImageP &image, int frameIndex,
                 const ArgsBuilder &buildArgs);

  // Closes ffmpeg's standard input and waits for it to finish encoding,
  // reporting errors. Returns false if no frame was streamed, in which case
  // the caller runs ffmpeg over the intermediate images as before.
  bool finishPipe();

  // Executes ffmpeg with custom arguments
  void runFfmpeg(const QStringList &preInputArgs,
                 const QStringList &postInputArgs,
                 bool inputPathIncluded  = false,
                 bool outputPathIncluded = false, bool overwrite = true,
                 bool asynchronous = true);

  void runFfmpeg(const QStringList &preInputArgs,
                 const QStringList &postInputArgs, const TFilePath &outputPath);

  // Throws TImageException on ffprobe failure (synchronous use only)
  QString runFfprobe(const QStringList &args) const;

  void cleanUpFiles();
  void addToCleanUp(const QString &path);

  void setFrameRate(double fps);
  void setPath(const TFilePath &path);

  void saveSoundTrack(TSoundTrack *soundTrack);

  bool checkFilesExist() const;

  static bool checkFormat(std::string format);

  // Getters with const, now use mutable caching for performance
  double getFrameRate() const;
  TDimension getSize() const;
  int getFrameCount() const;
  ffmpegFileInfo getInfo() const;

  void getFramesFromMovie(int frame = -1);
  TRasterImageP getImage(int frameIndex);

  TFilePath getFfmpegCache() const;

  void disablePrecompute();

  int getGifFrameCount();

private:
  QString m_intermediateFormat = "png";
  QString m_audioPath;
  QString m_audioFormat;
  QStringList m_audioArgs;

  // Mutable members for caching inside const functions
  mutable double m_frameRate = 0.0;
  mutable int m_lx           = 0;
  mutable int m_ly           = 0;
  mutable int m_frameCount   = 0;
  mutable bool m_infoCached =
      false;  // indicates whether cache has been attempted

  int m_bpp         = 0;
  int m_startNumber = std::numeric_limits<int>::max();
  int m_ffmpegTimeoutMs;  // Value comes from constructor (user config)
  int m_sampleRate    = 0;
  int m_channelCount  = 0;
  int m_bitsPerSample = 0;

  bool m_hasSoundTrack = false;

  TFilePath m_path;
  QVector<QString> m_cleanUpList;
  // Intermediate frames may be encoded concurrently. Protect their shared
  // bookkeeping without holding a lock during image conversion or PNG writes.
  QMutex m_intermediateMutex;

  // Unique identifier for temporary files (based on full path hash)
  QString m_tempBaseName;

  // Raw video pipe state (see enablePipe())
  bool m_pipeEnabled       = false;
  int m_pipeNextFrame      = 0;
  FfmpegPipeWriter *m_pipe = nullptr;
  QByteArray m_pipeLastFrame;  // repeated to fill gaps
  QString m_pipeLogPath;
  QMutex m_pipeMutex;

  QString cleanPathSymbols() const;
  bool waitFfmpeg(QProcess &process, bool async) const;
  void startPipe(int lx, int ly, const ArgsBuilder &buildArgs);

  // not allow copy of temporary files
  Ffmpeg(const Ffmpeg &)            = delete;
  Ffmpeg &operator=(const Ffmpeg &) = delete;
};

class TLevelReaderFFmpeg final : public TLevelReader {
public:
  explicit TLevelReaderFFmpeg(const TFilePath &path);
  ~TLevelReaderFFmpeg() override;

  TImageReaderP getFrameReader(TFrameId fid) override;
  TLevelP loadInfo() override;
  TImageP load(int frameIndex);
  TDimension getSize();

  static TLevelReader *create(const TFilePath &f) {
    return new TLevelReaderFFmpeg(f);
  }

private:
  std::unique_ptr<Ffmpeg> m_ffmpegReader;
  bool m_framesExtracted = false;
  TDimension m_size;
  int m_frameCount        = -1;
  int m_lx                = 0;
  int m_ly                = 0;
  TImageInfo *m_imageInfo = nullptr;
};

#endif  // TTIO_FFMPEG_INCLUDED
