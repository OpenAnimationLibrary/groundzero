#include "../printprocessfx.cpp"
#include "tparamcontainer.h"

#include <QCoreApplication>
#include <cmath>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
using PrintProcess::Color;

void require(bool value, const std::string &message) {
  if (!value) throw std::runtime_error(message);
}
bool near(double a, double b, double tolerance = 1e-9) {
  return std::abs(a - b) <= tolerance;
}
bool same(const Color &a, const Color &b) {
  return a.r == b.r && a.g == b.g && a.b == b.b && a.a == b.a;
}
bool close(const Color &a, const Color &b) {
  return near(a.r, b.r) && near(a.g, b.g) && near(a.b, b.b) && near(a.a, b.a);
}

struct Image {
  int w, h;
  std::vector<Color> pixels;
  Image(int w_, int h_, Color c = Color())
      : w(w_), h(h_), pixels(size_t(w_) * h_, c) {}
  Color &at(int x, int y) { return pixels[size_t(y) * w + x]; }
  const Color &at(int x, int y) const { return pixels[size_t(y) * w + x]; }
};

std::vector<Color> print(const Image &image, const PrintProcess::Settings &s,
                         double ox = 0, double oy = 0) {
  std::vector<Color> out;
  require(PrintProcess::render(image.pixels, image.w, image.h, ox, oy, s, out),
          "Print was canceled");
  return out;
}

// Only the black plate, no grain or gain: the plate's ink share is exact
PrintProcess::Settings blackOnly() {
  PrintProcess::Settings s;
  for (int k = 0; k < 3; ++k) s.plates[k].enabled = false;
  s.blackGeneration = 1;
  s.dotGain         = 0;
  s.grain           = 0;
  s.output          = PrintProcess::Output::Black;
  return s;
}

void testCoverage() {
  for (PrintProcess::Shape shape :
       {PrintProcess::Shape::Round, PrintProcess::Shape::Diamond,
        PrintProcess::Shape::Line})
    for (double tone : {0.1, 0.5, 0.85}) {
      const double grey = 1 - tone;
      Image image(96, 96, Color{grey, grey, grey, 1});
      PrintProcess::Settings s = blackOnly();
      s.shape                  = shape;
      s.cellSize               = 8;
      const auto out           = print(image, s);
      double ink               = 0;
      for (const Color &c : out) ink += 1 - c.r;
      ink /= out.size();
      require(near(ink, tone, 0.02), "A tone of " + std::to_string(tone) +
                                         " inks " + std::to_string(ink) +
                                         " of the cells for shape " +
                                         std::to_string(int(shape)));
    }
  // Dot gain raises the middle tones
  Image grey(64, 64, Color{0.5, 0.5, 0.5, 1});
  PrintProcess::Settings s = blackOnly();
  double plain = 0, gained = 0;
  for (const Color &c : print(grey, s)) plain += 1 - c.r;
  s.dotGain = 0.4;
  for (const Color &c : print(grey, s)) gained += 1 - c.r;
  require(gained > plain * 1.1, "Dot gain did not raise the middle tone");
}

void testScreen() {
  // A screen at 0 degrees repeats with its cell
  Image image(64, 32, Color{0.4, 0.4, 0.4, 1});
  PrintProcess::Settings s = blackOnly();
  s.cellSize               = 8;
  s.plates[3].angle        = 0;
  const auto out           = print(image, s);
  for (int y = 0; y < 32; ++y)
    for (int x = 0; x + 8 < 64; ++x)
      require(same(out[size_t(y) * 64 + x], out[size_t(y) * 64 + x + 8]),
              "A 0 degree screen does not repeat with its cell");
  // Another angle is another pattern
  s.plates[3].angle = 30;
  const auto turned = print(image, s);
  bool differs      = false;
  for (size_t i = 0; i < out.size(); ++i)
    differs = differs || !same(out[i], turned[i]);
  require(differs, "The screen angle did not change the pattern");
}

void testMisregistration() {
  // A drawing on white: an integer offset moves that plate exactly
  Image image(60, 50, Color{1, 1, 1, 1});
  for (int y = 15; y < 35; ++y)
    for (int x = 15; x < 40; ++x) image.at(x, y) = Color{0.3, 0.3, 0.3, 1};
  PrintProcess::Settings s = blackOnly();
  const auto still         = print(image, s);
  s.plates[3].offsetX      = 3;
  s.plates[3].offsetY      = -2;
  const auto moved         = print(image, s);
  for (int y = 4; y < 46; ++y)
    for (int x = 4; x < 56; ++x)
      // The same dots, computed in another order: equal up to rounding
      require(
          close(moved[size_t(y) * 60 + x], still[size_t(y + 2) * 60 + x - 3]),
          "Misregistration did not move the plate by its offset");
  // Other plates stay where they are
  PrintProcess::Settings all;
  all.grain  = 0;
  all.output = PrintProcess::Output::Cyan;
  Image cyan(40, 40, Color{0.2, 0.8, 0.9, 1});
  const auto a          = print(cyan, all);
  all.plates[3].offsetX = 5;
  const auto b          = print(cyan, all);
  for (size_t i = 0; i < a.size(); ++i)
    require(same(a[i], b[i]), "Moving black moved the cyan plate");
}

void testPaperAndInks() {
  PrintProcess::Settings s;
  s.grain = 0;
  // White prints nothing: bare paper
  for (const Color &c : print(Image(20, 20, Color{1, 1, 1, 1}), s))
    require(near(c.r, s.paper.r) && near(c.g, s.paper.g) &&
                near(c.b, s.paper.b) && near(c.a, 1),
            "White is not bare paper");
  // A full cyan plate is paper filtered by the cyan ink
  s.dotGain = 0;
  for (int k = 1; k < 4; ++k) s.plates[k].enabled = false;
  // (away from the image's edge, where its samples include no ink)
  const auto solid = print(Image(20, 20, Color{0, 1, 1, 1}), s);
  for (int y = 1; y < 19; ++y)
    for (int x = 1; x < 19; ++x) {
      const Color &c = solid[size_t(y) * 20 + x];
      require(near(c.r, s.paper.r * s.plates[0].ink.r, 1e-6) &&
                  near(c.g, s.paper.g * s.plates[0].ink.g, 1e-6) &&
                  near(c.b, s.paper.b * s.plates[0].ink.b, 1e-6),
              "A full plate is not its ink on paper: " + std::to_string(c.r) +
                  "," + std::to_string(c.g) + "," + std::to_string(c.b));
    }
  // Grain changes the paper, and the seed reproduces it
  s.grain = 0.5;
  const Image white(30, 30, Color{1, 1, 1, 1});
  const auto grainy = print(white, s), again = print(white, s);
  bool varies = false;
  for (size_t i = 0; i < grainy.size(); ++i) {
    require(same(grainy[i], again[i]), "Grain is not repeatable");
    varies = varies || !same(grainy[i], grainy[0]);
  }
  require(varies, "Grain did not texture the paper");
}

void testTransparent() {
  Image image(30, 30);
  for (int y = 10; y < 20; ++y)
    for (int x = 10; x < 20; ++x) image.at(x, y) = Color{0.5, 0.2, 0.2, 1};
  PrintProcess::Settings s;
  s.transparentBackground = true;
  const auto out          = print(image, s);
  require(near(out[0].a, 0) && near(out[size_t(15) * 30 + 15].a, 1),
          "A transparent background did not keep the source's alpha");
  s.transparentBackground = false;
  require(near(print(image, s)[0].a, 1), "Paper is not opaque");
}

Image painting(int w, int h) {
  Image image(w, h, Color{1, 1, 1, 1});
  for (int y = 0; y < h; ++y)
    for (int x = 0; x < w; ++x) {
      const double u = double(x) / w, v = double(y) / h;
      if ((x - w / 2) * (x - w / 2) + (y - h / 2) * (y - h / 2) < w * h / 8)
        image.at(x, y) = Color{u, 0.5 * v, 1 - u * v, 1};
    }
  return image;
}

void testTiling() {
  const Image image = painting(72, 64);
  PrintProcess::Settings s;
  s.plates[0].offsetX = 1.5;
  s.plates[1].offsetY = -2.25;
  s.cellSize          = 5.5;
  const int halo      = PrintProcess::halo(s);
  const auto full     = print(image, s);
  for (int x0 : {0, 19, 45})
    for (int y0 : {0, 22, 40}) {
      const int tw = 21, th = 17;
      Image window(tw + 2 * halo, th + 2 * halo);
      for (int y = 0; y < window.h; ++y)
        for (int x = 0; x < window.w; ++x) {
          const int sx = x0 - halo + x, sy = y0 - halo + y;
          if (sx >= 0 && sy >= 0 && sx < image.w && sy < image.h)
            window.at(x, y) = image.at(sx, sy);
        }
      const auto part = print(window, s, x0 - halo, y0 - halo);
      for (int y = 0; y < th; ++y)
        for (int x = 0; x < tw; ++x) {
          const int sx = x0 + x, sy = y0 + y;
          if (sx < halo || sy < halo || sx >= image.w - halo ||
              sy >= image.h - halo)
            continue;
          require(same(full[size_t(sy) * image.w + sx],
                       part[size_t(y + halo) * window.w + x + halo]),
                  "A tile differs from the full frame at " +
                      std::to_string(sx) + "," + std::to_string(sy));
        }
    }
}

void testCancel() {
  int canceled = 1;
  std::vector<Color> out(4);
  require(!PrintProcess::render(Image(8, 8).pixels, 8, 8, 0, 0,
                                PrintProcess::Settings(), out, &canceled) &&
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
    return "print-source";
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
  const Image image = painting(40, 36);
  TFxP fx           = new PrintProcessFx;
  TFxP source       = new Source(image);
  require(fx->connect("Source", source.getPointer()), "Cannot connect source");
  auto &printFx = static_cast<PrintProcessFx &>(*fx);
  numeric(*fx, "offsetMagentaX")->setDefaultValue(2);
  TRenderSettings settings;
  settings.m_bpp = std::is_same<Pixel, TPixelF>::value    ? 128
                   : std::is_same<Pixel, TPixel64>::value ? 64
                                                          : 32;
  auto render    = [&](TPointD pos, int w, int h) {
    TRasterPT<Pixel> raster(w, h);
    TTile tile(raster, pos);
    printFx.doCompute(tile, 0, settings);
    return raster;
  };
  const TRasterPT<Pixel> whole = render(TPointD(0, 0), 40, 36);
  const TRasterPT<Pixel> part  = render(TPointD(11, 7), 13, 12);
  for (int y = 0; y < 12; ++y)
    for (int x = 0; x < 13; ++x)
      require(part->pixels(y)[x] == whole->pixels(y + 7)[x + 11],
              "An FX tile differs from the whole frame");
  // Paper is opaque over the whole frame
  const TPixelD corner = PixelConverter<TPixelD>::from(whole->pixels(0)[0]);
  require(near(corner.m, 1, 1e-4) && corner.r > 0.8,
          "The FX did not print on paper");
  // The alias follows the settings
  const std::string before = printFx.getAlias(0, settings);
  numeric(*fx, "angleBlack")->setDefaultValue(10);
  require(printFx.getAlias(0, settings) != before,
          "The alias ignores the screen angle");
}
}  // namespace

int main(int argc, char **argv) {
  QCoreApplication app(argc, argv);
  try {
    testCoverage();
    testScreen();
    testMisregistration();
    testPaperAndInks();
    testTransparent();
    testTiling();
    testCancel();
    testFx<TPixel32>();
    testFx<TPixel64>();
    testFx<TPixelF>();
    std::cout << "PASS: print coverage, screens, misregistration, paper, inks, "
                 "tiling and pixel depths\n";
    return 0;
  } catch (const TException &error) {
    std::cerr << QString::fromStdWString(error.getMessage()).toStdString()
              << '\n';
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
  }
  return 1;
}
