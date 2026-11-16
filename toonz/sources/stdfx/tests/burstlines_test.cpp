#include "../burstlinesfx.cpp"
#include "comiclinesfx_testutil.h"

#include <QCoreApplication>
#include <iostream>

namespace {
using namespace ComicLinesFxTest;

// One 6-unit line along -x, from radius 300 in to radius 50
void prepare(TFx &fx) {
  singleLine(fx, 250, 6);
  setPoint(fx, "center", TPointD(0, 0));
  set(fx, "innerRadius", 50);
  set(fx, "outerRadius", 300);
}

template <class Pixel>
void testFx() {
  TFxP holder = new BurstLinesFx;
  auto &fx    = static_cast<BurstLinesFx &>(*holder);
  prepare(fx);
  const TRenderSettings settings = settingsFor<Pixel>();

  // Output pixel (x, y) is centred on world (x - 319.5, y - 15.5)
  const TRasterPT<Pixel> whole =
      render<Pixel>(fx, TPointD(-320, -16), 640, 32, 0, settings);
  require(approx(alpha(whole, 120, 16), 1), "The burst line is missing");
  require(approx(alpha(whole, 520, 16), 0), "A line drew on the far side");
  require(approx(alpha(whole, 300, 16), 0), "The centre hole is not empty");
  require(approx(alpha(whole, 5, 16), 0), "A line passed the outer radius");
  require(approx(alpha(whole, 120, 24), 0), "The line is wider than asked");
  requireSameTile(whole,
                  render<Pixel>(fx, TPointD(-320 + 101, -16 + 9), 53, 13, 0,
                                settings),
                  101, 9);

  // Falloff thins the line towards the centre
  set(fx, "falloff", 1);
  const TRasterPT<Pixel> thinned =
      render<Pixel>(fx, TPointD(-320, -16), 640, 32, 0, settings);
  require(approx(alpha(thinned, 30, 17), 1) &&
              approx(alpha(thinned, 220, 18), 0),
          "Falloff did not thin the line towards the centre");
  set(fx, "falloff", 0);

  // A quarter turn of twist moves the inner end off the -x axis
  set(fx, "twist", 90);
  const TRasterPT<Pixel> twisted =
      render<Pixel>(fx, TPointD(-320, -16), 640, 32, 0, settings);
  require(approx(alpha(twisted, 260, 16), 0), "Twist did not turn the line");
  set(fx, "twist", 0);

  require(fx.getAlias(0, settings) == fx.getAlias(5, settings),
          "Still lines have a different cache key on another frame");
  set(fx, "speed", 5);
  require(fx.getAlias(0, settings) != fx.getAlias(1, settings),
          "Travelling lines share a cache key across frames");
}
}  // namespace

int main(int argc, char **argv) {
  QCoreApplication app(argc, argv);
  try {
    testFx<TPixel32>();
    testFx<TPixel64>();
    testFx<TPixelF>();
    std::cout << "PASS: burst lines place, centre hole, tiles, falloff, twist, "
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
