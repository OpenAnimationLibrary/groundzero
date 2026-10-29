#pragma once

#ifndef PALETTETRANSFORM_H
#define PALETTETRANSFORM_H

#include <algorithm>
#include <cmath>
#include <string>

//=============================================================================
// Pure helpers of the Colour Script FX (doc/colour_script.md): a hue,
// saturation and lightness shift of the colours of a scene.
//
//   hue        degrees added to the hue (wrapping around)
//   saturation -1..1: -1 is grey, 0 unchanged, 1 fully saturated; in between
//              the saturation moves that fraction of the way
//   lightness  -1..1: -1 is black, 0 unchanged, 1 white, the same way
//
// On straight colours in 0..1. Styles whose name starts with a chosen prefix
// are kept as they are (isKept()), so that characters can keep their colours
// while the set changes.
//
// Header only and independent of the rest of OpenToonz, so that it can be
// unit tested alone (toonzlib/tests/colourscript_test.cpp).
//=============================================================================

namespace PaletteTransform {

struct Shift {
  double hue        = 0.0;
  double saturation = 0.0;
  double lightness  = 0.0;

  bool isIdentity() const {
    return std::abs(std::remainder(hue, 360.0)) < 1e-9 &&
           std::abs(saturation) < 1e-9 && std::abs(lightness) < 1e-9;
  }
  bool operator==(const Shift &s) const {
    return hue == s.hue && saturation == s.saturation &&
           lightness == s.lightness;
  }
  bool operator!=(const Shift &s) const { return !(*this == s); }

  //! Finite values in range
  void clamp() {
    auto bounded = [](double v, double lo, double hi) {
      return std::isfinite(v) ? std::min(std::max(v, lo), hi) : 0.0;
    };
    hue        = std::isfinite(hue) ? std::remainder(hue, 360.0) : 0.0;
    saturation = bounded(saturation, -1.0, 1.0);
    lightness  = bounded(lightness, -1.0, 1.0);
  }
};

inline void rgbToHsl(const double rgb[3], double &h, double &s, double &l) {
  const double mx = std::max(rgb[0], std::max(rgb[1], rgb[2]));
  const double mn = std::min(rgb[0], std::min(rgb[1], rgb[2]));
  l               = 0.5 * (mx + mn);
  const double d  = mx - mn;
  if (d <= 1e-12) {
    h = s = 0.0;
    return;
  }
  s = l > 0.5 ? d / (2.0 - mx - mn) : d / (mx + mn);
  if (mx == rgb[0])
    h = (rgb[1] - rgb[2]) / d + (rgb[1] < rgb[2] ? 6.0 : 0.0);
  else if (mx == rgb[1])
    h = (rgb[2] - rgb[0]) / d + 2.0;
  else
    h = (rgb[0] - rgb[1]) / d + 4.0;
  h *= 60.0;
}

namespace detail {
inline double hueToChannel(double p, double q, double t) {
  if (t < 0.0) t += 1.0;
  if (t > 1.0) t -= 1.0;
  if (t < 1.0 / 6.0) return p + (q - p) * 6.0 * t;
  if (t < 0.5) return q;
  if (t < 2.0 / 3.0) return p + (q - p) * (2.0 / 3.0 - t) * 6.0;
  return p;
}
}  // namespace detail

inline void hslToRgb(double h, double s, double l, double rgb[3]) {
  if (s <= 1e-12) {
    rgb[0] = rgb[1] = rgb[2] = l;
    return;
  }
  const double q = l < 0.5 ? l * (1.0 + s) : l + s - l * s;
  const double p = 2.0 * l - q;
  const double t = h / 360.0;
  rgb[0]         = detail::hueToChannel(p, q, t + 1.0 / 3.0);
  rgb[1]         = detail::hueToChannel(p, q, t);
  rgb[2]         = detail::hueToChannel(p, q, t - 1.0 / 3.0);
}

//! Moves v (in 0..1) toward 0 or 1 by the fraction amount (-1..1)
inline double moveToward(double v, double amount) {
  return amount < 0.0 ? v * (1.0 + amount) : v + (1.0 - v) * amount;
}

//! Applies the shift to straight colours in 0..1, in place.
inline void applyStraight(const Shift &shift, double rgb[3]) {
  if (shift.isIdentity()) return;
  double c[3];
  for (int i = 0; i < 3; ++i) c[i] = std::min(std::max(rgb[i], 0.0), 1.0);
  double h, s, l;
  rgbToHsl(c, h, s, l);
  h = std::fmod(h + shift.hue, 360.0);
  if (h < 0.0) h += 360.0;
  s = moveToward(s, shift.saturation);
  l = moveToward(l, shift.lightness);
  hslToRgb(h, s, l, rgb);
}

//! Applies the shift to premultiplied colours in 0..1 with alpha a.
inline void applyPremultiplied(const Shift &shift, double rgb[3], double a) {
  if (shift.isIdentity() || !(a > 0.0)) return;
  for (int i = 0; i < 3; ++i) rgb[i] /= a;
  applyStraight(shift, rgb);
  for (int i = 0; i < 3; ++i) rgb[i] *= a;
}

//! Applies the shift to an 8-bit straight colour.
inline void apply8(const Shift &shift, int &r, int &g, int &b) {
  if (shift.isIdentity()) return;
  double rgb[3] = {r / 255.0, g / 255.0, b / 255.0};
  applyStraight(shift, rgb);
  auto to8 = [](double v) {
    return (int)std::lround(std::min(std::max(v, 0.0), 1.0) * 255.0);
  };
  r = to8(rgb[0]), g = to8(rgb[1]), b = to8(rgb[2]);
}

//! Whether a style keeps its colours: its name starts with the prefix (an
//! empty prefix keeps none).
inline bool isKept(const std::wstring &styleName, const std::wstring &prefix) {
  return !prefix.empty() && styleName.compare(0, prefix.size(), prefix) == 0;
}

}  // namespace PaletteTransform

#endif  // PALETTETRANSFORM_H
