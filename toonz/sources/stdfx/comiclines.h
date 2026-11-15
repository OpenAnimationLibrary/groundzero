#pragma once

// Comic line engine: seeded speed and focus lines as a pure function of a
// position. A Field is built once from Settings; sample() then returns the
// premultiplied colour at any point, so a render is identical at every tile
// split, preview zoom and thread count.
//
// Two layouts share everything else. Parallel lines run along a direction
// inside a band; Radial lines run from an outer radius towards a centre. Each
// line has a position across the layout, a start and length along it, a width
// profile and a colour, all drawn from one seed. Every property has its own
// random stream, so changing one amount does not reshuffle the others.

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <stdexcept>
#include <vector>

namespace ComicLines {

const double pi     = 3.14159265358979323846;
const int maxLines  = 20000;
const double minLen = 1e-9;

struct Rgba {
  double r = 0, g = 0, b = 0, a = 0;
};

enum class Layout { Parallel, Radial };

struct Settings {
  Layout layout = Layout::Parallel;

  // Parallel: the band starts at the origin and runs bandLength along the
  // direction; lines spread over bandWidth across it, centred on the axis.
  // Radial: the origin is the centre.
  double originX = 0, originY = 0;
  double directionX = 1, directionY = 0;
  double bandLength = 400, bandWidth = 400;
  bool tile = false;  // Parallel: repeat the band along and across

  double innerRadius = 60, outerRadius = 500;
  double twist       = 0;  // Radial: radians a line turns from outer to inner
  double falloff     = 0;  // Radial: width scales with (radius / outer)^falloff

  int count             = 60;
  int groupSize         = 1;
  double attraction     = 0;  // 0 fills the group's slot, 1 stacks the lines
  double positionRandom = 1;

  double length       = 300;
  double lengthRandom = 0.5;
  double startRandom  = 1;

  double thickness       = 4;
  double thicknessRandom = 0.5;
  double taper           = 0;  // width lost from the start to the end
  double inLength = 0.2, outLength = 0.2;  // shares of the line that swell

  double waveAmplitude = 0, wavePeriod = 100, wavePhase = 0;
  double wavePhaseRandom = 0;

  Rgba colorA{0, 0, 0, 1}, colorB{0, 0, 0, 1};
  double colorRandom   = 0;
  double opacity       = 1;
  double opacityRandom = 0;

  double travel      = 0;  // distance every line has moved along itself
  std::uint32_t seed = 1;
};

enum Property {
  GroupPosition = 1,
  LinePosition,
  Length,
  Start,
  Thickness,
  Phase,
  Colour,
  Opacity
};

struct Line {
  double position = 0;  // across: units (Parallel) or radians (Radial)
  double start = 0, length = 0;
  double halfWidth = 0;
  double phase     = 0;
  Rgba color;  // straight alpha
  int index = 0;
};

struct Field {
  Settings settings;
  std::vector<Line> lines;  // sorted by position
  double along  = 0;        // length of the layout along the lines
  double across = 0;        // extent across: bandWidth or 2 pi
  double reach  = 0;        // widest half width plus the wave amplitude
};

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

inline double wrap(double value, double period) {
  const double r = std::fmod(value, period);
  return r < 0 ? r + period : r;
}

inline bool finite(double v) { return std::isfinite(v); }

}  // namespace detail

// A value in [0, 1) that depends only on the seed, the line and the property
inline double random(std::uint32_t seed, int index, Property property) {
  std::uint32_t h = detail::mix(seed + 0x9e3779b9U);
  h               = detail::mix(h ^ (std::uint32_t(index) * 0x85ebca6bU));
  h               = detail::mix(h ^ (std::uint32_t(property) * 0xc2b2ae35U));
  return h * (1.0 / 4294967296.0);
}

// The seed in use at a frame: it holds for interval frames, then changes.
// An interval of 0 or less never changes it.
inline std::uint32_t frameSeed(std::uint32_t seed, double frame,
                               int interval) {
  if (interval <= 0 || !detail::finite(frame)) return seed;
  const double step = std::floor(frame / interval);
  const std::uint32_t plane =
      std::uint32_t(std::int64_t(std::max(-2147483648.0,
                                          std::min(2147483647.0, step))));
  return detail::mix(seed ^ detail::mix(plane + 0x27d4eb2dU));
}

inline void validate(const Settings &s) {
  using detail::finite;
  const bool ok =
      finite(s.originX) && finite(s.originY) && finite(s.directionX) &&
      finite(s.directionY) && finite(s.bandLength) && finite(s.bandWidth) &&
      finite(s.innerRadius) && finite(s.outerRadius) && finite(s.twist) &&
      finite(s.falloff) && finite(s.attraction) && finite(s.positionRandom) &&
      finite(s.length) && finite(s.lengthRandom) && finite(s.startRandom) &&
      finite(s.thickness) && finite(s.thicknessRandom) && finite(s.taper) &&
      finite(s.inLength) && finite(s.outLength) && finite(s.waveAmplitude) &&
      finite(s.wavePeriod) && finite(s.wavePhase) &&
      finite(s.wavePhaseRandom) && finite(s.colorRandom) &&
      finite(s.opacity) && finite(s.opacityRandom) && finite(s.travel) &&
      s.count >= 0 && s.count <= maxLines && s.groupSize >= 1 &&
      s.thickness >= 0 && s.waveAmplitude >= 0 && s.falloff >= 0;
  if (!ok) throw std::invalid_argument("Comic Lines: invalid settings");
  for (const Rgba *c : {&s.colorA, &s.colorB})
    if (!finite(c->r) || !finite(c->g) || !finite(c->b) || !finite(c->a))
      throw std::invalid_argument("Comic Lines: invalid colour");
}

inline Field build(const Settings &settings) {
  validate(settings);
  using detail::unit;
  Field f;
  f.settings        = settings;
  const Settings &s = f.settings;
  const bool radial = s.layout == Layout::Radial;
  f.along  = radial ? s.outerRadius - s.innerRadius : s.bandLength;
  f.across = radial ? 2 * pi : s.bandWidth;
  if (!(f.along > minLen) || !(f.across > minLen) || s.count == 0 ||
      (radial && !(s.outerRadius > minLen)))
    return f;  // nothing to draw

  const int groups     = (s.count + s.groupSize - 1) / s.groupSize;
  const double slot    = f.across / groups;
  const double spread  = slot * (1 - unit(s.attraction));
  const double jitter  = unit(s.positionRandom);
  const double maxHalf = 0.5 * s.thickness;
  f.lines.reserve(size_t(s.count));
  for (int i = 0; i < s.count; ++i) {
    const int group  = i / s.groupSize;
    const int member = i % s.groupSize;
    const int size   = group == groups - 1 ? s.count - group * s.groupSize
                                           : s.groupSize;
    const double centre =
        (group + 0.5) * slot +
        (random(s.seed, group, GroupPosition) - 0.5) * slot * jitter;
    Line line;
    line.index    = i;
    line.position = centre + ((member + 0.5) / size - 0.5) * spread +
                    (random(s.seed, i, LinePosition) - 0.5) * (spread / size) *
                        jitter;
    line.position = radial || s.tile
                        ? detail::wrap(line.position, f.across)
                        : std::max(0.0, std::min(f.across, line.position));
    line.length = std::min(
        f.along, std::max(0.0, s.length) *
                     (1 - unit(s.lengthRandom) * random(s.seed, i, Length)));
    line.start = unit(s.startRandom) * random(s.seed, i, Start) *
                 (f.along - line.length);
    line.halfWidth =
        maxHalf * (1 - unit(s.thicknessRandom) * random(s.seed, i, Thickness));
    line.phase = s.wavePhase +
                 2 * pi * unit(s.wavePhaseRandom) * random(s.seed, i, Phase);
    const double blend = unit(s.colorRandom) * random(s.seed, i, Colour);
    line.color.r       = s.colorA.r + (s.colorB.r - s.colorA.r) * blend;
    line.color.g       = s.colorA.g + (s.colorB.g - s.colorA.g) * blend;
    line.color.b       = s.colorA.b + (s.colorB.b - s.colorA.b) * blend;
    line.color.a = unit(s.colorA.a + (s.colorB.a - s.colorA.a) * blend) *
                   unit(s.opacity) *
                   (1 - unit(s.opacityRandom) * random(s.seed, i, Opacity));
    if (line.length > minLen && line.halfWidth > 0 && line.color.a > 0)
      f.lines.push_back(line);
  }
  std::stable_sort(f.lines.begin(), f.lines.end(),
                   [](const Line &a, const Line &b) {
                     return a.position < b.position;
                   });
  f.reach = maxHalf + s.waveAmplitude;
  return f;
}

namespace detail {

// Width multiplier at t in [0, 1] along a line: swell in, swell out, taper
inline double profile(const Settings &s, double t) {
  double w        = 1 - unit(s.taper) * t;
  const double in = unit(s.inLength), out = unit(s.outLength);
  if (in > 0 && t < in) w *= t / in;
  if (out > 0 && t > 1 - out) w *= (1 - t) / out;
  return std::max(0.0, w);
}

// Share of a pixel of the given size, centred at distance d from the line's
// centre, that a line of the given half width covers
inline double coverage(double d, double halfWidth, double pixel) {
  const double low  = std::max(d - 0.5 * pixel, -halfWidth);
  const double high = std::min(d + 0.5 * pixel, halfWidth);
  return high > low ? std::min(1.0, (high - low) / pixel) : 0.0;
}

}  // namespace detail

// The premultiplied colour of the lines at (x, y). pixel is the size of an
// output pixel in the same units, and sets the antialiasing.
inline Rgba sample(const Field &f, double x, double y, double pixel) {
  Rgba out;
  if (f.lines.empty() || !(pixel > 0)) return out;
  const Settings &s = f.settings;
  const bool radial = s.layout == Layout::Radial;
  const double rx = x - s.originX, ry = y - s.originY;

  double along, position, radius = 0, window, widthScale = 1;
  bool periodic;
  if (radial) {
    radius = std::hypot(rx, ry);
    if (radius < s.innerRadius - pixel || radius > s.outerRadius + pixel)
      return out;
    along = s.outerRadius - radius;
    if (along < 0 || along > f.along) return out;
    position = detail::wrap(
        std::atan2(ry, rx) - s.twist * (along / f.along), f.across);
    const double reach = f.reach + pixel;
    window     = radius > reach ? std::asin(reach / radius) : 0.5 * pi;
    widthScale = s.falloff > 0 ? std::pow(radius / s.outerRadius, s.falloff)
                               : 1.0;
    periodic   = true;
  } else {
    const double norm = std::hypot(s.directionX, s.directionY);
    if (!(norm > minLen)) return out;
    const double dx = s.directionX / norm, dy = s.directionY / norm;
    along    = rx * dx + ry * dy;
    position = ry * dx - rx * dy + 0.5 * f.across;
    window   = f.reach + pixel;
    periodic = s.tile;
    if (s.tile) {
      along    = detail::wrap(along, f.along);
      position = detail::wrap(position, f.across);
    } else if (along < 0 || along > f.along || position < -window ||
               position > f.across + window) {
      return out;
    }
  }

  // Every line whose centre is within the window of this position, in the
  // order of their positions. The order depends only on the position, so a
  // pixel composites the same way in every tile.
  auto composite = [&](const Line &line, double delta) {
    if (radial && std::abs(delta) > 0.5 * pi) return;
    const double local = detail::wrap(along - line.start - s.travel, f.along);
    if (local > line.length) return;
    const double t    = local / line.length;
    const double half = line.halfWidth * detail::profile(s, t) * widthScale;
    if (!(half > 0)) return;
    double d = radial ? radius * std::sin(delta) : delta;
    if (s.waveAmplitude > 0 && s.wavePeriod > minLen)
      d -= s.waveAmplitude *
           std::sin(2 * pi * local / s.wavePeriod + line.phase);
    const double ends =
        detail::unit(std::min(local, line.length - local) / pixel + 0.5);
    const double alpha = detail::coverage(d, half, pixel) * ends * line.color.a;
    if (!(alpha > 0)) return;
    out.r = line.color.r * alpha + out.r * (1 - alpha);
    out.g = line.color.g * alpha + out.g * (1 - alpha);
    out.b = line.color.b * alpha + out.b * (1 - alpha);
    out.a = alpha + out.a * (1 - alpha);
  };
  auto gather = [&](double shift) {
    const double p = position + shift;
    auto first     = std::lower_bound(
        f.lines.begin(), f.lines.end(), p - window,
        [](const Line &line, double v) { return line.position < v; });
    for (auto it = first; it != f.lines.end() && it->position <= p + window;
         ++it)
      composite(*it, p - it->position);
  };
  // A window wider than half the layout would find a line twice
  if (periodic && window < 0.5 * f.across) {
    if (position + window > f.across) gather(-f.across);
    gather(0);
    if (position - window < 0) gather(f.across);
  } else {
    gather(0);
  }
  return out;
}

}  // namespace ComicLines
