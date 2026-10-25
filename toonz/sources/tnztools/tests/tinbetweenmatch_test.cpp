// Standalone checks of the pure helpers behind the smarter morphing options
// (automatic stroke matching, zone hints, custom ease); no tvrender needed.
// g++ -std=c++17 -I../../include tinbetweenmatch_test.cpp -o
// /tmp/tinbetweenmatch_test
#include "tinbetweenmatch.h"
#include <cmath>
#include <cstdlib>
#include <iostream>

using namespace TInbetweenMatch;

void check(bool condition, const char *what) {
  if (condition) return;
  std::cerr << "Check failed: " << what << '\n';
  std::exit(1);
}

StrokeInfo stroke(double cx, double cy, double w, double h, double length,
                  int style) {
  StrokeInfo info;
  info.m_cx      = cx;
  info.m_cy      = cy;
  info.m_width   = w;
  info.m_height  = h;
  info.m_length  = length;
  info.m_styleId = style;
  return info;
}

double totalCost(const std::vector<double> &cost, int cols,
                 const std::vector<int> &assignment) {
  double sum = 0;
  for (size_t r = 0; r < assignment.size(); r++)
    if (assignment[r] >= 0) sum += cost[r * cols + assignment[r]];
  return sum;
}

int main() {
  // --- matchCost
  StrokeInfo a = stroke(0, 0, 10, 10, 40, 1);
  check(matchCost(a, a, 100) == 0, "identical strokes cost nothing");
  StrokeInfo moved     = stroke(10, 0, 10, 10, 40, 1);
  StrokeInfo moreMoved = stroke(30, 0, 10, 10, 40, 1);
  check(matchCost(a, moved, 100) < matchCost(a, moreMoved, 100),
        "the cost grows with the centroid distance");
  // normalised by the diagonal: same offset, twice the diagonal, half the cost
  check(std::fabs(matchCost(a, moved, 100) - 2 * matchCost(a, moved, 200)) <
            1e-12,
        "the centroid distance is relative to the diagonal");
  StrokeInfo otherStyle = stroke(0, 0, 10, 10, 40, 2);
  check(matchCost(a, otherStyle, 100) > matchCost(a, a, 100),
        "different styles cost more");
  StrokeInfo longer = stroke(0, 0, 10, 10, 80, 1);
  check(matchCost(a, longer, 100) > 0, "length difference costs");
  StrokeInfo bigger = stroke(0, 0, 30, 10, 40, 1);
  check(matchCost(a, bigger, 100) > 0, "size difference costs");
  check(std::isfinite(matchCost(StrokeInfo(), StrokeInfo(), 0)),
        "degenerate input stays finite");

  // --- assignMinCost: the anti-diagonal is optimal here (3 + 4 + 3)
  {
    std::vector<double> cost = {1, 2, 3,  //
                                2, 4, 6,  //
                                3, 6, 9};
    std::vector<int> r       = assignMinCost(cost, 3, 3);
    check(totalCost(cost, 3, r) == 10, "3x3 optimum");
  }
  {
    // greedy would take 0 for row 0 and be forced into 10 for row 1
    std::vector<double> cost = {0, 1,  //
                                1, 10};
    std::vector<int> r       = assignMinCost(cost, 2, 2);
    check(r[0] == 1 && r[1] == 0, "optimal, not greedy");
  }
  {
    // more rows than columns: one row is left out, the cheapest are kept
    std::vector<double> cost = {5, 1,  //
                                1, 5,  //
                                9, 9};
    std::vector<int> r       = assignMinCost(cost, 3, 2);
    check(r.size() == 3 && r[0] == 1 && r[1] == 0 && r[2] == -1,
          "more rows than columns");
  }
  {
    // more columns than rows
    std::vector<double> cost = {5, 9, 1,  //
                                9, 2, 7};
    std::vector<int> r       = assignMinCost(cost, 2, 3);
    check(r[0] == 2 && r[1] == 1, "more columns than rows");
  }
  check(assignMinCost({}, 0, 0).empty(), "empty problem");
  {
    std::vector<int> r = assignMinCost({}, 2, 0);
    check(r.size() == 2 && r[0] == -1 && r[1] == -1, "no columns");
  }
  {
    // a permutation is always returned: no column is used twice
    std::vector<double> cost(5 * 5);
    for (int i = 0; i < 25; i++) cost[i] = (i * 7) % 11;
    std::vector<int> r = assignMinCost(cost, 5, 5);
    std::vector<bool> seen(5, false);
    for (int c : r) {
      check(c >= 0 && c < 5 && !seen[c], "valid permutation");
      seen[c] = true;
    }
  }

  // --- matching of strokes given in a shuffled order
  {
    std::vector<StrokeInfo> first  = {stroke(0, 0, 10, 10, 40, 1),
                                      stroke(100, 0, 10, 10, 40, 1),
                                      stroke(200, 0, 10, 10, 40, 1)};
    std::vector<StrokeInfo> second = {stroke(205, 5, 10, 10, 40, 1),
                                      stroke(3, 2, 10, 10, 40, 1),
                                      stroke(104, 1, 10, 10, 40, 1)};
    std::vector<double> cost(9);
    for (int i = 0; i < 3; i++)
      for (int j = 0; j < 3; j++)
        cost[i * 3 + j] = matchCost(first[i], second[j], 300);
    std::vector<int> r = assignMinCost(cost, 3, 3);
    check(r[0] == 1 && r[1] == 2 && r[2] == 0, "shuffled strokes are found");
  }

  // --- zoneMatch
  {
    std::vector<Point> a = {{1, 1}, {5, 5}, {50, 50}};
    std::vector<Point> b = {{101, 101}, {104, 104}, {150, 150}};
    Rect zoneA = {0, 0, 10, 10}, zoneB = {100, 100, 110, 110};
    std::vector<bool> usedA(3, false), usedB(3, false);
    auto pairs = zoneMatch(a, b, zoneA, zoneB, usedA, usedB);
    check(pairs.size() == 2, "two strokes in each zone");
    check(pairs[0] == std::make_pair(0, 0) && pairs[1] == std::make_pair(1, 1),
          "nearest first");
    check(
        usedA[0] && usedA[1] && !usedA[2] && usedB[0] && usedB[1] && !usedB[2],
        "used flags");
    // used strokes are skipped by the following hints
    auto again = zoneMatch(a, b, zoneA, zoneB, usedA, usedB);
    check(again.empty(), "already matched strokes are not reused");
  }
  {
    // more strokes in the first zone: the ones left have no partner
    std::vector<Point> a = {{1, 1}, {9, 9}};
    std::vector<Point> b = {{108, 109}};
    Rect zoneA = {0, 0, 10, 10}, zoneB = {100, 100, 110, 110};
    std::vector<bool> usedA(2, false), usedB(1, false);
    auto pairs = zoneMatch(a, b, zoneA, zoneB, usedA, usedB);
    check(pairs.size() == 1 && pairs[0].first == 1, "closest one is chosen");
    check(!usedA[0], "the other one stays free");
  }

  // --- customEase
  check(customEase(0, 0.3, 0.7) == 0 && customEase(1, 0.3, 0.7) == 1,
        "endpoints");
  for (double t = 0; t <= 1.0; t += 0.05)
    check(std::fabs(customEase(t, 0, 0) - t) < 1e-9, "0/0 is linear");
  {
    const double strengths[] = {0, 0.25, 0.5, 1};
    for (double s : strengths)
      for (double e : strengths) {
        double previous = 0;
        for (int i = 1; i <= 100; i++) {
          double v = customEase(i / 100.0, s, e);
          check(v >= previous - 1e-12 && v <= 1.0 + 1e-12, "monotonic");
          previous = v;
        }
      }
  }
  check(customEase(0.1, 1, 0) < 0.1, "slow start is behind linear early on");
  check(customEase(0.9, 0, 1) > 0.9, "slow end is ahead of linear late");
  check(std::fabs(customEase(0.5, 0.6, 0.6) - 0.5) < 1e-9,
        "symmetric strengths pass through the center");
  check(std::fabs(customEase(0.2, 0.8, 0.4) + customEase(0.8, 0.4, 0.8) - 1) <
            1e-9,
        "mirrored curve");
  check(customEase(-1, 1, 1) == 0 && customEase(2, 1, 1) == 1, "t is clamped");
  check(std::fabs(customEase(0.5, 5, -3) - customEase(0.5, 1, 0)) < 1e-12,
        "strengths are clamped");

  std::cout << "TInbetween matching checks passed\n";
}
