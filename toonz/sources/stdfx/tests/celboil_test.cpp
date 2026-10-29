#include "../celboilfx.cpp"
#include "tparamcontainer.h"

#include <QCoreApplication>
#include <cmath>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
using CelBoil::Color;

void require(bool value, const std::string &message) {
  if (!value) throw std::runtime_error(message);
}
bool approx(double a, double b, double tolerance = 1e-9) {
  return std::abs(a - b) <= tolerance;
}
bool same(const Color &a, const Color &b) {
  return a.r == b.r && a.g == b.g && a.b == b.b && a.a == b.a;
}

const Color red{1, 0, 0, 1}, black{0, 0, 0, 1}, clear{0, 0, 0, 0};

struct Image {
  int w, h;
  std::vector<Color> pixels;
  Image(int w_, int h_, Color c = clear)
      : w(w_), h(h_), pixels(size_t(w_) * h_, c) {}
  Color &at(int x, int y) { return pixels[size_t(y) * w + x]; }
  const Color &at(int x, int y) const { return pixels[size_t(y) * w + x]; }
};

// Red paint with black lines: a frame and a cross
Image drawing(int w, int h) {
  Image image(w, h, red);
  for (int y = 0; y < h; ++y)
    for (int x = 0; x < w; ++x) {
      const bool frame = x < 2 || y < 2 || x >= w - 2 || y >= h - 2;
      const bool cross = std::abs(x - w / 2) < 1 || std::abs(y - h / 2) < 1;
      if (frame || cross) image.at(x, y) = black;
    }
  return image;
}

std::vector<Color> boil(const Image &image, const CelBoil::Settings &s,
                        double frame, double ox = 0, double oy = 0) {
  std::vector<Color> out;
  require(
      CelBoil::render(image.pixels, image.w, image.h, ox, oy, s, frame, out),
      "Boil was canceled");
  return out;
}

void testPhase() {
  CelBoil::Settings s;
  s.cycle = 2;
  require(CelBoil::phase(s, 0) == 0 && CelBoil::phase(s, 1) == 0 &&
              CelBoil::phase(s, 2) == 1 && CelBoil::phase(s, 5) == 2,
          "A wobble is not held for its cycle");
  s.cycle = 4;
  s.blend = 0.5;
  require(CelBoil::phase(s, 0) == 0 && CelBoil::phase(s, 1) == 0 &&
              CelBoil::phase(s, 2) == 0,
          "A blend started before its share of the cycle");
  require(approx(CelBoil::phase(s, 3), 0.5), "A blend is not halfway");
  require(CelBoil::phase(s, 4) == 1, "A blend did not end on the next plane");
}

void testHold() {
  const Image image = drawing(48, 40);
  CelBoil::Settings s;
  s.amplitude   = 2.5;
  s.cycle       = 3;
  const auto f0 = boil(image, s, 0), f2 = boil(image, s, 2),
             f3 = boil(image, s, 3);
  bool held = true, changed = false, moved = false;
  for (size_t i = 0; i < f0.size(); ++i) {
    held    = held && same(f0[i], f2[i]);
    changed = changed || !same(f0[i], f3[i]);
    moved   = moved || !same(f0[i], image.pixels[i]);
  }
  require(held, "Frames of one cycle differ");
  require(changed, "The next cycle has the same wobble");
  require(moved, "The boil did not move anything");
  // The seed reproduces a wobble, and another seed gives another one
  const auto again = boil(image, s, 0);
  s.seed           = 2;
  const auto other = boil(image, s, 0);
  bool reproduced = true, differs = false;
  for (size_t i = 0; i < f0.size(); ++i) {
    reproduced = reproduced && same(f0[i], again[i]);
    differs    = differs || !same(f0[i], other[i]);
  }
  require(reproduced, "The same seed gave another wobble");
  require(differs, "Another seed gave the same wobble");
}

void testAmplitude() {
  for (double detail : {0.0, 0.7}) {
    CelBoil::Settings s;
    s.amplitude    = 3;
    s.detail       = detail;
    double largest = 0;
    for (int i = 0; i < 4000; ++i) {
      double dx, dy;
      CelBoil::displacement(s, i % 7, i * 1.37, i * 0.73, dx, dy);
      largest = std::max(largest, std::max(std::abs(dx), std::abs(dy)));
    }
    require(largest <= s.amplitude * (1 + detail) + 1e-12,
            "A displacement exceeds the amplitude");
    require(largest > s.amplitude * 0.3, "The displacement is too small");
  }
  // No amplitude: the image exactly
  const Image image = drawing(20, 20);
  CelBoil::Settings s;
  s.amplitude    = 0;
  const auto out = boil(image, s, 5);
  for (size_t i = 0; i < out.size(); ++i)
    require(same(out[i], image.pixels[i]), "Zero amplitude changed a pixel");
}

void testTiling() {
  const Image image = drawing(64, 56);
  for (CelBoil::Target target :
       {CelBoil::Target::Image, CelBoil::Target::Lines}) {
    CelBoil::Settings s;
    s.amplitude     = 3.5;
    s.detail        = 0.4;
    s.size          = 7;
    s.target        = target;
    const int halo  = CelBoil::halo(s);
    const auto full = boil(image, s, 4);
    for (int x0 : {0, 17, 40})
      for (int y0 : {0, 23, 36}) {
        const int tw = 19, th = 17;
        Image window(tw + 2 * halo, th + 2 * halo);
        for (int y = 0; y < window.h; ++y)
          for (int x = 0; x < window.w; ++x) {
            const int sx = x0 - halo + x, sy = y0 - halo + y;
            if (sx >= 0 && sy >= 0 && sx < image.w && sy < image.h)
              window.at(x, y) = image.at(sx, sy);
          }
        const auto part = boil(window, s, 4, x0 - halo, y0 - halo);
        for (int y = 0; y < th; ++y)
          for (int x = 0; x < tw; ++x) {
            const int sx = x0 + x, sy = y0 + y;
            if (sx < halo || sy < halo || sx >= image.w - halo ||
                sy >= image.h - halo)
              continue;
            require(same(full[size_t(sy) * image.w + sx],
                         part[size_t(y + halo) * window.w + x + halo]),
                    std::string("A tile differs from the full frame (") +
                        (target == CelBoil::Target::Lines ? "lines" : "image") +
                        ") at " + std::to_string(sx) + "," +
                        std::to_string(sy));
          }
      }
  }
}

void testLinesOnly() {
  const Image image = drawing(60, 60);
  CelBoil::Settings s;
  s.amplitude    = 2.5;
  s.target       = CelBoil::Target::Lines;
  const auto out = boil(image, s, 0);
  bool moved     = false;
  for (int y = 0; y < image.h; ++y)
    for (int x = 0; x < image.w; ++x) {
      const Color &c = out[size_t(y) * image.w + x];
      // No holes: where a line moved away, its paint shows
      require(approx(c.a, 1, 1e-9), "A line left a hole at " + std::to_string(x) +
                                      "," + std::to_string(y));
      // Paint far from every line stays exactly where it was
      const int fromCross = std::min(std::abs(x - 30), std::abs(y - 30));
      const int fromFrame =
          std::min(std::min(x, y), std::min(image.w - 1 - x, image.h - 1 - y));
      if (fromCross > 6 && fromFrame > 7)
        require(same(c, image.at(x, y)), "Paint away from the lines moved");
      moved = moved || !same(c, image.at(x, y));
    }
  require(moved, "The lines did not move");
}

void testCancel() {
  const Image image = drawing(16, 16);
  int canceled      = 1;
  std::vector<Color> out(4);
  require(!CelBoil::render(image.pixels, 16, 16, 0, 0, CelBoil::Settings(), 0,
                           out, &canceled) &&
              out.empty(),
          "Cancellation did not clear the result");
}

// The FX on rasters of each depth
class Source final : public TStandardRasterFx {
  Image m_image;

public:
  explicit Source(const Image &image) : m_image(image) {}
  const TPersistDeclaration *getDeclaration() const override { return nullptr; }
  bool canHandle(const TRenderSettings &, double) override { return true; }
  std::string getAlias(double, const TRenderSettings &) const override {
    return "boil-source";
  }
  bool doGetBBox(double, TRectD &box, const TRenderSettings &) override {
    box = TRectD(0, 0, m_image.w, m_image.h);
    return true;
  }
  template <class Pixel>
  void fill(const TRasterPT<Pixel> &raster, TPointD pos) {
    raster->clear();
    for (int y = 0; y < raster->getLy(); ++y)
      for (int x = 0; x < raster->getLx(); ++x) {
        const int sx = x + int(pos.x), sy = y + int(pos.y);
        if (sx < 0 || sy < 0 || sx >= m_image.w || sy >= m_image.h) continue;
        const Color &c = m_image.at(sx, sy);
        raster->pixels(y)[x] =
            PixelConverter<Pixel>::from(TPixelD(c.r, c.g, c.b, c.a));
      }
  }
  void doCompute(TTile &tile, double, const TRenderSettings &) override {
    if (TRaster32P r = tile.getRaster())
      fill<TPixel32>(r, tile.m_pos);
    else if (TRaster64P r = tile.getRaster())
      fill<TPixel64>(r, tile.m_pos);
    else if (TRasterFP r = tile.getRaster())
      fill<TPixelF>(r, tile.m_pos);
  }
  void compute(TTile &tile, double frame, const TRenderSettings &s) override {
    doCompute(tile, frame, s);
  }
};

TDoubleParamP numeric(TFx &fx, const std::string &name) {
  return TParamP(fx.getParams()->getParam(name));
}

template <class Pixel>
void testFx() {
  const Image image = drawing(40, 36);
  TFxP fx           = new CelBoilFx;
  TFxP source       = new Source(image);
  require(fx->connect("Source", source.getPointer()), "Cannot connect source");
  auto &boilFx = static_cast<CelBoilFx &>(*fx);
  numeric(*fx, "amplitude")->setDefaultValue(2);
  TRenderSettings settings;
  settings.m_bpp = std::is_same<Pixel, TPixelF>::value    ? 128
                   : std::is_same<Pixel, TPixel64>::value ? 64
                                                          : 32;
  auto render    = [&](TPointD pos, int w, int h, double frame) {
    TRasterPT<Pixel> raster(w, h);
    TTile tile(raster, pos);
    boilFx.doCompute(tile, frame, settings);
    return raster;
  };
  const TRasterPT<Pixel> whole = render(TPointD(0, 0), 40, 36, 0);
  const TRasterPT<Pixel> part  = render(TPointD(13, 9), 11, 10, 0);
  for (int y = 0; y < 10; ++y)
    for (int x = 0; x < 11; ++x)
      require(part->pixels(y)[x] == whole->pixels(y + 9)[x + 13],
              "An FX tile differs from the whole frame");
  // The cache key: shared within a hold, new for the next wobble
  const std::string a0 = boilFx.getAlias(0, settings),
                    a1 = boilFx.getAlias(1, settings),
                    a2 = boilFx.getAlias(2, settings);
  require(a0 == a1, "Frames of one hold have different cache keys");
  require(a0 != a2, "A new wobble reuses the previous cache key");
  // No amplitude: the source passes through
  numeric(*fx, "amplitude")->setDefaultValue(0);
  const TRasterPT<Pixel> still = render(TPointD(0, 0), 40, 36, 7);
  for (int y = 0; y < 36; ++y)
    for (int x = 0; x < 40; ++x) {
      const TPixelD p = PixelConverter<TPixelD>::from(still->pixels(y)[x]);
      const Color &c  = image.at(x, y);
      require(approx(p.r, c.r, 1e-6) && approx(p.m, c.a, 1e-6),
              "No amplitude changed the source");
    }
}
}  // namespace

int main(int argc, char **argv) {
  QCoreApplication app(argc, argv);
  try {
    testPhase();
    testHold();
    testAmplitude();
    testTiling();
    testLinesOnly();
    testCancel();
    testFx<TPixel32>();
    testFx<TPixel64>();
    testFx<TPixelF>();
    std::cout << "PASS: boil holds, blends, amplitude, tiling, lines only and "
                 "pixel depths\n";
    return 0;
  } catch (const TException &error) {
    std::cerr << QString::fromStdWString(error.getMessage()).toStdString()
              << '\n';
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
  }
  return 1;
}
