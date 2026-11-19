#include "automation_cells.h"
#include "automation_scene_shared.h"

#include "toonz/toonzscene.h"
#include "toonz/tstageobject.h"
#include "toonz/tstageobjecttree.h"
#include "toonz/txshcell.h"
#include "toonz/txsheet.h"
#include "toonz/txshlevelcolumn.h"

#include <algorithm>
#include <vector>

namespace GroundzeroAutomation {

CellModel CellModel::read(TXsheet& sheet, int index) {
  CellModel model;
  TXshColumn* column = sheet.getColumn(index);
  if (!column || column->isEmpty() || !column->getCellColumn()) return model;
  int first = 0;
  int last  = -1;
  column->getRange(first, last);
  model.m_first = first;
  model.m_cells.reserve(std::max(0, last - first + 1));
  for (int row = first; row <= last; ++row)
    model.m_cells.push_back(sheet.getCell(row, index));
  model.rebuild(first, last, [&model](int row) {
    return model.m_cells[row - model.m_first];
  });
  return model;
}

TXshCell CellModel::at(int row) const {
  if (row < m_first || row > last()) return TXshCell();
  return m_cells[row - m_first];
}

bool CellModel::equals(const CellModel& other) const {
  return firstDifference(other) < 0;
}

int CellModel::firstDifference(const CellModel& other) const {
  if (empty() && other.empty()) return -1;
  const int from =
      empty() ? other.first()
              : (other.empty() ? first() : std::min(first(), other.first()));
  const int to = std::max(last(), other.last());
  for (int row = from; row <= to; ++row)
    if (at(row) != other.at(row)) return row;
  return -1;
}

int CellModel::changedRows(const CellModel& other) const {
  if (empty() && other.empty()) return 0;
  const int from =
      empty() ? other.first()
              : (other.empty() ? first() : std::min(first(), other.first()));
  const int to = std::max(last(), other.last());
  int changed  = 0;
  for (int row = from; row <= to; ++row)
    if (at(row) != other.at(row)) ++changed;
  return changed;
}

// Replace the cells by cellAt(row) over [from, to], then drop the blank cells
// at both ends, as every native primitive does.
template <typename CellAt>
void CellModel::rebuild(int from, int to, const CellAt& cellAt) {
  std::vector<TXshCell> cells;
  if (to >= from) cells.reserve(to - from + 1);
  for (int row = from; row <= to; ++row) cells.push_back(cellAt(row));
  int head = 0;
  while (head < int(cells.size()) && cells[head].isEmpty()) ++head;
  int tail = int(cells.size());
  while (tail > head && cells[tail - 1].isEmpty()) --tail;
  m_cells.assign(cells.begin() + head, cells.begin() + tail);
  m_first = m_cells.empty() ? 0 : from + head;
}

void CellModel::write(int row, const std::vector<TXshCell>& cells) {
  if (cells.empty()) return;
  const int end         = row + int(cells.size()) - 1;
  const int from        = empty() ? row : std::min(first(), row);
  const int to          = empty() ? end : std::max(last(), end);
  const CellModel prior = *this;
  rebuild(from, to, [&](int at) {
    return at >= row && at <= end ? cells[at - row] : prior.at(at);
  });
}

void CellModel::clear(int row, int count) {
  if (empty() || count <= 0) return;
  const CellModel prior = *this;
  rebuild(first(), last(), [&](int at) {
    return at >= row && at < row + count ? TXshCell() : prior.at(at);
  });
}

void CellModel::remove(int row, int count) {
  if (empty() || count <= 0 || row > last()) return;
  const CellModel prior = *this;
  const int from = first() < row ? first() : std::max(row, first() - count);
  rebuild(from, last(), [&](int at) {
    return at < row ? prior.at(at) : prior.at(at + count);
  });
}

void CellModel::insert(int row, int count) {
  if (empty() || count <= 0 || row > last()) return;
  const CellModel prior = *this;
  const int from        = first() < row ? first() : first() + count;
  rebuild(from, last() + count, [&](int at) {
    if (at < row) return prior.at(at);
    return at < row + count ? TXshCell() : prior.at(at - count);
  });
}

void modelCellPlan(CellPlan& plan) {
  plan.after = plan.before;
  for (const CellOp& op : plan.ops) {
    switch (op.kind) {
    case CellOp::Write:
      plan.after.write(op.row, op.cells);
      break;
    case CellOp::Clear:
      plan.after.clear(op.row, op.count);
      break;
    case CellOp::Remove:
      plan.after.remove(op.row, op.count);
      break;
    case CellOp::Insert:
      plan.after.insert(op.row, op.count);
      break;
    }
  }
}

namespace {

bool allEmpty(const std::vector<TXshCell>& cells) {
  for (const TXshCell& cell : cells)
    if (!cell.isEmpty()) return false;
  return true;
}

// TXsheet::setCells returns false for a refused cell or a locked column; the
// other primitives return nothing and are checked by the model comparison.
bool applyNative(TXsheet& sheet, int index, const CellOp& op) {
  switch (op.kind) {
  case CellOp::Write:
    if (allEmpty(op.cells)) {
      sheet.clearCells(op.row, index, int(op.cells.size()));
      return true;
    }
    return sheet.setCells(op.row, index, int(op.cells.size()), op.cells.data());
  case CellOp::Clear:
    sheet.clearCells(op.row, index, op.count);
    return true;
  case CellOp::Remove:
    sheet.removeCells(op.row, index, op.count);
    return true;
  case CellOp::Insert:
    sheet.insertCells(op.row, index, op.count);
    return true;
  }
  return false;
}

}  // namespace

void applyCellPlans(Document& document, std::vector<CellPlan>& plans) {
  TXsheet& sheet = *document.scene->getXsheet();
  for (CellPlan& plan : plans) {
    const int index      = plan.index;
    TStageObject* object = sheet.getStageObjectTree()->getStageObject(
        TStageObjectId::ColumnId(index), false);
    const std::string name = object ? object->getName() : std::string();
    for (const CellOp& op : plan.ops)
      if (!applyNative(sheet, index, op))
        throw Error("native_error",
                    "A preflighted native cell write was refused.",
                    {{"column_id", plan.columnId}, {"frame", op.row}});
    // Native writes rename a column when the editor links names to levels.
    if (object && object->getName() != name) object->setName(name);
    const int row = CellModel::read(sheet, index).firstDifference(plan.after);
    if (row >= 0)
      throw Error("native_error",
                  "The native cells differ from the worker's cell model.",
                  {{"column_id", plan.columnId}, {"frame", row}});
  }
}

namespace SceneShared {
namespace {

struct CellRange {
  int start, length;
};

CellRange readRange(const QJsonObject& params) {
  const int start  = integerParam(params, "start_frame", 0, MaxFrame, -1);
  const int length = integerParam(params, "length", 1, MaxSpan, -1);
  if (start < 0 || length < 1 || start > MaxFrame - length + 1)
    throw Error("invalid_argument",
                "The exposure interval exceeds the supported frame range.");
  return {start, length};
}

void validateDrawing(TXshLevel* level, const TFrameId& frame) {
  std::vector<TFrameId> drawings;
  level->getFids(drawings);
  if (std::find(drawings.begin(), drawings.end(), frame) == drawings.end())
    throw Error("missing_drawing",
                "The requested drawing does not exist in the level.");
}

bool preflightCells(TXsheet* sheet, int index, const CellRange& range,
                    const TXshCell& cell, bool overwrite) {
  bool changed = false;
  for (int row = range.start; row < range.start + range.length; ++row) {
    const TXshCell& existing = sheet->getCell(row, index);
    if (!overwrite && !existing.isEmpty())
      throw Error("occupied_cells",
                  "The interval contains cells; set overwrite to replace.");
    if (existing != cell) changed = true;
  }
  return changed;
}

QJsonValue exposeColumn(Context& context, const QJsonObject& params) {
  Document& document = getDocument(context, params);
  TXshColumn* column = document.column(requiredString(params, "column_id"));
  const int index    = columnIndex(document, column);
  TStageObject* object =
      tree(document)->getStageObject(TStageObjectId::ColumnId(index), false);
  requireEditable(document, object);
  TXshLevel* level = document.level(requiredString(params, "level_id"));
  if (column->getSoundColumn() || level->getSoundLevel())
    throw Error("incompatible_column",
                "Place sound with audio.clip.place, not drawing exposures.");
  const TFrameId frame = readDrawingId(params);
  validateDrawing(level, frame);
  const CellRange range = readRange(params);
  const TXshCell cell(level, frame);
  TXshLevelColumn* target = column->getLevelColumn();
  if (!target || !target->canSetCell(cell))
    throw Error("incompatible_column",
                "This column cannot expose the requested level type.");
  const bool overwrite = boolParam(params, "overwrite", false);
  TXsheet* sheet       = document.scene->getXsheet();
  if (preflightCells(sheet, index, range, cell, overwrite)) {
    CellPlan plan;
    plan.column   = column;
    plan.index    = index;
    plan.columnId = document.idForColumn(column);
    plan.before   = CellModel::read(*sheet, index);
    plan.ops.push_back({CellOp::Write, range.start, range.length,
                        std::vector<TXshCell>(range.length, cell)});
    modelCellPlan(plan);
    std::vector<CellPlan> plans{plan};
    context.markMutationStarted();
    applyCellPlans(document, plans);
  }
  return QJsonObject{{"column_id", document.idForColumn(column)},
                     {"start_frame", range.start},
                     {"length", range.length},
                     {"level_id", document.idForLevel(level)},
                     {"drawing_id", drawingId(frame)}};
}

QJsonValue inspectCells(Context& context, const QJsonObject& params) {
  Document& document = getDocument(context, params);
  TXshColumn* column = document.column(requiredString(params, "column_id"));
  if (column->getSoundColumn())
    throw Error("incompatible_column",
                "Sound columns have clips, not cells; use audio.inspect.");
  const int index       = columnIndex(document, column);
  const CellRange range = readRange(params);
  QJsonArray cells;
  for (int row = range.start; row < range.start + range.length; ++row) {
    const TXshCell& cell = document.scene->getXsheet()->getCell(row, index);
    QJsonObject value{{"frame", row}, {"empty", cell.isEmpty()}};
    if (!cell.isEmpty()) {
      value.insert("level_id", document.idForLevel(cell.m_level.getPointer()));
      value.insert("drawing_id", drawingId(cell.m_frameId));
    }
    cells.append(value);
  }
  return QJsonObject{{"column_id", document.idForColumn(column)},
                     {"cells", cells}};
}

}  // namespace

void addCellOperations(QJsonArray& catalog) {
  QJsonObject column = merge(documentSchema(), {{"column_id", textSchema()}});
  QJsonObject range =
      merge(column, {{"start_frame", integerSchema(0, MaxFrame)},
                     {"length", integerSchema(1, MaxSpan)}});
  catalog.append(operation(
      "column.cells", "Read an interval of zero-based timeline cells.", range,
      {"document_id", "column_id", "start_frame", "length"}));
  const QJsonObject drawing{
      {"type", "object"},
      {"additionalProperties", false},
      {"properties",
       QJsonObject{{"number", integerSchema(1, 999999)},
                   {"suffix", QJsonObject{{"type", "string"},
                                          {"pattern", "^[a-z]{0,8}$"}}}}},
      {"required", QJsonArray{"number", "suffix"}}};
  range.insert("drawing_id", drawing);
  range.insert("level_id", textSchema());
  range.insert("overwrite", QJsonObject{{"type", "boolean"}});
  catalog.append(operation(
      "column.expose",
      "Expose an existing drawing as a hold; frames start at 0.", range,
      {"document_id", "column_id", "level_id", "drawing_id", "start_frame",
       "length"},
      true));
}

void addCellHandlers(SceneHandlers& handlers) {
  handlers.insert(
      {{"column.expose", exposeColumn}, {"column.cells", inspectCells}});
}

}  // namespace SceneShared
}  // namespace GroundzeroAutomation
