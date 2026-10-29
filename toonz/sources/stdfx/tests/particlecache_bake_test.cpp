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
#include "trasterimage.h"
#include "tthread.h"
#include "trop.h"
#include "toonz/fxdag.h"
#include "toonz/levelproperties.h"
#include "toonz/levelset.h"
#include "toonz/particlebake.h"
#include "toonz/particlecache.h"
#include "toonz/scenefx.h"
#include "toonz/stage.h"
#include "toonz/tcamera.h"
#include "toonz/tcolumnfx.h"
#include "toonz/tcolumnfxset.h"
#include "toonz/toonzscene.h"
#include "toonz/tstageobject.h"
#include "toonz/tstageobjecttree.h"
#include "toonz/txshcell.h"
#include "toonz/txsheet.h"
#include "toonz/txshleveltypes.h"
#include "toonz/txshsimplelevel.h"
#include "toonz/txshzeraryfxcolumn.h"

#include <QGuiApplication>
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
  TIntParamP m_lastFrame;
public:
  ParticleBakeSpriteFixture() : m_lastFrame(1) {
    bindParam(this, "fixtureLastFrame", m_lastFrame);
  }
  bool canHandle(const TRenderSettings &, double) override { return true; }
  std::string getAlias(double frame, const TRenderSettings &settings) const override {
    return "particle-bake-sprite-fixture:" + std::to_string(m_lastFrame->getValue()) +
           ":" + fixtureAlias(frame, settings);
  }
  TFxTimeRegion getTimeRegion() const override {
    return m_lastFrame->getValue() < 0 ? TFxTimeRegion::createUnlimited()
        : TFxTimeRegion(0, m_lastFrame->getValue() + 1);
  }
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

class ParticleBakeControlBoundsFixture final : public TStandardRasterFx {
  FX_PLUGIN_DECLARATION(ParticleBakeControlBoundsFixture)
  TIntParamP m_mode;
public:
  ParticleBakeControlBoundsFixture() : m_mode(0) {
    bindParam(this, "boundsMode", m_mode);
  }
  bool canHandle(const TRenderSettings &, double) override { return true; }
  std::string getAlias(double frame, const TRenderSettings &settings) const override {
    return "particle-bake-control-bounds-fixture:" + std::to_string(m_mode->getValue()) +
        ":" + fixtureAlias(frame, settings);
  }
  bool doGetBBox(double, TRectD &box, const TRenderSettings &) override {
    switch (m_mode->getValue()) {
    case 1:
    case 2: box = TConsts::infiniteRectD; break;
    case 3: box = TRectD(0, 0, 64, 64); break;
    default: box = TRectD(0, 0, 256, 256); break;
    }
    return true;
  }
  void doCompute(TTile &, double, const TRenderSettings &) override {
    throw std::runtime_error("Unsafe control allocation reached fixture evaluation");
  }
};
FX_PLUGIN_IDENTIFIER(ParticleBakeControlBoundsFixture, "particleBakeControlBoundsFixture")

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
                            bool requireArtwork = true,
                            const TRectD &domain = Domain,
                            const TRectD &pixelDomain = TRectD(),
                            int proceduralLastFrame = 0) {
  ParticleBake::Options options;
  options.domain = domain;
  options.pixelDomain = pixelDomain;
  options.proceduralArtworkLastFrame = proceduralLastFrame;
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
  const bool backgroundOnly = fx->getFxType() == "STD_iwa_TiledParticlesFx" &&
      parameter<TIntEnumParam>(fx, "rendermode")->getValue() == 2;
  if (backgroundOnly) {
    // The native background-only roll removes origins without ever creating
    // flying particles. Its prepared plate is the complete expected output.
    require(result.particles.empty(),
            "Background-only native capture acquired flying particle state");
    require(!result.draws.empty() && std::all_of(result.draws.begin(), result.draws.end(),
                [](const ParticleBake::DrawSample &draw) { return draw.background; }),
            "Background-only native capture lost its prepared background plate");
  } else
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
    compare(reference, composite(result, settings), label + "/capture/frame=" + std::to_string(frame));
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

void testShortLivedRandomStacking() {
  TRasterFxP source = fixture(false);
  enumeration(source.getPointer(), "top_layer", 4);  // Random stacking.
  number(source.getPointer(), "birth_rate", 1);
  range(source.getPointer(), "lifetime", 2, 2);
  range(source.getPointer(), "trail", 0, 0);
  range(source.getPointer(), "speed", 0, 0);
  range(source.getPointer(), "scale", 100, 100);
  TRenderSettings settings;
  settings.m_cameraBox = Domain;
  ParticleBake::Options options;
  options.domain = Domain;
  std::map<std::uint64_t, TPointD> positions;
  std::size_t survivingIdentities = 0;
  RenderSession session;
  for (int frame = 0; frame != 5; ++frame) {
    session.begin(frame);
    try {
      TRasterP reference = image(32);
      TTile tile(reference, Domain.getP00());
      // Keep one live renderer warm from the first birth, so the reference
      // consumes the causal RNG stream even after earlier particles die.
      source->compute(tile, frame, settings);
      ParticleBake::Frame captured;
      QString error;
      require(ParticleBake::captureFrame(source.getPointer(), frame, settings,
                                          options, captured, error),
              "Short-lived random-stack capture failed: " + error.toStdString());
      compare(reference, composite(captured, settings),
              "standard/short-lived random stacking/frame=" + std::to_string(frame));
      for (const auto &particle : captured.particles) {
        const auto previous = positions.find(particle.id);
        if (previous != positions.end()) {
          ++survivingIdentities;
          require(previous->second == particle.position,
                  "Stationary particle changed position under a surviving birth identity");
        }
        positions[particle.id] = particle.position;
      }
    } catch (...) { session.end(frame); throw; }
    session.end(frame);
  }
  require(survivingIdentities > 0,
          "Short-lived fixture never exercised a surviving birth identity");
}

void testControlSamplingLimits() {
  TRenderSettings settings;
  ParticleBake::Options options;
  options.domain = Domain;
  options.maxRasterDimension = 128;
  RenderSession session;
  session.begin(0);
  try {
    for (int mode = 0; mode != 4; ++mode) {
      TRasterFxP source = fixture(false);
      auto *control = new ParticleBakeControlBoundsFixture;
      integer(control, "boundsMode", mode);
      source->getInputPort("Control1")->setFx(control);
      if (mode == 1 || mode == 2) {
        // Infinite controls inherit emitter bounds after getBBox. Exercise
        // invalid resulting coordinates before the engine converts them to int.
        number(source.getPointer(), "length", mode == 1 ? 8e12
            : std::numeric_limits<double>::infinity());
      }
      if (mode == 3) {
        // The output and an 8-bit control fit this budget; the gradient's
        // 16-bit control must be rejected before allocating or evaluating it.
        options.maxArtworkBytes = 25000;
        integer(source.getPointer(), "gravity_ctrl", 1);
      }
      ParticleBake::Frame captured;
      QString error;
      require(!ParticleBake::captureFrame(source.getPointer(), 0, settings,
                                           options, captured, error),
              "Capture accepted an unsafe control sampling rectangle");
      require(error.contains(mode == 1 || mode == 2 ? "coordinate limits"
                                                    : "sampling exceeds"),
              "Control was evaluated before its bounds were rejected: " + error.toStdString());
      require(captured.particles.empty() && captured.draws.empty(),
              "Rejected control bounds leaked partial capture output");
    }
  } catch (...) { session.end(0); throw; }
  session.end(0);
}

void testSourcePointLimits() {
  TRenderSettings settings;
  ParticleBake::Options options;
  options.domain = Domain;
  options.maxDraws = 32;
  RenderSession session;
  session.begin(0);
  try {
    for (int mode = 0; mode != 4; ++mode) {
      TRasterFxP source = fixture(false);
      number(source.getPointer(), "birth_rate", 1);
      integer(source.getPointer(), "source_ctrl", 1);
      if (mode == 1)
        parameter<TBoolParam>(source.getPointer(), "source_gradation")->setValue(true);
      if (mode == 2)
        parameter<TBoolParam>(source.getPointer(), "multi_source")->setValue(true);
      if (mode == 3) {
        integer(source.getPointer(), "scale_ctrl", 1);
        parameter<TBoolParam>(source.getPointer(), "perspective_distribution")->setValue(true);
      }
      ParticleBake::Frame captured;
      QString error;
      require(!ParticleBake::captureFrame(source.getPointer(), 0, settings,
                                           options, captured, error),
              "Capture allowed its source point vectors to exceed the particle limit");
      require(error.contains("particle limit") && captured.particles.empty() &&
                  captured.draws.empty(),
              "Source point limit failed without rejecting partial capture output");
    }
  } catch (...) { session.end(0); throw; }
  session.end(0);
}

void testFractionalViewport(bool tiled) {
  TRasterFxP source = fixture(tiled);
  TRenderSettings settings;
  settings.m_affine = TTranslation(.25, -.75) * TScale(1.25);
  const TRectD viewport(-32.5, -31.5, 32.5, 31.5);
  settings.m_cameraBox = viewport;
  TRaster32P reference(65, 63);
  reference->clear();
  {
    RenderSession session;
    session.begin(3);
    TTile tile(reference, viewport.getP00());
    try { source->compute(tile, 3, settings); }
    catch (...) { session.end(3); throw; }
    session.end(3);
  }
  const auto captured = capture(source.getPointer(), 3, settings, true,
                                 settings.m_affine.inv() * viewport, viewport);
  require(captured.pixelDomain == viewport,
          "Explicit odd-sized viewport lost its half-pixel origin");
  TRaster32P replay(65, 63);
  replay->clear();
  for (const auto &draw : captured.draws)
    TRop::over(replay, draw.artwork,
               TTranslation(-viewport.x0, -viewport.y0) *
                   settings.m_affine * draw.rasterToWorld);
  compare(reference, replay, tiled ? "tiled/fractional viewport"
                                   : "standard/fractional viewport");
}

void testProceduralArtworkHorizon(bool tiled) {
  constexpr int horizon = 4;
  TRasterFxP unlimited = fixture(tiled);
  TRasterFxP finite = fixture(tiled);
  TFx *procedural = unlimited->getInputPort("Texture1")->getFx();
  integer(procedural, "fixtureLastFrame", -1);
  integer(finite->getInputPort("Texture1")->getFx(), "fixtureLastFrame", horizon);
  require(procedural->getTimeRegion().isUnlimited(),
          "Procedural artwork fixture did not declare an unlimited time range");
  TRenderSettings settings;
  settings.m_cameraBox = Domain;
  const std::string label = tiled ? "tiled/procedural horizon" : "standard/procedural horizon";
  std::vector<ParticleBake::DrawSample> firstState;
  bool animatedDrawing = false;
  for (int frame : {7, 3, 5, 3}) {
    // A bounded texture keeps its own range even when the procedural horizon
    // is zero. Never dispatch normal rendering on the unbounded fixture.
    const auto expected = capture(finite.getPointer(), frame, settings, true,
                                   Domain, TRectD(), 0);
    const auto actual = capture(unlimited.getPointer(), frame, settings, true,
                                 Domain, TRectD(), horizon);
    compareState(expected.particles, actual.particles, label + "/finite equivalent");
    compare(composite(expected, settings), composite(actual, settings),
            label + "/finite equivalent image");
    for (const auto &particle : actual.particles) {
      require(particle.artworkFrame >= 0 && particle.artworkFrame <= horizon,
              label + ": sampled beyond the fixed artwork horizon");
      animatedDrawing = animatedDrawing || particle.artworkFrame > 0;
    }
    if (frame == 3) {
      if (firstState.empty()) firstState = actual.particles;
      else compareState(firstState, actual.particles, label + "/random request order");
    }
  }
  require(animatedDrawing, label + ": fixture did not exercise animated procedural artwork");
  require(procedural->getTimeRegion().isUnlimited(),
          label + ": capture modified the original artwork time range");
  ParticleBake::Options options;
  options.domain = Domain;
  options.proceduralArtworkLastFrame = horizon;
  ParticleBake::Frame rejected;
  QString error;
  RenderSession session;
  session.begin(3);
  require(!ParticleBake::captureFrame(unlimited.getPointer(), 3, settings, options,
                                      rejected, error, [] { return true; }),
          label + ": cancelled procedural capture succeeded");
  require(rejected.draws.empty() && rejected.particles.empty() && !error.isEmpty(),
          label + ": cancelled capture retained partial output");
  options.proceduralArtworkLastFrame = -1;
  require(!ParticleBake::captureFrame(unlimited.getPointer(), 3, settings, options,
                                      rejected, error),
          label + ": invalid procedural artwork horizon was accepted");
  require(rejected.draws.empty() && rejected.particles.empty() && !error.isEmpty(),
          label + ": rejected horizon retained partial output");
  session.end(3);
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

void testScenePreparation(bool tiled, bool fullAffine = false) {
  QTemporaryDir temporary;
  require(temporary.isValid(), "Cannot create scene bake fixture directory");
  ToonzScene scene;
  TXsheet *xsheet = scene.getXsheet();
  TCamera *camera = xsheet->getStageObjectTree()->getCurrentCamera();
  require(camera != nullptr, "Scene bake fixture has no camera");
  camera->setSize(TDimensionD(64.0 / Stage::inch, 64.0 / Stage::inch));
  camera->setRes(TDimension(64, 64));
  TStageObject *cameraPlacement = xsheet->getStageObject(
      xsheet->getStageObjectTree()->getCurrentCameraId());
  cameraPlacement->getParam(TStageObject::T_X)->setValue(0, -2.0 / Stage::inch);
  cameraPlacement->getParam(TStageObject::T_Y)->setValue(0, 1.0 / Stage::inch);
  TXshSimpleLevelP artwork = new TXshSimpleLevel(L"particle-bake-scene-artwork");
  artwork->setType(OVL_XSHLEVEL);
  artwork->setScene(&scene);
  artwork->setPath(TFilePath(temporary.filePath("artwork..png").toStdWString()));
  artwork->getProperties()->setDpiPolicy(LevelProperties::DP_CustomDpi);
  artwork->getProperties()->setDpi(Stage::inch);
  scene.getLevelSet()->insertLevel(artwork.getPointer());
  std::vector<TRasterP> drawings;
  for (int drawing = 0; drawing < 2; ++drawing) {
    TRaster32P raster(12, 12);
    TTile tile(raster, TPointD(-6, -6));
    pattern(tile, drawing, false);
    drawings.push_back(raster->clone());
    TRasterImageP image(raster);
    image->setDpi(Stage::inch, Stage::inch);
    artwork->setFrame(TFrameId(drawing + 1), image);
    require(xsheet->setCell(drawing, 0,
                            TXshCell(artwork.getPointer(), TFrameId(drawing + 1))),
            "Cannot expose scene artwork drawing");
  }
  TFx *artworkColumn = xsheet->getColumn(0)->getFx();
  TRasterFxP source = fixture(tiled);
  source->getInputPort("Texture1")->setFx(artworkColumn);
  TXshZeraryFxColumnP column = new TXshZeraryFxColumn(8);
  TZeraryColumnFx *wrapper = column->getZeraryColumnFx();
  wrapper->setZeraryFx(source.getPointer());
  xsheet->insertColumn(1, column.getPointer());
  xsheet->clearCells(4, 1);
  TStageObject *artworkPlacement = xsheet->getStageObject(TStageObjectId::ColumnId(0));
  TStageObject *sourcePlacement = xsheet->getStageObject(TStageObjectId::ColumnId(1));
  artworkPlacement->getParam(TStageObject::T_X)->setValue(0, 3.0 / Stage::inch);
  sourcePlacement->getParam(TStageObject::T_X)->setValue(0, 4.0 / Stage::inch);
  sourcePlacement->getParam(TStageObject::T_Y)->setValue(0, -3.0 / Stage::inch);
  if (fullAffine) {
    sourcePlacement->getParam(TStageObject::T_Angle)->setValue(0, 25.0);
    sourcePlacement->getParam(TStageObject::T_ShearX)->setValue(0, .2);
    sourcePlacement->getParam(TStageObject::T_ScaleX)->setValue(0, -1.1);
    sourcePlacement->getParam(TStageObject::T_ScaleY)->setValue(0, .85);
  }
  const TAffine originalPlacement = sourcePlacement->getPlacement(3);
  require(!originalPlacement.isIdentity(), "Scene fixture has no column placement");
  require(cameraPlacement->getParam(TStageObject::T_X, 3) == -2.0 / Stage::inch &&
              cameraPlacement->getParam(TStageObject::T_Y, 3) == 1.0 / Stage::inch &&
              artworkPlacement->getParam(TStageObject::T_X, 3) == 3.0 / Stage::inch &&
              sourcePlacement->getParam(TStageObject::T_X, 3) == 4.0 / Stage::inch &&
              sourcePlacement->getParam(TStageObject::T_Y, 3) == -3.0 / Stage::inch,
          "Scene fixture lost its configured artwork, camera or source translation");
  require(xsheet->getCell(3, 0).isEmpty() && xsheet->getCell(4, 1).isEmpty(),
          "Scene fixture did not create artwork and generator exposure gaps");

  TRenderSettings settings;
  settings.m_cameraBox = Domain;
  const std::string label = std::string(tiled ? "tiled/scene" : "standard/scene") +
      (fullAffine ? "/rotated-sheared-reflected" : "/translated");
  const std::string sourceAlias = source->getAlias(3, settings);
  TFx *controlBefore = source->getInputPort("Control1")->getFx();
  const TXshCell sourceCell = xsheet->getCell(3, 1);
  auto build = [&](int frame, BSFX_Transforms_Enum transforms) {
    return buildSceneFx(&scene, double(frame), xsheet, TFxP(wrapper),
                        transforms, false);
  };
  // Use the production scene builder as the source of the camera DPI, camera
  // and column transforms, just as the bake dialog does. Strip only its outer
  // affine nodes; artwork's own transforms remain in the native source graph.
  TFxP expanded = build(3, BSFX_DEFAULT_TR);
  TFxP unwrapped = expanded;
  TAffine placement;
  while (auto affine = dynamic_cast<NaAffineFx *>(unwrapped.getPointer())) {
    placement = placement * affine->getPlacement(3);
    unwrapped = affine->getInputPort(0)->getFx();
  }
  require(ParticleBake::isSupported(unwrapped.getPointer()),
          label + ": full scene expansion did not retain the native generator");
  TRenderSettings bakeSettings(settings);
  bakeSettings.m_affine = settings.m_affine * placement;
  const TRectD bakeDomain = bakeSettings.m_affine.inv() * Domain;
  require(bakeDomain != Domain, label + ": source-local viewport did not change");
  if (fullAffine)
    require(placement.det() < 0 && placement.a12 != 0 && placement.a21 != 0,
            label + ": fixture did not exercise a reflected non-axis-aligned affine");
  auto sceneImage = [&](int frame) {
    TFxP owner = build(frame, BSFX_DEFAULT_TR);
    auto *raster = dynamic_cast<TRasterFx *>(owner.getPointer());
    require(raster != nullptr, label + ": scene lost an exposed particle frame");
    return render(raster, frame, settings);
  };
  std::map<int, TRasterP> before;
  for (int frame : {3, 5}) {
    before.emplace(frame, sceneImage(frame));
    TFxP prepared = build(frame, BSFX_NO_TR);
    auto *native = dynamic_cast<TRasterFx *>(prepared.getPointer());
    require(native && native != source.getPointer() && ParticleBake::isSupported(native),
            label + ": wrapper did not produce a detached native bake source");
    require(native->getInputPort("Texture1")->isConnected() &&
                native->getInputPort("Texture1")->getFx() != artworkColumn,
            label + ": scene preparation lost artwork placement at an empty cell");
    // Match both the pixel domain and affine received by the placed native FX.
    // This matters for tiled particles, whose initial grid uses the pixel tile.
    const auto captured = capture(native, frame, bakeSettings, true, bakeDomain, Domain);
    require(captured.pixelDomain == Domain, label + ": capture enlarged the pixel viewport");
    compare(before.at(frame), render(native, frame, bakeSettings),
            label + "/prepared source placement");
    compare(before.at(frame), composite(captured, bakeSettings),
            label + "/prepared capture");
    compare(before.at(frame), sceneImage(frame), label + "/source after capture");
  }
  require(!build(4, BSFX_NO_TR), label + ": empty generator exposure produced a source");

  ParticleCache::Metadata metadata;
  metadata.firstFrame = 3;
  metadata.lastFrame = 5;
  metadata.framesPerSecond = 24;
  const QString package = temporary.filePath("scene-cache");
  QString error;
  int emptyFrames = 0;
  const bool saved = ParticleBake::writePackage(package, metadata,
      [&](int frame, ParticleBake::Frame &result, QString &) {
        // This is the dialog's source preparation path: preserve empty source
        // exposures, otherwise expand the column wrapper without its placement.
        if (column->getCell(frame).isEmpty()) {
          ++emptyFrames;
          require(!build(frame, BSFX_NO_TR), "Empty scene frame unexpectedly expanded");
          result = ParticleBake::Frame();
          result.frame = frame;
          result.domain = bakeDomain;
          result.pixelDomain = Domain;
          result.renderAffine = bakeSettings.m_affine;
          result.bitsPerPixel = bakeSettings.m_bpp;
          result.linearColorSpace = bakeSettings.m_linearColorSpace;
          result.colorSpaceGamma = bakeSettings.m_colorSpaceGamma;
          return true;
        }
        TFxP prepared = build(frame, BSFX_NO_TR);
        auto *native = dynamic_cast<TRasterFx *>(prepared.getPointer());
        require(native && ParticleBake::isSupported(native), "Cannot prepare scene bake source");
        result = capture(native, frame, bakeSettings, true, bakeDomain, Domain);
        return true;
      }, error);
  require(saved, label + ": scene package failed: " + error.toStdString());
  require(emptyFrames == 1, label + ": exposure gap was not included in the package");
  require(source->getAlias(3, settings) == sourceAlias &&
              source->getInputPort("Texture1")->getFx() == artworkColumn &&
              source->getInputPort("Control1")->getFx() == controlBefore &&
              wrapper->getZeraryFx() == source.getPointer(),
          label + ": baking changed the live source graph");

  ParticleCache::Sequence sequence;
  require(ParticleCache::openSequence(QDir(package).filePath("cache.otpc"), sequence, error),
          label + ": cannot reopen scene package");
  ParticleCache::Playback playback;
  playback.startFrame = 3;
  playback.sceneFramesPerSecond = 24;
  playback.interpolate = false;
  for (int frame : {3, 4, 5}) {
    const auto snapshot = ParticleCache::sampleSequence(
        sequence, frame, playback, sequence.mapping, error);
    require(bool(snapshot), label + ": cannot read scene package frame");
    ParticleBake::Frame restored;
    require(ParticleBake::loadFrame(*snapshot, restored, error),
            label + ": cannot restore scene draw recipe");
    require(restored.pixelDomain == Domain && restored.renderAffine == bakeSettings.m_affine,
            label + ": package changed the exact viewport or sampling affine");
    if (frame == 4)
      require(restored.draws.empty() && restored.particles.empty(),
              label + ": empty exposure acquired particle data");
    else
      compare(before.at(frame), composite(restored, bakeSettings),
              label + "/stored scene draw recipe");
  }

  TFxP cache = TFx::create("STD_particleCacheFx");
  require(bool(cache), "Particle Cache FX was not registered");
  parameter<TStringParam>(cache.getPointer(), "cacheFile")->setValue(
      QDir(package).filePath("cache.otpc").toStdWString());
  number(cache.getPointer(), "startFrame", 3);
  number(cache.getPointer(), "sceneFps", 24);
  cache->getInputPort("Artwork1")->setFx(artworkColumn);
  // Keep the procedural generator in the scene while replacing only the
  // wrapper's inner FX, matching Bake and Use Cache's undoable replacement.
  xsheet->getFxDag()->getInternalFxs()->addFx(source.getPointer());
  wrapper->setZeraryFx(cache.getPointer());
  for (int frame : {5, 3, 3})
    compare(before.at(frame), sceneImage(frame), label + "/placed cache playback");
  require(!build(4, BSFX_DEFAULT_TR), label + ": replacement filled an empty exposure");
  auto *cacheRaster = dynamic_cast<TRasterFx *>(cache.getPointer());
  require(cacheRaster != nullptr, "Particle Cache FX is not a raster effect");
  const TRaster32P empty = render(cacheRaster, 4, settings);
  for (int y = 0; y < empty->getLy(); ++y)
    for (int x = 0; x < empty->getLx(); ++x)
      require(empty->pixels(y)[x] == TPixel32::Transparent,
              label + ": persisted empty exposure contains pixels");
  wrapper->setZeraryFx(source.getPointer());
  xsheet->getFxDag()->getInternalFxs()->removeFx(source.getPointer());
  for (int frame : {3, 5})
    compare(before.at(frame), sceneImage(frame), label + "/restored procedural source");
  require(sourcePlacement->getPlacement(3) == originalPlacement &&
              xsheet->getCell(3, 1) == sourceCell && xsheet->getCell(4, 1).isEmpty(),
          label + ": cache replacement changed placement or exposure cells");
  for (int drawing = 0; drawing < 2; ++drawing) {
    const TRasterImageP image = artwork->getFrame(TFrameId(drawing + 1), false);
    require(bool(image), "Scene bake lost an artwork drawing");
    compare(drawings[drawing], image->getRaster(), label + "/original artwork retained");
    require(xsheet->getCell(drawing, 0).m_level.getPointer() == artwork.getPointer() &&
                xsheet->getCell(drawing, 0).getFrameId() == TFrameId(drawing + 1),
            label + ": source artwork exposures changed");
  }
}
}  // namespace

int main(int argc, char **argv) {
  QGuiApplication application(argc, argv);
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
    testShortLivedRandomStacking();
    testControlSamplingLimits();
    testSourcePointLimits();
    testFractionalViewport(false);
    testFractionalViewport(true);
    testProceduralArtworkHorizon(false);
    testProceduralArtworkHorizon(true);
    testCancellationAndLimits();
    testPackagePlayback(false, 0, 32);
    testPackagePlayback(false, 0, 64, true);
    testPackagePlayback(true, 0, 32);
    testPackagePlayback(true, 3, 64);
    testPackageFailureAtomicity();
    testScenePreparation(false);
    testScenePreparation(true);
    testScenePreparation(false, true);
    testScenePreparation(true, true);
    std::cout << "Native particle capture/package/playback: 25 cases passed\n";
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
