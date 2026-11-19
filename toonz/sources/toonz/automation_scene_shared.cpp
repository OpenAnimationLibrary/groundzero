#include "automation_scene_shared.h"

#include "tfilepath.h"
#include "toonz/toonzscene.h"
#include "toonz/tstageobject.h"
#include "toonz/tstageobjecttree.h"
#include "toonz/txshcolumn.h"
#include "toonz/txsheet.h"

#include <cmath>

namespace GroundzeroAutomation {
namespace SceneShared {

QJsonObject textSchema() { return {{"type", "string"}, {"minLength", 1}}; }

QJsonObject numberSchema(double minimum, double maximum) {
  return {{"type", "number"}, {"minimum", minimum}, {"maximum", maximum}};
}

QJsonObject integerSchema(int minimum, int maximum) {
  return {{"type", "integer"}, {"minimum", minimum}, {"maximum", maximum}};
}

QJsonObject enumSchema(const QJsonArray& values) {
  return {{"type", "string"}, {"enum", values}};
}

QJsonObject documentSchema() { return {{"document_id", textSchema()}}; }

QJsonObject merge(QJsonObject target, const QJsonObject& source) {
  for (auto item = source.begin(); item != source.end(); ++item)
    target.insert(item.key(), item.value());
  return target;
}

QJsonObject cameraSchema() {
  return {{"width_px", integerSchema(1, 8192)},
          {"height_px", integerSchema(1, 8192)},
          {"width_in", numberSchema(0.001, 10000)},
          {"height_in", numberSchema(0.001, 10000)}};
}

double numberParam(const QJsonObject& params, const QString& key,
                   double minimum, double maximum, double fallback) {
  if (!params.contains(key)) return fallback;
  const QJsonValue value = params.value(key);
  const double number    = value.toDouble();
  if (!value.isDouble() || !std::isfinite(number) || number < minimum ||
      number > maximum)
    throw Error("invalid_argument",
                key + " must be a finite number within its stated limits.");
  return number;
}

bool boolParam(const QJsonObject& params, const QString& key, bool fallback) {
  if (!params.contains(key)) return fallback;
  if (!params.value(key).isBool())
    throw Error("invalid_argument", key + " must be a boolean.");
  return params.value(key).toBool();
}

QString nameParam(const QJsonObject& params) {
  const QString name = requiredString(params, "name");
  if (name.toUcs4().size() > 128 || name.contains(QChar::Null))
    throw Error("invalid_argument",
                "name must contain 1 to 128 characters without NUL.");
  return name;
}

Document& getDocument(Context& context, const QJsonObject& params) {
  return context.document(requiredString(params, "document_id"));
}

TStageObjectTree* tree(Document& document) {
  return document.scene->getXsheet()->getStageObjectTree();
}

int columnIndex(Document& document, TXshColumn* column) {
  TXsheet* sheet = document.scene->getXsheet();
  for (int index = 0; index < sheet->getColumnCount(); ++index)
    if (sheet->getColumn(index) == column) return index;
  throw Error("stale_handle", "The column no longer belongs to this document.");
}

void requireEditable(Document& document, TStageObject* object) {
  bool locked = object->isLocked();
  if (object->getId().isColumn()) {
    TXshColumn* column =
        document.scene->getXsheet()->getColumn(object->getId().getIndex());
    locked = locked || (column && column->isLocked());
  }
  if (locked) throw Error("locked", "Unlock the target before modifying it.");
}

QJsonObject drawingId(const TFrameId& frame) {
  return {{"number", frame.getNumber()}, {"suffix", frame.getLetter()}};
}

TFrameId readDrawingId(const QJsonObject& params) {
  if (!params.value("drawing_id").isObject())
    throw Error("invalid_argument",
                "drawing_id must contain number and suffix.");
  const QJsonObject id = params.value("drawing_id").toObject();
  const int number     = integerParam(id, "number", 1, 999999, -1);
  if (number < 1 || !id.value("suffix").isString())
    throw Error("invalid_argument",
                "drawing_id requires a positive number and string suffix.");
  const QString suffix = id.value("suffix").toString();
  if (suffix.size() > 8)
    throw Error("invalid_argument",
                "drawing_id suffix is limited to eight ASCII letters.");
  for (const QChar letter : suffix)
    if (letter < QLatin1Char('a') || letter > QLatin1Char('z'))
      throw Error("invalid_argument",
                  "drawing_id suffix must contain lowercase ASCII letters.");
  return TFrameId(number, suffix);
}

}  // namespace SceneShared
}  // namespace GroundzeroAutomation
