#pragma once

#include "glbrenderer.h"
#include "trasterfx.h"
#include "toonz/tcolumnfx.h"

#include <memory>

// Internal contract for FX nodes that can provide an unevaluated 3D render
// result to another 3D-aware FX. It is deliberately separate from raster
// compute so lighting can be applied before the model is flattened to pixels.
class T3DRenderSource {
public:
  virtual ~T3DRenderSource() = default;

  // Predict raster dependencies without flattening geometry. Model-only
  // sources have none; image planes plan their complete source texture.
  virtual void dryCompute3D(double, const TRenderSettings &) const {}
  virtual bool hasSurface3D() const { return false; }

  virtual std::shared_ptr<const otglb::RenderScene> get3DRenderScene(
      double frame, const int *canceled,
      const otglb::LightingRig *lighting                   = nullptr,
      const std::vector<otglb::ModelTransform> *transforms = nullptr,
      const TRenderSettings *renderSettings = nullptr,
      const std::shared_ptr<const otglb::SurfaceRequest> &surface = {}) const = 0;

  // Bounds queries must not evaluate a raster input outside a render session.
  virtual std::shared_ptr<const otglb::RenderScene> get3DRenderGeometry(
      double frame, const int *canceled,
      const std::vector<otglb::ModelTransform> *transforms = nullptr,
      const TRenderSettings *renderSettings = nullptr,
      const std::shared_ptr<const otglb::SurfaceRequest> &surface = {}) const {
    return get3DRenderScene(frame, canceled, nullptr, transforms,
                            renderSettings, surface);
  }
};

// The schematic connects a zerary column, whereas the render tree connects the
// underlying FX. Keep the original column connection for ownership, exposure
// and scene persistence; unwrap it only when checking/accessing the 3D source.
class T3DSourcePort final : public TRasterFxPort {
  static T3DRenderSource *resolve(TFx *fx) {
    if (auto *column = dynamic_cast<TZeraryColumnFx *>(fx))
      fx = column->getZeraryFx();
    return dynamic_cast<T3DRenderSource *>(fx);
  }

public:
  bool accepts(TFx *fx) const override { return !fx || resolve(fx); }

  void setFx(TFx *fx) override {
    if (!accepts(fx))
      throw TException("Fx: 3D source port requires a compatible 3D FX");
    TRasterFxPort::setFx(fx);
  }

  T3DRenderSource *source() const { return resolve(getFx()); }
};
