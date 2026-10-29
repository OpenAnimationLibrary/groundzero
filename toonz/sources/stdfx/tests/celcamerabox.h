#pragma once

// Shared check for Cel effects that cover an infinite source with the camera
// box. The host leaves the box in the caller's output coordinates when it
// renders the effect on its own grid, so a reduced or translated render must
// still fill the whole camera, not only the part the box covers in world
// units.

#include "stdfx.h"
#include "tfxcachemanager.h"
#include "trenderer.h"

#include <stdexcept>
#include <string>
#include <vector>

namespace CelCameraBoxTest {

class InfiniteFill final : public TStandardRasterFx {
public:
  const TPersistDeclaration *getDeclaration() const override { return nullptr; }
  bool canHandle(const TRenderSettings &, double) override { return true; }
  std::string getAlias(double, const TRenderSettings &) const override {
    return "cel-camera-box-infinite-fill";
  }
  bool doGetBBox(double, TRectD &box, const TRenderSettings &) override {
    box = TConsts::infiniteRectD;
    return true;
  }
  void doCompute(TTile &tile, double, const TRenderSettings &) override {
    if (TRaster32P raster = tile.getRaster())
      raster->fill(TPixel32(200, 100, 50, 255));
    else if (TRaster64P raster = tile.getRaster())
      raster->fill(TPixel64(50000, 25000, 12000, 65535));
    else if (TRasterFP raster = tile.getRaster())
      raster->fill(TPixelF(.8f, .4f, .2f, 1.f));
  }
};

inline void setParam(TFx &fx, const std::string &name, double value) {
  TDoubleParamP param(TParamP(fx.getParams()->getParam(name)));
  if (!param) throw std::runtime_error("Missing parameter " + name);
  param->setDefaultValue(value);
}

// Renders fx over an infinite opaque source through the host's affine
// routing, at half resolution and with translated and fractional cameras.
inline void checkInfiniteSourceCoversCamera(TRasterFx &fx,
                                            const std::string &name) {
  TFxP fill = new InfiniteFill;
  if (!fx.connect("Source", fill.getPointer()))
    throw std::runtime_error(name + ": cannot connect the source port");
  struct Case {
    TAffine affine;
    TPointD origin;
  };
  const std::vector<Case> cases{
      {TScale(.5), TPointD(-8, -8)},
      {TTranslation(3, -2) * TScale(.5), TPointD(-8, -8)},
      {TScale(.5), TPointD(-8.25, -7.5)},
      {TTranslation(1.25, .75) * TScale(.25), TPointD(-8, -8)}};
  for (const Case &test : cases) {
    TRenderer renderer(1);
    const unsigned long id = TRenderer::buildRenderId();
    renderer.install(id);
    renderer.declareRenderStart(id);
    renderer.declareFrameStart(0);
    std::string failure;
    try {
      TRenderSettings settings;
      settings.m_bpp       = 32;
      settings.m_affine    = test.affine;
      settings.m_cameraBox = TRectD(test.origin, TDimensionD(16, 16));
      TRectD rect          = settings.m_cameraBox;
      TFxCacheManager::instance()->onRenderStatusStart(TRenderer::FIRSTRUN);
      fx.dryCompute(rect, 0, settings);
      TFxCacheManager::instance()->onRenderStatusEnd(TRenderer::FIRSTRUN);
      TFxCacheManager::instance()->onRenderStatusStart(TRenderer::IDLE);
      TRaster32P raster(16, 16);
      raster->clear();
      TTile tile(raster, test.origin);
      fx.compute(tile, 0, settings);
      for (const TPoint &p : {TPoint(1, 1), TPoint(14, 1), TPoint(1, 14),
                              TPoint(14, 14), TPoint(8, 8)})
        if (raster->pixels(p.y)[p.x].m == 0)
          failure = name + ": an infinite source left the camera at " +
                    std::to_string(p.x) + "," + std::to_string(p.y) +
                    " uncovered";
    } catch (...) {
      renderer.declareFrameEnd(0);
      renderer.declareRenderEnd(id);
      renderer.uninstall();
      fx.disconnect("Source");
      throw;
    }
    renderer.declareFrameEnd(0);
    renderer.declareRenderEnd(id);
    renderer.uninstall();
    if (!failure.empty()) {
      fx.disconnect("Source");
      throw std::runtime_error(failure);
    }
  }
  fx.disconnect("Source");
}

}  // namespace CelCameraBoxTest
