// Standalone test of the pure helpers of the Light Shading Fx. It needs no
// OpenToonz library; build and run it by hand from toonz/sources/stdfx:
//
//   g++ -std=c++17 -Wall -o /tmp/lightshading_test tests/lightshading_test.cpp
//   /tmp/lightshading_test
#include "../lightshading.h"

#include <cmath>
#include <cstdio>
#include <vector>

using namespace lightshading;

static int failures = 0;

static void check(bool condition, const char *message) {
  if (condition) return;
  std::printf("FAIL: %s\n", message);
  ++failures;
}

static bool isNear(double a, double b, double eps = 1e-6) {
  return std::fabs(a - b) <= eps;
}

int main() {
  // Radial intensity: 1 at the light, 0 at the radius and beyond
  check(isNear(radialIntensity(0.0, 100.0), 1.0), "radial: 1 at the light");
  check(isNear(radialIntensity(50.0, 100.0), 0.5), "radial: 0.5 halfway");
  check(isNear(radialIntensity(100.0, 100.0), 0.0), "radial: 0 at the radius");
  check(isNear(radialIntensity(250.0, 100.0), 0.0), "radial: 0 beyond");
  check(isNear(radialIntensity(10.0, 0.0), 0.0), "radial: no radius, no light");

  // Directional intensity: facing the light is lit, facing away is shadow
  check(isNear(directionalIntensity(1, 0, 1, 0), 1.0), "directional: facing");
  check(isNear(directionalIntensity(-1, 0, 1, 0), 0.0), "directional: away");
  check(isNear(directionalIntensity(0, 1, 1, 0), 0.5), "directional: sideways");
  check(isNear(directionalIntensity(0, 0, 1, 0), 0.5),
        "directional: no normal");
  check(isNear(directionalIntensity(1, 0, 0, 0), 0.5), "directional: no light");
  check(isNear(directionalIntensity(0.5, 0, 3, 0), 0.75),
        "directional: short normal, L not normalised");
  check(isNear(directionalIntensity(8, 0, 1, 0), 1.0),
        "directional: long normal");
  check(isNear(directionalIntensity(1, 1, 1, 0), 0.5 + 0.5 * std::sqrt(0.5)),
        "directional: 45 degrees");

  // Quantisation to bands: a neutral middle band and N steps on each side
  check(isNear(quantizeLevel(0.5, 1), 0.0), "1 band: middle is neutral");
  check(isNear(quantizeLevel(0.3, 1), 0.0), "1 band: 0.3 is neutral");
  check(isNear(quantizeLevel(0.2, 1), -1.0), "1 band: 0.2 is shadow");
  check(isNear(quantizeLevel(0.0, 1), -1.0), "1 band: 0 is shadow");
  check(isNear(quantizeLevel(0.8, 1), 1.0), "1 band: 0.8 is light");
  check(isNear(quantizeLevel(1.0, 1), 1.0), "1 band: 1 is light");
  check(isNear(quantizeLevel(0.5, 3), 0.0), "3 bands: middle is neutral");
  check(isNear(quantizeLevel(1.0, 3), 1.0), "3 bands: top is full light");
  check(isNear(quantizeLevel(0.0, 3), -1.0), "3 bands: bottom is full shadow");
  check(isNear(quantizeLevel(0.0, 2), -1.0), "2 bands: bottom is full shadow");
  check(isNear(quantizeLevel(0.2, 2), -0.5), "2 bands: 0.2 is half shadow");
  check(isNear(quantizeLevel(-3.0, 4), -1.0), "clamps below 0");
  check(isNear(quantizeLevel(7.0, 4), 1.0), "clamps above 1");
  check(isNear(quantizeLevel(0.5, 0), 0.0), "0 bands counts as 1");

  // The levels are flat: at most N distinct values per side
  for (int bands = 1; bands <= 6; ++bands) {
    std::vector<double> seen;
    double previous = -2.0;
    bool monotonic  = true;
    for (int i = 0; i <= 1000; ++i) {
      double level = quantizeLevel(i / 1000.0, bands);
      if (level < previous) monotonic = false;
      previous   = level;
      bool known = false;
      for (double v : seen) known = known || isNear(v, level);
      if (!known) seen.push_back(level);
      check(level >= -1.0 && level <= 1.0, "level out of range");
    }
    check(monotonic, "levels do not grow with the intensity");
    check((int)seen.size() == 2 * bands + 1, "2N + 1 distinct levels");
  }

  // Shading amount follows the side of the level
  check(isNear(shadingAmount(-0.5, 0.6, 0.2), 0.3), "amount: shadow side");
  check(isNear(shadingAmount(0.5, 0.6, 0.2), 0.1), "amount: light side");
  check(isNear(shadingAmount(0.0, 0.6, 0.2), 0.0), "amount: neutral");
  check(isNear(shadingAmount(-1.0, 3.0, 0.2), 1.0), "amount: clamped");

  // Box blur: keeps the sum away from the border, treats outside as 0
  {
    const int w = 9, h = 7;
    std::vector<float> buf(w * h, 0.0f), tmp;
    buf[3 * w + 4] = 9.0f;
    boxBlur(buf, w, h, 1, tmp);
    check(isNear(buf[3 * w + 4], 1.0, 1e-5), "blur: centre");
    check(isNear(buf[2 * w + 3], 1.0, 1e-5), "blur: corner of the window");
    check(isNear(buf[3 * w + 6], 0.0, 1e-5), "blur: outside the window");
    double sum = 0.0;
    for (float v : buf) sum += v;
    check(isNear(sum, 9.0, 1e-4), "blur: total is kept");

    std::vector<float> flat(w * h, 1.0f);
    boxBlur(flat, w, h, 2, tmp);
    check(isNear(flat[3 * w + 4], 1.0, 1e-5), "blur: flat interior stays flat");
    check(isNear(flat[0], 9.0 / 25.0, 1e-5), "blur: border sees 0 outside");

    std::vector<float> same(w * h, 0.5f);
    boxBlur(same, w, h, 0, tmp);
    check(isNear(same[10], 0.5), "blur: radius 0 is the identity");
  }

  // A rounded volume: the blurred edge of a block gives a normal that points
  // outwards, and so lights the edge that faces the light
  {
    const int w = 40, h = 20, r = 3;
    std::vector<float> alpha(w * h, 0.0f), tmp;
    for (int y = 5; y < 15; ++y)
      for (int x = 10; x < 30; ++x) alpha[y * w + x] = 1.0f;
    boxBlur(alpha, w, h, r, tmp);
    boxBlur(alpha, w, h, r, tmp);
    const double gain = 2.0 * r + 1.0;
    auto intensityAt  = [&](int x, int y, double lx, double ly) {
      double gx = (alpha[y * w + x + 1] - alpha[y * w + x - 1]) / 2.0;
      double gy = (alpha[(y + 1) * w + x] - alpha[(y - 1) * w + x]) / 2.0;
      return directionalIntensity(-gx * gain, -gy * gain, lx, ly);
    };
    // Light from the left: the left edge is lit, the right edge in shadow
    check(intensityAt(10, 10, -1, 0) > 0.9, "volume: left edge, light left");
    check(intensityAt(29, 10, -1, 0) < 0.1, "volume: right edge, light left");
    check(isNear(intensityAt(20, 10, -1, 0), 0.5, 1e-3),
          "volume: the inside is neutral");
    // Light from above (+y is up in the tiles)
    check(intensityAt(20, 14, 0, 1) > 0.9, "volume: top edge, light above");
    check(intensityAt(20, 5, 0, 1) < 0.1, "volume: bottom edge, light above");
  }

  if (failures) {
    std::printf("%d check(s) failed\n", failures);
    return 1;
  }
  std::printf("lightshading_test: all checks passed\n");
  return 0;
}
