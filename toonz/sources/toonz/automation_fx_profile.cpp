#include "automation.h"
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

// A finite number moved to the nearest value its schema admits.
double clampedNumber(double value, const QJsonObject& schema) {
  const double lowest  = std::numeric_limits<double>::lowest();
  const double minimum = schema.value("minimum").toDouble(lowest);
  const double maximum =
      schema.value("maximum").toDouble(std::numeric_limits<double>::max());
  double result = std::clamp(value, minimum, maximum);
  if (schema.value("type") == "integer")
    result =
        std::clamp(std::round(result), std::ceil(minimum), std::floor(maximum));
  return result;
}

void clampCurve(TDoubleParam* curve, const QJsonObject& schema) {
  if (std::isfinite(curve->getDefaultValue()))
    curve->setDefaultValue(clampedNumber(curve->getDefaultValue(), schema));
  for (int index = 0; index < curve->getKeyframeCount(); ++index) {
    TDoubleKeyframe key = curve->getKeyframe(index);
    if (!std::isfinite(key.m_value)) continue;
    key.m_value = clampedNumber(key.m_value, schema);
    curve->setKeyframe(index, key);
  }
}

void validateCurve(TDoubleParam* curve, const QJsonObject& schema) {
  if (curve->getKeyframeCount() > kMaximumKeys)
    throw Error("RESOURCE_LIMIT", "Effect parameter exceeds 4096 scalar keys.");
  validateAutomationFxNumber(curve->getDefaultValue(), schema);
  for (int index = 0; index < curve->getKeyframeCount(); ++index) {
    const TDoubleKeyframe& key = curve->getKeyframe(index);
    validateAutomationFxNumber(key.m_value, schema);
    const auto previous =
        index == 0 ? TDoubleKeyframe::None : TDoubleKeyframe::Linear;
    if (key.m_type != TDoubleKeyframe::Linear || key.m_prevType != previous ||
        key.m_step != 1)
      throw Error("UNSUPPORTED_PARAMETER",
                  "Admitted FX accept only scalar linear animation keys.");
  }
}

// Reads the channels the native color evaluates: disabling the matte replaces
// the matte curve with one that the parameter set's children do not list.
void validatePixel(TPixelParam* pixel) {
  const QJsonObject unit{{"type", "number"}, {"minimum", 0}, {"maximum", 1}};
  for (TDoubleParam* curve :
       {pixel->getRed().getPointer(), pixel->getGreen().getPointer(),
        pixel->getBlue().getPointer()})
    validateCurve(curve, unit);
  if (pixel->isMatteEnabled())
    return validateCurve(pixel->getMatte().getPointer(), unit);
  // A disabled matte stores 255.0 natively and is read clamped to 255.
  try {
    validateCurve(pixel->getMatte().getPointer(),
                  {{"type", "number"}, {"minimum", 1}, {"maximum", 255}});
  } catch (const Error& error) {
    if (error.code != "OUT_OF_RANGE") throw;
    throw Error("OUT_OF_RANGE",
                "This color has no native matte; its alpha must be 255.");
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

void validateStored(TParam* param, const QJsonObject& schema) {
  if (auto* number = dynamic_cast<TDoubleParam*>(param))
    return validateCurve(number, schema);
  if (auto* point = dynamic_cast<TPointParam*>(param)) {
    validateCurve(channel(point, 0), schema.value("items").toObject());
    validateCurve(channel(point, 1), schema.value("items").toObject());
    return;
  }
  if (auto* pixel = dynamic_cast<TPixelParam*>(param))
    return validatePixel(pixel);
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
    validateStored(entry->getParam(), schema);
  }
  for (const char* constraint : profile.constraints)
    if (std::string(constraint) == kSeparation) requireGradientSeparation(fx);
}

void clampAutomationFxValues(TFx* fx) {
  const TypeProfile* profile = findProfile(fx->getFxType());
  if (!profile || fx->getFxVersion() != profile->version) return;
  TParamContainer* params = fx->getParams();
  for (int index = 0; index < params->getParamCount(); ++index) {
    const TParamVar* entry = params->getParamVar(index);
    if (entry->isObsolete()) continue;
    TParam* param = entry->getParam();
    const QJsonObject schema =
        automationFxParameterSchema(fx, param, entry->getName());
    if (schema.isEmpty()) continue;
    if (auto* number = dynamic_cast<TDoubleParam*>(param)) {
      clampCurve(number, schema);
    } else if (auto* point = dynamic_cast<TPointParam*>(param)) {
      clampCurve(channel(point, 0), schema.value("items").toObject());
      clampCurve(channel(point, 1), schema.value("items").toObject());
    } else if (auto* integer = dynamic_cast<TIntParam*>(param)) {
      integer->setValue(int(clampedNumber(integer->getValue(), schema)));
    }
  }
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
      validateStored(entry->getParam(), schema);
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
  return {{"seed_parameters", QJsonArray{}},
          {"constraints", constraints},
          {"extent_parameters", extents},
          {"required_inputs", inputs},
          {"replay_class", "stateless"}};
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
