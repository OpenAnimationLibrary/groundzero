#pragma once

#include "tvectorimage.h"
#include "tstroke.h"
#include "tregion.h"
#include "tpalette.h"
#include "tcolorstyles.h"

namespace VectorImageSnapshot {
inline bool sameRegion(const TRegion *a, const TRegion *b) {
  if (a->getStyle() != b->getStyle() ||
      a->getEdgeCount() != b->getEdgeCount() ||
      a->getSubregionCount() != b->getSubregionCount()) return false;
  for (UINT i = 0; i < a->getEdgeCount(); ++i) {
    const auto *x = a->getEdge(i), *y = b->getEdge(i);
    if (x->m_index != y->m_index || x->m_w0 != y->m_w0 ||
        x->m_w1 != y->m_w1 || x->m_styleId != y->m_styleId) return false;
  }
  for (int i = 0; i < a->getSubregionCount(); ++i)
    if (!sameRegion(a->getSubregion(i), b->getSubregion(i))) return false;
  return true;
}

// A modal event loop may run a script or reload an asset. Check every reviewed
// source, including unchanged strokes/fills/groups, before the first live edit.
// Region computation is confined to detached clones even during validation.
inline bool sameContent(const TVectorImageP &live, const TVectorImageP &before) {
  if (!live || live->getStrokeCount() != before->getStrokeCount() ||
      live->isInsideGroup() != before->isInsideGroup() ||
      live->getAutocloseTolerance() != before->getAutocloseTolerance()) return false;
  for (UINT i = 0; i < live->getStrokeCount(); ++i) {
    const auto *a = live->getStroke(i), *b = before->getStroke(i);
    const auto &x = a->outlineOptions(), &y = b->outlineOptions();
    if (a->getId() != b->getId() || *a != *b || a->getStyle() != b->getStyle() ||
        a->isSelfLoop() != b->isSelfLoop() ||
        x.m_capStyle != y.m_capStyle || x.m_joinStyle != y.m_joinStyle ||
        x.m_miterLower != y.m_miterLower || x.m_miterUpper != y.m_miterUpper ||
        x.m_patternFrameOffset != y.m_patternFrameOffset ||
        x.m_patternFrameStep != y.m_patternFrameStep || x.m_opacity != y.m_opacity ||
        x.m_opacityProfile != y.m_opacityProfile ||
        live->getGroupDepth(i) != before->getGroupDepth(i) ||
        live->getGroupByStroke(i) != before->getGroupByStroke(i) ||
        live->isEnteredGroupStroke(i) != before->isEnteredGroupStroke(i) ||
        (i && live->getCommonGroupDepth(i-1, i) != before->getCommonGroupDepth(i-1, i)))
      return false;
  }
  const auto *a = live->getPalette(), *b = before->getPalette();
  if (bool(a) != bool(b) || (a && a->getStyleCount() != b->getStyleCount())) return false;
  if (a) for (int i = 0; i < a->getStyleCount(); ++i)
    if (!(*a->getStyle(i) == *b->getStyle(i))) return false;
  TVectorImageP current = live->cloneWithGroupContext();
  current->findRegions();
  if (current->getRegionCount() != before->getRegionCount()) return false;
  for (UINT i = 0; i < current->getRegionCount(); ++i)
    if (!sameRegion(current->getRegion(i), before->getRegion(i))) return false;
  return true;
}
}  // namespace VectorImageSnapshot
