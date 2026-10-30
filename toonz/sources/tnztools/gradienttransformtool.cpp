//-----------------------------------------------------------------------------
//  gradienttransformtool.cpp
//
//  Gradient Transform tool: click a vector region filled with a Linear or
//  Radial Gradient style and drag handles in the viewer to move, rotate and
//  resize the gradient. The handles edit the parameters of the palette style
//  (the same values as the Style Editor), so every region using that style
//  follows. The maths is in tools/gradienthandles.h.
//  "Make Unique Style" (options bar button, right-click menu and the command
//  MI_MakeUniqueStyle) gives the picked region its own copy of the style, so
//  that the handles change that region alone; its naming and "is it shared"
//  decisions are in tools/uniquestyle.h.
//-----------------------------------------------------------------------------

// TnzTools includes
#include "toonzqt/uitranslations.h"
#include "tools/tool.h"
#include "tools/cursors.h"
#include "tools/toolutils.h"
#include "tools/tooloptions.h"
#include "tools/toolcommandids.h"
#include "tools/toolhandle.h"
#include "tools/gradienthandles.h"
#include "tools/uniquestyle.h"

// TnzQt includes
#include "toonzqt/menubarcommand.h"
#include "../toonz/menubarcommandids.h"

// TnzLib includes
#include "toonz/palettecontroller.h"
#include "toonz/palettecmd.h"
#include "toonz/tpalettehandle.h"
#include "toonz/txsheethandle.h"
#include "toonz/txshlevelhandle.h"
#include "toonz/txshsimplelevel.h"

// TnzCore includes
#include "tvectorimage.h"
#include "tregion.h"
#include "tpalette.h"
#include "tcolorstyles.h"
#include "tundo.h"
#include "tgl.h"
#include "tmsgcore.h"
#include "historytypes.h"

// Qt includes
#include <QAction>
#include <QCoreApplication>
#include <QHBoxLayout>
#include <QLabel>
#include <QMenu>
#include <QMutexLocker>
#include <QObject>
#include <QPushButton>
#include <QString>

// STD includes
#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

namespace GH = GradientHandles;

namespace {

const double HandleRadiusPx  = 4.0;   // half size of a handle on screen
const double PickRadiusPx    = 8.0;   // how near the pointer must be
const double MinHandleGapPx  = 12.0;  // end handles never hide the centre
const double SnapStepDegrees = 15.0;  // Shift while turning a linear gradient

const TPixel32 LineColor(79, 128, 255);
const TPixel32 HotColor(255, 200, 0);

//! The double parameters of a gradient style, in the style's order.
std::vector<double> readParams(const TColorStyle *style) {
  std::vector<double> values;
  int count = style->getParamCount();
  for (int i = 0; i < count; ++i)
    values.push_back(style->getParamValue(TColorStyle::double_tag(), i));
  return values;
}

//! Replaces style \p styleId of \p palette with a copy holding \p values,
//! the way the Style Editor applies a change (the viewers redraw the regions
//! when the style object changes). Returns false if the style is no longer a
//! gradient.
bool setStyleParams(TPalette *palette, int styleId,
                    const std::vector<double> &values, bool editedFlag) {
  if (!palette || styleId <= 0 || styleId >= palette->getStyleCount())
    return false;
  TColorStyle *current = palette->getStyle(styleId);
  if (!current || GH::kindFromTagId(current->getTagId()) == GH::NoGradient)
    return false;
  TColorStyle *style = current->clone();
  int count          = std::min(style->getParamCount(), (int)values.size());
  for (int i = 0; i < count; ++i) style->setParamValue(i, values[i]);
  style->setIsEditedFlag(editedFlag);
  style->invalidateIcon();
  palette->setStyle(styleId, style);  // the palette takes ownership
  return true;
}

//! A style copied from a studio palette is marked as edited when it changes,
//! as the Style Editor does.
bool editedFlagAfterChange(const TColorStyle *style) {
  if (style->getGlobalName() != L"" && style->getOriginalName() != L"")
    return true;
  return style->getIsEditedFlag();
}

//! Whether \p styleId is a Linear or Radial Gradient style of \p palette
//! that the handles can edit.
bool isGradientStyle(const TPalette *palette, int styleId) {
  if (!palette || styleId <= 0 || styleId >= palette->getStyleCount())
    return false;
  TColorStyle *style = palette->getStyle(styleId);
  return style && GH::kindFromTagId(style->getTagId()) != GH::NoGradient &&
         style->getParamCount() >= 4;
}

//=============================================================================
// Make Unique Style helpers
//-----------------------------------------------------------------------------

//! TPalette::addStyle refuses new styles from this style id on.
const int MaxPaletteStyleCount = 4096;

//! Appends the style ids of \p region and of all its sub-regions.
void collectRegionStyles(const TRegion *region, std::vector<int> &ids) {
  if (!region) return;
  ids.push_back(region->getStyle());
  UINT count = region->getSubregionCount();
  for (UINT i = 0; i < count; ++i)
    collectRegionStyles(region->getSubregion(i), ids);
}

//! How many regions of \p vi (sub-regions included) use \p styleId.
int countRegionsWithStyle(const TVectorImageP &vi, int styleId) {
  if (!vi || styleId <= 0) return 0;
  std::vector<int> ids;
  UINT count = vi->getRegionCount();
  for (UINT i = 0; i < count; ++i) collectRegionStyles(vi->getRegion(i), ids);
  return UniqueStyle::countUses(ids, styleId);
}

//! The names of the styles in the pages of \p palette.
std::vector<std::wstring> paletteStyleNames(const TPalette *palette) {
  std::vector<std::wstring> names;
  int pageCount = palette->getPageCount();
  for (int p = 0; p < pageCount; ++p) {
    const TPalette::Page *page = palette->getPage(p);
    if (!page) continue;
    int styleCount = page->getStyleCount();
    for (int i = 0; i < styleCount; ++i) {
      const TColorStyle *style = page->getStyle(i);
      if (style) names.push_back(style->getName());
    }
  }
  return names;
}

//=============================================================================
// MakeUniqueRegionUndo
//-----------------------------------------------------------------------------

//! The region part of Make Unique Style: the region under a point changes
//! from one style id to another, like the Fill tool's VectorFillUndo (private
//! to filltool.cpp), with the same notifications. The region is found with
//! TVectorImage::getRegion(), as the tool picks it, rather than with
//! TVectorImage::fill(), which skips regions outside an entered group. The
//! palette part is the AddStylesUndo of PaletteCmd::addStyles, in the same
//! undo block. The current style follows the region, so the handles and the
//! Style Editor edit the style the region has after the undo or redo.
class MakeUniqueRegionUndo final : public ToolUtils::TToolUndo {
  TPaletteP m_palette;
  TPointD m_point;
  int m_oldStyleId, m_newStyleId;

  void apply(int fromStyleId, int toStyleId) const {
    TVectorImageP vi = m_level->getFrame(m_frameId, true);
    if (!vi) return;
    {
      QMutexLocker lock(vi->getMutex());
      TRegion *region = vi->getRegion(m_point);
      if (region && region->getStyle() == fromStyleId)
        region->setStyle(toStyleId);
    }
    TTool::Application *app = TTool::getApplication();
    if (!app) return;
    PaletteController *pc = app->getPaletteController();
    TPaletteHandle *ph    = pc ? pc->getCurrentLevelPalette() : 0;
    if (ph && ph->getPalette() == m_palette.getPointer() &&
        m_palette->getStylePage(toStyleId))
      app->setCurrentLevelStyleIndex(toStyleId);
    app->getCurrentXsheet()->notifyXsheetChanged();
    notifyImageChanged();
  }

public:
  MakeUniqueRegionUndo(TXshSimpleLevel *level, const TFrameId &fid,
                       TPalette *palette, const TPointD &point, int oldStyleId,
                       int newStyleId)
      : TToolUndo(level, fid)
      , m_palette(palette)
      , m_point(point)
      , m_oldStyleId(oldStyleId)
      , m_newStyleId(newStyleId) {}

  void undo() const override { apply(m_newStyleId, m_oldStyleId); }
  void redo() const override { apply(m_oldStyleId, m_newStyleId); }

  int getSize() const override { return sizeof(*this); }

  QString getToolName() override {
    return UiTranslations::text(QObject::tr("Make Unique Style : Style#%1 > Style#%2"))
        .arg(QString::number(m_oldStyleId))
        .arg(QString::number(m_newStyleId));
  }

  int getHistoryType() override { return HistoryType::Palette; }
};

//=============================================================================
// GradientTransformUndo
//-----------------------------------------------------------------------------

//! One drag of a handle: the style parameters before and after it.
class GradientTransformUndo final : public TUndo {
  TPaletteHandle *m_paletteHandle;
  TPaletteP m_palette;
  int m_styleId;
  int m_frame;
  std::vector<double> m_before, m_after;
  bool m_editedBefore, m_editedAfter;

  void apply(const std::vector<double> &values, bool edited) const {
    if (!setStyleParams(m_palette.getPointer(), m_styleId, values, edited))
      return;
    if (m_palette->isKeyframe(m_styleId, m_frame))
      m_palette->setKeyframe(m_styleId, m_frame);
    // As UndoPaletteChange in the Style Editor: the dirty flag is left alone,
    // because m_palette may not be the current palette any more.
    if (m_paletteHandle) m_paletteHandle->notifyColorStyleChanged(false, false);
  }

public:
  GradientTransformUndo(TPaletteHandle *paletteHandle, TPalette *palette,
                        int styleId, const std::vector<double> &before,
                        const std::vector<double> &after, bool editedBefore,
                        bool editedAfter)
      : m_paletteHandle(paletteHandle)
      , m_palette(palette)
      , m_styleId(styleId)
      , m_frame(palette->getFrame())
      , m_before(before)
      , m_after(after)
      , m_editedBefore(editedBefore)
      , m_editedAfter(editedAfter) {}

  void undo() const override { apply(m_before, m_editedBefore); }
  void redo() const override { apply(m_after, m_editedAfter); }

  int getSize() const override {
    return (int)(sizeof(*this) +
                 (m_before.size() + m_after.size()) * sizeof(double));
  }

  QString getHistoryString() override {
    return UiTranslations::text(QObject::tr("Gradient Transform  Palette : %1  Style#%2"))
        .arg(QString::fromStdWString(m_palette->getPaletteName()))
        .arg(QString::number(m_styleId));
  }

  int getHistoryType() override { return HistoryType::Palette; }
};

//=============================================================================
// Options bar: the Make Unique button, the state of the picked region and a
// short help text (the tool has no settings). Defined after the tool.
//-----------------------------------------------------------------------------

class GradientTransformTool;

class GradientTransformToolOptionsBox final : public ToolOptionsBox {
  GradientTransformTool *m_tool;
  QPushButton *m_makeUniqueButton;
  QLabel *m_stateLabel;

public:
  GradientTransformToolOptionsBox(QWidget *parent, GradientTransformTool *tool);

  void updateStatus() override;
};

//=============================================================================
// Drawing helpers
//-----------------------------------------------------------------------------

TPixel32 opaque(TPixel32 color) {
  color.m = 255;
  return color;
}

void drawSegmentWithHalo(const TPointD &a, const TPointD &b,
                         const TPixel32 &color) {
  glLineWidth(3.0f);
  tglColor(TPixel32::White);
  tglDrawSegment(a, b);
  glLineWidth(1.0f);
  tglColor(color);
  tglDrawSegment(a, b);
}

void drawCircleWithHalo(const TPointD &center, double radius,
                        const TPixel32 &color) {
  if (!(radius > 0.0)) return;
  glLineWidth(3.0f);
  tglColor(TPixel32::White);
  tglDrawCircle(center, radius);
  glLineWidth(1.0f);
  tglColor(color);
  tglDrawCircle(center, radius);
}

void drawDashedSegment(const TPointD &a, const TPointD &b,
                       const TPixel32 &color) {
  glLineStipple(1, 0x3F33);
  glEnable(GL_LINE_STIPPLE);
  tglColor(color);
  tglDrawSegment(a, b);
  glDisable(GL_LINE_STIPPLE);
}

void drawSquareHandle(const TPointD &p, double r, double px,
                      const TPixel32 &fill, bool hot) {
  tglColor(hot ? HotColor : TPixel32::Black);
  tglFillRect(p.x - r - px, p.y - r - px, p.x + r + px, p.y + r + px);
  tglColor(TPixel32::White);
  tglFillRect(p.x - r, p.y - r, p.x + r, p.y + r);
  tglColor(fill);
  tglFillRect(p.x - r + px, p.y - r + px, p.x + r - px, p.y + r - px);
}

void drawRoundHandle(const TPointD &p, double r, double px,
                     const TPixel32 &fill, bool hot) {
  tglColor(hot ? HotColor : TPixel32::Black);
  tglDrawDisk(p, r + px);
  tglColor(TPixel32::White);
  tglDrawDisk(p, r);
  tglColor(fill);
  tglDrawDisk(p, r - px);
}

//! Text at a constant screen size, as the Animate tool's labels.
void drawLabel(const TPointD &p, const std::string &text, double px) {
  double scale = px * 1.6;
  for (int pass = 0; pass < 2; ++pass) {
    double offset = (pass == 0) ? px : 0.0;  // white shadow, then black
    glPushMatrix();
    glTranslated(p.x + offset, p.y - offset, 0.0);
    glScaled(scale, scale, 1.0);
    tglColor(pass == 0 ? TPixel32::White : TPixel32::Black);
    tglDrawText(TPointD(0, 0), text);
    glPopMatrix();
  }
}

//! Where to draw an end handle: on its point, but at least minGap away from
//! the centre (along unit direction dir) so that it can always be grabbed.
TPointD shownPosition(const TPointD &center, const TPointD &handle,
                      const TPointD &dir, double minGap) {
  double d = std::hypot(handle.x - center.x, handle.y - center.y);
  if (d >= minGap) return handle;
  return TPointD(center.x + dir.x * minGap, center.y + dir.y * minGap);
}

//=============================================================================
// The current state of the edited gradient, read from the palette
//-----------------------------------------------------------------------------

struct GradientView {
  GH::Kind kind;
  std::vector<double> values;  // style parameters
  TPointD center;
  TPointD head, tail;            // true positions (see Handle)
  TPointD headShown, tailShown;  // where they are drawn and picked
  double outerRadius, innerRadius;
  TPixel32 pointColor, mainColor;

  GradientView() : kind(GH::NoGradient), outerRadius(0.0), innerRadius(0.0) {}
};

//=============================================================================
// GradientTransformTool
//-----------------------------------------------------------------------------

class GradientTransformTool final : public TTool {
public:
  //! Linear: Head is the main colour end, Tail the point colour end.
  //! Radial: Head is on the outer circle (Radius), Tail on the inner circle
  //! (Smoothness).
  enum Handle { NoHandle, CenterHandle, HeadHandle, TailHandle };

private:
  // The edited region: the one under m_anchor in one frame of one level.
  int m_styleId;
  TPointD m_anchor;
  TRectD m_box;
  TXshSimpleLevel *m_level;
  TFrameId m_fid;

  Handle m_hot, m_drag;
  TPointD m_grabOffset;
  std::vector<double> m_dragStartValues;
  bool m_dragStartEdited;
  bool m_dragChanged;
  double m_radialHandleAngle;  // degrees; display only
  int m_cursorId;

  // Make Unique Style: how many regions of the edited drawing use m_styleId,
  // and the state last announced to the options bars.
  int m_styleUses;
  int m_shownStyleId, m_shownStyleUses;
  bool m_active;

public:
  GradientTransformTool()
      : TTool(T_GradientTransform)
      , m_styleId(0)
      , m_level(0)
      , m_hot(NoHandle)
      , m_drag(NoHandle)
      , m_dragStartEdited(false)
      , m_dragChanged(false)
      , m_radialHandleAngle(0.0)
      , m_cursorId(ToolCursor::CURSOR_ARROW)
      , m_styleUses(0)
      , m_shownStyleId(0)
      , m_shownStyleUses(0)
      , m_active(false) {
    bind(TTool::VectorImage);
  }

  ToolType getToolType() const override { return TTool::LevelReadTool; }

  ToolOptionsBox *createOptionsBox() override {
    return new GradientTransformToolOptionsBox(0, this);
  }

  int getCursorId() const override { return m_cursorId; }

  bool isDragging() const override { return m_drag != NoHandle; }

  void onActivate() override {
    // The options bars read the state themselves when the tool switches.
    m_active = false;
    refreshTarget();
    m_shownStyleId   = m_styleId;
    m_shownStyleUses = m_styleUses;
    m_active         = true;
  }

  void onDeactivate() override {
    finishDrag();
    m_hot    = NoHandle;
    m_active = false;
  }

  void onImageChanged() override {
    if (m_drag != NoHandle || m_styleId <= 0) return;
    refreshTarget();  // another frame or an edited drawing
    invalidate();
  }

  void draw() override;
  void mouseMove(const TPointD &pos, const TMouseEvent &e) override;
  void leftButtonDown(const TPointD &pos, const TMouseEvent &e) override;
  void leftButtonDrag(const TPointD &pos, const TMouseEvent &e) override;
  void leftButtonUp(const TPointD &pos, const TMouseEvent &e) override;
  void rightButtonDown(const TPointD &pos, const TMouseEvent &e) override;
  void addContextMenuItems(QMenu *menu) override;

  // Make Unique Style

  //! A region is picked in the current drawing and other regions of the
  //! drawing use its style.
  bool offersMakeUnique() const {
    return m_styleId > 0 && isSameCell() && m_styleUses > 1;
  }
  //! What the options bar says about the picked region.
  QString uniqueStateText() const;
  //! Makes the style of the picked region unique, or shows why it cannot.
  void runMakeUniqueStyle();

private:
  TPaletteHandle *paletteHandle() const {
    TTool::Application *app = getApplication();
    PaletteController *pc   = app ? app->getPaletteController() : 0;
    return pc ? pc->getCurrentLevelPalette() : 0;
  }

  TPalette *palette() const {
    TPaletteHandle *ph = paletteHandle();
    return ph ? ph->getPalette() : 0;
  }

  TXshSimpleLevel *currentLevel() const {
    TTool::Application *app = getApplication();
    TXshLevelHandle *lh     = app ? app->getCurrentLevel() : 0;
    return lh ? lh->getSimpleLevel() : 0;
  }

  bool isSameCell() const {
    return m_level && currentLevel() == m_level && getCurrentFid() == m_fid;
  }

  //! Forgets the edited region; the caller then calls notifyUniqueState().
  void clearTarget() {
    m_styleId   = 0;
    m_level     = 0;
    m_hot       = NoHandle;
    m_styleUses = 0;
  }

  void notifyUniqueState();
  QString makeUniqueBlocker();
  bool makeUniqueStyle(QString &reason);

  TRegion *gradientRegionAt(const TVectorImageP &vi, const TPointD &pos) const;
  void refreshTarget();
  bool pickTarget(const TPointD &pos);
  bool currentView(GradientView &view) const;
  Handle pickHandle(const TPointD &pos) const;
  void dragTo(const TPointD &pos, bool snap);
  void finishDrag();
  int cursorFor(Handle handle) const;
};

//-----------------------------------------------------------------------------

//! The region under \p pos if it is filled with a gradient style of the
//! palette the viewer draws \p vi with (the level palette), else 0.
TRegion *GradientTransformTool::gradientRegionAt(const TVectorImageP &vi,
                                                 const TPointD &pos) const {
  if (!vi) return 0;
  TPalette *pl = palette();
  if (!pl || (vi->getPalette() && vi->getPalette() != pl)) return 0;
  TRegion *region = vi->getRegion(pos);
  if (!region || !isGradientStyle(pl, region->getStyle())) return 0;
  return region;
}

//-----------------------------------------------------------------------------

//! Finds the edited region again (the drawing may have changed): its box is
//! the reference of the gradient. Drops it if it is gone or no longer has a
//! gradient style; follows it to its new style if it has another gradient
//! (for example after undoing Make Unique Style).
void GradientTransformTool::refreshTarget() {
  if (m_styleId <= 0) return;
  TVectorImageP vi;
  if (isSameCell()) vi = TImageP(getImage(false));
  TRegion *region = gradientRegionAt(vi, m_anchor);
  if (!region)
    clearTarget();
  else {
    m_styleId   = region->getStyle();
    m_box       = region->getBBox();
    m_styleUses = countRegionsWithStyle(vi, m_styleId);
  }
  notifyUniqueState();
}

//-----------------------------------------------------------------------------

bool GradientTransformTool::pickTarget(const TPointD &pos) {
  clearTarget();

  TVectorImageP vi = TImageP(getImage(false));
  TRegion *region  = gradientRegionAt(vi, pos);
  if (!region) {
    notifyUniqueState();
    return false;
  }

  int styleId         = region->getStyle();
  m_styleId           = styleId;
  m_anchor            = pos;
  m_box               = region->getBBox();
  m_level             = currentLevel();
  m_fid               = getCurrentFid();
  m_radialHandleAngle = 0.0;
  m_styleUses         = countRegionsWithStyle(vi, styleId);
  notifyUniqueState();

  // Make it the current style, so the Style Editor shows the values that the
  // handles change.
  if (getApplication()->getCurrentLevelStyleIndex() != styleId)
    getApplication()->setCurrentLevelStyleIndex(styleId);
  return true;
}

//-----------------------------------------------------------------------------

bool GradientTransformTool::currentView(GradientView &view) const {
  if (m_styleId <= 0) return false;
  TPalette *pl = palette();
  if (!pl || m_styleId >= pl->getStyleCount()) return false;
  TColorStyle *style = pl->getStyle(m_styleId);
  if (!style) return false;
  view.kind = GH::kindFromTagId(style->getTagId());
  if (view.kind == GH::NoGradient || style->getParamCount() < 4) return false;

  view.values   = readParams(style);
  double minGap = MinHandleGapPx * getPixelSize();

  if (view.kind == GH::Linear) {
    GH::LinearParams p  = GH::linearParamsFromArray(&view.values[0]);
    GH::LinearHandles h = GH::linearParamsToHandles(m_box, p);
    double a            = p.angle * GH::Pi / 180.0;
    TPointD dir(std::cos(a), std::sin(a));
    view.center    = h.center;
    view.head      = h.head;
    view.tail      = h.tail;
    view.headShown = shownPosition(h.center, h.head, dir, minGap);
    view.tailShown =
        shownPosition(h.center, h.tail, TPointD(-dir.x, -dir.y), minGap);
  } else {
    GH::RadialParams p = GH::radialParamsFromArray(&view.values[0]);
    GH::RadialHandles h =
        GH::radialParamsToHandles(m_box, p, m_radialHandleAngle);
    double a = m_radialHandleAngle * GH::Pi / 180.0;
    TPointD dir(std::cos(a), std::sin(a));
    view.center      = h.center;
    view.head        = h.outer;
    view.tail        = h.inner;
    view.outerRadius = h.outerRadius;
    view.innerRadius = h.innerRadius;
    view.headShown   = shownPosition(h.center, h.outer, dir, minGap);
    view.tailShown =
        shownPosition(h.center, h.inner, TPointD(-dir.x, -dir.y), minGap);
  }

  int colorCount = style->getColorParamCount();
  view.pointColor =
      colorCount > 0 ? style->getColorParamValue(0) : style->getMainColor();
  view.mainColor =
      colorCount > 1 ? style->getColorParamValue(1) : style->getMainColor();
  return true;
}

//-----------------------------------------------------------------------------

GradientTransformTool::Handle GradientTransformTool::pickHandle(
    const TPointD &pos) const {
  GradientView view;
  if (!currentView(view)) return NoHandle;

  double maxDist = PickRadiusPx * getPixelSize();
  // On a tie the centre wins: it is listed first.
  const Handle handles[] = {CenterHandle, HeadHandle, TailHandle};
  const TPointD points[] = {view.center, view.headShown, view.tailShown};
  Handle best            = NoHandle;
  double bestDist        = 0.0;
  for (int i = 0; i < 3; ++i) {
    double d = std::hypot(pos.x - points[i].x, pos.y - points[i].y);
    if (d > maxDist) continue;
    if (best == NoHandle || d < bestDist) {
      best     = handles[i];
      bestDist = d;
    }
  }
  return best;
}

//-----------------------------------------------------------------------------

int GradientTransformTool::cursorFor(Handle handle) const {
  if (handle == NoHandle) return ToolCursor::CURSOR_ARROW;
  if (handle == CenterHandle) return ToolCursor::MoveCursor;
  GradientView view;
  if (currentView(view) && view.kind == GH::Linear)
    return ToolCursor::RotCursor;
  return ToolCursor::ScaleCursor;
}

//-----------------------------------------------------------------------------

void GradientTransformTool::draw() {
  if (m_styleId <= 0 || !isSameCell()) return;
  GradientView view;
  if (!currentView(view)) return;

  double px = getPixelSize();
  double r  = HandleRadiusPx * px;

  // The region's box: the positions are percentages of it.
  ToolUtils::drawRect(m_box, TPixel32::Black, 0x3F33, true);

  if (view.kind == GH::Linear) {
    // The blend runs between the two lines across the head and the tail.
    double dx  = view.head.x - view.center.x;
    double dy  = view.head.y - view.center.y;
    double len = std::hypot(dx, dy);
    if (len > 1e-9) {
      double half = std::max(
          0.5 * std::hypot(GH::boxWidth(m_box), GH::boxHeight(m_box)), 20 * px);
      TPointD n(-dy / len * half, dx / len * half);
      drawDashedSegment(TPointD(view.head.x - n.x, view.head.y - n.y),
                        TPointD(view.head.x + n.x, view.head.y + n.y),
                        LineColor);
      drawDashedSegment(TPointD(view.tail.x - n.x, view.tail.y - n.y),
                        TPointD(view.tail.x + n.x, view.tail.y + n.y),
                        LineColor);
    }
    drawSegmentWithHalo(view.tailShown, view.headShown, LineColor);
  } else {
    drawCircleWithHalo(view.center, view.outerRadius, LineColor);
    drawCircleWithHalo(view.center, view.innerRadius, LineColor);
    drawSegmentWithHalo(view.tailShown, view.headShown, LineColor);
  }

  bool dragging = m_drag != NoHandle;
  Handle lit    = dragging ? m_drag : m_hot;
  drawSquareHandle(view.tailShown, r, px, opaque(view.pointColor),
                   lit == TailHandle);
  drawSquareHandle(view.headShown, r, px, opaque(view.mainColor),
                   lit == HeadHandle);
  drawRoundHandle(view.center, r * 1.25, px, LineColor, lit == CenterHandle);

  TPalette *pl      = palette();
  std::string label = "Style #" + std::to_string(m_styleId);
  if (pl && pl->isLocked())
    label += " (palette locked)";
  else if (m_styleUses > 1)
    label += " (shared by " + std::to_string(m_styleUses) +
             " regions in this drawing)";
  else
    label += " (not shared in this drawing)";
  drawLabel(TPointD(view.center.x + 3 * r, view.center.y - 6 * r), label, px);
  glLineWidth(1.0f);
}

//-----------------------------------------------------------------------------

void GradientTransformTool::mouseMove(const TPointD &pos, const TMouseEvent &) {
  if (m_drag != NoHandle) return;
  if (m_styleId > 0 && !isSameCell()) {
    clearTarget();
    notifyUniqueState();
  }
  Handle hot = pickHandle(pos);
  m_cursorId = cursorFor(hot);
  if (hot != m_hot) {
    m_hot = hot;
    invalidate();
  }
}

//-----------------------------------------------------------------------------

void GradientTransformTool::leftButtonDown(const TPointD &pos,
                                           const TMouseEvent &) {
  finishDrag();
  refreshTarget();

  Handle handle = pickHandle(pos);
  if (handle == NoHandle) {
    // Pick the region under the pointer (or nothing).
    pickTarget(pos);
    m_cursorId = cursorFor(NoHandle);
    invalidate();
    return;
  }

  TPalette *pl = palette();
  GradientView view;
  if (!pl || pl->isLocked() || !currentView(view)) {
    invalidate();
    return;
  }

  TPointD grabbed   = handle == CenterHandle ? view.center
                      : handle == HeadHandle ? view.head
                                             : view.tail;
  m_grabOffset      = TPointD(pos.x - grabbed.x, pos.y - grabbed.y);
  m_dragStartValues = view.values;
  m_dragStartEdited = pl->getStyle(m_styleId)->getIsEditedFlag();
  m_dragChanged     = false;
  m_drag            = handle;
  m_hot             = handle;
  m_cursorId        = cursorFor(handle);

  if (getApplication()->getCurrentLevelStyleIndex() != m_styleId)
    getApplication()->setCurrentLevelStyleIndex(m_styleId);
  invalidate();
}

//-----------------------------------------------------------------------------

void GradientTransformTool::leftButtonDrag(const TPointD &pos,
                                           const TMouseEvent &e) {
  if (m_drag == NoHandle) return;
  dragTo(pos, e.isShiftPressed());
}

//-----------------------------------------------------------------------------

void GradientTransformTool::leftButtonUp(const TPointD &pos,
                                         const TMouseEvent &) {
  finishDrag();
  m_hot      = pickHandle(pos);
  m_cursorId = cursorFor(m_hot);
  invalidate();
}

//-----------------------------------------------------------------------------

void GradientTransformTool::dragTo(const TPointD &pos, bool snap) {
  GradientView view;
  TPalette *pl = palette();
  if (!pl || !currentView(view)) return;
  TColorStyle *style = pl->getStyle(m_styleId);

  TPointD target(pos.x - m_grabOffset.x, pos.y - m_grabOffset.y);
  std::vector<double> values = view.values;

  if (view.kind == GH::Linear) {
    GH::LinearParams p = GH::linearParamsFromArray(&values[0]);
    if (m_drag == CenterHandle)
      p = GH::linearMoveCenter(m_box, p, target);
    else
      p = GH::linearDragEnd(
          m_box, p, m_drag == HeadHandle ? GH::LinearHead : GH::LinearTail,
          target, snap ? SnapStepDegrees : 0.0);
    GH::linearParamsToArray(p, &values[0]);
  } else {
    GH::RadialParams p = GH::radialParamsFromArray(&values[0]);
    if (m_drag == CenterHandle)
      p = GH::radialMoveCenter(m_box, p, target);
    else {
      double angle = GH::angleOfVector(target.x - view.center.x,
                                       target.y - view.center.y, 0.0);
      if (m_drag == HeadHandle) {
        p                   = GH::radialDragOuter(m_box, p, target);
        m_radialHandleAngle = angle;
      } else {
        p                   = GH::radialDragInner(m_box, p, target);
        m_radialHandleAngle = GH::normalizeAngle(angle + 180.0);
      }
    }
    GH::radialParamsToArray(p, &values[0]);
  }

  // Keep changed values inside the style's own ranges (values that were not
  // touched are left as they are).
  for (int i = 0; i < (int)values.size() && i < style->getParamCount(); ++i) {
    if (values[i] == view.values[i]) continue;
    double lo = 0.0, hi = 0.0;
    style->getParamRange(i, lo, hi);
    if (lo < hi) values[i] = GH::clampValue(values[i], lo, hi);
  }

  if (values == view.values) {
    invalidate();  // only the radial handles turned
    return;
  }

  if (!setStyleParams(pl, m_styleId, values, editedFlagAfterChange(style)))
    return;
  m_dragChanged = true;
  if (TPaletteHandle *ph = paletteHandle()) ph->notifyColorStyleChanged(true);
  invalidate();
}

//-----------------------------------------------------------------------------

//! Ends a drag: one undo step with the values before and after it.
void GradientTransformTool::finishDrag() {
  if (m_drag == NoHandle) return;
  m_drag = NoHandle;
  if (!m_dragChanged) return;
  m_dragChanged = false;

  TPaletteHandle *ph = paletteHandle();
  TPalette *pl       = palette();
  if (!ph || !pl || m_styleId <= 0 || m_styleId >= pl->getStyleCount()) return;
  TColorStyle *style = pl->getStyle(m_styleId);
  if (!style || GH::kindFromTagId(style->getTagId()) == GH::NoGradient) return;

  // In case the frame is a palette keyframe, update it (as the Style Editor).
  if (pl->isKeyframe(m_styleId, pl->getFrame()))
    pl->setKeyframe(m_styleId, pl->getFrame());
  pl->setDirtyFlag(true);

  TUndoManager::manager()->add(new GradientTransformUndo(
      ph, pl, m_styleId, m_dragStartValues, readParams(style),
      m_dragStartEdited, style->getIsEditedFlag()));
  ph->notifyColorStyleChanged(false);
}

//=============================================================================
// Make Unique Style
//-----------------------------------------------------------------------------

//! Tells the options bars that the picked style or its number of regions
//! changed (they read the rest from the tool).
void GradientTransformTool::notifyUniqueState() {
  if (!m_active) return;  // onActivate() resynchronizes
  if (m_styleId == m_shownStyleId && m_styleUses == m_shownStyleUses) return;
  m_shownStyleId          = m_styleId;
  m_shownStyleUses        = m_styleUses;
  TTool::Application *app = getApplication();
  ToolHandle *toolHandle  = app ? app->getCurrentTool() : 0;
  if (toolHandle && toolHandle->getTool() == this)
    toolHandle->notifyToolChanged();
}

//-----------------------------------------------------------------------------

QString GradientTransformTool::uniqueStateText() const {
  if (m_styleId <= 0 || !isSameCell())
    return QCoreApplication::translate("GradientTransformTool",
                                       "No gradient region picked");
  if (m_styleUses > 1)
    return UiTranslations::text(QCoreApplication::translate(
               "GradientTransformTool",
               "Style #%1 is shared by %2 regions in this drawing"))
        .arg(m_styleId)
        .arg(m_styleUses);
  return UiTranslations::text(QCoreApplication::translate("GradientTransformTool",
                                     "Style #%1 is not shared in this drawing"))
      .arg(m_styleId);
}

//-----------------------------------------------------------------------------

//! Why Make Unique Style cannot run now (empty when it can).
QString GradientTransformTool::makeUniqueBlocker() {
  if (m_styleId <= 0 || !isSameCell())
    return QCoreApplication::translate(
        "GradientTransformTool",
        "Click a region filled with a Linear or Radial Gradient with the "
        "Gradient Transform tool first.");
  if (m_styleUses <= 1)
    return UiTranslations::text(QCoreApplication::translate(
               "GradientTransformTool",
               "No other region of this drawing uses style #%1, so it is "
               "already unique here. If other drawings use it, duplicate the "
               "style in the palette and fill this region with the copy."))
        .arg(m_styleId);
  TPalette *pl = palette();
  if (!pl || !pl->getStylePage(m_styleId))
    return QCoreApplication::translate(
        "GradientTransformTool",
        "The style of the picked region is not in a page of the level "
        "palette.");
  if (pl->isLocked())
    return QCoreApplication::translate(
        "GradientTransformTool",
        "The palette is locked: unlock it to add a copy of the style.");
  if (pl->isCleanupPalette() || pl->getStyleCount() >= MaxPaletteStyleCount)
    return QCoreApplication::translate(
        "GradientTransformTool", "No style can be added to this palette.");
  if (!m_level || m_level->isFrameReadOnly(m_fid))
    return QCoreApplication::translate(
        "GradientTransformTool",
        "The current drawing is locked (read-only level or frame): its "
        "regions cannot change style.");
  return QString();
}

//-----------------------------------------------------------------------------

//! Adds a copy of the picked region's style to the palette, right after the
//! original, and gives the copy to that region only. One undo step.
bool GradientTransformTool::makeUniqueStyle(QString &reason) {
  finishDrag();
  refreshTarget();
  reason = makeUniqueBlocker();
  if (!reason.isEmpty()) return false;

  TPaletteHandle *ph   = paletteHandle();
  TPalette *pl         = ph ? ph->getPalette() : 0;
  TVectorImageP vi     = TImageP(getImage(true));
  TRegion *region      = gradientRegionAt(vi, m_anchor);
  TPalette::Page *page = pl ? pl->getStylePage(m_styleId) : 0;
  if (!pl || !page || !region || region->getStyle() != m_styleId) {
    reason = QCoreApplication::translate(
        "GradientTransformTool",
        "The picked region changed: click it again and retry.");
    return false;
  }

  int oldStyleId  = m_styleId;
  int indexInPage = page->search(oldStyleId) + 1;  // right after the original
  int newStyleId  = pl->getStyleCount();           // TPalette::addStyle appends

  // The copy looks the same, has its own name and, like a new style, is not
  // linked to a studio palette (in a studio palette it gets its own global
  // name, as PaletteCmd::createStyle does).
  TColorStyle *copy = pl->getStyle(oldStyleId)->clone();
  copy->setName(
      UniqueStyle::uniqueName(copy->getName(), paletteStyleNames(pl)));
  copy->setOriginalName(L"");
  copy->setIsEditedFlag(false);
  if (pl->getGlobalName() != L"")
    copy->setGlobalName(L"-" + pl->getGlobalName() + L"-" +
                        std::to_wstring(newStyleId));
  else
    copy->setGlobalName(L"");

  TUndoManager::manager()->beginBlock();
  PaletteCmd::addStyles(ph, page->getIndex(), indexInPage,
                        std::vector<TColorStyle *>(1, copy));
  delete copy;  // the palette holds its own clone
  if (page->getStyleId(indexInPage) != newStyleId) {
    TUndoManager::manager()->popUndo(1);  // nothing was added
    TUndoManager::manager()->endBlock();
    reason = QCoreApplication::translate(
        "GradientTransformTool", "No style can be added to this palette.");
    return false;
  }
  {
    QMutexLocker lock(vi->getMutex());
    region->setStyle(newStyleId);
  }
  TUndoManager::manager()->add(new MakeUniqueRegionUndo(
      m_level, m_fid, pl, m_anchor, oldStyleId, newStyleId));
  TUndoManager::manager()->endBlock();

  // The handles and the Style Editor now edit the copy.
  m_styleId   = newStyleId;
  m_styleUses = countRegionsWithStyle(vi, newStyleId);
  ph->notifyPaletteChanged();
  getApplication()->setCurrentLevelStyleIndex(newStyleId);
  getApplication()->getCurrentXsheet()->notifyXsheetChanged();
  notifyImageChanged();
  notifyUniqueState();
  invalidate();
  return true;
}

//-----------------------------------------------------------------------------

void GradientTransformTool::runMakeUniqueStyle() {
  QString reason;
  if (!isEnabled())
    reason = QCoreApplication::translate(
        "GradientTransformTool",
        "The Gradient Transform tool cannot edit the current column or level "
        "(it may be locked, hidden or not a Toonz Vector level).");
  else
    makeUniqueStyle(reason);
  if (!reason.isEmpty()) DVGui::warning(reason);
}

//-----------------------------------------------------------------------------

//! A right click on a gradient region picks it, as a left click does, so
//! that the context menu acts on it; elsewhere the picked region stays.
void GradientTransformTool::rightButtonDown(const TPointD &pos,
                                            const TMouseEvent &) {
  if (m_drag != NoHandle) return;
  refreshTarget();
  if (pickHandle(pos) != NoHandle) return;
  TVectorImageP vi = TImageP(getImage(false));
  TRegion *region  = gradientRegionAt(vi, pos);
  if (!region) return;
  if (m_styleId > 0 && region == gradientRegionAt(vi, m_anchor)) return;
  pickTarget(pos);
  m_hot      = NoHandle;
  m_cursorId = cursorFor(NoHandle);
  invalidate();
}

//-----------------------------------------------------------------------------

void GradientTransformTool::addContextMenuItems(QMenu *menu) {
  if (!offersMakeUnique()) return;
  QAction *action = CommandManager::instance()->getAction(MI_MakeUniqueStyle);
  if (!action) return;
  menu->addAction(action);
  menu->addSeparator();
}

//=============================================================================
// GradientTransformToolOptionsBox
//-----------------------------------------------------------------------------

GradientTransformToolOptionsBox::GradientTransformToolOptionsBox(
    QWidget *parent, GradientTransformTool *tool)
    : ToolOptionsBox(parent)
    , m_tool(tool)
    , m_makeUniqueButton(0)
    , m_stateLabel(0) {
  m_makeUniqueButton = new QPushButton(
      QCoreApplication::translate("GradientTransformTool", "Make Unique"));
  m_makeUniqueButton->setFixedHeight(20);
  m_makeUniqueButton->setToolTip(QCoreApplication::translate(
      "GradientTransformTool",
      "Make Unique Style: give the picked region its own copy of the style "
      "(added to the palette after the original), so that the handles "
      "change this region only. Available when other regions of the drawing "
      "use the same style."));
  m_layout->addWidget(m_makeUniqueButton, 0);

  m_stateLabel = new QLabel;
  m_stateLabel->setFixedHeight(20);
  m_layout->addWidget(m_stateLabel, 0);

  addSeparator();

  QLabel *label = addLabel(QCoreApplication::translate(
      "GradientTransformTool",
      "Click a region filled with a Linear or Radial Gradient and drag its "
      "handles (Shift: snap the angle). The gradient belongs to the style: "
      "every region using the same style follows."));
  label->setToolTip(QCoreApplication::translate(
      "GradientTransformTool",
      "To change only one region, use Make Unique first: the region gets "
      "its own copy of the style."));
  m_layout->addStretch(1);

  GradientTransformTool *target = m_tool;
  connect(m_makeUniqueButton, &QPushButton::clicked, this, [target]() {
    if (target) target->runMakeUniqueStyle();
  });

  updateStatus();
}

//-----------------------------------------------------------------------------

void GradientTransformToolOptionsBox::updateStatus() {
  ToolOptionsBox::updateStatus();
  if (!m_tool) return;
  m_stateLabel->setText(m_tool->uniqueStateText());
  m_makeUniqueButton->setEnabled(m_tool->offersMakeUnique());
}

//-----------------------------------------------------------------------------

GradientTransformTool gradientTransformTool;

//=============================================================================
// MI_MakeUniqueStyle: Make Unique Style on the region picked by the tool
//-----------------------------------------------------------------------------

class MakeUniqueStyleCommandHandler final : public MenuItemHandler {
public:
  MakeUniqueStyleCommandHandler() : MenuItemHandler(MI_MakeUniqueStyle) {}

  void execute() override {
    TTool::Application *app = TTool::getApplication();
    ToolHandle *toolHandle  = app ? app->getCurrentTool() : 0;
    if (!toolHandle || toolHandle->getTool() != &gradientTransformTool) {
      DVGui::warning(QCoreApplication::translate(
          "GradientTransformTool",
          "Make Unique Style works on the region picked with the Gradient "
          "Transform tool: choose that tool and click a region filled with a "
          "gradient first."));
      return;
    }
    gradientTransformTool.runMakeUniqueStyle();
  }
} makeUniqueStyleCommandHandler;

}  // namespace
