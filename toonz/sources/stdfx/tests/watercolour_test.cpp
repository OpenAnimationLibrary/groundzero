#include "../watercolour.h"

#include <cmath>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {
using namespace Watercolour;

void require(bool value, const std::string &message) {
  if (!value) throw std::runtime_error(message);
}
bool approx(double a, double b, double tolerance = 1e-9) {
  return std::abs(a - b) <= tolerance;
}
bool same(const Color &a, const Color &b) {
  return a.r == b.r && a.g == b.g && a.b == b.b && a.a == b.a;
}

const Color paint{0.6, 0.4, 0.2, 1}, clear{0, 0, 0, 0};

struct Image {
  int w, h;
  std::vector<Color> pixels;
  Image(int w_, int h_, Color c = clear)
      : w(w_), h(h_), pixels(std::size_t(w_) * h_, c) {}
  Color &at(int x, int y) { return pixels[std::size_t(y) * w + x]; }
  const Color &at(int x, int y) const { return pixels[std::size_t(y) * w + x]; }
};

// Paint on the left of x = edge, clear on the right
Image half(int w, int h, int edge) {
  Image image(w, h);
  for (int y = 0; y < h; ++y)
    for (int x = 0; x < edge; ++x) image.at(x, y) = paint;
  return image;
}

// Everything off: the image passes through
Settings plain() {
  Settings s;
  s.wobble = s.bleed = s.edgeDarkening = s.granulation = s.paperGrain = 0;
  s.paper                                                           = false;
  return s;
}

Image render(const Image &image, const Settings &s, double frame = 0,
             double ox = 0, double oy = 0) {
  Image out(image.w, image.h);
  require(Watercolour::render(image.pixels, image.w, image.h, ox, oy, s, frame,
                              out.pixels),
          "A render was canceled");
  return out;
}

void testPassThrough() {
  const Image image = half(40, 20, 17);
  const Image out   = render(image, plain());
  for (std::size_t i = 0; i < image.pixels.size(); ++i)
    require(same(out.pixels[i], image.pixels[i]),
            "With everything off the image changed");
}

void testBleed() {
  Settings s = plain();
  s.bleed    = 8;  // two passes of radius 4: colour reaches 8 pixels
  require(blurRadius(s) == 4, "The blur radius is not half the bleed");
  const Image out = render(half(80, 40, 40), s);
  const int y     = 20;
  // The last painted pixel is x = 39
  require(out.at(47, y).a > 0, "Colour did not reach the bleed distance");
  require(approx(out.at(48, y).a, 0), "Colour went past the bleed distance");
  require(approx(out.at(31, y).a, 1) && out.at(32, y).a < 1,
          "The wash thinned further in than the bleed distance");
  for (int x = 33; x <= 48; ++x)
    require(out.at(x, y).a <= out.at(x - 1, y).a,
            "The bleed does not fall off away from the paint");
  // Premultiplied: the colour falls with its alpha
  require(approx(out.at(44, y).r, paint.r * out.at(44, y).a, 1e-12),
          "The bled colour is not the paint's colour");
}

void testEdgeDarkening() {
  Settings s      = plain();
  s.edgeDarkening = 0.5;
  s.edgeWidth     = 4;
  const Image out = render(half(60, 30, 30), s);
  const int y     = 15;
  require(approx(out.at(29, y).r, 0.5 * paint.r),
          "The edge did not lose the asked share of its colour");
  require(approx(out.at(29, y).a, 1), "Edge darkening changed the alpha");
  require(approx(out.at(25, y).r, paint.r) && approx(out.at(10, y).r, paint.r),
          "The darkening reached past its width");
  for (int x = 26; x <= 29; ++x)
    require(out.at(x, y).r < out.at(x - 1, y).r,
            "The pigment does not gather towards the edge");
  // A small difference is not an edge
  Image flat(60, 30, paint);
  for (int x = 30; x < 60; ++x) flat.at(x, y) = {0.62, 0.4, 0.2, 1};
  require(approx(render(flat, s).at(29, y).r, paint.r),
          "A difference under the tolerance made an edge");
}

void testGranulation() {
  Settings s    = plain();
  s.granulation = 0.4;
  s.granuleSize = 6;
  const Image out = render(Image(64, 64, paint), s);
  double low = 1, high = 0;
  for (const Color &c : out.pixels) {
    require(c.r <= paint.r + 1e-12 && c.r >= 0.6 * paint.r - 1e-12,
            "Granulation left its range");
    require(approx(c.a, 1), "Granulation changed the alpha");
    low  = std::min(low, c.r);
    high = std::max(high, c.r);
  }
  require(high - low > 0.05, "Granulation is flat");
}

void testHold() {
  Settings s = plain();
  s.wobble   = 4;
  s.hold     = 2;
  const Image image = half(48, 48, 24);
  const Image f0 = render(image, s, 0), f1 = render(image, s, 1),
              f2 = render(image, s, 2);
  bool held = true, changed = false;
  for (std::size_t i = 0; i < f0.pixels.size(); ++i) {
    held    = held && same(f0.pixels[i], f1.pixels[i]);
    changed = changed || !same(f0.pixels[i], f2.pixels[i]);
  }
  require(held, "The wash changed inside a hold");
  require(changed, "The wash did not change after a hold");
  require(plane(s, 1) == plane(s, 0) && plane(s, 2) == plane(s, 0) + 1,
          "The noise plane does not follow the hold");
  // The wobble moved the edge, but by no more than its amount
  bool moved = false;
  // (away from the image's border, where it reads from outside the image)
  for (int y = 6; y < 42; ++y)
    for (int x = 6; x < 42; ++x) {
      moved = moved || !same(f0.at(x, y), image.at(x, y));
      require(same(f0.at(x, y), image.at(x, y)) || std::abs(x - 23.5) <= 5,
              "The wobble moved the edge further than its amount");
    }
  require(moved, "The wobble moved nothing");
}

// A tile with its halo gives the same pixels as the whole frame
void testTiling() {
  Settings s;  // everything on
  s.paper           = false;
  const int h       = halo(s);
  const Image image = [&] {
    Image big(120, 100);
    for (int y = 0; y < big.h; ++y)
      for (int x = 0; x < big.w; ++x)
        if ((x - 60) * (x - 60) + (y - 50) * (y - 50) < 900)
          big.at(x, y) = (x + y) % 23 < 11 ? paint : Color{0.1, 0.3, 0.7, 1};
    return big;
  }();
  const Image whole = render(image, s, 3, 0, 0);
  const int left = 41, bottom = 33, w = 30, ht = 24;
  Image tile(w + 2 * h, ht + 2 * h);
  for (int y = 0; y < tile.h; ++y)
    for (int x = 0; x < tile.w; ++x) {
      const int sx = x + left - h, sy = y + bottom - h;
      if (sx >= 0 && sy >= 0 && sx < image.w && sy < image.h)
        tile.at(x, y) = image.at(sx, sy);
    }
  const Image part = render(tile, s, 3, left - h, bottom - h);
  for (int y = 0; y < ht; ++y)
    for (int x = 0; x < w; ++x)
      require(same(part.at(x + h, y + h), whole.at(x + left, y + bottom)),
              "A tile differs from the whole frame");
}

void testPaper() {
  Settings s   = plain();
  s.paper      = true;
  s.paperColor = Rgb{0.9, 0.8, 0.7};
  const Image out = render(half(40, 20, 20), s);
  require(approx(out.at(30, 10).r, 0.9) && approx(out.at(30, 10).g, 0.8) &&
              approx(out.at(30, 10).b, 0.7) && approx(out.at(30, 10).a, 1),
          "Bare paper is not the paper colour");
  require(approx(out.at(5, 10).r, paint.r) && approx(out.at(5, 10).a, 1),
          "Opaque paint let the paper through");
  s.paperGrain   = 0.3;
  const Image grained = render(half(40, 20, 20), s, 0);
  const Image later   = render(half(40, 20, 20), s, 9);
  double low = 1, high = 0;
  for (int x = 22; x < 40; ++x) {
    low  = std::min(low, grained.at(x, 10).r);
    high = std::max(high, grained.at(x, 10).r);
    require(grained.at(x, 10).r <= 0.9 + 1e-12 &&
                grained.at(x, 10).r >= 0.63 - 1e-12,
            "The paper grain left its range");
    require(same(grained.at(x, 10), later.at(x, 10)),
            "The paper changed from one hold to the next");
  }
  require(high - low > 0.02, "The paper grain is flat");
}

void testWhiteToTransparent() {
  const Color white{1, 1, 1, 1}, black{0, 0, 0, 1}, pink{1, 0.5, 0.5, 1};
  require(approx(withoutWhite(white).a, 0), "White is not transparent");
  const Color kept = withoutWhite(black);
  require(approx(kept.a, 1) && approx(kept.r, 0), "Black changed");
  // Pink is half-opaque red: over white it looks the same
  const Color red = withoutWhite(pink);
  require(approx(red.a, 0.5) && approx(red.r, 0.5) && approx(red.g, 0) &&
              approx(red.b, 0),
          "Pink is not half-opaque red");
  require(approx(red.r + (1 - red.a), pink.r) &&
              approx(red.g + (1 - red.a), pink.g),
          "A cleared colour does not look the same over white");
  const Color half = withoutWhite({0.25, 0.25, 0.25, 0.5});  // premultiplied
  require(approx(half.a, 0.25) && approx(half.r, 0),
          "A half-transparent grey did not keep its transparency");
  require(approx(withoutWhite(Color()).a, 0), "Clear did not stay clear");

  // In the wash: a white sheet behind the paint is gone
  Settings s           = plain();
  s.whiteToTransparent = true;
  Image image(40, 20, white);
  for (int y = 0; y < 20; ++y)
    for (int x = 0; x < 20; ++x) image.at(x, y) = paint;
  const Image out = render(image, s);
  require(approx(out.at(30, 10).a, 0), "The white sheet stayed opaque");
  require(out.at(10, 10).a > 0.3, "The paint was cleared with the white");
  s.whiteToTransparent = false;
  require(approx(render(image, s).at(30, 10).a, 1),
          "White was cleared without the option");
}

void testLimits() {
  auto rejects = [](const Settings &s) {
    try {
      halo(s);
    } catch (const std::invalid_argument &) {
      return true;
    }
    return false;
  };
  Settings s;
  s.bleed = maxBleed + 1;
  require(rejects(s), "An oversized bleed was accepted");
  s      = Settings();
  s.hold = 0;
  require(rejects(s), "A hold of no frames was accepted");
  s        = Settings();
  s.wobble = std::nan("");
  require(rejects(s), "A non-finite wobble was accepted");
  require(halo(plain()) == 2, "With everything off the halo is not minimal");
  const Image image = half(20, 20, 10);
  Image out(20, 20);
  int canceled = 1;
  require(!Watercolour::render(image.pixels, 20, 20, 0, 0, Settings(), 0,
                               out.pixels, &canceled),
          "A canceled render reported success");
}
}  // namespace

int main() {
  try {
    testPassThrough();
    testBleed();
    testEdgeDarkening();
    testGranulation();
    testHold();
    testTiling();
    testPaper();
    testWhiteToTransparent();
    testLimits();
    std::cout << "PASS: watercolour pass-through, bleed, edge darkening, "
                 "granulation, hold, tiling, paper and limits\n";
    return 0;
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
  }
  return 1;
}
