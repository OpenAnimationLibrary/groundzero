#include "toonz/contouroutline.h"

#include "tcurves.h"
#include "tstroke.h"
#include "tvectorimage.h"
#include "tpalette.h"
#include "tcolorstyles.h"
#include "tsimplecolorstyles.h"
#include "../common/tvectorimage/tvectorimageP.h"

#include <algorithm>
#include <cmath>
#include <memory>
#include <map>
#include <set>
#include <stdexcept>

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
  if (stroke.getChunkCount() <= 0) return false;

  const int count = stroke.getControlPointCount();
  for (int i = 0; i < count; i++)
    if (stroke.getControlPoint(i).thick > 1e-6) return true;
  return false;
}

//---------------------------------------------------------------------------------

bool ContourOutline::computeOutlinePolygons(
    const TStroke &input, std::vector<std::vector<TPointD>> &polygons,
    double sampleError) {
  polygons.clear();
  if (!canConvert(input)) return false;
  // Put a closed curve's parameter seam inside a quadratic. Its two tangent
  // limits then coincide, so both offset sides close without end caps or a
  // bridge across the hollow center. The geometry itself is unchanged.
  std::unique_ptr<TStroke> rotated;
  if (input.isSelfLoop() && input.getLength() > c_minLength) {
    TStroke first, last;
    input.split(input.getParameterAtControlPoint(1), first, last);
    std::vector<TThickPoint> points, tail;
    last.getControlPoints(points);
    first.getControlPoints(tail);
    if (!points.empty() && tail.size() >= 3) {
      points.insert(points.end(), tail.begin() + 1, tail.end());
      rotated.reset(new TStroke(points));
      rotated->setSelfLoop(true);
      rotated->outlineOptions() = input.outlineOptions();
    }
  }
  const TStroke &stroke = rotated ? *rotated : input;
  std::vector<TPointD> polygon;

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

  if (stroke.isSelfLoop() && stroke.getLength() > c_minLength) {
    for (auto &cycle : side) {
      removeDuplicates(cycle, tolerance * 0.1);
      if (cycle.size() < 3) return false;
      polygons.push_back(cycle);
    }
    return true;
  }

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
  polygons.push_back(polygon);
  return true;
}

//---------------------------------------------------------------------------------

bool ContourOutline::computeOutlinePolygon(const TStroke &stroke,
    std::vector<TPointD> &polygon, double sampleError) {
  polygon.clear();
  if (stroke.isSelfLoop()) return false; // A single cycle cannot represent a ring.
  std::vector<std::vector<TPointD>> polygons;
  if (!computeOutlinePolygons(stroke, polygons, sampleError)) return false;
  polygon = polygons.front();
  return true;
}

namespace {
bool fitContour(const std::vector<TPointD> &polygon,
                std::vector<TThickPoint> &controlPoints,
                const ContourOutline::Options &options) {
  controlPoints.clear();

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

}  // namespace

bool ContourOutline::makeOutlineContours(
    const TStroke &stroke, std::vector<std::vector<TThickPoint>> &contours,
    const Options &options) {
  contours.clear();
  std::vector<std::vector<TPointD>> polygons;
  if (!computeOutlinePolygons(stroke, polygons, options.m_sampleError)) return false;
  for (const auto &polygon : polygons) {
    std::vector<TThickPoint> points;
    if (!fitContour(polygon, points, options)) { contours.clear(); return false; }
    contours.push_back(std::move(points));
  }
  return !contours.empty();
}

bool ContourOutline::makeOutlineControlPoints(
    const TStroke &stroke, std::vector<TThickPoint> &controlPoints,
    const Options &options) {
  controlPoints.clear();
  if (stroke.isSelfLoop()) return false;
  std::vector<std::vector<TThickPoint>> contours;
  if (!makeOutlineContours(stroke, contours, options)) return false;
  controlPoints = std::move(contours.front());
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

namespace {
bool insideCycles(const std::vector<std::vector<TPointD>> &cycles,
                  const TPointD &point) {
  bool inside = false;
  for (const auto &cycle : cycles)
    for (size_t i = 0, j = cycle.size() - 1; i < cycle.size(); j = i++) {
      const auto &a = cycle[i], &b = cycle[j];
      if ((a.y > point.y) != (b.y > point.y) &&
          point.x < (b.x - a.x) * (point.y - a.y) / (b.y - a.y) + a.x)
        inside = !inside;
    }
  return inside;
}

struct ConvertedContour {
  std::vector<std::vector<TThickPoint>> boundaries;
  std::vector<std::vector<TPointD>> fittedCycles;
  int fillStyle = 0;
};

bool supportedContourStyle(const TColorStyle *style) {
  // Procedural stroke/texture styles do not have an equivalent flat region
  // appearance. Explicitly refuse them instead of silently changing artwork.
  return style && style->getTagId() == 3 && style->isStrokeStyle() &&
         style->isRegionStyle();
}

void fillContourRegion(TRegion *region, const ConvertedContour &contour) {
  TPointD point;
  if (region->getInternalPoint(point))
    region->setStyle(insideCycles(contour.fittedCycles, point) ? contour.fillStyle : 0);
  for (UINT i = 0; i < region->getSubregionCount(); ++i)
    fillContourRegion(region->getSubregion(i), contour);
}

bool hasPaintedRegion(const TRegion *region) {
  if (region->getStyle() != 0) return true;
  for (UINT i = 0; i < region->getSubregionCount(); ++i)
    if (hasPaintedRegion(region->getSubregion(i))) return true;
  return false;
}
}  // namespace

TVectorImageP ContourOutline::convertImage(
    const TVectorImageP &source, const std::vector<int> &indices,
    const Options &options) {
  if (!source || !source->getPalette()) return TVectorImageP();
  // Every decision and geometry calculation precedes document mutation.
  TVectorImageP original = source->clone();
  original->findRegions();
  TPalette *sourcePalette = source->getPalette();
  TPaletteP palette;
  std::map<std::pair<int, double>, int> opacityStyles;
  std::map<int, ConvertedContour> contours;
  for (int index : indices) {
    if (index < 0 || index >= int(original->getStrokeCount())) continue;
    const TStroke &stroke = *original->getStroke(index);
    if (!canConvert(stroke)) continue;
    if (!stroke.getOpacityProfile().empty())
      throw std::runtime_error("Contour conversion cannot preserve opacity that varies along a stroke. Remove the opacity profile before converting.");
    if (stroke.getStyle() < 0 || stroke.getStyle() >= sourcePalette->getStyleCount())
      throw std::runtime_error("A selected stroke refers to a missing palette style. No drawing was changed.");
    TColorStyle *style = sourcePalette->getStyle(stroke.getStyle());
    if (!supportedContourStyle(style))
      throw std::runtime_error("Contour conversion preserves solid-color strokes. Texture, procedural and region-only styles cannot be preserved as contour fills.");
    if (sourcePalette->getKeyframeCount(stroke.getStyle()) > 0)
      throw std::runtime_error("Contour conversion cannot preserve an animated stroke style. Duplicate a non-animated style before converting.");
    ConvertedContour contour;
    if (!makeOutlineContours(stroke, contour.boundaries, options))
      throw std::runtime_error("A selected stroke could not be converted to a closed contour. No drawing was changed.");
    contour.fillStyle = stroke.getStyle();
    if (stroke.getOpacity() < 1.0) {
      if (sourcePalette->isLocked())
        throw std::runtime_error("Unlock the palette before converting a partially transparent stroke; its opacity needs a new fill style.");
      const auto key = std::make_pair(stroke.getStyle(), stroke.getOpacity());
      auto found = opacityStyles.find(key);
      if (found == opacityStyles.end()) {
        if (!palette) palette = sourcePalette->clone();
        std::unique_ptr<TColorStyle> copy(style->clone());
        copy->setGlobalName(L"");
        copy->setOriginalName(L"");
        copy->setIsEditedFlag(false);
        copy->setName(style->getName() + L" (contour opacity)");
        for (int c = 0; c < copy->getColorParamCount(); ++c) {
          TPixel32 color = copy->getColorParamValue(c);
          color.m = static_cast<UCHAR>(std::lround(color.m * stroke.getOpacity()));
          copy->setColorParamValue(c, color);
        }
        copy->invalidateIcon();
        const int id = palette->addStyle(copy.release());
        if (id < 0 || palette->getPageCount() == 0 || palette->getPage(0)->addStyle(id) < 0)
          throw std::runtime_error("The palette has no room for the contour opacity style. No drawing was changed.");
        palette->setDirtyFlag(true);
        opacityStyles[key] = id;
        contour.fillStyle = id;
      } else contour.fillStyle = found->second;
    }
    for (const auto &points : contour.boundaries) {
      TStroke boundary(points);
      std::vector<TPointD> cycle;
      const int samples = std::max(128, 16 * boundary.getChunkCount());
      for (int i = 0; i < samples; ++i)
        cycle.push_back(boundary.getPoint(double(i) / samples));
      contour.fittedCycles.push_back(std::move(cycle));
    }
    contours[index] = std::move(contour);
  }
  if (contours.empty()) return TVectorImageP();

  TVectorImageP result = new TVectorImage;
  result->setPalette(palette ? palette.getPointer() : sourcePalette);
  result->m_imp->m_autocloseTolerance = original->m_imp->m_autocloseTolerance;
  result->m_imp->m_maxGroupId = original->m_imp->m_maxGroupId;
  result->m_imp->m_maxGhostGroupId = original->m_imp->m_maxGhostGroupId;
  std::vector<std::pair<int, int>> fillSources;
  std::map<int, int> contourGroups;
  auto append = [&](TStroke *stroke, const TGroupId &group) {
    const int outputIndex = int(result->m_imp->m_strokes.size());
    result->m_imp->m_strokes.push_back(new VIStroke(stroke, group));
    return outputIndex;
  };
  auto child = [&](const TGroupId &parent) {
    TGroupId group = parent;
    group.m_id.insert(group.m_id.begin(), ++result->m_imp->m_maxGroupId);
    return group;
  };
  for (int first = 0; first < int(original->getStrokeCount());) {
    int last = first + 1;
    while (last < int(original->getStrokeCount()) && original->sameGroup(first, last)) ++last;
    bool affected = false;
    for (int i = first; i < last; ++i) affected |= contours.count(i) != 0;
    const TGroupId oldGroup = original->getVIStroke(first)->m_groupId;
    if (!affected) {
      for (int i = first; i < last; ++i) {
        auto *stroke = new TStroke(*original->getStroke(i));
        stroke->setId(original->getStroke(i)->getId());
        fillSources.emplace_back(i, append(stroke, oldGroup));
      }
    } else {
      TGroupId parent = oldGroup;
      // An implicit group has no named hierarchy to preserve. Give its
      // replacement one parent so the generated layers remain selectable.
      if (original->getGroupDepth(first) == 0) {
        parent.m_id.clear();
        parent.m_id.push_back(++result->m_imp->m_maxGroupId);
      }
      const TGroupId background = child(parent);
      // Preserve exact fill boundaries, including holes and fills assembled
      // from several strokes. These helpers are invisible as strokes.
      bool hasFills = false;
      for (UINT r = 0; r < original->getRegionCount() && !hasFills; ++r)
        hasFills = original->sameGroupStrokeAndRegion(first, r) &&
                   hasPaintedRegion(original->getRegion(r));
      if (hasFills) {
        for (int i = first; i < last; ++i) {
          auto *boundary = new TStroke(*original->getStroke(i));
          for (int p = 0; p < boundary->getControlPointCount(); ++p) {
            auto point = boundary->getControlPoint(p);
            point.thick = 0;
            boundary->setControlPoint(p, point);
          }
          boundary->setStyle(0);
          boundary->setOpacity(0);
          fillSources.emplace_back(i, append(boundary, background));
        }
      }
      for (int i = first; i < last; ++i) {
        const TGroupId paintLayer = child(parent);
        auto converted = contours.find(i);
        if (converted == contours.end()) {
          auto *stroke = new TStroke(*original->getStroke(i));
          stroke->setId(original->getStroke(i)->getId());
          append(stroke, paintLayer);
        } else {
          int firstBoundary = -1;
          for (const auto &points : converted->second.boundaries) {
            auto *boundary = new TStroke(points);
            boundary->setSelfLoop(true);
            boundary->setStyle(0);
            const int at = append(boundary, paintLayer);
            if (firstBoundary < 0) firstBoundary = at;
          }
          contourGroups[firstBoundary] = i;
        }
      }
    }
    first = last;
  }
  result->findRegions();
  for (const auto &sourceAndTarget : fillSources)
    TVectorImage::transferStrokeColors(original, sourceAndTarget.first, result,
                                       sourceAndTarget.second);
  for (const auto &group : contourGroups)
    for (UINT r = 0; r < result->getRegionCount(); ++r)
      if (result->sameGroupStrokeAndRegion(group.first, r))
        fillContourRegion(result->getRegion(r), contours.at(group.second));
  return result;
}
