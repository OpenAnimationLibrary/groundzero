#pragma once

#ifndef MULTIPLANEDEPTH_H
#define MULTIPLANEDEPTH_H

#include <algorithm>
#include <cmath>

//=============================================================================
// Pure helpers of the scene's Multiplane Depth (doc/multiplane_depth.md):
// depth of field and atmospheric perspective from the Z of the columns.
//
// The camera's Focus Z channel gives the plane in focus, in the units of the
// Z channel; its Aperture channel, in stops, scales the blur (each stop
// doubles it). For a column at a Z:
//
//   blur  = min(maxBlur, blurPerUnit * |z - focus|) * 2^aperture
//   behind = max(0, focus - z)            (the columns farther than the focus)
//   haze  = 1 - exp(-hazeDensity * behind)
//   desaturation = min(1, desaturationPerUnit * behind)
//
// The blur is in the units of the Blur FX's Value, as seen by the camera; the
// render divides it by the column's scale on screen, so a column pushed back
// (and so drawn smaller) is not blurred less. Desaturation moves the colours
// toward their luma, then the haze mixes them toward the haze colour; the
// alpha is unchanged.
//
// Header only and independent of the rest of OpenToonz, so that it can be
// unit tested alone (toonzlib/tests/multiplanedepth_test.cpp).
//=============================================================================

namespace MultiplaneDepth {

struct Settings {
  bool enabled               = false;
  double blurPerUnit         = 0.02;  //!< Blur Value per unit of Z
  double maxBlur             = 30.0;  //!< Blur Value
  double hazeDensity         = 0.0;   //!< per unit of Z behind the focus
  double desaturationPerUnit = 0.0;   //!< per unit of Z behind the focus
  double hazeColor[3]        = {0.80, 0.86, 0.94};  //!< 0..1, straight

  bool operator==(const Settings &s) const {
    return enabled == s.enabled && blurPerUnit == s.blurPerUnit &&
           maxBlur == s.maxBlur && hazeDensity == s.hazeDensity &&
           desaturationPerUnit == s.desaturationPerUnit &&
           hazeColor[0] == s.hazeColor[0] && hazeColor[1] == s.hazeColor[1] &&
           hazeColor[2] == s.hazeColor[2];
  }
  bool operator!=(const Settings &s) const { return !(*this == s); }

  //! Valid values: non-negative and finite (the colour in 0..1)
  void clamp() {
    auto unit = [](double v, double hi) {
      return std::isfinite(v) ? std::min(std::max(v, 0.0), hi) : 0.0;
    };
    blurPerUnit         = unit(blurPerUnit, 1000.0);
    maxBlur             = unit(maxBlur, 1000.0);
    hazeDensity         = unit(hazeDensity, 1000.0);
    desaturationPerUnit = unit(desaturationPerUnit, 1000.0);
    for (double &c : hazeColor) c = unit(c, 1.0);
  }
};

//! What the depth does to one column.
struct Column {
  double blur         = 0.0;  //!< Blur Value, as seen by the camera
  double haze         = 0.0;  //!< 0..1
  double desaturation = 0.0;  //!< 0..1

  bool hasBlur() const { return blur > 1e-6; }
  bool hasColor() const { return haze > 1e-6 || desaturation > 1e-6; }
};

inline Column evaluate(const Settings &s, double z, double focus,
                       double apertureStops) {
  Column c;
  if (!s.enabled || !std::isfinite(z) || !std::isfinite(focus)) return c;
  const double stops    = std::isfinite(apertureStops)
                              ? std::min(std::max(apertureStops, -20.0), 10.0)
                              : 0.0;
  const double distance = std::abs(z - focus);
  c.blur = std::min(s.maxBlur, s.blurPerUnit * distance) * std::pow(2.0, stops);
  const double behind = std::max(0.0, focus - z);
  c.haze              = 1.0 - std::exp(-s.hazeDensity * behind);
  c.desaturation      = std::min(1.0, s.desaturationPerUnit * behind);
  return c;
}

//! The blur in the column's own units, from the one seen by the camera and
//! the scale of the column on screen.
inline double localBlur(double blur, double screenScale) {
  if (!(screenScale > 1e-9) || !std::isfinite(screenScale)) return 0.0;
  return blur / screenScale;
}

//! Applies desaturation then haze to premultiplied colours in 0..1 (alpha
//! a), in place.
inline void applyPremultiplied(const Column &c, const double haze[3],
                               double rgb[3], double a) {
  const double luma = 0.299 * rgb[0] + 0.587 * rgb[1] + 0.114 * rgb[2];
  for (int i = 0; i < 3; ++i) {
    double v = rgb[i] + c.desaturation * (luma - rgb[i]);
    v        = v + c.haze * (haze[i] * a - v);
    rgb[i]   = v;
  }
}

}  // namespace MultiplaneDepth

#endif  // MULTIPLANEDEPTH_H
