#pragma once

#ifndef ENVELOPE_LAYOUT_INCLUDED
#define ENVELOPE_LAYOUT_INCLUDED

// Pure maths behind the Envelope mode of the vector Selection tool. It has no
// dependency on tools, images or panels, so it can be tested on its own.
//
// In the Envelope mode the box around the selection is replaced by a closed
// polygon, the envelope, that runs along the outline of the box. Its vertices
// are the handles the user drags. The artwork is then warped with the mean
// value coordinates of that polygon (see TEnvelopeDeformer), which carry the
// art on the outline exactly to the moved outline.
//
// This header builds the handles from a box, tells which handles are next to
// each other, decides if a moved envelope may be used (it must not fold over
// itself, or the art would turn inside out), and measures how much a line has
// to become thicker or thinner where the art is stretched or squeezed.

#include "tgeometry.h"

#include <algorithm>
#include <cmath>
#include <vector>

namespace EnvelopeLayout {

//! Segments on each side of the box in the layout the tool uses. Two means a
//! handle on each corner and one in the middle of each side: 8 handles.
const int kDefaultSegmentsPerSide = 2;

//! Returned where no handle is meant.
const int kNoHandle = -1;

//! The deformed envelope may not be smaller than this fraction of the area of
//! the rest envelope.
const double kMinAreaRatio = 1e-3;

//! A rest envelope must have at least this fraction of the area of its
//! diagonal squared (the area ratio of a flat sliver is near zero).
const double kMinRestAreaRatio = 1e-6;

//! Line thickness is multiplied by a factor between these limits, whatever the
//! stretching, so that a corner of the envelope cannot produce huge or
//! vanishing lines.
const double kMinThicknessFactor = 0.1;
const double kMaxThicknessFactor = 10.0;

//! The step used to estimate the derivatives of the map, as a fraction of the
//! diagonal of the envelope.
const double kJacobianStepFraction = 0.002;

//! A chunk of a stroke is cut in pieces that are about this fraction of the
//! diagonal of the envelope, and in no more than kMaxPieces pieces.
const double kPieceFraction = 1.0 / 16.0;
const int kMaxPieces        = 16;

//=============================================================================
// Layout
//=============================================================================

//! Number of handles of a layout with \b segmentsPerSide segments per side.
inline int handleCount(int segmentsPerSide = kDefaultSegmentsPerSide) {
  return 4 * std::max(1, segmentsPerSide);
}

//! The handles along the outline of the quadrilateral with corners \b c0
//! (bottom left), \b c1 (bottom right), \b c2 (top right) and \b c3 (top left).
//! This is the order of the corners of the Selection tool box, and it runs
//! counter clockwise when y points up. Each side is cut in \b segmentsPerSide
//! equal segments. Handle 0 is on \b c0 and the handles follow the outline
//! counter clockwise. With the default two segments the 8 handles are: bottom
//! left, middle of the bottom side, bottom right, middle of the right side, top
//! right, middle of the top side, top left, middle of the left side.
inline std::vector<TPointD> perimeter(
    const TPointD &c0, const TPointD &c1, const TPointD &c2, const TPointD &c3,
    int segmentsPerSide = kDefaultSegmentsPerSide) {
  const int n              = std::max(1, segmentsPerSide);
  const TPointD corners[4] = {c0, c1, c2, c3};

  std::vector<TPointD> handles;
  handles.reserve(4 * n);
  for (int side = 0; side < 4; ++side) {
    const TPointD &a = corners[side];
    const TPointD &b = corners[(side + 1) % 4];
    for (int k = 0; k < n; ++k) {
      const double t = double(k) / n;
      handles.push_back(a + (b - a) * t);
    }
  }
  return handles;
}

//! The handles along the outline of the axis aligned box from \b bottomLeft to
//! \b topRight.
inline std::vector<TPointD> perimeter(
    const TPointD &bottomLeft, const TPointD &topRight,
    int segmentsPerSide = kDefaultSegmentsPerSide) {
  return perimeter(bottomLeft, TPointD(topRight.x, bottomLeft.y), topRight,
                   TPointD(bottomLeft.x, topRight.y), segmentsPerSide);
}

//! True if the handle \b index sits on a corner of the box.
inline bool isCorner(int index, int segmentsPerSide = kDefaultSegmentsPerSide) {
  const int n = std::max(1, segmentsPerSide);
  return index >= 0 && index < 4 * n && index % n == 0;
}

//! \b index moved by \b step handles along the outline, wrapping around.
inline int neighbour(int index, int step, int count) {
  if (count <= 0) return 0;
  int r = (index + step) % count;
  return (r < 0) ? r + count : r;
}

//! The handle after \b index (counter clockwise) among \b count handles.
inline int nextIndex(int index, int count) {
  return neighbour(index, 1, count);
}

//! The handle before \b index (clockwise) among \b count handles.
inline int previousIndex(int index, int count) {
  return neighbour(index, -1, count);
}

//! The Selection tool numbers the handles of its box 0 to 3 for the corners
//! (bottom left, bottom right, top right, top left) and 4 to 7 for the middle
//! of the sides (bottom, right, top, left). This returns the index of the same
//! handle in the default layout of perimeter(), or kNoHandle for any other
//! number.
inline int handleFromToolPoint(int toolPoint) {
  if (toolPoint >= 0 && toolPoint < 4) return 2 * toolPoint;
  if (toolPoint >= 4 && toolPoint < 8) return 2 * (toolPoint - 4) + 1;
  return kNoHandle;
}

//! The opposite of handleFromToolPoint(), for the default layout.
inline int toolPointFromHandle(int handle) {
  if (handle < 0 || handle >= 8) return kNoHandle;
  return (handle % 2 == 0) ? handle / 2 : 4 + handle / 2;
}

//! The handle of \b handles that is nearest to \b pos and not farther than
//! \b maxDistance, or kNoHandle.
inline int findHandle(const std::vector<TPointD> &handles, const TPointD &pos,
                      double maxDistance) {
  int best         = kNoHandle;
  double bestDist2 = maxDistance * maxDistance;
  for (int i = 0; i < (int)handles.size(); ++i) {
    const double d2 = norm2(handles[i] - pos);
    if (best == kNoHandle ? d2 <= bestDist2 : d2 < bestDist2) {
      best      = i;
      bestDist2 = d2;
    }
  }
  return best;
}

//=============================================================================
// Validation
//=============================================================================

//! True if both coordinates are finite numbers.
inline bool isFinitePoint(const TPointD &p) {
  return std::isfinite(p.x) && std::isfinite(p.y);
}

//! True if every point is finite.
inline bool allFinite(const std::vector<TPointD> &polygon) {
  for (const TPointD &p : polygon)
    if (!isFinitePoint(p)) return false;
  return true;
}

//! Signed area of the polygon: positive when its vertices run counter
//! clockwise (y pointing up).
inline double signedArea(const std::vector<TPointD> &polygon) {
  const int n = (int)polygon.size();
  double area = 0.0;
  for (int i = 0; i < n; ++i) area += cross(polygon[i], polygon[(i + 1) % n]);
  return 0.5 * area;
}

//! Length of the diagonal of the bounding box of the polygon.
inline double diagonal(const std::vector<TPointD> &polygon) {
  if (polygon.empty()) return 0.0;
  TPointD p0 = polygon[0], p1 = polygon[0];
  for (const TPointD &p : polygon) {
    p0.x = std::min(p0.x, p.x);
    p0.y = std::min(p0.y, p.y);
    p1.x = std::max(p1.x, p.x);
    p1.y = std::max(p1.y, p.y);
  }
  return norm(p1 - p0);
}

namespace Detail {

// 1 if c is left of the line from a to b, -1 if it is right, 0 if it is on the
// line (within eps).
inline int orientation(const TPointD &a, const TPointD &b, const TPointD &c,
                       double eps) {
  const double v = cross(b - a, c - a);
  return (v > eps) ? 1 : ((v < -eps) ? -1 : 0);
}

// For c on the line through a and b: is it on the segment?
inline bool onSegment(const TPointD &a, const TPointD &b, const TPointD &c) {
  return std::min(a.x, b.x) <= c.x && c.x <= std::max(a.x, b.x) &&
         std::min(a.y, b.y) <= c.y && c.y <= std::max(a.y, b.y);
}

// True if the segments ab and cd have a point in common.
inline bool segmentsIntersect(const TPointD &a, const TPointD &b,
                              const TPointD &c, const TPointD &d, double eps) {
  const int o1 = orientation(a, b, c, eps);
  const int o2 = orientation(a, b, d, eps);
  const int o3 = orientation(c, d, a, eps);
  const int o4 = orientation(c, d, b, eps);
  if (o1 != o2 && o3 != o4) return true;
  if (o1 == 0 && onSegment(a, b, c)) return true;
  if (o2 == 0 && onSegment(a, b, d)) return true;
  if (o3 == 0 && onSegment(c, d, a)) return true;
  if (o4 == 0 && onSegment(c, d, b)) return true;
  return false;
}

}  // namespace Detail

//! True if the closed polygon does not cross or touch itself: no two edges meet
//! (except consecutive edges at their common vertex), and no edge folds back on
//! the one before it. Needs at least 3 finite vertices.
inline bool isSimple(const std::vector<TPointD> &polygon) {
  const int n = (int)polygon.size();
  if (n < 3 || !allFinite(polygon)) return false;

  const double size = diagonal(polygon);
  const double eps  = 1e-12 * size * size;

  // A vertex where the polygon goes back over the edge it came along
  for (int v = 0; v < n; ++v) {
    const TPointD &a = polygon[(v + n - 1) % n];
    const TPointD &b = polygon[v];
    const TPointD &c = polygon[(v + 1) % n];
    if (Detail::orientation(a, b, c, eps) == 0 && (b - a) * (c - b) < 0.0)
      return false;
  }

  for (int i = 0; i < n; ++i) {
    for (int j = i + 1; j < n; ++j) {
      // Consecutive edges share a vertex
      if (j == i + 1 || (i == 0 && j == n - 1)) continue;
      if (Detail::segmentsIntersect(polygon[i], polygon[(i + 1) % n],
                                    polygon[j], polygon[(j + 1) % n], eps))
        return false;
    }
  }
  return true;
}

//! True if the polygon can be the rest envelope: it has at least 3 finite
//! vertices, it does not cross itself, and it is not a sliver with an almost
//! null area.
inline bool isUsableRest(const std::vector<TPointD> &polygon) {
  if (polygon.size() < 3 || !allFinite(polygon)) return false;

  const double size = diagonal(polygon);
  if (!(size > 0.0) || !std::isfinite(size)) return false;
  if (!(std::fabs(signedArea(polygon)) > kMinRestAreaRatio * size * size))
    return false;

  return isSimple(polygon);
}

//! True if the envelope \b deformed, moved from \b rest, may be used to warp
//! the art: it has as many vertices as \b rest, they are all finite, no two
//! consecutive ones coincide, it does not cross itself, it runs in the same
//! direction as \b rest (it is not turned inside out) and its area is not
//! almost null compared with the one of \b rest.
inline bool isValidDeformation(const std::vector<TPointD> &rest,
                               const std::vector<TPointD> &deformed) {
  if (!isUsableRest(rest) || deformed.size() != rest.size()) return false;
  if (!allFinite(deformed)) return false;

  const double restArea = signedArea(rest);
  const double area     = signedArea(deformed);
  if (restArea * area <= 0.0) return false;  // Turned inside out, or flat
  if (!(std::fabs(area) >= kMinAreaRatio * std::fabs(restArea))) return false;

  const int n         = (int)deformed.size();
  const double merge2 = 1e-12 * std::max(diagonal(rest), diagonal(deformed)) *
                        std::max(diagonal(rest), diagonal(deformed));
  for (int i = 0; i < n; ++i)
    if (norm2(deformed[i] - deformed[(i + 1) % n]) <= merge2) return false;

  return isSimple(deformed);
}

//! The place for handle \b index of the envelope \b current that is nearest to
//! \b target while \b current moved there stays a valid deformation of
//! \b rest. \b current is supposed to be valid. When \b target is not valid the
//! handle stops where the path from its current place to \b target meets the
//! limit, so that dragging past it slides along instead of jumping.
inline TPointD constrainMove(const std::vector<TPointD> &rest,
                             std::vector<TPointD> current, int index,
                             const TPointD &target) {
  if (index < 0 || index >= (int)current.size()) return TPointD();

  const TPointD from = current[index];
  if (!isFinitePoint(target)) return from;

  current[index] = target;
  if (isValidDeformation(rest, current)) return target;

  double lo = 0.0, hi = 1.0;  // lo is valid (from), hi is not (target)
  for (int i = 0; i < 24; ++i) {
    const double mid = 0.5 * (lo + hi);
    current[index]   = from + (target - from) * mid;
    if (isValidDeformation(rest, current))
      lo = mid;
    else
      hi = mid;
  }
  return from + (target - from) * lo;
}

//=============================================================================
// Thickness
//=============================================================================

//! How many times a small area around the point \b p grows under \b map, which
//! is a callable taking a TPointD and returning the TPointD it goes to. The
//! derivatives are estimated with a central difference of half width \b step.
//! Returns 1 when the estimate is not a finite number or \b step is not
//! positive.
template <class Map>
double areaScale(const Map &map, const TPointD &p, double step) {
  if (!(step > 0.0) || !std::isfinite(step)) return 1.0;

  const TPointD dx =
      map(TPointD(p.x + step, p.y)) - map(TPointD(p.x - step, p.y));
  const TPointD dy =
      map(TPointD(p.x, p.y + step)) - map(TPointD(p.x, p.y - step));
  const double area = std::fabs(cross(dx, dy)) / (4.0 * step * step);
  return std::isfinite(area) ? area : 1.0;
}

//! Factor to apply to the thickness of a line at the point \b p: the square
//! root of the local area scale (a 2x stretch both ways makes 4x the area, so
//! the line becomes 2x thicker), limited to the range from kMinThicknessFactor
//! to kMaxThicknessFactor. Always a finite number.
template <class Map>
double thicknessFactor(const Map &map, const TPointD &p, double step) {
  const double f = std::sqrt(areaScale(map, p, step));
  if (!std::isfinite(f)) return 1.0;
  return std::min(kMaxThicknessFactor, std::max(kMinThicknessFactor, f));
}

//=============================================================================
// Subdivision
//=============================================================================

//! In how many pieces to cut a chunk of a stroke before the stroke is warped,
//! so that curves bend smoothly: the chunk has about \b chunkLength (the
//! length of its control polygon) and the envelope has the diagonal
//! \b envelopeDiagonal. From 1 (the chunk is kept) to kMaxPieces.
inline int piecesForChunk(double chunkLength, double envelopeDiagonal) {
  if (!(chunkLength > 0.0) || !(envelopeDiagonal > 0.0) ||
      !std::isfinite(chunkLength) || !std::isfinite(envelopeDiagonal))
    return 1;

  const double pieces =
      std::ceil(chunkLength / (kPieceFraction * envelopeDiagonal));
  if (!(pieces > 1.0)) return 1;
  if (pieces >= kMaxPieces) return kMaxPieces;
  return (int)pieces;
}

}  // namespace EnvelopeLayout

#endif  // ENVELOPE_LAYOUT_INCLUDED
