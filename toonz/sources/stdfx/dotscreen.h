#pragma once

// Dot Screen: a lattice of dots whose size, aspect, rotation, opacity and
// colour can each be driven by a control image. A Grid holds the dots that
// can touch a region; sample() returns the premultiplied colour at a point.
// A dot depends only on its lattice cell and on the control images at the
// cell's centre, so any tiling of the frame gives the same picture.

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <vector>

namespace DotScreen {

const double pi           = 3.14159265358979323846;
const int modifierCount   = 3;
const double maxSize      = 3.0;  // a dot's diameter, in cells
const std::size_t maxDots = 4u * 1024u * 1024u;  // about 350 MB of dots

// Premultiplied when it is a pixel, straight when it is a setting
struct Rgba {
  double r = 0, g = 0, b = 0, a = 0;
};

enum class Shape { Round, Square, Diamond, Line };
enum class Target { None, Size, Aspect, Rotation, Opacity, Colour };
enum class Mode { Set, Scale, Offset };

// The control image's strength at a dot (its luminance times its alpha, 0 to
// 1) picks a value between low and high. That value replaces the property,
// multiplies it or is added to it. For Colour, high is the share of the
// control image's colour the dot takes.
struct Modifier {
  Target target = Target::None;
  Mode mode     = Mode::Scale;
  double low = 0, high = 1;
  bool invert = false;
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

  Rgba colorA{0, 0, 0, 1}, colorB{0, 0, 0, 1};
  double colorRandom = 0, sizeRandom = 0, rotationRandom = 0,
         opacityRandom = 0;
  std::uint32_t seed   = 1;

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
  double reach = 0;       // the farthest any dot extends from its centre
  double cosine = 1, sine = 0;
};

enum Property {
  JitterX = 1,
  JitterY,
  Size,
  Rotation,
  Opacity,
  Colour
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
inline double strength(const Rgba &c) {
  return unit(0.2126 * c.r + 0.7152 * c.g + 0.0722 * c.b);
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
            finite(s.softness) && s.softness >= 0 && finite(s.colorRandom) &&
            finite(s.sizeRandom) && finite(s.rotationRandom) &&
            finite(s.opacityRandom);
  for (const Modifier &m : s.modifiers)
    ok = ok && finite(m.low) && finite(m.high);
  for (const Rgba *c : {&s.colorA, &s.colorB})
    ok = ok && finite(c->r) && finite(c->g) && finite(c->b) && finite(c->a);
  if (!ok) throw std::invalid_argument("Dot Screen: invalid settings");
}

// How far from a point the control images and the lattice are read: the
// largest dot, a cell of jitter and stagger, and the edge softness
inline double reach(const Settings &s) {
  validate(s);
  return s.spacing * (0.5 * maxSize * 4.0 + 2.0) + s.softness;
}

// The dots that can touch the rectangle. control(k, x, y) returns the
// premultiplied colour of control image k at a point; it is called only for
// modifiers that have a target.
template <class Control>
Grid build(const Settings &settings, double minX, double minY, double maxX,
           double maxY, Control &&control) {
  validate(settings);
  using detail::unit;
  Grid g;
  g.settings        = settings;
  const Settings &s = g.settings;
  g.cosine          = std::cos(s.angle);
  g.sine            = std::sin(s.angle);
  if (!(maxX >= minX) || !(maxY >= minY)) return g;

  // The rectangle in lattice cells, grown by what a dot can reach
  const double grow = reach(s) / s.spacing;
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
  if (!(uMin > -limit && uMax < limit && vMin > -limit && vMax < limit))
    throw std::length_error("Dot Screen: the region is too large");
  g.i0      = int(std::floor(uMin - grow));
  g.j0      = int(std::floor(vMin - grow));
  g.columns = int(std::ceil(uMax + grow)) - g.i0 + 1;
  g.rows    = int(std::ceil(vMax + grow)) - g.j0 + 1;
  if (std::size_t(g.columns) > maxDots / std::size_t(g.rows))
    throw std::length_error("Dot Screen: too many dots; raise the spacing");
  g.dots.resize(std::size_t(g.columns) * std::size_t(g.rows));

  const double jitter = unit(s.jitter);
  for (int row = 0; row < g.rows; ++row) {
    const int j        = g.j0 + row;
    const double shift = (j & 1) ? s.stagger - std::floor(s.stagger) : 0.0;
    for (int col = 0; col < g.columns; ++col) {
      const int i = g.i0 + col;
      Dot &dot    = g.dots[std::size_t(row) * g.columns + col];
      dot.column  = i + shift + (random(s.seed, i, j, JitterX) - 0.5) * jitter;
      const double v = j + (random(s.seed, i, j, JitterY) - 0.5) * jitter;
      dot.x = (dot.column * g.cosine - v * g.sine) * s.spacing;
      dot.y = (dot.column * g.sine + v * g.cosine) * s.spacing;

      double size = s.size * (1 - unit(s.sizeRandom) * random(s.seed, i, j, Size));
      double aspect   = s.aspect;
      double rotation = s.rotation + 2 * pi * unit(s.rotationRandom) *
                                         (random(s.seed, i, j, Rotation) - 0.5);
      double opacity = s.opacity * (1 - unit(s.opacityRandom) *
                                            random(s.seed, i, j, Opacity));
      const double blend = unit(s.colorRandom) * random(s.seed, i, j, Colour);
      Rgba color;
      color.r = s.colorA.r + (s.colorB.r - s.colorA.r) * blend;
      color.g = s.colorA.g + (s.colorB.g - s.colorA.g) * blend;
      color.b = s.colorA.b + (s.colorB.b - s.colorA.b) * blend;
      color.a = s.colorA.a + (s.colorB.a - s.colorA.a) * blend;

      for (int k = 0; k < modifierCount; ++k) {
        const Modifier &m = s.modifiers[k];
        if (m.target == Target::None) continue;
        const Rgba c = control(k, dot.x, dot.y);
        double st    = detail::strength(c);
        if (m.invert) st = 1 - st;
        switch (m.target) {
        case Target::Size:
          size = detail::apply(m, size, st);
          break;
        case Target::Aspect:
          aspect = detail::apply(m, aspect, st);
          break;
        case Target::Rotation:
          rotation = detail::apply(m, rotation, st);
          break;
        case Target::Opacity:
          opacity = detail::apply(m, opacity, st);
          break;
        case Target::Colour:
          if (c.a > 1e-9) {
            const double share = unit(m.high);
            color.r += (unit(c.r / c.a) - color.r) * share;
            color.g += (unit(c.g / c.a) - color.g) * share;
            color.b += (unit(c.b / c.a) - color.b) * share;
          }
          break;
        case Target::None:
          break;
        }
      }
      dot.radius =
          0.5 * s.spacing * std::max(0.0, std::min(maxSize, size));
      dot.aspect  = std::max(0.05, std::min(4.0, aspect));
      dot.cosine  = std::cos(s.angle + rotation);
      dot.sine    = std::sin(s.angle + rotation);
      color.a     = unit(color.a) * unit(opacity);
      dot.color   = color;
      g.reach     = std::max(g.reach, dot.radius * std::max(1.0, dot.aspect));
    }
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

// The premultiplied colour of the screen at (x, y). pixel is the size of an
// output pixel in the same units, and sets the antialiasing.
inline Rgba sample(const Grid &g, double x, double y, double pixel) {
  Rgba out;
  if (g.dots.empty() || !(pixel > 0)) return out;
  const Settings &s  = g.settings;
  const double u     = (x * g.cosine + y * g.sine) / s.spacing;
  const double v     = (-x * g.sine + y * g.cosine) / s.spacing;
  const double edge  = pixel + s.softness;
  const int around   = int(std::ceil((g.reach + edge) / s.spacing + 1.0));
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
      // A line is cut where the next dot's line begins, so a row of lines
      // joins into a stripe without overlapping
      if (s.shape == Shape::Line && std::abs(u - dot.column) > 0.5) continue;
      const double dx = x - dot.x, dy = y - dot.y;
      const double lx = (dx * dot.cosine + dy * dot.sine) / dot.aspect;
      const double ly = -dx * dot.sine + dy * dot.cosine;
      const double cover =
          detail::unit(0.5 - distance(s.shape, lx, ly, dot.radius) / edge);
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
