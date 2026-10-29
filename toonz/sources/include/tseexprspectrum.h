#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <limits>

namespace TSeExprSpectrum {
// Native representation of the upstream spectrum editor's twelve values.
// It describes a spectrum, not a fluid simulation or a rendering service.
struct Parameters {
  int resolution = 10;
  double tileSize = 100.0;
  double lengthCutoff = 0.01;
  double amplitude = 1.0;
  double windAngle = 0.0;
  double windSpeed = 10.0;
  double directionalFactorExponent = 2.0;
  double directionalReflectionDamping = 0.0;
  std::array<double, 3> flowDirection{{0.0, 0.0, 0.0}};
  double sharpen = 0.0;
  double time = 0.0;
  double filterWidth = 1.0;
};

inline bool valid(const Parameters &p) {
  return p.resolution >= 1 && p.resolution <= 20 &&
         std::isfinite(p.tileSize) && p.tileSize > 0 &&
         std::isfinite(p.windSpeed) && p.windSpeed > 0 &&
         std::isfinite(p.lengthCutoff) && p.lengthCutoff >= 0 &&
         std::isfinite(p.amplitude) && p.amplitude >= 0 &&
         std::isfinite(p.directionalFactorExponent) &&
         p.directionalFactorExponent > -3.99 &&
         p.directionalFactorExponent <= 32 &&
         std::isfinite(p.windAngle) &&
         std::isfinite(p.directionalReflectionDamping) &&
         std::isfinite(p.flowDirection[0]) &&
         std::isfinite(p.flowDirection[1]) &&
         std::isfinite(p.flowDirection[2]) && std::isfinite(p.sharpen) &&
         std::isfinite(p.time) && std::isfinite(p.filterWidth);
}

inline std::size_t sampleCount() {
  // Matches the upstream logarithmic 0.01-step spectrum, including the last
  // sample reaching the upper wavenumber bound.
  return static_cast<std::size_t>(std::ceil((std::log(3000000.0) + 5.0) / 0.01)) + 1;
}

inline double value(const Parameters &p, double position) {
  if (!valid(p) || !std::isfinite(position))
    return std::numeric_limits<double>::quiet_NaN();
  if (p.amplitude == 0) return 0;
  const auto count = sampleCount();
  const auto index = std::clamp<long long>(
      static_cast<long long>(std::clamp(position, 0.0, 1.0) * count - 1.0),
      0, static_cast<long long>(count - 1));
  const double k = std::exp(double(index) * 0.01 - 5.0);
  const double length = p.windSpeed * p.windSpeed / 9.8;
  const double exponent = p.directionalFactorExponent;
  const auto power = [&](double x) {
    return p.amplitude * std::exp(-1.0 / (x * x)) / std::pow(x, 4.0 + exponent);
  };
  const double peak = std::sqrt(2.0 / (4.0 + exponent));
  const double scale = 0.95 / std::pow(power(peak), 0.1);
  const double damping = std::exp(-std::pow(k / length * p.lengthCutoff, 2.0));
  return scale * std::pow(std::max(0.0, power(k) * damping), 0.1);
}

inline std::array<double, 2> gridRange(const Parameters &p) {
  if (!valid(p)) return {{0, 0}};
  const double length = p.windSpeed * p.windSpeed / 9.8;
  const double coefficient = 2.0 * 3.14159265358979323846 / p.tileSize;
  const auto toPosition = [](double k) {
    return std::clamp((std::log(k) + 5.0) / 0.01 / sampleCount(), 0.0, 1.0);
  };
  return {{toPosition(length * coefficient),
           toPosition(length * coefficient * double(std::size_t(1) << p.resolution))}};
}
}  // namespace TSeExprSpectrum
