#include "../roughstroke.h"

#include <cmath>
#include <cstdint>
#include <iostream>
#include <sstream>
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

// The drawn stroke's opacity, here fading from 1 to 0 along it
void testOpacity() {
  Path faded              = line();
  faded.points[1].opacity = 0;
  const Image out         = render(faded, plain());
  // Pixel 35 is centred 25.5 along, pixel 85 75.5 along; a pixel takes the
  // colour of a vertex up to a step away
  require(approx(out.at(35, 20).a, 0.745, 0.015) &&
              approx(out.at(85, 20).a, 0.245, 0.015),
          "The stroke's opacity is not applied along it");
  require(approx(out.at(35, 20).r, 0.8 * out.at(35, 20).a),
          "The stroke's opacity changed its colour");
  Settings s       = plain();
  s.useStrokeColor = false;
  s.copies         = 3;
  s.copyOffset     = 4;
  for (const Copy &copy : build(faded, 0, s))
    require(approx(copy.vertices.back().color.a, 0) &&
                approx(copy.vertices.front().color.a, 1),
            "A copy in the gradient mode lost the stroke's opacity");
}

// The distance covered at a speed that changes from frame to frame
void testCovered() {
  // 0.25 a frame falling to 0 at frame 8, held before and after
  auto slowing = [](int f) {
    return f <= 0 ? 0.25 : f >= 8 ? 0.0 : 0.25 * (1 - f / 8.0);
  };
  require(approx(covered(slowing, 2.5, 0, 8), 0.25 + 0.21875 + 0.5 * 0.1875),
          "The distance is not the speeds of the frames passed");
  require(approx(covered(slowing, 10, 0, 8), 0.25 * (8 - 28 / 8.0)),
          "The distance after the last change is wrong");
  double before = covered(slowing, -6, 0, 8);
  for (double frame = -5.75; frame <= 20; frame += 0.25) {
    const double now = covered(slowing, frame, 0, 8);
    require(now >= before, "Slowing down moved the stretch back");
    before = now;
  }
  // A constant speed is speed * frame, before frame 0 too
  auto steady = [](int) { return 0.3; };
  for (double frame : {-7.5, -1.0, 0.0, 0.4, 12.25})
    require(approx(covered(steady, frame, 0, 0), 0.3 * frame),
            "A constant speed does not cover speed * frame");
  // Backwards from frame 0, at 1 a frame up to frame -4 and 0 from frame 0
  auto rising = [](int f) { return f <= -4 ? 1.0 : f >= 0 ? 0.0 : -f / 4.0; };
  require(approx(covered(rising, -2, -4, 0), -0.75) &&
              approx(covered(rising, -1.5, -4, 0), -0.5) &&
              approx(covered(rising, -9, -4, 0), -(1 + 0.75 + 0.5 + 0.25 + 5)),
          "The distance before frame 0 is wrong");
  require(covered(steady, std::nan(""), 0, 0) == 0,
          "A frame that is not finite covered a distance");
  bool rejected = false;
  try {
    covered(steady, 1, 1, 4);
  } catch (const std::invalid_argument &) {
    rejected = true;
  }
  require(rejected, "A frame range without frame 0 was accepted");
}

void testFilter() {
  // Straight (0.8, 0.1, 0.2) at alpha 0.5, through a half transparent red
  Rgba pixel{0.4, 0.05, 0.1, 0.5};
  filter(pixel, Rgba{1, 0, 0, 0.5});
  require(approx(pixel.r, 0.25) && approx(pixel.g, 0.025) &&
              approx(pixel.b, 0.05) && approx(pixel.a, 0.25),
          "The colour filter is not the render's");
  // Opaque black is no filter; a transparent pixel stays transparent
  Rgba same{0.4, 0.05, 0.1, 0.5};
  filter(same, Rgba{0, 0, 0, 1});
  require(approx(same.r, 0.4) && approx(same.g, 0.05) && approx(same.b, 0.1) &&
              approx(same.a, 0.5),
          "Opaque black changed the pixel");
  Rgba empty;
  filter(empty, Rgba{1, 1, 1, 1});
  require(empty.r == 0 && empty.a == 0, "A transparent pixel was filtered");
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

// Windows of several sizes hold the same pixels as the whole image
void requireTiles(const Path &path, const Settings &s,
                  const std::string &what) {
  const Image whole = render(path, s);
  for (int size : {7, 16, 37})
    for (int wy = 0; wy < whole.h; wy += size)
      for (int wx = 0; wx < whole.w; wx += size) {
        const Image part = render(path, s, size, size, wx, wy);
        for (int y = 0; y < size && wy + y < whole.h; ++y)
          for (int x = 0; x < size && wx + x < whole.w; ++x) {
            const Rgba &a = part.at(x, y), &b = whole.at(wx + x, wy + y);
            require(a.r == b.r && a.g == b.g && a.b == b.b && a.a == b.a,
                    what + ": a window differs from the whole image");
          }
      }
}

// A wavy path across the image, thicker towards its end
Path curve() {
  Path path;
  for (int i = 0; i <= 40; ++i)
    path.points.push_back(
        {10 + 2.5 * i, 24 + 12 * std::sin(i * 0.3), 0.5 + 0.02 * i});
  path.color = Rgba{0.8, 0.1, 0.2, 1};
  return path;
}

double alongRow(const Image &image, int y, int x0, int x1) {
  double sum = 0;
  for (int x = x0; x <= x1; ++x) sum += image.at(x, y).a;
  return sum;
}
double alongColumn(const Image &image, int x) {
  double sum = 0;
  for (int y = 0; y < image.h; ++y) sum += image.at(x, y).a;
  return sum;
}

void testDashes() {
  Settings s   = plain();
  s.dashLength = 10;
  s.dashGap    = 10;
  const auto dashes = build(line(), 0, s);
  require(dashes.size() == 5,
          "100 pixels in dashes of 10 and gaps of 10 are not 5 dashes");
  for (std::size_t k = 0; k < dashes.size(); ++k)
    require(approx(dashes[k].vertices.front().x, 10 + 20.0 * k) &&
                approx(dashes[k].vertices.back().x, 20 + 20.0 * k),
            "A dash is not where the pattern puts it");
  // Along its middle a dash covers its length and its round ends; across,
  // the width
  const Image out = render(line(), s);
  require(approx(alongRow(out, 20, 0, out.w - 1), 5 * (10 + 4)),
          "The dashes do not cover their lengths and round ends");
  require(approx(alongColumn(out, 15), 4), "A dash is not the stroke's width");
  require(approx(out.at(25, 20).a, 0), "A gap was drawn");

  // The phase moves the pattern along: from 15, the ends cut two dashes
  s.dashPhase       = 15;
  const auto moved  = build(line(), 0, s);
  auto length       = [](const Copy &run) {
    return run.vertices.back().x - run.vertices.front().x;
  };
  require(moved.size() == 6 && approx(length(moved.front()), 5) &&
              approx(length(moved.back()), 5) &&
              approx(moved[1].vertices.front().x, 25),
          "The phase did not move the dashes along");
  // A whole period, either way, is no move
  for (double phase : {-5.0, 35.0}) {
    s.dashPhase      = phase;
    const auto same  = build(line(), 0, s);
    require(same.size() == moved.size(), "A whole period changed the dashes");
    for (std::size_t k = 0; k < same.size(); ++k)
      require(approx(same[k].vertices.front().x, moved[k].vertices.front().x,
                     1e-9) &&
                  approx(same[k].vertices.back().x,
                         moved[k].vertices.back().x, 1e-9),
              "A whole period moved the dashes");
  }

  // The dashes stay on the path while the revealed stretch, here from 25
  // to 75 along it, travels over them
  s.dashPhase         = 0;
  s.revealEnd         = 0.5;
  s.travel            = 0.25;
  const auto revealed = build(line(), 0, s);
  require(revealed.size() == 3 &&
              approx(revealed[0].vertices.front().x, 35) &&
              approx(revealed[0].vertices.back().x, 40) &&
              approx(revealed[1].vertices.front().x, 50),
          "The dashes moved with the revealed stretch");

  // Every copy has the dashes; without a gap the stroke is solid
  s        = plain();
  s.copies = 3;
  s.copyOffset = 2;
  s.dashLength = 10;
  s.dashGap    = 10;
  require(build(line(), 0, s).size() == 15, "A copy has no dashes");
  s.dashGap = 0;
  require(build(line(), 0, s).size() == 3, "Dashes without gaps were cut");

  s               = Settings();
  s.copies        = 3;
  s.copyOffset    = 2;
  s.waveAmplitude = 2;
  s.dashLength    = 6;
  s.dashGap       = 4;
  s.dashPhase     = 2;
  requireTiles(curve(), s, "Dashes");
}

// Dashes without length: round dots as wide as the stroke
void testDots() {
  Settings s   = plain();
  s.width      = 6;
  s.dashLength = 0;
  s.dashGap    = 20;
  Path path    = line();
  path.points[0].x += 0.5;  // a dot at 30.5, on the centre of pixel 30
  path.points[1].x += 0.5;
  const auto dots = build(path, 0, s);
  require(dots.size() == 6, "Not a dot every 20 pixels from end to end");
  for (const Copy &dot : dots)
    require(dot.vertices.size() >= 2 &&
                dot.vertices.front().x == dot.vertices.back().x &&
                dot.vertices.front().y == dot.vertices.back().y,
            "A dot is not in one place");
  const Image out = render(path, s);
  require(approx(alongRow(out, 20, 21, 40), 6) && approx(alongColumn(out, 30), 6),
          "A dot's diameter is not the width");
  // Round: the corner of the square around it is left out
  require(approx(out.at(33, 23).a, 0) && out.at(32, 22).a > 0.5,
          "A dot is not round");
}

void testWidthNoise() {
  Path path;  // 2000 pixels: 50 features of the noise
  path.points = {{0, 20.5, 0}, {2000, 20.5, 0}};
  Settings s       = plain();  // half width 2
  s.widthNoise     = 0.5;
  s.widthNoiseSize = 40;
  const auto runs  = build(path, 0, s);
  require(runs.size() == 1, "Width noise cut the stroke");
  const std::vector<Vertex> &v = runs[0].vertices;
  double low = 1e9, high = -1e9;
  for (std::size_t i = 0; i < v.size(); ++i) {
    require(v[i].half >= 2 * (1 - 0.5) - 1e-12 &&
                v[i].half <= 2 * (1 + 0.5) + 1e-12,
            "The half width left 1 +/- the noise times its own");
    low  = std::min(low, v[i].half);
    high = std::max(high, v[i].half);
    // Smooth: the noise changes by at most 3 / widthNoiseSize a pixel
    if (i > 0)
      require(std::abs(v[i].half - v[i - 1].half) <=
                  2 * 0.5 * 3 * (v[i].x - v[i - 1].x) / 40 + 1e-12,
              "The width jumps");
  }
  require(low < 2 * 0.7 && high > 2 * 1.3, "The width hardly varies");

  // Its own random values: the drift, the copies and their colours are
  // those without it, and it is not the drift
  Settings rough   = plain();
  rough.copies     = 4;
  rough.copyOffset = 3;
  rough.copyOpacityRandom = 0.5;
  rough.drift      = 2;
  rough.driftSize  = 40;
  Settings noisy   = rough;
  noisy.widthNoise = 0.5;
  noisy.widthNoiseSize = 40;
  const auto before = build(path, 0, rough), after = build(path, 0, noisy);
  bool notDrift = false;
  for (std::size_t c = 0; c < before.size(); ++c)
    for (std::size_t i = 0; i < before[c].vertices.size(); ++i) {
      const Vertex &a = before[c].vertices[i], &b = after[c].vertices[i];
      require(a.x == b.x && a.y == b.y && a.color.a == b.color.a,
              "Width noise changed other random values");
      // Copy 0 sits on the path: its drift is its offset from it
      if (c == 0)
        notDrift = notDrift || std::abs((b.half / a.half - 1) / 0.5 -
                                        (a.y - 20.5) / 2) > 0.1;
    }
  require(notDrift, "Width noise follows the drift");
  // Each copy and each path has its own
  bool copies = false, paths = false;
  const auto other = build(path, 1, noisy);
  for (std::size_t i = 0; i < after[0].vertices.size(); ++i) {
    copies = copies || after[0].vertices[i].half != after[1].vertices[i].half;
    paths  = paths || after[0].vertices[i].half != other[0].vertices[i].half;
  }
  require(copies && paths, "Copies or paths share their width noise");

  s.copies     = 3;
  s.copyOffset = 2;
  s.widthNoiseSize = 15;
  requireTiles(curve(), s, "Width noise");
  s = Settings();
  s.width      = 4;
  s.widthNoise = 0.5;
  require(approx(reach(s), 2 * 1.5 + 1), "The reach misses the width noise");
}

void testOvershoot() {
  Settings s  = plain();
  s.overshoot = 5;
  const auto runs = build(line(), 0, s);
  require(runs.size() == 1 && approx(runs[0].vertices.front().x, 5) &&
              approx(runs[0].vertices.back().x, 115) &&
              approx(runs[0].vertices.front().y, 20.5) &&
              approx(runs[0].vertices.back().y, 20.5),
          "Overshoot 5 does not reach 5 pixels past each end");
  const Image out = render(line(), s);
  require(approx(alongRow(out, 20, 0, out.w - 1), 110 + 4),
          "The overshoot is not drawn");
  // Along the path's tangent at each end; a repeated point does not turn it
  Path bent;
  bent.points     = {{10, 10, 0}, {10, 10, 0}, {50, 10, 0}, {50, 50, 0}};
  const auto ends = build(bent, 0, s);
  require(approx(ends[0].vertices.front().x, 5) &&
              approx(ends[0].vertices.front().y, 10) &&
              approx(ends[0].vertices.back().x, 50) &&
              approx(ends[0].vertices.back().y, 55),
          "The overshoot does not follow the path's tangents");
  // The reveal takes the copy with its overshoot: half of 110 pixels
  s.revealEnd = 0.5;
  require(approx(build(line(), 0, s)[0].vertices.back().x, 60),
          "The reveal left the overshoot out");
  s.revealEnd = 1;

  // Each end of each copy falls short by its own share, up to the random
  // amount; the copies keep their offsets
  s.copies          = 6;
  s.copyOffset      = 3;
  const auto even   = build(line(), 0, s);
  s.overshootRandom = 0.6;
  const auto varied = build(line(), 0, s);
  bool endsDiffer = false, copiesDiffer = false;
  for (std::size_t c = 0; c < varied.size(); ++c) {
    const double head = 10 - varied[c].vertices.front().x;
    const double tail = varied[c].vertices.back().x - 110;
    for (double past : {head, tail})
      require(past >= 5 * (1 - 0.6) - 1e-9 && past <= 5 + 1e-9,
              "An overshoot left its random range");
    require(varied[c].vertices.front().y == even[c].vertices.front().y,
            "Overshoot randomness moved a copy");
    endsDiffer = endsDiffer || std::abs(head - tail) > 0.1;
    copiesDiffer =
        copiesDiffer ||
        std::abs(head - (10 - varied[0].vertices.front().x)) > 0.1;
  }
  require(endsDiffer && copiesDiffer,
          "The ends or the copies share their overshoot");

  // Dashes go on from the path's start, not the overshoot's
  s                 = plain();
  s.overshoot       = 5;
  s.overshootRandom = 0.5;
  s.dashLength      = 10;
  s.dashGap         = 10;
  const auto dashed = build(line(), 0, s);
  require(dashed.size() == 6 && approx(dashed[0].vertices.front().x, 10) &&
              approx(dashed[0].vertices.back().x, 20) &&
              approx(dashed.back().vertices.front().x, 110),
          "The dashes moved with the overshoot");

  // Every new setting at once, in windows
  s                 = Settings();
  s.copies          = 3;
  s.copyOffset      = 2;
  s.waveAmplitude   = 2;
  s.drift           = 1;
  s.dashLength      = 6;
  s.dashGap         = 4;
  s.widthNoise      = 0.4;
  s.widthNoiseSize  = 15;
  s.overshoot       = 6;
  s.overshootRandom = 0.5;
  requireTiles(curve(), s, "Dashes, width noise and overshoot");
  s           = Settings();
  s.width     = 4;
  s.overshoot = 7;
  require(approx(reach(s), 2 + 7 + 1), "The reach misses the overshoot");
}

// The caller scales every distance: the dashes and widths scale with them
void testScale() {
  Path path;
  path.points = {{10, 20, 1}, {60, 45, 1.5}, {110, 30, 0.5}};
  Settings s   = plain();
  s.width      = 3;
  s.inLength   = 0.2;
  s.dashLength = 10;
  s.dashGap    = 6;
  s.dashPhase  = 3;
  s.widthNoise = 0.4;
  s.widthNoiseSize = 15;
  s.overshoot  = 4;
  s.overshootRandom = 0.5;
  Path doubled = path;
  for (Point &p : doubled.points) {
    p.x *= 2;
    p.y *= 2;
    p.thickness *= 2;
  }
  Settings twice   = s;
  twice.width      = 2 * s.width;
  twice.dashLength = 2 * s.dashLength;
  twice.dashGap    = 2 * s.dashGap;
  twice.dashPhase  = 2 * s.dashPhase;
  twice.widthNoiseSize = 2 * s.widthNoiseSize;
  twice.overshoot      = 2 * s.overshoot;
  const auto small = build(path, 0, s), large = build(doubled, 0, twice);
  require(small.size() == large.size(), "Scaling changed the dashes");
  for (std::size_t k = 0; k < small.size(); ++k)
    for (bool end : {false, true}) {
      const Vertex &a = end ? small[k].vertices.back() : small[k].vertices[0];
      const Vertex &b = end ? large[k].vertices.back() : large[k].vertices[0];
      require(approx(b.x, 2 * a.x, 1e-9) && approx(b.y, 2 * a.y, 1e-9) &&
                  approx(b.half, 2 * a.half, 1e-9),
              "A scaled dash did not scale");
    }
}

// FNV-1a over the channels rounded to 2^-20, so that last-bit differences
// between compilers and maths libraries leave it unchanged
std::uint64_t checksum(const std::vector<Rgba> &pixels) {
  std::uint64_t hash = 0xcbf29ce484222325ULL;
  for (const Rgba &p : pixels)
    for (double channel : {p.r, p.g, p.b, p.a}) {
      const std::uint64_t q = std::uint64_t(std::llround(channel * 1048576.0));
      for (int byte = 0; byte < 8; ++byte) {
        hash ^= (q >> (8 * byte)) & 0xffU;
        hash *= 0x100000001b3ULL;
      }
    }
  return hash;
}

std::string hex(std::uint64_t value) {
  std::ostringstream out;
  out << "0x" << std::hex << value;
  return out.str();
}

// A curve with changing thickness, a long diagonal and a corner, drawn into
// a window at (-3, 2). Colour and opacity hold along each path: where
// segments tie at full coverage a pixel takes the colour of the first one,
// and which one that is can change with the last bit of a coverage, so a
// colour that changes along a path would tie the checksum to the compiler.
std::uint64_t drawScene(const Settings &s) {
  Path curve;
  for (int i = 0; i <= 40; ++i)
    curve.points.push_back(
        {10 + 2.5 * i, 30 + 14 * std::sin(i * 0.3), 0.5 + 0.02 * i, 0.85});
  curve.color = Rgba{0.8, 0.1, 0.2, 1};
  Path diagonal;
  diagonal.points = {{5, 80, 1.5, 1}, {150, 12, 0.25, 1}};
  diagonal.color  = Rgba{0.1, 0.6, 0.3, 0.8};
  Path corner;
  corner.points = {{30, 70, 1, 0.7}, {120, 70, 1, 0.7}, {120, 20, 2, 0.7},
                   {60, 45, 0.5, 0.7}};
  corner.color = Rgba{0.2, 0.3, 0.9, 1};
  const std::vector<Path> paths = {curve, diagonal, corner};
  Image image(160, 96, -3, 2);
  for (std::size_t i = 0; i < paths.size(); ++i)
    for (const Copy &copy : build(paths[i], int(i), s))
      draw(copy, image.w, image.h, image.ox, image.oy, image.pixels);
  return checksum(image.pixels);
}

// Drawings recorded before dashes, width variation and overshoot, which
// leave them unchanged while off
void testChecksum() {
  const std::uint64_t plain = drawScene(Settings());
  require(plain == 0xec4f4b0f06a8dc23ULL,
          "The default drawing changed: checksum " + hex(plain));
  // Every other setting, except the colour gradient (see drawScene)
  Settings s;
  s.width             = 1.5;
  s.thicknessScale    = 1.5;
  s.inLength          = 0.2;
  s.outLength         = 0.3;
  s.copies            = 4;
  s.copyOffset        = 3;
  s.copyWidthRandom   = 0.5;
  s.copyOpacityRandom = 0.4;
  s.waveAmplitude     = 2;
  s.wavePeriod        = 25;
  s.wavePhase         = 0.7;
  s.wavePhaseRandom   = 0.5;
  s.drift             = 1.5;
  s.driftSize         = 20;
  s.opacity           = 0.9;
  s.revealStart       = 0.1;
  s.revealEnd         = 0.8;
  s.travel            = 0.35;  // the stretch crosses the end and loops
  s.loop              = true;
  s.step              = 1.25;
  s.seed              = 7;
  const std::uint64_t full = drawScene(s);
  require(full == 0xb2dfd7d3c22f3673ULL,
          "The drawing with every setting changed: checksum " + hex(full));
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
  s            = Settings();
  s.dashLength = -1;
  s.dashGap    = 1;
  require(rejects(s), "A negative dash length was accepted");
  s            = Settings();
  s.dashGap    = -1;
  require(rejects(s), "A negative gap was accepted");
  s           = Settings();
  s.dashGap   = 1;
  s.dashPhase = std::nan("");
  require(rejects(s), "A non-finite dash phase was accepted");
  s         = Settings();
  s.dashGap = 1e308;
  s.dashLength = 1e308;
  require(rejects(s), "A dash pattern beyond range was accepted");
  s                = Settings();
  s.widthNoiseSize = 0;
  require(rejects(s), "Width noise without a size was accepted");
  s            = Settings();
  s.widthNoise = std::nan("");
  require(rejects(s), "A non-finite width noise was accepted");
  s           = Settings();
  s.overshoot = -1;
  require(rejects(s), "A negative overshoot was accepted");
  s                 = Settings();
  s.overshootRandom = std::nan("");
  require(rejects(s), "A non-finite overshoot randomness was accepted");
  // Dots far closer than a pixel would be millions of runs
  s         = Settings();
  s.dashGap = 1e-9;
  bool tooMany = false;
  try {
    build(line(), 0, s);
  } catch (const std::length_error &) {
    tooMany = true;
  }
  require(tooMany, "Millions of dots were accepted");
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
    testOpacity();
    testCovered();
    testFilter();
    testTiling();
    testDashes();
    testDots();
    testWidthNoise();
    testOvershoot();
    testScale();
    testChecksum();
    testLimits();
    std::cout << "PASS: rough stroke width, reveal, travel, loop, swell, "
                 "copies, wave, drift, gradient, opacity, speed, filter, "
                 "tiling, dashes, dots, width noise, overshoot, scale, "
                 "recorded drawings and limits\n";
    return 0;
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
  }
  return 1;
}
