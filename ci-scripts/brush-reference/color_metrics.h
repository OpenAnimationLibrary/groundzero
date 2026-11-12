#pragma once
#include <algorithm>
#include <cmath>

// Independent numerical implementation of CIEDE2000, kL=kC=kH=1.
// Equations and numerical reference: Sharma, Wu and Dalal (2005),
// https://www.ece.rochester.edu/~gsharma/ciede2000/
// No reference implementation code is included.
namespace BrushMetrics {
struct Lab {
  double l, a, b;
};
inline Lab srgbLab(double r, double g, double b) {
  auto linear = [](double x) {
    return x <= .04045 ? x / 12.92 : std::pow((x + .055) / 1.055, 2.4);
  };
  r = linear(r);
  g = linear(g);
  b = linear(b);
  auto f = [](double t) {
    constexpr double delta = 6.0 / 29;
    return t > delta * delta * delta ? std::cbrt(t)
                                     : t / (3 * delta * delta) + 4.0 / 29;
  };
  const double x = f((.4124564 * r + .3575761 * g + .1804375 * b) / .95047);
  const double y = f(.2126729 * r + .7151522 * g + .0721750 * b);
  const double z = f((.0193339 * r + .1191920 * g + .9503041 * b) / 1.08883);
  return {116 * y - 16, 500 * (x - y), 200 * (y - z)};
}
inline double deltaE00(Lab first, Lab second) {
  constexpr double pi = 3.14159265358979323846;
  auto radians = [=](double d) { return d * pi / 180; };
  auto hue = [=](double a, double b) {
    double h = std::atan2(b, a) * 180 / pi;
    return h < 0 ? h + 360 : h;
  };
  auto chromaRatio = [](double c) {
    double power = std::pow(c / 25, 7);
    return std::sqrt(power / (power + 1));
  };
  const double c =
      (std::hypot(first.a, first.b) + std::hypot(second.a, second.b)) / 2;
  const double g = .5 * (1 - chromaRatio(c));
  const double a1 = (1 + g) * first.a, a2 = (1 + g) * second.a;
  const double c1 = std::hypot(a1, first.b), c2 = std::hypot(a2, second.b);
  const double h1 = c1 == 0 ? 0 : hue(a1, first.b);
  const double h2 = c2 == 0 ? 0 : hue(a2, second.b);
  double dh = h2 - h1;
  if (c1 * c2 == 0)
    dh = 0;
  else if (dh > 180)
    dh -= 360;
  else if (dh < -180)
    dh += 360;
  const double dl = second.l - first.l, dc = c2 - c1;
  const double dH = 2 * std::sqrt(c1 * c2) * std::sin(radians(dh / 2));
  const double lbar = (first.l + second.l) / 2, cbar = (c1 + c2) / 2;
  double hbar = (h1 + h2) / 2;
  if (c1 * c2 == 0)
    hbar = h1 + h2;
  else if (std::abs(h1 - h2) > 180)
    hbar += h1 + h2 < 360 ? 180 : -180;
  const double t = 1 - .17 * std::cos(radians(hbar - 30)) +
                   .24 * std::cos(radians(2 * hbar)) +
                   .32 * std::cos(radians(3 * hbar + 6)) -
                   .20 * std::cos(radians(4 * hbar - 63));
  const double l50 = (lbar - 50) * (lbar - 50);
  const double sl = 1 + .015 * l50 / std::sqrt(20 + l50);
  const double sc = 1 + .045 * cbar, sh = 1 + .015 * cbar * t;
  const double theta = 30 * std::exp(-std::pow((hbar - 275) / 25, 2));
  const double rt = -2 * chromaRatio(cbar) * std::sin(radians(2 * theta));
  const double l = dl / sl, cc = dc / sc, hh = dH / sh;
  return std::sqrt(std::max(0.0, l * l + cc * cc + hh * hh + rt * cc * hh));
}
} // namespace BrushMetrics
