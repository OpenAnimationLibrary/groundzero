#pragma once

#include "tvectorimage.h"
#include "tstroke.h"
#include "tregion.h"
#include "tools/artlayer.h"
#include "toonz/contouroutline.h"
#include <QPainterPath>
#include <QPolygonF>
#include <algorithm>
#include <memory>
#include <set>
#include <utility>
#include <vector>

// Native vector clipping geometry. Region boundaries remain quadratic; Qt's
// path intersection flattens only the resulting clipping contours. No raster
// mask is stored in a drawing or used to decide what the artist painted.
namespace VectorPaintClip {
inline QPainterPath boundary(const TRegion &region) {
  QPainterPath path;
  bool first = true;
  for (UINT e = 0; e < region.getEdgeCount(); ++e) {
    const TEdge *edge = region.getEdge(e);
    if (!edge || !edge->m_s) continue;
    int a = 0, b = 0; double ta = 0, tb = 0;
    if (edge->m_s->getChunkAndT(edge->m_w0, a, ta) ||
        edge->m_s->getChunkAndT(edge->m_w1, b, tb)) continue;
    const bool reverse = a > b || (a == b && ta > tb);
    for (int i = a;; i += reverse ? -1 : 1) {
      const auto *q = edge->m_s->getChunk(i);
      const double from = i == a ? ta : (reverse ? 1. : 0.);
      const double to = i == b ? tb : (reverse ? 0. : 1.);
      const TPointD p = q->getPoint(from), end = q->getPoint(to);
      const TPointD control = p + q->getSpeed(from) * ((to - from) * .5);
      if (first) path.moveTo(p.x, p.y), first = false;
      else path.lineTo(p.x, p.y); // native autoclose gaps
      path.quadTo(control.x, control.y, end.x, end.y);
      if (i == b) break;
    }
  }
  path.closeSubpath();
  return path;
}
inline QPainterPath regionArea(const TRegion &region) {
  QPainterPath result = boundary(region);
  for (UINT i = 0; i < region.getSubregionCount(); ++i)
    result = result.subtracted(boundary(*region.getSubregion(i)));
  return result;
}
inline QPainterPath outline(const TStroke &stroke) {
  std::vector<std::vector<TPointD>> contours;
  if (!ContourOutline::computeOutlinePolygons(stroke, contours)) return {};
  QPainterPath path;
  path.setFillRule(Qt::OddEvenFill);
  for (const auto &points : contours) {
    for (size_t i = 0; i < points.size(); ++i)
      if (i == 0) path.moveTo(points[i].x, points[i].y);
      else path.lineTo(points[i].x, points[i].y);
    path.closeSubpath();
  }
  return path;
}
inline QPainterPath selectedArea(const TVectorImageP &source,
                                const std::set<int> &indices) {
  // Region discovery may round stroke coordinates. Mask capture/validation
  // is a readback operation, so run it on a detached image. Group membership
  // survives clone; entered-group filtering has already used the source.
  TVectorImageP image = source->clone();
  QPainterPath area;
  for (int index : indices) area = area.united(outline(*image->getStroke(index)));
  image->findRegions();
  std::vector<std::pair<TRegion *, bool>> regions;
  for (UINT i = 0; i < image->getRegionCount(); ++i)
    regions.emplace_back(image->getRegion(i), false);
  while (!regions.empty()) {
    auto entry = regions.back(); regions.pop_back();
    auto *region = entry.first;
    bool allSelected = region->getEdgeCount() > 0;
    for (UINT i = 0; i < region->getEdgeCount(); ++i) {
      const auto *edge = region->getEdge(i);
      if (!edge || edge->m_index < 0 || !indices.count(edge->m_index)) allSelected = false;
    }
    // An unfilled child of a selected closed shape is its hole. Selecting
    // both boundaries must not add that hole back to the painting mask.
    if (allSelected && (!entry.second || region->getStyle() != 0))
      area = area.united(regionArea(*region));
    for (UINT i = 0; i < region->getSubregionCount(); ++i)
      regions.emplace_back(region->getSubregion(i), entry.second || allSelected);
  }
  return area;
}
struct SelectionMask {
  TVectorImageP image;
  QPainterPath area;
  std::vector<std::shared_ptr<TStroke>> strokes;
  std::vector<int> depths;
  int enteredDepth = 0;
};
inline SelectionMask &selectionMask() { static SelectionMask mask; return mask; }
inline void captureSelection(const TVectorImageP &image, const std::set<int> &indices) {
  auto &mask = selectionMask(); mask = SelectionMask(); mask.image = image;
  if (!image || indices.empty()) return;
  ArtLayer::Filter filter(*image);
  std::set<int> editable;
  for (int index : indices) {
    if (index < 0 || index >= int(image->getStrokeCount()) ||
        !image->isEnteredGroupStroke(index) || !filter.isEditable(index)) continue;
    editable.insert(index);
  }
  mask.area = selectedArea(image, editable);
  mask.enteredDepth = image->isInsideGroup();
  // Anchor the untouched live geometry; later paint additions may change
  // indices but not these identities.
  for (int index : editable) {
    mask.strokes.emplace_back(new TStroke(*image->getStroke(index)));
    mask.depths.push_back(image->getGroupDepth(index));
  }
}
inline QPainterPath selectionArea(const TVectorImageP &destination,
                                  bool sameAreaOnAllDrawings = false) {
  const auto &mask = selectionMask();
  if (!destination || !mask.image || mask.area.isEmpty() ||
      (!sameAreaOnAllDrawings && destination != mask.image) ||
      mask.enteredDepth != mask.image->isInsideGroup()) return {};
  ArtLayer::Filter filter(*mask.image);
  std::set<int> indices;
  for (size_t a = 0; a < mask.strokes.size(); ++a) {
    const auto &original = *mask.strokes[a];
    int index = mask.image->getStrokeIndexById(original.getId());
    if (index < 0 || !filter.isEditable(index) ||
        !mask.image->isEnteredGroupStroke(index) ||
        mask.image->getGroupDepth(index) != mask.depths[a]) return {};
    const auto &now = *mask.image->getStroke(index);
    if (now.getControlPointCount() != original.getControlPointCount() ||
        now.isSelfLoop() != original.isSelfLoop()) return {};
    const auto &aOptions = now.outlineOptions();
    const auto &bOptions = original.outlineOptions();
    if (aOptions.m_capStyle != bOptions.m_capStyle ||
        aOptions.m_joinStyle != bOptions.m_joinStyle ||
        aOptions.m_miterLower != bOptions.m_miterLower ||
        aOptions.m_miterUpper != bOptions.m_miterUpper) return {};
    for (int i = 0; i < now.getControlPointCount(); ++i)
      if (now.getControlPoint(i) != original.getControlPoint(i)) return {};
    indices.insert(index);
  }
  // Fill topology and holes can change without changing the selected
  // centerlines (for example after editing an intersecting boundary).
  if (selectedArea(mask.image, indices) != mask.area) return {};
  return mask.area;
}
inline QPainterPath inside(const TVectorImageP &source, const TPointD &start) {
  if (!source) return {};
  TVectorImageP image = source->clone();
  image->findRegions();
  auto *region = image->getRegion(start);
  if (!region || region->getStyle() == 0) return {};
  ArtLayer::Filter filter(*source);
  for (UINT i = 0; i < region->getEdgeCount(); ++i) {
    if (!region->getEdge(i)) return {};
    int index = region->getEdge(i)->m_index;
    if (index >= int(image->getStrokeCount())) return {};
    if (index >= 0 && (!source->isEnteredGroupStroke(index) || !filter.isEditable(index))) return {};
  }
  return regionArea(*region);
}
} // namespace VectorPaintClip
