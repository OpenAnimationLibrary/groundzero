#include "../motionlinesfx.cpp"
#include "comiclinesfx_testutil.h"

#include <QCoreApplication>
#include <iostream>

namespace {
using namespace ComicLinesFxTest;

// One 4-unit line from x = -50 to x = 50 on the x axis; the band runs on to
// x = 350
void prepare(TFx &fx) {
  singleLine(fx, 100, 4);
  setPoint(fx, "start", TPointD(-50, 0));
  setPoint(fx, "end", TPointD(350, 0));
  set(fx, "width", 200);
}

template <class Pixel>
void testFx() {
  TFxP holder = new MotionLinesFx;
  auto &fx    = static_cast<MotionLinesFx &>(*holder);
  prepare(fx);
  TRenderSettings settings = settingsFor<Pixel>();

  // Output pixel (x, y) is centred on world (x - 63.5, y - 31.5)
  const TRasterPT<Pixel> whole =
      render<Pixel>(fx, TPointD(-64, -32), 128, 64, 0, settings);
  require(approx(alpha(whole, 64, 32), 1), "The line's centre is not opaque");
  require(approx(alpha(whole, 64, 38), 0), "The line is wider than asked");
  require(approx(alpha(whole, 120, 32), 0), "The line is longer than asked");
  requireSameTile(whole,
                  render<Pixel>(fx, TPointD(-64 + 37, -32 + 21), 29, 17, 0,
                                settings),
                  37, 21);

  // The cache key follows what changes the picture, and nothing else
  require(fx.getAlias(0, settings) == fx.getAlias(5, settings),
          "Still lines have a different cache key on another frame");
  set(fx, "hold", 3);
  require(fx.getAlias(0, settings) == fx.getAlias(2, settings),
          "Frames of one hold have different cache keys");
  require(fx.getAlias(2, settings) != fx.getAlias(3, settings),
          "A new hold reuses the previous cache key");
  set(fx, "hold", 0);
  set(fx, "speed", 10);
  require(fx.getAlias(0, settings) != fx.getAlias(1, settings),
          "Travelling lines share a cache key across frames");

  // After 3 frames at 10 units a frame the line covers x = -20 to 80
  const TRasterPT<Pixel> moved =
      render<Pixel>(fx, TPointD(-64, -32), 160, 64, 3, settings);
  require(approx(alpha(moved, 24, 32), 0), "The line left a trail behind it");
  require(approx(alpha(moved, 124, 32), 1), "The line did not travel");
  set(fx, "speed", 0);

  // Twice the scale: twice the width in pixels, the same place in the world
  settings.m_affine = TScale(2);
  const TRasterPT<Pixel> scaled =
      render<Pixel>(fx, TPointD(-128, -64), 256, 128, 0, settings);
  require(approx(alpha(scaled, 129, 67), 1), "A scaled line is too thin");
  require(approx(alpha(scaled, 129, 70), 0), "A scaled line is too thick");
  settings.m_affine = TAffine();

  // Tiling repeats the band: world x = 374.5 is 24.5 into the next band
  const TRasterPT<Pixel> outside =
      render<Pixel>(fx, TPointD(370, -4), 8, 8, 0, settings);
  require(approx(alpha(outside, 4, 4), 0), "The band repeated without Tile");
  TBoolParamP tile = TParamP(fx.getParams()->getParam("tile"));
  require(bool(tile), "No Tile parameter");
  tile->setValue(true);
  const TRasterPT<Pixel> repeated =
      render<Pixel>(fx, TPointD(370, -4), 8, 8, 0, settings);
  require(approx(alpha(repeated, 4, 4), 1), "Tile did not repeat the band");
}
}  // namespace

int main(int argc, char **argv) {
  QCoreApplication app(argc, argv);
  try {
    testFx<TPixel32>();
    testFx<TPixel64>();
    testFx<TPixelF>();
    std::cout << "PASS: motion lines place, tiles, cache keys, travel, scale, "
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
