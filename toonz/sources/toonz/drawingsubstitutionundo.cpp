#include "toonzqt/uitranslations.h"
#include "drawingsubstitutionundo.h"

// Tnz6 includes
#include "tapp.h"
#include "cellselection.h"

// TnzQt includes
#include "toonzqt/tselectionhandle.h"

// TnzLib includes
#include "toonz/txsheet.h"
#include "toonz/txshcolumn.h"
#include "toonz/txshsimplelevel.h"
#include "toonz/txshchildlevel.h"
#include "toonz/tscenehandle.h"
#include "toonz/txsheethandle.h"
#include "toonz/tframehandle.h"
#include "toonz/tcolumnhandle.h"

// TnzCore includes
#include "historytypes.h"

// Qt includes
#include <QObject>

// STD includes
#include <algorithm>

namespace {

void notifyChanged() {
  TApp::instance()->getCurrentXsheet()->notifyXsheetChanged();
  TApp::instance()->getCurrentScene()->setDirtyFlag(true);
}

}  // namespace

//=============================================================================
// SubstituteDrawingUndo
//-----------------------------------------------------------------------------

void SubstituteDrawingUndo::addCell(int row, int col, const TXshCell &oldCell) {
  CellEntry entry = {row, col, oldCell};
  m_cells.push_back(entry);
}

//-----------------------------------------------------------------------------

void SubstituteDrawingUndo::undo() const {
  TXsheet *xsh = TApp::instance()->getCurrentXsheet()->getXsheet();
  for (const CellEntry &entry : m_cells)
    xsh->setCell(entry.m_row, entry.m_col, entry.m_oldCell);
  notifyChanged();
}

//-----------------------------------------------------------------------------

void SubstituteDrawingUndo::redo() const {
  TXsheet *xsh = TApp::instance()->getCurrentXsheet()->getXsheet();
  for (const CellEntry &entry : m_cells)
    xsh->setCell(entry.m_row, entry.m_col, m_newCell);
  notifyChanged();
}

//-----------------------------------------------------------------------------

int SubstituteDrawingUndo::getSize() const {
  return sizeof(*this) + m_cells.size() * sizeof(CellEntry);
}

//-----------------------------------------------------------------------------

QString SubstituteDrawingUndo::getHistoryString() {
  return UiTranslations::text(QObject::tr("Drawing Substitution  Level : %1  Frame : %2"))
      .arg(QString::fromStdWString(m_newCell.m_level->getName()))
      .arg(
          QString::fromStdString(m_newCell.m_frameId.expand(TFrameId::NO_PAD)));
}

//-----------------------------------------------------------------------------

int SubstituteDrawingUndo::getHistoryType() { return HistoryType::Xsheet; }

//-----------------------------------------------------------------------------

SubstituteDrawingUndo *SubstituteDrawingUndo::create(const TXshLevelP &level,
                                                     const TFrameId &fid,
                                                     bool skipSameFrame) {
  TApp *app    = TApp::instance();
  TXsheet *xsh = app->getCurrentXsheet()->getXsheet();
  if (!xsh || !level) return 0;

  int r0 = app->getCurrentFrame()->getFrame();
  int c0 = app->getCurrentColumn()->getColumnIndex();
  int r1 = r0, c1 = c0;

  TCellSelection *selection = dynamic_cast<TCellSelection *>(
      app->getCurrentSelection()->getSelection());
  if (selection) {
    TCellSelection::Range range = selection->getSelectedCells();
    if (!range.isEmpty()) {
      r0 = std::min(range.m_r0, range.m_r1);
      r1 = std::max(range.m_r0, range.m_r1);
      c0 = std::min(range.m_c0, range.m_c1);
      c1 = std::max(range.m_c0, range.m_c1);
    }
  }
  r0 = std::max(r0, 0);
  c0 = std::max(c0, 0);

  SubstituteDrawingUndo *undo = new SubstituteDrawingUndo(TXshCell(level, fid));
  for (int c = c0; c <= c1; c++) {
    TXshColumn *column = xsh->getColumn(c);
    if (!column || column->isLocked()) continue;
    for (int r = r0; r <= r1; r++) {
      TXshCell cell = xsh->getCell(r, c);
      if (cell.isEmpty() || cell.m_level.getPointer() != level.getPointer() ||
          (skipSameFrame && cell.m_frameId == fid))
        continue;
      undo->addCell(r, c, cell);
    }
  }

  if (undo->isEmpty()) {
    delete undo;
    return 0;
  }
  return undo;
}

//-----------------------------------------------------------------------------

void getSubstitutionCurrentCell(TXshLevelP &level, TFrameId &fid) {
  level = TXshLevelP();
  fid   = TFrameId();

  TApp *app    = TApp::instance();
  TXsheet *xsh = app->getCurrentXsheet()->getXsheet();
  int row      = app->getCurrentFrame()->getFrame();
  int col      = app->getCurrentColumn()->getColumnIndex();
  if (!xsh || row < 0 || col < 0) return;

  TXshCell cell = xsh->getCell(row, col);
  if (cell.isEmpty() || !cell.m_level) return;
  if (!cell.m_level->getSimpleLevel() && !cell.m_level->getChildLevel()) return;

  level = cell.m_level;
  fid   = cell.m_frameId;
}
