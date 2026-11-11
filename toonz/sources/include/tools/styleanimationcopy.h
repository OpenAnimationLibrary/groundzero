#pragma once
#include "tpalette.h"
#include "tcolorstyles.h"

namespace StyleAnimationCopy {
using Keys = TPalette::StyleAnimation;

inline Keys capture(const TPalette *source, int sourceId,
                    const TColorStyle &targetIdentity) {
  Keys keys;
  TPaletteP snapshot = source->clone();
  for (const auto &[frame, key] : source->getStyleAnimation(sourceId)) {
    snapshot->setFrame(frame);
    TColorStyleP style(snapshot->getStyle(sourceId)->clone());
    style->setName(targetIdentity.getName());
    style->setGlobalName(targetIdentity.getGlobalName());
    style->setOriginalName(targetIdentity.getOriginalName());
    style->setIsEditedFlag(targetIdentity.getIsEditedFlag());
    keys.emplace(frame, TPalette::StyleKey{style, key.interpolation});
  }
  return keys;
}

inline void install(TPalette *target, int targetId, const Keys &keys,
                    const TColorStyle &currentAppearance) {
  while (target->getKeyframeCount(targetId))
    target->clearKeyframe(targetId, target->getKeyframe(targetId, 0));
  for (const auto &[frame, key] : keys) {
    target->setStyle(targetId, key.style->clone());
    target->setKeyframe(targetId, frame, key.interpolation);
  }
  target->setStyle(targetId, currentAppearance.clone());
}
}  // namespace StyleAnimationCopy
