#include "toonz/contouroutline.h"

#include "tcurves.h"
#include "tstroke.h"

#include <algorithm>
#include <cmath>
#include <memory>

//****************************************************************************************
//    Local namespace stuff
//****************************************************************************************

namespace {

const double c_pi = 3.14159265358979323846;

// Speeds and lengths below this are taken as zero.
const double c_minLength = 1e-9;

// Bounds of the steps of a chunk, and of an arc, so that wild input cannot
// make the polygon explode.
const int c_minSteps    = 2;
const int c_maxSteps    = 128;
const int c_maxArcSteps = 64;

// How many segments at each side of a join are searched for the crossing of
// the inner offset curves.
const int c_joinWindow = 32;

//---------------------------------------------------------------------------------

inline double crossProduct(const TPointD &a, const TPointD &b) {
  return a.x * b.y - a.y * b.x;
}

inline double dotProduct(const TPointD &a, const TPointD &b) {
  return a.x * b.x + a.y * b.y;
}

inline TPointD scaled(const TPointD &a, double k) {
  return TPointD(a.x * k, a.y * k);
}

// The left normal of a unit tangent.
inline TPointD leftNormal(const TPointD &t) { return TPointD(-t.y, t.x); }

//---------------------------------------------------------------------------------

// A point of the centerline, with its unit tangent and its radius.
struct Sample {
  TPointD m_p;
  TPointD m_t;
  double m_r;

  Sample() : m_p(), m_t(1.0, 0.0), m_r(0.0) {}
};

//---------------------------------------------------------------------------------

// The unit tangent of a chunk. Where the speed vanishes (coincident control
// points) the tangent is taken a little inside the chunk.
TPointD unitTangent(const TThickQuadratic &q, double t) {
  static const double offsets[] = {0.0, 0.02, 0.1, 0.3};
  for (double offset : offsets) {
    double tt     = t < 0.5 ? t + offset : t - offset;
    TPointD speed = q.getSpeed(tt);
    double n      = norm(speed);
    if (n > c_minLength) return TPointD(speed.x / n, speed.y / n);
  }
  TPointD chord = q.getP2() - q.getP0();
  double n      = norm(chord);
  if (n > c_minLength) return TPointD(chord.x / n, chord.y / n);
  return TPointD(1.0, 0.0);
}

//---------------------------------------------------------------------------------

Sample sampleChunk(const TThickQuadratic &q, double t) {
  Sample s;
  s.m_p = q.getPoint(t);
  s.m_t = unitTangent(q, t);
  s.m_r = std::max(0.0, q.getThickPoint(t).thick);
  return s;
}

//---------------------------------------------------------------------------------

// The signed angle from the unit vector a to the unit vector b.
inline double signedAngle(const TPointD &a, const TPointD &b) {
  return std::atan2(crossProduct(a, b), dotProduct(a, b));
}

//---------------------------------------------------------------------------------

// Number of steps of a chunk: enough for the polygon to stay within the
// tolerance of the position, of the thickness and of the rotation of the
// normal (that matters for the offset at the radius rMax).
int chunkSteps(const TThickQuadratic &q, double tolerance, double angleStep) {
  TPointD p0 = q.getP0(), p1 = q.getP1(), p2 = q.getP2();
  TPointD second(p0.x - 2.0 * p1.x + p2.x, p0.y - 2.0 * p1.y + p2.y);
  double thickSecond = std::abs(
      q.getThickP0().thick - 2.0 * q.getThickP1().thick + q.getThickP2().thick);

  double flat = std::sqrt((norm(second) + thickSecond) / (4.0 * tolerance));
  double turn = std::abs(signedAngle(unitTangent(q, 0.0), unitTangent(q, 1.0)));

  double steps = std::max(flat, turn / angleStep);
  if (!(steps < c_maxSteps)) return c_maxSteps;  // also catches NaN
  return std::max(c_minSteps, (int)std::ceil(steps));
}

//---------------------------------------------------------------------------------

// Appends to \b out the points strictly between the angles 0 and \b sweep of
// the arc of radius \b r around \b center, starting at the unit direction
// \b from.
void addArc(std::vector<TPointD> &out, const TPointD &center, double r,
            const TPointD &from, double sweep, double angleStep) {
  int count = (int)std::ceil(std::abs(sweep) / angleStep);
  count     = std::min(count, c_maxArcSteps);
  for (int i = 1; i < count; i++) {
    double a = sweep * i / count;
    double c = std::cos(a), s = std::sin(a);
    TPointD d(from.x * c - from.y * s, from.x * s + from.y * c);
    out.push_back(TPointD(center.x + r * d.x, center.y + r * d.y));
  }
}

//---------------------------------------------------------------------------------

// A join between two chunks that meet at a corner. The inner side of the
// corner has the offset curves crossing each other: that is found later,
// when the whole side is known.
struct Join {
  int m_inner;       // 0: the right side, 1: the left side
  size_t m_index;    // index, in the inner side, of the last point before it
  TPointD m_center;  // the point of the centerline
};

//---------------------------------------------------------------------------------

bool segmentsCross(const TPointD &a, const TPointD &b, const TPointD &c,
                   const TPointD &d, TPointD &x) {
  TPointD r = b - a, s = d - c;
  double den = crossProduct(r, s);
  if (std::abs(den) < 1e-12) return false;
  TPointD ac = c - a;
  double t   = crossProduct(ac, s) / den;
  double u   = crossProduct(ac, r) / den;
  if (t < 0.0 || t > 1.0 || u < 0.0 || u > 1.0) return false;
  x = TPointD(a.x + t * r.x, a.y + t * r.y);
  return true;
}

//---------------------------------------------------------------------------------

// Cuts the loop that the inner offset curves of a corner make around the join
// (v[at] is the last point before the corner and v[at + 1] the first one
// after). If the curves do not cross close by, goes through the centerline.
void trimInnerJoin(std::vector<TPointD> &v, const Join &join) {
  size_t at = join.m_index;
  if (at + 1 >= v.size()) return;

  size_t lo = at > (size_t)c_joinWindow ? at - c_joinWindow : 0;
  size_t hi = std::min(v.size() - 1, at + 1 + (size_t)c_joinWindow);

  bool found   = false;
  size_t bestI = 0, bestJ = 0;
  size_t bestSum = 0;
  TPointD bestX;
  for (size_t i = at; i-- > lo;) {
    for (size_t j = at + 1; j < hi; j++) {
      size_t sum = (at - i) + (j - at - 1);
      if (found && sum >= bestSum) continue;
      TPointD x;
      if (!segmentsCross(v[i], v[i + 1], v[j], v[j + 1], x)) continue;
      found   = true;
      bestI   = i;
      bestJ   = j;
      bestSum = sum;
      bestX   = x;
    }
  }

  if (found) {
    // v[bestI + 1 .. bestJ] are replaced by the crossing point
    v[bestI + 1] = bestX;
    v.erase(v.begin() + bestI + 2, v.begin() + bestJ + 1);
  } else {
    v.insert(v.begin() + at + 1, join.m_center);
  }
}

//---------------------------------------------------------------------------------

// Removes consecutive coincident points, and the last one if it coincides
// with the first.
void removeDuplicates(std::vector<TPointD> &polygon, double distance) {
  std::vector<TPointD> out;
  out.reserve(polygon.size());
  for (const TPointD &p : polygon)
    if (out.empty() || norm(p - out.back()) > distance) out.push_back(p);
  while (out.size() > 1 && norm(out.back() - out.front()) <= distance)
    out.pop_back();
  polygon.swap(out);
}

}  // namespace

//****************************************************************************************
//    Interface
//****************************************************************************************

bool ContourOutline::canConvert(const TStroke &stroke) {
  if (stroke.getChunkCount() <= 0 || stroke.isSelfLoop()) return false;

  const int count = stroke.getControlPointCount();
  for (int i = 0; i < count; i++)
    if (stroke.getControlPoint(i).thick > 1e-6) return true;
  return false;
}

//---------------------------------------------------------------------------------

bool ContourOutline::computeOutlinePolygon(const TStroke &stroke,
                                           std::vector<TPointD> &polygon,
                                           double sampleError) {
  polygon.clear();
  if (!canConvert(stroke)) return false;

  const double tolerance                 = std::max(sampleError, 1e-4);
  const TStroke::OutlineOptions &options = stroke.outlineOptions();

  double maxRadius = 0.0;
  for (int i = 0; i < stroke.getControlPointCount(); i++)
    maxRadius = std::max(maxRadius, (double)stroke.getControlPoint(i).thick);

  // Largest angle between two samples such that an arc of radius maxRadius
  // stays within the tolerance of its chord.
  double angleStep = c_pi / 2.0;
  if (maxRadius > tolerance)
    angleStep = std::max(0.02, 2.0 * std::acos(1.0 - tolerance / maxRadius));

  std::vector<TPointD> side[2];  // right and left of the direction of drawing
  std::vector<Join> joins;
  Sample first, last;
  bool haveFirst = false;

  const int chunkCount = stroke.getChunkCount();
  for (int c = 0; c < chunkCount; c++) {
    const TThickQuadratic &q = *stroke.getChunk(c);
    const int steps          = chunkSteps(q, tolerance, angleStep);

    for (int k = 0; k <= steps; k++) {
      Sample s = sampleChunk(q, (double)k / steps);

      if (k == 0 && haveFirst) {
        // Join with the end of the previous chunk
        double angle = signedAngle(last.m_t, s.m_t);
        double r     = std::max(last.m_r, s.m_r);
        if (r * std::abs(angle) <= tolerance) {
          last = s;  // smooth enough: the first sample is the last one
          continue;
        }

        // At a left turn the outer side is the right one, and vice versa
        const bool leftTurn = angle > 0.0;
        const int outer     = leftTurn ? 0 : 1;
        const int inner     = 1 - outer;
        const double sign   = leftTurn ? -1.0 : 1.0;

        Join join;
        join.m_inner  = inner;
        join.m_index  = side[inner].size() - 1;
        join.m_center = s.m_p;
        joins.push_back(join);

        TPointD fromDir = scaled(leftNormal(last.m_t), sign);
        TPointD toDir   = scaled(leftNormal(s.m_t), sign);
        if (options.m_joinStyle == TStroke::OutlineOptions::ROUND_JOIN) {
          addArc(side[outer], s.m_p, r, fromDir, angle, angleStep);
        } else if (options.m_joinStyle == TStroke::OutlineOptions::MITER_JOIN) {
          double d = 1.0 + dotProduct(fromDir, toDir);
          if (d > 1e-6) {
            TPointD sum(fromDir.x + toDir.x, fromDir.y + toDir.y);
            if (norm(sum) / d <= options.m_miterUpper)
              side[outer].push_back(
                  TPointD(s.m_p.x + r * sum.x / d, s.m_p.y + r * sum.y / d));
          }
        }  // bevel: the straight line between the offset points
      }

      TPointD n = leftNormal(s.m_t);
      side[0].push_back(TPointD(s.m_p.x - s.m_r * n.x, s.m_p.y - s.m_r * n.y));
      side[1].push_back(TPointD(s.m_p.x + s.m_r * n.x, s.m_p.y + s.m_r * n.y));
      if (!haveFirst) {
        first     = s;
        haveFirst = true;
      }
      last = s;
    }
  }

  // Inner corners, from the last one so that the indices stay valid
  for (size_t i = joins.size(); i-- > 0;)
    trimInnerJoin(side[joins[i].m_inner], joins[i]);

  // Right side forward, end cap, left side backward, start cap
  polygon = side[0];

  {
    TPointD n = leftNormal(last.m_t);
    TPointD t = last.m_t;
    double r  = last.m_r;
    if (options.m_capStyle == TStroke::OutlineOptions::ROUND_CAP) {
      addArc(polygon, last.m_p, r, scaled(n, -1.0), c_pi, angleStep);
    } else if (options.m_capStyle == TStroke::OutlineOptions::PROJECTING_CAP) {
      polygon.push_back(
          TPointD(last.m_p.x + r * (t.x - n.x), last.m_p.y + r * (t.y - n.y)));
      polygon.push_back(
          TPointD(last.m_p.x + r * (t.x + n.x), last.m_p.y + r * (t.y + n.y)));
    }
  }

  polygon.insert(polygon.end(), side[1].rbegin(), side[1].rend());

  {
    TPointD n = leftNormal(first.m_t);
    TPointD t = first.m_t;
    double r  = first.m_r;
    if (options.m_capStyle == TStroke::OutlineOptions::ROUND_CAP) {
      addArc(polygon, first.m_p, r, n, c_pi, angleStep);
    } else if (options.m_capStyle == TStroke::OutlineOptions::PROJECTING_CAP) {
      polygon.push_back(TPointD(first.m_p.x + r * (n.x - t.x),
                                first.m_p.y + r * (n.y - t.y)));
      polygon.push_back(TPointD(first.m_p.x + r * (-n.x - t.x),
                                first.m_p.y + r * (-n.y - t.y)));
    }
  }

  removeDuplicates(polygon, tolerance * 0.1);
  if (polygon.size() < 3) {
    polygon.clear();
    return false;
  }
  return true;
}

//---------------------------------------------------------------------------------

bool ContourOutline::makeOutlineControlPoints(
    const TStroke &stroke, std::vector<TThickPoint> &controlPoints,
    const Options &options) {
  controlPoints.clear();

  std::vector<TPointD> polygon;
  if (!computeOutlinePolygon(stroke, polygon, options.m_sampleError))
    return false;

  std::vector<TThickPoint> points;
  points.reserve(polygon.size() + 1);
  for (const TPointD &p : polygon) points.push_back(TThickPoint(p.x, p.y, 0.0));
  points.push_back(points.front());  // closed

  // The fit is relaxed until it is small enough: the sequence of errors is
  // fixed, so the result is deterministic.
  const int maxRelaxations = 8;
  double error             = std::max(options.m_fitError, 1e-4);
  std::unique_ptr<TStroke> fit;
  for (int i = 0; i <= maxRelaxations; i++) {
    fit.reset(TStroke::interpolate(points, error));
    if (!fit || fit->getControlPointCount() < 3) return false;
    if (fit->getControlPointCount() <= options.m_maxControlPoints) break;
    error *= 2.0;
  }

  fit->setSelfLoop(true);  // first and last control points are the same

  const int count = fit->getControlPointCount();
  controlPoints.reserve(count);
  for (int i = 0; i < count; i++) {
    TThickPoint p = fit->getControlPoint(i);
    controlPoints.push_back(TThickPoint(p.x, p.y, 0.0));
  }
  return true;
}

//---------------------------------------------------------------------------------

TStroke *ContourOutline::makeOutlineStroke(const TStroke &stroke,
                                           const Options &options) {
  std::vector<TThickPoint> controlPoints;
  if (!makeOutlineControlPoints(stroke, controlPoints, options)) return 0;

  TStroke *outline = new TStroke(controlPoints);
  outline->setSelfLoop(true);
  outline->setStyle(stroke.getStyle());
  outline->outlineOptions() = stroke.outlineOptions();
  return outline;
}

//---------------------------------------------------------------------------------

double ContourOutline::polygonArea(const std::vector<TPointD> &polygon) {
  double sum     = 0.0;
  const size_t n = polygon.size();
  for (size_t i = 0; i < n; i++) {
    const TPointD &a = polygon[i];
    const TPointD &b = polygon[(i + 1) % n];
    sum += crossProduct(a, b);
  }
  return 0.5 * sum;
}
