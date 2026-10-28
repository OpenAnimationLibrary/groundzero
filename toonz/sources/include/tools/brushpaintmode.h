#pragma once

#ifndef BRUSHPAINTMODE_INCLUDED
#define BRUSHPAINTMODE_INCLUDED

// Paint modes of the vector Brush tool (its "Paint:" option), like the brush
// modes of other animation programs:
//
//   - Normal: the stroke is added as before, where the Draw Order option says.
//   - Paint Fills: the stroke is replaced by the filled outline shape it draws
//     (a closed stroke of thickness 0, that is Colour Art), placed just below
//     the lowest Line Art stroke, so that the existing lines stay on top.
//   - Paint Behind: the same shape, placed at the bottom of the drawing.
//
// Pure logic with no GUI, Qt or Toonz dependency, so that it can be unit
// tested on its own (tnztools/tests/brushpaintmode_test.cpp): the place the
// new stroke or shape gets in the stacking order, and the names the mode is
// saved with in the user settings and in Brush presets.

#include <cstddef>
#include <string>
#include <vector>

namespace BrushPaintMode {

//! Values of the option, in the order of its menu.
enum Mode { MODE_NORMAL = 0, MODE_PAINT_FILLS, MODE_PAINT_BEHIND };
const int kModeCount = 3;

//! Values of the Brush's Draw Order option, which only the Normal mode uses.
enum DrawOrder { ORDER_OVER_ALL = 0, ORDER_UNDER_ALL, ORDER_PALETTE };

//! Returned by insertionIndex() when the place depends on the palette (Normal
//! mode with Palette Order): the tool then places the stroke itself, below
//! the strokes whose style comes before its own in the palette.
const int kByPalette = -1;

//! The mode of an option index. Anything unknown is MODE_NORMAL.
inline Mode modeFromIndex(int index) {
  return (index > MODE_NORMAL && index < kModeCount) ? Mode(index)
                                                     : MODE_NORMAL;
}

//! The name the mode is saved with (never translated, no spaces).
inline const char *toString(Mode mode) {
  switch (mode) {
  case MODE_PAINT_FILLS:
    return "PaintFills";
  case MODE_PAINT_BEHIND:
    return "PaintBehind";
  default:
    return "Normal";
  }
}

//! The mode of a saved name. An unknown or empty name (for instance one saved
//! by a newer version) is MODE_NORMAL. Names are case sensitive.
inline Mode fromString(const std::string &name) {
  for (int i = 0; i < kModeCount; ++i)
    if (name == toString(Mode(i))) return Mode(i);
  return MODE_NORMAL;
}

//! Whether the mode replaces the stroke with its filled outline shape.
inline bool makesShape(Mode mode) {
  return mode == MODE_PAINT_FILLS || mode == MODE_PAINT_BEHIND;
}

//! A stroke with some thickness is Line Art; a stroke whose control points
//! all have thickness 0 is Colour Art (a fill boundary). The thickness given
//! here is the largest one of the stroke (the tool passes the largest
//! absolute value of its control points).
inline bool isLineArt(double thickness) { return thickness > 0.0; }

//! Index of the lowest Line Art stroke, or the stroke count if there is none.
inline int firstLineArt(const std::vector<double> &thicknesses) {
  for (std::size_t i = 0; i < thicknesses.size(); ++i)
    if (isLineArt(thicknesses[i])) return int(i);
  return int(thicknesses.size());
}

//! Where a new stroke goes in a drawing whose strokes, from the bottom of the
//! stacking order up, have the given thicknesses: the index it has once it is
//! inserted, from 0 (the bottom) to the stroke count (the top).
//!  - MODE_PAINT_BEHIND: 0, below everything;
//!  - MODE_PAINT_FILLS: just below the lowest Line Art stroke, that is the
//!    index of the first stroke with a thickness above 0, or the top when the
//!    drawing has no Line Art (only Colour Art, or nothing);
//!  - MODE_NORMAL: as the Draw Order says: the top for Over All, the bottom
//!    for Under All, and kByPalette for Palette Order.
inline int insertionIndex(const std::vector<double> &thicknesses, Mode mode,
                          DrawOrder drawOrder = ORDER_OVER_ALL) {
  switch (mode) {
  case MODE_PAINT_BEHIND:
    return 0;
  case MODE_PAINT_FILLS:
    return firstLineArt(thicknesses);
  default:
    break;
  }
  switch (drawOrder) {
  case ORDER_UNDER_ALL:
    return 0;
  case ORDER_PALETTE:
    return kByPalette;
  default:
    return int(thicknesses.size());
  }
}

}  // namespace BrushPaintMode

#endif  // BRUSHPAINTMODE_INCLUDED
