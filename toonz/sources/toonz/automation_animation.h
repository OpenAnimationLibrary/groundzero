#pragma once

#include "tdoublekeyframe.h"

#include <QJsonArray>
#include <QJsonObject>
#include <QString>

#include <vector>

class TDoubleParam;

namespace GroundzeroAutomation {

struct AnimationKeyEdit final {
  std::vector<TDoubleKeyframe> keys;
  bool changed = false;
};

struct AnimationCurveEdit final {
  std::vector<TDoubleKeyframe> keys;
  double defaultValue = 0;
  bool changed        = false;
};

// Prepare a whole-curve copy without changing either native parameter.
//
// Args:
//   source: Supported source curve, which may be editor-locked.
//   destination: Writable supported curve whose identity stays intact.
// Returns:
//   Independently owned key records and default value, fully validated.
// Throws:
//   Error: Unsupported, locked or out-of-bounds native content.
AnimationCurveEdit prepareAnimationCurveCopy(const TDoubleParam& source,
                                             const TDoubleParam& destination);

// Prepare exact rational time mapping, preserving values and native metadata.
//
// Args:
//   curve: Writable supported curve.
//   mapping: Bounded origin_frame, shift_frames and positive rational scale.
// Returns:
//   Validated replacement records without mutating the source curve.
// Throws:
//   Error: Nonintegral frame/step mapping or unsupported prospective content.
AnimationCurveEdit prepareAnimationCurveRetime(const TDoubleParam& curve,
                                               const QJsonObject& mapping);

// Apply prepared records to the existing destination, preserving its identity.
void applyAnimationCurveEdit(TDoubleParam& destination,
                             const AnimationCurveEdit& edit);

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

// Prepare the editor's Set Breakdown Key: split the segment containing
// request.frame and give the new key either request.favour_percent of the way
// from the previous key's value to the next one's, or request.value.
//
// Args:
//   curve: Writable native base curve; it is not modified.
//   request: frame, and exactly one of favour_percent or value.
// Returns:
//   The new key and the segment ends whose facing handle changed, ascending.
// Throws:
//   Error: locked, unsupported content, a frame that is a key or lies outside
//   the keys, or a result outside the profile.
AnimationKeyEdit prepareAnimationBreakdown(const TDoubleParam& curve,
                                           const QJsonObject& request);

// Write the prepared records to the curve.
void applyAnimationKeyEdit(TDoubleParam& curve, const AnimationKeyEdit& edit);

// Return an empty string for supported content, or its concrete admission
// reason.
QString animationUnsupportedReason(const TDoubleParam& curve);

// Inspect native keys without substituting editable records for unsupported
// types.
QJsonArray inspectAnimationKeys(const TDoubleParam& curve);

// Require the bounded native curve profile before evaluating samples.
void requireSupportedAnimationCurve(const TDoubleParam& curve);

}  // namespace GroundzeroAutomation
