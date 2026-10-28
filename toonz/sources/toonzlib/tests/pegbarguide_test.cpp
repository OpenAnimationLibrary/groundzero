// Regression test of the Pegbar Guide layout (toonz/pegbarguide.h): the holes
// of the 12 field Acme field guide file (stuff/config/fdg/fld_12_acme.fdg)
// turned to each side of the field, and the field size read from a file name.

#include "toonz/pegbarguide.h"

#include <cmath>
#include <iostream>
#include <stdexcept>

namespace {

void require(bool condition, const char *message) {
  if (!condition) throw std::runtime_error(message);
}

bool near(double a, double b) { return std::abs(a - b) < 1e-4; }

CleanupTypes::DOT dot(float x, float y, int lx, int ly) {
  CleanupTypes::DOT d = {};
  d.x                 = x;
  d.y                 = y;
  d.lx                = lx;
  d.ly                = ly;
  return d;
}

// The values of fld_12_acme.fdg: in the scanner frame the holes run down a
// vertical line, the side slots 3 x 16 mm, the centre hole 6 mm round.
CleanupTypes::FDG_INFO acme12() {
  CleanupTypes::FDG_INFO info;
  info.m_name = "fld_12_acme";
  info.dots   = {dot(254.0f, 63.7f, 3, 16), dot(254.0f, 165.1f, 6, 6),
                 dot(254.0f, 266.5f, 3, 16)};
  info.dist_ctr_to_ctr_hole  = 133.2;
  info.dist_ctr_hole_to_edge = 12.7;
  return info;
}

//------------------------------------------------------------------

void testTop() {
  const std::vector<PegbarGuide::Hole> holes =
      PegbarGuide::holes(acme12(), PegbarGuide::Top);
  require(holes.size() == 3, "top: three holes");
  // The centre hole above the field centre, round
  require(near(holes[1].center.x, 0) && near(holes[1].center.y, 133.2),
          "top: centre hole position");
  require(holes[1].round, "top: centre hole round");
  require(near(holes[1].width, 6) && near(holes[1].height, 6),
          "top: centre hole size");
  // The slots about 4 inches either side, lying along the pegbar
  require(near(holes[0].center.x, -101.4) && near(holes[2].center.x, 101.4),
          "top: slots 101.4 mm from the centre hole");
  require(near(holes[0].center.y, 133.2), "top: slots on the pegbar line");
  require(!holes[0].round, "top: slot not round");
  require(near(holes[0].width, 16) && near(holes[0].height, 3),
          "top: slot along the pegbar");
}

void testSides() {
  const std::vector<PegbarGuide::Hole> bottom =
      PegbarGuide::holes(acme12(), PegbarGuide::Bottom);
  require(near(bottom[1].center.y, -133.2), "bottom: below the centre");
  require(near(bottom[0].width, 16), "bottom: slot along the pegbar");

  const std::vector<PegbarGuide::Hole> left =
      PegbarGuide::holes(acme12(), PegbarGuide::Left);
  require(near(left[1].center.x, -133.2) && near(left[1].center.y, 0),
          "left: centre hole position");
  require(near(left[0].width, 3) && near(left[0].height, 16),
          "left: slot along the pegbar");
  require(near(std::abs(left[0].center.y), 101.4), "left: slot distance");

  const std::vector<PegbarGuide::Hole> right =
      PegbarGuide::holes(acme12(), PegbarGuide::Right);
  require(near(right[1].center.x, 133.2), "right: centre hole position");
}

void testEmpty() {
  CleanupTypes::FDG_INFO none;
  require(PegbarGuide::holes(none, PegbarGuide::Top).empty(), "no holes");
  CleanupTypes::FDG_INFO noDistance = acme12();
  noDistance.dist_ctr_to_ctr_hole   = 0;
  require(PegbarGuide::holes(noDistance, PegbarGuide::Top).empty(),
          "no distance");
}

void testPaperEdgeAndField() {
  require(near(PegbarGuide::paperEdge(acme12()), 145.9), "paper edge");
  require(PegbarGuide::fieldFromName("fld_12_acme") == 12, "field 12");
  require(PegbarGuide::fieldFromName("fld_16_oxbry") == 16, "field 16");
  require(PegbarGuide::fieldFromName("fld_10_4mm_acme") == 10, "field 10");
  require(PegbarGuide::fieldFromName("jpn_cel_tv") == 0, "no field");
  require(PegbarGuide::fieldFromName("fld_") == 0, "no number");
  require(PegbarGuide::fieldFromName("fld_x") == 0, "not a number");
}

}  // namespace

int main() {
  try {
    testTop();
    testSides();
    testEmpty();
    testPaperEdgeAndField();
  } catch (const std::exception &e) {
    std::cerr << "pegbarguide_test failed: " << e.what() << std::endl;
    return 1;
  }
  std::cout << "pegbarguide_test passed" << std::endl;
  return 0;
}
