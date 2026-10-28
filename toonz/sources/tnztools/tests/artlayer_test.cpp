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

// Minimal stand-in for TVectorImage: strokes in stacking order.
struct Image {
  std::vector<Stroke> strokes;

  unsigned getStrokeCount() const { return (unsigned)strokes.size(); }
  const Stroke *getStroke(unsigned n) const { return &strokes[n]; }
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
  // On a copy: centerline is used below as a Line Art stroke.
  Stroke centerlineDrawn = centerline;
  applyTarget(TARGET_COLOUR_ART, centerlineDrawn);
  require(roleOf(centerlineDrawn) == COLOUR_ART,
          "Centerline becomes Colour Art");

  // Underlay and Overlay: the place of Line Art among the Colour Art strokes.
  Stroke bottom = line, middle = line, top = line;
  Image image{{bottom, bottom, colour, middle, colour, top}};
  ColourRange range = colourRange(image);
  require(range.first == 2 && range.last == 4, "Colour Art range");
  require(!range.isEmpty(), "Colour Art range is not empty");
  require(roleInStack(range, 0, false) == UNDERLAY, "Below is Underlay");
  require(roleInStack(range, 1, false) == UNDERLAY, "Below is Underlay");
  require(roleInStack(range, 2, true) == COLOUR_ART, "Colour Art stays");
  require(roleInStack(range, 3, false) == LINE_ART, "Between is Line Art");
  require(roleInStack(range, 4, true) == COLOUR_ART, "Colour Art stays");
  require(roleInStack(range, 5, false) == OVERLAY, "Above is Overlay");

  int counts[RoleCount];
  countRoles(image, counts);
  require(counts[UNDERLAY] == 2 && counts[COLOUR_ART] == 2 &&
              counts[LINE_ART] == 1 && counts[OVERLAY] == 1,
          "Strokes per role");

  // Without Colour Art there are no Underlay and Overlay strokes.
  Image lines{{line, line, centerline}};
  ColourRange none = colourRange(lines);
  require(none.isEmpty(), "No Colour Art range");
  for (int i = 0; i < 3; ++i)
    require(roleInStack(none, i, false) == LINE_ART, "All Line Art");
  countRoles(lines, counts);
  require(counts[LINE_ART] == 3 && counts[UNDERLAY] == 0 &&
              counts[OVERLAY] == 0 && counts[COLOUR_ART] == 0,
          "Line Art only");
  Image empty;
  require(colourRange(empty).isEmpty(), "Empty image");

  // A block of Colour Art below the Line Art (Create Colour Art) or above it
  // (the Lasso Cutter's closing strokes) leaves it Line Art.
  Image below{{colour, colour, line, line}};
  Image above{{line, line, colour}};
  Image single{{colour, line, line}};
  for (const Image *image : {&below, &above, &single}) {
    ColourRange block = colourRange(*image);
    require(!block.isEmpty() && !block.isLayered(), "Colour Art block");
    countRoles(*image, counts);
    require(
        counts[UNDERLAY] == 0 && counts[OVERLAY] == 0 && counts[LINE_ART] == 2,
        "Colour Art block keeps Line Art");
    require(effectiveTarget(TARGET_OVERLAY, block) == TARGET_LINE_ART &&
                effectiveTarget(TARGET_UNDERLAY, block) == TARGET_LINE_ART,
            "Underlay and Overlay need Line Art between the Colour Art");
  }
  require(colourRange(image).isLayered(), "Line Art between the Colour Art");

  // The targets: Underlay and Overlay need layered Colour Art, else act as
  // Line Art.
  require(effectiveTarget(TARGET_UNDERLAY, none) == TARGET_LINE_ART,
          "Underlay without Colour Art");
  require(effectiveTarget(TARGET_OVERLAY, none) == TARGET_LINE_ART,
          "Overlay without Colour Art");
  require(effectiveTarget(TARGET_UNDERLAY, range) == TARGET_UNDERLAY,
          "Underlay with Colour Art");
  require(effectiveTarget(TARGET_OVERLAY, range) == TARGET_OVERLAY,
          "Overlay with Colour Art");
  require(effectiveTarget(TARGET_OFF, none) == TARGET_OFF, "Off is kept");
  require(effectiveTarget(TARGET_COLOUR_ART, none) == TARGET_COLOUR_ART,
          "Colour Art target is kept");

  require(isEditable(TARGET_UNDERLAY, UNDERLAY), "Underlay target");
  require(!isEditable(TARGET_UNDERLAY, OVERLAY), "Underlay target");
  require(!isEditable(TARGET_UNDERLAY, LINE_ART), "Underlay target");
  require(!isEditable(TARGET_UNDERLAY, COLOUR_ART), "Underlay target");
  require(isEditable(TARGET_OVERLAY, OVERLAY), "Overlay target");
  require(!isEditable(TARGET_OVERLAY, UNDERLAY), "Overlay target");
  require(!isEditable(TARGET_OVERLAY, COLOUR_ART), "Overlay target");

  // The Line Art target covers every stroke with thickness; Off is unchanged.
  require(isEditable(TARGET_LINE_ART, UNDERLAY), "Line Art edits Underlay");
  require(isEditable(TARGET_LINE_ART, OVERLAY), "Line Art edits Overlay");
  require(!isEditable(TARGET_COLOUR_ART, UNDERLAY), "Colour Art only");
  require(!isEditable(TARGET_COLOUR_ART, OVERLAY), "Colour Art only");
  for (Role r : {LINE_ART, COLOUR_ART, UNDERLAY, OVERLAY})
    require(isEditable(TARGET_OFF, r), "Off must edit every role");

  // Underlay and Overlay are Line Art for the display options.
  require(!isFaded(TARGET_LINE_ART, SHOW_LINE_ART, UNDERLAY), "Show Line");
  require(!isFaded(TARGET_LINE_ART, SHOW_LINE_ART, OVERLAY), "Show Line");
  require(isFaded(TARGET_COLOUR_ART, SHOW_COLOUR_ART, UNDERLAY), "Show Col");
  require(isFaded(TARGET_COLOUR_ART, SHOW_COLOUR_ART, OVERLAY), "Show Col");

  // Hidden role masks use one bit per role.
  int mask = roleBit(UNDERLAY) | roleBit(OVERLAY);
  require(mask == 12, "Role bits");
  require(roleBit(LINE_ART) == 1 && roleBit(COLOUR_ART) == 2, "Role bits");

  // New strokes of the Underlay and Overlay targets keep their thickness.
  Stroke underlayDrawn = line;
  applyTarget(TARGET_UNDERLAY, underlayDrawn);
  applyTarget(TARGET_OVERLAY, underlayDrawn);
  require(underlayDrawn.setCount == 0 && roleOf(underlayDrawn) == LINE_ART,
          "Underlay and Overlay must not change strokes");

  std::cout << "Art layer role checks passed\n";
}
