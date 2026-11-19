#include "automation_cells.h"
#include "automation_scene_host.h"
#include "automation_scene_shared.h"

#include "toonz/toonzscene.h"
#include "toonz/tstageobject.h"
#include "toonz/tstageobjecttree.h"
#include "toonz/txshcell.h"
#include "toonz/txshchildlevel.h"
#include "toonz/txsheet.h"
#include "toonz/txshlevelcolumn.h"
#include "toonz/txshleveltypes.h"

#include <QSet>

#include <algorithm>
#include <map>
#include <vector>

namespace GroundzeroAutomation {
namespace SceneShared {
namespace {

constexpr int LastRow           = 999999;
constexpr int MaxCallColumns    = 64;
constexpr int MaxColumnCells    = 10000;
constexpr int MaxCallCells      = 100000;
constexpr qint64 MaxBlockCells  = 1000000;
constexpr qint64 MaxStoredCells = 1000000;
constexpr int MaxReportedRows   = 64;
constexpr int MaxPairs          = 4096;

// ----------------------------------------------------------------- targets

struct Target {
  TXshColumn* column;
  int index;
  QString id;
};

void requireTopSheet(const QJsonObject& params) {
  if (!params.value("sheet_path").toArray().isEmpty())
    throw Error("unsupported_target",
                "Cell tools edit the top sheet; nested sheets are not editable "
                "yet.",
                {{"reason", "nested_sheet_editing"}});
}

bool showsSubSheet(TXsheet& sheet, int index) {
  const CellModel cells = CellModel::read(sheet, index);
  for (int row = cells.first(); row <= cells.last(); ++row) {
    const TXshCell cell = cells.at(row);
    if (!cell.isEmpty() && cell.m_level->getChildLevel()) return true;
  }
  return false;
}

void requireLevelColumn(Document& document, const Target& target) {
  QString kind  = "level";
  QString owner = "";
  switch (target.column->getColumnType()) {
  case TXshColumn::eLevelType:
    if (!showsSubSheet(*document.scene->getXsheet(), target.index)) return;
    kind  = "sub_xsheet";
    owner = "subxsheet.instance.set";
    break;
  case TXshColumn::eSoundType:
    kind  = "sound";
    owner = "audio.clip.place";
    break;
  case TXshColumn::eZeraryFxType:
    kind  = "generator";
    owner = "fx.create";
    break;
  default:
    kind = "unsupported";
    break;
  }
  throw Error("incompatible_column",
              "Cell tools edit level columns; use the column's own tools.",
              {{"column_id", target.id},
               {"kind", kind},
               {"owner_tool", owner.isEmpty() ? QJsonValue() : owner}});
}

bool isLocked(Document& document, const Target& target) {
  if (target.column->isLocked()) return true;
  TStageObject* object = tree(document)->getStageObject(
      TStageObjectId::ColumnId(target.index), false);
  return object && object->isLocked();
}

void requireUnlocked(Document& document, const std::vector<Target>& targets) {
  QJsonArray locked;
  for (const Target& target : targets)
    if (isLocked(document, target)) locked.append(target.id);
  if (!locked.isEmpty())
    throw Error("locked", "Unlock every target column before editing cells.",
                {{"column_ids", locked}});
}

// Resolve distinct level columns; `unlocked` also refuses locked ones.
std::vector<Target> readTargets(Document& document, const QJsonArray& ids,
                                bool unlocked) {
  std::vector<Target> targets;
  QSet<QString> seen;
  for (const QJsonValue& value : ids) {
    const QString id = value.toString();
    if (seen.contains(id))
      throw Error("invalid_params", "A column is named twice.",
                  {{"reason", "duplicate_column"}, {"column_id", id}});
    seen.insert(id);
    TXshColumn* column = document.column(id);
    targets.push_back({column, columnIndex(document, column), id});
  }
  for (const Target& target : targets) requireLevelColumn(document, target);
  if (unlocked) requireUnlocked(document, targets);
  return targets;
}

std::vector<Target> readTargets(Document& document, const QJsonObject& params,
                                bool unlocked) {
  return readTargets(document, params.value("column_ids").toArray(), unlocked);
}

// --------------------------------------------------------------- drawings

TXshLevel* readCellLevel(Document& document, const QString& id) {
  TXshLevel* level = nullptr;
  try {
    level = document.level(id);
  } catch (const Error&) {
    throw Error("missing_level", "No level of this document has that handle.",
                {{"level_id", id}});
  }
  if (level->getSoundLevel())
    throw Error("incompatible_column", "Sound levels are placed as clips.",
                {{"level_id", id}, {"owner_tool", "audio.clip.place"}});
  if (level->getChildLevel())
    throw Error("unsupported_level",
                "Sub-xsheet levels are exposed by their own tools.",
                {{"level_id", id}});
  const int type = level->getType();
  if (type != PLI_XSHLEVEL && type != TZP_XSHLEVEL && type != OVL_XSHLEVEL)
    throw Error("unsupported_level", "Cell tools expose drawing levels only.",
                {{"level_id", id}});
  return level;
}

bool hasDrawing(TXshLevel* level, const TFrameId& frame) {
  std::vector<TFrameId> drawings;
  level->getFids(drawings);
  return std::find(drawings.begin(), drawings.end(), frame) != drawings.end();
}

TFrameId readFrame(const QJsonObject& drawing) {
  return readDrawingId({{"drawing_id", drawing}});
}

void requireSettable(const Target& target, const TXshCell& cell) {
  if (cell.isEmpty()) return;
  TXshLevelColumn* column = target.column->getLevelColumn();
  if (!column || !column->canSetCell(cell))
    throw Error("incompatible_column",
                "The column cannot expose a cell of this level type.",
                {{"column_id", target.id},
                 {"kind", "level"},
                 {"owner_tool", QJsonValue()}});
}

// ------------------------------------------------------------------ plans

CellPlan newPlan(Document& document, const Target& target) {
  CellPlan plan;
  plan.column   = target.column;
  plan.index    = target.index;
  plan.columnId = target.id;
  plan.before   = CellModel::read(*document.scene->getXsheet(), target.index);
  return plan;
}

void requireFrameRange(int last) {
  if (last > LastRow)
    throw Error("invalid_params",
                "The edit would place a cell past frame 999999.",
                {{"reason", "frame_range"}, {"frame", last}});
}

void requireBlock(qint64 columns, qint64 length) {
  if (columns * length > MaxBlockCells)
    throw Error("resource_limit",
                "One call edits at most 1,000,000 rows times columns.");
}

// The save profile's bounds on the cells the document would store.
void requireStoredCells(Document& document,
                        const std::vector<CellPlan>& plans) {
  TXsheet& sheet = *document.scene->getXsheet();
  std::map<int, const CellPlan*> planned;
  for (const CellPlan& plan : plans) planned[plan.index] = &plan;
  qint64 stored = 0;
  for (int index = 0; index < sheet.getColumnCount(); ++index) {
    const auto found = planned.find(index);
    if (found != planned.end()) {
      if (found->second->after.last() > LastRow)
        throw Error("scene_limit", "No cell may be stored past frame 999999.");
      stored += found->second->after.span();
      continue;
    }
    TXshColumn* column = sheet.getColumn(index);
    if (column && column->getLevelColumn())
      stored += CellModel::read(sheet, index).span();
  }
  if (stored > MaxStoredCells)
    throw Error("scene_limit",
                "The document would store more than 1,000,000 cells.");
}

QJsonValue cellRange(const CellModel& cells) {
  if (cells.empty()) return QJsonValue();
  return QJsonObject{{"first_frame", cells.first()},
                     {"last_frame", cells.last()}};
}

QJsonObject scope() {
  return {{"sheet_path", QJsonArray()},
          {"definition_level_id", QJsonValue()},
          {"instance_columns", 0},
          {"shared", false}};
}

using RowsDelta = int (*)(const CellPlan&, int start, int length);

int noShift(const CellPlan&, int, int) { return 0; }

int insertShift(const CellPlan& plan, int start, int length) {
  return !plan.before.empty() && plan.before.last() >= start ? length : 0;
}

int removeShift(const CellPlan& plan, int start, int length) {
  return !plan.before.empty() && plan.before.last() >= start + length ? -length
                                                                      : 0;
}

// Check the stored bounds, apply every changed plan and describe the result.
QJsonObject runPlans(Context& context, Document& document,
                     std::vector<CellPlan>& plans, RowsDelta delta, int start,
                     int length) {
  for (CellPlan& plan : plans) modelCellPlan(plan);
  requireStoredCells(document, plans);
  std::vector<CellPlan> changed;
  for (const CellPlan& plan : plans)
    if (!plan.before.equals(plan.after)) changed.push_back(plan);
  if (!changed.empty()) {
    context.markMutationStarted();
    applyCellPlans(document, changed);
  }
  QJsonArray columns;
  int total = 0;
  for (const CellPlan& plan : plans) {
    const int rows = plan.before.changedRows(plan.after);
    total += rows;
    columns.append(
        QJsonObject{{"column_id", plan.columnId},
                    {"changed", rows > 0},
                    {"cells_changed", rows},
                    {"rows_delta", rows > 0 ? delta(plan, start, length) : 0},
                    {"cell_range_before", cellRange(plan.before)},
                    {"cell_range_after", cellRange(plan.after)}});
  }
  return {{"columns", columns},
          {"cells_changed", total},
          {"scope", scope()},
          {"curves_moved", false}};
}

// ---------------------------------------------------------------- cells.set

struct Runs {
  std::vector<TXshCell> cells;
  std::vector<std::pair<int, bool>> spans;  // length, drawing run
};

Runs readRuns(Document& document, const Target& target,
              const QJsonArray& runs) {
  Runs result;
  for (int index = 0; index < runs.size(); ++index) {
    const QJsonObject run = runs.at(index).toObject();
    const int length      = run.value("length").toInt();
    if (int(result.cells.size()) + length > MaxColumnCells)
      throw Error("resource_limit", "A column takes at most 10000 cells.",
                  {{"column_id", target.id}});
    if (run.contains("empty")) {
      result.cells.insert(result.cells.end(), length, TXshCell());
      result.spans.push_back({length, false});
      continue;
    }
    const QString levelId = run.value("level_id").toString();
    TXshLevel* level      = readCellLevel(document, levelId);
    const TFrameId frame  = readFrame(run.value("drawing_id").toObject());
    if (!hasDrawing(level, frame))
      throw Error("missing_drawing", "The level has no such drawing.",
                  {{"column_id", target.id},
                   {"run_index", index},
                   {"level_id", levelId},
                   {"drawing_id", drawingId(frame)}});
    const TXshCell cell(level, frame);
    requireSettable(target, cell);
    result.cells.insert(result.cells.end(), length, cell);
    result.spans.push_back({length, true});
  }
  return result;
}

void requireFree(const CellPlan& plan, int start, int length,
                 QJsonArray& occupied) {
  for (int row = start; row < start + length; ++row)
    if (!plan.before.at(row).isEmpty() && occupied.size() < MaxReportedRows)
      occupied.append(
          QJsonObject{{"column_id", plan.columnId}, {"frame", row}});
}

void addRunOps(CellPlan& plan, int start, const Runs& runs) {
  int row = start;
  int at  = 0;
  for (const auto& span : runs.spans) {
    if (span.second)
      plan.ops.push_back(
          {CellOp::Write, row, span.first,
           std::vector<TXshCell>(runs.cells.begin() + at,
                                 runs.cells.begin() + at + span.first)});
    else
      plan.ops.push_back({CellOp::Clear, row, span.first, {}});
    row += span.first;
    at += span.first;
  }
}

QJsonValue setCells(Context& context, const QJsonObject& params) {
  Document& document = getDocument(context, params);
  requireTopSheet(params);
  const QJsonArray columns = params.value("columns").toArray();
  QJsonArray ids;
  for (const QJsonValue& column : columns)
    ids.append(column.toObject().value("column_id"));
  const std::vector<Target> targets = readTargets(document, ids, true);
  const bool overwrite              = params.value("overwrite").toBool();
  std::vector<CellPlan> plans;
  QJsonArray occupied;
  int total = 0;
  for (size_t index = 0; index < targets.size(); ++index) {
    const QJsonObject column = columns.at(int(index)).toObject();
    const Runs runs =
        readRuns(document, targets[index], column.value("runs").toArray());
    const int start  = column.value("start_frame").toInt();
    const int length = int(runs.cells.size());
    total += length;
    if (total > MaxCallCells)
      throw Error("resource_limit", "One call writes at most 100000 cells.");
    requireFrameRange(start + length - 1);
    CellPlan plan = newPlan(document, targets[index]);
    if (!overwrite) requireFree(plan, start, length, occupied);
    addRunOps(plan, start, runs);
    plans.push_back(plan);
  }
  if (!occupied.isEmpty())
    throw Error("occupied_cells",
                "The intervals contain cells; set overwrite to replace them.",
                {{"rows", occupied}});
  return runPlans(context, document, plans, noShift, 0, 0);
}

// ------------------------------------------ cells.clear, remove and insert

QJsonValue intervalEdit(Context& context, const QJsonObject& params,
                        CellOp::Kind kind, RowsDelta delta) {
  Document& document = getDocument(context, params);
  requireTopSheet(params);
  const std::vector<Target> targets = readTargets(document, params, true);
  const int start                   = params.value("start_frame").toInt();
  const int length                  = params.value("length").toInt();
  requireFrameRange(start + length - 1);
  requireBlock(qint64(targets.size()), length);
  std::vector<CellPlan> plans;
  for (const Target& target : targets) {
    CellPlan plan = newPlan(document, target);
    plan.ops.push_back({kind, start, length, {}});
    modelCellPlan(plan);
    if (kind == CellOp::Insert && !plan.after.empty())
      requireFrameRange(plan.after.last());
    plans.push_back(plan);
  }
  return runPlans(context, document, plans, delta, start, length);
}

QJsonValue clearCells(Context& context, const QJsonObject& params) {
  return intervalEdit(context, params, CellOp::Clear, noShift);
}

QJsonValue removeCells(Context& context, const QJsonObject& params) {
  return intervalEdit(context, params, CellOp::Remove, removeShift);
}

QJsonValue insertCells(Context& context, const QJsonObject& params) {
  return intervalEdit(context, params, CellOp::Insert, insertShift);
}

// --------------------------------------------------------------- cells.copy

struct CopyRequest {
  std::vector<Target> sources, destinations;
  int start, length, destination;
  bool insert, move;
};

CopyRequest readCopy(Document& document, const QJsonObject& params) {
  const QJsonObject source      = params.value("source").toObject();
  const QJsonObject destination = params.value("destination").toObject();
  CopyRequest request;
  request.move   = params.value("source_after").toString() == "clear";
  request.insert = params.value("placement").toString() == "insert";
  request.sources =
      readTargets(document, source.value("column_ids").toArray(), request.move);
  request.destinations =
      readTargets(document, destination.value("column_ids").toArray(), true);
  if (request.sources.size() != request.destinations.size())
    throw Error("invalid_params",
                "Name as many destination columns as source columns.",
                {{"reason", "column_count"}});
  request.start       = source.value("start_frame").toInt();
  request.length      = source.value("length").toInt();
  request.destination = destination.value("start_frame").toInt();
  requireFrameRange(request.start + request.length - 1);
  requireFrameRange(request.destination + request.length - 1);
  requireBlock(qint64(request.sources.size()), request.length);
  return request;
}

void refuseOverlappingMove(const CopyRequest& request) {
  if (!request.insert || !request.move) return;
  for (const Target& source : request.sources)
    for (const Target& destination : request.destinations)
      if (source.column == destination.column)
        throw Error(
            "invalid_params",
            "An inserting move cannot target one of its source "
            "columns: the insert would shift the moved rows.",
            {{"reason", "overlapping_move_insert"}, {"column_id", source.id}});
}

CellPlan& planFor(Document& document, std::vector<CellPlan>& plans,
                  const Target& target) {
  for (CellPlan& plan : plans)
    if (plan.column == target.column) return plan;
  plans.push_back(newPlan(document, target));
  return plans.back();
}

QJsonValue copyCells(Context& context, const QJsonObject& params) {
  Document& document = getDocument(context, params);
  requireTopSheet(params);
  const CopyRequest request = readCopy(document, params);
  refuseOverlappingMove(request);
  std::vector<std::vector<TXshCell>> blocks;
  int cleared = 0;
  for (size_t index = 0; index < request.sources.size(); ++index) {
    const CellModel cells = CellModel::read(*document.scene->getXsheet(),
                                            request.sources[index].index);
    std::vector<TXshCell> block;
    for (int row = request.start; row < request.start + request.length; ++row)
      block.push_back(cells.at(row));
    for (const TXshCell& cell : block) {
      requireSettable(request.destinations[index], cell);
      cleared += cell.isEmpty() ? 0 : 1;
    }
    blocks.push_back(block);
  }
  std::vector<CellPlan> plans;
  plans.reserve(request.sources.size() * 2);
  if (request.move)
    for (const Target& source : request.sources)
      planFor(document, plans, source)
          .ops.push_back({CellOp::Clear, request.start, request.length, {}});
  for (size_t index = 0; index < request.destinations.size(); ++index) {
    CellPlan& plan = planFor(document, plans, request.destinations[index]);
    if (request.insert)
      plan.ops.push_back(
          {CellOp::Insert, request.destination, request.length, {}});
    plan.ops.push_back(
        {CellOp::Write, request.destination, request.length, blocks[index]});
  }
  for (CellPlan& plan : plans) {
    modelCellPlan(plan);
    if (!plan.after.empty()) requireFrameRange(plan.after.last());
  }
  QJsonObject result =
      runPlans(context, document, plans, request.insert ? insertShift : noShift,
               request.destination, request.length);
  result.insert("source_cells_cleared", request.move ? cleared : 0);
  result.insert("placement", params.value("placement"));
  return result;
}

// ------------------------------------------------------------ cells.replace

using CellMap = std::map<std::pair<TXshLevel*, TFrameId>, TXshCell>;

struct Replacement {
  TXshLevel* from = nullptr;
  TXshLevel* to   = nullptr;
  CellMap pairs;
};

std::pair<TXshLevel*, TFrameId> readReference(Document& document,
                                              const QJsonObject& value) {
  TXshLevel* level =
      readCellLevel(document, value.value("level_id").toString());
  return {level, readFrame(value.value("drawing_id").toObject())};
}

Replacement readReplacement(Document& document, const QJsonObject& mapping) {
  Replacement result;
  if (mapping.value("kind").toString() == "level") {
    result.from =
        readCellLevel(document, mapping.value("from_level_id").toString());
    result.to =
        readCellLevel(document, mapping.value("to_level_id").toString());
    if (result.from == result.to)
      throw Error("invalid_params", "Replace a level by a different level.",
                  {{"reason", "same_level"}});
    return result;
  }
  const QJsonArray pairs = mapping.value("pairs").toArray();
  if (pairs.size() > MaxPairs)
    throw Error("invalid_params", "A mapping holds at most 4096 pairs.");
  for (const QJsonValue& value : pairs) {
    const auto from = readReference(document, value["from"].toObject());
    const auto to   = readReference(document, value["to"].toObject());
    if (from == to || result.pairs.count(from))
      throw Error("invalid_params",
                  "Each pair maps one distinct drawing to a different one.",
                  {{"reason", "pairs"}});
    if (!hasDrawing(to.first, to.second))
      throw Error("missing_drawing", "The target level has no such drawing.",
                  {{"drawings", QJsonArray{drawingId(to.second)}}});
    result.pairs[from] = TXshCell(to.first, to.second);
  }
  return result;
}

TXshCell replaced(const Replacement& mapping, const TXshCell& cell,
                  QJsonArray& missing) {
  if (cell.isEmpty()) return cell;
  TXshLevel* level = cell.m_level.getPointer();
  if (!mapping.from) {
    const auto found = mapping.pairs.find({level, cell.m_frameId});
    return found == mapping.pairs.end() ? cell : found->second;
  }
  if (level != mapping.from) return cell;
  if (!hasDrawing(mapping.to, cell.m_frameId)) {
    const QJsonObject id = drawingId(cell.m_frameId);
    if (!missing.contains(id) && missing.size() < MaxReportedRows)
      missing.append(id);
  }
  return TXshCell(mapping.to, cell.m_frameId);
}

void addReplaceOps(CellPlan& plan, const Target& target, int start, int length,
                   const Replacement& mapping, QJsonArray& missing) {
  int row = start;
  while (row < start + length) {
    const TXshCell cell = plan.before.at(row);
    const TXshCell next = replaced(mapping, cell, missing);
    if (next == cell) {
      ++row;
      continue;
    }
    requireSettable(target, next);
    std::vector<TXshCell> run;
    for (; row < start + length; ++row) {
      const TXshCell current = plan.before.at(row);
      const TXshCell swapped = replaced(mapping, current, missing);
      if (swapped == current) break;
      run.push_back(swapped);
    }
    plan.ops.push_back(
        {CellOp::Write, row - int(run.size()), int(run.size()), run});
  }
}

QJsonValue replaceCells(Context& context, const QJsonObject& params) {
  Document& document = getDocument(context, params);
  requireTopSheet(params);
  const std::vector<Target> targets = readTargets(document, params, true);
  const int start                   = params.value("start_frame").toInt();
  const int length                  = params.value("length").toInt();
  requireFrameRange(start + length - 1);
  requireBlock(qint64(targets.size()), length);
  const Replacement mapping =
      readReplacement(document, params.value("mapping").toObject());
  std::vector<CellPlan> plans;
  QJsonArray missing;
  for (const Target& target : targets) {
    CellPlan plan = newPlan(document, target);
    addReplaceOps(plan, target, start, length, mapping, missing);
    plans.push_back(plan);
  }
  if (!missing.isEmpty())
    throw Error("missing_drawing",
                "The target level lacks drawings the interval exposes.",
                {{"drawings", missing}});
  return runPlans(context, document, plans, noShift, start, length);
}

// ------------------------------------------------------------------ catalog

QJsonObject handleSchema() {
  return {{"type", "string"},
          {"minLength", 1},
          {"maxLength", 128},
          {"pattern", "^[A-Za-z0-9_-]+$"}};
}

QJsonObject listSchema(const QJsonObject& items, int maximum) {
  return {{"type", "array"},
          {"items", items},
          {"minItems", 1},
          {"maxItems", maximum}};
}

QJsonObject objectSchema(const QJsonObject& properties,
                         const QJsonArray& required) {
  return {{"type", "object"},
          {"additionalProperties", false},
          {"properties", properties},
          {"required", required}};
}

QJsonObject drawingSchema() {
  return objectSchema({{"number", integerSchema(1, 999999)},
                       {"suffix", QJsonObject{{"type", "string"},
                                              {"minLength", 0},
                                              {"maxLength", 8},
                                              {"pattern", "^[a-z]{0,8}$"}}}},
                      {"number", "suffix"});
}

QJsonObject sheetSchema(const QJsonObject& properties) {
  return merge(properties, {{"document_id", handleSchema()},
                            {"sheet_path", sheetPathSchema()}});
}

QJsonObject runSchema() {
  const QJsonObject length = integerSchema(1, MaxColumnCells);
  return {{"oneOf",
           QJsonArray{objectSchema({{"level_id", handleSchema()},
                                    {"drawing_id", drawingSchema()},
                                    {"length", length}},
                                   {"level_id", "drawing_id", "length"}),
                      objectSchema({{"empty", QJsonObject{{"const", true}}},
                                    {"length", length}},
                                   {"empty", "length"})}}};
}

QJsonObject intervalSchema(int maximumLength) {
  return sheetSchema(
      {{"column_ids", listSchema(handleSchema(), MaxCallColumns)},
       {"start_frame", integerSchema(0, LastRow)},
       {"length", integerSchema(1, maximumLength)}});
}

const QJsonArray IntervalRequired{"document_id", "column_ids", "start_frame",
                                  "length"};

void addSetOperation(QJsonArray& catalog) {
  const QJsonObject column =
      objectSchema({{"column_id", handleSchema()},
                    {"start_frame", integerSchema(0, LastRow)},
                    {"runs", listSchema(runSchema(), MaxColumnCells)}},
                   {"column_id", "start_frame", "runs"});
  catalog.append(operation(
      "cells.set",
      "Write explicit runs of drawings and blanks into up to 64 level columns "
      "at once, all or nothing. Rows are zero-based frames; later rows never "
      "shift. overwrite false refuses any occupied target row.",
      sheetSchema({{"columns", listSchema(column, MaxCallColumns)},
                   {"overwrite", QJsonObject{{"type", "boolean"}}}}),
      {"document_id", "columns", "overwrite"}, true));
}

void addIntervalOperations(QJsonArray& catalog) {
  catalog.append(operation(
      "cells.clear",
      "Blank an interval of up to 64 level columns without shifting: later "
      "exposures keep their frame numbers.",
      intervalSchema(MaxCallCells), IntervalRequired, true));
  catalog.append(operation(
      "cells.remove",
      "Remove an interval of rows from up to 64 level columns and shift their "
      "later cells up (delete time). Other columns do not move.",
      intervalSchema(MaxCallCells), IntervalRequired, true));
  catalog.append(operation(
      "cells.insert",
      "Insert blank rows at a frame in up to 64 level columns, shifting their "
      "later cells down; a column with no cell at or after the frame does not "
      "change.",
      intervalSchema(MaxColumnCells), IntervalRequired, true));
}

void addCopyOperation(QJsonArray& catalog) {
  const QJsonObject columns = listSchema(handleSchema(), MaxCallColumns);
  const QJsonObject source =
      objectSchema({{"column_ids", columns},
                    {"start_frame", integerSchema(0, LastRow)},
                    {"length", integerSchema(1, MaxColumnCells)}},
                   {"column_ids", "start_frame", "length"});
  const QJsonObject destination = objectSchema(
      {{"column_ids", columns}, {"start_frame", integerSchema(0, LastRow)}},
      {"column_ids", "start_frame"});
  catalog.append(operation(
      "cells.copy",
      "Copy a block of exposure references (cells, not drawings) from up to "
      "64 columns to as many destination columns, inserting or replacing; "
      "source_after clear makes it a move. The clipboard is not used.",
      sheetSchema({{"source", source},
                   {"destination", destination},
                   {"placement", enumSchema({"insert", "replace"})},
                   {"source_after", enumSchema({"keep", "clear"})}}),
      {"document_id", "source", "destination", "placement", "source_after"},
      true));
}

void addReplaceOperation(QJsonArray& catalog) {
  const QJsonObject reference = objectSchema(
      {{"level_id", handleSchema()}, {"drawing_id", drawingSchema()}},
      {"level_id", "drawing_id"});
  const QJsonObject level =
      objectSchema({{"kind", QJsonObject{{"const", "level"}}},
                    {"from_level_id", handleSchema()},
                    {"to_level_id", handleSchema()}},
                   {"kind", "from_level_id", "to_level_id"});
  const QJsonObject pairs = objectSchema(
      {{"kind", QJsonObject{{"const", "drawings"}}},
       {"pairs",
        listSchema(objectSchema({{"from", reference}, {"to", reference}},
                                {"from", "to"}),
                   MaxPairs)}},
      {"kind", "pairs"});
  QJsonObject properties = intervalSchema(MaxCallCells);
  properties.insert("mapping",
                    QJsonObject{{"oneOf", QJsonArray{level, pairs}}});
  QJsonArray required = IntervalRequired;
  required.append("mapping");
  catalog.append(operation(
      "cells.replace",
      "Replace exposure references in an interval of up to 64 level columns: "
      "every cell of one level by the same drawing of another, or explicit "
      "drawing pairs. Timing does not change.",
      properties, required, true));
}

}  // namespace

void addCellToolOperations(QJsonArray& catalog) {
  addSetOperation(catalog);
  addIntervalOperations(catalog);
  addCopyOperation(catalog);
  addReplaceOperation(catalog);
}

void addCellToolHandlers(SceneHandlers& handlers) {
  handlers.insert({{"cells.set", setCells},
                   {"cells.clear", clearCells},
                   {"cells.remove", removeCells},
                   {"cells.insert", insertCells},
                   {"cells.copy", copyCells},
                   {"cells.replace", replaceCells}});
}

}  // namespace SceneShared

}  // namespace GroundzeroAutomation
