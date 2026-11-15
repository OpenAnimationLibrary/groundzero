#include "../roughstroke.h"

#include <cmath>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {
using namespace RoughStroke;

void require(bool value, const std::string &message) {
  if (!value) throw std::runtime_error(message);
}
bool approx(double a, double b, double tolerance = 1e-9) {
  return std::abs(a - b) <= tolerance;
}

// A straight path from (10, 20.5) to (110, 20.5): it runs through the
// centres of the pixels of row 20
Path line(double thickness = 0) {
  Path path;
  path.points = {{10, 20.5, thickness}, {110, 20.5, thickness}};
  path.color  = Rgba{0.8, 0.1, 0.2, 1};
  return path;
}

// A constant 4-pixel stroke, nothing random
Settings plain() {
  Settings s;
  s.width    = 4;
  s.inLength = s.outLength = 0;
  return s;
}

struct Image {
  int w, h;
  double ox, oy;
  std::vector<Rgba> pixels;
  Image(int w_, int h_, double ox_ = 0, double oy_ = 0)
      : w(w_), h(h_), ox(ox_), oy(oy_), pixels(std::size_t(w_) * h_) {}
  const Rgba &at(int x, int y) const { return pixels[std::size_t(y) * w + x]; }
};

Image render(const Path &path, const Settings &s, int w = 128, int h = 48,
             double ox = 0, double oy = 0, int index = 0) {
  Image image(w, h, ox, oy);
  for (const Copy &copy : build(path, index, s))
    draw(copy, w, h, ox, oy, image.pixels);
  return image;
}

void testWidth() {
  const Image out = render(line(), plain());
  require(approx(out.at(60, 20).a, 1) && approx(out.at(60, 21).a, 1),
          "The stroke's middle is not opaque");
  require(approx(out.at(60, 22).a, 0.5), "The stroke's edge is not half");
  require(approx(out.at(60, 23).a, 0), "The stroke is wider than asked");
  require(approx(out.at(60, 20).r, 0.8) && approx(out.at(60, 20).g, 0.1),
          "The stroke did not take the drawn colour");
  // The drawn stroke's own thickness, scaled, adds to the width
  Settings s       = plain();
  s.width          = 0;
  s.thicknessScale = 2;
  const Image drawn = render(line(1.5), s);  // half width 3
  require(approx(drawn.at(60, 22).a, 1) && approx(drawn.at(60, 23).a, 0.5) &&
              approx(drawn.at(60, 24).a, 0),
          "The drawn thickness was not scaled");
  s.thicknessScale = 0;
  require(approx(render(line(1.5), s).at(60, 20).a, 0),
          "A stroke without width was drawn");
}

void testReveal() {
  Settings s    = plain();
  s.revealEnd   = 0.5;  // x = 10 to 60
  const Image half = render(line(), s);
  require(approx(half.at(40, 20).a, 1), "The revealed half is missing");
  require(approx(half.at(70, 20).a, 0), "The hidden half was drawn");
  int covered = 0;
  for (int x = 0; x < 128; ++x)
    if (half.at(x, 20).a > 0.5) ++covered;
  require(covered >= 50 && covered <= 54,
          "Half a reveal does not cover half the length");
  // Travel moves the stretch; without Loop it leaves the path
  s.travel          = 0.25;  // x = 35 to 85
  const Image moved = render(line(), s);
  require(approx(moved.at(20, 20).a, 0) && approx(moved.at(80, 20).a, 1),
          "Travel did not move the revealed stretch");
  s.travel         = 0.75;  // x = 85 to 135: clipped at 110
  const Image left = render(line(), s);
  require(approx(left.at(100, 20).a, 1) && approx(left.at(20, 20).a, 0),
          "A stretch that left the path came back without Loop");
  s.loop              = true;  // x = 85 to 110, and 10 to 35
  const Image looped = render(line(), s);
  require(approx(looped.at(100, 20).a, 1) && approx(looped.at(20, 20).a, 1) &&
              approx(looped.at(60, 20).a, 0),
          "A looping stretch did not return at the other end");
  require(build(line(), 0, s).size() == 2,
          "A stretch across the end is not two runs");
  // More than a lap, or backwards, lands in the same place
  for (double travel : {1.75, -0.25, -3.25}) {
    s.travel         = travel;
    const Image laps = render(line(), s);
    for (int x = 0; x < 128; ++x)
      require(laps.at(x, 20).a == looped.at(x, 20).a,
              "A looping stretch is not the same after whole laps");
  }
}

void testProfile() {
  Settings s  = plain();
  s.width     = 12;
  s.inLength  = 0.5;
  s.outLength = 0;
  const Image out = render(line(), s);
  // Half width 3 a quarter of the way along, 6 from the middle on
  require(approx(out.at(35, 24).a, 0) && approx(out.at(35, 21).a, 1),
          "The stroke did not swell in");
  require(approx(out.at(85, 24).a, 1), "The swollen stroke is too thin");
  // The swell follows the stretch across a loop's seam
  s.revealEnd = 0.5;
  s.travel    = 0.75;
  s.loop      = true;
  const auto runs = build(line(), 0, s);
  require(runs.size() == 2 &&
              approx(runs[0].vertices.back().half,
                     runs[1].vertices.front().half, 1e-9),
          "The width jumps where a looping stretch wraps");
}

void testCopies() {
  Settings s   = plain();
  s.width      = 2;
  s.copies     = 6;
  s.copyOffset = 8;
  const auto copies = build(line(), 3, s);
  require(copies.size() == 6, "Copies are missing");
  require(approx(copies[0].vertices[0].y, 20.5),
          "The first copy left the path");
  bool spread = false;
  for (const Copy &copy : copies) {
    for (const Vertex &v : copy.vertices)
      require(std::abs(v.y - 20.5) <= 8 + 1e-9,
              "A copy is further from the path than the offset");
    spread = spread || std::abs(copy.vertices[0].y - 20.5) > 1;
  }
  require(spread, "The copies all sit on the path");
  // Width and opacity randomness leave the first copy as drawn
  s.copyWidthRandom = s.copyOpacityRandom = 1;
  const auto varied = build(line(), 3, s);
  require(approx(varied[0].vertices[5].half, copies[0].vertices[5].half) &&
              approx(varied[0].vertices[5].color.a, 1),
          "Copy randomness changed the first copy");
  bool thinner = false;
  for (std::size_t c = 1; c < varied.size(); ++c)
    thinner = thinner || varied[c].vertices[5].half < copies[c].vertices[5].half;
  require(thinner, "Copy width randomness changed no copy");
  // Another path with the same settings gets other offsets
  const auto other = build(line(), 4, s);
  bool differs     = false;
  for (std::size_t c = 1; c < 6; ++c)
    differs = differs || other[c].vertices[0].y != copies[c].vertices[0].y;
  require(differs, "Two paths share their copies' offsets");
  // The same path twice is the same
  const auto again = build(line(), 3, s);
  for (std::size_t c = 0; c < 6; ++c)
    require(again[c].vertices[0].y == copies[c].vertices[0].y,
            "One seed gave two different sets of copies");
}

void testWaveAndDrift() {
  Settings s        = plain();
  s.waveAmplitude   = 5;
  s.wavePeriod      = 40;
  s.wavePhaseRandom = 0;
  const auto wave   = build(line(), 0, s);
  double low = 1e9, high = -1e9;
  for (const Vertex &v : wave[0].vertices) {
    low  = std::min(low, v.y);
    high = std::max(high, v.y);
  }
  require(approx(low, 15.5, 0.05) && approx(high, 25.5, 0.05),
          "The wave does not reach its amplitude on both sides");
  s               = plain();
  s.drift         = 3;
  s.driftSize     = 10;
  const auto bent = build(line(), 0, s);
  low = 1e9, high = -1e9;
  for (const Vertex &v : bent[0].vertices) {
    require(std::abs(v.y - 20.5) <= 3 + 1e-9, "The drift left its amount");
    low  = std::min(low, v.y);
    high = std::max(high, v.y);
  }
  require(high - low > 1, "The drift is flat");
  require(approx(reach(s), 3 + 2 + 1), "The reach misses the drift");
}

void testGradient() {
  Settings s       = plain();
  s.useStrokeColor = false;
  s.startColor     = Rgba{1, 0, 0, 1};
  s.endColor       = Rgba{0, 0, 1, 1};
  const Image out  = render(line(), s);
  require(out.at(15, 20).r > 0.9 && out.at(105, 20).b > 0.9,
          "The gradient does not run from the start to the end colour");
  require(approx(out.at(60, 20).r, 0.5, 0.02) &&
              approx(out.at(60, 20).b, 0.5, 0.02),
          "The gradient's middle is not the mix of its ends");
  s.opacity = 0.5;
  require(approx(render(line(), s).at(60, 20).a, 0.5),
          "Opacity is not applied");
}

// A window of the image holds the same pixels as the whole image
void testTiling() {
  Path path;
  for (int i = 0; i <= 40; ++i)
    path.points.push_back(
        {10 + 2.5 * i, 24 + 12 * std::sin(i * 0.3), 0.5 + 0.02 * i});
  Settings s;
  s.copies          = 4;
  s.copyOffset      = 3;
  s.waveAmplitude   = 2;
  s.drift           = 1.5;
  s.copyWidthRandom = s.copyOpacityRandom = 0.5;
  const Image whole = render(path, s);
  const Image part  = render(path, s, 37, 19, 41, 13);
  for (int y = 0; y < 19; ++y)
    for (int x = 0; x < 37; ++x) {
      const Rgba &a = part.at(x, y), &b = whole.at(x + 41, y + 13);
      require(a.r == b.r && a.g == b.g && a.b == b.b && a.a == b.a,
              "A window differs from the whole image");
    }
}

void testLimits() {
  auto rejects = [](const Settings &s) {
    try {
      build(line(), 0, s);
    } catch (const std::invalid_argument &) {
      return true;
    }
    return false;
  };
  Settings s;
  s.copies = maxCopies + 1;
  require(rejects(s), "Too many copies were accepted");
  s      = Settings();
  s.step = 0;
  require(rejects(s), "No step was accepted");
  s       = Settings();
  s.width = std::nan("");
  require(rejects(s), "A non-finite width was accepted");
  Path dot;
  dot.points = {{5, 5, 1}, {5, 5, 1}};
  require(build(dot, 0, Settings()).empty(), "A path without length drew");
  Path one;
  one.points = {{5, 5, 1}};
  require(build(one, 0, Settings()).empty(), "A single point drew");
  s             = Settings();
  s.revealStart = s.revealEnd = 0.4;
  require(build(line(), 0, s).empty(), "An empty reveal drew");
  require(frameSeed(7, 0, 3) == frameSeed(7, 2, 3) &&
              frameSeed(7, 2, 3) != frameSeed(7, 3, 3) &&
              frameSeed(7, 9, 0) == 7,
          "The held seed does not follow its interval");
}
}  // namespace

int main() {
  try {
    testWidth();
    testReveal();
    testProfile();
    testCopies();
    testWaveAndDrift();
    testGradient();
    testTiling();
    testLimits();
    std::cout << "PASS: rough stroke width, reveal, travel, loop, swell, "
                 "copies, wave, drift, gradient, tiling and limits\n";
    return 0;
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
  }
  return 1;
}
