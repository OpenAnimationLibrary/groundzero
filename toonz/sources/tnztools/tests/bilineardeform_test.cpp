// Standalone checks of the maths behind Free Deform (bilinear map of a box to
// four new corners, and the local area scale used for the line thickness); no
// GUI or application initialization.
// g++ -std=c++17 -DLINUX -I../../include bilineardeform_test.cpp -o
// /tmp/bilineardeform_test
#include "tools/bilineardeform.h"
#include <cmath>
#include <cstdlib>
#include <iostream>

using BilinearDeform::Map;

void check(bool condition, const char *what) {
  if (condition) return;
  std::cerr << "Check failed: " << what << '\n';
  std::exit(1);
}

void expect(double actual, double expected, const char *what) {
  if (std::abs(actual - expected) < 1e-9) return;
  std::cerr << what << ": expected " << expected << ", got " << actual << '\n';
  std::exit(1);
}

void expectPoint(const TPointD &actual, const TPointD &expected,
                 const char *what) {
  expect(actual.x, expected.x, what);
  expect(actual.y, expected.y, what);
}

// The area scale the way it is estimated on the original point: sample the map
// on both sides of the point along each axis and take the area of the
// parallelogram, relative to the area of the sampled square.
double sampledAreaScale(const Map &m, const TPointD &p, double eps) {
  TPointD dx = m.map(TPointD(p.x + eps, p.y)) - m.map(TPointD(p.x - eps, p.y));
  TPointD dy = m.map(TPointD(p.x, p.y + eps)) - m.map(TPointD(p.x, p.y - eps));
  return std::abs(cross(dx, dy)) / (4 * eps * eps);
}

int main() {
  const TPointD p00(0, 0), p11(10, 10);
  const TPointD probes[] = {TPointD(0, 0),  TPointD(10, 0), TPointD(10, 10),
                            TPointD(0, 10), TPointD(5, 5),  TPointD(2.5, 7),
                            TPointD(9, 1)};

  // Identity corners: identity map, area scale 1, thickness factor 1.
  {
    Map m(p00, p11, TPointD(0, 0), TPointD(10, 0), TPointD(10, 10),
          TPointD(0, 10));
    for (const TPointD &p : probes) {
      expectPoint(m.map(p), p, "identity map");
      expect(m.areaScale(p), 1, "identity area scale");
      expect(m.thicknessFactor(p), 1, "identity thickness factor");
    }
    check(!m.isDegenerate(), "identity box is not degenerate");
  }

  // The four corners go to the four new points.
  {
    TPointD a(1, 2), b(12, -1), c(14, 13), d(-2, 9);
    Map m(p00, p11, a, b, c, d);
    expectPoint(m.map(TPointD(0, 0)), a, "bottom left corner");
    expectPoint(m.map(TPointD(10, 0)), b, "bottom right corner");
    expectPoint(m.map(TPointD(10, 10)), c, "top right corner");
    expectPoint(m.map(TPointD(0, 10)), d, "top left corner");
    // the centre goes to the average of the corners for a bilinear map
    expectPoint(m.map(TPointD(5, 5)), 0.25 * (a + b + c + d), "centre");
  }

  // A uniform 2x scale of the corners: area scale 4, thickness factor 2.
  {
    Map m(p00, p11, TPointD(0, 0), TPointD(20, 0), TPointD(20, 20),
          TPointD(0, 20));
    for (const TPointD &p : probes) {
      expectPoint(m.map(p), TPointD(2 * p.x, 2 * p.y), "2x map");
      expect(m.areaScale(p), 4, "2x area scale");
      expect(m.thicknessFactor(p), 2, "2x thickness factor");
    }
  }

  // The original box need not touch the origin.
  {
    Map m(TPointD(100, -50), TPointD(104, -46), TPointD(0, 0), TPointD(8, 0),
          TPointD(8, 8), TPointD(0, 8));
    expectPoint(m.map(TPointD(102, -48)), TPointD(4, 4), "offset box map");
    expect(m.thicknessFactor(TPointD(102, -48)), 2, "offset box factor");
  }

  // A pure translation does not change the thickness.
  {
    Map m(p00, p11, TPointD(30, -5), TPointD(40, -5), TPointD(40, 5),
          TPointD(30, 5));
    for (const TPointD &p : probes) {
      expectPoint(m.map(p), p + TPointD(30, -5), "translation map");
      expect(m.areaScale(p), 1, "translation area scale");
      expect(m.thicknessFactor(p), 1, "translation thickness factor");
    }
  }

  // A mirror keeps the area scale positive.
  {
    Map m(p00, p11, TPointD(10, 0), TPointD(0, 0), TPointD(0, 10),
          TPointD(10, 10));
    expect(m.areaScale(TPointD(3, 4)), 1, "mirror area scale");
    expect(m.thicknessFactor(TPointD(3, 4)), 1, "mirror thickness factor");
  }

  // A corner pinch: the top right corner is pulled to the middle of the box.
  // With s and t the position in the box, the area scale is 1 - (s + t) / 2.
  {
    Map m(p00, p11, TPointD(0, 0), TPointD(10, 0), TPointD(5, 5),
          TPointD(0, 10));
    expect(m.areaScale(TPointD(0, 0)), 1, "pinch far corner");
    expect(m.areaScale(TPointD(5, 5)), 0.5, "pinch centre");
    expect(m.thicknessFactor(TPointD(5, 5)), std::sqrt(0.5),
           "pinch centre factor");
    expect(m.areaScale(TPointD(10, 0)), 0.5, "pinch bottom right");
    expect(m.areaScale(TPointD(10, 10)), 0, "pinch tip");
    for (const TPointD &p : probes) {
      double f = m.thicknessFactor(p);
      check(std::isfinite(f) && f >= 0, "pinch factor is finite");
    }
    check(std::abs(m.thicknessFactor(TPointD(5, 5)) - 1) > 0.1,
          "pinch changes the thickness");

    // The area scale agrees with the numeric estimate around the original
    // point (not around the deformed one).
    for (const TPointD &p : probes)
      expect(m.areaScale(p), sampledAreaScale(m, p, 1e-2),
             "pinch matches sampled estimate");
  }

  // A general quadrilateral agrees with the sampled estimate too.
  {
    Map m(TPointD(-3, 4), TPointD(7, 9), TPointD(-6, 1), TPointD(11, 3),
          TPointD(13, 17), TPointD(-8, 12));
    const TPointD pts[] = {TPointD(-3, 4), TPointD(0, 6), TPointD(7, 9),
                           TPointD(2.2, 8.1)};
    for (const TPointD &p : pts) {
      expect(m.areaScale(p), sampledAreaScale(m, p, 1e-2),
             "quad matches sampled estimate");
      check(m.thicknessFactor(p) > 0, "quad factor is positive");
    }
  }

  // Degenerate boxes (no width or no height) must not divide by zero.
  {
    Map flat(TPointD(3, 0), TPointD(3, 10), TPointD(0, 0), TPointD(5, 0),
             TPointD(5, 5), TPointD(0, 5));
    check(flat.isDegenerate(), "zero width box is degenerate");
    TPointD q = flat.map(TPointD(3, 4));
    check(std::isfinite(q.x) && std::isfinite(q.y), "zero width map finite");
    expect(flat.areaScale(TPointD(3, 4)), 1, "zero width area scale");
    expect(flat.thicknessFactor(TPointD(3, 4)), 1, "zero width factor");

    Map thin(TPointD(0, 7), TPointD(10, 7), TPointD(0, 0), TPointD(5, 0),
             TPointD(5, 5), TPointD(0, 5));
    check(thin.isDegenerate(), "zero height box is degenerate");
    q = thin.map(TPointD(4, 7));
    check(std::isfinite(q.x) && std::isfinite(q.y), "zero height map finite");
    expect(thin.thicknessFactor(TPointD(4, 7)), 1, "zero height factor");

    Map point(TPointD(1, 1), TPointD(1, 1), TPointD(0, 0), TPointD(1, 0),
              TPointD(1, 1), TPointD(0, 1));
    expect(point.thicknessFactor(TPointD(1, 1)), 1, "empty box factor");
  }

  std::cout << "bilineardeform_test: all checks passed\n";
  return 0;
}
