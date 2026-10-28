// Standalone checks of the width profile arithmetic; no GUI or application
// initialization.
// g++ -std=c++17 -I../../include widthprofile_test.cpp -o
// /tmp/widthprofile_test
#include "tools/widthprofile.h"
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <initializer_list>
#include <iostream>
#include <string>
#include <vector>

using namespace WidthProfile;

static void fail(const char *what, int line) {
  std::cerr << "widthprofile_test failed at line " << line << ": " << what
            << '\n';
  std::exit(1);
}

#define CHECK(cond)                                                            \
  do {                                                                         \
    if (!(cond)) fail(#cond, __LINE__);                                        \
  } while (0)

static bool nearly(double a, double b, double eps = 1e-9) {
  return std::fabs(a - b) <= eps;
}

#define CHECK_NEAR(a, b)                                                       \
  do {                                                                         \
    if (!nearly((a), (b))) {                                                   \
      std::cerr << "got " << (a) << ", expected " << (b) << '\n';              \
      fail(#a " ~ " #b, __LINE__);                                             \
    }                                                                          \
  } while (0)

static Profile makeProfile(std::initializer_list<Point> points) {
  Profile p;
  p.name = "test";
  for (const Point &pt : points) p.points.push_back(pt);
  return p;
}

static void testEvaluate() {
  Profile p = makeProfile({Point(0.0, 0.2), Point(0.5, 1.0), Point(1.0, 0.0)});

  // The points themselves
  CHECK_NEAR(evaluate(p, 0.0), 0.2);
  CHECK_NEAR(evaluate(p, 0.5), 1.0);
  CHECK_NEAR(evaluate(p, 1.0), 0.0);
  // Linear in between
  CHECK_NEAR(evaluate(p, 0.25), 0.6);
  CHECK_NEAR(evaluate(p, 0.75), 0.5);
  // Clamped outside 0..1
  CHECK_NEAR(evaluate(p, -3.0), 0.2);
  CHECK_NEAR(evaluate(p, 7.0), 0.0);
  // Not a number counts as the start
  CHECK_NEAR(evaluate(p, std::nan("")), 0.2);

  // Profile that starts late and ends early keeps its end ratios
  Profile q = makeProfile({Point(0.25, 0.4), Point(0.75, 0.8)});
  CHECK_NEAR(evaluate(q, 0.0), 0.4);
  CHECK_NEAR(evaluate(q, 0.25), 0.4);
  CHECK_NEAR(evaluate(q, 0.5), 0.6);
  CHECK_NEAR(evaluate(q, 1.0), 0.8);

  // Two points at the same position make a step, no division by zero
  Profile step = makeProfile(
      {Point(0.0, 0.0), Point(0.5, 0.0), Point(0.5, 1.0), Point(1.0, 1.0)});
  CHECK_NEAR(evaluate(step, 0.25), 0.0);
  CHECK_NEAR(evaluate(step, 0.75), 1.0);
  double atStep = evaluate(step, 0.5);
  CHECK(atStep == 0.0 || atStep == 1.0);

  // Ratios outside 0..1 are clamped
  Profile wild = makeProfile({Point(0.0, -4.0), Point(1.0, 9.0)});
  CHECK(evaluate(wild, 0.0) >= 0.0);
  CHECK(evaluate(wild, 1.0) <= 1.0);
  CHECK(evaluate(wild, 0.5) >= 0.0 && evaluate(wild, 0.5) <= 1.0);

  // Degenerate profiles: empty is uniform, one point is a constant
  Profile empty;
  CHECK(empty.empty());
  CHECK_NEAR(evaluate(empty, 0.0), 1.0);
  CHECK_NEAR(evaluate(empty, 0.37), 1.0);
  Profile one = makeProfile({Point(0.3, 0.6)});
  CHECK_NEAR(evaluate(one, 0.0), 0.6);
  CHECK_NEAR(evaluate(one, 0.3), 0.6);
  CHECK_NEAR(evaluate(one, 1.0), 0.6);

  // normalize() sorts and clamps
  Profile messy =
      makeProfile({Point(0.8, 0.5), Point(-1.0, 2.0), Point(0.4, 0.1)});
  normalize(messy);
  CHECK(messy.points.size() == 3);
  CHECK_NEAR(messy.points[0].pos, 0.0);
  CHECK_NEAR(messy.points[0].ratio, 1.0);
  CHECK_NEAR(messy.points[1].pos, 0.4);
  CHECK_NEAR(messy.points[2].pos, 0.8);
}

static void testBuiltIns() {
  for (int kind = 0; kind < BUILTIN_COUNT; ++kind) {
    Profile p = builtIn(kind);
    CHECK(!p.empty());
    CHECK(p.name == builtInName(kind));
    CHECK(!p.name.empty());
    double last = -1.0;
    for (const Point &pt : p.points) {
      CHECK(pt.pos >= last);  // sorted
      CHECK(pt.pos >= 0.0 && pt.pos <= 1.0);
      CHECK(pt.ratio >= 0.0 && pt.ratio <= 1.0);
      last = pt.pos;
    }
    for (int i = 0; i <= 20; ++i) {
      double v = evaluate(p, i / 20.0);
      CHECK(v >= 0.0 && v <= 1.0);
    }
  }

  Profile uniform = builtIn(UNIFORM);
  for (int i = 0; i <= 20; ++i) CHECK_NEAR(evaluate(uniform, i / 20.0), 1.0);

  Profile both = builtIn(TAPER_BOTH);
  CHECK(evaluate(both, 0.0) < 0.01);
  CHECK(evaluate(both, 1.0) < 0.01);
  CHECK_NEAR(evaluate(both, 0.5), 1.0);
  CHECK_NEAR(evaluate(both, 0.4), 1.0);
  CHECK_NEAR(evaluate(both, 0.6), 1.0);
  // It rises and falls without a dip
  CHECK(evaluate(both, 0.05) < evaluate(both, 0.2));
  CHECK(evaluate(both, 0.95) < evaluate(both, 0.8));
  // Symmetric
  for (int i = 0; i <= 10; ++i)
    CHECK(
        nearly(evaluate(both, i / 10.0), evaluate(both, 1.0 - i / 10.0), 1e-9));

  Profile start = builtIn(TAPER_START);
  CHECK(evaluate(start, 0.0) < 0.01);
  CHECK_NEAR(evaluate(start, 1.0), 1.0);
  CHECK_NEAR(evaluate(start, 0.5), 1.0);

  Profile end = builtIn(TAPER_END);
  CHECK_NEAR(evaluate(end, 0.0), 1.0);
  CHECK_NEAR(evaluate(end, 0.5), 1.0);
  CHECK(evaluate(end, 1.0) < 0.01);

  Profile pulse = builtIn(PULSE);
  CHECK_NEAR(evaluate(pulse, 0.5), 1.0);
  CHECK(evaluate(pulse, 0.0) < 0.5);
  CHECK(evaluate(pulse, 0.0) > 0.0);  // thin, not pointed
  CHECK_NEAR(evaluate(pulse, 0.0), evaluate(pulse, 1.0));
  CHECK(evaluate(pulse, 0.25) < 1.0);

  // An unknown kind is the uniform profile
  Profile unknown = builtIn(-5);
  CHECK(unknown.name == "Uniform");
  CHECK_NEAR(evaluate(unknown, 0.3), 1.0);
  CHECK(std::string(builtInName(BUILTIN_COUNT)).empty());
}

static void testFromSamples() {
  // A stroke 100 long whose thickness goes 0 -> 10 -> 0 in a triangle
  std::vector<double> lens, thick;
  for (int i = 0; i <= 20; ++i) {
    double s = 5.0 * i;
    lens.push_back(s);
    thick.push_back(s <= 50.0 ? 0.2 * s : 0.2 * (100.0 - s));
  }
  Profile p = fromSamples(lens, thick);
  CHECK(!p.empty());
  CHECK_NEAR(evaluate(p, 0.0), 0.0);
  CHECK_NEAR(evaluate(p, 0.5), 1.0);
  CHECK_NEAR(evaluate(p, 1.0), 0.0);
  CHECK_NEAR(evaluate(p, 0.25), 0.5);
  CHECK_NEAR(evaluate(p, 0.9), 0.2);
  // The simplification kept the three corners only
  CHECK(p.points.size() == 3);

  // Scale does not matter, only the shape
  std::vector<double> bigThick;
  for (double t : thick) bigThick.push_back(t * 7.0);
  Profile big = fromSamples(lens, bigThick);
  CHECK_NEAR(evaluate(big, 0.25), evaluate(p, 0.25));

  // So does the origin of the lengths
  std::vector<double> shifted;
  for (double s : lens) shifted.push_back(s + 1000.0);
  Profile moved = fromSamples(shifted, thick);
  CHECK_NEAR(evaluate(moved, 0.25), 0.5);

  // Building a profile from the built-in one's own samples gives it back
  Profile taper = builtIn(TAPER_BOTH);
  std::vector<double> l2, t2;
  for (int i = 0; i <= 100; ++i) {
    l2.push_back(i);
    t2.push_back(3.0 * evaluate(taper, i / 100.0));
  }
  Profile rebuilt = fromSamples(l2, t2, 0.002);
  for (int i = 0; i <= 20; ++i)
    CHECK(nearly(evaluate(rebuilt, i / 20.0), evaluate(taper, i / 20.0), 0.01));

  // A constant thickness is a flat profile with two points
  std::vector<double> flat(lens.size(), 4.0);
  Profile uniform = fromSamples(lens, flat);
  CHECK(uniform.points.size() == 2);
  CHECK_NEAR(evaluate(uniform, 0.3), 1.0);

  // Degenerate input
  CHECK(fromSamples(std::vector<double>(), std::vector<double>()).empty());
  CHECK(fromSamples(lens, std::vector<double>(lens.size(), 0.0)).empty());
  Profile single =
      fromSamples(std::vector<double>(1, 5.0), std::vector<double>(1, 2.0));
  CHECK(single.points.size() == 1);
  CHECK_NEAR(evaluate(single, 0.5), 1.0);
  // A dot: samples at one place
  Profile dot =
      fromSamples(std::vector<double>(3, 8.0), std::vector<double>(3, 2.0));
  CHECK(dot.points.size() == 1);
  // Different counts use the shorter list
  Profile shortList = fromSamples(lens, std::vector<double>(2, 1.0));
  CHECK(!shortList.empty());
}

static void testPositionsAndMapping() {
  // Lengths of 7 control points (on and off the curve)
  std::vector<double> lens = {10.0, 12.0, 15.0, 22.0, 30.0, 33.0, 40.0};

  std::vector<double> pos = normalizedPositions(lens);
  CHECK(pos.size() == lens.size());
  CHECK_NEAR(pos.front(), 0.0);
  CHECK_NEAR(pos.back(), 1.0);
  for (size_t i = 1; i < pos.size(); ++i) CHECK(pos[i] >= pos[i - 1]);
  CHECK_NEAR(pos[3], 0.4);

  // Lengths that go backwards by rounding never give positions that do
  std::vector<double> bumpy    = {0.0, 5.0, 4.9999, 10.0};
  std::vector<double> bumpyPos = normalizedPositions(bumpy);
  for (size_t i = 1; i < bumpyPos.size(); ++i)
    CHECK(bumpyPos[i] >= bumpyPos[i - 1]);

  // Taper both ends mapped to the points: same count, scaled to the maximum
  Profile both          = builtIn(TAPER_BOTH);
  std::vector<double> t = mapToThickness(both, lens, 8.0);
  CHECK(t.size() == lens.size());
  double biggest = 0.0;
  for (size_t i = 0; i < t.size(); ++i) {
    CHECK(t[i] >= MinThickness - 1e-12);
    CHECK(t[i] <= 8.0 + 1e-12);
    biggest = std::max(biggest, t[i]);
  }
  CHECK_NEAR(biggest, 8.0);  // 40% of the length is on the plateau
  CHECK(t.front() < 0.1);    // tips are hairlines, not zero
  CHECK(t.back() < 0.1);
  CHECK(t.front() >= MinThickness - 1e-12);
  CHECK(t.back() >= MinThickness - 1e-12);

  // Uniform keeps the maximum everywhere
  std::vector<double> u = mapToThickness(builtIn(UNIFORM), lens, 3.0);
  for (double v : u) CHECK_NEAR(v, 3.0);

  // The floor is the one asked for, and never above the maximum
  std::vector<double> floored = mapToThickness(both, lens, 8.0, 1.5);
  for (double v : floored) CHECK(v >= 1.5 - 1e-12);
  std::vector<double> capped = mapToThickness(both, lens, 0.01, 1.5);
  for (double v : capped) CHECK(v <= 0.01 + 1e-12);

  // The tips of an end taper
  std::vector<double> e = mapToThickness(builtIn(TAPER_END), lens, 8.0);
  CHECK_NEAR(e.front(), 8.0);
  CHECK(e.back() < 0.1);
  for (size_t i = 1; i < e.size(); ++i) CHECK(e[i] <= e[i - 1] + 1e-12);

  // Degenerate input
  CHECK(mapToThickness(both, std::vector<double>(), 8.0).empty());
  // No thickness stays none
  std::vector<double> none = mapToThickness(both, lens, 0.0);
  CHECK(none.size() == lens.size());
  for (double v : none) CHECK(v == 0.0);
  // No length: a dot keeps its maximum
  std::vector<double> dot =
      mapToThickness(both, std::vector<double>(4, 12.0), 5.0);
  CHECK(dot.size() == 4);
  for (double v : dot) CHECK_NEAR(v, 5.0);
  // One control point
  std::vector<double> one =
      mapToThickness(both, std::vector<double>(1, 0.0), 5.0);
  CHECK(one.size() == 1);
  CHECK_NEAR(one[0], 5.0);
  // Empty profile is uniform; one-point profile is its ratio
  std::vector<double> emptyMap = mapToThickness(Profile(), lens, 6.0);
  for (double v : emptyMap) CHECK_NEAR(v, 6.0);
  std::vector<double> oneMap =
      mapToThickness(makeProfile({Point(0.5, 0.5)}), lens, 6.0);
  for (double v : oneMap) CHECK_NEAR(v, 3.0);
}

static void testFalloff() {
  CHECK_NEAR(falloffWeight(0.0, 10.0), 1.0);
  CHECK_NEAR(falloffWeight(10.0, 10.0), 0.0);
  CHECK_NEAR(falloffWeight(25.0, 10.0), 0.0);
  CHECK_NEAR(falloffWeight(5.0, 10.0), 0.5);
  CHECK_NEAR(falloffWeight(-5.0, 10.0), falloffWeight(5.0, 10.0));
  double last = 2.0;
  for (int i = 0; i <= 20; ++i) {
    double w = falloffWeight(i * 0.5, 10.0);
    CHECK(w >= 0.0 && w <= 1.0);
    CHECK(w <= last);  // never rises away from the center
    last = w;
  }
  // No radius: only the center point
  CHECK_NEAR(falloffWeight(0.0, 0.0), 1.0);
  CHECK_NEAR(falloffWeight(1.0, 0.0), 0.0);
  CHECK_NEAR(falloffWeight(0.0, -3.0), 1.0);

  // Weights of control points along an open stroke
  std::vector<double> lens = {0.0, 10.0, 20.0, 30.0, 40.0, 50.0};
  std::vector<double> w    = falloffWeights(lens, 20.0, 20.0);
  CHECK(w.size() == lens.size());
  CHECK_NEAR(w[2], 1.0);
  CHECK_NEAR(w[0], 0.0);
  CHECK_NEAR(w[4], 0.0);
  CHECK_NEAR(w[5], 0.0);
  CHECK_NEAR(w[1], w[3]);
  CHECK(w[1] > 0.0 && w[1] < 1.0);

  // On a closed stroke of length 50 the seam is not a step: points at 2 and
  // at 48 are both 4 away from 0
  std::vector<double> loop = {0.0, 2.0, 25.0, 48.0, 50.0};
  std::vector<double> lw   = falloffWeights(loop, 0.0, 10.0, true, 50.0);
  CHECK_NEAR(lw[0], 1.0);
  CHECK_NEAR(lw[4], 1.0);
  CHECK_NEAR(lw[1], lw[3]);
  CHECK_NEAR(lw[2], 0.0);
  std::vector<double> openW = falloffWeights(loop, 0.0, 10.0);
  CHECK_NEAR(openW[3], 0.0);  // not wrapped when open

  // The distance to the thickness
  CHECK_NEAR(thicknessFromDistance(5.0), 5.0);
  CHECK_NEAR(thicknessFromDistance(0.0), MinThickness);
  CHECK_NEAR(thicknessFromDistance(-2.0), MinThickness);
  CHECK_NEAR(thicknessFromDistance(1e9), MaxThickness);
  CHECK_NEAR(thicknessFromDistance(std::nan("")), MinThickness);

  // Blending: full change at the center, nothing outside
  CHECK_NEAR(blendThickness(4.0, 3.0, 1.0), 7.0);
  CHECK_NEAR(blendThickness(4.0, 3.0, 0.5), 5.5);
  CHECK_NEAR(blendThickness(4.0, 3.0, 0.0), 4.0);
  CHECK_NEAR(blendThickness(4.0, -100.0, 1.0), MinThickness);
  CHECK_NEAR(blendThickness(4.0, 1e9, 1.0), MaxThickness);
  // A zero thickness tip is left alone where nothing changes, and a thick
  // point already past the maximum is not cut
  CHECK_NEAR(blendThickness(0.0, -5.0, 0.7), 0.0);
  CHECK_NEAR(blendThickness(0.0, 5.0, 0.0), 0.0);
  CHECK_NEAR(blendThickness(300.0, 5.0, 0.0), 300.0);
}

static void testText() {
  Profile p =
      makeProfile({Point(0.0, 0.0), Point(0.35, 1.0), Point(1.0, 0.25)});
  p.name           = "My profile";
  std::string line = toText(p);
  CHECK(line.find('\n') == std::string::npos);

  Profile back;
  CHECK(fromText(line, back));
  CHECK(back.name == "My profile");
  CHECK(back.points.size() == 3);
  for (int i = 0; i <= 10; ++i)
    CHECK(nearly(evaluate(back, i / 10.0), evaluate(p, i / 10.0), 1e-4));

  // Several profiles, with junk lines around
  std::vector<Profile> all;
  addOrReplace(all, p);
  addOrReplace(all, builtIn(PULSE));
  Profile again = p;
  again.points.pop_back();
  addOrReplace(all, again);  // replaces the first
  CHECK(all.size() == 2);
  CHECK(all[0].points.size() == 2);

  std::string text = "# comment\n\n" + formatProfiles(all) +
                     "no tab here\nname\t\nname\t0.5\nname\tx:y\n\r\n";
  std::vector<Profile> parsed = parseProfiles(text);
  CHECK(parsed.size() == 2);
  CHECK(parsed[0].name == "My profile");
  CHECK(parsed[1].name == "Pulse");
  CHECK(parsed[1].points.size() == builtIn(PULSE).points.size());

  // Windows line ends
  std::vector<Profile> crlf = parseProfiles("a\t0:1,1:0.5\r\nb\t0:0,1:1\r\n");
  CHECK(crlf.size() == 2);
  CHECK(crlf[0].name == "a");
  CHECK_NEAR(evaluate(crlf[0], 1.0), 0.5);

  // Out of range values are clamped, points are sorted
  Profile odd;
  CHECK(fromText("odd\t1:5,0:-2", odd));
  CHECK(odd.points.size() == 2);
  CHECK_NEAR(odd.points[0].pos, 0.0);
  CHECK_NEAR(odd.points[0].ratio, 0.0);
  CHECK_NEAR(odd.points[1].ratio, 1.0);

  // A name cannot break the format
  Profile tabbed = makeProfile({Point(0.0, 1.0), Point(1.0, 1.0)});
  tabbed.name    = "bad\tname\nhere";
  Profile tabbedBack;
  CHECK(fromText(toText(tabbed), tabbedBack));
  CHECK(tabbedBack.points.size() == 2);

  // Rejected lines leave nothing behind
  Profile none = p;
  CHECK(!fromText("", none));
  CHECK(none.empty());
  CHECK(!fromText("\t0:1", none));
  CHECK(!fromText("name\t", none));
  CHECK(!fromText("name\t0.2", none));
}

int main() {
  testEvaluate();
  testBuiltIns();
  testFromSamples();
  testPositionsAndMapping();
  testFalloff();
  testText();
  std::cout << "widthprofile_test: all checks passed\n";
  return 0;
}
