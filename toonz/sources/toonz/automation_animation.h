#pragma once

#include "tdoublekeyframe.h"

#include <QJsonArray>
#include <QJsonObject>
#include <QString>

#include <vector>

class TDoubleParam;

namespace GroundzeroAutomation {

// Whether a cyclic curve is inside the profile of the caller's target. Stage
// base channels qualify cycles; weights, path positions and offset-layer
// curves keep refusing them.
enum class CyclePolicy { Refuse, Allow };

struct AnimationKeyEdit final {
  // Replacement or new records, ascending by frame.
  std::vector<TDoubleKeyframe> keys;
  // Key frames removed before the records are written.
  std::vector<int> deletedFrames;
  bool changed = false;
};

struct AnimationCurveEdit final {
  std::vector<TDoubleKeyframe> keys;
  double defaultValue = 0;
  bool cycle          = false;
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
AnimationKeyEdit prepareAnimationKeyEdit(
    const TDoubleParam& curve, const QJsonArray& keys,
    CyclePolicy policy = CyclePolicy::Refuse);

// Validate deletion on an isolated prospective curve before changing native
// data.
void validateAnimationKeyDeletion(const TDoubleParam& curve,
                                  const std::vector<int>& frames,
                                  CyclePolicy policy = CyclePolicy::Refuse);

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
AnimationKeyEdit prepareAnimationBreakdown(
    const TDoubleParam& curve, const QJsonObject& request,
    CyclePolicy policy = CyclePolicy::Refuse);

// Prepare a shift of whole key records by frame_offset frames and value_offset.
// The key order is kept and every unselected key is left unchanged; a shift
// that would cross or land on another key, leave a handle outside its segment
// or change a key's incoming control kind is refused.
AnimationKeyEdit prepareAnimationKeyMove(
    const TDoubleParam& curve, const std::vector<int>& frames, int frameOffset,
    double valueOffset, CyclePolicy policy = CyclePolicy::Refuse);

// Prepare verbatim copies of key records at frame_offset frames and
// value_offset. No existing key is replaced and no key's incoming control kind
// changes.
AnimationKeyEdit prepareAnimationKeyDuplicate(
    const TDoubleParam& curve, const std::vector<int>& frames, int frameOffset,
    double valueOffset = 0, CyclePolicy policy = CyclePolicy::Refuse);

// Delete the prepared frames, then write the prepared records to the curve.
void applyAnimationKeyEdit(TDoubleParam& curve, const AnimationKeyEdit& edit);

// Validate a cycle flag change on a stage base channel curve; returns whether
// it changes the curve. The curve's keys must be inside the profile.
bool prepareAnimationCycle(const TDoubleParam& curve, bool enabled);

// Refuse an animation.sample request whose cyclic folds, summed over the
// frames, would take too long.
void requireAnimationSampleBudget(const TDoubleParam& curve,
                                  const std::vector<double>& frames);

// Return an empty string for supported content, or its concrete admission
// reason.
QString animationUnsupportedReason(const TDoubleParam& curve,
                                   CyclePolicy policy = CyclePolicy::Refuse);

// Inspect native keys without substituting editable records for unsupported
// types.
QJsonArray inspectAnimationKeys(const TDoubleParam& curve);

// Require the bounded native curve profile before evaluating samples.
void requireSupportedAnimationCurve(const TDoubleParam& curve,
                                    CyclePolicy policy = CyclePolicy::Refuse);

}  // namespace GroundzeroAutomation
