#pragma once

#include <algorithm>
#include <array>
#include <cmath>

struct TPaletteKeyInterpolation {
  enum class Timing { Linear, Hold, EaseIn, EaseOut, EaseInOut };
  enum class Space { PaletteRgb, LinearLight };

  Timing timing = Timing::Linear;
  Space space   = Space::PaletteRgb;

  bool operator==(const TPaletteKeyInterpolation &other) const {
    return timing == other.timing && space == other.space;
  }
  bool operator!=(const TPaletteKeyInterpolation &other) const {
    return !(*this == other);
  }

  static Timing timingFromValue(int value) {
    return value >= int(Timing::Linear) && value <= int(Timing::EaseInOut)
               ? Timing(value)
               : Timing::Linear;
  }
  static Space spaceFromValue(int value) {
    return value == int(Space::LinearLight) ? Space::LinearLight
                                            : Space::PaletteRgb;
  }

  double weight(double t) const {
    if (t <= 0.0) return 0.0;
    if (t >= 1.0) return 1.0;
    switch (timing) {
    case Timing::Hold:
      return 0.0;
    case Timing::EaseIn:
      return t * t;
    case Timing::EaseOut:
      return t * (2.0 - t);
    case Timing::EaseInOut:
      return t * t * (3.0 - 2.0 * t);
    case Timing::Linear:
      return t;
    }
    return t;
  }
};

namespace PaletteColorInterpolation {
using Rgba = std::array<int, 4>;

inline double decodeSrgb(double value) {
  return value <= 0.04045 ? value / 12.92
                          : std::pow((value + 0.055) / 1.055, 2.4);
}

inline double encodeSrgb(double value) {
  return value <= 0.0031308 ? 12.92 * value
                            : 1.055 * std::pow(value, 1.0 / 2.4) - 0.055;
}

inline Rgba blendLinearLight(const Rgba &a, const Rgba &b, double weight) {
  if (weight <= 0.0) return a;
  if (weight >= 1.0) return b;
  const double aWeight = (1.0 - weight) * a[3];
  const double bWeight = weight * b[3];
  const double alpha   = aWeight + bWeight;
  if (alpha <= 0.0) return {};
  Rgba result{0, 0, 0, int(std::lround(alpha))};
  for (int channel = 0; channel < 3; ++channel) {
    const double linear = (aWeight * decodeSrgb(a[channel] / 255.0) +
                           bWeight * decodeSrgb(b[channel] / 255.0)) /
                          alpha;
    result[channel] =
        int(std::lround(255.0 * encodeSrgb(std::clamp(linear, 0.0, 1.0))));
  }
  return result;
}
}  // namespace PaletteColorInterpolation
