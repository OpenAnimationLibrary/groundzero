#pragma once
#include "toonz/particlebake.h"
class TFx;
class TTile;
class TFxTimeRegion;
namespace ParticleBakeCapture {
class RasterLockGuard {
  TRasterP m_raster;
public:
  explicit RasterLockGuard(const TRasterP &raster) : m_raster(raster) {
    if (m_raster) m_raster->lock();
  }
  ~RasterLockGuard() { if (m_raster) m_raster->unlock(); }
  RasterLockGuard(const RasterLockGuard &) = delete;
  RasterLockGuard &operator=(const RasterLockGuard &) = delete;
};
bool active(const TFx *source);
// Every upstream native simulation uses private state during capture, while
// active(source) restricts draw/state recording to the selected generator.
bool isolating();
void checkCancel(const TFx *source);
int artworkLastFrame(const TFx *source, const TFxTimeRegion &region);
void checkParticleCount(const TFx *source, double count);
void checkRasterSize(const TFx *source, double width, double height, int pixelSize);
void checkSimulationRange(const TFx *source, int start, int end);
void beginDraw(const TFx *source, const ParticleBake::DrawSample &sample);
void particle(const TFx *source, const ParticleBake::DrawSample &sample);
void appearance(const TFx *source, double opacity, const TPixel32 &tint,
                double tintMix);
// affine maps artwork raster pixels to the output tile's pixel coordinates.
void record(const TFx *source, const TRasterP &artwork, const TAffine &affine,
            const TTile *tile);
void background(const TFx *source, const TTile *tile);
}
// Direct dispatch avoids the top-level FX image cache during explicit capture.
bool computeStandardParticlesForBake(TRasterFx *, TTile &, double,
                                     const TRenderSettings &);
bool computeTiledParticlesForBake(TRasterFx *, TTile &, double,
                                  const TRenderSettings &);
