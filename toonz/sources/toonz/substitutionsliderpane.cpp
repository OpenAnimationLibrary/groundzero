#include "toonzqt/uitranslations.h"
#include "substitutionsliderpane.h"
#include "drawingsubstitutionundo.h"

// Tnz6 includes
#include "tapp.h"

// TnzLib includes
#include "toonz/txshcell.h"
#include "toonz/tscenehandle.h"
#include "toonz/txsheethandle.h"
#include "toonz/tframehandle.h"
#include "toonz/tcolumnhandle.h"
#include "toonz/txshlevelhandle.h"

// TnzCore includes
#include "tundo.h"

// Qt includes
#include <QLabel>
#include <QSignalBlocker>
#include <QSlider>
#include <QVBoxLayout>

// STD includes
#include <algorithm>

//=============================================================================
// SubstitutionSliderPane
//-----------------------------------------------------------------------------

SubstitutionSliderPane::SubstitutionSliderPane(QWidget *parent,
                                               Qt::WindowFlags flags)
    : QFrame(parent, flags), m_dragUndo(0), m_dragChanged(false) {
  setObjectName("SubstitutionSliderPane");

  m_label = new QLabel(this);

  m_slider = new QSlider(Qt::Horizontal, this);
  m_slider->setRange(0, 0);
  m_slider->setSingleStep(1);
  m_slider->setPageStep(1);
  m_slider->setEnabled(false);
  m_slider->setToolTip(
      tr("Drag to swap the drawing of the current cell (or of the selected "
         "cells) live"));

  // layout
  QVBoxLayout *mainLayout = new QVBoxLayout();
  mainLayout->setContentsMargins(5, 5, 5, 5);
  mainLayout->setSpacing(5);
  {
    mainLayout->addWidget(m_label);
    mainLayout->addWidget(m_slider);
    mainLayout->addStretch(1);
  }
  setLayout(mainLayout);

  connect(m_slider, SIGNAL(sliderPressed()), this, SLOT(onSliderPressed()));
  connect(m_slider, SIGNAL(sliderReleased()), this, SLOT(onSliderReleased()));
  connect(m_slider, SIGNAL(valueChanged(int)), this, SLOT(onValueChanged(int)));
}

//-----------------------------------------------------------------------------

SubstitutionSliderPane::~SubstitutionSliderPane() { delete m_dragUndo; }

//-----------------------------------------------------------------------------

void SubstitutionSliderPane::showEvent(QShowEvent *) {
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
  refresh();
}

//-----------------------------------------------------------------------------

void SubstitutionSliderPane::hideEvent(QHideEvent *) {
  endDrag();

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

  // Do not keep the level alive while the panel is hidden.
  m_level = TXshLevelP();
  m_fids.clear();
}

//-----------------------------------------------------------------------------

void SubstitutionSliderPane::updateLabel(const TFrameId &currentFid) {
  if (!m_level) {
    m_label->setText(tr("No level in the current cell"));
    return;
  }
  m_label->setText(
      UiTranslations::text(tr("%1  Frame : %2  (%3 drawings)"))
          .arg(QString::fromStdWString(m_level->getName()))
          .arg(QString::fromStdString(currentFid.expand(TFrameId::NO_PAD)))
          .arg(int(m_fids.size())));
}

//-----------------------------------------------------------------------------

void SubstitutionSliderPane::refresh() {
  // the drag itself changes the xsheet: do not fight the slider
  if (m_dragUndo) return;

  TXshLevelP level;
  TFrameId currentFid;
  getSubstitutionCurrentCell(level, currentFid);

  std::vector<TFrameId> fids;
  if (level) level->getFids(fids);
  m_level = level;
  m_fids.swap(fids);

  QSignalBlocker blocker(m_slider);
  int count = int(m_fids.size());
  m_slider->setRange(0, std::max(count - 1, 0));
  m_slider->setEnabled(count > 1);
  std::vector<TFrameId>::const_iterator it =
      std::find(m_fids.begin(), m_fids.end(), currentFid);
  m_slider->setValue(it == m_fids.end() ? 0 : int(it - m_fids.begin()));
  updateLabel(currentFid);
}

//-----------------------------------------------------------------------------

void SubstitutionSliderPane::onSliderPressed() {
  endDrag();
  if (!m_level) return;

  // remember every cell which the drag may swap
  TXshLevelP level;
  TFrameId currentFid;
  getSubstitutionCurrentCell(level, currentFid);
  if (level.getPointer() != m_level.getPointer()) return;
  m_dragUndo    = SubstituteDrawingUndo::create(m_level, currentFid, false);
  m_dragChanged = false;
}

//-----------------------------------------------------------------------------

void SubstitutionSliderPane::onSliderReleased() { endDrag(); }

//-----------------------------------------------------------------------------
/*! Ends the drag in progress, if any: the swaps have already been applied, so
    the undo is only registered, once for the whole drag.                 */

void SubstitutionSliderPane::endDrag() {
  SubstituteDrawingUndo *undo = m_dragUndo;
  m_dragUndo                  = 0;
  if (!undo) return;
  if (m_dragChanged)
    TUndoManager::manager()->add(undo);
  else
    delete undo;
  m_dragChanged = false;
  refresh();
}

//-----------------------------------------------------------------------------

void SubstitutionSliderPane::onValueChanged(int value) {
  if (!m_level || value < 0 || value >= (int)m_fids.size()) return;
  const TFrameId &fid = m_fids[value];

  if (m_dragUndo) {
    // live swap, registered as a single undo when the slider is released
    m_dragUndo->setNewCell(TXshCell(m_level, fid));
    m_dragChanged = true;
    updateLabel(fid);
    m_dragUndo->redo();
    return;
  }

  // keyboard or page step: one undo per change
  SubstituteDrawingUndo *undo =
      SubstituteDrawingUndo::create(m_level, fid, true);
  if (!undo) {
    refresh();
    return;
  }
  TUndoManager::manager()->add(undo);
  undo->redo();
}
