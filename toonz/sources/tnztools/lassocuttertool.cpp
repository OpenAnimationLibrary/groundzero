// TnzTools includes
#include "tools/tool.h"
#include "tools/toolutils.h"
#include "tools/cursors.h"

// TnzQt includes
#include "toonzqt/imageutils.h"
#include "toonzqt/strokesdata.h"

// TnzLib includes
#include "toonz/txshlevelhandle.h"
#include "toonz/tframehandle.h"
#include "toonz/tcolumnhandle.h"
#include "toonz/txsheethandle.h"
#include "toonz/tobjecthandle.h"
#include "toonz/txshsimplelevel.h"
#include "toonz/strokegenerator.h"
#include "toonz/stage2.h"

// TnzBase includes
#include "tenv.h"

// TnzCore includes
#include "tundo.h"
#include "tstroke.h"
#include "tregion.h"
#include "tvectorimage.h"
#include "tproperty.h"
#include "tconvert.h"
#include "tgl.h"

// Qt includes
#include <QApplication>
#include <QClipboard>
#include <QCoreApplication>  // For Qt translation support
#include <QMutexLocker>

// STD includes
#include <algorithm>
#include <map>
#include <memory>
#include <set>

using namespace ToolUtils;

//=============================================================================
// Lasso Cutter Tool
//
//  Draw a lasso (or a rectangle) over a Toonz Vector drawing: the strokes
//  crossing its boundary are split where they cross it, and the pieces inside
//  are put in the clipboard ("Cut" also removes them from the drawing), ready
//  to be pasted in another drawing or column. This is the cut-out workflow of
//  separating a character into body parts.
//=============================================================================

namespace {

TEnv::StringVar LassoCutterType("LassoCutterType", "Lasso");
TEnv::StringVar LassoCutterMode("LassoCutterMode", "Cut");

#define LASSO_TYPE L"Lasso"
#define RECT_TYPE L"Rectangle"

#define CUT_MODE L"Cut"
#define COPY_MODE L"Copy"

// Minimal W distance between two cut points (and from the stroke ends).
const double minCutDistance = 1e-3;

//-----------------------------------------------------------------------------

void eraseStrokes(TVectorImage *image,
                  const std::map<int, VIStroke *> &strokes) {
  std::vector<int> indexes;
  indexes.reserve(strokes.size());
  for (const auto &stroke : strokes) indexes.push_back(stroke.first);
  image->removeStrokes(indexes, true, false);
}

//-----------------------------------------------------------------------------

void insertStrokes(TVectorImage *image,
                   const std::map<int, VIStroke *> &strokes) {
  // ascending order: each stroke goes back to its own index
  for (const auto &stroke : strokes)
    image->insertStrokeAt(cloneVIStroke(stroke.second), stroke.first);
}

//=============================================================================
// LassoCutUndo
//-----------------------------------------------------------------------------

/*! Undo of a "Cut": the strokes that were split or cut out (\b m_oldStrokes,
    at their original indexes) are swapped with the pieces left in the drawing
    (\b m_newStrokes, at their final indexes). The untouched strokes keep their
    relative order, so the indexes are valid in both directions.
*/
class LassoCutUndo final : public ToolUtils::TToolUndo {
  std::vector<TFilledRegionInf> m_oldFillInformation, m_newFillInformation;
  std::map<int, VIStroke *> m_oldStrokes, m_newStrokes;

public:
  LassoCutUndo(TXshSimpleLevel *level, const TFrameId &frameId)
      : ToolUtils::TToolUndo(level, frameId) {
    TVectorImageP image = level->getFrame(m_frameId, true);
    if (image)
      ImageUtils::getFillingInformationInArea(image, m_oldFillInformation,
                                              image->getBBox());
  }

  ~LassoCutUndo() {
    for (const auto &stroke : m_oldStrokes) deleteVIStroke(stroke.second);
    for (const auto &stroke : m_newStrokes) deleteVIStroke(stroke.second);
  }

  void addOldStroke(int index, VIStroke *stroke) {
    m_oldStrokes[index] = cloneVIStroke(stroke);
  }

  void addNewStroke(int index, VIStroke *stroke) {
    m_newStrokes[index] = cloneVIStroke(stroke);
  }

  void onAdd() override {
    ToolUtils::TToolUndo::onAdd();
    TVectorImageP image = m_level->getFrame(m_frameId, true);
    if (image)
      ImageUtils::getFillingInformationInArea(image, m_newFillInformation,
                                              image->getBBox());
  }

  void swapStrokes(const std::map<int, VIStroke *> &toRemove,
                   const std::map<int, VIStroke *> &toInsert,
                   const std::vector<TFilledRegionInf> &fillInformation) const {
    TTool::Application *app = TTool::getApplication();
    if (!app) return;

    if (app->getCurrentFrame()->isEditingScene()) {
      app->getCurrentColumn()->setColumnIndex(m_col);
      app->getCurrentFrame()->setFrame(m_row);
    } else
      app->getCurrentFrame()->setFid(m_frameId);

    TVectorImageP image = m_level->getFrame(m_frameId, true);
    if (!image) return;
    {
      QMutexLocker lock(image->getMutex());
      eraseStrokes(image.getPointer(), toRemove);
      insertStrokes(image.getPointer(), toInsert);

      if (image->isComputedRegionAlmostOnce()) image->findRegions();

      for (const TFilledRegionInf &info : fillInformation) {
        TRegion *reg = image->getRegion(info.m_regionId);
        if (reg) reg->setStyle(info.m_styleId);
      }
    }
    app->getCurrentXsheet()->notifyXsheetChanged();
    notifyImageChanged();
  }

  void undo() const override {
    swapStrokes(m_newStrokes, m_oldStrokes, m_oldFillInformation);
  }

  void redo() const override {
    swapStrokes(m_oldStrokes, m_newStrokes, m_newFillInformation);
  }

  int getSize() const override {
    return sizeof(*this) +
           (m_oldFillInformation.capacity() + m_newFillInformation.capacity()) *
               sizeof(TFilledRegionInf) +
           (m_oldStrokes.size() + m_newStrokes.size()) * 500;
  }

  QString getToolName() override { return QString("Lasso Cutter Tool"); }
};

//=============================================================================
// LassoArea
//-----------------------------------------------------------------------------

//! The area enclosed by the lasso stroke.
class LassoArea {
  TVectorImage m_image;
  const TStroke *m_stroke;
  TRectD m_bbox;

public:
  LassoArea(const TStroke &lasso) {
    TStroke *stroke = new TStroke(lasso);
    m_image.addStroke(stroke);
    m_image.findRegions();
    m_stroke = m_image.getStroke(0);
    m_bbox   = m_stroke->getBBox();
  }

  bool isValid() const { return m_image.getRegionCount() > 0; }
  const TStroke *stroke() const { return m_stroke; }
  const TRectD &bbox() const { return m_bbox; }

  bool contains(const TPointD &p) const {
    if (!m_bbox.contains(p)) return false;
    for (UINT i = 0; i < m_image.getRegionCount(); i++)
      if (m_image.getRegion(i)->contains(p)) return true;
    return false;
  }
};

//-----------------------------------------------------------------------------

//! Returns the W values where \b stroke crosses the lasso, sorted and without
//! values too close to each other or to the stroke ends.
std::vector<double> findCuts(const LassoArea &lasso, const TStroke *stroke) {
  std::vector<DoublePair> intersections;
  intersect(lasso.stroke(), stroke, intersections, false);

  std::vector<double> ws;
  for (const DoublePair &intersection : intersections)
    ws.push_back(intersection.second);
  std::sort(ws.begin(), ws.end());

  std::vector<double> cuts;
  double last = 0.0;
  for (double w : ws) {
    if (w - last < minCutDistance || 1.0 - w < minCutDistance) continue;
    cuts.push_back(w);
    last = w;
  }
  return cuts;
}

//-----------------------------------------------------------------------------

/*! Splits the strokes of \b vi crossing the lasso and marks in \b isInside the
    indexes of the strokes (or pieces) inside it. If \b undo is given, it
    receives the original strokes that are split or inside, and \b isNew marks
    the pieces created by the split, parallel to the strokes of \b vi.
*/
void splitByLasso(TVectorImage *vi, const LassoArea &lasso,
                  std::vector<char> &isInside, std::vector<char> &isNew,
                  LassoCutUndo *undo) {
  int count = (int)vi->getStrokeCount();
  isInside.assign(count, 0);
  isNew.assign(count, 0);

  // backwards, so that splitting a stroke leaves the lower indexes valid
  for (int i = count - 1; i >= 0; i--) {
    if (!vi->inCurrentGroup(i)) continue;
    TStroke *stroke = vi->getStroke(i);
    if (!lasso.bbox().overlaps(stroke->getBBox())) continue;

    std::vector<double> cuts = findCuts(lasso, stroke);
    if (cuts.empty()) {
      if (lasso.contains(stroke->getPoint(0.5))) {
        isInside[i] = 1;
        if (undo) undo->addOldStroke(i, vi->getVIStroke(i));
      }
      continue;
    }

    // the pieces between consecutive cuts
    std::vector<DoublePair> ranges;
    double last = 0.0;
    for (double w : cuts) {
      ranges.push_back(DoublePair(last, w));
      last = w;
    }
    ranges.push_back(DoublePair(last, 1.0));

    // split only the strokes having pieces both inside and outside
    bool someInside = false, someOutside = false;
    for (const DoublePair &range : ranges) {
      if (lasso.contains(stroke->getPoint(0.5 * (range.first + range.second))))
        someInside = true;
      else
        someOutside = true;
    }
    if (!someInside) continue;
    if (!someOutside) {
      isInside[i] = 1;
      if (undo) undo->addOldStroke(i, vi->getVIStroke(i));
      continue;
    }

    if (undo) undo->addOldStroke(i, vi->getVIStroke(i));

    // a self loop gets its first and last pieces joined
    int oldCount = (int)vi->getStrokeCount();
    vi->splitStroke(i, ranges);
    int pieceCount = (int)vi->getStrokeCount() - oldCount + 1;

    isInside.erase(isInside.begin() + i);
    isNew.erase(isNew.begin() + i);
    for (int j = 0; j < pieceCount; j++) {
      TStroke *piece = vi->getStroke(i + j);
      isInside.insert(isInside.begin() + i + j,
                      lasso.contains(piece->getPoint(0.5)) ? 1 : 0);
      isNew.insert(isNew.begin() + i + j, 1);
    }
  }
}

}  // namespace

//=============================================================================
// LassoCutterTool
//-----------------------------------------------------------------------------

class LassoCutterTool final : public TTool {
  Q_DECLARE_TR_FUNCTIONS(LassoCutterTool)

  TPropertyGroup m_prop;
  TEnumProperty m_type;
  TEnumProperty m_mode;

  StrokeGenerator m_track;
  TPointD m_firstPos;
  TRectD m_selectingRect;
  bool m_active, m_firstTime;

public:
  LassoCutterTool()
      : TTool("T_LassoCutter")
      , m_type("Type:")
      , m_mode("Mode:")
      , m_active(false)
      , m_firstTime(true) {
    bind(TTool::VectorImage);

    m_prop.bind(m_type);
    m_type.addValue(LASSO_TYPE);
    m_type.addValue(RECT_TYPE);
    m_prop.bind(m_mode);
    m_mode.addValue(CUT_MODE);
    m_mode.addValue(COPY_MODE);

    m_type.setId("Type");
    m_mode.setId("Mode");
  }

  ToolType getToolType() const override { return TTool::LevelWriteTool; }

  int getCursorId() const override { return ToolCursor::CutterCursor; }

  TPropertyGroup *getProperties(int targetType) override { return &m_prop; }

  void updateTranslation() override {
    m_type.setQStringName(tr("Type:"));
    m_type.setItemUIName(LASSO_TYPE, tr("Lasso"));
    m_type.setItemUIName(RECT_TYPE, tr("Rectangle"));
    m_mode.setQStringName(tr("Mode:"));
    m_mode.setItemUIName(CUT_MODE, tr("Cut"));
    m_mode.setItemUIName(COPY_MODE, tr("Copy"));
  }

  bool onPropertyChanged(std::string propertyName) override {
    LassoCutterType = ::to_string(m_type.getValue());
    LassoCutterMode = ::to_string(m_mode.getValue());
    return true;
  }

  void onActivate() override {
    if (m_firstTime) {
      m_type.setValue(::to_wstring(LassoCutterType.getValue()));
      m_mode.setValue(::to_wstring(LassoCutterMode.getValue()));
      m_firstTime = false;
    }
    reset();
  }

  void onDeactivate() override { reset(); }

  void reset() {
    m_active = false;
    m_track.clear();
    m_selectingRect.empty();
  }

  void draw() override {
    if (!m_active) return;
    bool blackBg = ToonzCheck::instance()->getChecks() & ToonzCheck::eBlackBg;
    if (m_type.getValue() == RECT_TYPE) {
      TPixel color = blackBg ? TPixel32::White : TPixel32::Red;
      drawRect(m_selectingRect, color, 0xFFFF, true);
    } else if (!m_track.isEmpty()) {
      tglColor(blackBg ? TPixel32::White : TPixel32::Black);
      m_track.drawAllFragments();
    }
  }

  void leftButtonDown(const TPointD &pos, const TMouseEvent &e) override {
    reset();
    TVectorImageP vi = TImageP(getImage(false));
    if (!vi) return;
    m_active = true;
    if (m_type.getValue() == RECT_TYPE) {
      m_selectingRect = TRectD(pos.x, pos.y, pos.x + 1, pos.y + 1);
    } else {
      m_firstPos = pos;
      m_track.add(TThickPoint(pos, 1), getPixelSize() * getPixelSize());
    }
    invalidate();
  }

  void leftButtonDrag(const TPointD &pos, const TMouseEvent &e) override {
    if (!m_active) return;
    if (m_type.getValue() == RECT_TYPE) {
      m_selectingRect.x1 = pos.x;
      m_selectingRect.y1 = pos.y;
      invalidate();
    } else {
      m_track.add(TThickPoint(pos, 1), getPixelSize() * getPixelSize());
      invalidate(m_track.getModifiedRegion());
    }
  }

  void leftButtonUp(const TPointD &pos, const TMouseEvent &e) override {
    if (!m_active) return;
    std::unique_ptr<TStroke> lasso(makeLasso());
    reset();
    if (lasso) cutOut(*lasso);
    invalidate();
  }

  //! Returns the closed stroke delimiting the area, or 0 if it is too small.
  TStroke *makeLasso() {
    double minSize = 3 * getPixelSize();
    if (m_type.getValue() == RECT_TYPE) {
      TRectD rect = m_selectingRect;
      if (rect.x0 > rect.x1) std::swap(rect.x0, rect.x1);
      if (rect.y0 > rect.y1) std::swap(rect.y0, rect.y1);
      if (rect.getLx() < minSize || rect.getLy() < minSize) return 0;

      std::vector<TPointD> corners = {rect.getP00(), rect.getP10(),
                                      rect.getP11(), rect.getP01(),
                                      rect.getP00()};
      std::vector<TThickPoint> points;
      for (int i = 0; i < (int)corners.size() - 1; i++) {
        points.push_back(TThickPoint(corners[i], 1));
        points.push_back(TThickPoint(0.5 * (corners[i] + corners[i + 1]), 1));
      }
      points.push_back(TThickPoint(corners.back(), 1));
      return new TStroke(points);
    }

    if (m_track.isEmpty()) return 0;
    m_track.add(TThickPoint(m_firstPos, 1), getPixelSize() * getPixelSize());
    m_track.filterPoints();
    TStroke *stroke = m_track.makeStroke((30.0 / 11) * getPixelSize());
    if (!stroke) return 0;
    TRectD bbox = stroke->getBBox();
    if (bbox.getLx() < minSize || bbox.getLy() < minSize) {
      delete stroke;
      return 0;
    }
    return stroke;
  }

  //! Cuts (or copies) the art inside \b lassoStroke to the clipboard.
  void cutOut(const TStroke &lassoStroke) {
    // not on the vector image of a spline path
    if (getApplication()->getCurrentObject()->isSpline()) return;
    TVectorImageP vi = TImageP(getImage(false));
    if (!vi || vi->getStrokeCount() == 0) return;

    LassoArea lasso(lassoStroke);
    if (!lasso.isValid()) return;

    bool cut = (m_mode.getValue() == CUT_MODE);

    // "Copy" works on a clone, leaving the drawing untouched
    TVectorImageP image;
    std::unique_ptr<LassoCutUndo> undo;
    if (cut) {
      image = TImageP(getImage(true));
      if (!image) return;
      TXshSimpleLevel *level =
          getApplication()->getCurrentLevel()->getSimpleLevel();
      if (!level) return;
      undo.reset(new LassoCutUndo(level, getCurrentFid()));
    } else
      image = vi->clone();

    std::vector<char> isInside, isNew;
    std::set<int> insideIndexes;
    {
      QMutexLocker lock(image->getMutex());
      splitByLasso(image.getPointer(), lasso, isInside, isNew, undo.get());
      for (int i = 0; i < (int)isInside.size(); i++)
        if (isInside[i]) insideIndexes.insert(i);
    }

    // nothing inside: the drawing has not been modified
    if (insideIndexes.empty()) return;

    StrokesData *data = new StrokesData();
    data->setImage(image, insideIndexes);
    data->setClipboardFormats();
    QApplication::clipboard()->setMimeData(data, QClipboard::Clipboard);

    if (!cut) return;

    {
      QMutexLocker lock(image->getMutex());
      std::vector<int> toRemove(insideIndexes.begin(), insideIndexes.end());
      image->removeStrokes(toRemove, true, true);
      for (int i = (int)toRemove.size() - 1; i >= 0; i--)
        isNew.erase(isNew.begin() + toRemove[i]);
      for (int i = 0; i < (int)isNew.size(); i++)
        if (isNew[i]) undo->addNewStroke(i, image->getVIStroke(i));
    }

    TUndoManager::manager()->add(undo.release());
    notifyImageChanged();
  }

} lassoCutterTool;
