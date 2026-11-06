#pragma once
#include "toonz/rasterbrushmaskmetadata.h"
#include "rasterbrushsourcemetadata.h"
#include <QJsonArray>
#include <QRegularExpression>
#include <QSet>
#include <cmath>
#include <stdexcept>
namespace RasterBrush {
namespace Detail {
inline void sourceMaskRequire(bool condition) {
  if (!condition) throw std::runtime_error("Invalid preserved source mask metadata");
}
inline const QStringList &sourceMaskGeometryNumbers() {
  static const QStringList keys{"angle", "spacing", "autoSpacingCoeff", "scale", "randomness", "density", "diameter", "ratio", "hfade", "vfade", "spikes"};
  return keys;
}
inline const QStringList &sourceMaskGeometryFlags() {
  static const QStringList keys{"useAutoSpacing", "antialiasEdges"};
  return keys;
}
inline const QStringList &sourceMaskUnsupportedTags() {
  static const QStringList tags{"unrecognized-setting", "unrecognized-geometry", "invalid-declaration", "unsupported-tip-kind", "unresolved-resource", "unsupported-tip-application", "unprojectable-generated", "unverified-resource-color-contract"};
  return tags;
}
inline void validateSourceMaskDefinition(const SourceMaskDefinitionV1 &value) {
  static const QRegularExpression hash("^[0-9a-f]{64}$");
  sourceMaskRequire(hash.match(value.sourceFieldsSha256).hasMatch());
  sourceMaskRequire(value.sourceResourceSha256.isEmpty() || hash.match(value.sourceResourceSha256).hasMatch());
  sourceMaskRequire(QStringList{"absent", "canonical-rgba8-srgb", "alpha-u8"}.contains(value.resourceEncoding));
  sourceMaskRequire((value.tipKind == "decoded-resource") == (value.resourceEncoding != "absent"));
  // This declaration is required even for manifest-only, unbound resources.
  if (value.resourceEncoding == "canonical-rgba8-srgb")
    sourceMaskRequire(value.unsupportedFeatures.contains("unverified-resource-color-contract"));
  sourceMaskRequire(QStringList{"absent", "multiply", "subtract", "unsupported"}.contains(value.composition));
  sourceMaskRequire(QStringList{"absent", "generated", "decoded-resource", "unsupported"}.contains(value.tipKind));
  sourceMaskRequire(QStringList{"absent", "alpha-mask", "legacy-unspecified", "unsupported"}.contains(value.application));
  sourceMaskRequire(!value.masterSizeCoeff || sourceMetadataNumber(*value.masterSizeCoeff));
  sourceMaskRequire(value.geometry.size() <= 18 && value.controlValues.size() <= 16);
  for (auto item = value.geometry.begin(); item != value.geometry.end(); ++item) {
    if (sourceMaskGeometryNumbers().contains(item.key())) sourceMaskRequire(item.value().isDouble() && sourceMetadataNumber(item.value().toDouble()));
    else if (sourceMaskGeometryFlags().contains(item.key())) sourceMaskRequire(item.value().isBool());
    else if (item.key() == "generator") sourceMaskRequire(item.value().isString() && QStringList{"default", "soft", "gauss"}.contains(item.value().toString()));
    else if (item.key() == "shape") sourceMaskRequire(item.value().isString() && QStringList{"circle", "rect"}.contains(item.value().toString()));
    else if (item.key() == "tipType") sourceMaskRequire(item.value().isString() && QStringList{"auto_brush", "png_brush", "gbr_brush", "gih_brush"}.contains(item.value().toString()));
    else sourceMaskRequire(false);
  }
  for (auto item = value.controlValues.begin(); item != value.controlValues.end(); ++item)
    sourceMaskRequire(sourceSensorTargetsV1().contains(item.key()) && item.value().isDouble() && sourceMetadataNumber(item.value().toDouble()));
  QSet<QString> tags;
  for (const auto &tag : value.unsupportedFeatures) {
    sourceMaskRequire(sourceMaskUnsupportedTags().contains(tag) && !tags.contains(tag));
    tags.insert(tag);
  }
  sourceMaskRequire((value.tipKind == "generated") == value.generated.has_value());
  if (value.generated) {
    const auto &generated = *value.generated;
    sourceMaskRequire(validateSourceGeneratedDefinitionV1(generated).empty());
    const QString generator = generated.generator == SourceGeneratedGenerator::Soft ? "soft"
        : generated.generator == SourceGeneratedGenerator::Gaussian ? "gauss" : "default";
    const QString shape = generated.shape == SourceGeneratedShape::Rectangle ? "rect" : "circle";
    const QJsonObject duplicated{{"generator", generator}, {"shape", shape},
        {"diameter", generated.diameter}, {"ratio", generated.ratio},
        {"hfade", generated.horizontalFade}, {"vfade", generated.verticalFade},
        {"spikes", generated.spikes}};
    // Compare only declarations actually present in the source projection.
    // Typed defaults do not manufacture geometry keys or absent source flags.
    for (auto item = duplicated.begin(); item != duplicated.end(); ++item)
      if (value.geometry.contains(item.key()))
        sourceMaskRequire(value.geometry.value(item.key()) == item.value());
    if (value.geometry.contains("antialiasEdges"))
      sourceMaskRequire(generated.antialiasEdges.has_value() &&
          value.geometry.value("antialiasEdges").toBool() == *generated.antialiasEdges);
  }
  if (value.geometry.contains("tipType")) {
    const QString type = value.geometry.value("tipType").toString();
    if (value.tipKind == "generated") sourceMaskRequire(type == "auto_brush");
    if (value.tipKind == "decoded-resource") sourceMaskRequire(type == "png_brush" || type == "gbr_brush");
  }
  if (value.tipImage) {
    const auto &image = *value.tipImage;
    sourceMaskRequire(image.color == (value.resourceEncoding == "canonical-rgba8-srgb"));
    sourceMaskRequire(value.tipKind == "decoded-resource" && image.width >= 1 && image.height >= 1 && image.width <= 8192 && image.height <= 8192 && size_t(image.width) * image.height <= 2u * 1024u * 1024u && image.pixels.size() == size_t(image.width) * image.height);
    for (const auto &pixel : image.pixels)
      sourceMaskRequire(std::isfinite(pixel.a) && std::isfinite(pixel.r) && std::isfinite(pixel.g) && std::isfinite(pixel.b) && pixel.a >= 0 && pixel.a <= 1 && pixel.r >= 0 && pixel.r <= pixel.a && pixel.g >= 0 && pixel.g <= pixel.a && pixel.b >= 0 && pixel.b <= pixel.a);
  }
}
// Both declarations preserve the same original masked targetValue scalar.
// Other scopes, active Preset values and missing declarations stay independent.
inline void validateSourceMaskSensorConsistency(const SourceMaskDefinitionV1 &mask,
                                                const SourceSensorsV1 &sensors) {
  for (const auto &control : sensors.controls)
    if (control.scope == "masked" && control.strength && mask.controlValues.contains(control.target)) {
      const auto value = mask.controlValues.value(control.target);
      sourceMaskRequire(value.isDouble() && value.toDouble() == *control.strength);
    }
}
inline QJsonObject sourceMaskDefinitionJson(const SourceMaskDefinitionV1 &value, const QString &imageId, const QJsonValue &generated) {
  validateSourceMaskDefinition(value);
  sourceMaskRequire((value.tipKind == "decoded-resource") == !imageId.isEmpty());
  sourceMaskRequire(bool(value.tipImage) == !imageId.isEmpty());
  return {{"version", 1}, {"enabled", sourceMetadataOptional(value.enabled)}, {"useMasterSize", sourceMetadataOptional(value.useMasterSize)}, {"masterSizeCoeff", sourceMetadataOptional(value.masterSizeCoeff)}, {"composition", value.composition}, {"tipKind", value.tipKind}, {"application", value.application}, {"geometry", value.geometry}, {"controlValues", value.controlValues}, {"generated", generated}, {"image", imageId}, {"sourceFieldsSha256", value.sourceFieldsSha256}, {"sourceResourceSha256", value.sourceResourceSha256}, {"resourceEncoding", value.resourceEncoding}, {"unsupportedFeatures", QJsonArray::fromStringList(value.unsupportedFeatures)}};
}
template <class GeneratedReader>
inline SourceMaskDefinitionV1 sourceMaskDefinition(const QJsonValue &input, GeneratedReader readGenerated, QString &imageId) {
  sourceMaskRequire(input.isObject());
  const auto object = input.toObject();
  const QStringList keys{"version", "enabled", "useMasterSize", "masterSizeCoeff", "composition", "tipKind", "application", "geometry", "controlValues", "generated", "image", "sourceFieldsSha256", "sourceResourceSha256", "resourceEncoding", "unsupportedFeatures"};
  sourceMaskRequire(object.size() == keys.size());
  for (const auto &key : keys) sourceMaskRequire(object.contains(key));
  sourceMaskRequire(object["version"].isDouble() && object["version"].toDouble() == 1);
  SourceMaskDefinitionV1 result;
  result.enabled = sourceMetadataBoolean(object["enabled"]);
  result.useMasterSize = sourceMetadataBoolean(object["useMasterSize"]);
  result.masterSizeCoeff = sourceMetadataDouble(object["masterSizeCoeff"]);
  for (const QString &key : {"composition", "tipKind", "application", "image", "sourceFieldsSha256", "sourceResourceSha256", "resourceEncoding"}) sourceMaskRequire(object[key].isString() && object[key].toString().size() <= 128);
  result.composition = object["composition"].toString(); result.tipKind = object["tipKind"].toString(); result.application = object["application"].toString(); result.sourceFieldsSha256 = object["sourceFieldsSha256"].toString();
  result.sourceResourceSha256 = object["sourceResourceSha256"].toString();
  result.resourceEncoding = object["resourceEncoding"].toString();
  sourceMaskRequire(object["geometry"].isObject() && object["controlValues"].isObject());
  result.geometry = object["geometry"].toObject(); result.controlValues = object["controlValues"].toObject();
  if (!object["generated"].isNull()) result.generated = readGenerated(object["generated"]);
  sourceMaskRequire(object["unsupportedFeatures"].isArray() && object["unsupportedFeatures"].toArray().size() <= sourceMaskUnsupportedTags().size());
  for (const auto &tag : object["unsupportedFeatures"].toArray()) { sourceMaskRequire(tag.isString()); result.unsupportedFeatures.append(tag.toString()); }
  imageId = object["image"].toString();
  static const QRegularExpression identity("^[0-9a-f]{64}$");
  sourceMaskRequire(imageId.isEmpty() || identity.match(imageId).hasMatch());
  sourceMaskRequire((result.tipKind == "decoded-resource") == !imageId.isEmpty());
  validateSourceMaskDefinition(result);
  return result;
}
inline void sourceMaskUnsupported(SourceMaskDefinitionV1 &value, const QString &tag) {
  if (!value.unsupportedFeatures.contains(tag)) value.unsupportedFeatures.append(tag);
}
}  // namespace Detail
}  // namespace RasterBrush
