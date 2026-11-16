#include "../roughstrokefx.cpp"
#include "tdoublekeyframe.h"
#include "tparamcontainer.h"
#include "tfxutil.h"
#include "toonz/scenefx.h"
#include "toonz/sceneproperties.h"
#include "toonz/toonzscene.h"
#include "toonz/txsheet.h"
#include "toonz/txshleveltypes.h"
#include "toonz/txshsimplelevel.h"

#include <QCoreApplication>
#include <cmath>
#include <functional>
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

// A straight stroke from (-50, y) to (50, y) in style 1
TStroke *stroke(double y, double thickness) {
  std::vector<TThickPoint> points = {TThickPoint(-50, y, thickness),
                                     TThickPoint(0, y, thickness),
                                     TThickPoint(50, y, thickness)};
  TStroke *drawn                  = new TStroke(points);
  drawn->setStyle(1);
  return drawn;
}

// A vector level in the first column of a scene, on rows 0 to 15, holding
// one straight stroke of half width 1 from (-50, 0) to (50, 0), in a red
// style, and what draw adds to the drawing
class VectorLevel {
  ToonzScene m_scene;
  TXshLevelP m_level;
  TXshLevelColumnP m_column;
  TPalette *m_palette;

public:
  using Drawing = std::function<void(TVectorImage &)>;
  explicit VectorLevel(const Drawing &draw = Drawing())
      : m_level(new TXshSimpleLevel(L"rough-stroke-fixture"))
      , m_column(new TXshLevelColumn)
      , m_palette(new TPalette) {
    auto *simple = m_level->getSimpleLevel();
    simple->setType(PLI_XSHLEVEL);
    simple->setScene(&m_scene);
    simple->setPath(TFilePath("rough-stroke-fixture.pli"));
    TVectorImageP image = new TVectorImage;
    // As in a scene, the drawing takes its level's palette
    m_palette->setStyle(1, TPixel32(204, 26, 51));
    simple->setPalette(m_palette);
    image->setPalette(m_palette);
    image->addStroke(stroke(0, 1));
    if (draw) draw(*image);
    simple->setFrame(TFrameId(1), image);
    m_scene.getXsheet()->insertColumn(0, m_column.getPointer());
    for (int row = 0; row < 16; ++row)
      if (!m_column->setCell(row, TXshCell(m_level, TFrameId(1))))
        throw std::runtime_error("Cannot create the vector level fixture");
  }
  TFxP fx() { return m_column->getFx(); }
  TXshLevelColumn &column() { return *m_column; }
  ToonzScene &scene() { return m_scene; }
  TPalette &palette() { return *m_palette; }
};

void set(TFx &fx, const std::string &name, double value) {
  TDoubleParamP param = TParamP(fx.getParams()->getParam(name));
  require(bool(param), "No numeric parameter " + name);
  param->setDefaultValue(value);
}

// A plain stroke: the drawn half width 1 plus half of a width of 2
void plain(TFx &fx) {
  for (const char *name : {"inLength", "outLength", "drift"}) set(fx, name, 0);
  set(fx, "width", 2);
}

template <class Pixel>
TRenderSettings settingsFor() {
  TRenderSettings settings;
  settings.m_bpp = std::is_same<Pixel, TPixelF>::value    ? 128
                   : std::is_same<Pixel, TPixel64>::value ? 64
                                                          : 32;
  return settings;
}

template <class Pixel>
TRasterPT<Pixel> render(RoughStrokeFx &fx, const TRenderSettings &settings,
                        TPointD pos, int w, int h, double frame) {
  TRasterPT<Pixel> raster(w, h);
  TTile tile(raster, pos);
  fx.doCompute(tile, frame, settings);
  return raster;
}

// Output pixel (x, y) is centred on world (x - 63.5, y - 15.5)
template <class Pixel>
TRasterPT<Pixel> renderWhole(RoughStrokeFx &fx, double frame = 0) {
  return render<Pixel>(fx, settingsFor<Pixel>(), TPointD(-64, -16), 128, 32,
                       frame);
}

template <class Pixel>
TPixelD pixel(const TRasterPT<Pixel> &raster, int x, int y) {
  return PixelConverter<TPixelD>::from(raster->pixels(y)[x]);
}

template <class Pixel>
void testFx() {
  VectorLevel level;
  TFxP holder = new RoughStrokeFx;
  auto &fx    = static_cast<RoughStrokeFx &>(*holder);
  TFxP source = level.fx();
  require(fx.connect("Source", source.getPointer()), "Cannot connect Source");
  plain(fx);
  TRenderSettings settings = settingsFor<Pixel>();
  auto render              = [&](TPointD pos, int w, int h, double frame) {
    return ::render<Pixel>(fx, settings, pos, w, h, frame);
  };

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

// What the drawing says about its strokes: those a render hides, their
// opacity, and the colours of an animated palette
template <class Pixel>
void testDrawing() {
  // A plain colour stroke without thickness at y = 8, hidden in a render,
  // and the stroke at half opacity
  VectorLevel level([](TVectorImage &image) {
    image.addStroke(stroke(8, 0));
    image.getStroke(0)->setOpacity(0.5);
  });
  // The stroke fading from opaque to transparent along it
  VectorLevel faded([](TVectorImage &image) {
    require(
        image.getStroke(0)->setOpacityProfile({TPointD(0, 1), TPointD(1, 0)}),
        "Cannot fade the stroke");
  });
  // The style red at frame 0 and blue from frame 12 on
  VectorLevel animated([](TVectorImage &image) {
    TPalette *palette = image.getPalette();
    palette->getStyle(1)->setMainColor(TPixel32(0, 0, 255));
    palette->setKeyframe(1, 12);
    palette->getStyle(1)->setMainColor(TPixel32(204, 26, 51));
    palette->setKeyframe(1, 0);
  });
  TFxP holder = new RoughStrokeFx;
  auto &fx    = static_cast<RoughStrokeFx &>(*holder);
  plain(fx);

  require(fx.connect("Source", level.fx().getPointer()), "Cannot connect");
  const TRasterPT<Pixel> out = renderWhole<Pixel>(fx);
  require(approx(pixel(out, 64, 24).m, 0) && approx(pixel(out, 64, 23).m, 0),
          "A stroke hidden in a render was drawn");
  require(approx(pixel(out, 64, 16).m, 0.5, 3e-3) &&
              approx(pixel(out, 64, 16).r, 0.5 * 204 / 255, 3e-3),
          "The stroke's opacity is not applied");

  require(fx.connect("Source", faded.fx().getPointer()), "Cannot connect");
  const TRasterPT<Pixel> fade = renderWhole<Pixel>(fx);
  // Pixel 24 is 10.5 along the stroke, pixel 104 90.5 along; a pixel takes
  // the opacity of a vertex up to a step before it
  require(approx(pixel(fade, 24, 16).m, 0.895, 0.025) &&
              approx(pixel(fade, 104, 16).m, 0.095, 0.025),
          "The stroke's opacity along it is not applied");

  // The colour at the frame drawn, with the palette left at its own frame
  require(fx.connect("Source", animated.fx().getPointer()), "Cannot connect");
  const int paletteFrame      = animated.palette().getFrame();
  const TRasterPT<Pixel> blue = renderWhole<Pixel>(fx, 14);
  require(approx(pixel(blue, 64, 16).b, 1, 3e-3) &&
              approx(pixel(blue, 64, 16).r, 0, 3e-3),
          "An animated style did not take its colour at the frame");
  const TRasterPT<Pixel> red = renderWhole<Pixel>(fx, 0);
  require(approx(pixel(red, 64, 16).r, 204.0 / 255, 3e-3) &&
              approx(pixel(red, 64, 16).b, 51.0 / 255, 3e-3),
          "An animated style did not take its colour at frame 0");
  require(animated.palette().getFrame() == paletteFrame &&
              animated.palette().getStyle(1)->getMainColor() ==
                  TPixel32(204, 26, 51),
          "Rough Stroke changed the palette");
}

// The render puts the column's colour filter and opacity, its Colour Effect
// and its Multiplane Depth between the level and the FX: Rough Stroke reads
// the strokes through them, and applies the colours. Any other FX hides the
// level.
template <class Pixel>
void testColumnNodes() {
  VectorLevel level;
  TFxP holder = new RoughStrokeFx;
  auto &fx    = static_cast<RoughStrokeFx &>(*holder);
  require(fx.connect("Source", level.fx().getPointer()), "Cannot connect");
  plain(fx);
  auto build = [&](const char *below) {
    TFxP built = buildSceneFx(&level.scene(), level.scene().getXsheet(), 0,
                              holder, false);
    require(dynamic_cast<RoughStrokeFx *>(built.getPointer()) &&
                built->getInputPort(0)->getFx() &&
                built->getInputPort(0)->getFx()->getFxType() == below,
            std::string("The render did not put ") + below + " below");
    return built;
  };

  // At half opacity through the red filter
  level.scene().getProperties()->enableColumnColorFilterOnRender(true);
  level.column().setColorFilterId(1);
  level.column().setOpacity(128);
  TFxP filtered     = build("columnColorFilterFx");
  const TPixelD a   = pixel(
      renderWhole<Pixel>(static_cast<RoughStrokeFx &>(*filtered)), 64, 16);
  const double half = 128.0 / 255;
  require(approx(a.m, half, 3e-3), "The column's opacity is not applied");
  require(approx(a.r, half, 3e-3) && approx(a.g, 26.0 / 255 * half, 3e-3) &&
              approx(a.b, 51.0 / 255 * half, 3e-3),
          "The column's colour filter is not applied");

  // And at half alpha in the column's Colour Effect
  level.column()
      .getLevelColumnFx()
      ->getColorEffectParam(ColumnColorEffect::Alpha)
      ->setDefaultValue(50);
  TFxP effected   = build("columnColorEffectFx");
  const TPixelD b = pixel(
      renderWhole<Pixel>(static_cast<RoughStrokeFx &>(*effected)), 64, 16);
  require(approx(b.m, half / 2, 3e-3) && approx(b.r, half / 2, 3e-3),
          "The column's Colour Effect is not applied");

  // The depth's haze is seen through, though not applied
  const double white[3] = {1, 1, 1};
  TFxP hazed            = TFxUtil::makeColumnDepth(level.fx(), 0.5, 0, white);
  require(fx.connect("Source", hazed.getPointer()), "Cannot connect depth");
  require(approx(pixel(renderWhole<Pixel>(fx), 64, 16).m, 1),
          "The strokes are not read through the depth");

  // Another FX between the level and Rough Stroke: the source passes
  TFxP innerHolder = new RoughStrokeFx;
  auto &inner      = static_cast<RoughStrokeFx &>(*innerHolder);
  require(inner.connect("Source", level.fx().getPointer()) &&
              fx.connect("Source", innerHolder.getPointer()),
          "Cannot connect Rough Stroke to Rough Stroke");
  set(fx, "width", 40);
  const TRenderSettings settings = settingsFor<Pixel>();
  TRectD innerBox, outerBox;
  require(inner.doGetBBox(0, innerBox, settings) &&
              fx.doGetBBox(0, outerBox, settings) && outerBox == innerBox,
          "The strokes were read through another FX");
}

// What does not depend on the pixels
void testSettings() {
  VectorLevel level;
  TFxP holder = new RoughStrokeFx;
  auto &fx    = static_cast<RoughStrokeFx &>(*holder);
  TRenderSettings info;
  const TRectD tile(0, 0, 1024, 1024);
  require(fx.getMemoryRequirement(tile, 0, info) == 0,
          "An FX without source needs memory");
  require(fx.connect("Source", level.fx().getPointer()), "Cannot connect");
  // The strokes and a copy's window as doubles: 72 bytes a pixel
  require(fx.getMemoryRequirement(tile, 0, info) == 72,
          "The memory estimate is not that of the strokes and a copy");

  // A stretch is left to the render; rotation and an even scale are drawn
  info.m_affine = TScale(2, 1);
  require(!fx.canHandle(info, 0), "A stretch is drawn as if even");
  info.m_affine = TRotation(30) * TScale(3);
  require(fx.canHandle(info, 0), "A rotation is not drawn");

  // A keyed Speed falling from 0.25 to 0 at frame 8: the stretch slows down
  // and stops, and never moves back
  TDoubleParamP speed = TParamP(fx.getParams()->getParam("speed"));
  TDoubleKeyframe slow(0, 0.25), stop(8, 0);
  slow.m_type = stop.m_type = TDoubleKeyframe::Linear;
  speed->setKeyframe(slow);
  speed->setKeyframe(stop);
  double before = fx.settings(0, 1).travel;
  for (double frame = 0.5; frame <= 16; frame += 0.5) {
    const double travel = fx.settings(frame, 1).travel;
    require(travel >= before, "A slowing stretch moved back");
    before = travel;
  }
  require(approx(fx.settings(12, 1).travel, 0.25 * (8 - 28 / 8.0), 1e-9),
          "A keyed speed is not added up frame by frame");
}
}  // namespace

int main(int argc, char **argv) {
  QCoreApplication app(argc, argv);
  try {
    testFx<TPixel32>();
    testFx<TPixel64>();
    testFx<TPixelF>();
    testDrawing<TPixel32>();
    testDrawing<TPixel64>();
    testDrawing<TPixelF>();
    testColumnNodes<TPixel32>();
    testColumnNodes<TPixel64>();
    testColumnNodes<TPixelF>();
    testSettings();
    std::cout << "PASS: rough stroke place, colour, tiles, reveal, travel, "
                 "scale, transforms, pixel depths, hidden strokes, opacity, "
                 "animated palettes, column nodes, memory and keyed speed\n";
    return 0;
  } catch (const TException &error) {
    std::cerr << QString::fromStdWString(error.getMessage()).toStdString()
              << '\n';
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
  }
  return 1;
}
