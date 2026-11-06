// Standalone checks of the vector Brush's paint modes (Paint Fills, Paint
// Behind): where the new shape goes in the stacking order, and the names the
// mode is saved with. No GUI or application initialization.
// g++ -std=c++17 -I../../include brushpaintmode_test.cpp -o brushpaintmode_test
#include "tools/brushpaintmode.h"
#include <cstdlib>
#include <iostream>
#include <limits>
#include <string>
#include <vector>

using namespace BrushPaintMode;

namespace {

void check(bool condition, const char *what) {
  if (condition) return;
  std::cerr << "Check failed: " << what << '\n';
  std::exit(1);
}

const DrawOrder kOrders[] = {ORDER_OVER_ALL, ORDER_UNDER_ALL, ORDER_PALETTE};

void testEmptyDrawing() {
  const std::vector<double> none;
  check(firstLineArt(none) == 0, "empty: no Line Art means the end (0)");
  for (DrawOrder order : kOrders) {
    check(insertionIndex(none, MODE_PAINT_BEHIND, order) == 0,
          "empty: Paint Behind goes at 0");
    check(insertionIndex(none, MODE_PAINT_FILLS, order) == 0,
          "empty: Paint Fills goes at 0");
  }
  check(insertionIndex(none, MODE_NORMAL, ORDER_OVER_ALL) == 0,
        "empty: Normal Over All goes at 0");
  check(insertionIndex(none, MODE_NORMAL, ORDER_UNDER_ALL) == 0,
        "empty: Normal Under All goes at 0");
  check(insertionIndex(none, MODE_NORMAL, ORDER_PALETTE) == kByPalette,
        "empty: Normal Palette Order is left to the palette");
}

void testOnlyColourArt() {
  const std::vector<double> colour = {0.0, 0.0, 0.0, 0.0};
  check(firstLineArt(colour) == 4, "only Colour Art: no Line Art");
  for (DrawOrder order : kOrders) {
    check(insertionIndex(colour, MODE_PAINT_BEHIND, order) == 0,
          "only Colour Art: Paint Behind goes at the bottom");
    check(insertionIndex(colour, MODE_PAINT_FILLS, order) == 4,
          "only Colour Art: Paint Fills goes on top");
  }
  check(insertionIndex(colour, MODE_NORMAL) == 4,
        "only Colour Art: Normal defaults to Over All");
  check(insertionIndex(colour, MODE_NORMAL, ORDER_UNDER_ALL) == 0,
        "only Colour Art: Normal Under All goes at the bottom");
}

void testOnlyLineArt() {
  const std::vector<double> lines = {1.0, 2.5, 0.3};
  check(firstLineArt(lines) == 0, "only Line Art: the first stroke");
  for (DrawOrder order : kOrders) {
    check(insertionIndex(lines, MODE_PAINT_BEHIND, order) == 0,
          "only Line Art: Paint Behind goes at the bottom");
    check(insertionIndex(lines, MODE_PAINT_FILLS, order) == 0,
          "only Line Art: Paint Fills goes below every line");
  }
  check(insertionIndex(lines, MODE_NORMAL, ORDER_OVER_ALL) == 3,
        "only Line Art: Normal Over All goes on top");
  check(insertionIndex(lines, MODE_NORMAL, ORDER_UNDER_ALL) == 0,
        "only Line Art: Normal Under All goes at the bottom");
  check(insertionIndex(lines, MODE_NORMAL, ORDER_PALETTE) == kByPalette,
        "only Line Art: Normal Palette Order is left to the palette");
}

void testMixedOrders() {
  struct Case {
    std::vector<double> thicknesses;
    int fills;
    const char *what;
  };
  const double nan   = std::numeric_limits<double>::quiet_NaN();
  const Case cases[] = {
      {{0, 0, 2, 0, 1}, 2, "Colour Art below the lines stays below the shape"},
      {{2, 0, 1}, 0, "a line at the bottom puts the shape at the bottom"},
      {{0, 1, 0}, 1, "between the lowest Colour Art and the line"},
      {{0, 0, 0, 5}, 3, "below the single line on top"},
      {{0, 1e-9, 0}, 1, "any thickness above 0 is Line Art"},
      {{0, nan, 0, 3}, 3, "a NaN thickness is not Line Art"},
      {{0, -1, 0}, 3, "a negative thickness is not Line Art"},
  };
  for (const Case &c : cases) {
    const int count = int(c.thicknesses.size());
    check(firstLineArt(c.thicknesses) == c.fills, c.what);
    check(insertionIndex(c.thicknesses, MODE_PAINT_FILLS) == c.fills, c.what);
    for (DrawOrder order : kOrders) {
      check(insertionIndex(c.thicknesses, MODE_PAINT_FILLS, order) == c.fills,
            "the Draw Order does not change Paint Fills");
      check(insertionIndex(c.thicknesses, MODE_PAINT_BEHIND, order) == 0,
            "the Draw Order does not change Paint Behind");
    }
    check(insertionIndex(c.thicknesses, MODE_NORMAL, ORDER_OVER_ALL) == count,
          "Normal Over All goes on top");
    check(insertionIndex(c.thicknesses, MODE_NORMAL, ORDER_UNDER_ALL) == 0,
          "Normal Under All goes at the bottom");
    const int index = insertionIndex(c.thicknesses, MODE_PAINT_FILLS);
    check(index >= 0 && index <= count, "the index is a valid place");
    for (int i = 0; i < index; ++i)
      check(!isLineArt(c.thicknesses[i]), "no Line Art below the shape");
  }
}

void testMapping() {
  for (int i = 0; i < kModeCount; ++i) {
    const Mode mode = Mode(i);
    check(modeFromIndex(i) == mode, "an option index maps to its mode");
    check(fromString(toString(mode)) == mode, "the saved name round trips");
    const std::string name = toString(mode);
    check(!name.empty() && name.find(' ') == std::string::npos,
          "saved names are single words");
    for (int j = 0; j < i; ++j)
      check(name != toString(Mode(j)), "saved names are distinct");
  }
  check(std::string(toString(MODE_NORMAL)) == "Normal", "Normal is \"Normal\"");
  check(fromString("PaintFills") == MODE_PAINT_FILLS, "PaintFills");
  check(fromString("PaintBehind") == MODE_PAINT_BEHIND, "PaintBehind");
  check(fromString("") == MODE_NORMAL, "an empty name is Normal");
  check(fromString("PaintInside") == MODE_PAINT_INSIDE, "PaintInside persists");
  check(fromString("PaintSelection") == MODE_PAINT_SELECTION, "PaintSelection persists");
  check(fromString("UnknownMode") == MODE_NORMAL, "an unknown name is Normal");
  check(makesShape(MODE_PAINT_INSIDE) && makesShape(MODE_PAINT_SELECTION), "constrained modes create shapes");
  check(fromString("paintfills") == MODE_NORMAL, "names are case sensitive");
  check(fromString("Paint Fills") == MODE_NORMAL,
        "the menu text is not a saved name");
  check(modeFromIndex(-1) == MODE_NORMAL, "index -1 is Normal");
  check(modeFromIndex(kModeCount) == MODE_NORMAL, "index past the end");
  check(modeFromIndex(1000) == MODE_NORMAL, "a large index is Normal");

  check(!makesShape(MODE_NORMAL), "Normal keeps the stroke");
  check(makesShape(MODE_PAINT_FILLS), "Paint Fills makes a shape");
  check(makesShape(MODE_PAINT_BEHIND), "Paint Behind makes a shape");
}

}  // namespace

int main() {
  testEmptyDrawing();
  testOnlyColourArt();
  testOnlyLineArt();
  testMixedOrders();
  testMapping();
  std::cout << "Brush paint mode checks passed\n";
  return 0;
}
