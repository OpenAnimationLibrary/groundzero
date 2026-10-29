// Native particle evaluation must survive capture, persistence and playback.
// Run through the production FX registry and renderer, including both native
// particle generators. Checks deliberately remain enabled in Release builds.
#include "../stdfx.h"
#include "tfx.h"
#include "tfxparam.h"
#include "tnotanimatableparam.h"
#include "tparamset.h"
#include "trasterfx.h"
#include "trenderer.h"
#include "tthread.h"
#include "trop.h"
#include "toonz/particlebake.h"
#include "toonz/particlecache.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>

#include <algorithm>
#include <cmath>
#include <iostream>
#include <limits>
#include <map>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

DV_IMPORT_API void initStdFx();

namespace {
void require(bool condition, const std::string &message) {
  if (!condition) throw std::runtime_error(message);
}
const TRectD Domain(-32, -32, 32, 32);

std::string fixtureAlias(double frame, const TRenderSettings &settings) {
  const TAffine &a = settings.m_affine;
  return std::to_string(frame) + ":" + std::to_string(settings.m_bpp) +
      ":" + std::to_string(a.a11) + ":" + std::to_string(a.a12) +
      ":" + std::to_string(a.a13) + ":" + std::to_string(a.a21) +
      ":" + std::to_string(a.a22) + ":" + std::to_string(a.a23) +
      ":" + std::to_string(settings.m_linearColorSpace);
}

template <class Parameter>
Parameter *parameter(TFx *fx, const char *name) {
  auto *value = dynamic_cast<Parameter *>(fx->getParams()->getParam(name));
  require(value != nullptr, std::string("Missing parameter: ") + name);
  return value;
}
void number(TFx *fx, const char *name, double value) {
  parameter<TDoubleParam>(fx, name)->setDefaultValue(value);
}
void range(TFx *fx, const char *name, double first, double last) {
  parameter<TRangeParam>(fx, name)->setDefaultValue(DoublePair(first, last));
}
void integer(TFx *fx, const char *name, int value) {
  parameter<TIntParam>(fx, name)->setValue(value);
}
void enumeration(TFx *fx, const char *name, int value) {
  parameter<TIntEnumParam>(fx, name)->setValue(value);
}

void pattern(TTile &tile, double frame, bool background) {
  TRaster32P pixels(tile.getRaster()->getSize());
  for (int y = 0; y < pixels->getLy(); ++y) {
    TPixel32 *row = pixels->pixels(y);
    for (int x = 0; x < pixels->getLx(); ++x) {
      const int wx = int(std::floor(tile.m_pos.x + x));
      const int wy = int(std::floor(tile.m_pos.y + y));
      const bool inside = background ||
          (wx >= -4 && wx < 4 && wy >= -4 && wy < 4);
      if (!inside) { row[x] = TPixel32::Transparent; continue; }
      const int alpha = background ? 255 : ((wx == -4 || wy == -4) ? 96 : 208);
      const bool alternate = ((wx / 4 + wy / 4 + int(frame)) & 1) != 0;
      row[x] = TPixel32((alternate ? 220 : 70) * alpha / 255,
                       (alternate ? 65 : 190) * alpha / 255,
                       110 * alpha / 255, alpha);
    }
  }
  if (TRaster32P destination = tile.getRaster()) destination->copy(pixels);
  else TRop::convert(tile.getRaster(), pixels);
}
}  // namespace

class ParticleBakeSpriteFixture final : public TStandardRasterFx {
  FX_PLUGIN_DECLARATION(ParticleBakeSpriteFixture)
public:
  bool canHandle(const TRenderSettings &, double) override { return true; }
  std::string getAlias(double frame, const TRenderSettings &settings) const override {
    return "particle-bake-sprite-fixture:" + fixtureAlias(frame, settings);
  }
  TFxTimeRegion getTimeRegion() const override { return TFxTimeRegion(0, 1); }
  bool doGetBBox(double, TRectD &box, const TRenderSettings &) override {
    box = TRectD(-4, -4, 4, 4);
    return true;
  }
  void doCompute(TTile &tile, double frame, const TRenderSettings &) override {
    pattern(tile, frame, false);
  }
};
FX_PLUGIN_IDENTIFIER(ParticleBakeSpriteFixture, "particleBakeSpriteFixture")

class ParticleBakeBackgroundFixture final : public TStandardRasterFx {
  FX_PLUGIN_DECLARATION(ParticleBakeBackgroundFixture)
public:
  bool canHandle(const TRenderSettings &, double) override { return true; }
  std::string getAlias(double frame, const TRenderSettings &settings) const override {
    return "particle-bake-background-fixture:" + fixtureAlias(frame, settings);
  }
  TFxTimeRegion getTimeRegion() const override { return TFxTimeRegion(0, 31); }
  bool doGetBBox(double, TRectD &box, const TRenderSettings &) override {
    box = Domain;
    return true;
  }
  void doCompute(TTile &tile, double frame, const TRenderSettings &) override {
    pattern(tile, frame, true);
  }
};
FX_PLUGIN_IDENTIFIER(ParticleBakeBackgroundFixture, "particleBakeBackgroundFixture")

namespace {
class RenderSession {
  TRenderer renderer;
  unsigned long id;
public:
  RenderSession() : renderer(1), id(TRenderer::buildRenderId()) {
    renderer.install(id);
    renderer.declareRenderStart(id);
  }
  ~RenderSession() {
    renderer.declareRenderEnd(id);
    renderer.uninstall();
  }
  unsigned long identity() const { return id; }
  void begin(double frame) { renderer.declareFrameStart(frame); }
  void end(double frame) { renderer.declareFrameEnd(frame); }
};

TRasterFxP fixture(bool tiled, int mode = 0) {
  TFxP fx = TFx::create(tiled ? "STD_iwa_TiledParticlesFx" : "STD_particlesFx");
  require(bool(fx), "Native particle FX was not registered");
  fx->getInputPort("Texture1")->setFx(new ParticleBakeSpriteFixture);
  fx->getInputPort("Control1")->setFx(new ParticleBakeBackgroundFixture);
  integer(fx.getPointer(), "random_seed", 127);
  integer(fx.getPointer(), "starting_frame", 1);
  number(fx.getPointer(), "birth_rate", 2.5);
  number(fx.getPointer(), "length", 24);
  number(fx.getPointer(), "height", 20);
  range(fx.getPointer(), "lifetime", 20, 20);
  range(fx.getPointer(), "speed", .75, 1.5);
  range(fx.getPointer(), "scale", 80, 115);
  range(fx.getPointer(), "rot", -15, 25);
  range(fx.getPointer(), "opacity", 65, 100);
  enumeration(fx.getPointer(), "animation", 2);  // Cycle the two artwork frames.
  if (tiled) {
    integer(fx.getPointer(), "base_ctrl", 1);
    enumeration(fx.getPointer(), "rendermode", mode);
    number(fx.getPointer(), "triangleSize", 8);
    number(fx.getPointer(), "margin", 0);
    number(fx.getPointer(), "flap_velocity", 12);
  } else {
    range(fx.getPointer(), "trail", 2, 2);
    number(fx.getPointer(), "trail_step", 1);
    number(fx.getPointer(), "birth_color_fade", 35);
    integer(fx.getPointer(), "birth_color_ctrl", 1);
  }
  auto *raster = dynamic_cast<TRasterFx *>(fx.getPointer());
  require(raster != nullptr && ParticleBake::isSupported(fx.getPointer()),
          "Native particle capture does not support the fixture");
  return TRasterFxP(raster);
}

TRasterP image(int bpp) {
  TRasterP raster;
  if (bpp == 64) raster = TRaster64P(64, 64);
  else raster = TRaster32P(64, 64);
  raster->clear();
  return raster;
}

TRasterP render(TRasterFx *fx, double frame, const TRenderSettings &settings,
                int strips = 1) {
  RenderSession session;
  session.begin(frame);
  TRasterP result = image(settings.m_bpp);
  try {
    for (int i = 0; i < strips; ++i) {
      const int y0 = 64 * i / strips, y1 = 64 * (i + 1) / strips;
      TRasterP part;
      if (settings.m_bpp == 64) part = TRaster64P(64, y1 - y0);
      else part = TRaster32P(64, y1 - y0);
      part->clear();
      TTile tile(part, TPointD(-32, -32 + y0));
      fx->compute(tile, frame, settings);
      result->copy(tile.getRaster(), TPoint(0, y0));
    }
  } catch (...) { session.end(frame); throw; }
  session.end(frame);
  return result;
}

template <class Pixel>
void comparePixels(const TRasterPT<Pixel> &expected,
                    const TRasterPT<Pixel> &actual, const std::string &label) {
  require(bool(expected) && bool(actual), label + ": unexpected pixel precision");
  require(expected->getSize() == actual->getSize(), label + ": wrong dimensions");
  std::size_t occupied = 0, differing = 0;
  int maximum = 0;
  for (int y = 0; y < expected->getLy(); ++y) {
    const auto *a = expected->pixels(y), *b = actual->pixels(y);
    for (int x = 0; x < expected->getLx(); ++x) {
      if (a[x].m) ++occupied;
      const int delta = std::max({std::abs(int(a[x].r) - int(b[x].r)),
                                 std::abs(int(a[x].g) - int(b[x].g)),
                                 std::abs(int(a[x].b) - int(b[x].b)),
                                 std::abs(int(a[x].m) - int(b[x].m))});
      if (delta) ++differing;
      maximum = std::max(maximum, delta);
    }
  }
  require(occupied > 0, label + ": empty reference does not validate playback");
  require(differing == 0, label + ": " + std::to_string(differing) +
          " pixels changed; maximum channel difference " + std::to_string(maximum));
}
void compare(const TRasterP &expected, const TRasterP &actual,
             const std::string &label) {
  if (TRaster64P raster = expected)
    comparePixels<TPixel64>(raster, TRaster64P(actual), label);
  else comparePixels<TPixel32>(TRaster32P(expected), TRaster32P(actual), label);
}

ParticleBake::Frame capture(TRasterFx *fx, double frame,
                            const TRenderSettings &settings,
                            bool requireArtwork = true) {
  ParticleBake::Options options;
  options.domain = Domain;
  ParticleBake::Frame result;
  QString error;
  RenderSession outer;
  outer.begin(frame);
  const auto rendererId = TRenderer::instance().rendererId();
  const bool ok = ParticleBake::captureFrame(fx, frame, settings, options, result,
                                             error);
  const bool preserved = TRenderer::renderId() == outer.identity() &&
      TRenderer::instance().rendererId() == rendererId;
  outer.end(frame);
  require(ok, "Capture failed: " + error.toStdString());
  require(preserved, "Capture replaced the active renderer context");
  require(!result.particles.empty(), "Native capture returned no live particle state");
  if (requireArtwork)
    require(!result.draws.empty(), "Native capture returned no artwork draws");
  std::set<std::uint64_t> identities;
  for (const auto &particle : result.particles) {
    require(identities.insert(particle.id).second,
            "Native capture repeated a live particle identity");
    require(!particle.artwork && !particle.background,
            "Native live particle state contains prepared drawing data");
  }
  for (const auto &draw : result.draws)
    require(bool(draw.artwork), "Captured draw has no prepared artwork");
  return result;
}

void compareState(const std::vector<ParticleBake::DrawSample> &expected,
                  const std::vector<ParticleBake::DrawSample> &actual,
                  const std::string &label) {
  require(expected.size() == actual.size(), label + ": live particle count changed");
  std::map<std::uint64_t, const ParticleBake::DrawSample *> byId;
  for (const auto &p : actual) byId.emplace(p.id, &p);
  for (const auto &a : expected) {
    const auto found = byId.find(a.id);
    require(found != byId.end(), label + ": live particle identity changed");
    const auto &b = *found->second;
    require(a.position == b.position && a.velocity == b.velocity &&
                a.age == b.age && a.lifetime == b.lifetime &&
                a.source == b.source && a.artworkFrame == b.artworkFrame &&
                a.scale == b.scale && a.angle == b.angle &&
                a.opacity == b.opacity && a.tint == b.tint &&
                a.tintMix == b.tintMix && a.rasterToWorld == b.rasterToWorld,
            label + ": live particle state depends on frame request order");
  }
}

void compareInterchange(const std::vector<ParticleBake::DrawSample> &expected,
                        const ParticleCache::Snapshot &actual, double fps) {
  require(actual.hasStableIds && actual.hasVelocity,
          "Native interchange lost identity or velocity attributes");
  require(expected.size() == actual.particles.size(),
          "Native interchange omitted live particles or added trail draws");
  std::map<std::int64_t, const ParticleCache::Particle *> byId;
  for (const auto &p : actual.particles) byId.emplace(p.id, &p);
  auto near = [](double a, double b) {
    return std::abs(a - b) <= 2e-6 * std::max({1.0, std::abs(a), std::abs(b)});
  };
  for (const auto &a : expected) {
    const auto found = byId.find(std::int64_t(a.id));
    require(found != byId.end(), "Native interchange lost a live birth identity");
    const auto &b = *found->second;
    require(near(a.position.x, b.position[0]) && near(a.position.y, b.position[1]) &&
                near(a.velocity.x, b.velocity[0]) && near(a.velocity.y, b.velocity[1]) &&
                near(a.age / fps, b.age) && near(a.lifetime / fps, b.lifetime) &&
                near(a.scale, b.size) && near(a.angle, b.rotation) &&
                near(a.opacity, b.tint[3]) && a.source == b.artworkIndex &&
                near(a.artworkFrame, b.artworkFrame),
            "Native interchange changed live particle state or time units");
  }
}

TRasterP composite(const ParticleBake::Frame &frame,
                   const TRenderSettings &settings) {
  TRasterP result = image(settings.m_bpp);
  for (const auto &draw : frame.draws)
    TRop::over(result, draw.artwork, TTranslation(32, 32) *
               settings.m_affine * draw.rasterToWorld);
  return result;
}

void testCapture(bool tiled, int mode, int bpp, bool motionBlur = false) {
  TRasterFxP fx = fixture(tiled, mode);
  if (motionBlur) {
    parameter<TBoolParam>(fx.getPointer(), "motion_blur")->setValue(true);
    number(fx.getPointer(), "motion_blur_gamma_adjust", .4);
  }
  TRenderSettings settings;
  settings.m_bpp = bpp;
  settings.m_cameraBox = Domain;
  const std::string label = std::string(tiled ? "tiled" : "standard") +
      "/mode=" + std::to_string(mode) + "/bpp=" + std::to_string(bpp) +
      (motionBlur ? "/motion blur" : "");
  std::vector<ParticleBake::DrawSample> firstState;
  for (int frame : {3, 7, 3}) {
    const TRasterP reference = render(fx.getPointer(), frame, settings);
    const auto result = capture(fx.getPointer(), frame, settings);
    compare(reference, composite(result, settings), label + "/capture");
    if (frame == 3) {
      if (firstState.empty()) firstState = result.particles;
      else compareState(firstState, result.particles, label);
    }
  }
}

void testInvisibleState(bool tiled) {
  TRasterFxP fx = fixture(tiled);
  range(fx.getPointer(), "scale", 0, 0);
  range(fx.getPointer(), "speed", 0, 0);
  TRenderSettings settings;
  const auto result = capture(fx.getPointer(), 3, settings, false);
  std::size_t newborns = 0;
  for (const auto &particle : result.particles) {
    require(particle.scale == 0, "Invisible-state fixture did not produce zero-size particles");
    if (particle.age == 0) {
      ++newborns;
      require(particle.velocity == TPointD(),
              "Newborn velocity incorrectly includes displacement from the origin");
    }
  }
  require(newborns > 0, "Invisible-state fixture did not exercise newborn particles");
  const auto later = capture(fx.getPointer(), 7, settings, false);
  require(later.particles.size() > result.particles.size(),
          "Invisible particles stopped participating in simulation");
  compareState(result.particles, capture(fx.getPointer(), 3, settings, false).particles,
               tiled ? "tiled invisible state" : "standard invisible state");
}

void testCancellationAndLimits() {
  TRasterFxP fx = fixture(false);
  TRenderSettings settings;
  ParticleBake::Options options;
  options.domain = Domain;
  ParticleBake::Frame result;
  QString error;
  RenderSession outer;
  outer.begin(7);
  const auto rendererId = TRenderer::instance().rendererId();
  require(!ParticleBake::captureFrame(fx.getPointer(), 7, settings, options,
                                      result, error, [] { return true; }),
          "Pre-cancelled capture succeeded");
  require(result.draws.empty() && result.particles.empty() && !error.isEmpty(),
          "Cancelled capture exposed partial draws or no error");
  int polls = 0;
  require(!ParticleBake::captureFrame(fx.getPointer(), 7, settings, options,
                                      result, error, [&] { return ++polls >= 3; }),
          "Capture ignored cancellation during evaluation");
  require(polls >= 3 && result.draws.empty() && result.particles.empty(),
          "Mid-capture cancellation was not exercised or leaked partial data");
  options.maxDraws = 1;
  require(!ParticleBake::captureFrame(fx.getPointer(), 7, settings, options,
                                      result, error),
          "Capture ignored its draw-count limit");
  require(result.draws.empty() && result.particles.empty(),
          "Failed capture retained partial output");
  TRenderSettings overflowing(settings);
  overflowing.m_affine = TScale(std::numeric_limits<double>::max());
  require(!ParticleBake::captureFrame(fx.getPointer(), 7, overflowing, options,
                                      result, error),
          "Capture accepted an overflowing transformed sampling domain");
  require(result.draws.empty() && result.particles.empty() && !error.isEmpty(),
          "Invalid sampling domain retained partial capture data");
  require(TRenderer::renderId() == outer.identity() &&
              TRenderer::instance().rendererId() == rendererId,
          "Cancellation or resource limit replaced the active renderer");
  outer.end(7);
}

void testPackagePlayback(bool tiled, int mode, int bpp, bool motionBlur = false) {
  QTemporaryDir temporary;
  require(temporary.isValid(), "Cannot create temporary particle package directory");
  const QString package = temporary.filePath("native-cache");
  TRasterFxP source = fixture(tiled, mode);
  if (motionBlur) {
    parameter<TBoolParam>(source.getPointer(), "motion_blur")->setValue(true);
    number(source.getPointer(), "motion_blur_gamma_adjust", .4);
  }
  TRenderSettings settings;
  settings.m_bpp = bpp;
  settings.m_cameraBox = Domain;
  ParticleCache::Metadata metadata;
  metadata.firstFrame = 3;
  metadata.lastFrame = 5;
  metadata.framesPerSecond = 24;
  QString error;
  int written = 0;
  std::map<int, std::vector<ParticleBake::DrawSample>> states;
  const bool saved = ParticleBake::writePackage(
      package, metadata,
      [&](int frame, ParticleBake::Frame &result, QString &) {
        result = capture(source.getPointer(), frame, settings);
        states.emplace(frame, result.particles);
        ++written;
        return true;
      }, error);
  require(saved, "Cannot save native particle package: " + error.toStdString());
  require(written == 3, "Native package did not evaluate its complete frame range");
  const QString manifest = QDir(package).filePath("cache.otpc");
  ParticleCache::Sequence sequence;
  require(ParticleCache::openSequence(manifest, sequence, error),
          "Cannot reopen native particle package: " + error.toStdString());
  require(sequence.frames.size() == 3, "Native package lost frames");

  TFxP cacheOwner = TFx::create("STD_particleCacheFx");
  require(bool(cacheOwner), "Particle Cache FX was not registered");
  auto *cache = dynamic_cast<TRasterFx *>(cacheOwner.getPointer());
  require(cache != nullptr, "Particle Cache FX is not a raster effect");
  parameter<TStringParam>(cache, "cacheFile")->setValue(manifest.toStdWString());
  number(cache, "startFrame", metadata.firstFrame);
  number(cache, "sceneFps", metadata.framesPerSecond);

  ParticleCache::Playback playback;
  playback.startFrame = metadata.firstFrame;
  playback.sceneFramesPerSecond = metadata.framesPerSecond;
  playback.interpolate = false;
  const std::string label = std::string(tiled ? "tiled" : "standard") +
      "/package/mode=" + std::to_string(mode) + "/bpp=" + std::to_string(bpp) +
      (motionBlur ? "/motion blur" : "");
  // Reverse/repeated frame requests must read the same persisted samples,
  // independent of both simulation and image-cache history.
  for (int frame : {5, 3, 4, 3}) {
    TRasterP reference = render(source.getPointer(), frame, settings);
    const auto snapshot = ParticleCache::sampleSequence(
        sequence, frame, playback, sequence.mapping, error);
    require(bool(snapshot), "Cannot sample native package: " + error.toStdString());
    compareInterchange(states.at(frame), *snapshot, metadata.framesPerSecond);
    ParticleBake::Frame restored;
    require(ParticleBake::loadFrame(*snapshot, restored, error),
            "Cannot read prepared sprites: " + error.toStdString());
    require(!restored.draws.empty(), "Package lost all prepared sprites");
    compare(reference, composite(restored, settings), label + "/stored draws");
    compare(reference, render(cache, frame, settings), label + "/FX playback");
    compare(reference, render(cache, frame, settings, 4), label + "/tiled playback");
  }

  // A collected/copied package must be self-contained: no relative path may
  // silently point back to the original package's sprite directory.
  const QString moved = temporary.filePath("relocated-cache");
  require(QDir().rename(package, moved), "Cannot relocate particle package fixture");
  parameter<TStringParam>(cache, "cacheFile")->setValue(
      QDir(moved).filePath("cache.otpc").toStdWString());
  ParticleCache::clearMemoryCache();
  compare(render(source.getPointer(), 4, settings), render(cache, 4, settings),
          label + "/relocated");
}

void testPackageFailureAtomicity() {
  QTemporaryDir temporary;
  require(temporary.isValid(), "Cannot create failure fixture directory");
  TRasterFxP source = fixture(false);
  TRenderSettings settings;
  ParticleCache::Metadata metadata;
  metadata.firstFrame = 3;
  metadata.lastFrame = 5;
  QString error;
  int completed = 0;
  const QString cancelled = temporary.filePath("cancelled-cache");
  const bool saved = ParticleBake::writePackage(
      cancelled, metadata,
      [&](int frame, ParticleBake::Frame &result, QString &) {
        result = capture(source.getPointer(), frame, settings);
        return true;
      }, error, [&] { return completed >= 1; },
      [&](int count, int) { completed = count; });
  require(!saved && completed >= 1 && !error.isEmpty(),
          "Export did not exercise cancellation after writing a frame");
  require(!QFileInfo::exists(cancelled),
          "Cancelled export published an incomplete cache package");

  const QString existing = temporary.filePath("existing-cache");
  require(QDir().mkpath(existing), "Cannot create existing destination fixture");
  const QString markerPath = QDir(existing).filePath("keep.txt");
  QFile marker(markerPath);
  require(marker.open(QIODevice::WriteOnly) && marker.write("existing asset") == 14,
          "Cannot create existing asset marker");
  marker.close();
  int evaluated = 0;
  require(!ParticleBake::writePackage(
      existing, metadata,
      [&](int, ParticleBake::Frame &, QString &) { ++evaluated; return false; },
      error), "Export replaced a pre-existing directory");
  require(evaluated == 0, "Export evaluated before rejecting existing output");
  require(marker.open(QIODevice::ReadOnly) && marker.readAll() == "existing asset",
          "Failed export modified an existing asset");
  marker.close();
  require(!QFileInfo::exists(QDir(existing).filePath("cache.otpc")),
          "Failed export published a success manifest");
}
}  // namespace

int main(int argc, char **argv) {
  QCoreApplication application(argc, argv);
  TThread::init();
  initStdFx();
  try {
    testCapture(false, 0, 32);
    testCapture(false, 0, 64);
    testCapture(false, 0, 32, true);
    testCapture(true, 0, 32);   // Background plus flying pieces.
    testCapture(true, 2, 32);   // Remaining background only.
    testCapture(true, 3, 64);   // Illuminated fluttering pieces.
    testInvisibleState(false);
    testInvisibleState(true);
    testCancellationAndLimits();
    testPackagePlayback(false, 0, 32);
    testPackagePlayback(false, 0, 64, true);
    testPackagePlayback(true, 0, 32);
    testPackagePlayback(true, 3, 64);
    testPackageFailureAtomicity();
    std::cout << "Native particle capture/package/playback: 14 cases passed\n";
    return 0;
  } catch (const std::exception &error) {
    std::cerr << "particlecache_bake: " << error.what() << '\n';
    return 1;
  } catch (const TException &error) {
    std::cerr << "particlecache_bake: "
              << QString::fromStdWString(error.getMessage()).toStdString() << '\n';
    return 1;
  }
}
