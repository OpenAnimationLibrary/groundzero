#include "tools/vectorpaintclip.h"


#include "toonzqt/uitranslations.h"
#include "vectorselectiontool.h"

// TnzTools includes
#include "tools/toolhandle.h"
#include "tools/imagegrouping.h"
#include "tools/cursors.h"
#include "tools/artlayer.h"
#include "tools/bilineardeform.h"
#include "tools/envelopelayout.h"
#include "tools/selectionopacity.h"
#include "tools/vectorfillreview.h"

// TnzQt includes
#include "toonzqt/selectioncommandids.h"
#include "toonzqt/tselectionhandle.h"
#include "toonzqt/imageutils.h"

// TnzLib includes
#include "toonz/txsheethandle.h"
#include "toonz/txshlevelhandle.h"
#include "toonz/tobjecthandle.h"
#include "toonz/tstageobject.h"
#include "toonz/tdeformer.h"

// TnzBase includes
#include "tenv.h"

// TnzCore includes
#include "drawutil.h"
#include "tcurves.h"

// Qt includes
#include <QApplication>
#include <QKeyEvent>
#include <limits>

using namespace ToolUtils;
using namespace DragSelectionTool;

//********************************************************************************
//    Global variables
//********************************************************************************

namespace {

VectorSelectionTool l_vectorSelectionTool(TTool::Vectors);
TEnv::IntVar l_strokeSelectConstantThickness("SelectionToolConstantThickness",
                                             0);
TEnv::IntVar l_strokeSelectIncludeIntersection(
    "SelectionToolIncludeIntersection", 0);
TEnv::IntVar l_strokeSelectEnvelope("SelectionToolEnvelope", 0);
TEnv::IntVar l_strokeSelectReviewFills("SelectionToolReviewFills", 0);

const int l_dragThreshold = 10;  //!< Distance, in pixels, the user has to
                                 //!  move from a button press to trigger a
                                 //!  selection drag.
}  // namespace

//********************************************************************************
//    Local namespace  stuff
//********************************************************************************

namespace {

// Get four points from vector image based on style IDs
FourPoints getFourPointsFromVectorImage(const TVectorImageP &img,
                                        const std::set<int> &styleIds,
                                        double &maxThickness) {
  FourPoints p;

  if (styleIds.empty()) {
    p = img->getBBox();

    for (UINT i = 0; i < img->getStrokeCount(); i++) {
      TStroke *s = img->getStroke(i);

      for (int j = 0; j < s->getControlPointCount(); j++) {
        double thick = s->getControlPoint(j).thick;
        if (maxThickness < thick) maxThickness = thick;
      }
    }
  } else {
    TRectD bbox;

    for (UINT i = 0; i < img->getStrokeCount(); i++) {
      TStroke *s = img->getStroke(i);
      if (!styleIds.count(s->getStyle())) continue;

      if (bbox.isEmpty())
        bbox = s->getBBox();
      else
        bbox += s->getBBox();

      for (int j = 0; j < s->getControlPointCount(); j++) {
        double thick = s->getControlPoint(j).thick;
        if (maxThickness < thick) maxThickness = thick;
      }
    }

    p = bbox;
  }

  return p;
}

//-----------------------------------------------------------------------------

// Get stroke index from position
bool getStrokeIndexFromPos(UINT &index, const TVectorImageP &vi,
                           const TPointD &pos, double pixelSize, TAffine aff) {
  if (!vi) return false;
  double t, dist2 = 0;
  double maxDist   = 5 * pixelSize;
  double maxDist2  = maxDist * maxDist;
  double checkDist = maxDist2 * 4;

  if (vi->getNearestStroke(pos, t, index, dist2)) {
    TStroke *strokeRef = vi->getStroke(index);
    TThickPoint cursor = strokeRef->getThickPoint(t);
    double len         = cursor.thick * pixelSize * sqrt(aff.det());
    checkDist          = std::max(checkDist, (len * len));
  }

  // Line Art / Colour Art: strokes of the other role can't be picked
  if (dist2 < checkDist && index < vi->getStrokeCount() &&
      !ArtLayer::Filter(*vi).isEditable(index))
    return false;

  return (dist2 < checkDist);
}

//-----------------------------------------------------------------------------

// Check if current frame or not selected
static bool currentOrNotSelected(const VectorSelectionTool &tool,
                                 const TFrameId &fid) {
  return (
      tool.getCurrentFid() == fid ||
      (tool.isSelectedFramesType() && tool.getTargetFrames().count(fid) == 0));
}

//-----------------------------------------------------------------------------

// Notify selection changed
inline void notifySelectionChanged() {
  TTool::getApplication()->getCurrentSelection()->notifySelectionChanged();
}

//-----------------------------------------------------------------------------

// The control points of \b stroke with each chunk cut in the number of equal
// pieces EnvelopeLayout::piecesForChunk() asks for, so that the warped curve
// bends smoothly. The shape and the thickness profile are unchanged.
void subdivideForEnvelope(const TStroke &stroke, double envelopeDiagonal,
                          std::vector<TThickPoint> &points) {
  points.clear();
  const int chunkCount = stroke.getChunkCount();
  if (chunkCount <= 0) return;

  points.push_back(stroke.getChunk(0)->getThickP0());
  for (int c = 0; c < chunkCount; ++c) {
    const TThickQuadratic *chunk = stroke.getChunk(c);
    TThickPoint p0 = chunk->getThickP0(), p1 = chunk->getThickP1(),
                p2 = chunk->getThickP2();

    const double length = tdistance(p0, p1) + tdistance(p1, p2);
    const int pieces = EnvelopeLayout::piecesForChunk(length, envelopeDiagonal);

    // De Casteljau: cut the first of the k remaining pieces off the rest
    for (int k = pieces; k > 1; --k) {
      const double t      = 1.0 / k;
      const TThickPoint a = p0 * (1.0 - t) + p1 * t;
      const TThickPoint b = p1 * (1.0 - t) + p2 * t;
      const TThickPoint m = a * (1.0 - t) + b * t;
      points.push_back(a);
      points.push_back(m);
      p0 = m;
      p1 = b;
    }
    points.push_back(p1);
    points.push_back(p2);
  }
}

}  // namespace

//********************************************************************************
//    VectorFreeDeformer  implementation
//********************************************************************************

VectorFreeDeformer::VectorFreeDeformer(TVectorImageP vi,
                                       std::set<int> strokeIndexes)
    : FreeDeformer()
    , m_vi(vi)
    , m_strokeIndexes(strokeIndexes)
    , m_preserveThickness(false)
    , m_computeRegion(false)
    , m_flip(false) {
  TRectD r;

  std::set<int>::iterator it, iEnd = m_strokeIndexes.end();
  for (it = m_strokeIndexes.begin(); it != iEnd; ++it) {
    TStroke *stroke = m_vi->getStroke(*it);
    r += stroke->getBBox();
    m_originalStrokes.push_back(new TStroke(*stroke));
  }

  m_originalP00 = r.getP00();
  m_originalP11 = r.getP11();
  m_newPoints.push_back(m_originalP00);
  m_newPoints.push_back(r.getP10());
  m_newPoints.push_back(m_originalP11);
  m_newPoints.push_back(r.getP01());
}

//-----------------------------------------------------------------------------

VectorFreeDeformer::~VectorFreeDeformer() {
  clearPointerContainer(m_originalStrokes);
}

//-----------------------------------------------------------------------------

void VectorFreeDeformer::setPreserveThickness(bool preserveThickness) {
  m_preserveThickness = preserveThickness;
}

//-----------------------------------------------------------------------------

void VectorFreeDeformer::setComputeRegion(bool computeRegion) {
  m_computeRegion = computeRegion;
}

//-----------------------------------------------------------------------------

void VectorFreeDeformer::setFlip(bool flip) { m_flip = flip; }

//-----------------------------------------------------------------------------

void VectorFreeDeformer::setPoint(int index, const TPointD &p) {
  m_newPoints[index] = p;
}

//-----------------------------------------------------------------------------

void VectorFreeDeformer::setPoints(const TPointD &p0, const TPointD &p1,
                                   const TPointD &p2, const TPointD &p3) {
  m_newPoints[0] = p0;
  m_newPoints[1] = p1;
  m_newPoints[2] = p2;
  m_newPoints[3] = p3;
}

//-----------------------------------------------------------------------------

void VectorFreeDeformer::setEnvelope(const std::vector<TPointD> &rest,
                                     const std::vector<TPointD> &deformed) {
  // The pieces of the strokes depend on the size of the rest envelope
  if (rest != m_restEnvelope) m_envelopeSources.clear();

  m_restEnvelope     = rest;
  m_deformedEnvelope = deformed;
  m_isEnvelope       = true;
}

//-----------------------------------------------------------------------------

void VectorFreeDeformer::deformRegions() {
  if (m_strokeIndexes.empty() || !m_computeRegion) return;

  std::vector<int> selectedIndexes(m_strokeIndexes.begin(),
                                   m_strokeIndexes.end());

  m_vi->notifyChangedStrokes(selectedIndexes, m_originalStrokes, m_flip);
  m_computeRegion = false;
}

//-----------------------------------------------------------------------------

void VectorFreeDeformer::deformImage() {
  if (m_isEnvelope) {
    deformEnvelopeImage();
    return;
  }

  // debug
  assert(m_strokeIndexes.size() == m_originalStrokes.size());

  // release
  if (m_strokeIndexes.size() != m_originalStrokes.size()) {
    return;
  }

  QMutexLocker lock(m_vi->getMutex());

  std::size_t i = 0;
  for (auto it = m_strokeIndexes.begin(), end = m_strokeIndexes.end();
       it != end; ++it) {
    TStroke *stroke         = m_vi->getStroke(*it);
    TStroke *originalStroke = m_originalStrokes[i++];

    assert(stroke->getControlPointCount() ==
           originalStroke->getControlPointCount());
    for (int j = 0, count = stroke->getControlPointCount(); j < count; ++j) {
      TThickPoint p = deform(originalStroke->getControlPoint(j));
      stroke->setControlPoint(j, p);
    }
  }

  if (m_computeRegion) deformRegions();
}

//-----------------------------------------------------------------------------

TThickPoint VectorFreeDeformer::deform(TThickPoint point) {
  BilinearDeform::Map bilinear(m_originalP00, m_originalP11, m_newPoints[0],
                               m_newPoints[1], m_newPoints[2], m_newPoints[3]);

  TPointD p        = bilinear.map(point);
  double thickness = point.thick;
  // The line gets thicker or thinner as the surface around the original point
  // does. The scale is measured on the original point, not on the deformed one.
  if (!m_preserveThickness) thickness *= bilinear.thicknessFactor(point);
  return TThickPoint(p, thickness);
}

//-----------------------------------------------------------------------------

void VectorFreeDeformer::deformEnvelopeImage() {
  if (m_strokeIndexes.size() != m_originalStrokes.size()) {
    assert(false);
    return;
  }

  // A flat envelope, or one folded over itself, would turn the art inside
  // out: the strokes are left as they are
  if (!EnvelopeLayout::isValidDeformation(m_restEnvelope, m_deformedEnvelope))
    return;

  const double diagonal = EnvelopeLayout::diagonal(m_restEnvelope);

  // Strokes with few control points are cut in shorter pieces once, so that
  // they bend smoothly instead of following only their control points
  if (m_envelopeSources.size() != m_originalStrokes.size()) {
    m_envelopeSources.clear();
    m_envelopeSources.resize(m_originalStrokes.size());
    for (std::size_t i = 0; i < m_originalStrokes.size(); ++i)
      subdivideForEnvelope(*m_originalStrokes[i], diagonal,
                           m_envelopeSources[i]);
  }

  // Mean value coordinates of the rest envelope, applied to the moved one.
  // The art on the outline follows the outline exactly.
  const std::vector<TPointD> &rest     = m_restEnvelope;
  const std::vector<TPointD> &deformed = m_deformedEnvelope;
  auto map = [&rest, &deformed](const TPointD &p) -> TPointD {
    TPointD q;
    if (TEnvelopeDeformer::mapThrough(rest, deformed, p, q) &&
        EnvelopeLayout::isFinitePoint(q))
      return q;
    return p;
  };
  const double step = EnvelopeLayout::kJacobianStepFraction * diagonal;

  QMutexLocker lock(m_vi->getMutex());

  std::vector<TThickPoint> points;
  std::size_t i = 0;
  for (auto it = m_strokeIndexes.begin(), end = m_strokeIndexes.end();
       it != end; ++it, ++i) {
    const std::vector<TThickPoint> &source = m_envelopeSources[i];
    TStroke *stroke                        = m_vi->getStroke(*it);
    if (!stroke || source.size() < 3) continue;

    points = source;
    for (TThickPoint &point : points) {
      const TPointD original(point.x, point.y);
      double thickness = point.thick;
      // The line gets thicker or thinner as the surface around the original
      // point does (the estimate is finite and clamped)
      if (!m_preserveThickness)
        thickness *= EnvelopeLayout::thicknessFactor(map, original, step);
      point = TThickPoint(map(original), thickness);
    }
    stroke->reshape(points.data(), (int)points.size());
  }

  if (m_computeRegion) deformRegions();
}

//********************************************************************************
//    UndoChangeStrokes  implementation
//********************************************************************************

DragSelectionTool::UndoChangeStrokes::UndoChangeStrokes(
    TXshSimpleLevel *level, const TFrameId &frameId, VectorSelectionTool *tool,
    const StrokeSelection &selection)
    : ToolUtils::TToolUndo(level, frameId)
    , m_tool(tool)
    , m_selectionCount(tool->getSelectionCount())  // Not related to selection
    , m_oldBBox(tool->getBBox())
    , m_oldCenter(tool->getCenter())
    , m_oldDeformValues(tool->m_deformValues)
    , m_newDeformValues()
    , m_flip(false) {
  TVectorImageP vi = m_level->getFrame(m_frameId, false);
  if (!vi) {
    assert(vi);
    return;
  }

  const StrokeSelection::IndexesContainer &indexes = selection.getSelection();
  m_indexes.assign(indexes.begin(), indexes.end());

  registerStrokes(true);
}

//-----------------------------------------------------------------------------

DragSelectionTool::UndoChangeStrokes::UndoChangeStrokes(
    TXshSimpleLevel *level, const TFrameId &frameId, VectorSelectionTool *tool,
    const LevelSelection &selection)
    : ToolUtils::TToolUndo(level, frameId)
    , m_tool(tool)
    , m_selectionCount(tool->getSelectionCount())  // Not related to selection
    , m_oldBBox(tool->getBBox())
    , m_oldCenter(tool->getCenter())
    , m_oldDeformValues(tool->m_deformValues)
    , m_newDeformValues()
    , m_flip(false) {
  TVectorImageP vi = m_level->getFrame(m_frameId, false);
  if (!vi) {
    assert(vi);
    return;
  }

  m_indexes = getSelectedStrokes(*vi, selection);

  registerStrokes(true);
}

//-----------------------------------------------------------------------------

DragSelectionTool::UndoChangeStrokes::~UndoChangeStrokes() {
  clearPointerContainer(m_oldStrokes);
  clearPointerContainer(m_newStrokes);
}

//-----------------------------------------------------------------------------

void DragSelectionTool::UndoChangeStrokes::registerStrokes(bool beforeModify) {
  TVectorImageP vi = m_level->getFrame(m_frameId, false);
  if (!vi) {
    assert(vi);
    return;
  }

  std::vector<TStroke *> &strokes = beforeModify ? m_oldStrokes : m_newStrokes;
  clearPointerContainer(strokes);
  if (beforeModify && m_tool->reviewFills()) m_beforeImage = vi->clone();

  TRectD bbox;

  int s, sCount = int(m_indexes.size());
  for (s = 0; s != sCount; ++s) {
    TStroke *stroke = vi->getStroke(m_indexes[s]);
    bbox += stroke->getBBox();

    strokes.push_back(new TStroke(*stroke));
  }

  auto &fills = beforeModify ? m_regionsData : m_newRegionsData;
  fills.clear();
  // Capture every region: opening a boundary can affect a containing region
  // well beyond the selected strokes' bounding box.
  if (!vi->getBBox().isEmpty())
    ImageUtils::getFillingInformationOverlappingArea(vi, fills, vi->getBBox());
  if (!beforeModify) {
    m_newBBox         = m_tool->getBBox();
    m_newCenter       = m_tool->getCenter();
    m_newDeformValues = m_tool->m_deformValues;
  }
}

//-----------------------------------------------------------------------------

void DragSelectionTool::UndoChangeStrokes::transform(
    const std::vector<TStroke *> &strokes, FourPoints bbox, TPointD center,
    DragSelectionTool::DeformValues deformValues, bool notify) const {
  TVectorImageP image = m_level->getFrame(m_frameId, true);
  if (!image) {
    assert(image);
    return;
  }

  int s, sCount = int(m_indexes.size());
  for (s = 0; s != sCount; ++s) {
    int index = m_indexes[s];

    TStroke *sourcesStroke = strokes[s], *stroke = image->getStroke(index);

    int cp, cpCount = stroke->getControlPointCount();
    if (cpCount == sourcesStroke->getControlPointCount()) {
      for (cp = 0; cp != cpCount; ++cp)
        stroke->setControlPoint(cp, sourcesStroke->getControlPoint(cp));
    } else {
      // The Envelope mode adds control points: restore the whole stroke
      std::vector<TThickPoint> points;
      sourcesStroke->getControlPoints(points);
      stroke->reshape(points.data(), (int)points.size());
    }
  }

  image->notifyChangedStrokes(m_indexes, strokes, m_flip);

  if (!m_tool->isSelectionEmpty() &&
      m_selectionCount == m_tool->getSelectionCount()) {
    m_tool->setBBox(bbox);
    m_tool->setCenter(center);
  } else
    m_tool->computeBBox();

  m_tool->m_deformValues = deformValues;
  if (notify) {
    m_tool->notifyImageChanged();
    TTool::getApplication()->getCurrentXsheet()->notifyXsheetChanged();
    TTool::getApplication()->getCurrentTool()->notifyToolChanged();
  }
}

//-----------------------------------------------------------------------------

void DragSelectionTool::UndoChangeStrokes::restoreRegions() const {
  TVectorImageP vi = m_level->getFrame(m_frameId, true);
  if (!vi) {
    assert(vi);
    return;
  }

  ImageUtils::assignFillingInformation(*vi, m_regionsData);
}

//-----------------------------------------------------------------------------

void DragSelectionTool::UndoChangeStrokes::undo() const {
  transform(m_oldStrokes, m_oldBBox, m_oldCenter, m_oldDeformValues);
  restoreRegions();
  m_tool->notifyImageChanged();
}

//-----------------------------------------------------------------------------

void DragSelectionTool::UndoChangeStrokes::redo() const {
  transform(m_newStrokes, m_newBBox, m_newCenter, m_newDeformValues);
  TVectorImageP image = currentImage();
  if (image) ImageUtils::assignFillingInformation(*image, m_newRegionsData);
  m_tool->notifyImageChanged();
}

TVectorImageP DragSelectionTool::UndoChangeStrokes::currentImage() const {
  return m_level->getFrame(m_frameId, false);
}

void DragSelectionTool::UndoChangeStrokes::restoreBefore() const {
  transform(m_oldStrokes, m_oldBBox, m_oldCenter, m_oldDeformValues, false);
  restoreRegions();
}

//-----------------------------------------------------------------------------

int DragSelectionTool::UndoChangeStrokes::getSize() const {
  size_t size = sizeof(*this) + m_indexes.size() * sizeof(int) +
      (m_regionsData.size() + m_newRegionsData.size()) * sizeof(TFilledRegionInf);
  for (const auto *stroke : m_oldStrokes)
    size += sizeof(TStroke) + stroke->getControlPointCount() * sizeof(TThickPoint);
  for (const auto *stroke : m_newStrokes)
    size += sizeof(TStroke) + stroke->getControlPointCount() * sizeof(TThickPoint);
  return int(std::min(size, size_t(std::numeric_limits<int>::max())));
}

//=============================================================================
// UndoChangeOutlineStyle
//-----------------------------------------------------------------------------

class UndoChangeOutlineStyle final : public ToolUtils::TToolUndo {
  std::vector<TStroke::OutlineOptions> m_oldOptions, m_newOptions;
  FourPoints m_oldBBox, m_newBBox;
  VectorSelectionTool *m_tool;
  std::vector<int> m_indexes;
  int m_selectionCount;

public:
  UndoChangeOutlineStyle(TXshSimpleLevel *level, const TFrameId &frameId,
                         VectorSelectionTool *tool);
  ~UndoChangeOutlineStyle() {}
  void registerStrokes(bool beforeModify = false);
  void transform(const std::vector<TStroke::OutlineOptions> &options,
                 FourPoints bbox) const;
  void undo() const override;
  void redo() const override;
  int getSize() const override;
};

//-----------------------------------------------------------------------------

UndoChangeOutlineStyle::UndoChangeOutlineStyle(TXshSimpleLevel *level,
                                               const TFrameId &frameId,
                                               VectorSelectionTool *tool)
    : ToolUtils::TToolUndo(level, frameId)
    , m_tool(tool)
    , m_oldBBox(tool->getBBox())
    , m_selectionCount(tool->getSelectionCount()) {
  TVectorImageP image = m_level->getFrame(m_frameId, false);
  assert(!!image);
  if (!image) return;
  StrokeSelection *strokeSelection =
      dynamic_cast<StrokeSelection *>(tool->getSelection());
  int i;
  for (i = 0; i < (int)image->getStrokeCount(); i++) {
    if (!strokeSelection->isSelected(i) && !m_tool->isLevelType() &&
        !m_tool->isSelectedFramesType())
      continue;
    m_indexes.push_back(i);
  }
  registerStrokes(true);
}

//-----------------------------------------------------------------------------

void UndoChangeOutlineStyle::registerStrokes(bool beforeModify) {
  TVectorImageP image = m_level->getFrame(m_frameId, false);
  assert(!!image);
  if (!image) return;
  int i;
  for (i = 0; i < (int)m_indexes.size(); i++) {
    if (beforeModify)
      m_oldOptions.push_back(image->getStroke(m_indexes[i])->outlineOptions());
    else
      m_newOptions.push_back(image->getStroke(m_indexes[i])->outlineOptions());
  }
  if (!beforeModify) m_newBBox = m_tool->getBBox();
}

//-----------------------------------------------------------------------------

void UndoChangeOutlineStyle::transform(
    const std::vector<TStroke::OutlineOptions> &options,
    FourPoints bbox) const {
  TVectorImageP image = m_level->getFrame(m_frameId, true);
  assert(!!image);
  if (!image) return;
  int i;
  for (i = 0; i < (int)m_indexes.size(); i++) {
    int index                                 = m_indexes[i];
    image->getStroke(index)->outlineOptions() = options[i];
  }
  if (!m_tool->isSelectionEmpty() &&
      m_selectionCount == m_tool->getSelectionCount())
    m_tool->setBBox(bbox);
  else
    m_tool->computeBBox();
  m_tool->notifyImageChanged();
  TTool::getApplication()->getCurrentXsheet()->notifyXsheetChanged();
  TTool::getApplication()->getCurrentTool()->notifyToolChanged();
}

//-----------------------------------------------------------------------------

void UndoChangeOutlineStyle::undo() const {
  transform(m_oldOptions, m_oldBBox);
  TTool::getApplication()->getCurrentTool()->notifyToolChanged();
  m_tool->notifyImageChanged();
}

//-----------------------------------------------------------------------------

void UndoChangeOutlineStyle::redo() const {
  transform(m_newOptions, m_newBBox);
  TTool::getApplication()->getCurrentTool()->notifyToolChanged();
  m_tool->notifyImageChanged();
}

//-----------------------------------------------------------------------------

int UndoChangeOutlineStyle::getSize() const {
  // NOTE: This is definitely wrong... sizeof(vector) DOES NOT correspond to its
  // actual size - as it is internally allocated with new!!
  return sizeof(*this) + sizeof(*m_tool);
}

//=============================================================================
// VectorDeformTool::VFDScopedBlock
//-----------------------------------------------------------------------------

struct VectorDeformTool::VFDScopedBlock  //! Type bounding the scope of free
                                         //! deformers in the tool.
{
  VFDScopedBlock(SelectionTool *tool) : m_tool(tool) {
    m_tool->setNewFreeDeformer();
  }
  ~VFDScopedBlock() { m_tool->clearDeformers(); }

private:
  SelectionTool *m_tool;  //!< [\p external]  Tool owning the deformers.
};

//=============================================================================
// VectorDeformTool
//-----------------------------------------------------------------------------

DragSelectionTool::VectorDeformTool::VectorDeformTool(VectorSelectionTool *tool)
    : DeformTool(tool), m_undo() {
  if (!TTool::getApplication()->getCurrentObject()->isSpline()) {
    m_undo.reset(new UndoChangeStrokes(
        TTool::getApplication()->getCurrentLevel()->getSimpleLevel(),
        tool->getCurrentFid(), tool, tool->strokeSelection()));
  }
}

//-----------------------------------------------------------------------------

DragSelectionTool::VectorDeformTool::~VectorDeformTool() {
  // DO NOT REMOVE - DESTRUCTS TYPES INCOMPLETE IN THE HEADER
  cancel();
}

void DragSelectionTool::VectorDeformTool::cancel() {
  if (m_isReviewing) m_cancelled = true;
  if (m_undo) {
    m_undo->restoreBefore();
    m_undo.reset();
    m_cancelled = true;
    m_tool->invalidate();
  }
  m_isDragging = false;
}

//-----------------------------------------------------------------------------

void DragSelectionTool::VectorDeformTool::applyTransform(FourPoints bbox,
                                                         bool onFastDragging) {
  SelectionTool *tool = getTool();

  std::unique_ptr<VFDScopedBlock> localVfdScopedBlock;
  if (!m_vfdScopedBlock) {
    std::unique_ptr<VFDScopedBlock> &vfdScopedBlock =
        m_isDragging ? m_vfdScopedBlock : localVfdScopedBlock;

    vfdScopedBlock.reset(new VFDScopedBlock(tool));
  }

  VectorFreeDeformer *freeDeformer =
      static_cast<VectorFreeDeformer *>(tool->getFreeDeformer());

  const bool stayedTheSame = bbox.getP00() == freeDeformer->getPoint(0) &&
                             bbox.getP10() == freeDeformer->getPoint(1) &&
                             bbox.getP11() == freeDeformer->getPoint(2) &&
                             bbox.getP01() == freeDeformer->getPoint(3);

  setDeformerShape(freeDeformer, 0, bbox);
  freeDeformer->setComputeRegion(!m_isDragging);
  freeDeformer->setPreserveThickness(tool->isConstantThickness());
  freeDeformer->setFlip(isFlip());

  if (!TTool::getApplication()->getCurrentObject()->isSpline() && m_undo)
    m_undo->setFlip(isFlip());

  freeDeformer->deformImage();

  tool->invalidate();

  if (!stayedTheSame) tool->m_deformValues.m_isSelectionModified = true;

  if (!m_isDragging && (tool->isLevelType() || tool->isSelectedFramesType()))
    transformWholeLevel();
}

//-----------------------------------------------------------------------------

void DragSelectionTool::VectorDeformTool::addTransformUndo() {
  if (TTool::getApplication()->getCurrentObject()->isSpline())
    TUndoManager::manager()->add(
        new UndoPath(getTool()
                         ->getXsheet()
                         ->getStageObject(getTool()->getObjectId())
                         ->getSpline()));
  else if (m_undo) {
    m_undo->registerStrokes();
    std::vector<std::unique_ptr<UndoChangeStrokes>> changes;
    changes.push_back(std::move(m_undo));
    commitChanges(changes);
  }
}

bool DragSelectionTool::VectorDeformTool::commitChanges(
    std::vector<std::unique_ptr<UndoChangeStrokes>> &changes) {
  std::vector<VectorFillReview::Entry> entries;
  for (const auto &change : changes)
    if (change->beforeImage())
      entries.emplace_back(QString::fromStdString(change->frameId().expand()),
                           change->beforeImage(), change->currentImage());
  m_isReviewing = !entries.empty();
  const bool accepted = VectorFillReview::review(entries);
  m_isReviewing = false;
  if (!accepted || m_cancelled) {
    for (auto it = changes.rbegin(); it != changes.rend(); ++it)
      (*it)->restoreBefore();
    changes.clear();
    m_cancelled = true;
    m_tool->invalidate();
    return false;
  }
  TUndoManager::manager()->beginBlock();
  for (auto &change : changes) {
    const TFrameId fid = change->frameId();
    change->discardReviewImage();
    TUndoManager::manager()->add(change.release());
    m_tool->notifyImageChanged(fid);
  }
  TUndoManager::manager()->endBlock();
  return true;
}

//-----------------------------------------------------------------------------

void DragSelectionTool::VectorDeformTool::transformWholeLevel() {
  VectorSelectionTool *tool = dynamic_cast<VectorSelectionTool *>(m_tool);
  assert(tool);

  assert(!tool->levelSelection().isEmpty());

  TXshSimpleLevel *level =
      TTool::getApplication()->getCurrentLevel()->getSimpleLevel();

  std::vector<TFrameId> fids;
  level->getFids(fids);

  // Remove unwanted fids
  fids.erase(std::remove_if(fids.begin(), fids.end(),
                            [tool](const TFrameId &fid) {
                              return currentOrNotSelected(*tool, fid);
                            }),
             fids.end());

  // Stage every drawing before opening the review. No undo entry or dirty
  // notification is emitted until all drawings are accepted together.
  std::vector<std::unique_ptr<UndoChangeStrokes>> changes;
  if (m_undo) {
    m_undo->registerStrokes();
    changes.push_back(std::move(m_undo));
  }
  for (int f = 0; f < int(fids.size()); ++f) {
    const TFrameId &fid = fids[f];
    const int t = f + 1;
    TVectorImageP vi = level->getFrame(fid, true);
    if (!vi) continue;
    std::unique_ptr<UndoChangeStrokes> undo(
        new UndoChangeStrokes(level, fid, tool, tool->levelSelection()));
    const FourPoints bbox = tool->getBBox(t);
    auto *freeDeformer =
        static_cast<VectorFreeDeformer *>(tool->getFreeDeformer(t));
    setDeformerShape(freeDeformer, t, bbox);
    freeDeformer->setComputeRegion(true);
    freeDeformer->setPreserveThickness(tool->isConstantThickness());
    freeDeformer->setFlip(isFlip());
    freeDeformer->deformImage();
    undo->setFlip(isFlip());
    undo->registerStrokes();
    changes.push_back(std::move(undo));
  }
  commitChanges(changes);
}

//-----------------------------------------------------------------------------

void DragSelectionTool::VectorDeformTool::setDeformerShape(
    VectorFreeDeformer *deformer, int index, const FourPoints &bbox) {
  deformer->setPoints(bbox.getP00(), bbox.getP10(), bbox.getP11(),
                      bbox.getP01());
}

//-----------------------------------------------------------------------------

bool DragSelectionTool::VectorDeformTool::isFlip() {
  TPointD scaleValue = getTool()->m_deformValues.m_scaleValue;
  return m_startScaleValue.x * scaleValue.x < 0 ||
         m_startScaleValue.y * scaleValue.y < 0;
}

//-----------------------------------------------------------------------------

void DragSelectionTool::VectorDeformTool::leftButtonUp(const TPointD &pos,
                                                       const TMouseEvent &e) {
  std::unique_ptr<VFDScopedBlock> vfdScopedBlock(std::move(m_vfdScopedBlock));

  SelectionTool *tool = getTool();
  VectorFreeDeformer *deformer =
      dynamic_cast<VectorFreeDeformer *>(tool->getFreeDeformer());
  if (!deformer) return;

  deformer->setComputeRegion(true);
  deformer->setFlip(isFlip());
  deformer->deformRegions();

  if (!tool->isLevelType() && !tool->isSelectedFramesType())
    addTransformUndo();
  else
    transformWholeLevel();

  m_isDragging = false;

  if (!m_cancelled) tool->notifyImageChanged();

  VectorSelectionTool *selectionTool =
      dynamic_cast<VectorSelectionTool *>(m_tool);
  selectionTool->setResetCenter(true);
}

//=============================================================================
// VectorRotationTool
//-----------------------------------------------------------------------------

DragSelectionTool::VectorRotationTool::VectorRotationTool(
    VectorSelectionTool *tool)
    : VectorDeformTool(tool), m_rotation(new Rotation(this)) {}

//-----------------------------------------------------------------------------

void DragSelectionTool::VectorRotationTool::transform(TAffine aff,
                                                      double angle) {
  SelectionTool *tool = getTool();
  FourPoints newBbox(tool->getBBox() * aff);
  TPointD center = m_rotation->getStartCenter();
  int i;
  for (i = 0; i < tool->getBBoxsCount(); i++) {
    aff = TRotation(center, angle);
    tool->setBBox(tool->getBBox(i) * aff, i);
  }
  applyTransform(newBbox);
}

//-----------------------------------------------------------------------------

void DragSelectionTool::VectorRotationTool::leftButtonDrag(
    const TPointD &pos, const TMouseEvent &e) {
  VectorSelectionTool *tool = dynamic_cast<VectorSelectionTool *>(m_tool);
  tool->setResetCenter(false);
  m_rotation->leftButtonDrag(pos, e);
}

//-----------------------------------------------------------------------------

void DragSelectionTool::VectorRotationTool::draw() { m_rotation->draw(); }

//=============================================================================
// VectorFreeDeformTool
//-----------------------------------------------------------------------------

DragSelectionTool::VectorFreeDeformTool::VectorFreeDeformTool(
    VectorSelectionTool *tool)
    : VectorDeformTool(tool), m_freeDeform(new FreeDeform(this)) {}

//-----------------------------------------------------------------------------

void DragSelectionTool::VectorFreeDeformTool::leftButtonDrag(
    const TPointD &pos, const TMouseEvent &e) {
  VectorSelectionTool *tool = dynamic_cast<VectorSelectionTool *>(m_tool);
  tool->setResetCenter(false);
  m_freeDeform->leftButtonDrag(pos, e);
}

//=============================================================================
// VectorEnvelopeTool
//-----------------------------------------------------------------------------

DragSelectionTool::VectorEnvelopeTool::VectorEnvelopeTool(
    VectorSelectionTool *tool)
    : VectorDeformTool(tool) {
  // The envelopes at the press: the handles on the outline of each box
  tool->clearEnvelopes();
  for (int i = 0; i < tool->getBBoxsCount(); ++i)
    m_restEnvelopes.push_back(tool->getEnvelope(i));
}

//-----------------------------------------------------------------------------

DragSelectionTool::VectorEnvelopeTool::~VectorEnvelopeTool() {
  getTool()->clearEnvelopes();
}

//-----------------------------------------------------------------------------

void DragSelectionTool::VectorEnvelopeTool::setDeformerShape(
    VectorFreeDeformer *deformer, int index, const FourPoints &bbox) {
  VectorDeformTool::setDeformerShape(deformer, index, bbox);
  if (index < 0 || index >= (int)m_restEnvelopes.size()) return;

  deformer->setEnvelope(m_restEnvelopes[index], getTool()->getEnvelope(index));
}

//-----------------------------------------------------------------------------

void DragSelectionTool::VectorEnvelopeTool::leftButtonDrag(
    const TPointD &pos, const TMouseEvent &e) {
  VectorSelectionTool *tool = dynamic_cast<VectorSelectionTool *>(m_tool);
  if (!tool || m_restEnvelopes.empty()) return;
  tool->setResetCenter(false);

  const int handle =
      EnvelopeLayout::handleFromToolPoint(tool->getSelectedPoint());
  if (handle == EnvelopeLayout::kNoHandle) return;

  double pixelSize = tool->getPixelSize();
  bool isFastDragging =
      tdistance2(pos, getCurPos()) > 9.0 * pixelSize * pixelSize;

  // The handle moves by the same distance in the box of every frame the tool
  // edits (the current frame's box is the first one). A handle stops where its
  // envelope would cross itself or turn inside out.
  const TPointD offset = pos - getStartPos();
  int count            = (tool->isLevelType() || tool->isSelectedFramesType())
                             ? (int)m_restEnvelopes.size()
                             : 1;
  count                = std::min(count, tool->getBBoxsCount());

  for (int i = 0; i < count; ++i) {
    const std::vector<TPointD> &rest = m_restEnvelopes[i];
    if (handle >= (int)rest.size() || !EnvelopeLayout::isUsableRest(rest))
      continue;

    std::vector<TPointD> envelope = tool->getEnvelope(i);
    if (envelope.size() != rest.size()) continue;

    envelope[handle] = EnvelopeLayout::constrainMove(rest, envelope, handle,
                                                     rest[handle] + offset);
    tool->setEnvelope(envelope, i);

    // The box follows the corners of the envelope
    FourPoints bbox = tool->getBBox(i);
    for (int corner = 0; corner < 4; ++corner)
      bbox.setPoint(corner,
                    envelope[EnvelopeLayout::handleFromToolPoint(corner)]);
    tool->setBBox(bbox, i);
  }
  if (norm2(offset) > 0.0) tool->m_deformValues.m_isSelectionModified = true;

  setCurPos(pos);
  applyTransform(tool->getBBox(), isFastDragging);
}

//-----------------------------------------------------------------------------

void DragSelectionTool::VectorEnvelopeTool::leftButtonUp(const TPointD &pos,
                                                         const TMouseEvent &e) {
  // The other frames are warped here, with the envelopes still in place
  VectorDeformTool::leftButtonUp(pos, e);
  getTool()->clearEnvelopes();
}

//=============================================================================
// VectorMoveSelectionTool
//-----------------------------------------------------------------------------

DragSelectionTool::VectorMoveSelectionTool::VectorMoveSelectionTool(
    VectorSelectionTool *tool)
    : VectorDeformTool(tool), m_moveSelection(new MoveSelection(this)) {}

//-----------------------------------------------------------------------------

void DragSelectionTool::VectorMoveSelectionTool::transform(TAffine aff) {
  SelectionTool *tool = getTool();
  int i;
  for (i = 0; i < (int)tool->getBBoxsCount(); i++)
    tool->setBBox(tool->getBBox(i) * aff, i);
  getTool()->setCenter(aff * tool->getCenter());
  applyTransform(tool->getBBox());
}

//-----------------------------------------------------------------------------

void DragSelectionTool::VectorMoveSelectionTool::leftButtonDown(
    const TPointD &pos, const TMouseEvent &e) {
  m_moveSelection->leftButtonDown(pos, e);
  VectorDeformTool::leftButtonDown(pos, e);
}

//-----------------------------------------------------------------------------

void DragSelectionTool::VectorMoveSelectionTool::leftButtonDrag(
    const TPointD &pos, const TMouseEvent &e) {
  VectorSelectionTool *tool = dynamic_cast<VectorSelectionTool *>(m_tool);
  tool->setResetCenter(false);
  if (e.isCtrlPressed() ||
      norm2(pos - getStartPos()) > l_dragThreshold * getTool()->getPixelSize())
    m_moveSelection->leftButtonDrag(pos, e);
  else  // snap to the original position
    m_moveSelection->leftButtonDrag(getStartPos(), e);
}

//=============================================================================
// VectorScaleTool
//-----------------------------------------------------------------------------

DragSelectionTool::VectorScaleTool::VectorScaleTool(VectorSelectionTool *tool,
                                                    ScaleType type)
    : VectorDeformTool(tool), m_scale(new Scale(this, type)) {}

//-----------------------------------------------------------------------------

TPointD DragSelectionTool::VectorScaleTool::transform(int index, TPointD newPos,
                                                      bool onFastDragging) {
  SelectionTool *tool = getTool();
  TPointD scaleValue  = tool->m_deformValues.m_scaleValue;

  std::vector<FourPoints> startBboxs = m_scale->getStartBboxs();
  TPointD center                     = m_scale->getStartCenter();
  FourPoints bbox = m_scale->bboxScaleInCenter(index, startBboxs[0], newPos,
                                               scaleValue, center, true);
  if (bbox == startBboxs[0]) return scaleValue;

  bool scaleInCenter = m_scale->scaleInCenter();
  // If I didn't scale relative to center, calculate the position of the new
  // center
  if (!scaleInCenter)
    tool->setCenter(m_scale->getNewCenter(index, startBboxs[0], scaleValue));

  if (tool->isLevelType() || tool->isSelectedFramesType()) {
    int i;
    for (i = 1; i < (int)tool->getBBoxsCount(); i++) {
      FourPoints oldBbox = startBboxs[i];
      TPointD frameCenter =
          scaleInCenter ? center
                        : oldBbox.getPoint(getSymmetricPointIndex(index));
      TPointD newp =
          m_scale->getScaledPoint(index, oldBbox, scaleValue, frameCenter);
      FourPoints newBbox = m_scale->bboxScaleInCenter(
          index, oldBbox, newp, scaleValue, frameCenter, false);
      tool->setBBox(newBbox, i);
      if (!scaleInCenter)
        tool->setCenter(m_scale->getNewCenter(index, oldBbox, scaleValue), i);
    }
  }
  tool->setBBox(bbox);
  applyTransform(bbox);
  return scaleValue;
}

//-----------------------------------------------------------------------------

void DragSelectionTool::VectorScaleTool::leftButtonDown(const TPointD &pos,
                                                        const TMouseEvent &e) {
  m_scale->leftButtonDown(pos, e);
  VectorDeformTool::leftButtonDown(pos, e);
}

//-----------------------------------------------------------------------------

void DragSelectionTool::VectorScaleTool::leftButtonDrag(const TPointD &pos,
                                                        const TMouseEvent &e) {
  VectorSelectionTool *tool = dynamic_cast<VectorSelectionTool *>(m_tool);
  tool->setResetCenter(false);
  m_scale->leftButtonDrag(pos, e);
}

//=============================================================================
// VectorChangeThicknessTool
//-----------------------------------------------------------------------------

DragSelectionTool::VectorChangeThicknessTool::VectorChangeThicknessTool(
    VectorSelectionTool *tool)
    : DragTool(tool), m_curPos(), m_firstPos(), m_thicknessChange(0) {
  TVectorImageP vi = tool->getImage(false);
  assert(vi);

  setStrokesThickness(*vi);

  TXshSimpleLevel *level =
      TTool::getApplication()->getCurrentLevel()->getSimpleLevel();
  m_undo.reset(new UndoChangeStrokes(level, tool->getCurrentFid(), tool,
                                     tool->strokeSelection()));
}

//-----------------------------------------------------------------------------

VectorChangeThicknessTool::~VectorChangeThicknessTool() {}

//-----------------------------------------------------------------------------

namespace {
namespace SetStrokeThickness {

using namespace DragSelectionTool;

struct Data {
  VectorChangeThicknessTool &m_tool;
  const TVectorImage &m_vi;
};
}  // namespace SetStrokeThickness
}  // namespace

void DragSelectionTool::VectorChangeThicknessTool::setStrokesThickness(
    TVectorImage &vi) {
  struct locals {
    static void setThickness(const SetStrokeThickness::Data &data, int s) {
      const TStroke &stroke = *data.m_vi.getStroke(s);

      std::vector<double> strokeThickness;

      int cp, cpCount = stroke.getControlPointCount();
      strokeThickness.reserve(cpCount);

      for (cp = 0; cp != cpCount; ++cp)
        strokeThickness.push_back(stroke.getControlPoint(cp).thick);

      data.m_tool.m_strokesThickness[s] = strokeThickness;
    }

  };  // locals

  SetStrokeThickness::Data data = {*this, vi};

  VectorSelectionTool *vsTool = static_cast<VectorSelectionTool *>(m_tool);
  const LevelSelection &levelSelection = vsTool->levelSelection();

  if (levelSelection.isEmpty()) {
    StrokeSelection *strokeSelection =
        static_cast<StrokeSelection *>(m_tool->getSelection());
    const std::set<int> &selectedStrokeIdxs = strokeSelection->getSelection();

    std::for_each(selectedStrokeIdxs.begin(), selectedStrokeIdxs.end(),
                  [&data](int s) { locals::setThickness(data, s); });
  } else {
    std::vector<int> strokeIdxs = getSelectedStrokes(vi, levelSelection);

    std::for_each(strokeIdxs.begin(), strokeIdxs.end(),
                  [&data](int s) { locals::setThickness(data, s); });
  }
}

//-----------------------------------------------------------------------------

namespace {
namespace ChangeImageThickness {

using namespace DragSelectionTool;

struct Data {
  VectorChangeThicknessTool &m_tool;
  TVectorImage &m_vi;
  double m_newThickness;
};
}  // namespace ChangeImageThickness
}  // namespace

void DragSelectionTool::VectorChangeThicknessTool::changeImageThickness(
    TVectorImage &vi, double newThickness) {
  struct locals {
    static void changeThickness(const ChangeImageThickness::Data &data, int s) {
      TStroke &stroke = *data.m_vi.getStroke(s);

      for (int cp = 0; cp < (int)stroke.getControlPointCount(); ++cp) {
        double thickness =
            tcrop(data.m_tool.m_strokesThickness[s][cp] + data.m_newThickness,
                  0.0, 255.0);

        TThickPoint point(TPointD(stroke.getControlPoint(cp)), thickness);

        stroke.setControlPoint(cp, point);
      }
    }

  };  // locals
  m_isIncrementalMode             = true;
  ChangeImageThickness::Data data = {*this, vi, newThickness};

  VectorSelectionTool *vsTool = static_cast<VectorSelectionTool *>(getTool());
  const LevelSelection &levelSelection = vsTool->levelSelection();

  if (levelSelection.isEmpty()) {
    StrokeSelection *strokeSelection =
        static_cast<StrokeSelection *>(m_tool->getSelection());
    const std::set<int> &selectedStrokeIdxs = strokeSelection->getSelection();

    std::for_each(selectedStrokeIdxs.begin(), selectedStrokeIdxs.end(),
                  [&data](int s) { locals::changeThickness(data, s); });
  } else {
    std::vector<int> strokeIdxs = getSelectedStrokes(vi, levelSelection);

    std::for_each(strokeIdxs.begin(), strokeIdxs.end(),
                  [&data](int s) { locals::changeThickness(data, s); });
  }
}

void DragSelectionTool::VectorChangeThicknessTool::setImageThickness(
    TVectorImage &vi, double newFixedThickness) {
  m_isIncrementalMode = false;
  newFixedThickness   = tcrop(newFixedThickness, 0.0, 255.0);

  VectorSelectionTool *vsTool = static_cast<VectorSelectionTool *>(getTool());
  const LevelSelection &levelSelection = vsTool->levelSelection();

  std::vector<int> strokeIdxs;

  if (levelSelection.isEmpty()) {
    StrokeSelection *strokeSelection =
        static_cast<StrokeSelection *>(m_tool->getSelection());
    const std::set<int> &selectedStrokeIdxs = strokeSelection->getSelection();
    strokeIdxs.assign(selectedStrokeIdxs.begin(), selectedStrokeIdxs.end());
  } else {
    strokeIdxs = getSelectedStrokes(vi, levelSelection);
  }

  std::for_each(strokeIdxs.begin(), strokeIdxs.end(),
                [&newFixedThickness, &vi](int s) {
                  TStroke &stroke = *vi.getStroke(s);
                  for (int cp = 0; cp < stroke.getControlPointCount(); ++cp) {
                    stroke.setControlPoint(
                        cp, TThickPoint(TPointD(stroke.getControlPoint(cp)),
                                        newFixedThickness));
                  }
                });
}

//-----------------------------------------------------------------------------

void DragSelectionTool::VectorChangeThicknessTool::addUndo() {
  TVectorImageP curVi = getTool()->getImage(true);
  if (!curVi) return;

  m_undo->registerStrokes();

  SelectionTool *tool = getTool();
  if (tool->isLevelType() || tool->isSelectedFramesType()) {
    VectorSelectionTool *vtool = dynamic_cast<VectorSelectionTool *>(tool);
    assert(vtool && !vtool->levelSelection().isEmpty());

    TXshSimpleLevel *level =
        TTool::getApplication()->getCurrentLevel()->getSimpleLevel();

    // Retrieve frames available in the level
    std::vector<TFrameId> fids;
    level->getFids(fids);

    // Remove unwanted frames
    fids.erase(std::remove_if(fids.begin(), fids.end(),
                              [vtool](const TFrameId &fid) {
                                return currentOrNotSelected(*vtool, fid);
                              }),
               fids.end());

    TUndoManager::manager()->beginBlock();
    {
      // Current frame added separately
      TUndoManager::manager()->add(m_undo.release());  // Inside an undo block

      // Iterate remaining ones
      int f, fCount = int(fids.size());
      for (f = 0; f != fCount; ++f) {
        const TFrameId &fid = fids[f];

        TVectorImageP vi = level->getFrame(fid, true);
        if (!vi) {
          assert(vi);
          continue;
        }

        // Transform fid's selection
        std::unique_ptr<UndoChangeStrokes> undo(
            new UndoChangeStrokes(level, fid, vtool, vtool->levelSelection()));
        if (m_isIncrementalMode) {
          setStrokesThickness(*vi);
          changeImageThickness(*vi, m_thicknessChange);
        } else
          setImageThickness(*vi, m_thicknessChange);

        m_strokesThickness.clear();
        undo->registerStrokes();

        TUndoManager::manager()->add(undo.release());
      }
    }
    TUndoManager::manager()->endBlock();

    // Finally, notify changed frames
    std::for_each(fids.begin(), fids.end(), [this](const TFrameId &fid) {
      m_tool->notifyImageChanged(fid);
    });
  } else
    TUndoManager::manager()->add(m_undo.release());  // Outside any undo block
}

//-----------------------------------------------------------------------------

void DragSelectionTool::VectorChangeThicknessTool::leftButtonDown(
    const TPointD &pos, const TMouseEvent &e) {
  m_curPos   = pos;
  m_firstPos = pos;
}

//-----------------------------------------------------------------------------

void DragSelectionTool::VectorChangeThicknessTool::leftButtonDrag(
    const TPointD &pos, const TMouseEvent &e) {
  TAffine aff;
  TPointD delta    = pos - m_curPos;
  TVectorImageP vi = getTool()->getImage(true);
  if (!vi) return;
  VectorSelectionTool *tool = dynamic_cast<VectorSelectionTool *>(m_tool);
  tool->setResetCenter(false);
  m_thicknessChange = (pos.y - m_firstPos.y) * 0.2;
  changeImageThickness(*vi, m_thicknessChange);
  getTool()->m_deformValues.m_maxSelectionThickness = m_thicknessChange;
  getTool()->computeBBox();
  getTool()->invalidate();
  m_curPos = pos;
  getTool()->notifyImageChanged();
  TTool::getApplication()->getCurrentTool()->notifyToolChanged();
}

//-----------------------------------------------------------------------------

void DragSelectionTool::VectorChangeThicknessTool::leftButtonUp(
    const TPointD &pos, const TMouseEvent &e) {
  TVectorImageP curVi = getTool()->getImage(true);
  if (!curVi) return;
  addUndo();
  m_strokesThickness.clear();
}

//=============================================================================
namespace {
//-----------------------------------------------------------------------------

bool getGroupBBox(const TVectorImage &vi, int strokeIndex, TRectD &gBox) {
  if (!vi.isStrokeGrouped(strokeIndex)) return false;

  gBox = vi.getStroke(strokeIndex)->getBBox();

  int s, sCount = int(vi.getStrokeCount());
  for (s = 0; s != sCount; ++s) {
    if (vi.sameGroup(s, strokeIndex)) gBox += vi.getStroke(s)->getBBox();
  }

  return true;
}

//=============================================================================
// UndoEnterGroup
//-----------------------------------------------------------------------------

class UndoEnterGroup final : public TUndo {
  int m_strokeIndex;
  TVectorImageP m_vi;

public:
  UndoEnterGroup(TVectorImageP vi, int strokeIndex)
      : m_vi(vi), m_strokeIndex(strokeIndex) {}
  void undo() const override {
    m_vi->exitGroup();
    TTool::getApplication()->getCurrentXsheet()->notifyXsheetChanged();
  }
  void redo() const override {
    m_vi->enterGroup(m_strokeIndex);
    TTool::getApplication()->getCurrentXsheet()->notifyXsheetChanged();
  }
  int getSize() const override { return sizeof(*this); }
};

//=============================================================================
// UndoExitGroup
//-----------------------------------------------------------------------------

class UndoExitGroup final : public TUndo {
  int m_strokeIndex;
  TVectorImageP m_vi;

public:
  UndoExitGroup(TVectorImageP vi, int strokeIndex)
      : m_vi(vi), m_strokeIndex(strokeIndex) {}
  void undo() const override {
    m_vi->enterGroup(m_strokeIndex);
    TTool::getApplication()->getCurrentXsheet()->notifyXsheetChanged();
  }
  void redo() const override {
    m_vi->exitGroup();
    TTool::getApplication()->getCurrentXsheet()->notifyXsheetChanged();
  }

  int getSize() const override { return sizeof(*this); }
};

//=============================================================================
// UndoReOrderStrokes
//-----------------------------------------------------------------------------

struct ReorderData {
  TFrameId frameId;
  std::vector<int> oldIndexes;
  std::vector<int> newIndexes;
};
typedef std::vector<ReorderData> ReorderDatas;

class UndoReorderStrokes final : public TUndo {
protected:
  TXshSimpleLevelP m_level;
  ReorderDatas m_reOrderDatas;

public:
  UndoReorderStrokes(TXshSimpleLevelP level, ReorderDatas reOrderDatas)
      : m_level(level), m_reOrderDatas(reOrderDatas) {}

  ~UndoReorderStrokes() {}

  QString getHistoryString() {
    return UiTranslations::text(QObject::tr("Sort Vector Strokes With Palette Order Level : %1"))
        .arg(QString::fromStdWString(m_level->getName()));
  }

  void undo() const override {
    for (const ReorderData &data : m_reOrderDatas) {
      TVectorImageP vi = m_level->getFrame(data.frameId, true);
      if (!vi) continue;
      vi->reOrderStrokes(data.newIndexes, data.oldIndexes);
    }
    TTool::getApplication()->getCurrentTool()->getTool()->notifyImageChanged();
  }

  void redo() const override {
    for (const ReorderData &data : m_reOrderDatas) {
      TVectorImageP vi = m_level->getFrame(data.frameId, true);
      if (!vi) continue;
      vi->reOrderStrokes(data.oldIndexes, data.newIndexes);
      TTool::getApplication()
          ->getCurrentTool()
          ->getTool()
          ->notifyImageChanged();
    }
  }

  int getSize() const override { return sizeof(*this); }
};

// With palette Order
void sortStylesIds(QVector<int> &styles, TPaletteP palette) {
  if (!palette) return;
  QVector<int> tmpStyles = styles;
  styles.clear();
  for (int p = 0; p < palette->getPageCount(); p++) {
    TPalette::Page *page = palette->getPage(p);
    for (int s = 0; s < page->getStyleCount(); s++) {
      int tmpId = page->getStyleId(s);
      if (tmpStyles.indexOf(tmpId, 0) != -1) {
        styles.append(tmpId);
      }
      if (tmpStyles.count() == styles.count()) return;
    }
  }
  assert(false);
}

}  // namespace

//=============================================================================
// VectorSelectionTool
//-----------------------------------------------------------------------------

VectorSelectionTool::VectorSelectionTool(int targetType)
    : SelectionTool(targetType)
    , m_selectionTarget("Mode:")
    , m_includeIntersection("Include Intersection", false)
    , m_constantThickness("Preserve Thickness", false)
    , m_envelope("Envelope", false)
    , m_reviewFills("Review Fills", false)
    , m_levelSelection(m_strokeSelection)
    , m_capStyle("Cap")
    , m_joinStyle("Join")
    , m_miterJoinLimit("Miter:", 0, 100, 4)
    , m_opacity("Opacity:", 0, 100, 100)
    , m_selectionCount(0)
    , m_canEnterGroup(true)
    , m_resetCenter(true) {
  assert(targetType == TTool::Vectors);
  m_prop.bind(m_selectionTarget);
  m_prop.bind(m_includeIntersection);
  m_prop.bind(m_constantThickness);
  m_prop.bind(m_envelope);
  m_prop.bind(m_reviewFills);
  ArtLayer::bindProperties(m_prop);

  m_selectionTarget.addValue(NORMAL_TYPE);
  m_selectionTarget.addValue(SELECTED_FRAMES_TYPE);
  m_selectionTarget.addValue(ONION_SKIN_FRAMES_TYPE);
  m_selectionTarget.addValue(ALL_LEVEL_TYPE);
  m_selectionTarget.addValue(SAME_STYLE_TYPE);
  m_selectionTarget.addValue(STYLE_SELECTED_FRAMES_TYPE);
  m_selectionTarget.addValue(STYLE_LEVEL_TYPE);
  m_selectionTarget.addValue(BOUNDARY_TYPE);
  m_selectionTarget.addValue(BOUNDARY_SELECTED_FRAMES_TYPE);
  m_selectionTarget.addValue(BOUNDARY_LEVEL_TYPE);

  m_strokeSelection.setView(this);

  m_includeIntersection.setId("IncludeIntersection");
  m_constantThickness.setId("PreserveThickness");
  m_envelope.setId("Envelope");
  m_reviewFills.setId("ReviewFills");
  m_selectionTarget.setId("SelectionMode");

  m_capStyle.addValue(BUTT_WSTR, QString::fromStdWString(BUTT_WSTR));
  m_capStyle.addValue(ROUNDC_WSTR, QString::fromStdWString(ROUNDC_WSTR));
  m_capStyle.addValue(PROJECTING_WSTR,
                      QString::fromStdWString(PROJECTING_WSTR));
  m_capStyle.setId("Cap");

  m_joinStyle.addValue(MITER_WSTR, QString::fromStdWString(MITER_WSTR));
  m_joinStyle.addValue(ROUNDJ_WSTR, QString::fromStdWString(ROUNDJ_WSTR));
  m_joinStyle.addValue(BEVEL_WSTR, QString::fromStdWString(BEVEL_WSTR));
  m_joinStyle.setId("Join");

  m_miterJoinLimit.setId("Miter");
  m_opacity.setId("Opacity");

  m_outlineProps.bind(m_capStyle);
  m_outlineProps.bind(m_joinStyle);
  m_outlineProps.bind(m_miterJoinLimit);
  m_outlineProps.bind(m_opacity);
}

//------------------------------------------------------------------------------

void VectorSelectionTool::setNewFreeDeformer() {
  clearDeformers();

  TVectorImageP vi =
      getImage(true);  // BAD: Should not be done at tool creation...
  if (!vi) return;

  // Current freeDeformer always at index 0
  m_freeDeformers.push_back(
      new VectorFreeDeformer(vi, m_strokeSelection.getSelection()));

  if (isLevelType() || isSelectedFramesType()) {
    TXshSimpleLevel *level =
        TTool::getApplication()->getCurrentLevel()->getSimpleLevel();

    // All SELECTED frames' subsequent freeDeformers are stored sequentially
    // after that
    std::vector<TFrameId> fids;
    level->getFids(fids);

    fids.erase(std::remove_if(fids.begin(), fids.end(),
                              [this](const TFrameId &fid) {
                                return currentOrNotSelected(*this, fid);
                              }),
               fids.end());

    std::vector<TFrameId>::iterator ft, fEnd = fids.end();
    for (ft = fids.begin(); ft != fEnd; ++ft) {
      if (TVectorImageP levelVi = level->getFrame(*ft, false)) {
        const std::vector<int> &selectedStrokeIdxs =
            ::getSelectedStrokes(*levelVi, m_levelSelection);
        std::set<int> strokesIndices(selectedStrokeIdxs.begin(),
                                     selectedStrokeIdxs.end());

        m_freeDeformers.push_back(
            new VectorFreeDeformer(levelVi, strokesIndices));
      }
    }
  }
}

//-----------------------------------------------------------------------------

bool VectorSelectionTool::isEnvelopeMode() const {
  // Motion paths keep the standard handles
  return m_envelope.getValue() &&
         !TTool::getApplication()->getCurrentObject()->isSpline();
}

//-----------------------------------------------------------------------------

bool VectorSelectionTool::isLevelType() const {
  return m_levelSelection.framesMode() == LevelSelection::FRAMES_ALL;
}

//-----------------------------------------------------------------------------

bool VectorSelectionTool::isSelectedFramesType() const {
  return m_levelSelection.framesMode() == LevelSelection::FRAMES_SELECTED ||
         isOnionSkinFramesType();
}

//-----------------------------------------------------------------------------

bool VectorSelectionTool::isOnionSkinFramesType() const {
  return m_levelSelection.framesMode() == LevelSelection::FRAMES_ONION_SKIN;
}

//-----------------------------------------------------------------------------

const std::set<TFrameId> &VectorSelectionTool::getTargetFrames() const {
  return isOnionSkinFramesType() ? m_onionFrames : getSelectedFrames();
}

//-----------------------------------------------------------------------------

bool VectorSelectionTool::refreshOnionFrames() {
  std::set<TFrameId> frames = LevelSelection::onionSkinFrames();
  if (frames == m_onionFrames) return false;

  m_onionFrames.swap(frames);
  return true;
}

//-----------------------------------------------------------------------------

bool VectorSelectionTool::isSameStyleType() const {
  return m_levelSelection.filter() == LevelSelection::SELECTED_STYLES;
}

//-----------------------------------------------------------------------------

bool VectorSelectionTool::isModifiableSelectionType() const {
  return (m_levelSelection.isEmpty() ||
          m_levelSelection.filter() == LevelSelection::SELECTED_STYLES);
}

//-----------------------------------------------------------------------------

void VectorSelectionTool::updateTranslation() {
  m_selectionTarget.setQStringName(tr("Mode:"));
  m_selectionTarget.setItemUIName(NORMAL_TYPE, tr("Standard"));
  m_selectionTarget.setItemUIName(SELECTED_FRAMES_TYPE, tr("Selected Frames"));
  m_selectionTarget.setItemUIName(ONION_SKIN_FRAMES_TYPE,
                                  tr("Onion Skin Frames"));
  m_selectionTarget.setItemUIName(ALL_LEVEL_TYPE, tr("Whole Level"));
  m_selectionTarget.setItemUIName(SAME_STYLE_TYPE, tr("Same Style"));
  m_selectionTarget.setItemUIName(STYLE_SELECTED_FRAMES_TYPE,
                                  tr("Same Style on Selected Frames"));
  m_selectionTarget.setItemUIName(STYLE_LEVEL_TYPE,
                                  tr("Same Style on Whole Level"));
  m_selectionTarget.setItemUIName(BOUNDARY_TYPE, tr("Boundary Strokes"));
  m_selectionTarget.setItemUIName(BOUNDARY_SELECTED_FRAMES_TYPE,
                                  tr("Boundaries on Selected Frames"));
  m_selectionTarget.setItemUIName(BOUNDARY_LEVEL_TYPE,
                                  tr("Boundaries on Whole Level"));

  m_includeIntersection.setQStringName(tr("Include Intersection"));
  m_constantThickness.setQStringName(tr("Preserve Thickness"));
  m_envelope.setQStringName(tr("Envelope"));
  m_reviewFills.setQStringName(tr("Review Fills"));

  m_capStyle.setQStringName(tr("Cap"));
  m_capStyle.setItemUIName(BUTT_WSTR, tr("Butt cap"));
  m_capStyle.setItemUIName(ROUNDC_WSTR, tr("Round cap"));
  m_capStyle.setItemUIName(PROJECTING_WSTR, tr("Projecting cap"));

  m_joinStyle.setQStringName(tr("Join"));
  m_joinStyle.setItemUIName(MITER_WSTR, tr("Miter join"));
  m_joinStyle.setItemUIName(ROUNDJ_WSTR, tr("Round join"));
  m_joinStyle.setItemUIName(BEVEL_WSTR, tr("Bevel join"));

  m_miterJoinLimit.setQStringName(tr("Miter:"));
  m_opacity.setQStringName(tr("Opacity:"));
  ArtLayer::updateTranslation();
  SelectionTool::updateTranslation();
}

//-----------------------------------------------------------------------------

void VectorSelectionTool::updateSelectionTarget() {
  // Make the correct selection current
  if (m_selectionTarget.getIndex() == NORMAL_TYPE_IDX) {
    std::set<int> selectedStrokes;  // Retain previously selected strokes across
    selectedStrokes.swap(
        m_strokeSelection.getSelection());  // current selection change

    m_strokeSelection.makeCurrent();  // Empties any (different) previously
                                      // current selection on its own
    selectedStrokes.swap(m_strokeSelection.getSelection());
    return;
  }

  m_levelSelection.makeCurrent();  // Same here

  // Choose frames mode
  LevelSelection::FramesMode framesMode;
  switch (m_selectionTarget.getIndex()) {
  case SAME_STYLE_TYPE_IDX:
  case BOUNDARY_TYPE_IDX:
    framesMode = LevelSelection::FRAMES_CURRENT;
    break;

  case ALL_LEVEL_TYPE_IDX:
  case STYLE_LEVEL_TYPE_IDX:
  case BOUNDARY_LEVEL_TYPE_IDX:
    framesMode = LevelSelection::FRAMES_ALL;
    break;

  case ONION_SKIN_FRAMES_TYPE_IDX:
    framesMode = LevelSelection::FRAMES_ONION_SKIN;
    break;

  case SELECTED_FRAMES_TYPE_IDX:
  case STYLE_SELECTED_FRAMES_TYPE_IDX:
  case BOUNDARY_SELECTED_FRAMES_TYPE_IDX:
    framesMode = LevelSelection::FRAMES_SELECTED;
    break;
  }

  if (framesMode != m_levelSelection.framesMode()) clearSelectedStrokes();

  m_levelSelection.framesMode() = framesMode;

  // Choose filter
  LevelSelection::Filter filter;
  switch (m_selectionTarget.getIndex()) {
  case SELECTED_FRAMES_TYPE_IDX:
  case ONION_SKIN_FRAMES_TYPE_IDX:
  case ALL_LEVEL_TYPE_IDX:
    filter = LevelSelection::WHOLE;
    selectedStyles().clear();
    break;

  case SAME_STYLE_TYPE_IDX:
  case STYLE_SELECTED_FRAMES_TYPE_IDX:
  case STYLE_LEVEL_TYPE_IDX:
    filter = LevelSelection::SELECTED_STYLES;
    break;

  case BOUNDARY_TYPE_IDX:
  case BOUNDARY_SELECTED_FRAMES_TYPE_IDX:
  case BOUNDARY_LEVEL_TYPE_IDX:
    filter = LevelSelection::BOUNDARY_STROKES;
    selectedStyles().clear();
    break;
  }

  if (filter != m_levelSelection.filter()) clearSelectedStrokes();

  m_levelSelection.filter() = filter;
}

//-----------------------------------------------------------------------------

void VectorSelectionTool::finalizeSelection() {
  // The frames shown by the onion skin depend on the current frame and on the
  // onion skin settings: take them again before the boxes are computed.
  if (isOnionSkinFramesType()) refreshOnionFrames();

  TVectorImageP vi = getImage(false);
  if (vi && !m_levelSelection.isEmpty()) {
    std::set<int> &selection = m_strokeSelection.getSelection();
    selection.clear();

    // Apply base additive selection
    if (!isSelectedFramesType() || getTargetFrames().count(getCurrentFid())) {
      // Apply filters
      std::vector<int> selectedStrokes =
          getSelectedStrokes(*vi, m_levelSelection);
      std::set<int>(selectedStrokes.begin(), selectedStrokes.end())
          .swap(selection);
    }
  }

  // Line Art / Colour Art: keep only the strokes of the edited role
  if (vi && ArtLayer::target() != ArtLayer::TARGET_OFF) {
    std::set<int> &selection = m_strokeSelection.getSelection();
    int strokeCount          = vi->getStrokeCount();
    ArtLayer::Filter filter(*vi);
    for (auto it = selection.begin(); it != selection.end();) {
      if (*it >= 0 && *it < strokeCount && !filter.isEditable(*it))
        it = selection.erase(it);
      else
        ++it;
    }
  }

  computeBBox();

  const auto opacity = SelectionOpacity::summarize(
      m_strokeSelection.getImage(), m_strokeSelection.getSelection());
  if (opacity.count && !opacity.mixed)
    m_opacity.setValue(100.0 * opacity.value);

  TTool::getApplication()
      ->getCurrentTool()
      ->notifyToolChanged();  // Refreshes toolbar values
}

//-----------------------------------------------------------------------------

void VectorSelectionTool::clearSelectedStrokes() {
  m_strokeSelection.selectNone();
  m_levelSelection.styles().clear();
  m_deformValues.reset();
}

//-----------------------------------------------------------------------------

void VectorSelectionTool::modifySelectionOnClick(TImageP image,
                                                 const TPointD &pos,
                                                 const TMouseEvent &e) {
  TVectorImageP vi = TVectorImageP(image);
  assert(m_strokeSelection.getImage() == vi);

  if (!vi) return;

  updateSelectionTarget();  // Make selection current. This is necessary in case
  // some other selection context made another selection current.
  m_firstPos = m_curPos = pos;
  m_selectingRect       = FourPoints();
  m_selecting           = false;
  m_justSelected        = false;

  updateAction(pos, e);
  if (m_what != Inside && m_what != Outside && m_what != ADD_SELECTION) return;

  UINT index         = 0;
  bool modifiableSel = isModifiableSelectionType(),
       strokeAtPos   = getStrokeIndexFromPos(index, vi, pos, getPixelSize(),
                                             getViewer()->getViewMatrix()),
       addStroke     = strokeAtPos && !m_strokeSelection.isSelected(index),
       toggleStroke  = strokeAtPos && e.isShiftPressed();

  m_selecting =
      (modifiableSel && !strokeAtPos  // There must be no stroke under cursor
       && (e.isShiftPressed()  // and either the user is explicitly performing
                               // additional selection
           || (m_strokeSelectionType.getIndex() !=
               POLYLINE_SELECTION_IDX)  // or the tool support immediate
                                        // selection on clear
           || m_strokeSelection
                  .isEmpty()));  // or the strokes list was already cleared

  bool clearTargets     = !(strokeAtPos || e.isShiftPressed() || m_selecting),
       clearSelection   = (addStroke || !strokeAtPos) && !e.isShiftPressed(),
       selectionChanged = clearSelection;

  assert(clearTargets ? clearSelection : true);

  if (clearTargets) m_levelSelection.selectNone();

  if (clearSelection) {
    m_strokeSelection.selectNone();
    selectedStyles().clear();  // Targets are preserved here
  }

  if (strokeAtPos)
    selectionChanged = m_justSelected = selectStroke(index, toggleStroke);

  if (selectionChanged) {
    m_deformValues.reset();  // Resets selection values shown in the toolbar

    finalizeSelection();
    notifySelectionChanged();

    invalidate();
  }
}

//-----------------------------------------------------------------------------

void VectorSelectionTool::leftButtonDoubleClick(const TPointD &pos,
                                                const TMouseEvent &e) {
  TVectorImageP vi = getImage(false);
  if (!vi) return;

  if (m_strokeSelectionType.getIndex() == POLYLINE_SELECTION_IDX &&
      !m_polyline.empty()) {
    closePolyline(pos);
    selectRegionVectorImage(m_includeIntersection.getValue());

    m_selecting = false;
    invalidate();

    return;
  }

  int strokeIndex;
  if ((strokeIndex = vi->pickGroup(pos)) >= 0) {
    if (vi->canEnterGroup(strokeIndex) && m_canEnterGroup) {
      if (vi->enterGroup(strokeIndex)) {
        clearSelectedStrokes();
        TUndoManager::manager()->add(new UndoEnterGroup(vi, strokeIndex));
      }
    }
  } else if ((strokeIndex = vi->exitGroup()) >= 0)
    TUndoManager::manager()->add(new UndoExitGroup(vi, strokeIndex));

  finalizeSelection();
  invalidate();
}

//-----------------------------------------------------------------------------

void VectorSelectionTool::leftButtonDrag(const TPointD &pos,
                                         const TMouseEvent &e) {
  if (m_dragTool) {
    if (!m_strokeSelection.isEditable()) return;

    m_dragTool->leftButtonDrag(pos, e);
    return;
  }

  TVectorImageP vi = getImage(false);
  if (!vi) return;

  double pixelSize        = getPixelSize();
  TTool::Application *app = TTool::getApplication();
  if (!app || m_justSelected || !m_selecting ||
      tdistance2(pos, m_curPos) < 9.0 * pixelSize * pixelSize)
    return;

  m_curPos = pos;

  if (m_strokeSelectionType.getIndex() == FREEHAND_SELECTION_IDX) {
    freehandDrag(pos);
    invalidate();
  } else if (m_strokeSelectionType.getIndex() == RECT_SELECTION_IDX) {
    bool selectOverlappingStroke = (m_firstPos.x > pos.x);

    TRectD rect(m_firstPos, pos);
    m_selectingRect = rect;

    std::set<int> oldSelection;
    if (m_shiftPressed) oldSelection = m_strokeSelection.getSelection();

    clearSelectedStrokes();

    QMutexLocker lock(vi->getMutex());

    m_strokeSelection.setImage(vi);  // >_<  Shouldn't be done here...?

    bool selectionChanged = false;

    int s, sCount = vi->getStrokeCount();
    for (s = 0; s != sCount; ++s) {
      if (!vi->isEnteredGroupStroke(s)) continue;

      TStroke *stroke = vi->getStroke(s);

      if (m_strokeSelection.isSelected(s)) continue;

      bool inSelection = selectOverlappingStroke
                             ? rect.overlaps(stroke->getBBox())
                             : rect.contains(stroke->getBBox());

      if (inSelection || (m_shiftPressed && oldSelection.count(s)))
        selectionChanged = (selectStroke(s, false) || selectionChanged);
    }

    if (selectionChanged) finalizeSelection();

    invalidate();
  }
}

//-----------------------------------------------------------------------------

void VectorSelectionTool::leftButtonDown(const TPointD &pos,
                                         const TMouseEvent &e) {
  if (getViewer() && getViewer()->getGuidedStrokePickerMode()) {
    getViewer()->doPickGuideStroke(pos);
    return;
  }

  // The onion skin may have been changed since the boxes were computed
  if (isOnionSkinFramesType() && refreshOnionFrames()) finalizeSelection();

  SelectionTool::leftButtonDown(pos, e);
}

//-----------------------------------------------------------------------------

void VectorSelectionTool::leftButtonUp(const TPointD &pos,
                                       const TMouseEvent &e) {
  m_leftButtonMousePressed = false;
  m_shiftPressed           = false;

  if (m_dragTool) {
    if (!m_strokeSelection.isEditable()) {
      delete m_dragTool;
      m_dragTool = 0;
      return;
    }

    m_dragTool->leftButtonUp(pos, e);
    delete m_dragTool;
    m_dragTool = 0;
    invalidate();
    return;
  }

  if (!m_selecting) return;

  // Complete selection
  TVectorImageP vi = getImage(false);

  if (vi) {
    if (m_strokeSelectionType.getIndex() == RECT_SELECTION_IDX)
      notifySelectionChanged();
    else if (m_strokeSelectionType.getIndex() == FREEHAND_SELECTION_IDX) {
      QMutexLocker lock(vi->getMutex());

      closeFreehand(pos);

      if (m_stroke->getControlPointCount() > 3)
        selectRegionVectorImage(m_includeIntersection.getValue());

      delete m_stroke;  // >:(
      m_stroke = 0;
      m_track.clear();
    }
  }

  m_selecting    = false;
  m_justSelected = false;
  invalidate();
}

//-----------------------------------------------------------------------------

void VectorSelectionTool::addContextMenuItems(QMenu *menu) {
  menu->addAction(CommandManager::instance()->getAction(MI_EditStrokeOpacityProfile));
  menu->addAction(CommandManager::instance()->getAction(MI_RemoveEndpoints));
  menu->addAction(
      CommandManager::instance()->getAction(MI_ConvertToOutlineShapes));
  menu->addAction(
      CommandManager::instance()->getAction(MI_SortWithPaletteOrder));
  if (!m_strokeSelection.isEmpty())
    menu->addAction(
        CommandManager::instance()->getAction(MI_PairInbetweenStrokes));
  if (!m_strokeSelection.isEmpty())
    menu->addAction(
        CommandManager::instance()->getAction(MI_AddInbetweenZoneHint));
  if (StrokeSelection::hasInbetweenStrokePairs())
    menu->addAction(
        CommandManager::instance()->getAction(MI_ClearInbetweenStrokePairs));
  menu->addSeparator();

  m_strokeSelection.getGroupCommand()->addMenuItems(menu);

  if (!m_strokeSelection.isEmpty()) {
    menu->addSeparator();
    if (QApplication::keyboardModifiers() & Qt::ShiftModifier) {
      QAction *saveSelectionAction =
          menu->addAction(tr("Save Vector Selection As..."));
      QObject::connect(saveSelectionAction, &QAction::triggered, menu,
                       [this]() { m_strokeSelection.saveVectorSelectionAs(); });
    } else {
      menu->addAction(
          CommandManager::instance()->getAction(MI_SaveAsCustomVectorBrush));
    }
  }
}

//-----------------------------------------------------------------------------

void VectorSelectionTool::drawInLevelType(const TVectorImage &vi) {
  glPushMatrix();

  FourPoints bbox = getBBox();
  if (!bbox.isEmpty()) {
    TPixel32 frameColor(127, 127, 127);
    double pixelSize = getPixelSize();

    drawFourPoints(bbox, TPixel32::Black, 0x5555, true);
    drawFourPoints(bbox.enlarge(pixelSize * (-4)), frameColor, 0xffff, true);
    drawFourPoints(bbox.enlarge(pixelSize * (-2)), frameColor, 0x8888, true);

    drawCommandHandle(&vi);
  }

  drawSelectedStrokes(vi);

  if (m_selecting && !m_selectingRect.isEmpty()) drawRectSelection(&vi);

  if (m_strokeSelectionType.getIndex() == POLYLINE_SELECTION_IDX)
    drawPolylineSelection();

  glPopMatrix();
}

//-----------------------------------------------------------------------------

void VectorSelectionTool::drawSelectedStrokes(const TVectorImage &vi) {
  glEnable(GL_LINE_STIPPLE);

  double pixelSize = getPixelSize();

  int s, sCount = vi.getStrokeCount();
  for (s = 0; s != sCount; ++s) {
    if (m_strokeSelection.isSelected(s)) {
      TStroke *stroke = vi.getStroke(s);

      glLineStipple(1, 0xF0F0);
      tglColor(TPixel32::Black);
      drawStrokeCenterline(*stroke, pixelSize);

      glLineStipple(1, 0x0F0F);
      tglColor(TPixel32::White);
      drawStrokeCenterline(*stroke, pixelSize);
    }
  }

  glDisable(GL_LINE_STIPPLE);
}

//-----------------------------------------------------------------------------

void VectorSelectionTool::drawGroup(const TVectorImage &vi) {
  int s, sCount = vi.getStrokeCount();
  for (s = 0; s != sCount; ++s) {
    if (m_strokeSelection.isSelected(s)) {
      TRectD gBox;
      if (getGroupBBox(vi, s, gBox)) drawRect(gBox, TPixel::Blue, 0xffff);
    }
  }
}

//-----------------------------------------------------------------------------

void VectorSelectionTool::draw() {
  TVectorImageP vi = getImage(false);
  if (!vi) return;

  if (isLevelType() || isSelectedFramesType()) {
    drawInLevelType(*vi);
    return;
  }

  glPushMatrix();

  if (m_strokeSelection.isEmpty())  // o_o  WTF!?
    m_bboxs.clear();                //

  // common draw
  if (getBBoxsCount() > 0) drawCommandHandle(vi.getPointer());

  if (m_selecting && !m_selectingRect.isEmpty())
    drawRectSelection(vi.getPointer());

  TRectD bbox = vi->getBBox();
  TPixel32 frameColor(140, 140, 140);
  tglColor(frameColor);
  drawRect(bbox, frameColor, 0x5555, true);

  drawSelectedStrokes(*vi);

  if (m_strokeSelectionType.getIndex() == POLYLINE_SELECTION_IDX)
    drawPolylineSelection();
  else if (m_strokeSelectionType.getIndex() == FREEHAND_SELECTION_IDX)
    drawFreehandSelection();

  if (m_levelSelection.isEmpty()) drawGroup(*vi);

  glPopMatrix();
}

//-----------------------------------------------------------------------------

TSelection *VectorSelectionTool::getSelection() {
  TImage *image    = getImage(false);
  TVectorImageP vi = image;
  if (!vi) return 0;

  return &m_strokeSelection;
}

//-----------------------------------------------------------------------------

bool VectorSelectionTool::isSelectionEmpty() {
  TVectorImageP vi =
      getImage(false);  // We want at least current image to be visible.
  if (!vi)              // This is somewhat in line to preventing tool actions
    return true;        // on non-visible levels.

  return m_strokeSelection
      .isEmpty();  // Same here, something should be visibly selected.
}

//-----------------------------------------------------------------------------

void VectorSelectionTool::computeBBox() {
  m_bboxs.clear();
  m_envelopes.clear();
  if (canResetCenter()) m_centers.clear();

  TVectorImageP vi = getImage(false);
  if (!vi) return;

  if (isLevelType() || isSelectedFramesType()) {
    double maxThickness = 0;

    // Calculate current image's bbox - it is always the first one (index 0)
    if (vi && (isLevelType() || getTargetFrames().count(getCurrentFid()) > 0)) {
      FourPoints bbox =
          getFourPointsFromVectorImage(vi, selectedStyles(), maxThickness);

      m_bboxs.push_back(bbox);
      m_centers.push_back((bbox.getP00() + bbox.getP11()) * 0.5);
    }

    // All subsequent SELECTED frames' bboxes come sequentially starting from 1
    if (TXshSimpleLevel *level =
            TTool::getApplication()->getCurrentLevel()->getSimpleLevel()) {
      std::vector<TFrameId> fids;
      level->getFids(fids);

      for (int i = 0; i < (int)fids.size(); ++i) {
        if (getCurrentFid() == fids[i] ||
            (isSelectedFramesType() && getTargetFrames().count(fids[i]) == 0))
          continue;

        TVectorImageP vi = level->getFrame(fids[i], false);
        if (!vi) continue;

        FourPoints p =
            getFourPointsFromVectorImage(vi, selectedStyles(), maxThickness);

        m_bboxs.push_back(p);
        m_centers.push_back(0.5 * (p.getP00() + p.getP11()));
        m_deformValues.m_maxSelectionThickness = maxThickness;
      }
    }
  } else if (vi) {
    TRectD newBbox;
    double maxThickness = 0;

    for (int i = 0; i < (int)vi->getStrokeCount(); i++) {
      TStroke *stroke = vi->getStroke(i);

      if (m_strokeSelection.isSelected(i)) {
        newBbox += stroke->getBBox();

        for (int j = 0; j < stroke->getControlPointCount(); j++) {
          double thick = stroke->getControlPoint(j).thick;
          if (maxThickness < thick) maxThickness = thick;
        }
      }
    }

    m_deformValues.m_maxSelectionThickness = maxThickness;
    FourPoints bbox;
    bbox = newBbox;
    m_bboxs.push_back(bbox);
    if (canResetCenter())
      m_centers.push_back(0.5 * (bbox.getP11() + bbox.getP00()));
  }

  ++m_selectionCount;
}

void VectorSelectionTool::notifyImageChanged() {
  TXshSimpleLevel *level;
  std::set<TFrameId> frames;

  switch (m_levelSelection.framesMode()) {
  case LevelSelection::FramesMode::FRAMES_NONE:
  case LevelSelection::FramesMode::FRAMES_CURRENT:
    TTool::notifyImageChanged(getCurrentFid());
    break;
  case LevelSelection::FramesMode::FRAMES_SELECTED:
  case LevelSelection::FramesMode::FRAMES_ONION_SKIN:
    frames = getTargetFrames();
    for (TFrameId fid : frames) {
      TTool::notifyImageChanged(fid);
    }
    break;
  case LevelSelection::FramesMode::FRAMES_ALL:
    level     = TTool::getApplication()->getCurrentLevel()->getSimpleLevel();
    int count = level->getFrameCount();
    for (int i = 0; i < count; i++) {
      TTool::notifyImageChanged(level->getFrameId(i));
    }
    break;
  }
}

//-----------------------------------------------------------------------------

bool VectorSelectionTool::selectStroke(int index, bool toggle) {
  TVectorImageP vi = getImage(false);
  assert(vi);
  assert(m_strokeSelection.getImage() == vi);

  if (!vi->isEnteredGroupStroke(index))  // If index is not in current group
    return false;

  if (index < 0 || index >= int(vi->getStrokeCount()))  // Should be asserted...
    return false;

  ArtLayer::Filter artFilter(*vi);
  if (!artFilter.isEditable(index)) return false;

  bool wasSelected = m_strokeSelection.isSelected(index),
       selectState = !(wasSelected && toggle);

  // (De)Select additional strokes related to ours
  std::set<int> &selectedStyles = this->selectedStyles();

  if (isSameStyleType())  // Color selection
  {
    TStroke *refStroke = vi->getStroke(index);
    assert(refStroke);

    int style = refStroke->getStyle();

    if (selectState)
      selectedStyles.insert(style);
    else
      selectedStyles.erase(style);
  } else if (vi->isStrokeGrouped(index) &&
             vi->selectable(index))  // Group selection
  {
    int s, sCount = vi->getStrokeCount();
    for (s = 0; s != sCount; ++s) {
      if (vi->selectable(s) && vi->sameSubGroup(index, s) &&
          artFilter.isEditable(s))
        m_strokeSelection.select(s, selectState);
    }
  } else  // Single stroke selection
    m_strokeSelection.select(index, selectState);

  return (selectState != wasSelected);
}

//-----------------------------------------------------------------------------

void VectorSelectionTool::onActivate() {
  ArtLayer::loadSettings();
  if (m_firstTime) {
    m_includeIntersection.setValue(l_strokeSelectIncludeIntersection ? 1 : 0);
    m_constantThickness.setValue(l_strokeSelectConstantThickness ? 1 : 0);
    m_envelope.setValue(l_strokeSelectEnvelope ? 1 : 0);
    m_reviewFills.setValue(l_strokeSelectReviewFills ? 1 : 0);
    m_strokeSelection.setSceneHandle(
        TTool::getApplication()->getCurrentScene());
  }

  SelectionTool::onActivate();
}

//-----------------------------------------------------------------------------

void VectorSelectionTool::onDeactivate() {
  bool reviewing = false;
  if (auto *deform = dynamic_cast<VectorDeformTool *>(m_dragTool)) {
    deform->cancel();
    reviewing = deform->isReviewing();
    // The modal review owns this drag's call stack. Let leftButtonUp dispose
    // it once the context-switch rejection has unwound the dialog.
    if (!deform->isReviewing()) {
      delete m_dragTool;
      m_dragTool = nullptr;
    }
    m_leftButtonMousePressed = false;
  }
  if (!reviewing)
    VectorPaintClip::captureSelection(m_strokeSelection.getImage(),
                                      m_strokeSelection.getSelection());
  if (isLevelType()) return;

  SelectionTool::onDeactivate();
}

//-----------------------------------------------------------------------------

void VectorSelectionTool::doOnActivate() {
  TVectorImageP vi = getImage(false);
  m_strokeSelection.setImage(vi);

  updateSelectionTarget();

  finalizeSelection();
  invalidate();
}

//-----------------------------------------------------------------------------

void VectorSelectionTool::onImageChanged() {
  TVectorImageP vi          = getImage(false);
  TVectorImageP selectedImg = m_strokeSelection.getImage();

  // If the image changed, we reset the selection
  if (vi != selectedImg) {
    m_strokeSelection.selectNone();
    m_strokeSelection.setImage(vi);

    // Clear style selection if there's no image or different palettes
    if (!vi || !selectedImg || vi->getPalette() != selectedImg->getPalette())
      selectedStyles().clear();

    finalizeSelection();
    return;  // nothing more to do
  }

  // Same image: remove invalid indices
  if (!m_strokeSelection.isEmpty()) {
    int strokeCount = vi ? vi->getStrokeCount() : 0;
    std::set<int> copy =
        m_strokeSelection.getSelection();  // avoid invalid iterator
    for (int idx : copy) {
      if (idx < 0 || idx >= strokeCount) m_strokeSelection.select(idx, false);
    }
  }

  finalizeSelection();
}

//-----------------------------------------------------------------------------

bool VectorSelectionTool::isDragging() const {
  return m_enabled && m_leftButtonMousePressed;
}

bool VectorSelectionTool::keyDown(QKeyEvent *event) {
  if (event->key() == Qt::Key_Escape && m_dragTool) {
    if (auto *deform = dynamic_cast<VectorDeformTool *>(m_dragTool)) {
      deform->cancel();
      // A modal review is on this drag object's stack. Its caller owns the
      // rollback and destruction once the review has returned.
      if (!deform->isReviewing()) {
        delete m_dragTool;
        m_dragTool = nullptr;
      }
      m_leftButtonMousePressed = false;
      invalidate();
      return true;
    }
  }
  return SelectionTool::keyDown(event);
}

bool VectorSelectionTool::isEventAcceptable(QEvent *event) {
  if (dynamic_cast<VectorDeformTool *>(m_dragTool) &&
      static_cast<QKeyEvent *>(event)->key() == Qt::Key_Escape) return true;
  return SelectionTool::isEventAcceptable(event);
}

//-----------------------------------------------------------------------------

void VectorSelectionTool::doOnDeactivate() {
  m_strokeSelection.selectNone();
  m_levelSelection.selectNone();
  m_deformValues.reset();

  m_polyline.clear();

  TTool::getApplication()->getCurrentSelection()->setSelection(0);

  invalidate();
}

//-----------------------------------------------------------------------------

TPropertyGroup *VectorSelectionTool::getProperties(int idx) {
  switch (idx) {
  case 0:
    return &m_prop;
  case 1:
    return &m_outlineProps;
  default:
    return 0;
  };
}

//-----------------------------------------------------------------------------

bool VectorSelectionTool::onPropertyChanged(std::string propertyName) {
  if (propertyName == m_reviewFills.getName()) {
    l_strokeSelectReviewFills = m_reviewFills.getValue() ? 1 : 0;
    return true;
  }
  if (ArtLayer::onPropertyChanged(propertyName)) {
    finalizeSelection();  // Drops the strokes of a role no longer edited
    invalidate();
    return true;
  }

  if (!m_strokeSelection.isEditable()) return false;

  if (SelectionTool::onPropertyChanged(propertyName)) return true;

  if (propertyName == m_includeIntersection.getName())
    l_strokeSelectIncludeIntersection = (int)(m_includeIntersection.getValue());
  if (propertyName == m_constantThickness.getName())
    l_strokeSelectConstantThickness = (int)(m_constantThickness.getValue());
  else if (propertyName == m_envelope.getName()) {
    l_strokeSelectEnvelope = (int)(m_envelope.getValue());
    invalidate();
  } else if (propertyName == m_selectionTarget.getName())
    doOnActivate();
  else if (propertyName == m_capStyle.getName()) {
    if (m_strokeSelection.isEmpty()) return true;

    TXshSimpleLevel *level =
        TTool::getApplication()->getCurrentLevel()->getSimpleLevel();
    UndoChangeOutlineStyle *undo =
        new UndoChangeOutlineStyle(level, getCurrentFid(), this);

    int newCapStyle = m_capStyle.getIndex();

    TVectorImageP vi = m_strokeSelection.getImage();

    const std::set<int> &indices = m_strokeSelection.getSelection();

    std::set<int>::iterator it;
    for (it = indices.begin(); it != indices.end(); ++it) {
      TStroke *stroke = vi->getStroke(*it);

      stroke->outlineOptions().m_capStyle =
          (TStroke::OutlineOptions::CapStyle)newCapStyle;
      stroke->invalidate();
    }

    computeBBox();
    invalidate();

    level->setDirtyFlag(true);

    undo->registerStrokes();
    TUndoManager::manager()->add(undo);

    notifyImageChanged();
  } else if (propertyName == m_joinStyle.getName()) {
    if (m_strokeSelection.isEmpty()) return true;

    TXshSimpleLevel *level =
        TTool::getApplication()->getCurrentLevel()->getSimpleLevel();
    UndoChangeOutlineStyle *undo =
        new UndoChangeOutlineStyle(level, getCurrentFid(), this);

    int newJoinStyle = m_joinStyle.getIndex();

    TVectorImageP vi = m_strokeSelection.getImage();

    const std::set<int> &indices = m_strokeSelection.getSelection();

    std::set<int>::iterator it;
    for (it = indices.begin(); it != indices.end(); ++it) {
      TStroke *stroke = vi->getStroke(*it);

      stroke->outlineOptions().m_joinStyle =
          (TStroke::OutlineOptions::JoinStyle)newJoinStyle;
      stroke->invalidate();
    }

    computeBBox();
    invalidate();

    level->setDirtyFlag(true);

    undo->registerStrokes();
    TUndoManager::manager()->add(undo);

    notifyImageChanged();
  } else if (propertyName == m_miterJoinLimit.getName()) {
    if (m_strokeSelection.isEmpty()) return true;

    TXshSimpleLevel *level =
        TTool::getApplication()->getCurrentLevel()->getSimpleLevel();
    UndoChangeOutlineStyle *undo =
        new UndoChangeOutlineStyle(level, getCurrentFid(), this);

    int upper = m_miterJoinLimit.getValue();

    TVectorImageP vi = m_strokeSelection.getImage();

    const std::set<int> &indices = m_strokeSelection.getSelection();

    std::set<int>::iterator it;
    for (it = indices.begin(); it != indices.end(); ++it) {
      TStroke *stroke = vi->getStroke(*it);

      stroke->outlineOptions().m_miterUpper = upper;
      stroke->invalidate();
    }

    computeBBox();
    invalidate();

    level->setDirtyFlag(true);

    undo->registerStrokes();
    TUndoManager::manager()->add(undo);

    notifyImageChanged();
  } else if (propertyName == m_opacity.getName()) {
    if (m_strokeSelection.isEmpty()) return true;

    const auto current = SelectionOpacity::summarize(
        m_strokeSelection.getImage(), m_strokeSelection.getSelection());
    const double opacity = 0.01 * m_opacity.getValue();
    if (!current.count ||
        (!current.mixed && std::abs(current.value - opacity) <= 1e-9))
      return true;

    TXshSimpleLevel *level =
        TTool::getApplication()->getCurrentLevel()->getSimpleLevel();
    UndoChangeOutlineStyle *undo =
        new UndoChangeOutlineStyle(level, getCurrentFid(), this);

    TVectorImageP vi = m_strokeSelection.getImage();

    const std::set<int> &indices = m_strokeSelection.getSelection();

    std::set<int>::iterator it;
    for (it = indices.begin(); it != indices.end(); ++it) {
      TStroke *stroke = vi->getStroke(*it);

      stroke->setOpacity(opacity);
      stroke->invalidate();
    }

    computeBBox();
    invalidate();

    level->setDirtyFlag(true);

    undo->registerStrokes();
    TUndoManager::manager()->add(undo);

    notifyImageChanged();
  } else
    return false;

  return true;
}

//-----------------------------------------------------------------------------

void VectorSelectionTool::selectionOutlineStyle(int &capStyle, int &joinStyle) {
  // Check the selection's outline style group validity
  const std::set<int> &selection = m_strokeSelection.getSelection();
  if (selection.empty()) {
    capStyle = joinStyle = -1;
    return;
  }

  TVectorImageP vi = m_strokeSelection.getImage();
  const TStroke::OutlineOptions &beginOptions =
      vi->getStroke(*selection.begin())->outlineOptions();

  capStyle  = beginOptions.m_capStyle;
  joinStyle = beginOptions.m_joinStyle;

  std::set<int>::const_iterator it;
  for (it = selection.begin(); it != selection.end(); ++it) {
    const TStroke::OutlineOptions &options =
        vi->getStroke(*it)->outlineOptions();

    if (capStyle != options.m_capStyle) capStyle = -1;
    if (joinStyle != options.m_joinStyle) joinStyle = -1;
    if (capStyle < 0 && joinStyle < 0) return;
  }
}

//-----------------------------------------------------------------------------

void VectorSelectionTool::selectRegionVectorImage(bool includeIntersect) {
  if (!m_stroke) return;

  TVectorImageP vi(getImage(false));
  if (!vi) return;

  m_strokeSelection.setImage(vi);

  TVectorImage selectImg;
  selectImg.addStroke(new TStroke(*m_stroke));
  selectImg.findRegions();

  int sCount = int(vi->getStrokeCount()),
      rCount = int(selectImg.getRegionCount());

  bool selectionChanged = false;

  for (int s = 0; s != sCount; ++s) {
    TStroke *currentStroke = vi->getStroke(s);

    for (int r = 0; r != rCount; ++r) {
      TRegion *region = selectImg.getRegion(r);

      if (region->contains(*currentStroke, true))
        selectionChanged = selectStroke(s, false) || selectionChanged;
    }

    if (includeIntersect) {
      std::vector<DoublePair> intersections;
      intersect(m_stroke, currentStroke, intersections, false);
      if (intersections.size() > 0) {
        selectionChanged = selectStroke(s, false) || selectionChanged;
      }
    }
  }

  if (selectionChanged) {
    finalizeSelection();
    notifySelectionChanged();
    invalidate();
  }
}

//-----------------------------------------------------------------------------

void VectorSelectionTool::updateAction(TPointD pos, const TMouseEvent &e) {
  TImageP image    = getImage(false);
  TVectorImageP vi = (TVectorImageP)image;
  if (!vi) return;

  SelectionTool::updateAction(pos, e);
  if (m_what != Outside || m_cursorId != ToolCursor::StrokeSelectCursor) return;

  if (!m_strokeSelection.isEditable()) return;

  FourPoints bbox = getBBox();
  UINT index      = 0;

  if ((isLevelType() &&
       bbox.contains(pos))  // What about isSelectedFramesType()??
      || (getStrokeIndexFromPos(index, vi, pos, getPixelSize(),
                                getViewer()->getViewMatrix()) &&
          m_strokeSelection.isSelected(index))) {
    m_what = Inside;
    m_cursorId =
        isLevelType() ? ToolCursor::LevelSelectCursor : ToolCursor::MoveCursor;
  }
}

//-----------------------------------------------------------------------------

void VectorSelectionTool::onSelectedFramesChanged() {
  // The frames of the onion skin mode do not depend on the selected ones
  if (m_levelSelection.framesMode() ==
      LevelSelection::FRAMES_SELECTED)  // False also in case m_levelSelection
                                        // is not current
    finalizeSelection();
}

//=============================================================================
//
// sortWithPaletteOrder
//
//-----------------------------------------------------------------------------

void VectorSelectionTool::AttachedLevelSelection::sortWithPaletteOrder() {
  TXshSimpleLevel *level =
      TTool::getApplication()->getCurrentLevel()->getSimpleLevel();
  SelectionTool *tool = dynamic_cast<SelectionTool *>(
      TTool::getApplication()->getCurrentTool()->getTool());
  if (!tool) return;
  if (framesMode() == FRAMES_NONE) return;
  if (!m_strokeSelection.isEditable()) return;
  ReorderDatas reOrderDatas;

  if (framesMode() == FRAMES_CURRENT) {
    TVectorImageP vi = m_strokeSelection.getImage();
    std::vector<int> indexVector;
    for (auto index : m_strokeSelection.getSelection()) {
      if (vi->isStrokeGrouped(index)) continue;
      indexVector.push_back(index);
    }
    if (!indexVector.empty()) {
      ReorderData data = {tool->getCurrentFid(), std::move(indexVector), {}};
      reOrderDatas.push_back(data);
    }
  } else {
    std::set<TFrameId> fids;
    if (framesMode() == FRAMES_ALL)
      for (int i = 0; i < level->getFrameCount(); ++i)
        fids.insert(level->getFrameId(i));
    else if (framesMode() == FRAMES_ONION_SKIN)
      fids = onionSkinFrames();
    else  // FRAMES_SELECTED
      fids = getSelectedFrames();
    for (auto id : fids) {
      TVectorImageP vi = level->getFrame(id, true);
      if (!vi) continue;
      std::vector<int> indexVector;
      for (int i = 0; i < vi->getStrokeCount(); ++i) {
        if (vi->isStrokeGrouped(i)) continue;
        if (filter() == SELECTED_STYLES &&
            this->styles().find(vi->getStroke(i)->getStyle()) ==
                this->styles().end())
          continue;
        indexVector.push_back(i);
      }
      if (!indexVector.empty())
        reOrderDatas.push_back({id, std::move(indexVector), {}});
    }
  };

  if (reOrderDatas.empty()) return;
  for (ReorderData &data : reOrderDatas) {
    TPalette *palette = m_strokeSelection.getImage()->getPalette();
    TVectorImageP vi  = level->getFrame(data.frameId, true);
    if (!vi) continue;

    QHash<int, int> originalIndexToStyle;
    std::set<int> styles;
    for (auto index : data.oldIndexes) {
      TStroke *stroke = vi->getStroke(index);
      int styleId     = stroke->getStyle();
      styles.insert(stroke->getStyle());
      originalIndexToStyle.insert(index, styleId);
    }
    // Sort Styles
    QVector<int> styleOrder = QVector<int>(styles.begin(), styles.end());
    sortStylesIds(styleOrder, palette);
    // Build map from style to it's order
    QHash<int, int> styleIndexMap;
    for (int i = 0; i < styleOrder.size(); ++i) {
      styleIndexMap[styleOrder[i]] = i;
    }
    // Sort indexes by palette order
    std::vector<int> newOrder, oldOrder;
    newOrder = oldOrder = data.oldIndexes;
    std::stable_sort(newOrder.begin(), newOrder.end(), [&](int a, int b) {
      return styleIndexMap.value(originalIndexToStyle[a], -1) >
             styleIndexMap.value(originalIndexToStyle[b], -1);
    });
    // Remove don't need indexes
    data.oldIndexes.clear();
    data.newIndexes.clear();
    for (int i = 0; i < oldOrder.size(); i++) {
      if (oldOrder[i] != newOrder[i]) {
        data.oldIndexes.push_back(newOrder[i]);
        data.newIndexes.push_back(oldOrder[i]);
      }
    }
    // Do reorder
    if (data.oldIndexes.empty()) continue;
    vi->reOrderStrokes(data.oldIndexes, data.newIndexes);
  }

  reOrderDatas.erase(std::remove_if(reOrderDatas.begin(), reOrderDatas.end(),
                                    [](const ReorderData &data) {
                                      return data.oldIndexes.empty();
                                    }),
                     reOrderDatas.end());

  if (reOrderDatas.empty()) return;
  TUndoManager::manager()->add(new UndoReorderStrokes(level, reOrderDatas));

  tool->notifyImageChanged();
}
