#include "storyboardpane.h"

// Tnz6 includes
#include "tapp.h"
#include "menubarcommandids.h"

// TnzQt includes
#include "toonzqt/icongenerator.h"
#include "toonzqt/menubarcommand.h"
#include "toonzqt/lineedit.h"

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
#include "toutputproperties.h"

// TnzCore includes
#include "tundo.h"
#include "historytypes.h"

// Qt includes
#include <QHBoxLayout>
#include <QLabel>
#include <QListWidget>
#include <QPainter>
#include <QPushButton>
#include <QSignalBlocker>
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
    return QObject::tr("Rename Storyboard Panel  Frame : %1  %2 > %3")
        .arg(frameStr)
        .arg(m_oldTag.m_label)
        .arg(m_newTag.m_label);
  }
  int getHistoryType() override { return HistoryType::Xsheet; }
};

}  // namespace

//=============================================================================
// StoryboardPane
//-----------------------------------------------------------------------------

StoryboardPane::StoryboardPane(QWidget *parent, Qt::WindowFlags flags)
    : QFrame(parent, flags), m_frameCount(0), m_captionFrame(-1) {
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

  m_captionField = new DVGui::LineEdit(this);
  m_captionField->setToolTip(
      tr("Caption of the current panel (the navigation tag label)"));

  QPushButton *newButton = new QPushButton(tr("New Panel at Current Frame"));
  newButton->setToolTip(
      tr("Start a new panel at the current frame by adding a navigation tag"));
  newButton->setFocusPolicy(Qt::NoFocus);
  m_deleteButton = new QPushButton(tr("Delete Panel"));
  m_deleteButton->setToolTip(
      tr("Remove the navigation tag starting the current panel"));
  m_deleteButton->setFocusPolicy(Qt::NoFocus);
  m_playButton = new QPushButton(tr("Play Animatic"));
  m_playButton->setToolTip(
      tr("Play the scene in the viewer from the start of the current panel"));
  m_playButton->setFocusPolicy(Qt::NoFocus);

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
/*! Returns the thumbnail of a panel, with a strip of the tag color on top.
    Panels with no drawing at their first frame show the frame number.   */

QPixmap StoryboardPane::panelIcon(int index, bool onDemand) const {
  TDimension iconSize = IconGenerator::instance()->getIconSize();
  QPixmap pm;
  const Panel &panel = m_panels[index];
  if (panel.m_level)
    pm = IconGenerator::instance()->getIcon(panel.m_level.getPointer(),
                                            panel.m_fid, true, onDemand);
  if (pm.isNull()) {
    if (onDemand) return pm;
    pm = QPixmap(iconSize.lx, iconSize.ly);
    pm.fill(Qt::lightGray);
    QPainter p(&pm);
    p.setPen(Qt::darkGray);
    p.drawText(pm.rect(), Qt::AlignCenter, QString::number(panel.m_r0 + 1));
  } else
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
    item->setToolTip(tr("%1\nFrames %2 - %3")
                         .arg(panelCaption(i))
                         .arg(panel.m_r0 + 1)
                         .arg(panel.m_r1 + 1));
    item->setIcon(QIcon(panelIcon(i, false)));
  }

  if (count == 0)
    m_infoLabel->setText(
        tr("No panels: add a navigation tag with \"New Panel at Current "
           "Frame\"."));
  else
    m_infoLabel->setText(tr("%1 panels").arg(count));
  m_playButton->setEnabled(m_frameCount > 0);
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
}

//-----------------------------------------------------------------------------

void StoryboardPane::onIconGenerated() {
  for (int i = 0; i < m_panelList->count() && i < (int)m_panels.size(); i++) {
    if (!m_panels[i].m_level) continue;
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
    frame when there is none) with the viewer's Play command.            */

void StoryboardPane::onPlayAnimatic() {
  int index = currentPanelIndex();
  int start = index >= 0 ? m_panels[index].m_r0 : 0;
  TApp::instance()->getCurrentFrame()->setFrame(start);
  CommandManager::instance()->execute(MI_Play);
}
