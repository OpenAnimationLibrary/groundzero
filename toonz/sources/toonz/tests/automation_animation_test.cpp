#include "automation.h"
#include "automation_animation.h"
#include "tests/automation_animation_test.h"

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
  curve->enableCycle(true);
  rejected(*curve, "unsupported_interpolation",
           [&]() { prepareAnimationCurveCopy(*curve, *doubled); });
  curve->enableCycle(false);
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
}

}  // namespace GroundzeroAutomation
