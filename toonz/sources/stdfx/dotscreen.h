#pragma once

// Dot Screen: a lattice of dots whose size, aspect, rotation, opacity and
// colour can each be driven by a control image or a built-in ramp. A Grid
// holds the dots that can touch a region; sample() returns the premultiplied
// colour at a point. A dot depends only on its lattice cell and on the
// control images and ramps at its centre, so any tiling of the frame gives
// the same picture.

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <stdexcept>
#include <vector>

namespace DotScreen {

const double pi           = 3.14159265358979323846;
const int modifierCount   = 3;
const double maxSize      = 3.0;  // a dot's diameter, in cells
const double minAspect    = 0.05;
const double maxAspect    = 4.0;
const std::size_t maxDots = 4u * 1024u * 1024u;  // about 350 MB of dots

// Premultiplied when it is a pixel, straight when it is a setting
struct Rgba {
  double r = 0, g = 0, b = 0, a = 0;
};

enum class Shape { Round, Square, Diamond, Line };
enum class Target { None, Size, Aspect, Rotation, Opacity, Colour };
enum class Mode { Set, Scale, Offset };
enum class Source { Input, LinearRamp, RadialRamp };

// The control image's luminance at a dot (of its straight colour, 0 to 1)
// picks a value between low and high. That value replaces the property,
// multiplies it or is added to it. For Colour, high is the share of the
// control image's colour the dot takes. A modifier with a target also
// multiplies the dot's opacity by the control image's alpha, so a
// transparent area of the control image gets no dots.
//
// With a ramp as its source the control image is an opaque grey, black at
// the start point and white at the end point (see rampStrength()), in the
// coordinates of the dot centres.
struct Modifier {
  Target target = Target::None;
  Mode mode     = Mode::Scale;
  double low = 0, high = 1;
  bool invert   = false;
  Source source = Source::Input;
  double startX = -200, startY = 0, endX = 200, endY = 0;
};

struct Settings {
  double spacing = 12;  // distance between dot centres
  double angle   = 0;   // radians
  double stagger = 0;   // share of a cell that odd rows are shifted by
  double jitter  = 0;   // share of a cell a dot may leave its place by

  Shape shape     = Shape::Round;
  double size     = 0.7;  // diameter, as a share of the spacing
  double aspect   = 1;    // width over height
  double rotation = 0;    // radians, on top of the lattice angle
  double opacity  = 1;
  double softness = 0;  // extra edge width, in the same units as spacing
  double pixel    = 1;  // an output pixel, in those units: the antialiasing

  Rgba colorA{0, 0, 0, 1}, colorB{0, 0, 0, 1};
  // At random, up to that share: the colour moves towards colorB; the size,
  // aspect and opacity are lost; the rotation turns by half a turn either way
  double colorRandom = 0, sizeRandom = 0, aspectRandom = 0,
         rotationRandom = 0, opacityRandom = 0;
  std::uint32_t seed = 1;

  Modifier modifiers[modifierCount];
};

struct Dot {
  double x = 0, y = 0;      // centre
  double column = 0;        // centre along the lattice rows, in cells
  double radius = 0;        // half the height
  double aspect = 1;
  double cosine = 1, sine = 0;  // of the dot's own direction
  Rgba color;                   // straight
};

struct Grid {
  Settings settings;
  int i0 = 0, j0 = 0, columns = 0, rows = 0;
  std::vector<Dot> dots;  // rows * columns, row by row
  double reach = 0;  // the farthest any dot's edge is from its centre
  double cosine = 1, sine = 0;
};

// Each random stream; a new one goes at the end, so the others keep their
// values
enum Property {
  JitterX = 1,
  JitterY,
  Size,
  Rotation,
  Opacity,
  Colour,
  Aspect
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
inline bool finite(double v) { return std::isfinite(v); }
// The luminance of a premultiplied colour's straight colour
inline double strength(const Rgba &c) {
  if (!(c.a > 1e-9)) return 0;
  return unit((0.2126 * c.r + 0.7152 * c.g + 0.0722 * c.b) / c.a);
}
inline double apply(const Modifier &m, double value, double s) {
  const double v = m.low + (m.high - m.low) * s;
  switch (m.mode) {
  case Mode::Set:
    return v;
  case Mode::Offset:
    return value + v;
  case Mode::Scale:
    break;
  }
  return value * v;
}

// The values a property can take
struct Range {
  double low, high;
};
// The values a modifier can give a property that could take those in value
inline Range apply(const Modifier &m, Range value) {
  const double a = std::min(m.low, m.high), b = std::max(m.low, m.high);
  switch (m.mode) {
  case Mode::Set:
    return {a, b};
  case Mode::Offset:
    return {value.low + a, value.high + b};
  case Mode::Scale:
    break;
  }
  const double products[] = {value.low * a, value.low * b, value.high * a,
                             value.high * b};
  return {*std::min_element(std::begin(products), std::end(products)),
          *std::max_element(std::begin(products), std::end(products))};
}
// The largest value a property clamped to [low, high] can take, if it can
// take no more than v; anything not below high, NaN included, gives high
inline double upTo(double v, double low, double high) {
  return v < high ? std::max(low, v) : high;
}

}  // namespace detail

// A value in [0, 1) that depends only on the seed, the cell and the property
inline double random(std::uint32_t seed, int i, int j, Property property) {
  std::uint32_t h = detail::mix(seed + 0x9e3779b9U);
  h               = detail::mix(h ^ (std::uint32_t(i) * 0x85ebca6bU));
  h               = detail::mix(h ^ (std::uint32_t(j) * 0xc2b2ae35U));
  h               = detail::mix(h ^ (std::uint32_t(property) * 0x27d4eb2dU));
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
  bool ok = finite(s.spacing) && s.spacing >= 1e-3 && finite(s.angle) &&
            finite(s.stagger) && finite(s.jitter) && finite(s.size) &&
            finite(s.aspect) && finite(s.rotation) && finite(s.opacity) &&
            finite(s.softness) && s.softness >= 0 && finite(s.pixel) &&
            s.pixel > 0 && finite(s.colorRandom) && finite(s.sizeRandom) &&
            finite(s.aspectRandom) && finite(s.rotationRandom) &&
            finite(s.opacityRandom);
  for (const Modifier &m : s.modifiers)
    ok = ok && finite(m.low) && finite(m.high) && finite(m.startX) &&
         finite(m.startY) && finite(m.endX) && finite(m.endY);
  for (const Rgba *c : {&s.colorA, &s.colorB})
    ok = ok && finite(c->r) && finite(c->g) && finite(c->b) && finite(c->a);
  if (!ok) throw std::invalid_argument("Dot Screen: invalid settings");
}

// How far from its centre a dot reaches, its antialiased edge included
inline double extent(const Settings &s, double radius, double aspect) {
  const double r = radius + 0.5 * (s.pixel + s.softness);
  switch (s.shape) {
  case Shape::Square:  // to a corner
    return r * std::hypot(aspect, 1.0);
  case Shape::Diamond:  // to a vertex
    return r * std::sqrt(2.0) * std::max(1.0, aspect);
  case Shape::Line:  // half a cell along it, to a corner
    return std::hypot(0.5 * s.spacing, r);
  case Shape::Round:
    break;
  }
  return r * std::max(1.0, aspect);
}

// How far from a point the control images and the lattice are read: the
// largest dot that the settings and the modifiers on Size and Aspect can
// make, and how far jitter and stagger can move a dot from its cell. A
// modifier gives a value between low and high whatever its source, so a
// ramp moves the margin no more than a control image would.
inline double reach(const Settings &s) {
  validate(s);
  using detail::Range;
  // Randomness takes a share away, which a negative factor can turn into
  // the largest value
  const double least = s.size * (1 - detail::unit(s.sizeRandom));
  Range size{std::min(s.size, least), std::max(s.size, least)};
  const double narrowest = s.aspect * (1 - detail::unit(s.aspectRandom));
  Range aspect{std::min(s.aspect, narrowest), std::max(s.aspect, narrowest)};
  for (const Modifier &m : s.modifiers) {
    if (m.target == Target::Size) size = detail::apply(m, size);
    if (m.target == Target::Aspect) aspect = detail::apply(m, aspect);
  }
  const double radius = 0.5 * s.spacing * detail::upTo(size.high, 0, maxSize);
  const double dot    = extent(
      s, radius, detail::upTo(aspect.high, minAspect, maxAspect));
  const double moved =
      s.stagger - std::floor(s.stagger) + 0.5 * detail::unit(s.jitter);
  // With a little room for rounding
  return (dot + moved * s.spacing) * (1 + 1e-9);
}

// The grey, 0 to 1, of a modifier's ramp at a point. A linear ramp goes from
// the start point to the end point along the line between them, and is
// constant across it; a radial ramp grows with the distance from the start
// point and is white from the distance of the end point on. When the two
// points coincide the ramp is white everywhere.
inline double rampStrength(const Modifier &m, double x, double y) {
  const double dx = m.endX - m.startX, dy = m.endY - m.startY;
  const double px = x - m.startX, py = y - m.startY;
  if (m.source == Source::RadialRamp) {
    const double radius = std::hypot(dx, dy);
    return radius > 0 ? detail::unit(std::hypot(px, py) / radius) : 1.0;
  }
  const double squared = dx * dx + dy * dy;
  return squared > 0 ? detail::unit((px * dx + py * dy) / squared) : 1.0;
}

namespace detail {

// A dot's properties before they are clamped
struct Look {
  double size, aspect, rotation, opacity;
  double presence = 1;  // the product of the control images' alphas
  Rgba color;
};

inline Look look(const Settings &s, int i, int j) {
  Look l;
  l.size   = s.size * (1 - unit(s.sizeRandom) * random(s.seed, i, j, Size));
  l.aspect =
      s.aspect * (1 - unit(s.aspectRandom) * random(s.seed, i, j, Aspect));
  l.rotation = s.rotation + 2 * pi * unit(s.rotationRandom) *
                                (random(s.seed, i, j, Rotation) - 0.5);
  l.opacity =
      s.opacity * (1 - unit(s.opacityRandom) * random(s.seed, i, j, Opacity));
  const double blend = unit(s.colorRandom) * random(s.seed, i, j, Colour);
  l.color.r = s.colorA.r + (s.colorB.r - s.colorA.r) * blend;
  l.color.g = s.colorA.g + (s.colorB.g - s.colorA.g) * blend;
  l.color.b = s.colorA.b + (s.colorB.b - s.colorA.b) * blend;
  l.color.a = s.colorA.a + (s.colorB.a - s.colorA.a) * blend;
  return l;
}

inline double *property(Look &l, Target target) {
  switch (target) {
  case Target::Size:
    return &l.size;
  case Target::Aspect:
    return &l.aspect;
  case Target::Rotation:
    return &l.rotation;
  case Target::Opacity:
    return &l.opacity;
  case Target::Colour:
  case Target::None:
    break;
  }
  return nullptr;
}

// Change a dot by a modifier whose control image is c at the dot's centre
inline void modify(Look &l, const Modifier &m, const Rgba &c) {
  l.presence *= unit(c.a);
  if (double *value = property(l, m.target)) {
    const double st = strength(c);
    *value          = apply(m, *value, m.invert ? 1 - st : st);
  } else if (m.target == Target::Colour && c.a > 1e-9) {
    const double share = unit(m.high);
    l.color.r += (unit(c.r / c.a) - l.color.r) * share;
    l.color.g += (unit(c.g / c.a) - l.color.g) * share;
    l.color.b += (unit(c.b / c.a) - l.color.b) * share;
  }
}

// The dot of cell (i, j); control is as in build()
template <class Control>
Dot place(const Grid &g, int i, int j, Control &control) {
  const Settings &s   = g.settings;
  const double jitter = unit(s.jitter);
  const double shift  = (j & 1) ? s.stagger - std::floor(s.stagger) : 0.0;
  Dot dot;
  dot.column = i + shift + (random(s.seed, i, j, JitterX) - 0.5) * jitter;
  const double v = j + (random(s.seed, i, j, JitterY) - 0.5) * jitter;
  dot.x = (dot.column * g.cosine - v * g.sine) * s.spacing;
  dot.y = (dot.column * g.sine + v * g.cosine) * s.spacing;

  Look l = look(s, i, j);
  for (int k = 0; k < modifierCount; ++k) {
    const Modifier &m = s.modifiers[k];
    if (m.target == Target::None) continue;
    if (m.source == Source::Input) {
      modify(l, m, control(k, dot.x, dot.y));
    } else {
      const double grey = rampStrength(m, dot.x, dot.y);
      modify(l, m, Rgba{grey, grey, grey, 1});
    }
  }
  dot.radius = 0.5 * s.spacing * std::max(0.0, std::min(maxSize, l.size));
  dot.aspect = std::max(minAspect, std::min(maxAspect, l.aspect));
  dot.cosine  = std::cos(s.angle + l.rotation);
  dot.sine    = std::sin(s.angle + l.rotation);
  dot.color   = l.color;
  dot.color.a = unit(l.color.a) * unit(l.opacity) * l.presence;
  return dot;
}

}  // namespace detail

// The dots that can touch the rectangle. control(k, x, y) returns the
// premultiplied colour of control image k at a point; it is called only for
// modifiers that have a target and take their source from the input.
template <class Control>
Grid build(const Settings &settings, double minX, double minY, double maxX,
           double maxY, Control &&control) {
  Grid g;
  g.settings        = settings;
  const Settings &s = g.settings;
  // The rectangle in lattice cells is grown by what a dot can reach
  const double grow = reach(s) / s.spacing;
  g.cosine          = std::cos(s.angle);
  g.sine            = std::sin(s.angle);
  if (!(maxX >= minX) || !(maxY >= minY)) return g;

  double uMin = 0, uMax = 0, vMin = 0, vMax = 0;
  bool first = true;
  for (double x : {minX, maxX})
    for (double y : {minY, maxY}) {
      const double u = (x * g.cosine + y * g.sine) / s.spacing;
      const double v = (-x * g.sine + y * g.cosine) / s.spacing;
      uMin = first ? u : std::min(uMin, u);
      uMax = first ? u : std::max(uMax, u);
      vMin = first ? v : std::min(vMin, v);
      vMax = first ? v : std::max(vMax, v);
      first = false;
    }
  const double limit = 1e9;
  if (!(uMin - grow > -limit && uMax + grow < limit && vMin - grow > -limit &&
        vMax + grow < limit))
    throw std::length_error("Dot Screen: the region is too large");
  g.i0      = int(std::floor(uMin - grow));
  g.j0      = int(std::floor(vMin - grow));
  g.columns = int(std::ceil(uMax + grow)) - g.i0 + 1;
  g.rows    = int(std::ceil(vMax + grow)) - g.j0 + 1;
  if (std::size_t(g.columns) > maxDots / std::size_t(g.rows))
    throw std::length_error("Dot Screen: too many dots; raise the spacing");
  g.dots.resize(std::size_t(g.columns) * std::size_t(g.rows));

  for (int row = 0; row < g.rows; ++row)
    for (int col = 0; col < g.columns; ++col) {
      Dot &dot = g.dots[std::size_t(row) * g.columns + col];
      dot      = detail::place(g, g.i0 + col, g.j0 + row, control);
      g.reach  = std::max(g.reach, extent(s, dot.radius, dot.aspect));
    }
  return g;
}

// Signed distance from a point in the dot's own frame to its edge
inline double distance(Shape shape, double x, double y, double radius) {
  const double ax = std::abs(x), ay = std::abs(y);
  switch (shape) {
  case Shape::Square:
    return std::max(ax, ay) - radius;
  case Shape::Diamond:
    return (ax + ay) * 0.70710678118654752 - radius;
  case Shape::Line:
    return ay - radius;
  case Shape::Round:
    break;
  }
  return std::hypot(x, y) - radius;
}

// The premultiplied colour of the screen at (x, y)
inline Rgba sample(const Grid &g, double x, double y) {
  Rgba out;
  if (g.dots.empty()) return out;
  const Settings &s  = g.settings;
  const double u     = (x * g.cosine + y * g.sine) / s.spacing;
  const double v     = (-x * g.sine + y * g.cosine) / s.spacing;
  const double edge  = s.pixel + s.softness;
  const int around   = int(std::ceil(g.reach / s.spacing + 1.0));
  const int rowMid   = int(std::floor(v + 0.5)) - g.j0;
  const int colMid   = int(std::floor(u + 0.5)) - g.i0;
  const int rowFirst = std::max(0, rowMid - around);
  const int rowLast  = std::min(g.rows - 1, rowMid + around);
  const int colFirst = std::max(0, colMid - around - 1);
  const int colLast  = std::min(g.columns - 1, colMid + around + 1);
  for (int row = rowFirst; row <= rowLast; ++row)
    for (int col = colFirst; col <= colLast; ++col) {
      const Dot &dot = g.dots[std::size_t(row) * g.columns + col];
      if (!(dot.radius > 0) || !(dot.color.a > 0)) continue;
      const double dx = x - dot.x, dy = y - dot.y;
      const double along = dx * dot.cosine + dy * dot.sine;
      // A line is cut half a cell either side of its centre, so a row of
      // lines along it joins into a stripe without overlapping
      if (s.shape == Shape::Line && std::abs(along) > 0.5 * s.spacing)
        continue;
      const double ly = -dx * dot.sine + dy * dot.cosine;
      const double cover = detail::unit(
          0.5 - distance(s.shape, along / dot.aspect, ly, dot.radius) / edge);
      const double alpha = cover * dot.color.a;
      if (!(alpha > 0)) continue;
      out.r = dot.color.r * alpha + out.r * (1 - alpha);
      out.g = dot.color.g * alpha + out.g * (1 - alpha);
      out.b = dot.color.b * alpha + out.b * (1 - alpha);
      out.a = alpha + out.a * (1 - alpha);
    }
  return out;
}

}  // namespace DotScreen
