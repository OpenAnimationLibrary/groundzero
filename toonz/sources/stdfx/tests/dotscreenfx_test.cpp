#include "../dotscreenfx.cpp"
#include "tparamcontainer.h"

#include <QCoreApplication>
#include <cmath>
#include <iostream>
#include <stdexcept>
#include <string>
#include <type_traits>

namespace {
void require(bool value, const std::string &message) {
  if (!value) throw std::runtime_error(message);
}
bool approx(double a, double b, double tolerance = 1e-4) {
  return std::abs(a - b) <= tolerance;
}

// Opaque grey: black up to world x = -90, white from x = 90
class Ramp final : public TStandardRasterFx {
public:
  const TPersistDeclaration *getDeclaration() const override { return nullptr; }
  bool canHandle(const TRenderSettings &, double) override { return true; }
  std::string getAlias(double, const TRenderSettings &) const override {
    return "dot-screen-ramp";
  }
  bool doGetBBox(double, TRectD &box, const TRenderSettings &) override {
    box = TConsts::infiniteRectD;
    return true;
  }
  template <class Pixel>
  void fill(const TRasterPT<Pixel> &raster, TPointD pos,
            const TAffine &outputToWorld) {
    for (int y = 0; y < raster->getLy(); ++y)
      for (int x = 0; x < raster->getLx(); ++x) {
        const TPointD p = outputToWorld * (pos + TPointD(x + 0.5, y + 0.5));
        const double v  = std::max(0.0, std::min(1.0, (p.x + 90) / 180));
        raster->pixels(y)[x] = PixelConverter<Pixel>::from(TPixelD(v, v, v, 1));
      }
  }
  void doCompute(TTile &tile, double, const TRenderSettings &info) override {
    const TAffine inverse = info.m_affine.inv();
    if (TRaster32P r = tile.getRaster())
      fill<TPixel32>(r, tile.m_pos, inverse);
    else if (TRaster64P r = tile.getRaster())
      fill<TPixel64>(r, tile.m_pos, inverse);
    else if (TRasterFP r = tile.getRaster())
      fill<TPixelF>(r, tile.m_pos, inverse);
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

template <class Pixel>
void testFx() {
  TFxP holder = new DotScreenFx;
  auto &fx    = static_cast<DotScreenFx &>(*holder);
  set(fx, "spacing", 20);
  set(fx, "angle", 0);
  set(fx, "size", 1);
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
  auto alpha = [](const TRasterPT<Pixel> &raster, int x, int y) {
    return PixelConverter<TPixelD>::from(raster->pixels(y)[x]).m;
  };

  // Without an input the dots are plain: radius 10 at every lattice point.
  // Output pixel (x, y) is centred on world (x - 127.5, y - 15.5).
  const TRasterPT<Pixel> plain = render(TPointD(-128, -16), 256, 32, 0);
  require(approx(alpha(plain, 128, 16), 1) && approx(alpha(plain, 135, 16), 1),
          "A plain dot is not the size asked for");
  require(approx(alpha(plain, 138, 26), 0), "A cell corner is not empty");

  // With the ramp on Source the first modifier makes a halftone of it: a
  // full dot on black, half a dot on mid grey, none on white
  TFxP ramp = new Ramp;
  require(fx.connect("Source", ramp.getPointer()), "Cannot connect Source");
  const TRasterPT<Pixel> whole = render(TPointD(-128, -16), 256, 32, 0);
  require(approx(alpha(whole, 28, 16), 1) && approx(alpha(whole, 35, 16), 1),
          "Black did not give a full dot");
  require(approx(alpha(whole, 128, 16), 1) && approx(alpha(whole, 135, 16), 0),
          "Mid grey did not give half a dot");
  require(approx(alpha(whole, 228, 16), 0), "White gave a dot");
  const TRasterPT<Pixel> part = render(TPointD(-128 + 23, -16 + 5), 31, 19, 0);
  for (int y = 0; y < 19; ++y)
    for (int x = 0; x < 31; ++x)
      require(part->pixels(y)[x] == whole->pixels(y + 5)[x + 23],
              "An FX tile differs from the whole frame");

  // Inverted, white gives the full dot
  TBoolParamP invert = TParamP(fx.getParams()->getParam("invert1"));
  require(bool(invert), "No invert parameter");
  invert->setValue(true);
  const TRasterPT<Pixel> inverted = render(TPointD(-128, -16), 256, 32, 0);
  require(approx(alpha(inverted, 228, 16), 1) &&
              approx(alpha(inverted, 28, 16), 0),
          "Invert did not swap black and white");
  invert->setValue(false);

  // Twice the scale: the same dots in the world, twice the pixels
  settings.m_affine              = TScale(2);
  const TRasterPT<Pixel> scaled = render(TPointD(-256, -32), 512, 64, 0);
  require(approx(alpha(scaled, 263, 32), 1) && approx(alpha(scaled, 268, 32), 0),
          "A scaled halftone dot is not the size asked for");
  settings.m_affine = TAffine();

  // A modifier whose input is not connected changes nothing
  fx.disconnect("Source");
  set(fx, "target2", 4);  // Opacity
  set(fx, "mode2", 0);    // Set
  set(fx, "low2", 0);
  set(fx, "high2", 0);
  const TRasterPT<Pixel> unplugged = render(TPointD(-128, -16), 256, 32, 0);
  require(approx(alpha(unplugged, 128, 16), 1) &&
              approx(alpha(unplugged, 135, 16), 1),
          "A modifier without its input changed the dots");

  // The cache key follows the held seed
  require(fx.getAlias(0, settings) == fx.getAlias(5, settings),
          "A still screen has a different cache key on another frame");
  set(fx, "hold", 3);
  require(fx.getAlias(0, settings) == fx.getAlias(2, settings) &&
              fx.getAlias(2, settings) != fx.getAlias(3, settings),
          "The cache key does not follow the hold");
}
}  // namespace

int main(int argc, char **argv) {
  QCoreApplication app(argc, argv);
  try {
    testFx<TPixel32>();
    testFx<TPixel64>();
    testFx<TPixelF>();
    std::cout << "PASS: dot screen plain dots, halftone, tiles, invert, scale, "
                 "cache keys and pixel depths\n";
    return 0;
  } catch (const TException &error) {
    std::cerr << QString::fromStdWString(error.getMessage()).toStdString()
              << '\n';
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
  }
  return 1;
}
