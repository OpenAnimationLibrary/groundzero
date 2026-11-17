#pragma once

#include "tdoublekeyframe.h"

#include <QJsonArray>
#include <QString>

#include <vector>

class TDoubleParam;

namespace GroundzeroAutomation {

struct AnimationKeyEdit final {
  std::vector<TDoubleKeyframe> keys;
  bool changed = false;
};

// Prepare a complete sparse key edit without modifying the source curve.
//
// Args:
//   curve: Native base curve whose current metadata must be preserved.
//   keys: Proposed records with independent incoming and outgoing controls.
// Returns:
//   Validated replacements and whether applying them changes native key state.
// Throws:
//   Error: Unsupported content, locked curve, invalid records or interval
//   bounds.
AnimationKeyEdit prepareAnimationKeyEdit(const TDoubleParam& curve,
                                         const QJsonArray& keys);

// Validate deletion on an isolated prospective curve before changing native
// data.
void validateAnimationKeyDeletion(const TDoubleParam& curve,
                                  const std::vector<int>& frames);

// Return an empty string for supported content, or its concrete admission
// reason.
QString animationUnsupportedReason(const TDoubleParam& curve);

// Inspect native keys without substituting editable records for unsupported
// types.
QJsonArray inspectAnimationKeys(const TDoubleParam& curve);

// Require the bounded native curve profile before evaluating samples.
void requireSupportedAnimationCurve(const TDoubleParam& curve);

}  // namespace GroundzeroAutomation
