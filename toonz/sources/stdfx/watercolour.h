#pragma once

// Watercolour: flat-colour art becomes a painted wash. Region edges wander
// (a noise field moves where each pixel is read from), colour bleeds past
// them (a blur), pigment gathers at the edges and granulates inside the
// regions, and the result can sit on grained paper.
//
// Every output pixel depends only on its global position and on source
// pixels within halo() of it, so any tiling of the frame gives the same
// image. The noise holds for a number of frames, so the paint does not
// shimmer on every frame.

#include "celdistance.h"
#include "celnoise.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <vector>

namespace Watercolour {

// Premultiplied RGBA in [0, 1]
struct Color {
  double r = 0, g = 0, b = 0, a = 0;
};
// Straight RGB in [0, 1]
struct Rgb {
  double r = 1, g = 1, b = 1;
};

const double maxWobble = 64, maxBleed = 64, maxEdgeWidth = 64;

struct Settings {
  double wobble     = 3;   // how far an edge wanders, in pixels
  double wobbleSize = 30;  // size of the wander's features, in pixels
  double bleed      = 4;   // how far colour spreads past an edge, in pixels

  double edgeDarkening = 0.35;  // share of the colour lost at an edge
  double edgeWidth     = 6;     // how far in from an edge it reaches
  double edgeTolerance = 0.08;  // channel difference that makes an edge

  double granulation = 0.25;  // share of the colour lost in the grain
  double granuleSize = 12;    // in pixels

  bool paper = true;  // lay the wash on opaque paper
  Rgb paperColor{0.98, 0.96, 0.92};
  double paperGrain = 0.15;  // share of the colour lost in the paper's grain
  double paperSize  = 2;     // in pixels

  int hold           = 2;  // frames each noise is held for
  std::uint32_t seed = 1;
};

inline void validate(const Settings &s) {
  auto finite = [](double v) { return std::isfinite(v); };
  const bool ok =
      finite(s.wobble) && s.wobble >= 0 && s.wobble <= maxWobble &&
      finite(s.wobbleSize) && s.wobbleSize >= 1 && finite(s.bleed) &&
      s.bleed >= 0 && s.bleed <= maxBleed && finite(s.edgeDarkening) &&
      finite(s.edgeWidth) && s.edgeWidth >= 0 &&
      s.edgeWidth <= maxEdgeWidth && finite(s.edgeTolerance) &&
      s.edgeTolerance >= 0 && finite(s.granulation) &&
      finite(s.granuleSize) && s.granuleSize >= 1 && finite(s.paperGrain) &&
      finite(s.paperSize) && s.paperSize >= 1 && finite(s.paperColor.r) &&
      finite(s.paperColor.g) && finite(s.paperColor.b) && s.hold >= 1;
  if (!ok) throw std::invalid_argument("Watercolour: invalid settings");
}

// The blur runs twice with this radius, so colour spreads twice as far
inline int blurRadius(const Settings &s) {
  return int(std::floor(0.5 * s.bleed + 0.5));
}

// The pixels around a tile that its result depends on
inline int halo(const Settings &s) {
  validate(s);
  const double wander = s.wobble > 0 ? s.wobble + 1 : 0;
  const double edge   = s.edgeDarkening > 0 ? s.edgeWidth : 0;
  return int(std::ceil(wander + 2 * blurRadius(s) + edge)) + 2;
}

// The noise plane of a frame: it changes every hold frames
inline double plane(const Settings &s, double frame) {
  validate(s);
  return std::isfinite(frame) ? std::floor(frame / s.hold + 1e-9) : 0.0;
}

namespace detail {

inline double unit(double v) { return std::max(0.0, std::min(1.0, v)); }

inline double noise(const Settings &s, std::uint32_t salt, double size,
                    double offset, double x, double y) {
  CelNoise::Settings n;
  n.type      = CelNoise::Smooth;
  n.seed      = s.seed * 2654435761u + salt;
  n.offset    = offset;
  n.thickness = std::max(1.0, std::min(100.0, size));
  return CelNoise::sample(n, x, y);
}

// Bilinear sample at pixel (x, y) moved by (ox, oy); outside is clear. The
// weights depend only on the offset, so every tile computes the same.
inline Color sample(const std::vector<Color> &image, int w, int h, int x,
                    int y, double ox, double oy) {
  const double fx = std::floor(ox), fy = std::floor(oy);
  const int x0 = x + int(fx), y0 = y + int(fy);
  const double tx = ox - fx, ty = oy - fy;
  auto at = [&](int px, int py) {
    return px < 0 || py < 0 || px >= w || py >= h
               ? Color()
               : image[std::size_t(py) * w + px];
  };
  const Color a = at(x0, y0), b = at(x0 + 1, y0), c = at(x0, y0 + 1),
              d = at(x0 + 1, y0 + 1);
  auto mix = [&](double pa, double pb, double pc, double pd) {
    return (pa * (1 - tx) + pb * tx) * (1 - ty) +
           (pc * (1 - tx) + pd * tx) * ty;
  };
  return {mix(a.r, b.r, c.r, d.r), mix(a.g, b.g, c.g, d.g),
          mix(a.b, b.b, c.b, d.b), mix(a.a, b.a, c.a, d.a)};
}

// A box blur along one axis. Each pixel sums its own window from left to
// right, so the result does not depend on where the tile starts.
inline void blurAxis(const std::vector<Color> &in, std::vector<Color> &out,
                     int w, int h, int radius, bool horizontal) {
  out.assign(in.size(), Color());
  const double weight = 1.0 / (2 * radius + 1);
  for (int y = 0; y < h; ++y)
    for (int x = 0; x < w; ++x) {
      Color sum;
      for (int k = -radius; k <= radius; ++k) {
        const int sx = horizontal ? x + k : x, sy = horizontal ? y : y + k;
        if (sx < 0 || sy < 0 || sx >= w || sy >= h) continue;
        const Color &c = in[std::size_t(sy) * w + sx];
        sum.r += c.r;
        sum.g += c.g;
        sum.b += c.b;
        sum.a += c.a;
      }
      out[std::size_t(y) * w + x] = {sum.r * weight, sum.g * weight,
                                     sum.b * weight, sum.a * weight};
    }
}

}  // namespace detail

// image is w by h premultiplied pixels whose pixel (0, 0) is at the global
// position (originX, originY). out has the same size; its pixels are exact
// at least halo() away from the image's border. Returns false if canceled.
inline bool render(const std::vector<Color> &image, int w, int h,
                   double originX, double originY, const Settings &s,
                   double frame, std::vector<Color> &out,
                   const int *canceled = nullptr) {
  validate(s);
  const std::size_t area = CelDistance::checkedArea(w, h);
  if (image.size() != area)
    throw std::invalid_argument("Watercolour: image size mismatch");
  auto stop = [&] { return canceled && *canceled; };
  const double offset = plane(s, frame);

  // The edges wander
  std::vector<Color> wash(area);
  for (int y = 0; y < h; ++y) {
    if (stop()) return false;
    for (int x = 0; x < w; ++x) {
      double dx = 0, dy = 0;
      if (s.wobble > 0) {
        const double gx = originX + x, gy = originY + y;
        dx = s.wobble *
             (2 * detail::noise(s, 0x68e31da4u, s.wobbleSize, offset, gx, gy) -
              1);
        dy = s.wobble *
             (2 * detail::noise(s, 0xb5297a4du, s.wobbleSize, offset, gx, gy) -
              1);
      }
      wash[std::size_t(y) * w + x] = detail::sample(image, w, h, x, y, dx, dy);
    }
  }

  // Distance to the nearest edge between two regions of the wandered wash
  std::vector<double> edge(area, 0.0);
  if (s.edgeDarkening > 0 && s.edgeWidth > 0) {
    std::vector<std::uint8_t> seeds(area, 0);
    auto differs = [&](const Color &a, const Color &b) {
      return std::max(std::max(std::abs(a.r - b.r), std::abs(a.g - b.g)),
                      std::max(std::abs(a.b - b.b), std::abs(a.a - b.a))) >
             s.edgeTolerance;
    };
    for (int y = 0; y < h; ++y)
      for (int x = 0; x < w; ++x) {
        const Color &c = wash[std::size_t(y) * w + x];
        seeds[std::size_t(y) * w + x] =
            (x > 0 && differs(c, wash[std::size_t(y) * w + x - 1])) ||
            (x + 1 < w && differs(c, wash[std::size_t(y) * w + x + 1])) ||
            (y > 0 && differs(c, wash[std::size_t(y - 1) * w + x])) ||
            (y + 1 < h && differs(c, wash[std::size_t(y + 1) * w + x]));
      }
    CelDistance::Field field;
    if (!CelDistance::transform(seeds, w, h, CelDistance::Metric::Euclidean,
                                field, canceled))
      return false;
    for (std::size_t i = 0; i < area; ++i) {
      const double t = detail::unit(1 - field.distance[i] / s.edgeWidth);
      edge[i]        = detail::unit(s.edgeDarkening) * t * t * (3 - 2 * t);
    }
  }

  // The colour bleeds
  const int radius = blurRadius(s);
  if (radius > 0) {
    std::vector<Color> pass;
    for (int repeat = 0; repeat < 2; ++repeat) {
      if (stop()) return false;
      detail::blurAxis(wash, pass, w, h, radius, true);
      if (stop()) return false;
      detail::blurAxis(pass, wash, w, h, radius, false);
    }
  }

  // Pigment gathers at the edges and in the grain, on paper
  out.assign(area, Color());
  const double granulation = detail::unit(s.granulation);
  const double paperGrain  = detail::unit(s.paperGrain);
  for (int y = 0; y < h; ++y) {
    if (stop()) return false;
    for (int x = 0; x < w; ++x) {
      const std::size_t i = std::size_t(y) * w + x;
      const double gx = originX + x, gy = originY + y;
      double keep = 1 - edge[i];
      if (granulation > 0)
        keep *= 1 - granulation * detail::noise(s, 0x1b56c4e9u, s.granuleSize,
                                                offset, gx, gy);
      Color c = wash[i];
      c.r *= keep;
      c.g *= keep;
      c.b *= keep;
      // The paper does not change with the hold: it is the same sheet
      const double sheet =
          paperGrain > 0
              ? 1 - paperGrain *
                        detail::noise(s, 0x7f4a7c15u, s.paperSize, 0, gx, gy)
              : 1.0;
      if (s.paper) {
        const double bare = 1 - detail::unit(c.a);
        c.r = (c.r + bare * detail::unit(s.paperColor.r)) * sheet;
        c.g = (c.g + bare * detail::unit(s.paperColor.g)) * sheet;
        c.b = (c.b + bare * detail::unit(s.paperColor.b)) * sheet;
        c.a = 1;
      } else {
        c.r *= sheet;
        c.g *= sheet;
        c.b *= sheet;
      }
      out[i] = c;
    }
  }
  return !stop();
}

}  // namespace Watercolour
