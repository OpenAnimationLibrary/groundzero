// Standalone checks of the maths behind the Envelope mode of the vector
// Selection tool (handles around a box, neighbours, validation of a moved
// envelope, thickness factor, subdivision); no GUI or application
// initialization. The warp itself is done by TEnvelopeDeformer, which needs the
// whole of toonzlib, so the tests that need a map use a small mean value
// coordinates evaluation of their own.
// g++ -std=c++17 -DLINUX -I../../include envelopelayout_test.cpp -o
// /tmp/envelopelayout_test
#include "tools/envelopelayout.h"
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <vector>

using namespace EnvelopeLayout;

namespace {

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

const double kNaN = std::numeric_limits<double>::quiet_NaN();
const double kInf = std::numeric_limits<double>::infinity();

// The 8 handles of the box from (0, 0) to (40, 20)
std::vector<TPointD> box() { return perimeter(TPointD(0, 0), TPointD(40, 20)); }

// A simple mean value coordinates map, as in Floater's paper: the point p of
// the polygon \b from goes to the same combination of the vertices of \b to.
// Valid inside the polygon, which is all the tests need.
class MeanValueMap {
  std::vector<TPointD> m_from, m_to;

public:
  MeanValueMap(const std::vector<TPointD> &from, const std::vector<TPointD> &to)
      : m_from(from), m_to(to) {}

  TPointD operator()(const TPointD &p) const {
    const int n = (int)m_from.size();
    std::vector<double> r(n), t(n);
    for (int i = 0; i < n; ++i) {
      TPointD a = m_from[i] - p, b = m_from[(i + 1) % n] - p;
      r[i] = norm(a);
      // p on a vertex or on an edge: linear interpolation along the edge
      if (r[i] < 1e-12) return m_to[i];
      const double rb = norm(b);
      if (rb < 1e-12) return m_to[(i + 1) % n];
      if (std::abs(cross(a, b)) < 1e-12 * r[i] * rb && a * b < 0) {
        const double s = r[i] / (r[i] + rb);
        return m_to[i] + (m_to[(i + 1) % n] - m_to[i]) * s;
      }
      // tan(alpha / 2) where alpha is the signed angle from a to b
      t[i] = cross(a, b) / (r[i] * rb + a * b);
    }
    TPointD sum;
    double wSum = 0;
    for (int i = 0; i < n; ++i) {
      const double w = (t[(i + n - 1) % n] + t[i]) / r[i];
      sum += m_to[i] * w;
      wSum += w;
    }
    return sum * (1.0 / wSum);
  }
};

// The polygon scaled about the origin
std::vector<TPointD> scaled(const std::vector<TPointD> &poly, double sx,
                            double sy) {
  std::vector<TPointD> out;
  for (const TPointD &p : poly) out.push_back(TPointD(p.x * sx, p.y * sy));
  return out;
}

// The polygon moved by delta
std::vector<TPointD> moved(const std::vector<TPointD> &poly,
                           const TPointD &delta) {
  std::vector<TPointD> out;
  for (const TPointD &p : poly) out.push_back(p + delta);
  return out;
}

void testLayout() {
  // Corners and middles, counter clockwise from the bottom left corner
  const std::vector<TPointD> h = box();
  check(h.size() == 8, "8 handles for the default layout");
  expectPoint(h[0], TPointD(0, 0), "handle 0 is the bottom left corner");
  expectPoint(h[1], TPointD(20, 0), "handle 1 is the middle of the bottom");
  expectPoint(h[2], TPointD(40, 0), "handle 2 is the bottom right corner");
  expectPoint(h[3], TPointD(40, 10), "handle 3 is the middle of the right");
  expectPoint(h[4], TPointD(40, 20), "handle 4 is the top right corner");
  expectPoint(h[5], TPointD(20, 20), "handle 5 is the middle of the top");
  expectPoint(h[6], TPointD(0, 20), "handle 6 is the top left corner");
  expectPoint(h[7], TPointD(0, 10), "handle 7 is the middle of the left");

  check(signedArea(h) > 0, "the handles run counter clockwise");
  expect(signedArea(h), 40.0 * 20.0, "the handles enclose the box");
  expect(diagonal(h), std::sqrt(40.0 * 40.0 + 20.0 * 20.0), "diagonal");

  // The box given by its four corners in the order of the Selection tool
  const std::vector<TPointD> same =
      perimeter(TPointD(0, 0), TPointD(40, 0), TPointD(40, 20), TPointD(0, 20));
  check(same.size() == h.size(), "same number of handles from the corners");
  for (size_t i = 0; i < h.size(); ++i)
    expectPoint(same[i], h[i], "corners give the same handles as the box");

  // A slanted quadrilateral: the middles are the middles of the sides
  const std::vector<TPointD> q =
      perimeter(TPointD(0, 0), TPointD(10, 2), TPointD(12, 12), TPointD(-2, 8));
  expectPoint(q[1], TPointD(5, 1), "middle of the bottom side");
  expectPoint(q[3], TPointD(11, 7), "middle of the right side");
  expectPoint(q[5], TPointD(5, 10), "middle of the top side");
  expectPoint(q[7], TPointD(-1, 4), "middle of the left side");

  // Other numbers of segments per side
  check(handleCount() == 8, "default handle count");
  check(handleCount(1) == 4, "one segment per side is the four corners");
  check(handleCount(3) == 12, "three segments per side");
  check(handleCount(0) == 4, "at least one segment per side");
  const std::vector<TPointD> four =
      perimeter(TPointD(0, 0), TPointD(40, 20), 1);
  check(four.size() == 4, "four handles");
  expectPoint(four[2], TPointD(40, 20), "top right of the four corners");
  const std::vector<TPointD> twelve =
      perimeter(TPointD(0, 0), TPointD(30, 30), 3);
  check(twelve.size() == 12, "twelve handles");
  expectPoint(twelve[1], TPointD(10, 0), "a third of the bottom side");
  expectPoint(twelve[2], TPointD(20, 0), "two thirds of the bottom side");
  expectPoint(twelve[3], TPointD(30, 0), "bottom right of twelve");
  check(isCorner(0) && !isCorner(1) && isCorner(2) && !isCorner(7),
        "corners of the default layout");
  check(isCorner(3, 3) && !isCorner(4, 3) && !isCorner(12, 3),
        "corners with three segments per side");
}

void testNeighbours() {
  check(nextIndex(0, 8) == 1, "next of 0");
  check(nextIndex(7, 8) == 0, "next of the last wraps to the first");
  check(previousIndex(0, 8) == 7, "previous of the first wraps to the last");
  check(previousIndex(5, 8) == 4, "previous of 5");
  check(neighbour(6, 3, 8) == 1, "three after 6 wraps");
  check(neighbour(1, -3, 8) == 6, "three before 1 wraps");
  check(neighbour(2, 16, 8) == 2, "a full turn or more");
  check(neighbour(2, -17, 8) == 1, "several turns back");
  check(neighbour(3, 1, 0) == 0, "no handles");

  // Each handle is both the next of its previous and the previous of its next
  for (int i = 0; i < 8; ++i) {
    check(nextIndex(previousIndex(i, 8), 8) == i, "next of previous");
    check(previousIndex(nextIndex(i, 8), 8) == i, "previous of next");
  }
}

void testToolNumbers() {
  // The Selection tool numbers the handles of its box:
  // 0 BL, 1 BR, 2 TR, 3 TL corners, 4 bottom, 5 right, 6 top, 7 left middles.
  const std::vector<TPointD> h = box();
  const TPointD fromTool[8] = {TPointD(0, 0),   TPointD(40, 0), TPointD(40, 20),
                               TPointD(0, 20),  TPointD(20, 0), TPointD(40, 10),
                               TPointD(20, 20), TPointD(0, 10)};
  for (int toolPoint = 0; toolPoint < 8; ++toolPoint) {
    const int handle = handleFromToolPoint(toolPoint);
    check(handle >= 0 && handle < 8, "a handle for each tool point");
    expectPoint(h[handle], fromTool[toolPoint], "tool point and its handle");
    check(toolPointFromHandle(handle) == toolPoint, "round trip tool point");
  }
  for (int handle = 0; handle < 8; ++handle)
    check(handleFromToolPoint(toolPointFromHandle(handle)) == handle,
          "round trip handle");
  check(handleFromToolPoint(8) == kNoHandle, "NONE has no handle");
  check(handleFromToolPoint(-1) == kNoHandle, "negative has no handle");
  check(toolPointFromHandle(8) == kNoHandle, "no tool point past the end");
  check(toolPointFromHandle(-1) == kNoHandle, "no tool point before the start");
}

void testFindHandle() {
  const std::vector<TPointD> h = box();
  check(findHandle(h, TPointD(1, 1), 3) == 0, "near the bottom left corner");
  check(findHandle(h, TPointD(21, -1), 3) == 1, "near the bottom middle");
  check(findHandle(h, TPointD(39, 11), 3) == 3, "near the right middle");
  check(findHandle(h, TPointD(20, 10), 3) == kNoHandle,
        "the centre is no handle");
  check(findHandle(h, TPointD(5, 5), 3) == kNoHandle,
        "too far from any handle");
  check(findHandle(std::vector<TPointD>(), TPointD(0, 0), 3) == kNoHandle,
        "no handles");
  // Between two handles: the nearest one wins
  check(findHandle(h, TPointD(8, 0), 50) == 0, "nearest of two");
  check(findHandle(h, TPointD(12, 0), 50) == 1, "nearest of two, the other");
}

void testValidation() {
  const std::vector<TPointD> rest = box();

  // A normal polygon is accepted, and so is the rest itself
  check(isUsableRest(rest), "the box is a usable rest envelope");
  check(isValidDeformation(rest, rest), "no move is valid");
  check(isSimple(rest), "the box is simple");

  // A handle pulled out, a corner pinched in, all the box moved or scaled
  {
    std::vector<TPointD> d = rest;
    d[5]                   = TPointD(20, 35);  // top middle pulled up
    check(isValidDeformation(rest, d), "a pulled handle");
    d    = rest;
    d[4] = TPointD(30, 12);  // top right corner pinched in
    check(isValidDeformation(rest, d), "a pinched corner");
    check(isValidDeformation(rest, moved(rest, TPointD(100, -50))),
          "a translation");
    check(isValidDeformation(rest, scaled(rest, 3, 0.5)), "a scale");
    check(isValidDeformation(rest, scaled(rest, 0.05, 0.05)),
          "a big reduction is allowed");
  }

  // A concave envelope is fine as long as it does not cross itself
  {
    std::vector<TPointD> d = rest;
    d[5]                   = TPointD(20, 5);  // top middle pushed deep inside
    check(isValidDeformation(rest, d), "a dent in the top");
  }

  // Not a number, not finite
  {
    std::vector<TPointD> d = rest;
    d[2]                   = TPointD(kNaN, 0);
    check(!isValidDeformation(rest, d), "NaN is rejected");
    check(!isSimple(d), "NaN is not simple");
    d[2] = TPointD(0, kInf);
    check(!isValidDeformation(rest, d), "infinity is rejected");
    d[2] = TPointD(-kInf, 0);
    check(!isValidDeformation(rest, d), "minus infinity is rejected");
  }

  // Collapsed to a point, to a line, or without area
  {
    std::vector<TPointD> point(8, TPointD(7, 7));
    check(!isValidDeformation(rest, point), "collapsed to a point");

    std::vector<TPointD> line;
    for (int i = 0; i < 8; ++i) line.push_back(TPointD(5.0 * i, 2.0 * i));
    check(!isValidDeformation(rest, line), "collapsed to a line");

    // All the box squeezed flat: zero height
    check(!isValidDeformation(rest, scaled(rest, 1, 0)), "zero height");
    check(!isValidDeformation(rest, scaled(rest, 0, 1)), "zero width");
    // Almost flat, below the limit
    check(!isValidDeformation(rest, scaled(rest, 1, 1e-5)), "almost flat");

    // Two neighbours on the same place
    std::vector<TPointD> d = rest;
    d[1]                   = d[0];
    check(!isValidDeformation(rest, d), "a handle on the one before it");
  }

  // Bow tie: two edges cross
  {
    std::vector<TPointD> bow;
    bow.push_back(TPointD(0, 0));
    bow.push_back(TPointD(10, 10));
    bow.push_back(TPointD(10, 0));
    bow.push_back(TPointD(0, 10));
    check(!isSimple(bow), "a bow tie crosses itself");
    check(!isUsableRest(bow), "a bow tie is no rest envelope");

    // The 8 handles of the box with two opposite corners swapped
    std::vector<TPointD> d = rest;
    std::swap(d[2], d[6]);
    check(!isValidDeformation(rest, d), "swapped corners");

    // The left middle pulled across to the right of the right edge
    d    = rest;
    d[7] = TPointD(55, 10);
    check(!isValidDeformation(rest, d),
          "the left side pulled through the right");

    // A middle pushed through the opposite side
    d    = rest;
    d[1] = TPointD(20, 25);
    check(!isValidDeformation(rest, d), "the bottom pushed through the top");
  }

  // Turned inside out: the same shape in the other direction
  {
    std::vector<TPointD> reversed(rest.rbegin(), rest.rend());
    check(isSimple(reversed), "the reversed box is simple");
    check(!isValidDeformation(rest, reversed), "reversed order is rejected");
    // Mirrored: left and right swapped
    check(!isValidDeformation(rest, scaled(rest, -1, 1)), "a mirror");
  }

  // A polygon that goes back over itself
  {
    std::vector<TPointD> fold;
    fold.push_back(TPointD(0, 0));
    fold.push_back(TPointD(10, 0));
    fold.push_back(TPointD(4, 0));  // back along the edge it came by
    fold.push_back(TPointD(4, 8));
    check(!isSimple(fold), "an edge that folds back");
  }

  // Different sizes, too few points, a rest that is itself degenerate
  {
    check(!isValidDeformation(
              rest, std::vector<TPointD>(rest.begin(), rest.begin() + 7)),
          "different number of vertices");
    check(!isUsableRest(std::vector<TPointD>(rest.begin(), rest.begin() + 2)),
          "two points are no polygon");
    check(!isUsableRest(std::vector<TPointD>()), "no points");
    check(!isUsableRest(scaled(rest, 1, 0)), "a flat rest envelope");
    check(!isValidDeformation(scaled(rest, 1, 0), rest),
          "nothing is valid from a flat rest envelope");
    check(!isUsableRest(std::vector<TPointD>(8, TPointD(3, 3))),
          "a rest envelope reduced to a point");
  }
}

void testConstrainMove() {
  const std::vector<TPointD> rest = box();

  // A valid target is taken as it is
  expectPoint(constrainMove(rest, rest, 5, TPointD(20, 40)), TPointD(20, 40),
              "valid target");

  // Pulling the top middle down through the bottom stops before the bottom
  {
    const TPointD target(20, -30);
    const TPointD got = constrainMove(rest, rest, 5, target);
    check(got.y > 0, "stops above the bottom side");
    check(got.y < 20, "moved down");
    expect(got.x, 20, "stays on the path");
    std::vector<TPointD> d = rest;
    d[5]                   = got;
    check(isValidDeformation(rest, d), "the result is valid");
    // and it is close to the limit: a little farther would not be valid
    d[5] = TPointD(got.x, got.y - 0.01);
    check(!isValidDeformation(rest, d), "the result is at the limit");
  }

  // A bad handle number or target leaves things where they are
  check(constrainMove(rest, rest, 8, TPointD(1, 1)) == TPointD(),
        "bad index gives the origin");
  expectPoint(constrainMove(rest, rest, 2, TPointD(kNaN, 3)), rest[2],
              "NaN target keeps the handle");
  expectPoint(constrainMove(rest, rest, 2, TPointD(kInf, 3)), rest[2],
              "infinite target keeps the handle");
}

void testIdentityMap() {
  // The identity envelope gives the identity map, inside the box
  const std::vector<TPointD> rest = box();
  const MeanValueMap identity(rest, rest);
  const TPointD probes[] = {
      TPointD(20, 10),    TPointD(1, 1), TPointD(39, 19), TPointD(5, 15),
      TPointD(33.3, 2.2), TPointD(0, 7), TPointD(20, 0),  TPointD(40, 20)};
  for (const TPointD &p : probes)
    expectPoint(identity(p), p, "identity envelope, identity map");

  // A translation and a scale of the envelope move and scale the points
  const MeanValueMap shift(rest, moved(rest, TPointD(7, -3)));
  const MeanValueMap twice(rest, scaled(rest, 2, 2));
  for (const TPointD &p : probes) {
    expectPoint(shift(p), p + TPointD(7, -3), "translated envelope");
    expectPoint(twice(p), TPointD(2 * p.x, 2 * p.y), "scaled envelope");
  }

  // The points on the outline follow the moved handles exactly
  std::vector<TPointD> d = rest;
  d[5]                   = TPointD(20, 35);  // top middle up
  const MeanValueMap bent(rest, d);
  expectPoint(bent(TPointD(20, 20)), TPointD(20, 35), "the handle itself");
  expectPoint(bent(TPointD(10, 20)), TPointD(10, 27.5), "half way on the edge");
  expectPoint(bent(TPointD(0, 20)), TPointD(0, 20), "the corner stays");
  expectPoint(bent(TPointD(30, 20)), TPointD(30, 27.5), "half way, other side");
  expectPoint(bent(TPointD(20, 0)), TPointD(20, 0), "the bottom stays");
}

void testThickness() {
  const std::vector<TPointD> rest = box();
  const double step               = kJacobianStepFraction * diagonal(rest);
  const TPointD p(15, 8);

  // Nothing moved: area scale 1, thickness factor 1
  {
    const MeanValueMap m(rest, rest);
    expect(areaScale(m, p, step), 1, "identity area scale");
    expect(thicknessFactor(m, p, step), 1, "identity thickness factor");
  }

  // A pure translation does not change the thickness
  {
    const MeanValueMap m(rest, moved(rest, TPointD(30, 12)));
    expect(thicknessFactor(m, p, step), 1, "translation thickness factor");
  }

  // A 2x scale: area 4, thickness 2. A 3x1 stretch: area 3, thickness sqrt(3)
  {
    const MeanValueMap m(rest, scaled(rest, 2, 2));
    expect(areaScale(m, p, step), 4, "2x area scale");
    expect(thicknessFactor(m, p, step), 2, "2x thickness factor");

    const MeanValueMap s(rest, scaled(rest, 3, 1));
    expect(thicknessFactor(s, p, step), std::sqrt(3.0), "3x1 thickness factor");

    const MeanValueMap h(rest, scaled(rest, 0.5, 0.5));
    expect(thicknessFactor(h, p, step), 0.5, "half size thickness factor");
  }

  // A bend that keeps the area keeps the thickness: a shear
  {
    std::vector<TPointD> sheared;
    for (const TPointD &q : rest)
      sheared.push_back(TPointD(q.x + 0.5 * q.y, q.y));
    const MeanValueMap m(rest, sheared);
    expect(thicknessFactor(m, p, step), 1, "shear thickness factor");
  }

  // Always finite and inside the limits
  {
    const MeanValueMap huge(rest, scaled(rest, 1000, 1000));
    expect(thicknessFactor(huge, p, step), kMaxThicknessFactor,
           "clamped from above");
    const MeanValueMap tiny(rest, scaled(rest, 0.001, 0.001));
    expect(thicknessFactor(tiny, p, step), kMinThicknessFactor,
           "clamped from below");

    // A map without any area: the factor is the lower limit
    auto flat = [](const TPointD &q) { return TPointD(q.x, 0); };
    expect(thicknessFactor(flat, p, step), kMinThicknessFactor, "flat map");

    // A map that gives NaN or infinity: no change
    auto bad = [](const TPointD &q) { return TPointD(kNaN, q.y); };
    expect(thicknessFactor(bad, p, step), 1, "NaN map");
    auto infinite = [](const TPointD &q) { return TPointD(kInf, q.y); };
    expect(thicknessFactor(infinite, p, step), 1, "infinite map");

    // A step that is not a positive number: no change
    const MeanValueMap m(rest, scaled(rest, 2, 2));
    expect(thicknessFactor(m, p, 0), 1, "zero step");
    expect(thicknessFactor(m, p, -1), 1, "negative step");
    expect(thicknessFactor(m, p, kNaN), 1, "NaN step");
  }
}

void testPieces() {
  const double diag = 100;
  check(piecesForChunk(0, diag) == 1, "an empty chunk is kept");
  check(piecesForChunk(1, diag) == 1, "a short chunk is kept");
  check(piecesForChunk(diag / 16, diag) == 1, "a chunk of the piece size");
  check(piecesForChunk(diag / 16 * 1.01, diag) == 2,
        "a bit longer is cut in two");
  check(piecesForChunk(diag / 4, diag) == 4, "a quarter of the diagonal");
  check(piecesForChunk(diag, diag) == kMaxPieces, "as long as the diagonal");
  check(piecesForChunk(1e9, diag) == kMaxPieces, "never more than the limit");
  check(piecesForChunk(50, 0) == 1, "no envelope size");
  check(piecesForChunk(50, -3) == 1, "negative envelope size");
  check(piecesForChunk(kNaN, diag) == 1, "NaN length");
  check(piecesForChunk(50, kNaN) == 1, "NaN envelope size");
  check(piecesForChunk(kInf, diag) == 1, "infinite length");
  check(piecesForChunk(-5, diag) == 1, "negative length");
}

}  // namespace

int main() {
  testLayout();
  testNeighbours();
  testToolNumbers();
  testFindHandle();
  testValidation();
  testConstrainMove();
  testIdentityMap();
  testThickness();
  testPieces();
  std::cout << "envelopelayout_test: all checks passed\n";
  return 0;
}
