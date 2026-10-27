#pragma once

#ifndef TINBETWEENMATCH_H
#define TINBETWEENMATCH_H

// Pure helpers behind the smarter morphing options of TInbetween (automatic
// stroke matching, zone hints and the custom ease curve). Kept free of any
// toonz type, so that they can be unit tested without linking tvrender.
// See toonz/sources/tnztools/tests/tinbetweenmatch_test.cpp.

#include <algorithm>
#include <cmath>
#include <limits>
#include <tuple>
#include <utility>
#include <vector>

namespace TInbetweenMatch {

//-----------------------------------------------------------------------------
// Automatic matching
//-----------------------------------------------------------------------------

//! Shape descriptor of a stroke.
struct StrokeInfo {
  double m_cx     = 0;  //!< centroid (mean of the control points)
  double m_cy     = 0;
  double m_width  = 0;  //!< bounding box size
  double m_height = 0;
  double m_length = 0;  //!< length of the stroke
  int m_styleId   = 0;
};

//! Weights of the terms of the matching cost. Distances are expressed in
//! fractions of the bounding box diagonal of the two images, so that the
//! cost does not depend on the drawing scale.
struct Weights {
  double m_position      = 1.0;   //!< centroid distance
  double m_size          = 0.5;   //!< bounding box size difference
  double m_length        = 0.5;   //!< relative length difference
  double m_styleMismatch = 0.25;  //!< added when the style ids differ, i.e.
                                  //!< the equal style bonus
};

//! Cost of matching stroke \b a with stroke \b b; always finite and >= 0.
//! \b diagonal is the bounding box diagonal of the two images.
inline double matchCost(const StrokeInfo &a, const StrokeInfo &b,
                        double diagonal, const Weights &w = Weights()) {
  if (!(diagonal > 1e-9)) diagonal = 1.0;

  double position = std::hypot(a.m_cx - b.m_cx, a.m_cy - b.m_cy) / diagonal;
  double size =
      (std::fabs(a.m_width - b.m_width) + std::fabs(a.m_height - b.m_height)) /
      diagonal;
  double maxLength = std::max(a.m_length, b.m_length);
  double length =
      maxLength > 1e-9 ? std::fabs(a.m_length - b.m_length) / maxLength : 0.0;

  double cost = w.m_position * position + w.m_size * size +
                w.m_length * length +
                (a.m_styleId != b.m_styleId ? w.m_styleMismatch : 0.0);
  return std::isfinite(cost) ? std::max(cost, 0.0) : 1e12;
}

//! Minimum cost assignment of \b rows items to \b cols items, with the cost
//! given as a row-major matrix (cost[row * cols + col]). Both counts can be
//! different: min(rows, cols) items get a partner, the others get -1.
//! Returns, for each row, the assigned column or -1.
//! This is the Hungarian method (shortest augmenting paths with potentials),
//! O(min(rows, cols)^2 * max(rows, cols)); the result is optimal, not greedy.
inline std::vector<int> assignMinCost(const std::vector<double> &cost, int rows,
                                      int cols) {
  std::vector<int> result(rows > 0 ? rows : 0, -1);
  if (rows <= 0 || cols <= 0 || cost.size() < size_t(rows) * size_t(cols))
    return result;

  // the algorithm needs n <= m: transpose when there are more rows
  const bool transposed = rows > cols;
  const int n           = transposed ? cols : rows;
  const int m           = transposed ? rows : cols;
  auto c                = [&](int i, int j) {
    double v =
        transposed ? cost[size_t(j) * cols + i] : cost[size_t(i) * cols + j];
    return std::isfinite(v) ? v : 1e12;
  };

  const double inf = std::numeric_limits<double>::max();
  std::vector<double> u(n + 1, 0.0), v(m + 1, 0.0);
  std::vector<int> p(m + 1, 0), way(m + 1, 0);  // p[j]: row assigned to col j
  for (int i = 1; i <= n; i++) {
    p[0]   = i;
    int j0 = 0;
    std::vector<double> minv(m + 1, inf);
    std::vector<char> used(m + 1, 0);
    do {
      used[j0]     = 1;
      int i0       = p[j0];
      int j1       = 0;
      double delta = inf;
      for (int j = 1; j <= m; j++) {
        if (used[j]) continue;
        double cur = c(i0 - 1, j - 1) - u[i0] - v[j];
        if (cur < minv[j]) {
          minv[j] = cur;
          way[j]  = j0;
        }
        if (minv[j] < delta) {
          delta = minv[j];
          j1    = j;
        }
      }
      for (int j = 0; j <= m; j++) {
        if (used[j]) {
          u[p[j]] += delta;
          v[j] -= delta;
        } else
          minv[j] -= delta;
      }
      j0 = j1;
    } while (p[j0] != 0);
    do {
      int j1 = way[j0];
      p[j0]  = p[j1];
      j0     = j1;
    } while (j0);
  }

  for (int j = 1; j <= m; j++) {
    if (p[j] == 0) continue;
    if (transposed)
      result[j - 1] = p[j] - 1;
    else
      result[p[j] - 1] = j - 1;
  }
  return result;
}

//-----------------------------------------------------------------------------
// Zone hints
//-----------------------------------------------------------------------------

struct Point {
  double x = 0, y = 0;
};

struct Rect {
  double x0 = 0, y0 = 0, x1 = 0, y1 = 0;
  bool contains(const Point &p) const {
    return x0 <= p.x && p.x <= x1 && y0 <= p.y && p.y <= y1;
  }
};

//! Matches the points \b a inside \b zoneA with the points \b b inside
//! \b zoneB (e.g. stroke centroids). The nearest pairs are matched first,
//! measuring the distance of each point from the center of its zone, until
//! one of the two sets is exhausted. Points already flagged in \b usedA /
//! \b usedB (which must have the size of \b a / \b b) are skipped; the
//! matched ones get flagged. Returns (index in a, index in b) pairs.
inline std::vector<std::pair<int, int>> zoneMatch(
    const std::vector<Point> &a, const std::vector<Point> &b, const Rect &zoneA,
    const Rect &zoneB, std::vector<bool> &usedA, std::vector<bool> &usedB) {
  const double ax = (zoneA.x0 + zoneA.x1) * 0.5,
               ay = (zoneA.y0 + zoneA.y1) * 0.5;
  const double bx = (zoneB.x0 + zoneB.x1) * 0.5,
               by = (zoneB.y0 + zoneB.y1) * 0.5;

  // (squared distance, index in a, index in b)
  std::vector<std::tuple<double, int, int>> candidates;
  for (int i = 0; i < int(a.size()); i++) {
    if (usedA[i] || !zoneA.contains(a[i])) continue;
    for (int j = 0; j < int(b.size()); j++) {
      if (usedB[j] || !zoneB.contains(b[j])) continue;
      double dx = (a[i].x - ax) - (b[j].x - bx);
      double dy = (a[i].y - ay) - (b[j].y - by);
      candidates.push_back(std::make_tuple(dx * dx + dy * dy, i, j));
    }
  }
  std::sort(candidates.begin(), candidates.end());

  std::vector<std::pair<int, int>> pairs;
  for (const auto &candidate : candidates) {
    int i = std::get<1>(candidate), j = std::get<2>(candidate);
    if (usedA[i] || usedB[j]) continue;
    usedA[i] = true;
    usedB[j] = true;
    pairs.push_back(std::make_pair(i, j));
  }
  return pairs;
}

//-----------------------------------------------------------------------------
// Custom ease curve
//-----------------------------------------------------------------------------

//! Velocity curve of the Custom interpolation: the cubic Bezier with control
//! points (0,0) (slowStart,0) (1-slowEnd,1) (1,1), evaluated at the time
//! \b t, i.e. the y for which x == t. Both strengths are in [0,1] (they get
//! clamped): 0 / 0 is the linear interpolation, a higher slowStart makes the
//! motion start more gently, a higher slowEnd makes it land more gently.
//! s(0) = 0 and s(1) = 1, and s is increasing.
inline double customEase(double t, double slowStart, double slowEnd) {
  if (!(t > 0.0)) return 0.0;
  if (!(t < 1.0)) return 1.0;
  const double a = std::min(std::max(slowStart, 0.0), 1.0);
  const double b = std::min(std::max(slowEnd, 0.0), 1.0);

  // x(u) is monotonic for a, b in [0,1], so it can be inverted by bisection
  auto x = [&](double u) {
    double w = 1.0 - u;
    return 3.0 * w * w * u * a + 3.0 * w * u * u * (1.0 - b) + u * u * u;
  };
  double lo = 0.0, hi = 1.0;
  for (int i = 0; i < 50; i++) {
    double mid = (lo + hi) * 0.5;
    if (x(mid) < t)
      lo = mid;
    else
      hi = mid;
  }
  double u = (lo + hi) * 0.5, w = 1.0 - u;
  return 3.0 * w * u * u + u * u * u;
}

}  // namespace TInbetweenMatch

#endif
