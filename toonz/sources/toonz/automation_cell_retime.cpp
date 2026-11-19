#include "automation_cell_retime.h"

#include "toonz/txsheet.h"

#include <algorithm>
#include <cmath>
#include <vector>

namespace GroundzeroAutomation {
namespace {

// Each port follows its TXsheet function (toonzlib/txsheet.cpp) step for step
// on one column, with the model's primitives in place of the native ones.

void reverse(CellModel& cells, int r0, int r1) {
  for (int i1 = r0, i2 = r1; i1 < i2; i1++, i2--) {
    const TXshCell first = cells.at(i1);
    const TXshCell last  = cells.at(i2);
    cells.set(i1, last);
    cells.set(i2, first);
  }
}

void swing(CellModel& cells, int r0, int r1) {
  const int moved = r1 + 1;
  cells.insert(moved, r1 - r0);
  for (int i1 = moved, i2 = r1 - 1; i2 >= r0; i1++, i2--)
    cells.set(i1, cells.at(i2));
}

void setOrClear(CellModel& cells, int row, const TXshCell& cell) {
  if (cell.isEmpty())
    cells.clear(row, 1);
  else
    cells.set(row, cell);
}

std::vector<TXshCell> rows(const CellModel& cells, int r0, int r1) {
  std::vector<TXshCell> result;
  for (int row = r0; row <= r1; ++row) result.push_back(cells.at(row));
  return result;
}

void step(CellModel& cells, int r0, int r1, int factor) {
  const std::vector<TXshCell> source = rows(cells, r0, r1);
  cells.insert(r1 + 1, int(source.size()) * (factor - 1));
  int row = r0;
  for (const TXshCell& cell : source) {
    for (int repeat = 0; repeat < factor; repeat++)
      setOrClear(cells, row + repeat, cell);
    row += factor;
  }
}

void each(CellModel& cells, int r0, int r1, int factor) {
  const int count = r1 - r0 + 1;
  if (count < factor) return;
  const int kept = count % factor ? count / factor + 1 : count / factor;
  std::vector<TXshCell> source;
  for (int index = 0; index < kept; ++index)
    source.push_back(cells.at(r0 + index * factor));
  cells.remove(r0 + kept, count - kept);
  for (int index = 0; index < kept; ++index)
    setOrClear(cells, r0 + index, source[index]);
}

void reframe(CellModel& cells, int r0, int r1, int step, int withBlank) {
  const int count = r1 - r0 + 1;
  if (count < 1) return;
  std::vector<TXshCell> order;
  for (int row = r0; row <= r1; ++row)
    if (order.empty() || order.back() != cells.at(row))
      order.push_back(cells.at(row));
  if (withBlank >= 0) {
    std::vector<TXshCell> drawn;
    for (const TXshCell& cell : order)
      if (!cell.isEmpty()) drawn.push_back(cell);
    order = drawn;
  }
  if (order.empty()) return;
  int total = int(order.size()) * step;
  if (withBlank > 0) total += int(order.size()) * withBlank * step;
  if (count < total)
    cells.insert(r1 + 1, total - count);
  else if (count > total)
    cells.remove(r0 + total, count - total);
  int row = r0;
  for (const TXshCell& cell : order) {
    for (int repeat = 0; repeat < step; repeat++)
      setOrClear(cells, row + repeat, cell);
    row += step;
    if (withBlank > 0) {
      for (int blank = 0; blank < withBlank * step; blank++)
        cells.clear(row + blank, 1);
      row += withBlank * step;
    }
  }
}

void increaseStep(CellModel& cells, int r0, int r1) {
  int row = r0;
  int end = r1;
  while (row <= end) {
    const TXshCell cell = cells.at(row);
    if (cell.isEmpty()) {
      row++;
      continue;
    }
    cells.insert(row, 1);
    cells.set(row, cell);
    end++;
    row++;
    while (cell == cells.at(row) && row <= end) row++;
  }
}

void decreaseStep(CellModel& cells, int r0, int r1) {
  int row = r0;
  int end = r1;
  while (row <= end) {
    const TXshCell cell = cells.at(row);
    row++;
    if (cell.isEmpty()) continue;
    bool removed = false;
    while (cell == cells.at(row) && row <= end) {
      if (removed) {
        row++;
        continue;
      }
      removed = true;
      cells.remove(row, 1);
      end--;
    }
  }
}

void resetStep(CellModel& cells, int r0, int r1) {
  std::vector<TXshCell> order;
  int row = r0;
  while (row <= r1) {
    const TXshCell cell = cells.at(row);
    order.push_back(cell);
    row++;
    while (cell == cells.at(row) && row <= r1) row++;
  }
  cells.remove(r0, r1 - r0 + 1);
  cells.insert(r0, int(order.size()));
  for (size_t index = 0; index < order.size(); ++index)
    cells.set(r0 + int(index), order[index]);
}

void repeat(CellModel& cells, int r0, int r1, int until) {
  const int chunk = r1 - r0 + 1;
  cells.insert(r1 + 1, until - r1);
  for (int row = r1 + 1; row <= until; ++row)
    cells.set(row, cells.at(r0 + (row - (r1 + 1)) % chunk));
}

void rollUp(CellModel& cells, int r0, int r1) {
  const TXshCell cell = cells.at(r0);
  cells.remove(r0, 1);
  cells.insert(r1, 1);
  cells.set(r1, cell);
}

void rollDown(CellModel& cells, int r0, int r1) {
  const TXshCell cell = cells.at(r1);
  cells.remove(r1, 1);
  cells.insert(r0, 1);
  cells.set(r0, cell);
}

// TXsheet::timeStretch clamps to the last cell, which rows from twice the
// length on would otherwise round past.
int stretchedSource(int row, int oldCount, int newCount) {
  return std::min(
      oldCount - 1,
      static_cast<int>(std::round(row * double(oldCount) / double(newCount))));
}

void timeStretch(CellModel& cells, int r0, int r1, int newCount) {
  const int oldCount                 = r1 - r0 + 1;
  const std::vector<TXshCell> source = rows(cells, r0, r1);
  if (newCount > oldCount) {
    cells.insert(r0 + 1, newCount - oldCount);
    for (int row = newCount - 1; row >= 0; row--) {
      const int from = stretchedSource(row, oldCount, newCount);
      if (from < row) cells.set(row + r0, source[from]);
    }
    return;
  }
  for (int row = 0; row < newCount; row++) {
    const int from = stretchedSource(row, oldCount, newCount);
    if (from > row) cells.set(row + r0, source[from]);
  }
  const int removed = oldCount - newCount;
  cells.remove(r1 - removed + 1, removed);
}

// The last row of a reframe interval extended over a hold that continues
// past it, as the editor's ReframeUndo does.
int heldEnd(const CellModel& cells, int r1) {
  int end             = r1;
  const TXshCell held = cells.at(r1);
  while (end < cells.last() && cells.at(end + 1) == held) end++;
  return end;
}

}  // namespace

CellOp retimeOp(const RetimeRequest& request, const CellModel& before) {
  const int r0 = request.start;
  int r1       = request.start + request.length - 1;
  CellOp op{CellOp::Composite, r0, request.length, {}, {}, {}};
  const int factor = request.factor;
  switch (request.kind) {
  case RetimeKind::Reverse:
    op.model  = [=](CellModel& cells) { reverse(cells, r0, r1); };
    op.native = [=](TXsheet& sheet, int c) {
      sheet.reverseCells(r0, c, r1, c);
    };
    break;
  case RetimeKind::Swing:
    op.model  = [=](CellModel& cells) { swing(cells, r0, r1); };
    op.native = [=](TXsheet& sheet, int c) { sheet.swingCells(r0, c, r1, c); };
    break;
  case RetimeKind::Step:
    op.model  = [=](CellModel& cells) { step(cells, r0, r1, factor); };
    op.native = [=](TXsheet& sheet, int c) {
      sheet.stepCells(r0, c, r1, c, factor);
    };
    break;
  case RetimeKind::Each:
    op.model  = [=](CellModel& cells) { each(cells, r0, r1, factor); };
    op.native = [=](TXsheet& sheet, int c) {
      sheet.eachCells(r0, c, r1, c, factor);
    };
    break;
  case RetimeKind::Reframe: {
    if (request.extendOverHold) r1 = heldEnd(before, r1);
    const int blank = request.withBlank;
    op.model = [=](CellModel& cells) { reframe(cells, r0, r1, factor, blank); };
    op.native = [=](TXsheet& sheet, int c) {
      sheet.reframeCells(r0, r1, c, factor, blank);
    };
    break;
  }
  case RetimeKind::IncreaseStep:
    op.model  = [=](CellModel& cells) { increaseStep(cells, r0, r1); };
    op.native = [=](TXsheet& sheet, int c) {
      int end = r1;
      sheet.increaseStepCells(r0, c, end, c);
    };
    break;
  case RetimeKind::DecreaseStep:
    op.model  = [=](CellModel& cells) { decreaseStep(cells, r0, r1); };
    op.native = [=](TXsheet& sheet, int c) {
      int end = r1;
      sheet.decreaseStepCells(r0, c, end, c);
    };
    break;
  case RetimeKind::ResetStep:
    op.model  = [=](CellModel& cells) { resetStep(cells, r0, r1); };
    op.native = [=](TXsheet& sheet, int c) {
      sheet.resetStepCells(r0, c, r1, c);
    };
    break;
  case RetimeKind::Repeat: {
    const int until = request.until;
    op.model        = [=](CellModel& cells) { repeat(cells, r0, r1, until); };
    op.native       = [=](TXsheet& sheet, int c) {
      sheet.duplicateCells(r0, c, r1, c, until);
    };
    break;
  }
  case RetimeKind::RollUp:
    op.model  = [=](CellModel& cells) { rollUp(cells, r0, r1); };
    op.native = [=](TXsheet& sheet, int c) { sheet.rollupCells(r0, c, r1, c); };
    break;
  case RetimeKind::RollDown:
    op.model  = [=](CellModel& cells) { rollDown(cells, r0, r1); };
    op.native = [=](TXsheet& sheet, int c) {
      sheet.rolldownCells(r0, c, r1, c);
    };
    break;
  case RetimeKind::TimeStretch: {
    const int count = request.newLength;
    op.model  = [=](CellModel& cells) { timeStretch(cells, r0, r1, count); };
    op.native = [=](TXsheet& sheet, int c) {
      sheet.timeStretch(r0, c, r1, c, count);
    };
    break;
  }
  }
  return op;
}

int retimeRowsAdded(const RetimeRequest& request) {
  const int length = request.length;
  switch (request.kind) {
  case RetimeKind::Swing:
    return length - 1;
  case RetimeKind::Step:
    return length * (request.factor - 1);
  case RetimeKind::Reframe:
    return std::max(
        0, length * request.factor * (1 + std::max(0, request.withBlank)) -
               length);
  case RetimeKind::IncreaseStep:
    return length;
  case RetimeKind::Repeat:
    return request.until - (request.start + length - 1);
  case RetimeKind::TimeStretch:
    return std::max(0, request.newLength - length);
  default:
    return 0;
  }
}

}  // namespace GroundzeroAutomation
