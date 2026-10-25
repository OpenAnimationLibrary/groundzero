// Standalone checks of the Spine export math; no GUI or application
// initialization.
// g++ -std=c++17 -I.. spineexport_test.cpp -o /tmp/spineexport_test
#include "spineexport.h"
#include <cmath>
#include <cstdlib>
#include <iostream>

using namespace SpineExport;

namespace {

void expect(double actual, double expected, const char *what) {
  if (std::abs(actual - expected) < 1e-9) return;
  std::cerr << what << ": expected " << expected << ", got " << actual << '\n';
  std::exit(1);
}

// Bone local matrix as composed by Spine runtimes (Bone::updateWorldTransform)
void compose(const BoneTransform &t, double m[6]) {
  const double toRadians = 3.14159265358979323846 / 180.0;
  double rotationY       = (t.m_rotation + 90.0 + t.m_shearY) * toRadians;
  m[0]                   = std::cos(t.m_rotation * toRadians) * t.m_scaleX;
  m[1]                   = std::cos(rotationY) * t.m_scaleY;
  m[2]                   = t.m_x;
  m[3]                   = std::sin(t.m_rotation * toRadians) * t.m_scaleX;
  m[4]                   = std::sin(rotationY) * t.m_scaleY;
  m[5]                   = t.m_y;
}

BoneTransform roundTrip(const double m[6],
                        const BoneTransform *previous = nullptr) {
  BoneTransform t =
      decomposeAffine(m[0], m[1], m[2], m[3], m[4], m[5], previous);
  double r[6];
  compose(t, r);
  for (int i = 0; i < 6; ++i) expect(r[i], m[i], "round trip");
  return t;
}

}  // namespace

int main() {
  expect(normalizeDegrees(540.0), 180.0, "normalize");
  expect(normalizeDegrees(-180.0), 180.0, "normalize");
  expect(unwrapDegrees(-170.0, 170.0), 190.0, "unwrap");

  // Identity and translation
  double identity[6] = {1, 0, 12.5, 0, 1, -3};
  BoneTransform t    = roundTrip(identity);
  expect(t.m_rotation, 0, "identity rotation");
  expect(t.m_scaleX, 1, "identity scaleX");
  expect(t.m_scaleY, 1, "identity scaleY");
  expect(t.m_shearY, 0, "identity shear");
  expect(t.m_x, 12.5, "identity x");
  expect(t.m_y, -3, "identity y");

  // Rotation, scale and shear
  BoneTransform source;
  source.m_rotation = 30, source.m_scaleX = 2, source.m_scaleY = 0.5,
  source.m_shearY = 15;
  double m[6];
  compose(source, m);
  t = roundTrip(m);
  expect(t.m_rotation, 30, "rotation");
  expect(t.m_scaleX, 2, "scaleX");
  expect(t.m_scaleY, 0.5, "scaleY");
  expect(t.m_shearY, 15, "shear");

  // A horizontal flip keeps rotation 0 with a negative scaleX
  double flip[6] = {-1, 0, 0, 0, 1, 0};
  t              = roundTrip(flip);
  expect(t.m_rotation, 0, "flip rotation");
  expect(t.m_scaleX, -1, "flip scaleX");
  expect(t.m_scaleY, 1, "flip scaleY");

  // Turning by 180 degrees keeps positive scales on the first frame
  double turn[6] = {-1, 0, 0, 0, -1, 0};
  t              = roundTrip(turn);
  expect(t.m_scaleX, 1, "turn scaleX");
  expect(t.m_rotation, 180, "turn rotation");

  // Squashing scaleX through 0 to -1 stays continuous (no 180 degrees jump)
  BoneTransform previous = roundTrip(identity);
  for (double sx = 0.75; sx >= -1.0; sx -= 0.25) {
    if (std::abs(sx) < 1e-12) continue;
    double squash[6] = {sx, 0, 0, 0, 1, 0};
    previous         = roundTrip(squash, &previous);
    expect(previous.m_rotation, 0, "squash rotation");
    expect(previous.m_scaleX, sx, "squash scaleX");
  }

  // A continuous spin is unwrapped past 180 degrees
  previous = roundTrip(identity);
  for (int angle = 20; angle <= 720; angle += 20) {
    BoneTransform spin;
    spin.m_rotation = angle;
    compose(spin, m);
    previous = roundTrip(m, &previous);
    expect(previous.m_rotation, angle, "spin rotation");
    expect(previous.m_scaleX, 1, "spin scaleX");
  }

  std::cout << "spineexport_test passed\n";
  return 0;
}
