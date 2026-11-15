#include "../watercolourfx.cpp"
#include "tparamcontainer.h"

#include <QCoreApplication>
#include <cmath>
#include <iostream>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <vector>

namespace {
using Watercolour::Color;

void require(bool value, const std::string &message) {
  if (!value) throw std::runtime_error(message);
}
bool approx(double a, double b, double tolerance = 1e-6) {
  return std::abs(a - b) <= tolerance;
}

const int width = 48, height = 40, edge = 24;
const Color paintColour{0.6, 0.4, 0.2, 1};

// Paint on the left half of a 48 by 40 image, clear on the right
class Source final : public TStandardRasterFx {
public:
  const TPersistDeclaration *getDeclaration() const override { return nullptr; }
  bool canHandle(const TRenderSettings &, double) override { return true; }
  std::string getAlias(double, const TRenderSettings &) const override {
    return "watercolour-source";
  }
  bool doGetBBox(double, TRectD &box, const TRenderSettings &) override {
    box = TRectD(0, 0, width, height);
    return true;
  }
  template <class Pixel>
  void fill(const TRasterPT<Pixel> &raster, TPointD pos) {
    raster->clear();
    for (int y = 0; y < raster->getLy(); ++y)
      for (int x = 0; x < raster->getLx(); ++x) {
        const int sx = x + int(pos.x), sy = y + int(pos.y);
        if (sx < 0 || sy < 0 || sx >= edge || sy >= height) continue;
        raster->pixels(y)[x] = PixelConverter<Pixel>::from(TPixelD(
            paintColour.r, paintColour.g, paintColour.b, paintColour.a));
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

void set(TFx &fx, const std::string &name, double value) {
  TDoubleParamP param = TParamP(fx.getParams()->getParam(name));
  require(bool(param), "No numeric parameter " + name);
  param->setDefaultValue(value);
}
void setPaper(TFx &fx, bool value) {
  TBoolParamP paper = TParamP(fx.getParams()->getParam("paper"));
  require(bool(paper), "No paper parameter");
  paper->setValue(value);
}

template <class Pixel>
void testFx() {
  TFxP holder = new WatercolourFx;
  TFxP source = new Source;
  require(holder->connect("Source", source.getPointer()),
          "Cannot connect source");
  auto &fx = static_cast<WatercolourFx &>(*holder);
  TRenderSettings settings;
  settings.m_bpp = std::is_same<Pixel, TPixelF>::value    ? 128
                   : std::is_same<Pixel, TPixel64>::value ? 64
                                                          : 32;
  auto render    = [&](TPointD pos, int w, int h, double frame) {
    TRasterPT<Pixel> raster(w, h);
    TTile tile(raster, pos);
    fx.doCompute(tile, frame, settings);
    return raster;
  };
  auto pixel = [](const TRasterPT<Pixel> &raster, int x, int y) {
    return PixelConverter<TPixelD>::from(raster->pixels(y)[x]);
  };

  // The wash without paper: a tile equals the same part of the whole frame
  setPaper(fx, false);
  const TRasterPT<Pixel> whole = render(TPointD(0, 0), width, height, 0);
  const TRasterPT<Pixel> part  = render(TPointD(15, 11), 17, 13, 0);
  for (int y = 0; y < 13; ++y)
    for (int x = 0; x < 17; ++x)
      require(part->pixels(y)[x] == whole->pixels(y + 11)[x + 15],
              "An FX tile differs from the whole frame");
  // Colour bled past the edge, and thinned before it
  require(pixel(whole, edge + 1, 20).m > 0, "Colour did not bleed past the edge");
  require(pixel(whole, edge - 2, 20).m < 1, "The wash did not thin at its edge");
  require(approx(pixel(whole, 45, 20).m, 0), "Colour bled across the frame");

  // The cache key: shared within a hold, new for the next one
  const std::string a0 = fx.getAlias(0, settings), a1 = fx.getAlias(1, settings),
                    a2 = fx.getAlias(2, settings);
  require(a0 == a1, "Frames of one hold have different cache keys");
  require(a0 != a2, "A new hold reuses the previous cache key");

  // On paper: bare paper away from the paint, opaque everywhere
  setPaper(fx, true);
  set(fx, "paperGrain", 0);
  const TRasterPT<Pixel> onPaper = render(TPointD(0, 0), width, height, 0);
  const TPixelD bare             = pixel(onPaper, 45, 20);
  require(approx(bare.m, 1) && approx(bare.r, 250.0 / 255, 2e-3) &&
              approx(bare.g, 245.0 / 255, 2e-3) &&
              approx(bare.b, 235.0 / 255, 2e-3),
          "Bare paper is not the paper colour");
  require(approx(pixel(onPaper, edge, 20).m, 1), "The paper has a hole");
  // The paper covers the drawing and its bleed, not an infinite area
  {
    TRectD box;
    require(fx.doGetBBox(0, box, settings), "The wash has no box");
    require(box != TConsts::infiniteRectD && box.x0 < 0 && box.y0 < 0 &&
                box.x1 > width && box.y1 > height && box.x1 < width + 100,
            "The paper's box is not the drawing grown by its reach");
  }

  // The memory estimate covers a 4K tile and its halo at every depth
  {
    const TRectD frame4k(0, 0, 3840, 2160);
    const double h =
        Watercolour::halo(fx.settings(0, settings));
    const double area  = (3840 + 2 * h) * (2160 + 2 * h);
    const double bytes = area * (16 + 4 * sizeof(Watercolour::Color) +
                                 2 * sizeof(double) + sizeof(int) + 1);
    const int estimate = fx.getMemoryRequirement(frame4k, 0, settings);
    require(estimate >= int(bytes / (1024.0 * 1024.0)),
            "The memory estimate is below what a 4K tile allocates");
    require(estimate < 4 * int(bytes / (1024.0 * 1024.0)),
            "The memory estimate is far above what a 4K tile allocates");
  }

  // Everything off: the source passes through
  setPaper(fx, false);
  for (const char *name :
       {"wobble", "bleed", "edgeDarkening", "granulation", "paperGrain"})
    set(fx, name, 0);
  const TRasterPT<Pixel> still = render(TPointD(0, 0), width, height, 7);
  for (int y = 0; y < height; ++y)
    for (int x = 0; x < width; ++x) {
      const TPixelD p = pixel(still, x, y);
      const bool painted = x < edge;
      require(approx(p.m, painted ? 1 : 0) &&
                  approx(p.r, painted ? paintColour.r : 0, 3e-3),
              "With everything off the source changed");
    }
}
}  // namespace

int main(int argc, char **argv) {
  QCoreApplication app(argc, argv);
  try {
    testFx<TPixel32>();
    testFx<TPixel64>();
    testFx<TPixelF>();
    std::cout << "PASS: watercolour tiles, bleed, cache keys, paper, "
                 "pass-through and pixel depths\n";
    return 0;
  } catch (const TException &error) {
    std::cerr << QString::fromStdWString(error.getMessage()).toStdString()
              << '\n';
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
  }
  return 1;
}
