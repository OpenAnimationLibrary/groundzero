#include "toonz/tcurvedeformer.h"

#include <algorithm>
#include <cmath>

//****************************************************************************************
//    Local namespace stuff
//****************************************************************************************

namespace {

typedef TBezierChain::Anchor Anchor;

// Samples per segment of the polyline that measures the curve (and finds the
// basin of the nearest point of a projection).
const int c_samples = 24;

// Speeds below this are taken as zero when finding a tangent.
const double c_minSpeed = 1e-9;

//---------------------------------------------------------------------------------

// Control points of the segment s of the chain.
struct Segment {
  TPointD m_p0, m_p1, m_p2, m_p3;
};

Segment segmentOf(const std::vector<Anchor> &anchors, int s) {
  return {anchors[s].m_pos, anchors[s].m_out, anchors[s + 1].m_in,
          anchors[s + 1].m_pos};
}

// Splits the chain's parameter g, which is s + t on the segment s, clamping
// it to the chain.
void splitParameter(const std::vector<Anchor> &anchors, double g, int &s,
                    double &t) {
  int last = (int)anchors.size() - 2;
  s        = std::max(0, std::min((int)std::floor(g), last));
  t        = g - s;
}

TPointD pointAt(const std::vector<Anchor> &anchors, double g) {
  int s;
  double t;
  splitParameter(anchors, g, s, t);
  Segment c = segmentOf(anchors, s);

  double m = 1.0 - t;
  return c.m_p0 * (m * m * m) + c.m_p1 * (3.0 * m * m * t) +
         c.m_p2 * (3.0 * m * t * t) + c.m_p3 * (t * t * t);
}

TPointD velocityAt(const std::vector<Anchor> &anchors, double g) {
  int s;
  double t;
  splitParameter(anchors, g, s, t);
  Segment c = segmentOf(anchors, s);

  double m = 1.0 - t;
  return ((c.m_p1 - c.m_p0) * (m * m) + (c.m_p2 - c.m_p1) * (2.0 * m * t) +
          (c.m_p3 - c.m_p2) * (t * t)) *
         3.0;
}

TPointD accelerationAt(const std::vector<Anchor> &anchors, double g) {
  int s;
  double t;
  splitParameter(anchors, g, s, t);
  Segment c = segmentOf(anchors, s);

  return ((c.m_p2 - c.m_p1 * 2.0 + c.m_p0) * (1.0 - t) +
          (c.m_p3 - c.m_p2 * 2.0 + c.m_p1) * t) *
         6.0;
}

// The unit tangent. Where the speed vanishes (a handle on its anchor), it is
// the direction the curve leaves the point in: that of the acceleration, or
// failing that of the chord of the segment.
TPointD tangentAt(const std::vector<Anchor> &anchors, double g) {
  TPointD v(velocityAt(anchors, g));
  double n = norm(v);
  if (n > c_minSpeed) return v * (1.0 / n);

  int s;
  double t;
  splitParameter(anchors, g, s, t);

  TPointD a(accelerationAt(anchors, g));
  if (t > 0.5) a = -a;
  n = norm(a);
  if (n > c_minSpeed) return a * (1.0 / n);

  Segment c = segmentOf(anchors, s);
  TPointD chord(c.m_p3 - c.m_p0);
  n = norm(chord);
  if (n > c_minSpeed) return chord * (1.0 / n);

  return TPointD(1.0, 0.0);
}

//---------------------------------------------------------------------------------

// Returns the chain's parameter of the point at the length s along the
// polyline with the passed cumulative lengths, whose vertices are at the
// uniform parameters j / c_samples.
double parameterAtLength(const std::vector<double> &lengths, double s) {
  int j = (int)(std::upper_bound(lengths.begin(), lengths.end(), s) -
                lengths.begin()) -
          1;
  j = std::max(0, std::min(j, (int)lengths.size() - 2));

  double span = lengths[j + 1] - lengths[j];
  double f    = (span > 0.0) ? (s - lengths[j]) / span : 0.0;
  f           = std::max(0.0, std::min(f, 1.0));
  return (j + f) / c_samples;
}

// The inverse of parameterAtLength().
double lengthAtParameter(const std::vector<double> &lengths, double g) {
  double x = g * c_samples;
  int j    = std::max(0, std::min((int)std::floor(x), (int)lengths.size() - 2));
  return lengths[j] + (x - j) * (lengths[j + 1] - lengths[j]);
}

//---------------------------------------------------------------------------------

inline double nonNegative(double v) {
  // The negated test also maps NaNs to 0
  return (v > 0.0) ? v : 0.0;
}

inline bool contains(const TRectD &box, const TPointD &p) {
  return p.x >= box.x0 && p.x <= box.x1 && p.y >= box.y0 && p.y <= box.y1;
}

inline bool overlaps(const TRectD &a, const TRectD &b) {
  return a.x0 <= b.x1 && b.x0 <= a.x1 && a.y0 <= b.y1 && b.y0 <= a.y1;
}

inline TRectD unionOf(const TRectD &a, const TRectD &b) {
  return TRectD(std::min(a.x0, b.x0), std::min(a.y0, b.y0),
                std::max(a.x1, b.x1), std::max(a.y1, b.y1));
}

inline TRectD enlargedBox(const TRectD &box, double d) {
  return TRectD(box.x0 - d, box.y0 - d, box.x1 + d, box.y1 + d);
}

inline bool isInfinite(const TRectD &rect) {
  const double big = 1e30;
  return rect.x0 <= -big || rect.y0 <= -big || rect.x1 >= big || rect.y1 >= big;
}

}  // namespace

//****************************************************************************************
//    TBezierChain  implementation
//****************************************************************************************

TBezierChain::TBezierChain(const std::vector<Anchor> &anchors)
    : m_anchors(anchors), m_length(0.0) {
  int n = (int)m_anchors.size();
  if (n < 2) return;

  int count = (n - 1) * c_samples + 1;
  m_samples.resize(count);
  m_lengths.resize(count);

  for (int j = 0; j < count; ++j) {
    m_samples[j] = pointAt(m_anchors, double(j) / c_samples);
    m_lengths[j] =
        (j > 0) ? m_lengths[j - 1] + norm(m_samples[j] - m_samples[j - 1])
                : 0.0;
  }
  m_length = m_lengths.back();
}

//---------------------------------------------------------------------------------

bool TBezierChain::isValid() const {
  // The negated test also rejects NaNs
  return m_anchors.size() >= 2 && m_length > 1e-9;
}

//---------------------------------------------------------------------------------

TPointD TBezierChain::getPoint(double u) const {
  if (!isValid()) return TPointD();

  // Beyond the ends, the curve goes on along the tangents of its ends
  if (u <= 0.0)
    return m_anchors.front().m_pos + tangentAt(m_anchors, 0.0) * (u * m_length);

  double last = double(m_anchors.size() - 1);
  if (u >= 1.0)
    return m_anchors.back().m_pos +
           tangentAt(m_anchors, last) * ((u - 1.0) * m_length);

  return pointAt(m_anchors, parameterAtLength(m_lengths, u * m_length));
}

//---------------------------------------------------------------------------------

TPointD TBezierChain::getTangent(double u) const {
  if (!isValid()) return TPointD(1.0, 0.0);

  if (u <= 0.0) return tangentAt(m_anchors, 0.0);
  if (u >= 1.0) return tangentAt(m_anchors, double(m_anchors.size() - 1));

  return tangentAt(m_anchors, parameterAtLength(m_lengths, u * m_length));
}

//---------------------------------------------------------------------------------

bool TBezierChain::project(const TPointD &p, Projection &result) const {
  if (!isValid() || !std::isfinite(p.x) || !std::isfinite(p.y)) return false;

  // Find the nearest vertex of the sampling polyline
  int best        = 0;
  double bestDist = norm2(m_samples[0] - p);
  for (int j = 1; j < (int)m_samples.size(); ++j) {
    double dist = norm2(m_samples[j] - p);
    if (dist < bestDist) best = j, bestDist = dist;
  }

  // Refine it: find the zero of f(g) = (C(g) - p) . C'(g), the derivative of
  // the squared distance, within the neighbour samples. Newton steps, with a
  // bisection when they leave the bracket.
  const double gMax = double(m_anchors.size() - 1);

  double lo = std::max(0.0, (best - 1.0) / c_samples);
  double hi = std::min(gMax, (best + 1.0) / c_samples);
  double g  = double(best) / c_samples;

  for (int it = 0; it < 30; ++it) {
    TPointD r(pointAt(m_anchors, g) - p), v(velocityAt(m_anchors, g));
    double f = r * v;
    if (f > 0.0)
      hi = std::min(hi, g);
    else
      lo = std::max(lo, g);

    double df   = norm2(v) + r * accelerationAt(m_anchors, g);
    double gNew = (df > 1e-12) ? g - f / df : 0.5 * (lo + hi);
    if (!(gNew >= lo && gNew <= hi)) gNew = 0.5 * (lo + hi);

    bool done = std::fabs(gNew - g) < 1e-12;
    g         = gNew;
    if (done) break;
  }

  // A point past an end of the curve projects on the end's tangent line,
  // which the bisection only approaches
  if (g < 1.0 / c_samples) {
    TPointD r(p - pointAt(m_anchors, 0.0));
    if (r * tangentAt(m_anchors, 0.0) <= 0.0) g = 0.0;
  }
  if (g > gMax - 1.0 / c_samples) {
    TPointD r(p - pointAt(m_anchors, gMax));
    if (r * tangentAt(m_anchors, gMax) >= 0.0) g = gMax;
  }

  TPointD c(pointAt(m_anchors, g)), t(tangentAt(m_anchors, g));
  TPointD r(p - c);

  double u;
  if (g <= 0.0) {
    double s = r * t;
    u        = (s < 0.0) ? s / m_length : 0.0;
  } else if (g >= gMax) {
    double s = r * t;
    u        = (s > 0.0) ? 1.0 + s / m_length : 1.0;
  } else
    u = lengthAtParameter(m_lengths, g) / m_length;

  result.m_u        = u;
  result.m_d        = r * rotate90(t);
  result.m_distance = norm(r);
  return true;
}

//---------------------------------------------------------------------------------

double TBezierChain::distanceOf(double u, double d) const {
  if (u < 0.0) return std::sqrt(u * u * m_length * m_length + d * d);
  if (u > 1.0)
    return std::sqrt((u - 1.0) * (u - 1.0) * m_length * m_length + d * d);
  return std::fabs(d);
}

//---------------------------------------------------------------------------------

TRectD TBezierChain::getControlBounds() const {
  if (m_anchors.empty()) return TRectD();

  TRectD box(m_anchors[0].m_pos.x, m_anchors[0].m_pos.y, m_anchors[0].m_pos.x,
             m_anchors[0].m_pos.y);
  auto add = [&box](const TPointD &p) {
    box.x0 = std::min(box.x0, p.x), box.y0 = std::min(box.y0, p.y);
    box.x1 = std::max(box.x1, p.x), box.y1 = std::max(box.y1, p.y);
  };

  int last = (int)m_anchors.size() - 1;
  for (int i = 0; i <= last; ++i) {
    add(m_anchors[i].m_pos);
    if (i > 0) add(m_anchors[i].m_in);
    if (i < last) add(m_anchors[i].m_out);
  }
  return box;
}

//****************************************************************************************
//    TCurveDeformer  implementation
//****************************************************************************************

TCurveDeformer::TCurveDeformer(
    const std::vector<TBezierChain::Anchor> &rest,
    const std::vector<TBezierChain::Anchor> &deformed, double width,
    double falloff, double widthScale)
    : m_rest(rest)
    , m_deformed(deformed)
    , m_width(nonNegative(width))
    , m_falloff(0.0)
    , m_widthScale(std::max(nonNegative(widthScale), 1e-3))
    , m_scale(1.0) {
  m_falloff = std::max(nonNegative(falloff), m_width);
  if (!isValid()) return;

  TRectD restBounds = m_rest.getControlBounds();
  TRectD defBounds  = m_deformed.getControlBounds();

  // The points moved by the deformation lie within falloff of the rest curve;
  // their images are within that distance, scaled by how much the width and
  // the length are stretched, of the deformed curve.
  double reach = std::max(std::max(1.0, m_widthScale),
                          m_deformed.getLength() / m_rest.getLength());

  m_restBox     = enlargedBox(restBounds, m_falloff);
  m_deformedBox = enlargedBox(defBounds, m_falloff * reach);
  m_reachBox    = unionOf(m_restBox, m_deformedBox);

  TRectD all      = unionOf(restBounds, defBounds);
  double diagonal = norm(TPointD(all.x1 - all.x0, all.y1 - all.y0));
  if (diagonal > 0.0) m_scale = diagonal;
}

//---------------------------------------------------------------------------------

bool TCurveDeformer::isValid() const {
  return m_rest.isValid() && m_deformed.isValid() &&
         m_rest.getAnchorCount() == m_deformed.getAnchorCount();
}

//---------------------------------------------------------------------------------

bool TCurveDeformer::isIdentity(double tolerance) const {
  int n = m_rest.getAnchorCount();
  if (n != m_deformed.getAnchorCount()) return false;
  if (std::fabs(m_widthScale - 1.0) > 1e-12) return false;

  // The in handle of the first anchor and the out handle of the last one do
  // not shape the curves
  for (int i = 0; i < n; ++i) {
    const TBezierChain::Anchor &a = m_rest.getAnchor(i);
    const TBezierChain::Anchor &b = m_deformed.getAnchor(i);

    if (norm(a.m_pos - b.m_pos) > tolerance) return false;
    if (i > 0 && norm(a.m_in - b.m_in) > tolerance) return false;
    if (i < n - 1 && norm(a.m_out - b.m_out) > tolerance) return false;
  }
  return true;
}

//---------------------------------------------------------------------------------

double TCurveDeformer::influence(double distance) const {
  if (distance <= m_width) return 1.0;
  if (!(distance < m_falloff)) return 0.0;  // Also when they are equal

  double t = (distance - m_width) / (m_falloff - m_width);  // In (0, 1)
  return 1.0 - t * t * (3.0 - 2.0 * t);
}

//---------------------------------------------------------------------------------

TPointD TCurveDeformer::deform(const TPointD &p, double frame) const {
  if (!isValid() || !contains(m_restBox, p)) return p;

  TBezierChain::Projection pr;
  if (!m_rest.project(p, pr)) return p;

  double w = influence(pr.m_distance);
  if (!(w > 0.0)) return p;

  TPointD q(m_deformed.getPoint(pr.m_u) +
            m_deformed.getNormal(pr.m_u) * (pr.m_d * m_widthScale));
  return (w >= 1.0) ? q : p + (q - p) * w;
}

//---------------------------------------------------------------------------------

bool TCurveDeformer::undeform(const TPointD &p, double frame,
                              TPointD &result) const {
  result = p;
  if (!isValid() || !contains(m_reachBox, p)) return true;

  // The inverse of the full deformation, which is the answer where the
  // deformation applies in full there
  TBezierChain::Projection pd;
  bool hasFull = false;
  TPointD full;
  double fullWeight = 0.0;

  if (m_deformed.project(p, pd)) {
    double d   = pd.m_d / m_widthScale;
    full       = m_rest.getPoint(pd.m_u) + m_rest.getNormal(pd.m_u) * d;
    fullWeight = influence(m_rest.distanceOf(pd.m_u, d));
    if (fullWeight >= 1.0) {
      result = full;
      return true;
    }
    hasFull = true;
  }

  // Out of the influence of the curve, the point did not move
  TBezierChain::Projection pr;
  if (m_rest.project(p, pr) && !(influence(pr.m_distance) > 0.0)) return true;

  // In between, start from the best of the point itself and the inverse of the
  // full deformation blended by its weight, then refine with Newton steps on
  // deform(x) = p, with the Jacobian by finite differences (as the Deformer
  // Stack does).
  TPointD x(p);
  double err = norm(deform(x, frame) - p);
  if (hasFull) {
    TPointD x1(p + (full - p) * fullWeight);
    double err1 = norm(deform(x1, frame) - p);
    if (err1 < err) x = x1, err = err1;
  }

  const double h = 1e-5 * m_scale, tolerance = 1e-7 * m_scale;
  TPointD f(deform(x, frame));

  for (int it = 0; it < 10 && err > tolerance; ++it) {
    TPointD fx(deform(x + TPointD(h, 0.0), frame));
    TPointD fy(deform(x + TPointD(0.0, h), frame));

    TPointD jx((fx - f) * (1.0 / h)), jy((fy - f) * (1.0 / h));
    double det = cross(jx, jy);
    if (std::fabs(det) < 1e-12) break;  // Folded there

    TPointD r(p - f);
    TPointD xNew(x + TPointD(cross(r, jy), cross(jx, r)) * (1.0 / det));

    TPointD fNew(deform(xNew, frame));
    double errNew = norm(fNew - p);
    if (!(errNew < err)) break;  // No progress, e.g. near a fold

    x = xNew, f = fNew, err = errNew;
  }

  if (!std::isfinite(x.x) || !std::isfinite(x.y)) return false;
  result = x;
  return true;
}

//---------------------------------------------------------------------------------

TRectD TCurveDeformer::deformedBounds(const TRectD &sourceRect,
                                      double frame) const {
  if (sourceRect.isEmpty()) return TRectD();
  if (!isValid() || isInfinite(sourceRect)) return sourceRect;

  // Moved points are within the rest box, and go to within the deformed box
  // (or between them, the boxes' union being convex)
  if (!overlaps(sourceRect, m_restBox)) return sourceRect;
  return unionOf(sourceRect, m_deformedBox);
}

//---------------------------------------------------------------------------------

TRectD TCurveDeformer::sourceBounds(const TRectD &deformedRect,
                                    double frame) const {
  if (deformedRect.isEmpty()) return TRectD();
  if (!isValid() || isInfinite(deformedRect)) return deformedRect;

  // A point is either not moved, and so in the rect, or within the rest box
  if (!overlaps(deformedRect, m_reachBox)) return deformedRect;
  return unionOf(deformedRect, m_restBox);
}
