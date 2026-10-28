#include "../mosaicfx.cpp"
#include "../celfxsourcegrid.h"
#include "toonz/toonzscene.h"
#include "toonz/txshcell.h"
#include "toonz/txshlevelcolumn.h"
#include "toonz/txshleveltypes.h"
#include "toonz/txshsimplelevel.h"
#include "trasterimage.h"
#include "ttoonzimage.h"
#include "tropcm.h"
#include "tpixelutils.h"
#include "tstroke.h"
#include "tvectorimage.h"
#include "tfxcachemanager.h"
#include "tfxutil.h"
#include "trenderer.h"
#include "tthread.h"

#include <QCoreApplication>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <type_traits>
#include <vector>

namespace {
void require(bool condition, const char *message) {
  if (!condition) throw std::runtime_error(message);
}

// Replace only the pixel operation, retaining a production effect's affine
// policy. This observes host routing without depending on its image algorithm.
class GridProbe final : public TStandardRasterFx {
  TRasterFxP m_policy;
  TAffine m_grid;
  bool m_custom;
public:
  std::vector<TAffine> computed, predicted;
  GridProbe(const TAffine &grid, bool custom, TRasterFx *policy = nullptr)
      : m_policy(policy), m_grid(grid), m_custom(custom) {}
  const TPersistDeclaration *getDeclaration() const override { return nullptr; }
  bool usesCustomRenderGrid() const override {
    return m_policy ? m_policy->usesCustomRenderGrid() : m_custom;
  }
  bool canHandle(const TRenderSettings &settings, double frame) override {
    return m_policy ? m_policy->canHandle(settings, frame) : false;
  }
  TAffine handledAffine(const TRenderSettings &settings, double frame) override {
    return m_policy ? m_policy->handledAffine(settings, frame) : m_grid;
  }
  std::string getAlias(double, const TRenderSettings &) const override {
    return "render-grid-probe";
  }
  bool doGetBBox(double, TRectD &box, const TRenderSettings &) override {
    box = TRectD(-32, -32, 32, 32);
    return true;
  }
  void doCompute(TTile &tile, double, const TRenderSettings &settings) override {
    computed.push_back(settings.m_affine);
    TRaster32P raster = tile.getRaster();
    require(bool(raster), "Unexpected probe precision");
    raster->fill(TPixel32(240, 80, 40, 255));
  }
  void doDryCompute(TRectD &, double, const TRenderSettings &settings) override {
    predicted.push_back(settings.m_affine);
  }
};

void checkRoute(GridProbe &probe, const TAffine &request,
                const TAffine &expected) {
  TRenderer renderer(1);
  const unsigned long id = TRenderer::buildRenderId();
  renderer.install(id);
  renderer.declareRenderStart(id);
  renderer.declareFrameStart(0);
  try {
    TRenderSettings settings;
    settings.m_bpp = 32;
    settings.m_affine = request;
    settings.m_cameraBox = TRectD(-8, -8, 8, 8);
    TRaster32P raster(16, 16);
    raster->clear();
    TTile tile(raster, TPointD(-8, -8));
    probe.compute(tile, 0, settings);
    require(!probe.computed.empty(), "Host skipped the pixel operation");
    for (const auto &affine : probe.computed)
      require(affine == expected, "Compute selected the wrong render grid");
    require(raster->pixels(8)[8].m > 0, "Transform discarded the rendered image");
    TRectD rect(-8, -8, 8, 8);
    // The direct renderer fixture drives the predictive cache phase explicitly.
    TFxCacheManager::instance()->onRenderStatusStart(TRenderer::FIRSTRUN);
    probe.dryCompute(rect, 0, settings);
    TFxCacheManager::instance()->onRenderStatusEnd(TRenderer::FIRSTRUN);
    TFxCacheManager::instance()->onRenderStatusStart(TRenderer::IDLE);
    require(!probe.predicted.empty(), "Host skipped the dry operation");
    for (const auto &affine : probe.predicted)
      require(affine == expected, "Dry compute selected a different render grid");
  } catch (...) {
    renderer.declareFrameEnd(0);
    renderer.declareRenderEnd(id);
    renderer.uninstall();
    throw;
  }
  renderer.declareFrameEnd(0);
  renderer.declareRenderEnd(id);
  renderer.uninstall();
}

void testOptIn() {
  // An identity caller still needs the native grid. A canonical fractional
  // translation also verifies that re-entry through TrFx terminates.
  for (const auto &grid : std::vector<TAffine>{TScale(4, 2),
                          TTranslation(.25, .5) * TScale(4, 2)}) {
    TFxP owner = new GridProbe(grid, true);
    checkRoute(*static_cast<GridProbe *>(owner.getPointer()), TAffine(), grid);
  }
  TFxP legacy = new GridProbe(TScale(2), false);
  checkRoute(*static_cast<GridProbe *>(legacy.getPointer()), TAffine(), TAffine());
  TFxP transformed = new GridProbe(TScale(2), false);
  checkRoute(*static_cast<GridProbe *>(transformed.getPointer()), TScale(3), TScale(2));
}

void testMosaicCompatibility() {
  TFxP policy = new MosaicFx;
  TDoubleParamP size(TParamP(policy->getParams()->getParam("size")));
  TDoubleParamP distance(TParamP(policy->getParams()->getParam("distance")));
  size->setDefaultValue(1.25);
  distance->setDefaultValue(0);
  auto *rasterPolicy = static_cast<TRasterFx *>(policy.getPointer());
  require(rasterPolicy->handledAffine(TRenderSettings(), 0) != TAffine(),
          "Mosaic fixture must select a fractional cell grid");
  require(!rasterPolicy->usesCustomRenderGrid(), "Mosaic unexpectedly opted in");
  TFxP owner = new GridProbe(TAffine(), false, rasterPolicy);
  checkRoute(*static_cast<GridProbe *>(owner.getPointer()), TAffine(), TAffine());
}

void testLevelCompatibility() {
  ToonzScene scene;
  TXshLevelP level = new TXshSimpleLevel(L"render-grid-fixture");
  auto *simple = level->getSimpleLevel();
  simple->setType(OVL_XSHLEVEL);
  simple->setScene(&scene);
  TRaster32P image(9, 7);
  image->fill(TPixel32::White);
  simple->setFrame(TFrameId(1), TRasterImageP(image));
  TXshLevelColumn column;
  require(column.setCell(0, TXshCell(level, TFrameId(1))), "Cannot create level fixture");
  auto *policy = static_cast<TRasterFx *>(column.getFx());
  require(!policy->canHandle(TRenderSettings(), 0), "Raster level fixture is inactive");
  require(policy->handledAffine(TRenderSettings(), 0) == TTranslation(-.5, -.5),
          "Odd raster level must retain its half-pixel policy");
  require(!policy->usesCustomRenderGrid(), "Level column unexpectedly opted in");
  policy->setName(L"LevelColumn AffineFx");
  require(CelFxSourceGrid::renderAffine(policy, 0) == TTranslation(.5, .5),
          "Odd raster level lost its native pixel phase");
  TFxP owner = new GridProbe(TAffine(), false, policy);
  checkRoute(*static_cast<GridProbe *>(owner.getPointer()), TAffine(), TAffine());
}

void testVectorLevelGrid() {
  ToonzScene scene;
  TXshLevelP level = new TXshSimpleLevel(L"vector-grid-fixture");
  auto *simple = level->getSimpleLevel();
  simple->setType(PLI_XSHLEVEL);
  simple->setScene(&scene);
  simple->setPalette(new TPalette);
  TVectorImageP image(new TVectorImage());
  const std::vector<TThickPoint> points{
      TThickPoint(-3.125, -1.375, .5), TThickPoint(1.375, 2.625, .5),
      TThickPoint(5.875, -1.375, .5)};
  auto *stroke = new TStroke(points);
  stroke->setStyle(1);
  image->addStroke(stroke);
  simple->setFrame(TFrameId(1), image);
  TXshLevelColumn column;
  require(column.setCell(0, TXshCell(level, TFrameId(1))), "Cannot create vector level fixture");
  auto *source = static_cast<TRasterFx *>(column.getFx());
  // The scene builder applies this marker even when vector DPI is identity.
  source->setName(L"LevelColumn AffineFx");
  TRenderSettings settings;
  settings.m_affine = TRotation(17) * TScale(2, 3);
  require(source->canHandle(settings, 0), "Vector level lost its continuous affine policy");
  settings.m_getFullSizeBBox = true;
  TRectD bounds;
  require(source->doGetBBox(0, bounds, settings) && !bounds.isEmpty() &&
              (bounds.x0 != std::floor(bounds.x0) || bounds.y0 != std::floor(bounds.y0)),
          "Vector fixture must have fractional continuous bounds");
  const TAffine grid = CelFxSourceGrid::renderAffine(source, 0);
  require(grid == TAffine(), "Vector artwork bounds were mistaken for a native raster canvas");
  TFxP placed = TFxUtil::makeAffine(TFxP(source), TTranslation(2.5, -.25) * TScale(2, 3));
  require(CelFxSourceGrid::renderAffine(placed.getPointer(), 0) == TAffine(),
          "Placed vector level stopped using the world pixel grid");
  TFxP owner = new GridProbe(grid, true);
  checkRoute(*static_cast<GridProbe *>(owner.getPointer()), TAffine(), TAffine());
}

class RasterLevel {
  ToonzScene m_scene;
  TXshLevelP m_level;
  TXshLevelColumn m_column;
public:
  explicit RasterLevel(const TImageP &image, int type = OVL_XSHLEVEL,
                       TPalette *palette = nullptr)
      : m_level(new TXshSimpleLevel(L"raster-tile-fixture")) {
    auto *simple = m_level->getSimpleLevel();
    simple->setType(type);
    simple->setScene(&m_scene);
    simple->setPalette(palette);
    static unsigned int identity = 0;
    simple->setPath(TFilePath("raster-tile-fixture-" + std::to_string(++identity) +
                              (type == OVL_XSHLEVEL ? ".png" : ".tlv")));
    simple->setFrame(TFrameId(1), image);
    require(m_column.setCell(0, TXshCell(m_level, TFrameId(1))),
            "Cannot create real raster level");
  }
  TRasterFx &fx() { return *static_cast<TRasterFx *>(m_column.getFx()); }
};

struct RasterRenderContext {
  TRenderer renderer;
  unsigned long id;
  RasterRenderContext() : renderer(1), id(TRenderer::buildRenderId()) {
    renderer.install(id); renderer.declareRenderStart(id); renderer.declareFrameStart(0);
  }
  ~RasterRenderContext() {
    renderer.declareFrameEnd(0); renderer.declareRenderEnd(id); renderer.uninstall();
  }
};

template <class Pixel>
TRasterPT<Pixel> renderLevel(TRasterFx &fx, TRenderSettings settings,
                            const TDimension &size, const TPointD &origin,
                            int columns, int rows, bool linear = false) {
  RasterRenderContext context;
  settings.m_bpp = std::is_same<Pixel, TPixelF>::value ? 128 :
                  std::is_same<Pixel, TPixel64>::value ? 64 : 32;
  settings.m_linearColorSpace = linear;
  settings.m_cameraBox = TRectD(origin, TDimensionD(size.lx, size.ly));
  TRasterPT<Pixel> output(size); output->clear(); output->setLinear(linear);
  for (int row = 0; row < rows; ++row)
    for (int column = 0; column < columns; ++column) {
      const int x0 = size.lx * column / columns, x1 = size.lx * (column + 1) / columns;
      const int y0 = size.ly * row / rows, y1 = size.ly * (row + 1) / rows;
      TRasterPT<Pixel> part(x1 - x0, y1 - y0);
      part->clear(); part->setLinear(linear);
      TTile tile(part, origin + TPointD(x0, y0));
      fx.compute(tile, 0, settings);
      output->copy(tile.getRaster(), TPoint(x0, y0));
    }
  return output;
}

template <class Pixel>
TRasterPT<Pixel> wholeSourceReference(const TRasterPT<Pixel> &source,
                                     const TRenderSettings &settings,
                                     const TDimension &size, const TPointD &origin) {
  TRasterPT<Pixel> output(size); output->clear();
  TTile input(source, TPointD(-0.5 * source->getLx(), -0.5 * source->getLy()));
  TTile tile(output, origin);
  // The reference bypasses level extraction entirely: the unchanged resampler
  // sees the complete raster and its exact centered origin for every pixel.
  TRasterFx::applyAffine(tile, input, settings);
  return output;
}

template <class Pixel>
void compareRaster(const TRasterPT<Pixel> &expected, const TRasterPT<Pixel> &actual,
                   const std::string &context, bool exact = false) {
  const double tolerance = exact ? 0.0 : std::is_same<Pixel, TPixelF>::value ? 2e-5 :
      std::is_same<Pixel, TPixel64>::value ? 1.0 / 65535.0 : 1.0 / 255.0;
  for (int y = 0; y < expected->getLy(); ++y)
    for (int x = 0; x < expected->getLx(); ++x) {
      const TPixelD a = toPixelD(expected->pixels(y)[x]), b = toPixelD(actual->pixels(y)[x]);
      if (std::max({std::abs(a.r-b.r), std::abs(a.g-b.g),
                    std::abs(a.b-b.b), std::abs(a.m-b.m)}) > tolerance) {
        std::ostringstream message;
        message << context << " at " << x << ',' << y << ": expected "
                << a.r << ',' << a.g << ',' << a.b << ',' << a.m << "; actual "
                << b.r << ',' << b.g << ',' << b.b << ',' << b.m;
        throw std::runtime_error(message.str());
      }
    }
}

template <class Pixel>
TRasterPT<Pixel> rasterPattern(const TDimension &size) {
  TRasterPT<Pixel> image(size); image->clear();
  for (int y = 2; y < size.ly - 2; ++y)
    for (int x = 2; x < size.lx - 2; ++x) {
      const double alpha = (x + y) % 3 == 0 ? .5 : 1.0;
      const TPixelD color = (x + 2*y) % 2 ? TPixelD(alpha, 0, 0, alpha) :
                                                   TPixelD(0, alpha, 0, alpha);
      image->pixels(y)[x] = PixelConverter<Pixel>::from(color);
    }
  image->pixels(size.ly / 2)[size.lx / 2] = Pixel::Transparent;
  return image;
}

template <class Pixel>
void testFractionalHoleCoverage() {
  const bool floating = std::is_same<Pixel, TPixelF>::value;
  const double alpha = floating ? .5 : std::floor(Pixel::maxChannelValue / 2.0);
  TRasterPT<Pixel> image(9, 7); image->fill(Pixel(0, alpha, 0, alpha));
  image->pixels(3)[4] = Pixel::Transparent;
  RasterLevel level{TRasterImageP(image)};
  TRenderSettings settings;
  settings.m_quality = TRenderSettings::StandardResampleQuality;
  // World (-1,0) lies between four pixel centers, one of which is the hole.
  // Triangle weights therefore retain exactly three quarters of the coverage.
  const double coverage = floating ? alpha * .75 : std::floor(alpha * .75 + .5);
  const Pixel expected(0, coverage, 0, coverage);
  for (int rows : {1, 4}) {
    const auto output = renderLevel<Pixel>(level.fx(), settings,
        TDimension(32, 24), TPointD(-16, -12), 1, rows);
    require(output->pixels(12)[15] == expected,
            "Fractional raster extraction changed analytical hole coverage");
  }
}

template <class Pixel>
void testRasterExtraction() {
  const TDimension viewport(32, 24);
  const TPointD origin(-16, -12);
  for (const TDimension &size : {TDimension(12, 10), TDimension(13, 11),
                                TDimension(13, 10), TDimension(12, 11)}) {
    const auto image = rasterPattern<Pixel>(size);
    RasterLevel level{TRasterImageP(image)};
    for (int quality = TRenderSettings::HighResampleQuality;
         quality <= TRenderSettings::Bilinear_FilterResampleQuality; ++quality) {
      TRenderSettings settings;
      settings.m_quality = static_cast<TRenderSettings::ResampleQuality>(quality);
      const std::string description = "Raster extraction " + std::to_string(size.lx) +
          "x" + std::to_string(size.ly) + " filter " + std::to_string(quality);
      const auto expected = wholeSourceReference(image, settings, viewport, origin);
      const auto full = renderLevel<Pixel>(level.fx(), settings, viewport, origin, 1, 1);
      compareRaster(expected, full, description + " full source");
      compareRaster(expected, renderLevel<Pixel>(level.fx(), settings, viewport, origin, 1, 4),
                    description + " horizontal strips");
      compareRaster(expected, renderLevel<Pixel>(level.fx(), settings, viewport, origin, 3, 4),
                    description + " rectangular tiles");
      // Aligning each source pixel to the output must remain an exact copy,
      // including for odd canvases and smoothing resampling filters.
      settings.m_affine = TTranslation(size.lx % 2 ? .5 : 0, size.ly % 2 ? .5 : 0);
      compareRaster(wholeSourceReference(image, settings, viewport, origin),
                    renderLevel<Pixel>(level.fx(), settings, viewport, origin, 3, 4),
                    description + " aligned copy", true);
    }
    for (auto quality : {TRenderSettings::StandardResampleQuality,
                         TRenderSettings::HighResampleQuality,
                         TRenderSettings::Gauss_FilterResampleQuality,
                         TRenderSettings::ClosestPixel_FilterResampleQuality}) {
      for (const TAffine &affine : std::vector<TAffine>{TAffine(),
          TTranslation(.25, -.375), TScale(.5, .25),
          TTranslation(1.25, -.5) * TRotation(23) * TScale(1.4, .7),
          TTranslation(2, -1) * TScale(-1, 1)}) {
        TRenderSettings settings; settings.m_quality = quality; settings.m_affine = affine;
        for (bool linear : {false, true}) {
          const auto full = renderLevel<Pixel>(level.fx(), settings, viewport, origin, 1, 1, linear);
          compareRaster(full, renderLevel<Pixel>(level.fx(), settings, viewport, origin, 3, 4, linear),
                        "Transformed real raster changed across tiles");
        }
      }
    }
    TRenderSettings settings;
    for (const TPointD &cropOrigin : {TPointD(-3, 1), TPointD(-16.25, -12.5), TPointD(100, 100)}) {
      const TDimension crop(15, 13);
      compareRaster(wholeSourceReference(image, settings, crop, cropOrigin),
                    renderLevel<Pixel>(level.fx(), settings, crop, cropOrigin, 3, 4),
                    "Partial or out-of-bounds raster tile changed source geometry");
    }
  }
}

void testColormappedExtraction() {
  TPaletteP palette = new TPalette;
  for (const TDimension &size : {TDimension(12, 10), TDimension(13, 11)}) {
    TRasterCM32P image(size); image->clear();
    const TRect savebox(2, 2, size.lx - 3, size.ly - 3);
    for (int y = savebox.y0; y <= savebox.y1; ++y)
      for (int x = savebox.x0; x <= savebox.x1; ++x)
        image->pixels(y)[x] = TPixelCM32(1, 0, (x + y) % 2 ? 0 : 128);
    TToonzImageP toon(image, savebox);
    RasterLevel level(toon, TZP_XSHLEVEL, palette.getPointer());
    TRaster32P colors(size); colors->clear();
    TRop::convert(colors, image, palette);
    for (auto quality : {TRenderSettings::StandardResampleQuality,
                         TRenderSettings::HighResampleQuality,
                         TRenderSettings::Gauss_FilterResampleQuality}) {
      TRenderSettings settings; settings.m_quality = quality;
      const TDimension viewport(32, 24); const TPointD origin(-16, -12);
      compareRaster(renderLevel<TPixel32>(level.fx(), settings, viewport, origin, 1, 1),
                    renderLevel<TPixel32>(level.fx(), settings, viewport, origin, 3, 4),
                    "Colormapped savebox changed across tiles");
      // Reference conversion uses the entire raster; compare the interior
      // savebox where host bbox clipping cannot truncate a filter's fringe.
      const TDimension interior(3, 3); const TPointD at(-1, -1);
      compareRaster(wholeSourceReference(colors, settings, interior, at),
                    renderLevel<TPixel32>(level.fx(), settings, interior, at, 3, 3),
                    "Colormapped fractional extraction lost neighboring samples");
    }
  }
}
}

int main(int argc, char **argv) {
  QCoreApplication app(argc, argv);
  TThread::init();
  TRenderer::initialize();
  int result = 1;
  try {
    testOptIn();
    testMosaicCompatibility();
    testLevelCompatibility();
    testVectorLevelGrid();
    std::cout << "Checking real raster extraction across tiles" << std::endl;
    testFractionalHoleCoverage<TPixel32>();
    testFractionalHoleCoverage<TPixel64>();
    testFractionalHoleCoverage<TPixelF>();
    testRasterExtraction<TPixel32>(); testRasterExtraction<TPixel64>(); testRasterExtraction<TPixelF>();
    testColormappedExtraction();
    std::cout << "PASS: custom render grids and legacy affine policies\n";
    result = 0;
  } catch (const TException &error) {
    std::cerr << QString::fromStdWString(error.getMessage()).toStdString() << '\n';
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
  }
  TThread::shutdown();
  return result;
}
