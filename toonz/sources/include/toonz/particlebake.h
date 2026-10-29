#pragma once
#ifndef TOONZ_PARTICLEBAKE_H
#define TOONZ_PARTICLEBAKE_H

#include "tcommon.h"
#include "tgeometry.h"
#include "traster.h"
#include "toonz/particlecache.h"
#include <QString>
#include <cstdint>
#include <functional>
#include <vector>

class TFx;
class TRasterFx;
class TRenderSettings;

#ifdef TNZSTDFX_EXPORTS
#define PARTICLEBAKE_API DV_EXPORT_API
#else
#define PARTICLEBAKE_API DV_IMPORT_API
#endif

// Explicit, read-only capture of native particle evaluation. It performs no
// filesystem writes and never changes the source graph. Samples are in draw
// order; repeated birth IDs represent trail contributions, not extra particles.
namespace ParticleBake {
struct DrawSample {
  std::uint64_t id = 0;
  TPointD position, velocity;
  // Age/lifetime/trail distance are source-frame units; velocity is units/sec.
  double age = 0, lifetime = 0;
  int source = 0, artworkFrame = 0, trailDistance = 0;
  double scale = 1, angle = 0, opacity = 1, tintMix = 0;
  TPixel32 tint = TPixel32::White;
  TPointD history[3];
  bool background = false, motionBlur = false;
  double blurGamma = 0, trailOpacityStart = 1, trailOpacityEnd = 1;
  // Prepared artwork retains Iwa base-image extraction/illumination and native
  // colour/blur math. This is an individual sprite or a tiled-background
  // plate, never a flattened frame containing the complete simulation.
  // rasterToWorld maps raster pixel coordinates to the fixed bake domain.
  TRasterP artwork;
  TAffine rasterToWorld;
};
struct Frame {
  double frame = 0;
  TRectD domain;
  // Actual sampled pixel viewport, preserving an explicit fractional origin.
  // Unlike a transformed world AABB, this does not grow for rotated columns.
  TRectD pixelDomain;
  TAffine renderAffine;
  int bitsPerPixel = 32;
  bool linearColorSpace = false;
  double colorSpaceGamma = 2.2;
  // Current live simulation state, including invisible/newborn particles.
  // These have no artwork; each birth ID occurs once.
  std::vector<DrawSample> particles;
  std::vector<DrawSample> draws;
};
struct Options {
  // Finite native FX-coordinate rectangle, fixed for the entire bake. It is
  // required because tiled particles and infinite control images use a domain.
  TRectD domain;
  // Optional exact output-pixel viewport. When empty, renderAffine * domain
  // determines the viewport. Supply the camera viewport for scene-built FXs so
  // inverse/forward AABB transforms do not change tiled-particle origins.
  TRectD pixelDomain;
  // Inclusive last source-artwork frame used for textures with no temporal end.
  // Keep fixed across every sample in a bake; zero holds procedural artwork at 0.
  int proceduralArtworkLastFrame = 0;
  double framesPerSecond = 24;
  int maxRasterDimension = 16384;
  std::uint64_t maxArtworkBytes = 512ULL * 1024 * 1024;
  std::size_t maxDraws = 1000000;
};
using Cancel = std::function<bool()>;
PARTICLEBAKE_API bool isSupported(const TFx *fx);
// Call on the rendering/UI thread with the same scene-built source graph used
// for preview, not an unexpanded schematic FX with unresolved column ports.
// A fresh private simulation state is used on each call; output does not depend
// on previous previews, cache contents or the order in which frames are baked.
// An optional evaluation image supports verifying the individual draw recipe;
// the image is not stored in the cache package.
PARTICLEBAKE_API bool captureFrame(TRasterFx *fx, double frame,
                                   const TRenderSettings &settings,
                                   const Options &options, Frame &result,
                                   QString &error, const Cancel &cancel = Cancel(),
                                   TRasterP *evaluatedRaster = nullptr);
using FrameProvider = std::function<bool(int frame, Frame &, QString &error)>;
PARTICLEBAKE_API bool writePackage(const QString &directory,
                                   const ParticleCache::Metadata &metadata,
                                   const FrameProvider &provider, QString &error,
                                   const Cancel &cancel = {},
                                   const ParticleCache::Progress &progress = {});
PARTICLEBAKE_API bool loadFrame(const ParticleCache::Snapshot &snapshot,
                                Frame &frame, QString &error,
                                const Cancel &cancel = {});
// Includes native metadata and verified artwork bytes, not just particle data.
PARTICLEBAKE_API QString frameRevision(const ParticleCache::Snapshot &snapshot,
                                       QString &error);
}
#undef PARTICLEBAKE_API
#endif
