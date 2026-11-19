#pragma once

#include "automation.h"

#include "toonz/txshcell.h"

#include <QJsonObject>
#include <QString>

#include <functional>
#include <vector>

class TXsheet;

namespace GroundzeroAutomation {

/// The cells of one column as the native column stores them: a first row and
/// a run of cells with no blank cell at either end.
///
/// The model implements the native cell primitives independently, so the
/// worker can check every bound on the prospective cells before the first
/// native change and compare the native result with them afterwards.
class CellModel final {
public:
  /// Read the cells of the column at a sheet index.
  static CellModel read(TXsheet& sheet, int index);

  bool empty() const { return m_cells.empty(); }
  /// The first non-empty row; 0 when empty.
  int first() const { return m_first; }
  /// The last non-empty row; first() - 1 when empty.
  int last() const { return m_first + int(m_cells.size()) - 1; }
  /// Rows from the first to the last cell, as the save profile counts them.
  int span() const { return int(m_cells.size()); }
  TXshCell at(int row) const;
  bool equals(const CellModel& other) const;
  /// The first row at which two models differ, or -1.
  int firstDifference(const CellModel& other) const;
  /// Rows whose cell differs between two models.
  int changedRows(const CellModel& other) const;

  /// Write cells from a row on, blanks included; nothing shifts.
  void write(int row, const std::vector<TXshCell>& cells);
  /// Blank rows without shifting (TXshCellColumn::clearCells).
  void clear(int row, int count);
  /// Remove rows and shift later cells up (TXshCellColumn::removeCells).
  void remove(int row, int count);
  /// Insert blank rows and shift later cells down; nothing happens on an
  /// empty column or past the last cell (TXshCellColumn::insertEmptyCells).
  void insert(int row, int count);

  /// Write one cell; a blank above the first or below the last cell
  /// changes nothing (TXsheet::setCell).
  void set(int row, const TXshCell& cell);

private:
  void trim();

  int m_first = 0;
  std::vector<TXshCell> m_cells;
};

/// One native cell primitive on one column.
///
/// A composite op is one native range function (reverse, step, reframe and
/// so on): `native` calls it on the sheet, `model` applies the worker's port
/// of it, built from the model's primitives, to the model.
struct CellOp final {
  enum Kind { Write, Clear, Remove, Insert, Composite };
  Kind kind;
  int row;
  int count;
  std::vector<TXshCell> cells;
  std::function<void(CellModel&)> model;
  std::function<void(TXsheet&, int)> native;
};

/// The edit of one column: its primitives, and its cells before and after.
struct CellPlan final {
  TXshColumn* column = nullptr;
  int index          = 0;
  QString columnId;
  CellModel before;
  CellModel after;
  std::vector<CellOp> ops;
};

/// Apply a plan's primitives to its model, filling `after` from `before`.
void modelCellPlan(CellPlan& plan);

/// Apply every plan natively, restore column names and compare each column
/// with its model.
///
/// The caller has checked every bound. The mutation is marked started just
/// before each native write, so a refusal of the first write is a plain
/// rejection with nothing changed, and a later failure lists the writes that
/// landed under `completed`. A write fault armed at `native:set-cells` refuses
/// the first write, as `TXsheet::setCells` does for a cell it will not take;
/// armed at `native:set-cells-later` it refuses the first write after one has
/// landed.
///
/// Throws:
///   Error: `native_error` naming the column and first differing row when a
///     native call refuses or the native cells differ from the model.
void applyCellPlans(Context& context, Document& document,
                    std::vector<CellPlan>& plans);

/// Run the cell model domain tests.
void runAutomationCellsTests(Context& context);

}  // namespace GroundzeroAutomation
