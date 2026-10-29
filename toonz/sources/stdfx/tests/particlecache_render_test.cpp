// SPDX-License-Identifier: BSD-3-Clause
// Production particle cache rendering: precision, source drawing choice,
// persistence, temporal sampling and independence of tile/request order.
#include "../stdfx.h"
#include "tfxparam.h"
#include "tnotanimatableparam.h"
#include "tparamset.h"
#include "trenderer.h"
#include "tstream.h"
#include "trop.h"
#include "toonz/particlecache.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QTemporaryDir>

#include <algorithm>
#include <cmath>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

DV_IMPORT_API void initStdFx();
namespace {
void require(bool condition, const std::string &message) {
  if (!condition) throw std::runtime_error(message);
}
template <class P> P *param(TFx *fx, const char *name) {
  auto *p = dynamic_cast<P *>(fx->getParams()->getParam(name));
  require(p != nullptr, std::string("Missing parameter: ") + name);
  return p;
}
void number(TFx *fx, const char *name, double value) {
  param<TDoubleParam>(fx, name)->setDefaultValue(value);
}
void mode(TFx *fx, const char *name, int value) {
  param<TIntEnumParam>(fx, name)->setValue(value);
}
TRasterP image(int bpp, int width = 64, int height = 64) {
  if (bpp == 128) return TRasterFP(width, height);
  if (bpp == 64) return TRaster64P(width, height);
  return TRaster32P(width, height);
}
}

class ParticleCacheArtworkFixture final : public TStandardRasterFx {
  FX_PLUGIN_DECLARATION(ParticleCacheArtworkFixture)
public:
  ParticleCacheArtworkFixture() { enableComputeInFloat(true); }
  bool canHandle(const TRenderSettings &, double) override { return true; }
  bool doGetBBox(double, TRectD &box, const TRenderSettings &) override {
    box = TRectD(-2, -2, 2, 2); return true;
  }
  TFxTimeRegion getTimeRegion() const override { return TFxTimeRegion(0, 3); }
  void doCompute(TTile &tile, double frame, const TRenderSettings &settings) override {
    TRasterFP data(tile.getRaster()->getLx(), tile.getRaster()->getLy());
    const TAffine inverse = settings.m_affine.inv();
    for (int y = 0; y < data->getLy(); ++y) for (int x = 0; x < data->getLx(); ++x) {
      const TPointD q = inverse * TPointD(tile.m_pos.x + x, tile.m_pos.y + y);
      const float alpha = std::abs(q.x) <= 2 && std::abs(q.y) <= 2 ? 1 : 0;
      data->pixels(y)[x] = TPixelF(float(.123456 + frame * .1) * alpha,
                                  .234567f * alpha, .345678f * alpha, alpha);
    }
    if (TRasterFP target = tile.getRaster()) target->copy(data);
    else TRop::convert(tile.getRaster(), data);
  }
};
FX_PLUGIN_IDENTIFIER(ParticleCacheArtworkFixture, "particleCacheArtworkFixture")

namespace {
TRasterFxP effect(const QString &path, bool artwork = false) {
  TFxP fx = TFx::create("STD_particleCacheFx");
  require(bool(fx), "Particle Cache FX not registered");
  param<TStringParam>(fx.getPointer(), "cacheFile")->setValue(path.toStdWString());
  param<TBoolParam>(fx.getPointer(), "linearTint")->setValue(false);
  if (artwork) fx->getInputPort("Artwork1")->setFx(new ParticleCacheArtworkFixture);
  auto *raster = dynamic_cast<TRasterFx *>(fx.getPointer());
  require(raster != nullptr, "Particle Cache is not a raster FX");
  return TRasterFxP(raster);
}

TRasterP render(TRasterFxP fx, double frame, int bpp = 32, int strips = 1,
                const TAffine &affine = TAffine()) {
  TRenderer renderer(1);
  const auto id = TRenderer::buildRenderId();
  renderer.install(id); renderer.declareRenderStart(id); renderer.declareFrameStart(frame);
  TRasterP result = image(bpp); result->clear();
  try {
    TRenderSettings settings; settings.m_bpp = bpp; settings.m_affine = affine;
    for (int i = 0; i < strips; ++i) {
      const int y0 = 64 * i / strips, y1 = 64 * (i + 1) / strips;
      TTile tile(image(bpp, 64, y1 - y0), TPointD(-32, -32 + y0));
      fx->compute(tile, frame, settings);
      result->copy(tile.getRaster(), TPoint(0, y0));
    }
  } catch (...) {
    renderer.declareFrameEnd(frame); renderer.declareRenderEnd(id); renderer.uninstall();
    throw;
  }
  renderer.declareFrameEnd(frame); renderer.declareRenderEnd(id); renderer.uninstall();
  return result;
}

template <class PIXEL>
void compare(const TRasterPT<PIXEL> &a, const TRasterPT<PIXEL> &b, const char *message) {
  require(bool(a) && bool(b), "Unexpected raster precision");
  std::size_t occupied = 0;
  for (int y = 0; y < a->getLy(); ++y) for (int x = 0; x < a->getLx(); ++x) {
    const auto &p = a->pixels(y)[x], &q = b->pixels(y)[x];
    if (p.m > 0) ++occupied;
    require(p.r == q.r && p.g == q.g && p.b == q.b && p.m == q.m, message);
  }
  require(occupied > 0, "Comparison accidentally rendered no particles");
}

ParticleCache::Snapshot snapshot(int frame, bool reverse = false) {
  ParticleCache::Snapshot result;
  result.hasStableIds = true; result.hasVelocity = true; result.hasArtworkFrame = true;
  ParticleCache::Particle a;
  a.id = 100; a.position = {{double(frame - 1) * 8, 0, 0}};
  a.size = 4; a.tint = {{.8, .3, .2, .75}}; a.velocity = {{192, 0, 0}};
  a.artworkFrame = frame - 1;
  ParticleCache::Particle b = a;
  b.id = 200; b.position = {{-6, 2, 0}}; b.tint = {{.1, .7, .8, .5}};
  result.particles = {a, b};
  if (reverse) std::reverse(result.particles.begin(), result.particles.end());
  return result;
}

QString package(const QString &directory, bool reverse = false) {
  ParticleCache::Metadata metadata; metadata.firstFrame = 1; metadata.lastFrame = 3;
  QString error;
  require(ParticleCache::writePackage(directory, metadata,
      [reverse](int frame, QString &) -> ParticleCache::SnapshotPtr {
        return std::make_shared<ParticleCache::Snapshot>(snapshot(frame, reverse));
      }, ParticleCache::WriteOptions(), error), error.toStdString());
  return QDir(directory).filePath("cache.otpc");
}

void testTilesAndOrder(const QString &path, const QString &reordered) {
  auto fx = effect(path);
  for (int bpp : {32, 64, 128}) {
    const auto full = render(fx, 1, bpp), tiles = render(fx, 1, bpp, 4);
    if (bpp == 32) compare<TPixel32>(full, tiles, "8-bit tiled output differs");
    else if (bpp == 64) compare<TPixel64>(full, tiles, "16-bit tiled output differs");
    else compare<TPixelF>(full, tiles, "Float tiled output differs");
  }
  const TAffine affine = TTranslation(1.25, -2.75) * TRotation(17) * TScale(1.3, .7);
  compare<TPixelF>(render(fx, 1, 128, 1, affine), render(fx, 1, 128, 4, affine),
                   "Camera affine changes tiled render output");
  const auto first = render(fx, 0);
  render(fx, 2); render(fx, 1);
  compare<TPixel32>(first, render(fx, 0), "Playback depends on frame request order");
  compare<TPixel32>(render(fx, 0), render(effect(reordered), 0),
                    "Stable-ID draw order depends on particle file row order");
}

void testTiming(const QString &path) {
  auto fx = effect(path);
  const auto first = render(fx, 0), last = render(fx, 2);
  mode(fx.getPointer(), "endBehavior", 1);
  compare<TPixel32>(first, render(fx, 3), "Loop endpoint does not wrap");
  number(fx.getPointer(), "timeOffset", 2); number(fx.getPointer(), "speed", -1);
  compare<TPixel32>(last, render(fx, 0), "Reverse playback starts at wrong cache frame");
  compare<TPixel32>(first, render(fx, 2), "Reverse playback ends at wrong cache frame");
  number(fx.getPointer(), "timeOffset", 0); number(fx.getPointer(), "speed", 1);
  number(fx.getPointer(), "sceneFps", 48);
  compare<TPixel32>(last, render(fx, 4), "FPS conversion changes particle timing");
}

void testArtworkPrecision(const QString &directory) {
  ParticleCache::Snapshot data; data.hasStableIds = true;
  ParticleCache::Particle p; p.id = 1; p.size = 1; data.particles.push_back(p);
  const QString file = QDir(directory).filePath("precise.bgeo"); QString error;
  require(ParticleCache::writeFrame(file, data, ParticleCache::WriteOptions(), error), error.toStdString());
  auto fx = effect(file, true); mode(fx.getPointer(), "sizeMode", 1);
  TRasterFP floating = render(fx, 0, 128);
  const auto pixel = floating->pixels(32)[32];
  require(std::abs(pixel.r - .123456) < .00001 && pixel.m == 1,
          "Float artwork was quantized or incorrectly premultiplied");
  TRaster64P sixteen = render(fx, 0, 64);
  require(std::abs(int(sixteen->pixels(32)[32].r) - int(.123456 * 65535 + .5)) <= 1,
          "16-bit artwork was reduced to 8-bit precision");
  mode(fx.getPointer(), "artworkAnimation", 1);
  number(fx.getPointer(), "artworkStart", 2);
  TRasterFP different = render(fx, 0, 128);
  require(std::abs(different->pixels(32)[32].r - .323456) < .00001,
          "Fixed artwork frame did not select the requested drawing");
}

void testTrailsAndBlur(const QString &path) {
  auto fx = effect(path, true);
  mode(fx.getPointer(), "interpolation", 1);
  param<TIntParam>(fx.getPointer(), "trailSamples")->setValue(2);
  param<TIntParam>(fx.getPointer(), "motionBlurSamples")->setValue(4);
  number(fx.getPointer(), "shutter", 1);
  compare<TPixelF>(render(fx, 1, 128), render(fx, 1, 128, 4),
                   "Trails/motion blur depend on tile boundaries");
  auto noTrails = effect(path, true);
  TRasterFP with = render(fx, 1, 128), without = render(noTrails, 1, 128);
  bool different = false;
  for (int y = 0; y < with->getLy(); ++y) for (int x = 0; x < with->getLx(); ++x)
    different = different || with->pixels(y)[x].m != without->pixels(y)[x].m;
  require(different, "Trail/blur settings did not change rendered output");
}

void testAliasInvalidation(const QString &directory) {
  const QString path = QDir(directory).filePath("mutable.bgeo");
  const QString replacement = QDir(directory).filePath("replacement.bgeo");
  QString error;
  const auto first = snapshot(1), second = snapshot(2);
  require(ParticleCache::writeFrame(path, first, ParticleCache::WriteOptions(), error), error.toStdString());
  require(ParticleCache::writeFrame(replacement, second, ParticleCache::WriteOptions(), error), error.toStdString());
  auto fx = effect(path);
  const auto before = fx->getAlias(0, TRenderSettings());
  require(QFile::remove(path) && QFile::rename(replacement, path), "Cannot replace cache fixture");
  const auto after = fx->getAlias(0, TRenderSettings());
  require(before != after, "Replacing particle data did not invalidate FX image cache");
}

void testPersistence(const QString &path, const QString &directory) {
  auto fx = effect(path);
  number(fx.getPointer(), "positionX", 3);
  const QString saved = QDir(directory).filePath("particle-fx.tnz");
  { TOStream output(TFilePath(saved.toStdWString())); output << fx.getPointer(); }
  TPersist *persistent = nullptr;
  { TIStream input(TFilePath(saved.toStdWString())); input >> persistent; }
  auto *loaded = dynamic_cast<TRasterFx *>(persistent);
  require(loaded != nullptr, "Particle cache FX failed persistence round trip");
  TRasterFxP restored(loaded);
  compare<TPixel32>(render(fx, 0), render(restored, 0), "Saved FX settings changed playback");
  TRasterFxP cloned(dynamic_cast<TRasterFx *>(fx->clone()));
  compare<TPixel32>(render(fx, 0), render(cloned, 0), "Cloned FX settings changed playback");
}
}

int main(int argc, char **argv) {
  QCoreApplication application(argc, argv);
  try {
    initStdFx();
    QTemporaryDir directory; require(directory.isValid(), "Cannot create fixture directory");
    const QString path = package(directory.path() + "/normal");
    const QString reordered = package(directory.path() + "/reordered", true);
    testTilesAndOrder(path, reordered);
    testTiming(path);
    testArtworkPrecision(directory.path());
    testTrailsAndBlur(path);
    testPersistence(path, directory.path());
    testAliasInvalidation(directory.path());
    std::cout << "Particle cache rendering: 6 groups passed (precision, tiles/IDs, timing, temporal FX, persistence, invalidation).\n";
    return 0;
  } catch (const std::exception &e) {
    std::cerr << "Particle cache render failure: " << e.what() << '\n';
    return 1;
  } catch (const TException &e) {
    std::cerr << "Particle cache render failure: " << QString::fromStdWString(e.getMessage()).toStdString() << '\n';
    return 1;
  }
}
