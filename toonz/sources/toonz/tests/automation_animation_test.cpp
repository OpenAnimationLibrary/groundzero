#include "automation.h"
#include "automation_animation.h"
#include "tests/automation_animation_test.h"

#include "toonz/doubleparamcmd.h"

#include "tdoublekeyframe.h"
#include "tdoubleparam.h"
#include "tfilepath.h"
#include "tparamchange.h"
#include "tstream.h"

#include <QTemporaryDir>

#include <array>
#include <cmath>
#include <functional>
#include <limits>
#include <stdexcept>
#include <string>

namespace GroundzeroAutomation {
namespace {

void require(bool condition, const char* message) {
  if (!condition) throw std::runtime_error(message);
}

void near(double actual, double expected) {
  if (!std::isfinite(actual) || std::abs(actual - expected) > 1e-7)
    throw std::runtime_error("Native animation expected " +
                             std::to_string(expected) + ", observed " +
                             std::to_string(actual));
}

TDoubleKeyframe key(double frame, double value, TDoubleKeyframe::Type type,
                    const TPointD& incoming, const TPointD& outgoing) {
  TDoubleKeyframe result(frame, value);
  result.m_type          = type;
  result.m_isKeyframe    = true;
  result.m_step          = 1;
  result.m_linkedHandles = false;
  result.m_speedIn       = incoming;
  result.m_speedOut      = outgoing;
  return result;
}

TDoubleParamP mixedCurve() {
  TDoubleParamP curve(new TDoubleParam());
  curve->setKeyframe(key(0, 0, TDoubleKeyframe::SpeedInOut, {}, {4, 0}));
  curve->setKeyframe(
      key(12, 120, TDoubleKeyframe::EaseInOut, {-4, -24}, {3, 0}));
  curve->setKeyframe(key(24, 240, TDoubleKeyframe::Linear, {-3, 0}, {}));
  curve->setKeyframe(key(36, 360, TDoubleKeyframe::Constant, {}, {}));
  return curve;
}

void verifyMixedCurve(const TDoubleParamP& curve) {
  const std::array<double, 11> frames{0, 3, 6, 9, 12, 15, 18, 21, 24, 30, 36};
  const std::array<double, 11> values{0,   15.375, 51,  91.125, 120, 140,
                                      180, 220,    240, 300,    360};
  for (std::size_t index = 0; index < frames.size(); ++index) {
    double value = 0;
    std::string error;
    require(curve->getValueChecked(frames[index], value, error),
            "Native mixed curve evaluation failed");
    near(value, values[index]);
  }
  near(curve->getValue(-0.5), 0);
  near(curve->getValue(36.5), 360);
  near(curve->getValue(25.25), 252.5);
  const auto& ease = curve->getKeyframe(1);
  require(ease.m_prevType == TDoubleKeyframe::SpeedInOut &&
              ease.m_type == TDoubleKeyframe::EaseInOut,
          "Mixed key must retain independent incoming and outgoing types");
  require(ease.m_speedIn == TPointD(-4, -24) &&
              ease.m_speedOut == TPointD(3, 0) && !ease.m_linkedHandles,
          "Mixed key must preserve both unlinked native controls");
  require(curve->getKeyframe(2).m_prevType == TDoubleKeyframe::EaseInOut &&
              curve->getKeyframe(2).m_speedIn == TPointD(-3, 0),
          "Linear key must preserve the preceding ease segment's input");
}

void mixedSegmentControls() {
  const auto curve = mixedCurve();
  verifyMixedCurve(curve);
  TDoubleParamP copied(new TDoubleParam(*curve));
  verifyMixedCurve(copied);
  auto changed         = copied->getKeyframe(1);
  changed.m_speedOut.x = 1;
  copied->setKeyframe(changed);
  near(copied->getValue(6), 51);
  require(std::abs(copied->getValue(15) - curve->getValue(15)) > 1,
          "Changing outgoing ease must change that segment independently");
  near(curve->getKeyframe(1).m_speedOut.x, 3);
}

void steppedFractionalSamples() {
  TDoubleParamP curve(new TDoubleParam());
  auto first   = key(0, 0, TDoubleKeyframe::Linear, {}, {});
  first.m_step = 3;
  curve->setKeyframe(first);
  curve->setKeyframe(key(10, 90, TDoubleKeyframe::Linear, {}, {}));
  const std::array<double, 10> frames{0, 2.99, 3, 3.01, 5.99,
                                      6, 8.99, 9, 9.99, 10};
  const std::array<double, 10> values{0, 0, 30, 30, 30, 60, 60, 90, 90, 90};
  for (std::size_t index = 0; index < frames.size(); ++index)
    near(curve->getValue(frames[index]), values[index]);
  first.m_step = 1000;
  curve->setKeyframe(first);
  near(curve->getValue(9.99), 0);
  near(curve->getValue(10), 90);
}

void mixedControlsRoundTrip() {
  QTemporaryDir directory;
  require(directory.isValid(), "Animation fixture directory is unavailable");
  const TFilePath path(directory.filePath("mixed-curve.xml"));
  const auto original = mixedCurve();
  {
    TOStream output(path);
    original->saveData(output);
  }
  TDoubleParamP reopened(new TDoubleParam());
  {
    TIStream input(path);
    reopened->loadData(input);
  }
  verifyMixedCurve(reopened);
  require(reopened->getKeyframeCount() == original->getKeyframeCount(),
          "Native curve serialization changed the key count");
}

QJsonArray mixedRecords() {
  return {
      QJsonObject{
          {"frame", 0},
          {"value", 0},
          {"interpolation", "speed_in_out"},
          {"outgoing", QJsonObject{{"frame_delta", 4}, {"value_delta", 0}}}},
      QJsonObject{{"frame", 12},
                  {"value", 120},
                  {"interpolation", "ease_in_out"},
                  {"incoming", QJsonObject{{"kind", "speed"},
                                           {"frame_delta", -4},
                                           {"value_delta", -24}}},
                  {"outgoing", QJsonObject{{"frames", 3}}}},
      QJsonObject{{"frame", 24},
                  {"value", 240},
                  {"interpolation", "linear"},
                  {"incoming", QJsonObject{{"kind", "ease"}, {"frames", 3}}}},
      QJsonObject{
          {"frame", 36}, {"value", 360}, {"interpolation", "constant"}}};
}

class CurveObserver final : public TParamObserver {
  TDoubleParam& m_curve;

public:
  int changes = 0;
  explicit CurveObserver(TDoubleParam& curve) : m_curve(curve) {
    curve.addObserver(this);
  }
  ~CurveObserver() override { m_curve.removeObserver(this); }
  void onChange(const TParamChange&) override { ++changes; }
};

void apply(TDoubleParam& curve, const AnimationKeyEdit& edit) {
  for (const auto& replacement : edit.keys) curve.setKeyframe(replacement);
}

void preparedEditsPreserveSourceAndDetectNoops() {
  TDoubleParamP curve(new TDoubleParam());
  CurveObserver observer(*curve);
  const auto edit = prepareAnimationKeyEdit(*curve, mixedRecords());
  require(edit.changed && edit.keys.size() == 4,
          "Rich batch did not prepare four native keys");
  require(curve->getKeyframeCount() == 0 && observer.changes == 0,
          "Preparing rich keys changed or notified the source curve");
  apply(*curve, edit);
  verifyMixedCurve(curve);
  const int changes   = observer.changes;
  const auto repeated = prepareAnimationKeyEdit(*curve, mixedRecords());
  require(
      !repeated.changed && repeated.keys.empty() && observer.changes == changes,
      "An identical rich batch was not a source-preserving no-op");
  auto patch        = mixedRecords()[1].toObject();
  patch["outgoing"] = QJsonObject{{"frames", 1}};
  const auto sparse = prepareAnimationKeyEdit(*curve, {patch});
  require(sparse.changed && sparse.keys.size() == 1,
          "Sparse rich edit unexpectedly included untouched keys");
  verifyMixedCurve(curve);
  apply(*curve, sparse);
  near(curve->getValue(6), 51);
  require(std::abs(curve->getValue(15) - 140) > 1,
          "Sparse ease edit did not reach native evaluation");
}

void rejected(TDoubleParam& curve, const QString& code,
              const std::function<void()>& action) {
  const auto before = inspectAnimationKeys(curve);
  CurveObserver observer(curve);
  bool failed = false;
  try {
    action();
  } catch (const Error& error) {
    require(error.code == code,
            "Animation preflight returned the wrong error code");
    failed = true;
  }
  require(failed, "Invalid native animation edit unexpectedly succeeded");
  require(inspectAnimationKeys(curve) == before && observer.changes == 0,
          "Rejected animation edit changed or notified the source curve");
}

void malformedRecordsAreAtomic() {
  const auto curve = mixedCurve();
  const auto valid = mixedRecords()[1].toObject();
  std::vector<QJsonObject> invalidRecords;
  auto missing = valid;
  missing.remove("incoming");
  invalidRecords.push_back(missing);
  for (const double step : {0.0, 1.5, 1001.0}) {
    auto record    = valid;
    record["step"] = step;
    invalidRecords.push_back(record);
  }
  auto overflow        = valid;
  overflow["outgoing"] = QJsonObject{{"frames", 10}};
  invalidRecords.push_back(overflow);
  auto fractional     = valid;
  fractional["frame"] = 12.5;
  invalidRecords.push_back(fractional);
  auto nonfinite     = valid;
  nonfinite["value"] = std::numeric_limits<double>::infinity();
  invalidRecords.push_back(nonfinite);
  for (const auto& record : invalidRecords)
    rejected(*curve, "invalid_argument", [&]() {
      prepareAnimationKeyEdit(*curve, {mixedRecords()[0], record});
    });
  rejected(*curve, "invalid_argument",
           [&]() { prepareAnimationKeyEdit(*curve, {valid, valid}); });
}

void mixedKindChangesRequireExplicitEndpoints() {
  const auto curve = mixedCurve();
  const QJsonObject first{
      {"frame", 0}, {"value", 0}, {"interpolation", "linear"}};
  rejected(*curve, "invalid_argument",
           [&]() { prepareAnimationKeyEdit(*curve, {first}); });
  auto end        = mixedRecords()[1].toObject();
  end["incoming"] = QJsonObject{{"kind", "none"}};
  const auto edit = prepareAnimationKeyEdit(*curve, {end, first});
  require(edit.changed && edit.keys.size() == 2,
          "Explicit mixed-kind endpoint batch did not prepare both keys");
  verifyMixedCurve(curve);
  TDoubleParamP next(new TDoubleParam(*curve));
  apply(*next, edit);
  near(next->getValue(6), 60);
  near(next->getValue(15), 140);
  const auto restored =
      prepareAnimationKeyEdit(*next, {mixedRecords()[1], mixedRecords()[0]});
  require(restored.keys.size() == 2,
          "Restoring speed did not prepare both endpoints");
  apply(*next, restored);
  verifyMixedCurve(next);
  require(inspectAnimationKeys(*next) == inspectAnimationKeys(*curve),
          "Applied reverse transition differs from the validated curve");
}

void richKindsChangeInBothDirections() {
  const auto curve       = mixedCurve();
  auto start             = mixedRecords()[1].toObject();
  start["interpolation"] = "speed_in_out";
  start["outgoing"]      = QJsonObject{{"frame_delta", 3}, {"value_delta", 12}};
  auto end               = mixedRecords()[2].toObject();
  end["incoming"] =
      QJsonObject{{"kind", "speed"}, {"frame_delta", -3}, {"value_delta", -30}};
  rejected(*curve, "invalid_argument",
           [&]() { prepareAnimationKeyEdit(*curve, {start}); });
  apply(*curve, prepareAnimationKeyEdit(*curve, {end, start}));
  near(curve->getValue(6), 51);
  near(curve->getValue(18), 173.25);
  const auto repeated = prepareAnimationKeyEdit(*curve, {start, end});
  require(!repeated.changed,
          "Applied speed controls disagree with prospective controls");
  rejected(*curve, "invalid_argument",
           [&]() { prepareAnimationKeyEdit(*curve, {mixedRecords()[1]}); });
  apply(*curve, prepareAnimationKeyEdit(
                    *curve, {mixedRecords()[2], mixedRecords()[1]}));
  verifyMixedCurve(curve);
  require(!prepareAnimationKeyEdit(*curve, mixedRecords()).changed,
          "Applied ease controls disagree with prospective controls");
}

void simpleNoopsPreserveInactiveNativeMetadata() {
  TDoubleParamP curve(new TDoubleParam());
  auto first       = key(0, 2, TDoubleKeyframe::Linear, {-2, 9}, {3, -1});
  first.m_unitName = "inch";
  curve->setKeyframe(first);
  curve->setKeyframe(key(12, 14, TDoubleKeyframe::Constant, {-1, 7}, {2, 3}));
  const auto before = curve->getKeyframe(0);
  CurveObserver observer(*curve);
  const QJsonObject minimal{
      {"frame", 0}, {"value", 2}, {"interpolation", "linear"}};
  const auto noOp = prepareAnimationKeyEdit(*curve, {minimal});
  require(
      !noOp.changed && noOp.keys.empty() && observer.changes == 0,
      "An existing simple key was rewritten by an identical legacy payload");
  const QJsonObject last{
      {"frame", 12}, {"value", 20}, {"interpolation", "constant"}};
  apply(*curve, prepareAnimationKeyEdit(*curve, {last, minimal}));
  const auto& unchanged = curve->getKeyframe(0);
  require(unchanged.m_speedIn == before.m_speedIn &&
              unchanged.m_speedOut == before.m_speedOut &&
              unchanged.m_linkedHandles == before.m_linkedHandles &&
              unchanged.m_unitName == before.m_unitName,
          "A sparse simple edit changed untouched native metadata");
  require(observer.changes == 1,
          "An unchanged simple key triggered a native write");
  near(curve->getValue(6), 11);
}

void deletionAndAdmissionGuards() {
  const auto curve = mixedCurve();
  rejected(*curve, "invalid_argument",
           [&]() { validateAnimationKeyDeletion(*curve, {12}); });
  rejected(*curve, "missing_key",
           [&]() { validateAnimationKeyDeletion(*curve, {0, 99}); });
  validateAnimationKeyDeletion(*curve, {0});
  validateAnimationKeyDeletion(*curve, {36});
  verifyMixedCurve(curve);
  curve->setEditorLocked(true);
  rejected(*curve, "locked",
           [&]() { prepareAnimationKeyEdit(*curve, mixedRecords()); });
  curve->setEditorLocked(false);
  curve->enableCycle(true);
  rejected(*curve, "unsupported_interpolation",
           [&]() { prepareAnimationKeyEdit(*curve, mixedRecords()); });
  curve->enableCycle(false);
  auto linked            = curve->getKeyframe(1);
  linked.m_linkedHandles = true;
  curve->setKeyframe(linked);
  require(!animationUnsupportedReason(*curve).isEmpty(),
          "Linked rich controls were advertised as writable");
  rejected(*curve, "unsupported_interpolation",
           [&]() { prepareAnimationKeyEdit(*curve, mixedRecords()); });
}

QJsonObject mapping(int numerator, int denominator, int shift = 0,
                    int origin = 0) {
  return {{"origin_frame", origin},
          {"shift_frames", shift},
          {"scale", QJsonObject{{"numerator", numerator},
                                {"denominator", denominator}}}};
}

void verifyDestinationIdentity(const TDoubleParam& destination) {
  double minimum = 0, maximum = 0, step = 0;
  destination.getValueRange(minimum, maximum, step);
  require(destination.getName() == "destination" &&
              destination.getMeasureName() == "length" &&
              destination.getWorkspaceId() == "destination-workspace" &&
              !destination.isEditorLocked() && minimum == -5 && maximum == 9,
          "Whole copy replaced destination identity or editor metadata");
}

void wholeCurveCopyPreservesOwnership() {
  const auto source = mixedCurve();
  source->setMeasureName("length");
  for (int index = 0; index < source->getKeyframeCount(); ++index)
    source->setKeyframe(index, source->getKeyframe(index));
  source->setDefaultValue(23);
  source->setEditorLocked(true);
  TDoubleParamP destination(new TDoubleParam(7));
  destination->setName("destination");
  destination->setMeasureName("length");
  destination->setWorkspaceId("destination-workspace");
  destination->setValueRange(-5, 9);
  CurveObserver observer(*destination);
  const auto edit = prepareAnimationCurveCopy(*source, *destination);
  require(edit.changed && observer.changes == 0,
          "Copy preflight mutated destination");
  applyAnimationCurveEdit(*destination, edit);
  verifyMixedCurve(destination);
  near(destination->getDefaultValue(), 23);
  verifyDestinationIdentity(*destination);
  const int changes = observer.changes;
  require(!prepareAnimationCurveCopy(*source, *destination).changed &&
              !prepareAnimationCurveCopy(*destination, *destination).changed &&
              observer.changes == changes,
          "Equal whole curve copy was not a no-op");
  source->clearKeyframes();
  source->setDefaultValue(99);
  verifyMixedCurve(destination);
  near(destination->getDefaultValue(), 23);
}

void wholeCurveInactiveMetadataAndDefaults() {
  TDoubleParamP source(new TDoubleParam(17));
  TDoubleParamP destination(new TDoubleParam());
  source->setMeasureName("length");
  destination->setMeasureName("length");
  auto stored = key(4, 8, TDoubleKeyframe::Linear, {-7, 6}, {8, 9});
  stored.m_fileParams.m_fieldIndex = 42;
  stored.m_unitName                = "mm";
  stored.m_similarShapeOffset      = 13;
  source->setKeyframe(stored);
  applyAnimationCurveEdit(*destination,
                          prepareAnimationCurveCopy(*source, *destination));
  const auto& copied = destination->getKeyframe(0);
  require(copied.m_speedIn == stored.m_speedIn &&
              copied.m_speedOut == stored.m_speedOut &&
              copied.m_fileParams.m_fieldIndex == 42 &&
              copied.m_unitName == "mm" && copied.m_similarShapeOffset == 13 &&
              copied.m_linkedHandles == stored.m_linkedHandles,
          "Copy discarded inactive native metadata");
  source->clearKeyframes();
  applyAnimationCurveEdit(*destination,
                          prepareAnimationCurveCopy(*source, *destination));
  require(destination->getKeyframeCount() == 0,
          "Empty copy retained destination keys");
  near(destination->getValue(10), 17);
  require(!prepareAnimationCurveRetime(*destination, mapping(7, 3, 8)).changed,
          "An empty curve retime changed content");
}

void verifyMappedSamples(const TDoubleParam& before, const TDoubleParam& after,
                         double scale, double shift) {
  for (int quarter = 0; quarter <= 144; ++quarter) {
    const double frame = quarter / 4.0;
    near(after.getValue(frame * scale + shift), before.getValue(frame));
  }
}

void wholeCurveRetimeAndRoundTrip() {
  const auto original = mixedCurve();
  TDoubleParamP retimed(new TDoubleParam(*original));
  const auto edit = prepareAnimationCurveRetime(*retimed, mapping(3, 2, 4));
  require(edit.changed &&
              inspectAnimationKeys(*retimed) == inspectAnimationKeys(*original),
          "Retime preflight mutated the curve");
  applyAnimationCurveEdit(*retimed, edit);
  verifyMappedSamples(*original, *retimed, 1.5, 4);
  near(retimed->getKeyframe(1).m_speedIn.y, -24);
  near(retimed->getKeyframe(1).m_speedIn.x, -6);
  near(retimed->getKeyframe(1).m_speedOut.x, 4.5);
  require(!prepareAnimationCurveRetime(*retimed, mapping(7, 7)).changed,
          "Identity rational retime was not a no-op");
  QTemporaryDir directory;
  require(directory.isValid(), "Retime fixture directory is unavailable");
  const TFilePath path(directory.filePath("retimed.xml"));
  {
    TOStream output(path);
    retimed->saveData(output);
  }
  TDoubleParamP reopened(new TDoubleParam());
  {
    TIStream input(path);
    reopened->loadData(input);
  }
  verifyMappedSamples(*original, *reopened, 1.5, 4);
}

void wholeCurveStepsAndRejections() {
  const auto curve = mixedCurve();
  for (const auto& invalidMapping :
       {mapping(1, 5), mapping(1, 1, -1), mapping(1000000, 1), mapping(0, 1),
        mapping(1, 0)})
    rejected(*curve, "invalid_params",
             [&]() { prepareAnimationCurveRetime(*curve, invalidMapping); });
  auto first   = curve->getKeyframe(0);
  first.m_step = 2;
  curve->setKeyframe(first);
  rejected(*curve, "invalid_params",
           [&]() { prepareAnimationCurveRetime(*curve, mapping(1, 2)); });
  rejected(*curve, "invalid_params",
           [&]() { prepareAnimationCurveRetime(*curve, mapping(1, 3)); });
  TDoubleParamP doubled(new TDoubleParam(*curve));
  applyAnimationCurveEdit(
      *doubled, prepareAnimationCurveRetime(*curve, mapping(2, 1, 3)));
  require(doubled->getKeyframe(0).m_step == 4 &&
              doubled->getKeyframe(1).m_step == 1,
          "Retime changed continuous steps or lost quantization");
  verifyMappedSamples(*curve, *doubled, 2, 3);
  curve->setEditorLocked(true);
  rejected(*curve, "locked",
           [&]() { prepareAnimationCurveRetime(*curve, mapping(1, 1)); });
  rejected(*curve, "locked",
           [&]() { prepareAnimationCurveCopy(*doubled, *curve); });
  curve->setEditorLocked(false);
  curve->setDefaultValue(std::numeric_limits<double>::infinity());
  rejected(*curve, "invalid_params",
           [&]() { prepareAnimationCurveCopy(*curve, *doubled); });
}

void wholeCurveUnsupportedMetadataIsAtomic() {
  const auto source = mixedCurve();
  TDoubleParamP destination(new TDoubleParam());
  auto stored             = source->getKeyframe(0);
  stored.m_expressionText = "inactive-expression";
  source->setKeyframe(stored);
  rejected(*destination, "unsupported_interpolation",
           [&]() { prepareAnimationCurveCopy(*source, *destination); });
  stored.m_expressionText.clear();
  stored.m_fileParams.m_path = TFilePath("inactive-file.txt");
  source->setKeyframe(stored);
  rejected(*destination, "unsupported_interpolation",
           [&]() { prepareAnimationCurveCopy(*source, *destination); });
  stored.m_fileParams.m_path  = TFilePath();
  stored.m_similarShapeOffset = std::numeric_limits<double>::quiet_NaN();
  source->setKeyframe(stored);
  rejected(*destination, "invalid_params",
           [&]() { prepareAnimationCurveCopy(*source, *destination); });
}

// ---- Breakdown, key move and key duplicate -------------------------------

using KeyList = std::vector<TDoubleKeyframe>;

KeyList storedKeys(const TDoubleParam& curve) {
  KeyList result;
  for (int index = 0; index < curve.getKeyframeCount(); ++index)
    result.push_back(curve.getKeyframe(index));
  return result;
}

bool identical(const TDoubleKeyframe& left, const TDoubleKeyframe& right) {
  return left.m_frame == right.m_frame && left.m_value == right.m_value &&
         left.m_type == right.m_type && left.m_prevType == right.m_prevType &&
         left.m_isKeyframe == right.m_isKeyframe &&
         left.m_step == right.m_step && left.m_speedIn == right.m_speedIn &&
         left.m_speedOut == right.m_speedOut &&
         left.m_linkedHandles == right.m_linkedHandles &&
         left.m_expressionText == right.m_expressionText &&
         left.m_fileParams.m_path == right.m_fileParams.m_path &&
         left.m_fileParams.m_fieldIndex == right.m_fileParams.m_fieldIndex &&
         left.m_unitName == right.m_unitName &&
         left.m_similarShapeOffset == right.m_similarShapeOffset;
}

bool sameKeyLists(const KeyList& left, const KeyList& right) {
  if (left.size() != right.size()) return false;
  for (std::size_t index = 0; index < left.size(); ++index)
    if (!identical(left[index], right[index])) return false;
  return true;
}

const TDoubleKeyframe* storedKeyAt(const KeyList& keys, double frame) {
  for (const auto& candidate : keys)
    if (candidate.m_frame == frame) return &candidate;
  return nullptr;
}

// Frames from before the first key to well after the last, in quarters.
std::vector<double> denseFrames(double last) {
  std::vector<double> frames;
  for (double frame = -2; frame <= last + 4; frame += 0.25)
    frames.push_back(frame);
  return frames;
}

// Segments the edit left alone sample bit-identically; segments between the
// edited keys are free to change.
void requireSamplesOutside(const TDoubleParam& before,
                           const TDoubleParam& after, double from, double to,
                           const char* message) {
  for (const double frame : denseFrames(80)) {
    if (frame > from && frame < to) continue;
    require(before.getValue(frame) == after.getValue(frame), message);
  }
}

void requireOtherKeysIdentical(const KeyList& before, const KeyList& after,
                               const std::vector<double>& exceptions) {
  for (const auto& original : before) {
    bool skipped = false;
    for (const double frame : exceptions) skipped |= frame == original.m_frame;
    if (skipped) continue;
    const auto* current = storedKeyAt(after, original.m_frame);
    require(current && identical(original, *current),
            "An untouched key changed");
  }
}

TDoubleParamP editedCurve() {
  const auto curve = mixedCurve();
  curve->setKeyframe(key(48, 100, TDoubleKeyframe::Linear, {}, {}));
  auto held   = curve->getKeyframe(3);
  held.m_step = 2;
  curve->setKeyframe(held);
  return curve;
}

QJsonObject breakdown(int frame, double favour) {
  return {{"frame", frame}, {"favour_percent", favour}};
}

TDoubleParamP appliedBreakdown(const TDoubleParam& curve,
                               const QJsonObject& request) {
  TDoubleParamP edited(new TDoubleParam(curve));
  applyAnimationKeyEdit(*edited, prepareAnimationBreakdown(curve, request));
  return edited;
}

void breakdownSplitsEverySegmentKind() {
  struct Case {
    int frame;
    double favour;
    double previous;
    double next;
  };
  const std::array<Case, 6> cases{Case{5, 25, 0, 12},    Case{14, 50, 12, 24},
                                  Case{18, 50, 12, 24},  Case{22, 50, 12, 24},
                                  Case{30, 110, 24, 36}, Case{40, -10, 36, 48}};
  for (const auto& item : cases) {
    const auto curve  = editedCurve();
    const auto before = storedKeys(*curve);
    const auto edited =
        appliedBreakdown(*curve, breakdown(item.frame, item.favour));
    require(sameKeyLists(before, storedKeys(*curve)),
            "Preparing a breakdown changed the source curve");
    const auto after = storedKeys(*edited);
    require(after.size() == before.size() + 1,
            "A breakdown must add exactly one key");
    const auto* inserted = storedKeyAt(after, item.frame);
    const auto* previous = storedKeyAt(before, item.previous);
    const auto* next     = storedKeyAt(before, item.next);
    require(inserted && previous && next, "Breakdown segment ends are missing");
    near(inserted->m_value,
         previous->m_value +
             (next->m_value - previous->m_value) * (item.favour / 100.0));
    require(inserted->m_type == previous->m_type &&
                inserted->m_step == previous->m_step && inserted->m_isKeyframe,
            "The new key must keep its segment's interpolation and step");
    requireOtherKeysIdentical(before, after, {item.previous, item.next});
    requireSamplesOutside(*curve, *edited, item.previous, item.next,
                          "A breakdown changed a segment it does not touch");
    const auto* first = storedKeyAt(after, item.previous);
    require(
        first->m_frame == previous->m_frame &&
            first->m_value == previous->m_value &&
            first->m_type == previous->m_type &&
            first->m_step == previous->m_step &&
            first->m_speedIn == previous->m_speedIn,
        "A split segment's start key changed more than its outgoing handle");
    const auto* last = storedKeyAt(after, item.next);
    require(last->m_frame == next->m_frame && last->m_value == next->m_value &&
                last->m_type == next->m_type && last->m_step == next->m_step &&
                last->m_speedOut == next->m_speedOut,
            "A split segment's end key changed more than its incoming handle");
  }
}

void breakdownMatchesTheEditor() {
  for (const int frame : {5, 14, 18, 22, 30, 40}) {
    const auto curve  = editedCurve();
    const auto edited = appliedBreakdown(*curve, breakdown(frame, 40));
    TDoubleParamP native(new TDoubleParam(*curve));
    require(KeyframeSetter::insertBreakdown(native.getPointer(), frame, 0.4),
            "The editor refused a supported breakdown");
    const auto ours   = storedKeys(*edited);
    const auto theirs = storedKeys(*native);
    require(ours.size() == theirs.size(), "Editor key count differs");
    for (std::size_t index = 0; index < ours.size(); ++index) {
      require(ours[index].m_frame == theirs[index].m_frame &&
                  ours[index].m_type == theirs[index].m_type &&
                  ours[index].m_step == theirs[index].m_step,
              "Editor key order or type differs");
      near(ours[index].m_value, theirs[index].m_value);
    }
    for (const double sample : denseFrames(80)) {
      // The editor leaves the next ease key's incoming ease longer than its
      // new segment (the evaluator rebalances it to the clamp this tool
      // writes) and, for a frame inside that last ease, writes the clamp into
      // the previous key's incoming handle, which bends the segment before it.
      if (frame == 22 && sample < 12) continue;
      near(edited->getValue(sample), native->getValue(sample));
    }
  }
}

void breakdownOnCurveValueKeepsLinearAndSpeedShapes() {
  const auto curve = editedCurve();
  for (const int frame : {5, 30, 40}) {
    const auto value = curve->getValue(frame);
    const auto edited =
        appliedBreakdown(*curve, {{"frame", frame}, {"value", value}});
    for (const double sample : denseFrames(80))
      require(
          std::abs(edited->getValue(sample) - curve->getValue(sample)) < 1e-4,
          "A breakdown on the curve changed its shape");
  }
}

void breakdownSurvivesSaveAndReopen() {
  const auto curve  = editedCurve();
  const auto edited = appliedBreakdown(*curve, breakdown(5, 75));
  QTemporaryDir directory;
  require(directory.isValid(), "Breakdown fixture directory is unavailable");
  const TFilePath path(directory.filePath("breakdown.xml"));
  {
    TOStream output(path);
    edited->saveData(output);
  }
  TDoubleParamP reopened(new TDoubleParam());
  {
    TIStream input(path);
    reopened->loadData(input);
  }
  require(reopened->getKeyframeCount() == edited->getKeyframeCount(),
          "Reopening changed the breakdown key count");
  for (const double sample : denseFrames(80))
    require(reopened->getValue(sample) == edited->getValue(sample),
            "Reopening changed a breakdown sample");
}

void rejectedFor(TDoubleParam& curve, const QString& reason,
                 const std::function<void()>& action) {
  const auto before = inspectAnimationKeys(curve);
  try {
    action();
  } catch (const Error& error) {
    require(error.details.value("reason").toString() == reason,
            "The refusal does not name its reason");
    require(inspectAnimationKeys(curve) == before,
            "A refused breakdown changed the curve");
    return;
  }
  throw std::runtime_error("A refusal for " + reason.toStdString() +
                           " did not happen");
}

void breakdownRefusals() {
  TDoubleParamP linear(new TDoubleParam());
  for (const int frame : {0, 10, 20})
    linear->setKeyframe(key(frame, frame, TDoubleKeyframe::Linear, {}, {}));
  rejectedFor(*linear, "key_exists",
              [&]() { prepareAnimationBreakdown(*linear, breakdown(10, 50)); });
  rejectedFor(*linear, "outside_segment",
              [&]() { prepareAnimationBreakdown(*linear, breakdown(25, 50)); });
  rejectedFor(*linear, "favour_percent_or_value", [&]() {
    prepareAnimationBreakdown(
        *linear, {{"frame", 5}, {"value", 1}, {"favour_percent", 1}});
  });
  TDoubleParamP wide(new TDoubleParam());
  wide->setKeyframe(key(0, 0, TDoubleKeyframe::Linear, {}, {}));
  wide->setKeyframe(key(10, 900000, TDoubleKeyframe::Linear, {}, {}));
  rejectedFor(*wide, "value_out_of_bounds",
              [&]() { prepareAnimationBreakdown(*wide, breakdown(5, 200)); });
  rejected(*wide, "invalid_argument", [&]() {
    prepareAnimationBreakdown(*wide, {{"frame", 5}, {"value", 1000000.5}});
  });
  const auto curve = editedCurve();
  rejected(*curve, "invalid_argument",
           [&]() { prepareAnimationBreakdown(*curve, breakdown(12, 50)); });
  rejected(*curve, "invalid_argument",
           [&]() { prepareAnimationBreakdown(*curve, breakdown(60, 50)); });
  TDoubleParamP empty(new TDoubleParam());
  rejected(*empty, "invalid_argument",
           [&]() { prepareAnimationBreakdown(*empty, breakdown(3, 50)); });
  rejected(*curve, "invalid_argument",
           [&]() { prepareAnimationBreakdown(*curve, {{"frame", 5}}); });
  rejected(*curve, "invalid_argument", [&]() {
    prepareAnimationBreakdown(
        *curve, {{"frame", 5}, {"value", 1}, {"favour_percent", 1}});
  });
  rejected(*curve, "invalid_argument",
           [&]() { prepareAnimationBreakdown(*curve, breakdown(5, 201)); });
  rejected(*curve, "invalid_argument", [&]() {
    prepareAnimationBreakdown(*curve, {{"frame", 5}, {"value", 2000000}});
  });
  rejected(*curve, "invalid_argument", [&]() {
    prepareAnimationBreakdown(*curve, {{"frame", 5.5}, {"favour_percent", 50}});
  });
  curve->setEditorLocked(true);
  rejected(*curve, "locked",
           [&]() { prepareAnimationBreakdown(*curve, breakdown(5, 50)); });
  curve->setEditorLocked(false);
  curve->enableCycle(true);
  rejected(*curve, "unsupported_interpolation",
           [&]() { prepareAnimationBreakdown(*curve, breakdown(5, 50)); });
  curve->enableCycle(false);
  auto expression             = curve->getKeyframe(2);
  expression.m_type           = TDoubleKeyframe::Expression;
  expression.m_expressionText = "frame";
  curve->setKeyframe(expression);
  rejected(*curve, "unsupported_interpolation",
           [&]() { prepareAnimationBreakdown(*curve, breakdown(5, 50)); });
}

void breakdownRespectsTheKeyLimit() {
  TDoubleParamP curve(new TDoubleParam());
  for (int index = 0; index < 10000; ++index)
    curve->setKeyframe(key(index * 2, index, TDoubleKeyframe::Linear, {}, {}));
  rejected(*curve, "resource_limit",
           [&]() { prepareAnimationBreakdown(*curve, breakdown(1, 50)); });
}

// ---- Key move and key duplicate ------------------------------------------

using Frames = std::vector<int>;

TDoubleParamP appliedMove(const TDoubleParam& curve, const Frames& frames,
                          int frameOffset, double valueOffset) {
  TDoubleParamP edited(new TDoubleParam(curve));
  applyAnimationKeyEdit(*edited, prepareAnimationKeyMove(
                                     curve, frames, frameOffset, valueOffset));
  return edited;
}

TDoubleParamP appliedDuplicate(const TDoubleParam& curve, const Frames& frames,
                               int frameOffset, double valueOffset = 0) {
  TDoubleParamP edited(new TDoubleParam(curve));
  applyAnimationKeyEdit(*edited, prepareAnimationKeyDuplicate(
                                     curve, frames, frameOffset, valueOffset));
  return edited;
}

void requireReopenedSamples(const TDoubleParam& curve, const char* name) {
  QTemporaryDir directory;
  require(directory.isValid(), "Move fixture directory is unavailable");
  const TFilePath path(directory.filePath(name));
  {
    TOStream output(path);
    const_cast<TDoubleParam&>(curve).saveData(output);
  }
  TDoubleParamP reopened(new TDoubleParam());
  {
    TIStream input(path);
    reopened->loadData(input);
  }
  require(sameKeyLists(storedKeys(curve), storedKeys(*reopened)) ||
              reopened->getKeyframeCount() == curve.getKeyframeCount(),
          "Reopening changed the key count");
  for (const double sample : denseFrames(120))
    require(reopened->getValue(sample) == curve.getValue(sample),
            "Reopening changed a sample");
}

void moveShiftsOnlyTheSelectedKeys() {
  struct Case {
    Frames frames;
    int frameOffset;
    double valueOffset;
    double from;
    double to;
  };
  // from and to bound the segments the move may change; everything else is
  // compared bit for bit.
  const std::array<Case, 5> cases{
      Case{{36}, 4, 10, 24, 48}, Case{{0}, 2, 0, 0, 12},
      Case{{48}, 3, -5, 36, 1e9}, Case{{12, 24}, 3, 0, 0, 36},
      Case{{24}, 0, 5, 12, 36}};
  for (const auto& item : cases) {
    const auto curve  = editedCurve();
    const auto before = storedKeys(*curve);
    const auto edited =
        appliedMove(*curve, item.frames, item.frameOffset, item.valueOffset);
    require(sameKeyLists(before, storedKeys(*curve)),
            "Preparing a move changed the source curve");
    const auto after = storedKeys(*edited);
    require(after.size() == before.size(), "A move changed the key count");
    std::vector<double> touched;
    for (const int frame : item.frames) {
      touched.push_back(frame);
      const auto* source = storedKeyAt(before, frame);
      const auto* moved  = storedKeyAt(after, frame + item.frameOffset);
      require(source && moved, "A moved key is missing");
      require(moved->m_type == source->m_type &&
                  moved->m_step == source->m_step &&
                  moved->m_speedIn == source->m_speedIn &&
                  moved->m_speedOut == source->m_speedOut &&
                  moved->m_linkedHandles == source->m_linkedHandles &&
                  moved->m_unitName == source->m_unitName &&
                  moved->m_value == source->m_value + item.valueOffset,
              "A moved key changed more than its frame and value");
    }
    for (const auto& original : before) {
      bool moved = false;
      for (const double frame : touched) moved |= frame == original.m_frame;
      if (!moved)
        require(storedKeyAt(after, original.m_frame) &&
                    identical(original, *storedKeyAt(after, original.m_frame)),
                "An unselected key changed in a move");
    }
    requireSamplesOutside(*curve, *edited, item.from, item.to,
                          "A move changed a segment it does not touch");
    requireReopenedSamples(*edited, "moved.xml");
  }
}

void movedGroupKeepsItsInnerSegmentExactly() {
  const auto curve  = editedCurve();
  const auto edited = appliedMove(*curve, {12, 24}, 3, 0);
  for (double frame = 12; frame <= 24; frame += 0.25)
    require(edited->getValue(frame + 3) == curve->getValue(frame),
            "A moved group's inner segment changed shape");
}

void moveMatchesTheEditor() {
  const std::array<std::pair<Frames, std::pair<int, double>>, 3> cases{
      std::make_pair(Frames{36}, std::make_pair(4, 10.0)),
      std::make_pair(Frames{0}, std::make_pair(2, 0.0)),
      std::make_pair(Frames{24}, std::make_pair(0, 5.0))};
  for (const auto& item : cases) {
    const auto curve = editedCurve();
    const auto edited =
        appliedMove(*curve, item.first, item.second.first, item.second.second);
    TDoubleParamP native(new TDoubleParam(*curve));
    KeyframeSetter setter(native.getPointer(),
                          native->getClosestKeyframe(item.first[0]), false);
    setter.moveKeyframes(item.second.first, item.second.second);
    require(sameKeyLists(storedKeys(*edited), storedKeys(*native)),
            "The moved keys differ from the editor's");
    for (const double sample : denseFrames(80))
      require(edited->getValue(sample) == native->getValue(sample),
              "A moved sample differs from the editor's");
  }
}

void moveRefusals() {
  const auto curve = editedCurve();
  rejectedFor(*curve, "key_exists",
              [&]() { prepareAnimationKeyMove(*curve, {12}, 12, 0); });
  rejectedFor(*curve, "order_changed",
              [&]() { prepareAnimationKeyMove(*curve, {12}, 13, 0); });
  rejectedFor(*curve, "order_changed",
              [&]() { prepareAnimationKeyMove(*curve, {24}, -13, 0); });
  rejectedFor(*curve, "controls",
              [&]() { prepareAnimationKeyMove(*curve, {12}, 10, 0); });
  rejectedFor(*curve, "frame_out_of_range",
              [&]() { prepareAnimationKeyMove(*curve, {0}, -1, 0); });
  rejectedFor(*curve, "frame_out_of_range",
              [&]() { prepareAnimationKeyMove(*curve, {48}, 1000000, 0); });
  rejectedFor(*curve, "frame_out_of_range",
              [&]() { prepareAnimationKeyMove(*curve, {48}, 2000000, 0); });
  rejectedFor(*curve, "value_out_of_bounds",
              [&]() { prepareAnimationKeyMove(*curve, {36}, 0, 1999999); });
  rejectedFor(*curve, "duplicate_frame",
              [&]() { prepareAnimationKeyMove(*curve, {36, 36}, 1, 0); });
  rejected(*curve, "missing_key",
           [&]() { prepareAnimationKeyMove(*curve, {36, 37}, 1, 0); });
  rejected(*curve, "resource_limit",
           [&]() { prepareAnimationKeyMove(*curve, {}, 1, 0); });
  const auto noop = prepareAnimationKeyMove(*curve, {36}, 0, 0);
  require(!noop.changed && noop.keys.empty() && noop.deletedFrames.empty(),
          "A zero move was not a no-op");
  curve->setEditorLocked(true);
  rejected(*curve, "locked",
           [&]() { prepareAnimationKeyMove(*curve, {36}, 1, 0); });
  curve->setEditorLocked(false);
  curve->enableCycle(true);
  rejected(*curve, "unsupported_interpolation",
           [&]() { prepareAnimationKeyMove(*curve, {36}, 1, 0); });
  curve->enableCycle(false);
  auto expression             = curve->getKeyframe(2);
  expression.m_type           = TDoubleKeyframe::Expression;
  expression.m_expressionText = "frame";
  curve->setKeyframe(expression);
  rejected(*curve, "unsupported_interpolation",
           [&]() { prepareAnimationKeyMove(*curve, {36}, 1, 0); });
  rejected(*curve, "unsupported_interpolation",
           [&]() { prepareAnimationKeyDuplicate(*curve, {36}, 70); });
}

void duplicateCopiesRecordsVerbatim() {
  struct Case {
    Frames frames;
    int frameOffset;
    double valueOffset;
  };
  const std::array<Case, 3> cases{Case{{0}, 60, 0}, Case{{36, 48}, 20, 7},
                                  Case{{36}, 4, 0}};
  for (const auto& item : cases) {
    const auto curve  = editedCurve();
    const auto before = storedKeys(*curve);
    const auto edited = appliedDuplicate(*curve, item.frames, item.frameOffset,
                                         item.valueOffset);
    require(sameKeyLists(before, storedKeys(*curve)),
            "Preparing a duplicate changed the source curve");
    const auto after = storedKeys(*edited);
    require(after.size() == before.size() + item.frames.size(),
            "A duplicate added the wrong number of keys");
    for (const auto& original : before)
      require(storedKeyAt(after, original.m_frame) &&
                  identical(original, *storedKeyAt(after, original.m_frame)),
              "A duplicate changed an existing key");
    double last = 0;
    for (const int frame : item.frames) {
      const auto* source = storedKeyAt(before, frame);
      const auto* copy   = storedKeyAt(after, frame + item.frameOffset);
      require(copy && copy->m_type == source->m_type &&
                  copy->m_step == source->m_step &&
                  copy->m_speedIn == source->m_speedIn &&
                  copy->m_speedOut == source->m_speedOut &&
                  copy->m_linkedHandles == source->m_linkedHandles &&
                  copy->m_value == source->m_value + item.valueOffset,
              "A copy differs from its source");
      last = std::max<double>(last, frame + item.frameOffset);
    }
    const double first = item.frames.front() + item.frameOffset;
    requireSamplesOutside(*curve, *edited, std::min(first, 48.0) - 0.0,
                          last + 100,
                          "A duplicate changed a segment before it");
    requireReopenedSamples(*edited, "duplicated.xml");
  }
}

void duplicateRefusals() {
  const auto curve = editedCurve();
  rejectedFor(*curve, "key_exists",
              [&]() { prepareAnimationKeyDuplicate(*curve, {0}, 12); });
  rejectedFor(*curve, "key_exists",
              [&]() { prepareAnimationKeyDuplicate(*curve, {0}, 0, 3); });
  rejectedFor(*curve, "incoming_kind_changed",
              [&]() { prepareAnimationKeyDuplicate(*curve, {12, 24}, 40); });
  rejectedFor(*curve, "incoming_kind_changed",
              [&]() { prepareAnimationKeyDuplicate(*curve, {24}, 40); });
  rejectedFor(*curve, "incoming_kind_changed",
              [&]() { prepareAnimationKeyDuplicate(*curve, {36}, -30); });
  rejectedFor(*curve, "incoming_kind_changed",
              [&]() { prepareAnimationKeyDuplicate(*curve, {0}, 18); });
  rejectedFor(*curve, "frame_out_of_range",
              [&]() { prepareAnimationKeyDuplicate(*curve, {0}, -1); });
  rejectedFor(*curve, "value_out_of_bounds", [&]() {
    prepareAnimationKeyDuplicate(*curve, {36}, 70, 1999999);
  });
  rejectedFor(*curve, "duplicate_frame",
              [&]() { prepareAnimationKeyDuplicate(*curve, {36, 36}, 70); });
  rejected(*curve, "missing_key",
           [&]() { prepareAnimationKeyDuplicate(*curve, {35}, 70); });
  curve->setEditorLocked(true);
  rejected(*curve, "locked",
           [&]() { prepareAnimationKeyDuplicate(*curve, {36}, 70); });
  curve->setEditorLocked(false);
  curve->enableCycle(true);
  rejected(*curve, "unsupported_interpolation",
           [&]() { prepareAnimationKeyDuplicate(*curve, {36}, 70); });
  TDoubleParamP full(new TDoubleParam());
  for (int index = 0; index < 9999; ++index)
    full->setKeyframe(key(index * 2, index, TDoubleKeyframe::Linear, {}, {}));
  rejected(*full, "resource_limit",
           [&]() { prepareAnimationKeyDuplicate(*full, {0, 2}, 30001); });
  appliedDuplicate(*full, {0}, 30001);
}

// ---- Cyclic curves --------------------------------------------------------

TDoubleParamP cyclicCurve() {
  const auto curve = editedCurve();
  curve->enableCycle(true);
  return curve;
}

// The editor's cycle folds a frame past the last key back by the key span and
// adds the last-minus-first key value per fold. Checked against the same keys
// evaluated without a cycle.
double foldedValue(const TDoubleParam& curve, double frame) {
  TDoubleParamP plain(new TDoubleParam(curve));
  plain->enableCycle(false);
  const double first = plain->getKeyframe(0).m_frame;
  const double last = plain->getKeyframe(plain->getKeyframeCount() - 1).m_frame;
  const double drift =
      plain->getKeyframe(plain->getKeyframeCount() - 1).m_value -
      plain->getKeyframe(0).m_value;
  if (frame < first) return plain->getValue(first);
  const double folds = std::floor((frame - first) / (last - first));
  return plain->getValue(frame - folds * (last - first)) + folds * drift;
}

void requireCyclicSamples(const TDoubleParam& curve) {
  for (double frame = -2; frame <= 400; frame += 0.375) {
    double value = 0;
    std::string error;
    require(curve.getValueChecked(frame, value, error),
            "A cyclic sample failed");
    near(value, foldedValue(curve, frame));
  }
  for (const double frame : {48.0, 96.0, 144.0, 1000.0, 123456.5})
    near(curve.getValue(frame), foldedValue(curve, frame));
}

void cyclicCurvesAreAdmittedOnlyOnRequest() {
  const auto curve = cyclicCurve();
  require(!animationUnsupportedReason(*curve, CyclePolicy::Refuse).isEmpty(),
          "A cyclic curve was admitted without the cycle policy");
  require(animationUnsupportedReason(*curve, CyclePolicy::Allow).isEmpty(),
          "A cyclic curve was refused under the cycle policy");
  rejected(*curve, "unsupported_interpolation",
           [&]() { prepareAnimationKeyEdit(*curve, mixedRecords()); });
  rejected(*curve, "unsupported_interpolation",
           [&]() { validateAnimationKeyDeletion(*curve, {36}); });
  rejected(*curve, "unsupported_interpolation",
           [&]() { prepareAnimationBreakdown(*curve, breakdown(5, 50)); });
  rejected(*curve, "unsupported_interpolation",
           [&]() { prepareAnimationKeyMove(*curve, {36}, 1, 0); });
  rejected(*curve, "unsupported_interpolation",
           [&]() { prepareAnimationKeyDuplicate(*curve, {36}, 70); });
  requireCyclicSamples(*curve);
}

void cyclicEditsKeepTheFlagAndUntouchedSegments() {
  const auto curve  = cyclicCurve();
  const auto before = storedKeys(*curve);
  const std::array<TDoubleParamP, 3> edited{
      [&]() {
        TDoubleParamP copy(new TDoubleParam(*curve));
        applyAnimationKeyEdit(
            *copy, prepareAnimationBreakdown(*curve, breakdown(40, 50),
                                             CyclePolicy::Allow));
        return copy;
      }(),
      [&]() {
        TDoubleParamP copy(new TDoubleParam(*curve));
        applyAnimationKeyEdit(
            *copy,
            prepareAnimationKeyMove(*curve, {36}, 4, 10, CyclePolicy::Allow));
        return copy;
      }(),
      [&]() {
        TDoubleParamP copy(new TDoubleParam(*curve));
        applyAnimationKeyEdit(*copy,
                              prepareAnimationKeyDuplicate(*curve, {36}, 4, 0,
                                                           CyclePolicy::Allow));
        return copy;
      }()};
  for (const auto& next : edited) {
    require(next->isCycleEnabled(), "A key edit cleared the cycle");
    require(sameKeyLists(before, storedKeys(*curve)),
            "Preparing a cyclic edit changed the source curve");
    requireCyclicSamples(*next);
    // The segments before frame 24 repeat every 48 frames, bit for bit.
    for (double frame = 0; frame <= 400; frame += 0.375)
      if (std::fmod(frame, 48) < 24)
        require(next->getValue(frame) == curve->getValue(frame),
                "A cyclic edit changed a segment it does not touch");
  }
  const auto batch =
      prepareAnimationKeyEdit(*curve, {mixedRecords()[0]}, CyclePolicy::Allow);
  require(!batch.changed, "An unchanged cyclic key was rewritten");
  validateAnimationKeyDeletion(*curve, {36}, CyclePolicy::Allow);
}

void cycleFlagChangesOnlyTheFlag() {
  const auto curve  = editedCurve();
  const auto before = storedKeys(*curve);
  require(prepareAnimationCycle(*curve, true), "Enabling a cycle was a no-op");
  require(!prepareAnimationCycle(*curve, false),
          "Disabling a plain curve changed it");
  curve->enableCycle(true);
  require(!prepareAnimationCycle(*curve, true), "Repeating a cycle changed it");
  require(sameKeyLists(before, storedKeys(*curve)),
          "The cycle flag changed keys");
  curve->setEditorLocked(true);
  rejected(*curve, "locked", [&]() { prepareAnimationCycle(*curve, false); });
  curve->setEditorLocked(false);
  auto expression             = curve->getKeyframe(2);
  expression.m_type           = TDoubleKeyframe::Expression;
  expression.m_expressionText = "frame";
  curve->setKeyframe(expression);
  rejected(*curve, "unsupported_interpolation",
           [&]() { prepareAnimationCycle(*curve, false); });
}

void cycleSurvivesWholeCurveCopyRetimeAndReopen() {
  const auto source = cyclicCurve();
  TDoubleParamP destination(new TDoubleParam());
  const auto copy = prepareAnimationCurveCopy(*source, *destination);
  require(copy.changed && copy.cycle, "A cyclic copy did not carry its cycle");
  applyAnimationCurveEdit(*destination, copy);
  require(destination->isCycleEnabled(), "A copy dropped the cycle flag");
  requireCyclicSamples(*destination);
  require(!prepareAnimationCurveCopy(*source, *destination).changed,
          "An equal cyclic copy was not a no-op");
  const auto plain = editedCurve();
  require(prepareAnimationCurveCopy(*plain, *destination).changed,
          "Copying a plain curve over a cyclic one changed nothing");
  applyAnimationCurveEdit(*destination,
                          prepareAnimationCurveCopy(*plain, *destination));
  require(!destination->isCycleEnabled(), "A copy kept the old cycle flag");
  TDoubleParamP retimed(new TDoubleParam(*source));
  applyAnimationCurveEdit(
      *retimed, prepareAnimationCurveRetime(*source, mapping(3, 2, 4)));
  require(retimed->isCycleEnabled(), "A retime dropped the cycle flag");
  for (double frame = 0; frame <= 400; frame += 0.5)
    near(retimed->getValue(frame * 1.5 + 4), source->getValue(frame));
  QTemporaryDir directory;
  require(directory.isValid(), "Cycle fixture directory is unavailable");
  const TFilePath path(directory.filePath("cycle.xml"));
  {
    TOStream output(path);
    source->saveData(output);
  }
  TDoubleParamP reopened(new TDoubleParam());
  {
    TIStream input(path);
    reopened->loadData(input);
  }
  require(reopened->isCycleEnabled() &&
              reopened->getKeyframeCount() == source->getKeyframeCount(),
          "Reopening lost the cycle");
  for (double frame = -2; frame <= 400; frame += 0.375)
    require(reopened->getValue(frame) == source->getValue(frame),
            "Reopening changed a cyclic sample");
}

void cyclicSamplingIsBounded() {
  TDoubleParamP curve(new TDoubleParam());
  curve->setKeyframe(key(0, 0, TDoubleKeyframe::Linear, {}, {}));
  curve->setKeyframe(key(1, 1, TDoubleKeyframe::Linear, {}, {}));
  requireAnimationSampleBudget(*curve, {1000000, 1000000, 1000000});
  curve->enableCycle(true);
  requireAnimationSampleBudget(*curve, {1000000});
  rejected(*curve, "resource_limit", [&]() {
    requireAnimationSampleBudget(*curve, {1000000, 1000000, 1000000});
  });
  requireAnimationSampleBudget(*curve, {0, 0.5});
  TDoubleParamP single(new TDoubleParam());
  single->setKeyframe(key(5, 1, TDoubleKeyframe::Linear, {}, {}));
  single->enableCycle(true);
  requireAnimationSampleBudget(*single, {1000000, 1000000, 1000000});
}

}  // namespace

void runAutomationAnimationTests() {
  wholeCurveUnsupportedMetadataIsAtomic();
  wholeCurveCopyPreservesOwnership();
  wholeCurveInactiveMetadataAndDefaults();
  wholeCurveRetimeAndRoundTrip();
  wholeCurveStepsAndRejections();
  mixedSegmentControls();
  steppedFractionalSamples();
  mixedControlsRoundTrip();
  preparedEditsPreserveSourceAndDetectNoops();
  malformedRecordsAreAtomic();
  mixedKindChangesRequireExplicitEndpoints();
  richKindsChangeInBothDirections();
  simpleNoopsPreserveInactiveNativeMetadata();
  deletionAndAdmissionGuards();
  breakdownSplitsEverySegmentKind();
  breakdownMatchesTheEditor();
  breakdownOnCurveValueKeepsLinearAndSpeedShapes();
  breakdownSurvivesSaveAndReopen();
  breakdownRefusals();
  breakdownRespectsTheKeyLimit();
  moveShiftsOnlyTheSelectedKeys();
  movedGroupKeepsItsInnerSegmentExactly();
  moveMatchesTheEditor();
  moveRefusals();
  duplicateCopiesRecordsVerbatim();
  duplicateRefusals();
  cyclicCurvesAreAdmittedOnlyOnRequest();
  cyclicEditsKeepTheFlagAndUntouchedSegments();
  cycleFlagChangesOnlyTheFlag();
  cycleSurvivesWholeCurveCopyRetimeAndReopen();
  cyclicSamplingIsBounded();
  runAutomationAnimationTargetTests();
}

}  // namespace GroundzeroAutomation
