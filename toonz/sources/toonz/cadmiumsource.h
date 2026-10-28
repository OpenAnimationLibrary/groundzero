#pragma once

#include "toonz/imagemanager.h"
#include "toonz/txshsimplelevel.h"
#include "toonz/txshleveltypes.h"

namespace Cadmium {
inline bool canPaintLevel(const TXshSimpleLevel *level) {
  if (!level) return false;
  if (level->getType() == OVL_XSHLEVEL) return true;
  return (level->getType() == TZP_XSHLEVEL || level->getType() == PLI_XSHLEVEL) &&
         !level->isReadOnly();
}

inline TImageP loadDrawing(TXshSimpleLevel *level, const TFrameId &fid) {
  if (!level) return TImageP();
  UCHAR flags = ImageManager::none;
  if (level->is16BitChannelLevel()) flags |= ImageManager::is64bitEnabled;
  if (level->isFloatChannelLevel()) flags |= ImageManager::isFloatEnabled;
  return level->getFullsampledFrame(fid, flags);
}
}
