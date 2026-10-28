// Standalone checks of the pure maths behind the Gradient Transform tool; no
// GUI or application initialization.
// Build by hand:
//   g++ -std=c++17 -DLINUX -I../../include gradienthandles_test.cpp
#include "tools/gradienthandles.h"

#include <cmath>
#include <iostream>
#include <limits>
#include <vector>

using namespace GradientHandles;

namespace {

int failures = 0;

void check(bool condition, const char *what) {
  if (condition) return;
  std::cerr << "Check failed: " << what << '\n';
  failures++;
}

bool isNear(double a, double b, double eps = 1e-9) {
  return std::fabs(a - b) <= eps;
}

bool isNearPoint(const TPointD &a, const TPointD &b, double eps = 1e-9) {
  return isNear(a.x, b.x, eps) && isNear(a.y, b.y, eps);
}

bool isFinitePoint(const TPointD &p) {
  return std::isfinite(p.x) && std::isfinite(p.y);
}

// Same angle, treating -180 and 180 as the same direction.
bool sameAngle(double a, double b, double eps = 1e-7) {
  double d = std::fabs(normalizeAngle(a - b));
  return d <= eps || std::fabs(d - 360.0) <= eps;
}

// What TLinGradFillStyle::getRects / TRadGradFillStyle::drawRegion compute for
// the centre, written the way the style does it.
TPointD styleCenter(const TRectD &bbox, double xPos, double yPos) {
  double lx = bbox.x1 - bbox.x0;
  double ly = bbox.y1 - bbox.y0;
  TPointD center((bbox.x1 + bbox.x0) / 2.0, (bbox.y1 + bbox.y0) / 2.0);
  return TPointD(center.x + xPos * 0.01 * lx * 0.5,
                 center.y + yPos * 0.01 * ly * 0.5);
}

std::vector<TRectD> sampleBoxes() {
  std::vector<TRectD> boxes;
  boxes.push_back(TRectD(0, 0, 200, 200));         // square
  boxes.push_back(TRectD(-100, -50, 300, 50));     // wide, around the origin
  boxes.push_back(TRectD(40, -300, 100, 500));     // tall, off centre
  boxes.push_back(TRectD(-37.5, -12.25, -2, 80));  // fractional, negative x
  return boxes;
}

void testHelpers() {
  check(isNear(normalizeAngle(350), -10), "350 wraps to -10");
  check(isNear(normalizeAngle(-10), -10), "-10 stays");
  check(isNear(normalizeAngle(-190), 170), "-190 wraps to 170");
  check(isNear(normalizeAngle(190), -170), "190 wraps to -170");
  check(isNear(normalizeAngle(720), 0), "720 wraps to 0");
  check(isNear(normalizeAngle(180), 180), "180 stays");
  check(isNear(normalizeAngle(-180), -180), "-180 stays");
  check(isNear(normalizeAngle(540), 180), "540 wraps to 180");
  check(isNear(normalizeAngle(std::numeric_limits<double>::infinity()), 0),
        "infinity gives 0");

  check(isNear(snapAngle(7, 15), 0), "7 snaps to 0");
  check(isNear(snapAngle(8, 15), 15), "8 snaps to 15");
  check(isNear(snapAngle(-8, 15), -15), "-8 snaps to -15");
  check(isNear(snapAngle(178, 15), 180), "178 snaps to 180");
  check(isNear(snapAngle(33, 0), 33), "no step, no snap");

  check(isNear(angleOfVector(0, 5, 99), 90), "up is 90");
  check(isNear(angleOfVector(-3, 0, 99), 180), "left is 180");
  check(isNear(angleOfVector(0, -2, 99), -90), "down is -90");
  check(isNear(angleOfVector(0, 0, 42), 42), "null vector uses the fallback");

  check(kindFromTagId(LinearGradientTagId) == Linear, "linear tag");
  check(kindFromTagId(RadialGradientTagId) == Radial, "radial tag");
  check(kindFromTagId(1137) == NoGradient, "other tag");
  check(kindFromTagId(0) == NoGradient, "zero tag");

  check(isNear(clampValue(5, 0, 3), 3) && isNear(clampValue(-5, 0, 3), 0) &&
            isNear(clampValue(2, 0, 3), 2),
        "clamp");
}

void testStyleDefaults() {
  // TLinGradFillStyle(color): angle 0, x 0, y 0, size 100.
  TRectD box(0, 0, 200, 200);
  LinearHandles h = linearParamsToHandles(box, LinearParams());
  check(isNearPoint(h.center, TPointD(100, 100)), "default linear centre");
  check(isNearPoint(h.head, TPointD(200, 100)), "default linear head");
  check(isNearPoint(h.tail, TPointD(0, 100)), "default linear tail");

  // TRadGradFillStyle(color): x 0, y 0, radius 50, smooth 50.
  RadialHandles r = radialParamsToHandles(box, RadialParams());
  check(isNearPoint(r.center, TPointD(100, 100)), "default radial centre");
  check(isNear(r.outerRadius, 50), "default radial outer radius");
  check(isNear(r.innerRadius, 25), "default radial inner radius");
  check(isNearPoint(r.outer, TPointD(150, 100)), "default radial outer handle");
  check(isNearPoint(r.inner, TPointD(75, 100)), "default radial inner handle");

  // Hand-computed offset: wide box, x 50 %, y -100 %.
  TRectD wide(-100, -50, 300, 50);  // lx 400, ly 100, centre (100, 0)
  LinearHandles w = linearParamsToHandles(wide, LinearParams(90, 50, -100, 10));
  check(isNearPoint(w.center, TPointD(200, -50)), "offset centre");
  check(isNearPoint(w.head, TPointD(200, -40)), "offset head at 90 degrees");
  check(isNearPoint(w.tail, TPointD(200, -60)), "offset tail at 90 degrees");

  // Radius is a percent of half the LONGER side.
  RadialHandles rw =
      radialParamsToHandles(wide, RadialParams(0, 0, 100, 100), 0.0);
  check(isNear(rw.outerRadius, 200), "radius 100 % is half the longer side");
  check(isNear(rw.innerRadius, 0), "smooth 100 % has no solid centre");
  RadialHandles rt = radialParamsToHandles(TRectD(0, 0, 60, 400),
                                           RadialParams(0, 0, 10, 25), 0.0);
  check(isNear(rt.outerRadius, 20), "tall box radius");
  check(isNear(rt.innerRadius, 15), "tall box inner radius");
}

void testMatchesStyleFormulas() {
  const double xs[] = {-100, -37.5, 0, 12.25, 100};
  const double ys[] = {-100, -5, 0, 64, 100};
  for (const TRectD &box : sampleBoxes()) {
    for (double x : xs) {
      for (double y : ys) {
        TPointD expected = styleCenter(box, x, y);
        check(isNearPoint(gradientCenter(box, x, y), expected),
              "gradientCenter matches the style");
        check(isNearPoint(
                  linearParamsToHandles(box, LinearParams(0, x, y, 7)).center,
                  expected),
              "linear centre matches the style");
        RadialParams rp(x, y, 40, 30);
        check(isNearPoint(radialParamsToHandles(box, rp).center, expected),
              "radial centre matches the style");
        double lx = box.x1 - box.x0, ly = box.y1 - box.y0;
        double r1 = 0.5 * std::max(lx, ly) * rp.radius * 0.01;
        double r0 = r1 * (100.0 - rp.smooth) * 0.01;
        check(isNear(radialOuterRadius(box, rp), r1), "outer radius matches");
        check(isNear(radialInnerRadius(box, rp), r0), "inner radius matches");
      }
    }
  }
}

void testLinearRoundTrip() {
  const LinearParams sets[] = {
      LinearParams(0, 0, 0, 100),        LinearParams(45, 25, -30, 60),
      LinearParams(-120, -80, 90, 3),    LinearParams(90, 100, 100, 500),
      LinearParams(-179, -100, -100, 1), LinearParams(135.5, 12.5, 7.25, 222)};
  for (const TRectD &box : sampleBoxes()) {
    for (const LinearParams &p : sets) {
      LinearHandles h = linearParamsToHandles(box, p);
      check(isFinitePoint(h.center) && isFinitePoint(h.head) &&
                isFinitePoint(h.tail),
            "linear handles are finite");

      LinearParams c = linearMoveCenter(box, p, h.center);
      check(isNear(c.x, p.x, 1e-7) && isNear(c.y, p.y, 1e-7),
            "linear centre round trip");
      check(isNear(c.angle, p.angle) && isNear(c.size, p.size),
            "moving the centre keeps angle and size");

      LinearParams a = linearDragEnd(box, p, LinearHead, h.head);
      check(sameAngle(a.angle, p.angle), "linear head angle round trip");
      check(isNear(a.size, p.size, 1e-7), "linear head size round trip");
      check(isNear(a.x, p.x) && isNear(a.y, p.y),
            "dragging an end keeps the centre");

      LinearParams b = linearDragEnd(box, p, LinearTail, h.tail);
      check(sameAngle(b.angle, p.angle), "linear tail angle round trip");
      check(isNear(b.size, p.size, 1e-7), "linear tail size round trip");
    }
  }
}

void testLinearBehaviour() {
  TRectD box(0, 0, 200, 100);  // centre (100, 50)
  LinearParams p(0, 0, 0, 100);

  // Drag the head straight up, 30 units from the centre.
  LinearParams up = linearDragEnd(box, p, LinearHead, TPointD(100, 80));
  check(isNear(up.angle, 90) && isNear(up.size, 30), "head dragged up");
  // Dragging the tail up turns the line the other way round.
  LinearParams tailUp = linearDragEnd(box, p, LinearTail, TPointD(100, 80));
  check(isNear(tailUp.angle, -90) && isNear(tailUp.size, 30),
        "tail dragged up");

  // Size is clamped to what the style accepts.
  LinearParams big = linearDragEnd(box, p, LinearHead, TPointD(5000, 50));
  check(isNear(big.size, LinearSizeMax) && isNear(big.angle, 0), "size max");
  LinearParams tiny = linearDragEnd(box, p, LinearHead, TPointD(100.2, 50));
  check(isNear(tiny.size, LinearSizeMin), "size min");

  // A handle dropped on the centre keeps the angle.
  LinearParams same = linearDragEnd(box, LinearParams(33, 0, 0, 80), LinearHead,
                                    TPointD(100, 50));
  check(isNear(same.angle, 33) && isNear(same.size, LinearSizeMin),
        "handle on the centre keeps the angle");

  // Snapping.
  LinearParams snapped =
      linearDragEnd(box, p, LinearHead, TPointD(100 + 50, 50 + 12), 15.0);
  check(isNear(snapped.angle, 15), "angle snapped to 15 degrees");
  check(isNear(snapped.size, std::hypot(50, 12)), "snapping keeps the length");

  // The centre is clamped to the box.
  LinearParams outside = linearMoveCenter(box, p, TPointD(1000, -1000));
  check(isNear(outside.x, PercentMax) && isNear(outside.y, PercentMin),
        "centre clamped to the box");
  LinearParams edge = linearMoveCenter(box, p, TPointD(0, 100));
  check(isNear(edge.x, -100) && isNear(edge.y, 100),
        "box corner is -100 / +100");

  // Angle wrap: 350 and -10 describe the same line.
  LinearHandles h350 =
      linearParamsToHandles(box, LinearParams(350, 10, -10, 80));
  LinearHandles hm10 =
      linearParamsToHandles(box, LinearParams(-10, 10, -10, 80));
  check(isNearPoint(h350.head, hm10.head) && isNearPoint(h350.tail, hm10.tail),
        "350 and -10 give the same handles");
  LinearParams back =
      linearDragEnd(box, LinearParams(350, 10, -10, 80), LinearHead, h350.head);
  check(isNear(back.angle, -10, 1e-7), "350 comes back as -10");
  LinearParams back180 = linearDragEnd(
      box, LinearParams(-180, 0, 0, 50), LinearHead,
      linearParamsToHandles(box, LinearParams(-180, 0, 0, 50)).head);
  check(sameAngle(back180.angle, 180), "-180 and 180 are the same direction");
  check(back180.angle >= LinearAngleMin && back180.angle <= LinearAngleMax,
        "angle stays inside the style range");
}

void testRadialRoundTrip() {
  const RadialParams sets[] = {
      RadialParams(0, 0, 50, 50), RadialParams(-60, 30, 12.5, 80),
      RadialParams(100, -100, 100, 1), RadialParams(20, 20, 0.5, 99),
      RadialParams(-5.5, 77, 33.3, 66.6)};
  const double handleAngles[] = {0, 37, -90, 180, 123.4};
  for (const TRectD &box : sampleBoxes()) {
    for (const RadialParams &p : sets) {
      for (double angle : handleAngles) {
        RadialHandles h = radialParamsToHandles(box, p, angle);
        check(isFinitePoint(h.center) && isFinitePoint(h.outer) &&
                  isFinitePoint(h.inner),
              "radial handles are finite");

        RadialParams c = radialMoveCenter(box, p, h.center);
        check(isNear(c.x, p.x, 1e-7) && isNear(c.y, p.y, 1e-7),
              "radial centre round trip");
        check(isNear(c.radius, p.radius) && isNear(c.smooth, p.smooth),
              "moving the centre keeps radius and smooth");

        RadialParams o = radialDragOuter(box, p, h.outer);
        check(isNear(o.radius, p.radius, 1e-7), "outer handle round trip");
        check(isNear(o.smooth, p.smooth) && isNear(o.x, p.x),
              "dragging the outer handle keeps the rest");

        RadialParams i = radialDragInner(box, p, h.inner);
        check(isNear(i.smooth, p.smooth, 1e-6), "inner handle round trip");
        check(isNear(i.radius, p.radius) && isNear(i.y, p.y),
              "dragging the inner handle keeps the rest");

        // The outer handle is on the outer circle, the inner one opposite.
        check(isNear(std::hypot(h.outer.x - h.center.x, h.outer.y - h.center.y),
                     h.outerRadius, 1e-7),
              "outer handle on its circle");
        check(isNear(std::hypot(h.inner.x - h.center.x, h.inner.y - h.center.y),
                     h.innerRadius, 1e-7),
              "inner handle on its circle");
      }
    }
  }
}

void testRadialClamping() {
  TRectD box(0, 0, 400, 200);    // longer side 400, centre (200, 100)
  RadialParams p(0, 0, 50, 50);  // outer radius 100, inner 50

  RadialParams huge = radialDragOuter(box, p, TPointD(200 + 5000, 100));
  check(isNear(huge.radius, RadialRadiusMax), "radius max");
  RadialParams none = radialDragOuter(box, p, TPointD(200, 100));
  check(isNear(none.radius, RadialRadiusMin), "radius min");
  RadialParams half = radialDragOuter(box, p, TPointD(200, 100 + 50));
  check(isNear(half.radius, 25), "radius 25 % at 50 units");

  RadialParams centre = radialDragInner(box, p, TPointD(200, 100));
  check(isNear(centre.smooth, RadialSmoothMax), "inner handle on centre: 100");
  RadialParams onOuter = radialDragInner(box, p, TPointD(200 + 100, 100));
  check(isNear(onOuter.smooth, RadialSmoothMin), "inner handle on outer: min");
  RadialParams beyond = radialDragInner(box, p, TPointD(200 + 400, 100));
  check(isNear(beyond.smooth, RadialSmoothMin), "inner handle beyond: min");
  RadialParams mid = radialDragInner(box, p, TPointD(200, 100 - 25));
  check(isNear(mid.smooth, 75),
        "inner handle at a quarter of the outer radius");

  RadialParams moved = radialMoveCenter(box, p, TPointD(-1000, 1000));
  check(isNear(moved.x, PercentMin) && isNear(moved.y, PercentMax),
        "radial centre clamped to the box");
}

void testDegenerateBoxes() {
  std::vector<TRectD> flat;
  flat.push_back(TRectD());                // empty rect: all zero
  flat.push_back(TRectD(10, 20, 10, 20));  // a point
  flat.push_back(TRectD(10, 0, 10, 100));  // zero width
  flat.push_back(TRectD(0, 5, 100, 5));    // zero height
  flat.push_back(TRectD(30, 30, 20, 20));  // inverted
  const TPointD targets[] = {TPointD(0, 0), TPointD(55, -12), TPointD(1e6, 1e6),
                             TPointD(10, 20)};
  for (const TRectD &box : flat) {
    LinearParams lp(30, 20, -40, 80);
    RadialParams rp(20, -40, 60, 40);
    LinearHandles lh = linearParamsToHandles(box, lp);
    RadialHandles rh = radialParamsToHandles(box, rp, 15);
    check(isFinitePoint(lh.center) && isFinitePoint(lh.head) &&
              isFinitePoint(lh.tail),
          "flat box: linear handles are finite");
    check(isFinitePoint(rh.center) && isFinitePoint(rh.outer) &&
              isFinitePoint(rh.inner),
          "flat box: radial handles are finite");
    for (const TPointD &t : targets) {
      LinearParams a = linearMoveCenter(box, lp, t);
      check(std::isfinite(a.x) && std::isfinite(a.y), "flat box: finite move");
      check(a.x >= PercentMin && a.x <= PercentMax && a.y >= PercentMin &&
                a.y <= PercentMax,
            "flat box: percent in range");
      LinearParams b = linearDragEnd(box, lp, LinearHead, t, 15.0);
      check(std::isfinite(b.angle) && std::isfinite(b.size),
            "flat box: finite linear drag");
      check(b.size >= LinearSizeMin && b.size <= LinearSizeMax,
            "flat box: size in range");
      RadialParams c = radialMoveCenter(box, rp, t);
      check(std::isfinite(c.x) && std::isfinite(c.y),
            "flat box: finite radial move");
      RadialParams d = radialDragOuter(box, rp, t);
      RadialParams e = radialDragInner(box, rp, t);
      check(std::isfinite(d.radius) && std::isfinite(e.smooth),
            "flat box: finite radius and smooth");
      check(d.radius >= RadialRadiusMin && d.radius <= RadialRadiusMax,
            "flat box: radius in range");
      check(e.smooth >= RadialSmoothMin && e.smooth <= RadialSmoothMax,
            "flat box: smooth in range");
    }
  }

  // Zero width: x cannot be told from the pointer and stays; y still follows.
  TRectD narrow(10, 0, 10, 100);
  LinearParams keep =
      linearMoveCenter(narrow, LinearParams(0, 33, 0, 100), TPointD(500, 75));
  check(isNear(keep.x, 33), "zero width keeps X");
  check(isNear(keep.y, 50), "zero width still sets Y");
  // Zero height.
  TRectD low(0, 5, 100, 5);
  RadialParams keepY =
      radialMoveCenter(low, RadialParams(0, -20, 50, 50), TPointD(75, 500));
  check(isNear(keepY.y, -20), "zero height keeps Y");
  check(isNear(keepY.x, 50), "zero height still sets X");
  // A box without any extent cannot size a radius: values stay.
  RadialParams same = radialDragOuter(
      TRectD(10, 20, 10, 20), RadialParams(0, 0, 40, 60), TPointD(99, 99));
  check(isNear(same.radius, 40) && isNear(same.smooth, 60),
        "point box keeps the radius");
  RadialParams sameInner = radialDragInner(
      TRectD(10, 20, 10, 20), RadialParams(0, 0, 40, 60), TPointD(99, 99));
  check(isNear(sameInner.smooth, 60), "point box keeps the smoothness");
}

void testArrays() {
  double v[4] = {0, 0, 0, 0};
  LinearParams lp(-30, 5, 6, 77);
  linearParamsToArray(lp, v);
  check(isNear(v[0], -30) && isNear(v[1], 5) && isNear(v[2], 6) &&
            isNear(v[3], 77),
        "linear array order is Angle, X, Y, Size");
  LinearParams lb = linearParamsFromArray(v);
  check(isNear(lb.angle, -30) && isNear(lb.x, 5) && isNear(lb.y, 6) &&
            isNear(lb.size, 77),
        "linear array round trip");

  RadialParams rp(1, 2, 33, 44);
  radialParamsToArray(rp, v);
  check(isNear(v[0], 1) && isNear(v[1], 2) && isNear(v[2], 33) &&
            isNear(v[3], 44),
        "radial array order is X, Y, Radius, Smooth");
  RadialParams rb = radialParamsFromArray(v);
  check(isNear(rb.x, 1) && isNear(rb.y, 2) && isNear(rb.radius, 33) &&
            isNear(rb.smooth, 44),
        "radial array round trip");
}

}  // namespace

int main() {
  testHelpers();
  testStyleDefaults();
  testMatchesStyleFormulas();
  testLinearRoundTrip();
  testLinearBehaviour();
  testRadialRoundTrip();
  testRadialClamping();
  testDegenerateBoxes();
  testArrays();

  if (failures) {
    std::cerr << "gradienthandles_test: " << failures << " check(s) failed\n";
    return 1;
  }
  std::cout << "gradienthandles_test: all checks passed\n";
  return 0;
}
