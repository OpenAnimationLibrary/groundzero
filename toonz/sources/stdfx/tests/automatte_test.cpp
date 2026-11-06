// Standalone test of the pure helper of the Auto Matte Fx. It needs no
// OpenToonz library; build and run it by hand from toonz/sources/stdfx:
//
//   g++ -std=c++17 -Wall -o /tmp/automatte_test tests/automatte_test.cpp
//   /tmp/automatte_test
#include "../automatte.h"

#include <cstdio>
#include <vector>

static int failures = 0;

static void check(bool condition, const char *message) {
  if (condition) return;
  std::printf("FAIL: %s\n", message);
  ++failures;
}

// A 9 x 9 image with a square outline from (2, 2) to (6, 6)
static std::vector<float> outline(bool gap) {
  const int w = 9;
  std::vector<float> alpha(w * w, 0.0f);
  for (int i = 2; i <= 6; ++i) {
    alpha[2 * w + i] = alpha[6 * w + i] = 1.0f;
    alpha[i * w + 2] = alpha[i * w + 6] = 1.0f;
  }
  if (gap) alpha[2 * w + 4] = 0.0f;  // a hole in the top edge
  alpha[0] = 0.25f;                  // an antialiased pixel outside
  return alpha;
}

int main() {
  const int w = 9;
  const std::vector<float> closed = automatte::silhouette(outline(false), w, w, 0.5);
  check(closed[4 * w + 4] == 1.0f, "the enclosed centre is filled");
  check(closed[3 * w + 3] == 1.0f, "the enclosed area next to the line is filled");
  check(closed[2 * w + 2] == 1.0f, "the line stays opaque");
  check(closed[1 * w + 4] == 0.0f, "outside the outline stays transparent");
  check(closed[0] == 0.25f, "an outside pixel keeps its own alpha");

  const std::vector<float> open = automatte::silhouette(outline(true), w, w, 0.5);
  check(open[4 * w + 4] == 0.0f, "a gap lets the outside in");

  std::vector<float> faint(w * w, 0.4f);
  const std::vector<float> none = automatte::silhouette(faint, w, w, 0.5);
  check(none[4 * w + 4] == 0.4f, "below the threshold nothing is a line");

  int cancel = 1;
  check(automatte::silhouette(outline(false), w, w, 0.5, &cancel).size() ==
            std::size_t(w * w),
        "a small image finishes before the first cancel check");

  if (failures == 0) std::printf("automatte: PASS\n");
  return failures == 0 ? 0 : 1;
}
