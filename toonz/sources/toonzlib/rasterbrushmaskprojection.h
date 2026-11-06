#pragma once
#include "rasterbrushmaskmetadata.h"
#include <QCryptographicHash>
#include <QJsonDocument>
#include <QMap>
#include <QXmlStreamReader>

namespace RasterBrush {
namespace Detail {
// Names are resolver-only output and are never serialized into declarations.
inline std::optional<SourceMaskDefinitionV1> preserveKppSourceMask(
    const QMap<QString, QString> &fields, QString &resourceName, QString &resourceMd5) {
  resourceName.clear(); resourceMd5.clear();
  QJsonObject raw;
  for (auto item = fields.begin(); item != fields.end(); ++item)
    if (item.key().startsWith("MaskingBrush/")) raw[item.key()] = item.value();
  if (raw.isEmpty()) return {};
  SourceMaskDefinitionV1 result;
  result.sourceFieldsSha256 = QString::fromLatin1(QCryptographicHash::hash(
      QJsonDocument(raw).toJson(QJsonDocument::Compact), QCryptographicHash::Sha256).toHex());
  auto flag = [&](const QString &key, std::optional<bool> &target) {
    if (!fields.contains(key)) return;
    if (fields[key] == "true" || fields[key] == "false") target = fields[key] == "true";
    else sourceMaskUnsupported(result, "invalid-declaration");
  };
  auto numeric = [&](const QString &text, double &target) {
    bool ok = false; const double number = text.toDouble(&ok);
    if (!ok || !sourceMetadataNumber(number)) { sourceMaskUnsupported(result, "invalid-declaration"); return false; }
    target = number; return true;
  };
  flag("MaskingBrush/Enabled", result.enabled);
  flag("MaskingBrush/UseMasterSize", result.useMasterSize);
  if (fields.contains("MaskingBrush/MasterSizeCoeff")) {
    double number; if (numeric(fields["MaskingBrush/MasterSizeCoeff"], number)) result.masterSizeCoeff = number;
  }
  if (fields.contains("MaskingBrush/MaskingCompositeOp")) {
    const auto mode = fields["MaskingBrush/MaskingCompositeOp"];
    result.composition = mode == "multiply" || mode == "subtract" ? mode : "unsupported";
    if (result.composition == "unsupported") sourceMaskUnsupported(result, "unrecognized-setting");
  }
  const QString prefix = "MaskingBrush/Preset/";
  QSet<QString> known{"MaskingBrush/Enabled", "MaskingBrush/UseMasterSize", "MaskingBrush/MasterSizeCoeff",
      "MaskingBrush/MaskingCompositeOp", prefix + "paintop", prefix + "brush_definition", prefix + "Texture/Pattern/Enabled"};
  for (const auto &target : sourceSensorTargetsV1()) {
    for (const QString &suffix : {"Value", "UseCurve", "UseSameCurve", "commonCurve", "Sensor", "curveMode"}) known.insert(prefix + target + suffix);
    known.insert(prefix + "Pressure" + target);
    if (fields.contains(prefix + target + "Value")) {
      double number; if (numeric(fields[prefix + target + "Value"], number)) result.controlValues[target] = number;
    }
  }
  for (auto item = raw.begin(); item != raw.end(); ++item)
    if (!known.contains(item.key())) sourceMaskUnsupported(result, "unrecognized-setting");
  if ((fields.contains(prefix + "paintop") && fields[prefix + "paintop"] != "paintbrush") ||
      fields.value(prefix + "Texture/Pattern/Enabled") == "true") sourceMaskUnsupported(result, "unrecognized-setting");
  const QString definition = fields.value(prefix + "brush_definition");
  if (definition.isEmpty()) return result;
  if (definition.size() > 128 * 1024) { result.tipKind = "unsupported"; sourceMaskUnsupported(result, "invalid-declaration"); return result; }
  QXmlStreamReader xml(definition);
  int depth = 0, brushes = 0, masks = 0; bool invalid = false; QString softnessCurve;
  while (!xml.atEnd()) {
    xml.readNext();
    if (xml.isDTD() || xml.isEntityReference() || (xml.isCharacters() && !xml.isWhitespace())) { invalid = true; break; }
    if (xml.isEndElement()) { --depth; continue; }
    if (!xml.isStartElement()) continue;
    ++depth;
    const bool brush = depth == 1 && xml.name() == QStringLiteral("Brush");
    const bool mask = depth == 2 && xml.name() == QStringLiteral("MaskGenerator");
    if ((!brush && !mask) || !xml.namespaceUri().isEmpty() || !xml.namespaceDeclarations().empty()) { invalid = true; break; }
    if (brush) ++brushes; else ++masks;
    for (const auto &attribute : xml.attributes()) {
      const QString name = attribute.name().toString(), value = attribute.value().toString();
      if (!attribute.namespaceUri().isEmpty()) { invalid = true; break; }
      const bool numericAttribute = brush
          ? QStringList{"angle", "spacing", "autoSpacingCoeff", "scale", "randomness", "density"}.contains(name)
          : QStringList{"diameter", "ratio", "hfade", "vfade", "spikes"}.contains(name);
      const bool flagAttribute = brush ? name == "useAutoSpacing" : name == "antialiasEdges";
      if (numericAttribute) {
        double number; if (numeric(value, number)) result.geometry[name] = number;
      } else if (flagAttribute) {
        if (value == "0" || value == "1") result.geometry[name] = value == "1";
        else sourceMaskUnsupported(result, "invalid-declaration");
      } else if (brush && name == "type") {
        if (QStringList{"auto_brush", "png_brush", "gbr_brush", "gih_brush"}.contains(value)) result.geometry["tipType"] = value;
        else sourceMaskUnsupported(result, "unsupported-tip-kind");
      } else if (mask && name == "id") {
        if (QStringList{"default", "soft", "gauss"}.contains(value)) result.geometry["generator"] = value;
        else sourceMaskUnsupported(result, "unprojectable-generated");
      } else if (mask && name == "type") {
        if (value == "circle" || value == "rect") result.geometry["shape"] = value;
        else sourceMaskUnsupported(result, "unprojectable-generated");
      } else if (brush && name == "filename") resourceName = value;
      else if (brush && name == "md5sum") resourceMd5 = value.toLower();
      else if (mask && name == "softness_curve") softnessCurve = value;
      else if (!(brush && QStringList{"BrushVersion", "brushApplication", "ColorAsMask"}.contains(name))) sourceMaskUnsupported(result, "unrecognized-geometry");
    }
    if (brush) {
      const auto attrs = xml.attributes(); const QString mode = attrs.value("brushApplication").toString(), legacy = attrs.value("ColorAsMask").toString();
      result.application = mode == "0" || (mode.isEmpty() && legacy == "1") ? "alpha-mask"
          : mode.isEmpty() && legacy.isEmpty() ? "legacy-unspecified" : "unsupported";
      if (result.application == "unsupported") sourceMaskUnsupported(result, "unsupported-tip-application");
    }
  }
  if (invalid || xml.hasError() || brushes != 1 || masks > 1) { result.tipKind = "unsupported"; resourceName.clear(); resourceMd5.clear(); sourceMaskUnsupported(result, "invalid-declaration"); return result; }
  if (result.geometry.value("tipType") != "auto_brush") {
    if (masks) sourceMaskUnsupported(result, "unrecognized-geometry");
    result.tipKind = "unsupported";
    return result;
  }
  if (!resourceName.isEmpty() || !resourceMd5.isEmpty()) sourceMaskUnsupported(result, "unrecognized-geometry");
  resourceName.clear(); resourceMd5.clear();
  if (masks != 1 || !result.geometry.contains("generator") || !result.geometry.contains("shape")) { result.tipKind = "unsupported"; sourceMaskUnsupported(result, "unprojectable-generated"); return result; }
  SourceGeneratedDefinitionV1 generated;
  generated.generator = result.geometry["generator"] == "soft" ? SourceGeneratedGenerator::Soft : result.geometry["generator"] == "gauss" ? SourceGeneratedGenerator::Gaussian : SourceGeneratedGenerator::Default;
  generated.shape = result.geometry["shape"] == "rect" ? SourceGeneratedShape::Rectangle : SourceGeneratedShape::Circle;
  const bool complete = result.geometry.contains("diameter") && result.geometry.contains("ratio") && result.geometry.contains("hfade") && result.geometry.contains("vfade") && result.geometry.contains("spikes");
  generated.diameter = result.geometry["diameter"].toDouble(); generated.ratio = result.geometry["ratio"].toDouble();
  generated.horizontalFade = result.geometry["hfade"].toDouble(); generated.verticalFade = result.geometry["vfade"].toDouble();
  if (result.geometry.contains("antialiasEdges")) generated.antialiasEdges = result.geometry["antialiasEdges"].toBool();
  const double spikes = result.geometry["spikes"].toDouble();
  if (generated.generator == SourceGeneratedGenerator::Soft) {
    // Bound encoded size and token count before allocating the split list.
    if (softnessCurve.size() <= 65536 && softnessCurve.count(';') <= 1024) {
      const auto points = softnessCurve.split(';', Qt::SkipEmptyParts);
      if (points.size() >= 2 && points.size() <= 1024)
        for (const auto &point : points) {
          if (point.count(',') != 1) { generated.softnessCurve.clear(); break; }
          const auto coordinates = point.split(',');
          double x, y;
          if (!numeric(coordinates[0], x) || !numeric(coordinates[1], y)) {
            generated.softnessCurve.clear();
            break;
          }
          generated.softnessCurve.push_back({x, y});
        }
    }
  } else if (!softnessCurve.isEmpty()) sourceMaskUnsupported(result, "unrecognized-geometry");
  if (complete && spikes == 2 && validateSourceGeneratedDefinitionV1(generated).empty()) { result.generated = generated; result.tipKind = "generated"; }
  else { result.tipKind = "unsupported"; sourceMaskUnsupported(result, "unprojectable-generated"); }
  return result;
}
}  // namespace Detail
}  // namespace RasterBrush
