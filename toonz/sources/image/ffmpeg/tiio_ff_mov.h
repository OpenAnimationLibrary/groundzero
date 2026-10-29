#pragma once

#ifndef TTIO_FFMPEG_MOV_INCLUDED
#define TTIO_FFMPEG_MOV_INCLUDED

#include "tproperty.h"
#include "tlevel_io.h"
#include "tiio_ffmpeg.h"

#include <QCoreApplication>

//===========================================================
//
//  TLevelWriterFFMov
//
//===========================================================

class TLevelWriterFFMov : public TLevelWriter {
public:
  TLevelWriterFFMov(const TFilePath &path, TPropertyGroup *winfo);
  ~TLevelWriterFFMov();
  void finalize() override;
  void abort() noexcept override;
  void requestAbort() noexcept override;
  void setFrameRate(double fps) override;

  TImageWriterP getFrameWriter(TFrameId fid) override;
  void save(const TImageP &image, int frameIndex);

  void saveSoundTrack(TSoundTrack *st) override;

  static TLevelWriter *create(const TFilePath &path, TPropertyGroup *winfo) {
    return new TLevelWriterFFMov(path, winfo);
  }

private:
  void buildFfmpegArgs(QStringList &preIArgs, QStringList &postIArgs) const;

  std::unique_ptr<Ffmpeg> ffmpegWriter;
  mutable QMutex m_metadataMutex;
  int m_lx = 0, m_ly = 0;
  int m_scale;
  int m_vidQuality;
};

//===========================================================
//
//  TLevelReaderFFMov
//
//===========================================================

class TLevelReaderFFMov final : public TLevelReader {
public:
  TLevelReaderFFMov(const TFilePath &path);
  ~TLevelReaderFFMov();
  TImageReaderP getFrameReader(TFrameId fid) override;

  static TLevelReader *create(const TFilePath &f) {
    return new TLevelReaderFFMov(f);
  }

  TLevelP loadInfo() override;
  TImageP load(int frameIndex);
  TDimension getSize();
private:
  std::unique_ptr<Ffmpeg> ffmpegReader;
  bool ffmpegFramesCreated = false;
  TDimension m_size;
  int m_frameCount = 0, m_lx = 0, m_ly = 0;
};

//===========================================================================

namespace Tiio {

//===========================================================================

class FFMovWriterProperties : public TPropertyGroup {
  Q_DECLARE_TR_FUNCTIONS(FFMovWriterProperties)
public:
  TIntProperty m_vidQuality;
  TIntProperty m_scale;
  FFMovWriterProperties();
  void updateTranslation() override;
};

//===========================================================================

}  // namespace Tiio

#endif
