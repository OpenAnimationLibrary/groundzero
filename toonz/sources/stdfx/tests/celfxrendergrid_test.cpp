#include "../mosaicfx.cpp"
#include "toonz/toonzscene.h"
#include "toonz/txshcell.h"
#include "toonz/txshlevelcolumn.h"
#include "toonz/txshleveltypes.h"
#include "toonz/txshsimplelevel.h"
#include "trasterimage.h"
#include "tfxcachemanager.h"
#include "trenderer.h"
#include "tthread.h"

#include <QCoreApplication>
#include <iostream>
#include <stdexcept>
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
  TFxP owner = new GridProbe(TAffine(), false, policy);
  checkRoute(*static_cast<GridProbe *>(owner.getPointer()), TAffine(), TAffine());
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
