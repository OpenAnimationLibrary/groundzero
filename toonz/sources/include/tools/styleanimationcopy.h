#pragma once
#include "tpalette.h"
#include "tcolorstyles.h"
#include <memory>
#include <utility>
#include <vector>

namespace StyleAnimationCopy {
using Keys = std::vector<std::pair<int, std::shared_ptr<TColorStyle>>>;

inline Keys capture(const TPalette *source, int sourceId,
                    const TColorStyle &targetIdentity) {
  Keys keys;
  TPaletteP snapshot = source->clone();
  for (int i = 0; i < source->getKeyframeCount(sourceId); ++i) {
    const int frame = source->getKeyframe(sourceId, i);
    snapshot->setFrame(frame);
    std::shared_ptr<TColorStyle> style(snapshot->getStyle(sourceId)->clone());
    style->setName(targetIdentity.getName());
    style->setGlobalName(targetIdentity.getGlobalName());
    style->setOriginalName(targetIdentity.getOriginalName());
    style->setIsEditedFlag(targetIdentity.getIsEditedFlag());
    keys.emplace_back(frame, std::move(style));
  }
  return keys;
}

inline void install(TPalette *target, int targetId, const Keys &keys,
                    const TColorStyle &currentAppearance) {
  while (target->getKeyframeCount(targetId))
    target->clearKeyframe(targetId, target->getKeyframe(targetId, 0));
  for (const auto &key : keys) {
    target->setStyle(targetId, key.second->clone());
    target->setKeyframe(targetId, key.first);
  }
  target->setStyle(targetId, currentAppearance.clone());
}
}  // namespace StyleAnimationCopy
