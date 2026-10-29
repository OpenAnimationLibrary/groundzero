#pragma once

#ifndef LOTTIEMATH_H
#define LOTTIEMATH_H

// Pure helpers of the Lottie (Bodymovin JSON) export: transform maths, Bezier
// conversions, keyframe reduction and the JSON snippets of the format. They
// depend only on the C++ library and on the JSON classes of Qt Core, so that
// they can be checked standalone (see toonzlib/tests/lottiemath_test.cpp).
//
// Conventions:
//  - Lottie is y-down, with the origin at the top left corner of the
//    composition; the stage is y-up, with the origin at the camera center.
//  - A layer transform is T(position) * R(rotation) * S(scale) * T(-anchor),
//    the rotation in degrees, clockwise on screen (the usual rotation matrix
//    in y-down coordinates), the scale in percent. The anchor is always 0
//    here, and skew is not written.
//  - Every number written goes through num(), which rounds it and replaces a
//    NaN or an infinity, so that the JSON never contains them.

#include <QJsonArray>
#include <QJsonObject>
#include <QJsonValue>

#include <algorithm>
#include <cmath>
#include <utility>
#include <vector>

namespace LottieMath {

const double c_pi = 3.14159265358979323846;

//! Largest magnitude written: larger values are clamped.
const double c_maxValue = 1e7;

// Precision of the written values
const double c_positionUnit = 1e-3;  //!< transform positions, pixels
const double c_pathUnit     = 1e-2;  //!< path vertices and tangents, pixels
const double c_angleUnit    = 1e-3;  //!< degrees
const double c_percentUnit  = 1e-3;  //!< scale and opacity, percent
const double c_colorUnit    = 1e-4;  //!< color components, 0..1
const double c_timeUnit     = 1e-6;  //!< seconds (time remap)

//-----------------------------------------------------------------------------
// Numbers
//-----------------------------------------------------------------------------

//! Returns \e value clamped to +-c_maxValue, or \e fallback if it is a NaN or
//! an infinity.
inline double finite(double value, double fallback = 0.0) {
  if (!std::isfinite(value)) value = fallback;
  if (!std::isfinite(value)) return 0.0;
  return std::max(-c_maxValue, std::min(c_maxValue, value));
}

//! Returns finite(value) rounded to a multiple of \e unit (if positive),
//! never "-0".
inline double num(double value, double unit = 0.0) {
  value = finite(value);
  if (unit > 0.0) value = std::round(value / unit) * unit;
  if (!std::isfinite(value)) return 0.0;
  return (value == 0.0) ? 0.0 : value;
}

//! Returns the angle equivalent to \e angle in (-180, 180].
inline double normalizeDegrees(double angle) {
  if (!std::isfinite(angle)) return 0.0;
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

//-----------------------------------------------------------------------------
// Geometry
//-----------------------------------------------------------------------------

struct Point {
  double x = 0.0, y = 0.0;
  Point() {}
  Point(double px, double py) : x(px), y(py) {}
};

inline Point operator+(const Point &a, const Point &b) {
  return Point(a.x + b.x, a.y + b.y);
}
inline Point operator-(const Point &a, const Point &b) {
  return Point(a.x - b.x, a.y - b.y);
}
inline Point operator*(double k, const Point &p) {
  return Point(k * p.x, k * p.y);
}

inline bool nearPoints(const Point &a, const Point &b, double eps) {
  return std::abs(a.x - b.x) <= eps && std::abs(a.y - b.y) <= eps;
}

/*!
2D affine transform, as TAffine:
  x' = a11 * x + a12 * y + a13
  y' = a21 * x + a22 * y + a23
(a * b)(p) is a(b(p)).
*/
struct Affine {
  double a11 = 1.0, a12 = 0.0, a13 = 0.0;
  double a21 = 0.0, a22 = 1.0, a23 = 0.0;

  Affine() {}
  Affine(double b11, double b12, double b13, double b21, double b22, double b23)
      : a11(b11), a12(b12), a13(b13), a21(b21), a22(b22), a23(b23) {}

  Affine operator*(const Affine &b) const {
    return Affine(a11 * b.a11 + a12 * b.a21, a11 * b.a12 + a12 * b.a22,
                  a11 * b.a13 + a12 * b.a23 + a13, a21 * b.a11 + a22 * b.a21,
                  a21 * b.a12 + a22 * b.a22, a21 * b.a13 + a22 * b.a23 + a23);
  }

  Point operator*(const Point &p) const {
    return Point(a11 * p.x + a12 * p.y + a13, a21 * p.x + a22 * p.y + a23);
  }

  double det() const { return a11 * a22 - a12 * a21; }

  bool isFinite() const {
    return std::isfinite(a11) && std::isfinite(a12) && std::isfinite(a13) &&
           std::isfinite(a21) && std::isfinite(a22) && std::isfinite(a23);
  }

  //! Returns the inverse, or the identity if the transform is singular.
  Affine inv() const {
    double d = det();
    if (!isFinite() || !std::isfinite(d) || std::abs(d) < 1e-12)
      return Affine();
    return Affine(a22 / d, -a12 / d, (a12 * a23 - a22 * a13) / d, -a21 / d,
                  a11 / d, (a21 * a13 - a11 * a23) / d);
  }

  bool isNear(const Affine &b, double linearEps, double translationEps) const {
    return std::abs(a11 - b.a11) <= linearEps &&
           std::abs(a12 - b.a12) <= linearEps &&
           std::abs(a21 - b.a21) <= linearEps &&
           std::abs(a22 - b.a22) <= linearEps &&
           std::abs(a13 - b.a13) <= translationEps &&
           std::abs(a23 - b.a23) <= translationEps;
  }
};

inline Affine translation(double x, double y) {
  return Affine(1.0, 0.0, x, 0.0, 1.0, y);
}

inline Affine scaling(double sx, double sy) {
  return Affine(sx, 0.0, 0.0, 0.0, sy, 0.0);
}

//! The rotation matrix of \e degrees: counterclockwise in y-up coordinates
//! (the stage), clockwise in y-down ones (Lottie).
inline Affine rotation(double degrees) {
  double a = degrees * c_pi / 180.0;
  double c = std::cos(a), s = std::sin(a);
  return Affine(c, -s, 0.0, s, c, 0.0);
}

//! The same transform in a y-down frame: m conjugated by the y flip
//! diag(1, -1). It turns a local transform of the stage (y-up) into a
//! Lottie one (y-down).
inline Affine flipY(const Affine &m) {
  return Affine(m.a11, -m.a12, m.a13, -m.a21, m.a22, -m.a23);
}

/*!
Returns the Lottie transform of a layer without parent from \e cameraPixels,
the transform from the layer's own frame to camera pixels, both in the stage
convention (y-up, origin at the camera center): the layer frame becomes
y-down, as flipY() does, and it is placed in the composition of the given
size (y-down, origin at the top left corner).
*/
inline Affine toComposition(const Affine &cameraPixels, double width,
                            double height) {
  return translation(0.5 * width, 0.5 * height) * flipY(cameraPixels);
}

//! Converts a point in camera pixels (y-up, origin at the camera center) to
//! composition pixels (y-down, origin at the top left corner).
inline Point toComposition(const Point &p, double width, double height) {
  return Point(p.x + 0.5 * width, 0.5 * height - p.y);
}

//! Lottie layer transform (the anchor is 0).
struct Transform {
  double m_x = 0.0, m_y = 0.0;        //!< position, pixels
  double m_rotation = 0.0;            //!< degrees, clockwise on screen
  double m_sx = 100.0, m_sy = 100.0;  //!< scale, percent
  double m_shear = 0.0;  //!< |cosine| of the angle between the transformed
                         //!< axes: the shear which is not written (0 when
                         //!< the axes stay perpendicular)
};

/*!
Decomposes a transform of the Lottie (y-down) space as
T(x, y) * R(rotation) * S(sx, sy). The shear, which a Lottie transform
without skew cannot hold, is dropped and measured in m_shear. Every
transform has two decompositions, the second one negating both scales and
turning by 180 degrees: the one with the rotation nearest to the \e previous
one is returned, its angle unwrapped against it, so that keys baked frame by
frame interpolate without spins. Without \e previous, positive x scales are
preferred. A non finite transform gives the previous one (or the identity).
*/
inline Transform decompose(const Affine &m,
                           const Transform *previous = nullptr) {
  const double toDegrees = 180.0 / c_pi;
  const double eps       = 1e-12;

  if (!m.isFinite()) return previous ? *previous : Transform();

  Transform t;
  t.m_x = m.a13, t.m_y = m.a23;

  double sx = std::hypot(m.a11, m.a21), sy = 0.0, angle = 0.0;
  double norm1 = std::hypot(m.a12, m.a22);
  if (sx > eps) {
    angle = std::atan2(m.a21, m.a11) * toDegrees;
    sy    = m.det() / sx;
    if (norm1 > eps)
      t.m_shear = std::abs(m.a11 * m.a12 + m.a21 * m.a22) / (sx * norm1);
  } else if (norm1 > eps) {
    // The second column is R(angle) * (0, sy)
    sx    = 0.0;
    sy    = norm1;
    angle = std::atan2(-m.a12, m.a22) * toDegrees;
  } else {
    sx = sy = 0.0;
    angle   = previous ? previous->m_rotation : 0.0;
  }

  if (previous) {
    double reference = previous->m_rotation;
    if (std::abs(normalizeDegrees(angle + 180.0 - reference)) <
        std::abs(normalizeDegrees(angle - reference))) {
      sx = -sx, sy = -sy;
      angle += 180.0;
    }
    t.m_rotation = unwrapDegrees(angle, reference);
  } else
    t.m_rotation = normalizeDegrees(angle);

  t.m_sx = sx * 100.0, t.m_sy = sy * 100.0;
  return t;
}

//-----------------------------------------------------------------------------
// Bezier paths
//-----------------------------------------------------------------------------

//! The inner control points of the cubic equal to the quadratic p0, p1, p2.
inline void quadraticToCubic(const Point &p0, const Point &p1, const Point &p2,
                             Point &c1, Point &c2) {
  c1 = p0 + (2.0 / 3.0) * (p1 - p0);
  c2 = p2 + (2.0 / 3.0) * (p1 - p2);
}

//! The part between \e ta and \e tb of the quadratic p0, p1, p2 (reversed if
//! tb < ta), as a quadratic q0, q1, q2.
inline void subQuadratic(const Point &p0, const Point &p1, const Point &p2,
                         double ta, double tb, Point &q0, Point &q1,
                         Point &q2) {
  auto at = [&](double t) {
    double u = 1.0 - t;
    return Point(u * u * p0.x + 2.0 * u * t * p1.x + t * t * p2.x,
                 u * u * p0.y + 2.0 * u * t * p1.y + t * t * p2.y);
  };
  q0 = at(ta), q2 = at(tb);
  // The derivative at ta, scaled by the parameter length
  Point d = (2.0 * (1.0 - ta)) * (p1 - p0) + (2.0 * ta) * (p2 - p1);
  q1      = q0 + (0.5 * (tb - ta)) * d;
}

//! A Lottie path: vertices with their in and out tangents, relative to them.
struct Path {
  std::vector<Point> m_v, m_in, m_out;
  bool m_closed = false;
};

/*!
Builds Lottie paths from line and quadratic segments. A segment starting
away from the current point starts a new sub-path, or is joined to it by a
line (addQuadratic() with joinGaps). close() merges a last vertex equal to
the first one.
*/
class PathBuilder {
  std::vector<Path> m_paths;
  bool m_open = false;
  double m_eps;

  Path &current() { return m_paths.back(); }

  void addVertex(const Point &p, const Point &in) {
    current().m_v.push_back(p);
    current().m_in.push_back(in);
    current().m_out.push_back(Point());
  }

public:
  explicit PathBuilder(double eps = 1e-6) : m_eps(eps) {}

  bool isOpen() const { return m_open; }

  Point currentPoint() const {
    return (m_open && !m_paths.back().m_v.empty()) ? m_paths.back().m_v.back()
                                                   : Point();
  }

  void moveTo(const Point &p) {
    if (m_open && current().m_v.size() < 2) m_paths.pop_back();
    m_paths.push_back(Path());
    m_open = true;
    addVertex(p, Point());
  }

  void lineTo(const Point &p) {
    if (!m_open) {
      moveTo(p);
      return;
    }
    if (nearPoints(p, currentPoint(), m_eps)) return;
    addVertex(p, Point());
  }

  void cubicTo(const Point &c1, const Point &c2, const Point &p) {
    if (!m_open) moveTo(c1);  // should not happen: start somewhere
    Point from = currentPoint();
    if (nearPoints(p, from, m_eps) && nearPoints(c1, from, m_eps) &&
        nearPoints(c2, from, m_eps))
      return;  // degenerate
    current().m_out.back() = c1 - from;
    addVertex(p, c2 - p);
  }

  void quadTo(const Point &c, const Point &p) {
    Point c1, c2;
    quadraticToCubic(currentPoint(), c, p, c1, c2);
    cubicTo(c1, c2, p);
  }

  void addQuadratic(const Point &p0, const Point &p1, const Point &p2,
                    bool joinGaps) {
    if (!m_open)
      moveTo(p0);
    else if (!nearPoints(p0, currentPoint(), m_eps)) {
      if (joinGaps)
        lineTo(p0);
      else
        moveTo(p0);
    }
    Point c1, c2;
    quadraticToCubic(p0, p1, p2, c1, c2);
    cubicTo(c1, c2, p2);
  }

  void close() {
    if (!m_open) return;
    Path &path = current();
    size_t n   = path.m_v.size();
    if (n >= 2 && nearPoints(path.m_v[n - 1], path.m_v[0], m_eps)) {
      path.m_in[0] = path.m_in[n - 1] + (path.m_v[n - 1] - path.m_v[0]);
      path.m_v.pop_back(), path.m_in.pop_back(), path.m_out.pop_back();
    }
    path.m_closed = true;
    m_open        = false;
  }

  //! The paths with at least 2 vertices.
  std::vector<Path> paths() const {
    std::vector<Path> result;
    for (const Path &path : m_paths)
      if (path.m_v.size() >= 2) result.push_back(path);
    return result;
  }
};

//-----------------------------------------------------------------------------
// Keyframes
//-----------------------------------------------------------------------------

//! Whether linear interpolation between keys \e a and \e b restores every
//! value between them within \e tolerance.
inline bool restoredBetween(const std::vector<std::vector<double>> &values,
                            int a, int b, double tolerance, int &worst) {
  double worstError = -1.0;
  worst             = -1;
  for (int i = a + 1; i < b; ++i) {
    double t     = double(i - a) / double(b - a);
    double error = 0.0;
    size_t dims  = std::min(values[i].size(),
                            std::min(values[a].size(), values[b].size()));
    for (size_t d = 0; d < dims; ++d) {
      double va = values[a][d], vb = values[b][d];
      double expected = va + (vb - va) * t;
      double slack    = 1e-9 * (1.0 + std::abs(expected));
      error = std::max(error, std::abs(values[i][d] - expected) - slack);
    }
    if (error > worstError) worstError = error, worst = i;
  }
  return worstError <= tolerance;
}

/*!
Returns the indices of the keys kept from per frame \e values (all of the
same dimension): the first and the last ones, and the others needed so that
linear interpolation between the kept keys restores every value within
\e tolerance (a tolerance of 0 drops the keys inside runs of equal values
and the keys of constant speed moves, as SpineExport's bakeKeys() drops the
first ones). Returns only the first key if all the values are equal (within
the tolerance) to it.
*/
inline std::vector<int> keptKeys(const std::vector<std::vector<double>> &values,
                                 double tolerance = 0.0) {
  std::vector<int> kept;
  int count = (int)values.size();
  if (count == 0) return kept;

  bool allEqual = true;
  for (int i = 1; i < count && allEqual; ++i)
    for (size_t d = 0; d < std::min(values[i].size(), values[0].size()); ++d)
      if (std::abs(values[i][d] - values[0][d]) >
          tolerance + 1e-9 * (1.0 + std::abs(values[0][d]))) {
        allEqual = false;
        break;
      }
  if (allEqual || count == 1) {
    kept.push_back(0);
    return kept;
  }

  // Ramer-Douglas-Peucker on the frame axis, without recursion
  std::vector<char> keep(count, 0);
  keep[0] = keep[count - 1] = 1;
  std::vector<std::pair<int, int>> stack(1, std::make_pair(0, count - 1));
  while (!stack.empty()) {
    std::pair<int, int> span = stack.back();
    stack.pop_back();
    if (span.second - span.first < 2) continue;
    int worst = -1;
    if (restoredBetween(values, span.first, span.second, tolerance, worst) ||
        worst < 0)
      continue;
    keep[worst] = 1;
    stack.push_back(std::make_pair(span.first, worst));
    stack.push_back(std::make_pair(worst, span.second));
  }
  for (int i = 0; i < count; ++i)
    if (keep[i]) kept.push_back(i);
  return kept;
}

//! Returns the indices of the keys of a stepped (hold) channel: the first one
//! and those whose value differs from the previous one.
inline std::vector<int> keptHoldKeys(
    const std::vector<std::vector<double>> &values) {
  std::vector<int> kept;
  for (int i = 0; i < (int)values.size(); ++i)
    if (i == 0 || values[i] != values[i - 1]) kept.push_back(i);
  return kept;
}

//! A static property: {"a":0,"k":value}.
inline QJsonObject staticProperty(double value, double unit) {
  QJsonObject json;
  json["a"] = 0;
  json["k"] = num(value, unit);
  return json;
}

//! A static property: {"a":0,"k":[values]}.
inline QJsonObject staticProperty(const std::vector<double> &values,
                                  double unit) {
  QJsonArray k;
  for (double v : values) k.append(num(v, unit));
  QJsonObject json;
  json["a"] = 0;
  json["k"] = k;
  return json;
}

/*!
Returns a property from its values at frames firstFrame, firstFrame + 1...:
static if every value is the same, else animated with linear keys (keys
that interpolation restores within \e tolerance are dropped, see keptKeys())
or, with \e hold, with hold keys where the value changes. A \e scalar
property writes its static value as a number (keys always hold arrays).
Values are rounded to \e unit first.
*/
inline QJsonObject animatedProperty(std::vector<std::vector<double>> values,
                                    double firstFrame, double unit,
                                    double tolerance, bool scalar,
                                    bool hold = false) {
  if (values.empty()) values.push_back(std::vector<double>(1, 0.0));
  for (std::vector<double> &value : values)
    for (double &v : value) v = num(v, unit);

  std::vector<int> keys =
      hold ? keptHoldKeys(values) : keptKeys(values, tolerance);
  if (keys.size() <= 1) {
    if (scalar && !values[0].empty()) return staticProperty(values[0][0], 0.0);
    return staticProperty(values[0], 0.0);
  }

  auto toArray = [](const std::vector<double> &value) {
    QJsonArray array;
    for (double v : value) array.append(v);
    return array;
  };
  auto easing = [](size_t dimension, double v) {
    QJsonArray a;
    for (size_t d = 0; d < dimension; ++d) a.append(v);
    QJsonObject json;
    json["x"] = a;
    json["y"] = a;
    return json;
  };

  QJsonArray k;
  for (size_t i = 0; i < keys.size(); ++i) {
    const std::vector<double> &value = values[keys[i]];
    QJsonObject key;
    key["t"] = num(firstFrame + keys[i], 0.0);
    key["s"] = toArray(value);
    if (hold)
      key["h"] = 1;
    else if (i + 1 < keys.size()) {
      key["e"] = toArray(values[keys[i + 1]]);
      key["i"] = easing(value.size(), 1.0);  // linear
      key["o"] = easing(value.size(), 0.0);
    }
    k.append(key);
  }
  QJsonObject json;
  json["a"] = 1;
  json["k"] = k;
  return json;
}

//! Tolerances of the transform keys reduction.
struct KeyTolerance {
  double m_position = 0.01;  //!< pixels
  double m_rotation = 0.01;  //!< degrees
  double m_scale    = 0.01;  //!< percent
};

/*!
Returns the transform ("ks") of a layer from its local transforms at frames
firstFrame, firstFrame + 1..., in the Lottie space of its parent, with a
static \e opacity in percent. Returns in \e maxShear the largest shear
dropped (see decompose()). The anchor, position and scale have 3 components,
z being 0 (100 for the scale), as After Effects exports them.
*/
inline QJsonObject layerTransform(
    const std::vector<Affine> &frames, double firstFrame, double opacity,
    const KeyTolerance &tolerance = KeyTolerance(),
    double *maxShear              = nullptr) {
  std::vector<std::vector<double>> p, r, s;
  Transform previous;
  bool first   = true;
  double shear = 0.0;
  for (const Affine &aff : frames) {
    Transform t = decompose(aff, first ? nullptr : &previous);
    shear       = std::max(shear, finite(t.m_shear));
    p.push_back({t.m_x, t.m_y, 0.0});
    r.push_back({t.m_rotation});
    s.push_back({t.m_sx, t.m_sy, 100.0});
    previous = t, first = false;
  }
  if (frames.empty()) {
    p.push_back({0.0, 0.0, 0.0});
    r.push_back({0.0});
    s.push_back({100.0, 100.0, 100.0});
  }
  if (maxShear) *maxShear = shear;

  QJsonObject ks;
  ks["a"] = staticProperty(std::vector<double>{0.0, 0.0, 0.0}, 0.0);
  ks["p"] = animatedProperty(p, firstFrame, c_positionUnit,
                             tolerance.m_position, false);
  ks["r"] =
      animatedProperty(r, firstFrame, c_angleUnit, tolerance.m_rotation, true);
  ks["s"] =
      animatedProperty(s, firstFrame, c_percentUnit, tolerance.m_scale, false);
  ks["o"] = staticProperty(std::max(0.0, std::min(100.0, finite(opacity))),
                           c_percentUnit);
  return ks;
}

//! The transform of a layer which does not move.
inline QJsonObject staticLayerTransform(const Affine &aff, double opacity,
                                        double *maxShear = nullptr) {
  return layerTransform(std::vector<Affine>(1, aff), 0.0, opacity,
                        KeyTolerance(), maxShear);
}

//-----------------------------------------------------------------------------
// Shapes
//-----------------------------------------------------------------------------

//! A Lottie color, [r, g, b, 1] with components in 0..1, from 8 bit ones.
//! The alpha goes into the opacity of the fill or the stroke.
inline QJsonArray color(int r, int g, int b) {
  auto c = [](int v) {
    return num(std::max(0, std::min(255, v)) / 255.0, c_colorUnit);
  };
  QJsonArray json;
  json.append(c(r));
  json.append(c(g));
  json.append(c(b));
  json.append(1);
  return json;
}

//! Opacity in percent from an 8 bit alpha and a 0..1 factor.
inline double opacityPercent(int alpha, double factor = 1.0) {
  double o = std::max(0, std::min(255, alpha)) / 255.0 * finite(factor, 1.0);
  return num(std::max(0.0, std::min(1.0, o)) * 100.0, c_percentUnit);
}

//! A path shape ("sh").
inline QJsonObject pathShape(const Path &path, const QString &name = "Path") {
  auto points = [](const std::vector<Point> &v) {
    QJsonArray array;
    for (const Point &p : v) {
      QJsonArray xy;
      xy.append(num(p.x, c_pathUnit));
      xy.append(num(p.y, c_pathUnit));
      array.append(xy);
    }
    return array;
  };
  QJsonObject k;
  k["c"] = path.m_closed;
  k["v"] = points(path.m_v);
  k["i"] = points(path.m_in);
  k["o"] = points(path.m_out);
  QJsonObject ks;
  ks["a"] = 0;
  ks["k"] = k;
  QJsonObject json;
  json["ty"] = "sh";
  json["nm"] = name;
  json["ks"] = ks;
  return json;
}

//! Fill rules of fillShape()
enum FillRule { NonZero = 1, EvenOdd = 2 };

//! A fill ("fl"); \e opacity in percent.
inline QJsonObject fillShape(const QJsonArray &rgb, double opacity, int rule) {
  QJsonObject c;
  c["a"] = 0;
  c["k"] = rgb;
  QJsonObject json;
  json["ty"] = "fl";
  json["nm"] = "Fill";
  json["c"]  = c;
  json["o"]  = staticProperty(opacity, c_percentUnit);
  json["r"]  = (rule == EvenOdd) ? 2 : 1;
  return json;
}

// Line caps and joins of strokeShape()
enum LineCap { ButtCap = 1, RoundCap = 2, SquareCap = 3 };
enum LineJoin { MiterJoin = 1, RoundJoin = 2, BevelJoin = 3 };

//! A stroke ("st"); \e opacity in percent, \e width in pixels.
inline QJsonObject strokeShape(const QJsonArray &rgb, double opacity,
                               double width, int cap, int join,
                               double miterLimit) {
  QJsonObject c;
  c["a"] = 0;
  c["k"] = rgb;
  QJsonObject json;
  json["ty"] = "st";
  json["nm"] = "Stroke";
  json["c"]  = c;
  json["o"]  = staticProperty(opacity, c_percentUnit);
  json["w"]  = staticProperty(std::max(0.0, finite(width)), c_positionUnit);
  json["lc"] = std::max(1, std::min(3, cap));
  json["lj"] = std::max(1, std::min(3, join));
  json["ml"] = num(std::max(1.0, finite(miterLimit, 4.0)), c_positionUnit);
  return json;
}

//! The identity transform ("tr") that ends the items of a group.
inline QJsonObject groupTransform() {
  QJsonObject json;
  json["ty"] = "tr";
  json["nm"] = "Transform";
  json["p"]  = staticProperty(std::vector<double>{0.0, 0.0}, 0.0);
  json["a"]  = staticProperty(std::vector<double>{0.0, 0.0}, 0.0);
  json["s"]  = staticProperty(std::vector<double>{100.0, 100.0}, 0.0);
  json["r"]  = staticProperty(0.0, 0.0);
  json["o"]  = staticProperty(100.0, 0.0);
  json["sk"] = staticProperty(0.0, 0.0);
  json["sa"] = staticProperty(0.0, 0.0);
  return json;
}

//! A group ("gr") of \e items (paths first, then the stroke and the fill
//! that paint them), with its transform appended.
inline QJsonObject group(const QString &name, QJsonArray items) {
  items.append(groupTransform());
  QJsonObject json;
  json["ty"] = "gr";
  json["nm"] = name;
  json["np"] = items.size() - 1;
  json["it"] = items;
  return json;
}

//-----------------------------------------------------------------------------
// Validation
//-----------------------------------------------------------------------------

//! Whether every number in \e value is finite.
inline bool allFinite(const QJsonValue &value) {
  if (value.isDouble()) return std::isfinite(value.toDouble());
  if (value.isArray()) {
    const QJsonArray array = value.toArray();
    for (int i = 0; i < array.size(); ++i)
      if (!allFinite(array.at(i))) return false;
  } else if (value.isObject()) {
    const QJsonObject object = value.toObject();
    for (auto it = object.constBegin(); it != object.constEnd(); ++it)
      if (!allFinite(it.value())) return false;
  }
  return true;
}

//! Returns \e value with every number that is not finite replaced by 0.
inline QJsonValue sanitized(const QJsonValue &value) {
  if (value.isDouble())
    return QJsonValue(std::isfinite(value.toDouble()) ? value.toDouble() : 0.0);
  if (value.isArray()) {
    QJsonArray array = value.toArray();
    for (int i = 0; i < array.size(); ++i) array[i] = sanitized(array.at(i));
    return array;
  }
  if (value.isObject()) {
    QJsonObject object = value.toObject();
    for (auto it = object.begin(); it != object.end(); ++it)
      it.value() = sanitized(it.value());
    return object;
  }
  return value;
}

}  // namespace LottieMath

#endif  // LOTTIEMATH_H
