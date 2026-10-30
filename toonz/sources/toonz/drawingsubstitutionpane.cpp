#include "drawingsubstitutionpane.h"
#include "drawingsubstitutionundo.h"

// Tnz6 includes
#include "tapp.h"
#include "cellselection.h"
#include "menubarcommandids.h"

// TnzQt includes
#include "toonzqt/icongenerator.h"
#include "toonzqt/menubarcommand.h"
#include "toonzqt/tselectionhandle.h"
#include "toonzqt/uitranslations.h"

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

//=============================================================================
// DrawingSubstitutionPane
//-----------------------------------------------------------------------------

DrawingSubstitutionPane::DrawingSubstitutionPane(QWidget *parent,
                                                 Qt::WindowFlags flags)
    : QFrame(parent, flags) {
  setObjectName("DrawingSubstitutionPane");

  m_levelLabel = new QLabel(this);
  m_levelLabel->setObjectName("drawingSubstitutionLevelName");

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

void DrawingSubstitutionPane::refresh() {
  TXshLevelP level;
  TFrameId currentFid;
  getSubstitutionCurrentCell(level, currentFid);

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
    UiTranslations::setCaption(m_levelLabel, "text",
                                tr("No level in the current cell"));
    return;
  }
  UiTranslations::setLiteral(m_levelLabel, "text",
                              QString::fromStdWString(m_level->getName()));

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
  SubstituteDrawingUndo *undo =
      SubstituteDrawingUndo::create(m_level, fid, true);
  if (!undo) return;
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
