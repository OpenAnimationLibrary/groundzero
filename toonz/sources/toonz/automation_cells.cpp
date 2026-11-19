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
  model.trim();
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

// Drop the blank cells at both ends, as every native primitive does.
void CellModel::trim() {
  auto head = m_cells.begin();
  while (head != m_cells.end() && head->isEmpty()) ++head;
  m_first += int(head - m_cells.begin());
  m_cells.erase(m_cells.begin(), head);
  while (!m_cells.empty() && m_cells.back().isEmpty()) m_cells.pop_back();
  if (m_cells.empty()) m_first = 0;
}

void CellModel::write(int row, const std::vector<TXshCell>& cells) {
  if (cells.empty()) return;
  const int end = row + int(cells.size()) - 1;
  if (empty()) {
    m_first = row;
    m_cells = cells;
    trim();
    return;
  }
  if (row < m_first) {
    m_cells.insert(m_cells.begin(), m_first - row, TXshCell());
    m_first = row;
  }
  if (end > last()) m_cells.resize(end - m_first + 1);
  std::copy(cells.begin(), cells.end(), m_cells.begin() + (row - m_first));
  trim();
}

void CellModel::set(int row, const TXshCell& cell) {
  if (row >= m_first && row <= last() && !cell.isEmpty()) {
    m_cells[row - m_first] = cell;
    return;
  }
  write(row, {cell});
}

void CellModel::clear(int row, int count) {
  if (empty() || count <= 0) return;
  const int from = std::max(row, m_first);
  const int to   = std::min(row + count - 1, last());
  for (int at = from; at <= to; ++at) m_cells[at - m_first] = TXshCell();
  trim();
}

void CellModel::remove(int row, int count) {
  if (empty() || count <= 0 || row > last()) return;
  if (row < m_first) {
    if (row + count <= m_first) {
      m_first -= count;
      return;
    }
    count -= m_first - row;
    m_first = row;
  }
  count            = std::min(count, last() - row + 1);
  const auto begin = m_cells.begin() + (row - m_first);
  m_cells.erase(begin, begin + count);
  trim();
}

void CellModel::insert(int row, int count) {
  if (empty() || count <= 0 || row > last()) return;
  if (row <= m_first) {
    m_first += count;
    return;
  }
  m_cells.insert(m_cells.begin() + (row - m_first), count, TXshCell());
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
    case CellOp::Composite:
      op.model(plan.after);
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
  case CellOp::Composite:
    op.native(sheet, index);
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

// Rows 0 to 999999: the native sheet holds at most 1,000,000 rows
// (TXsheet::maxRowCount), the bound the save profile checks.
constexpr int LastRow = 999999;

CellRange readRange(const QJsonObject& params) {
  const int start  = integerParam(params, "start_frame", 0, LastRow, -1);
  const int length = integerParam(params, "length", 1, MaxSpan, -1);
  if (start < 0 || length < 1 || start > LastRow - length + 1)
    throw Error("invalid_params",
                "The interval ends past frame 999999, the last native row.",
                {{"reason", "frame_range"}, {"frame", start + length - 1}});
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
  QJsonObject range = merge(column, {{"start_frame", integerSchema(0, LastRow)},
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
