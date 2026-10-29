#pragma once

#ifndef TTIO_APNG_INCLUDED
#define TTIO_APNG_INCLUDED

#include "tproperty.h"
#include "tlevel_io.h"
#include "tiio_ffmpeg.h"
#include <QCoreApplication>

//===========================================================
//
//  TLevelWriterAPng
//
//===========================================================

class TLevelWriterAPng : public TLevelWriter {
public:
  TLevelWriterAPng(const TFilePath &path, TPropertyGroup *winfo);
  ~TLevelWriterAPng();
  void finalize() override;
  void abort() noexcept override;
  void requestAbort() noexcept override;
  void setFrameRate(double fps) override;

  TImageWriterP getFrameWriter(TFrameId fid) override;
  void save(const TImageP &image, int frameIndex);

  void saveSoundTrack(TSoundTrack *st) override;

  static TLevelWriter *create(const TFilePath &path, TPropertyGroup *winfo) {
    return new TLevelWriterAPng(path, winfo);
  }

private:
  void buildFfmpegArgs(QStringList &preIArgs, QStringList &postIArgs) const;

  std::unique_ptr<Ffmpeg> ffmpegWriter;
  mutable QMutex m_metadataMutex;
  int m_lx = 0, m_ly = 0;
  int m_scale;
  bool m_looping;
  bool m_extPng;
};

//===========================================================
//
//  TLevelReaderAPng
//
//===========================================================

class TLevelReaderAPng final : public TLevelReader {
public:
  TLevelReaderAPng(const TFilePath &path);
  ~TLevelReaderAPng();
  TImageReaderP getFrameReader(TFrameId fid) override;

  static TLevelReader *create(const TFilePath &f) {
    return new TLevelReaderAPng(f);
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

class APngWriterProperties : public TPropertyGroup {
  Q_DECLARE_TR_FUNCTIONS(APngWriterProperties)
public:
  TIntProperty m_scale;
  TBoolProperty m_looping;
  TBoolProperty m_extPng;
  APngWriterProperties();
  void updateTranslation() override;
};

//===========================================================================

}  // namespace Tiio

#endif
