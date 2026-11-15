#include "../roughstrokefx.cpp"
#include "tparamcontainer.h"
#include "tfxutil.h"
#include "toonz/toonzscene.h"
#include "toonz/txshleveltypes.h"
#include "toonz/txshsimplelevel.h"

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

// A vector level in a column, holding one straight stroke of half width 1
// from (-50, 0) to (50, 0), in a red style
class VectorLevel {
  ToonzScene m_scene;
  TXshLevelP m_level;
  TXshLevelColumn m_column;

public:
  VectorLevel() : m_level(new TXshSimpleLevel(L"rough-stroke-fixture")) {
    auto *simple = m_level->getSimpleLevel();
    simple->setType(PLI_XSHLEVEL);
    simple->setScene(&m_scene);
    simple->setPath(TFilePath("rough-stroke-fixture.pli"));
    TVectorImageP image = new TVectorImage;
    TPalette *palette   = new TPalette;
    palette->setStyle(1, TPixel32(204, 26, 51));
    image->setPalette(palette);
    std::vector<TThickPoint> points = {TThickPoint(-50, 0, 1),
                                       TThickPoint(0, 0, 1),
                                       TThickPoint(50, 0, 1)};
    TStroke *stroke = new TStroke(points);
    stroke->setStyle(1);
    image->addStroke(stroke);
    simple->setFrame(TFrameId(1), image);
    if (!m_column.setCell(0, TXshCell(m_level, TFrameId(1))))
      throw std::runtime_error("Cannot create the vector level fixture");
  }
  TFxP fx() { return m_column.getFx(); }
};

void set(TFx &fx, const std::string &name, double value) {
  TDoubleParamP param = TParamP(fx.getParams()->getParam(name));
  require(bool(param), "No numeric parameter " + name);
  param->setDefaultValue(value);
}

template <class Pixel>
void testFx() {
  VectorLevel level;
  TFxP holder = new RoughStrokeFx;
  auto &fx    = static_cast<RoughStrokeFx &>(*holder);
  TFxP source = level.fx();
  require(fx.connect("Source", source.getPointer()), "Cannot connect Source");
  // A plain stroke: the drawn half width 1 plus half of a width of 2
  for (const char *name : {"inLength", "outLength", "drift"}) set(fx, name, 0);
  set(fx, "width", 2);
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

  // Output pixel (x, y) is centred on world (x - 63.5, y - 15.5)
  const TRasterPT<Pixel> whole = render(TPointD(-64, -16), 128, 32, 0);
  require(approx(pixel(whole, 64, 16).m, 1) && approx(pixel(whole, 64, 17).m, 1),
          "The stroke is missing");
  require(approx(pixel(whole, 64, 18).m, 0), "The stroke is wider than asked");
  require(approx(pixel(whole, 5, 16).m, 0), "The stroke passed its start");
  require(approx(pixel(whole, 64, 16).r, 204.0 / 255, 3e-3) &&
              approx(pixel(whole, 64, 16).g, 26.0 / 255, 3e-3),
          "The stroke did not take its style's colour");
  const TRasterPT<Pixel> part = render(TPointD(-64 + 31, -16 + 7), 43, 15, 0);
  for (int y = 0; y < 15; ++y)
    for (int x = 0; x < 43; ++x)
      require(part->pixels(y)[x] == whole->pixels(y + 7)[x + 31],
              "An FX tile differs from the whole frame");

  // Half revealed: the first half of the stroke only
  set(fx, "revealEnd", 0.5);
  const TRasterPT<Pixel> half = render(TPointD(-64, -16), 128, 32, 0);
  require(approx(pixel(half, 43, 16).m, 1) && approx(pixel(half, 84, 16).m, 0),
          "Half a reveal did not draw the first half only");
  // The stretch travels by Speed each frame, and the cache key follows it
  require(fx.getAlias(0, settings) == fx.getAlias(1, settings),
          "A still stroke has a different cache key on another frame");
  set(fx, "hold", 0);
  set(fx, "speed", 0.25);
  require(fx.getAlias(0, settings) != fx.getAlias(1, settings),
          "A travelling stroke shares a cache key across frames");
  const TRasterPT<Pixel> moved = render(TPointD(-64, -16), 128, 32, 2);
  require(approx(pixel(moved, 43, 16).m, 0) && approx(pixel(moved, 84, 16).m, 1),
          "The revealed stretch did not travel");
  set(fx, "speed", 0);
  set(fx, "revealEnd", 1);

  // Twice the scale: twice the width in pixels
  settings.m_affine              = TScale(2);
  const TRasterPT<Pixel> scaled = render(TPointD(-128, -32), 256, 64, 0);
  require(approx(pixel(scaled, 128, 35).m, 1) &&
              approx(pixel(scaled, 128, 37).m, 0),
          "A scaled stroke is not the width asked for");
  settings.m_affine = TAffine();

  // A transform between the level and the FX moves the strokes with it
  TFxP placed = TFxUtil::makeAffine(source, TTranslation(0, 10));
  require(fx.connect("Source", placed.getPointer()), "Cannot connect transform");
  const TRasterPT<Pixel> lifted = render(TPointD(-64, -16), 128, 32, 0);
  require(approx(pixel(lifted, 64, 26).m, 1) && approx(pixel(lifted, 64, 16).m, 0),
          "The strokes did not follow the level's transform");
}
}  // namespace

int main(int argc, char **argv) {
  QCoreApplication app(argc, argv);
  try {
    testFx<TPixel32>();
    testFx<TPixel64>();
    testFx<TPixelF>();
    std::cout << "PASS: rough stroke place, colour, tiles, reveal, travel, "
                 "scale, transforms and pixel depths\n";
    return 0;
  } catch (const TException &error) {
    std::cerr << QString::fromStdWString(error.getMessage()).toStdString()
              << '\n';
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
  }
  return 1;
}
