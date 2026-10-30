#include "toonzqt/uitranslations.h"
#include "artlayermanagerpane.h"

// Tnz6 includes
#include "tapp.h"
#include "menubarcommandids.h"
#include "sceneviewer.h"

// TnzTools includes
#include "tools/artlayer.h"
#include "tools/tool.h"

// TnzQt includes
#include "toonzqt/menubarcommand.h"

// TnzLib includes
#include "toonz/tcolumnhandle.h"
#include "toonz/tframehandle.h"
#include "toonz/tscenehandle.h"
#include "toonz/txsheethandle.h"
#include "toonz/txshlevelhandle.h"

// TnzCore includes
#include "tvectorimage.h"

// Qt includes
#include <QCheckBox>
#include <QGridLayout>
#include <QLabel>
#include <QPushButton>
#include <QSignalBlocker>
#include <QVBoxLayout>

//=============================================================================
// ArtLayerManagerPane
//-----------------------------------------------------------------------------

ArtLayerManagerPane::ArtLayerManagerPane(QWidget *parent, Qt::WindowFlags flags)
    : QFrame(parent, flags) {
  setObjectName("ArtLayerManagerPane");

  m_drawingLabel = new QLabel(this);

  // From the top of the stacking order to the bottom
  struct Description {
    ArtLayer::Role m_role;
    QString m_name, m_toolTip;
  };
  const Description descriptions[RoleCount] = {
      {ArtLayer::OVERLAY, tr("Overlay"),
       tr("Line Art strokes above the Colour Art strokes. Show or hide them in "
          "the viewer.")},
      {ArtLayer::LINE_ART, tr("Line Art"),
       tr("The other strokes with thickness. Show or hide them in the "
          "viewer.")},
      {ArtLayer::COLOUR_ART, tr("Colour Art"),
       tr("Strokes with thickness 0: they bound the fills and are not "
          "rendered. Show them in the viewer.")},
      {ArtLayer::UNDERLAY, tr("Underlay"),
       tr("Line Art strokes below the Colour Art strokes. Show or hide them in "
          "the viewer.")}};

  QGridLayout *rowsLayout = new QGridLayout();
  rowsLayout->setContentsMargins(0, 0, 0, 0);
  rowsLayout->setHorizontalSpacing(10);
  rowsLayout->setVerticalSpacing(4);
  rowsLayout->setColumnStretch(0, 1);
  rowsLayout->addWidget(new QLabel(tr("Show in viewer"), this), 0, 0);
  rowsLayout->addWidget(new QLabel(tr("Strokes"), this), 0, 1);

  for (int i = 0; i < RoleCount; ++i) {
    Row &row      = m_rows[i];
    row.m_role    = descriptions[i].m_role;
    row.m_visible = new QCheckBox(descriptions[i].m_name, this);
    row.m_visible->setToolTip(descriptions[i].m_toolTip);
    row.m_count = new QLabel(this);
    row.m_count->setAlignment(Qt::AlignRight | Qt::AlignVCenter);

    rowsLayout->addWidget(row.m_visible, i + 1, 0);
    rowsLayout->addWidget(row.m_count, i + 1, 1);

    int role = row.m_role;
    connect(row.m_visible, &QCheckBox::toggled, this,
            [this, role](bool visible) { onVisibilityToggled(role, visible); });
  }

  QLabel *noteLabel = new QLabel(
      tr("Underlay and Overlay are Line Art strokes below the first and above "
         "the last Colour Art stroke of the drawing. Choose the layer to edit "
         "with \"Art Layer:\" in the tool options."),
      this);
  noteLabel->setWordWrap(true);

  QPushButton *createButton =
      new QPushButton(tr("Create Colour Art from Line Art"), this);
  createButton->setToolTip(
      tr("Add to the drawing, or to the selected drawings, a copy of each Line "
         "Art stroke with thickness 0"));

  QVBoxLayout *mainLayout = new QVBoxLayout();
  mainLayout->setContentsMargins(5, 5, 5, 5);
  mainLayout->setSpacing(5);
  {
    mainLayout->addWidget(m_drawingLabel);
    mainLayout->addLayout(rowsLayout);
    mainLayout->addWidget(createButton, 0, Qt::AlignLeft);
    mainLayout->addWidget(noteLabel);
    mainLayout->addStretch(1);
  }
  setLayout(mainLayout);

  connect(createButton, SIGNAL(clicked()), this, SLOT(onCreateColourArt()));
}

//-----------------------------------------------------------------------------

void ArtLayerManagerPane::showEvent(QShowEvent *) {
  TApp *app = TApp::instance();
  connect(app->getCurrentScene(), SIGNAL(sceneSwitched()), this,
          SLOT(refresh()), Qt::UniqueConnection);
  connect(app->getCurrentXsheet(), SIGNAL(xsheetSwitched()), this,
          SLOT(refresh()), Qt::UniqueConnection);
  connect(app->getCurrentXsheet(), SIGNAL(xsheetChanged()), this,
          SLOT(refresh()), Qt::UniqueConnection);
  connect(app->getCurrentColumn(), SIGNAL(columnIndexSwitched()), this,
          SLOT(refresh()), Qt::UniqueConnection);
  connect(app->getCurrentLevel(), SIGNAL(xshLevelSwitched(TXshLevel *)), this,
          SLOT(refresh()), Qt::UniqueConnection);
  connect(app->getCurrentLevel(), SIGNAL(xshLevelChanged()), this,
          SLOT(refresh()), Qt::UniqueConnection);
  connect(app->getCurrentFrame(), SIGNAL(frameSwitched()), this,
          SLOT(refresh()), Qt::UniqueConnection);
  updateVisibility();
  refresh();
}

//-----------------------------------------------------------------------------

void ArtLayerManagerPane::hideEvent(QHideEvent *) {
  TApp *app = TApp::instance();
  disconnect(app->getCurrentScene(), SIGNAL(sceneSwitched()), this,
             SLOT(refresh()));
  disconnect(app->getCurrentXsheet(), SIGNAL(xsheetSwitched()), this,
             SLOT(refresh()));
  disconnect(app->getCurrentXsheet(), SIGNAL(xsheetChanged()), this,
             SLOT(refresh()));
  disconnect(app->getCurrentColumn(), SIGNAL(columnIndexSwitched()), this,
             SLOT(refresh()));
  disconnect(app->getCurrentLevel(), SIGNAL(xshLevelSwitched(TXshLevel *)),
             this, SLOT(refresh()));
  disconnect(app->getCurrentLevel(), SIGNAL(xshLevelChanged()), this,
             SLOT(refresh()));
  disconnect(app->getCurrentFrame(), SIGNAL(frameSwitched()), this,
             SLOT(refresh()));
}

//-----------------------------------------------------------------------------

//! Sets the check boxes from the state of the viewer.
void ArtLayerManagerPane::updateVisibility() {
  for (Row &row : m_rows) {
    QSignalBlocker blocker(row.m_visible);
    if (row.m_role == ArtLayer::COLOUR_ART)
      row.m_visible->setChecked(ArtLayer::colourArtShown());
    else
      row.m_visible->setChecked(
          !(ArtLayer::hiddenRoles() &
            ArtLayer::roleBit(ArtLayer::Role(row.m_role))));
  }
}

//-----------------------------------------------------------------------------

void ArtLayerManagerPane::refresh() {
  TApp *app = TApp::instance();
  if (app->getCurrentFrame()->isPlaying()) return;

  TVectorImageP vi(TTool::getImage(false));
  if (!vi) {
    m_drawingLabel->setText(
        tr("The current drawing is not a Toonz Vector one."));
    for (Row &row : m_rows) row.m_count->setText("-");
    return;
  }

  int counts[ArtLayer::RoleCount];
  ArtLayer::countRoles(*vi, counts);

  m_drawingLabel->setText(
      UiTranslations::text(tr("%1 strokes in the current drawing")).arg(vi->getStrokeCount()));
  for (Row &row : m_rows)
    row.m_count->setText(QString::number(counts[row.m_role]));
}

//-----------------------------------------------------------------------------

void ArtLayerManagerPane::repaintViewer() {
  if (SceneViewer *viewer = TApp::instance()->getActiveViewer())
    viewer->GLInvalidateAll();
}

//-----------------------------------------------------------------------------

void ArtLayerManagerPane::onVisibilityToggled(int role, bool visible) {
  if (role == ArtLayer::COLOUR_ART)
    ArtLayer::setColourArtShown(visible);
  else
    ArtLayer::setRoleHidden(ArtLayer::Role(role), !visible);
  repaintViewer();
}

//-----------------------------------------------------------------------------

void ArtLayerManagerPane::onCreateColourArt() {
  CommandManager::instance()->execute(MI_CreateColourArt);
  refresh();
}
