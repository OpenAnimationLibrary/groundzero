// Regression test of the conversion of a variable-width brush stroke to its
// closed outline shape (ContourOutline): the area and the bounding box of the
// shape are compared with the stroke, analytically for the simple cases and
// against a brute force union of discs along the centerline for the others.

#include "toonz/contouroutline.h"

#include "tgeometry.h"
#include "tstroke.h"
#include "tvectorimage.h"
#include "tpalette.h"
#include "tregion.h"
#include "tsimplecolorstyles.h"

#include <algorithm>
#include <cmath>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <vector>

namespace {

const double c_pi = 3.14159265358979323846;

void require(bool condition, const char *message) {
  if (!condition) throw std::runtime_error(message);
}

//------------------------------------------------------------------

// Points of the outline of a closed stroke, sampled on its parameter.
std::vector<TPointD> sampleClosedStroke(const TStroke &stroke, int count) {
  std::vector<TPointD> points;
  for (int i = 0; i < count; i++)
    points.push_back(stroke.getPoint((double)i / count));
  return points;
}

bool insidePolygon(const std::vector<TPointD> &polygon, const TPointD &p) {
  bool inside    = false;
  const size_t n = polygon.size();
  for (size_t i = 0, j = n - 1; i < n; j = i++) {
    const TPointD &a = polygon[i];
    const TPointD &b = polygon[j];
    if ((a.y > p.y) != (b.y > p.y) &&
        p.x < (b.x - a.x) * (p.y - a.y) / (b.y - a.y) + a.x)
      inside = !inside;
  }
  return inside;
}

//------------------------------------------------------------------

// Bounding box that can be a point (TRectD cannot).
struct Box {
  double x0, y0, x1, y1;

  Box() : x0(1e300), y0(1e300), x1(-1e300), y1(-1e300) {}

  void add(double x, double y) {
    x0 = std::min(x0, x);
    y0 = std::min(y0, y);
    x1 = std::max(x1, x);
    y1 = std::max(y1, y);
  }
};

//------------------------------------------------------------------

// The union of the discs along the centerline of the stroke.
struct DiscUnion {
  std::vector<TThickPoint> m_discs;
  Box m_bbox;

  explicit DiscUnion(const TStroke &stroke) {
    const int count = 2000;
    for (int i = 0; i <= count; i++) {
      TThickPoint p = stroke.getThickPoint((double)i / count);
      m_discs.push_back(p);
      m_bbox.add(p.x - p.thick, p.y - p.thick);
      m_bbox.add(p.x + p.thick, p.y + p.thick);
    }
  }

  bool contains(const TPointD &q) const {
    for (const TThickPoint &d : m_discs) {
      double dx = q.x - d.x, dy = q.y - d.y;
      if (dx * dx + dy * dy <= d.thick * d.thick) return true;
    }
    return false;
  }
};

//------------------------------------------------------------------

Box boundingBox(const std::vector<TPointD> &points) {
  Box box;
  for (const TPointD &p : points) box.add(p.x, p.y);
  return box;
}

//------------------------------------------------------------------

void requireClosedZeroThickness(const TStroke &outline, int maxControlPoints) {
  require(outline.isSelfLoop(), "outline is not a self loop");
  // TStroke counts every control point with thickness <= 0 as a centerline
  // point, so a zero-thickness outline is a centerline stroke for it.
  require(outline.isCenterLine(), "outline has thickness");
  const int count = outline.getControlPointCount();
  require(count >= 5, "outline has too few control points");
  require(count <= maxControlPoints, "outline has too many control points");
  require(outline.getControlPoint(0) == outline.getControlPoint(count - 1),
          "outline is not closed");
  for (int i = 0; i < count; i++)
    require(outline.getControlPoint(i).thick == 0.0,
            "outline control point has thickness");
}

//------------------------------------------------------------------

// Checks the outline stroke of \b stroke against the union of discs.
void checkAgainstDiscs(const TStroke &stroke, const char *name) {
  std::unique_ptr<TStroke> outline(ContourOutline::makeOutlineStroke(stroke));
  require(outline.get() != 0, name);
  requireClosedZeroThickness(*outline, 400);

  std::vector<TPointD> shape = sampleClosedStroke(*outline, 4000);
  DiscUnion discs(stroke);

  // bounding boxes
  Box box                = boundingBox(shape);
  const double tolerance = 0.6;
  require(std::abs(box.x0 - discs.m_bbox.x0) < tolerance &&
              std::abs(box.y0 - discs.m_bbox.y0) < tolerance &&
              std::abs(box.x1 - discs.m_bbox.x1) < tolerance &&
              std::abs(box.y1 - discs.m_bbox.y1) < tolerance,
          name);

  // area: cells in only one of the two shapes
  const double step = 0.5;
  double both = 0.0, onlyShape = 0.0, onlyDiscs = 0.0;
  for (double y = discs.m_bbox.y0 - 2.0; y < discs.m_bbox.y1 + 2.0; y += step)
    for (double x = discs.m_bbox.x0 - 2.0; x < discs.m_bbox.x1 + 2.0;
         x += step) {
      TPointD p(x, y);
      bool a = insidePolygon(shape, p), b = discs.contains(p);
      if (a && b)
        both += 1.0;
      else if (a)
        onlyShape += 1.0;
      else if (b)
        onlyDiscs += 1.0;
    }
  const double cell = step * step;
  require(both * cell > 100.0, name);
  require((onlyShape + onlyDiscs) / both < 0.04, name);

  // the shoelace area of the polygon of the stroke agrees as well
  double area = ContourOutline::polygonArea(shape);
  require(area > 0.0, name);
  require(std::abs(area - both * cell) / area < 0.04, name);
}

//------------------------------------------------------------------

void testStraightRound() {
  std::vector<TThickPoint> cps = {TThickPoint(0, 0, 5), TThickPoint(50, 0, 5),
                                  TThickPoint(100, 0, 5)};
  TStroke stroke(cps);

  // analytic: a rectangle and two half discs
  const double expected = 2.0 * 5.0 * 100.0 + c_pi * 25.0;

  std::vector<TPointD> polygon;
  require(ContourOutline::computeOutlinePolygon(stroke, polygon),
          "straight: no polygon");
  double polygonArea = ContourOutline::polygonArea(polygon);
  require(polygonArea > 0.0, "straight: polygon is not counter-clockwise");
  require(std::abs(polygonArea - expected) / expected < 0.01,
          "straight: polygon area");

  std::unique_ptr<TStroke> outline(ContourOutline::makeOutlineStroke(stroke));
  require(outline.get() != 0, "straight: no outline");
  requireClosedZeroThickness(*outline, 400);
  require(outline->getStyle() == stroke.getStyle(), "straight: style");

  std::vector<TPointD> shape = sampleClosedStroke(*outline, 4000);
  double area                = ContourOutline::polygonArea(shape);
  require(std::abs(area - expected) / expected < 0.02, "straight: area");

  Box box = boundingBox(shape);
  require(std::abs(box.x0 + 5.0) < 0.6 && std::abs(box.x1 - 105.0) < 0.6 &&
              std::abs(box.y0 + 5.0) < 0.6 && std::abs(box.y1 - 5.0) < 0.6,
          "straight: bounding box");

  checkAgainstDiscs(stroke, "straight: discs");
}

//------------------------------------------------------------------

void testTaperedButt() {
  std::vector<TThickPoint> cps = {TThickPoint(0, 0, 1), TThickPoint(50, 0, 3.5),
                                  TThickPoint(100, 0, 6)};
  TStroke stroke(cps);
  stroke.outlineOptions().m_capStyle = TStroke::OutlineOptions::BUTT_CAP;

  // analytic: a trapezoid, the half width goes linearly from 1 to 6
  const double expected = 100.0 * (2.0 * 1.0 + 2.0 * 6.0) * 0.5;

  std::unique_ptr<TStroke> outline(ContourOutline::makeOutlineStroke(stroke));
  require(outline.get() != 0, "tapered: no outline");
  requireClosedZeroThickness(*outline, 400);
  require(
      outline->outlineOptions().m_capStyle == TStroke::OutlineOptions::BUTT_CAP,
      "tapered: outline options");

  std::vector<TPointD> shape = sampleClosedStroke(*outline, 4000);
  double area                = ContourOutline::polygonArea(shape);
  require(std::abs(area - expected) / expected < 0.02, "tapered: area");

  Box box = boundingBox(shape);
  require(std::abs(box.x0) < 0.6 && std::abs(box.x1 - 100.0) < 0.6 &&
              std::abs(box.y0 + 6.0) < 0.6 && std::abs(box.y1 - 6.0) < 0.6,
          "tapered: bounding box");
}

//------------------------------------------------------------------

void testProjectingCap() {
  std::vector<TThickPoint> cps = {TThickPoint(0, 0, 4), TThickPoint(50, 0, 4),
                                  TThickPoint(100, 0, 4)};
  TStroke stroke(cps);
  stroke.outlineOptions().m_capStyle = TStroke::OutlineOptions::PROJECTING_CAP;

  std::vector<TPointD> polygon;
  require(ContourOutline::computeOutlinePolygon(stroke, polygon),
          "projecting: no polygon");
  // a rectangle that goes past both ends by the radius
  const double expected = (100.0 + 8.0) * 8.0;
  require(std::abs(ContourOutline::polygonArea(polygon) - expected) / expected <
              0.005,
          "projecting: area");
  Box box = boundingBox(polygon);
  require(std::abs(box.x0 + 4.0) < 1e-6 && std::abs(box.x1 - 104.0) < 1e-6,
          "projecting: bounding box");
}

//------------------------------------------------------------------

void testArc() {
  std::vector<TThickPoint> cps = {TThickPoint(0, 0, 3), TThickPoint(50, 60, 4),
                                  TThickPoint(100, 0, 5)};
  TStroke stroke(cps);
  checkAgainstDiscs(stroke, "arc: discs");
}

//------------------------------------------------------------------

void testCorner() {
  // two chunks that meet at a right angle in (50, 0)
  std::vector<TThickPoint> cps = {TThickPoint(0, 0, 4), TThickPoint(25, 0, 4),
                                  TThickPoint(50, 0, 4), TThickPoint(50, 25, 4),
                                  TThickPoint(50, 50, 4)};
  TStroke stroke(cps);
  checkAgainstDiscs(stroke, "corner: discs");

  // the inner side of the corner is cut: the polygon does not overlap itself,
  // so its area is the one of the union and not more
  std::vector<TPointD> polygon;
  require(ContourOutline::computeOutlinePolygon(stroke, polygon),
          "corner: no polygon");
  // two 8 x 50 bars, minus the square of 4 x 4 they share, plus the two
  // round caps (a disc of radius 4) and the round outer join (a quarter)
  const double expected = 2.0 * 8.0 * 50.0 - 16.0 + c_pi * 16.0 + c_pi * 4.0;
  require(std::abs(ContourOutline::polygonArea(polygon) - expected) / expected <
              0.008,
          "corner: polygon area");

  // the same with a bevel join: the outer corner is cut
  stroke.outlineOptions().m_joinStyle = TStroke::OutlineOptions::BEVEL_JOIN;
  std::vector<TPointD> bevel;
  require(ContourOutline::computeOutlinePolygon(stroke, bevel),
          "corner: no bevel polygon");
  require(
      ContourOutline::polygonArea(bevel) < ContourOutline::polygonArea(polygon),
      "corner: bevel is not smaller than round");
}

//------------------------------------------------------------------

void testWavy() {
  // a long stroke with many chunks and a thickness that changes
  std::vector<TThickPoint> cps;
  const int chunks = 24;
  for (int i = 0; i <= 2 * chunks; i++) {
    double x = i * 10.0;
    double y = (i % 2) ? 25.0 * ((i / 2) % 2 ? 1.0 : -1.0) : 0.0;
    cps.push_back(TThickPoint(x, y, 2.0 + 1.5 * std::sin(i * 0.4)));
  }
  TStroke stroke(cps);

  ContourOutline::Options options;
  std::vector<TThickPoint> first, second;
  require(ContourOutline::makeOutlineControlPoints(stroke, first, options),
          "wavy: no outline");
  require(ContourOutline::makeOutlineControlPoints(stroke, second, options),
          "wavy: no outline (2)");
  require((int)first.size() <= options.m_maxControlPoints,
          "wavy: too many control points");
  require(first.size() == second.size(), "wavy: not deterministic");
  for (size_t i = 0; i < first.size(); i++)
    require(first[i].x == second[i].x && first[i].y == second[i].y,
            "wavy: not deterministic");

  // a tight bound relaxes the fit
  ContourOutline::Options tight;
  tight.m_maxControlPoints = 120;
  std::vector<TThickPoint> tightOutline;
  require(ContourOutline::makeOutlineControlPoints(stroke, tightOutline, tight),
          "wavy: no tight outline");
  require(tightOutline.size() <= (size_t)tight.m_maxControlPoints ||
              tightOutline.size() <= first.size(),
          "wavy: the bound did not help");
}

//------------------------------------------------------------------

void testNotConvertible() {
  // a centerline stroke has no thickness
  std::vector<TThickPoint> centerline = {
      TThickPoint(0, 0, -1), TThickPoint(10, 0, -1), TThickPoint(20, 0, -1)};
  TStroke centerlineStroke(centerline);
  require(!ContourOutline::canConvert(centerlineStroke), "centerline");
  require(ContourOutline::makeOutlineStroke(centerlineStroke) == 0,
          "centerline: outline");

  // so does a stroke of zero thickness
  std::vector<TThickPoint> flat = {TThickPoint(0, 0, 0), TThickPoint(10, 0, 0),
                                   TThickPoint(20, 0, 0)};
  TStroke flatStroke(flat);
  require(!ContourOutline::canConvert(flatStroke), "zero thickness");

  // A ring has two independent boundary cycles; the single-outline legacy
  // API deliberately cannot represent it.
  std::vector<TThickPoint> ring = {
      TThickPoint(0, 0, 2), TThickPoint(10, 20, 2), TThickPoint(20, 0, 2),
      TThickPoint(10, -20, 2), TThickPoint(0, 0, 2)};
  TStroke ringStroke(ring);
  ringStroke.setSelfLoop(true);
  require(ContourOutline::canConvert(ringStroke), "closed loop not convertible");
  std::vector<std::vector<TThickPoint>> contours;
  require(ContourOutline::makeOutlineContours(ringStroke, contours) && contours.size() == 2,
          "closed loop needs two independent boundaries");
  for (const auto &points : contours) {
    TStroke boundary(points); boundary.setSelfLoop(true);
    requireClosedZeroThickness(boundary, 400);
  }
  require(ContourOutline::makeOutlineStroke(ringStroke) == nullptr,
          "single-outline API silently flattened the hole");

  // a dot is a disc
  std::vector<TThickPoint> dot = {TThickPoint(5, 5, 3), TThickPoint(5, 5, 3),
                                  TThickPoint(5, 5, 3)};
  TStroke dotStroke(dot);
  std::vector<TPointD> polygon;
  // At the default error of 0.05 the disc of radius 3 is an 18-gon, 2% short
  // of its area; at 0.01 it is a 40-gon, 0.4% short
  require(ContourOutline::computeOutlinePolygon(dotStroke, polygon, 0.01),
          "dot: no polygon");
  require(std::abs(ContourOutline::polygonArea(polygon) - c_pi * 9.0) /
                  (c_pi * 9.0) <
              0.01,
          "dot: area");
}

TVectorImageP layeredFixture(bool fillCenter) {
  TVectorImageP image = new TVectorImage;
  TPaletteP palette = new TPalette;
  const int red = palette->addStyle(new TSolidColorStyle(TPixel32(255,0,0)));
  palette->getPage(0)->addStyle(red);
  image->setPalette(palette.getPointer());
  auto *line = new TStroke(std::vector<TThickPoint>{{-25,0,2},{0,0,2},{25,0,2}});
  line->setStyle(1); image->addStroke(line);
  auto *ring = new TStroke(std::vector<TThickPoint>{
      {-12,-12,3},{0,-12,3},{12,-12,3},{12,0,3},{12,12,3},
      {0,12,3},{-12,12,3},{-12,0,3},{-12,-12,3}});
  ring->setSelfLoop(true); ring->setStyle(red); ring->setOpacity(.5);
  image->addStroke(ring);
  image->findRegions();
  if (fillCenter) {
    TRegion *region = image->getRegion(TPointD(0,5));
    require(region, "closed-loop fixture has no center region");
    region->setStyle(1);
  }
  return image;
}

void testNativeLayeredConversion() {
  auto source = layeredFixture(false);
  std::vector<std::vector<TThickPoint>> boundaries;
  require(ContourOutline::makeOutlineContours(*source->getStroke(1), boundaries),
          "closed-loop footprint fixture conversion failed");
  std::vector<std::vector<TPointD>> cycles;
  for (const auto &points : boundaries) {
    TStroke boundary(points);
    cycles.push_back(sampleClosedStroke(boundary, 2000));
  }
  DiscUnion footprint(*source->getStroke(1));
  int agreement = 0, difference = 0;
  for (double y = -17.125; y < 17.; y += .5)
    for (double x = -17.125; x < 17.; x += .5) {
      const TPointD point(x,y);
      bool inContour = false;
      for (const auto &cycle : cycles) inContour ^= insidePolygon(cycle, point);
      const bool inBrush = footprint.contains(point);
      if (inContour && inBrush) ++agreement;
      else if (inContour != inBrush) ++difference;
    }
  require(agreement > 100 && double(difference) / agreement < .04,
          "closed-loop contour footprint differs from the brush band");
  const int originalCount = source->getPalette()->getStyleCount();
  auto converted = ContourOutline::convertImage(source, {1});
  require(converted, "native ring conversion failed");
  require(source->getStrokeCount() == 2 && source->getPalette()->getStyleCount() == originalCount,
          "conversion mutated source image or palette");
  require(converted->getPalette()->getStyleCount() == originalCount + 1,
          "stroke opacity was not represented in a dedicated fill style");
  int lineIndex = converted->getStrokeIndexById(source->getStroke(0)->getId());
  require(lineIndex >= 0, "unselected stroke identity lost");
  auto *line = converted->getStroke(lineIndex);
  for (int i = 0; i < line->getControlPointCount(); ++i)
    require(line->getControlPoint(i) == source->getStroke(0)->getControlPoint(i),
            "unselected stroke geometry changed");
  bool paintedBand = false;
  for (UINT r = 0; r < converted->getRegionCount(); ++r) {
    auto *region = converted->getRegion(r);
    if (region->getStyle() <= 0) continue;
    if (region->contains(TPointD(12,0))) {
      paintedBand = true;
      require(converted->getPalette()->getStyle(region->getStyle())->getMainColor().m == 128,
              "contour alpha does not match source opacity");
      require(!region->getRegion(TPointD(0,5)) ||
                  region->getRegion(TPointD(0,5))->getStyle() == 0,
              "hollow ring center was filled");
      bool aboveLine = false;
      for (UINT s = lineIndex + 1; s < converted->getStrokeCount(); ++s)
        aboveLine |= converted->sameGroupStrokeAndRegion(s, r);
      require(aboveLine, "converted top stroke moved underneath unselected line");
    }
  }
  require(paintedBand, "ring band did not receive paint");

  auto filled = layeredFixture(true);
  auto withFill = ContourOutline::convertImage(filled, {1});
  bool preserved = false;
  for (UINT r = 0; r < withFill->getRegionCount(); ++r) {
    auto *root = withFill->getRegion(r);
    if (!root->contains(TPointD(0,5))) continue;
    auto *inner = root->getRegion(TPointD(0,5));
    preserved |= inner && inner->getStyle() == 1;
  }
  require(preserved, "existing center fill lost during ring conversion");
  filled->getPalette()->setKeyframe(filled->getStroke(1)->getStyle(), 0);
  bool refused = false;
  try { ContourOutline::convertImage(filled, {1}); }
  catch (const std::runtime_error &) { refused = true; }
  require(refused && filled->getStrokeCount() == 2,
          "animated style changed without an explicit unsupported-appearance refusal");
  auto locked = layeredFixture(false);
  locked->getPalette()->setIsLocked(true);
  refused = false;
  try { ContourOutline::convertImage(locked, {1}); }
  catch (const std::runtime_error &) { refused = true; }
  require(refused && locked->getPalette()->getStyleCount() == originalCount,
          "locked palette was modified for an opacity style");
}

}  // namespace

//------------------------------------------------------------------

int main() {
  try {
    testStraightRound();
    testTaperedButt();
    testProjectingCap();
    testArc();
    testCorner();
    testWavy();
    testNotConvertible();
    testNativeLayeredConversion();
  } catch (const std::exception &e) {
    std::cerr << "contour_outline_test failed: " << e.what() << std::endl;
    return 1;
  }

  std::cout << "contour_outline_test passed" << std::endl;
  return 0;
}
