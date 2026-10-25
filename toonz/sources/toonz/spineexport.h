#pragma once

#ifndef SPINEEXPORT_H
#define SPINEEXPORT_H

#include <algorithm>
#include <cmath>
#include <map>
#include <utility>
#include <vector>

//=============================================================================
// SpineExport
//-----------------------------------------------------------------------------
// Pure math helpers of the "Export to Spine JSON" command (spineexport.cpp),
// kept free of OpenToonz and Qt dependencies so that they can be checked
// standalone (see tests/spineexport_test.cpp).
//-----------------------------------------------------------------------------

namespace SpineExport {

//! Local transform of a Spine bone. Angles are in degrees, counterclockwise;
//! shearX is always 0.
struct BoneTransform {
  double m_x = 0.0, m_y = 0.0;
  double m_rotation = 0.0;
  double m_scaleX = 1.0, m_scaleY = 1.0;
  double m_shearY = 0.0;
};

//! Returns the angle equivalent to \e angle in (-180, 180].
inline double normalizeDegrees(double angle) {
  angle = std::fmod(angle, 360.0);
  if (angle <= -180.0)
    angle += 360.0;
  else if (angle > 180.0)
    angle -= 360.0;
  return angle;
}

//! Returns the angle equivalent to \e angle nearest to \e reference.
inline double unwrapDegrees(double angle, double reference) {
  return reference + normalizeDegrees(angle - reference);
}

/*!
Decomposes the affine transform
  | a11 a12 a13 |
  | a21 a22 a23 |
into a Spine bone transform, as Spine runtimes compose them:
  a11 = cos(rotation) * scaleX    a12 = cos(rotation + 90 + shearY) * scaleY
  a21 = sin(rotation) * scaleX    a22 = sin(rotation + 90 + shearY) * scaleY
  a13 = x                         a23 = y
Every transform has two such decompositions, the second one negating both
scales and turning by 180 degrees. The one with the rotation nearest to the
\e previous one is returned, and its angles are unwrapped against it, so that
keys baked frame by frame interpolate without spins. Without \e previous (the
first frame), positive scales are preferred unless the transform flips, in
which case the rotation nearest to 0 is preferred.
*/
inline BoneTransform decomposeAffine(double a11, double a12, double a13,
                                     double a21, double a22, double a23,
                                     const BoneTransform *previous = nullptr) {
  const double toDegrees = 180.0 / 3.14159265358979323846;
  const double eps       = 1e-12;

  BoneTransform t;
  t.m_x = a13, t.m_y = a23;

  double scaleX = std::hypot(a11, a21), scaleY = std::hypot(a12, a22);
  double det = a11 * a22 - a12 * a21;

  double rotation = 0.0;
  if (scaleX > eps)
    rotation = std::atan2(a21, a11) * toDegrees;
  else if (scaleY > eps)
    rotation = std::atan2(a22, a12) * toDegrees - 90.0;

  if (det < 0.0) scaleY = -scaleY;

  double shearY = 0.0;
  if (std::abs(scaleY) > eps)
    shearY = normalizeDegrees(
        std::atan2(a22 / scaleY, a12 / scaleY) * toDegrees - rotation - 90.0);

  // Choose between (scaleX, scaleY, rotation) and its 180 degrees twin
  double reference = previous ? previous->m_rotation : 0.0;
  bool useTwin     = (previous || det < 0.0) &&
                 std::abs(normalizeDegrees(rotation + 180.0 - reference)) <
                     std::abs(normalizeDegrees(rotation - reference));
  if (useTwin) {
    scaleX = -scaleX, scaleY = -scaleY;
    rotation += 180.0;
  }

  t.m_scaleX = scaleX, t.m_scaleY = scaleY;
  if (previous) {
    t.m_rotation = unwrapDegrees(rotation, previous->m_rotation);
    t.m_shearY   = unwrapDegrees(shearY, previous->m_shearY);
  } else {
    t.m_rotation = normalizeDegrees(rotation);
    t.m_shearY   = shearY;
  }
  return t;
}

//-----------------------------------------------------------------------------
// Mesh attachments
//-----------------------------------------------------------------------------

/*!
Returns the closed loops formed by the boundary edges (the edges of one
single triangle) of a triangle mesh, given as flat vertex index triplets.
Holes give loops of their own. A vertex shared by several boundary edges
(a pinch) can be visited more than once by a loop.
*/
inline std::vector<std::vector<int>> boundaryLoops(
    const std::vector<int> &triangles) {
  typedef std::pair<int, int> Edge;

  std::map<Edge, int> edgeCounts;
  for (size_t i = 0; i + 2 < triangles.size(); i += 3)
    for (int e = 0; e < 3; ++e) {
      int a = triangles[i + e], b = triangles[i + (e + 1) % 3];
      if (a == b) continue;
      ++edgeCounts[Edge(std::min(a, b), std::max(a, b))];
    }

  std::map<Edge, bool> used;  // boundary edges, true once part of a loop
  std::map<int, std::vector<int>> neighbours;
  for (const auto &ec : edgeCounts) {
    if (ec.second != 1) continue;
    used[ec.first] = false;
    neighbours[ec.first.first].push_back(ec.first.second);
    neighbours[ec.first.second].push_back(ec.first.first);
  }

  std::vector<std::vector<int>> loops;
  for (auto &start : used) {
    if (start.second) continue;
    start.second = true;

    int first = start.first.first, current = start.first.second;
    std::vector<int> loop(1, first);
    bool closed = false;
    while (true) {
      loop.push_back(current);
      int next    = -1;
      Edge nextEd = Edge(0, 0);
      for (int n : neighbours[current]) {
        Edge ed(std::min(current, n), std::max(current, n));
        if (used[ed]) continue;
        next   = n;
        nextEd = ed;
        break;
      }
      if (next < 0) break;
      used[nextEd] = true;
      if (next == first) {
        closed = true;
        break;
      }
      current = next;
    }
    if (closed && loop.size() >= 3) loops.push_back(loop);
  }
  return loops;
}

//-----------------------------------------------------------------------------

//! Absolute area of the polygon of \e loop, with vertex positions in \e xy
//! (x and y interleaved).
inline double loopArea(const std::vector<int> &loop,
                       const std::vector<double> &xy) {
  double area = 0.0;
  for (size_t i = 0; i < loop.size(); ++i) {
    int a = loop[i], b = loop[(i + 1) % loop.size()];
    area += xy[2 * a] * xy[2 * b + 1] - xy[2 * b] * xy[2 * a + 1];
  }
  return std::abs(0.5 * area);
}

//-----------------------------------------------------------------------------

/*!
Returns the vertices of the outer boundary of a triangle mesh (flat index
triplets, with the vertex positions in \e xy), in loop order. A mesh made of
several disconnected pieces has one outer loop per piece, one after the
other: of all the boundary loops of a piece (its outline and its holes) the
one enclosing the largest area is its outer one. A vertex is listed once.
*/
inline std::vector<int> outerBoundary(const std::vector<int> &triangles,
                                      const std::vector<double> &xy) {
  std::vector<std::vector<int>> loops = boundaryLoops(triangles);

  // Connected pieces (union-find on the vertices of the triangles)
  std::map<int, int> parent;
  auto find = [&](int v) {
    if (!parent.count(v)) parent[v] = v;
    while (parent[v] != v) v = parent[v] = parent[parent[v]];
    return v;
  };
  for (size_t i = 0; i + 2 < triangles.size(); i += 3) {
    int a = find(triangles[i]);
    for (int e = 1; e < 3; ++e) {
      int b = find(triangles[i + e]);
      if (a != b) parent[b] = a;
    }
  }

  std::vector<int> bestLoops;  // index in loops, per piece in order found
  std::map<int, size_t> pieceBest;
  for (size_t l = 0; l < loops.size(); ++l) {
    int piece = find(loops[l][0]);
    auto it   = pieceBest.find(piece);
    if (it == pieceBest.end()) {
      pieceBest[piece] = bestLoops.size();
      bestLoops.push_back((int)l);
    } else if (loopArea(loops[l], xy) >
               loopArea(loops[bestLoops[it->second]], xy))
      bestLoops[it->second] = (int)l;
  }

  std::vector<int> hull;
  std::vector<bool> listed(xy.size() / 2, false);
  for (int l : bestLoops)
    for (int v : loops[l])
      if (!listed[v]) listed[v] = true, hull.push_back(v);
  return hull;
}

//-----------------------------------------------------------------------------

//! Vertex order of a Spine mesh: the hull vertices come first, in loop
//! order, then the other ones in their original order.
struct MeshLayout {
  std::vector<int> m_order;  //!< Original vertex index, per exported vertex
  std::vector<int> m_remap;  //!< Exported vertex index, per original vertex
  int m_hull = 0;            //!< Number of hull vertices
};

inline MeshLayout meshLayout(int vertexCount, const std::vector<int> &triangles,
                             const std::vector<double> &xy) {
  MeshLayout layout;
  layout.m_order = outerBoundary(triangles, xy);
  layout.m_hull  = (int)layout.m_order.size();

  std::vector<bool> listed(vertexCount, false);
  for (int v : layout.m_order) listed[v] = true;
  for (int v = 0; v < vertexCount; ++v)
    if (!listed[v]) layout.m_order.push_back(v);

  layout.m_remap.assign(vertexCount, 0);
  for (int i = 0; i < vertexCount; ++i) layout.m_remap[layout.m_order[i]] = i;
  return layout;
}

//-----------------------------------------------------------------------------

/*!
Normalizes a point against the image rectangle with lower left corner
(\e left, \e bottom) and size (\e width, \e height), all in y-up pixels, to
the texture coordinates of Spine, where v grows downwards from the top of
the image.
*/
inline void meshUv(double x, double y, double left, double bottom, double width,
                   double height, double &u, double &v) {
  u = (x - left) / width;
  v = (bottom + height - y) / height;
}

//-----------------------------------------------------------------------------

/*!
Returns in \e before and \e after the whole pixels to add before and after
the interval [\e lo, \e hi] so that it covers [\e coverLo, \e coverHi].
*/
inline void padToCover(double lo, double hi, double coverLo, double coverHi,
                       int &before, int &after) {
  const double eps = 1e-6;
  before           = std::max(0, (int)std::ceil(lo - coverLo - eps));
  after            = std::max(0, (int)std::ceil(coverHi - hi - eps));
}

}  // namespace SpineExport

#endif  // SPINEEXPORT_H
