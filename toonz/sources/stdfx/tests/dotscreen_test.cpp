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
double coveredShare(const Settings &s, int cells = 4) {
  const Grid g      = plain(s, cells * s.spacing);
  const double side = cells * s.spacing, step = 0.05;
  double sum = 0;
  int count  = 0;
  for (double y = 0.5 * step; y < side; y += step)
    for (double x = 0.5 * step; x < side; x += step, ++count)
      sum += sample(g, x, y, step).a;
  return sum / count;
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
}

void testPlacement() {
  Settings s;
  s.spacing     = 20;
  s.size        = 0.5;
  const Grid g  = plain(s);
  require(approx(sample(g, 0, 0, 1).a, 1) && approx(sample(g, 20, 40, 1).a, 1),
          "Dots are not on the lattice");
  require(approx(sample(g, 10, 10, 1).a, 0), "A cell corner is not empty");
  // Half a cell of stagger moves odd rows only
  s.stagger         = 0.5;
  const Grid offset = plain(s);
  require(approx(sample(offset, 0, 0, 1).a, 1) &&
              approx(sample(offset, 10, 20, 1).a, 1) &&
              approx(sample(offset, 0, 20, 1).a, 0),
          "Stagger did not shift the odd rows by half a cell");
  // A turned lattice is the same lattice, turned
  s.stagger         = 0;
  s.angle           = pi / 6;
  const Grid turned = plain(s);
  const double c = std::cos(s.angle), sn = std::sin(s.angle);
  const double points[4][2] = {{20, 0}, {20, 40}, {7, 3}, {10, 10}};
  for (const auto &point : points)
    require(approx(sample(turned, point[0] * c - point[1] * sn,
                          point[0] * sn + point[1] * c, 1)
                       .a,
                   sample(g, point[0], point[1], 1).a, 1e-9),
            "The angle did not turn the lattice about the origin");
}

void testTiling() {
  Settings s;
  s.spacing        = 9;
  s.size           = 1.4;
  s.angle          = 0.4;
  s.jitter         = 0.8;
  s.sizeRandom     = 0.6;
  s.rotationRandom = 1;
  s.opacityRandom  = 0.5;
  s.colorRandom    = 1;
  s.colorB         = Rgba{1, 0, 0, 1};
  s.shape          = Shape::Diamond;
  s.aspect         = 2;
  s.modifiers[0].target = Target::Size;
  const Grid whole = build(s, -80, -80, 80, 80, ramp);
  const Grid part  = build(s, 3, -11, 31, 12, ramp);
  for (double y = -11; y <= 12; y += 0.37)
    for (double x = 3; x <= 31; x += 0.41) {
      const Rgba a = sample(whole, x, y, 1), b = sample(part, x, y, 1);
      require(a.r == b.r && a.g == b.g && a.b == b.b && a.a == b.a,
              "A smaller region gives a different picture");
    }
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
  require(approx(plain(s).dots[0].color.a, 0.2), "Set did not replace");
  s.modifiers[1].mode = Mode::Offset;
  require(approx(plain(s).dots[0].color.a, 0.7), "Offset did not add");

  // Colour takes the control image's colour, not its strength
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
              approx(tinted.dots[0].color.a, 1),
          "The colour modifier did not take the control image's colour");
}

// One dot three cells wide: it reaches into the cells around it, and into a
// region that does not hold its centre
void testLargeDot() {
  Settings s;
  s.spacing             = 10;
  s.size                = 3;  // radius 15
  s.modifiers[0].target = Target::Opacity;
  s.modifiers[0].mode   = Mode::Set;
  auto onlyOrigin       = [](int, double x, double y) {
    const double v = std::abs(x) < 1 && std::abs(y) < 1 ? 1.0 : 0.0;
    return Rgba{v, v, v, 1};
  };
  const Grid g = build(s, -40, -40, 40, 40, onlyOrigin);
  require(approx(sample(g, 0, 14, 0.1).a, 1) &&
              approx(sample(g, 14, 0, 0.1).a, 1) &&
              approx(sample(g, -10, -10, 0.1).a, 1),
          "A large dot stops at its own cell");
  require(approx(sample(g, 0, 16, 0.1).a, 0), "A large dot is too large");
  const Grid away = build(s, 13, -2, 20, 2, onlyOrigin);
  require(approx(sample(away, 14, 0, 0.1).a, 1),
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
  s         = Settings();
  s.spacing = 1e-3;
  bool tooMany = false;
  try {
    build(s, -1e4, -1e4, 1e4, 1e4, none);
  } catch (const std::length_error &) {
    tooMany = true;
  }
  require(tooMany, "An unbounded number of dots was accepted");
  s      = Settings();
  s.size = 0;
  require(approx(sample(plain(s), 0, 0, 1).a, 0), "A dot without size drew");
}
}  // namespace

int main() {
  try {
    testDotArea();
    testPlacement();
    testTiling();
    testModifiers();
    testLargeDot();
    testRandomness();
    testInvalid();
    std::cout << "PASS: dot screen areas, lattice, tiling, modifiers, large dots, "
                 "randomness and limits\n";
    return 0;
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
  }
  return 1;
}
