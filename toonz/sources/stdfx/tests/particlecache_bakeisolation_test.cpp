// A native generator used as a control must remain outside the selected
// generator's recorded state, and outside its live renderer caches.
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

#include <QGuiApplication>
#include <iostream>
#include <stdexcept>

DV_IMPORT_API void initStdFx();

namespace {
const TRectD Domain(-32, -32, 32, 32);
void require(bool condition, const std::string &message) {
  if (!condition) throw std::runtime_error(message);
}
template <class Parameter>
Parameter *parameter(TFx *fx, const char *name) {
  auto *p = dynamic_cast<Parameter *>(fx->getParams()->getParam(name));
  require(p != nullptr, std::string("Missing parameter: ") + name);
  return p;
}
void number(TFx *fx, const char *name, double value) {
  parameter<TDoubleParam>(fx, name)->setDefaultValue(value);
}
void integer(TFx *fx, const char *name, int value) {
  parameter<TIntParam>(fx, name)->setValue(value);
}
void range(TFx *fx, const char *name, double first, double last) {
  parameter<TRangeParam>(fx, name)->setDefaultValue(DoublePair(first, last));
}
}

class ParticleBakeIsolationArtwork final : public TStandardRasterFx {
  FX_PLUGIN_DECLARATION(ParticleBakeIsolationArtwork)
public:
  bool canHandle(const TRenderSettings &, double) override { return true; }
  std::string getAlias(double frame, const TRenderSettings &) const override {
    return "particle-bake-isolation-artwork:" + std::to_string(frame) + ":" +
        std::to_string(getIdentifier());
  }
  TFxTimeRegion getTimeRegion() const override { return TFxTimeRegion(0, 2); }
  bool doGetBBox(double, TRectD &box, const TRenderSettings &) override {
    box = TRectD(-4, -4, 4, 4);
    return true;
  }
  void doCompute(TTile &tile, double frame, const TRenderSettings &settings) override {
    TRaster32P pixels(tile.getRaster()->getSize());
    const TAffine inverse = settings.m_affine.inv();
    for (int y = 0; y < pixels->getLy(); ++y)
      for (int x = 0; x < pixels->getLx(); ++x) {
        const TPointD p = inverse * (tile.m_pos + TPointD(x, y));
        pixels->pixels(y)[x] = p.x >= -4 && p.x < 4 && p.y >= -4 && p.y < 4
            ? (frame == 0 ? TPixel32(240, 70, 40) : TPixel32(40, 190, 230))
            : TPixel32::Transparent;
      }
    if (TRaster32P output = tile.getRaster()) output->copy(pixels);
    else TRop::convert(tile.getRaster(), pixels);
  }
};
FX_PLUGIN_IDENTIFIER(ParticleBakeIsolationArtwork, "particleBakeIsolationArtwork")

namespace {
class RenderSession {
  TRenderer renderer;
  unsigned long id;
public:
  RenderSession() : renderer(1), id(TRenderer::buildRenderId()) {
    renderer.install(id);
    renderer.declareRenderStart(id);
    renderer.declareFrameStart(3);
  }
  ~RenderSession() {
    renderer.declareFrameEnd(3);
    renderer.declareRenderEnd(id);
    renderer.uninstall();
  }
};

TRasterFxP generator(bool tiled, int seed, double birthRate) {
  TFxP fx = TFx::create(tiled ? "STD_iwa_TiledParticlesFx" : "STD_particlesFx");
  require(bool(fx), "Native isolation fixture was not registered");
  fx->getInputPort("Texture1")->setFx(new ParticleBakeIsolationArtwork);
  integer(fx.getPointer(), "random_seed", seed);
  integer(fx.getPointer(), "starting_frame", 1);
  number(fx.getPointer(), "birth_rate", birthRate);
  number(fx.getPointer(), "length", 32);
  number(fx.getPointer(), "height", 32);
  range(fx.getPointer(), "lifetime", 40, 40);
  range(fx.getPointer(), "speed", 0, 0);
  range(fx.getPointer(), "scale", 100, 100);
  range(fx.getPointer(), "opacity", 100, 100);
  if (tiled) {
    parameter<TIntEnumParam>(fx.getPointer(), "rendermode")->setValue(1);
    number(fx.getPointer(), "triangleSize", 8);
    number(fx.getPointer(), "margin", 0);
  }
  auto *raster = dynamic_cast<TRasterFx *>(fx.getPointer());
  require(raster != nullptr, "Isolation fixture is not a raster generator");
  return TRasterFxP(raster);
}

TRaster32P composite(const ParticleBake::Frame &frame) {
  TRaster32P result(64, 64);
  result->clear();
  for (const auto &draw : frame.draws)
    TRop::over(result, draw.artwork,
               TTranslation(32, 32) * frame.renderAffine * draw.rasterToWorld);
  return result;
}
void compare(const TRaster32P &a, const TRaster32P &b) {
  std::size_t occupied = 0;
  for (int y = 0; y < a->getLy(); ++y)
    for (int x = 0; x < a->getLx(); ++x) {
      if (a->pixels(y)[x].m) ++occupied;
      require(a->pixels(y)[x] == b->pixels(y)[x],
              "Upstream preview history changed native capture pixels");
    }
  require(occupied > 0, "Isolation capture fixture produced no visible pixels");
}

void testIsolation(bool rootTiled, bool controlTiled) {
  TRasterFxP control = generator(controlTiled, 87, 16);
  TRasterFxP root = generator(rootTiled, 127, 2);
  root->getInputPort("Control1")->setFx(control.getPointer());
  integer(root.getPointer(), "birth_color_ctrl", 1);
  number(root.getPointer(), "birth_color_fade", 75);
  const unsigned long rootId = root->getIdentifier(), controlId = control->getIdentifier();
  TRenderSettings settings;
  settings.m_cameraBox = Domain;
  ParticleBake::Options options;
  options.domain = Domain;
  auto capture = [&]() {
    ParticleBake::Frame result;
    QString error;
    const bool ok = ParticleBake::captureFrame(root.getPointer(), 3, settings,
                                                options, result, error);
    require(ok,
            "Nested capture failed: " + error.toStdString());
    require(result.particles.size() == 8,
            "Capture omitted selected-root births or included upstream particles");
    require(result.draws.size() == 8,
            "Capture omitted selected-root artwork or included upstream draws");
    require(root->getIdentifier() == rootId && control->getIdentifier() == controlId,
            "Capture changed the live graph's cache identities");
    return result;
  };
  ParticleBake::Frame fresh;
  {
    RenderSession session;
    fresh = capture();
  }
  {
    RenderSession session;
    // Seed the upstream native manager and image cache with a distant viewport
    // in the caller's active renderer before evaluating the selected root.
    TRasterP warmed;
    if (rootTiled) warmed = TRaster64P(64, 64);
    else warmed = TRaster32P(64, 64);
    warmed->clear();
    TTile tile(warmed, TPointD(-96, -96));
    TRenderSettings warmSettings(settings);
    warmSettings.m_bpp = rootTiled ? 64 : 32;
    // Frame zero is the first upstream request during root preroll. Warming
    // only the final frame would let the upstream manager reset itself when
    // the capture subsequently asks for an earlier control frame.
    control->compute(tile, 0, warmSettings);
    const auto captured = capture();
    compare(composite(fresh), composite(captured));
    compare(composite(captured), composite(capture()));
  }
}

void testUpstreamLimits(bool controlTiled) {
  TRasterFxP control = generator(controlTiled, 87, 16);
  TRasterFxP root = generator(false, 127, 1);
  root->getInputPort("Control1")->setFx(control.getPointer());
  integer(root.getPointer(), "birth_color_ctrl", 1);
  number(root.getPointer(), "birth_color_fade", 75);
  ParticleBake::Options options;
  options.domain = Domain;
  options.maxDraws = 12;
  TRenderSettings settings;
  settings.m_cameraBox = Domain;
  ParticleBake::Frame result;
  QString error;
  RenderSession session;
  require(!ParticleBake::captureFrame(root.getPointer(), 3, settings, options, result, error),
          "Upstream native simulation bypassed the capture particle limit");
  require(error.contains("particle limit") && result.particles.empty() && result.draws.empty(),
          "Upstream limit failure did not discard the partial capture");
}
}

int main(int argc, char **argv) {
  QGuiApplication application(argc, argv);
  TThread::init();
  initStdFx();
  try {
    for (bool rootTiled : {false, true})
      for (bool controlTiled : {false, true}) testIsolation(rootTiled, controlTiled);
    testUpstreamLimits(false);
    testUpstreamLimits(true);
    std::cout << "Native particle capture isolation: 6 cases passed\n";
    return 0;
  } catch (const std::exception &error) {
    std::cerr << "particlecache_bakeisolation: " << error.what() << '\n';
    return 1;
  } catch (const TException &error) {
    std::cerr << "particlecache_bakeisolation: "
              << QString::fromStdWString(error.getMessage()).toStdString() << '\n';
    return 1;
  }
}
