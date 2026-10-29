#include "../particlesfx.h"
#include "../particleweather.h"
#include "stdfx.h"
#include "tfxcachemanager.h"
#include "tparamcontainer.h"
#include "trenderer.h"
#include "tthread.h"

#include <QGuiApplication>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
void require(bool value, const std::string &message) {
  if (!value) throw std::runtime_error(message);
}
bool approx(double a, double b, double tolerance = 1e-9) {
  return std::abs(a - b) <= tolerance;
}

void testDepth() {
  require(ParticleWeather::depthOf(12345, 1) == 0, "One layer is not no depth");
  std::set<double> seen;
  for (int seed = 0; seed < 400; ++seed) {
    const double d = ParticleWeather::depthOf(seed, 4);
    require(d >= 0 && d <= 1, "A depth is out of range");
    require(d == ParticleWeather::depthOf(seed, 4),
            "A depth is not repeatable");
    seen.insert(d);
  }
  require(seen.size() == 4, "Particles do not fill every depth layer");
  require(approx(ParticleWeather::depthFactor(0, 0.3), 1) &&
              approx(ParticleWeather::depthFactor(1, 0.3), 0.3),
          "The depth factor does not go from 1 to the far value");
}

void testTurbulence() {
  double dx, dy;
  ParticleWeather::turbulence(0, 40, 0.1, 5, 1, 10, 10, dx, dy);
  require(dx == 0 && dy == 0, "No turbulence pushed a particle");
  double largest = 0, a, b;
  bool drifts    = false;
  for (int i = 0; i < 2000; ++i) {
    ParticleWeather::turbulence(3, 20, 0.2, i % 13, 7, i * 1.7, i * 0.9, dx,
                                dy);
    largest = std::max(largest, std::max(std::abs(dx), std::abs(dy)));
    ParticleWeather::turbulence(3, 20, 0.2, i % 13 + 5, 7, i * 1.7, i * 0.9, a,
                                b);
    drifts = drifts || a != dx || b != dy;
  }
  require(largest <= 3 + 1e-12, "Turbulence exceeds its strength");
  require(largest > 1, "Turbulence is too weak");
  require(drifts, "Turbulence does not drift over time");
}

void testCollide() {
  using ParticleWeather::Collision;
  ParticleWeather::Motion m = {5, 6, 1, -2, false, 30};
  ParticleWeather::collide(Collision::Die, 4, 4, m);
  require(m.lifetime == 0, "A disappearing particle lives on");
  m = {5, 6, 1, -2, false, 30};
  ParticleWeather::collide(Collision::Settle, 4, 4, m);
  require(m.settled && m.x == 4 && m.y == 4 && m.vx == 0 && m.vy == 0 &&
              m.lifetime == 30,
          "A settling particle did not rest where it was");
  m = {5, 6, 1, -2, false, 30};
  ParticleWeather::collide(Collision::Bounce, 4, 4, m);
  require(!m.settled && m.x == 4 && m.vy > 0 && m.vx < 1 && m.lifetime == 30,
          "A bouncing particle did not bounce back");
}

// The FX, rendered for a few frames: falling particles over a matte
class Matte final : public TStandardRasterFx {
  double m_top;

public:
  explicit Matte(double top) : m_top(top) {}
  const TPersistDeclaration *getDeclaration() const override { return nullptr; }
  bool canHandle(const TRenderSettings &, double) override { return true; }
  std::string getAlias(double, const TRenderSettings &) const override {
    return "weather-matte";
  }
  bool doGetBBox(double, TRectD &box, const TRenderSettings &) override {
    box = TConsts::infiniteRectD;
    return true;
  }
  void doCompute(TTile &tile, double, const TRenderSettings &) override {
    TRaster32P raster = tile.getRaster();
    if (!raster) return;
    raster->clear();
    for (int y = 0; y < raster->getLy(); ++y)
      if (tile.m_pos.y + y < m_top)
        for (int x = 0; x < raster->getLx(); ++x)
          raster->pixels(y)[x] = TPixel32(255, 255, 255, 255);
  }
};

// A small round sprite: with a texture the FX does not draw its default one
// through OpenGL, which headless machines may not have
class Dot final : public TStandardRasterFx {
public:
  const TPersistDeclaration *getDeclaration() const override { return nullptr; }
  bool canHandle(const TRenderSettings &, double) override { return true; }
  std::string getAlias(double, const TRenderSettings &) const override {
    return "weather-dot";
  }
  // One frame, as a level column would give
  TFxTimeRegion getTimeRegion() const override { return TFxTimeRegion(0, 1); }
  bool doGetBBox(double, TRectD &box, const TRenderSettings &) override {
    box = TRectD(-3, -3, 3, 3);
    return true;
  }
  void doCompute(TTile &tile, double, const TRenderSettings &) override {
    TRaster32P raster = tile.getRaster();
    if (!raster) return;
    raster->clear();
    for (int y = 0; y < raster->getLy(); ++y)
      for (int x = 0; x < raster->getLx(); ++x) {
        const double px = tile.m_pos.x + x + 0.5, py = tile.m_pos.y + y + 0.5;
        if (px * px + py * py <= 9)
          raster->pixels(y)[x] = TPixel32(255, 255, 255, 255);
      }
  }
};

TDoubleParamP numeric(TFx &fx, const std::string &name) {
  return TParamP(fx.getParams()->getParam(name));
}
TIntParamP integer(TFx &fx, const std::string &name) {
  return TParamP(fx.getParams()->getParam(name));
}
TIntEnumParamP choice(TFx &fx, const std::string &name) {
  return TParamP(fx.getParams()->getParam(name));
}

struct Frame {
  std::vector<double> alpha;   // per pixel
  double ink             = 0;  // total alpha
  double below           = 0;  // alpha below y = 0
  std::uint64_t checksum = 0;
};

const int W = 160, H = 160;

// A fresh render of frame `frame` (the particles are stepped from the start)
Frame render(TFx &fx, int frame) {
  TRenderer renderer(1);
  const unsigned long id = TRenderer::buildRenderId();
  renderer.install(id);
  renderer.declareRenderStart(id);
  renderer.declareFrameStart(frame);
  Frame out;
  try {
    TRenderSettings settings;
    settings.m_bpp       = 32;
    settings.m_cameraBox = TRectD(-W / 2.0, -H / 2.0, W / 2.0, H / 2.0);
    TRaster32P raster(W, H);
    raster->clear();
    TTile tile(raster, TPointD(-W / 2.0, -H / 2.0));
    static_cast<ParticlesFx &>(fx).doCompute(tile, frame, settings);
    out.alpha.resize(size_t(W) * H);
    for (int y = 0; y < H; ++y)
      for (int x = 0; x < W; ++x) {
        const TPixel32 &p            = raster->pixels(y)[x];
        const double a               = p.m / 255.0;
        out.alpha[size_t(y) * W + x] = a;
        out.ink += a;
        if (y < H / 2) out.below += a;
        out.checksum = out.checksum * 1099511628211ull ^
                       (std::uint64_t(p.r) << 24 | std::uint64_t(p.g) << 16 |
                        std::uint64_t(p.b) << 8 | p.m);
      }
  } catch (...) {
    renderer.declareFrameEnd(frame);
    renderer.declareRenderEnd(id);
    renderer.uninstall();
    throw;
  }
  renderer.declareFrameEnd(frame);
  renderer.declareRenderEnd(id);
  renderer.uninstall();
  return out;
}

// Particles born across the top, falling under gravity
TFxP rain() {
  static TFxP dot = new Dot;
  TFxP fx         = new ParticlesFx;
  require(fx->connect("Texture1", dot.getPointer()), "Cannot connect a sprite");
  numeric(*fx, "birth_rate")->setDefaultValue(20);
  numeric(*fx, "gravity")->setDefaultValue(2);
  numeric(*fx, "length")->setDefaultValue(140);
  numeric(*fx, "height")->setDefaultValue(4);
  TPointParamP center = TParamP(fx->getParams()->getParam("center"));
  center->setDefaultValue(TPointD(0, 70));
  return fx;
}

void testFx() {
  // Defaults are the weather explicitly off: same pixels
  TFxP plain = rain(), off = rain();
  integer(*off, "collision_ctrl")->setValue(0);
  numeric(*off, "turbulence")->setDefaultValue(0);
  integer(*off, "depth_layers")->setValue(1);
  const Frame a = render(*plain, 30), b = render(*off, 30);
  require(a.ink > 5, "The particles did not render");
  require(a.checksum == b.checksum, "Weather off is not the default render");
  std::cout << "default render checksum " << a.checksum << '\n';

  // Particles fall below the middle without a collision image
  require(a.below > 1, "No particle fell below the middle");

  // A matte filling the lower half: particles that hit it disappear
  TFxP matte = new Matte(0);
  for (int mode = 0; mode < 3; ++mode) {
    TFxP fx = rain();
    require(fx->connect("Control1", matte.getPointer()),
            "Cannot connect the collision image");
    integer(*fx, "collision_ctrl")->setValue(1);
    choice(*fx, "collision_mode")->setValue(mode);
    const Frame f = render(*fx, 30);
    require(f.ink > 1, "Collision removed every particle");
    // A particle's sprite can reach a little past its centre
    double deep = 0;
    for (int y = 0; y < H / 2 - 8; ++y)
      for (int x = 0; x < W; ++x) deep += f.alpha[size_t(y) * W + x];
    require(deep < 1e-9, "A particle went through the collision image (mode " +
                             std::to_string(mode) + ")");
  }
  // Settling keeps particles resting on the surface: more ink near it than
  // disappearing does
  auto nearSurface = [&](const Frame &f) {
    double sum = 0;
    for (int y = H / 2 - 8; y < H / 2 + 8; ++y)
      for (int x = 0; x < W; ++x) sum += f.alpha[size_t(y) * W + x];
    return sum;
  };
  TFxP vanish = rain(), settle = rain();
  for (TFxP fx : {vanish, settle}) {
    require(fx->connect("Control1", matte.getPointer()), "Cannot connect");
    integer(*fx, "collision_ctrl")->setValue(1);
  }
  choice(*settle, "collision_mode")->setValue(1);
  require(
      nearSurface(render(*settle, 60)) > nearSurface(render(*vanish, 60)) * 2,
      "Settled particles do not gather on the surface");

  // Turbulence moves the particles
  TFxP windy = rain();
  numeric(*windy, "turbulence")->setDefaultValue(4);
  require(render(*windy, 30).checksum != a.checksum,
          "Turbulence did not move the particles");

  // Depth layers: far particles are smaller and fainter, so less ink
  TFxP deep = rain();
  integer(*deep, "depth_layers")->setValue(4);
  numeric(*deep, "depth_far_scale")->setDefaultValue(0.3);
  numeric(*deep, "depth_far_opacity")->setDefaultValue(0.4);
  require(render(*deep, 30).ink < a.ink * 0.9,
          "Depth layers did not shrink and fade the far particles");
}
}  // namespace

int main(int argc, char **argv) {
  QGuiApplication app(argc, argv);
  TThread::init();
  TRenderer::initialize();
  int result = 1;
  try {
    testDepth();
    testTurbulence();
    testCollide();
    testFx();
    std::cout << "PASS: particle depth layers, turbulence, collision and "
                 "default renders\n";
    result = 0;
  } catch (const TException &error) {
    std::cerr << QString::fromStdWString(error.getMessage()).toStdString()
              << '\n';
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
  }
  TThread::shutdown();
  return result;
}
