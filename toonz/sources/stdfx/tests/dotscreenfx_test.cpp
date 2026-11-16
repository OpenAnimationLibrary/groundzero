#include "../dotscreenfx.cpp"
#include "tparamcontainer.h"

#include <QCoreApplication>
#include <cmath>
#include <iostream>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <vector>

namespace {
void require(bool value, const std::string &message) {
  if (!value) throw std::runtime_error(message);
}
bool approx(double a, double b, double tolerance = 1e-4) {
  return std::abs(a - b) <= tolerance;
}

// An input that paints every output pixel from its position in the world
class Pattern : public TStandardRasterFx {
  virtual TPixelD at(TPointD world) const = 0;

public:
  const TPersistDeclaration *getDeclaration() const override { return nullptr; }
  bool canHandle(const TRenderSettings &, double) override { return true; }
  bool doGetBBox(double, TRectD &box, const TRenderSettings &) override {
    box = TConsts::infiniteRectD;
    return true;
  }
  template <class Pixel>
  void fill(const TRasterPT<Pixel> &raster, TPointD pos,
            const TAffine &outputToWorld) {
    for (int y = 0; y < raster->getLy(); ++y)
      for (int x = 0; x < raster->getLx(); ++x)
        raster->pixels(y)[x] = PixelConverter<Pixel>::from(
            at(outputToWorld * (pos + TPointD(x + 0.5, y + 0.5))));
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

// Opaque grey: black up to world x = -90, white from x = 90
class Ramp final : public Pattern {
  TPixelD at(TPointD p) const override {
    const double v = std::max(0.0, std::min(1.0, (p.x + 90) / 180));
    return TPixelD(v, v, v, 1);
  }

public:
  std::string getAlias(double, const TRenderSettings &) const override {
    return "dot-screen-ramp";
  }
};

// Opaque black within 60 units of the world origin, transparent beyond
class Disc final : public Pattern {
  TPixelD at(TPointD p) const override {
    return std::hypot(p.x, p.y) < 60 ? TPixelD(0, 0, 0, 1)
                                     : TPixelD(0, 0, 0, 0);
  }

public:
  std::string getAlias(double, const TRenderSettings &) const override {
    return "dot-screen-disc";
  }
};

void set(TFx &fx, const std::string &name, double value) {
  TDoubleParamP param = TParamP(fx.getParams()->getParam(name));
  require(bool(param), "No numeric parameter " + name);
  param->setDefaultValue(value);
}

void setPoint(TFx &fx, const std::string &name, TPointD value) {
  TPointParamP param = TParamP(fx.getParams()->getParam(name));
  require(bool(param), "No point parameter " + name);
  param->setDefaultValue(value);
}

template <class Pixel>
int bitsOf() {
  return std::is_same<Pixel, TPixelF>::value    ? 128
         : std::is_same<Pixel, TPixel64>::value ? 64
                                                : 32;
}

template <class Pixel>
TRasterPT<Pixel> renderTile(DotScreenFx &fx, const TRenderSettings &settings,
                            TPointD pos, int w, int h) {
  TRasterPT<Pixel> raster(w, h);
  TTile tile(raster, pos);
  fx.doCompute(tile, 0, settings);
  return raster;
}

template <class Pixel>
TPixelD pixelOf(const TRasterPT<Pixel> &raster, int x, int y) {
  return PixelConverter<TPixelD>::from(raster->pixels(y)[x]);
}

template <class Pixel>
void testFx() {
  TFxP holder = new DotScreenFx;
  auto &fx    = static_cast<DotScreenFx &>(*holder);
  set(fx, "spacing", 20);
  set(fx, "angle", 0);
  set(fx, "size", 1);
  TRenderSettings settings;
  settings.m_bpp = bitsOf<Pixel>();
  auto render    = [&](TPointD pos, int w, int h, double frame) {
    TRasterPT<Pixel> raster(w, h);
    TTile tile(raster, pos);
    fx.doCompute(tile, frame, settings);
    return raster;
  };
  auto alpha = [](const TRasterPT<Pixel> &raster, int x, int y) {
    return pixelOf(raster, x, y).m;
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

  // Turned and scaled: a tile is that part of the whole frame
  settings.m_affine = TRotation(30) * TScale(1.5);
  const TRasterPT<Pixel> turned = render(TPointD(-128, -64), 256, 128, 0);
  const TRasterPT<Pixel> corner =
      render(TPointD(-128 + 37, -64 + 21), 41, 23, 0);
  for (int y = 0; y < 23; ++y)
    for (int x = 0; x < 41; ++x) {
      const TPixelD a = pixelOf(corner, x, y),
                    b = pixelOf(turned, x + 37, y + 21);
      require(approx(a.r, b.r, 1e-6) && approx(a.m, b.m, 1e-6),
              "A turned FX tile differs from the whole frame");
    }
  settings.m_affine = TAffine();

  // A halftone of a disc on a transparent background has dots only on the
  // disc, and there they are the dots of black
  TFxP disc = new Disc;
  fx.disconnect("Source");
  require(fx.connect("Source", disc.getPointer()), "Cannot connect the disc");
  const TRasterPT<Pixel> cel = render(TPointD(-128, -16), 256, 32, 0);
  for (int y = 0; y < 32; ++y)
    for (int x = 0; x < 256; ++x) {
      const double worldX = x - 127.5;
      if (std::abs(worldX) < 50)
        require(cel->pixels(y)[x] == plain->pixels(y)[x],
                "The dots on an opaque area changed");
      if (std::abs(worldX) > 71)
        require(alpha(cel, x, y) == 0, "A transparent area of Source has dots");
    }

  // A modifier whose input is not connected changes nothing
  fx.disconnect("Source");
  set(fx, "target2", 4);  // Opacity
  set(fx, "mode2", 0);    // Set
  set(fx, "low2", 0);
  set(fx, "high2", 0);
  set(fx, "source2", 0);  // Input
  const TRasterPT<Pixel> unplugged = render(TPointD(-128, -16), 256, 32, 0);
  require(approx(alpha(unplugged, 128, 16), 1) &&
              approx(alpha(unplugged, 135, 16), 1),
          "A modifier without its input changed the dots");
  // A ramp needs no input: opacity 0 everywhere
  set(fx, "source2", 1);
  const TRasterPT<Pixel> ramped = render(TPointD(-128, -16), 256, 32, 0);
  require(alpha(ramped, 128, 16) == 0 && alpha(ramped, 28, 16) == 0,
          "A modifier on a ramp did nothing without an input");
  set(fx, "source2", 0);

  set(fx, "aspectRandom", 0.25);
  require(fx.settings(0).aspectRandom == 0.25,
          "Aspect randomness does not reach the dots");

  // The cache key follows the held seed
  require(fx.getAlias(0, settings) == fx.getAlias(5, settings),
          "A still screen has a different cache key on another frame");
  set(fx, "hold", 3);
  require(fx.getAlias(0, settings) == fx.getAlias(2, settings) &&
              fx.getAlias(2, settings) != fx.getAlias(3, settings),
          "The cache key does not follow the hold");
}

// Rotation on Control2 turns dots 15 wide and 5 high upright, by Set or
// Add in degrees, and by Multiply with a plain factor
template <class Pixel>
void testRotationModifier() {
  TFxP holder = new DotScreenFx;
  auto &fx    = static_cast<DotScreenFx &>(*holder);
  set(fx, "spacing", 40);
  set(fx, "angle", 0);
  set(fx, "shape", 1);  // Square
  set(fx, "size", 0.25);
  set(fx, "aspect", 3);
  set(fx, "rotation", 45);
  TFxP ramp = new Ramp;
  require(fx.connect("Control2", ramp.getPointer()), "Cannot connect Control2");
  set(fx, "target2", 3);  // Rotation
  TRenderSettings settings;
  settings.m_bpp = bitsOf<Pixel>();
  const struct {
    double mode, value;
    const char *name;
  } cases[] = {{0, 90, "Set"}, {1, 2, "Multiply"}, {2, 45, "Add"}};
  for (const auto &c : cases) {
    set(fx, "mode2", c.mode);
    set(fx, "low2", c.value);
    set(fx, "high2", c.value);
    // Output pixel (x, y) is centred on world (x - 127.5, y - 15.5)
    const TRasterPT<Pixel> raster =
        renderTile<Pixel>(fx, settings, TPointD(-128, -16), 256, 32);
    require(approx(pixelOf(raster, 128, 28).m, 1) &&
                approx(pixelOf(raster, 140, 16).m, 0),
            std::string("The rotation modifier did not turn the dots "
                        "upright by ") +
                c.name);
  }
}

// The first modifier's halftone on its built-in ramps, with nothing
// connected: a ramp is a control image of that grey, tied to the scene
template <class Pixel>
void testRamps() {
  TFxP holder = new DotScreenFx;
  auto &fx    = static_cast<DotScreenFx &>(*holder);
  set(fx, "spacing", 20);
  set(fx, "angle", 0);
  set(fx, "size", 1);
  TRenderSettings settings;
  settings.m_bpp = bitsOf<Pixel>();
  auto alpha     = [](const TRasterPT<Pixel> &raster, int x, int y) {
    return pixelOf(raster, x, y).m;
  };

  // Black at x = -90 to white at x = 90, as the Ramp input. Output pixel
  // (x, y) is centred on world (x - 127.5, y - 15.5).
  set(fx, "source1", 1);  // Linear Ramp
  setPoint(fx, "rampStart1", TPointD(-90, 0));
  setPoint(fx, "rampEnd1", TPointD(90, 0));
  const TRasterPT<Pixel> linear =
      renderTile<Pixel>(fx, settings, TPointD(-128, -16), 256, 32);
  require(approx(alpha(linear, 28, 16), 1) && approx(alpha(linear, 35, 16), 1),
          "A linear ramp's start did not give a full dot");
  require(approx(alpha(linear, 128, 16), 1) &&
              approx(alpha(linear, 135, 16), 0),
          "A linear ramp's middle did not give half a dot");
  require(approx(alpha(linear, 228, 16), 0), "A linear ramp's end gave a dot");

  // An image of the same ramp on Source gives the same dots, but for the
  // image's quantization; with the ramp chosen the image is not read
  TFxP ramp = new Ramp;
  require(fx.connect("Source", ramp.getPointer()), "Cannot connect Source");
  set(fx, "source1", 0);  // Input
  const TRasterPT<Pixel> image =
      renderTile<Pixel>(fx, settings, TPointD(-128, -16), 256, 32);
  TFxP disc = new Disc;
  fx.disconnect("Source");
  require(fx.connect("Source", disc.getPointer()), "Cannot connect the disc");
  set(fx, "source1", 1);
  const TRasterPT<Pixel> ignored =
      renderTile<Pixel>(fx, settings, TPointD(-128, -16), 256, 32);
  fx.disconnect("Source");
  for (int y = 0; y < 32; ++y)
    for (int x = 0; x < 256; ++x) {
      require(approx(alpha(linear, x, y), alpha(image, x, y), 0.03),
              "A linear ramp differs from an image of it");
      require(ignored->pixels(y)[x] == linear->pixels(y)[x],
              "A modifier on a ramp read its input");
    }

  // A full dot at the centre, none from 100 units away, the same in every
  // direction. Output pixel (x, y) is centred on world (x - 127.5,
  // y - 127.5).
  set(fx, "source1", 2);  // Radial Ramp
  setPoint(fx, "rampStart1", TPointD(0, 0));
  setPoint(fx, "rampEnd1", TPointD(60, 80));
  const TRasterPT<Pixel> radial =
      renderTile<Pixel>(fx, settings, TPointD(-128, -128), 256, 256);
  require(approx(alpha(radial, 128, 128), 1) &&
              approx(alpha(radial, 135, 128), 1),
          "A radial ramp's centre did not give a full dot");
  // The dots 60 from the centre have radius 4: a point 2.5 beyond such a
  // dot's centre is covered, one 5.5 beyond is not
  const int covered[4][2] = {{190, 128}, {65, 128}, {128, 190}, {128, 65}};
  const int beyond[4][2]  = {{193, 128}, {62, 128}, {128, 193}, {128, 62}};
  for (int k = 0; k < 4; ++k)
    require(approx(alpha(radial, covered[k][0], covered[k][1]), 1) &&
                approx(alpha(radial, beyond[k][0], beyond[k][1]), 0),
            "A radial ramp does not shrink the dots with the distance");
  require(approx(alpha(radial, 228, 128), 0) &&
              approx(alpha(radial, 128, 228), 0),
          "A radial ramp's end gave a dot");

  // A tile is that part of the whole frame, turned and scaled too
  const TRasterPT<Pixel> part =
      renderTile<Pixel>(fx, settings, TPointD(-128 + 37, -128 + 141), 53, 29);
  for (int y = 0; y < 29; ++y)
    for (int x = 0; x < 53; ++x)
      require(part->pixels(y)[x] == radial->pixels(y + 141)[x + 37],
              "A ramp's tile differs from the whole frame");
  settings.m_affine = TRotation(30) * TScale(1.5);
  const TRasterPT<Pixel> turned =
      renderTile<Pixel>(fx, settings, TPointD(-192, -192), 384, 384);
  const TRasterPT<Pixel> corner =
      renderTile<Pixel>(fx, settings, TPointD(-192 + 211, -192 + 59), 47, 31);
  for (int y = 0; y < 31; ++y)
    for (int x = 0; x < 47; ++x) {
      const TPixelD a = pixelOf(corner, x, y),
                    b = pixelOf(turned, x + 211, y + 59);
      require(approx(a.r, b.r, 1e-6) && approx(a.m, b.m, 1e-6),
              "A turned ramp's tile differs from the whole frame");
    }
}

// The FX draws under rotation and uniform scale; the renderer applies any
// other transform to its result
void testHandledTransforms() {
  TFxP holder = new DotScreenFx;
  auto &fx    = static_cast<DotScreenFx &>(*holder);
  const std::vector<TAffine> handled = {
      TAffine(), TScale(2), TRotation(30) * TScale(1.5),
      TTranslation(3.5, -2) * TRotation(-75)};
  const std::vector<TAffine> passed = {TScale(2, 1),
                                       TScale(1, 0.5) * TRotation(30),
                                       TShear(0.3, 0), TScale(-1, 1)};
  TRenderSettings settings;
  for (const TAffine &affine : handled) {
    settings.m_affine = affine;
    require(fx.canHandle(settings, 0),
            "Dot Screen does not draw under rotation and uniform scale");
  }
  for (const TAffine &affine : passed) {
    settings.m_affine = affine;
    require(!fx.canHandle(settings, 0),
            "Dot Screen draws under a non-uniform transform");
  }
}
}  // namespace

int main(int argc, char **argv) {
  QCoreApplication app(argc, argv);
  try {
    testFx<TPixel32>();
    testFx<TPixel64>();
    testFx<TPixelF>();
    testRotationModifier<TPixel32>();
    testRotationModifier<TPixelF>();
    testRamps<TPixel32>();
    testRamps<TPixel64>();
    testRamps<TPixelF>();
    testHandledTransforms();
    std::cout << "PASS: dot screen plain dots, halftone, tiles, invert, scale, "
                 "rotation, transparent input, rotation modifier, ramps, "
                 "handled transforms, cache keys and pixel depths\n";
    return 0;
  } catch (const TException &error) {
    std::cerr << QString::fromStdWString(error.getMessage()).toStdString()
              << '\n';
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
  }
  return 1;
}
