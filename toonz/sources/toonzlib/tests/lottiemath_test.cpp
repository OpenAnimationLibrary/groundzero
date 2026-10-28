// Regression test of the Lottie export maths (toonz/lottiemath.h): the
// transform conversions (the y flip, rotations, non-uniform and negative
// scales), the quadratic to cubic Bezier conversion and the path builder, the
// keyframe reduction, the colors and the rejection of NaN and infinities.

#include "toonz/lottiemath.h"

#include <QJsonArray>
#include <QJsonObject>
#include <QJsonValue>

#include <cmath>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <vector>

using namespace LottieMath;

namespace {

void require(bool condition, const char *message) {
  if (!condition) throw std::runtime_error(message);
}

bool near(double a, double b, double eps = 1e-9) {
  return std::abs(a - b) <= eps;
}

bool near(const Point &a, const Point &b, double eps = 1e-9) {
  return near(a.x, b.x, eps) && near(a.y, b.y, eps);
}

// T(x, y) * R(rotation) * S(sx, sy), as a Lottie player composes a transform
// with a zero anchor
Affine recompose(const Transform &t) {
  return translation(t.m_x, t.m_y) * rotation(t.m_rotation) *
         scaling(t.m_sx / 100.0, t.m_sy / 100.0);
}

bool sameAffine(const Affine &a, const Affine &b, double eps = 1e-9) {
  return a.isNear(b, eps, eps);
}

const double c_nan = std::numeric_limits<double>::quiet_NaN();
const double c_inf = std::numeric_limits<double>::infinity();

//------------------------------------------------------------------

void testNumbers() {
  require(num(c_nan) == 0.0, "num: NaN");
  require(num(c_inf) == 0.0, "num: infinity");
  require(num(-c_inf) == 0.0, "num: -infinity");
  require(num(1e300) == c_maxValue, "num: clamped");
  require(num(-1e300) == -c_maxValue, "num: clamped below");
  require(!std::signbit(num(-0.0)), "num: no -0");
  require(!std::signbit(num(-0.0004, 0.001)), "num: no rounded -0");
  require(near(num(1.23456, 0.01), 1.23), "num: rounding");
  require(finite(c_nan, 5.0) == 5.0, "finite: fallback");
  require(near(normalizeDegrees(540.0), 180.0), "normalize: 540");
  require(near(normalizeDegrees(-190.0), 170.0), "normalize: -190");
  require(near(unwrapDegrees(-170.0, 170.0), 190.0), "unwrap");
}

//------------------------------------------------------------------

void testTransforms() {
  // The y flip: camera pixels (y-up, centered) to the composition (y-down,
  // top left origin)
  require(near(toComposition(Point(10, 20), 200, 100), Point(110, 30)),
          "point: y flip");
  require(near(toComposition(Point(0, 0), 200, 100), Point(100, 50)),
          "point: camera center");

  Transform t = decompose(toComposition(translation(10, 20), 200, 100));
  require(near(t.m_x, 110) && near(t.m_y, 30), "root: position flipped");
  require(near(t.m_rotation, 0) && near(t.m_sx, 100) && near(t.m_sy, 100),
          "root: no rotation or scale");

  // A local translation of the stage, up, is a move up in Lottie
  t = decompose(flipY(translation(3, 4)));
  require(near(t.m_x, 3) && near(t.m_y, -4), "local: y flipped");

  // A stage rotation by 90 degrees (counterclockwise) is -90 in Lottie
  // (clockwise positive), and the x axis points up on screen
  Affine r90 = flipY(rotation(90));
  t          = decompose(r90);
  require(near(t.m_rotation, -90), "rotation 90: angle");
  require(near(t.m_sx, 100) && near(t.m_sy, 100), "rotation 90: scale");
  require(near(r90 * Point(1, 0), Point(0, -1)), "rotation 90: x axis up");
  require(sameAffine(recompose(t), r90), "rotation 90: round trip");

  // Non-uniform scale
  t = decompose(flipY(scaling(2, 3)));
  require(near(t.m_sx, 200) && near(t.m_sy, 300) && near(t.m_rotation, 0),
          "scale: non-uniform");
  require(near(t.m_shear, 0), "scale: no shear");

  Affine rs = flipY(translation(5, -7) * rotation(30) * scaling(2, 0.5));
  t         = decompose(rs);
  require(near(t.m_rotation, -30) && near(t.m_sx, 200) && near(t.m_sy, 50),
          "rotation and scale");
  require(near(t.m_x, 5) && near(t.m_y, 7), "rotation and scale: position");
  require(sameAffine(recompose(t), rs), "rotation and scale: round trip");

  // A mirror keeps the matrix
  Affine mirror = flipY(scaling(-1, 1));
  t             = decompose(mirror);
  require(sameAffine(recompose(t), mirror), "mirror: round trip");
  require(t.m_sx * t.m_sy < 0, "mirror: one negative scale");

  // Unwrapped against the previous frame: no spin through 360 degrees
  Transform previous;
  previous.m_rotation = 170;
  t                   = decompose(rotation(-170), &previous);
  require(near(t.m_rotation, -170 + 360), "unwrap: continues past 180");
  require(sameAffine(recompose(t), rotation(-170)), "unwrap: round trip");

  // The twin decomposition nearest to the previous rotation
  previous.m_rotation = 180;
  t                   = decompose(scaling(-1, -1), &previous);
  require(near(t.m_rotation, 180) && near(t.m_sx, 100) && near(t.m_sy, 100),
          "twin: rotation kept");

  // Shear is measured, not written
  t = decompose(Affine(1, 1, 0, 0, 1, 0));
  require(near(t.m_shear, std::sqrt(0.5), 1e-9), "shear: measured");

  // Degenerate and invalid transforms
  t = decompose(scaling(0, 0));
  require(near(t.m_sx, 0) && near(t.m_sy, 0), "degenerate: zero scale");
  t = decompose(Affine(c_nan, 0, 0, 0, 1, c_inf));
  require(std::isfinite(t.m_x) && std::isfinite(t.m_y) &&
              std::isfinite(t.m_rotation) && std::isfinite(t.m_sx) &&
              std::isfinite(t.m_sy),
          "NaN: finite transform");

  // Inverse
  Affine a = translation(3, 4) * rotation(25) * scaling(2, 3);
  require(sameAffine(a * a.inv(), Affine()), "inverse");
  require(sameAffine(scaling(0, 1).inv(), Affine()), "inverse: singular");
}

//------------------------------------------------------------------

void testBezier() {
  Point c1, c2;
  quadraticToCubic(Point(0, 0), Point(3, 3), Point(6, 0), c1, c2);
  require(near(c1, Point(2, 2)) && near(c2, Point(4, 2)),
          "quadratic to cubic: control points");

  PathBuilder builder;
  builder.addQuadratic(Point(0, 0), Point(3, 3), Point(6, 0), true);
  std::vector<Path> paths = builder.paths();
  require(paths.size() == 1 && paths[0].m_v.size() == 2, "path: vertices");
  require(
      near(paths[0].m_v[0], Point(0, 0)) && near(paths[0].m_v[1], Point(6, 0)),
      "path: end points");
  require(near(paths[0].m_out[0], Point(2, 2)) &&
              near(paths[0].m_in[1], Point(-2, 2)),
          "path: relative tangents");
  require(near(paths[0].m_in[0], Point()) && near(paths[0].m_out[1], Point()),
          "path: free ends");
  require(!paths[0].m_closed, "path: open");

  // Sub-curves, forward and backward
  Point q0, q1, q2;
  subQuadratic(Point(0, 0), Point(3, 3), Point(6, 0), 0, 1, q0, q1, q2);
  require(
      near(q0, Point(0, 0)) && near(q1, Point(3, 3)) && near(q2, Point(6, 0)),
      "sub-quadratic: whole");
  subQuadratic(Point(0, 0), Point(3, 3), Point(6, 0), 1, 0, q0, q1, q2);
  require(
      near(q0, Point(6, 0)) && near(q1, Point(3, 3)) && near(q2, Point(0, 0)),
      "sub-quadratic: reversed");
  subQuadratic(Point(0, 0), Point(3, 3), Point(6, 0), 0, 0.5, q0, q1, q2);
  require(near(q0, Point(0, 0)) && near(q1, Point(1.5, 1.5)) &&
              near(q2, Point(3, 1.5)),
          "sub-quadratic: first half");

  // A closed loop: the last vertex, equal to the first, is merged into it
  PathBuilder square;
  square.moveTo(Point(0, 0));
  square.lineTo(Point(10, 0));
  square.lineTo(Point(10, 10));
  square.lineTo(Point(0, 10));
  square.lineTo(Point(0, 0));
  square.close();
  paths = square.paths();
  require(paths.size() == 1 && paths[0].m_v.size() == 4 && paths[0].m_closed,
          "square: closed with 4 vertices");

  PathBuilder loop;
  loop.addQuadratic(Point(0, 0), Point(5, -5), Point(10, 0), true);
  loop.addQuadratic(Point(10, 0), Point(5, 5), Point(0, 0), true);
  loop.close();
  paths = loop.paths();
  require(paths.size() == 1 && paths[0].m_v.size() == 2, "loop: merged");
  // The tangent into the merged vertex comes from the last segment
  Point expectedIn = Point(0, 0) + (2.0 / 3.0) * (Point(5, 5) - Point(0, 0));
  require(near(paths[0].m_in[0], expectedIn), "loop: tangent kept");

  // Gaps: joined by a line, or a new sub-path
  PathBuilder joined;
  joined.addQuadratic(Point(0, 0), Point(1, 1), Point(2, 0), true);
  joined.addQuadratic(Point(3, 0), Point(4, 1), Point(5, 0), true);
  paths = joined.paths();
  require(paths.size() == 1 && paths[0].m_v.size() == 4, "gap: joined");
  PathBuilder split;
  split.addQuadratic(Point(0, 0), Point(1, 1), Point(2, 0), false);
  split.addQuadratic(Point(3, 0), Point(4, 1), Point(5, 0), false);
  require(split.paths().size() == 2, "gap: new sub-path");

  // Degenerate segments are dropped
  PathBuilder dot;
  dot.addQuadratic(Point(1, 1), Point(1, 1), Point(1, 1), true);
  require(dot.paths().empty(), "degenerate: no path");

  // JSON
  QJsonObject sh = pathShape(joined.paths().front());
  require(sh["ty"].toString() == "sh", "sh: type");
  QJsonObject k = sh["ks"].toObject()["k"].toObject();
  require(k["v"].toArray().size() == 4 && k["i"].toArray().size() == 4 &&
              k["o"].toArray().size() == 4,
          "sh: vertex counts");
}

//------------------------------------------------------------------

std::vector<std::vector<double>> scalars(const std::vector<double> &v) {
  std::vector<std::vector<double>> values;
  for (double x : v) values.push_back(std::vector<double>(1, x));
  return values;
}

void testKeys() {
  require(keptKeys(scalars({})).empty(), "keys: none");
  require(keptKeys(scalars({4, 4, 4, 4})).size() == 1, "keys: constant");
  require(keptKeys(scalars({0, 1, 2, 3})) == std::vector<int>({0, 3}),
          "keys: constant speed");
  require(keptKeys(scalars({0, 1, 2, 2, 2})) == std::vector<int>({0, 2, 4}),
          "keys: run of equal values");
  require(
      keptKeys(scalars({0, 0, 5, 0, 0})) == std::vector<int>({0, 1, 2, 3, 4}),
      "keys: spike kept");
  require(keptKeys(scalars({0, 1.001, 2}), 0.01).size() == 2,
          "keys: within tolerance");
  require(keptKeys(scalars({0, 1.5, 2}), 0.01).size() == 3,
          "keys: beyond tolerance");

  std::vector<std::vector<double>> xy = {{0, 0}, {1, 5}, {2, 10}, {3, 10}};
  require(keptKeys(xy) == std::vector<int>({0, 2, 3}), "keys: 2 dimensions");

  require(keptHoldKeys(scalars({1, 1, 2, 2, 3})) == std::vector<int>({0, 2, 4}),
          "hold keys: changes");

  // Static and animated properties
  QJsonObject p = animatedProperty(scalars({7, 7, 7}), 0, 0, 0, true);
  require(
      p["a"].toInt() == 0 && p["k"].isDouble() && near(p["k"].toDouble(), 7),
      "property: static scalar");
  p = animatedProperty(xy, 10, 0, 0, false);
  require(p["a"].toInt() == 1, "property: animated");
  QJsonArray keys = p["k"].toArray();
  require(keys.size() == 3, "property: key count");
  require(near(keys[0].toObject()["t"].toDouble(), 10) &&
              near(keys[1].toObject()["t"].toDouble(), 12) &&
              near(keys[2].toObject()["t"].toDouble(), 13),
          "property: key times");
  require(keys[0].toObject().contains("e") &&
              keys[0].toObject()["i"].toObject()["x"].toArray().size() == 2,
          "property: linear easing per dimension");
  require(!keys[2].toObject().contains("e"), "property: last key has no end");
  p    = animatedProperty(scalars({1, 1, 2}), 0, 0, 0, true, true);
  keys = p["k"].toArray();
  require(keys.size() == 2 && keys[0].toObject()["h"].toInt() == 1,
          "property: hold keys");

  // NaN and infinities never reach the JSON
  p = animatedProperty(scalars({c_nan, c_inf, 3}), 0, 0, 0, true);
  require(allFinite(p), "property: NaN rejected");

  // Layer transforms
  std::vector<Affine> frames;
  for (int f = 0; f <= 10; ++f) frames.push_back(translation(f * 2.0, 5));
  double shear    = 1;
  QJsonObject ks  = layerTransform(frames, 0, 100, KeyTolerance(), &shear);
  QJsonObject pos = ks["p"].toObject();
  require(pos["a"].toInt() == 1 && pos["k"].toArray().size() == 2,
          "transform: linear move in 2 keys");
  require(ks["r"].toObject()["a"].toInt() == 0, "transform: static rotation");
  require(ks["s"].toObject()["a"].toInt() == 0, "transform: static scale");
  require(near(ks["o"].toObject()["k"].toDouble(), 100), "transform: opacity");
  require(near(shear, 0), "transform: no shear");

  frames.clear();
  for (int f = 0; f <= 10; ++f) frames.push_back(flipY(rotation(f * 36.0)));
  ks           = layerTransform(frames, 0, 50);
  QJsonArray r = ks["r"].toObject()["k"].toArray();
  require(r.size() == 2, "transform: turn in 2 keys");
  require(near(r[1].toObject()["s"].toArray()[0].toDouble(), -360),
          "transform: full turn unwrapped");

  frames.assign(3, Affine(c_nan, 0, 0, 0, 1, 0));
  require(allFinite(layerTransform(frames, 0, c_nan)),
          "transform: NaN rejected");
}

//------------------------------------------------------------------

void testColors() {
  QJsonArray c = color(255, 128, 0);
  require(c.size() == 4, "color: 4 components");
  require(near(c[0].toDouble(), 1) && near(c[1].toDouble(), 0.502, 1e-4) &&
              near(c[2].toDouble(), 0) && near(c[3].toDouble(), 1),
          "color: 0..1 components");
  c = color(300, -5, 255);
  require(near(c[0].toDouble(), 1) && near(c[1].toDouble(), 0),
          "color: clamped");
  require(near(opacityPercent(255), 100), "opacity: opaque");
  require(near(opacityPercent(255, 0.5), 50), "opacity: factor");
  require(near(opacityPercent(0), 0), "opacity: transparent");
  require(near(opacityPercent(255, c_nan), 100), "opacity: NaN factor");

  QJsonObject fill = fillShape(color(0, 0, 0), 100, EvenOdd);
  require(fill["ty"].toString() == "fl" && fill["r"].toInt() == 2,
          "fill: even-odd");
  QJsonObject stroke =
      strokeShape(color(0, 0, 0), 100, c_nan, RoundCap, BevelJoin, c_inf);
  require(stroke["ty"].toString() == "st" && stroke["lc"].toInt() == 2 &&
              stroke["lj"].toInt() == 3,
          "stroke: cap and join");
  require(allFinite(stroke), "stroke: NaN rejected");

  QJsonArray items;
  items.append(fill);
  QJsonObject gr = group("Group", items);
  QJsonArray it  = gr["it"].toArray();
  require(gr["ty"].toString() == "gr" && it.size() == 2 &&
              it[1].toObject()["ty"].toString() == "tr",
          "group: transform last");
}

//------------------------------------------------------------------

void testValidation() {
  QJsonObject object;
  QJsonArray array;
  array.append(1.0);
  array.append(c_nan);
  object["a"] = array;
  object["b"] = c_inf;
  // Depending on the Qt version, a NaN may already be stored as null
  QJsonValue clean = sanitized(object);
  require(allFinite(clean), "sanitized: all finite");
  const QJsonArray a = clean.toObject().value("a").toArray();
  require(a.size() == 2, "sanitized: kept");
  require(near(a.at(0).toDouble(), 1), "sanitized: values kept");
}

}  // namespace

int main() {
  try {
    testNumbers();
    testTransforms();
    testBezier();
    testKeys();
    testColors();
    testValidation();
  } catch (const std::exception &e) {
    std::cerr << "lottiemath_test failed: " << e.what() << std::endl;
    return 1;
  }
  std::cout << "lottiemath_test passed" << std::endl;
  return 0;
}
