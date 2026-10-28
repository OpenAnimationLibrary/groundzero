#pragma once

#ifndef COLUMNCOLOREFFECT_H
#define COLUMNCOLOREFFECT_H

#include <algorithm>
#include <cmath>
#include <cstring>

//=============================================================================
// Pure helpers of the per-column Color Effect (doc/animate_color_effect.md),
// modelled on the Color Effect of Adobe Animate: Brightness, Tint, Alpha and
// Advanced (a multiply and an offset per channel).
//
// The values are animatable parameters of the level column fx
// (TLevelColumnFx), named below. At a frame they make one Transform: a
// multiply m and an offset c per channel, applied as
//
//     out = clamp(m * in + c, 0, 255)
//
// on STRAIGHT (not premultiplied) channels in the 0..255 range, in the order
// red, green, blue, alpha. The offsets are in 0..255 units, so a Transform is
// exactly what TGenericColorFunction(m, c) takes for vector drawings. For
// premultiplied pixels (the raster viewer and the render) use
// applyPremultiplied(), which unpremultiplies, applies and premultiplies
// again.
//
// The effects are composed in this order, each on the result of the previous
// one, into a single Transform clamped once at the end:
//   1. Advanced:   m = mul / 100, c = add, per channel (alpha included);
//   2. Tint:       rgb: m = 1 - amount, c = tint * amount (alpha unchanged);
//   3. Brightness: b > 0 blends towards white (m = 1 - b, c = 255 * b),
//                  b < 0 scales towards black (m = 1 + b), alpha unchanged;
//   4. Alpha:      the alpha multiply and offset are scaled by alpha / 100, so
//                  Alpha 0 is fully transparent whatever comes before.
// (amount, b and alpha in 0..1 here, from the percentages of the parameters.)
//
// Header only and independent of the rest of OpenToonz, so that it can be
// unit tested alone (toonzlib/tests/columncoloreffect_test.cpp).
//=============================================================================

namespace ColumnColorEffect {

//! The animatable numeric parameters (the tint color is a separate color
//! parameter, see tintColorParamName()).
enum Param {
  Brightness = 0,  //!< -100..100 (percent), 0 = none
  TintAmount,      //!< 0..100 (percent), 0 = none
  Alpha,           //!< 0..100 (percent), 100 = none
  RedMul,          //!< Advanced multipliers, -100..100 (percent), 100 = none
  GreenMul,
  BlueMul,
  AlphaMul,
  RedAdd,  //!< Advanced offsets, -255..255, 0 = none
  GreenAdd,
  BlueAdd,
  AlphaAdd,
  ParamCount
};

//! The name of the parameter in the level column fx (and in the scene file).
inline const char *paramName(int p) {
  static const char *const names[ParamCount] = {
      "brightness", "tintAmount", "alpha",    "redMul",  "greenMul", "blueMul",
      "alphaMul",   "redAdd",     "greenAdd", "blueAdd", "alphaAdd"};
  return (p >= 0 && p < ParamCount) ? names[p] : "";
}

//! The label of the parameter in the Function Editor.
inline const char *paramLabel(int p) {
  static const char *const labels[ParamCount] = {
      "Brightness",       "Tint Amount",      "Alpha",
      "Red Multiplier",   "Green Multiplier", "Blue Multiplier",
      "Alpha Multiplier", "Red Offset",       "Green Offset",
      "Blue Offset",      "Alpha Offset"};
  return (p >= 0 && p < ParamCount) ? labels[p] : "";
}

//! The index of the parameter called \b name, or -1.
inline int paramIndex(const char *name) {
  for (int p = 0; p < ParamCount; ++p)
    if (std::strcmp(name, paramName(p)) == 0) return p;
  return -1;
}

//! The name of the tint color parameter (a TPixelParam, opaque black by
//! default; its alpha is not used).
inline const char *tintColorParamName() { return "tintColor"; }

//! The value of the parameter that has no effect.
inline double defaultValue(int p) {
  switch (p) {
  case Alpha:
  case RedMul:
  case GreenMul:
  case BlueMul:
  case AlphaMul:
    return 100.0;
  default:
    return 0.0;
  }
}

//! The range of the parameter; the values outside it are clamped.
inline void valueRange(int p, double &minValue, double &maxValue) {
  switch (p) {
  case Brightness:
    minValue = -100.0, maxValue = 100.0;
    break;
  case TintAmount:
  case Alpha:
    minValue = 0.0, maxValue = 100.0;
    break;
  case RedMul:
  case GreenMul:
  case BlueMul:
  case AlphaMul:
    minValue = -100.0, maxValue = 100.0;
    break;
  default:
    minValue = -255.0, maxValue = 255.0;
    break;
  }
}

inline double clampValue(int p, double value) {
  double minValue = 0.0, maxValue = 0.0;
  valueRange(p, minValue, maxValue);
  if (!std::isfinite(value)) return defaultValue(p);
  return std::min(std::max(value, minValue), maxValue);
}

//=============================================================================

//! The values of the parameters at a frame.
struct Values {
  double m_value[ParamCount];
  double m_tint[3];  //!< The tint color, straight r, g, b in 0..255

  Values() {
    for (int p = 0; p < ParamCount; ++p) m_value[p] = defaultValue(p);
    m_tint[0] = m_tint[1] = m_tint[2] = 0.0;
  }

  double get(int p) const { return m_value[p]; }
  void set(int p, double value) { m_value[p] = value; }
  void setTint(double r, double g, double b) {
    m_tint[0] = r, m_tint[1] = g, m_tint[2] = b;
  }
};

//=============================================================================

//! A multiply and an offset per channel (r, g, b, alpha) of straight 0..255
//! channels: out = clamp(m * in + c, 0, 255). The default is the identity.
struct Transform {
  double m[4];
  double c[4];

  Transform() {
    for (int i = 0; i < 4; ++i) m[i] = 1.0, c[i] = 0.0;
  }

  bool isIdentity() const {
    for (int i = 0; i < 4; ++i)
      if (std::abs(m[i] - 1.0) > 1e-9 || std::abs(c[i]) > 1e-9) return false;
    return true;
  }

  //! The transform that applies this one, then \b next (without clamping in
  //! between).
  Transform then(const Transform &next) const {
    Transform t;
    for (int i = 0; i < 4; ++i) {
      t.m[i] = next.m[i] * m[i];
      t.c[i] = next.m[i] * c[i] + next.c[i];
    }
    return t;
  }

  //! Applies the transform to straight channels in 0..255, in place.
  void applyStraight(double rgba[4]) const {
    for (int i = 0; i < 4; ++i)
      rgba[i] = std::min(std::max(m[i] * rgba[i] + c[i], 0.0), 255.0);
  }

  //! Applies the transform to premultiplied channels in 0..maxValue (255 for
  //! 8-bit pixels, 65535 for 16-bit ones), in place. A fully transparent pixel
  //! stays fully transparent: an alpha offset changes the drawn pixels only,
  //! as it does on vector drawings, where only the drawn colors change.
  void applyPremultiplied(double rgba[4], double maxValue) const {
    const double alpha = rgba[3];
    if (!(alpha > 0.0) || !(maxValue > 0.0)) {
      rgba[0] = rgba[1] = rgba[2] = rgba[3] = 0.0;
      return;
    }
    // Straight channels in 0..255
    double straight[4];
    for (int i = 0; i < 3; ++i)
      straight[i] = std::min(std::max(rgba[i] / alpha, 0.0), 1.0) * 255.0;
    straight[3] = std::min(alpha / maxValue, 1.0) * 255.0;

    applyStraight(straight);

    // Back to premultiplied channels in 0..maxValue
    const double outAlpha = straight[3] / 255.0;
    for (int i = 0; i < 3; ++i)
      rgba[i] = straight[i] / 255.0 * outAlpha * maxValue;
    rgba[3] = outAlpha * maxValue;
  }
};

//=============================================================================
// The effects, each as a Transform

//! Advanced: the multipliers (percent) and the offsets (0..255 units), as
//! given (clamped to their ranges).
inline Transform advancedTransform(const Values &v) {
  Transform t;
  for (int i = 0; i < 4; ++i) {
    t.m[i] = clampValue(RedMul + i, v.get(RedMul + i)) / 100.0;
    t.c[i] = clampValue(RedAdd + i, v.get(RedAdd + i));
  }
  return t;
}

//! Tint: blends the colors towards the tint color by the amount; the alpha is
//! not changed.
inline Transform tintTransform(const Values &v) {
  Transform t;
  const double amount = clampValue(TintAmount, v.get(TintAmount)) / 100.0;
  for (int i = 0; i < 3; ++i) {
    const double tint = std::min(std::max(v.m_tint[i], 0.0), 255.0);
    t.m[i]            = 1.0 - amount;
    t.c[i]            = tint * amount;
  }
  return t;
}

//! Brightness: towards white when positive, towards black when negative; the
//! alpha is not changed.
inline Transform brightnessTransform(const Values &v) {
  Transform t;
  const double b = clampValue(Brightness, v.get(Brightness)) / 100.0;
  for (int i = 0; i < 3; ++i) {
    if (b > 0.0)
      t.m[i] = 1.0 - b, t.c[i] = 255.0 * b;
    else
      t.m[i] = 1.0 + b, t.c[i] = 0.0;
  }
  return t;
}

//! Alpha: scales the alpha.
inline Transform alphaTransform(const Values &v) {
  Transform t;
  t.m[3] = clampValue(Alpha, v.get(Alpha)) / 100.0;
  return t;
}

//! The whole Color Effect: Advanced, then Tint, then Brightness, then Alpha.
inline Transform compose(const Values &v) {
  return advancedTransform(v)
      .then(tintTransform(v))
      .then(brightnessTransform(v))
      .then(alphaTransform(v));
}

}  // namespace ColumnColorEffect

#endif  // COLUMNCOLOREFFECT_H
