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
#include <limits>
#include <vector>

static int failures = 0;

static void check(bool condition, const char *message) {
  if (condition) return;
  std::printf("FAIL: %s\n", message);
  ++failures;
}

static bool near(double a, double b) { return std::fabs(a - b) < 1e-6; }

static void testOutlineMargin() {
  check(autopatch::outlineMargin(0.0) == 2, "margin: wrong zero-width support");
  check(autopatch::outlineMargin(1.0) == 3,
        "margin: wrong whole-pixel support");
  check(autopatch::outlineMargin(1.25) == 4,
        "margin: fractional support is not rounded up");

  const int limit      = (std::numeric_limits<int>::max)();
  const double largest = double(limit - 2);
  check(autopatch::outlineMargin(largest) == limit,
        "margin: the representable boundary is rejected");
  const double invalid[] = {
      -1.0, std::numeric_limits<double>::quiet_NaN(),
      std::numeric_limits<double>::infinity(),
      (std::numeric_limits<double>::max)(),
      std::nextafter(largest, std::numeric_limits<double>::infinity())};
  for (double radius : invalid)
    check(autopatch::outlineMargin(radius) == -1,
          "margin: an unrepresentable support is accepted");
}

static void testOutlineSize() {
  const int limit = (std::numeric_limits<int>::max)();
  for (int pixelBytes : {4, 8, 16}) {
    check(autopatch::canRenderOutline(320.0, 240.0, 12, pixelBytes),
          "size: ordinary outline support is rejected");
    const int width = limit / pixelBytes / 5 - 4;
    check(autopatch::canRenderOutline(width, 1.0, 2, pixelBytes),
          "size: the byte-count boundary is rejected");
    check(!autopatch::canRenderOutline(width + 1.0, 1.0, 2, pixelBytes),
          "size: byte-count overflow is accepted");
    check(!autopatch::canRenderOutline(width + 0.25, 1.0, 2, pixelBytes),
          "size: fractional dimensions bypass the byte-count boundary");
    const int side   = int(std::sqrt(double(limit / pixelBytes)));
    const int margin = (side - 1) / 2;
    check(autopatch::canRenderOutline(1.0, 1.0, margin, pixelBytes),
          "size: the largest square support is rejected");
    check(!autopatch::canRenderOutline(1.0, 1.0, margin + 1, pixelBytes),
          "size: an oversized square support is accepted");
  }
  check(!autopatch::canRenderOutline(1.0, 1.0, limit, 4),
        "size: doubled margin overflow is accepted");
  check(!autopatch::canRenderOutline(limit, 1.0, 2, 4),
        "size: padded dimension overflow is accepted");
}

static void testInvalidOutlineSize() {
  const double invalid[] = {0.0, -1.0, std::numeric_limits<double>::quiet_NaN(),
                            std::numeric_limits<double>::infinity(),
                            (std::numeric_limits<double>::max)()};
  for (double dimension : invalid) {
    check(!autopatch::canRenderOutline(dimension, 1.0, 2, 4),
          "size: invalid width is accepted");
    check(!autopatch::canRenderOutline(1.0, dimension, 2, 4),
          "size: invalid height is accepted");
  }
  check(!autopatch::canRenderOutline(1.0, 1.0, -1, 4),
        "size: negative margin is accepted");
  check(!autopatch::canRenderOutline(1.0, 1.0, 2, 0),
        "size: zero pixel size is accepted");
}

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
  testOutlineMargin();
  testOutlineSize();
  testInvalidOutlineSize();
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
