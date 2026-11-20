#include "automation_animation_targets.h"
#include "automation.h"
#include "automation_animation.h"
#include "automation_fx.h"
#include "automation_fx_profile.h"

#include "tdoubleparam.h"
#include "tfx.h"
#include "tmacrofx.h"
#include "tparamcontainer.h"
#include "tparamset.h"
#include "toonz/txshcolumn.h"
#include "toonz/txshlevelcolumn.h"

#include <QJsonArray>
#include <QSet>

#include <cmath>
#include <vector>

namespace GroundzeroAutomation {
namespace {

constexpr int MaxFrame    = 1000000;
constexpr int MaxSamples  = 10000;
constexpr double MaxBound = 1000000;
constexpr int MaxChannels = 512;

struct ExternalChannel final {
  TDoubleParam* curve;
  QString channel;
  QString unit;
  QString kind;
  QString valueSpace;
  QString editVia;
  // For an effect channel: the parameter and the index of the channel in a
  // point or color parameter (-1 for a scalar).
  QString parameter;
  int component = -1;
};

QString requiredText(const QJsonObject& params, const QString& field) {
  const auto value = params.value(field);
  if (!value.isString() || value.toString().isEmpty())
    throw Error("invalid_params", field + " must be a non-empty string.");
  return value.toString();
}

QString unitFor(const TDoubleParam& curve) {
  const std::string measure = curve.getMeasureName();
  return measure.empty() ? "scalar"
                         : "native_" + QString::fromStdString(measure);
}

void addEffectChannels(TParam* param, const QString& name,
                       std::vector<ExternalChannel>& channels) {
  if (auto* number = dynamic_cast<TDoubleParam*>(param)) {
    channels.push_back({number, name, unitFor(*number), "fx_parameter",
                        "fx_parameter", "animation.set_keys", name});
    return;
  }
  auto* set = dynamic_cast<TParamSet*>(param);
  if (!set) return;
  for (int index = 0; index < set->getParamCount(); ++index) {
    auto* number =
        dynamic_cast<TDoubleParam*>(set->getParam(index).getPointer());
    if (!number) continue;
    const QString component =
        name + "." + QString::fromStdString(set->getParamName(index));
    channels.push_back({number, component, unitFor(*number), "fx_parameter",
                        "fx_parameter", "animation.set_keys", name, index});
  }
}

std::vector<ExternalChannel> effectChannels(Document& document,
                                            const QString& fxId) {
  TFx* fx = automationOwnedFx(document, fxId);
  if (dynamic_cast<TMacroFx*>(fx))
    throw Error("unsupported_target",
                "Expand the macro before reading member parameters.");
  TParamContainer* params = automationParameterFx(fx)->getParams();
  std::vector<ExternalChannel> channels;
  for (int index = 0; index < params->getParamCount(); ++index) {
    const TParamVar* entry = params->getParamVar(index);
    if (entry->isHidden() || entry->isObsolete()) continue;
    addEffectChannels(entry->getParam(),
                      QString::fromStdString(entry->getName()), channels);
    if (channels.size() > MaxChannels)
      throw Error("resource_limit", "The effect has too many parameters.");
  }
  return channels;
}

TXshLevelColumn* levelColumn(Document& document, const QString& columnId) {
  auto* column = dynamic_cast<TXshLevelColumn*>(document.column(columnId));
  if (!column)
    throw Error("unsupported_target",
                "Only level columns have a time remap curve.");
  return column;
}

std::vector<ExternalChannel> remapChannels(Document& document,
                                           const QString& columnId) {
  TXshLevelColumn* column = levelColumn(document, columnId);
  return {{column->getTimeRemapParam(), "time_remap", "frame_number",
           "time_remap", "time_remap", QString()}};
}

std::vector<ExternalChannel> externalChannels(Document& document,
                                              const QJsonObject& params) {
  return params.contains("fx_id")
             ? effectChannels(document, requiredText(params, "fx_id"))
             : remapChannels(document, requiredText(params, "column_id"));
}

QJsonObject targetRecord(const QJsonObject& params,
                         const ExternalChannel& item) {
  QJsonObject target{{"channel", item.channel}, {"unit", item.unit}};
  for (const char* field : {"fx_id", "column_id"})
    if (params.contains(field)) target.insert(field, params.value(field));
  return target;
}

AutomationFxChannel fxChannel(Document& document, const QJsonObject& params,
                              const ExternalChannel& item) {
  return {automationOwnedFx(document, requiredText(params, "fx_id")),
          item.curve, item.parameter, item.channel, item.component};
}

// Bounds and kinds an effect channel admits, and why it cannot be written
// (empty when it can).
struct EffectAdmission final {
  QString reason;
  double minimum  = -MaxBound;
  double maximum  = MaxBound;
  bool linearOnly = false;
};

EffectAdmission effectAdmission(Document& document, const QJsonObject& params,
                                const ExternalChannel& item) {
  EffectAdmission result;
  const AutomationFxChannel channel = fxChannel(document, params, item);
  try {
    automationRequireFxChannelWritable(document, channel);
    TFx* native              = automationParameterFx(channel.fx);
    const std::string name   = item.parameter.toStdString();
    const QJsonObject schema = automationFxChannelSchema(
        native, native->getParams()->getParam(name), name, item.component);
    result.minimum    = schema.value("minimum").toDouble(-MaxBound);
    result.maximum    = schema.value("maximum").toDouble(MaxBound);
    result.linearOnly = automationFxLinearOnly(native, name);
    result.reason     = animationUnsupportedReason(*item.curve);
  } catch (const Error& error) {
    result.reason = error.message;
  }
  return result;
}

QJsonObject describeRemap(const QJsonObject& params,
                          const ExternalChannel& item) {
  const TDoubleParam& curve = *item.curve;
  double minimum = 0, maximum = 0, step = 1;
  if (!curve.getValueRange(minimum, maximum, step) ||
      std::abs(minimum) > MaxBound || std::abs(maximum) > MaxBound) {
    minimum = -MaxBound;
    maximum = MaxBound;
  }
  const QString content = animationUnsupportedReason(curve, CyclePolicy::Allow);
  return {{"channel", item.channel},
          {"unit", item.unit},
          {"kind", item.kind},
          {"target", targetRecord(params, item)},
          {"edit_via", QJsonValue()},
          {"editor_locked", curve.isEditorLocked()},
          {"cycle", curve.isCycleEnabled()},
          {"cycle_supported", false},
          {"key_count", curve.getKeyframeCount()},
          {"writable", false},
          {"unsupported_reason",
           content.isEmpty()
               ? QString("Read only: remap curves have no animation edit tool.")
               : content},
          {"interpolations", QJsonArray()},
          {"frame_bounds", QJsonArray{0, MaxFrame}},
          {"value_bounds", QJsonArray{minimum, maximum}},
          {"step_bounds", QJsonArray{1, 1000}},
          {"key_time", "integer"},
          {"sample_time", "fractional"}};
}

QJsonObject describe(Document& document, const QJsonObject& params,
                     const ExternalChannel& item) {
  if (item.parameter.isEmpty()) return describeRemap(params, item);
  const TDoubleParam& curve       = *item.curve;
  const EffectAdmission admission = effectAdmission(document, params, item);
  const QJsonArray kinds =
      admission.linearOnly
          ? QJsonArray{"linear"}
          : QJsonArray{"constant", "linear", "ease_in_out", "speed_in_out"};
  return {{"channel", item.channel},
          {"unit", item.unit},
          {"kind", item.kind},
          {"target", targetRecord(params, item)},
          {"edit_via", item.editVia},
          {"editor_locked", curve.isEditorLocked()},
          {"cycle", curve.isCycleEnabled()},
          {"cycle_supported", false},
          {"key_count", curve.getKeyframeCount()},
          {"writable", admission.reason.isEmpty()},
          {"unsupported_reason", admission.reason.isEmpty()
                                     ? QJsonValue()
                                     : QJsonValue(admission.reason)},
          {"interpolations", kinds},
          {"frame_bounds", QJsonArray{0, MaxFrame}},
          {"value_bounds", QJsonArray{admission.minimum, admission.maximum}},
          {"step_bounds", QJsonArray{1, admission.linearOnly ? 1 : 1000}},
          {"key_time", "integer"},
          {"sample_time", "fractional"}};
}

ExternalChannel findChannel(Document& document, const QJsonObject& params) {
  const QString name = requiredText(params, "channel");
  for (const auto& item : externalChannels(document, params))
    if (item.channel == name) {
      if (requiredText(params, "unit") != item.unit)
        throw Error("invalid_unit",
                    "This channel requires unit " + item.unit + ".");
      return item;
    }
  throw Error("unsupported_channel",
              "This target has no such animation channel.");
}

QJsonObject identity(const QJsonObject& params, const ExternalChannel& item) {
  QJsonObject result{{"channel", item.channel}, {"unit", item.unit}};
  for (const char* field : {"fx_id", "column_id"})
    if (params.contains(field)) result.insert(field, params.value(field));
  return result;
}

}  // namespace

bool isAnimationExternalRequest(const QJsonObject& params) {
  const int named = int(params.contains("object_id")) +
                    int(params.contains("fx_id")) +
                    int(params.contains("column_id"));
  if (named > 1)
    throw Error("invalid_params",
                "Name exactly one of object_id, fx_id and column_id.");
  return params.contains("fx_id") || params.contains("column_id");
}

QJsonObject describeAnimationExternalChannels(Document& document,
                                              const QJsonObject& params) {
  if (params.contains("layer_id"))
    throw Error("invalid_params", "layer_id belongs to an object_id request.");
  QJsonArray channels;
  const auto items = externalChannels(document, params);
  for (const auto& item : items)
    channels.append(describe(document, params, item));
  QJsonObject result{{"channels", channels},
                     {"value_space", items.empty() ? "fx_parameter"
                                                   : items.front().valueSpace}};
  if (params.contains("fx_id")) result.insert("fx_id", params.value("fx_id"));
  if (params.contains("column_id")) {
    result.insert("column_id", params.value("column_id"));
    result.insert("remap_enabled",
                  levelColumn(document, requiredText(params, "column_id"))
                      ->isTimeRemapEnabled());
  }
  return result;
}

AutomationFxChannel automationFxChannel(Document& document,
                                        const QJsonObject& params) {
  if (params.contains("layer_id"))
    throw Error("invalid_params", "layer_id belongs to an object_id request.");
  return fxChannel(document, params, findChannel(document, params));
}

QJsonObject inspectAnimationExternalKeys(Document& document,
                                         const QJsonObject& params) {
  const auto item    = findChannel(document, params);
  QJsonObject result = identity(params, item);
  result.insert("value_space", item.valueSpace);
  result.insert("editor_locked", item.curve->isEditorLocked());
  result.insert("cycle", item.curve->isCycleEnabled());
  result.insert("keys", inspectAnimationKeys(*item.curve));
  return result;
}

QJsonObject sampleAnimationExternal(Document& document,
                                    const QJsonObject& params) {
  const auto item = findChannel(document, params);
  requireSupportedAnimationCurve(*item.curve, CyclePolicy::Allow);
  const QJsonValue requested = params.value("frames");
  if (!requested.isArray() || requested.toArray().isEmpty() ||
      requested.toArray().size() > MaxSamples)
    throw Error("invalid_argument",
                "frames must contain 1 through 10000 numbers.");
  QSet<double> seen;
  std::vector<double> frames;
  for (const auto& value : requested.toArray()) {
    const double frame = value.toDouble();
    if (!value.isDouble() || !std::isfinite(frame) || frame < 0 ||
        frame > MaxFrame || seen.contains(frame))
      throw Error("invalid_argument",
                  "frames must be distinct numbers from 0 through 1000000.");
    seen.insert(frame);
    frames.push_back(frame);
  }
  requireAnimationSampleBudget(*item.curve, frames);
  QJsonArray values;
  for (const double frame : frames) {
    double value = 0;
    std::string error;
    if (!item.curve->getValueChecked(frame, value, error) ||
        !std::isfinite(value))
      throw Error("invalid_argument",
                  "Native animation evaluation failed: " +
                      QString::fromStdString(error),
                  {{"frame", frame}});
    values.append(QJsonObject{{"frame", frame}, {"value", value}});
  }
  QJsonObject result = identity(params, item);
  result.insert("value_space", item.valueSpace);
  result.insert("values", values);
  return result;
}

}  // namespace GroundzeroAutomation
