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
#include "toonz/txshlevel.h"
#include "toonz/txshcell.h"
#include "toonz/txsheet.h"
#include "toonz/tscenehandle.h"
#include "toonz/toonzscene.h"
#include "toonz/levelset.h"
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
//
//  Closing the cut edge: where the lasso cuts through a filled region, the
//  lasso boundary between two consecutive crossing points is added to the
//  drawing as an invisible Colour Art stroke (thickness 0, see
//  tools/artlayer.h), so that both the pieces taken out and the part left
//  behind are closed and keep their fill. The closing strokes are copied with
//  the inside pieces and, in Cut mode, they stay in the drawing too. Only the
//  lasso parts that lie in a filled region are closed. They are added to the
//  current group, like a stroke drawn with the Brush.
//
//  "Paste To" puts the pieces taken out in a new drawing in one step:
//   - New Frame: a new vector frame after the last frame of the level. When
//     the current xsheet cell shows that level, the new frame is also put in
//     the cell below it (or after the last cell of the column when that one is
//     taken). Otherwise, and when editing the level, only the frame is added.
//   - New Column: a new vector level "<level>_cut" with the pieces in frame 1,
//     in a new column at the right of the current one, at the current row.
//  The pieces are merged in the new drawing as the selection tool pastes them
//  (StrokesData::getImage), so the palette styles are merged by that code.
//  The paste and the Cut are a single undo step.
//=============================================================================

namespace {

TEnv::StringVar LassoCutterType("LassoCutterType", "Lasso");
TEnv::StringVar LassoCutterMode("LassoCutterMode", "Cut");
TEnv::StringVar LassoCutterPasteTo("LassoCutterPasteTo", "None");

#define LASSO_TYPE L"Lasso"
#define RECT_TYPE L"Rectangle"

#define CUT_MODE L"Cut"
#define COPY_MODE L"Copy"

#define PASTE_NONE L"None"
#define PASTE_FRAME L"New Frame"
#define PASTE_COLUMN L"New Column"

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
//! values too close to each other or to the stroke ends. \b lassoWs receives
//! the W values on the lasso of the same crossing points, in the same order.
std::vector<double> findCuts(const LassoArea &lasso, const TStroke *stroke,
                             std::vector<double> &lassoWs) {
  std::vector<DoublePair> intersections;
  intersect(lasso.stroke(), stroke, intersections, false);

  // sorted by the W on the stroke
  std::sort(intersections.begin(), intersections.end(),
            [](const DoublePair &a, const DoublePair &b) {
              return a.second < b.second;
            });

  std::vector<double> cuts;
  double last = 0.0;
  for (const DoublePair &intersection : intersections) {
    double w = intersection.second;
    if (w - last < minCutDistance || 1.0 - w < minCutDistance) continue;
    cuts.push_back(w);
    lassoWs.push_back(intersection.first);
    last = w;
  }
  return cuts;
}

//-----------------------------------------------------------------------------

//! A point where a split stroke crosses the lasso: its W on the lasso and the
//! style of the stroke.
struct CutPoint {
  double lassoW;
  int styleId;
};

//! Minimal W distance on the lasso between two cut points.
const double minLassoDistance = 1e-4;

//! Returns a new stroke, copy of the part of \b stroke between the W values
//! \b w0 < \b w1 (in [0, 1]), or 0.
TStroke *extractStrokePart(const TStroke &stroke, double w0, double w1) {
  const double eps = 1e-6;
  bool cutStart = w0 > eps, cutEnd = w1 < 1.0 - eps;
  if (!cutStart && !cutEnd) return new TStroke(stroke);

  std::vector<double> pars;
  if (cutStart) pars.push_back(w0);
  if (cutEnd) pars.push_back(w1);
  std::vector<TStroke *> parts;
  splitStroke(stroke, pars, parts);

  TStroke *part = 0;
  for (int i = 0; i < (int)parts.size(); i++) {
    if (i == (cutStart ? 1 : 0))
      part = parts[i];
    else
      delete parts[i];
  }
  return part;
}

//-----------------------------------------------------------------------------

//! Returns a new stroke, copy of the part of the closed \b stroke going from
//! the W value \b w0 to its end and from its start to \b w1, or 0.
TStroke *extractWrappedPart(const TStroke &stroke, double w0, double w1) {
  std::unique_ptr<TStroke> tail, head;
  if (w0 < 1.0 - minLassoDistance)
    tail.reset(extractStrokePart(stroke, w0, 1.0));
  if (w1 > minLassoDistance) head.reset(extractStrokePart(stroke, 0.0, w1));
  if (!tail && !head) return 0;
  if (!head) return tail.release();
  if (!tail) return head.release();

  // the end of the tail is the start of the head
  std::vector<TThickPoint> points;
  for (int i = 0; i < tail->getControlPointCount(); i++)
    points.push_back(tail->getControlPoint(i));
  for (int i = 1; i < head->getControlPointCount(); i++)
    points.push_back(head->getControlPoint(i));
  return new TStroke(points);
}

//-----------------------------------------------------------------------------

/*! Returns the strokes closing the cut edge, to be added to the drawing: the
    parts of the lasso between consecutive \b cutPoints which lie in a region
    filled in \b original, the drawing before the cut. They have thickness 0,
    so they are Colour Art strokes and they are not rendered.
*/
void makeClosingStrokes(TVectorImage *original, const TStroke &lasso,
                        std::vector<CutPoint> cutPoints,
                        std::vector<TStroke *> &closings) {
  // without regions there are no fills to keep
  if (!original->isComputedRegionAlmostOnce()) return;

  std::sort(
      cutPoints.begin(), cutPoints.end(),
      [](const CutPoint &a, const CutPoint &b) { return a.lassoW < b.lassoW; });
  std::vector<CutPoint> points;
  for (const CutPoint &point : cutPoints)
    if (points.empty() ||
        point.lassoW - points.back().lassoW >= minLassoDistance)
      points.push_back(point);
  if (points.size() < 2) return;

  QMutexLocker lock(original->getMutex());
  original->findRegions();

  int count = (int)points.size();
  for (int i = 0; i < count; i++) {
    // the last part goes from the last cut point to the first one, through the
    // start of the (closed) lasso
    bool wrapped = (i == count - 1);
    double w0    = points[i].lassoW;
    double w1    = wrapped ? points[0].lassoW + 1.0 : points[i + 1].lassoW;
    double mid   = 0.5 * (w0 + w1);
    if (mid > 1.0) mid -= 1.0;

    TRegion *region = original->getRegion(lasso.getPoint(mid));
    if (!region || region->getStyle() == 0) continue;

    TStroke *closing = wrapped ? extractWrappedPart(lasso, w0, points[0].lassoW)
                               : extractStrokePart(lasso, w0, w1);
    if (!closing) continue;

    for (int j = 0; j < closing->getControlPointCount(); j++) {
      TThickPoint point = closing->getControlPoint(j);
      point.thick       = 0;
      closing->setControlPoint(j, point);
    }
    closing->setStyle(points[i].styleId);
    closings.push_back(closing);
  }
}

//-----------------------------------------------------------------------------

/*! Splits the strokes of \b vi crossing the lasso and marks in \b isInside the
    indexes of the strokes (or pieces) inside it. If \b undo is given, it
    receives the original strokes that are split or inside, \b isNew marks
    the pieces created by the split, parallel to the strokes of \b vi, and
    \b cutPoints receives where the split strokes cross the lasso.
*/
void splitByLasso(TVectorImage *vi, const LassoArea &lasso,
                  std::vector<char> &isInside, std::vector<char> &isNew,
                  LassoCutUndo *undo, std::vector<CutPoint> &cutPoints) {
  int count = (int)vi->getStrokeCount();
  isInside.assign(count, 0);
  isNew.assign(count, 0);

  // backwards, so that splitting a stroke leaves the lower indexes valid
  for (int i = count - 1; i >= 0; i--) {
    if (!vi->inCurrentGroup(i)) continue;
    TStroke *stroke = vi->getStroke(i);
    if (!lasso.bbox().overlaps(stroke->getBBox())) continue;

    std::vector<double> lassoWs;
    std::vector<double> cuts = findCuts(lasso, stroke, lassoWs);
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

    for (double lassoW : lassoWs)
      cutPoints.push_back(CutPoint{lassoW, stroke->getStyle()});

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

//-----------------------------------------------------------------------------

/*! Adds to \b vi the strokes closing the cut edge (see makeClosingStrokes), and
    updates the arrays parallel to its strokes: the closing strokes are new
    (\b isNew), not inside (\b isInside) and marked in \b isClosing. The regions
    are found again, transferring the fills to the two sides of the closing
    strokes. \b original is the drawing before the split.
*/
void addClosingStrokes(TVectorImage *vi, TVectorImage *original,
                       const LassoArea &lasso,
                       const std::vector<CutPoint> &cutPoints,
                       std::vector<char> &isInside, std::vector<char> &isNew,
                       std::vector<char> &isClosing) {
  isClosing.assign(isInside.size(), 0);

  std::vector<TStroke *> closings;
  makeClosingStrokes(original, *lasso.stroke(), cutPoints, closings);
  if (closings.empty()) return;

  for (TStroke *closing : closings) {
    // the stroke goes at the end, or after the strokes of the current group
    int index = vi->addStroke(closing, false);
    if (index < 0 || index > (int)isInside.size()) index = (int)isInside.size();
    isInside.insert(isInside.begin() + index, 0);
    isNew.insert(isNew.begin() + index, 1);
    isClosing.insert(isClosing.begin() + index, 1);
  }
  vi->findRegions();
}

//=============================================================================
// LassoPasteUndo
//-----------------------------------------------------------------------------

/*! Undo of the "Paste To" options: the frame added to a level (\b New Frame,
    with its xsheet cell if any) or the level and the column created (\b New
    Column).
*/
class LassoPasteUndo final : public TUndo {
  TXshSimpleLevelP m_level;
  TFrameId m_frameId;
  TVectorImageP m_image;  // New Frame: the drawing, to bring the frame back
  bool m_newColumn, m_placed;
  int m_row, m_col;

public:
  LassoPasteUndo(TXshSimpleLevel *level, const TFrameId &frameId,
                 const TVectorImageP &image, bool newColumn, bool placed,
                 int row, int col)
      : m_level(level)
      , m_frameId(frameId)
      , m_image(image)
      , m_newColumn(newColumn)
      , m_placed(placed)
      , m_row(row)
      , m_col(col) {}

  static void notify() {
    TTool::Application *app = TTool::getApplication();
    if (!app) return;
    app->getCurrentScene()->notifySceneChanged();
    app->getCurrentScene()->notifyCastChange();
    app->getCurrentXsheet()->notifyXsheetChanged();
    app->getCurrentLevel()->notifyLevelChange();
  }

  void undo() const override {
    TTool::Application *app = TTool::getApplication();
    if (!app) return;
    TXsheet *xsh = app->getCurrentXsheet()->getXsheet();
    if (m_newColumn) {
      xsh->removeColumn(m_col);
      if (TLevelSet *levelSet =
              app->getCurrentScene()->getScene()->getLevelSet())
        levelSet->removeLevel(m_level.getPointer());
    } else {
      if (m_placed) xsh->setCell(m_row, m_col, TXshCell());
      m_level->eraseFrame(m_frameId);
    }
    notify();
  }

  void redo() const override {
    TTool::Application *app = TTool::getApplication();
    if (!app) return;
    TXsheet *xsh = app->getCurrentXsheet()->getXsheet();
    if (m_newColumn) {
      if (TLevelSet *levelSet =
              app->getCurrentScene()->getScene()->getLevelSet())
        levelSet->insertLevel(m_level.getPointer());
      xsh->insertColumn(m_col);
    } else {
      TVectorImageP image = m_image->clone();
      m_level->setFrame(m_frameId, image.getPointer());
      m_level->setDirtyFlag(true);
    }
    if (m_placed)
      xsh->setCell(m_row, m_col, TXshCell(m_level.getPointer(), m_frameId));
    notify();
  }

  int getSize() const override { return sizeof(*this) + 500; }

  QString getHistoryString() override {
    return QObject::tr("Lasso Cutter Tool   Paste To Level : %1")
        .arg(QString::fromStdWString(m_level->getName()));
  }
};

//-----------------------------------------------------------------------------

/*! Pastes the strokes of \b data in a new drawing, as the "Paste To" option
    says: a new frame at the end of \b srcLevel, or the frame 1 of a new
    vector level "<level>_cut" in a new column. Adds its undo. Returns false if
    nothing was created.
*/
bool pasteInNewDrawing(const StrokesData &data, TXshSimpleLevel *srcLevel,
                       bool newColumn) {
  TTool::Application *app = TTool::getApplication();
  if (!app || !srcLevel || !data.m_image || data.m_image->getStrokeCount() == 0)
    return false;
  ToonzScene *scene = app->getCurrentScene()->getScene();
  TXsheet *xsh      = app->getCurrentXsheet()->getXsheet();
  if (!scene || !xsh) return false;

  bool editingLevel = app->getCurrentFrame()->isEditingLevel();
  int curRow = editingLevel ? -1 : app->getCurrentFrame()->getFrameIndex();
  int curCol = editingLevel ? -1 : app->getCurrentColumn()->getColumnIndex();

  TXshSimpleLevel *level = srcLevel;
  TFrameId fid;
  if (newColumn) {
    // the name is made unique by the scene, if "<level>_cut" is already used
    TXshLevel *newLevel =
        scene->createNewLevel(PLI_XSHLEVEL, srcLevel->getName() + L"_cut");
    level = newLevel ? newLevel->getSimpleLevel() : 0;
    if (!level) return false;
    fid = TFrameId(1);
  } else {
    // the next free frame number after the last frame
    std::vector<TFrameId> fids;
    srcLevel->getFids(fids);
    int number = fids.empty() ? 1 : fids.back().getNumber() + 1;
    fid        = TFrameId(number);
    while (srcLevel->isFid(fid)) fid = TFrameId(++number);
  }

  TVectorImageP image = new TVectorImage();
  level->setFrame(fid, image.getPointer());
  std::set<int> indexes;
  data.getImage(image, indexes, true);
  level->setDirtyFlag(true);

  int row = -1, col = -1;
  bool placed = false;
  TXshCell cell(level, fid);
  if (newColumn) {
    // at the right of the current column, at the current row
    col = (curCol >= 0) ? curCol + 1 : xsh->getColumnCount();
    row = std::max(curRow, 0);
    xsh->insertColumn(col);
    placed = xsh->setCell(row, col, cell);
  } else if (curCol >= 0 &&
             xsh->getCell(curRow, curCol).getSimpleLevel() == srcLevel) {
    // below the current cell, or after the last one of the column
    col = curCol;
    row = curRow + 1;
    if (!xsh->getCell(row, col).isEmpty()) {
      int r0, r1;
      xsh->getCellRange(col, r0, r1);
      row = r1 + 1;
    }
    placed = xsh->setCell(row, col, cell);
  }

  TUndoManager::manager()->add(new LassoPasteUndo(
      level, fid, newColumn ? TVectorImageP() : image->clone(), newColumn,
      placed, row, col));
  LassoPasteUndo::notify();
  return true;
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
  TEnumProperty m_pasteTo;

  StrokeGenerator m_track;
  TPointD m_firstPos;
  TRectD m_selectingRect;
  bool m_active, m_firstTime;

public:
  LassoCutterTool()
      : TTool("T_LassoCutter")
      , m_type("Type:")
      , m_mode("Mode:")
      , m_pasteTo("Paste To:")
      , m_active(false)
      , m_firstTime(true) {
    bind(TTool::VectorImage);

    m_prop.bind(m_type);
    m_type.addValue(LASSO_TYPE);
    m_type.addValue(RECT_TYPE);
    m_prop.bind(m_mode);
    m_mode.addValue(CUT_MODE);
    m_mode.addValue(COPY_MODE);
    m_prop.bind(m_pasteTo);
    m_pasteTo.addValue(PASTE_NONE);
    m_pasteTo.addValue(PASTE_FRAME);
    m_pasteTo.addValue(PASTE_COLUMN);

    m_type.setId("Type");
    m_mode.setId("Mode");
    m_pasteTo.setId("PasteTo");
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
    m_pasteTo.setQStringName(tr("Paste To:"));
    m_pasteTo.setItemUIName(PASTE_NONE, tr("None"));
    m_pasteTo.setItemUIName(PASTE_FRAME, tr("New Frame"));
    m_pasteTo.setItemUIName(PASTE_COLUMN, tr("New Column"));
  }

  bool onPropertyChanged(std::string propertyName) override {
    LassoCutterType    = ::to_string(m_type.getValue());
    LassoCutterMode    = ::to_string(m_mode.getValue());
    LassoCutterPasteTo = ::to_string(m_pasteTo.getValue());
    return true;
  }

  void onActivate() override {
    if (m_firstTime) {
      m_type.setValue(::to_wstring(LassoCutterType.getValue()));
      m_mode.setValue(::to_wstring(LassoCutterMode.getValue()));
      m_pasteTo.setValue(::to_wstring(LassoCutterPasteTo.getValue()));
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

    // "Paste To" works on the Toonz Vector levels
    TXshSimpleLevel *level =
        getApplication()->getCurrentLevel()->getSimpleLevel();
    bool paste = (m_pasteTo.getValue() != PASTE_NONE) && level &&
                 level->getType() == PLI_XSHLEVEL;

    // the drawing before the cut, to find where the cut edge crosses the fills
    TVectorImageP original = vi;

    // "Copy" works on a clone, leaving the drawing untouched
    TVectorImageP image;
    std::unique_ptr<LassoCutUndo> undo;
    if (cut) {
      image = TImageP(getImage(true));
      if (!image) return;
      if (!level) return;
      if (image->isComputedRegionAlmostOnce()) {
        QMutexLocker lock(image->getMutex());
        original = image->clone();
      }
      undo.reset(new LassoCutUndo(level, getCurrentFid()));
    } else
      image = vi->clone();

    std::vector<char> isInside, isNew, isClosing;
    std::vector<CutPoint> cutPoints;
    std::set<int> insideIndexes, copyIndexes;
    {
      QMutexLocker lock(image->getMutex());
      splitByLasso(image.getPointer(), lasso, isInside, isNew, undo.get(),
                   cutPoints);
      for (int i = 0; i < (int)isInside.size(); i++)
        if (isInside[i]) insideIndexes.insert(i);

      // nothing inside: the drawing has not been modified
      if (insideIndexes.empty()) return;

      // close the cut edge, so that the fills are kept on both sides; the
      // closing strokes may shift the indexes
      addClosingStrokes(image.getPointer(), original.getPointer(), lasso,
                        cutPoints, isInside, isNew, isClosing);
      insideIndexes.clear();
      for (int i = 0; i < (int)isInside.size(); i++) {
        if (isInside[i]) insideIndexes.insert(i);
        if (isInside[i] || isClosing[i]) copyIndexes.insert(i);
      }
    }

    // the pieces and the closing strokes go to the clipboard...
    StrokesData *data = new StrokesData();
    data->setImage(image, copyIndexes);
    data->setClipboardFormats();

    // ...and to the new drawing, if asked
    std::unique_ptr<StrokesData> pasteData;
    if (paste) {
      pasteData.reset(new StrokesData());
      pasteData->setImage(image, copyIndexes);
    }

    // the clipboard owns the data
    QApplication::clipboard()->setMimeData(data, QClipboard::Clipboard);

    if (!cut && !pasteData) return;

    if (cut) {
      // the closing strokes stay in the drawing, closing the part left behind
      QMutexLocker lock(image->getMutex());
      std::vector<int> toRemove(insideIndexes.begin(), insideIndexes.end());
      image->removeStrokes(toRemove, true, true);
      for (int i = (int)toRemove.size() - 1; i >= 0; i--)
        isNew.erase(isNew.begin() + toRemove[i]);
      for (int i = 0; i < (int)isNew.size(); i++)
        if (isNew[i]) undo->addNewStroke(i, image->getVIStroke(i));
    }

    // the cut and the paste are one undo step
    TUndoManager *manager = TUndoManager::manager();
    bool block            = cut && pasteData;
    if (block) manager->beginBlock();
    if (cut) {
      manager->add(undo.release());
      notifyImageChanged();
    }
    if (pasteData)
      pasteInNewDrawing(*pasteData, level,
                        m_pasteTo.getValue() == PASTE_COLUMN);
    if (block) manager->endBlock();
  }

} lassoCutterTool;
