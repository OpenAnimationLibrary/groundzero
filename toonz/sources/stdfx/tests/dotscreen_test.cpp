#include "../dotscreen.h"

#include <cmath>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {
using namespace DotScreen;

void require(bool value, const std::string &message) {
  if (!value) throw std::runtime_error(message);
}
bool approx(double a, double b, double tolerance = 1e-9) {
  return std::abs(a - b) <= tolerance;
}
double degrees(double value) { return value * pi / 180; }

Rgba none(int, double, double) { return Rgba(); }

// A control image that is white on the right and black on the left
Rgba ramp(int, double x, double) {
  const double v = std::max(0.0, std::min(1.0, (x + 100) / 200));
  return Rgba{v, v, v, 1};
}

Grid plain(const Settings &s, double half = 60) {
  return build(s, -half, -half, half, half, none);
}

// The share of a square of cells that the dots cover
double coveredShare(Settings s, int cells = 2) {
  const double side = cells * s.spacing, step = 0.05;
  s.pixel           = step;
  const Grid g      = plain(s, side);
  double sum        = 0;
  int count         = 0;
  for (double y = 0.5 * step; y < side; y += step)
    for (double x = 0.5 * step; x < side; x += step, ++count)
      sum += sample(g, x, y).a;
  return sum / count;
}

// How much of a point one dot covers, from its shape alone
double cover(const Grid &g, const Dot &dot, double x, double y) {
  const Settings &s   = g.settings;
  const double dx     = x - dot.x, dy = y - dot.y;
  const double along  = dx * dot.cosine + dy * dot.sine;
  const double across = -dx * dot.sine + dy * dot.cosine;
  if (s.shape == Shape::Line && std::abs(along) > 0.5 * s.spacing) return 0;
  return detail::unit(
      0.5 - distance(s.shape, along / dot.aspect, across, dot.radius) /
                (s.pixel + s.softness));
}

// Every dot of the grid drawn over a point, in the order sample() draws them
Rgba everyDot(const Grid &g, double x, double y) {
  Rgba out;
  for (const Dot &dot : g.dots) {
    if (!(dot.radius > 0) || !(dot.color.a > 0)) continue;
    const double alpha = cover(g, dot, x, y) * dot.color.a;
    if (!(alpha > 0)) continue;
    out.r = dot.color.r * alpha + out.r * (1 - alpha);
    out.g = dot.color.g * alpha + out.g * (1 - alpha);
    out.b = dot.color.b * alpha + out.b * (1 - alpha);
    out.a = alpha + out.a * (1 - alpha);
  }
  return out;
}

// The screen over a region as the FX draws a tile: the control images are
// read only within reach() of the region and are transparent beyond it
Grid tile(const Settings &s, double x0, double y0, double x1, double y1) {
  const double r = reach(s);
  return build(s, x0, y0, x1, y1, [&](int k, double x, double y) {
    const bool inside =
        x >= x0 - r && x <= x1 + r && y >= y0 - r && y <= y1 + r;
    return inside ? ramp(k, x, y) : Rgba();
  });
}

void requireSameTiles(const Settings &s, const std::string &what) {
  const Grid whole = tile(s, -80, -80, 80, 80);
  const Grid part  = tile(s, 3, -11, 31, 12);
  for (double y = -11; y <= 12; y += 0.37)
    for (double x = 3; x <= 31; x += 0.41) {
      const Rgba a = sample(whole, x, y), b = sample(part, x, y);
      require(a.r == b.r && a.g == b.g && a.b == b.b && a.a == b.a,
              "A smaller region gives a different picture: " + what);
    }
}

void testDotArea() {
  Settings s;
  s.spacing = 10;
  s.size    = 0.6;  // radius 3
  require(approx(coveredShare(s), pi * 9 / 100, 0.004),
          "A round dot does not cover its area");
  s.shape = Shape::Square;  // side 6
  require(approx(coveredShare(s), 36.0 / 100, 0.004),
          "A square dot does not cover its area");
  s.shape = Shape::Line;  // a stripe 6 wide, every 10
  require(approx(coveredShare(s), 0.6, 0.004),
          "Lines do not join into a stripe of their width");
  s.opacity = 0.5;
  require(approx(coveredShare(s), 0.3, 0.004),
          "Stripes overlap where two lines meet");
  // A turned line is one cell long; turned a quarter it joins the next
  // line of its column
  s.rotation = degrees(45);
  require(approx(coveredShare(s), 0.3, 0.004),
          "A turned line is not one cell long");
  s.rotation = degrees(90);
  require(approx(coveredShare(s), 0.3, 0.004),
          "Lines turned along the columns overlap");
}

void testPlacement() {
  Settings s;
  s.spacing     = 20;
  s.size        = 0.5;
  const Grid g  = plain(s);
  require(approx(sample(g, 0, 0).a, 1) && approx(sample(g, 20, 40).a, 1),
          "Dots are not on the lattice");
  require(approx(sample(g, 10, 10).a, 0), "A cell corner is not empty");
  // Half a cell of stagger moves odd rows only
  s.stagger         = 0.5;
  const Grid offset = plain(s);
  require(approx(sample(offset, 0, 0).a, 1) &&
              approx(sample(offset, 10, 20).a, 1) &&
              approx(sample(offset, 0, 20).a, 0),
          "Stagger did not shift the odd rows by half a cell");
  // A turned lattice is the same lattice, turned
  s.stagger         = 0;
  s.angle           = pi / 6;
  const Grid turned = plain(s);
  const double c = std::cos(s.angle), sn = std::sin(s.angle);
  const double points[4][2] = {{20, 0}, {20, 40}, {7, 3}, {10, 10}};
  for (const auto &point : points)
    require(approx(sample(turned, point[0] * c - point[1] * sn,
                          point[0] * sn + point[1] * c)
                       .a,
                   sample(g, point[0], point[1]).a, 1e-9),
            "The angle did not turn the lattice about the origin");
}

// sample() looks only at the dots near a point; it must find every dot
// that covers it
void testWindow() {
  auto compare = [](const Settings &s, const std::string &what) {
    const Grid g = build(s, -12, -12, 12, 12, ramp);
    for (double y = -12; y <= 12; y += 0.43)
      for (double x = -12; x <= 12; x += 0.39) {
        const Rgba a = sample(g, x, y), b = everyDot(g, x, y);
        require(approx(a.a, b.a, 1e-12) && approx(a.r, b.r, 1e-12),
                "A dot that covers a point is not drawn: " + what);
      }
  };
  Settings line;
  line.spacing = 10;
  line.angle   = 0.3;
  line.shape   = Shape::Line;
  line.opacity = 0.5;
  for (double size : {0.4, 1.9, 3.0})
    for (double turn : {30.0, 60.0, 80.0, 90.0, 135.0}) {
      line.size     = size;
      line.rotation = degrees(turn);
      line.softness = size > 1 ? 3 : 0;
      compare(line, "a line turned " + std::to_string(int(turn)));
    }
  line.jitter         = 0.7;
  line.stagger        = 0.5;
  line.rotationRandom = 1;
  compare(line, "lines turned at random");

  Settings dot;
  dot.spacing        = 10;
  dot.rotation       = 0.6;
  dot.jitter         = 1;
  dot.stagger        = 0.9;
  dot.rotationRandom = 1;
  for (Shape shape : {Shape::Square, Shape::Diamond, Shape::Round})
    for (double size : {1.4, 3.0})
      for (double aspect : {0.25, 1.3, 4.0}) {
        dot.shape    = shape;
        dot.size     = size;
        dot.aspect   = aspect;
        dot.softness = aspect > 1 ? 6 : 0;
        compare(dot, "shape " + std::to_string(int(shape)) + ", aspect " +
                         std::to_string(aspect));
      }
}

void testTiling() {
  Settings s;
  s.spacing        = 9;
  s.size           = 1.4;
  s.angle          = 0.4;
  s.jitter         = 0.8;
  s.sizeRandom     = 0.6;
  s.aspectRandom   = 0.7;
  s.rotationRandom = 1;
  s.opacityRandom  = 0.5;
  s.colorRandom    = 1;
  s.colorB         = Rgba{1, 0, 0, 1};
  s.shape          = Shape::Diamond;
  s.aspect         = 2;
  s.modifiers[0].target = Target::Size;
  requireSameTiles(s, "diamonds");

  // Turned lines whose size follows the control image
  Settings line;
  line.spacing          = 10;
  line.shape            = Shape::Line;
  line.opacity          = 0.5;
  line.size             = 1.9;
  line.modifiers[0].target = Target::Size;
  for (double turn : {80.0, 90.0}) {
    line.rotation = degrees(turn);
    requireSameTiles(line, "lines turned " + std::to_string(int(turn)));
  }
  // Thin lines along the columns: the end of a line whose centre is three
  // units below the region reaches into it
  line.spacing = 7;
  line.size    = 0.2;
  requireSameTiles(line, "thin lines along the columns");
  line.spacing             = 10;
  line.size                = 1;
  line.modifiers[0].mode   = Mode::Set;
  line.modifiers[0].low    = 0;
  line.modifiers[0].high   = 1.9;
  requireSameTiles(line, "lines set to a size");

  // Dots that the modifiers make as large as they can be
  Settings large;
  large.spacing        = 8;
  large.angle          = 0.2;
  large.jitter         = 1;
  large.stagger        = 0.7;
  large.rotationRandom = 1;
  large.softness       = 2;
  large.modifiers[0]   = {Target::Size, Mode::Set, 0, 5, false};
  large.modifiers[1]   = {Target::Aspect, Mode::Scale, 1, 10, false};
  for (Shape shape :
       {Shape::Round, Shape::Square, Shape::Diamond, Shape::Line}) {
    large.shape = shape;
    requireSameTiles(large,
                     "largest dots of shape " + std::to_string(int(shape)));
  }
}

// How far from its centre a dot covers any point, found along rays
double measuredExtent(const Grid &g, const Dot &dot, double limit) {
  double farthest = 0;
  for (int k = 0; k < 180; ++k) {
    const double a = 2 * pi * k / 180, cx = std::cos(a), cy = std::sin(a);
    double in = 0, out = limit;
    if (cover(g, dot, dot.x + out * cx, dot.y + out * cy) > 0) return limit;
    for (int step = 0; step < 32; ++step) {
      const double mid = 0.5 * (in + out);
      (cover(g, dot, dot.x + mid * cx, dot.y + mid * cy) > 0 ? in : out) = mid;
    }
    farthest = std::max(farthest, in);
  }
  return farthest;
}

// reach() bounds every dot the modifiers can make, with the cells that
// jitter and stagger move it by; the grid's reach bounds its own dots
Grid requireBounded(const Settings &s, const std::string &what) {
  const Grid g       = build(s, -110, 0, 110, 0, ramp);
  const double bound = reach(s);
  for (int row = 0; row < g.rows; ++row)
    for (int col = 0; col < g.columns; ++col) {
      const Dot &dot = g.dots[std::size_t(row) * g.columns + col];
      if (!(dot.radius > 0)) continue;
      // How far the dot is from its cell, in cells
      const double u = dot.column - g.i0 - col;
      const double v =
          (-dot.x * g.sine + dot.y * g.cosine) / s.spacing - g.j0 - row;
      const double moved    = s.spacing * std::max(std::abs(u), std::abs(v));
      const double measured = measuredExtent(g, dot, 2 * bound);
      require(measured <= g.reach,
              "A dot reaches beyond its grid's reach: " + what);
      require(measured + moved <= bound,
              "A dot reaches beyond reach(): " + what);
    }
  return g;
}

// As requireBounded(), and the bound is met: a dot is as large as the
// modifiers allow
void requireReach(const Settings &s, double size, double aspect,
                  const std::string &what) {
  const Grid g = requireBounded(s, what);
  bool largest = false;
  for (const Dot &dot : g.dots)
    largest = largest || (dot.radius > 0 &&
                          approx(dot.radius, 0.5 * s.spacing * size) &&
                          approx(dot.aspect, aspect));
  require(largest, "No dot is as large as the modifiers allow: " + what);
}

void testReach() {
  Settings s;
  s.spacing        = 10;
  s.angle          = 0.5;
  s.jitter         = 0.6;
  s.stagger        = 0.3;
  s.rotationRandom = 1;
  s.softness       = 2;
  s.pixel          = 0.5;
  // Set, Multiply and Add beyond the largest size and aspect
  Settings set     = s;
  set.modifiers[0] = {Target::Size, Mode::Set, 0, 5, false};
  set.modifiers[1] = {Target::Aspect, Mode::Set, 1, 9, false};
  Settings scale   = s;
  scale.size       = 1.5;
  scale.modifiers[0] = {Target::Size, Mode::Scale, 0, 4, false};
  scale.modifiers[2] = {Target::Aspect, Mode::Offset, 0, 6, false};
  // Two negative factors make a positive size; Invert puts 9 at white
  Settings negative     = s;
  negative.modifiers[0] = {Target::Size, Mode::Scale, -6, -6, false};
  negative.modifiers[1] = {Target::Size, Mode::Scale, 0, -1, false};
  negative.modifiers[2] = {Target::Aspect, Mode::Set, 9, 1, true};
  // The FX's halftone: 1 at black, 0 at white
  Settings halftone     = s;
  halftone.modifiers[0] = {Target::Size, Mode::Scale, 1, 0, false};
  // Aspect randomness narrows a dot, which a negative factor turns into the
  // widest one: aspects from 1.5 up to nearly 3.5
  Settings flipped     = s;
  flipped.aspectRandom = 1;
  flipped.modifiers[0] = {Target::Aspect, Mode::Scale, -2, -2, false};
  flipped.modifiers[1] = {Target::Aspect, Mode::Offset, 3.5, 3.5, false};
  for (Shape shape :
       {Shape::Round, Shape::Square, Shape::Diamond, Shape::Line}) {
    const std::string name = "shape " + std::to_string(int(shape));
    for (Settings *t : {&set, &scale, &negative, &halftone, &flipped})
      t->shape = shape;
    requireReach(set, maxSize, maxAspect, name + ", Set");
    requireReach(scale, maxSize, maxAspect, name + ", Multiply and Add");
    requireReach(negative, maxSize, maxAspect, name + ", negative factors");
    requireReach(halftone, halftone.size, 1, name + ", halftone");
    const Grid wide =
        requireBounded(flipped, name + ", random aspect, negative factor");
    double widest = 0;
    for (const Dot &dot : wide.dots) widest = std::max(widest, dot.aspect);
    require(widest > 3, "Aspect randomness did not widen the flipped dots");
    // Randomness only narrows a dot, so it leaves the margin as it was
    Settings narrowed     = halftone;
    narrowed.aspectRandom = 1;
    require(reach(narrowed) == reach(halftone),
            "Aspect randomness grew reach(): " + name);
    // Dots that cannot grow are read over a smaller margin; a line is
    // always a cell long
    require(reach(halftone) <
                (shape == Shape::Line ? 0.6 : 0.25) * reach(set),
            "reach() does not follow the modifiers: " + name);
  }
  // Modifiers on other targets do not move the margin
  Settings turned     = halftone;
  turned.modifiers[1] = {Target::Rotation, Mode::Scale, 100, 1000, false};
  turned.modifiers[2] = {Target::Opacity, Mode::Offset, -5, 5, false};
  require(reach(turned) == reach(halftone),
          "A modifier that does not change a dot's size changed reach()");
}

void testModifiers() {
  Settings s;
  s.spacing = 10;
  s.size    = 1;
  s.modifiers[0].target = Target::Size;  // black gives no dot, white a full one
  const Grid g = build(s, -100, -20, 100, 20, ramp);
  double last  = -1;
  for (int i = -10; i <= 10; ++i) {
    const Dot &dot =
        g.dots[std::size_t(0 - g.j0) * g.columns + std::size_t(i - g.i0)];
    require(approx(dot.x, i * 10.0) && dot.radius >= last,
            "The size modifier does not grow with the control image");
    last = dot.radius;
    require(approx(dot.radius, 5.0 * (i * 10.0 + 100) / 200),
            "The size modifier did not scale the dot by the strength");
    require(dot.aspect == 1 && dot.color.a == 1 && dot.cosine == 1,
            "The size modifier changed another property");
  }
  s.modifiers[0].invert = true;
  const Grid inverse    = build(s, -100, -20, 100, 20, ramp);
  require(approx(inverse.dots[std::size_t(0 - inverse.j0) * inverse.columns +
                              std::size_t(-10 - inverse.i0)]
                     .radius,
                 5.0),
          "Invert did not turn black into a full dot");

  // Set replaces, Offset adds
  s                     = Settings();
  s.opacity             = 0.5;
  s.modifiers[1].target = Target::Opacity;
  s.modifiers[1].mode   = Mode::Set;
  s.modifiers[1].low    = 0.2;
  s.modifiers[1].high   = 0.2;
  auto white = [](int, double, double) { return Rgba{1, 1, 1, 1}; };
  require(approx(build(s, 0, 0, 1, 1, white).dots[0].color.a, 0.2),
          "Set did not replace");
  s.modifiers[1].mode = Mode::Offset;
  require(approx(build(s, 0, 0, 1, 1, white).dots[0].color.a, 0.7),
          "Offset did not add");

  // Colour takes the control image's colour, not its strength, and is as
  // opaque as the control image
  s                     = Settings();
  s.modifiers[2].target = Target::Colour;
  const Grid tinted     = build(s, -10, -10, 10, 10,
                                [](int k, double, double) {
                                  return k == 2 ? Rgba{0.1, 0.2, 0.05, 0.5}
                                                : Rgba{1, 1, 1, 1};
                                });
  require(approx(tinted.dots[0].color.r, 0.2) &&
              approx(tinted.dots[0].color.g, 0.4) &&
              approx(tinted.dots[0].color.b, 0.1) &&
              approx(tinted.dots[0].color.a, 0.5),
          "The colour modifier did not take the control image's colour");
}

// Where a modifier's control image is transparent there are no dots; where
// it is half transparent its straight colour drives the dot, at half opacity
void testTransparentControl() {
  Settings s;
  s.spacing      = 10;
  s.modifiers[0] = {Target::Size, Mode::Scale, 1, 0, false};  // the halftone
  s.modifiers[1] = {Target::Opacity, Mode::Set, 1, 1, false};
  auto cel       = [](int k, double x, double) {
    return k == 0 && x > 0 ? Rgba() : Rgba{0, 0, 0, 1};
  };
  for (bool invert : {false, true}) {
    s.modifiers[0].invert = invert;
    const Grid g          = build(s, -30, -10, 30, 10, cel);
    for (const Dot &dot : g.dots) {
      if (dot.x > 1)
        require(dot.color.a == 0, "A transparent control area has dots");
      if (dot.x < -1)
        require(dot.color.a == 1 &&
                    approx(dot.radius, invert ? 0 : 0.5 * 10 * s.size),
                "An opaque control area does not give the usual dot");
    }
    require(sample(g, 20, 0).a == 0 && sample(g, 20.5, 10.5).a == 0,
            "A transparent control area is drawn");
  }

  s.modifiers[0].invert = false;
  s.modifiers[1].target = Target::None;
  const Grid half       = build(s, -10, -10, 10, 10, [](int, double, double) {
    return Rgba{0.25, 0.25, 0.25, 0.5};  // straight mid grey
  });
  require(approx(half.dots[0].radius, 0.5 * 0.5 * 10 * s.size) &&
              approx(half.dots[0].color.a, 0.5),
          "A half transparent control area is not taken as its own colour, "
          "at half opacity");
}

// One dot three cells wide: it reaches into the cells around it, and into a
// region that does not hold its centre
void testLargeDot() {
  Settings s;
  s.spacing             = 10;
  s.size                = 3;  // radius 15
  s.pixel               = 0.1;
  s.modifiers[0].target = Target::Opacity;
  s.modifiers[0].mode   = Mode::Set;
  auto onlyOrigin       = [](int, double x, double y) {
    const double v = std::abs(x) < 1 && std::abs(y) < 1 ? 1.0 : 0.0;
    return Rgba{v, v, v, 1};
  };
  const Grid g = build(s, -40, -40, 40, 40, onlyOrigin);
  require(approx(sample(g, 0, 14).a, 1) && approx(sample(g, 14, 0).a, 1) &&
              approx(sample(g, -10, -10).a, 1),
          "A large dot stops at its own cell");
  require(approx(sample(g, 0, 16).a, 0), "A large dot is too large");
  const Grid away = build(s, 13, -2, 20, 2, onlyOrigin);
  require(approx(sample(away, 14, 0).a, 1),
          "A dot is missing from a region its centre is outside of");
}

void testRandomness() {
  Settings s;
  s.jitter      = 1;
  s.sizeRandom  = 1;
  const Grid a = plain(s), b = plain(s);
  bool moved   = false;
  for (std::size_t k = 0; k < a.dots.size(); ++k) {
    require(a.dots[k].x == b.dots[k].x && a.dots[k].radius == b.dots[k].radius,
            "One seed gave two different screens");
    require(a.dots[k].radius <= 0.5 * s.spacing * s.size + 1e-12,
            "Size randomness grew a dot");
  }
  s.seed       = 9;
  const Grid c = plain(s);
  for (std::size_t k = 0; k < a.dots.size(); ++k)
    moved = moved || a.dots[k].x != c.dots[k].x;
  require(moved, "Another seed gave the same screen");
}

// Aspect randomness narrows each dot by a share from its own random stream
// and leaves everything else, the other random streams included, as it was
void testAspectRandomness() {
  Settings s;
  s.spacing        = 10;
  s.aspect         = 2;
  s.jitter         = 1;
  s.stagger        = 0.5;
  s.sizeRandom     = 1;
  s.rotationRandom = 1;
  s.opacityRandom  = 1;
  s.colorRandom    = 1;
  s.colorB         = Rgba{1, 0.5, 0, 0.5};
  Settings narrowed     = s;
  narrowed.aspectRandom = 0.5;
  const Grid a = plain(s), b = plain(narrowed);
  require(a.dots.size() == b.dots.size() && a.i0 == b.i0 && a.j0 == b.j0,
          "Aspect randomness changed the lattice");
  bool changed = false;
  for (int row = 0; row < a.rows; ++row)
    for (int col = 0; col < a.columns; ++col) {
      const std::size_t k = std::size_t(row) * a.columns + col;
      const Dot &p = a.dots[k], &q = b.dots[k];
      require(p.x == q.x && p.y == q.y && p.radius == q.radius &&
                  p.cosine == q.cosine && p.sine == q.sine &&
                  p.color.r == q.color.r && p.color.g == q.color.g &&
                  p.color.b == q.color.b && p.color.a == q.color.a,
              "Aspect randomness changed another property");
      const double share = random(s.seed, a.i0 + col, a.j0 + row, Aspect);
      require(p.aspect == 2 && approx(q.aspect, 2 * (1 - 0.5 * share)),
              "Aspect randomness does not take its own share of the aspect");
      changed = changed || q.aspect < p.aspect - 0.1;
    }
  require(changed, "Aspect randomness changed no dot");
}

// A modifier whose source is a ramp from (x0, y0) to (x1, y1)
Modifier fromRamp(Source source, double x0, double y0, double x1, double y1) {
  Modifier m;
  m.source = source;
  m.startX = x0;
  m.startY = y0;
  m.endX   = x1;
  m.endY   = y1;
  return m;
}

// A ramp drives a modifier without a control image: black at its start
// point, white at its end point, opaque everywhere
void testRamps() {
  auto clamp  = [](double v) { return std::max(0.0, std::min(1.0, v)); };
  bool read   = false;
  auto unread = [&](int, double, double) {
    read = true;
    return Rgba();
  };
  // Dots of size 1 multiplied by the ramp: radius 0 at black, 5 at white
  auto requireRadii = [&](Modifier m, auto grey, const std::string &what) {
    Settings s;
    s.spacing      = 10;
    s.size         = 1;
    m.target       = Target::Size;
    s.modifiers[1] = m;
    const Grid g   = build(s, -120, -60, 120, 60, unread);
    require(!read, "A ramp read a control image: " + what);
    for (const Dot &dot : g.dots)
      require(approx(dot.radius, 5 * grey(dot.x, dot.y)) && dot.color.a == 1,
              "The dots do not follow the ramp: " + what);
  };
  requireRadii(fromRamp(Source::LinearRamp, -100, 0, 100, 0),
               [&](double x, double) { return clamp((x + 100) / 200); },
               "a level linear ramp");
  // Projected on the line from the start to the end, 30 by 40
  requireRadii(fromRamp(Source::LinearRamp, 10, 20, 40, 60),
               [&](double x, double y) {
                 return clamp(((x - 10) * 30 + (y - 20) * 40) / 2500);
               },
               "a slanted linear ramp");
  requireRadii(fromRamp(Source::RadialRamp, 10, 20, 40, 60),
               [&](double x, double y) {
                 return clamp(std::hypot(x - 10, y - 20) / 50);
               },
               "a radial ramp");
  auto white = [](double, double) { return 1.0; };
  requireRadii(fromRamp(Source::LinearRamp, 10, 20, 10, 20), white,
               "a linear ramp of no length");
  requireRadii(fromRamp(Source::RadialRamp, 10, 20, 10, 20), white,
               "a radial ramp of no length");
  // Black before the start, white beyond the end
  const Modifier level = fromRamp(Source::LinearRamp, -100, 0, 100, 0);
  const Modifier round = fromRamp(Source::RadialRamp, 0, 0, 30, 40);
  require(rampStrength(level, -300, 7) == 0 &&
              rampStrength(level, 300, -7) == 1 &&
              rampStrength(round, 0, 0) == 0 &&
              rampStrength(round, -60, 80) == 1,
          "A ramp leaves 0 to 1 beyond its ends");

  // Invert and Colour read a ramp as they read a grey control image
  Settings s;
  s.spacing             = 10;
  s.size                = 1;
  s.modifiers[0]        = fromRamp(Source::RadialRamp, 10, 20, 40, 60);
  s.modifiers[0].target = Target::Size;
  s.modifiers[0].invert = true;
  const Grid inverse    = build(s, 10, 20, 10, 20, unread);
  // The dot of cell (1, 2), at the ramp's centre
  const Dot &centre = inverse.dots[std::size_t(2 - inverse.j0) *
                                       inverse.columns +
                                   std::size_t(1 - inverse.i0)];
  require(approx(centre.x, 10) && approx(centre.y, 20) &&
              approx(centre.radius, 5),
          "Invert did not swap a ramp's ends");
  s.modifiers[0]        = fromRamp(Source::LinearRamp, -200, 0, 200, 0);
  s.modifiers[0].target = Target::Colour;  // all of the ramp's colour
  const Grid grey       = build(s, -250, -10, 250, 10, unread);
  for (const Dot &dot : grey.dots) {
    const double v = clamp((dot.x + 200) / 400);
    require(approx(dot.color.r, v) && approx(dot.color.g, v) &&
                approx(dot.color.b, v) && dot.color.a == 1,
            "A colour modifier did not take a ramp's grey");
  }
  require(!read, "A ramp read a control image");

  // Ramps belong to the scene, not to the region drawn
  Settings tiled;
  tiled.spacing             = 9;
  tiled.angle               = 0.4;
  tiled.jitter              = 0.8;
  tiled.shape               = Shape::Diamond;
  tiled.aspect              = 2;
  tiled.size                = 1.4;
  tiled.modifiers[0]        = fromRamp(Source::LinearRamp, -40, -30, 50, 20);
  tiled.modifiers[0].target = Target::Size;
  tiled.modifiers[0].mode   = Mode::Set;
  tiled.modifiers[0].high   = 1.9;
  tiled.modifiers[1]        = fromRamp(Source::RadialRamp, 20, 0, 20, 35);
  tiled.modifiers[1].target = Target::Rotation;
  tiled.modifiers[1].mode   = Mode::Offset;
  tiled.modifiers[1].high   = 2;
  // and a control image beside them
  tiled.modifiers[2] = {Target::Opacity, Mode::Set, 0.2, 1, false};
  requireSameTiles(tiled, "ramps");
}

void testInvalid() {
  auto rejects = [](const Settings &s) {
    try {
      plain(s);
    } catch (const std::invalid_argument &) {
      return true;
    }
    return false;
  };
  Settings s;
  s.spacing = 0;
  require(rejects(s), "No spacing was accepted");
  s      = Settings();
  s.size = std::nan("");
  require(rejects(s), "A non-finite size was accepted");
  s       = Settings();
  s.pixel = 0;
  require(rejects(s), "No pixel size was accepted");
  s         = Settings();
  s.spacing = 1e-3;
  bool tooMany = false;
  try {
    build(s, -1e4, -1e4, 1e4, 1e4, none);
  } catch (const std::length_error &) {
    tooMany = true;
  }
  require(tooMany, "An unbounded number of dots was accepted");
  s       = Settings();
  s.pixel = 1e12;
  bool tooLarge = false;
  try {
    plain(s);
  } catch (const std::length_error &) {
    tooLarge = true;
  }
  require(tooLarge, "A region grown beyond the lattice limits was accepted");
  s      = Settings();
  s.size = 0;
  require(approx(sample(plain(s), 0, 0).a, 0), "A dot without size drew");
}
}  // namespace

int main() {
  try {
    testDotArea();
    testPlacement();
    testWindow();
    testTiling();
    testReach();
    testModifiers();
    testTransparentControl();
    testLargeDot();
    testRandomness();
    testAspectRandomness();
    testRamps();
    testInvalid();
    std::cout << "PASS: dot screen areas, lattice, search window, tiling, "
                 "reach, modifiers, transparent controls, large dots, "
                 "randomness, aspect randomness, ramps and limits\n";
    return 0;
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
  }
  return 1;
}
