#include "drawingsubstitutionpane.h"

// Tnz6 includes
#include "tapp.h"
#include "cellselection.h"
#include "menubarcommandids.h"

// TnzQt includes
#include "toonzqt/icongenerator.h"
#include "toonzqt/menubarcommand.h"
#include "toonzqt/tselectionhandle.h"

// TnzLib includes
#include "toonz/txsheet.h"
#include "toonz/txshcell.h"
#include "toonz/txshcolumn.h"
#include "toonz/txshsimplelevel.h"
#include "toonz/txshchildlevel.h"
#include "toonz/tscenehandle.h"
#include "toonz/txsheethandle.h"
#include "toonz/tframehandle.h"
#include "toonz/tcolumnhandle.h"
#include "toonz/txshlevelhandle.h"

// TnzCore includes
#include "tundo.h"
#include "historytypes.h"

// Qt includes
#include <QHBoxLayout>
#include <QLabel>
#include <QListWidget>
#include <QPushButton>
#include <QSignalBlocker>
#include <QVBoxLayout>

// STD includes
#include <algorithm>

namespace {

//=============================================================================
// SubstituteDrawingUndo
//-----------------------------------------------------------------------------
/*! Exposes one drawing in a set of cells, remembering their previous
    content.                                                              */

class SubstituteDrawingUndo final : public TUndo {
  struct CellEntry {
    int m_row, m_col;
    TXshCell m_oldCell;
  };
  std::vector<CellEntry> m_cells;
  TXshCell m_newCell;

public:
  SubstituteDrawingUndo(const TXshCell &newCell) : m_newCell(newCell) {}

  void addCell(int row, int col, const TXshCell &oldCell) {
    CellEntry entry = {row, col, oldCell};
    m_cells.push_back(entry);
  }
  bool isEmpty() const { return m_cells.empty(); }

  void undo() const override {
    TXsheet *xsh = TApp::instance()->getCurrentXsheet()->getXsheet();
    for (const CellEntry &entry : m_cells)
      xsh->setCell(entry.m_row, entry.m_col, entry.m_oldCell);
    notify();
  }

  void redo() const override {
    TXsheet *xsh = TApp::instance()->getCurrentXsheet()->getXsheet();
    for (const CellEntry &entry : m_cells)
      xsh->setCell(entry.m_row, entry.m_col, m_newCell);
    notify();
  }

  int getSize() const override {
    return sizeof(*this) + m_cells.size() * sizeof(CellEntry);
  }

  QString getHistoryString() override {
    return QObject::tr("Drawing Substitution  Level : %1  Frame : %2")
        .arg(QString::fromStdWString(m_newCell.m_level->getName()))
        .arg(QString::fromStdString(
            m_newCell.m_frameId.expand(TFrameId::NO_PAD)));
  }
  int getHistoryType() override { return HistoryType::Xsheet; }

private:
  static void notify() {
    TApp::instance()->getCurrentXsheet()->notifyXsheetChanged();
    TApp::instance()->getCurrentScene()->setDirtyFlag(true);
  }
};

}  // namespace

//=============================================================================
// DrawingSubstitutionPane
//-----------------------------------------------------------------------------

DrawingSubstitutionPane::DrawingSubstitutionPane(QWidget *parent,
                                                 Qt::WindowFlags flags)
    : QFrame(parent, flags) {
  setObjectName("DrawingSubstitutionPane");

  m_levelLabel = new QLabel(this);

  m_drawingList = new QListWidget(this);
  m_drawingList->setViewMode(QListView::IconMode);
  m_drawingList->setResizeMode(QListView::Adjust);
  m_drawingList->setMovement(QListView::Static);
  m_drawingList->setSelectionMode(QAbstractItemView::SingleSelection);
  m_drawingList->setWrapping(true);
  m_drawingList->setSpacing(2);

  QPushButton *prevButton = new QPushButton(tr("Previous"), this);
  prevButton->setToolTip(
      tr("Expose the previous drawing of the level (Drawing Substitution "
         "Backward)"));
  prevButton->setFocusPolicy(Qt::NoFocus);
  QPushButton *nextButton = new QPushButton(tr("Next"), this);
  nextButton->setToolTip(
      tr("Expose the next drawing of the level (Drawing Substitution "
         "Forward)"));
  nextButton->setFocusPolicy(Qt::NoFocus);

  // layout
  QVBoxLayout *mainLayout = new QVBoxLayout();
  mainLayout->setContentsMargins(5, 5, 5, 5);
  mainLayout->setSpacing(5);
  {
    QHBoxLayout *topLayout = new QHBoxLayout();
    topLayout->setContentsMargins(0, 0, 0, 0);
    topLayout->setSpacing(5);
    {
      topLayout->addWidget(m_levelLabel, 1);
      topLayout->addWidget(prevButton);
      topLayout->addWidget(nextButton);
    }
    mainLayout->addLayout(topLayout);
    mainLayout->addWidget(m_drawingList, 1);
  }
  setLayout(mainLayout);

  connect(m_drawingList, SIGNAL(itemClicked(QListWidgetItem *)), this,
          SLOT(onItemClicked(QListWidgetItem *)));
  connect(prevButton, SIGNAL(clicked()), this, SLOT(onPrevious()));
  connect(nextButton, SIGNAL(clicked()), this, SLOT(onNext()));
}

//-----------------------------------------------------------------------------

void DrawingSubstitutionPane::showEvent(QShowEvent *) {
  TApp *app = TApp::instance();
  connect(app->getCurrentScene(), SIGNAL(sceneSwitched()), this,
          SLOT(refresh()), Qt::UniqueConnection);
  connect(app->getCurrentXsheet(), SIGNAL(xsheetSwitched()), this,
          SLOT(refresh()), Qt::UniqueConnection);
  connect(app->getCurrentXsheet(), SIGNAL(xsheetChanged()), this,
          SLOT(refresh()), Qt::UniqueConnection);
  connect(app->getCurrentFrame(), SIGNAL(frameSwitched()), this,
          SLOT(refresh()), Qt::UniqueConnection);
  connect(app->getCurrentColumn(), SIGNAL(columnIndexSwitched()), this,
          SLOT(refresh()), Qt::UniqueConnection);
  connect(app->getCurrentLevel(), SIGNAL(xshLevelChanged()), this,
          SLOT(refresh()), Qt::UniqueConnection);
  connect(IconGenerator::instance(), SIGNAL(iconGenerated()), this,
          SLOT(onIconGenerated()), Qt::UniqueConnection);
  refresh();
}

//-----------------------------------------------------------------------------

void DrawingSubstitutionPane::hideEvent(QHideEvent *) {
  TApp *app = TApp::instance();
  disconnect(app->getCurrentScene(), SIGNAL(sceneSwitched()), this,
             SLOT(refresh()));
  disconnect(app->getCurrentXsheet(), SIGNAL(xsheetSwitched()), this,
             SLOT(refresh()));
  disconnect(app->getCurrentXsheet(), SIGNAL(xsheetChanged()), this,
             SLOT(refresh()));
  disconnect(app->getCurrentFrame(), SIGNAL(frameSwitched()), this,
             SLOT(refresh()));
  disconnect(app->getCurrentColumn(), SIGNAL(columnIndexSwitched()), this,
             SLOT(refresh()));
  disconnect(app->getCurrentLevel(), SIGNAL(xshLevelChanged()), this,
             SLOT(refresh()));
  disconnect(IconGenerator::instance(), SIGNAL(iconGenerated()), this,
             SLOT(onIconGenerated()));

  // Do not keep the level alive while the panel is hidden.
  m_level = TXshLevelP();
  m_fids.clear();
  m_drawingList->clear();
}

//-----------------------------------------------------------------------------
/*! Returns the level and the frame id exposed in the current cell, or a null
    level if the cell does not hold a drawing level or a sub-xsheet.       */

void DrawingSubstitutionPane::getCurrentCell(TXshLevelP &level,
                                             TFrameId &fid) const {
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

//-----------------------------------------------------------------------------

void DrawingSubstitutionPane::refresh() {
  TXshLevelP level;
  TFrameId currentFid;
  getCurrentCell(level, currentFid);

  std::vector<TFrameId> fids;
  if (level) level->getFids(fids);

  // Rebuild the thumbnails only when the level or its drawings changed.
  if (level.getPointer() != m_level.getPointer() || fids != m_fids) {
    m_level = level;
    m_fids.swap(fids);
    rebuildList();
  }

  // highlight the drawing exposed in the current cell
  QSignalBlocker blocker(m_drawingList);
  m_drawingList->clearSelection();
  if (!m_level) return;
  std::vector<TFrameId>::const_iterator it =
      std::find(m_fids.begin(), m_fids.end(), currentFid);
  if (it == m_fids.end()) return;
  QListWidgetItem *item = m_drawingList->item(int(it - m_fids.begin()));
  if (!item) return;
  m_drawingList->setCurrentItem(item);
  m_drawingList->scrollToItem(item);
}

//-----------------------------------------------------------------------------

void DrawingSubstitutionPane::rebuildList() {
  m_drawingList->clear();

  if (!m_level) {
    m_levelLabel->setText(tr("No level in the current cell"));
    return;
  }
  m_levelLabel->setText(QString::fromStdWString(m_level->getName()));

  // Thumbnails are generated for drawing levels only: rendering every frame
  // of a sub-xsheet would be too expensive.
  bool useIcons = m_level->getSimpleLevel() != 0;
  if (useIcons) {
    TDimension iconSize = IconGenerator::instance()->getIconSize();
    m_drawingList->setIconSize(QSize(iconSize.lx, iconSize.ly));
  }

  for (int i = 0; i < (int)m_fids.size(); i++) {
    QListWidgetItem *item = new QListWidgetItem(
        QString::fromStdString(m_fids[i].expand(TFrameId::NO_PAD)),
        m_drawingList);
    item->setData(Qt::UserRole, i);
    if (useIcons) {
      QPixmap pm =
          IconGenerator::instance()->getIcon(m_level.getPointer(), m_fids[i]);
      if (!pm.isNull()) item->setIcon(QIcon(pm));
    }
  }
}

//-----------------------------------------------------------------------------

void DrawingSubstitutionPane::onIconGenerated() {
  if (!m_level || !m_level->getSimpleLevel()) return;

  for (int i = 0; i < m_drawingList->count() && i < (int)m_fids.size(); i++) {
    // onDemand: only fetch the icons already generated
    QPixmap pm = IconGenerator::instance()->getIcon(m_level.getPointer(),
                                                    m_fids[i], true, true);
    if (!pm.isNull()) m_drawingList->item(i)->setIcon(QIcon(pm));
  }
}

//-----------------------------------------------------------------------------

void DrawingSubstitutionPane::onItemClicked(QListWidgetItem *item) {
  if (!item || !m_level) return;
  int index = item->data(Qt::UserRole).toInt();
  if (index < 0 || index >= (int)m_fids.size()) return;
  substitute(m_fids[index]);
  // restore the highlight if nothing was changed
  refresh();
}

//-----------------------------------------------------------------------------
/*! Exposes \b fid in the current cell or, if there is a cell selection, in
    each of its cells which holds the panel's level. Empty cells, cells of
    other levels and locked columns are left untouched.                   */

void DrawingSubstitutionPane::substitute(const TFrameId &fid) {
  TApp *app    = TApp::instance();
  TXsheet *xsh = app->getCurrentXsheet()->getXsheet();
  if (!xsh || !m_level) return;

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

  SubstituteDrawingUndo *undo =
      new SubstituteDrawingUndo(TXshCell(m_level, fid));
  for (int c = c0; c <= c1; c++) {
    TXshColumn *column = xsh->getColumn(c);
    if (!column || column->isLocked()) continue;
    for (int r = r0; r <= r1; r++) {
      TXshCell cell = xsh->getCell(r, c);
      if (cell.isEmpty() || cell.m_level.getPointer() != m_level.getPointer() ||
          cell.m_frameId == fid)
        continue;
      undo->addCell(r, c, cell);
    }
  }

  if (undo->isEmpty()) {
    delete undo;
    return;
  }
  TUndoManager::manager()->add(undo);
  undo->redo();
}

//-----------------------------------------------------------------------------

void DrawingSubstitutionPane::onPrevious() {
  CommandManager::instance()->execute(MI_DrawingSubBackward);
}

//-----------------------------------------------------------------------------

void DrawingSubstitutionPane::onNext() {
  CommandManager::instance()->execute(MI_DrawingSubForward);
}
