#include "automation_path.h"
#include "automation_scene_shared.h"

#include "automation_rig.h"
#include "automation_scene_host.h"

#include "tfx.h"
#include "toonz/fxdag.h"
#include "toonz/sceneproperties.h"
#include "toonz/tcolumnfx.h"
#include "toonz/tcolumnfxset.h"
#include "toonz/toonzscene.h"
#include "toonz/tstageobject.h"
#include "toonz/tstageobjecttree.h"
#include "toonz/txshcell.h"
#include "toonz/txsheet.h"
#include "toonz/txshlevelcolumn.h"

#include <cmath>
#include <algorithm>
#include <map>
#include <memory>
#include <set>
#include <vector>

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
          {"preview_visible", column->isPreviewVisible()},
          {"camstand_visible", column->isCamstandVisible()},
          {"color_filter_id", column->getColorFilterId()},
          {"locked", column->isLocked()},
          {"opacity", column->getOpacity() / 255.0},
          {"last_frame", column->getMaxFrame()}};
}

namespace {

constexpr int MaxReported = 64;

void requireTopSheet(const QJsonObject& params) {
  if (!params.value("sheet_path").toArray().isEmpty())
    throw Error("unsupported_target",
                "Column tools edit the top sheet; nested sheets are not "
                "editable yet.",
                {{"reason", "nested_sheet_editing"}});
}

TXsheet* sheetOf(Document& document) { return document.scene->getXsheet(); }

TStageObject* columnObject(Document& document, int index) {
  return tree(document)->getStageObject(TStageObjectId::ColumnId(index), false);
}

int readPosition(const QJsonObject& params, const QString& key, int fallback,
                 int count) {
  const int position =
      params.contains(key) ? params.value(key).toInt() : fallback;
  if (position < 0 || position > count)
    throw Error("invalid_params", key + " lies past the last column.",
                {{"reason", "position"}, {"column_count", count}});
  return position;
}

QJsonValue createColumn(Context& context, const QJsonObject& params) {
  Document& document = getDocument(context, params);
  requireTopSheet(params);
  requireAutomationPathObjectGrowth(*document.scene);
  const QString name = nameParam(params);
  TXsheet* sheet     = sheetOf(document);
  const int count    = sheet->getColumnCount();
  if (count >= MaxEntities)
    throw Error("resource_limit", "The document already has 4096 columns.");
  const int index = readPosition(params, "position", count, count);
  context.markMutationStarted();
  // Later column objects are renumbered with their pointers, so their
  // handles follow (TStageObjectTree::insertColumn).
  sheet->insertColumn(index, new TXshLevelColumn());
  TXshColumn* column = sheet->getColumn(index);
  column->setPreviewVisible(true);
  column->setCamstandVisible(true);
  sheet->getStageObject(TStageObjectId::ColumnId(index))
      ->setName(name.toStdString());
  return describeColumn(document, column);
}

// --------------------------------------------------------------- update

struct ColumnState {
  std::string name;
  bool preview, camstand, locked;
  UCHAR alpha;
  int filter;
};

ColumnState currentState(TXshColumn* column, TStageObject* object) {
  return {object->getName(),           column->isPreviewVisible(),
          column->isCamstandVisible(), column->isLocked(),
          column->getOpacity(),        column->getColorFilterId()};
}

bool sameState(const ColumnState& left, const ColumnState& right) {
  return left.name == right.name && left.preview == right.preview &&
         left.camstand == right.camstand && left.locked == right.locked &&
         left.alpha == right.alpha && left.filter == right.filter;
}

void readVisibility(const QJsonObject& params, ColumnState& state) {
  const bool split =
      params.contains("preview_visible") || params.contains("camstand_visible");
  if (params.contains("visible") && split)
    throw Error("invalid_params",
                "Give visible, or the separate preview_visible and "
                "camstand_visible flags, not both.",
                {{"reason", "visibility_fields"}});
  if (params.contains("visible")) {
    state.preview  = boolParam(params, "visible", state.preview);
    state.camstand = state.preview;
  }
  state.preview  = boolParam(params, "preview_visible", state.preview);
  state.camstand = boolParam(params, "camstand_visible", state.camstand);
}

void readFilter(Document& document, const QJsonObject& params,
                ColumnState& state) {
  if (!params.contains("color_filter_id")) return;
  const int filter = params.value("color_filter_id").toInt();
  const int count  = document.scene->getProperties()->getColorFilters().size();
  if (filter >= count)
    throw Error("invalid_params", "The scene has no color filter with that id.",
                {{"reason", "color_filter_id"}, {"filter_count", count}});
  state.filter = filter;
}

void refuseSoundFields(TXshColumn* column, const QJsonObject& params) {
  if (!column->getSoundColumn()) return;
  for (const char* key : {"opacity", "preview_visible", "color_filter_id"})
    if (params.contains(key))
      throw Error("invalid_params",
                  QString("Sound columns have no %1.").arg(key),
                  {{"reason", "sound_column"}});
}

bool changesColumnContent(const QJsonObject& params) {
  for (const char* key : {"name", "visible", "preview_visible",
                          "camstand_visible", "opacity", "color_filter_id"})
    if (params.contains(key)) return true;
  return false;
}

QJsonValue updateColumn(Context& context, const QJsonObject& params) {
  Document& document   = getDocument(context, params);
  TXshColumn* column   = document.column(requiredString(params, "column_id"));
  TStageObject* object = columnObject(document, columnIndex(document, column));
  refuseSoundFields(column, params);
  const ColumnState current = currentState(column, object);
  ColumnState next          = current;
  if (params.contains("name")) next.name = nameParam(params).toStdString();
  readVisibility(params, next);
  readFilter(document, params, next);
  next.locked = boolParam(params, "locked", current.locked);
  next.alpha  = static_cast<UCHAR>(std::lround(
      numberParam(params, "opacity", 0, 1, current.alpha / 255.0) * 255));
  if (changesColumnContent(params)) requireEditable(document, object);
  if (sameState(current, next)) return describeColumn(document, column);
  context.markMutationStarted();
  object->setName(next.name);
  column->setPreviewVisible(next.preview);
  column->setCamstandVisible(next.camstand);
  column->setOpacity(next.alpha);
  column->setColorFilterId(next.filter);
  column->lock(next.locked);
  return describeColumn(document, column);
}

// ------------------------------------------------------------ FX branch

std::vector<TFx*> consumers(TFx* fx) {
  std::vector<TFx*> result;
  for (int index = 0; index < fx->getOutputConnectionCount(); ++index)
    if (TFx* owner = fx->getOutputConnection(index)->getOwnerFx())
      result.push_back(owner);
  return result;
}

bool fedOnlyBy(TFx* fx, const std::set<TFx*>& sources) {
  bool connected = false;
  for (int index = 0; index < fx->getInputPortCount(); ++index) {
    TFx* input = fx->getInputPort(index)->getFx();
    if (!input) continue;
    if (!sources.count(input)) return false;
    connected = true;
  }
  return connected;
}

// The effects every input of which comes from the column, directly or
// through such effects: the editor's column-header delete and copy rule.
std::vector<TFx*> exclusiveBranch(Document& document, TFx* columnFx) {
  std::set<TFx*> sources{columnFx};
  std::vector<TFx*> branch;
  TFxSet* internal = sheetOf(document)->getFxDag()->getInternalFxs();
  for (size_t at = 0; at <= branch.size(); ++at) {
    TFx* from = at == 0 ? columnFx : branch[at - 1];
    for (TFx* consumer : consumers(from))
      if (internal->containsFx(consumer) && !sources.count(consumer) &&
          fedOnlyBy(consumer, sources)) {
        sources.insert(consumer);
        branch.push_back(consumer);
      }
  }
  return branch;
}

template <typename Handles, typename Value>
void forgetHandles(Handles& handles, const Value& value) {
  for (auto entry = handles.begin(); entry != handles.end();) {
    if (entry.value() == value)
      entry = handles.erase(entry);
    else
      ++entry;
  }
}

void forgetEffect(Document& document, TFx* fx) {
  forgetHandles(document.effects, fx);
}

void removeEffect(Document& document, TFx* fx) {
  FxDag* dag = sheetOf(document)->getFxDag();
  forgetEffect(document, fx);
  while (fx->getOutputConnectionCount() > 0)
    fx->getOutputConnection(0)->setFx(nullptr);
  fx->disconnectAll();
  dag->removeFxId(fx);
  dag->removeFromXsheet(fx);
  dag->getInternalFxs()->removeFx(fx);
}

// ---------------------------------------------------------------- delete

void forgetObject(Document& document, TStageObject* object) {
  for (int index = 0; index < object->getOffsetLayerCount(); ++index) {
    const TDoubleParamP weight = object->getOffsetLayer(index).m_weight;
    forgetHandles(document.offsetLayers, weight);
  }
  forgetHandles(document.objects, object);
}

std::vector<TStageObject*> childrenOf(Document& document,
                                      TStageObject* object) {
  std::vector<TStageObject*> children;
  for (int index = 0; index < tree(document)->getStageObjectCount(); ++index) {
    TStageObject* other = tree(document)->getStageObject(index);
    if (other != object && other->getParent() == object->getId())
      children.push_back(other);
  }
  return children;
}

QJsonArray fxHandles(Document& document, const std::vector<TFx*>& effects) {
  QJsonArray handles;
  for (TFx* fx : effects)
    if (handles.size() < 4096) handles.append(document.idForFx(fx));
  return handles;
}

void requireFxPolicy(Document& document, TFx* fx, const QString& policy) {
  if (!fx || !policy.isEmpty() || fx->getOutputConnectionCount() == 0) return;
  QJsonArray fed;
  for (TFx* consumer : consumers(fx))
    if (fed.size() < MaxReported) fed.append(document.idForFx(consumer));
  throw Error("has_dependents",
              "The column feeds effects; give fx_policy delete_exclusive or "
              "disconnect.",
              {{"fx", fed}, {"reason", "fx_consumers"}});
}

void requireChildPolicy(Document& document, TStageObject* object,
                        const QString& policy) {
  if (policy == "reattach_to_parent" || !object->hasChildren()) return;
  QJsonArray children;
  for (TStageObject* child : childrenOf(document, object))
    if (children.size() < MaxReported)
      children.append(document.idForObject(child));
  throw Error("has_dependents",
              "Reparent the column's children, or give children_policy "
              "reattach_to_parent.",
              {{"children", children}, {"reason", "children"}});
}

struct Reattached {
  TStageObject* child;
  std::vector<TAffine> before;
};

QJsonArray reportReattached(Document& document,
                            const std::vector<Reattached>& children,
                            const std::vector<int>& frames) {
  QJsonArray result;
  for (const Reattached& entry : children) {
    TStageObject* parent =
        tree(document)->getStageObject(entry.child->getParent(), false);
    QJsonObject row{
        {"object_id", document.idForObject(entry.child)},
        {"parent_id",
         parent ? QJsonValue(document.idForObject(parent)) : QJsonValue()}};
    insertAutomationWorldChanges(
        row, frames, entry.before,
        automationPlacements(*document.scene, *entry.child, frames));
    result.append(row);
  }
  return result;
}

void requireDeletable(Document& document, TXshColumn* column) {
  if (column->getColumnType() != TXshColumn::eLevelType &&
      column->getColumnType() != TXshColumn::eSoundType)
    throw Error("incompatible_column",
                "Use the owning subsystem to delete this column type.");
}

QJsonValue deleteColumn(Context& context, const QJsonObject& params) {
  Document& document = getDocument(context, params);
  requireTopSheet(params);
  const QString id   = requiredString(params, "column_id");
  TXshColumn* column = document.column(id);
  requireDeletable(document, column);
  const int index      = columnIndex(document, column);
  TStageObject* object = columnObject(document, index);
  requireEditable(document, object);
  const QString fxPolicy = params.value("fx_policy").toString();
  TFx* fx                = column->getFx();
  requireFxPolicy(document, fx, fxPolicy == "refuse" ? "" : fxPolicy);
  requireChildPolicy(document, object,
                     params.value("children_policy").toString());
  const std::vector<int> frames = automationReportFrames(params);
  std::vector<Reattached> children;
  for (TStageObject* child : childrenOf(document, object))
    children.push_back(
        {child, automationPlacements(*document.scene, *child, frames)});
  const std::vector<TFx*> branch = fx && fxPolicy == "delete_exclusive"
                                       ? exclusiveBranch(document, fx)
                                       : std::vector<TFx*>();
  std::vector<TFx*> disconnected;
  if (fx)
    for (TFx* consumer : consumers(fx))
      if (std::find(branch.begin(), branch.end(), consumer) == branch.end())
        disconnected.push_back(consumer);
  const QJsonArray removed  = fxHandles(document, branch);
  const QJsonArray detached = fxHandles(document, disconnected);
  context.markMutationStarted();
  for (TFx* effect : branch) removeEffect(document, effect);
  if (fx) {
    forgetEffect(document, fx);
    sheetOf(document)->getFxDag()->removeFxId(fx);
  }
  document.columns.remove(id);
  forgetObject(document, object);
  // Native removal reattaches children to the column's parent, keeping their
  // local keys (TStageObjectTree::removeColumn).
  sheetOf(document)->removeColumn(index);
  // removeColumn leaves the frame count of the removed cells in place.
  sheetOf(document)->updateFrameCount();
  return QJsonObject{
      {"column_id", id},
      {"deleted", true},
      {"fx_removed", removed},
      {"fx_disconnected", detached},
      {"children_reattached", reportReattached(document, children, frames)},
      {"expressions_blocking", QJsonArray()},
      {"notes_reanchored", 0}};
}

// ------------------------------------------------------------------ move

std::vector<TXshColumn*> columnOrder(Document& document) {
  std::vector<TXshColumn*> order;
  for (int index = 0; index < sheetOf(document)->getColumnCount(); ++index)
    order.push_back(sheetOf(document)->getColumn(index));
  return order;
}

QJsonValue moveColumns(Context& context, const QJsonObject& params) {
  Document& document = getDocument(context, params);
  requireTopSheet(params);
  const std::vector<TXshColumn*> before = columnOrder(document);
  const int target =
      readPosition(params, "before_position", 0, int(before.size()));
  std::set<TXshColumn*> moving;
  for (const QJsonValue& value : params.value("column_ids").toArray()) {
    TXshColumn* column = document.column(value.toString());
    if (!column || moving.count(column))
      throw Error("invalid_params", "A column is named twice.",
                  {{"reason", "duplicate_column"}});
    moving.insert(column);
  }
  std::vector<TXshColumn*> block, rest;
  int insertAt = 0;
  for (int index = 0; index < int(before.size()); ++index) {
    const bool moved = moving.count(before[index]) > 0;
    (moved ? block : rest).push_back(before[index]);
    if (!moved && index < target) ++insertAt;
  }
  std::vector<TXshColumn*> after = rest;
  after.insert(after.begin() + insertAt, block.begin(), block.end());
  QJsonArray moved;
  for (int index = 0; index < int(after.size()); ++index) {
    const int from = int(std::find(before.begin(), before.end(), after[index]) -
                         before.begin());
    if (from != index && moved.size() < 4096)
      moved.append(
          QJsonObject{{"column_id", document.idForColumn(after[index])},
                      {"position_before", from},
                      {"position_after", index}});
  }
  if (!moved.isEmpty()) {
    context.markMutationStarted();
    for (int index = 0; index < int(after.size()); ++index) {
      const int from = columnIndex(document, after[index]);
      if (from != index) sheetOf(document)->moveColumn(from, index);
    }
    if (columnOrder(document) != after)
      throw Error("native_error",
                  "The native column order differs from the "
                  "requested order.");
  }
  return QJsonObject{
      {"moved", moved},
      {"expressions_rewritten", 0},
      {"notes_reanchored", 0},
      {"scope", QJsonObject{{"sheet_path", QJsonArray()},
                            {"definition_level_id", QJsonValue()},
                            {"instance_columns", 0},
                            {"shared", false}}}};
}

// ------------------------------------------------------------- duplicate

void requireDuplicable(TXshColumn* column, TStageObject* object) {
  if (column->getColumnType() != TXshColumn::eLevelType ||
      !column->getLevelColumn())
    throw Error("incompatible_column",
                "Only level columns are duplicated here.",
                {{"owner_tool", column->getSoundColumn() ? "audio.column.create"
                                                         : "fx.duplicate"}});
  const int status = object->getStatus() & TStageObject::STATUS_MASK;
  if (status != TStageObject::XY)
    throw Error("unsupported_target",
                "Columns that follow a motion path or IK are not duplicated.",
                {{"reason", "path_or_ik"}});
}

QJsonArray sharedLevels(Document& document, TXshColumn* column) {
  std::set<TXshLevel*> levels;
  int first = 0;
  int last  = -1;
  if (!column->isEmpty()) column->getRange(first, last);
  const int index = columnIndex(document, column);
  for (int row = first; row <= last; ++row)
    if (TXshLevel* level =
            sheetOf(document)->getCell(row, index).m_level.getPointer())
      levels.insert(level);
  QJsonArray handles;
  for (TXshLevel* level : levels)
    if (handles.size() < 256) handles.append(document.idForLevel(level));
  return handles;
}

void requireGraphRoom(Document& document, size_t added) {
  if (sheetOf(document)->getFxDag()->getInternalFxs()->getFxCount() +
          int(added) >=
      MaxEntities)
    throw Error("resource_limit", "A sheet holds at most 4096 effects.");
}

// Clone the exclusive branch onto the copied column; clones of terminal
// effects join the output node, as their sources do.
QJsonArray copyBranch(Document& document, const std::vector<TFx*>& branch,
                      TFx* sourceFx, TFx* copyFx) {
  FxDag* dag = sheetOf(document)->getFxDag();
  std::map<TFx*, TFx*> clones{{sourceFx, copyFx}};
  QJsonArray map;
  for (TFx* fx : branch) {
    TFx* clone = fx->clone(false);
    dag->assignUniqueId(clone);
    dag->getInternalFxs()->addFx(clone);
    if (dag->getTerminalFxs()->containsFx(fx)) dag->addToXsheet(clone);
    clones[fx] = clone;
    map.append(QJsonObject{{"source_fx_id", document.idForFx(fx)},
                           {"fx_id", document.idForFx(clone)}});
  }
  for (TFx* fx : branch)
    for (int port = 0; port < fx->getInputPortCount(); ++port)
      if (TFx* input = fx->getInputPort(port)->getFx())
        clones[fx]->getInputPort(port)->setFx(clones.at(input));
  return map;
}

QJsonValue duplicateColumn(Context& context, const QJsonObject& params) {
  Document& document = getDocument(context, params);
  requireTopSheet(params);
  TXshColumn* source   = document.column(requiredString(params, "column_id"));
  const int from       = columnIndex(document, source);
  TStageObject* object = columnObject(document, from);
  requireDuplicable(source, object);
  const QString name = nameParam(params);
  TXsheet* sheet     = sheetOf(document);
  if (sheet->getColumnCount() >= MaxEntities)
    throw Error("resource_limit", "The document already has 4096 columns.");
  requireAutomationPathObjectGrowth(*document.scene);
  const int index =
      readPosition(params, "position", from + 1, sheet->getColumnCount());
  const bool keepParent =
      params.value("parent_policy").toString() == "same_parent";
  const std::vector<TFx*> branch =
      params.value("fx_policy").toString() == "exclusive_branch"
          ? exclusiveBranch(document, source->getFx())
          : std::vector<TFx*>();
  requireGraphRoom(document, branch.size() + 1);
  const QJsonArray levels = sharedLevels(document, source);
  const int children      = int(childrenOf(document, object).size());
  const bool terminal =
      sheet->getFxDag()->getTerminalFxs()->containsFx(source->getFx());
  context.markMutationStarted();
  sheet->insertColumn(index, source->clone());
  TXshColumn* copy     = sheet->getColumn(index);
  TStageObject* copied = columnObject(document, index);
  std::unique_ptr<TStageObjectParams> state(object->getParams());
  copied->assignParams(state.get());
  copied->setName(name.toStdString());
  copied->setParent(keepParent ? object->getParent() : TStageObjectId::TableId);
  copied->setParentHandle(keepParent ? object->getParentHandle() : "B");
  if (!terminal) sheet->getFxDag()->removeFromXsheet(copy->getFx());
  const QJsonArray map =
      copyBranch(document, branch, source->getFx(), copy->getFx());
  return QJsonObject{
      {"column", describeColumn(document, copy)},
      {"source_column_id", document.idForColumn(source)},
      {"fx_map", map},
      {"shared_levels", levels},
      {"children_not_copied", children},
      {"independent_curves", true},
      {"scope", QJsonObject{{"sheet_path", QJsonArray()},
                            {"definition_level_id", QJsonValue()},
                            {"instance_columns", 0},
                            {"shared", false}}}};
}

// ----------------------------------------------------------------- kinds

struct Kind {
  const char *kind, *native, *createTool, *toolset, *deleteTool, *reason;
  bool cells, movable, duplicable;
};

QJsonValue nullable(const char* text) {
  return text ? QJsonValue(QString::fromLatin1(text)) : QJsonValue();
}

QJsonValue columnKinds(Context& context, const QJsonObject&) {
  static const Kind kinds[] = {
      {"level", "eLevelType", "column.create", "core", "column.delete", nullptr,
       true, true, true},
      {"sound", "eSoundType", "audio.column.create", "audio", "column.delete",
       nullptr, false, true, false},
      {"generator", "eZeraryFxType", "fx.create", "core", "fx.delete", nullptr,
       false, true, false},
      {"annotation", "eSoundTextType", nullptr, nullptr, nullptr,
       "not_implemented", false, true, false},
      {"sub_xsheet", "eLevelType", nullptr, nullptr, nullptr, "not_implemented",
       false, true, false},
      {"palette", "ePaletteType", nullptr, nullptr, nullptr,
       "not_in_editable_profile", false, true, false},
      {"mesh", "eMeshType", nullptr, nullptr, nullptr,
       "not_in_editable_profile", false, true, false}};
  QJsonArray rows;
  for (const Kind& kind : kinds) {
    QJsonValue reason = nullable(kind.reason);
    if (kind.toolset && !context.toolsets.contains(kind.toolset))
      reason = "toolset_disabled";
    rows.append(QJsonObject{{"kind", kind.kind},
                            {"native_type", kind.native},
                            {"creatable", kind.createTool != nullptr},
                            {"create_tool", nullable(kind.createTool)},
                            {"create_toolset", nullable(kind.toolset)},
                            {"cell_tools", kind.cells},
                            {"movable", kind.movable},
                            {"duplicable", kind.duplicable},
                            {"delete_tool", nullable(kind.deleteTool)},
                            {"available", reason.isNull()},
                            {"reason", reason}});
  }
  return QJsonObject{{"kinds", rows}};
}
}  // namespace

QJsonObject boolean() { return {{"type", "boolean"}}; }

QJsonObject handleList(int maximum) {
  return {{"type", "array"},
          {"items", textSchema()},
          {"minItems", 1},
          {"maxItems", maximum}};
}

void addColumnLifecycle(QJsonArray& catalog, const QJsonObject& sheet) {
  QJsonObject column = merge(sheet, {{"column_id", textSchema()}});
  catalog.append(operation(
      "column.delete",
      "Delete an unlocked level or sound column. A column whose FX feeds "
      "effects needs fx_policy (delete_exclusive removes the effects fed only "
      "by it; disconnect leaves them with an empty port); stage children need "
      "children_policy reattach_to_parent.",
      merge(column,
            {{"fx_policy",
              enumSchema({"refuse", "delete_exclusive", "disconnect"})},
             {"children_policy", enumSchema({"refuse", "reattach_to_parent"})},
             {"report_frames", automationReportFramesSchema()}}),
      {"document_id", "column_id"}, true));
  catalog.append(operation(
      "column.move",
      "Move columns as a block, in their current order, before a position of "
      "the original order (the column count means the end). Handles, stage "
      "objects and FX links follow the columns.",
      merge(sheet, {{"column_ids", handleList(64)},
                    {"before_position", integerSchema(0, MaxEntities)}}),
      {"document_id", "column_ids", "before_position"}, true));
  catalog.append(operation(
      "column.duplicate",
      "Duplicate a level column in its sheet: the cells share the same levels, "
      "the stage object and its curves are copied independently, and the FX "
      "branch fed only by the column is copied with exclusive_branch.",
      merge(column,
            {{"name", textSchema()},
             {"position", integerSchema(0, MaxEntities)},
             {"parent_policy", enumSchema({"same_parent", "table"})},
             {"fx_policy", enumSchema({"column_only", "exclusive_branch"})}}),
      {"document_id", "column_id", "name", "parent_policy", "fx_policy"},
      true));
  catalog.append(operation(
      "column.kinds",
      "List the native column kinds with the tools that create, edit and "
      "delete them, and why a kind is unavailable in this session.",
      {}, {}));
}

void addColumnOperations(QJsonArray& catalog) {
  const QJsonObject sheet =
      merge(documentSchema(), {{"sheet_path", sheetPathSchema()}});
  catalog.append(operation(
      "column.create",
      "Create an empty level column with a stable handle, at the end or "
      "before position; later columns keep their handles.",
      merge(sheet, {{"name", textSchema()},
                    {"position", integerSchema(0, MaxEntities)}}),
      {"document_id", "name"}, true));
  QJsonObject update =
      merge(merge(documentSchema(), {{"column_id", textSchema()}}),
            {{"name", textSchema()},
             {"visible", boolean()},
             {"preview_visible", boolean()},
             {"camstand_visible", boolean()},
             {"color_filter_id", integerSchema(0, 63)},
             {"locked", boolean()},
             {"opacity", numberSchema(0, 1)}});
  catalog.append(operation(
      "column.update",
      "Set column name, lock, opacity, color filter and visibility: visible "
      "sets both flags; preview_visible (render) and camstand_visible "
      "(viewer) set one each.",
      update, {"document_id", "column_id"}, true));
  addColumnLifecycle(catalog, sheet);
}

void addColumnHandlers(SceneHandlers& handlers) {
  handlers.insert({{"column.create", createColumn},
                   {"column.update", updateColumn},
                   {"column.delete", deleteColumn},
                   {"column.move", moveColumns},
                   {"column.duplicate", duplicateColumn},
                   {"column.kinds", columnKinds}});
}

}  // namespace SceneShared
}  // namespace GroundzeroAutomation
