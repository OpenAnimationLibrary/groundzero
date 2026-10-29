#pragma once

#include "tlevel_io.h"
#include "tiio.h"
#include <QString>

namespace MovieRenderOutput {

// Keep this aligned with writer registration in image/tiio.cpp. MOV uses the
// legacy writer when QuickTime is enabled; APNG always uses FFmpeg. The more
// general TFilePath::isFfmpegType() also serves import/UI policy and omits both.
inline bool usesFfmpeg(const TFilePath &path) {
  const QString type = QString::fromStdString(path.getType()).toLower();
  return path.isFfmpegType() || type == "apng" ||
         (type == "mov" && !Tiio::useQuicktime());
}

inline TLevelWriterP cancelableWriter(const TLevelWriterP &writer) {
  // Legacy writers still flush/close in their destructor. An extra owner
  // would keep their file open while LevelUpdater tries to rename it.
  return writer && usesFfmpeg(writer->getFilePath()) ? writer : TLevelWriterP();
}

}  // namespace MovieRenderOutput
