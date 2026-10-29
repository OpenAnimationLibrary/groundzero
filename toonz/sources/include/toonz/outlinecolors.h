#pragma once

#ifndef OUTLINECOLORS_INCLUDED
#define OUTLINECOLORS_INCLUDED

// The colours of the Outline View of the columns (see
// doc/animate_outline_view.md). Each column shown as outlines gets a colour
// from a fixed palette, chosen from its index: the colour is not saved. The
// palette is made of mid-tone colours that stay readable on the light and on
// the dark backgrounds of the viewer, and consecutive indices are far apart
// in hue so that neighbouring columns are told apart.
//
// No GUI or Toonz dependency, so that it can be tested alone.

namespace OutlineColors {

struct Rgb {
  unsigned char r, g, b;
};

constexpr int count = 8;  //!< Number of different colours

//! Position of the index in the palette; any int is accepted, including the
//! negative ones (the camera column) and the very large ones, which wrap.
inline int slot(int index) {
  int s = index % count;
  return s < 0 ? s + count : s;
}

//! The colour of the column with the given index.
inline Rgb colorForIndex(int index) {
  static const Rgb palette[count] = {
      {230, 57, 70},   // red
      {30, 110, 230},  // blue
      {30, 160, 80},   // green
      {235, 130, 20},  // orange
      {150, 70, 210},  // purple
      {0, 160, 170},   // teal
      {220, 60, 160},  // pink
      {150, 140, 0}    // olive
  };
  return palette[slot(index)];
}

}  // namespace OutlineColors

#endif  // OUTLINECOLORS_INCLUDED
