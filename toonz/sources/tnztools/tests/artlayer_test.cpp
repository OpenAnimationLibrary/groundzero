// Standalone Line Art / Colour Art role checks; no GUI or tool framework.
// g++ -std=c++17 -DLINUX -I../../include artlayer_test.cpp -o
// /tmp/artlayer_test
#include "tools/artlayer.h"
#include <cstdlib>
#include <iostream>
#include <vector>

using namespace ArtLayer;

namespace {
void require(bool condition, const char *message) {
  if (!condition) {
    std::cerr << message << std::endl;
    std::exit(1);
  }
}

// Minimal stand-in for TStroke: only the control point interface is used.
struct Point {
  double x, y, thick;
};

struct Stroke {
  std::vector<Point> points;
  int setCount = 0;

  int getControlPointCount() const { return (int)points.size(); }
  Point getControlPoint(int n) const { return points[n]; }
  void setControlPoint(int n, const Point &p) {
    points[n] = p;
    ++setCount;
  }
};
}  // namespace

int main() {
  Stroke line{{{0, 0, 1.5}, {1, 1, 2}, {2, 0, 0}}};
  Stroke colour{{{0, 0, 0}, {1, 1, 0}, {2, 0, 0}}};
  Stroke centerline{{{0, 0, -1}, {1, 1, -1}, {2, 0, -1}}};

  // The role is derived from thickness, as the renderer does.
  require(roleOf(line) == LINE_ART, "Thick stroke must be Line Art");
  require(roleOf(colour) == COLOUR_ART, "0 thickness must be Colour Art");
  require(roleOf(centerline) == LINE_ART,
          "Negative thickness is not a hidden stroke");

  // Off (the default) edits everything, exactly as before.
  require(isEditable(TARGET_OFF, LINE_ART), "Off must edit Line Art");
  require(isEditable(TARGET_OFF, COLOUR_ART), "Off must edit Colour Art");
  require(isEditable(TARGET_LINE_ART, LINE_ART), "Line Art target");
  require(!isEditable(TARGET_LINE_ART, COLOUR_ART), "Line Art target");
  require(isEditable(TARGET_COLOUR_ART, COLOUR_ART), "Colour Art target");
  require(!isEditable(TARGET_COLOUR_ART, LINE_ART), "Colour Art target");

  // Display options never fade anything while the feature is off.
  for (ShowMode d : {SHOW_BOTH, SHOW_LINE_ART, SHOW_COLOUR_ART}) {
    require(!isFaded(TARGET_OFF, d, LINE_ART), "Off must not fade");
    require(!isFaded(TARGET_OFF, d, COLOUR_ART), "Off must not fade");
  }
  require(!isFaded(TARGET_LINE_ART, SHOW_BOTH, COLOUR_ART), "Show Both");
  require(isFaded(TARGET_LINE_ART, SHOW_LINE_ART, COLOUR_ART), "Show Line");
  require(!isFaded(TARGET_LINE_ART, SHOW_LINE_ART, LINE_ART), "Show Line");
  require(isFaded(TARGET_COLOUR_ART, SHOW_COLOUR_ART, LINE_ART), "Show Col");
  require(!isFaded(TARGET_COLOUR_ART, SHOW_COLOUR_ART, COLOUR_ART),
          "Show Colour");

  // Only the Colour Art target changes new strokes.
  Stroke drawn = line;
  applyTarget(TARGET_OFF, drawn);
  applyTarget(TARGET_LINE_ART, drawn);
  require(drawn.setCount == 0 && roleOf(drawn) == LINE_ART,
          "Off and Line Art must not change strokes");
  applyTarget(TARGET_COLOUR_ART, drawn);
  require(roleOf(drawn) == COLOUR_ART, "Colour Art must zero the thickness");
  require(drawn.setCount == 2, "Points already at 0 are left untouched");
  require(drawn.points[1].x == 1 && drawn.points[1].y == 1,
          "Geometry must be preserved");
  applyTarget(TARGET_COLOUR_ART, centerline);
  require(roleOf(centerline) == COLOUR_ART, "Centerline becomes Colour Art");

  std::cout << "Art layer role checks passed\n";
}
