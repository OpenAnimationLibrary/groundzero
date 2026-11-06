

#include "tvectorgl.h"
#include "tgl.h"
#include "tpalette.h"
#include "tproperty.h"
#include "tthreadmessage.h"
#include "tvectorimage.h"
#include "drawutil.h"
#include "tcurveutil.h"
#include "tstroke.h"
#include "tstrokeutil.h"
#include "tvectorrenderdata.h"
#include "tstrokedeformations.h"
#include "tmathutil.h"
#include "tenv.h"

// Toonz includes
#include "toonz/tobjecthandle.h"
#include "toonz/txshlevelhandle.h"
#include "toonz/txshsimplelevel.h"

// TnzTools includes
#include "tools/tool.h"
#include "tools/toolutils.h"
#include "tools/cursors.h"
#include "tools/widthprofile.h"

// Qt includes
#include <QCoreApplication>  // For Qt translation support

// STD includes
#include <algorithm>
#include <cmath>
#include <map>

#define PUMP_MODE L"Pump"
#define THICKEN_MODE L"Thicken Brush"
#define THIN_MODE L"Thin Brush"
#define OPACITY_MODE L"Opacity Brush"
#define WIDTH_MODE L"Width"

using namespace ToolUtils;

TEnv::IntVar PumpToolWidthFalloff("PumpToolWidthFalloff", 30);

namespace {

//! Whether all the control points of \p s have zero thickness. Such strokes
//! (fill boundaries, gap-closing lines) are hidden by the renderer and must
//! not be made visible by the thickness brushes.
bool isZeroThick(const TStroke *s) {
  for (int i = 0; i < s->getControlPointCount(); ++i)
    if (s->getControlPoint(i).thick != 0) return false;
  return true;
}

//! Undo of an Opacity Brush drag: the opacity of some strokes of one frame.
class UndoStrokeOpacity final : public TToolUndo {
  std::vector<int> m_indices;
  std::vector<double> m_before, m_after;

  void apply(const std::vector<double> &opacities) const {
    TVectorImageP image = m_level->getFrame(m_frameId, true);
    if (!image) return;
    {
      QMutexLocker lock(image->getMutex());
      for (size_t i = 0; i < m_indices.size(); ++i) {
        if (m_indices[i] >= (int)image->getStrokeCount()) continue;
        image->getStroke(m_indices[i])->setOpacity(opacities[i]);
      }
    }
    m_level->setDirtyFlag(true);
    notifyImageChanged();
  }

public:
  UndoStrokeOpacity(TXshSimpleLevel *level, const TFrameId &frameId)
      : TToolUndo(level, frameId) {}

  void add(int index, double before, double after) {
    m_indices.push_back(index);
    m_before.push_back(before);
    m_after.push_back(after);
  }
  bool isEmpty() const { return m_indices.empty(); }

  void undo() const override { apply(m_before); }
  void redo() const override { apply(m_after); }
  int getSize() const override {
    return sizeof(*this) +
           m_indices.size() * (sizeof(int) + 2 * sizeof(double));
  }
  QString getToolName() override { return QObject::tr("Pump Tool Opacity"); }
};
}  // namespace

//*****************************************************************************
//    PumpTool declaration
//*****************************************************************************

class PumpTool final : public TTool {
  Q_DECLARE_TR_FUNCTIONS(PumpTool)

  int m_strokeStyleId, m_strokeIndex;  //!< Edited stroke indices
  TStroke *m_inStroke, *m_outStroke;   //!< Input/Output strokes
  std::vector<TStroke *>
      m_splitStrokes;  //!< Merging these, m_inStroke is reformed
  int m_stroke1Idx,
      m_stroke2Idx;  //!< Indices of deformed strokes among split ones

  TUndo *m_undo;  //!< Undo to be added upon non-trivial button up

  double m_actionW;               //!< The action center stroke parameter
  double m_actionS1, m_actionS2;  //!< Action center length in m_stroke
  double m_actionRadius;          //!< Tool action radius in curve length

  std::vector<double>
      m_splitPars;  //!< Split parameters for action localization
  std::vector<double> m_cpLenDiff1,
      m_cpLenDiff2;  //!< Distorted CPs' length distances from action center

  bool m_active;         //!< Whether a stroke is currently being edited
  bool m_enabled;        //!< Tells whether the image allows editing
  bool m_cursorEnabled;  //!< Whether the 'pump preview cursor' can be seen
  bool m_draw;           //!< Should be removed...?

  bool m_isCtrlPressed;  //!< Whether control key is held down or not
  int m_lockedStrokeIndex;

  TPointD m_oldPoint, m_downPoint;  //!< Mouse positions upon editing
  TThickPoint m_cursor;             //!< Pump preview cursor data
  int m_cursorId;

  double m_errorTol;  //!< Allowed approximation error during edit

  TDoubleProperty m_toolSize;
  TIntProperty m_accuracy;
  TEnumProperty m_mode;  //!< Pump, or a brush that thickens or thins lines
  TDoubleProperty m_strength;  //!< Thickness change of the brush modes
  TIntProperty m_opacity;      //!< Target stroke opacity (%) of Opacity Brush
  TIntProperty m_widthFalloff;  //!< Width mode: reach of a change, in pixels
  TPropertyGroup m_prop;

  // Brush modes: strokes touched during the current drag, by index, with the
  // undo that recorded each one before its first change
  bool m_brushing;
  TPointD m_brushLastPos;
  std::map<int, UndoModifyStroke *> m_brushUndos;
  std::map<int, TStroke *> m_brushOldStrokes;
  std::map<int, double> m_brushOldOpacity;  //!< Opacity Brush: before the drag

  // Width mode: the stroke picked by the press and the state of its drag. The
  // control points of the stroke are changed while dragging; the undo records
  // the stroke as it was at the press.
  bool m_widthDragging;  //!< A press picked a stroke, the button is down
  bool m_widthPrepared;  //!< The drag started: points added, weights computed
  int m_widthStrokeIndex;
  double m_widthPickW;  //!< Parameter of the stroke at the press
  TPointD m_widthDownPos, m_widthLastPos;
  TThickPoint m_widthCenter;  //!< Widened point of the stroke; thick is the
                              //!< width set by the drag
  int m_widthCenterCp;        //!< Control point at the widened point
  std::vector<double> m_widthBase;     //!< Thickness of each point at the press
  std::vector<double> m_widthWeights;  //!< Share of the change of each point
  UndoModifyStroke *m_widthUndo;
  TStroke *m_widthOldStroke;

public:
  PumpTool()
      : TTool("T_Pump")
      , m_active(false)
      , m_actionW(0)
      , m_strokeIndex((std::numeric_limits<UINT>::max)())
      , m_inStroke(0)
      , m_outStroke(0)
      , m_stroke1Idx(-1)
      , m_stroke2Idx(-1)
      , m_cursorEnabled(false)
      , m_cursorId(ToolCursor::PumpCursor)
      , m_actionRadius(1)
      , m_draw(false)
      , m_undo(0)
      , m_toolSize("Size:", 1, 100, 20)
      , m_accuracy("Accuracy:", 0, 100, 40)
      , m_mode("Mode:")
      , m_strength("Strength:", 1, 100, 20)
      , m_opacity("Opacity:", 0, 100, 50)
      , m_widthFalloff("Width Falloff:", 1, 100, 30)
      , m_brushing(false)
      , m_widthDragging(false)
      , m_widthPrepared(false)
      , m_widthStrokeIndex(-1)
      , m_widthPickW(0)
      , m_widthCenterCp(0)
      , m_widthUndo(0)
      , m_widthOldStroke(0)
      , m_enabled(false) {
    bind(TTool::VectorImage);

    m_splitPars.resize(2);

    m_mode.addValue(PUMP_MODE);
    m_mode.addValue(THICKEN_MODE);
    m_mode.addValue(THIN_MODE);
    m_mode.addValue(OPACITY_MODE);
    m_mode.addValue(WIDTH_MODE);

    m_prop.bind(m_mode);
    m_prop.bind(m_toolSize);
    m_prop.bind(m_accuracy);
    m_prop.bind(m_strength);
    m_prop.bind(m_opacity);
    m_prop.bind(m_widthFalloff);
  }

  ToolType getToolType() const override { return TTool::LevelWriteTool; }

  TPropertyGroup *getProperties(int targetType) override { return &m_prop; }

  void updateTranslation() override {
    m_toolSize.setQStringName(tr("Size:"));
    m_accuracy.setQStringName(tr("Accuracy:"));
    m_mode.setQStringName(tr("Mode:"));
    m_mode.setItemUIName(PUMP_MODE, tr("Pump"));
    m_mode.setItemUIName(THICKEN_MODE, tr("Thicken Brush"));
    m_mode.setItemUIName(THIN_MODE, tr("Thin Brush"));
    m_mode.setItemUIName(OPACITY_MODE, tr("Opacity Brush"));
    m_mode.setItemUIName(WIDTH_MODE, tr("Width"));
    m_strength.setQStringName(tr("Strength:"));
    m_opacity.setQStringName(tr("Opacity:"));
    m_widthFalloff.setQStringName(tr("Width Falloff:"));
  }

  void onActivate() override;
  bool onPropertyChanged(std::string propertyName) override;
  void onEnter() override;
  void onLeave() override;

  void draw() override;

  void leftButtonDown(const TPointD &pos, const TMouseEvent &e) override;
  void leftButtonDrag(const TPointD &pos, const TMouseEvent &e) override;
  void leftButtonUp(const TPointD &pos, const TMouseEvent &e) override;

  void mouseMove(const TPointD &pos, const TMouseEvent &e) override;
  bool moveCursor(const TPointD &pos);

  int getCursorId() const override {
    if (m_viewer && m_viewer->getGuidedStrokePickerMode())
      return m_viewer->getGuidedStrokePickerCursor();
    return m_cursorId;
  }
  void invalidateCursorArea();

  void onDeactivate() override;

private:
  double actionRadius(double strokeLength);
  void splitStroke(TStroke *s);
  TStroke *mergeStrokes(const std::vector<TStroke *> &strokes);

  bool getNearestStrokeWithLock(const TPointD &p, double &outW,
                                UINT &strokeIndex, double &dist2,
                                bool onlyInCurrentGroup = false);

  bool isWidthMode() const { return m_mode.getValue() == WIDTH_MODE; }
  bool isBrushMode() const {
    return m_mode.getValue() != PUMP_MODE && m_mode.getValue() != WIDTH_MODE;
  }
  double brushRadius() const { return m_toolSize.getValue() * getPixelSize(); }
  void brushAt(const TPointD &pos, double moved);
  void endBrush(bool accept);
  void opacityBrushAt(const TPointD &pos, double moved);
  void endOpacityBrush(bool accept);

  bool pickWidthStroke(const TPointD &pos, int &index, double &w);
  void widthDown(const TPointD &pos);
  void widthDrag(const TPointD &pos);
  void endWidth();
  void prepareWidth(TStroke *stroke);

} PumpToolInstance;

//*****************************************************************************
//    PumpTool implementation
//*****************************************************************************

void PumpTool::onActivate() {
  m_widthFalloff.setValue((int)PumpToolWidthFalloff, true);
}

//----------------------------------------------------------------------

bool PumpTool::onPropertyChanged(std::string propertyName) {
  if (propertyName == m_widthFalloff.getName()) {
    PumpToolWidthFalloff = m_widthFalloff.getValue();
    return true;
  }
  return false;
}

//----------------------------------------------------------------------

void PumpTool::onEnter() {
  m_draw = true;
  if (TTool::getApplication()->getCurrentObject()->isSpline() ||
      !(TVectorImageP)getImage(false)) {
    m_enabled  = false;
    m_cursorId = ToolCursor::CURSOR_NO;
  } else {
    m_enabled  = true;
    m_cursorId = ToolCursor::PumpCursor;
  }
}

//----------------------------------------------------------------------

void PumpTool::draw() {
  if (!m_draw || !m_enabled) return;

  if (isWidthMode()) {
    glColor3d(1.0, 0.0, 1.0);
    if (m_widthDragging) {
      // The circle is the new width at the widened point; the pointer is on it
      if (m_widthPrepared) {
        tglDrawCircle(m_widthCenter, m_widthCenter.thick);
        tglDrawSegment(TPointD(m_widthCenter.x, m_widthCenter.y),
                       m_widthLastPos);
      }
    } else if (m_cursorEnabled) {
      if (m_cursor.thick > 0) tglDrawCircle(m_cursor, m_cursor.thick);
      tglDrawCircle(m_cursor, m_cursor.thick + 4 * getPixelSize());
    }
    return;
  }

  if (isBrushMode()) {
    if (m_cursorEnabled) {
      glColor3d(1.0, 0.0, 1.0);
      tglDrawCircle(m_cursor, brushRadius());
    }
    return;
  }

  TVectorImageP vi = TImageP(getImage(false));
  if (!vi) return;

  QMutexLocker lock(vi->getMutex());

  TPalette *palette = vi->getPalette();
  assert(palette);
  if (m_active) {
    // Editing with the tool
    assert(m_outStroke);

    TRectD bboxD(m_outStroke->getBBox());
    TRect bbox(tfloor(bboxD.x0), tfloor(bboxD.y0), tceil(bboxD.x1) - 1,
               tceil(bboxD.y1) - 1);

    tglDraw(TVectorRenderData(TAffine(), bbox, palette, 0, true), m_outStroke);
  } else {
    // Hovering

    double w, dist;
    UINT index;

    if (m_cursorEnabled) {
      // Draw cursor
      glColor3d(1.0, 0.0, 1.0);
      if (m_cursor.thick > 0) tglDrawCircle(m_cursor, m_cursor.thick);
      tglDrawCircle(m_cursor, m_cursor.thick + 4 * getPixelSize());
    }

    if (getNearestStrokeWithLock(m_cursor, w, index, dist, true)) {
      TStroke *stroke  = vi->getStroke(index);
      double totalLen  = stroke->getLength();
      double actionLen = actionRadius(totalLen);

      tglColor(TPixel32::Red);

      if (totalLen < actionLen ||
          (stroke->isSelfLoop() && totalLen < actionLen + actionLen))
        drawStrokeCenterline(*stroke, getPixelSize());
      else {
        int i, chunckIndex1, chunckIndex2;
        double t, t1, t2, w1, w2;

        double len = stroke->getLength(w);

        double len1 = len - actionLen;
        if (len1 < 0) {
          if (stroke->isSelfLoop()) {
            len1 += totalLen;
          } else {
            len1 = 0;
          }
        }

        double len2 = len + actionLen;
        if (len2 > totalLen) {
          if (stroke->isSelfLoop()) {
            len2 -= totalLen;
          } else {
            len2 = totalLen;
          }
        }

        w1 = stroke->getParameterAtLength(len1);
        w2 = stroke->getParameterAtLength(len2);

        int chunkCount = stroke->getChunkCount();

        stroke->getChunkAndT(w1, chunckIndex1, t1);
        stroke->getChunkAndT(w2, chunckIndex2, t2);
        double step;

        const TThickQuadratic *q = 0;

        glBegin(GL_LINE_STRIP);

        q    = stroke->getChunk(chunckIndex1);
        step = computeStep(*q, getPixelSize());

        if (chunckIndex1 == chunckIndex2 && t1 < t2) {
          for (t = t1; t < t2; t += step) tglVertex(q->getPoint(t));

          tglVertex(stroke->getPoint(w2));
          glEnd();
          return;
        }

        for (t = t1; t < 1; t += step) tglVertex(q->getPoint(t));

        for (i = chunckIndex1 + 1; i != chunckIndex2; i++) {
          if (i == chunkCount) i = 0;

          if (i == chunckIndex2) break;

          q    = stroke->getChunk(i);
          step = computeStep(*q, getPixelSize());
          for (t = 0; t < 1; t += step) tglVertex(q->getPoint(t));
        }

        q    = stroke->getChunk(chunckIndex2);
        step = computeStep(*q, getPixelSize());
        for (t = 0; t < t2; t += step) tglVertex(q->getPoint(t));

        tglVertex(stroke->getPoint(w2));

        glEnd();
      }
    }
  }
}

//----------------------------------------------------------------------

void PumpTool::leftButtonDown(const TPointD &pos, const TMouseEvent &e) {
  if (getViewer() && getViewer()->getGuidedStrokePickerMode()) {
    getViewer()->doPickGuideStroke(pos);
    return;
  }

  if (m_active || !m_enabled) return;

  if (isWidthMode()) {
    if (!m_widthDragging) widthDown(pos);
    return;
  }

  if (isBrushMode()) {
    if (!(TVectorImageP)getImage(true)) return;
    m_brushing     = true;
    m_brushLastPos = pos;
    // A click without moving still makes a small change
    brushAt(pos, 0.25 * brushRadius());
    return;
  }

  m_isCtrlPressed = e.isCtrlPressed();

  assert(m_undo == 0);
  m_active = false;

  TVectorImageP vi(getImage(true));
  if (!vi) return;

  QMutexLocker lock(vi->getMutex());

  // set current point and init parameters
  m_oldPoint  = pos;
  m_downPoint = pos;

  m_inStroke = m_outStroke = 0;
  m_stroke1Idx = m_stroke2Idx = -1;
  m_splitPars[0] = m_splitPars[1] = -2;
  m_actionW                       = 0;

  m_errorTol = (1.0 - 0.01 * m_accuracy.getValue()) * getPixelSize();

  double dist2 = 0.0;
  int cpCount;
  int i;
  UINT index;

  if (getNearestStrokeWithLock(pos, m_actionW, index, dist2)) {
    // A stroke near the pressed point was found - modify it
    m_active      = true;
    m_strokeIndex = index;

    m_inStroke  = vi->getStroke(m_strokeIndex);
    m_outStroke = new TStroke(*m_inStroke);

    double totalLength = m_inStroke->getLength();
    TXshSimpleLevel *sl =
        TTool::getApplication()->getCurrentLevel()->getSimpleLevel();
    assert(sl);
    TFrameId id = getCurrentFid();

    // Allocate the modification undo - will be assigned to the undo manager on
    // mouse release
    m_undo = new UndoModifyStrokeAndPaint(sl, id, m_strokeIndex);

    // Set the stroke's style to 'none'. This is needed to make the original
    // stroke transparent,
    // while the deformed one is shown at its place.
    m_strokeStyleId = m_inStroke->getStyle();
    m_inStroke->setStyle(0);

    if (totalLength <= 0.0) {
      // Single point case
      cpCount = m_inStroke->getControlPointCount();
      m_cpLenDiff1.resize(cpCount);

      for (i = 0; i < cpCount; i++) m_cpLenDiff1[i] = 0.0;

      m_splitStrokes.resize(1);
      m_splitStrokes[0] = new TStroke(*m_inStroke);

      m_stroke1Idx = 0;
    } else
      // Common strokes - split the stroke according to deformation requirements
      splitStroke(m_inStroke);
  }

  invalidate();
}

//----------------------------------------------------------------------

void PumpTool::leftButtonDrag(const TPointD &pos, const TMouseEvent &e) {
  if (m_brushing) {
    m_cursor = TThickPoint(pos, 0);
    brushAt(pos, tdistance(pos, m_brushLastPos));
    m_brushLastPos = pos;
    return;
  }

  if (m_widthDragging) {
    widthDrag(pos);
    return;
  }

  if (!m_active || !m_enabled) return;

  TVectorImageP vi(getImage(true));
  if (!vi || !m_outStroke) return;

  m_isCtrlPressed = e.isCtrlPressed();

  QMutexLocker lock(vi->getMutex());

  // Revert current deformation, recovering the one from button press
  delete m_outStroke;
  m_outStroke = nullptr;

  // Retrieve cursor's vertical displacement
  TPointD delta = TPointD(0, (pos - m_downPoint).y);
  int deltaSign = tsign(delta.y);
  if (deltaSign == 0) {
    // Use a copy of the original stroke
    m_outStroke = new TStroke(*m_inStroke);
    m_outStroke->setStyle(m_strokeStyleId);
    invalidate();
    return;
  }

  // Build deformation upon the original stroke pieces
  TStroke *stroke1 = 0, *stroke2 = 0;

  stroke1 = new TStroke(*m_splitStrokes[m_stroke1Idx]);

  // Deform stroke1
  TStrokeThicknessDeformation deformer(stroke1, delta, m_actionS1,
                                       m_actionRadius, deltaSign);
  modifyThickness(*stroke1, deformer, m_cpLenDiff1, deltaSign < 0);

  if (m_stroke2Idx >= 0) {
    // Deform stroke2
    stroke2 = new TStroke(*m_splitStrokes[m_stroke2Idx]);

    TStrokeThicknessDeformation deformer2(stroke2, delta, m_actionS2,
                                          m_actionRadius, deltaSign);
    modifyThickness(*stroke2, deformer2, m_cpLenDiff2, deltaSign < 0);
  }

  // Apply deformation
  std::vector<TStroke *> splitStrokesCopy(m_splitStrokes);
  splitStrokesCopy[m_stroke1Idx] = stroke1;
  if (stroke2) splitStrokesCopy[m_stroke2Idx] = stroke2;

  m_outStroke = mergeStrokes(splitStrokesCopy);

  delete stroke1;
  delete stroke2;

  invalidate();
}

//----------------------------------------------------------------------

void PumpTool::leftButtonUp(const TPointD &pos, const TMouseEvent &e) {
  if (m_brushing) {
    endBrush(true);
    return;
  }

  if (m_widthDragging) {
    endWidth();
    return;
  }

  TVectorImageP vi;

  if (!m_active || !m_enabled) goto cleanup;

  vi = TVectorImageP(getImage(true));
  if (!vi) goto cleanup;

  m_isCtrlPressed = e.isCtrlPressed();

  {
    m_active = false;

    QMutexLocker lock(vi->getMutex());

    // Reset cursor data
    double t;
    UINT index;
    double dist2;
    if (getNearestStrokeWithLock(pos, t, index, dist2)) {
      TStroke *nearestStroke = vi->getStroke(index);
      if (nearestStroke) m_cursor = nearestStroke->getThickPoint(t);
    }

    if (m_outStroke &&
        !areAlmostEqual(m_downPoint, pos, PickRadius * getPixelSize())) {
      // Accept action

      // Clone input stroke - it is someway needed by the stroke change
      // notifier... I wonder why...
      TStroke *oldStroke = new TStroke(*m_inStroke);

      m_outStroke->swap(*m_inStroke);

      m_inStroke->invalidate();

      delete m_outStroke;
      m_outStroke = 0;

      assert(m_undo);
      TUndoManager::manager()->add(m_undo);
      m_undo = 0;

      vi->notifyChangedStrokes(m_strokeIndex, oldStroke);
      notifyImageChanged();

      delete oldStroke;
    }
  }

cleanup:

  if (m_inStroke)
    m_inStroke->setStyle(
        m_strokeStyleId);  // Make the image stroke visible again

  m_strokeIndex = m_strokeStyleId = -1;

  clearPointerContainer(m_splitStrokes);

  delete m_outStroke;
  m_inStroke = m_outStroke = 0;

  delete m_undo;
  m_undo = 0;

  invalidate();
}

//----------------------------------------------------------------------

void PumpTool::invalidateCursorArea() {
  double r = m_cursor.thick + 6;
  TPointD d(r, r);
  invalidate(TRectD(m_cursor - d, m_cursor + d));
}

//----------------------------------------------------------------------

void PumpTool::mouseMove(const TPointD &pos, const TMouseEvent &e) {
  if (m_active || !m_enabled || m_widthDragging) return;

  if (isWidthMode()) {
    // Cursor preview updates on 3-pixel steps
    if (tdistance2(pos, m_oldPoint) < 9.0 * sq(getPixelSize())) return;
    m_oldPoint      = pos;
    m_draw          = true;
    m_cursorEnabled = false;

    int index;
    double w;
    if (pickWidthStroke(pos, index, w)) {
      TVectorImageP vi(getImage(false));
      if (vi) {
        m_cursor        = vi->getStroke(index)->getThickPoint(w);
        m_cursorEnabled = true;
      }
    }
    invalidate();
    return;
  }

  if (isBrushMode()) {
    m_draw          = true;
    m_cursor        = TThickPoint(pos, 0);
    m_cursorEnabled = true;
    invalidate();
    return;
  }

  m_isCtrlPressed = e.isCtrlPressed();

  // Cursor preview updates on 3-pixel steps
  if (tdistance2(pos, m_oldPoint) < 9.0 * sq(getPixelSize())) return;

  if (!m_draw) m_draw = true;

  m_oldPoint = pos;

  if (moveCursor(pos)) {
    m_cursorEnabled = true;
    invalidate();
  } else
    m_cursorEnabled = false;

  invalidate();
}

//----------------------------------------------------------------------

bool PumpTool::moveCursor(const TPointD &pos) {
  TVectorImageP vi(getImage(false));
  if (vi) {
    double t;
    UINT index;
    double dist2;
    if (getNearestStrokeWithLock(pos, t, index, dist2)) {
      TStroke *stroke = vi->getStroke(index);
      if (stroke) {
        m_cursor = stroke->getThickPoint(t);
        return true;
      }
    }
  }

  return false;
}

//----------------------------------------------------------------------

void PumpTool::onDeactivate() {
  m_draw = false;
  if (m_brushing) endBrush(true);
  if (m_widthDragging) endWidth();
  if (m_active) {
    m_active = false;
    TVectorImageP vi(getImage(true));
    assert(!!vi && m_outStroke);
    if (!vi || !m_outStroke) return;

    clearPointerContainer(m_splitStrokes);
    if (m_splitPars[0] == -1) {
      delete m_outStroke;
      m_outStroke = 0;
    }

    // restore previous style
    assert(m_strokeIndex >= 0);
    if (m_strokeIndex >= 0) {
      TStroke *stroke = vi->getStroke(m_strokeIndex);
      stroke->setStyle(m_strokeStyleId);
    }

    assert(m_undo);
    delete m_undo;
    m_undo = 0;

    invalidate();

    m_strokeIndex = -1;
    m_outStroke   = 0;
  }
}

//----------------------------------------------------------------------

void PumpTool::onLeave() {
  if (!m_active) m_draw = false;
}

//*****************************************************************************
//    PumpTool privates
//*****************************************************************************

//! Width mode: the stroke whose outline is nearest to \p pos, if it is within
//! a few pixels of it. Returns its index and the parameter of the nearest
//! point of its center line. Strokes outside the entered group and strokes
//! without thickness (hidden by the renderer) are not picked; where strokes
//! overlap, the one drawn on top wins.
bool PumpTool::pickWidthStroke(const TPointD &pos, int &index, double &w) {
  TVectorImageP vi(getImage(false));
  if (!vi) return false;

  const double tolerance = 8.0 * getPixelSize();
  double best            = tolerance;
  bool found             = false;

  for (UINT i = 0; i < vi->getStrokeCount(); ++i) {
    if (!vi->inCurrentGroup(i)) continue;
    TStroke *stroke = vi->getStroke(i);
    if (!stroke->getBBox().enlarge(tolerance).contains(pos)) continue;
    if (isZeroThick(stroke)) continue;

    double nearestW = stroke->getW(pos);
    TThickPoint c   = stroke->getThickPoint(nearestW);
    // Distance to the outline: inside the stroke counts as 0
    double dist = std::max(0.0, tdistance(pos, TPointD(c.x, c.y)) - c.thick);
    if (dist <= best) {
      best  = dist;
      index = (int)i;
      w     = nearestW;
      found = true;
    }
  }
  return found;
}

//----------------------------------------------------------------------

//! Width mode, button press: picks the stroke to be widened. Nothing is
//! changed until the pointer moves.
void PumpTool::widthDown(const TPointD &pos) {
  TVectorImageP vi(getImage(true));
  if (!vi) return;

  QMutexLocker lock(vi->getMutex());

  TXshSimpleLevel *sl =
      TTool::getApplication()->getCurrentLevel()->getSimpleLevel();
  if (!sl) return;

  int index;
  double w;
  if (!pickWidthStroke(pos, index, w)) return;

  delete m_widthUndo;
  delete m_widthOldStroke;

  TStroke *stroke    = vi->getStroke(index);
  m_widthStrokeIndex = index;
  m_widthPickW       = w;
  m_widthDownPos     = pos;
  m_widthLastPos     = pos;
  m_widthCenter      = stroke->getThickPoint(w);
  m_widthPrepared    = false;
  m_widthUndo        = new UndoModifyStroke(sl, getCurrentFid(), index);
  m_widthOldStroke   = new TStroke(*stroke);
  m_widthDragging    = true;
  m_widthBase.clear();
  m_widthWeights.clear();

  invalidate();
}

//----------------------------------------------------------------------

//! Width mode, first move: makes the stroke ready for the drag. A control
//! point is added at the picked point unless there is one within two pixels
//! (adding a point does not change the shape). The thickness change fades out
//! over the falloff distance on both sides; where no control point lies at
//! that distance, one is added so that the fade ends on a control point and
//! no spike appears. The thickness of every point at this moment and its share
//! of the change are kept for the next moves.
void PumpTool::prepareWidth(TStroke *stroke) {
  const double pixel = getPixelSize();
  const double snap  = 2.0 * pixel;
  double radius      = std::max(1, m_widthFalloff.getValue()) * pixel;

  // The on-curve control point nearest, along the stroke, to the length s
  auto nearestOnCurve = [stroke](double s, double &dist) {
    int best = 0;
    dist     = (std::numeric_limits<double>::max)();
    for (int i = 0; i < stroke->getControlPointCount(); i += 2) {
      double d = std::abs(stroke->getLengthAtControlPoint(i) - s);
      if (d < dist) {
        dist = d;
        best = i;
      }
    }
    return best;
  };

  const bool closed = stroke->isSelfLoop();

  // The widened point: an existing control point close enough, or a new one
  double s0 = stroke->getLength(m_widthPickW);
  double dist;
  int nearest = nearestOnCurve(s0, dist);
  if (dist > snap) {
    stroke->insertControlPoints(m_widthPickW);
    s0 = stroke->getLength(m_widthPickW);
  } else
    s0 = stroke->getLengthAtControlPoint(nearest);

  // Where the fade ends, on each side
  double total = stroke->getLength();
  if (closed) radius = std::min(radius, 0.5 * total);
  double ends[2] = {s0 - radius, s0 + radius};
  for (int side = 0; side < 2; ++side) {
    double s = ends[side];
    if (closed) {
      if (s < 0.0) s += total;
      if (s > total) s -= total;
    }
    if (s <= 0.0 || s >= total) continue;
    nearestOnCurve(s, dist);
    if (dist > snap)
      stroke->insertControlPoints(stroke->getParameterAtLength(s));
  }

  // Thickness and length of every control point, then the share of each
  const int count = stroke->getControlPointCount();
  std::vector<double> lengths(count);
  m_widthBase.resize(count);
  for (int i = 0; i < count; ++i) {
    lengths[i]     = stroke->getLengthAtControlPoint(i);
    m_widthBase[i] = stroke->getControlPoint(i).thick;
  }

  int center = 0;
  dist       = (std::numeric_limits<double>::max)();
  for (int i = 0; i < count; i += 2) {
    double d = std::abs(lengths[i] - s0);
    if (d < dist) {
      dist   = d;
      center = i;
    }
  }
  m_widthCenterCp = center;
  m_widthCenter   = stroke->getControlPoint(center);
  m_widthWeights  = WidthProfile::falloffWeights(lengths, lengths[center],
                                                 radius, closed, lengths.back());
}

//----------------------------------------------------------------------

//! Width mode, move: the half thickness at the widened point becomes the
//! distance between the pointer and the center of the stroke there (so
//! the outline follows the pointer), and the points around it follow by their
//! share of the change.
void PumpTool::widthDrag(const TPointD &pos) {
  TVectorImageP vi(getImage(true));
  if (!vi || m_widthStrokeIndex < 0 ||
      m_widthStrokeIndex >= (int)vi->getStrokeCount())
    return;

  QMutexLocker lock(vi->getMutex());
  TStroke *stroke = vi->getStroke(m_widthStrokeIndex);
  m_widthLastPos  = pos;

  if (!m_widthPrepared) {
    // A click or a tremor does not change the stroke
    if (tdistance(pos, m_widthDownPos) < 2.0 * getPixelSize()) return;
    prepareWidth(stroke);
    m_widthPrepared = true;
  }

  const int count = stroke->getControlPointCount();
  if ((int)m_widthBase.size() != count || (int)m_widthWeights.size() != count)
    return;

  const double target = WidthProfile::thicknessFromDistance(
      tdistance(pos, TPointD(m_widthCenter.x, m_widthCenter.y)));
  const double delta = target - m_widthBase[m_widthCenterCp];

  for (int i = 0; i < count; ++i) {
    double weight  = m_widthWeights[i];
    double thick   = weight > 0.0 ? WidthProfile::blendThickness(m_widthBase[i],
                                                                 delta, weight)
                                  : m_widthBase[i];
    TThickPoint cp = stroke->getControlPoint(i);
    if (thick != cp.thick)
      stroke->setControlPoint(i, TThickPoint(cp.x, cp.y, thick));
  }
  stroke->invalidate();

  m_widthCenter.thick = target;
  invalidate();
}

//----------------------------------------------------------------------

//! Width mode, button release: one undo for the whole drag. A press that
//! did not start a drag leaves the stroke untouched.
void PumpTool::endWidth() {
  m_widthDragging = false;

  TVectorImageP vi(getImage(true));
  if (m_widthPrepared && vi && m_widthUndo && m_widthOldStroke &&
      m_widthStrokeIndex >= 0 &&
      m_widthStrokeIndex < (int)vi->getStrokeCount()) {
    TUndoManager::manager()->add(m_widthUndo);  // records the stroke now
    m_widthUndo = 0;

    {
      QMutexLocker lock(vi->getMutex());
      vi->notifyChangedStrokes(m_widthStrokeIndex, m_widthOldStroke);
    }
    notifyImageChanged();
  }

  delete m_widthUndo;
  m_widthUndo = 0;
  delete m_widthOldStroke;
  m_widthOldStroke   = 0;
  m_widthPrepared    = false;
  m_widthStrokeIndex = -1;
  m_widthBase.clear();
  m_widthWeights.clear();

  invalidate();
}

//----------------------------------------------------------------------

//! Thickens or thins the control points within the brush radius of every
//! stroke of the current image, by an amount proportional to the distance
//! the brush moved and fading toward the brush edge.
void PumpTool::brushAt(const TPointD &pos, double moved) {
  if (m_mode.getValue() == OPACITY_MODE) {
    opacityBrushAt(pos, moved);
    return;
  }

  TVectorImageP vi(getImage(true));
  if (!vi) return;

  QMutexLocker lock(vi->getMutex());

  TXshSimpleLevel *sl =
      TTool::getApplication()->getCurrentLevel()->getSimpleLevel();
  if (!sl) return;
  TFrameId fid = getCurrentFid();

  const double radius = brushRadius();
  const double sign   = (m_mode.getValue() == THIN_MODE) ? -1.0 : 1.0;
  const double amount = sign * 0.01 * m_strength.getValue() * moved;
  if (amount == 0.0) return;

  for (UINT i = 0; i < vi->getStrokeCount(); ++i) {
    // Like picking, only strokes of the entered group can be edited
    if (!vi->inCurrentGroup(i)) continue;
    TStroke *stroke = vi->getStroke(i);
    if (!stroke->getBBox().enlarge(radius).contains(pos)) continue;
    // Leave hidden zero-thickness strokes alone, as the renderer does
    if (isZeroThick(stroke)) continue;

    bool changed = false;
    for (int j = 0; j < stroke->getControlPointCount(); ++j) {
      TThickPoint cp = stroke->getControlPoint(j);
      double dist    = tdistance(TPointD(cp.x, cp.y), pos);
      if (dist >= radius) continue;

      if (!changed && m_brushUndos.count(i) == 0) {
        // Record the stroke before its first change in this drag
        m_brushUndos[i]      = new UndoModifyStroke(sl, fid, i);
        m_brushOldStrokes[i] = new TStroke(*stroke);
      }
      changed = true;

      double falloff = 1.0 - dist / radius;
      double thick   = std::max(0.0, cp.thick + amount * falloff * falloff);
      stroke->setControlPoint(j, TThickPoint(cp.x, cp.y, thick));
    }
    if (changed) stroke->invalidate();
  }

  invalidate();
}

//----------------------------------------------------------------------

//! Ends a brush drag: adds one undo for all the strokes it changed.
void PumpTool::endBrush(bool accept) {
  endOpacityBrush(accept);
  m_brushing = false;
  TVectorImageP vi(getImage(true));

  if (accept && vi && !m_brushUndos.empty()) {
    std::vector<int> indices;
    std::vector<TStroke *> oldStrokes;
    TUndoManager::manager()->beginBlock();
    for (auto &undo : m_brushUndos) {
      TUndoManager::manager()->add(undo.second);
      indices.push_back(undo.first);
      oldStrokes.push_back(m_brushOldStrokes[undo.first]);
    }
    TUndoManager::manager()->endBlock();
    m_brushUndos.clear();

    {
      QMutexLocker lock(vi->getMutex());
      vi->notifyChangedStrokes(indices, oldStrokes);
    }
    notifyImageChanged();
  }

  for (auto &undo : m_brushUndos) delete undo.second;
  m_brushUndos.clear();
  for (auto &old : m_brushOldStrokes) delete old.second;
  m_brushOldStrokes.clear();

  invalidate();
}

//! Moves the opacity of the strokes within the brush radius toward the
//! Opacity value, faster near the brush center. Opacity is a per-stroke value
//! (see doc/pencil_opacity.md), so a stroke is touched when any part of it is
//! under the brush.
void PumpTool::opacityBrushAt(const TPointD &pos, double moved) {
  TVectorImageP vi(getImage(true));
  if (!vi) return;

  QMutexLocker lock(vi->getMutex());

  const double radius = brushRadius();
  const double target = 0.01 * m_opacity.getValue();
  const double rate =
      std::min(1.0, 0.04 * m_strength.getValue() * moved / radius);
  if (rate <= 0.0) return;

  for (UINT i = 0; i < vi->getStrokeCount(); ++i) {
    // Like picking, only strokes of the entered group can be edited
    if (!vi->inCurrentGroup(i)) continue;
    TStroke *stroke = vi->getStroke(i);
    if (!stroke->getBBox().enlarge(radius).contains(pos)) continue;
    // Leave hidden zero-thickness strokes alone, as the renderer does
    if (isZeroThick(stroke)) continue;

    double w, dist2;
    if (!stroke->getNearestW(pos, w, dist2, false)) continue;
    double dist =
        std::max(0.0, std::sqrt(dist2) - stroke->getThickPoint(w).thick);
    if (dist >= radius) continue;

    const double old = stroke->getOpacity();
    // Record the stroke before its first change in this drag
    if (m_brushOldOpacity.count(i) == 0) m_brushOldOpacity[i] = old;

    double falloff = 1.0 - dist / radius;
    stroke->setOpacity(old + (target - old) * std::min(1.0, rate * falloff));
  }

  invalidate();
}

//----------------------------------------------------------------------

//! Ends an Opacity Brush drag: adds one undo for all the strokes it changed.
void PumpTool::endOpacityBrush(bool accept) {
  if (m_brushOldOpacity.empty()) return;

  TVectorImageP vi(getImage(true));
  TXshSimpleLevel *sl =
      TTool::getApplication()->getCurrentLevel()->getSimpleLevel();

  if (accept && vi && sl) {
    UndoStrokeOpacity *undo = new UndoStrokeOpacity(sl, getCurrentFid());
    for (const auto &old : m_brushOldOpacity) {
      if (old.first >= (int)vi->getStrokeCount()) continue;
      double after = vi->getStroke(old.first)->getOpacity();
      if (after != old.second) undo->add(old.first, old.second, after);
    }
    if (undo->isEmpty())
      delete undo;
    else {
      TUndoManager::manager()->add(undo);
      sl->setDirtyFlag(true);
      notifyImageChanged();
    }
  } else if (vi) {
    // Not accepted: restore the opacities
    for (const auto &old : m_brushOldOpacity)
      if (old.first < (int)vi->getStrokeCount())
        vi->getStroke(old.first)->setOpacity(old.second);
  }

  m_brushOldOpacity.clear();
  invalidate();
}

//----------------------------------------------------------------------

double PumpTool::actionRadius(double strokeLength) {
  double toolSize         = m_toolSize.getValue();
  double toolPercent      = toolSize * 0.01;
  double interpolationVal = pow(toolPercent, 5);
  double independentValue = 7.0 * toolSize;

  double actionRadius = (independentValue) * (1.0 - interpolationVal) +
                        (strokeLength * toolPercent) * interpolationVal;

  return std::max(actionRadius, independentValue);
}

//----------------------------------------------------------------------

/*
  Edited strokes are split near the corresponding editing position, in order
  to localize stroke manipulation.
  Only the localized part of the stroke will receive CP increase and thickness
  tuning needed for the tool action.
*/
void PumpTool::splitStroke(TStroke *s) {
  assert(m_splitStrokes.empty());

  TStroke *stroke1 = 0, *stroke2 = 0;

  // Build the action radius
  double totalLength = s->getLength();
  m_actionRadius     = actionRadius(totalLength);

  // Get the length at selected point and build the split (length) positions
  m_actionS1      = s->getLength(m_actionW);
  double startLen = m_actionS1 - m_actionRadius;
  double endLen   = m_actionS1 + m_actionRadius;

  // Now, perform splitting
  int i, cpCount;

  if ((startLen <= 0 && endLen >= totalLength) ||
      (s->isSelfLoop() && totalLength < (m_actionRadius + m_actionRadius))) {
    // The whole stroke is included in the action - no split
    m_splitStrokes.resize(1);

    m_splitPars[0] = -1;

    m_splitStrokes[0] = new TStroke(*s);

    m_stroke1Idx = 0;
    stroke1      = m_splitStrokes[m_stroke1Idx];

    TStrokeThicknessDeformation deformer(s, m_actionS1, m_actionRadius);
    increaseControlPoints(*stroke1, deformer, getPixelSize());
  } else {
    if (!s->isSelfLoop() || (startLen >= 0.0 && endLen <= totalLength)) {
      // Regular split positions, in the [0.0, totalLength] range.
      // Split points at extremities are dealt.

      m_splitPars[0] = s->getParameterAtLength(
          std::max(startLen, 0.0));  // Crop in the open case
      m_splitPars[1] = s->getParameterAtLength(std::min(endLen, totalLength));

      if (m_splitPars[0] ==
          0.0)  // the "&& m_splitPars[0] == totalLength" was dealt outside
      {
        m_splitStrokes.resize(2);
        m_splitStrokes[0] = new TStroke;
        m_splitStrokes[1] = new TStroke;

        s->split(m_splitPars[1], *(m_splitStrokes[0]), *(m_splitStrokes[1]));

        m_stroke1Idx = 0;
      } else {
        if (m_splitPars[1] == 1.0) {
          m_splitStrokes.resize(2);
          m_splitStrokes[0] = new TStroke;
          m_splitStrokes[1] = new TStroke;

          s->split(m_splitPars[0], *(m_splitStrokes[0]), *(m_splitStrokes[1]));
        } else
          ::splitStroke(*s, m_splitPars, m_splitStrokes);

        m_stroke1Idx = 1;

        // Update the edit point to refer to the central stroke piece
        m_actionS1 -= m_splitStrokes[0]->getLength();
      }

      stroke1 = m_splitStrokes[m_stroke1Idx];

      // Apply deformation to the middle piece
      TStrokeThicknessDeformation deformer(stroke1, m_actionS1, m_actionRadius);
      increaseControlPoints(*stroke1, deformer, getPixelSize());

      m_actionS2 = 0;
    } else {
      // Circular 'overflow' case - (exactly) one split point is outside the
      // regular scope.

      // Since the action diameter is < totalLength, these cases are mutually
      // exclusive.
      if (startLen < 0)
        startLen += totalLength;
      else {
        endLen -= totalLength;
        m_actionS1 -= totalLength;
      }

      // The deformation must be applied in two distinct strokes, since its
      // action interval crosses the junction point

      m_splitPars[0] = s->getParameterAtLength(endLen);
      m_splitPars[1] = s->getParameterAtLength(startLen);

      ::splitStroke(*s, m_splitPars, m_splitStrokes);
      assert(m_splitStrokes.size() >= 3);

      m_stroke1Idx = 0;
      m_stroke2Idx = 2;

      stroke1 = m_splitStrokes[m_stroke1Idx];
      stroke2 = m_splitStrokes[m_stroke2Idx];

      m_actionS2 = m_actionS1 + stroke2->getLength();

      TStrokeThicknessDeformation deformer(stroke1, m_actionS1, m_actionRadius);
      increaseControlPoints(*stroke1, deformer, getPixelSize());
      TStrokeThicknessDeformation deformer2(stroke2, m_actionS2,
                                            m_actionRadius);
      increaseControlPoints(*stroke2, deformer2, getPixelSize());

      cpCount = stroke2->getControlPointCount();
      m_cpLenDiff2.resize(cpCount);

      for (i = 0; i < cpCount; ++i)
        m_cpLenDiff2[i] = stroke2->getLengthAtControlPoint(i) - m_actionS2;
    }
  }

  cpCount = stroke1->getControlPointCount();
  m_cpLenDiff1.resize(cpCount);

  double diff;
  for (i = 0; i < cpCount; i++) {
    diff            = stroke1->getLengthAtControlPoint(i) - m_actionS1;
    m_cpLenDiff1[i] = (s->isSelfLoop() && stroke2 && totalLength - diff < diff)
                          ? totalLength - diff
                          : diff;
  }
}

//----------------------------------------------------------------------

/*
  A split stroke must be reassembled before it is output.
  In particular, it must be ensured that the merge does not add additional CPS
  at split points, leaving the output seamless.
*/
TStroke *PumpTool::mergeStrokes(const std::vector<TStroke *> &strokes) {
  assert(strokes.size() > 0);

  TStroke *mergedStroke;
  if (strokes.size() > 1) {
    if (m_errorTol > 0.0) {
      strokes[m_stroke1Idx]->reduceControlPoints(m_errorTol);
      if (m_stroke2Idx >= 0)
        strokes[m_stroke2Idx]->reduceControlPoints(m_errorTol);
    }

    // Merge split strokes
    mergedStroke = merge(strokes);
    // mergedStroke->reduceControlPoints(0.4*getPixelSize());    //Originally on
    // the whole result...

    if (m_inStroke->isSelfLoop()) {
      int cpCount = mergedStroke->getControlPointCount();

      TThickPoint p1   = mergedStroke->getControlPoint(0);
      TThickPoint p2   = mergedStroke->getControlPoint(cpCount - 1);
      TThickPoint midP = 0.5 * (p1 + p2);

      mergedStroke->setControlPoint(0, midP);
      mergedStroke->setControlPoint(cpCount - 1, midP);
      mergedStroke->setSelfLoop(true);
    }

  } else {
    mergedStroke = new TStroke(*strokes[0]);
    if (m_errorTol > 0.0) mergedStroke->reduceControlPoints(m_errorTol);
  }

  // Pump changes width, not the original stroke's opacity coordinates. The
  // first temporary piece contains only a sliced envelope; copying it onto
  // the reassembled stroke would stretch that slice across the whole line.
  mergedStroke->outlineOptions() = m_inStroke->outlineOptions();

  mergedStroke->setStyle(m_strokeStyleId);
  mergedStroke->invalidate();

  return mergedStroke;
}

bool PumpTool::getNearestStrokeWithLock(const TPointD &p, double &outW,
                                        UINT &strokeIndex, double &dist2,
                                        bool onlyInCurrentGroup) {
  TVectorImageP vi = TImageP(getImage(false));
  if (!vi) return false;

  if (m_lockedStrokeIndex >= static_cast<int>(vi->getStrokeCount())) {
    m_lockedStrokeIndex = -1;
  }

  if (m_isCtrlPressed && m_lockedStrokeIndex >= 0) {
    TStroke *stroke = vi->getStroke(m_lockedStrokeIndex);
    strokeIndex     = m_lockedStrokeIndex;
    return stroke->getNearestW(p, outW, dist2);
  }

  UINT index;
  if (vi->getNearestStroke(p, outW, index, dist2, onlyInCurrentGroup)) {
    m_lockedStrokeIndex = index;
    strokeIndex         = index;
    return true;
  }

  return false;
}
