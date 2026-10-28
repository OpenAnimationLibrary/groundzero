#pragma once

#ifndef PEGBAR_GUIDE_INCLUDED
#define PEGBAR_GUIDE_INCLUDED

#include "tgeometry.h"
#include "toonz/cleanupparameters.h"

#include <cctype>
#include <cmath>
#include <string>
#include <vector>

// The registration holes of a field guide file (stuff/config/fdg/*.fdg, as
// read for the clean-up autocenter) laid out around the field centre, for the
// Pegbar Guide of the viewer. Everything is in mm, x to the right, y up.
namespace PegbarGuide {

enum Side { Top, Bottom, Left, Right };

struct Hole {
  TPointD center;
  double width, height;
  bool round;  // the centre hole; the others are slots
};

// The holes of info with the pegbar on the given side of the field centre.
// The .fdg files give the holes in the scanner frame, in a line; the centre
// hole is the middle one, and the line is turned to run along the side.
inline std::vector<Hole> holes(const CleanupTypes::FDG_INFO &info, Side side) {
  std::vector<Hole> result;
  const std::vector<CleanupTypes::DOT> &dots = info.dots;
  if (dots.empty() || !(info.dist_ctr_to_ctr_hole > 0.0)) return result;

  const CleanupTypes::DOT &first = dots.front(), &last = dots.back();
  const CleanupTypes::DOT &center = dots[dots.size() / 2];
  TPointD u(last.x - first.x, last.y - first.y);
  const double length = std::sqrt(u.x * u.x + u.y * u.y);
  u                   = length > 0.0 ? u * (1.0 / length) : TPointD(0, 1);

  const double d = info.dist_ctr_to_ctr_hole;
  for (const CleanupTypes::DOT &dot : dots) {
    const double along = (dot.x - center.x) * u.x + (dot.y - center.y) * u.y;
    const double sizeAlong  = std::abs(dot.lx * u.x) + std::abs(dot.ly * u.y);
    const double sizeAcross = std::abs(dot.lx * u.y) + std::abs(dot.ly * u.x);
    Hole hole;
    hole.round = std::abs(dot.lx - dot.ly) <= 1;
    switch (side) {
    case Top:
      hole.center = TPointD(along, d);
      break;
    case Bottom:
      hole.center = TPointD(along, -d);
      break;
    case Left:
      hole.center = TPointD(-d, -along);
      break;
    case Right:
      hole.center = TPointD(d, along);
      break;
    }
    const bool horizontal = side == Top || side == Bottom;
    hole.width            = horizontal ? sizeAlong : sizeAcross;
    hole.height           = horizontal ? sizeAcross : sizeAlong;
    result.push_back(hole);
  }
  return result;
}

// Distance in mm from the field centre to the paper edge behind the pegs.
inline double paperEdge(const CleanupTypes::FDG_INFO &info) {
  return info.dist_ctr_to_ctr_hole + info.dist_ctr_hole_to_edge;
}

// The field size in the name of a field guide file, "fld_12_acme" -> 12;
// 0 when the name has none.
inline int fieldFromName(const std::string &name) {
  if (name.compare(0, 4, "fld_") != 0) return 0;
  int field                = 0;
  std::string::size_type i = 4;
  for (; i < name.size() && std::isdigit((unsigned char)name[i]); i++)
    field = field * 10 + (name[i] - '0');
  return i > 4 && field > 0 && field < 100 ? field : 0;
}

}  // namespace PegbarGuide

#endif
