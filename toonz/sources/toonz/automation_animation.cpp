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
      invalid(
          "Supply this end key's incoming control when changing its segment "
          "kind.",
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
