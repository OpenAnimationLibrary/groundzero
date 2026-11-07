#pragma once

#include "toonz/rasterbrushsourcemetadata.h"
#include <QCryptographicHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMap>
#include <QRegularExpression>
#include <QSet>
#include <QXmlStreamReader>
#include <cmath>
#include <limits>
#include <stdexcept>

namespace RasterBrush {
namespace Detail {
inline const QStringList &sourceSensorIdsV1() {
  static const QStringList ids{"pressure", "pressurein", "xtilt", "ytilt", "ascension", "declination",
      "rotation", "drawingangle", "time", "distance", "speed", "fuzzy", "fuzzystroke", "fade",
      "perspective", "tangentialpressure"};
  return ids;
}
inline const QStringList &sourceSensorTargetsV1() {
  static const QStringList targets{"Size", "Ratio", "Rotation", "Spacing", "Scatter", "Opacity", "Flow",
      "SmudgeRate", "ColorRate", "SmudgeRadius", "h", "s", "v", "Mirror", "Texture/Strength/"};
  return targets;
}
inline void sourceMetadataRequire(bool condition) {
  if (!condition) throw std::runtime_error("Invalid preserved source sensor metadata");
}
inline bool sourceMetadataNumber(double value) {
  return std::isfinite(value);  // Preservation does not impose active evaluator ranges.
}
inline void sourceMetadataKeys(const QJsonObject &object, const QStringList &keys) {
  sourceMetadataRequire(object.size() == keys.size());
  for (const auto &key : keys) sourceMetadataRequire(object.contains(key));
}
inline QJsonValue sourceSensorCurveJson(const std::optional<std::vector<Knot>> &curve, size_t &knots) {
  if (!curve) return QJsonValue(QJsonValue::Null);
  sourceMetadataRequire(curve->empty() || (curve->size() >= 2 && curve->size() <= 1024));
  knots += curve->size();
  sourceMetadataRequire(knots <= 65536);
  QJsonArray points;
  double previous = -1;
  for (const auto &point : *curve) {
    sourceMetadataRequire(std::isfinite(point.x) && std::isfinite(point.y) && point.x >= 0 &&
        point.x <= 1 && point.y >= 0 && point.y <= 1 && point.x > previous);
    points.append(QJsonArray{point.x, point.y});
    previous = point.x;
  }
  return points;
}
inline std::optional<std::vector<Knot>> sourceSensorCurve(const QJsonValue &value, size_t &knots) {
  if (value.isNull()) return {};
  sourceMetadataRequire(value.isArray());
  const auto points = value.toArray();
  sourceMetadataRequire(points.isEmpty() || (points.size() >= 2 && points.size() <= 1024));
  // Preflight before allocating typed knots, not only in final serialization.
  sourceMetadataRequire(knots <= 65536 && size_t(points.size()) <= 65536 - knots);
  knots += size_t(points.size());
  std::vector<Knot> result;
  result.reserve(size_t(points.size()));
  for (const auto &item : points) {
    sourceMetadataRequire(item.isArray() && item.toArray().size() == 2);
    const auto pair = item.toArray();
    sourceMetadataRequire(pair[0].isDouble() && pair[1].isDouble());
    result.push_back({pair[0].toDouble(), pair[1].toDouble()});
  }
  return result;
}
template <class T> inline QJsonValue sourceMetadataOptional(const std::optional<T> &value) {
  return value ? QJsonValue(*value) : QJsonValue(QJsonValue::Null);
}
inline std::optional<bool> sourceMetadataBoolean(const QJsonValue &value) {
  if (value.isNull()) return {};
  sourceMetadataRequire(value.isBool());
  return value.toBool();
}
inline std::optional<double> sourceMetadataDouble(const QJsonValue &value) {
  if (value.isNull()) return {};
  sourceMetadataRequire(value.isDouble() && sourceMetadataNumber(value.toDouble()));
  return value.toDouble();
}
inline std::optional<int> sourceMetadataInteger(const QJsonValue &value) {
  if (value.isNull()) return {};
  sourceMetadataRequire(value.isDouble() && std::isfinite(value.toDouble()) &&
      value.toDouble() >= std::numeric_limits<int>::min() &&
      value.toDouble() <= std::numeric_limits<int>::max() && value.toDouble() == std::floor(value.toDouble()));
  return value.toInt();
}
inline QString sourceSensorIntervalAttribute(const QString &id) {
  return id == "time" ? "duration" : (id == "distance" || id == "fade") ? "length" : "none";
}
inline QString sourceSensorIntervalUnit(const QString &id) {
  // Preserve source units; no native interval or event-count conversion occurs.
  return id == "time" ? "source-milliseconds" : id == "distance" ? "source-length" :
         id == "fade" ? "source-dab-count" : "none";
}
inline QJsonObject sourceSensorsJson(const SourceSensorsV1 &metadata) {
  sourceMetadataRequire(!metadata.controls.empty() && metadata.controls.size() <= 32);
  QJsonArray controls;
  QSet<QString> targets;
  size_t knots = 0;
  static const QRegularExpression sha256("^[0-9a-f]{64}$");
  const QStringList unsupportedNames{"enabled", "useCurve", "useSameCurve", "strength", "combinationCode", "commonCurve", "sensors"};
  for (const auto &control : metadata.controls) {
    // Check fixed labels before allocating their combined identity string.
    sourceMetadataRequire((control.scope == "primary" || control.scope == "masked") &&
                          sourceSensorTargetsV1().contains(control.target));
    const QString identity = control.scope + ':' + control.target;
    sourceMetadataRequire((control.scope == "primary" || control.scope == "masked") &&
        sourceSensorTargetsV1().contains(control.target) && !targets.contains(identity) &&
        control.sourceFieldsSha256.size() == 64 && sha256.match(control.sourceFieldsSha256).hasMatch() &&
        QStringList{"absent", "single", "list", "unsupported"}.contains(control.layout));
    targets.insert(identity);
    sourceMetadataRequire(!control.strength || sourceMetadataNumber(*control.strength));
    QSet<QString> unsupported;
    for (const auto &field : control.unsupportedFields) {
      sourceMetadataRequire(unsupportedNames.contains(field) && !unsupported.contains(field));
      unsupported.insert(field);
      if (field == "enabled") sourceMetadataRequire(!control.enabled);
      if (field == "useCurve") sourceMetadataRequire(!control.useCurve);
      if (field == "useSameCurve") sourceMetadataRequire(!control.useSameCurve);
      if (field == "strength") sourceMetadataRequire(!control.strength);
      if (field == "combinationCode") sourceMetadataRequire(!control.combinationCode);
      if (field == "commonCurve") sourceMetadataRequire(!control.commonCurve);
    }
    sourceMetadataRequire((control.layout == "single" && control.sensors.size() == 1) ||
        (control.layout == "list" && control.sensors.size() <= 16) ||
        ((control.layout == "absent" || control.layout == "unsupported") && control.sensors.empty()));
    sourceMetadataRequire((control.layout == "unsupported") == unsupported.contains("sensors"));
    QJsonArray sensors;
    QSet<QString> ids;
    for (const auto &sensor : control.sensors) {
      sourceMetadataRequire(sourceSensorIdsV1().contains(sensor.id) && !ids.contains(sensor.id));
      ids.insert(sensor.id);
      const bool interval = sourceSensorIntervalAttribute(sensor.id) != "none";
      sourceMetadataRequire(interval || (!sensor.interval && !sensor.periodic));
      sourceMetadataRequire(!sensor.interval || sourceMetadataNumber(*sensor.interval));
      const bool drawing = sensor.id == "drawingangle";
      sourceMetadataRequire(drawing || (!sensor.fanCornersEnabled && !sensor.fanCornersStep &&
          !sensor.angleOffset && !sensor.lockedAngleMode));
      sourceMetadataRequire((!sensor.fanCornersStep || sourceMetadataNumber(*sensor.fanCornersStep)) &&
          (!sensor.angleOffset || sourceMetadataNumber(*sensor.angleOffset)));
      const auto curve = sourceSensorCurveJson(sensor.curve, knots);
      sensors.append(QJsonObject{{"id", sensor.id}, {"curve", curve},
          {"intervalAttribute", sourceSensorIntervalAttribute(sensor.id)},
          {"intervalUnit", sourceSensorIntervalUnit(sensor.id)},
          {"interval", sourceMetadataOptional(sensor.interval)}, {"periodic", sourceMetadataOptional(sensor.periodic)},
          {"fanCornersEnabled", sourceMetadataOptional(sensor.fanCornersEnabled)},
          {"fanCornersStep", sourceMetadataOptional(sensor.fanCornersStep)},
          {"angleOffset", sourceMetadataOptional(sensor.angleOffset)},
          {"lockedAngleMode", sourceMetadataOptional(sensor.lockedAngleMode)}});
    }
    const auto commonCurve = sourceSensorCurveJson(control.commonCurve, knots);
    controls.append(QJsonObject{{"scope", control.scope}, {"target", control.target},
        {"enabled", sourceMetadataOptional(control.enabled)}, {"useCurve", sourceMetadataOptional(control.useCurve)},
        {"useSameCurve", sourceMetadataOptional(control.useSameCurve)}, {"strength", sourceMetadataOptional(control.strength)},
        {"combinationCode", sourceMetadataOptional(control.combinationCode)},
        {"commonCurve", commonCurve}, {"layout", control.layout},
        {"sourceFieldsSha256", control.sourceFieldsSha256},
        {"unsupportedFields", QJsonArray::fromStringList(control.unsupportedFields)}, {"sensors", sensors}});
  }
  QJsonObject root{{"version", 1}, {"format", "krita-source-sensor-declarations"},
      {"role", "preserved-metadata-only"}, {"curveInterpolation", "not-declared"}, {"controls", controls}};
  sourceMetadataRequire(QJsonDocument(root).toJson(QJsonDocument::Compact).size() <= 8 * 1024 * 1024);
  return root;
}
inline SourceSensorsV1 sourceSensors(const QJsonValue &value) {
  sourceMetadataRequire(value.isObject());
  const auto root = value.toObject();
  sourceMetadataKeys(root, {"version", "format", "role", "curveInterpolation", "controls"});
  sourceMetadataRequire(root["version"].isDouble() && root["version"].toDouble() == 1 &&
      root["format"] == "krita-source-sensor-declarations" && root["role"] == "preserved-metadata-only" &&
      root["curveInterpolation"] == "not-declared" && root["controls"].isArray() &&
      root["controls"].toArray().size() <= 32);
  SourceSensorsV1 result;
  size_t knots = 0;
  for (const auto &item : root["controls"].toArray()) {
    sourceMetadataRequire(item.isObject());
    const auto object = item.toObject();
    sourceMetadataKeys(object, {"scope", "target", "enabled", "useCurve", "useSameCurve", "strength",
        "combinationCode", "commonCurve", "layout", "sourceFieldsSha256", "unsupportedFields", "sensors"});
    for (const auto &key : {"scope", "target", "layout", "sourceFieldsSha256"}) sourceMetadataRequire(object[key].isString());
    SourceSensorControlV1 control;
    control.scope = object["scope"].toString(); control.target = object["target"].toString();
    control.layout = object["layout"].toString(); control.sourceFieldsSha256 = object["sourceFieldsSha256"].toString();
    control.enabled = sourceMetadataBoolean(object["enabled"]); control.useCurve = sourceMetadataBoolean(object["useCurve"]);
    control.useSameCurve = sourceMetadataBoolean(object["useSameCurve"]); control.strength = sourceMetadataDouble(object["strength"]);
    control.combinationCode = sourceMetadataInteger(object["combinationCode"]); control.commonCurve = sourceSensorCurve(object["commonCurve"], knots);
    sourceMetadataRequire(object["unsupportedFields"].isArray() && object["unsupportedFields"].toArray().size() <= 7 &&
        object["sensors"].isArray() && object["sensors"].toArray().size() <= 16);
    for (const auto &field : object["unsupportedFields"].toArray()) {
      sourceMetadataRequire(field.isString()); control.unsupportedFields << field.toString();
    }
    for (const auto &entry : object["sensors"].toArray()) {
      sourceMetadataRequire(entry.isObject());
      const auto sensorObject = entry.toObject();
      sourceMetadataKeys(sensorObject, {"id", "curve", "intervalAttribute", "intervalUnit", "interval", "periodic",
          "fanCornersEnabled", "fanCornersStep", "angleOffset", "lockedAngleMode"});
      sourceMetadataRequire(sensorObject["id"].isString());
      SourceSensorV1 sensor;
      sensor.id = sensorObject["id"].toString();
      sourceMetadataRequire(sensorObject["intervalAttribute"] == sourceSensorIntervalAttribute(sensor.id) &&
          sensorObject["intervalUnit"] == sourceSensorIntervalUnit(sensor.id));
      sensor.curve = sourceSensorCurve(sensorObject["curve"], knots); sensor.interval = sourceMetadataDouble(sensorObject["interval"]);
      sensor.periodic = sourceMetadataBoolean(sensorObject["periodic"]);
      sensor.fanCornersEnabled = sourceMetadataBoolean(sensorObject["fanCornersEnabled"]);
      sensor.fanCornersStep = sourceMetadataDouble(sensorObject["fanCornersStep"]); sensor.angleOffset = sourceMetadataDouble(sensorObject["angleOffset"]);
      sensor.lockedAngleMode = sourceMetadataInteger(sensorObject["lockedAngleMode"]);
      control.sensors.push_back(std::move(sensor));
    }
    result.controls.push_back(std::move(control));
  }
  sourceSensorsJson(result);  // One validator governs programmatic and imported metadata.
  return result;
}

inline bool sourceSensorKnots(const QString &text, std::optional<std::vector<Knot>> &output) {
  if (text.size() > 65536) return false;
  std::vector<Knot> result;
  if (!text.trimmed().isEmpty()) {
    const int separators = text.count(';');
    const int rowsCount = separators + (text.endsWith(';') ? 0 : 1);
    if (separators > 1024 || rowsCount < 2 || rowsCount > 1024) return false;
    auto rows = text.split(';', Qt::KeepEmptyParts);
    if (rows.back().isEmpty()) rows.removeLast();
    if (rows.size() < 2 || rows.size() > 1024) return false;
    double previous = -1;
    for (const auto &row : rows) {
      if (row.count(',') != 1) return false;
      const auto pair = row.split(',');
      if (pair.size() != 2) return false;
      bool xok = false, yok = false;
      const double x = pair[0].toDouble(&xok), y = pair[1].toDouble(&yok);
      if (!xok || !yok || !std::isfinite(x) || !std::isfinite(y) || x < 0 || x > 1 ||
          y < 0 || y > 1 || x <= previous) return false;
      result.push_back({x, y}); previous = x;
    }
  }
  output = std::move(result);
  return true;
}
inline bool sourceSensorXmlNode(QXmlStreamReader &xml, SourceSensorV1 &sensor) {
  if (!xml.namespaceUri().isEmpty() || !xml.namespaceDeclarations().empty()) return false;
  sensor.id = xml.attributes().value("id").toString();
  if (!sourceSensorIdsV1().contains(sensor.id)) return false;
  QSet<QString> allowed{"id"};
  const auto number = [&](const QString &key, std::optional<double> &value) {
    if (!xml.attributes().hasAttribute(key)) return true;
    bool ok = false; const double n = xml.attributes().value(key).toString().toDouble(&ok);
    if (!ok || !sourceMetadataNumber(n)) return false;
    value = n; return true;
  };
  const auto boolean = [&](const QString &key, std::optional<bool> &value) {
    if (!xml.attributes().hasAttribute(key)) return true;
    const auto text = xml.attributes().value(key);
    if (text != QStringLiteral("0") && text != QStringLiteral("1")) return false;
    value = text == QStringLiteral("1"); return true;
  };
  if (sourceSensorIntervalAttribute(sensor.id) != "none") {
    const auto key = sourceSensorIntervalAttribute(sensor.id);
    allowed.insert(key); allowed.insert("periodic");
    if (!number(key, sensor.interval) || !boolean("periodic", sensor.periodic)) return false;
  }
  if (sensor.id == "drawingangle") {
    allowed.unite(QSet<QString>{"fanCornersEnabled", "fanCornersStep", "angleOffset", "lockedAngleMode"});
    if (!boolean("fanCornersEnabled", sensor.fanCornersEnabled) || !number("fanCornersStep", sensor.fanCornersStep) ||
        !number("angleOffset", sensor.angleOffset)) return false;
    if (xml.attributes().hasAttribute("lockedAngleMode")) {
      bool ok = false; const int value = xml.attributes().value("lockedAngleMode").toString().toInt(&ok);
      if (!ok) return false;
      sensor.lockedAngleMode = value;
    }
  }
  for (const auto &attribute : xml.attributes())
    if (!attribute.namespaceUri().isEmpty() || !allowed.contains(attribute.name().toString())) return false;
  bool curve = false;
  while (!xml.atEnd()) {
    xml.readNext();
    if (xml.isEndElement()) return true;
    if (xml.isDTD() || xml.isEntityReference() || (xml.isCharacters() && !xml.isWhitespace())) return false;
    if (!xml.isStartElement()) continue;
    if (curve || xml.name() != QStringLiteral("curve") || !xml.attributes().empty() ||
        !xml.namespaceUri().isEmpty() || !xml.namespaceDeclarations().empty()) return false;
    curve = true;
    if (!sourceSensorKnots(xml.readElementText(QXmlStreamReader::ErrorOnUnexpectedElement), sensor.curve) || xml.hasError()) return false;
  }
  return false;
}
inline bool sourceSensorXml(const QString &text, SourceSensorControlV1 &control) {
  if (text.size() > 65536) return false;
  QXmlStreamReader xml(text);
  bool root = false;
  while (!xml.atEnd()) {
    xml.readNext();
    if (xml.isEntityReference() || (xml.isDTD() && xml.text().toString().trimmed() != "<!DOCTYPE params>") ||
        (xml.isCharacters() && !xml.isWhitespace())) return false;
    if (!xml.isStartElement()) continue;
    if (root || xml.name() != QStringLiteral("params") || !xml.namespaceUri().isEmpty() || !xml.namespaceDeclarations().empty()) return false;
    root = true;
    if (xml.attributes().value("id") != QStringLiteral("sensorslist")) {
      SourceSensorV1 sensor;
      if (!sourceSensorXmlNode(xml, sensor)) return false;
      control.layout = "single"; control.sensors.push_back(std::move(sensor));
    } else {
      if (xml.attributes().size() != 1) return false;
      control.layout = "list";
      QSet<QString> ids;
      bool ended = false;
      while (!xml.atEnd()) {
        xml.readNext();
        if (xml.isEndElement()) { ended = true; break; }
        if (xml.isDTD() || xml.isEntityReference() || (xml.isCharacters() && !xml.isWhitespace())) return false;
        if (!xml.isStartElement()) continue;
        SourceSensorV1 sensor;
        if (xml.name() != QStringLiteral("ChildSensor") || control.sensors.size() >= 16 ||
            !sourceSensorXmlNode(xml, sensor) || ids.contains(sensor.id)) return false;
        ids.insert(sensor.id); control.sensors.push_back(std::move(sensor));
      }
      if (!ended) return false;
    }
  }
  return root && !xml.hasError();
}
inline SourceSensorsV1 preserveKppSourceSensors(const QMap<QString, QString> &fields) {
  SourceSensorsV1 result;
  // Projection is optional metadata: crossing its aggregate quota must not
  // reject an otherwise bounded native fallback. Keep stable scope/target
  // order, charging each common curve before its atomic sensor-list block.
  // Strict programmatic/portable validation keeps its existing hard limit.
  constexpr size_t maximumProjectedKnots = 65536;
  size_t projectedKnots = 0;
  for (const QString scope : {QString("primary"), QString("masked")}) {
    const QString base = scope == "primary" ? "" : "MaskingBrush/Preset/";
    for (const auto &target : sourceSensorTargetsV1()) {
      const QMap<QString, QString> keys{{"enabled", base + "Pressure" + target}, {"useCurve", base + target + "UseCurve"},
          {"useSameCurve", base + target + "UseSameCurve"}, {"strength", base + target + "Value"},
          {"combinationCode", base + target + "curveMode"}, {"commonCurve", base + target + "commonCurve"},
          {"sensors", base + target + "Sensor"}};
      QJsonObject raw;
      for (auto i = keys.begin(); i != keys.end(); ++i)
        if (fields.contains(i.value())) raw[i.key()] = fields[i.value()];
      if (raw.isEmpty()) continue;
      SourceSensorControlV1 control;
      control.scope = scope; control.target = target;
      control.sourceFieldsSha256 = QString::fromLatin1(QCryptographicHash::hash(
          QJsonDocument(raw).toJson(QJsonDocument::Compact), QCryptographicHash::Sha256).toHex());
      const auto boolean = [&](const QString &key, std::optional<bool> &value) {
        if (!raw.contains(key)) return;
        if (raw[key] != "true" && raw[key] != "false") control.unsupportedFields << key;
        else value = raw[key] == "true";
      };
      boolean("enabled", control.enabled); boolean("useCurve", control.useCurve); boolean("useSameCurve", control.useSameCurve);
      if (raw.contains("strength")) {
        bool ok = false; const double value = raw["strength"].toString().toDouble(&ok);
        if (!ok || !sourceMetadataNumber(value)) control.unsupportedFields << "strength";
        else control.strength = value;
      }
      if (raw.contains("combinationCode")) {
        bool ok = false; const int value = raw["combinationCode"].toString().toInt(&ok);
        if (!ok) control.unsupportedFields << "combinationCode";
        else control.combinationCode = value;
      }
      if (raw.contains("commonCurve")) {
        const bool parsed = sourceSensorKnots(raw["commonCurve"].toString(), control.commonCurve);
        const size_t count = control.commonCurve ? control.commonCurve->size() : 0;
        if (!parsed || count > maximumProjectedKnots - projectedKnots) {
          control.commonCurve.reset();
          control.unsupportedFields << "commonCurve";
        } else projectedKnots += count;
      }
      if (raw.contains("sensors")) {
        const bool parsed = sourceSensorXml(raw["sensors"].toString(), control);
        size_t count = 0;
        if (parsed)
          for (const auto &sensor : control.sensors)
            if (sensor.curve) count += sensor.curve->size();
        if (!parsed || count > maximumProjectedKnots - projectedKnots) {
          control.layout = "unsupported";
          control.sensors.clear();
          control.unsupportedFields << "sensors";
        } else projectedKnots += count;
      }
      result.controls.push_back(std::move(control));
    }
  }
  if (!result.controls.empty()) sourceSensorsJson(result);
  return result;
}
}  // namespace Detail
}  // namespace RasterBrush
