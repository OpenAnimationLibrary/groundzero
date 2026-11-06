// Standalone test of the pure helper of the Auto-Patch Fx's Outline Only
// option. It needs no OpenToonz library; build and run it by hand from
// toonz/sources/stdfx:
//
//   g++ -std=c++17 -Wall -o /tmp/apo_test tests/autopatchoutline_test.cpp
//   /tmp/apo_test
#include "../autopatchoutline.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <vector>

static int failures = 0;

static void check(bool condition, const char *message) {
  if (condition) return;
  std::printf("FAIL: %s\n", message);
  ++failures;
}

static bool near(double a, double b) { return std::fabs(a - b) < 1e-6; }

// The distances agree with a brute force search on random images
static void testDistances() {
  std::srand(7);
  for (int round = 0; round < 20; ++round) {
    const int w = 1 + std::rand() % 23, h = 1 + std::rand() % 17;
    std::vector<unsigned char> outside(w * h);
    for (unsigned char &o : outside) o = (std::rand() % 9) == 0;

    const std::vector<double> dist =
        autopatch::squaredDistances(outside, w, h);
    check((int)dist.size() == w * h, "distances: wrong size");
    for (int y = 0; y < h; ++y)
      for (int x = 0; x < w; ++x) {
        double best = -1.0;
        for (int j = 0; j < h; ++j)
          for (int i = 0; i < w; ++i)
            if (outside[j * w + i]) {
              const double d = double(i - x) * (i - x) + double(j - y) * (j - y);
              if (best < 0.0 || d < best) best = d;
            }
        const double got = dist[y * w + x];
        if (best < 0.0)
          check(got >= 1e19, "distances: a pixel is near a missing site");
        else
          check(near(got, best), "distances: differs from brute force");
      }
  }
}

// The weights fall from 1 to 0 around the radius
static void testWeights() {
  const int w = 11;
  std::vector<unsigned char> outside(w, 0);
  outside[0] = 1;
  const std::vector<float> weights =
      autopatch::outlineWeights(outside, w, 1, 3.0);
  check(near(weights[0], 1.0) && near(weights[2], 1.0),
        "weights: not 1 within the radius");
  check(near(weights[3], 0.5), "weights: the limit is not antialiased");
  check(near(weights[4], 0.0) && near(weights[10], 0.0),
        "weights: not 0 beyond the radius");

  // A diagonal neighbour is sqrt(2) away
  std::vector<unsigned char> dot(25, 0);
  dot[12] = 1;
  const std::vector<float> disc = autopatch::outlineWeights(dot, 5, 5, 1.0);
  check(near(disc[12], 1.0) && near(disc[13], 0.5),
        "weights: wrong next to the site");
  check(disc[18] > 0.0f && disc[18] < 0.5f,
        "weights: wrong on the diagonal");

  // No outside pixel, or no radius: nothing is on the outline
  const std::vector<float> none =
      autopatch::outlineWeights(std::vector<unsigned char>(25, 0), 5, 5, 3.0);
  bool allZero = true;
  for (float v : none) allZero = allZero && v == 0.0f;
  check(allZero, "weights: set without an outside pixel");
  const std::vector<float> zero = autopatch::outlineWeights(dot, 5, 5, 0.0);
  check(zero[12] == 0.0f, "weights: set with no radius");

  // Bad sizes give nothing
  check(autopatch::outlineWeights(dot, 4, 5, 1.0).empty(),
        "weights: a wrong size is accepted");
}

// A seam and a detail line: only the seam is near the outside of Top
static void testSeamAndDetail() {
  // Top fills x >= 4 of a 16 x 5 image; its outline is the column x = 4,
  // and a detail line runs at x = 11, inside Top
  const int w = 16, h = 5;
  std::vector<unsigned char> outside(w * h, 0);
  for (int y = 0; y < h; ++y)
    for (int x = 0; x < 4; ++x) outside[y * w + x] = 1;
  const std::vector<float> weights =
      autopatch::outlineWeights(outside, w, h, 2.0);
  for (int y = 0; y < h; ++y) {
    check(near(weights[y * w + 4], 1.0), "seam: the outline is not patched");
    check(near(weights[y * w + 11], 0.0), "seam: the detail line is patched");
  }
}

int main() {
  testDistances();
  testWeights();
  testSeamAndDetail();
  if (failures) {
    std::printf("autopatchoutline_test: %d failure(s)\n", failures);
    return 1;
  }
  std::printf("autopatchoutline_test passed\n");
  return 0;
}
