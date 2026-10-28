//-----------------------------------------------------------------------------
//  gradienttransformtool.cpp
//
//  Gradient Transform tool: click a vector region filled with a Linear or
//  Radial Gradient style and drag handles in the viewer to move, rotate and
//  resize the gradient. The handles edit the parameters of the palette style
//  (the same values as the Style Editor), so every region using that style
//  follows. The maths is in tools/gradienthandles.h.
//-----------------------------------------------------------------------------

// TnzTools includes
#include "tools/tool.h"
#include "tools/cursors.h"
#include "tools/toolutils.h"
#include "tools/tooloptions.h"
#include "tools/toolcommandids.h"
#include "tools/gradienthandles.h"

// TnzLib includes
#include "toonz/palettecontroller.h"
#include "toonz/tpalettehandle.h"
#include "toonz/txshlevelhandle.h"
#include "toonz/txshsimplelevel.h"

// TnzCore includes
#include "tvectorimage.h"
#include "tregion.h"
#include "tpalette.h"
#include "tcolorstyles.h"
#include "tundo.h"
#include "tgl.h"
#include "historytypes.h"

// Qt includes
#include <QCoreApplication>
#include <QLabel>
#include <QObject>
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
    return QObject::tr("Gradient Transform  Palette : %1  Style#%2")
        .arg(QString::fromStdWString(m_palette->getPaletteName()))
        .arg(QString::number(m_styleId));
  }

  int getHistoryType() override { return HistoryType::Palette; }
};

//=============================================================================
// Options bar: a short help text, the tool has no settings
//-----------------------------------------------------------------------------

class GradientTransformToolOptionsBox final : public ToolOptionsBox {
public:
  explicit GradientTransformToolOptionsBox(QWidget *parent)
      : ToolOptionsBox(parent) {
    QLabel *label = addLabel(QCoreApplication::translate(
        "GradientTransformTool",
        "Click a region filled with a Linear or Radial Gradient and drag its "
        "handles (Shift: snap the angle). The gradient belongs to the style: "
        "every region using the same style follows."));
    label->setToolTip(QCoreApplication::translate(
        "GradientTransformTool",
        "To change only one region, duplicate its style in the palette and "
        "fill the region with the copy first."));
    m_layout->addStretch(1);
  }
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
      , m_cursorId(ToolCursor::CURSOR_ARROW) {
    bind(TTool::VectorImage);
  }

  ToolType getToolType() const override { return TTool::LevelReadTool; }

  ToolOptionsBox *createOptionsBox() override {
    return new GradientTransformToolOptionsBox(0);
  }

  int getCursorId() const override { return m_cursorId; }

  bool isDragging() const override { return m_drag != NoHandle; }

  void onActivate() override { refreshTarget(); }

  void onDeactivate() override {
    finishDrag();
    m_hot = NoHandle;
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

  void clearTarget() {
    m_styleId = 0;
    m_level   = 0;
    m_hot     = NoHandle;
  }

  void refreshTarget();
  bool pickTarget(const TPointD &pos);
  bool currentView(GradientView &view) const;
  Handle pickHandle(const TPointD &pos) const;
  void dragTo(const TPointD &pos, bool snap);
  void finishDrag();
  int cursorFor(Handle handle) const;
};

//-----------------------------------------------------------------------------

//! Finds the edited region again (the drawing may have changed): its box is
//! the reference of the gradient. Drops it if it is gone or restyled.
void GradientTransformTool::refreshTarget() {
  if (m_styleId <= 0) return;
  if (!isSameCell()) {
    clearTarget();
    return;
  }
  TVectorImageP vi = TImageP(getImage(false));
  if (!vi) {
    clearTarget();
    return;
  }
  TRegion *region = vi->getRegion(m_anchor);
  if (!region || region->getStyle() != m_styleId) {
    clearTarget();
    return;
  }
  m_box = region->getBBox();
}

//-----------------------------------------------------------------------------

bool GradientTransformTool::pickTarget(const TPointD &pos) {
  clearTarget();

  TVectorImageP vi = TImageP(getImage(false));
  if (!vi) return false;
  TRegion *region = vi->getRegion(pos);
  if (!region) return false;

  int styleId  = region->getStyle();
  TPalette *pl = palette();
  if (!pl || styleId <= 0 || styleId >= pl->getStyleCount()) return false;
  // Edit only the palette the viewer draws this image with.
  if (vi->getPalette() && vi->getPalette() != pl) return false;
  TColorStyle *style = pl->getStyle(styleId);
  if (!style || GH::kindFromTagId(style->getTagId()) == GH::NoGradient ||
      style->getParamCount() < 4)
    return false;

  m_styleId           = styleId;
  m_anchor            = pos;
  m_box               = region->getBBox();
  m_level             = currentLevel();
  m_fid               = getCurrentFid();
  m_radialHandleAngle = 0.0;

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
  label += (pl && pl->isLocked()) ? " (palette locked)"
                                  : " (shared by all its regions)";
  drawLabel(TPointD(view.center.x + 3 * r, view.center.y - 6 * r), label, px);
  glLineWidth(1.0f);
}

//-----------------------------------------------------------------------------

void GradientTransformTool::mouseMove(const TPointD &pos, const TMouseEvent &) {
  if (m_drag != NoHandle) return;
  if (m_styleId > 0 && !isSameCell()) clearTarget();
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

//-----------------------------------------------------------------------------

GradientTransformTool gradientTransformTool;

}  // namespace
