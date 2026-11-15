#include "../comiclines.h"

#include <chrono>
#include <cmath>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {
using namespace ComicLines;

void require(bool value, const std::string &message) {
  if (!value) throw std::runtime_error(message);
}
bool approx(double a, double b, double tolerance = 1e-9) {
  return std::abs(a - b) <= tolerance;
}

// One straight line on the band's axis, from the origin along +x
Settings single() {
  Settings s;
  s.count          = 1;
  s.positionRandom = s.lengthRandom = s.startRandom = s.thicknessRandom = 0;
  s.inLength = s.outLength = 0;
  s.bandLength             = 400;
  s.bandWidth              = 200;
  s.length                 = 100;
  s.thickness              = 4;
  return s;
}
double alphaAt(const Settings &s, double x, double y, double pixel = 1) {
  return sample(build(s), x, y, pixel).a;
}

void testSeed() {
  Settings s;
  s.count      = 200;
  s.groupSize  = 4;
  s.attraction = 0.5;
  const Field a = build(s), b = build(s);
  require(a.lines.size() == 200 && b.lines.size() == 200, "Lines are missing");
  for (size_t i = 0; i < a.lines.size(); ++i)
    require(a.lines[i].position == b.lines[i].position &&
                a.lines[i].length == b.lines[i].length &&
                a.lines[i].index == b.lines[i].index,
            "One seed gave two different fields");
  s.seed        = 2;
  const Field c = build(s);
  bool differs  = false;
  for (size_t i = 0; i < a.lines.size(); ++i)
    differs = differs || a.lines[i].position != c.lines[i].position;
  require(differs, "Another seed gave the same field");
  for (size_t i = 1; i < a.lines.size(); ++i)
    require(a.lines[i - 1].position <= a.lines[i].position,
            "Lines are not sorted across the layout");
}

// Each property draws from its own stream
void testIndependentRandomness() {
  Settings s;
  s.count = 100;
  Settings wide = s;
  wide.thicknessRandom = 1;
  wide.colorRandom     = 1;
  wide.colorB          = Rgba{1, 0, 0, 1};
  const Field a = build(s), b = build(wide);
  bool thicknessChanged = false;
  for (size_t i = 0; i < a.lines.size(); ++i) {
    require(a.lines[i].index == b.lines[i].index &&
                a.lines[i].position == b.lines[i].position &&
                a.lines[i].length == b.lines[i].length &&
                a.lines[i].start == b.lines[i].start,
            "A thickness or colour amount moved or resized a line");
    thicknessChanged =
        thicknessChanged || a.lines[i].halfWidth != b.lines[i].halfWidth;
    require(b.lines[i].color.r >= 0 && b.lines[i].color.r < 1,
            "A colour left the range between its two ends");
  }
  require(thicknessChanged, "The thickness amount changed nothing");
  // A long line is not always a thick one: the streams are separate
  int equal = 0;
  for (int i = 0; i < 100; ++i) {
    const double length = random(1, i, Length);
    require(length >= 0 && length < 1, "A random value left [0, 1)");
    if (length == random(1, i, Thickness)) ++equal;
  }
  require(equal == 0, "Two properties share one random stream");
}

void testCoverage() {
  Settings s = single();
  require(approx(alphaAt(s, 50, 0), 1), "The line's centre is not opaque");
  require(approx(alphaAt(s, 50, 2), 0.5), "The line's edge is not half");
  require(approx(alphaAt(s, 50, 3), 0), "The line leaks past its width");
  s.thickness = 0.5;
  require(approx(alphaAt(s, 50, 0), 0.5), "A half-pixel line is not half");
  s.thickness = 0;
  require(approx(alphaAt(s, 50, 0), 0), "A line without width was drawn");
  s           = single();
  s.opacity   = 0.25;
  require(approx(alphaAt(s, 50, 0), 0.25), "Opacity is not applied");
}

void testProfile() {
  Settings s = single();
  s.thickness = 20;
  s.taper     = 1;
  // Half width is 10 at the start and 5 at the middle
  require(alphaAt(s, 10, 8) > 0.99, "The tapered start is too thin");
  require(approx(alphaAt(s, 50, 8), 0), "The taper did not thin the line");
  require(alphaAt(s, 50, 4) > 0.99, "The tapered middle is too thin");
  s          = single();
  s.thickness = 20;
  s.inLength  = 0.5;
  require(approx(alphaAt(s, 25, 7), 0), "The line did not swell in");
  require(alphaAt(s, 60, 7) > 0.99, "The swollen line is too thin");
}

void testTravel() {
  Settings s = single();
  require(alphaAt(s, 20, 0) > 0.99 && approx(alphaAt(s, 120, 0), 0),
          "The resting line is not where it starts");
  s.travel = 30;
  require(approx(alphaAt(s, 20, 0), 0) && alphaAt(s, 120, 0) > 0.99,
          "Travel did not move the line by its distance");
  s.travel = 350;  // wraps: [350, 400) and [0, 50)
  require(alphaAt(s, 380, 0) > 0.99 && alphaAt(s, 30, 0) > 0.99 &&
              approx(alphaAt(s, 200, 0), 0),
          "A travelling line did not wrap inside its band");
}

void testFrameSeed() {
  require(frameSeed(7, 0, 0) == 7 && frameSeed(7, 99, 0) == 7,
          "Without an interval the seed changed");
  require(frameSeed(7, 0, 3) == frameSeed(7, 2, 3),
          "The seed changed inside a hold");
  require(frameSeed(7, 2, 3) != frameSeed(7, 3, 3),
          "The seed did not change after a hold");
  require(frameSeed(7, 3, 3) == frameSeed(7, 5, 3),
          "The second hold is not held");
  require(frameSeed(7, 0, 3) != frameSeed(8, 0, 3),
          "Two seeds share a held seed");
}

void testGroups() {
  Settings s;
  s.count          = 12;
  s.groupSize      = 3;
  s.attraction     = 1;
  s.positionRandom = 0;
  s.bandWidth      = 400;
  const Field f    = build(s);
  // Four groups of three, every member on its group's centre
  for (size_t i = 0; i < f.lines.size(); ++i)
    require(approx(f.lines[i].position, (i / 3 + 0.5) * 100.0),
            "Full attraction did not stack a group");
  s.attraction  = 0;
  const Field g = build(s);
  for (size_t i = 1; i < g.lines.size(); ++i)
    require(approx(g.lines[i].position - g.lines[i - 1].position, 400.0 / 12),
            "Without attraction the lines are not evenly spread");
}

void testTile() {
  Settings s = single();
  require(approx(alphaAt(s, 450, 0), 0), "The band repeated without tiling");
  s.tile = true;
  require(alphaAt(s, 450, 0) > 0.99 && alphaAt(s, 50, 200) > 0.99 &&
              alphaAt(s, -350, -200) > 0.99,
          "A tiled band does not repeat along and across");
}

void testRadial() {
  Settings s = single();
  s.layout      = Layout::Radial;
  s.innerRadius = 50;
  s.outerRadius = 300;
  s.length      = 250;
  s.thickness   = 6;
  // One line, at half a turn: it runs along -x from the outer radius
  require(alphaAt(s, -200, 0) > 0.99, "The radial line is missing");
  require(approx(alphaAt(s, 200, 0), 0), "A radial line drew on the far side");
  require(approx(alphaAt(s, -20, 0), 0), "The centre hole is not empty");
  require(approx(alphaAt(s, -320, 0), 0), "A line passed the outer radius");
  require(approx(alphaAt(s, -200, 3), 0.5), "The radial edge is not half");
  s.falloff = 1;  // width scales with radius / outer
  require(alphaAt(s, -290, 2) > 0.9 && approx(alphaAt(s, -100, 2), 0),
          "Falloff did not thin the line towards the centre");
  s.falloff = 0;
  s.twist   = pi / 2;  // a quarter turn by the inner radius
  require(alphaAt(s, -299, 0) > 0.9 && approx(alphaAt(s, -60, 0), 0),
          "Twist did not turn the line away from its start");
}

// A twisted line keeps its thickness across itself
void testTwistWidth() {
  Settings s    = single();
  s.layout      = Layout::Radial;
  s.innerRadius = 100;
  s.outerRadius = 700;
  s.length      = 600;
  s.thickness   = 8;
  s.twist       = 2 * pi / 3;
  const Field f = build(s);
  const double tilt = s.twist / (s.outerRadius - s.innerRadius);
  for (double radius : {150.0, 400.0, 650.0}) {
    // The line's point and direction at this radius
    const double along = s.outerRadius - radius;
    const double angle = pi + s.twist * along / (s.outerRadius - s.innerRadius);
    const double px = radius * std::cos(angle), py = radius * std::sin(angle);
    // Inwards the angle grows: the tangent is (dr, r dtheta) = (-1, r tilt)
    const double tr = -1, tt = radius * tilt, tn = std::hypot(tr, tt);
    const double dx = (tr * std::cos(angle) - tt * std::sin(angle)) / tn;
    const double dy = (tr * std::sin(angle) + tt * std::cos(angle)) / tn;
    // Sum the coverage along the normal: the line's width
    const double step = 0.01;
    double width      = 0;
    for (double u = -12; u <= 12; u += step)
      width += sample(f, px - dy * u, py + dx * u, step).a * step;
    require(approx(width, 8, 0.05),
            "A twisted line is " + std::to_string(width) + " wide at radius " +
                std::to_string(radius));
  }
}

// A band narrower than the lines' reach still tiles without seams
void testNarrowTile() {
  Settings narrow       = single();
  narrow.tile           = true;
  narrow.bandWidth      = 2;  // the reach spans three bands
  narrow.bandLength     = 100;
  narrow.length         = 100;
  narrow.count          = 1;
  narrow.thickness      = 2;
  narrow.waveAmplitude  = 4;
  narrow.wavePeriod     = 50;
  Settings wide         = narrow;
  wide.bandWidth        = 4;
  wide.count            = 2;
  wide.originY          = 1;  // bands are centred: line it up with the narrow one
  const Field a = build(narrow), b = build(wide);
  for (double y = -30; y <= 30; y += 0.37)
    for (double x = 0; x <= 100; x += 1.3)
      require(approx(sample(a, x, y, 1).a, sample(b, x, y, 1).a, 1e-9),
              "A narrow tiled band has a seam");
}

// The ends are antialiased: a line's total does not depend on where it
// starts within a pixel
void testEnds() {
  for (double length : {10.3, 0.4})
  for (double shift : {0.0, 0.25, 0.5, 0.75}) {
    Settings s     = single();
    s.originX      = shift;
    s.length       = length;
    const Field f  = build(s);
    double total   = 0;
    for (int x = -5; x < 30; ++x) total += sample(f, x + 0.5, 0.5, 1).a;
    require(approx(total, length, 1e-9),
            "A line's ends are not antialiased (total " +
                std::to_string(total) + ")");
  }
}

// Many lines over one pixel: none is dropped
void testCrowd() {
  Settings s = single();
  s.count     = 300;
  s.groupSize = 300;
  s.attraction = 1;  // all on the axis
  s.opacity    = 0.01;
  const double alpha = alphaAt(s, 50, 0);
  require(approx(alpha, 1 - std::pow(0.99, 300), 1e-9),
          "Lines were dropped where many overlap");
}

// The densest field over 480 by 270 pixels: a bound that catches a cost per
// pixel that grows with the number of lines, not a benchmark
void testCost() {
  for (Layout layout : {Layout::Parallel, Layout::Radial}) {
    Settings s;
    s.layout      = layout;
    s.count       = maxLines;
    s.groupSize   = 8;
    s.attraction  = 0.9;
    s.bandLength  = 1920;
    s.bandWidth   = 1080;
    s.originX     = -960;
    s.tile        = true;
    s.innerRadius = 50;
    s.outerRadius = 1200;
    s.length      = 800;
    s.thickness   = 12;
    s.waveAmplitude = 6;
    const Field f = build(s);
    const auto start = std::chrono::steady_clock::now();
    double sum       = 0;
    for (int y = 0; y < 270; ++y)
      for (int x = 0; x < 480; ++x)
        sum += sample(f, x - 240 + 0.5, y - 135 + 0.5, 1).a;
    const double seconds = std::chrono::duration<double>(
                               std::chrono::steady_clock::now() - start)
                               .count();
    require(sum > 0, "The densest field drew nothing");
    require(seconds < 20, "A dense frame took " + std::to_string(seconds) +
                              " seconds to sample");
  }
}

void testInvalid() {
  auto rejects = [](const Settings &s) {
    try {
      build(s);
    } catch (const std::invalid_argument &) {
      return true;
    }
    return false;
  };
  Settings s;
  s.count = maxLines + 1;
  require(rejects(s), "Too many lines were accepted");
  s           = Settings();
  s.thickness = std::nan("");
  require(rejects(s), "A non-finite thickness was accepted");
  s           = Settings();
  s.groupSize = 0;
  require(rejects(s), "An empty group was accepted");
  s            = Settings();
  s.bandLength = 0;
  require(build(s).lines.empty(), "A band without length drew lines");
  s       = Settings();
  s.count = 0;
  require(approx(sample(build(s), 0, 0, 1).a, 0), "No lines drew something");
}
}  // namespace

int main() {
  try {
    testSeed();
    testIndependentRandomness();
    testCoverage();
    testProfile();
    testTravel();
    testFrameSeed();
    testGroups();
    testTile();
    testRadial();
    testTwistWidth();
    testNarrowTile();
    testEnds();
    testCrowd();
    testCost();
    testInvalid();
    std::cout << "PASS: comic line seeds, randomness, coverage, profile, "
                 "travel, holds, groups, tiling and radial layout\n";
    return 0;
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
  }
  return 1;
}
