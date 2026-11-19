#include "automation_scene_host.h"

#include "toonz/toonzscene.h"
#include "toonz/tstageobject.h"
#include "toonz/tstageobjecttree.h"
#include "toonz/txshchildlevel.h"
#include "toonz/txshcolumn.h"
#include "toonz/txsheet.h"

#include <QJsonDocument>

#include <algorithm>

namespace GroundzeroAutomation {
namespace {

constexpr int MaxRows     = 65536;
constexpr int MaxPage     = 128;
constexpr int MaxResponse = 1048576;

// One instance of a sheet: the top sheet, or a child sheet reached through a
// path of exposing columns.
struct Instance final {
  TXsheet* sheet;
  QJsonArray path;
  int depth;
  TXshLevel* level;
};

struct Walk final {
  Document& document;
  std::vector<Instance> instances;
  std::vector<TXsheet*> stack;
  QStringList reasons;

  void incomplete(const QString& reason) {
    if (!reasons.contains(reason)) reasons.append(reason);
  }
};

void walkSheet(Walk& walk, const Instance& instance);

void walkChildren(Walk& walk, const Instance& instance) {
  for (int index = 0; index < instance.sheet->getColumnCount(); ++index) {
    TXshColumn* column = instance.sheet->getColumn(index);
    for (TXshLevel* level : columnLevels(column)) {
      TXshChildLevel* child = level->getChildLevel();
      if (!child || !child->getXsheet()) continue;
      if (instance.depth + 1 > SheetDepthLimit) {
        walk.incomplete("depth_limit");
        continue;
      }
      if (std::count(walk.stack.begin(), walk.stack.end(),
                     child->getXsheet())) {
        walk.incomplete("cycle");
        continue;
      }
      QJsonArray path = instance.path;
      path.append(QJsonObject{{"column_id", walk.document.idForColumn(column)},
                              {"level_id", walk.document.idForLevel(level)}});
      walkSheet(walk, {child->getXsheet(), path, instance.depth + 1, level});
    }
  }
}

void walkSheet(Walk& walk, const Instance& instance) {
  if (int(walk.instances.size()) >= MaxRows) {
    walk.incomplete("row_limit");
    return;
  }
  walk.instances.push_back(instance);
  walk.stack.push_back(instance.sheet);
  walkChildren(walk, instance);
  walk.stack.pop_back();
}

Walk walkDocument(Document& document) {
  Walk walk{document, {}, {}, {}};
  walkSheet(walk, {document.scene->getTopXsheet(), {}, 0, nullptr});
  return walk;
}

QString columnKind(TXshColumn* column) {
  if (column->getSoundColumn()) return "sound";
  if (column->getSoundTextColumn()) return "sound_text";
  if (column->getPaletteColumn()) return "palette";
  if (column->getMeshColumn()) return "mesh";
  if (column->getZeraryFxColumn()) return "generator";
  if (!column->getLevelColumn()) return "unknown";
  if (column->isEmpty()) return "empty";
  for (TXshLevel* level : columnLevels(column))
    if (level->getChildLevel()) return "child";
  return "level";
}

QString columnName(TXsheet* sheet, int index) {
  TStageObject* object = sheet->getStageObjectTree()->getStageObject(
      TStageObjectId::ColumnId(index), false);
  return object ? QString::fromStdString(object->getName())
                : QString("Col%1").arg(index + 1);
}

QJsonObject columnRow(Walk& walk, const Instance& instance, int index) {
  TXshColumn* column = instance.sheet->getColumn(index);
  QJsonArray levels;
  const std::vector<TXshLevel*> exposed = columnLevels(column);
  for (TXshLevel* level : exposed)
    if (levels.size() < 16) levels.append(walk.document.idForLevel(level));
  int first = 0, last = -1;
  column->getRange(first, last);
  const bool empty = column->isEmpty() || last < first;
  return {{"column_id", walk.document.idForColumn(column)},
          {"sheet_path", instance.path},
          {"index", index},
          {"kind", columnKind(column)},
          {"name", columnName(instance.sheet, index)},
          {"preview_visible", column->isPreviewVisible()},
          {"camstand_visible", column->isCamstandVisible()},
          {"locked", column->isLocked()},
          {"opacity", column->getOpacity() / 255.0},
          {"first_frame", empty ? QJsonValue() : QJsonValue(first)},
          {"last_frame", empty ? QJsonValue() : QJsonValue(last)},
          {"level_ids", levels},
          {"level_ids_truncated", exposed.size() > 16},
          {"duplicate_name", false}};
}

QString objectKind(const TStageObjectId& id) {
  if (id.isColumn()) return "column";
  if (id.isCamera()) return "camera";
  if (id.isPegbar()) return "pegbar";
  if (id.isTable()) return "table";
  return "unknown";
}

QJsonObject objectRow(Walk& walk, const Instance& instance,
                      TStageObject* object) {
  TStageObjectTree* tree = instance.sheet->getStageObjectTree();
  TStageObject* parent   = tree->getStageObject(object->getParent(), false);
  return {{"object_id", walk.document.idForObject(object)},
          {"sheet_path", instance.path},
          {"kind", objectKind(object->getId())},
          {"name", QString::fromStdString(object->getName())},
          {"parent_id", parent ? QJsonValue(walk.document.idForObject(parent))
                               : QJsonValue()},
          {"locked", object->isLocked()},
          {"is_current_camera", object->getId() == tree->getCurrentCameraId()},
          {"duplicate_name", false}};
}

// Mark rows whose name repeats another row of the same sheet instance.
void markDuplicates(QJsonArray& rows, int first) {
  QHash<QString, int> counts;
  for (int index = first; index < rows.size(); ++index)
    ++counts[rows.at(index).toObject().value("name").toString()];
  for (int index = first; index < rows.size(); ++index) {
    QJsonObject row = rows.at(index).toObject();
    if (counts.value(row.value("name").toString()) < 2) continue;
    row.insert("duplicate_name", true);
    rows.replace(index, row);
  }
}

bool full(Walk& walk, const QJsonArray& rows) {
  if (rows.size() < MaxRows) return false;
  walk.incomplete("row_limit");
  return true;
}

void sheetRows(Walk& walk, const std::vector<const Instance*>& chosen,
               QJsonArray& rows) {
  QHash<TXshLevel*, int> instances;
  for (const Instance& instance : walk.instances) ++instances[instance.level];
  for (const Instance* instance : chosen) {
    if (full(walk, rows)) return;
    rows.append(QJsonObject{
        {"sheet_path", instance->path},
        {"level_id", instance->level
                         ? QJsonValue(walk.document.idForLevel(instance->level))
                         : QJsonValue()},
        {"name", instance->level ? QJsonValue(QString::fromStdWString(
                                       instance->level->getName()))
                                 : QJsonValue()},
        {"depth", instance->depth},
        {"column_count", instance->sheet->getColumnCount()},
        {"frame_count", instance->sheet->getFrameCount()},
        {"instance_count", instances.value(instance->level)}});
  }
}

void columnRows(Walk& walk, const std::vector<const Instance*>& chosen,
                QJsonArray& rows) {
  for (const Instance* instance : chosen) {
    const int first = rows.size();
    for (int index = 0; index < instance->sheet->getColumnCount(); ++index) {
      if (!instance->sheet->getColumn(index)) continue;
      if (full(walk, rows)) return;
      rows.append(columnRow(walk, *instance, index));
    }
    markDuplicates(rows, first);
  }
}

void objectRows(Walk& walk, const std::vector<const Instance*>& chosen,
                QJsonArray& rows) {
  for (const Instance* instance : chosen) {
    const int first        = rows.size();
    TStageObjectTree* tree = instance->sheet->getStageObjectTree();
    for (int index = 0; index < tree->getStageObjectCount(); ++index) {
      if (full(walk, rows)) return;
      rows.append(objectRow(walk, *instance, tree->getStageObject(index)));
    }
    markDuplicates(rows, first);
  }
}

bool samePath(const QJsonArray& path, const QJsonArray& prefix, bool exact) {
  if (path.size() < prefix.size() || (exact && path.size() != prefix.size()))
    return false;
  for (int index = 0; index < prefix.size(); ++index)
    if (path.at(index) != prefix.at(index)) return false;
  return true;
}

// The instances a request covers: every instance, or the named one (and, for
// the sheets section, the instances below it).
std::vector<const Instance*> chosenInstances(const Walk& walk,
                                             const QJsonObject& params,
                                             bool subtree) {
  std::vector<const Instance*> chosen;
  const bool named      = params.contains("sheet_path");
  const QJsonArray path = params.value("sheet_path").toArray();
  bool found            = !named;
  for (const Instance& instance : walk.instances) {
    if (named && samePath(instance.path, path, true)) found = true;
    if (!named || samePath(instance.path, path, !subtree))
      chosen.push_back(&instance);
  }
  if (!found)
    throw Error("invalid_handle", "No sheet instance has that sheet_path.",
                {{"sheet_path", path}});
  return chosen;
}

QJsonObject inventoryPage(Walk& walk, const QJsonObject& params,
                          const QJsonArray& rows) {
  const int offset = integerParam(params, "offset", 0, MaxRows, 0);
  const int limit  = integerParam(params, "limit", 1, MaxPage, 32);
  if (offset > rows.size())
    throw Error("invalid_params", "offset exceeds the discovered rows.");
  const int end = std::min(offset + limit, int(rows.size()));
  QJsonArray items;
  for (int index = offset; index < end; ++index) items.append(rows.at(index));
  QJsonObject result{
      {"document_id", walk.document.id},
      {"source_revision", walk.document.revisionText()},
      {"section", params.value("section")},
      {"items", items},
      {"offset", offset},
      {"limit", limit},
      {"discovered", int(rows.size())},
      {"next_offset", end < rows.size() ? QJsonValue(end) : QJsonValue()},
      {"traversal_complete", walk.reasons.isEmpty()},
      {"reasons", QJsonArray::fromStringList(walk.reasons)}};
  const QJsonObject envelope{{"id", QString(128, QChar(1))},
                             {"result", result}};
  if (QJsonDocument(envelope).toJson(QJsonDocument::Compact).size() + 1 >
      MaxResponse)
    throw Error("response_too_large",
                "The inventory page exceeds one MiB; request a smaller limit.");
  return result;
}

QJsonValue inventory(Context& context, const QJsonObject& params) {
  Document& document = context.document(requiredString(params, "document_id"));
  if (requiredString(params, "expected_revision") != document.revisionText())
    throw Error(
        "stale_revision", "Refresh scene.summary before continuing.",
        {{"document_id", document.id}, {"revision", document.revisionText()}});
  Walk walk             = walkDocument(document);
  const QString section = requiredString(params, "section");
  const auto chosen     = chosenInstances(walk, params, section == "sheets");
  QJsonArray rows;
  if (section == "sheets") sheetRows(walk, chosen, rows);
  if (section == "columns") columnRows(walk, chosen, rows);
  if (section == "stage_objects") objectRows(walk, chosen, rows);
  return inventoryPage(walk, params, rows);
}

QJsonObject stepSchema() {
  const QJsonObject handle{
      {"type", "string"}, {"minLength", 1}, {"maxLength", 128}};
  return {
      {"type", "object"},
      {"additionalProperties", false},
      {"properties", QJsonObject{{"column_id", handle}, {"level_id", handle}}},
      {"required", QJsonArray{"column_id", "level_id"}}};
}

}  // namespace

TXsheet* resolveSheetPath(Document& document, const QJsonArray& path) {
  TXsheet* sheet = document.scene->getTopXsheet();
  for (const QJsonValue& value : path) {
    const QJsonObject step = value.toObject();
    TXshColumn* column     = nullptr;
    TXshLevel* level       = nullptr;
    for (int index = 0; index < sheet->getColumnCount() && !level; ++index) {
      TXshColumn* candidate = sheet->getColumn(index);
      if (!candidate || document.columns.value(
                            step.value("column_id").toString()) != candidate)
        continue;
      column = candidate;
      for (TXshLevel* exposed : columnLevels(column))
        if (exposed->getChildLevel() &&
            document.levels.value(step.value("level_id").toString()) == exposed)
          level = exposed;
    }
    if (!column || !level)
      throw Error("invalid_handle",
                  "The sheet_path does not name an instance of this scene.",
                  {{"sheet_path", path}});
    sheet = level->getChildLevel()->getXsheet();
  }
  return sheet;
}

QJsonObject sheetPathSchema() {
  return {{"type", "array"},
          {"items", stepSchema()},
          {"minItems", 0},
          {"maxItems", SheetDepthLimit}};
}

QJsonArray sceneInventoryOperations() {
  const QJsonObject handle{
      {"type", "string"}, {"minLength", 1}, {"maxLength", 128}};
  return {operation(
      "scene.inventory",
      "Page through a sheet-aware scene inventory: sheets, columns or stage "
      "objects. Rows carry sheet_path (the instance path from the top sheet) "
      "and the child level_id (the shared definition). Works on editable and "
      "read-only documents; never creates objects.",
      {{"document_id", handle},
       {"expected_revision",
        QJsonObject{{"type", "string"}, {"minLength", 1}, {"maxLength", 80}}},
       {"section", QJsonObject{{"type", "string"},
                               {"enum", QJsonArray{"sheets", "columns",
                                                   "stage_objects"}}}},
       {"sheet_path", sheetPathSchema()},
       {"offset",
        QJsonObject{{"type", "integer"}, {"minimum", 0}, {"maximum", MaxRows}}},
       {"limit",
        QJsonObject{
            {"type", "integer"}, {"minimum", 1}, {"maximum", MaxPage}}}},
      {"document_id", "expected_revision", "section"})};
}

QHash<TXshLevel*, QJsonArray> levelSheetPaths(Document& document,
                                              QStringList& reasons) {
  const Walk walk = walkDocument(document);
  reasons         = walk.reasons;
  QHash<TXshLevel*, QJsonArray> paths;
  for (const Instance& instance : walk.instances)
    for (int index = 0; index < instance.sheet->getColumnCount(); ++index)
      for (TXshLevel* level : columnLevels(instance.sheet->getColumn(index)))
        if (!paths.contains(level)) paths.insert(level, instance.path);
  return paths;
}

QHash<TXsheet*, QJsonArray> sheetPaths(Document& document,
                                       QStringList& reasons) {
  const Walk walk = walkDocument(document);
  reasons         = walk.reasons;
  QHash<TXsheet*, QJsonArray> paths;
  for (const Instance& instance : walk.instances)
    if (!paths.contains(instance.sheet))
      paths.insert(instance.sheet, instance.path);
  return paths;
}

QJsonValue executeSceneInventory(Context& context, const QString& method,
                                 const QJsonObject& params) {
  if (method != "scene.inventory")
    throw Error("method_not_found", "Unknown inventory operation.");
  return inventory(context, params);
}

}  // namespace GroundzeroAutomation
