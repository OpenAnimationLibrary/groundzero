#include "automation_path.h"
#include "automation_scene_shared.h"

#include "toonz/fxdag.h"
#include "toonz/toonzscene.h"
#include "toonz/tstageobject.h"
#include "toonz/tstageobjecttree.h"
#include "toonz/txsheet.h"
#include "toonz/txshlevelcolumn.h"

#include <cmath>

namespace GroundzeroAutomation {
namespace SceneShared {
namespace {

QString columnKind(TXshColumn* column) {
  if (column->getSoundColumn()) return "sound";
  if (column->getZeraryFxColumn()) return "generator";
  return "level";
}

}  // namespace

QJsonObject describeColumn(Document& document, TXshColumn* column) {
  const int index = columnIndex(document, column);
  // A user scene can hold a column without a stage object; reading never
  // creates one.
  TStageObject* object =
      tree(document)->getStageObject(TStageObjectId::ColumnId(index), false);
  return {{"column_id", document.idForColumn(column)},
          {"kind", columnKind(column)},
          {"object_id",
           object ? QJsonValue(document.idForObject(object)) : QJsonValue()},
          {"position", index},
          {"name", object ? QString::fromStdString(object->getName())
                          : QString("Col%1").arg(index + 1)},
          {"visible", column->isPreviewVisible()},
          {"locked", column->isLocked()},
          {"opacity", column->getOpacity() / 255.0},
          {"last_frame", column->getMaxFrame()}};
}

namespace {

QJsonValue createColumn(Context& context, const QJsonObject& params) {
  Document& document = getDocument(context, params);
  requireAutomationPathObjectGrowth(*document.scene);
  const QString name = nameParam(params);
  TXsheet* sheet     = document.scene->getXsheet();
  const int index    = sheet->getColumnCount();
  if (index >= MaxEntities)
    throw Error("resource_limit", "The document already has 4096 columns.");
  context.markMutationStarted();
  sheet->insertColumn(index, new TXshLevelColumn());
  TXshColumn* column = sheet->getColumn(index);
  column->setPreviewVisible(true);
  column->setCamstandVisible(true);
  sheet->getStageObject(TStageObjectId::ColumnId(index))
      ->setName(name.toStdString());
  return describeColumn(document, column);
}

bool changesColumnContent(const QJsonObject& params) {
  return params.contains("name") || params.contains("visible") ||
         params.contains("opacity");
}

bool matchesColumn(TXshColumn* column, TStageObject* object,
                   const QJsonObject& value) {
  return value.value("name").toString().toStdString() == object->getName() &&
         value.value("visible").toBool() == column->isPreviewVisible() &&
         value.value("visible").toBool() == column->isCamstandVisible() &&
         value.value("locked").toBool() == column->isLocked() &&
         value.value("alpha").toInt() == column->getOpacity();
}

QJsonValue updateColumn(Context& context, const QJsonObject& params) {
  Document& document   = getDocument(context, params);
  TXshColumn* column   = document.column(requiredString(params, "column_id"));
  TStageObject* object = tree(document)->getStageObject(
      TStageObjectId::ColumnId(columnIndex(document, column)), false);
  const QString name = params.contains("name")
                           ? nameParam(params)
                           : QString::fromStdString(object->getName());
  const bool visible = boolParam(params, "visible", column->isPreviewVisible());
  const bool locked  = boolParam(params, "locked", column->isLocked());
  const double opacity =
      numberParam(params, "opacity", 0, 1, column->getOpacity() / 255.0);
  if (column->getSoundColumn() && params.contains("opacity"))
    throw Error("invalid_params", "Sound columns have no opacity.");
  if (changesColumnContent(params)) requireEditable(document, object);
  const UCHAR alpha = static_cast<UCHAR>(std::lround(opacity * 255));
  const QJsonObject updated{{"name", name},
                            {"visible", visible},
                            {"locked", locked},
                            {"alpha", alpha}};
  if (matchesColumn(column, object, updated))
    return describeColumn(document, column);
  context.markMutationStarted();
  object->setName(name.toStdString());
  column->setPreviewVisible(visible);
  column->setCamstandVisible(visible);
  column->setOpacity(alpha);
  column->lock(locked);
  return describeColumn(document, column);
}

QJsonValue deleteColumn(Context& context, const QJsonObject& params) {
  Document& document = getDocument(context, params);
  const QString id   = requiredString(params, "column_id");
  TXshColumn* column = document.column(id);
  if (column->getColumnType() != TXshColumn::eLevelType &&
      column->getColumnType() != TXshColumn::eSoundType)
    throw Error("incompatible_column",
                "Use the owning subsystem to delete this column type.");
  const int index = columnIndex(document, column);
  TStageObject* object =
      tree(document)->getStageObject(TStageObjectId::ColumnId(index), false);
  requireEditable(document, object);
  if (object->hasChildren())
    throw Error("has_dependents",
                "Reparent the column's children before deleting it.");
  const QString objectId = document.idForObject(object);
  TFx* fx                = column->getFx();
  context.markMutationStarted();
  for (auto entry = document.effects.begin();
       entry != document.effects.end();) {
    if (entry.value() == fx)
      entry = document.effects.erase(entry);
    else
      ++entry;
  }
  if (fx) document.scene->getXsheet()->getFxDag()->removeFxId(fx);
  document.columns.remove(id);
  document.objects.remove(objectId);
  document.scene->getXsheet()->removeColumn(index);
  return QJsonObject{{"column_id", id}, {"deleted", true}};
}

}  // namespace

void addColumnOperations(QJsonArray& catalog) {
  QJsonObject column = merge(documentSchema(), {{"column_id", textSchema()}});
  catalog.append(operation("column.create",
                           "Append an empty level column with a stable handle.",
                           merge(documentSchema(), {{"name", textSchema()}}),
                           {"document_id", "name"}, true));
  QJsonObject update =
      merge(column, {{"name", textSchema()},
                     {"visible", QJsonObject{{"type", "boolean"}}},
                     {"locked", QJsonObject{{"type", "boolean"}}},
                     {"opacity", numberSchema(0, 1)}});
  catalog.append(operation("column.update",
                           "Set column name, visibility, lock or opacity.",
                           update, {"document_id", "column_id"}, true));
  catalog.append(operation("column.delete",
                           "Delete an unlocked column without stage children.",
                           column, {"document_id", "column_id"}, true));
}

void addColumnHandlers(SceneHandlers& handlers) {
  handlers.insert({{"column.create", createColumn},
                   {"column.update", updateColumn},
                   {"column.delete", deleteColumn}});
}

}  // namespace SceneShared
}  // namespace GroundzeroAutomation
