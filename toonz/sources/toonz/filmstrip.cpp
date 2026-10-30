#include <QAbstractItemModel>
#include <QSignalBlocker>
#include "toonzqt/uitranslations.h"
#include "cadmiumsource.h"


#include "filmstrip.h"
#include "toonzqt/planeviewer.h"
#include "tstroke.h"
#include "tgl.h"
#include <QTableWidget>
#include <QHeaderView>
#include "cadmiumpopup.h"
#include "tundo.h"

// Tnz6 includes
#include "tapp.h"
#include "filmstripcommand.h"
#include "frameheadgadget.h"
#include "floatingpanelcommand.h"
#include "menubarcommandids.h"
#include "filmstripselection.h"
#include "onionskinmaskgui.h"
#include "comboviewerpane.h"

// TnzQt includes
#include "toonzqt/icongenerator.h"
#include "toonzqt/trepetitionguard.h"
#include "toonzqt/gutil.h"
#include "toonzqt/tselectionhandle.h"
#include "toonzqt/dvdialog.h"

// TnzTools includes
#include "tools/strokeselection.h"
#include "tools/vectorimagesnapshot.h"
#include "tools/vectormodalguard.h"

// TnzLib includes
#include "toonz/txshlevelhandle.h"
#include "toonz/tcolumnhandle.h"
#include "toonz/txsheethandle.h"
#include "toonz/tframehandle.h"
#include "toonz/tonionskinmaskhandle.h"
#include "toonz/tobjecthandle.h"
#include "toonz/txshleveltypes.h"
#include "toonz/txshsimplelevel.h"
#include "toonz/stage2.h"
#include "toonz/levelproperties.h"
#include "toonz/palettecontroller.h"
#include "toonz/tpalettehandle.h"
#include "toonz/tscenehandle.h"
#include "toonz/toonzscene.h"
#include "toonz/levelset.h"
#include "toonz/preferences.h"

// TnzCore includes
#include "tpalette.h"
#include "tvectorimage.h"

// Qt includes
#include <QPainter>
#include <QScrollBar>
#include <QComboBox>
#include <QCheckBox>
#include <QSlider>
#include <QLabel>
#include <QGridLayout>
#include <QPushButton>
#include <QSettings>
#include <QApplication>
#include <QMainWindow>
#include <QMimeData>
#include <QDrag>
#include <QDragEnterEvent>
#include <QDragMoveEvent>
#include <QDropEvent>
#include <QTimer>
#include <QPointer>
#include <QStyleOptionButton>
#include <algorithm>

namespace {
constexpr int cadmiumGutter = 20;

class CadmiumReferencesUndo final : public TUndo {
  TXshSimpleLevelP m_level;
  std::set<TFrameId> m_before, m_after;
  void restore(const std::set<TFrameId> &references) const {
    m_level->clearCadmiumReferences();
    for (const TFrameId &fid : references)
      if (m_level->fid2index(fid) >= 0) m_level->setCadmiumReference(fid, true);
    TApp::instance()->getCurrentScene()->setDirtyFlag(true);
    TApp::instance()->getCurrentLevel()->notifyLevelChange();
  }
public:
  CadmiumReferencesUndo(TXshSimpleLevel *level, const std::set<TFrameId> &before,
                       const std::set<TFrameId> &after)
      : m_level(level), m_before(before), m_after(after) {}
  int getSize() const override {
    return clampSize(sizeof(*this) + (m_before.size() + m_after.size()) * sizeof(TFrameId));
  }
  void undo() const override { restore(m_before); }
  void redo() const override { restore(m_after); }
  QString getHistoryString() override { return QObject::tr("Cadmium Reference Marks"); }
};

void changeCadmiumReferences(TXshSimpleLevel *level,
                            const std::set<TFrameId> &drawings, bool marked) {
  if (!Cadmium::canPaintLevel(level)) return;
  const std::set<TFrameId> before = level->getCadmiumReferences();
  for (const TFrameId &fid : drawings)
    if (level->fid2index(fid) >= 0) level->setCadmiumReference(fid, marked);
  const std::set<TFrameId> after = level->getCadmiumReferences();
  if (before == after) return;
  TUndoManager::manager()->add(new CadmiumReferencesUndo(level, before, after));
  TApp::instance()->getCurrentScene()->setDirtyFlag(true);
  TApp::instance()->getCurrentLevel()->notifyLevelChange();
}

QString fidToFrameNumberWithLetter(int f) {
  QString str = QString::number((int)(f / 10));
  while (str.length() < 3) str.push_front("0");
  switch (f % 10) {
  case 1:
    str.append('A');
    break;
  case 2:
    str.append('B');
    break;
  case 3:
    str.append('C');
    break;
  case 4:
    str.append('D');
    break;
  case 5:
    str.append('E');
    break;
  case 6:
    str.append('F');
    break;
  case 7:
    str.append('G');
    break;
  case 8:
    str.append('H');
    break;
  case 9:
    str.append('I');
    break;
  default:
    str.append(' ');
    break;
  }
  return str;
}

class StripViewportResizeFilter final : public QObject {
  QPointer<FilmstripFrames> m_frames;
  bool m_busy = false;

public:
  StripViewportResizeFilter(FilmstripFrames *frames, QObject *viewport)
      : QObject(frames), m_frames(frames) {
    setObjectName("filmstripViewportResizeFilter");
    viewport->installEventFilter(this);
  }

  bool eventFilter(QObject *watched, QEvent *event) override {
    if (event->type() == QEvent::Resize && m_frames &&
        m_frames->parentWidget() == watched &&
        m_frames->isResponsiveThumbnails() && !m_frames->isDragInProgress() &&
        !m_busy) {
      m_busy = true;
      QPointer<StripViewportResizeFilter> self(this);
      QPointer<QObject> target(watched);
      m_frames->updateIconLayout();
      // Updating constraints can synchronously close the strip or viewport.
      // Do not access this filter after its owner destroys it, and do not
      // forward an event to a watched object that disappeared during layout.
      if (!self) return target.isNull();
      m_busy = false;
      if (!target) return true;
    }
    return QObject::eventFilter(watched, event);
  }
};

const int kResponsiveIconQuantStep    = 16;
const int kResponsiveRenderDebounceMs = 120;

Filmstrip *s_activeFilmstrip           = nullptr;
FilmstripFrames *s_dragSourceFilmstrip = nullptr;
std::set<TFrameId> s_dragSourceFrames;
}  // namespace

//=============================================================================
// Filmstrip
//-----------------------------------------------------------------------------

FilmstripFrames::FilmstripFrames(QScrollArea *parent, Qt::WindowFlags flags)
    : QFrame(parent, flags)
    , m_scrollArea(parent)
    , m_level(0)
    , m_selection(new TFilmstripSelection())
    , m_frameHeadGadget(0)
    , m_viewportResizeFilter(nullptr)
    , m_inbetweenDialog(0)
    , m_pos(0, 0)
    , m_prefIconSize(dimension2QSize(IconGenerator::instance()->getIconSize()))
    , m_iconSize(m_prefIconSize)
    , m_renderIconSize(m_prefIconSize)
    , m_frameLabelWidth(11)
    , m_selectingRange(0, 0)
    , m_scrollSpeed(0)
    , m_dragSelectionStartIndex(-1)
    , m_dragSelectionEndIndex(-1)
    , m_timerId(0)
    , m_selecting(false)
    , m_dragDropArmed(false)
    , m_readOnly(false) {
  setObjectName("filmStripFrames");
  setFrameStyle(QFrame::StyledPanel);

  setFocusPolicy(Qt::StrongFocus);
  setAcceptDrops(true);
  updateContentConstraints();
  if (m_isVertical) {
    setFixedHeight(parentWidget()->height());
  } else {
    setFixedWidth(parentWidget()->width());
  }

  // la testa mobile che indica il frame corrente (e gestisce la GUI dell'onion
  // skin)
  m_frameHeadGadget = new FilmstripFrameHeadGadget(this);

  m_selection->setView(this);
  m_selection->setCadmiumStrip(this);
  setMouseTracking(true);

  m_viewer = NULL;

  m_responsiveRenderTimer = new QTimer(this);
  m_responsiveRenderTimer->setSingleShot(true);
  connect(m_responsiveRenderTimer, SIGNAL(timeout()), this,
          SLOT(commitResponsiveRenderSize()));

  m_viewportResizeFilter = new StripViewportResizeFilter(this, m_scrollArea->viewport());
}

//-----------------------------------------------------------------------------

FilmstripFrames::~FilmstripFrames() {
  // The viewport survives takeWidget()/replacement. Remove its callback
  // before selection-detachment signals can synchronously reenter layout,
  // while every field used by updateIconLayout is still alive.
  delete m_viewportResizeFilter;
  m_viewportResizeFilter = nullptr;
  // The selection handle is non-owning. Detach while the concrete selection
  // and its command handlers are still alive, including closing an active strip.
  m_selection->makeNotCurrent();
  delete m_selection;
  delete m_frameHeadGadget;
}

//-----------------------------------------------------------------------------

TXshSimpleLevel *FilmstripFrames::getLevel() const { return m_level; }

//-----------------------------------------------------------------------------

void FilmstripFrames::setLevel(TXshSimpleLevel *level) {
  if (m_level == level) return;

  m_level = level;
  m_selection->selectNone();

  removeEventFilter(m_frameHeadGadget);
  if (m_isActive && m_level) installEventFilter(m_frameHeadGadget);

  if (m_isVertical)
    updateContentHeight(0);
  else
    updateContentWidth(0);

  if (isCurrentContextLevel())
    onFrameSwitched();
  else
    update();
}

//-----------------------------------------------------------------------------

void FilmstripFrames::setActive(bool active) {
  if (m_isActive == active) return;

  m_isActive = active;
  removeEventFilter(m_frameHeadGadget);
  if (m_isActive && m_level) installEventFilter(m_frameHeadGadget);
  update();
}

//-----------------------------------------------------------------------------

void FilmstripFrames::setSynchronized(bool synchronized) {
  if (m_isSynchronized == synchronized) return;

  m_isSynchronized = synchronized;
  if (isCurrentContextLevel())
    onFrameSwitched();
  else
    update();
}

//-----------------------------------------------------------------------------

bool FilmstripFrames::isCurrentLevel() const {
  return m_level &&
         m_level == TApp::instance()->getCurrentLevel()->getSimpleLevel();
}

//-----------------------------------------------------------------------------

bool FilmstripFrames::isActiveLevel() const {
  return m_isActive && isCurrentLevel();
}

//-----------------------------------------------------------------------------

bool FilmstripFrames::isCurrentContextLevel() const {
  return (m_isActive || m_isSynchronized) && isCurrentLevel();
}

//-----------------------------------------------------------------------------

void FilmstripFrames::activateLevel() {
  if (m_level) emit levelActivatedSignal(m_level);
}

//-----------------------------------------------------------------------------

QSize FilmstripFrames::quantizeResponsiveRenderSize(const QSize &layout,
                                                    const QSize &pref,
                                                    bool vertical) {
  if (layout.width() <= 0 || layout.height() <= 0) return pref;
  if (pref.width() <= 0 || pref.height() <= 0) return layout;

  auto quantize = [](int v) {
    if (v <= 0) return 1;
    const int q =
        ((v + kResponsiveIconQuantStep / 2) / kResponsiveIconQuantStep) *
        kResponsiveIconQuantStep;
    return std::max(kResponsiveIconQuantStep, q);
  };

  if (vertical) {
    const int w = quantize(layout.width());
    const int h = std::max(1, w * pref.height() / pref.width());
    return QSize(w, h);
  }
  const int h = quantize(layout.height());
  const int w = std::max(1, h * pref.width() / pref.height());
  return QSize(w, h);
}

//-----------------------------------------------------------------------------

void FilmstripFrames::updateContentConstraints() {
  if (!m_scrollArea) return;
  if (m_responsiveThumbnails) {
    m_scrollArea->setWidgetResizable(true);
    if (m_isVertical) {
      setMinimumWidth(0);
      setMaximumWidth(QWIDGETSIZE_MAX);
      setMinimumHeight(getFramesHeight());
      setMaximumHeight(QWIDGETSIZE_MAX);
    } else {
      setMinimumHeight(0);
      setMaximumHeight(QWIDGETSIZE_MAX);
      setMinimumWidth(getFramesWidth());
      setMaximumWidth(QWIDGETSIZE_MAX);
    }
  } else if (m_isVertical) {
    m_scrollArea->setWidgetResizable(false);
    setFixedWidth(m_iconSize.width() + fs_leftMargin + fs_rightMargin +
                  fs_iconMarginLR * 2 + cadmiumGutter);
    setMinimumHeight(0);
    setMaximumHeight(QWIDGETSIZE_MAX);
  } else {
    m_scrollArea->setWidgetResizable(false);
    setFixedHeight(getOneFrameHeight() + cadmiumGutter);
    setMinimumWidth(0);
    setMaximumWidth(QWIDGETSIZE_MAX);
  }
}

//-----------------------------------------------------------------------------

void FilmstripFrames::scheduleResponsiveRenderCommit() {
  if (!m_responsiveRenderTimer) return;
  m_responsiveRenderTimer->start(kResponsiveRenderDebounceMs);
}

//-----------------------------------------------------------------------------

void FilmstripFrames::commitResponsiveRenderSize() {
  if (!m_responsiveThumbnails) {
    m_renderIconSize = m_prefIconSize;
    return;
  }
  const QSize next =
      quantizeResponsiveRenderSize(m_iconSize, m_prefIconSize, m_isVertical);
  if (next == m_renderIconSize) return;
  m_renderIconSize = next;
  update();
}

//-----------------------------------------------------------------------------

void FilmstripFrames::updateIconLayout() {
  m_prefIconSize = dimension2QSize(IconGenerator::instance()->getIconSize());

  QSize effective = m_prefIconSize;
  if (m_responsiveThumbnails && m_scrollArea) {
    QWidget *vp = m_scrollArea->viewport();
    if (vp && m_prefIconSize.width() > 0 && m_prefIconSize.height() > 0) {
      if (m_isVertical) {
        const int availW =
            vp->width() - fs_leftMargin - fs_rightMargin - fs_iconMarginLR * 2 - cadmiumGutter;
        if (availW > 0) {
          const int w = availW;
          const int h =
              std::max(1, w * m_prefIconSize.height() / m_prefIconSize.width());
          effective = QSize(w, h);
        }
      } else {
        const int availH = vp->height() - fs_frameSpacing - fs_iconMarginTop -
                           fs_iconMarginBottom - cadmiumGutter;
        if (availH > 0) {
          const int h = availH;
          const int w =
              std::max(1, h * m_prefIconSize.width() / m_prefIconSize.height());
          effective = QSize(w, h);
        }
      }
    }
  }

  m_iconSize = effective;

  updateContentConstraints();
  if (m_frameHeadGadget) m_frameHeadGadget->updateFrameMetrics();
  if (m_isVertical)
    updateContentHeight();
  else
    updateContentWidth();

  if (m_responsiveThumbnails)
    scheduleResponsiveRenderCommit();
  else {
    if (m_responsiveRenderTimer) m_responsiveRenderTimer->stop();
    m_renderIconSize = m_prefIconSize;
  }
  update();
}

//-----------------------------------------------------------------------------

void FilmstripFrames::setResponsiveThumbnails(bool responsive) {
  if (m_responsiveThumbnails == responsive) return;
  m_responsiveThumbnails = responsive;
  updateIconLayout();
  if (responsive) commitResponsiveRenderSize();
}

//-----------------------------------------------------------------------------

bool FilmstripFrames::isOutOfBounds(int n) const {
  const int width = getIconSize().width() + fs_frameSpacing + fs_iconMarginLR +
                    fs_leftMargin + fs_rightMargin;
  const int hight = getIconSize().height() + fs_frameSpacing +
                    fs_iconMarginTop + fs_iconMarginBottom;
  int limit = m_isVertical ? width : hight;
  return n < 0 || n >= limit;
}

//-----------------------------------------------------------------------------

int FilmstripFrames::y2index(int y) const {
  const int dy = getIconSize().height() + fs_frameSpacing + fs_iconMarginTop +
                 fs_iconMarginBottom;
  return y / dy;
}

//-----------------------------------------------------------------------------

int FilmstripFrames::x2index(int x) const {
  const int dx = getIconSize().width() + fs_frameSpacing + fs_iconMarginLR +
                 fs_leftMargin + fs_rightMargin;
  return x / dx;
}

//-----------------------------------------------------------------------------

int FilmstripFrames::index2y(int index) const {
  const int dy = getIconSize().height() + fs_frameSpacing + fs_iconMarginTop +
                 fs_iconMarginBottom;
  return index * dy;
}

//-----------------------------------------------------------------------------

int FilmstripFrames::index2x(int index) const {
  const int dx = getIconSize().width() + fs_frameSpacing + fs_iconMarginLR +
                 fs_leftMargin + fs_rightMargin;
  return index * dx;
}

//-----------------------------------------------------------------------------

TFrameId FilmstripFrames::index2fid(int index) const {
  TXshSimpleLevel *sl = getLevel();
  if (!sl || index < 0) return TFrameId();
  return sl->index2fid(index);
}

//-----------------------------------------------------------------------------

int FilmstripFrames::fid2index(const TFrameId &fid) const {
  TXshSimpleLevel *sl = getLevel();
  if (!sl) return -1;
  return sl->guessIndex(fid);  // ATTENZIONE: dovrebbe usare fid2index()
}

//-----------------------------------------------------------------------------

int FilmstripFrames::getFramesHeight() const {
  TXshSimpleLevel *level = getLevel();
  int frameCount         = level ? level->getFrameCount() : 1;
  int frameHeight = m_iconSize.height() + fs_frameSpacing + fs_iconMarginTop +
                    fs_iconMarginBottom;
  return frameHeight * (frameCount + 1);
}

//-----------------------------------------------------------------------------

int FilmstripFrames::getFramesWidth() const {
  TXshSimpleLevel *level = getLevel();
  int frameCount         = level ? level->getFrameCount() : 1;
  int frameWidth = m_iconSize.width() + fs_frameSpacing + fs_leftMargin +
                   fs_rightMargin + fs_iconMarginLR;
  return frameWidth * (frameCount + 1);
}

//-----------------------------------------------------------------------------

int FilmstripFrames::getOneFrameHeight() const {
  return m_iconSize.height() + fs_frameSpacing + fs_iconMarginTop +
         fs_iconMarginBottom;
}

//-----------------------------------------------------------------------------

int FilmstripFrames::getOneFrameWidth() const {
  return m_iconSize.width() + fs_frameSpacing + fs_leftMargin +
         fs_iconMarginLR + fs_rightMargin;
}

//-----------------------------------------------------------------------------
void FilmstripFrames::updateContentHeight(int minimumHeight) {
  if (minimumHeight < 0)
    minimumHeight = visibleRegion().boundingRect().bottom();
  int contentHeight = getFramesHeight();
  if (contentHeight < minimumHeight) contentHeight = minimumHeight;
  int parentHeight = parentWidget()->height();
  if (contentHeight < parentHeight) contentHeight = parentHeight;
  if (m_responsiveThumbnails) {
    setMinimumHeight(contentHeight);
  } else if (contentHeight != height()) {
    setFixedHeight(contentHeight);
  }
}

//-----------------------------------------------------------------------------
void FilmstripFrames::updateContentWidth(int minimumWidth) {
  if (!m_responsiveThumbnails) setFixedHeight(getOneFrameHeight() + cadmiumGutter);
  if (minimumWidth < 0) minimumWidth = visibleRegion().boundingRect().right();
  int contentWidth = getFramesWidth();
  if (contentWidth < minimumWidth) contentWidth = minimumWidth;
  int parentWidth = parentWidget()->width();
  if (contentWidth < parentWidth) contentWidth = parentWidth;
  if (m_responsiveThumbnails) {
    setMinimumWidth(contentWidth);
  } else if (contentWidth != width()) {
    setFixedWidth(contentWidth);
  }
}

//-----------------------------------------------------------------------------

void FilmstripFrames::showFrame(int index) {
  TXshSimpleLevel *level = getLevel();
  if (!level) return;
  if (m_isVertical) {
    if (!level->isFid(index2fid(index))) {
      if (!level->isFid(index2fid(index))) return;
    }
    int y0 = index2y(index);
    int y1 = y0 + getOneFrameHeight();
    if (y1 > height()) {
      if (m_responsiveThumbnails)
        setMinimumHeight(y1);
      else
        setFixedHeight(y1);
    }
    m_scrollArea->ensureVisible(0, (y0 + y1) / 2, 50, (y1 - y0) / 2);
  } else {
    if (!level->isFid(index2fid(index))) {
      if (!level->isFid(index2fid(index - 1))) return;
    }
    int x0 = index2x(index);
    int x1 = x0 + getOneFrameWidth();
    if (x1 > width()) {
      if (m_responsiveThumbnails)
        setMinimumWidth(x1);
      else
        setFixedWidth(x1);
    }
    m_scrollArea->ensureVisible((x0 + x1) / 2, 0, (x1 - x0) / 2, 50);
  }
}

//-----------------------------------------------------------------------------

void FilmstripFrames::scroll(int dy) {
  if (m_isVertical) {
    QScrollBar *sb = m_scrollArea->verticalScrollBar();
    int sbValue    = sb->value();

    updateContentHeight(getFramesHeight());
    if (sbValue + dy > getFramesHeight()) {
      sb->setValue(getFramesHeight());
      return;
    }
    sb->setValue(sbValue + dy);
  } else {
    QScrollBar *sb = m_scrollArea->horizontalScrollBar();
    int sbValue    = sb->value();

    updateContentWidth(getFramesWidth());
    if (sbValue + dy > getFramesWidth()) {
      sb->setValue(getFramesWidth());
      return;
    }
    sb->setValue(sbValue + dy);
  }
}

//---------------------------------------------------------------------------

void FilmstripFrames::mouseDoubleClickEvent(QMouseEvent *event) {
  activateLevel();

  int index;
  if (m_isVertical) {
    index = y2index(event->pos().y());
  } else {
    index = x2index(event->pos().x());
  }
  select(index, ONLY_SELECT);  // ONLY_SELECT
}

//-----------------------------------------------------------------------------

void FilmstripFrames::select(int index, SelectionMode mode) {
  TXshSimpleLevel *sl = getLevel();
  bool outOfRange     = !sl || index < 0 || index >= sl->getFrameCount();

  TFrameId fid;
  if (!outOfRange) fid = index2fid(index);

  switch (mode) {
  // select one frame only
  case ONLY_SELECT:
    m_selection->selectNone();
    if (!outOfRange) m_selection->select(fid);
    break;
  case SIMPLE_SELECT:
    // Bail out if fid is already selected
    if (!outOfRange && m_selection->isSelected(fid)) return;

    m_selection->selectNone();
    if (!outOfRange) m_selection->select(fid);
    break;

  case SHIFT_SELECT:
    if (outOfRange) return;

    // Bail out if fid is already selected
    if (m_selection->isSelected(fid)) return;

    if (m_selection->isEmpty())
      m_selection->select(fid);
    else {
      TXshSimpleLevel *sl = getLevel();
      if (!sl) return;

      // seleziono il range da fid al piu' vicino frame selezionato (in entrambe
      // le direzioni)
      int frameCount = sl->getFrameCount();

      // calcolo il limite inferiore della selezione
      int ia = index;
      while (ia > 0 && !m_selection->isSelected(sl->index2fid(ia - 1))) ia--;
      if (ia == 0) ia = index;

      // calcolo il limite superiore della selezione
      int ib = index;
      while (ib < frameCount - 1 &&
             !m_selection->isSelected(sl->index2fid(ib + 1)))
        ib++;
      if (ib == frameCount - 1) ib = index;

      // seleziono
      for (int i = ia; i <= ib; i++) m_selection->select(sl->index2fid(i));
    }
    break;

  case CTRL_SELECT:
    if (outOfRange) return;

    m_selection->select(fid, !m_selection->isSelected(fid));
    break;

  case START_DRAG_SELECT:
    m_selection->selectNone();

    if (outOfRange) {
      m_dragSelectionStartIndex = m_dragSelectionEndIndex = -1;

      break;
    }

    m_selection->select(fid);
    m_dragSelectionStartIndex = index;
    break;

  case DRAG_SELECT:
    if (outOfRange || m_dragSelectionStartIndex < 0 ||
        m_dragSelectionEndIndex == index)
      return;

    m_dragSelectionEndIndex = index;

    m_selection->selectNone();

    int ia = m_dragSelectionStartIndex;
    int ib = index;

    if (ia > ib) std::swap(ia, ib);

    for (int i = ia; i <= ib; ++i) m_selection->select(index2fid(i));

    break;
  }

  TObjectHandle *objectHandle = TApp::instance()->getCurrentObject();
  if (objectHandle->isSpline()) objectHandle->setIsSpline(false);

  // Update current selection
  m_selection->makeCurrent();

  TSelectionHandle *selHandle = TApp::instance()->getCurrentSelection();
  selHandle->notifySelectionChanged();
}

//-----------------------------------------------------------------------------

void FilmstripFrames::showEvent(QShowEvent *) {
  TApp *app = TApp::instance();

  // cambiamenti al livello
  TXshLevelHandle *levelHandle = app->getCurrentLevel();
  bool ret                     = true;
  ret = ret && connect(levelHandle, SIGNAL(xshLevelSwitched(TXshLevel *)), this,
                       SLOT(onLevelSwitched(TXshLevel *)));
  ret = ret && connect(levelHandle, SIGNAL(xshLevelChanged()), this,
                       SLOT(onLevelChanged()));
  ret = ret && connect(levelHandle, SIGNAL(xshLevelViewChanged()), this,
                       SLOT(onLevelChanged()));

  // al frame corrente
  ret = ret && connect(app->getCurrentFrame(), SIGNAL(frameSwitched()), this,
                       SLOT(onFrameSwitched()));
  ret = ret && connect(app->getCurrentFrame(), SIGNAL(frameTypeChanged()), this,
                       SLOT(update()));

  // iconcine
  ret = ret && connect(IconGenerator::instance(), SIGNAL(iconGenerated()), this,
                       SLOT(update()));

  // onion skin
  ret = ret && connect(app->getCurrentOnionSkin(),
                       SIGNAL(onionSkinMaskChanged()), this, SLOT(update()));

  // active viewer change
  ret = ret &&
        connect(app, SIGNAL(activeViewerChanged()), this, SLOT(getViewer()));

  assert(ret);
  getViewer();
}

//-----------------------------------------------------------------------------

void FilmstripFrames::hideEvent(QHideEvent *) {
  TApp *app = TApp::instance();

  // cambiamenti al livello
  disconnect(app->getCurrentLevel(), 0, this, 0);

  // al frame corrente
  disconnect(app->getCurrentFrame(), SIGNAL(frameSwitched()), this,
             SLOT(onFrameSwitched()));
  disconnect(app->getCurrentFrame(), SIGNAL(frameTypeChanged()), this,
             SLOT(update()));

  // iconcine
  disconnect(IconGenerator::instance(), SIGNAL(iconGenerated()), this,
             SLOT(update()));

  // onion skin
  disconnect(app->getCurrentOnionSkin(), SIGNAL(onionSkinMaskChanged()), this,
             SLOT(update()));

  // active viewer change
  disconnect(app, SIGNAL(activeViewerChanged()), this, SLOT(getViewer()));

  if (m_viewer) {
    disconnect(m_viewer, SIGNAL(onZoomChanged()), this, SLOT(update()));
    disconnect(m_viewer, SIGNAL(refreshNavi()), this, SLOT(update()));
    m_viewer = nullptr;
  }
}

//-----------------------------------------------------------------------------

void FilmstripFrames::getViewer() {
  bool viewerChanged = false;
  if (m_viewer != TApp::instance()->getActiveViewer()) {
    if (m_viewer) {
      disconnect(m_viewer, SIGNAL(onZoomChanged()), this, SLOT(update()));
      disconnect(m_viewer, SIGNAL(refreshNavi()), this, SLOT(update()));
      disconnect(m_viewer, SIGNAL(aboutToBeDestroyed()), this,
                 SLOT(onViewerAboutToBeDestroyed()));
    }
    viewerChanged = true;
  }

  m_viewer = TApp::instance()->getActiveViewer();

  if (m_viewer && viewerChanged) {
    connect(m_viewer, SIGNAL(onZoomChanged()), this, SLOT(update()));
    connect(m_viewer, SIGNAL(refreshNavi()), this, SLOT(update()));
    connect(m_viewer, SIGNAL(aboutToBeDestroyed()), this,
            SLOT(onViewerAboutToBeDestroyed()));
    update();
  }
}

//-----------------------------------------------------------------------------

void FilmstripFrames::paintEvent(QPaintEvent *evt) {
  QPainter p(this);

  // p.setRenderHint(QPainter::Antialiasing, true);

  QRect clipRect = evt->rect();

  p.fillRect(clipRect, Qt::black);
  // thumbnail rect, including offsets
  QRect iconImgRect = QRect(QPoint(fs_leftMargin + fs_iconMarginLR,
                                   fs_frameSpacing / 2 + fs_iconMarginTop),
                            m_iconSize);
  // frame size with margins
  QSize frameSize = m_iconSize + QSize(fs_iconMarginLR * 2,
                                       fs_iconMarginTop + fs_iconMarginBottom);
  //  .. and with offset
  QRect frameRect =
      QRect(QPoint(fs_leftMargin, fs_frameSpacing / 2), frameSize);

  int oneFrameHeight = frameSize.height() + fs_frameSpacing;
  int oneFrameWidth  = frameSize.width() + fs_frameSpacing;

  // visible frame index range
  int i0, i1;
  if (m_isVertical) {
    i0 = y2index(clipRect.top());
    i1 = y2index(clipRect.bottom());
  } else {
    i0 = x2index(clipRect.left());
    i1 = x2index(clipRect.right());
  }

  // fids, frameCount <- frames del livello
  std::vector<TFrameId> fids;
  TXshSimpleLevel *sl = getLevel();
  const bool currentContextLevel = isCurrentContextLevel();
  if (sl)
    sl->getFids(fids);
  else {
    for (int i = i0; i <= i1; i++) {
      // draw white rectangles if obtaining the level is failed
      QRect iconRect;
      if (m_isVertical) {
        iconRect = frameRect.translated(QPoint(0, oneFrameHeight * i));
      } else {
        iconRect = frameRect.translated(QPoint(oneFrameWidth * i, 0));
      }
      p.setBrush(QColor(192, 192, 192));
      p.setPen(Qt::NoPen);
      p.drawRect(iconRect);
    }
    return;
  }

  //--- compute navigator rect ---

  QRect naviRect;

  if (currentContextLevel &&
      (sl->getType() == TZP_XSHLEVEL || sl->getType() == OVL_XSHLEVEL) &&
      m_viewer && m_viewer->isVisible()) {
    // imgSize: image's pixel size
    QSize imgSize(sl->getProperties()->getImageRes().lx,
                  sl->getProperties()->getImageRes().ly);
    // Viewer affine
    TAffine viewerAff = m_viewer->getViewMatrix();
    // pixel size which will be displayed with 100% scale in Viewer Stage
    TFrameId currentId = getCurrentFrameId();
    double imgPixelWidth =
        (double)(imgSize.width()) / sl->getDpi(currentId).x * Stage::inch;
    double imgPixelHeight =
        (double)(imgSize.height()) / sl->getDpi(currentId).y * Stage::inch;

    // get the image's corner positions in viewer matrix (with current zoom
    // scale)
    TPointD imgTopRight =
        viewerAff * TPointD(imgPixelWidth / 2.0f, imgPixelHeight / 2.0f);
    TPointD imgBottomLeft =
        viewerAff * TPointD(-imgPixelWidth / 2.0f, -imgPixelHeight / 2.0f);

    // pixel size in viewer matrix ( with current zoom scale )
    QSizeF imgSizeInViewer(imgTopRight.x - imgBottomLeft.x,
                           imgTopRight.y - imgBottomLeft.y);

    // ratio of the Viewer frame's position and size
    QRectF naviRatio(
        (-(float)m_viewer->width() * 0.5f - (float)imgBottomLeft.x) /
            imgSizeInViewer.width(),
        1.0f - ((float)m_viewer->height() * 0.5f - (float)imgBottomLeft.y) /
                   imgSizeInViewer.height(),
        (float)m_viewer->width() / imgSizeInViewer.width(),
        (float)m_viewer->height() / imgSizeInViewer.height());

    naviRect = QRect(iconImgRect.left() +
                         (int)(naviRatio.left() * (float)iconImgRect.width()),
                     iconImgRect.top() +
                         (int)(naviRatio.top() * (float)iconImgRect.height()),
                     (int)((float)iconImgRect.width() * naviRatio.width()),
                     (int)((float)iconImgRect.height() * naviRatio.height()));
    // for drag move
    m_naviRectPos = naviRect.center();

    naviRect = naviRect.intersected(frameRect);

    m_icon2ViewerRatio.setX(imgSizeInViewer.width() /
                            (float)iconImgRect.width());
    m_icon2ViewerRatio.setY(imgSizeInViewer.height() /
                            (float)iconImgRect.height());
  }

  //--- compute navigator rect end ---

  int frameCount = (int)fids.size();

  bool isReadOnly = false;
  if (sl) isReadOnly = sl->isReadOnly();

  int i;
  int iconWidth   = m_iconSize.width();
  int x0          = m_frameLabelWidth;
  int x1          = x0 + iconWidth;
  int frameHeight = m_iconSize.height();

  // linee orizzontali che separano i frames
  p.setPen(getLightLineColor());
  for (i = i0; i <= i1; i++) {
    if (m_isVertical) {
      int y = index2y(i) + frameHeight;
      p.drawLine(0, y, x1, y);
    } else {
      int x = index2x(i) + iconWidth;
      p.drawLine(x, 0, x, frameHeight);
    }
  }

  TFilmstripSelection::InbetweenRange range = m_selection->getInbetweenRange();

  // draw for each frames
  for (i = i0; i <= i1; i++) {
    QRect tmp_iconImgRect, tmp_frameRect;
    if (m_isVertical) {
      tmp_iconImgRect = iconImgRect.translated(QPoint(0, oneFrameHeight * i));
      tmp_frameRect   = frameRect.translated(QPoint(0, oneFrameHeight * i));
    } else {
      tmp_iconImgRect = iconImgRect.translated(QPoint(oneFrameWidth * i, 0));
      tmp_frameRect   = frameRect.translated(QPoint(oneFrameWidth * i, 0));
    }
    bool isCurrentFrame =
        currentContextLevel &&
        (i == sl->fid2index(TApp::instance()->getCurrentFrame()->getFid()));
    bool isSelected =
        (0 <= i && i < frameCount && m_selection->isSelected(fids[i]));
    TFrameId fid;
    if (0 <= i && i < frameCount) {
      fid = fids[i];
      // normal or inbetween (for vector levels)
      int flags = (sl->getType() == PLI_XSHLEVEL && range.first < fid &&
                   fid < range.second)
                      ? F_INBETWEEN_RANGE
                      : F_NORMAL;

      // draw icons
      drawFrameIcon(p, tmp_iconImgRect, i, fid, flags);

      p.setPen(Qt::NoPen);
      p.setBrush(Qt::NoBrush);
      p.drawRect(tmp_iconImgRect);

      // Frame number
      if (m_selection->isSelected(fids[i])) {
        if (currentContextLevel &&
            TApp::instance()->getCurrentFrame()->isEditingLevel() &&
            isCurrentFrame)
          p.setPen(Qt::red);
        else
          p.setPen(Qt::white);
      } else
        p.setPen(QColor(192, 192, 192));

      p.setBrush(Qt::NoBrush);
      // for single frame
      QString text;
      if (fid.getNumber() == TFrameId::EMPTY_FRAME ||
          fid.getNumber() == TFrameId::NO_FRAME) {
        text = QString("Single Frame");
      }
      // for sequential frame (with letter)
      else if (Preferences::instance()->isShowFrameNumberWithLettersEnabled()) {
        text = fidToFrameNumberWithLetter(fid.getNumber());
      }
      // for sequential frame
      else {
        QString letter = fid.getLetter();
        text = QString::number(fid.getNumber()).rightJustified(4, '0') +
               (!letter.isEmpty() ? letter : "");
      }
      p.drawText(tmp_frameRect.adjusted(0, 0, -3, 2), text,
                 QTextOption(Qt::AlignRight | Qt::AlignBottom));
      p.setPen(Qt::NoPen);

      // Read-only frames (lock)
      if (0 <= i && i < frameCount) {
        if (sl->isFrameReadOnly(fids[i])) {
          static QPixmap lockPixmap(":Resources/forbidden.png");
          p.drawPixmap(tmp_frameRect.bottomLeft() + QPoint(3, -13), lockPixmap);
        }
      }
    }

    // navigator rect
    if (m_showNavigator && naviRect.isValid() && fid >= 0 &&
        fid == getCurrentFrameId()) {
      p.setPen(QPen(Qt::red, 1));
      if (m_isVertical) {
        p.drawRect(naviRect.translated(0, oneFrameHeight * i));
      } else {
        p.drawRect(naviRect.translated(oneFrameWidth * i, 0));
      }
      p.setPen(Qt::NoPen);
    }

    // red frame for the current frame
    if (currentContextLevel &&
        TApp::instance()->getCurrentFrame()->isEditingLevel() &&
        (isCurrentFrame || isSelected)) {
      QPen pen;
      pen.setColor(Qt::red);
      pen.setWidth(2);
      pen.setJoinStyle(Qt::RoundJoin);
      p.setPen(pen);

      p.drawRect(tmp_frameRect.adjusted(-1, -1, 2, 2));
      p.setPen(Qt::NoPen);
    }
    if (0 <= i && i < frameCount) {
      QStyleOptionButton mark;
      mark.initFrom(this);
      mark.rect = cadmiumReferenceRect(i);
      mark.state |= sl->getCadmiumReferences().count(fids[i])
                        ? QStyle::State_On : QStyle::State_Off;
      if (!Cadmium::canPaintLevel(sl)) mark.state &= ~QStyle::State_Enabled;
      style()->drawControl(QStyle::CE_CheckBox, &mark, &p, this);
    }
  }

  // se sono in modalita' level edit faccio vedere la freccia che indica il
  // frame corrente
  if (currentContextLevel &&
      TApp::instance()->getCurrentFrame()->isEditingLevel())
    m_frameHeadGadget->draw(p, QColor(Qt::white), QColor(Qt::black));
}

//-----------------------------------------------------------------------------

void FilmstripFrames::drawFrameIcon(QPainter &p, const QRect &r, int index,
                                    const TFrameId &fid, int flags) {
  QPixmap pm;
  TXshSimpleLevel *sl = getLevel();
  TXshLevel *xl       = sl;
  if (sl && xl) {
    const bool needNativeSize =
        m_responsiveThumbnails &&
        (m_renderIconSize.width() > m_prefIconSize.width() ||
         m_renderIconSize.height() > m_prefIconSize.height());
    if (needNativeSize) {
      const TDimension dim(m_renderIconSize.width(), m_renderIconSize.height());
      pm = IconGenerator::instance()->getResponsiveIcon(xl, fid, dim);
    }
    if (pm.isNull()) pm = IconGenerator::instance()->getIcon(xl, fid);
  }
  if (!pm.isNull()) {
    if (pm.size() == r.size()) {
      p.drawPixmap(r.topLeft(), pm);
    } else {
      const QPixmap scaled =
          pm.scaled(r.size(), Qt::KeepAspectRatio, Qt::SmoothTransformation);
      QRect dest(scaled.rect());
      dest.moveCenter(r.center());
      p.drawPixmap(dest.topLeft(), scaled);
    }

    if (sl && sl->getType() == PLI_XSHLEVEL && flags & F_INBETWEEN_RANGE) {
      if (m_isVertical) {
        int x1 = r.right();
        int x0 = x1 - 12;
        int y0 = r.top();
        int y1 = r.bottom();
        p.fillRect(x0, y0, x1 - x0 + 1, y1 - y0 + 1,
                   QColor(180, 180, 180, 255));
        p.setPen(Qt::black);
        p.drawLine(x0 - 1, y0, x0 - 1, y1);

        QRectF txtRect(y0 + 1, -x1, y1 - y0 - 1, x1 - x0 + 1);
        QFontMetricsF tmpFm(p.font());
        QRectF bbox = tmpFm.boundingRect(
            txtRect, Qt::AlignBottom | Qt::AlignHCenter, tr("INBETWEEN"));
        double ratio = std::min(1.0, txtRect.width() / bbox.width());

        p.save();
        p.setRenderHint(QPainter::TextAntialiasing);
        p.rotate(90.0);
        p.scale(ratio, 1.0);
        p.drawText(QRectF(txtRect.left() / ratio, txtRect.top(),
                          txtRect.width() / ratio, txtRect.height()),
                   tr("INBETWEEN"),
                   QTextOption(Qt::AlignBottom | Qt::AlignHCenter));
        p.restore();
      } else {
        int x1 = r.right();
        int x0 = r.left();
        int y0 = r.bottom() - 15;
        int y1 = r.bottom();
        p.fillRect(x0, y0, x1 - x0 + 1, y1 - y0 + 1,
                   QColor(180, 180, 180, 255));
        p.setPen(Qt::black);
        p.drawLine(x0, y0, x1, y0);
        p.drawText(r, tr("INBETWEEN"),
                   QTextOption(Qt::AlignBottom | Qt::AlignHCenter));
      }
    }
  } else {
    // non riesco (per qualche ragione) a visualizzare l'icona
    p.fillRect(r, QColor(255, 200, 200));
    p.setPen(Qt::black);
    p.drawText(r, tr("no icon"), QTextOption(Qt::AlignCenter));
  }
}

void FilmstripFrames::enterEvent(TEnterEvent *event) { getViewer(); }

//-----------------------------------------------------------------------------

TFrameId FilmstripFrames::getCurrentFrameId() {
  TApp *app = TApp::instance();
  if (!isCurrentContextLevel()) return TFrameId();

  TFrameHandle *fh = app->getCurrentFrame();
  TFrameId currFid;
  if (fh->isEditingLevel())
    currFid = fh->getFid();
  else {
    TXsheet *xsh = app->getCurrentXsheet()->getXsheet();
    int col      = app->getCurrentColumn()->getColumnIndex();
    int row      = fh->getFrame();
    if (row < 0 || col < 0) return TFrameId();
    TXshCell cell = xsh->getCell(row, col);
    // if (cell.isEmpty()) return;
    currFid = cell.getFrameId();
  }
  return currFid;
}

//-----------------------------------------------------------------------------

void FilmstripFrames::markCadmiumReferences(bool marked) {
  changeCadmiumReferences(getLevel(), m_selection->getSelectedFids(), marked);
  update();
}

void FilmstripFrames::paintCadmiumSelection() {
  CadmiumPopup::open(getLevel(), m_selection->getSelectedFids(), this);
}

QRect FilmstripFrames::cadmiumReferenceRect(int index) const {
  if (m_isVertical)
    return QRect(width() - cadmiumGutter + 2,
                 index2y(index) + getOneFrameHeight() / 2 - 8, 16, 16);
  return QRect(index2x(index) + getOneFrameWidth() / 2 - 8,
               height() - cadmiumGutter + 2, 16, 16);
}

void FilmstripFrames::mousePressEvent(QMouseEvent *event) {
  m_selecting = false;
  int index   = 0;
  if (m_isVertical) {
    index = y2index(event->pos().y());
  } else {
    index = x2index(event->pos().x());
  }
  TFrameId fid = index2fid(index);

  TXshSimpleLevel *sl = getLevel();
  if (sl && sl->fid2index(fid) >= 0 && event->button() == Qt::LeftButton &&
      cadmiumReferenceRect(index).contains(event->pos())) {
    changeCadmiumReferences(sl, {fid}, !sl->getCadmiumReferences().count(fid));
    update();
    return;
  }
  if (sl) activateLevel();

  int frameHeight = m_iconSize.height() + fs_frameSpacing + fs_iconMarginTop +
                    fs_iconMarginBottom;
  int frameWidth = m_iconSize.width() + fs_frameSpacing + fs_iconMarginLR +
                   fs_leftMargin + fs_rightMargin;

  int i0;
  QPoint clickedPos;
  bool actualIconClicked;

  if (m_isVertical) {
    i0         = y2index(0);
    clickedPos = event->pos() - QPoint(0, (index - i0) * frameHeight);
  } else {
    i0         = x2index(0);
    clickedPos = event->pos() - QPoint((index - i0) * frameWidth, 0);
  }
  actualIconClicked =
      QRect(QPoint(fs_leftMargin + fs_iconMarginLR,
                   fs_frameSpacing / 2 +
                       fs_iconMarginTop)  //<- top-left position of the icon
            ,
            m_iconSize)
          .contains(clickedPos);

  if (event->button() == Qt::LeftButton ||
      event->button() == Qt::MiddleButton) {
    // navigator pan
    // make sure the viewer is visible and that a toonz raster or raster level
    // is current
    if (fid.getNumber() >= 0 && fid == getCurrentFrameId() &&
        (sl->getType() == TZP_XSHLEVEL || sl->getType() == OVL_XSHLEVEL) &&
        m_viewer && m_viewer->isVisible() && actualIconClicked &&
        event->button() == Qt::MiddleButton) {
      if (m_showNavigator) {
        m_isNavigatorPanning = true;
        execNavigatorPan(event->pos());
        QApplication::setOverrideCursor(Qt::ClosedHandCursor);
      }
    } else
      m_isNavigatorPanning = false;
    // end of navigator section

    // return if frame empty or middle button pressed
    if (fid == TFrameId() || event->button() == Qt::MiddleButton) {
      m_justStartedSelection = false;
      return;
    }

    // was the inbetween button clicked?
    bool inbetweenSelected = false;
    if (m_isVertical)
      inbetweenSelected = event->pos().x() > width() - cadmiumGutter - 20 - fs_rightMargin &&
                          event->pos().x() < width() - cadmiumGutter;
    else
      inbetweenSelected =
          event->pos().y() > height() - cadmiumGutter - fs_iconMarginBottom - 20 &&
          event->pos().y() < height() - cadmiumGutter - fs_iconMarginBottom - fs_frameSpacing;

    // with shift or control
    if (event->modifiers() & Qt::ShiftModifier) {
      select(index, SHIFT_SELECT);
      if (m_selection->isSelected(fid)) {
        // If the frame is already selected enable
        // drag'n'drop
        m_dragDropArmed = true;
        m_pos           = event->pos();
      }
    } else if (event->modifiers() & Qt::ControlModifier) {
      select(index, CTRL_SELECT);
      if (m_selection->isSelected(fid)) {
        // Ctrl-click-drag is the explicit copy gesture. Arm the drag
        // immediately after toggling this frame into the selection.
        m_dragDropArmed = true;
        m_pos           = event->pos();
      }
    } else if (sl->getType() == PLI_XSHLEVEL &&
             m_selection->isInInbetweenRange(fid) && inbetweenSelected) {
      QPointer<FilmstripFrames> self(this);
      inbetween();
      if (!self) return;
    } else {
      // move current frame when clicked without modifier
      TApp *tapp = TApp::instance();
      std::vector<TFrameId> fids;
      sl->getFids(fids);

      tapp->getCurrentFrame()->setFrameIds(fids);
      tapp->getCurrentFrame()->setFid(fid);

      if (actualIconClicked && !m_selection->isSelected(fid)) {
        // Plain click-drag on an unselected drawing should immediately move
        // that drawing. Multi-frame drag sources are built explicitly with
        // Shift/Ctrl selection instead of competing with drag-select here.
        select(index, ONLY_SELECT);
        m_dragDropArmed = true;
        m_pos           = event->pos();
      } else if (m_selection->isSelected(fid)) {
        // If it's already selected, drag the current selection. A click
        // without enough movement still collapses the group to this frame.
        m_dragDropArmed = true;
        m_pos           = event->pos();
        m_allowResetSelection    = true;
        m_indexForResetSelection = index;
      } else if (!actualIconClicked) {
        // this allows clicking the frame number to trigger an instant drag
        select(index, ONLY_SELECT);
        m_dragDropArmed = true;
        m_pos           = event->pos();
      }
    }
    update();
  } else if (event->button() == Qt::RightButton) {
    select(index);
  }
  m_justStartedSelection = false;
}

//-----------------------------------------------------------------------------

void FilmstripFrames::execNavigatorPan(const QPoint &point) {
  int index = y2index(point.y());
  if (!m_isVertical) index = x2index(point.x());
  TFrameId fid = index2fid(index);
  int i0       = y2index(0);
  if (!m_isVertical) i0 = x2index(0);

  int frameHeight = m_iconSize.height() + fs_frameSpacing + fs_iconMarginTop +
                    fs_iconMarginBottom;
  int frameWidth = m_iconSize.width() + fs_frameSpacing + fs_iconMarginLR +
                   fs_leftMargin + fs_rightMargin;
  QPoint clickedPos = point - QPoint(0, (index - i0) * frameHeight);
  if (!m_isVertical) clickedPos = point - QPoint((index - i0) * frameWidth, 0);

  if (fid != getCurrentFrameId()) return;

  QRect iconRect =
      QRect(QPoint(fs_leftMargin + fs_iconMarginLR,
                   fs_frameSpacing / 2 +
                       fs_iconMarginTop)  //<- top-left position of the icon
            ,
            m_iconSize);

  QPointF delta = m_naviRectPos - clickedPos;

  if (iconRect.left() > clickedPos.x() || iconRect.right() < clickedPos.x())
    delta.setX(0.0);
  if (iconRect.top() > clickedPos.y() || iconRect.bottom() < clickedPos.y())
    delta.setY(0.0);
  if (delta.x() == 0.0 && delta.y() == 0.0) return;

  delta.setX(delta.x() * m_icon2ViewerRatio.x());
  delta.setY(delta.y() * m_icon2ViewerRatio.y());

  if (m_viewer) m_viewer->navigatorPan(delta.toPoint());
}

//-----------------------------------------------------------------------------

void FilmstripFrames::mouseReleaseEvent(QMouseEvent *e) {
  stopAutoPanning();
  m_selecting          = false;
  m_dragDropArmed      = false;
  m_isNavigatorPanning = false;
  if (m_allowResetSelection) {
    select(m_indexForResetSelection, ONLY_SELECT);
    update();
  }
  m_allowResetSelection    = false;
  m_indexForResetSelection = -1;
  QApplication::restoreOverrideCursor();
}

//-----------------------------------------------------------------------------

void FilmstripFrames::mouseMoveEvent(QMouseEvent *e) {
  QPoint pos = e->pos();
  //m_dragDropArmed
  int index;
  if (m_isVertical) {
    index = y2index(e->pos().y());
    if (isOutOfBounds(e->pos().x())) m_dragDropArmed = true;
  } else {
    index = x2index(e->pos().x());
    if (isOutOfBounds(e->pos().y())) m_dragDropArmed = true;
  }
  if (e->buttons() & Qt::LeftButton || e->buttons() & Qt::MiddleButton) {
    // navigator pan
    if (m_showNavigator && m_isNavigatorPanning) {
      execNavigatorPan(e->pos());
      e->accept();
      return;
    }
    if (e->buttons() & Qt::MiddleButton) return;
    if (m_dragDropArmed) {
      if ((m_pos - e->pos()).manhattanLength() > 10) {
        startDragDrop();
        m_dragDropArmed       = false;
        m_allowResetSelection = false;
      }
    } else if (m_selecting) {
      m_pos = e->globalPos();
      select(index, DRAG_SELECT);
    }

    // autopan
    int speed = getOneFrameHeight() / 64;
    if (!m_isVertical) speed = getOneFrameWidth() / 64;

    QRect visibleRect = visibleRegion().boundingRect();
    int visibleTop    = visibleRect.top();
    int visibleBottom = visibleRect.bottom();
    if (m_isVertical) {
      if (pos.y() < visibleRect.top()) {
        m_scrollSpeed = -speed;
        if (visibleRect.top() - pos.y() > 30) {
          m_timerInterval = 50;
        } else if (visibleRect.top() - pos.y() > 15) {
          m_timerInterval = 150;
        } else {
          m_timerInterval = 300;
        }
      } else if (pos.y() > visibleRect.bottom()) {
        m_scrollSpeed = speed;
        if (pos.y() - visibleRect.bottom() > 30) {
          m_timerInterval = 50;
        } else if (pos.y() - visibleRect.bottom() > 15) {
          m_timerInterval = 150;
        } else {
          m_timerInterval = 300;
        }
      } else
        m_scrollSpeed = 0;
    } else {
      if (pos.x() < visibleRect.left()) {
        m_scrollSpeed = -speed;
        if (visibleRect.left() - pos.x() > 30) {
          m_timerInterval = 50;
        } else if (visibleRect.left() - pos.x() > 15) {
          m_timerInterval = 150;
        } else {
          m_timerInterval = 300;
        }
      } else if (pos.x() > visibleRect.right()) {
        m_scrollSpeed = speed;
        if (pos.x() - visibleRect.right() > 30) {
          m_timerInterval = 50;
        } else if (pos.x() - visibleRect.right() > 15) {
          m_timerInterval = 150;
        } else {
          m_timerInterval = 300;
        }
      } else
        m_scrollSpeed = 0;
    }
    if (m_scrollSpeed != 0) {
      startAutoPanning();
    } else
      stopAutoPanning();
    update();
  } else if (e->buttons() & Qt::MiddleButton) {
    // scroll con il tasto centrale
    pos = e->globalPos();
    if (m_isVertical) {
      scroll((m_pos.y() - pos.y()) * 10);
    } else {
      scroll((m_pos.x() - pos.x()) * 10);
    }
    m_pos = pos;
  } else {
    TFrameId fid        = index2fid(index);
    TXshSimpleLevel *sl = getLevel();

    if (m_isVertical && sl && m_selection && sl->getType() == PLI_XSHLEVEL &&
        m_selection->isInInbetweenRange(fid) &&
        e->pos().x() > width() - 20 - fs_rightMargin) {
      setToolTip(tr("Auto Inbetween"));
    }
    if (!m_isVertical && sl && m_selection && sl->getType() == PLI_XSHLEVEL &&
        m_selection->isInInbetweenRange(fid) &&
        e->pos().y() > height() - 15 - fs_iconMarginBottom &&
        e->pos().y() < height() - fs_iconMarginBottom - fs_frameSpacing) {
      setToolTip(tr("Auto Inbetween"));
    }
  }
}

//-----------------------------------------------------------------------------

void FilmstripFrames::keyPressEvent(QKeyEvent *event) {
  TXshSimpleLevel *level = getLevel();
  if (!level) return;

  activateLevel();
  TFrameHandle *fh = TApp::instance()->getCurrentFrame();
  std::vector<TFrameId> fids;
  level->getFids(fids);
  if (fids.empty()) return;

  // If on a level frame pass the frame id after the last frame to allow
  // creating a new frame with the down arrow key
  TFrameId newId = 0;
  if (Preferences::instance()->getDownArrowLevelStripNewFrame() &&
      fh->getFrameType() == TFrameHandle::LevelFrame) {
    int frameCount = (int)fids.size();
    newId          = index2fid(frameCount);
  }

  fh->setFrameIds(fids);
  if (event->key() == Qt::Key_Up || event->key() == Qt::Key_Left)
    fh->prevFrame();
  else if (event->key() == Qt::Key_Down || event->key() == Qt::Key_Right)
    fh->nextFrame(newId);
  else if (event->key() == Qt::Key_Home)
    fh->firstFrame();
  else if (event->key() == Qt::Key_End)
    fh->lastFrame();
  else if (event->key() == Qt::Key_PageDown) {
    if (m_isVertical) {
      int frameHeight   = m_iconSize.height();
      int visibleHeight = visibleRegion().begin()[0].height();
      int visibleFrames = double(visibleHeight) / double(frameHeight);
      scroll(visibleFrames * frameHeight);
    } else {
      int frameWidth    = m_iconSize.width();
      int visibleWidth  = visibleRegion().begin()[0].width();
      int visibleFrames = double(visibleWidth) / double(frameWidth);
      scroll(visibleFrames * frameWidth);
    }
    return;
  } else if (event->key() == Qt::Key_PageUp) {
    if (m_isVertical) {
      int frameHeight   = m_iconSize.height();
      int visibleHeight = visibleRegion().begin()[0].height();
      int visibleFrames = double(visibleHeight) / double(frameHeight);
      scroll(-visibleFrames * frameHeight);
    } else {
      int frameWidth    = m_iconSize.width();
      int visibleWidth  = visibleRegion().begin()[0].width();
      int visibleFrames = double(visibleWidth) / double(frameWidth);
      scroll(-visibleFrames * frameWidth);
    }
    return;
  } else
    return;

  m_selection->selectNone();
  if (getLevel()) m_selection->select(fh->getFid());
  int index = fid2index(fh->getFid());
  if (index >= 0) showFrame(index);
}

//-----------------------------------------------------------------------------

void FilmstripFrames::wheelEvent(QWheelEvent *event) {
  scroll(-event->angleDelta().y());
}

//-----------------------------------------------------------------------------

void FilmstripFrames::startAutoPanning() {
  if (m_timerId == 0) m_timerId = startTimer(m_timerInterval);
}

//-----------------------------------------------------------------------------

void FilmstripFrames::stopAutoPanning() {
  if (m_timerId != 0) {
    killTimer(m_timerId);
    m_timerId     = 0;
    m_scrollSpeed = 0;
  }
}

//-----------------------------------------------------------------------------

void FilmstripFrames::timerEvent(QTimerEvent *) {
  scroll(m_scrollSpeed);
  // reset the timer in case m_scroll speed has changed
  killTimer(m_timerId);
  m_timerId = 0;
  m_timerId = startTimer(m_timerInterval);
  if (m_selecting) {
    QPoint pos = mapFromGlobal(m_pos);
    int index  = y2index(pos.y());
    if (!m_isVertical) index = x2index(pos.x());
    select(index, DRAG_SELECT);
    showFrame(index);
    update();
  }
}

//-----------------------------------------------------------------------------

void FilmstripFrames::contextMenuEvent(QContextMenuEvent *event) {
  QMenu *menu         = new QMenu();
  TXshSimpleLevel *sl = getLevel();
  if (sl) activateLevel();
  m_selection->makeCurrent();

  bool isSubsequenceLevel = (sl && sl->isSubsequence());
  bool isReadOnly         = (sl && sl->isReadOnly());
  CommandManager *cm      = CommandManager::instance();

  menu->addAction(cm->getAction(MI_SelectAll));
  menu->addAction(cm->getAction(MI_InvertSelection));
  QMenu *cadmium = menu->addMenu(tr("Cadmium Assisted Paint"));
  cadmium->addAction(cm->getAction(MI_CadmiumMarkReferences));
  cadmium->addAction(cm->getAction(MI_CadmiumClearReferences));
  cadmium->addAction(cm->getAction(MI_CadmiumPaint));
  QAction *resyncAction = menu->addAction(tr("Resync Level Strips"));
  connect(resyncAction, &QAction::triggered, this,
          [this] { emit levelStripsResyncRequestedSignal(); });
  menu->addSeparator();
  if (!isSubsequenceLevel && !isReadOnly) {
    menu->addAction(cm->getAction(MI_Cut));
  }
  menu->addAction(cm->getAction(MI_Copy));

  if (!isSubsequenceLevel && !isReadOnly) {
    menu->addAction(cm->getAction(MI_Paste));
    menu->addAction(cm->getAction(MI_PasteInto));
    menu->addAction(cm->getAction(MI_Insert));
    menu->addAction(cm->getAction(MI_Clear));
    menu->addSeparator();
    menu->addAction(cm->getAction(MI_Reverse));
    menu->addAction(cm->getAction(MI_Swing));
    menu->addAction(cm->getAction(MI_Step2));
    menu->addAction(cm->getAction(MI_Step3));
    menu->addAction(cm->getAction(MI_Step4));
    menu->addAction(cm->getAction(MI_Each2));
    menu->addAction(cm->getAction(MI_Each3));
    menu->addAction(cm->getAction(MI_Each4));
    menu->addSeparator();
    menu->addAction(cm->getAction(MI_Duplicate));
    menu->addAction(cm->getAction(MI_MergeFrames));
  }
  menu->addAction(cm->getAction(MI_ExposeResource));
  if (!isSubsequenceLevel && !isReadOnly) {
    menu->addAction(cm->getAction(MI_AddFrames));
    menu->addAction(cm->getAction(MI_Renumber));
    if (sl && sl->getType() == TZP_XSHLEVEL)
      menu->addAction(cm->getAction(MI_RevertToCleanedUp));
  }
  if (sl && (sl->getType() == TZP_XSHLEVEL || sl->getType() == PLI_XSHLEVEL ||
             (sl->getType() == OVL_XSHLEVEL && !sl->getPath().isUneditable())))
    menu->addAction(cm->getAction(MI_RevertToLastSaved));
  menu->addSeparator();
  createSelectLevelMenu(menu);
  QMenu *panelMenu           = menu->addMenu(tr("Panel Settings"));
  QAction *toggleOrientation = panelMenu->addAction(tr("Toggle Orientation"));
  QAction *hideComboBox = panelMenu->addAction(tr("Show/Hide Drop Down Menu"));
  QAction *hideNavigator =
      panelMenu->addAction(tr("Show/Hide Level Navigator"));
  QAction *responsiveThumbs = panelMenu->addAction(tr("Responsive Thumbnails"));
  hideComboBox->setCheckable(true);
  hideComboBox->setChecked(m_showComboBox);
  hideNavigator->setCheckable(true);
  hideNavigator->setChecked(m_showNavigator);
  responsiveThumbs->setCheckable(true);
  responsiveThumbs->setChecked(m_responsiveThumbnails);
  connect(toggleOrientation, SIGNAL(triggered(bool)), this,
          SLOT(orientationToggled(bool)));
  connect(hideComboBox, SIGNAL(triggered(bool)), this,
          SLOT(comboBoxToggled(bool)));
  connect(hideNavigator, SIGNAL(triggered(bool)), this,
          SLOT(navigatorToggled(bool)));
  connect(responsiveThumbs, SIGNAL(triggered(bool)), this,
          SLOT(responsiveThumbnailsToggled(bool)));

  menu->exec(event->globalPos());
}

//-----------------------------------------------------------------------------

void FilmstripFrames::createSelectLevelMenu(QMenu *menu) {
  ToonzScene *scene = TApp::instance()->getCurrentScene()->getScene();
  if (scene) {
    std::vector<TXshLevel *> levels;
    scene->getLevelSet()->listLevels(levels);
    std::set<TXshLevel *> usedLevels;
    scene->getTopXsheet()->getUsedLevels(usedLevels);
    std::vector<TXshLevel *>::iterator it;
    int i       = 0;
    bool active = false;
    QMenu *levelSelectMenu;
    for (it = levels.begin(); it != levels.end(); ++it) {
      TXshSimpleLevel *sl = (*it)->getSimpleLevel();
      if (sl) {
        // register only used level in xsheet
        if (usedLevels.count(sl) == 0) continue;
        QString levelName = QString::fromStdWString(sl->getName());
        if (i == 0) {
          levelSelectMenu = menu->addMenu(tr("Select Level"));
          active          = true;
        }
        if (active) {
          QAction *action = levelSelectMenu->addAction(levelName);
          connect(action, &QAction::triggered, [=] { levelSelected(i); });
        }
        i++;
      }
    }
  }
}

//-----------------------------------------------------------------------------

void FilmstripFrames::levelSelected(int index) {
  emit(levelSelectedSignal(index));
}

//-----------------------------------------------------------------------------

void FilmstripFrames::comboBoxToggled(bool ignore) {
  emit(comboBoxToggledSignal());
}

//-----------------------------------------------------------------------------

void FilmstripFrames::navigatorToggled(bool ignore) {
  emit(navigatorToggledSignal());
}

//-----------------------------------------------------------------------------

void FilmstripFrames::responsiveThumbnailsToggled(bool responsive) {
  emit(responsiveThumbnailsToggledSignal(responsive));
}

//-----------------------------------------------------------------------------

void FilmstripFrames::orientationToggled(bool ignore) {
  m_isVertical = !m_isVertical;
  emit(orientationToggledSignal(m_isVertical));
}

//-----------------------------------------------------------------------------

void FilmstripFrames::setOrientation(bool isVertical) {
  m_isVertical = isVertical;
  updateIconLayout();
  TApp *app        = TApp::instance();
  TFrameHandle *fh = app->getCurrentFrame();
  TFrameId fid     = getCurrentFrameId();

  int index = fid2index(fid);
  if (index >= 0) {
    showFrame(index);
  }
  update();
}

//-----------------------------------------------------------------------------

void FilmstripFrames::setNavigator(bool showNavigator) {
  m_showNavigator = showNavigator;
}

//-----------------------------------------------------------------------------

void FilmstripFrames::setComboBox(bool showComboBox) {
  m_showComboBox = showComboBox;
}

//-----------------------------------------------------------------------------

void FilmstripFrames::onLevelChanged() {
  if (!isCurrentLevel()) return;

  if (m_isVertical)
    updateContentHeight();
  else
    updateContentWidth();
  update();
}

//-----------------------------------------------------------------------------

void FilmstripFrames::onLevelSwitched(TXshLevel *) {
  if (!isCurrentLevel()) return;

  if (m_isVertical)
    updateContentHeight(0);
  else
    updateContentWidth(0);

  if (isCurrentContextLevel())
    onFrameSwitched();  // deve visualizzare il frame corrente nella levelstrip
  else
    update();
}

//-----------------------------------------------------------------------------

void FilmstripFrames::onFrameSwitched() {
  if (!isCurrentContextLevel()) return;

  // no. interferische con lo shift-click per la selezione.
  // m_selection->selectNone();
  TApp *app        = TApp::instance();
  TFrameHandle *fh = app->getCurrentFrame();
  TFrameId fid     = getCurrentFrameId();

  int index = fid2index(fid);
  if (index >= 0) {
    showFrame(index);

    if (m_isActive) {
      TFilmstripSelection *fsSelection =
          dynamic_cast<TFilmstripSelection *>(TSelection::getCurrent());

      // don't select if already selected - may be part of a group selection
      if (!m_selection->isSelected(index2fid(index)) && fsSelection) {
        select(index, ONLY_SELECT);
        m_justStartedSelection = true;
      }
    }
  }
  update();
}

//-----------------------------------------------------------------------------

void FilmstripFrames::startDragDrop() {
  TRepetitionGuard guard;
  if (!guard.hasLock()) return;

  TXshSimpleLevel *sl = getLevel();
  if (!sl) return;
  const std::set<TFrameId> &fids = m_selection->getSelectedFids();
  if (fids.empty()) return;
  QByteArray byteArray;

  QMimeData *mimeData = new QMimeData;
  mimeData->setData("application/vnd.toonz.drawings", byteArray);
  QDrag *drag           = new QDrag(this);
  QPixmap dropThumbnail = IconGenerator::instance()->getIcon(sl, *fids.begin());
  if (!dropThumbnail.isNull()) drag->setPixmap(dropThumbnail);
  drag->setMimeData(mimeData);

  s_dragSourceFilmstrip = this;
  s_dragSourceFrames    = fids;
  m_dragInProgress      = true;
  drag->exec(Qt::CopyAction | Qt::MoveAction, Qt::MoveAction);
  m_dragInProgress = false;
  s_dragSourceFrames.clear();
  s_dragSourceFilmstrip = nullptr;

  // Responsive thumbnail mode can receive viewport resize events while
  // QDrag::exec() runs its nested event loop. Keep the frame metrics fixed
  // for the drag, then reconcile the responsive layout once the drop ends.
  if (m_responsiveThumbnails) updateIconLayout();
}

//-----------------------------------------------------------------------------

int FilmstripFrames::dropInsertionIndex(const QPoint &pos) const {
  TXshSimpleLevel *level = getLevel();
  if (!level) return 0;

  int index = m_isVertical ? y2index(pos.y()) : x2index(pos.x());
  index = std::max(0, std::min(index, level->getFrameCount()));

  if (index < level->getFrameCount()) {
    const int frameStart = m_isVertical ? index2y(index) : index2x(index);
    const int frameSize =
        m_isVertical ? getOneFrameHeight() : getOneFrameWidth();
    const int cursor = m_isVertical ? pos.y() : pos.x();
    const int local  = cursor - frameStart;

    // Responsive thumbnails can make the visible gutter very small. Give
    // each before/after boundary a wider logical snap zone without changing
    // thumbnail spacing or causing a layout reflow during the drag.
    const int edgeZone =
        m_responsiveThumbnails
            ? std::max(12, std::min(frameSize / 4, 24))
            : std::max(4, fs_frameSpacing);

    if (local <= edgeZone)
      return index;
    if (local >= frameSize - edgeZone)
      return std::min(index + 1, level->getFrameCount());

    if (cursor >= frameStart + frameSize / 2) ++index;
  }

  return std::max(0, std::min(index, level->getFrameCount()));
}

//-----------------------------------------------------------------------------

void FilmstripFrames::refreshLevelContent() {
  if (m_isVertical)
    updateContentHeight(0);
  else
    updateContentWidth(0);
  update();
}

//-----------------------------------------------------------------------------

void FilmstripFrames::selectDroppedFrames(const std::set<TFrameId> &fids) {
  m_selection->selectNone();
  for (const TFrameId &fid : fids) {
    int index = fid2index(fid);
    if (index >= 0) m_selection->select(fid);
  }
  if (!fids.empty()) {
    int index = fid2index(*fids.begin());
    if (index >= 0) showFrame(index);
  }
  update();
}

//-----------------------------------------------------------------------------

void FilmstripFrames::dragEnterEvent(QDragEnterEvent *event) {
  if (!s_dragSourceFilmstrip ||
      !event->mimeData()->hasFormat("application/vnd.toonz.drawings") ||
      !getLevel()) {
    event->ignore();
    return;
  }

  event->acceptProposedAction();
}

//-----------------------------------------------------------------------------

void FilmstripFrames::dragMoveEvent(QDragMoveEvent *event) {
  TXshSimpleLevel *sourceLevel =
      s_dragSourceFilmstrip ? s_dragSourceFilmstrip->getLevel() : nullptr;
  TXshSimpleLevel *targetLevel = getLevel();
  if (!sourceLevel || !targetLevel || targetLevel->isSubsequence() ||
      targetLevel->isReadOnly()) {
    event->ignore();
    return;
  }

  const bool copyRequested =
      event->keyboardModifiers() & Qt::ControlModifier;
  Qt::DropAction action = Qt::MoveAction;
  if (sourceLevel != targetLevel &&
      (copyRequested || sourceLevel->isReadOnly() ||
       sourceLevel->isSubsequence()))
    action = Qt::CopyAction;

  event->setDropAction(action);
  event->accept();
}

//-----------------------------------------------------------------------------

void FilmstripFrames::dropEvent(QDropEvent *event) {
  FilmstripFrames *sourceStrip = s_dragSourceFilmstrip;
  TXshSimpleLevel *sourceLevel = sourceStrip ? sourceStrip->getLevel() : nullptr;
  TXshSimpleLevel *targetLevel = getLevel();
  if (!sourceLevel || !targetLevel || s_dragSourceFrames.empty()) {
    event->ignore();
    return;
  }

  activateLevel();
  const int targetIndex = dropInsertionIndex(event->pos());

  if (sourceLevel == targetLevel) {
    std::vector<TFrameId> oldFids;
    targetLevel->getFids(oldFids);

    std::vector<TFrameId> moving;
    std::vector<TFrameId> remaining;
    int insertionIndex = 0;
    for (int i = 0; i < (int)oldFids.size(); ++i) {
      const TFrameId &fid = oldFids[i];
      if (s_dragSourceFrames.count(fid))
        moving.push_back(fid);
      else {
        if (i < targetIndex) ++insertionIndex;
        remaining.push_back(fid);
      }
    }
    if (moving.empty()) {
      event->ignore();
      return;
    }

    insertionIndex = std::min(insertionIndex, (int)remaining.size());
    std::vector<TFrameId> desired = remaining;
    desired.insert(desired.begin() + insertionIndex, moving.begin(), moving.end());
    if (desired == oldFids) {
      event->setDropAction(Qt::MoveAction);
      event->accept();
      return;
    }

    std::vector<std::pair<TFrameId, TFrameId>> renumberTable;
    std::set<TFrameId> movedFids;
    for (int i = 0; i < (int)oldFids.size(); ++i) {
      renumberTable.emplace_back(desired[i], oldFids[i]);
      if (i >= insertionIndex && i < insertionIndex + (int)moving.size())
        movedFids.insert(oldFids[i]);
    }

    FilmstripCmd::reorder(targetLevel, renumberTable);
    refreshLevelContent();
    selectDroppedFrames(movedFids);

    std::vector<TFrameId> fids;
    targetLevel->getFids(fids);
    TApp::instance()->getCurrentFrame()->setFrameIds(fids);
    if (!movedFids.empty())
      TApp::instance()->getCurrentFrame()->setFid(*movedFids.begin());

    event->setDropAction(Qt::MoveAction);
    event->accept();
    return;
  }

  if (sourceLevel->getType() != targetLevel->getType()) {
    DVGui::warning(tr("Frames cannot be dropped between different level types. "
                      "Level conversion can be added as a future drop option."));
    event->ignore();
    return;
  }

  std::vector<TFrameId> targetFids;
  targetLevel->getFids(targetFids);
  std::set<TFrameId> insertionPoint;
  if (targetFids.empty())
    insertionPoint.insert(TFrameId(1));
  else if (targetIndex < (int)targetFids.size())
    insertionPoint.insert(targetFids[targetIndex]);
  else
    insertionPoint.insert(targetFids.back() + 1);

  const bool copyRequested =
      event->keyboardModifiers() & Qt::ControlModifier;
  const bool canMove =
      !copyRequested && !sourceLevel->isReadOnly() &&
      !sourceLevel->isSubsequence();
  const Qt::DropAction dropAction =
      canMove ? Qt::MoveAction : Qt::CopyAction;

  if (canMove) TUndoManager::manager()->beginBlock();

  if (!FilmstripCmd::insertFramesFromLevel(sourceLevel, s_dragSourceFrames,
                                            targetLevel, insertionPoint)) {
    if (canMove) TUndoManager::manager()->endBlock();
    event->ignore();
    return;
  }

  if (canMove) {
    std::set<TFrameId> sourceFrames = s_dragSourceFrames;
    FilmstripCmd::remove(sourceLevel, sourceFrames);
    TUndoManager::manager()->endBlock();
    sourceStrip->m_selection->selectNone();
    sourceStrip->refreshLevelContent();
  }

  refreshLevelContent();
  selectDroppedFrames(insertionPoint);
  targetFids.clear();
  targetLevel->getFids(targetFids);
  TApp::instance()->getCurrentFrame()->setFrameIds(targetFids);
  if (!insertionPoint.empty())
    TApp::instance()->getCurrentFrame()->setFid(*insertionPoint.begin());

  event->setDropAction(dropAction);
  event->accept();
}

//-----------------------------------------------------------------------------

void FilmstripFrames::inbetween() {
  TFilmstripSelection::InbetweenRange range = m_selection->getInbetweenRange();
  if (range.first >= range.second || !getLevel()) return;

  auto *app = TApp::instance();
  auto *scene = app->getCurrentScene()->getScene();
  TXshSimpleLevelP level = getLevel();
  if (level->isReadOnly()) return;
  auto *selection = app->getCurrentSelection()->getSelection();
  const auto selected = m_selection->getSelectedFids();
  const std::string hints = level->getMorphHints();
  struct ReviewedFrame {
    TFrameId fid;
    TImageP image;
    TVectorImageP snapshot;
  };
  std::vector<ReviewedFrame> reviewed;
  std::vector<TFrameId> fids;
  level->getFids(fids);
  const auto first = std::find(fids.begin(), fids.end(), range.first);
  const auto last = std::find(fids.begin(), fids.end(), range.second);
  if (first == fids.end() || last == fids.end() || last - first < 2) return;
  try {
    for (auto it = first; it != last + 1; ++it) {
      if (it != first && it != last && level->isFrameReadOnly(*it)) return;
      ReviewedFrame frame{*it, level->getFrame(*it, false), TVectorImageP()};
      TVectorImageP image = frame.image;
      if (image) {
        frame.snapshot = image->cloneWithGroupContext();
        if (image->getPalette()) frame.snapshot->setPalette(image->getPalette()->clone());
        frame.snapshot->findRegions();
      } else if (frame.image) return;
      reviewed.push_back(std::move(frame));
    }
  } catch (...) {
    DVGui::warning(tr("The inbetween preview could not be prepared. The drawings were left unchanged."));
    return;
  }
  QPointer<FilmstripFrames> self(this);
  VectorModalGuard owner(this);
  bool invalidated = false;
  bool reviewing = true;
  QObject dialogContext;
  auto sameSource = [&]() {
    if (invalidated || !self || app->getCurrentScene()->getScene() != scene ||
        app->getCurrentLevel()->getSimpleLevel() != level.getPointer() ||
        app->getCurrentSelection()->getSelection() != selection ||
        self->getLevel() != level.getPointer() || level->isReadOnly() ||
        self->m_selection->getSelectedFids() != selected ||
        self->m_selection->getInbetweenRange() != range || level->getMorphHints() != hints)
      return false;
    std::vector<TFrameId> current;
    level->getFids(current);
    current.erase(std::remove_if(current.begin(), current.end(), [&](const TFrameId &fid) {
      return fid < range.first || fid > range.second;
    }), current.end());
    if (current.size() != reviewed.size()) return false;
    for (size_t i = 0; i < reviewed.size(); ++i) {
      const auto &before = reviewed[i];
      if (current[i] != before.fid ||
          (i > 0 && i + 1 < reviewed.size() && level->isFrameReadOnly(before.fid))) return false;
      const TImageP image = level->getFrame(before.fid, false);
      if (image != before.image || (before.snapshot &&
          !VectorImageSnapshot::sameContent(TVectorImageP(image), before.snapshot))) return false;
    }
    return true;
  };
  auto invalidate = [&]() {
    if (!reviewing) return;
    invalidated = true;
    if (self && self->m_inbetweenDialog) self->m_inbetweenDialog->reject();
  };
  connect(app->getCurrentScene(), &TSceneHandle::sceneSwitched, &dialogContext, invalidate);
  connect(app->getCurrentLevel(), &TXshLevelHandle::xshLevelSwitched, &dialogContext, invalidate);
  connect(app->getCurrentSelection(), &TSelectionHandle::selectionChanged, &dialogContext, [&]() {
    if (!self || self->m_selection->getSelectedFids() != selected) invalidate();
  });
  connect(app->getCurrentSelection(), &TSelectionHandle::selectionSwitched, &dialogContext, [&]() {
    if (app->getCurrentSelection()->getSelection() != selection) invalidate();
  });
  connect(app->getCurrentLevel(), &TXshLevelHandle::xshLevelChanged, &dialogContext, [&]() {
    if (!reviewing) return;
    bool unchanged = false;
    try { unchanged = sameSource(); } catch (...) { }
    if (!unchanged) invalidate();
  });

  QSettings settings;
  QString keyName("InbetweenInterpolation");
  QString currentItem = settings.value(keyName, QStringLiteral("Linear")).toString();
  int index;

  // stroke pairs set with the Pair Strokes for Inbetween command, if any
  TVectorImageP img0 = reviewed.front().snapshot;
  TVectorImageP img1 = reviewed.back().snapshot;
  std::vector<std::pair<int, int>> strokePairs =
      StrokeSelection::getInbetweenStrokePairs(level.getPointer(), range.first, range.second);
  FilmstripCmd::InbetweenOptions options;
  std::vector<std::pair<TRectD, TRectD>> zoneHints =
      StrokeSelection::getInbetweenZoneHints(level.getPointer(), range.first, range.second);

  {
    if (!m_inbetweenDialog) {
      m_inbetweenDialog = new InbetweenDialog(this);
      // The dialog is parented to the main window. Closing this strip must
      // close its modal loop too, without deleting it inside exec().
      auto *dialog = m_inbetweenDialog;
      connect(this, &QObject::destroyed, dialog, [dialog]() {
        dialog->reject();
        dialog->deleteLater();
      });
      connect(dialog, &QObject::destroyed, this, [this]() {
        m_inbetweenDialog = nullptr;
      });
    }

    // Default -> l'ultimo valore usato

    m_inbetweenDialog->setValue(currentItem);
    m_inbetweenDialog->setEase(settings.value("InbetweenSlowStart", 0).toInt(),
                               settings.value("InbetweenSlowEnd", 0).toInt());
    m_inbetweenDialog->setAutoMatch(
        settings.value("InbetweenAutoMatch", false).toBool());
    m_inbetweenDialog->setStrokePairCount(int(strokePairs.size()));
    m_inbetweenDialog->setZoneHintCount(int(zoneHints.size()));
    m_inbetweenDialog->setUnmatchedPolicy(settings.value("InbetweenUnmatchedPolicy", 0).toInt());
    m_inbetweenDialog->setPreviewSource(img0, img1, strokePairs, zoneHints);

    QPointer<InbetweenDialog> dialog(m_inbetweenDialog);
    int ret = owner.exec(*dialog);
    reviewing = false;
    if (!self || !dialog || !ret || invalidated || !owner.valid()) return;
    bool unchanged = false;
    try { unchanged = sameSource(); } catch (...) { }
    if (!unchanged) {
      DVGui::warning(tr("The drawings, range or morph hints changed while the preview was open. Reopen Inbetween to review them. No drawing was overwritten."));
      return;
    }
    if (!m_inbetweenDialog->useStrokePairs()) strokePairs.clear();
    if (m_inbetweenDialog->useZoneHints()) options.m_zoneHints = zoneHints;
    options.m_autoMatch = m_inbetweenDialog->autoMatch();
    options.m_unmatchedPolicy = m_inbetweenDialog->unmatchedPolicy();
    settings.setValue("InbetweenUnmatchedPolicy", options.m_unmatchedPolicy);
    options.m_slowStart = m_inbetweenDialog->slowStart() / 100.0;
    options.m_slowEnd   = m_inbetweenDialog->slowEnd() / 100.0;
    settings.setValue("InbetweenSlowStart", m_inbetweenDialog->slowStart());
    settings.setValue("InbetweenSlowEnd", m_inbetweenDialog->slowEnd());
    settings.setValue("InbetweenAutoMatch", options.m_autoMatch);

    currentItem = m_inbetweenDialog->getValue();
    if (currentItem.isEmpty()) return;
    index = m_inbetweenDialog->getIndex(currentItem);
    if (index < 0) return;

    // registro il nuovo valore
  }
  settings.setValue(keyName, currentItem);

  // lo converto nella notazione di FilmstripCmd
  const FilmstripCmd::InbetweenInterpolation codes[] = {
      FilmstripCmd::II_Linear, FilmstripCmd::II_EaseIn,
      FilmstripCmd::II_EaseOut, FilmstripCmd::II_EaseInOut,
      FilmstripCmd::II_Custom};

  FilmstripCmd::InbetweenInterpolation interpolation = codes[index];

  // inbetween
  try {
    FilmstripCmd::inbetween(level.getPointer(), range.first, range.second, interpolation,
                            strokePairs, options);
  } catch (...) {
    DVGui::warning(tr("The inbetweens could not be generated. The drawings were left unchanged."));
  }
}

//-----------------------------------------------------------------------------

void FilmstripFrames::onViewerAboutToBeDestroyed() {
  if (m_viewer) {
    disconnect(m_viewer, SIGNAL(onZoomChanged()), this, SLOT(update()));
    disconnect(m_viewer, SIGNAL(refreshNavi()), this, SLOT(update()));
    m_viewer = nullptr;
  }
}

//=============================================================================
// Filmstrip
//-----------------------------------------------------------------------------

Filmstrip::Filmstrip(QWidget *parent, Qt::WindowFlags flags) : QWidget(parent) {
  setSizePolicy(QSizePolicy::Minimum, QSizePolicy::Minimum);

  m_frameArea        = new QScrollArea(this);
  m_chooseLevelCombo = new QComboBox(this);
  UiTranslations::onLanguageChanged(m_chooseLevelCombo, [this] {
    const int noLevel = m_chooseLevelCombo->findData(-1);
    if (noLevel < 0) return;
    const QSignalBlocker block(m_chooseLevelCombo), modelBlock(m_chooseLevelCombo->model());
    m_chooseLevelCombo->setItemText(noLevel, tr("- No Current Level -"));
    m_chooseLevelCombo->update();
  });
  m_frames           = new FilmstripFrames(m_frameArea);

  //----
  m_frames->m_isVertical = m_isVertical;
  m_frameArea->setObjectName("filmScrollArea");
  m_frameArea->setFrameStyle(QFrame::StyledPanel);
  setOrientation(m_isVertical);

  m_frameArea->setWidget(m_frames);

  m_chooseLevelCombo->setMaxVisibleItems(50);
  m_chooseLevelCombo->setObjectName("filmLevelCombo");

  // layout
  QVBoxLayout *mainLayout = new QVBoxLayout();
  mainLayout->setContentsMargins(0, 0, 0, 0);
  mainLayout->setSpacing(0);
  {
    mainLayout->addWidget(m_chooseLevelCombo, 0);
    mainLayout->addWidget(m_frameArea, 1);
  }
  setLayout(mainLayout);

  setFocusProxy(m_frames);
  m_frames->setLevel(TApp::instance()->getCurrentLevel()->getSimpleLevel());

  onLevelSwitched(0);

  // signal-slot connections
  // switch the current level when the current index of m_chooseLevelCombo is
  // changed
  connect(m_chooseLevelCombo, SIGNAL(activated(int)), this,
          SLOT(onChooseLevelComboChanged(int)));
  connect(m_frames, SIGNAL(orientationToggledSignal(bool)), this,
          SLOT(orientationToggled(bool)));
  connect(m_frames, SIGNAL(comboBoxToggledSignal()), this,
          SLOT(comboBoxToggled()));
  connect(m_frames, SIGNAL(navigatorToggledSignal()), this,
          SLOT(navigatorToggled()));
  connect(m_frames, SIGNAL(responsiveThumbnailsToggledSignal(bool)), this,
          SLOT(responsiveThumbnailsToggled(bool)));
  connect(m_frames, SIGNAL(levelSelectedSignal(int)), this,
          SLOT(onChooseLevelComboChanged(int)));
  connect(m_frames, SIGNAL(levelActivatedSignal(TXshSimpleLevel *)), this,
          SLOT(onLevelActivated(TXshSimpleLevel *)));
  connect(m_frames, SIGNAL(levelStripsResyncRequestedSignal()), this,
          SLOT(resyncLevelStrips()));

  m_frames->setResponsiveThumbnails(m_responsiveThumbnails);
}

//-----------------------------------------------------------------------------
/*! switch the current level when the current index of m_chooseLevelCombo is
 * changed
 */
void Filmstrip::onChooseLevelComboChanged(int index) {
  m_syncWithCurrentLevel = false;
  m_frames->setSynchronized(false);

  TApp *tapp = TApp::instance();
  int noLevelIndex = m_chooseLevelCombo->findData(-1);

  if (index == noLevelIndex) {
    makeActive();
    m_frames->setLevel(0);
    tapp->getCurrentLevel()->setLevel(0);
    updateWindowTitle();
    return;
  }

  if (index < 0 || index >= (int)m_levels.size()) return;

  TXshSimpleLevel *level = m_levels[index];
  m_frames->setLevel(level);
  onLevelActivated(level);

  std::vector<TFrameId> fids;
  level->getFids(fids);

  QApplication::setOverrideCursor(Qt::WaitCursor);
  invalidateIcons(level, fids);
  QApplication::restoreOverrideCursor();

  updateWindowTitle();
}

//-----------------------------------------------------------------------------

void Filmstrip::onLevelActivated(TXshSimpleLevel *level) {
  if (!level) return;

  const bool wasActive = s_activeFilmstrip == this;
  makeActive();

  TApp *tapp              = TApp::instance();
  const bool levelChanged = tapp->getCurrentLevel()->getSimpleLevel() != level;
  if (wasActive && !levelChanged) return;

  std::vector<TFrameId> fids;
  level->getFids(fids);
  tapp->getCurrentFrame()->setFrameIds(fids);

  TFrameId workingFid;
  std::map<TXshSimpleLevel *, TFrameId>::iterator workingIt =
      m_workingFrames.find(level);
  if (workingIt != m_workingFrames.end() && level->isFid(workingIt->second))
    workingFid = workingIt->second;
  else if (!fids.empty())
    workingFid = fids.front();

  if (levelChanged) tapp->getCurrentLevel()->setLevel(level);

  if (tapp->getCurrentSelection()->getSelection())
    tapp->getCurrentSelection()->getSelection()->selectNone();

  if (workingFid != TFrameId()) tapp->getCurrentFrame()->setFid(workingFid);
}

//-----------------------------------------------------------------------------

void Filmstrip::resyncLevelStrips() {
  TApp *app                     = TApp::instance();
  TXshSimpleLevel *currentLevel = app->getCurrentLevel()->getSimpleLevel();

  TFrameId currentFid;
  TFrameHandle *frameHandle = app->getCurrentFrame();
  if (currentLevel && frameHandle->isEditingLevel() &&
      currentLevel->isFid(frameHandle->getFid()))
    currentFid = frameHandle->getFid();

  const QWidgetList widgets = QApplication::allWidgets();
  for (QWidget *widget : widgets) {
    Filmstrip *filmstrip = qobject_cast<Filmstrip *>(widget);
    if (!filmstrip) continue;

    filmstrip->m_syncWithCurrentLevel = true;
    filmstrip->m_frames->setSynchronized(true);
    filmstrip->m_frames->setLevel(currentLevel);

    if (currentLevel && currentFid != TFrameId()) {
      std::map<TXshSimpleLevel *, TFrameId>::iterator workingIt =
          filmstrip->m_workingFrames.find(currentLevel);
      if (workingIt != filmstrip->m_workingFrames.end())
        workingIt->second = currentFid;
    }

    filmstrip->updateWindowTitle();
    filmstrip->update();
  }
}

//-----------------------------------------------------------------------------
/*! update combo items when the contents of scene cast are changed
 */
void Filmstrip::updateChooseLevelComboItems() {
  TXshSimpleLevel *stripLevel = m_frames->getLevel();

  // clear items
  m_chooseLevelCombo->clear();
  for (auto oldLevel : m_levels) oldLevel->release();
  m_levels.clear();

  std::map<TXshSimpleLevel *, TFrameId> new_workingFrames;

  // correct and register items
  ToonzScene *scene = TApp::instance()->getCurrentScene()->getScene();
  if (scene) {
    std::vector<TXshLevel *> levels;
    scene->getLevelSet()->listLevels(levels);
    std::set<TXshLevel *> usedLevels;
    scene->getTopXsheet()->getUsedLevels(usedLevels);
    std::vector<TXshLevel *>::iterator it;

    for (it = levels.begin(); it != levels.end(); ++it) {
      // register only TLV and PLI
      TXshSimpleLevel *sl = (*it)->getSimpleLevel();
      if (sl) {
        // register only used level in xsheet
        if (usedLevels.count(sl) == 0) continue;

        sl->addRef();
        m_levels.push_back(sl);

        // create new m_workingFrames map with the new levelset
        TFrameId fId;
        std::map<TXshSimpleLevel *, TFrameId>::iterator WFit =
            m_workingFrames.find(sl);

        if (WFit != m_workingFrames.end())
          fId = WFit->second;
        else
          fId = sl->getFirstFid();

        new_workingFrames.insert(std::make_pair(sl, fId));

        QString levelName = QString::fromStdWString(sl->getName());
        if (sl->getProperties()->getDirtyFlag()) levelName += " *";

        // append the current working frame number to the item name
        if (fId != sl->getFirstFid() && fId.getNumber() >= 0)
          levelName +=
              QString("  [#") + QString::number(fId.getNumber()) + QString("]");

        m_chooseLevelCombo->addItem(levelName);
      }
    }
  }

  if (m_chooseLevelCombo->count() == 0)
    m_chooseLevelCombo->addItem(tr("- No Current Level -"), -1);

  // swap the list
  m_workingFrames.clear();
  m_workingFrames = new_workingFrames;

  if (std::find(m_levels.begin(), m_levels.end(), stripLevel) ==
      m_levels.end()) {
    TXshSimpleLevel *currentLevel =
        TApp::instance()->getCurrentLevel()->getSimpleLevel();
    if (std::find(m_levels.begin(), m_levels.end(), currentLevel) !=
        m_levels.end())
      stripLevel = currentLevel;
    else
      stripLevel = 0;
  }
  m_frames->setLevel(stripLevel);

  // synchronize the combo to this strip's assigned level
  updateCurrentLevelComboItem();
}

//-----------------------------------------------------------------------------
/*! synchronize the combo to the level assigned to this strip
 */
void Filmstrip::updateCurrentLevelComboItem() {
  if (m_chooseLevelCombo->count() == 1 &&
      m_chooseLevelCombo->itemData(0) == QVariant(-1)) {
    m_chooseLevelCombo->setCurrentIndex(0);
    return;
  }

  TXshSimpleLevel *currentLevel = m_frames->getLevel();
  if (!currentLevel) {
    int noLevelIndex = m_chooseLevelCombo->findData(-1);
    if (noLevelIndex == -1) {
      noLevelIndex = m_chooseLevelCombo->count();
      m_chooseLevelCombo->addItem(tr("- No Current Level -"), -1);
    }
    m_chooseLevelCombo->setCurrentIndex(noLevelIndex);
    return;
  }

  for (int i = 0; i < m_levels.size(); i++) {
    if (currentLevel == m_levels[i]) {
      m_chooseLevelCombo->setCurrentIndex(i);
      int noLevelIndex =
          m_chooseLevelCombo->findData(-1);
      if (noLevelIndex != -1) m_chooseLevelCombo->removeItem(noLevelIndex);
      return;
    }
  }

  int noLevelIndex = m_chooseLevelCombo->findData(-1);
  if (noLevelIndex == -1) {
    noLevelIndex = m_chooseLevelCombo->count();
    m_chooseLevelCombo->addItem(tr("- No Current Level -"), -1);
  }
  m_chooseLevelCombo->setCurrentIndex(noLevelIndex);
}

//-----------------------------------------------------------------------------

Filmstrip::~Filmstrip() {
  if (s_activeFilmstrip == this) {
    m_frames->setActive(false);
    s_activeFilmstrip = nullptr;
  }

  for (auto level : m_levels) level->release();
  m_levels.clear();
}

//-----------------------------------------------------------------------------

void Filmstrip::makeActive() {
  if (s_activeFilmstrip == this) {
    m_frames->setActive(true);
    return;
  }

  if (s_activeFilmstrip) s_activeFilmstrip->m_frames->setActive(false);

  s_activeFilmstrip = this;
  m_frames->setActive(true);
}

//-----------------------------------------------------------------------------

void Filmstrip::showEvent(QShowEvent *) {
  TApp *app = TApp::instance();
  if (!s_activeFilmstrip) makeActive();
  if (!m_frames->getLevel())
    m_frames->setLevel(app->getCurrentLevel()->getSimpleLevel());

  TXshLevelHandle *levelHandle = app->getCurrentLevel();
  bool ret = connect(levelHandle, SIGNAL(xshLevelSwitched(TXshLevel *)),
                     SLOT(onLevelSwitched(TXshLevel *)));
  ret      = ret &&
        connect(levelHandle, SIGNAL(xshLevelChanged()), SLOT(onLevelChanged()));

  // updateWindowTitle is called in the onLevelChanged
  ret = ret && connect(app->getPaletteController()->getCurrentLevelPalette(),
                       SIGNAL(colorStyleChangedOnMouseRelease()),
                       SLOT(onLevelChanged()));
  ret = ret && connect(levelHandle, SIGNAL(xshLevelTitleChanged()),
                       SLOT(onLevelChanged()));

  ret =
      ret && connect(m_frameArea->verticalScrollBar(),
                     SIGNAL(valueChanged(int)), this, SLOT(onSliderMoved(int)));

  TSceneHandle *sceneHandle = TApp::instance()->getCurrentScene();
  ret = ret && connect(sceneHandle, SIGNAL(sceneSwitched()), this,
                       SLOT(updateChooseLevelComboItems()));
  ret = ret && connect(sceneHandle, SIGNAL(castChanged()), this,
                       SLOT(updateChooseLevelComboItems()));

  ret = ret &&
        connect(TApp::instance()->getCurrentXsheet(), SIGNAL(xsheetChanged()),
                this, SLOT(updateChooseLevelComboItems()));

  ret = ret && connect(app->getCurrentFrame(), SIGNAL(frameSwitched()), this,
                       SLOT(onFrameSwitched()));

  assert(ret);

  updateChooseLevelComboItems();
  onFrameSwitched();
  onLevelSwitched(0);
}

//-----------------------------------------------------------------------------

void Filmstrip::hideEvent(QHideEvent *) {
  if (s_activeFilmstrip == this) {
    m_frames->setActive(false);
    s_activeFilmstrip = nullptr;
  }

  TApp *app                    = TApp::instance();
  TXshLevelHandle *levelHandle = app->getCurrentLevel();
  disconnect(levelHandle, SIGNAL(xshLevelSwitched(TXshLevel *)), this,
             SLOT(onLevelSwitched(TXshLevel *)));
  disconnect(levelHandle, SIGNAL(xshLevelChanged()), this,
             SLOT(onLevelChanged()));
  disconnect(TApp::instance()->getPaletteController()->getCurrentLevelPalette(),
             SIGNAL(colorStyleChangedOnMouseRelease()), this,
             SLOT(onLevelChanged()));

  disconnect(levelHandle, SIGNAL(xshLevelTitleChanged()), this,
             SLOT(onLevelChanged()));

  disconnect(m_frameArea->verticalScrollBar(), SIGNAL(valueChanged(int)), this,
             SLOT(onSliderMoved(int)));

  TSceneHandle *sceneHandle = TApp::instance()->getCurrentScene();
  disconnect(sceneHandle, SIGNAL(sceneSwitched()), this,
             SLOT(updateChooseLevelComboItems()));
  disconnect(sceneHandle, SIGNAL(castChanged()), this,
             SLOT(updateChooseLevelComboItems()));

  disconnect(TApp::instance()->getCurrentXsheet(), SIGNAL(xsheetChanged()),
             this, SLOT(updateChooseLevelComboItems()));

  disconnect(TApp::instance()->getCurrentFrame(), SIGNAL(frameSwitched()), this,
             SLOT(onFrameSwitched()));
}

//-----------------------------------------------------------------------------

void Filmstrip::resizeEvent(QResizeEvent *e) {
  if (m_isVertical) {
    m_frames->updateContentHeight();
    m_frameArea->verticalScrollBar()->setSingleStep(
        m_frames->getOneFrameHeight());
  } else {
    m_frames->updateContentWidth();
    m_frameArea->horizontalScrollBar()->setSingleStep(
        m_frames->getOneFrameWidth());
  }
}

//-----------------------------------------------------------------------------

void Filmstrip::onLevelChanged() { updateWindowTitle(); }

//-----------------------------------------------------------------------------

void Filmstrip::updateWindowTitle() {
  updateCurrentLevelComboItem();

  TXshSimpleLevel *level = m_frames->getLevel();

  QString levelName;

  if (!level) {
    UiTranslations::setCaption(parentWidget(), "windowTitle", tr("Level Strip"));
    return;
  }

  levelName = QString::fromStdWString(level->getName());
  if (level->getProperties()->getDirtyFlag()) levelName += " *";

  // parentWidget() is TPanel
  UiTranslations::setCaption(parentWidget(), "windowTitle",
      UiTranslations::text(tr("Level:  ")).appended(levelName));

  TFrameId workingFid;
  TFrameHandle *fh = TApp::instance()->getCurrentFrame();
  if (s_activeFilmstrip == this &&
      level == TApp::instance()->getCurrentLevel()->getSimpleLevel() &&
      fh->isEditingLevel() && level->isFid(fh->getFid())) {
    workingFid = fh->getFid();
  } else {
    std::map<TXshSimpleLevel *, TFrameId>::iterator workingIt =
        m_workingFrames.find(level);
    if (workingIt != m_workingFrames.end()) workingFid = workingIt->second;
  }

  if (workingFid.getNumber() >= 0)
    levelName += QString("  [#") + QString::number(workingFid.getNumber()) +
                 QString("]");

  if (m_chooseLevelCombo->currentIndex() >= 0)
    m_chooseLevelCombo->setItemText(m_chooseLevelCombo->currentIndex(),
                                    levelName);
}

//-----------------------------------------------------------------------------

void Filmstrip::onLevelSwitched(TXshLevel *oldLevel) {
  (void)oldLevel;

  if (m_syncWithCurrentLevel || s_activeFilmstrip == this) {
    TXshSimpleLevel *activeLevel =
        TApp::instance()->getCurrentLevel()->getSimpleLevel();
    m_frames->setLevel(activeLevel);
  }

  updateWindowTitle();

  int tc = ToonzCheck::instance()->getChecks();
  if (s_activeFilmstrip == this &&
      (tc & (ToonzCheck::eInk | ToonzCheck::ePaint))) {
    TXshLevel *sl = m_frames->getLevel();
    if (!sl) return;
    std::vector<TFrameId> fids;
    sl->getFids(fids);
    removeIcons(sl, fids, true);
  }
  update();
}

//-----------------------------------------------------------------------------

void Filmstrip::onSliderMoved(int val) {
  int oneFrameHeight = m_frames->getOneFrameHeight();
  int oneFrameWidth  = m_frames->getOneFrameWidth();
  if (m_isVertical) {
    int tmpVal =
        (int)((float)val / (float)oneFrameHeight + 0.5f) * oneFrameHeight;
    m_frameArea->verticalScrollBar()->setValue(tmpVal);
  } else {
    int tmpVal =
        (int)((float)val / (float)oneFrameWidth + 0.5f) * oneFrameWidth;
    m_frameArea->horizontalScrollBar()->setValue(tmpVal);
  }
}

//-----------------------------------------------------------------------------

void Filmstrip::onFrameSwitched() {
  if (!m_syncWithCurrentLevel && s_activeFilmstrip != this) return;

  TFrameHandle *fh = TApp::instance()->getCurrentFrame();
  if (!fh->isEditingLevel()) return;

  TXshSimpleLevel *level = m_frames->getLevel();
  if (!level || level != TApp::instance()->getCurrentLevel()->getSimpleLevel())
    return;

  TFrameId fid = fh->getFid();
  if (!level->isFid(fid)) return;

  std::map<TXshSimpleLevel *, TFrameId>::iterator WFit;
  WFit = m_workingFrames.find(level);
  if (WFit == m_workingFrames.end()) return;

  WFit->second = fid;

  QString levelName = QString::fromStdWString(level->getName());
  if (level->getProperties()->getDirtyFlag()) levelName += " *";
  if (fid.getNumber() >= 0)
    levelName +=
        QString("  [#") + QString::number(fid.getNumber()) + QString("]");

  m_chooseLevelCombo->setItemText(m_chooseLevelCombo->currentIndex(),
                                  levelName);
}

//-----------------------------------------------------------------------------

void Filmstrip::orientationToggled(bool isVertical) {
  setOrientation(isVertical);
}

//-----------------------------------------------------------------------------

void Filmstrip::comboBoxToggled() {
  if (m_chooseLevelCombo->isHidden())
    m_chooseLevelCombo->show();
  else
    m_chooseLevelCombo->hide();
  m_showComboBox = !m_chooseLevelCombo->isHidden();
  m_frames->setComboBox(m_showComboBox);
}

//-----------------------------------------------------------------------------

void Filmstrip::navigatorToggled() {
  m_showNavigator = !m_showNavigator;
  m_frames->setNavigator(m_showNavigator);
}

//-----------------------------------------------------------------------------

void Filmstrip::responsiveThumbnailsToggled(bool responsive) {
  m_responsiveThumbnails = responsive;
  m_frames->setResponsiveThumbnails(responsive);
}

//-----------------------------------------------------------------------------

void Filmstrip::setOrientation(bool isVertical) {
  m_isVertical = isVertical;
  if (isVertical) {
    m_frameArea->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_frameArea->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOn);
    m_frameArea->verticalScrollBar()->setObjectName("LevelStripScrollBar");
  } else {
    m_frameArea->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOn);
    m_frameArea->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_frameArea->horizontalScrollBar()->setObjectName("LevelStripScrollBar");
  }
  m_frames->setOrientation(m_isVertical);
  dynamic_cast<TPanel *>(parentWidget())
      ->setFixWidthMode(m_isVertical ? TPanel::sizeable : TPanel::variable);
}

// SaveLoadQSettings
void Filmstrip::save(QSettings &settings) const {
  UINT orientation = 0;
  orientation      = m_isVertical ? 1 : 0;
  settings.setValue("vertical", orientation);

  UINT showCombo = 0;
  showCombo      = m_chooseLevelCombo->isHidden() ? 0 : 1;
  settings.setValue("showCombo", showCombo);

  UINT navigator = 0;
  navigator      = m_showNavigator ? 1 : 0;
  settings.setValue("navigator", navigator);

  UINT responsive = m_responsiveThumbnails ? 1 : 0;
  settings.setValue("responsiveThumbnails", responsive);
}

void Filmstrip::load(QSettings &settings) {
  UINT orientation = settings.value("vertical", 1).toUInt();
  m_isVertical     = orientation == 1;
  setOrientation(m_isVertical);

  UINT navigator  = settings.value("navigator", 1).toUInt();
  m_showNavigator = navigator == 1;
  m_frames->setNavigator(m_showNavigator);

  UINT showCombo = settings.value("showCombo", 1).toUInt();
  m_showComboBox = showCombo == 1;
  if (showCombo == 1) {
    m_chooseLevelCombo->show();
  } else {
    m_chooseLevelCombo->hide();
  }
  m_frames->setComboBox(m_showComboBox);

  UINT responsive        = settings.value("responsiveThumbnails", 1).toUInt();
  m_responsiveThumbnails = responsive == 1;
  m_frames->setResponsiveThumbnails(m_responsiveThumbnails);
}

//=============================================================================
// inbetweenDialog
//-----------------------------------------------------------------------------

class MorphPreviewSwatch final : public PlaneViewer {
  TVectorImageP m_image;
  TRectD m_bounds;
  int m_highlight = -1;
public:
  explicit MorphPreviewSwatch(QWidget *parent) : PlaneViewer(parent) {
    setMinimumSize(190, 160);
    setBgColor(TPixel32(230,230,230), TPixel32(255,255,255));
  }
  void setImage(const TVectorImageP &image, const TRectD &bounds) {
    m_image = image;
    m_bounds = bounds;
    update();
  }
  void highlight(int index) { m_highlight = index; update(); }
  void paintGL() override {
    drawBackground();
    if (!m_image) return;
    const double sx = (width() - 20.0) / std::max(1.0, m_bounds.getLx());
    const double sy = (height() - 20.0) / std::max(1.0, m_bounds.getLy());
    const TPointD center((m_bounds.x0 + m_bounds.x1) * .5, (m_bounds.y0 + m_bounds.y1) * .5);
    viewAff() = TTranslation(width() * .5, height() * .5) *
                TScale(std::min(sx, sy)) * TTranslation(-center);
    glEnable(GL_BLEND);
    glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
    pushGLWorldCoordinates();
    draw(m_image);
    if (m_highlight >= 0 && m_highlight < int(m_image->getStrokeCount())) {
      const auto box = m_image->getStroke(m_highlight)->getBBox();
      glColor3d(1.0, .25, 0.0);
      glLineWidth(2.0);
      glBegin(GL_LINE_LOOP);
      glVertex2d(box.x0, box.y0); glVertex2d(box.x1, box.y0);
      glVertex2d(box.x1, box.y1); glVertex2d(box.x0, box.y1);
      glEnd();
      glLineWidth(1.0);
    }
    popGLCoordinates();
    glDisable(GL_BLEND);
  }
};

InbetweenDialog::InbetweenDialog(QWidget *parent)
    : Dialog(TApp::instance()->getMainWindow(), true, "InBetween"), m_previewTimer(nullptr) {
  setWindowTitle(tr("Inbetween"));

  QString linear(tr("Linear"));
  QString easeIn(tr("Ease In"));
  QString easeOut(tr("Ease Out"));
  QString easeInOut(tr("Ease In / Ease Out"));
  QStringList items;
  QString custom(tr("Custom"));
  items << linear << easeIn << easeOut << easeInOut << custom;

  beginHLayout();
  m_comboBox = new QComboBox(this);
  const QStringList keys = {"Linear", "Ease In", "Ease Out", "Ease In / Ease Out", "Custom"};
  for (int index = 0; index < items.size(); ++index)
    m_comboBox->addItem(items[index], keys[index]);
  UiTranslations::watchCombo(m_comboBox);
  addWidget(tr("Interpolation:"), m_comboBox);
  endHLayout();

  // strengths of the Custom interpolation: 0 is linear, 100 is the gentlest
  m_customEaseWidget = new QWidget(this);
  {
    QGridLayout *layout = new QGridLayout(m_customEaseWidget);
    layout->setContentsMargins(0, 0, 0, 0);
    auto addSlider = [&](int row, const QString &name, const QString &toolTip,
                         QSlider *&slider, QLabel *&valueLabel) {
      slider = new QSlider(Qt::Horizontal, m_customEaseWidget);
      slider->setRange(0, 100);
      slider->setToolTip(toolTip);
      valueLabel = new QLabel("0%", m_customEaseWidget);
      valueLabel->setMinimumWidth(40);
      QLabel *valueLabelPtr = valueLabel;
      connect(slider, &QSlider::valueChanged, valueLabelPtr,
              [valueLabelPtr](int value) {
                valueLabelPtr->setText(QString("%1%").arg(value));
              });
      layout->addWidget(new QLabel(name, m_customEaseWidget), row, 0);
      layout->addWidget(slider, row, 1);
      layout->addWidget(valueLabel, row, 2);
    };
    addSlider(0, tr("Slow Start:"),
              tr("How gently the motion starts: 0% is linear, 100% is the "
                 "gentlest start."),
              m_slowStartSlider, m_slowStartLabel);
    addSlider(1, tr("Slow End:"),
              tr("How gently the motion ends: 0% is linear, 100% is the "
                 "gentlest end."),
              m_slowEndSlider, m_slowEndLabel);
  }
  m_customEaseWidget->hide();
  addWidget(m_customEaseWidget);
  connect(m_comboBox, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
          [this](int) { updateCustomEaseVisibility(); });

  m_autoMatchCB = new QCheckBox(tr("Match Strokes Automatically"), this);
  m_autoMatchCB->setChecked(false);
  m_autoMatchCB->setToolTip(
      tr("Match the strokes of the two key drawings by position, size, "
         "length and style instead of by their order. Strokes paired "
         "manually keep their pairing."));
  addWidget(m_autoMatchCB);

  m_strokePairsCB = new QCheckBox(this);
  m_strokePairsCB->setChecked(true);
  m_strokePairsCB->setToolTip(
      tr("Use the stroke pairs set with Pair Strokes for Inbetween to match "
         "the strokes of the two key drawings."));
  m_strokePairsCB->hide();
  addWidget(m_strokePairsCB);

  m_zoneHintsCB = new QCheckBox(this);
  m_zoneHintsCB->setChecked(true);
  m_zoneHintsCB->setToolTip(
      tr("Use the zones set with Add Inbetween Zone Hint from Selection: "
         "strokes in a zone of the first key drawing are matched with the "
         "strokes in the corresponding zone of the other key drawing."));
  m_zoneHintsCB->hide();
  addWidget(m_zoneHintsCB);

  m_unmatchedCombo = new QComboBox(this);
  m_unmatchedCombo->setObjectName("morphUnmatchedPolicy");
  m_unmatchedCombo->addItem(tr("Omit unmatched strokes (legacy)"), TInbetween::OmitUnmatched);
  m_unmatchedCombo->addItem(tr("Hold unmatched strokes"), TInbetween::HoldUnmatched);
  m_unmatchedCombo->addItem(tr("Shrink disappearing / grow appearing strokes"), TInbetween::CollapseUnmatched);
  m_unmatchedCombo->addItem(tr("Fade disappearing / appearing stroke opacity"), TInbetween::FadeUnmatched);
  m_unmatchedCombo->setToolTip(tr("Fade multiplies each unmatched stroke's existing opacity by its visibility. Unmatched strokes bounding painted regions cannot fade; match them or choose another policy."));
  addWidget(tr("Unmatched strokes:"), m_unmatchedCombo);

  QWidget *previews = new QWidget(this);
  auto previewLayout = new QGridLayout(previews);
  previewLayout->addWidget(new QLabel(tr("First drawing"), previews), 0, 0);
  previewLayout->addWidget(new QLabel(tr("Generated drawing"), previews), 0, 1);
  previewLayout->addWidget(new QLabel(tr("Last drawing"), previews), 0, 2);
  m_firstPreview = new MorphPreviewSwatch(previews);
  m_resultPreview = new MorphPreviewSwatch(previews);
  m_lastPreview = new MorphPreviewSwatch(previews);
  m_firstPreview->setObjectName("morphFirstPreview");
  m_resultPreview->setObjectName("morphResultPreview");
  m_lastPreview->setObjectName("morphLastPreview");
  previewLayout->addWidget(m_firstPreview, 1, 0);
  previewLayout->addWidget(m_resultPreview, 1, 1);
  previewLayout->addWidget(m_lastPreview, 1, 2);
  addWidget(previews);
  m_previewPosition = new QSlider(Qt::Horizontal, this);
  m_previewPosition->setObjectName("morphPreviewPosition");
  m_previewPosition->setRange(0, 100);
  m_previewPosition->setValue(50);
  m_previewPosition->setToolTip(tr("Preview position between the two key drawings. Preview does not edit the level."));
  addWidget(tr("Preview position:"), m_previewPosition);
  m_reviewTable = new QTableWidget(this);
  m_reviewTable->setObjectName("morphCorrespondence");
  m_reviewTable->setColumnCount(3);
  m_reviewTable->setHorizontalHeaderLabels({tr("First stroke"), tr("Last stroke"), tr("Correspondence")});
  m_reviewTable->horizontalHeader()->setStretchLastSection(true);
  m_reviewTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
  m_reviewTable->setSelectionBehavior(QAbstractItemView::SelectRows);
  m_reviewTable->setSelectionMode(QAbstractItemView::SingleSelection);
  m_reviewTable->setMaximumHeight(160);
  addWidget(m_reviewTable);
  m_reviewStatus = new QLabel(this);
  m_reviewStatus->setObjectName("morphReviewStatus");
  m_reviewStatus->setWordWrap(true);
  addWidget(m_reviewStatus);
  m_previewTimer = new QTimer(this);
  m_previewTimer->setSingleShot(true);
  m_previewTimer->setInterval(120);
  connect(m_previewTimer, &QTimer::timeout, this, &InbetweenDialog::refreshPreview);
  connect(this, &QDialog::finished, this, [this](int) {
    m_previewTimer->stop();
    m_firstImage = m_lastImage = TVectorImageP();
    m_manualPairs.clear(); m_zones.clear();
    for (auto *preview : {m_firstPreview, m_resultPreview, m_lastPreview})
      preview->setImage(TVectorImageP(), TRectD());
  });
  connect(TApp::instance()->getCurrentScene(), &TSceneHandle::sceneSwitched,
          this, &QDialog::reject);
  connect(TApp::instance()->getCurrentLevel(), &TXshLevelHandle::xshLevelSwitched,
          this, &QDialog::reject);
  for (auto cb : {m_autoMatchCB, m_strokePairsCB, m_zoneHintsCB})
    connect(cb, &QCheckBox::toggled, this, [this](bool) { schedulePreview(); });
  for (auto slider : {m_previewPosition, m_slowStartSlider, m_slowEndSlider})
    connect(slider, &QSlider::valueChanged, this, [this](int) { schedulePreview(); });
  for (auto combo : {m_comboBox, m_unmatchedCombo})
    connect(combo, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
            [this](int) { schedulePreview(); });
  connect(m_reviewTable, &QTableWidget::currentCellChanged, this,
          [this](int row, int, int, int) {
            auto index = [&](int column) {
              auto item = m_reviewTable->item(row, column);
              return item ? item->data(Qt::UserRole).toInt() : -1;
            };
            m_firstPreview->highlight(index(0));
            m_lastPreview->highlight(index(1));
          });

  m_applyButton = new QPushButton(tr("Inbetween"), this);
  m_applyButton->setObjectName("morphApply");
  m_applyButton->setEnabled(false);
  QPushButton *cancelBtn = new QPushButton(tr("Cancel"), this);
  cancelBtn->setObjectName("morphCancel");
  connect(m_applyButton, &QPushButton::clicked, this, &InbetweenDialog::accept);
  connect(cancelBtn, SIGNAL(clicked()), this, SLOT(reject()));

  addButtonBarWidget(m_applyButton, cancelBtn);
}

void InbetweenDialog::setPreviewSource(
    const TVectorImageP &first, const TVectorImageP &last,
    const TInbetween::StrokeCorrespondence &pairs,
    const std::vector<std::pair<TRectD, TRectD>> &zones) {
  m_firstImage = first ? first->clone() : TVectorImageP();
  m_lastImage = last ? last->clone() : TVectorImageP();
  m_manualPairs = pairs;
  m_zones = zones;
  schedulePreview();
}
int InbetweenDialog::unmatchedPolicy() const { return m_unmatchedCombo->currentData().toInt(); }
void InbetweenDialog::setUnmatchedPolicy(int policy) {
  const int index = m_unmatchedCombo->findData(policy);
  m_unmatchedCombo->setCurrentIndex(index >= 0 ? index : 0);
}
void InbetweenDialog::schedulePreview() {
  m_previewValid = false;
  if (m_applyButton) m_applyButton->setEnabled(false);
  if (m_reviewTable) m_reviewTable->setProperty("previewReady", false);
  if (m_previewTimer) m_previewTimer->start();
}
void InbetweenDialog::refreshPreview() {
  m_previewValid = false;
  m_applyButton->setEnabled(false);
  m_reviewTable->setProperty("previewReady", false);
  if (!m_firstImage || !m_lastImage) return;
  TInbetween::MatchOptions options;
  options.m_autoMatch = autoMatch();
  options.m_unmatchedPolicy = static_cast<TInbetween::UnmatchedPolicy>(unmatchedPolicy());
  if (useZoneHints())
    for (const auto &zone : m_zones) options.m_zoneHints.emplace_back(zone.first, zone.second);
  const auto pairs = useStrokePairs() ? m_manualPairs : TInbetween::StrokeCorrespondence();
  try {
    const auto review = TInbetween::reviewCorrespondence(m_firstImage, m_lastImage, pairs, options);
    const QString names[] = {tr("Manual"), tr("Zone hint"), tr("Automatic"), tr("Index fallback")};
    m_reviewTable->setRowCount(0);
    auto addRow = [&](int first, int last, const QString &description, int source) {
      const int row = m_reviewTable->rowCount();
      m_reviewTable->insertRow(row);
      for (int column = 0; column < 2; ++column) {
        const int index = column ? last : first;
        auto item = new QTableWidgetItem(index < 0 ? tr("—") : QString::number(index + 1));
        item->setData(Qt::UserRole, index);
        m_reviewTable->setItem(row, column, item);
      }
      auto sourceItem = new QTableWidgetItem(description);
      sourceItem->setData(Qt::UserRole, source);
      m_reviewTable->setItem(row, 2, sourceItem);
    };
    for (const auto &p : review.pairs) addRow(p.first, p.last, names[p.source], p.source);
    for (int i : review.unmatchedFirst) addRow(i, -1, tr("Disappearing"), -1);
    for (int i : review.unmatchedLast) addRow(-1, i, tr("Appearing"), -2);
    const auto bounds = m_firstImage->getBBox() + m_lastImage->getBBox();
    m_firstPreview->setImage(m_firstImage, bounds);
    m_lastPreview->setImage(m_lastImage, bounds);
    if (options.m_unmatchedPolicy == TInbetween::FadeUnmatched &&
        TInbetween::fadeHasPaintedBoundaries(m_firstImage, m_lastImage, review)) {
      m_reviewStatus->setText(tr("Fade cannot animate unmatched strokes that bound painted regions, including holes and nested fills. Match those boundaries or choose Hold, Shrink/grow, or Omit. No drawing was changed."));
      m_resultPreview->setImage(TVectorImageP(), bounds);
      return;
    }
    double t = m_previewPosition->value() / 100.0;
    const auto algorithm = static_cast<TInbetween::TweenAlgorithm>(m_comboBox->currentIndex());
    double time = algorithm == TInbetween::CustomEaseInterpolation
        ? TInbetween::customEaseInterpolation(t, slowStart() / 100.0, slowEnd() / 100.0)
        : TInbetween::interpolation(t, algorithm);
    TInbetween tween(m_firstImage, m_lastImage, pairs, options);
    // Key drawings are never overwritten by the command, including in legacy
    // mode, so display the untouched endpoints at 0% and 100%.
    TVectorImageP result = t <= 0 ? m_firstImage : t >= 1 ? m_lastImage : tween.tween(time);
    m_resultPreview->setImage(result, bounds);
    m_reviewStatus->setText(tr("%1 pairs; %2 disappearing and %3 appearing strokes. Select a row to highlight its source strokes. Preview: %4%.")
        .arg(int(review.pairs.size())).arg(int(review.unmatchedFirst.size())).arg(int(review.unmatchedLast.size())).arg(m_previewPosition->value()));
    m_reviewTable->setProperty("previewReady", true);
    m_previewValid = true;
    m_applyButton->setEnabled(true);
  } catch (...) {
    m_reviewStatus->setText(tr("The preview could not be generated. No drawing was changed."));
    m_resultPreview->setImage(TVectorImageP(), m_firstImage->getBBox() + m_lastImage->getBBox());
  }
}

void InbetweenDialog::accept() {
  m_previewTimer->stop();
  refreshPreview();
  if (m_previewValid) DVGui::Dialog::accept();
}

//-----------------------------------------------------------------------------

QString InbetweenDialog::getValue() { return m_comboBox->currentData().toString(); }

//-----------------------------------------------------------------------------

void InbetweenDialog::setValue(const QString &value) {
  int currentIndex = m_comboBox->findData(value);
  if (currentIndex < 0) currentIndex = m_comboBox->findText(value);
  if (currentIndex < 0) currentIndex = 0;
  m_comboBox->setCurrentIndex(currentIndex);
}

//-----------------------------------------------------------------------------

int InbetweenDialog::getIndex(const QString &text) {
  const int index = m_comboBox->findData(text);
  return index >= 0 ? index : m_comboBox->findText(text);
}

//-----------------------------------------------------------------------------

void InbetweenDialog::setStrokePairCount(int count) {
  m_strokePairsCB->setText(UiTranslations::text(tr("Use Stroke Pairs (%1)")).arg(count));
  m_strokePairsCB->setVisible(count > 0);
}

//-----------------------------------------------------------------------------

bool InbetweenDialog::useStrokePairs() const {
  return !m_strokePairsCB->isHidden() && m_strokePairsCB->isChecked();
}

//-----------------------------------------------------------------------------

void InbetweenDialog::setZoneHintCount(int count) {
  m_zoneHintsCB->setText(UiTranslations::text(tr("Use Zone Hints (%1)")).arg(count));
  m_zoneHintsCB->setVisible(count > 0);
}

//-----------------------------------------------------------------------------

bool InbetweenDialog::useZoneHints() const {
  return !m_zoneHintsCB->isHidden() && m_zoneHintsCB->isChecked();
}

//-----------------------------------------------------------------------------

bool InbetweenDialog::autoMatch() const { return m_autoMatchCB->isChecked(); }

//-----------------------------------------------------------------------------

void InbetweenDialog::setAutoMatch(bool on) { m_autoMatchCB->setChecked(on); }

//-----------------------------------------------------------------------------

int InbetweenDialog::slowStart() const { return m_slowStartSlider->value(); }

//-----------------------------------------------------------------------------

int InbetweenDialog::slowEnd() const { return m_slowEndSlider->value(); }

//-----------------------------------------------------------------------------

void InbetweenDialog::setEase(int slowStart, int slowEnd) {
  m_slowStartSlider->setValue(slowStart);
  m_slowEndSlider->setValue(slowEnd);
}

//-----------------------------------------------------------------------------

// Custom is the last item of the interpolation list
void InbetweenDialog::updateCustomEaseVisibility() {
  m_customEaseWidget->setVisible(m_comboBox->currentIndex() ==
                                 m_comboBox->count() - 1);
}

//=============================================================================

OpenFloatingPanel openFilmstripCommand(MI_OpenFilmStrip, "FilmStrip",
                                       QObject::tr("Level: "));
