// SPDX-License-Identifier: BSD-3-Clause
// Production particle cache rendering: precision, source drawing choice,
// persistence, temporal sampling and independence of tile/request order.
#include "../stdfx.h"
#include "tfxparam.h"
#include "tnotanimatableparam.h"
#include "tparamset.h"
#include "trenderer.h"
#include "tstream.h"
#include "tthread.h"
#include "trop.h"
#include "toonz/particlebake.h"
#include "toonz/particlecache.h"

#include <QGuiApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>
#include <QThread>

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
  TDoubleParamP m_alpha, m_gain;
public:
  ParticleCacheArtworkFixture() : m_alpha(1.0), m_gain(1.0) {
    bindParam(this, "fixtureAlpha", m_alpha);
    bindParam(this, "fixtureGain", m_gain);
    enableComputeInFloat(true);
  }
  bool canHandle(const TRenderSettings &, double) override { return true; }
  bool doGetBBox(double, TRectD &box, const TRenderSettings &) override {
    box = TRectD(-2, -2, 2, 2); return true;
  }
  TFxTimeRegion getTimeRegion() const override { return TFxTimeRegion(0, 3); }
  std::string getAlias(double frame, const TRenderSettings &settings) const override {
    return TRasterFx::getAlias(frame, settings) + "[drawing:" +
           std::to_string(frame) + "]";
  }
  void doCompute(TTile &tile, double frame, const TRenderSettings &settings) override {
    TRasterFP data(tile.getRaster()->getLx(), tile.getRaster()->getLy());
    const TAffine inverse = settings.m_affine.inv();
    for (int y = 0; y < data->getLy(); ++y) for (int x = 0; x < data->getLx(); ++x) {
      const TPointD q = inverse * TPointD(tile.m_pos.x + x, tile.m_pos.y + y);
      const float alpha = std::abs(q.x) <= 2 && std::abs(q.y) <= 2
          ? float(m_alpha->getValue(frame)) : 0;
      const float gain = float(m_gain->getValue(frame));
      data->pixels(y)[x] = TPixelF(float(.123456 + frame * .1) * alpha * gain,
                                  .234567f * alpha * gain,
                                  .345678f * alpha * gain, alpha);
    }
    if (TRasterFP target = tile.getRaster()) target->copy(data);
    else TRop::convert(tile.getRaster(), data);
  }
};
FX_PLUGIN_IDENTIFIER(ParticleCacheArtworkFixture, "particleCacheArtworkFixture")

class ParticleCachePlacementFixture final : public TStandardRasterFx {
  FX_PLUGIN_DECLARATION(ParticleCachePlacementFixture)
public:
  bool canHandle(const TRenderSettings &, double) override { return true; }
  bool doGetBBox(double, TRectD &box, const TRenderSettings &) override {
    box = TRectD(-2, -2, 2, 2); return true;
  }
  void doCompute(TTile &, double, const TRenderSettings &) override {
    throw std::runtime_error("Placement input must never render as particle artwork");
  }
};
FX_PLUGIN_IDENTIFIER(ParticleCachePlacementFixture, "particleCachePlacementFixture")

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
                const TAffine &affine = TAffine(), bool linear = false) {
  TRenderer renderer(1);
  const auto id = TRenderer::buildRenderId();
  renderer.install(id); renderer.declareRenderStart(id); renderer.declareFrameStart(frame);
  TRasterP result = image(bpp); result->clear(); result->setLinear(linear);
  try {
    TRenderSettings settings; settings.m_bpp = bpp; settings.m_affine = affine;
    settings.m_linearColorSpace = linear;
    for (int i = 0; i < strips; ++i) {
      const int y0 = 64 * i / strips, y1 = 64 * (i + 1) / strips;
      TTile tile(image(bpp, 64, y1 - y0), TPointD(-32, -32 + y0));
      tile.getRaster()->setLinear(linear);
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

QString nativePackage(const QString &directory, const TRasterP &artwork) {
  ParticleCache::Metadata metadata;
  metadata.firstFrame = metadata.lastFrame = 0;
  QString error;
  require(ParticleBake::writePackage(directory, metadata,
      [&](int frame, ParticleBake::Frame &captured, QString &) {
        captured.frame = frame;
        captured.domain = TRectD(-32, -32, 32, 32);
        captured.bitsPerPixel = artwork->getPixelSize() * 8;
        captured.linearColorSpace = artwork->isLinear();
        ParticleBake::DrawSample draw;
        draw.id = 1;
        draw.artwork = artwork;
        draw.rasterToWorld = TTranslation(-4, -4);
        captured.draws.push_back(draw);
        captured.particles.push_back(draw);
        return true;
      }, error), error.toStdString());
  return QDir(directory).filePath("cache.otpc");
}

void testNativeFloatAndLinear(const QString &directory) {
  TRasterFP sprite(8, 8);
  sprite->fill(TPixelF(.24f, .16f, .08f, .4f));
  auto floatingFx = effect(nativePackage(directory + "/native-float", sprite));
  number(floatingFx.getPointer(), "opacity", 50);
  TRasterFP floating = render(floatingFx, 0, 128);
  const auto &pixel = floating->pixels(32)[32];
  require(std::abs(pixel.r - .12) < .00001 &&
              std::abs(pixel.g - .08) < .00001 &&
              std::abs(pixel.b - .04) < .00001 &&
              std::abs(pixel.m - .2) < .00001,
          "Native float artwork failed float playback or opacity premultiplication");

  TRaster32P gray(8, 8);
  gray->fill(TPixel32(128, 128, 128, 255));
  auto grayFx = effect(nativePackage(directory + "/native-gray", gray));
  for (bool linear : {false, true}) {
    TRasterFP output = render(grayFx, 0, 128, 1, TAffine(), linear);
    const double expected = linear ? std::pow(128.0 / 255, 2.2) : 128.0 / 255;
    const auto &p = output->pixels(32)[32];
    require(std::abs(p.r - expected) < .00002 && p.m == 1,
            "Native gray artwork must receive exactly one color-space conversion");
    require(output->isLinear() == linear, "Native output has the wrong color-space flag");
  }
}

void testArtworkTintColorSpace(const QString &directory) {
  ParticleCache::Snapshot data;
  data.hasStableIds = true;
  ParticleCache::Particle particle;
  particle.id = 1;
  particle.size = 1;
  particle.tint = {{.5, .7, .9, .6}};
  data.particles.push_back(particle);
  const QString file = QDir(directory).filePath("tinted.bgeo");
  QString error;
  require(ParticleCache::writeFrame(file, data, ParticleCache::WriteOptions(), error),
          error.toStdString());
  auto fx = effect(file, true);
  mode(fx.getPointer(), "sizeMode", 1);
  number(fx->getInputPort("Artwork1")->getFx(), "fixtureAlpha", .4);
  const double source[] = {.123456, .234567, .345678};
  for (bool tintIsLinear : {false, true}) {
    param<TBoolParam>(fx.getPointer(), "linearTint")->setValue(tintIsLinear);
    for (bool linear : {false, true}) {
      TRasterFP output = render(fx, 0, 128, 1, TAffine(), linear);
      const auto &p = output->pixels(32)[32];
      const double channels[] = {p.r, p.g, p.b};
      require(std::abs(p.m - .24) < .00001,
              "Artwork alpha and particle opacity must each be applied once");
      for (int channel = 0; channel < 3; ++channel) {
        double tint = particle.tint[channel];
        if (linear != tintIsLinear) tint = std::pow(tint, linear ? 2.2 : 1 / 2.2);
        const double artwork = linear ? std::pow(source[channel], 2.2) : source[channel];
        require(std::abs(channels[channel] - artwork * tint * .24) < .00002,
                "Artwork and tint must be combined in the requested color space");
      }
    }
  }
}

void testHdrIntegerConversion(const QString &directory) {
  ParticleCache::Snapshot data;
  data.hasStableIds = true;
  ParticleCache::Particle particle;
  particle.id = 1;
  data.particles.push_back(particle);
  const QString file = QDir(directory).filePath("hdr.bgeo");
  QString error;
  require(ParticleCache::writeFrame(file, data, ParticleCache::WriteOptions(), error),
          error.toStdString());
  auto fx = effect(file, true);
  mode(fx.getPointer(), "sizeMode", 1);
  TFx *artwork = fx->getInputPort("Artwork1")->getFx();
  number(artwork, "fixtureAlpha", .5);
  number(artwork, "fixtureGain", 16);
  TRasterFP floating = render(fx, 0, 128);
  const auto &p = floating->pixels(32)[32];
  require(p.r > p.m && p.g > 1 && p.m == .5f,
          "HDR fixture must contain bright color at partial alpha");
  for (int bpp : {32, 64}) {
    TRasterP expected = image(bpp);
    TRop::convert(expected, floating);
    TRasterP actual = render(fx, 0, bpp);
    if (bpp == 32) {
      const auto &a = TRaster32P(expected)->pixels(32)[32];
      const auto &b = TRaster32P(actual)->pixels(32)[32];
      require(a.r == b.r && a.g == b.g && a.b == b.b && a.m == b.m,
              "8-bit HDR output differs from standard float conversion");
    } else {
      const auto &a = TRaster64P(expected)->pixels(32)[32];
      const auto &b = TRaster64P(actual)->pixels(32)[32];
      require(a.r == b.r && a.g == b.g && a.b == b.b && a.m == b.m,
              "16-bit HDR output differs from standard float conversion");
    }
  }
}

void testPlacementRouting(const QString &directory) {
  ParticleCache::Snapshot data;
  data.hasStableIds = true;
  for (int id = 1; id <= 8; ++id) {
    ParticleCache::Particle particle;
    particle.id = id;
    particle.position = {{double(-27 + id * 6), 0, 0}};
    particle.artworkIndex = 1;
    data.particles.push_back(particle);
  }
  const QString file = QDir(directory).filePath("placement.bgeo");
  QString error;
  require(ParticleCache::writeFrame(file, data, ParticleCache::WriteOptions(), error),
          error.toStdString());
  auto addPlacement = [](const TRasterFxP &fx) {
    require(fx->dynamicPortGroupsCount() == 2 &&
                fx->dynamicPortGroup(1)->portsPrefix() == "Placement" &&
                fx->dynamicPortGroup(1)->minPortsCount() == 0 &&
                !fx->getInputPort("Placement1"),
            "Placement must be an optional second port group");
    require(fx->addInputPort("Placement1", new TRasterFxPort, 1),
            "Cannot add placement anchor port");
    fx->getInputPort("Placement1")->setFx(new ParticleCachePlacementFixture);
  };
  auto disks = effect(file), anchoredDisks = effect(file);
  addPlacement(anchoredDisks);
  for (int assignment : {0, 1}) {
    mode(disks.getPointer(), "artworkAssignment", assignment);
    mode(anchoredDisks.getPointer(), "artworkAssignment", assignment);
    compare<TPixel32>(render(disks, 0), render(anchoredDisks, 0),
                      "Placement input changed fallback disk rendering");
  }
  auto artwork = effect(file, true), anchoredArtwork = effect(file, true);
  addPlacement(anchoredArtwork);
  for (const auto &fx : {artwork, anchoredArtwork}) {
    require(fx->addInputPort("Artwork2", new TRasterFxPort, 0),
            "Cannot add second artwork port");
    fx->getInputPort("Artwork2")->setFx(new ParticleCacheArtworkFixture);
    number(fx->getInputPort("Artwork2")->getFx(), "fixtureAlpha", .3);
    mode(fx.getPointer(), "sizeMode", 1);
  }
  for (int assignment : {0, 1}) {
    mode(artwork.getPointer(), "artworkAssignment", assignment);
    mode(anchoredArtwork.getPointer(), "artworkAssignment", assignment);
    compare<TPixelF>(render(artwork, 0, 128), render(anchoredArtwork, 0, 128),
                     "Placement input changed artwork indices or random assignment");
  }
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

  // In-place rewrites with the same size: the alias is built from file stamps
  // and must still change, both right away and once the file has settled.
  const QString otherPath = QDir(directory).filePath("other.bgeo");
  require(ParticleCache::writeFrame(otherPath, snapshot(3),
                                    ParticleCache::WriteOptions(), error),
          error.toStdString());
  auto overwrite = [&](const QString &source) {
    QFile input(source), output(path);
    require(input.open(QIODevice::ReadOnly), "Cannot read cache fixture");
    const QByteArray bytes = input.readAll();
    require(output.open(QIODevice::ReadWrite) &&
                output.size() == bytes.size() &&
                output.write(bytes) == bytes.size(),
            "Cannot rewrite cache fixture in place");
  };
  overwrite(otherPath);
  const auto rewritten = fx->getAlias(0, TRenderSettings());
  require(rewritten != after,
          "An immediate same-size rewrite did not invalidate the alias");

  // Aliases are computed on the UI thread: they must not decode frames. An
  // undecodable frame only fails when it is rendered.
  const QString garbage = QDir(directory).filePath("garbage.bgeo");
  {
    QFile file(garbage);
    require(file.open(QIODevice::WriteOnly) &&
                file.write(QByteArray(256, 'x')) == 256,
            "Cannot write undecodable fixture");
  }
  auto undecodable = effect(garbage);
  require(undecodable->getAlias(0, TRenderSettings()).find(":error:") ==
              std::string::npos,
          "Computing the alias decoded the particle frame");
  bool failed = false;
  try {
    render(undecodable, 0);
  } catch (...) {
    failed = true;
  }
  require(failed, "An undecodable particle frame rendered");

  // Native frame revisions are memoized by stamps of the sprites they verify.
  TRaster32P sprite(8, 8);
  sprite->fill(TPixel32(10, 20, 30, 255));
  const QString nativePath = nativePackage(directory + "/native-alias", sprite);
  auto native              = effect(nativePath);
  const auto nativeAlias   = native->getAlias(0, TRenderSettings());
  require(nativeAlias.find(":error:") == std::string::npos,
          "Native alias failed: " + nativeAlias);

  // Let every fixture settle so stamps alone identify them, as for real caches.
  QThread::msleep(2200);
  const auto settled = fx->getAlias(0, TRenderSettings());
  require(settled == fx->getAlias(0, TRenderSettings()),
          "A settled alias is not stable");
  // Unsettled files also carry their content digest, so only settled aliases
  // are expected to repeat exactly.
  const auto settledNative = native->getAlias(0, TRenderSettings());
  require(settledNative.find(":error:") == std::string::npos &&
              native->getAlias(0, TRenderSettings()) == settledNative,
          "A settled native alias changed");
  const QString restore = QDir(directory).filePath("restore.bgeo");
  require(ParticleCache::writeFrame(restore, snapshot(1),
                                    ParticleCache::WriteOptions(), error),
          error.toStdString());
  overwrite(restore);
  require(
      fx->getAlias(0, TRenderSettings()) != settled,
      "A same-size rewrite of a settled frame did not invalidate the alias");
  const auto restored = render(fx, 0);
  compare<TPixel32>(restored, render(effect(restore), 0),
                    "A rewritten frame rendered stale particles");

  QDir artwork(QFileInfo(nativePath).absoluteDir().filePath("artwork"));
  const auto sprites = artwork.entryInfoList(QDir::Files);
  require(!sprites.isEmpty(), "Native package has no sprite files");
  {
    QFile file(sprites.front().absoluteFilePath());
    require(file.open(QIODevice::ReadWrite) && file.seek(file.size() - 1) &&
                file.write("\x01", 1) == 1,
            "Cannot corrupt native sprite fixture");
  }
  require(native->getAlias(0, TRenderSettings()).find(":error:") !=
              std::string::npos,
          "A corrupted native sprite was hidden by the revision memo");
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
  // TRenderer creates a QOffscreenSurface even for these CPU-only fixtures.
  // CTest selects the offscreen platform; QCoreApplication has no screen.
  QGuiApplication application(argc, argv);
  // TRenderer's executor dereferences the global thread manager on creation.
  TThread::init();
  try {
    initStdFx();
    QTemporaryDir directory; require(directory.isValid(), "Cannot create fixture directory");
    const QString path = package(directory.path() + "/normal");
    const QString reordered = package(directory.path() + "/reordered", true);
    testTilesAndOrder(path, reordered);
    testTiming(path);
    testArtworkPrecision(directory.path());
    testNativeFloatAndLinear(directory.path());
    testArtworkTintColorSpace(directory.path());
    testHdrIntegerConversion(directory.path());
    testPlacementRouting(directory.path());
    testTrailsAndBlur(path);
    testPersistence(path, directory.path());
    testAliasInvalidation(directory.path());
    std::cout << "Particle cache rendering: 10 groups passed (precision, tiles/IDs, timing, native float/linear, artwork tint/alpha, HDR integer conversion, placement routing, temporal FX, persistence, invalidation).\n";
    return 0;
  } catch (const std::exception &e) {
    std::cerr << "Particle cache render failure: " << e.what() << '\n';
    return 1;
  } catch (const TException &e) {
    std::cerr << "Particle cache render failure: " << QString::fromStdWString(e.getMessage()).toStdString() << '\n';
    return 1;
  }
}
