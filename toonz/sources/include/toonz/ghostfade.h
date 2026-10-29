#pragma once

#ifndef GHOST_FADE_INCLUDED
#define GHOST_FADE_INCLUDED

#include <algorithm>

// The Ghost Fade of the onion skin (doc/ghost_fade.md): viewer settings that
// change how the onion skin ghosts are drawn, on top of the paper thickness
// preference and the opacity of each marker. Header only and without toonz
// types, so that it can be tested on its own.
namespace GhostFade {

struct Settings {
  // Multiplies the opacity of every ghost, in [0, maxOpacity]: 1 draws them
  // as the paper thickness and the markers say, 0 hides them.
  double opacity = 1.0;
  // Whether the ghosts take the front and back onion skin colours.
  bool tint = true;
  // Only the relative ghosts this many frames apart from the current frame
  // are drawn: 1 draws them all.
  int step = 1;

  bool isDefault() const { return opacity == 1.0 && tint && step == 1; }
};

const double maxOpacity = 2.0;
const int maxStep       = 24;

// The settings brought back into their ranges (as read from the user's
// settings file, which may have been edited by hand).
inline Settings sanitized(Settings settings) {
  if (!(settings.opacity >= 0.0))  // also catches NaN
    settings.opacity = 0.0;
  settings.opacity = std::min(settings.opacity, maxOpacity);
  settings.step    = std::max(1, std::min(settings.step, maxStep));
  return settings;
}

// The opacity of a ghost, in [0, 1], from the opacity the onion skin gives it
// (paper thickness or marker, in [0, 1]) and the opacity multiplier. With the
// multiplier at 1 it is the opacity unchanged.
inline double ghostOpacity(double opacity, double multiplier) {
  return std::max(0.0, std::min(1.0, opacity * multiplier));
}

// Whether the relative ghost drow frames away from the current frame is drawn.
// Fixed markers are always drawn: they were placed one by one.
inline bool showsGhost(int drow, bool isFixed, int step) {
  if (isFixed || step <= 1) return true;
  return drow % step == 0;
}

}  // namespace GhostFade

#endif  // GHOST_FADE_INCLUDED
