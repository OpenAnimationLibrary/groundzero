#include "automation.h"
#include "automation_animation_targets.h"
#include "automation_fx.h"
#include "tests/automation_animation_test.h"

#include "tdoublekeyframe.h"
#include "tdoubleparam.h"
#include "tfx.h"
#include "tparamcontainer.h"
#include "toonz/fxdag.h"
#include "toonz/tcolumnfxset.h"
#include "toonz/timeremap.h"
#include "toonz/toonzscene.h"
#include "toonz/txsheet.h"
#include "toonz/txshlevelcolumn.h"

#include <QJsonArray>
#include <QJsonDocument>

#include <cmath>
#include <functional>
#include <memory>
#include <stdexcept>

namespace GroundzeroAutomation {
namespace {

void require(bool condition, const char* message) {
  if (!condition) throw std::runtime_error(message);
}

TDoubleKeyframe linearKey(double frame, double value) {
  TDoubleKeyframe key(frame, value);
  key.m_type       = TDoubleKeyframe::Linear;
  key.m_isKeyframe = true;
  return key;
}

void refused(const QString& code, const std::function<void()>& action) {
  try {
    action();
  } catch (const Error& error) {
    require(error.code == code,
            "An animation target refusal has the wrong code");
    return;
  }
  throw std::runtime_error("An animation target request was not refused: " +
                           code.toStdString());
}

QJsonObject channelNamed(const QJsonObject& described, const QString& name) {
  for (const auto& entry : described.value("channels").toArray())
    if (entry.toObject().value("channel").toString() == name)
      return entry.toObject();
  throw std::runtime_error("The descriptor lacks channel " +
                           name.toStdString());
}

QJsonObject request(const QString& field, const QString& id,
                    const QString& channel, const QString& unit) {
  return {{field, id}, {"channel", channel}, {"unit", unit}};
}

void effectParametersAreDescribedAndRead() {
  Document document(std::make_unique<ToonzScene>());
  TFxP effect = makeAutomationEffect("STD_blurFx");
  FxDag* dag  = document.scene->getXsheet()->getFxDag();
  dag->assignUniqueId(effect.getPointer());
  effect->setNewIdentifier();
  dag->getInternalFxs()->addFx(effect.getPointer());
  const QString id = document.idForFx(effect.getPointer());
  auto* value =
      dynamic_cast<TDoubleParam*>(effect->getParams()->getParam("value"));
  require(value, "The blur effect has no value parameter");
  value->setKeyframe(linearKey(0, 2));
  value->setKeyframe(linearKey(10, 12));
  const auto before = value->getKeyframe(0).m_value;

  const auto described =
      describeAnimationExternalChannels(document, {{"fx_id", id}});
  const auto channel = channelNamed(described, "value");
  require(channel.value("kind") == "fx_parameter" &&
              channel.value("edit_via") == "animation.set_keys" &&
              channel.value("writable").toBool() &&
              channel.value("key_count").toInt() == 2 &&
              !channel.value("cycle").toBool() &&
              channel.value("target").toObject().value("fx_id") == id &&
              channel.value("unsupported_reason").isNull() &&
              channel.value("value_bounds") == QJsonArray{0, 200} &&
              channel.value("interpolations").toArray().size() == 4,
          "An effect parameter descriptor is incomplete");
  const QString unit = channel.value("unit").toString();
  require(!unit.isEmpty(), "An effect parameter descriptor has no unit");

  const auto keys = inspectAnimationExternalKeys(
      document, request("fx_id", id, "value", unit));
  require(keys.value("keys").toArray().size() == 2 &&
              keys.value("fx_id") == id && !keys.contains("object_id") &&
              keys.value("value_space") == "fx_parameter",
          "Effect parameter keys differ from the native curve");
  auto sampleRequest = request("fx_id", id, "value", unit);
  sampleRequest.insert("frames", QJsonArray{0, 2.5, 10, 12});
  const auto sampled = sampleAnimationExternal(document, sampleRequest);
  const auto values  = sampled.value("values").toArray();
  require(values.size() == 4, "Effect parameter sampling lost a frame");
  for (const auto& entry : values) {
    const double frame = entry.toObject().value("frame").toDouble();
    require(
        entry.toObject().value("value").toDouble() == value->getValue(frame),
        "An effect parameter sample differs from the native value");
  }

  refused("unsupported_channel", [&]() {
    inspectAnimationExternalKeys(document, request("fx_id", id, "nope", unit));
  });
  refused("invalid_unit", [&]() {
    inspectAnimationExternalKeys(document,
                                 request("fx_id", id, "value", "inch"));
  });
  refused("invalid_params", [&]() {
    isAnimationExternalRequest({{"fx_id", id}, {"object_id", "object-1"}});
  });
  refused("invalid_params", [&]() {
    describeAnimationExternalChannels(document,
                                      {{"fx_id", id}, {"layer_id", "layer-1"}});
  });

  auto expression             = value->getKeyframe(1);
  expression.m_type           = TDoubleKeyframe::Expression;
  expression.m_expressionText = "secret_expression_text";
  value->setKeyframe(expression);
  const auto unsupported = inspectAnimationExternalKeys(
      document, request("fx_id", id, "value", unit));
  const auto last = unsupported.value("keys").toArray().at(1).toObject();
  require(last.value("interpolation") == "unsupported" &&
              !QJsonDocument(unsupported)
                   .toJson()
                   .contains("secret_expression_text"),
          "An expression key was not described as unsupported");
  refused("unsupported_interpolation",
          [&]() { sampleAnimationExternal(document, sampleRequest); });
  const auto locked = channelNamed(
      describeAnimationExternalChannels(document, {{"fx_id", id}}), "value");
  require(!locked.value("writable").toBool() &&
              !locked.value("unsupported_reason").toString().isEmpty(),
          "An effect channel outside the key profile was described writable");
  require(value->getKeyframe(0).m_value == before,
          "Reading effect parameters changed the curve");
}

void remapCurveIsDescribedAndSampled() {
  Document document(std::make_unique<ToonzScene>());
  auto* column = new TXshLevelColumn();
  document.scene->getXsheet()->insertColumn(0, column);
  const QString id = document.idForColumn(column);

  auto described =
      describeAnimationExternalChannels(document, {{"column_id", id}});
  auto channel = channelNamed(described, "time_remap");
  require(channel.value("kind") == "time_remap" &&
              channel.value("edit_via").isNull() &&
              !channel.value("writable").toBool() &&
              channel.value("key_count").toInt() == 0 &&
              !described.value("remap_enabled").toBool() &&
              channel.value("unit") == "frame_number",
          "An empty remap descriptor is wrong");

  TDoubleParam* remap = column->getTimeRemapParam();
  for (const auto& key : TimeRemap::loopKeys(0, 3))
    remap->setKeyframe(linearKey(key.first, key.second));
  remap->enableCycle(true);
  column->setTimeRemapEnabled(true);

  described = describeAnimationExternalChannels(document, {{"column_id", id}});
  channel   = channelNamed(described, "time_remap");
  require(channel.value("cycle").toBool() &&
              !channel.value("cycle_supported").toBool() &&
              channel.value("key_count").toInt() == 3 &&
              described.value("remap_enabled").toBool(),
          "A cycled remap descriptor is wrong");
  const auto keys = inspectAnimationExternalKeys(
      document, request("column_id", id, "time_remap", "frame_number"));
  require(keys.value("cycle").toBool() &&
              keys.value("keys").toArray().size() == 3 &&
              keys.value("column_id") == id,
          "Remap keys differ from the native curve");
  auto sampleRequest = request("column_id", id, "time_remap", "frame_number");
  QJsonArray frames;
  for (int frame = 0; frame < 40; ++frame) frames.append(frame);
  sampleRequest.insert("frames", frames);
  const auto sampled = sampleAnimationExternal(document, sampleRequest);
  for (const auto& entry : sampled.value("values").toArray()) {
    const int row = entry.toObject().value("frame").toInt();
    require(TimeRemap::valueToRow(entry.toObject().value("value").toDouble()) ==
                TimeRemap::loopRow(row, 0, 3),
            "A cycled remap sample does not loop the rows");
  }
  refused("invalid_handle", [&]() {
    describeAnimationExternalChannels(document, {{"column_id", "column-404"}});
  });
}

}  // namespace

void runAutomationAnimationTargetTests() {
  effectParametersAreDescribedAndRead();
  remapCurveIsDescribedAndSampled();
}

}  // namespace GroundzeroAutomation
