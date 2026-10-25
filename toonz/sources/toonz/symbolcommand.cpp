#include "symbolcommand.h"

// Tnz6 includes
#include "tapp.h"
#include "menubarcommandids.h"
#include "subscenecommand.h"
#include "columncommand.h"

// TnzQt includes
#include "toonzqt/dvdialog.h"
#include "toonzqt/menubarcommand.h"
#include "toonzqt/selection.h"
#include "historytypes.h"

// TnzLib includes
#include "toonz/toonzscene.h"
#include "toonz/levelset.h"
#include "toonz/txsheet.h"
#include "toonz/txshcell.h"
#include "toonz/txshcolumn.h"
#include "toonz/txshlevelcolumn.h"
#include "toonz/txshchildlevel.h"
#include "toonz/txsheethandle.h"
#include "toonz/tscenehandle.h"
#include "toonz/tcolumnhandle.h"
#include "toonz/tframehandle.h"

// TnzCore includes
#include "tundo.h"

// Qt includes
#include <QComboBox>
#include <QSpinBox>
#include <QLabel>
#include <QPushButton>
#include <QGridLayout>
#include <QMainWindow>

// STD includes
#include <algorithm>
#include <vector>

//*****************************************************************************
//    Local namespace
//*****************************************************************************

namespace {

TXsheet *currentXsheet() {
  return TApp::instance()->getCurrentXsheet()->getXsheet();
}

//-----------------------------------------------------------------------------

void notifyXsheetEdited() {
  TApp *app = TApp::instance();
  app->getCurrentXsheet()->notifyXsheetChanged();
  app->getCurrentScene()->setDirtyFlag(true);
}

//-----------------------------------------------------------------------------

int getSymbolFrameCount(TXshChildLevel *symbol) {
  return std::max(1, symbol->getFrameCount());
}

//-----------------------------------------------------------------------------

// Returns a column of the xsheet exposing the symbol, trying the current
// column first, and in row the first row exposing it; -1 if there is none.
// With onlySymbol the column must not expose other levels.
int findInstanceColumn(TXsheet *xsh, TXshChildLevel *symbol, int &row,
                       bool onlySymbol) {
  int current  = TApp::instance()->getCurrentColumn()->getColumnIndex();
  int colCount = xsh->getColumnCount();
  for (int i = -1; i < colCount; i++) {
    int c = (i < 0) ? current : i;
    if (c < 0 || c >= colCount || (i >= 0 && c == current)) continue;
    TXshColumn *column       = xsh->getColumn(c);
    TXshLevelColumn *lcolumn = column ? column->getLevelColumn() : 0;
    if (!lcolumn) continue;
    int r0 = 0, r1 = -1;
    if (lcolumn->getRange(r0, r1) == 0) continue;
    int firstRow = -1;
    bool other   = false;
    for (int r = r0; r <= r1; r++) {
      TXshCell cell = lcolumn->getCell(r);
      if (cell.isEmpty()) continue;
      if (cell.getChildLevel() == symbol) {
        if (firstRow < 0) firstRow = r;
      } else
        other = true;
    }
    if (firstRow < 0 || (onlySymbol && other)) continue;
    row = firstRow;
    return c;
  }
  return -1;
}

//-----------------------------------------------------------------------------

// Returns the symbol exposed by all the non-empty cells r0..r1 of the column,
// or 0 if the column is locked or the cells are not all of one symbol.
TXshChildLevel *getInstanceSymbol(TXsheet *xsh, int r0, int r1, int col) {
  TXshColumn *column = xsh->getColumn(col);
  if (!column || !column->getLevelColumn() || column->isLocked()) return 0;
  TXshChildLevel *symbol = 0;
  for (int r = r0; r <= r1; r++) {
    TXshCell cell = xsh->getCell(r, col);
    if (cell.isEmpty()) continue;
    TXshChildLevel *childLevel = cell.getChildLevel();
    if (!childLevel || (symbol && childLevel != symbol)) return 0;
    symbol = childLevel;
  }
  return symbol;
}

//=============================================================================
// InsertInstanceUndo
//-----------------------------------------------------------------------------

class InsertInstanceUndo final : public TUndo {
  TXshChildLevelP m_symbol;
  int m_row, m_col, m_frameCount;

public:
  InsertInstanceUndo(TXshChildLevel *symbol, int row, int col)
      : m_symbol(symbol)
      , m_row(row)
      , m_col(col)
      , m_frameCount(getSymbolFrameCount(symbol)) {}

  void undo() const override {
    TXsheet *xsh = currentXsheet();
    xsh->removeColumn(m_col);
    xsh->updateFrameCount();
    notifyXsheetEdited();
  }

  void redo() const override {
    TXsheet *xsh = currentXsheet();
    xsh->insertColumn(m_col);
    for (int r = 0; r < m_frameCount; r++)
      xsh->setCell(m_row + r, m_col,
                   TXshCell(m_symbol.getPointer(), TFrameId(r + 1)));
    xsh->updateFrameCount();
    notifyXsheetEdited();
  }

  int getSize() const override { return sizeof(*this); }

  QString getHistoryString() override {
    return QObject::tr("Insert Symbol Instance : %1")
        .arg(QString::fromStdWString(m_symbol->getName()));
  }
  int getHistoryType() override { return HistoryType::Xsheet; }
};

//=============================================================================
// RenameSymbolUndo
//-----------------------------------------------------------------------------

class RenameSymbolUndo final : public TUndo {
  TXshChildLevelP m_symbol;
  std::wstring m_oldName, m_newName;

  void setName(const std::wstring &name) const {
    TApp *app = TApp::instance();
    app->getCurrentScene()->getScene()->getLevelSet()->renameLevel(
        m_symbol.getPointer(), name);
    notifyXsheetEdited();
    app->getCurrentScene()->notifyCastChange();
  }

public:
  RenameSymbolUndo(TXshChildLevel *symbol, const std::wstring &oldName,
                   const std::wstring &newName)
      : m_symbol(symbol), m_oldName(oldName), m_newName(newName) {}

  void undo() const override { setName(m_oldName); }
  void redo() const override { setName(m_newName); }

  int getSize() const override { return sizeof(*this); }

  QString getHistoryString() override {
    return QObject::tr("Rename Symbol : %1 > %2")
        .arg(QString::fromStdWString(m_oldName))
        .arg(QString::fromStdWString(m_newName));
  }
  int getHistoryType() override { return HistoryType::Xsheet; }
};

//=============================================================================
// InstanceTimingUndo
//-----------------------------------------------------------------------------

class InstanceTimingUndo final : public TUndo {
  int m_r0;
  std::vector<int> m_cols;
  std::vector<std::vector<TXshCell>> m_oldCells, m_newCells;

  void setCells(const std::vector<std::vector<TXshCell>> &cells) const {
    TXsheet *xsh = currentXsheet();
    for (int i = 0; i < (int)m_cols.size(); i++)
      xsh->setCells(m_r0, m_cols[i], cells[i].size(), &cells[i][0]);
    xsh->updateFrameCount();
    notifyXsheetEdited();
  }

public:
  InstanceTimingUndo(int r0) : m_r0(r0) {}

  void addColumn(int col, const std::vector<TXshCell> &oldCells,
                 const std::vector<TXshCell> &newCells) {
    m_cols.push_back(col);
    m_oldCells.push_back(oldCells);
    m_newCells.push_back(newCells);
  }
  bool isEmpty() const { return m_cols.empty(); }

  void undo() const override { setCells(m_oldCells); }
  void redo() const override { setCells(m_newCells); }

  int getSize() const override {
    int rowCount = m_oldCells.empty() ? 0 : m_oldCells[0].size();
    return sizeof(*this) + 2 * m_cols.size() * rowCount * sizeof(TXshCell);
  }

  QString getHistoryString() override { return QObject::tr("Symbol Timing"); }
  int getHistoryType() override { return HistoryType::Xsheet; }
};

}  // namespace

//*****************************************************************************
//    SymbolCmd
//*****************************************************************************

void SymbolCmd::convertToSymbol(std::set<int> &indices) {
  indices.erase(-1);  // ignore the camera column
  if (indices.empty()) return;

  bool ok = false;
  QString name =
      DVGui::getText(QObject::tr("Convert to Symbol"),
                     QObject::tr("Symbol Name:"), QObject::tr("Symbol"), &ok)
          .trimmed();
  if (!ok) return;
  if (name.isEmpty()) name = QObject::tr("Symbol");

  // the name is made unique when the sub-xsheet level is created
  SubsceneCmd::collapse(indices, name.toStdWString());
}

//-----------------------------------------------------------------------------

bool SymbolCmd::insertInstance(TXshChildLevel *symbol, int row, int col) {
  if (!symbol) return false;
  TApp *app    = TApp::instance();
  TXsheet *xsh = currentXsheet();
  if (xsh->checkCircularReferences(TXshCell(symbol, TFrameId(1)))) {
    DVGui::warning(
        QObject::tr("The symbol %1 cannot be inserted here, because it "
                    "contains the current xsheet.")
            .arg(QString::fromStdWString(symbol->getName())));
    return false;
  }

  TUndo *undo =
      new InsertInstanceUndo(symbol, std::max(0, row), std::max(0, col));
  undo->redo();
  TUndoManager::manager()->add(undo);

  app->getCurrentColumn()->setColumnIndex(std::max(0, col));
  return true;
}

//-----------------------------------------------------------------------------

void SymbolCmd::editSymbol(TXshChildLevel *symbol) {
  if (!symbol) return;
  TApp *app    = TApp::instance();
  TXsheet *xsh = currentXsheet();
  int row      = 0;
  int col      = findInstanceColumn(xsh, symbol, row, false);
  if (col < 0) {
    DVGui::warning(
        QObject::tr("The symbol %1 has no instance in the current xsheet. "
                    "Insert an instance of it to edit it from here.")
            .arg(QString::fromStdWString(symbol->getName())));
    return;
  }

  // keep the current frame if it shows the symbol
  int frame = app->getCurrentFrame()->getFrame();
  if (frame >= 0 && xsh->getCell(frame, col).getChildLevel() == symbol)
    row = frame;

  // Open Sub-Xsheet opens the current cell when nothing is selected
  if (TSelection::getCurrent()) TSelection::getCurrent()->selectNone();
  app->getCurrentColumn()->setColumnIndex(col);
  app->getCurrentFrame()->setFrame(row);
  CommandManager::instance()->execute(MI_OpenChild);
}

//-----------------------------------------------------------------------------

void SymbolCmd::duplicateSymbol(TXshChildLevel *symbol) {
  if (!symbol) return;
  TXsheet *xsh = currentXsheet();
  int row      = 0;
  int col      = findInstanceColumn(xsh, symbol, row, true);
  if (col < 0) {
    DVGui::warning(
        QObject::tr("To duplicate the symbol %1, the current xsheet needs a "
                    "column exposing only this symbol.")
            .arg(QString::fromStdWString(symbol->getName())));
    return;
  }

  TUndoManager::manager()->beginBlock();
  // Clone Sub-Xsheet puts the copy in a new column next to the instance
  ColumnCmd::cloneChild(col);

  // name the copy after the symbol
  TXshChildLevel *copy = xsh->getCell(row, col + 1).getChildLevel();
  if (copy && copy != symbol) {
    TLevelSet *levelSet =
        TApp::instance()->getCurrentScene()->getScene()->getLevelSet();
    std::wstring name;
    for (int i = 2;; i++) {
      name = symbol->getName() + L"_" + std::to_wstring(i);
      if (!levelSet->hasLevel(name)) break;
    }
    renameSymbol(copy, name);
  }
  TUndoManager::manager()->endBlock();
}

//-----------------------------------------------------------------------------

bool SymbolCmd::renameSymbol(TXshChildLevel *symbol, const std::wstring &name) {
  if (!symbol || name.empty()) return false;
  std::wstring oldName = symbol->getName();
  if (oldName == name) return true;
  TLevelSet *levelSet =
      TApp::instance()->getCurrentScene()->getScene()->getLevelSet();
  if (levelSet->hasLevel(name)) return false;

  TUndo *undo = new RenameSymbolUndo(symbol, oldName, name);
  undo->redo();
  TUndoManager::manager()->add(undo);
  return true;
}

//-----------------------------------------------------------------------------

bool SymbolCmd::setInstanceTiming(int r0, int c0, int r1, int c1,
                                  TimingMode mode, int startFrame) {
  if (r0 > r1) std::swap(r0, r1);
  if (c0 > c1) std::swap(c0, c1);
  r0 = std::max(0, r0);
  c0 = std::max(0, c0);
  if (r1 < r0 || c1 < c0) return false;

  TXsheet *xsh = currentXsheet();
  int rowCount = r1 - r0 + 1;

  InstanceTimingUndo *undo = new InstanceTimingUndo(r0);
  for (int c = c0; c <= c1; c++) {
    TXshChildLevel *symbol = getInstanceSymbol(xsh, r0, r1, c);
    if (!symbol) continue;
    int frameCount = getSymbolFrameCount(symbol);
    int start      = std::min(std::max(startFrame, 1), frameCount);

    std::vector<TXshCell> oldCells(rowCount), newCells(rowCount);
    xsh->getCells(r0, c, rowCount, &oldCells[0]);
    for (int i = 0; i < rowCount; i++) {
      int frame;
      if (mode == Hold)
        frame = start;
      else if (mode == Loop)
        frame = start + i % (frameCount - start + 1);
      else  // PlayOnce: then hold the last frame
        frame = std::min(start + i, frameCount);
      newCells[i] = TXshCell(symbol, TFrameId(frame));
    }
    undo->addColumn(c, oldCells, newCells);
  }

  if (undo->isEmpty()) {
    delete undo;
    return false;
  }
  undo->redo();
  TUndoManager::manager()->add(undo);
  return true;
}

//-----------------------------------------------------------------------------

void SymbolCmd::openInstanceTimingDialog(int r0, int c0, int r1, int c1) {
  if (r0 > r1) std::swap(r0, r1);
  if (c0 > c1) std::swap(c0, c1);
  r0           = std::max(0, r0);
  c0           = std::max(0, c0);
  TXsheet *xsh = currentXsheet();

  // the longest symbol limits the start frame; by default the timing starts
  // from the frame of the first selected cell
  int maxFrameCount = 0, firstFrame = 1;
  for (int c = c0; c <= c1; c++) {
    TXshChildLevel *symbol = getInstanceSymbol(xsh, r0, r1, c);
    if (!symbol) continue;
    if (maxFrameCount == 0) {
      TXshCell cell = xsh->getCell(r0, c);
      if (!cell.isEmpty()) firstFrame = cell.getFrameId().getNumber();
    }
    maxFrameCount = std::max(maxFrameCount, getSymbolFrameCount(symbol));
  }
  if (maxFrameCount == 0) {
    DVGui::warning(
        QObject::tr("Select the cells of a symbol instance (a column exposing "
                    "a single sub-xsheet)."));
    return;
  }

  DVGui::Dialog dialog(TApp::instance()->getMainWindow(), true);
  dialog.setWindowTitle(QObject::tr("Symbol Timing"));

  QComboBox *modeCombo = new QComboBox(&dialog);
  modeCombo->addItem(QObject::tr("Play Once"), (int)PlayOnce);
  modeCombo->addItem(QObject::tr("Loop"), (int)Loop);
  modeCombo->addItem(QObject::tr("Hold"), (int)Hold);

  QSpinBox *startField = new QSpinBox(&dialog);
  startField->setRange(1, maxFrameCount);
  startField->setValue(std::min(std::max(firstFrame, 1), maxFrameCount));

  QGridLayout *layout = new QGridLayout();
  layout->addWidget(new QLabel(QObject::tr("Mode:"), &dialog), 0, 0,
                    Qt::AlignRight | Qt::AlignVCenter);
  layout->addWidget(modeCombo, 0, 1);
  layout->addWidget(new QLabel(QObject::tr("From Symbol Frame:"), &dialog), 1,
                    0, Qt::AlignRight | Qt::AlignVCenter);
  layout->addWidget(startField, 1, 1);
  dialog.addLayout(layout);

  QPushButton *okBtn = new QPushButton(QObject::tr("Apply"), &dialog);
  okBtn->setDefault(true);
  QPushButton *cancelBtn = new QPushButton(QObject::tr("Cancel"), &dialog);
  QObject::connect(okBtn, &QPushButton::clicked, &dialog, &QDialog::accept);
  QObject::connect(cancelBtn, &QPushButton::clicked, &dialog, &QDialog::reject);
  dialog.addButtonBarWidget(okBtn, cancelBtn);

  if (dialog.exec() != QDialog::Accepted) return;

  setInstanceTiming(r0, c0, r1, c1,
                    (TimingMode)modeCombo->currentData().toInt(),
                    startField->value());
}
