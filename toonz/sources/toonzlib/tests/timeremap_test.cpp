// Regression test of the pure helpers of the per-column time remap
// (toonz/timeremap.h): the value/row conversion, the loop, hold and reverse
// frame math, and the keys of the presets, evaluated through a TDoubleParam
// as the level column fx does.

#include "toonz/timeremap.h"

#include "tdoubleparam.h"
#include "tdoublekeyframe.h"

#include <iostream>
#include <stdexcept>
#include <vector>

namespace {

void require(bool condition, const char *message) {
  if (!condition) throw std::runtime_error(message);
}

//------------------------------------------------------------------

void testConversion() {
  for (int row = 0; row < 100; row++)
    require(TimeRemap::valueToRow(TimeRemap::rowToValue(row)) == row,
            "conversion: round trip");
  require(TimeRemap::valueToRow(1.0) == 0, "conversion: frame 1 is row 0");
  require(TimeRemap::valueToRow(2.4) == 1, "conversion: rounds down");
  require(TimeRemap::valueToRow(2.6) == 2, "conversion: rounds up");
}

//------------------------------------------------------------------

void testFrameMath() {
  // Loop of the rows 2..4 from the row 2 on
  const int loop[] = {0, 1, 2, 3, 4, 2, 3, 4, 2, 3, 4, 2};
  for (int row = 0; row < 12; row++)
    require(TimeRemap::loopRow(row, 2, 4) == loop[row], "loop: wrong row");
  require(TimeRemap::loopRow(7, 3, 3) == 3, "loop: a single row");
  require(TimeRemap::loopRow(7, 5, 3) == 5, "loop: inverted range");

  // Hold of the row 3 along the rows 3..6
  const int hold[] = {0, 1, 2, 3, 3, 3, 3, 7, 8};
  for (int row = 0; row < 9; row++)
    require(TimeRemap::holdRow(row, 3, 6, 3) == hold[row], "hold: wrong row");

  // Reverse of the rows 2..5
  const int rev[] = {0, 1, 5, 4, 3, 2, 6, 7};
  for (int row = 0; row < 8; row++)
    require(TimeRemap::reverseRow(row, 2, 5) == rev[row], "reverse: wrong row");
}

//------------------------------------------------------------------

// The row shown by a curve made of the keys, as TLevelColumnFx computes it
int curveRow(TDoubleParam &curve, int row) {
  int n = curve.getKeyframeCount();
  return TimeRemap::remapRow(row, n, n ? curve.keyframeIndexToFrame(0) : 0,
                             n ? curve.keyframeIndexToFrame(n - 1) : 0,
                             curve.isCycleEnabled(), curve.getValue(row));
}

void fillCurve(TDoubleParam &curve, const std::vector<TimeRemap::Key> &keys,
               bool cycled) {
  for (const TimeRemap::Key &key : keys) {
    TDoubleKeyframe k(key.first, key.second);
    k.m_type = TDoubleKeyframe::Linear;
    curve.setKeyframe(k);
  }
  curve.enableCycle(cycled);
}

void testUnused() {
  TDoubleParam curve(1.0);
  for (int row = 0; row < 20; row++)
    require(curveRow(curve, row) == row, "unused: not the identity");
}

void testLoopKeys() {
  const int ranges[][2] = {{0, 3}, {2, 4}, {5, 5}, {1, 8}, {0, 1}};
  for (const auto &range : ranges) {
    TDoubleParam curve(1.0);
    fillCurve(curve, TimeRemap::loopKeys(range[0], range[1]), true);
    for (int row = 0; row < 60; row++)
      require(
          curveRow(curve, row) == TimeRemap::loopRow(row, range[0], range[1]),
          "loop keys: differ from the loop math");
  }
}

void testHoldKeys() {
  const int holds[][3] = {{3, 6, 3}, {0, 4, 0}, {2, 2, 2}, {4, 9, 4}};
  for (const auto &hold : holds) {
    TDoubleParam curve(1.0);
    fillCurve(curve, TimeRemap::holdKeys(hold[0], hold[1], hold[2]), false);
    for (int row = 0; row < 40; row++)
      require(curveRow(curve, row) ==
                  TimeRemap::holdRow(row, hold[0], hold[1], hold[2]),
              "hold keys: differ from the hold math");
  }
}

void testReverseKeys() {
  const int ranges[][2] = {{0, 5}, {2, 5}, {3, 3}, {1, 12}};
  for (const auto &range : ranges) {
    TDoubleParam curve(1.0);
    fillCurve(curve, TimeRemap::reverseKeys(range[0], range[1]), false);
    for (int row = 0; row < 40; row++)
      require(curveRow(curve, row) ==
                  TimeRemap::reverseRow(row, range[0], range[1]),
              "reverse keys: differ from the reverse math");
  }
}

}  // namespace

int main() {
  try {
    testConversion();
    testFrameMath();
    testUnused();
    testLoopKeys();
    testHoldKeys();
    testReverseKeys();
  } catch (const std::exception &e) {
    std::cerr << "timeremap_test failed: " << e.what() << std::endl;
    return 1;
  }

  std::cout << "timeremap_test passed" << std::endl;
  return 0;
}
