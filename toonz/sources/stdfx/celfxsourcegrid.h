#pragma once

#include "trasterfx.h"
#include "tfxattributes.h"

#include <cmath>
#include <set>

// Pixel-neighborhood effects use the raster level's sampling grid before its
// DPI and geometry placements. Ambiguous multi-source graphs retain the
// ordinary world grid instead of arbitrarily choosing one input's resolution.
namespace CelFxSourceGrid {

inline bool invertible(const TAffine &affine) {
  for (double value : {affine.a11, affine.a12, affine.a13,
                       affine.a21, affine.a22, affine.a23})
    if (!std::isfinite(value)) return false;
  const double determinant = affine.det();
  if (!std::isfinite(determinant) || determinant == 0.0) return false;
  const TAffine inverse = affine.inv();
  for (double value : {inverse.a11, inverse.a12, inverse.a13,
                       inverse.a21, inverse.a22, inverse.a23})
    if (!std::isfinite(value)) return false;
  return true;
}

inline TAffine nativeToWorld(TFx *source, double frame) {
  TAffine placement;
  std::set<TFx *> visited;
  while (source && visited.insert(source).second) {
    auto *raster = dynamic_cast<TRasterFx *>(source);
    if (!raster) return TAffine();
    const bool enabled = source->getAttributes()->isEnabled();
    // Identity DPI needs no affine node. The scene builder marks the original
    // raster source in that case, so keep placements accumulated above it.
    if (enabled && source->getInputPortCount() == 0 &&
        source->getName() == L"LevelColumn AffineFx")
      return placement;
    auto *geometry = dynamic_cast<TGeometryFx *>(source);
    if (enabled && geometry && geometry->getActiveTimeRegion().contains(frame)) {
      placement = placement * geometry->getPlacement(frame);
      if (!invertible(placement)) return TAffine();
      auto *affine = dynamic_cast<NaAffineFx *>(geometry);
      if (affine && (affine->isDpiAffine() ||
                     affine->getName() == L"LevelColumn AffineFx"))
        return placement;
    }
    TFx *next = nullptr;
    if (!enabled) {
      const int port = source->getPreferredInputPort();
      if (port >= 0 && port < source->getInputPortCount() &&
          source->getInputPort(port)->isConnected())
        next = source->getInputPort(port)->getFx();
    } else {
      for (int port = 0; port < source->getInputPortCount(); ++port) {
        TFxPort *input = source->getInputPort(port);
        if (!input->isConnected()) continue;
        if (next || !dynamic_cast<TRasterFx *>(input->getFx())) return TAffine();
        next = input->getFx();
      }
    }
    source = next;
  }
  return TAffine();
}

inline TAffine renderAffine(TFx *source, double frame) {
  TAffine grid = nativeToWorld(source, frame).inv();
  // TRasterFx moves whole-pixel translations into the tile origin before
  // consulting handledAffine. Return the same canonical form to prevent a
  // repeated affine wrapper while retaining the native sampling phase.
  grid.a13 -= std::floor(grid.a13);
  grid.a23 -= std::floor(grid.a23);
  return grid;
}

}  // namespace CelFxSourceGrid
