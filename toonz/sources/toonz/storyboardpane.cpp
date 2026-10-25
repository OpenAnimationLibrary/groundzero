#include "storyboardpane.h"

// Tnz6 includes
#include "tapp.h"
#include "menubarcommandids.h"

// TnzQt includes
#include "toonzqt/icongenerator.h"
#include "toonzqt/menubarcommand.h"
#include "toonzqt/lineedit.h"
#include "toonzqt/flipconsole.h"

// TnzLib includes
#include "toonz/txsheet.h"
#include "toonz/txshcell.h"
#include "toonz/txshcolumn.h"
#include "toonz/txshsimplelevel.h"
#include "toonz/navigationtags.h"
#include "toonz/toonzscene.h"
#include "toonz/sceneproperties.h"
#include "toonz/tscenehandle.h"
#include "toonz/txsheethandle.h"
#include "toonz/tframehandle.h"
#include "toonz/tcamera.h"
#include "toonz/tstageobjecttree.h"
#include "toonz/txshlevelhandle.h"
#include "toutputproperties.h"

// TnzCore includes
#include "tundo.h"
#include "historytypes.h"

// Qt includes
#include <QApplication>
#include <QCheckBox>
#include <QDrag>
#include <QDragEnterEvent>
#include <QDragMoveEvent>
#include <QDropEvent>
#include <QHBoxLayout>
#include <QLabel>
#include <QListWidget>
#include <QMimeData>
#include <QMouseEvent>
#include <QPainter>
#include <QPushButton>
#include <QSignalBlocker>
#include <QTimer>
#include <QVBoxLayout>

// STD includes
#include <algorithm>
#include <climits>

namespace {

//-----------------------------------------------------------------------------
/*! Removes the tag at \b frame, then adds \b tag if not null. The tag color
    is restored as is, without changing the last tag color used.          */

void setNavigationTag(TXsheet *xsh, int frame, const NavigationTags::Tag *tag) {
  NavigationTags *navTags = xsh->getNavigationTags();
  navTags->removeTag(frame);
  if (tag) {
    navTags->m_tags.push_back(*tag);
    std::sort(navTags->m_tags.begin(), navTags->m_tags.end());
  }
}

//-----------------------------------------------------------------------------

void notifyTagsChanged() {
  TApp *app = TApp::instance();
  app->getCurrentXsheet()->notifyXsheetChanged();
  app->getCurrentScene()->setDirtyFlag(true);

  // keep the navigation tag commands in sync, as the xsheet viewer does
  TXsheet *xsh = app->getCurrentXsheet()->getXsheet();
  int row      = app->getCurrentFrame()->getFrame();
  if (!xsh) return;
  NavigationTags *navTags = xsh->getNavigationTags();
  int lastTag             = navTags->getPrevTag(INT_MAX);
  int firstTag            = navTags->getNextTag(-1);
  CommandManager::instance()->enable(MI_NextTaggedFrame, (row < lastTag));
  CommandManager::instance()->enable(MI_PrevTaggedFrame,
                                     firstTag != -1 && row > firstTag);
  CommandManager::instance()->enable(MI_EditTaggedFrame,
                                     navTags->isTagged(row));
  CommandManager::instance()->enable(MI_ClearTags, (navTags->getCount() > 0));
}

//=============================================================================
// StoryboardTagUndo
//-----------------------------------------------------------------------------
/*! Adds, removes or renames the navigation tag at one frame of an xsheet.
    A missing old tag means an addition, a missing new tag a removal.     */

class StoryboardTagUndo final : public TUndo {
  TXsheetP m_xsh;
  int m_frame;
  bool m_hasOld, m_hasNew;
  NavigationTags::Tag m_oldTag, m_newTag;

public:
  StoryboardTagUndo(TXsheet *xsh, int frame, const NavigationTags::Tag *oldTag,
                    const NavigationTags::Tag *newTag)
      : m_xsh(xsh)
      , m_frame(frame)
      , m_hasOld(oldTag != 0)
      , m_hasNew(newTag != 0) {
    if (oldTag) m_oldTag = *oldTag;
    if (newTag) m_newTag = *newTag;
  }

  void undo() const override {
    setNavigationTag(m_xsh.getPointer(), m_frame, m_hasOld ? &m_oldTag : 0);
    notifyTagsChanged();
  }

  void redo() const override {
    setNavigationTag(m_xsh.getPointer(), m_frame, m_hasNew ? &m_newTag : 0);
    notifyTagsChanged();
  }

  int getSize() const override { return sizeof(*this); }

  QString getHistoryString() override {
    QString frameStr = QString::number(m_frame + 1);
    if (!m_hasOld)
      return QObject::tr("New Storyboard Panel  Frame : %1").arg(frameStr);
    if (!m_hasNew)
      return QObject::tr("Delete Storyboard Panel  Frame : %1").arg(frameStr);
    if (m_oldTag.m_label == m_newTag.m_label)
      return QObject::tr("Edit Storyboard Panel Notes  Frame : %1")
          .arg(frameStr);
    return QObject::tr("Rename Storyboard Panel  Frame : %1  %2 > %3")
        .arg(frameStr)
        .arg(m_oldTag.m_label)
        .arg(m_newTag.m_label);
  }
  int getHistoryType() override { return HistoryType::Xsheet; }
};

//=============================================================================
// StoryboardMoveUndo
//-----------------------------------------------------------------------------
/*! Moves the cells of a panel before or after another one: the rows from
    \b row0 are replaced, in every cell column, by their permutation, and the
    navigation tags of the rows follow their cells (keeping captions, colors
    and notes).                                                           */

class StoryboardMoveUndo final : public TUndo {
  TXsheetP m_xsh;
  int m_row0;
  std::vector<int> m_columns;
  std::vector<std::vector<TXshCell>> m_oldCells, m_newCells;
  std::vector<NavigationTags::Tag> m_oldTags, m_newTags;

  void apply(const std::vector<std::vector<TXshCell>> &cells,
             const std::vector<NavigationTags::Tag> &tags) const {
    TXsheet *xsh = m_xsh.getPointer();
    for (int i = 0; i < (int)m_columns.size(); i++)
      xsh->setCells(m_row0, m_columns[i], (int)cells[i].size(),
                    cells[i].data());
    xsh->getNavigationTags()->m_tags = tags;
    notifyTagsChanged();
  }

public:
  /*! \b src holds, for each row from \b row0, the row its cells come from. */
  StoryboardMoveUndo(TXsheet *xsh, int row0, const std::vector<int> &src)
      : m_xsh(xsh), m_row0(row0) {
    int rowCount = (int)src.size();
    for (int c = 0; c < xsh->getColumnCount(); c++) {
      TXshColumn *column = xsh->getColumn(c);
      // sound columns do not keep their clips as plain cells
      if (!column || column->isEmpty() || column->isLocked() ||
          !column->getCellColumn() || column->getSoundColumn() ||
          column->getSoundTextColumn())
        continue;

      std::vector<TXshCell> oldCells(rowCount), newCells(rowCount);
      xsh->getCells(row0, c, rowCount, oldCells.data());
      bool used = false;
      for (int k = 0; k < rowCount; k++) {
        newCells[k] = oldCells[src[k] - row0];
        if (!oldCells[k].isEmpty()) used = true;
      }
      if (!used) continue;
      m_columns.push_back(c);
      m_oldCells.push_back(oldCells);
      m_newCells.push_back(newCells);
    }

    // the tag of a row moves to the row its cells go to
    std::vector<int> dest(rowCount);
    for (int k = 0; k < rowCount; k++) dest[src[k] - row0] = row0 + k;
    m_oldTags = xsh->getNavigationTags()->m_tags;
    m_newTags = m_oldTags;
    for (NavigationTags::Tag &tag : m_newTags)
      if (tag.m_frame >= row0 && tag.m_frame < row0 + rowCount)
        tag.m_frame = dest[tag.m_frame - row0];
    std::sort(m_newTags.begin(), m_newTags.end());
  }

  void undo() const override { apply(m_oldCells, m_oldTags); }
  void redo() const override { apply(m_newCells, m_newTags); }

  int getSize() const override {
    int size = sizeof(*this);
    for (const std::vector<TXshCell> &cells : m_oldCells)
      size += 2 * cells.size() * sizeof(TXshCell);
    return size + (int)(m_oldTags.size() + m_newTags.size()) * 64;
  }

  QString getHistoryString() override {
    return QObject::tr("Move Storyboard Panel");
  }
  int getHistoryType() override { return HistoryType::Xsheet; }
};

}  // namespace

//=============================================================================
// StoryboardPane
//-----------------------------------------------------------------------------

StoryboardPane::StoryboardPane(QWidget *parent, Qt::WindowFlags flags)
    : QFrame(parent, flags)
    , m_frameCount(0)
    , m_captionFrame(-1)
    , m_notesFrame(-1)
    , m_dragIndex(-1) {
  setObjectName("StoryboardPane");

  m_infoLabel = new QLabel(this);

  m_panelList = new QListWidget(this);
  m_panelList->setViewMode(QListView::IconMode);
  m_panelList->setResizeMode(QListView::Adjust);
  m_panelList->setMovement(QListView::Static);
  m_panelList->setSelectionMode(QAbstractItemView::SingleSelection);
  m_panelList->setWrapping(true);
  m_panelList->setWordWrap(true);
  m_panelList->setUniformItemSizes(true);
  m_panelList->setSpacing(4);
  // cards are dragged by hand (see eventFilter) to reorder the panels
  m_panelList->viewport()->setAcceptDrops(true);
  m_panelList->viewport()->installEventFilter(this);

  m_captionField = new DVGui::LineEdit(this);
  m_captionField->setToolTip(
      tr("Caption of the current panel (the navigation tag label)"));

  m_notesField = new DVGui::LineEdit(this);
  m_notesField->setToolTip(
      tr("Notes of the current panel, shown in the tooltip of its card"));

  m_cameraCheck = new QCheckBox(tr("Camera Thumbnails"), this);
  m_cameraCheck->setToolTip(
      tr("Show a camera render of the first frame of each panel instead of one "
         "drawing"));
  m_cameraCheck->setChecked(true);

  m_thumbTimer = new QTimer(this);
  m_thumbTimer->setSingleShot(true);
  m_thumbTimer->setInterval(500);

  QPushButton *newButton = new QPushButton(tr("New Panel at Current Frame"));
  newButton->setToolTip(
      tr("Start a new panel at the current frame by adding a navigation tag"));
  newButton->setFocusPolicy(Qt::NoFocus);
  m_deleteButton = new QPushButton(tr("Delete Panel"));
  m_deleteButton->setToolTip(
      tr("Remove the navigation tag starting the current panel"));
  m_deleteButton->setFocusPolicy(Qt::NoFocus);
  m_playButton = new QPushButton(tr("Play Animatic"));
  m_playButton->setToolTip(tr(
      "Play the scene in the viewer from the start to the end of the current "
      "panel"));
  m_playButton->setFocusPolicy(Qt::NoFocus);
  m_cameraCheck->setFocusPolicy(Qt::NoFocus);

  // layout
  QVBoxLayout *mainLayout = new QVBoxLayout();
  mainLayout->setContentsMargins(5, 5, 5, 5);
  mainLayout->setSpacing(5);
  {
    QHBoxLayout *buttonLayout = new QHBoxLayout();
    buttonLayout->setContentsMargins(0, 0, 0, 0);
    buttonLayout->setSpacing(5);
    {
      buttonLayout->addWidget(newButton);
      buttonLayout->addWidget(m_deleteButton);
      buttonLayout->addWidget(m_playButton);
      buttonLayout->addStretch(1);
    }
    mainLayout->addLayout(buttonLayout);

    QHBoxLayout *captionLayout = new QHBoxLayout();
    captionLayout->setContentsMargins(0, 0, 0, 0);
    captionLayout->setSpacing(5);
    {
      captionLayout->addWidget(new QLabel(tr("Caption:"), this));
      captionLayout->addWidget(m_captionField, 1);
    }
    mainLayout->addLayout(captionLayout);

    QHBoxLayout *notesLayout = new QHBoxLayout();
    notesLayout->setContentsMargins(0, 0, 0, 0);
    notesLayout->setSpacing(5);
    {
      notesLayout->addWidget(new QLabel(tr("Notes:"), this));
      notesLayout->addWidget(m_notesField, 1);
    }
    mainLayout->addLayout(notesLayout);

    mainLayout->addWidget(m_cameraCheck);

    mainLayout->addWidget(m_panelList, 1);
    mainLayout->addWidget(m_infoLabel);
  }
  setLayout(mainLayout);

  connect(m_panelList, SIGNAL(itemClicked(QListWidgetItem *)), this,
          SLOT(onItemClicked(QListWidgetItem *)));
  connect(m_panelList, SIGNAL(itemDoubleClicked(QListWidgetItem *)), this,
          SLOT(onItemDoubleClicked(QListWidgetItem *)));
  connect(m_captionField, SIGNAL(editingFinished()), this,
          SLOT(onCaptionEdited()));
  connect(m_notesField, SIGNAL(editingFinished()), this, SLOT(onNotesEdited()));
  connect(m_cameraCheck, SIGNAL(toggled(bool)), this,
          SLOT(onCameraThumbsToggled()));
  connect(m_thumbTimer, SIGNAL(timeout()), this, SLOT(regenerateThumbnails()));
  connect(newButton, SIGNAL(clicked()), this, SLOT(onNewPanel()));
  connect(m_deleteButton, SIGNAL(clicked()), this, SLOT(onDeletePanel()));
  connect(m_playButton, SIGNAL(clicked()), this, SLOT(onPlayAnimatic()));
}

//-----------------------------------------------------------------------------

void StoryboardPane::showEvent(QShowEvent *) {
  TApp *app = TApp::instance();
  connect(app->getCurrentScene(), SIGNAL(sceneSwitched()), this,
          SLOT(refresh()), Qt::UniqueConnection);
  connect(app->getCurrentXsheet(), SIGNAL(xsheetSwitched()), this,
          SLOT(refresh()), Qt::UniqueConnection);
  connect(app->getCurrentXsheet(), SIGNAL(xsheetChanged()), this,
          SLOT(refresh()), Qt::UniqueConnection);
  connect(app->getCurrentXsheet(), SIGNAL(xsheetChanged()), this,
          SLOT(scheduleThumbnails()), Qt::UniqueConnection);
  connect(app->getCurrentLevel(), SIGNAL(xshLevelChanged()), this,
          SLOT(scheduleThumbnails()), Qt::UniqueConnection);
  connect(app->getCurrentFrame(), SIGNAL(frameSwitched()), this,
          SLOT(onFrameSwitched()), Qt::UniqueConnection);
  connect(IconGenerator::instance(), SIGNAL(iconGenerated()), this,
          SLOT(onIconGenerated()), Qt::UniqueConnection);
  refresh();
}

//-----------------------------------------------------------------------------

void StoryboardPane::hideEvent(QHideEvent *) {
  TApp *app = TApp::instance();
  disconnect(app->getCurrentScene(), SIGNAL(sceneSwitched()), this,
             SLOT(refresh()));
  disconnect(app->getCurrentXsheet(), SIGNAL(xsheetSwitched()), this,
             SLOT(refresh()));
  disconnect(app->getCurrentXsheet(), SIGNAL(xsheetChanged()), this,
             SLOT(refresh()));
  disconnect(app->getCurrentXsheet(), SIGNAL(xsheetChanged()), this,
             SLOT(scheduleThumbnails()));
  disconnect(app->getCurrentLevel(), SIGNAL(xshLevelChanged()), this,
             SLOT(scheduleThumbnails()));
  m_thumbTimer->stop();
  disconnect(app->getCurrentFrame(), SIGNAL(frameSwitched()), this,
             SLOT(onFrameSwitched()));
  disconnect(IconGenerator::instance(), SIGNAL(iconGenerated()), this,
             SLOT(onIconGenerated()));

  // Do not keep the levels alive while the panel is hidden.
  m_panels.clear();
  m_panelList->clear();
}

//-----------------------------------------------------------------------------
/*! Builds the panels of the current xsheet from its navigation tags. The
    thumbnail of a panel is the drawing exposed at its first frame in the
    topmost camstand-visible column holding a drawing level.             */

void StoryboardPane::buildPanels(std::vector<Panel> &panels,
                                 int &frameCount) const {
  panels.clear();
  frameCount   = 0;
  TXsheet *xsh = TApp::instance()->getCurrentXsheet()->getXsheet();
  if (!xsh) return;

  frameCount                            = xsh->getFrameCount();
  std::vector<NavigationTags::Tag> tags = xsh->getNavigationTags()->getTags();
  std::sort(tags.begin(), tags.end());

  for (int i = 0; i < (int)tags.size(); i++) {
    Panel panel;
    panel.m_r0    = tags[i].m_frame;
    panel.m_r1    = (i + 1 < (int)tags.size())
                        ? tags[i + 1].m_frame - 1
                        : std::max(frameCount - 1, panel.m_r0);
    panel.m_label = tags[i].m_label;
    panel.m_notes = tags[i].m_notes;
    panel.m_color = tags[i].m_color;

    for (int c = xsh->getColumnCount() - 1; c >= 0; c--) {
      TXshColumn *column = xsh->getColumn(c);
      if (!column || !column->isCamstandVisible()) continue;
      TXshCell cell = xsh->getCell(panel.m_r0, c);
      if (cell.isEmpty() || !cell.m_level || !cell.m_level->getSimpleLevel())
        continue;
      panel.m_level = cell.m_level;
      panel.m_fid   = cell.m_frameId;
      break;
    }
    panels.push_back(panel);
  }
}

//-----------------------------------------------------------------------------

void StoryboardPane::refresh() {
  buildPanels(m_panels, m_frameCount);
  rebuildList();
  updateCurrentPanel();
}

//-----------------------------------------------------------------------------
/*! Navigation tags toggled from the xsheet do not notify a change, so check
    them on frame switches too.                                          */

void StoryboardPane::onFrameSwitched() {
  std::vector<Panel> panels;
  int frameCount;
  buildPanels(panels, frameCount);

  bool changed = frameCount != m_frameCount || panels.size() != m_panels.size();
  for (int i = 0; !changed && i < (int)panels.size(); i++)
    changed = panels[i].m_r0 != m_panels[i].m_r0 ||
              panels[i].m_r1 != m_panels[i].m_r1 ||
              panels[i].m_label != m_panels[i].m_label ||
              panels[i].m_notes != m_panels[i].m_notes ||
              panels[i].m_color != m_panels[i].m_color;

  if (changed) {
    m_panels.swap(panels);
    m_frameCount = frameCount;
    rebuildList();
  }
  updateCurrentPanel();
}

//-----------------------------------------------------------------------------

QString StoryboardPane::panelCaption(int index) const {
  if (index < 0 || index >= (int)m_panels.size()) return QString();
  const QString &label = m_panels[index].m_label;
  return label.isEmpty() ? tr("Panel %1").arg(index + 1) : label;
}

//-----------------------------------------------------------------------------
/*! Returns the size of the camera thumbnails: the camera resolution fitted in
    the icon size.                                                        */

TDimension StoryboardPane::cameraIconSize() const {
  TDimension iconSize = IconGenerator::instance()->getIconSize();
  TXsheet *xsh        = TApp::instance()->getCurrentXsheet()->getXsheet();
  TCamera *camera     = xsh ? xsh->getStageObjectTree()->getCurrentCamera() : 0;
  if (!camera) return iconSize;
  TDimension res = camera->getRes();
  if (res.lx <= 0 || res.ly <= 0) return iconSize;
  double scale =
      std::min((double)iconSize.lx / res.lx, (double)iconSize.ly / res.ly);
  return TDimension(std::max(1, qRound(res.lx * scale)),
                    std::max(1, qRound(res.ly * scale)));
}

//-----------------------------------------------------------------------------

QPixmap StoryboardPane::placeholderIcon(int index) const {
  TDimension iconSize = IconGenerator::instance()->getIconSize();
  QPixmap pm(iconSize.lx, iconSize.ly);
  pm.fill(Qt::lightGray);
  QPainter p(&pm);
  p.setPen(Qt::darkGray);
  p.drawText(pm.rect(), Qt::AlignCenter,
             QString::number(m_panels[index].m_r0 + 1));
  p.fillRect(0, 0, pm.width(), 4, m_panels[index].m_color);
  return pm;
}

//-----------------------------------------------------------------------------
/*! Returns the thumbnail of a panel, with a strip of the tag color on top.
    It is a camera render of the first frame of the panel, or the drawing
    exposed there when the camera thumbnails are off. A null pixmap is
    returned while a camera thumbnail is being generated (and requested,
    unless \b onDemand). Without a drawing at the first frame the frame
    number is shown.                                                      */

QPixmap StoryboardPane::panelIcon(int index, bool onDemand) const {
  QPixmap pm;
  const Panel &panel = m_panels[index];
  if (m_cameraCheck->isChecked()) {
    TXsheet *xsh = TApp::instance()->getCurrentXsheet()->getXsheet();
    pm           = IconGenerator::instance()->getXsheetFrameIcon(
                  xsh, panel.m_r0, cameraIconSize(), onDemand);
    if (pm.isNull()) return pm;
  } else {
    if (panel.m_level)
      pm = IconGenerator::instance()->getIcon(panel.m_level.getPointer(),
                                              panel.m_fid, true, onDemand);
    if (pm.isNull()) return onDemand ? pm : placeholderIcon(index);
  }
  pm = pm.copy();

  QPainter p(&pm);
  p.fillRect(0, 0, pm.width(), 4, panel.m_color);
  return pm;
}

//-----------------------------------------------------------------------------
/*! Updates the panel cards, reusing the existing items so that a click in
    progress is not lost.                                                */

void StoryboardPane::rebuildList() {
  QSignalBlocker blocker(m_panelList);

  int count = (int)m_panels.size();
  while (m_panelList->count() > count) delete m_panelList->takeItem(count);
  while (m_panelList->count() < count) new QListWidgetItem(m_panelList);

  TDimension iconSize = IconGenerator::instance()->getIconSize();
  m_panelList->setIconSize(QSize(iconSize.lx, iconSize.ly));
  // room for the caption and the duration lines below the thumbnail
  int lineHeight = m_panelList->fontMetrics().height();
  m_panelList->setGridSize(
      QSize(std::max(iconSize.lx, 80) + 16, iconSize.ly + 2 * lineHeight + 16));

  double fps = 0.0;
  if (ToonzScene *scene = TApp::instance()->getCurrentScene()->getScene())
    fps = scene->getProperties()->getOutputProperties()->getFrameRate();

  for (int i = 0; i < count; i++) {
    const Panel &panel = m_panels[i];
    int frames         = panel.m_r1 - panel.m_r0 + 1;
    QString duration   = tr("%1 fr").arg(frames);
    if (fps > 0.0)
      duration += tr(" (%1 s)").arg(QString::number(frames / fps, 'f', 1));

    QListWidgetItem *item = m_panelList->item(i);
    item->setText(panelCaption(i) + "\n" + duration);
    QString toolTip = tr("%1\nFrames %2 - %3")
                          .arg(panelCaption(i))
                          .arg(panel.m_r0 + 1)
                          .arg(panel.m_r1 + 1);
    if (!panel.m_notes.isEmpty()) toolTip += "\n\n" + panel.m_notes;
    item->setToolTip(toolTip);

    // a pending camera thumbnail keeps the previous icon of the card
    QPixmap pm = panelIcon(i, false);
    if (!pm.isNull())
      item->setIcon(QIcon(pm));
    else if (item->icon().isNull())
      item->setIcon(QIcon(placeholderIcon(i)));
  }

  updateInfoLabel();
  m_playButton->setEnabled(m_frameCount > 0);
}

//-----------------------------------------------------------------------------

void StoryboardPane::updateInfoLabel() {
  if (m_panels.empty())
    m_infoLabel->setText(
        tr("No panels: add a navigation tag with \"New Panel at Current "
           "Frame\"."));
  else
    m_infoLabel->setText(
        tr("%1 panels. Drag a card onto another one to reorder the panels.")
            .arg(m_panels.size()));
}

//-----------------------------------------------------------------------------
/*! Returns the panel containing the current frame, or -1.               */

int StoryboardPane::currentPanelIndex() const {
  int frame = TApp::instance()->getCurrentFrame()->getFrame();
  int index = -1;
  for (int i = 0; i < (int)m_panels.size(); i++)
    if (m_panels[i].m_r0 <= frame) index = i;
  return index;
}

//-----------------------------------------------------------------------------

void StoryboardPane::updateCurrentPanel() {
  int index = currentPanelIndex();

  {
    QSignalBlocker blocker(m_panelList);
    m_panelList->clearSelection();
    QListWidgetItem *item = index >= 0 ? m_panelList->item(index) : 0;
    if (item) {
      m_panelList->setCurrentItem(item);
      m_panelList->scrollToItem(item);
    }
  }

  m_deleteButton->setEnabled(index >= 0);
  m_captionField->setEnabled(index >= 0);
  // do not overwrite a caption being typed
  if (!m_captionField->hasFocus()) {
    m_captionField->setText(index >= 0 ? m_panels[index].m_label : QString());
    m_captionFrame = index >= 0 ? m_panels[index].m_r0 : -1;
    m_captionField->setPlaceholderText(panelCaption(index));
  }

  m_notesField->setEnabled(index >= 0);
  if (!m_notesField->hasFocus()) {
    m_notesField->setText(index >= 0 ? m_panels[index].m_notes : QString());
    m_notesFrame = index >= 0 ? m_panels[index].m_r0 : -1;
  }
}

//-----------------------------------------------------------------------------

void StoryboardPane::onIconGenerated() {
  for (int i = 0; i < m_panelList->count() && i < (int)m_panels.size(); i++) {
    if (!m_cameraCheck->isChecked() && !m_panels[i].m_level) continue;
    // onDemand: only fetch the icons already generated
    QPixmap pm = panelIcon(i, true);
    if (!pm.isNull()) m_panelList->item(i)->setIcon(QIcon(pm));
  }
}

//-----------------------------------------------------------------------------

void StoryboardPane::onItemClicked(QListWidgetItem *item) {
  int index = item ? m_panelList->row(item) : -1;
  if (index < 0 || index >= (int)m_panels.size()) return;
  TApp::instance()->getCurrentFrame()->setFrame(m_panels[index].m_r0);
}

//-----------------------------------------------------------------------------

void StoryboardPane::onItemDoubleClicked(QListWidgetItem *item) {
  onItemClicked(item);
  m_captionField->setFocus();
  m_captionField->selectAll();
}

//-----------------------------------------------------------------------------

void StoryboardPane::onCaptionEdited() {
  // the caption field keeps the panel it was filled with while it has focus
  int frame    = m_captionFrame;
  TXsheet *xsh = TApp::instance()->getCurrentXsheet()->getXsheet();
  if (frame < 0 || !xsh) return;

  QString label           = m_captionField->text().trimmed();
  NavigationTags *navTags = xsh->getNavigationTags();
  if (!navTags->isTagged(frame) || navTags->getTagLabel(frame) == label) return;

  NavigationTags::Tag oldTag = navTags->getTag(frame);
  NavigationTags::Tag newTag = oldTag;
  newTag.m_label             = label;
  StoryboardTagUndo *undo = new StoryboardTagUndo(xsh, frame, &oldTag, &newTag);
  TUndoManager::manager()->add(undo);
  undo->redo();
}

//-----------------------------------------------------------------------------

void StoryboardPane::onNotesEdited() {
  // the notes field keeps the panel it was filled with while it has focus
  int frame    = m_notesFrame;
  TXsheet *xsh = TApp::instance()->getCurrentXsheet()->getXsheet();
  if (frame < 0 || !xsh) return;

  QString notes           = m_notesField->text().trimmed();
  NavigationTags *navTags = xsh->getNavigationTags();
  if (!navTags->isTagged(frame) || navTags->getTag(frame).m_notes == notes)
    return;

  NavigationTags::Tag oldTag = navTags->getTag(frame);
  NavigationTags::Tag newTag = oldTag;
  newTag.m_notes             = notes;
  StoryboardTagUndo *undo = new StoryboardTagUndo(xsh, frame, &oldTag, &newTag);
  TUndoManager::manager()->add(undo);
  undo->redo();
}

//-----------------------------------------------------------------------------

void StoryboardPane::onNewPanel() {
  TApp *app    = TApp::instance();
  TXsheet *xsh = app->getCurrentXsheet()->getXsheet();
  int frame    = app->getCurrentFrame()->getFrame();
  if (!xsh || frame < 0 || xsh->isFrameTagged(frame)) return;

  NavigationTags::Tag newTag(frame, QString(),
                             xsh->getNavigationTags()->m_lastTagColorUsed);
  StoryboardTagUndo *undo = new StoryboardTagUndo(xsh, frame, 0, &newTag);
  TUndoManager::manager()->add(undo);
  undo->redo();
}

//-----------------------------------------------------------------------------

void StoryboardPane::onDeletePanel() {
  int index    = currentPanelIndex();
  TXsheet *xsh = TApp::instance()->getCurrentXsheet()->getXsheet();
  if (index < 0 || !xsh) return;

  int frame = m_panels[index].m_r0;
  if (!xsh->isFrameTagged(frame)) return;
  NavigationTags::Tag oldTag = xsh->getNavigationTags()->getTag(frame);
  StoryboardTagUndo *undo    = new StoryboardTagUndo(xsh, frame, &oldTag, 0);
  TUndoManager::manager()->add(undo);
  undo->redo();
}

//-----------------------------------------------------------------------------
/*! Plays the scene from the start of the current panel (or from the first
    frame when there is none) with the viewer's Play command. As Short Play
    does, the viewer is asked to pause at the last frame of the panel.   */

void StoryboardPane::onPlayAnimatic() {
  int index = currentPanelIndex();
  int start = index >= 0 ? m_panels[index].m_r0 : 0;

  FlipConsole *console = FlipConsole::getCurrent();
  if (console && index >= 0) {
    int end = std::min(m_panels[index].m_r1, m_frameCount - 1);
    // the viewer counts frames from 1
    if (end >= start) console->setStopAt(end + 1);
  }

  TApp::instance()->getCurrentFrame()->setFrame(start);
  CommandManager::instance()->execute(MI_Play);
}

//-----------------------------------------------------------------------------

void StoryboardPane::onCameraThumbsToggled() {
  rebuildList();
  scheduleThumbnails();
}

//-----------------------------------------------------------------------------
/*! The thumbnails are regenerated 500 ms after the last change of the xsheet
    or of its levels.                                                     */

void StoryboardPane::scheduleThumbnails() {
  if (m_cameraCheck->isChecked() && isVisible()) m_thumbTimer->start();
}

//-----------------------------------------------------------------------------

void StoryboardPane::regenerateThumbnails() {
  if (!m_cameraCheck->isChecked()) return;
  TXsheet *xsh = TApp::instance()->getCurrentXsheet()->getXsheet();
  IconGenerator::instance()->removeXsheetFrameIcons(xsh);
  buildPanels(m_panels, m_frameCount);
  rebuildList();
}

//-----------------------------------------------------------------------------
/*! Finds the card under \b pos (in viewport coordinates) and whether the
    position is on its right half, i.e. a panel dropped there goes after it. */

bool StoryboardPane::dropTarget(const QPoint &pos, int &index,
                                bool &after) const {
  QListWidgetItem *item = m_panelList->itemAt(pos);
  if (!item) return false;
  index = m_panelList->row(item);
  after = pos.x() > m_panelList->visualItemRect(item).center().x();
  return index >= 0 && index < (int)m_panels.size();
}

//-----------------------------------------------------------------------------
/*! Starts dragging the cards by hand and accepts the drops of the card
    mime type: the list itself would move the items, not the cells.      */

bool StoryboardPane::eventFilter(QObject *obj, QEvent *e) {
  if (obj != m_panelList->viewport()) return QFrame::eventFilter(obj, e);

  static const char *panelMime = "application/x-opentoonz-storyboard-panel";

  switch (e->type()) {
  case QEvent::MouseButtonPress: {
    QMouseEvent *me = static_cast<QMouseEvent *>(e);
    m_dragIndex     = -1;
    if (me->button() == Qt::LeftButton) {
      QListWidgetItem *item = m_panelList->itemAt(me->pos());
      m_dragStart           = me->pos();
      m_dragIndex           = item ? m_panelList->row(item) : -1;
    }
    return false;
  }
  case QEvent::MouseButtonRelease:
    m_dragIndex = -1;
    return false;
  case QEvent::MouseMove: {
    QMouseEvent *me = static_cast<QMouseEvent *>(e);
    if (m_dragIndex < 0 || !(me->buttons() & Qt::LeftButton) ||
        (me->pos() - m_dragStart).manhattanLength() <
            QApplication::startDragDistance())
      return false;

    int index   = m_dragIndex;
    m_dragIndex = -1;
    if (index >= (int)m_panels.size()) return false;

    QMimeData *mime = new QMimeData;
    mime->setData(panelMime, QByteArray::number(index));
    QDrag *drag = new QDrag(m_panelList);
    drag->setMimeData(mime);
    drag->setPixmap(
        m_panelList->item(index)->icon().pixmap(m_panelList->iconSize()));
    drag->exec(Qt::MoveAction);
    return true;
  }
  case QEvent::DragEnter:
  case QEvent::DragMove: {
    QDragMoveEvent *de = static_cast<QDragMoveEvent *>(e);
    if (!de->mimeData()->hasFormat(panelMime)) return false;
    int index;
    bool after;
    if (dropTarget(de->position().toPoint(), index, after)) {
      de->acceptProposedAction();
      m_infoLabel->setText(
          after ? tr("Move the panel after \"%1\"").arg(panelCaption(index))
                : tr("Move the panel before \"%1\"").arg(panelCaption(index)));
    } else {
      de->ignore();
      updateInfoLabel();
    }
    return true;
  }
  case QEvent::DragLeave:
    updateInfoLabel();
    return false;
  case QEvent::Drop: {
    QDropEvent *de = static_cast<QDropEvent *>(e);
    if (!de->mimeData()->hasFormat(panelMime)) return false;
    updateInfoLabel();
    int index;
    bool after;
    if (!dropTarget(de->position().toPoint(), index, after)) return true;
    de->setDropAction(Qt::MoveAction);
    de->accept();
    movePanel(de->mimeData()->data(panelMime).toInt(), index, after);
    return true;
  }
  default:
    break;
  }
  return QFrame::eventFilter(obj, e);
}

//-----------------------------------------------------------------------------
/*! Moves the cells (and the navigation tags) of panel \b from before or
    after panel \b to, as one undoable step.                             */

void StoryboardPane::movePanel(int from, int to, bool after) {
  int count = (int)m_panels.size();
  if (from < 0 || to < 0 || from >= count || to >= count || from == to) return;
  TXsheet *xsh = TApp::instance()->getCurrentXsheet()->getXsheet();
  if (!xsh) return;

  int a0  = m_panels[from].m_r0;
  int a1  = m_panels[from].m_r1;
  int ins = after ? m_panels[to].m_r1 + 1 : m_panels[to].m_r0;
  if (ins >= a0 && ins <= a1 + 1) return;  // already in place

  // src: for each row of the affected range, the row its cells come from
  int row0 = std::min(a0, ins);
  std::vector<int> src;
  if (ins > a1 + 1) {
    for (int r = a1 + 1; r < ins; r++) src.push_back(r);
    for (int r = a0; r <= a1; r++) src.push_back(r);
  } else {
    for (int r = a0; r <= a1; r++) src.push_back(r);
    for (int r = ins; r < a0; r++) src.push_back(r);
  }

  StoryboardMoveUndo *undo = new StoryboardMoveUndo(xsh, row0, src);
  TUndoManager::manager()->add(undo);
  undo->redo();
}
