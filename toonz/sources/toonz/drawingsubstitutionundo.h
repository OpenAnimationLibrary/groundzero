#pragma once

#ifndef DRAWINGSUBSTITUTIONUNDO_H
#define DRAWINGSUBSTITUTIONUNDO_H

#include "toonz/txshcell.h"
#include "toonz/txshlevel.h"
#include "tundo.h"

#include <vector>

//=============================================================================
// SubstituteDrawingUndo
//-----------------------------------------------------------------------------
/*! Exposes one drawing in a set of cells, remembering their previous
    content. Shared by the Drawing Substitution panel and the Substitution
    Slider.                                                               */

class SubstituteDrawingUndo final : public TUndo {
  struct CellEntry {
    int m_row, m_col;
    TXshCell m_oldCell;
  };
  std::vector<CellEntry> m_cells;
  TXshCell m_newCell;

public:
  SubstituteDrawingUndo(const TXshCell &newCell) : m_newCell(newCell) {}

  void addCell(int row, int col, const TXshCell &oldCell);
  bool isEmpty() const { return m_cells.empty(); }
  void setNewCell(const TXshCell &newCell) { m_newCell = newCell; }

  void undo() const override;
  void redo() const override;
  int getSize() const override;
  QString getHistoryString() override;
  int getHistoryType() override;

  /*! Builds an undo for the cells which hold \b level in the current cell
      selection (or in the current cell when nothing is selected). Empty
      cells, cells of other levels and locked columns are skipped. If
      \b skipSameFrame is true the cells already exposing \b fid are skipped
      too. The cells are not modified. Returns 0 if no cell is eligible. */
  static SubstituteDrawingUndo *create(const TXshLevelP &level,
                                       const TFrameId &fid, bool skipSameFrame);
};

//-----------------------------------------------------------------------------
/*! Returns the level and the frame id exposed in the current cell, or a null
    level if the cell does not hold a drawing level or a sub-xsheet.       */

void getSubstitutionCurrentCell(TXshLevelP &level, TFrameId &fid);

#endif  // DRAWINGSUBSTITUTIONUNDO_H
