#pragma once

#include "celdistance.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <vector>

// Compositing helpers: light wrap, edge extend and matte choke / spread.
// Each pixel depends only on the pixels within halo() of it, summed in a
// fixed order, so any tiling of the frame gives the same image.
namespace CelComposite {

// Premultiplied RGBA in [0, 1].
struct Color {
  double r = 0.0, g = 0.0, b = 0.0, a = 0.0;
};

namespace detail {

inline double unit(double v) { return std::max(0.0, std::min(1.0, v)); }

inline void checkSize(const std::vector<Color> &image, int w, int h) {
  if (image.size() != CelDistance::checkedArea(w, h))
    throw std::invalid_argument("Image size mismatch");
}

inline std::vector<std::uint8_t> solid(const std::vector<Color> &image,
                                       double threshold) {
  std::vector<std::uint8_t> mask(image.size());
  for (std::size_t i = 0; i < image.size(); ++i)
    mask[i] = image[i].a >= threshold;
  return mask;
}

// A colour of the same hue at another alpha.
inline Color withAlpha(const Color &c, double alpha) {
  if (!(c.a > 1e-12)) return Color();
  const double k = alpha / c.a;
  return Color{c.r * k, c.g * k, c.b * k, alpha};
}

// Separable Gaussian blur, each sum taken in the same order for every pixel.
inline bool blur(const std::vector<Color> &image, int w, int h, double radius,
                 std::vector<Color> &out, const int *canceled) {
  out = image;
  if (radius <= 0.0) return true;
  const int reach    = int(std::ceil(radius * 3.0));
  const double sigma = radius;
  std::vector<double> kernel(std::size_t(2 * reach + 1));
  double total = 0.0;
  for (int i = -reach; i <= reach; ++i)
    total += kernel[std::size_t(i + reach)] =
        std::exp(-0.5 * (i * i) / (sigma * sigma));
  for (double &k : kernel) k /= total;
  std::vector<Color> pass(image.size());
  for (int axis = 0; axis < 2; ++axis) {
    const std::vector<Color> &from = axis == 0 ? image : pass;
    std::vector<Color> &to         = axis == 0 ? pass : out;
    for (int y = 0; y < h; ++y) {
      if (CelDistance::isCanceled(canceled)) return false;
      for (int x = 0; x < w; ++x) {
        Color sum;
        for (int i = -reach; i <= reach; ++i) {
          const int sx = axis == 0 ? x + i : x, sy = axis == 0 ? y : y + i;
          if (sx < 0 || sy < 0 || sx >= w || sy >= h) continue;
          const Color &c = from[std::size_t(sy) * w + sx];
          const double k = kernel[std::size_t(i + reach)];
          sum.r += c.r * k;
          sum.g += c.g * k;
          sum.b += c.b * k;
          sum.a += c.a * k;
        }
        to[std::size_t(y) * w + x] = sum;
      }
    }
  }
  return true;
}

}  // namespace detail

// ---------------------------------------------------------------------------
// Light wrap: the background, blurred, bleeds onto the foreground's edges.

enum class WrapBlend { Screen, Add };

struct WrapSettings {
  double width     = 8.0;  // how far into the foreground, in pixels
  double softness  = 4.0;  // background blur, in pixels
  double intensity = 0.6;
  WrapBlend blend  = WrapBlend::Screen;
};

inline int wrapHalo(const WrapSettings &s) {
  if (!std::isfinite(s.width) || s.width < 0.0 || !std::isfinite(s.softness) ||
      s.softness < 0.0 || !std::isfinite(s.intensity))
    throw std::invalid_argument("Invalid light wrap settings");
  const double reach = s.width + 1.0 + std::ceil(s.softness * 3.0) + 1.0;
  if (reach > 1 << 20) throw std::length_error("Light wrap is too wide");
  return int(std::ceil(reach));
}

// The foreground with the background wrapped onto its edges, for w x h
// images. Returns false when canceled, with out cleared.
inline bool lightWrap(const std::vector<Color> &foreground,
                      const std::vector<Color> &background, int w, int h,
                      const WrapSettings &s, std::vector<Color> &out,
                      const int *canceled = nullptr) {
  wrapHalo(s);
  detail::checkSize(foreground, w, h);
  detail::checkSize(background, w, h);
  out.clear();
  std::vector<Color> light;
  if (!detail::blur(background, w, h, s.softness, light, canceled))
    return false;
  // The band inside the foreground's edge: 1 at the edge, fading to 0 at
  // width pixels in
  std::vector<std::uint8_t> outside(foreground.size());
  for (std::size_t i = 0; i < foreground.size(); ++i)
    outside[i] = foreground[i].a < 0.5;
  CelDistance::Field field;
  if (!CelDistance::transform(outside, w, h, CelDistance::Metric::Euclidean,
                              field, canceled, true))
    return false;
  out.resize(foreground.size());
  const double intensity = std::max(0.0, s.intensity);
  for (std::size_t i = 0; i < foreground.size(); ++i) {
    const Color &f = foreground[i];
    const double depth =
        s.width > 0.0 ? detail::unit(1.0 - (field.distance[i] - 0.5) / s.width)
                      : 0.0;
    const double amount = intensity * depth * detail::unit(f.a);
    // The light, as seen through the foreground's coverage
    const Color &l  = light[i];
    const double lr = l.r * amount, lg = l.g * amount, lb = l.b * amount;
    Color c = f;
    if (s.blend == WrapBlend::Screen) {
      // Screen of straight colours, kept at the foreground's alpha
      c.r = f.r + lr * (f.a - f.r);
      c.g = f.g + lg * (f.a - f.g);
      c.b = f.b + lb * (f.a - f.b);
    } else {
      c.r = std::min(f.a, f.r + lr * f.a);
      c.g = std::min(f.a, f.g + lg * f.a);
      c.b = std::min(f.a, f.b + lb * f.a);
    }
    out[i] = c;
  }
  if (CelDistance::isCanceled(canceled)) {
    out.clear();
    return false;
  }
  return true;
}

// ---------------------------------------------------------------------------
// Edge extend: the colour of the solid interior pushed out over the
// antialiased edge and beyond, so keys and blurs do not pick up the edge's
// mix of foreground and background.

struct ExtendSettings {
  double radius    = 4.0;   // in pixels
  double solid     = 0.99;  // alpha from which a pixel is interior
  bool extendAlpha = false;
};

inline int extendHalo(const ExtendSettings &s) {
  if (!std::isfinite(s.radius) || s.radius < 0.0 || !std::isfinite(s.solid))
    throw std::invalid_argument("Invalid edge extend settings");
  if (s.radius > 1 << 20) throw std::length_error("Edge extend is too wide");
  return int(std::ceil(s.radius)) + 1;
}

inline bool edgeExtend(const std::vector<Color> &image, int w, int h,
                       const ExtendSettings &s, std::vector<Color> &out,
                       const int *canceled = nullptr) {
  extendHalo(s);
  detail::checkSize(image, w, h);
  out.clear();
  CelDistance::Field field;
  if (!CelDistance::transform(detail::solid(image, s.solid), w, h,
                              CelDistance::Metric::Euclidean, field, canceled))
    return false;
  out = image;
  for (std::size_t i = 0; i < image.size(); ++i) {
    const int nearest = field.nearest[i];
    if (nearest < 0 || field.distance[i] <= 0.0 || field.distance[i] > s.radius)
      continue;
    const Color &interior = image[std::size_t(nearest)];
    const double alpha    = s.extendAlpha ? 1.0 : image[i].a;
    out[i]                = detail::withAlpha(interior, alpha);
  }
  if (CelDistance::isCanceled(canceled)) {
    out.clear();
    return false;
  }
  return true;
}

// ---------------------------------------------------------------------------
// Matte choke / spread: the alpha's edge moved in (negative amount) or out
// (positive), with a feather; colours keep their hue.

struct ChokeSettings {
  double amount  = -1.0;  // pixels: negative chokes, positive spreads
  double feather = 0.5;   // pixels
};

inline int chokeHalo(const ChokeSettings &s) {
  if (!std::isfinite(s.amount) || !std::isfinite(s.feather) || s.feather < 0.0)
    throw std::invalid_argument("Invalid choke settings");
  const double reach = std::abs(s.amount) + s.feather + 2.0;
  if (reach > 1 << 20) throw std::length_error("Choke is too wide");
  return int(std::ceil(reach));
}

inline bool choke(const std::vector<Color> &image, int w, int h,
                  const ChokeSettings &s, std::vector<Color> &out,
                  const int *canceled = nullptr) {
  chokeHalo(s);
  detail::checkSize(image, w, h);
  out.clear();
  const std::vector<std::uint8_t> matte = detail::solid(image, 0.5);
  std::vector<std::uint8_t> moved;
  if (!CelDistance::thin(matte, w, h, s.amount, CelDistance::Metric::Euclidean,
                         moved, canceled))
    return false;
  std::vector<double> coverage;
  if (!CelDistance::feather(moved, w, h, s.feather,
                            CelDistance::Metric::Euclidean,
                            CelDistance::Direction::Around, coverage, canceled))
    return false;
  // A spread needs colour where there was none: the nearest coloured pixel
  CelDistance::Field field;
  if (!CelDistance::transform(matte, w, h, CelDistance::Metric::Euclidean,
                              field, canceled))
    return false;
  out.resize(image.size());
  for (std::size_t i = 0; i < image.size(); ++i) {
    const double alpha = coverage[i];
    if (alpha <= 0.0) {
      out[i] = Color();
      continue;
    }
    const Color &source = image[i].a > 1e-12 || field.nearest[i] < 0
                              ? image[i]
                              : image[std::size_t(field.nearest[i])];
    out[i]              = detail::withAlpha(source, alpha);
  }
  if (CelDistance::isCanceled(canceled)) {
    out.clear();
    return false;
  }
  return true;
}

}  // namespace CelComposite
