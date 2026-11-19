// SPDX-License-Identifier: BSD-3-Clause

// A speed-in/speed-out segment of a function curve is a cubic Bezier whose
// frame coordinate x(u) is inverted to find u for a frame. When the cubic
// term of x(u) is zero or nearly zero, the inversion used to miss the root and
// return u = 1, so the segment gave its end value in between its keys.

#include "tcubicbezier.h"

#include <cmath>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>

namespace {

void require(bool value, const std::string& message) {
  if (!value) throw std::runtime_error(message);
}

struct Segment {
  double x0, outX, inX, x3;
};

std::string describe(const Segment& s, double x) {
  std::ostringstream out;
  out.precision(17);
  out << "segment " << s.x0 << ".." << s.x3 << " handles " << s.outX << ", "
      << s.inX << " at frame " << x;
  return out.str();
}

double bezierX(const Segment& s, double u) {
  double v = 1 - u;
  return s.x0 * v * v * v + 3 * (s.x0 + s.outX) * u * v * v +
         3 * (s.x3 + s.inX) * u * u * v + s.x3 * u * u * u;
}

// x(u) does not decrease on [0, 1] when the handles stay within the segment
double referenceU(const Segment& s, double x) {
  double lo = 0, hi = 1;
  for (int i = 0; i < 200; ++i) {
    double mid = 0.5 * (lo + hi);
    if (bezierX(s, mid) < x)
      lo = mid;
    else
      hi = mid;
  }
  return 0.5 * (lo + hi);
}

double solvedU(const Segment& s, double x) {
  return invCubicBezierX(x, TPointD(s.x0, 0), TPointD(s.outX, 0),
                         TPointD(s.inX, 0), TPointD(s.x3, 0));
}

void requireInverse(const Segment& s, int samples) {
  for (int k = 1; k < samples; ++k) {
    double x = s.x0 + (s.x3 - s.x0) * k / samples;
    double u = solvedU(s, x);
    require(std::fabs(u - referenceU(s, x)) <= 1e-9,
            "u matches the reference for " + describe(s, x));
  }
}

void reportedSegmentIsInterpolated() {
  // x(u) = 42 + 6 u^2, so frame 43 is at u = 1 / sqrt(6)
  double u = 1 / std::sqrt(6.0);
  double expected =
      74 + 46.5 * u - 121.5 * u * u + 85.5 * u * u * u;  // about 79.71
  double value = getCubicBezierY(43, TPointD(42, 74), TPointD(0, 15.5),
                                 TPointD(-4, -20), TPointD(48, 84.5));
  require(std::fabs(value - expected) <= 1e-9,
          "frame 43 of the reported segment is interpolated, not 84.5");
  require(std::fabs(solvedU({42, 0, -4, 48}, 43) - u) <= 1e-12,
          "frame 43 of the reported segment is at u = 1 / sqrt(6)");
}

void nearlyQuadraticSegmentsAreInverted() {
  // The cubic term of x(u) is span + 3 (outX - inX) - 3 span, which vanishes
  // for inX = outX - 2 span / 3; offset that by a relative delta.
  const double spans[]  = {1, 6, 37.5, 480};
  const double outs[]   = {0, 1.0 / 6, 1.0 / 3, 0.5, 2.0 / 3};
  const double deltas[] = {0,      1e-14, -1e-14, 1e-12, -1e-12, 1e-10,
                           -1e-10, 1e-8,  -1e-8,  1e-6,  -1e-6,  1e-4,
                           -1e-4,  1e-3,  -1e-3,  1e-2,  -1e-2};
  for (double span : spans)
    for (double out : outs)
      for (double delta : deltas) {
        double inX = (out - 2.0 / 3 + delta) * span;
        if (inX > 0) continue;
        requireInverse({100, out * span, inX, 100 + span}, 50);
      }
}

void linearAndZeroHandleSegmentsAreInverted() {
  // Handles of a third of the span make x(u) linear
  for (int k = 1; k < 30; ++k) {
    double x = 10 + 9.0 * k / 30;
    require(std::fabs(solvedU({10, 3, -3, 19}, x) - k / 30.0) <= 1e-12,
            "a linear segment gives u proportional to the frame");
  }
  requireInverse({0, 0, 0, 12}, 60);
  requireInverse({5, 0, -2, 11}, 60);
  requireInverse({5, 2, 0, 11}, 60);
}

void flatEndIsInverted() {
  // With no incoming handle x(u) is flat at u = 1; near that end the nearly
  // quadratic solve starts from u = 1, where a Newton step is infinite.
  const Segment s = {0, 4 + 6e-5, 0, 6};
  for (int k = 3; k <= 10; ++k) {
    double x = 6 - 6 * std::pow(10.0, -k);
    double u = solvedU(s, x);
    require(std::fabs(u - referenceU(s, x)) <= 1e-9,
            "u matches the reference for " + describe(s, x));
  }
}

void ordinarySegmentsAreInverted() {
  // Not both handles at full length: x(u) then has a flat inflection, where
  // u is only defined to about the cube root of the rounding error
  const double outs[] = {0.1, 0.25, 0.5, 0.9};
  const double ins[]  = {-0.05, -0.2, -0.7, -1};
  for (double out : outs)
    for (double in : ins) requireInverse({-20, out * 24, in * 24, 4}, 48);
}

}  // namespace

int main() {
  try {
    reportedSegmentIsInterpolated();
    nearlyQuadraticSegmentsAreInverted();
    linearAndZeroHandleSegmentsAreInverted();
    flatEndIsInverted();
    ordinarySegmentsAreInverted();
  } catch (const std::exception& error) {
    std::cerr << "cubicbezier: FAILED: " << error.what() << '\n';
    return 1;
  }
  std::cout << "cubicbezier: PASS\n";
  return 0;
}
