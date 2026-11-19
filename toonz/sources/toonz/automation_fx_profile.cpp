#include "automation.h"
#include "automation_animation.h"
#include "automation_fx_profile.h"

#include "tdoublekeyframe.h"
#include "tdoubleparam.h"
#include "tfx.h"
#include "tnotanimatableparam.h"
#include "tparamcontainer.h"
#include "tparamset.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <set>

namespace GroundzeroAutomation {
namespace {

constexpr int kMaximumKeys          = 4096;
constexpr double kMinimumSeparation = 0.001;
const char* const kSeparation       = "linear_gradient_separation";

// Admitted bounds that differ from the native range. A nonzero coefficient
// makes the parameter an extent: coefficient * |value| * render scale pixels
// pad the effect's input and must stay within extentPixels.
struct ParameterBound final {
  const char* name;
  double minimum;
  double maximum;
  double extentCoefficient = 0;
  double extentPixels      = 0;
};

struct TypeProfile final {
  const char* type;
  int version;
  std::vector<ParameterBound> bounds;
  // Ports without which the effect renders nothing or passes its source.
  std::vector<const char*> requiredInputs;
  std::vector<const char*> constraints;
  bool positiveDeterminant = false;
};

const std::vector<TypeProfile>& profiles() {
  static const std::vector<TypeProfile> table = {
      {"colorCardFx", 1, {}, {}, {}},
      {"overFx", 1, {}, {}, {}},
      {"STD_iwa_LinearGradientFx",
       1,
       {{"startPoint", -1e5, 1e5},
        {"endPoint", -1e5, 1e5},
        {"wave_amplitude", 0, 1e4},
        {"wave_frequency", -1e4, 1e4},
        {"wave_phase", -1e4, 1e4}},
       {},
       {kSeparation}},
      {"STD_radialGradientFx",
       1,
       {{"period", 0.01, 1e5}, {"innerperiod", 0, 1e5}},
       {},
       {}},
      {"STD_blurFx", 1, {{"value", 0, 200, 1, 1024}}, {}, {}},
      {"STD_glowFx",
       1,
       {{"value", 0, 200, 1, 1024}, {"brightness", 0, 1000}},
       {"Light"},
       {}},
      {"STD_brightContFx", 1, {}, {}, {}},
      {"STD_hsvScaleFx",
       1,
       {{"hue_scale", 0, 400},
        {"saturation_scale", 0, 400},
        {"value_scale", 0, 400}},
       {},
       {}},
      {"multFx", 1, {{"value", -255, 255}}, {}, {}},
      {"screenFx", 1, {}, {}, {}},
      {"inFx", 1, {}, {}, {}},
      {"outFx", 1, {}, {}, {}},
      {"invertFx", 1, {}, {}, {}},
      {"blendFx", 1, {}, {}, {}},
      {"STD_iwa_BloomFx", 3, {{"size", 0.1, 200, 1, 1024}}, {}, {}},
      {"STD_lightWrapFx",
       1,
       {{"width", 0, 50, 1, 1024}, {"softness", 0, 20, 3, 1024}},
       {"Background"},
       {}},
      {"STD_backlitFx", 1, {{"value", 0, 100, 1, 1024}}, {"Light"}, {}},
      {"STD_bodyHighLightFx",
       1,
       {{"point", -100, 100, 1, 1024}, {"blur", 0, 100, 1, 1024}},
       {},
       {}},
      {"STD_lightSpotFx",
       1,
       {{"softness", 0, 10}, {"a", 1, 5000}, {"b", 1, 5000}},
       {},
       {}},
      {"STD_targetSpotFx",
       1,
       {{"z", 0, 1e4},
        {"angle", -89, 89},
        {"sizeX", 0.01, 100},
        {"sizeY", 0.01, 100}},
       {},
       {}},
      {"STD_directionalBlurFx",
       1,
       {{"intensity", 0, 200, 1, 1024}, {"angle", -360, 360}},
       {},
       {}},
      {"STD_localBlurFx", 1, {{"value", 0, 200, 1, 1024}}, {"Reference"}, {}},
      {"STD_posterizeFx", 1, {}, {}, {}},
      {"STD_solarizeFx",
       1,
       {{"maximum", 0, 1.99}, {"peak_edge", 1, 255}},
       {},
       {}},
      {"STD_sharpenFx", 1, {{"intensity", 0, 1000}}, {}, {}},
      {"STD_embossFx", 1, {{"radius", 0.5, 5, 1, 64}}, {}, {}},
      {"STD_erodeDilateFx", 1, {{"radius", -50, 50, 1, 1024}}, {}, {}, true},
      {"STD_matteChokeFx",
       1,
       {{"amount", -20, 20, 1, 1024}, {"feather", 0, 20, 1, 1024}},
       {},
       {}},
      {"STD_channelMixerFx", 1, {}, {}, {}},
      {"STD_gammaFx", 1, {{"value", 0.1, 10}}, {}, {}},
      {"STD_iwa_AdjustExposureFx", 3, {{"scale", -3, 3}}, {}, {}}};
  return table;
}

const TypeProfile* findProfile(const std::string& type) {
  for (const TypeProfile& profile : profiles())
    if (type == profile.type) return &profile;
  return nullptr;
}

const TypeProfile& requireProfile(TFx* fx) {
  const TypeProfile* profile = findProfile(fx->getFxType());
  if (!profile)
    throw Error("UNSUPPORTED_FX",
                "Effect is outside the builtin automation profile.",
                {{"type", QString::fromStdString(fx->getFxType())}});
  return *profile;
}

const ParameterBound* findBound(const TypeProfile& profile,
                                const std::string& name) {
  for (const ParameterBound& bound : profile.bounds)
    if (name == bound.name) return &bound;
  return nullptr;
}

// Parameters a cross-parameter constraint reads. The separation check is
// exact on linear segments only, so they keep linear keys with step 1.
bool constrainedParameter(const TypeProfile& profile, const std::string& name) {
  for (const char* constraint : profile.constraints)
    if (std::string(constraint) == kSeparation &&
        (name == "startPoint" || name == "endPoint"))
      return true;
  return false;
}

QJsonObject rangeSchema(double minimum, double maximum, bool integer) {
  return {{"type", integer ? "integer" : "number"},
          {"minimum", minimum},
          {"maximum", maximum}};
}

QJsonObject doubleSchema(const TypeProfile& profile, const std::string& name,
                         TDoubleParam* param) {
  const bool integer = !param->getUIChoices().empty();
  QJsonObject schema;
  double minimum, maximum, step;
  if (const ParameterBound* bound = findBound(profile, name))
    schema = rangeSchema(bound->minimum, bound->maximum, integer);
  else if (param->getValueRange(minimum, maximum, step))
    schema = rangeSchema(minimum, maximum, integer);
  else
    return {};
  schema["measure"] = QString::fromStdString(param->getMeasureName());
  if (integer) {
    QJsonArray choices;
    for (const auto& choice : param->getUIChoices())
      choices.append(QString::fromStdString(choice));
    schema["choices"] = choices;
  }
  return schema;
}

QJsonObject fixedArraySchema(int size, const QJsonObject& items) {
  return {{"type", "array"},
          {"minItems", size},
          {"maxItems", size},
          {"items", items}};
}

QJsonObject pixelSchema(TPixelParam* param) {
  const QJsonObject channel{
      {"type", "integer"}, {"minimum", 0}, {"maximum", 255}};
  QJsonObject schema = fixedArraySchema(4, channel);
  if (!param->isMatteEnabled()) {
    QJsonObject matte     = channel;
    matte["const"]        = 255;
    schema["prefixItems"] = QJsonArray{channel, channel, channel, matte};
  }
  return schema;
}

QJsonObject pointSchema(const TypeProfile& profile, const std::string& name,
                        TPointParam* param) {
  const ParameterBound* bound = findBound(profile, name);
  if (!bound) return {};
  QJsonObject schema =
      fixedArraySchema(2, rangeSchema(bound->minimum, bound->maximum, false));
  schema["measure"] = QString::fromStdString(param->getX()->getMeasureName());
  return schema;
}

QJsonObject discreteSchema(TParam* param) {
  if (auto* integer = dynamic_cast<TIntParam*>(param)) {
    int minimum, maximum;
    integer->getValueRange(minimum, maximum);
    return {{"type", "integer"}, {"minimum", minimum}, {"maximum", maximum}};
  }
  if (dynamic_cast<TBoolParam*>(param)) return {{"type", "boolean"}};
  if (auto* enumeration = dynamic_cast<TEnumParam*>(param)) {
    QJsonArray values, captions;
    for (int index = 0; index < enumeration->getItemCount(); ++index) {
      int value;
      std::string caption;
      enumeration->getItem(index, value, caption);
      values.append(value);
      captions.append(QString::fromStdString(caption));
    }
    return {{"type", "integer"}, {"enum", values}, {"captions", captions}};
  }
  return {};
}

TDoubleParam* channel(TParamSet* set, int index) {
  return static_cast<TDoubleParam*>(set->getParam(index).getPointer());
}

// Where a curve sits, for refusals: the parameter, the channel name
// animation.channels publishes, and whether the parameter keeps linear keys.
struct CurveScope final {
  QString parameter;
  QString channel;
  bool linearOnly = false;
};

CurveScope componentScope(const CurveScope& scope, TParamSet* set, int index) {
  return {
      scope.parameter,
      scope.parameter + "." + QString::fromStdString(set->getParamName(index)),
      scope.linearOnly};
}

// Validates value against schema; a refusal names the channel and, for a key
// or a speed handle, its frame. Handles are points of the curve's convex hull,
// not stored values, so only their range is checked.
void requireCurveValue(double value, QJsonObject schema,
                       const CurveScope& scope, const QJsonValue& frame,
                       bool handle = false) {
  if (handle) schema["type"] = "number";
  try {
    validateAutomationFxNumber(value, schema);
  } catch (Error& error) {
    error.details.insert("parameter", scope.parameter);
    error.details.insert("channel", scope.channel);
    if (!frame.isNull()) error.details.insert("frame", frame);
    if (handle) error.details.insert("field", "speed_handle");
    throw;
  }
}

void requireLinearKeys(TDoubleParam* curve, const CurveScope& scope) {
  for (int index = 0; index < curve->getKeyframeCount(); ++index) {
    const TDoubleKeyframe& key = curve->getKeyframe(index);
    const auto previous =
        index == 0 ? TDoubleKeyframe::None : TDoubleKeyframe::Linear;
    if (key.m_type != TDoubleKeyframe::Linear || key.m_prevType != previous ||
        key.m_step != 1)
      throw Error("UNSUPPORTED_PARAMETER",
                  "This parameter's constraint is checked on linear segments, "
                  "so it accepts only linear keys with step 1.",
                  {{"parameter", scope.parameter},
                   {"channel", scope.channel},
                   {"frame", key.m_frame}});
  }
}

// An admitted curve holds keys the animation tools can write (constant,
// linear, ease_in_out, speed_in_out; no cycle) whose values, and the handle
// points of every speed segment, lie in the admitted bounds: ease and constant
// segments stay between their key values and a speed segment inside the hull
// of its four points, so the whole curve stays in bounds at every frame.
void validateCurve(TDoubleParam* curve, const QJsonObject& schema,
                   const CurveScope& scope) {
  if (curve->getKeyframeCount() > kMaximumKeys)
    throw Error("RESOURCE_LIMIT", "Effect parameter exceeds 4096 scalar keys.");
  requireCurveValue(curve->getDefaultValue(), schema, scope, QJsonValue());
  if (scope.linearOnly) requireLinearKeys(curve, scope);
  const QString reason = animationUnsupportedReason(*curve);
  if (!reason.isEmpty())
    throw Error("UNSUPPORTED_PARAMETER",
                "Effect keys must be ones the animation tools write: " + reason,
                {{"parameter", scope.parameter}, {"channel", scope.channel}});
  const int count = curve->getKeyframeCount();
  for (int index = 0; index < count; ++index) {
    const TDoubleKeyframe& key = curve->getKeyframe(index);
    requireCurveValue(key.m_value, schema, scope, key.m_frame);
    if (key.m_type != TDoubleKeyframe::SpeedInOut || index + 1 == count)
      continue;
    const TDoubleKeyframe& next = curve->getKeyframe(index + 1);
    requireCurveValue(key.m_value + key.m_speedOut.y, schema, scope,
                      key.m_frame, true);
    requireCurveValue(next.m_value + next.m_speedIn.y, schema, scope,
                      next.m_frame, true);
  }
}

// Reads the channels the native color evaluates: disabling the matte replaces
// the matte curve with one that the parameter set's children do not list.
void validatePixel(TPixelParam* pixel, const CurveScope& scope) {
  const QJsonObject unit{{"type", "number"}, {"minimum", 0}, {"maximum", 1}};
  for (int index = 0; index < 3; ++index)
    validateCurve(channel(pixel, index), unit,
                  componentScope(scope, pixel, index));
  const CurveScope matte = componentScope(scope, pixel, 3);
  if (pixel->isMatteEnabled())
    return validateCurve(pixel->getMatte().getPointer(), unit, matte);
  // A disabled matte stores 255.0 natively and is read clamped to 255.
  try {
    validateCurve(pixel->getMatte().getPointer(),
                  {{"type", "number"}, {"minimum", 1}, {"maximum", 255}},
                  matte);
  } catch (const Error& error) {
    if (error.code != "OUT_OF_RANGE") throw;
    throw Error("OUT_OF_RANGE",
                "This color has no native matte; its alpha must be 255.",
                error.details);
  }
}

void validateDiscrete(TParam* param, const QJsonObject& schema) {
  if (auto* integer = dynamic_cast<TIntParam*>(param))
    validateAutomationFxNumber(integer->getValue(), schema);
  auto* enumeration = dynamic_cast<TEnumParam*>(param);
  if (enumeration &&
      !schema.value("enum").toArray().contains(enumeration->getValue()))
    throw Error("OUT_OF_RANGE",
                "Parameter value is not a native enumeration choice.");
}

void validateStored(TParam* param, const QJsonObject& schema,
                    const CurveScope& scope) {
  if (auto* number = dynamic_cast<TDoubleParam*>(param))
    return validateCurve(number, schema, scope);
  if (auto* point = dynamic_cast<TPointParam*>(param)) {
    for (int axis = 0; axis < 2; ++axis)
      validateCurve(channel(point, axis), schema.value("items").toObject(),
                    componentScope(scope, point, axis));
    return;
  }
  if (auto* pixel = dynamic_cast<TPixelParam*>(param))
    return validatePixel(pixel, scope);
  validateDiscrete(param, schema);
}

double length(const TPointD& vector) { return std::hypot(vector.x, vector.y); }

TPointD separation(TPointParam* start, TPointParam* end, double frame) {
  return end->getValue(frame) - start->getValue(frame);
}

void separationViolated(double frame) {
  throw Error("PARAMETER_CONSTRAINT",
              "Linear gradient points must be equal or at least 0.001 "
              "apart at every frame.",
              {{"constraint", kSeparation},
               {"parameters", QJsonArray{"startPoint", "endPoint"}},
               {"frame", frame}});
}

// Both points are linear between the union of their key frames, so their
// difference is too: check its exact minimum length on each segment.
void requireSegmentSeparation(double from, const TPointD& first, double to,
                              const TPointD& last) {
  if (length(first) == 0 && length(last) == 0) return;
  const TPointD step   = last - first;
  const double squared = step.x * step.x + step.y * step.y;
  const double ratio =
      squared > 0 ? std::clamp(-(first.x * step.x + first.y * step.y) / squared,
                               0.0, 1.0)
                  : 0.0;
  if (length(first + step * ratio) < kMinimumSeparation)
    separationViolated(from + (to - from) * ratio);
}

void requireGradientSeparation(TFx* fx) {
  auto* start =
      dynamic_cast<TPointParam*>(fx->getParams()->getParam("startPoint"));
  auto* end = dynamic_cast<TPointParam*>(fx->getParams()->getParam("endPoint"));
  if (!start || !end)
    throw Error("NATIVE_FAILURE", "Linear gradient points are missing.");
  std::set<double> frames;
  for (TPointParam* point : {start, end})
    for (int axis = 0; axis < 2; ++axis) {
      std::set<double> keys;
      channel(point, axis)->getKeyframes(keys);
      frames.insert(keys.begin(), keys.end());
    }
  if (frames.empty()) frames.insert(0);
  double from      = *frames.begin();
  TPointD previous = separation(start, end, from);
  for (double frame : frames) {
    const TPointD current = separation(start, end, frame);
    requireSegmentSeparation(from, previous, frame, current);
    from     = frame;
    previous = current;
  }
}

QJsonObject constraintDescription(const char* id) {
  return {{"id", id},
          {"rule",
           "startPoint and endPoint are equal or at least 0.001 apart at "
           "every frame."},
          {"parameters", QJsonArray{"startPoint", "endPoint"}}};
}

QJsonObject missingInput(const std::string& type, const char* port) {
  if (type == "STD_glowFx")
    return {{"code", "GLOW_WITHOUT_LIGHT"},
            {"message",
             "Glow has no Light input; Source passes through unchanged."}};
  return {{"code", "REQUIRED_INPUT_MISSING"},
          {"port", port},
          {"message",
           QString("Connect the %1 input; without it this effect renders "
                   "nothing or passes its source through unchanged.")
               .arg(port)}};
}

}  // namespace

QString automationFxProfileName() { return "builtin-2d-v2"; }

const QStringList& automationFxProfileTypes() {
  static const QStringList types = [] {
    QStringList result;
    for (const TypeProfile& profile : profiles()) result.append(profile.type);
    return result;
  }();
  return types;
}

QJsonObject automationFxParameterSchema(TFx* fx, TParam* param,
                                        const std::string& name) {
  const TypeProfile& profile = requireProfile(fx);
  if (auto* number = dynamic_cast<TDoubleParam*>(param))
    return doubleSchema(profile, name, number);
  if (auto* pixel = dynamic_cast<TPixelParam*>(param))
    return pixelSchema(pixel);
  if (auto* point = dynamic_cast<TPointParam*>(param))
    return pointSchema(profile, name, point);
  return discreteSchema(param);
}

QJsonObject automationFxChannelSchema(TFx* fx, TParam* param,
                                      const std::string& name, int component) {
  const QJsonObject schema = automationFxParameterSchema(fx, param, name);
  if (schema.isEmpty() || component < 0) return schema;
  if (dynamic_cast<TPixelParam*>(param))
    return {{"type", "number"}, {"minimum", 0}, {"maximum", 1}};
  return schema.value("items").toObject();
}

bool automationFxLinearOnly(TFx* fx, const std::string& name) {
  return constrainedParameter(requireProfile(fx), name);
}

namespace {

// A native side at the type's extreme is unbounded (null).
QJsonValue nativeSide(double value) {
  return std::abs(value) >= 1e300 ? QJsonValue() : QJsonValue(value);
}

QJsonValue rangeObject(const QJsonValue& minimum, const QJsonValue& maximum) {
  return QJsonObject{{"minimum", minimum}, {"maximum", maximum}};
}

TDoubleParam* firstCurve(TParam* param) {
  if (auto* number = dynamic_cast<TDoubleParam*>(param)) return number;
  if (auto* point = dynamic_cast<TPointParam*>(param)) return channel(point, 0);
  if (auto* pixel = dynamic_cast<TPixelParam*>(param)) return channel(pixel, 0);
  return nullptr;
}

QString parameterKind(TParam* param) {
  if (auto* number = dynamic_cast<TDoubleParam*>(param))
    return number->getUIChoices().empty() ? "number" : "integer_choice";
  if (dynamic_cast<TIntParam*>(param)) return "integer";
  if (dynamic_cast<TEnumParam*>(param)) return "enum";
  if (dynamic_cast<TBoolParam*>(param)) return "boolean";
  if (dynamic_cast<TPixelParam*>(param)) return "color";
  if (dynamic_cast<TPointParam*>(param)) return "point";
  return "unsupported";
}

QJsonValue defaultValue(TParam* param) {
  if (auto* number = dynamic_cast<TDoubleParam*>(param))
    return number->getDefaultValue();
  if (auto* pixel = dynamic_cast<TPixelParam*>(param)) {
    const TPixel32 color = pixel->getDefaultValue();
    return QJsonArray{color.r, color.g, color.b, color.m};
  }
  if (auto* point = dynamic_cast<TPointParam*>(param)) {
    const TPointD value = point->getDefaultValue();
    return QJsonArray{value.x, value.y};
  }
  if (auto* integer = dynamic_cast<TIntParam*>(param))
    return integer->getDefaultValue();
  if (auto* enumeration = dynamic_cast<TEnumParam*>(param))
    return enumeration->getDefaultValue();
  if (auto* boolean = dynamic_cast<TBoolParam*>(param))
    return boolean->getDefaultValue();
  return QJsonValue();
}

QJsonValue nativeBounds(TParam* param) {
  if (auto* integer = dynamic_cast<TIntParam*>(param)) {
    int minimum = 0, maximum = 0;
    integer->getValueRange(minimum, maximum);
    return rangeObject(nativeSide(minimum <= -2147483647 ? -1e300 : minimum),
                       nativeSide(maximum >= 2147483647 ? 1e300 : maximum));
  }
  TDoubleParam* curve = firstCurve(param);
  double minimum = 0, maximum = 0, step = 0;
  if (!curve || dynamic_cast<TPixelParam*>(param) ||
      !curve->getValueRange(minimum, maximum, step))
    return QJsonValue();
  return rangeObject(nativeSide(minimum), nativeSide(maximum));
}

QJsonValue sliderBounds(TParam* param) {
  TDoubleParam* curve = firstCurve(param);
  double minimum = 0, maximum = 0;
  if (!curve || dynamic_cast<TPixelParam*>(param) ||
      !curve->getUISliderRange(minimum, maximum))
    return QJsonValue();
  return rangeObject(minimum, maximum);
}

QJsonValue admittedBounds(const QJsonObject& schema) {
  const QJsonObject numbers = schema.value("type") == "array"
                                  ? schema.value("items").toObject()
                                  : schema;
  if (!numbers.contains("minimum") || numbers.contains("enum"))
    return QJsonValue();
  return rangeObject(numbers.value("minimum"), numbers.value("maximum"));
}

QJsonArray choices(TParam* param) {
  QJsonArray result;
  if (auto* enumeration = dynamic_cast<TEnumParam*>(param))
    for (int index = 0; index < enumeration->getItemCount(); ++index) {
      int value;
      std::string caption;
      enumeration->getItem(index, value, caption);
      result.append(QJsonObject{{"id", value},
                                {"label", QString::fromStdString(caption)}});
    }
  if (auto* number = dynamic_cast<TDoubleParam*>(param)) {
    const auto& labels = number->getUIChoices();
    for (int index = 0; index < static_cast<int>(labels.size()); ++index)
      result.append(QJsonObject{
          {"id", index}, {"label", QString::fromStdString(labels[index])}});
  }
  return result;
}

}  // namespace

QJsonObject automationFxParameterFacts(TFx* fx, TParam* param,
                                       const std::string& name) {
  const QJsonObject schema  = automationFxParameterSchema(fx, param, name);
  const char* source        = schema.isEmpty()                      ? "none"
                              : findBound(requireProfile(fx), name) ? "profile"
                                                                    : "native";
  TDoubleParam* curve       = firstCurve(param);
  const std::string measure = curve ? curve->getMeasureName() : std::string();
  QJsonObject result{
      {"kind", parameterKind(param)},
      {"default", defaultValue(param)},
      {"bounds", QJsonObject{{"admitted", admittedBounds(schema)},
                             {"native", nativeBounds(param)},
                             {"slider", sliderBounds(param)},
                             {"source", source}}},
      {"measure",
       curve ? QJsonValue(QString::fromStdString(measure)) : QJsonValue()},
      {"unit", !curve ? QJsonValue()
               : measure.empty()
                   ? QJsonValue("scalar")
                   : QJsonValue("native_" + QString::fromStdString(measure))}};
  const QJsonArray menu = choices(param);
  if (!menu.isEmpty()) {
    result.insert("choices", menu);
    result.insert("label_is_identity", false);
  }
  return result;
}

void validateAutomationFxNumber(const QJsonValue& value,
                                const QJsonObject& schema) {
  if (!value.isDouble() || !std::isfinite(value.toDouble()))
    throw Error("INVALID_PARAMS",
                "Parameter value must be a finite JSON number.");
  const double number = value.toDouble();
  if (schema.value("type") == "integer" && std::floor(number) != number)
    throw Error("INVALID_PARAMS", "Parameter value must be an integer.");
  const double minimum =
      schema.value("minimum").toDouble(-std::numeric_limits<double>::max());
  const double maximum =
      schema.value("maximum").toDouble(std::numeric_limits<double>::max());
  if (number < minimum)
    throw Error("OUT_OF_RANGE",
                "Parameter value is below its admitted minimum.");
  if (number > maximum)
    throw Error("OUT_OF_RANGE",
                "Parameter value exceeds its admitted maximum.");
}

void validateAutomationFxProfile(TFx* fx) {
  const TypeProfile& profile = requireProfile(fx);
  if (fx->getFxVersion() != profile.version)
    throw Error(
        "UNSUPPORTED_FX",
        QString("This effect type requires native version %1.")
            .arg(profile.version),
        {{"type", profile.type}, {"native_fx_version", fx->getFxVersion()}});
  TParamContainer* params = fx->getParams();
  for (int index = 0; index < params->getParamCount(); ++index) {
    const TParamVar* entry = params->getParamVar(index);
    if (entry->isObsolete()) continue;
    const QJsonObject schema =
        automationFxParameterSchema(fx, entry->getParam(), entry->getName());
    if (schema.isEmpty())
      throw Error("UNSUPPORTED_PARAMETER",
                  "Admitted effect has a parameter outside the profile.",
                  {{"parameter", QString::fromStdString(entry->getName())}});
    const QString name = QString::fromStdString(entry->getName());
    validateStored(
        entry->getParam(), schema,
        {name, name, constrainedParameter(profile, entry->getName())});
  }
  for (const char* constraint : profile.constraints)
    if (std::string(constraint) == kSeparation) requireGradientSeparation(fx);
}

bool automationFxProfileViolations(TFx* fx, QStringList& violations) {
  const TypeProfile* profile = findProfile(fx->getFxType());
  if (!profile || fx->getFxVersion() != profile->version) return false;
  TParamContainer* params = fx->getParams();
  for (int index = 0; index < params->getParamCount(); ++index) {
    const TParamVar* entry = params->getParamVar(index);
    if (entry->isObsolete()) continue;
    const QJsonObject schema =
        automationFxParameterSchema(fx, entry->getParam(), entry->getName());
    try {
      if (schema.isEmpty())
        throw Error("UNSUPPORTED_PARAMETER", "Parameter outside the profile.");
      const QString name = QString::fromStdString(entry->getName());
      validateStored(
          entry->getParam(), schema,
          {name, name, constrainedParameter(*profile, entry->getName())});
    } catch (const Error&) {
      if (violations.size() < 16)
        violations.append(QString::fromStdString(entry->getName()).left(128));
    }
  }
  try {
    for (const char* constraint : profile->constraints)
      if (std::string(constraint) == kSeparation) requireGradientSeparation(fx);
  } catch (const Error&) {
    if (violations.size() < 16)
      violations.append("constraint:" + QString(kSeparation));
  }
  return true;
}

QJsonObject automationFxProfileDescription(const QString& type) {
  const TypeProfile* profile = findProfile(type.toStdString());
  if (!profile)
    throw Error("UNSUPPORTED_FX",
                "Effect is outside the builtin automation profile.",
                {{"type", type}});
  QJsonArray extents, constraints, inputs;
  for (const ParameterBound& bound : profile->bounds)
    if (bound.extentCoefficient > 0)
      extents.append(QJsonObject{{"name", bound.name},
                                 {"coefficient", bound.extentCoefficient},
                                 {"max_px", bound.extentPixels}});
  for (const char* constraint : profile->constraints)
    constraints.append(constraintDescription(constraint));
  for (const char* port : profile->requiredInputs) inputs.append(port);
  return {
      {"profile_version", profile->version}, {"seed_parameters", QJsonArray{}},
      {"constraints", constraints},          {"extent_parameters", extents},
      {"required_inputs", inputs},           {"replay_class", "stateless"}};
}

QJsonArray automationFxProfileDiagnostics(TFx* fx) {
  QJsonArray result;
  const TypeProfile* profile = findProfile(fx->getFxType());
  if (!profile) return result;
  for (const char* port : profile->requiredInputs) {
    TFxPort* input = fx->getInputPort(port);
    if (input && !input->isConnected())
      result.append(missingInput(fx->getFxType(), port));
  }
  return result;
}

std::vector<AutomationFxExtent> automationFxExtents(const std::string& type) {
  std::vector<AutomationFxExtent> extents;
  if (const TypeProfile* profile = findProfile(type))
    for (const ParameterBound& bound : profile->bounds)
      if (bound.extentCoefficient > 0)
        extents.push_back(
            {bound.name, bound.extentCoefficient, bound.extentPixels});
  return extents;
}

int automationFxMaxGroupPorts(const std::string& type) {
  // The image combinations composite their Source group in port order
  // (common/tfx/binaryFx.cpp); 64 bounds one node's inputs.
  static const std::set<std::string> combinations{"overFx", "multFx",
                                                  "screenFx", "blendFx"};
  return combinations.count(type) ? 64 : 0;
}

bool automationFxNeedsPositiveDeterminant(const std::string& type) {
  const TypeProfile* profile = findProfile(type);
  return profile && profile->positiveDeterminant;
}

QJsonObject automationFxProvenance(TFx* fx) {
  return {{"type", QString::fromStdString(fx->getFxType())},
          {"native_fx_version", fx->getFxVersion()},
          {"replay_class", "stateless"},
          {"seeds", QJsonObject{}}};
}

}  // namespace GroundzeroAutomation
