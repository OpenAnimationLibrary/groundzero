#pragma once
#ifndef TOONZ_MORPH_HINTS_H
#define TOONZ_MORPH_HINTS_H

#include "tvectorimage.h"
#include "tstroke.h"
#include "tfilepath.h"
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <cmath>
#include <map>

// Scene metadata, not PLI geometry. Unknown <info> attributes are ignored by
// older applications; opening and saving there drops only authoring hints.
namespace MorphHints {
constexpr int MaxRecords = 2048;
constexpr int MaxBytes = 2 * 1024 * 1024;
inline QJsonObject frame(const TFrameId &fid) {
  return {{"number", fid.getNumber()}, {"letter", fid.getLetter()}};
}
inline TFrameId frame(const QJsonObject &o) {
  return TFrameId(o.value("number").toInt(), o.value("letter").toString());
}
inline QJsonObject anchor(const TVectorImageP &image, int index) {
  if (!image || index < 0 || index >= int(image->getStrokeCount())) return {};
  const TStroke *stroke = image->getStroke(index);
  QJsonArray samples;
  for (int i = 0; i <= 8; ++i) {
    const TThickPoint p = stroke->getThickPoint(double(i) / 8);
    samples.append(QJsonArray{p.x, p.y, p.thick});
  }
  return {{"index", index}, {"points", stroke->getControlPointCount()},
          {"style", stroke->getStyle()}, {"loop", stroke->isSelfLoop()},
          {"samples", samples}};
}
inline bool matches(const QJsonObject &saved, const TStroke *stroke) {
  if (!stroke || saved.value("points").toInt() != stroke->getControlPointCount() ||
      saved.value("style").toInt() != stroke->getStyle() ||
      saved.value("loop").toBool() != stroke->isSelfLoop()) return false;
  const QJsonArray samples = saved.value("samples").toArray();
  if (samples.size() != 9) return false;
  // PLI uses fixed-point coordinates. Permit its quantization, not a changed
  // drawing. Ambiguous candidates are rejected by resolve().
  constexpr double tolerance = 1.0 / 128.0;
  for (int i = 0; i <= 8; ++i) {
    const auto sample = samples[i].toArray();
    if (sample.size() != 3) return false;
    const TThickPoint p = stroke->getThickPoint(double(i) / 8);
    const double values[] = {p.x, p.y, p.thick};
    for (int j = 0; j < 3; ++j)
      if (!sample[j].isDouble() || !std::isfinite(sample[j].toDouble()) ||
          std::abs(sample[j].toDouble() - values[j]) > tolerance) return false;
  }
  return true;
}
inline int resolve(const QJsonObject &saved, const TVectorImageP &image) {
  if (!image) return -1;
  int candidate = -1;
  for (int i = 0; i < int(image->getStrokeCount()); ++i) {
    if (!matches(saved, image->getStroke(i))) continue;
    if (candidate >= 0) return -1; // Do not silently guess identical strokes.
    candidate = i;
  }
  return candidate;
}
inline QJsonArray rect(const TRectD &r) { return {r.x0, r.y0, r.x1, r.y1}; }
inline TRectD rect(const QJsonArray &a) {
  return TRectD(a[0].toDouble(), a[1].toDouble(), a[2].toDouble(), a[3].toDouble());
}
inline bool validRect(const QJsonValue &value) {
  const auto a = value.toArray();
  if (a.size() != 4) return false;
  for (const auto &v : a)
    if (!v.isDouble() || !std::isfinite(v.toDouble())) return false;
  return a[0].toDouble() <= a[2].toDouble() && a[1].toDouble() <= a[3].toDouble();
}
inline bool validAnchor(const QJsonValue &value) {
  if (!value.isObject()) return false;
  const auto a = value.toObject();
  for (const char *key : {"index", "points", "style"}) {
    const auto v = a.value(key);
    if (!v.isDouble() || !std::isfinite(v.toDouble()) ||
        v.toDouble() != v.toInt(-1) || v.toInt(-1) < 0) return false;
  }
  if (a.value("points").toInt() < 3 || !a.value("loop").isBool()) return false;
  const auto samples = a.value("samples").toArray();
  if (samples.size() != 9) return false;
  for (const auto &point : samples) {
    const auto p = point.toArray();
    if (p.size() != 3) return false;
    for (const auto &coordinate : p)
      if (!coordinate.isDouble() || !std::isfinite(coordinate.toDouble())) return false;
  }
  return true;
}
inline bool decode(const std::string &text, QJsonArray &records) {
  records = {};
  if (text.empty()) return true;
  if (text.size() > MaxBytes) return false;
  QJsonParseError error;
  auto doc = QJsonDocument::fromJson(QByteArray::fromStdString(text), &error);
  if (error.error != QJsonParseError::NoError || !doc.isObject()) return false;
  const auto root = doc.object();
  if (!root.value("version").isDouble() || root.value("version").toDouble() != 1. ||
      !root.value("records").isArray()) return false;
  const auto items = root.value("records").toArray();
  if (items.size() > MaxRecords) return false;
  for (const auto &item : items) {
    if (!item.isObject()) return false;
    const auto o = item.toObject();
    const QString kind = o.value("kind").toString();
    if (kind != "pair" && kind != "zone") return false;
    for (const char *key : {"firstFrame", "lastFrame"}) {
      auto f = o.value(key).toObject();
      if (!f.value("number").isDouble() || !f.value("letter").isString() ||
          !std::isfinite(f.value("number").toDouble()) ||
          f.value("number").toDouble() != f.value("number").toInt() ||
          f.value("letter").toString().size() > 16) return false;
    }
    for (const char *key : {"first", "last"}) {
      if (!validAnchor(o.value(key))) return false;
    }
    if (kind == "zone" && (!validRect(o.value("firstRect")) ||
                           !validRect(o.value("lastRect")))) return false;
    for (const char *key : {"firstMembers", "lastMembers"}) {
      if (!o.contains(key)) continue;
      if (!o.value(key).isArray() || o.value(key).toArray().isEmpty()) return false;
      for (const auto &anchor : o.value(key).toArray())
        if (!validAnchor(anchor)) return false;
    }
  }
  records = items;
  return true;
}
inline std::string encode(const QJsonArray &records) {
  if (records.isEmpty()) return {};
  return QJsonDocument(QJsonObject{{"version", 1}, {"records", records}})
      .toJson(QJsonDocument::Compact).toStdString();
}
inline std::string renumber(const std::string &text,
                           const std::map<TFrameId, TFrameId> &table) {
  QJsonArray records;
  if (!decode(text, records)) return text; // Preserve unknown future metadata.
  QJsonArray result;
  for (const auto &value : records) {
    auto record = value.toObject();
    bool valid = true;
    for (const char *key : {"firstFrame", "lastFrame"}) {
      auto found = table.find(frame(record.value(key).toObject()));
      if (found == table.end()) { valid = false; break; }
      record[key] = frame(found->second);
    }
    if (valid) result.append(record);
  }
  return encode(result);
}
} // namespace MorphHints
#endif
