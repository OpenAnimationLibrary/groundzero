#pragma once

// Rough Stroke: hand-drawn strokes along paths. Each path gets one or more
// copies; a copy is the path moved sideways by a constant offset, a wave and
// a slow random drift, with a width that swells in and out. A stretch of the
// path can be revealed, and the stretch can travel along it.
//
// All positions are in output pixels. A copy's coverage at a pixel is the
// largest coverage of any of its segments there, so joints are not drawn
// twice and the result does not depend on the tile.

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <vector>

namespace RoughStroke {

const double pi         = 3.14159265358979323846;
const int maxCopies     = 32;
const double minLength  = 1e-6;
const std::size_t maxVertices = 4u * 1024u * 1024u;

// Straight alpha when it is a setting, premultiplied when it is a pixel
struct Rgba {
  double r = 0, g = 0, b = 0, a = 0;
};

struct Point {
  double x = 0, y = 0;
  double thickness = 0;  // the drawn stroke's own half width here
};

struct Path {
  std::vector<Point> points;
  Rgba color{0, 0, 0, 1};  // the drawn stroke's colour
};

struct Settings {
  double width          = 2;  // added to the drawn width
  double thicknessScale = 1;  // multiplies the drawn width
  double inLength = 0.1, outLength = 0.1;  // shares that swell in and out

  int copies               = 1;
  double copyOffset        = 0;  // how far a copy may sit from the path
  double copyWidthRandom   = 0;
  double copyOpacityRandom = 0;

  double waveAmplitude = 0, wavePeriod = 40, wavePhase = 0;
  double wavePhaseRandom = 1;
  double drift           = 0;   // random sideways drift along the stroke
  double driftSize       = 30;  // length of the drift's features

  bool useStrokeColor = true;
  Rgba startColor{0, 0, 0, 1}, endColor{0, 0, 0, 1};
  double opacity = 1;

  double revealStart = 0, revealEnd = 1;  // shares of the path's length
  double travel = 0;  // share of the length the revealed stretch has moved
  bool loop     = false;  // a stretch that leaves one end returns at the other

  double step        = 1.5;  // distance between the copy's vertices
  std::uint32_t seed = 1;
};

struct Vertex {
  double x = 0, y = 0, half = 0;
  Rgba color;  // straight
};

// One run of connected vertices
struct Copy {
  std::vector<Vertex> vertices;
};

enum Property { Offset = 1, Width, Opacity, Phase, Drift };

namespace detail {

inline std::uint32_t mix(std::uint32_t h) {
  h ^= h >> 16;
  h *= 0x7feb352dU;
  h ^= h >> 15;
  h *= 0x846ca68bU;
  h ^= h >> 16;
  return h;
}
inline double unit(double v) { return std::max(0.0, std::min(1.0, v)); }
inline bool finite(double v) { return std::isfinite(v); }

inline double coverage(double d, double half) {
  const double low  = std::max(d - 0.5, -half);
  const double high = std::min(d + 0.5, half);
  return high > low ? std::min(1.0, high - low) : 0.0;
}

}  // namespace detail

// A value in [0, 1) that depends only on the seed, the path, the copy and
// the property
inline double random(std::uint32_t seed, int path, int copy,
                     Property property, int cell = 0) {
  std::uint32_t h = detail::mix(seed + 0x9e3779b9U);
  h               = detail::mix(h ^ (std::uint32_t(path) * 0x85ebca6bU));
  h               = detail::mix(h ^ (std::uint32_t(copy) * 0xc2b2ae35U));
  h               = detail::mix(h ^ (std::uint32_t(property) * 0x27d4eb2dU));
  h               = detail::mix(h ^ (std::uint32_t(cell) * 0x165667b1U));
  return h * (1.0 / 4294967296.0);
}

// The seed in use at a frame: it holds for interval frames, then changes.
// An interval of 0 or less never changes it.
inline std::uint32_t frameSeed(std::uint32_t seed, double frame,
                               int interval) {
  if (interval <= 0 || !detail::finite(frame)) return seed;
  const double step = std::floor(frame / interval);
  const std::uint32_t plane = std::uint32_t(
      std::int64_t(std::max(-2147483648.0, std::min(2147483647.0, step))));
  return detail::mix(seed ^ detail::mix(plane + 0x27d4eb2dU));
}

inline void validate(const Settings &s) {
  using detail::finite;
  bool ok = finite(s.width) && finite(s.thicknessScale) &&
            finite(s.inLength) && finite(s.outLength) && s.copies >= 1 &&
            s.copies <= maxCopies && finite(s.copyOffset) &&
            finite(s.copyWidthRandom) && finite(s.copyOpacityRandom) &&
            finite(s.waveAmplitude) && finite(s.wavePeriod) &&
            finite(s.wavePhase) && finite(s.wavePhaseRandom) &&
            finite(s.drift) && finite(s.driftSize) && s.driftSize > 0 &&
            finite(s.opacity) && finite(s.revealStart) &&
            finite(s.revealEnd) && finite(s.travel) && finite(s.step) &&
            s.step >= 0.25;
  for (const Rgba *c : {&s.startColor, &s.endColor})
    ok = ok && finite(c->r) && finite(c->g) && finite(c->b) && finite(c->a);
  if (!ok) throw std::invalid_argument("Rough Stroke: invalid settings");
}

// How far from its path a copy can reach, beyond the drawn thickness
inline double reach(const Settings &s) {
  validate(s);
  return std::abs(s.copyOffset) + std::abs(s.waveAmplitude) +
         std::abs(s.drift) + 0.5 * std::max(0.0, s.width) + 1.0;
}

namespace detail {

// Smooth random drift in [-1, 1] at distance d along a copy
inline double drift(const Settings &s, int path, int copy, double d) {
  const double u   = d / s.driftSize;
  const double c   = std::floor(u);
  const double t   = u - c;
  const int cell   = int(std::max(-1e9, std::min(1e9, c)));
  const double a   = random(s.seed, path, copy, Drift, cell);
  const double b   = random(s.seed, path, copy, Drift, cell + 1);
  const double mix = t * t * (3 - 2 * t);
  return 2 * (a + (b - a) * mix) - 1;
}

struct Sampler {
  const std::vector<Point> &points;
  std::vector<double> lengths;  // cumulative
  explicit Sampler(const std::vector<Point> &p) : points(p) {
    lengths.assign(p.size(), 0.0);
    for (std::size_t i = 1; i < p.size(); ++i)
      lengths[i] = lengths[i - 1] +
                   std::hypot(p[i].x - p[i - 1].x, p[i].y - p[i - 1].y);
  }
  double total() const { return lengths.empty() ? 0.0 : lengths.back(); }
  // Position, thickness and unit normal at a distance along the path
  void at(double d, Point &point, double &nx, double &ny) const {
    const auto upper = std::upper_bound(lengths.begin(), lengths.end(), d);
    std::size_t i    = upper == lengths.begin()
                           ? 0
                           : std::size_t(upper - lengths.begin()) - 1;
    i                = std::min(i, points.size() - 2);
    // Skip segments without length
    while (i + 2 < points.size() && lengths[i + 1] - lengths[i] < minLength)
      ++i;
    while (i > 0 && lengths[i + 1] - lengths[i] < minLength) --i;
    const double span = lengths[i + 1] - lengths[i];
    const double t    = span > minLength ? unit((d - lengths[i]) / span) : 0;
    const Point &a = points[i], &b = points[i + 1];
    point.x         = a.x + (b.x - a.x) * t;
    point.y         = a.y + (b.y - a.y) * t;
    point.thickness = a.thickness + (b.thickness - a.thickness) * t;
    const double dx = b.x - a.x, dy = b.y - a.y;
    const double n  = std::hypot(dx, dy);
    nx              = n > minLength ? -dy / n : 0;
    ny              = n > minLength ? dx / n : 0;
  }
};

}  // namespace detail

// The copies of one path. index tells paths apart, so each has its own
// random values. A looping stretch that crosses the path's end gives two
// runs per copy.
inline std::vector<Copy> build(const Path &path, int index,
                               const Settings &s) {
  validate(s);
  using detail::unit;
  std::vector<Copy> copies;
  if (path.points.size() < 2) return copies;
  const detail::Sampler sampler(path.points);
  const double total = sampler.total();
  if (!(total > minLength)) return copies;

  // The revealed stretch, as distances along the path
  double from = unit(std::min(s.revealStart, s.revealEnd)) * total;
  double to   = unit(std::max(s.revealStart, s.revealEnd)) * total;
  const double length = to - from;
  if (!(length > minLength)) return copies;
  double shift = s.travel * total;
  if (s.loop) shift -= std::floor(shift / total) * total;
  from += shift;
  to += shift;

  // Runs of the stretch that lie on the path
  struct Run {
    double from, to, base;  // base: distance into the stretch at from
  };
  std::vector<Run> runs;
  if (s.loop) {
    if (to <= total) {
      runs.push_back({from, to, 0});
    } else {
      if (from < total) runs.push_back({from, total, 0});
      runs.push_back({std::max(0.0, from - total), to - total,
                      std::max(0.0, total - from)});
    }
  } else {
    const double a = std::max(0.0, from), b = std::min(total, to);
    if (b - a > minLength) runs.push_back({a, b, a - from});
  }

  const double in = unit(s.inLength), out = unit(s.outLength);
  for (int c = 0; c < s.copies; ++c) {
    // The first copy sits on the path; the others spread around it
    const double offset =
        c == 0 ? 0.0
               : s.copyOffset * (2 * random(s.seed, index, c, Offset) - 1);
    const double widthScale =
        1 - unit(s.copyWidthRandom) * random(s.seed, index, c, Width);
    const double alphaScale =
        unit(s.opacity) *
        (1 - unit(s.copyOpacityRandom) * random(s.seed, index, c, Opacity));
    const double phase =
        s.wavePhase +
        2 * pi * unit(s.wavePhaseRandom) * random(s.seed, index, c, Phase);
    for (const Run &run : runs) {
      const int count = int(std::ceil((run.to - run.from) / s.step));
      if (count < 1) continue;
      if (std::size_t(count) + 1 > maxVertices)
        throw std::length_error("Rough Stroke: a path is too long");
      Copy copy;
      copy.vertices.reserve(std::size_t(count) + 1);
      for (int k = 0; k <= count; ++k) {
        const double d = run.from + (run.to - run.from) * k / count;
        // t runs over the whole revealed stretch, across both runs
        const double t = unit((run.base + (d - run.from)) / length);
        Point p;
        double nx, ny;
        sampler.at(d, p, nx, ny);
        double side = offset;
        if (s.waveAmplitude != 0 && s.wavePeriod > minLength)
          side += s.waveAmplitude * std::sin(2 * pi * d / s.wavePeriod + phase);
        if (s.drift != 0) side += s.drift * detail::drift(s, index, c, d);
        double profile = 1;
        if (in > 0 && t < in) profile *= t / in;
        if (out > 0 && t > 1 - out) profile *= (1 - t) / out;
        Vertex v;
        v.x    = p.x + nx * side;
        v.y    = p.y + ny * side;
        v.half = std::max(0.0, (p.thickness * s.thicknessScale +
                                0.5 * s.width) *
                                   widthScale) *
                 std::max(0.0, profile);
        if (s.useStrokeColor) {
          v.color = path.color;
        } else {
          v.color.r = s.startColor.r + (s.endColor.r - s.startColor.r) * t;
          v.color.g = s.startColor.g + (s.endColor.g - s.startColor.g) * t;
          v.color.b = s.startColor.b + (s.endColor.b - s.startColor.b) * t;
          v.color.a = s.startColor.a + (s.endColor.a - s.startColor.a) * t;
        }
        v.color.a = unit(v.color.a) * alphaScale;
        copy.vertices.push_back(v);
      }
      copies.push_back(std::move(copy));
    }
  }
  return copies;
}

// Draws a copy over the w by h premultiplied image whose pixel (0, 0) has
// its centre at (originX + 0.5, originY + 0.5).
inline void draw(const Copy &copy, int w, int h, double originX,
                 double originY, std::vector<Rgba> &image) {
  if (w <= 0 || h <= 0 || image.size() != std::size_t(w) * std::size_t(h))
    throw std::invalid_argument("Rough Stroke: image size mismatch");
  const std::size_t n = copy.vertices.size();
  if (n < 2) return;
  // The window of the image the copy can touch
  double minX = 1e300, minY = 1e300, maxX = -1e300, maxY = -1e300;
  for (const Vertex &v : copy.vertices) {
    minX = std::min(minX, v.x - v.half);
    maxX = std::max(maxX, v.x + v.half);
    minY = std::min(minY, v.y - v.half);
    maxY = std::max(maxY, v.y + v.half);
  }
  const int x0 = std::max(0, int(std::floor(minX - originX - 1)));
  const int y0 = std::max(0, int(std::floor(minY - originY - 1)));
  const int x1 = std::min(w - 1, int(std::ceil(maxX - originX + 1)));
  const int y1 = std::min(h - 1, int(std::ceil(maxY - originY + 1)));
  if (x1 < x0 || y1 < y0) return;
  const int ww = x1 - x0 + 1, wh = y1 - y0 + 1;
  std::vector<double> cover(std::size_t(ww) * wh, 0.0);
  std::vector<Rgba> colour(std::size_t(ww) * wh);

  for (std::size_t i = 0; i + 1 < n; ++i) {
    const Vertex &a = copy.vertices[i], &b = copy.vertices[i + 1];
    const double half = std::max(a.half, b.half);
    if (!(half > 0)) continue;
    const double dx = b.x - a.x, dy = b.y - a.y;
    const double len2 = dx * dx + dy * dy;
    const int sx0 = std::max(
        x0, int(std::floor(std::min(a.x, b.x) - half - originX - 1)));
    const int sx1 = std::min(
        x1, int(std::ceil(std::max(a.x, b.x) + half - originX + 1)));
    const int sy0 = std::max(
        y0, int(std::floor(std::min(a.y, b.y) - half - originY - 1)));
    const int sy1 = std::min(
        y1, int(std::ceil(std::max(a.y, b.y) + half - originY + 1)));
    for (int y = sy0; y <= sy1; ++y)
      for (int x = sx0; x <= sx1; ++x) {
        const double px = originX + x + 0.5 - a.x;
        const double py = originY + y + 0.5 - a.y;
        const double t =
            len2 > minLength ? detail::unit((px * dx + py * dy) / len2) : 0.0;
        const double d = std::hypot(px - dx * t, py - dy * t);
        const double c =
            detail::coverage(d, a.half + (b.half - a.half) * t);
        const std::size_t k = std::size_t(y - y0) * ww + (x - x0);
        if (c > cover[k]) {
          cover[k]    = c;
          colour[k].r = a.color.r + (b.color.r - a.color.r) * t;
          colour[k].g = a.color.g + (b.color.g - a.color.g) * t;
          colour[k].b = a.color.b + (b.color.b - a.color.b) * t;
          colour[k].a = a.color.a + (b.color.a - a.color.a) * t;
        }
      }
  }
  for (int y = 0; y < wh; ++y)
    for (int x = 0; x < ww; ++x) {
      const std::size_t k = std::size_t(y) * ww + x;
      const double alpha  = cover[k] * colour[k].a;
      if (!(alpha > 0)) continue;
      Rgba &out = image[std::size_t(y + y0) * w + (x + x0)];
      out.r     = colour[k].r * alpha + out.r * (1 - alpha);
      out.g     = colour[k].g * alpha + out.g * (1 - alpha);
      out.b     = colour[k].b * alpha + out.b * (1 - alpha);
      out.a     = alpha + out.a * (1 - alpha);
    }
}

}  // namespace RoughStroke
