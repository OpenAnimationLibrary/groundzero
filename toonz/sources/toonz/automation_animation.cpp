#include "automation_animation.h"
#include "automation.h"

#include "tdoubleparam.h"

#include <QJsonObject>
#include <QSet>

#include <algorithm>
#include <array>
#include <cmath>
#include <map>

namespace GroundzeroAutomation {
namespace {

constexpr int MaxAnimationFrame    = 1000000;
constexpr int MaxAnimationKeys     = 10000;
constexpr double MaxAnimationValue = 1000000;

enum class IncomingKind { None, Speed, Ease };

struct ProposedKey final {
  TDoubleKeyframe native;
  IncomingKind incoming = IncomingKind::None;
};

[[noreturn]] void invalid(const QString& message, double frame,
                          const QString& field) {
  throw Error("invalid_argument", message,
              {{"frame", frame}, {"field", field}});
}

[[noreturn]] void refuse(const QString& reason, const QString& message,
                         double frame, const QString& field) {
  throw Error("invalid_argument", message,
              {{"frame", frame}, {"field", field}, {"reason", reason}});
}

double number(const QJsonObject& object, const QString& field, double minimum,
              double maximum, double frame) {
  const auto value    = object.value(field);
  const double result = value.toDouble();
  if (!value.isDouble() || !std::isfinite(result) || result < minimum ||
      result > maximum)
    invalid("Expected a finite number within the declared bounds.", frame,
            field);
  return result;
}

void fields(const QJsonObject& object, const QStringList& allowed, double frame,
            const QString& owner) {
  for (auto item = object.begin(); item != object.end(); ++item)
    if (!allowed.contains(item.key()))
      invalid("The record contains an inapplicable field.", frame,
              owner + item.key());
}

QJsonObject record(const QJsonValue& value, double frame,
                   const QString& field) {
  if (!value.isObject()) invalid("Expected an object.", frame, field);
  return value.toObject();
}

IncomingKind controlKind(TDoubleKeyframe::Type type) {
  if (type == TDoubleKeyframe::SpeedInOut) return IncomingKind::Speed;
  if (type == TDoubleKeyframe::EaseInOut) return IncomingKind::Ease;
  return IncomingKind::None;
}

TPointD speed(const QJsonObject& object, double frame, bool incoming) {
  fields(object,
         incoming ? QStringList{"kind", "frame_delta", "value_delta"}
                  : QStringList{"frame_delta", "value_delta"},
         frame, incoming ? "incoming." : "outgoing.");
  return {number(object, "frame_delta", incoming ? -MaxAnimationFrame : 0,
                 incoming ? 0 : MaxAnimationFrame, frame),
          number(object, "value_delta", -MaxAnimationValue, MaxAnimationValue,
                 frame)};
}

IncomingKind incomingControl(const QJsonObject& object, TDoubleKeyframe& key) {
  const auto value = object.value("incoming");
  if (value.isUndefined()) return IncomingKind::None;
  const auto incoming = record(value, key.m_frame, "incoming");
  const auto kind     = incoming.value("kind").toString();
  if (kind == "none") {
    fields(incoming, {"kind"}, key.m_frame, "incoming.");
    return IncomingKind::None;
  }
  if (kind == "speed") {
    key.m_speedIn = speed(incoming, key.m_frame, true);
    return IncomingKind::Speed;
  }
  if (kind == "ease") {
    fields(incoming, {"kind", "frames"}, key.m_frame, "incoming.");
    key.m_speedIn.x =
        -number(incoming, "frames", 0, MaxAnimationFrame, key.m_frame);
    return IncomingKind::Ease;
  }
  invalid("Incoming kind must be none, speed or ease.", key.m_frame,
          "incoming.kind");
}

void outgoingControl(const QJsonObject& object, TDoubleKeyframe& key) {
  const auto value = object.value("outgoing");
  if (controlKind(key.m_type) == IncomingKind::None) {
    if (!value.isUndefined())
      invalid("Constant and linear keys have no outgoing control.", key.m_frame,
              "outgoing");
    return;
  }
  const auto outgoing = record(value, key.m_frame, "outgoing");
  if (key.m_type == TDoubleKeyframe::SpeedInOut) {
    key.m_speedOut = speed(outgoing, key.m_frame, false);
    return;
  }
  fields(outgoing, {"frames"}, key.m_frame, "outgoing.");
  key.m_speedOut.x =
      number(outgoing, "frames", 0, MaxAnimationFrame, key.m_frame);
}

TDoubleKeyframe::Type interpolation(const QJsonObject& object, double frame) {
  const QString value = object.value("interpolation").toString();
  static const std::map<QString, TDoubleKeyframe::Type> types{
      {"constant", TDoubleKeyframe::Constant},
      {"linear", TDoubleKeyframe::Linear},
      {"ease_in_out", TDoubleKeyframe::EaseInOut},
      {"speed_in_out", TDoubleKeyframe::SpeedInOut}};
  const auto found = types.find(value);
  if (found == types.end())
    invalid("Interpolation is not in the supported native profile.", frame,
            "interpolation");
  return found->second;
}

ProposedKey proposedKey(const QJsonValue& value) {
  const auto object  = record(value, -1, "keys");
  const double frame = number(object, "frame", 0, MaxAnimationFrame, -1);
  if (frame != std::floor(frame))
    invalid("Key frames must be integers.", frame, "frame");
  fields(object,
         {"frame", "value", "interpolation", "step", "incoming", "outgoing"},
         frame, "");
  ProposedKey result;
  auto& key   = result.native;
  key.m_frame = frame;
  key.m_value =
      number(object, "value", -MaxAnimationValue, MaxAnimationValue, frame);
  key.m_type       = interpolation(object, frame);
  key.m_isKeyframe = true;
  if (object.contains("step")) {
    const double step = number(object, "step", 1, 1000, frame);
    if (step != std::floor(step))
      invalid("Step must be an integer.", frame, "step");
    key.m_step = static_cast<int>(step);
  }
  result.incoming = incomingControl(object, key);
  outgoingControl(object, key);
  key.m_linkedHandles = controlKind(key.m_type) == IncomingKind::None &&
                        result.incoming == IncomingKind::None;
  return result;
}

bool supportedType(TDoubleKeyframe::Type type) {
  return type == TDoubleKeyframe::Linear || type == TDoubleKeyframe::Constant ||
         type == TDoubleKeyframe::EaseInOut ||
         type == TDoubleKeyframe::SpeedInOut;
}

void validateNativeKey(const TDoubleKeyframe& key) {
  if (!supportedType(key.m_type))
    throw Error("unsupported_interpolation",
                "The curve contains an unsupported key.",
                {{"frame", key.m_frame}});
  const bool controls = controlKind(key.m_type) != IncomingKind::None ||
                        controlKind(key.m_prevType) != IncomingKind::None;
  if (controls && key.m_linkedHandles)
    throw Error("unsupported_interpolation",
                "Linked rich controls are not writable.",
                {{"frame", key.m_frame}});
  if (!key.m_expressionText.empty() || !key.m_fileParams.m_path.isEmpty())
    throw Error("unsupported_interpolation",
                "The curve contains external key metadata.");
}

void validateNumbers(const TDoubleKeyframe& key) {
  const std::array<double, 6> values{key.m_frame,      key.m_value,
                                     key.m_speedIn.x,  key.m_speedIn.y,
                                     key.m_speedOut.x, key.m_speedOut.y};
  for (const double value : values)
    if (!std::isfinite(value) || std::abs(value) > MaxAnimationValue)
      invalid("Native key or control exceeds finite profile bounds.",
              key.m_frame, "key");
  if (key.m_frame < 0 || key.m_frame != std::floor(key.m_frame))
    invalid("Native key frames must be nonnegative integers.", key.m_frame,
            "frame");
  if (key.m_step < 1 || key.m_step > 1000)
    invalid("Native step is outside 1 through 1000.", key.m_frame, "step");
}

bool controlsFit(const TDoubleKeyframe& first, const TDoubleKeyframe& last,
                 double span) {
  return first.m_speedOut.x >= 0 && first.m_speedOut.x <= span &&
         last.m_speedIn.x <= 0 && last.m_speedIn.x >= -span;
}

void validateInterval(const TDoubleKeyframe& first,
                      const TDoubleKeyframe& last) {
  const double span = last.m_frame - first.m_frame;
  if (span <= 0)
    invalid("Keys must be strictly ordered.", first.m_frame, "frame");
  if (controlKind(first.m_type) == IncomingKind::None) return;
  if (!controlsFit(first, last, span))
    invalid("A control extends beyond its native segment.", first.m_frame,
            "controls");
  if (first.m_type == TDoubleKeyframe::EaseInOut &&
      first.m_speedOut.x - last.m_speedIn.x > span)
    invalid("Ease durations overlap within their segment.", first.m_frame,
            "controls");
}

void validateCurve(const TDoubleParam& curve) {
  if (curve.isCycleEnabled())
    throw Error("unsupported_interpolation",
                "Cyclic curve editing is not qualified.");
  if (curve.getKeyframeCount() > MaxAnimationKeys)
    throw Error("resource_limit", "A channel may contain at most 10000 keys.");
  for (int index = 0; index < curve.getKeyframeCount(); ++index) {
    const auto& key = curve.getKeyframe(index);
    validateNativeKey(key);
    validateNumbers(key);
    if (index > 0) validateInterval(curve.getKeyframe(index - 1), key);
  }
}

bool sameControl(const TPointD& left, const TPointD& right, IncomingKind kind) {
  if (kind == IncomingKind::None) return true;
  if (kind == IncomingKind::Ease) return left.x == right.x;
  return left == right;
}

bool sameControls(const TDoubleKeyframe& left, const TDoubleKeyframe& right) {
  const auto incoming = controlKind(right.m_prevType);
  const auto outgoing = controlKind(right.m_type);
  if (incoming == IncomingKind::None && outgoing == IncomingKind::None)
    return true;
  return left.m_linkedHandles == right.m_linkedHandles &&
         sameControl(left.m_speedIn, right.m_speedIn, incoming) &&
         sameControl(left.m_speedOut, right.m_speedOut, outgoing);
}

bool sameKey(const TDoubleKeyframe& left, const TDoubleKeyframe& right) {
  return left.m_frame == right.m_frame && left.m_value == right.m_value &&
         left.m_type == right.m_type && left.m_step == right.m_step &&
         controlKind(left.m_prevType) == controlKind(right.m_prevType) &&
         sameControls(left, right);
}

const TDoubleKeyframe& exactKey(const TDoubleParam& curve, double frame) {
  const int index = curve.getClosestKeyframe(frame);
  if (index < 0 || curve.keyframeIndexToFrame(index) != frame)
    throw Error("native_error", "A prepared curve lost an expected key.",
                {{"frame", frame}});
  // getKeyframeAt returns shared scratch storage, which cannot compare two
  // curves.
  return curve.getKeyframe(index);
}

void validateTransition(const TDoubleParam& original, const TDoubleParam& next,
                        const QSet<int>& replaced, bool deleting = false) {
  for (int index = 0; index < next.getKeyframeCount(); ++index) {
    const auto& key = next.getKeyframe(index);
    if (replaced.contains(static_cast<int>(key.m_frame))) continue;
    if (deleting && index == 0) continue;
    const auto& before = exactKey(original, key.m_frame);
    if (controlKind(before.m_prevType) != controlKind(key.m_prevType))
      refuse("incoming_kind_changed",
             "Supply this end key's incoming control when changing its "
             "segment kind.",
             key.m_frame, "incoming");
  }
}

std::vector<ProposedKey> prepareRecords(TDoubleParam& prospective,
                                        const QJsonArray& keys,
                                        QSet<int>& replaced) {
  std::vector<ProposedKey> parsed;
  for (const auto& value : keys) {
    auto proposed   = proposedKey(value);
    const int frame = static_cast<int>(proposed.native.m_frame);
    if (replaced.contains(frame))
      invalid("Key frames must be unique.", frame, "frame");
    replaced.insert(frame);
    prospective.setKeyframe(proposed.native);
    parsed.push_back(proposed);
  }
  return parsed;
}

QJsonObject speedJson(const TPointD& value) {
  return {{"frame_delta", value.x}, {"value_delta", value.y}};
}

QJsonObject incomingJson(const TDoubleKeyframe& key) {
  const auto kind = controlKind(key.m_prevType);
  if (kind == IncomingKind::Ease)
    return {{"kind", "ease"}, {"frames", -key.m_speedIn.x}};
  if (kind == IncomingKind::Speed) {
    auto result = speedJson(key.m_speedIn);
    result.insert("kind", "speed");
    return result;
  }
  return {{"kind", "none"}};
}

QString interpolationName(TDoubleKeyframe::Type type) {
  switch (type) {
  case TDoubleKeyframe::Constant:
    return "constant";
  case TDoubleKeyframe::Linear:
    return "linear";
  case TDoubleKeyframe::EaseInOut:
    return "ease_in_out";
  case TDoubleKeyframe::SpeedInOut:
    return "speed_in_out";
  default:
    return "unsupported";
  }
}

QJsonObject keyJson(const TDoubleKeyframe& key) {
  QJsonObject result{{"frame", key.m_frame},
                     {"value", key.m_value},
                     {"step", key.m_step},
                     {"linked_handles", key.m_linkedHandles}};
  const bool previous =
      key.m_prevType == TDoubleKeyframe::None || supportedType(key.m_prevType);
  if (!supportedType(key.m_type) || !previous) {
    result.insert("interpolation", "unsupported");
    result.insert("native_type", int(key.m_type));
    result.insert("speed_in", speedJson(key.m_speedIn));
    result.insert("speed_out", speedJson(key.m_speedOut));
    return result;
  }
  result.insert("interpolation", interpolationName(key.m_type));
  result.insert("incoming", incomingJson(key));
  if (key.m_type == TDoubleKeyframe::SpeedInOut)
    result.insert("outgoing", speedJson(key.m_speedOut));
  if (key.m_type == TDoubleKeyframe::EaseInOut)
    result.insert("outgoing", QJsonObject{{"frames", key.m_speedOut.x}});
  return result;
}

void validateWholeCurve(const TDoubleParam& curve, bool writable) {
  if (writable && curve.isEditorLocked())
    throw Error("locked", "The destination animation channel is locked.");
  validateCurve(curve);
  for (int index = 0; index < curve.getKeyframeCount(); ++index) {
    const double offset = curve.getKeyframe(index).m_similarShapeOffset;
    if (!std::isfinite(offset))
      throw Error("invalid_params",
                  "Stored inactive key metadata must remain finite.");
  }
  const double value = curve.getDefaultValue();
  if (!std::isfinite(value) || std::abs(value) > MaxAnimationValue)
    throw Error("invalid_params",
                "The curve default value exceeds finite profile bounds.");
}

bool sameStoredControls(const TDoubleKeyframe& left,
                        const TDoubleKeyframe& right) {
  return left.m_speedIn == right.m_speedIn &&
         left.m_speedOut == right.m_speedOut &&
         left.m_linkedHandles == right.m_linkedHandles &&
         left.m_fileParams.m_fieldIndex == right.m_fileParams.m_fieldIndex &&
         left.m_unitName == right.m_unitName &&
         left.m_similarShapeOffset == right.m_similarShapeOffset;
}

bool sameStoredKey(const TDoubleKeyframe& left, const TDoubleKeyframe& right) {
  return left.m_frame == right.m_frame && left.m_value == right.m_value &&
         left.m_type == right.m_type && left.m_prevType == right.m_prevType &&
         left.m_isKeyframe == right.m_isKeyframe &&
         left.m_step == right.m_step && sameStoredControls(left, right);
}

bool matchesCurveEdit(const TDoubleParam& curve,
                      const AnimationCurveEdit& edit) {
  if (curve.getDefaultValue() != edit.defaultValue ||
      curve.getKeyframeCount() != static_cast<int>(edit.keys.size()))
    return false;
  for (int index = 0; index < curve.getKeyframeCount(); ++index)
    if (!sameStoredKey(curve.getKeyframe(index), edit.keys[index]))
      return false;
  return true;
}

void validateCurveEdit(const TDoubleParam& destination,
                       AnimationCurveEdit& edit) {
  TDoubleParamP prospective(new TDoubleParam(destination));
  applyAnimationCurveEdit(*prospective, edit);
  validateWholeCurve(*prospective, false);
  if (!matchesCurveEdit(*prospective, edit))
    throw Error("invalid_params",
                "Native curve replacement would alter stored key metadata.");
  edit.changed = !matchesCurveEdit(destination, edit);
}

qint64 mappingInteger(const QJsonObject& object, const QString& field,
                      int minimum, int maximum) {
  const auto value    = object.value(field);
  const double number = value.toDouble();
  if (!value.isDouble() || !std::isfinite(number) ||
      number != std::floor(number) || number < minimum || number > maximum)
    throw Error("invalid_params", "Time mapping requires a bounded integer.",
                {{"field", field}});
  return static_cast<qint64>(number);
}

struct TimeMapping final {
  qint64 origin;
  qint64 shift;
  qint64 numerator;
  qint64 denominator;
};

TimeMapping readTimeMapping(const QJsonObject& mapping) {
  const auto scale = mapping.value("scale").toObject();
  return {mappingInteger(mapping, "origin_frame", 0, MaxAnimationFrame),
          mappingInteger(mapping, "shift_frames", -MaxAnimationFrame,
                         MaxAnimationFrame),
          mappingInteger(scale, "numerator", 1, MaxAnimationFrame),
          mappingInteger(scale, "denominator", 1, MaxAnimationFrame)};
}

qint64 scaleExactly(qint64 value, const TimeMapping& mapping) {
  const qint64 product = value * mapping.numerator;
  if (product % mapping.denominator != 0)
    throw Error("invalid_params",
                "Time mapping produces a nonintegral key time or step.");
  return product / mapping.denominator;
}

TDoubleKeyframe retimedKey(TDoubleKeyframe key, const TimeMapping& mapping) {
  const qint64 frame =
      mapping.origin + mapping.shift +
      scaleExactly(static_cast<qint64>(key.m_frame) - mapping.origin, mapping);
  if (frame < 0 || frame > MaxAnimationFrame)
    throw Error("invalid_params",
                "Retimed key lies outside the supported frame range.");
  key.m_frame = static_cast<double>(frame);
  if (key.m_step > 1) {
    const qint64 step = scaleExactly(key.m_step, mapping);
    if (step < 2 || step > 1000)
      throw Error("invalid_params",
                  "Retimed quantized step must remain between 2 and 1000.");
    key.m_step = static_cast<int>(step);
  }
  const double ratio =
      static_cast<double>(mapping.numerator) / mapping.denominator;
  key.m_speedIn.x *= ratio;
  key.m_speedOut.x *= ratio;
  return key;
}

struct BreakdownRequest final {
  int frame;
  bool favoured;
  double favourPercent;
  double value;
};

BreakdownRequest readBreakdown(const QJsonObject& request) {
  const double frame = number(request, "frame", 0, MaxAnimationFrame, -1);
  if (frame != std::floor(frame))
    invalid("Breakdown frames must be integers.", frame, "frame");
  const bool favoured = request.contains("favour_percent");
  if (favoured == request.contains("value"))
    refuse("favour_percent_or_value",
           "Give exactly one of favour_percent and value.", frame,
           "favour_percent");
  BreakdownRequest result{static_cast<int>(frame), favoured, 0, 0};
  if (favoured)
    result.favourPercent = number(request, "favour_percent", -100, 200, frame);
  else
    result.value =
        number(request, "value", -MaxAnimationValue, MaxAnimationValue, frame);
  return result;
}

struct SplitSegment final {
  TDoubleKeyframe first;
  TDoubleKeyframe key;
  TDoubleKeyframe last;
};

// The editor's KeyframeSetter::createKeyframe for a key inside a segment. A
// speed segment is cut so the shape is kept; an ease segment gives the new key
// no ease and clamps its neighbours' eases to the new span (the editor clamps
// the wrong key's ease on the right side).
SplitSegment splitSegment(const TDoubleKeyframe& previous,
                          const TDoubleKeyframe& next, int frame) {
  SplitSegment split{previous, TDoubleKeyframe(frame, 0), next};
  auto& key        = split.key;
  key.m_isKeyframe = true;
  key.m_type       = previous.m_type;
  key.m_step       = previous.m_step;
  if (previous.m_type == TDoubleKeyframe::SpeedInOut) {
    splitSpeedInOutSegment(key, split.first, split.last);
  } else if (previous.m_type == TDoubleKeyframe::EaseInOut) {
    split.first.m_speedOut.x =
        std::min(split.first.m_speedOut.x, frame - previous.m_frame);
    split.last.m_speedIn.x =
        std::max(split.last.m_speedIn.x, frame - next.m_frame);
  }
  key.m_linkedHandles = controlKind(previous.m_type) == IncomingKind::None;
  return split;
}

// Validates a curve built by an edit, naming a handle that no longer fits its
// segment as a reason.
void validateProspective(const TDoubleParam& curve) {
  try {
    validateCurve(curve);
  } catch (Error& error) {
    if (error.code == "invalid_argument" &&
        error.details.value("field") == "controls")
      error.details.insert("reason", "controls");
    throw;
  }
}

void requireWritable(const TDoubleParam& curve) {
  if (curve.isEditorLocked())
    throw Error("locked", "The animation channel is locked.");
}

std::vector<int> selectedIndices(const TDoubleParam& curve,
                                 const std::vector<int>& frames) {
  if (frames.empty() || frames.size() > MaxAnimationKeys)
    throw Error("resource_limit", "frames must name 1 through 10000 keys.");
  std::vector<int> result;
  QSet<int> seen;
  for (const int frame : frames) {
    if (!curve.isKeyframe(frame))
      throw Error("missing_key", "Every requested key must exist.",
                  {{"frame", frame}});
    if (seen.contains(frame))
      refuse("duplicate_frame", "frames must not repeat a key.", frame,
             "frames");
    seen.insert(frame);
    result.push_back(curve.getClosestKeyframe(frame));
  }
  std::sort(result.begin(), result.end());
  return result;
}

void requireOffsets(int frameOffset, double valueOffset) {
  if (std::abs(frameOffset) > MaxAnimationFrame)
    refuse("frame_out_of_range", "frame_offset leaves the frame range.", 0,
           "frame_offset");
  if (!std::isfinite(valueOffset) ||
      std::abs(valueOffset) > 2 * MaxAnimationValue)
    refuse("value_out_of_bounds", "value_offset leaves the bounds.", 0,
           "value_offset");
}

// The record shifted by the offsets, within the profile's frame and value
// bounds.
TDoubleKeyframe shifted(TDoubleKeyframe record, int frameOffset,
                        double valueOffset) {
  const double frame = record.m_frame + frameOffset;
  if (frame < 0 || frame > MaxAnimationFrame)
    refuse("frame_out_of_range", "A shifted key leaves the frame range.",
           record.m_frame, "frame_offset");
  record.m_frame = frame;
  record.m_value += valueOffset;
  if (!std::isfinite(record.m_value) ||
      std::abs(record.m_value) > MaxAnimationValue)
    refuse("value_out_of_bounds", "A shifted key leaves the value bounds.",
           frame, "value_offset");
  return record;
}

}  // namespace

AnimationKeyEdit prepareAnimationKeyEdit(const TDoubleParam& curve,
                                         const QJsonArray& keys) {
  if (curve.isEditorLocked())
    throw Error("locked", "The animation channel is locked.");
  validateCurve(curve);
  if (keys.isEmpty() || keys.size() > MaxAnimationKeys)
    throw Error("resource_limit", "keys must contain 1 through 10000 records.");
  TDoubleParamP prospective(new TDoubleParam(curve));
  QSet<int> replaced;
  const auto parsed = prepareRecords(*prospective, keys, replaced);
  validateTransition(curve, *prospective, replaced);
  AnimationKeyEdit result;
  for (const auto& proposed : parsed) {
    const auto& key = exactKey(*prospective, proposed.native.m_frame);
    if (controlKind(key.m_prevType) != proposed.incoming)
      invalid("Incoming kind must match the prospective preceding segment.",
              key.m_frame, "incoming");
    const bool changed = !curve.isKeyframe(key.m_frame) ||
                         !sameKey(exactKey(curve, key.m_frame), key);
    if (changed) result.keys.push_back(key);
  }
  validateCurve(*prospective);
  result.changed = !result.keys.empty();
  std::sort(result.keys.begin(), result.keys.end());
  return result;
}

AnimationKeyEdit prepareAnimationBreakdown(const TDoubleParam& curve,
                                           const QJsonObject& request) {
  if (curve.isEditorLocked())
    throw Error("locked", "The animation channel is locked.");
  validateCurve(curve);
  const auto read = readBreakdown(request);
  if (curve.isKeyframe(read.frame))
    refuse("key_exists", "A key already exists at this frame.", read.frame,
           "frame");
  const int previousIndex = curve.getPrevKeyframe(read.frame);
  const int nextIndex     = curve.getNextKeyframe(read.frame);
  if (previousIndex < 0 || nextIndex < 0)
    refuse("outside_segment", "The frame must lie between two existing keys.",
           read.frame, "frame");
  if (curve.getKeyframeCount() >= MaxAnimationKeys)
    throw Error("resource_limit", "A channel may contain at most 10000 keys.");
  const auto& previous = curve.getKeyframe(previousIndex);
  const auto& next     = curve.getKeyframe(nextIndex);
  auto split           = splitSegment(previous, next, read.frame);
  split.key.m_value =
      read.favoured ? previous.m_value + (next.m_value - previous.m_value) *
                                             (read.favourPercent / 100.0)
                    : read.value;
  if (!std::isfinite(split.key.m_value) ||
      std::abs(split.key.m_value) > MaxAnimationValue)
    refuse("value_out_of_bounds", "The breakdown value leaves the bounds.",
           read.frame, "value");
  AnimationKeyEdit result;
  if (split.first.m_speedOut != previous.m_speedOut)
    result.keys.push_back(split.first);
  result.keys.push_back(split.key);
  if (split.last.m_speedIn != next.m_speedIn) result.keys.push_back(split.last);
  std::sort(result.keys.begin(), result.keys.end());
  TDoubleParamP prospective(new TDoubleParam(curve));
  applyAnimationKeyEdit(*prospective, result);
  validateProspective(*prospective);
  validateTransition(curve, *prospective, {read.frame});
  result.changed = true;
  return result;
}

AnimationKeyEdit prepareAnimationKeyMove(const TDoubleParam& curve,
                                         const std::vector<int>& frames,
                                         int frameOffset, double valueOffset) {
  requireWritable(curve);
  validateCurve(curve);
  const auto indices = selectedIndices(curve, frames);
  requireOffsets(frameOffset, valueOffset);
  AnimationKeyEdit result;
  if (frameOffset == 0 && valueOffset == 0) return result;
  QSet<int> oldFrames, newFrames;
  std::vector<double> order;
  for (int index = 0; index < curve.getKeyframeCount(); ++index) {
    auto record = curve.getKeyframe(index);
    if (std::binary_search(indices.begin(), indices.end(), index)) {
      oldFrames.insert(static_cast<int>(record.m_frame));
      record = shifted(record, frameOffset, valueOffset);
      newFrames.insert(static_cast<int>(record.m_frame));
      result.keys.push_back(record);
    }
    if (!order.empty() && record.m_frame <= order.back())
      refuse(record.m_frame == order.back() ? "key_exists" : "order_changed",
             "A moved key may not land on or cross another key.",
             record.m_frame, "frame_offset");
    order.push_back(record.m_frame);
  }
  for (const int frame : oldFrames)
    if (!newFrames.contains(frame)) result.deletedFrames.push_back(frame);
  std::sort(result.deletedFrames.begin(), result.deletedFrames.end());
  std::sort(result.keys.begin(), result.keys.end());
  TDoubleParamP prospective(new TDoubleParam(curve));
  applyAnimationKeyEdit(*prospective, result);
  validateProspective(*prospective);
  validateTransition(curve, *prospective, newFrames);
  result.changed = true;
  return result;
}

AnimationKeyEdit prepareAnimationKeyDuplicate(const TDoubleParam& curve,
                                              const std::vector<int>& frames,
                                              int frameOffset,
                                              double valueOffset) {
  requireWritable(curve);
  validateCurve(curve);
  const auto indices = selectedIndices(curve, frames);
  requireOffsets(frameOffset, valueOffset);
  AnimationKeyEdit result;
  QSet<int> created;
  std::map<int, IncomingKind> sourceKinds;
  for (const int index : indices) {
    const auto& source = curve.getKeyframe(index);
    auto record        = shifted(source, frameOffset, valueOffset);
    if (curve.isKeyframe(record.m_frame))
      refuse("key_exists", "A copy may not land on an existing key.",
             record.m_frame, "frame_offset");
    created.insert(static_cast<int>(record.m_frame));
    sourceKinds[static_cast<int>(record.m_frame)] =
        controlKind(source.m_prevType);
    result.keys.push_back(record);
  }
  TDoubleParamP prospective(new TDoubleParam(curve));
  applyAnimationKeyEdit(*prospective, result);
  validateProspective(*prospective);
  validateTransition(curve, *prospective, created);
  for (const auto& [frame, kind] : sourceKinds)
    if (controlKind(exactKey(*prospective, frame).m_prevType) != kind)
      refuse("incoming_kind_changed",
             "A copy would change its incoming control kind; use "
             "animation.set_keys to state it.",
             frame, "incoming");
  result.changed = true;
  return result;
}

void applyAnimationKeyEdit(TDoubleParam& curve, const AnimationKeyEdit& edit) {
  for (const int frame : edit.deletedFrames) curve.deleteKeyframe(frame);
  for (const auto& key : edit.keys) curve.setKeyframe(key);
}

void validateAnimationKeyDeletion(const TDoubleParam& curve,
                                  const std::vector<int>& frames) {
  if (curve.isEditorLocked())
    throw Error("locked", "The animation channel is locked.");
  validateCurve(curve);
  TDoubleParamP prospective(new TDoubleParam(curve));
  for (const int frame : frames) {
    if (!curve.isKeyframe(frame))
      throw Error("missing_key", "Every requested key must exist.",
                  {{"frame", frame}});
    prospective->deleteKeyframe(frame);
  }
  validateTransition(curve, *prospective, {}, true);
  validateCurve(*prospective);
}

AnimationCurveEdit prepareAnimationCurveCopy(const TDoubleParam& source,
                                             const TDoubleParam& destination) {
  validateWholeCurve(source, false);
  validateWholeCurve(destination, true);
  AnimationCurveEdit edit;
  edit.defaultValue = source.getDefaultValue();
  for (int index = 0; index < source.getKeyframeCount(); ++index)
    edit.keys.push_back(source.getKeyframe(index));
  validateCurveEdit(destination, edit);
  return edit;
}

AnimationCurveEdit prepareAnimationCurveRetime(const TDoubleParam& curve,
                                               const QJsonObject& mapping) {
  validateWholeCurve(curve, true);
  const auto time = readTimeMapping(mapping);
  AnimationCurveEdit edit;
  edit.defaultValue = curve.getDefaultValue();
  for (int index = 0; index < curve.getKeyframeCount(); ++index)
    edit.keys.push_back(retimedKey(curve.getKeyframe(index), time));
  validateCurveEdit(curve, edit);
  return edit;
}

void applyAnimationCurveEdit(TDoubleParam& destination,
                             const AnimationCurveEdit& edit) {
  destination.clearKeyframes();
  destination.setDefaultValue(edit.defaultValue);
  for (const auto& key : edit.keys) destination.setKeyframe(key);
}

QString animationUnsupportedReason(const TDoubleParam& curve) {
  try {
    validateCurve(curve);
  } catch (const Error& error) {
    return error.message;
  }
  return {};
}

QJsonArray inspectAnimationKeys(const TDoubleParam& curve) {
  if (curve.getKeyframeCount() > MaxAnimationKeys)
    throw Error("resource_limit", "Inspection exceeds 10000 native keys.");
  QJsonArray result;
  for (int index = 0; index < curve.getKeyframeCount(); ++index) {
    const auto& key = curve.getKeyframe(index);
    validateNumbers(key);
    result.append(keyJson(key));
  }
  return result;
}

void requireSupportedAnimationCurve(const TDoubleParam& curve) {
  validateCurve(curve);
}

}  // namespace GroundzeroAutomation
