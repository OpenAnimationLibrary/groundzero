#include "../celsmoother2fx.cpp"
#include "tmacrofx.h"
#include "tparamcontainer.h"
#include "tstream.h"
#include "trenderer.h"
#include "tthread.h"
#include "trop.h"

#include <QCoreApplication>
#include <QTemporaryDir>
#include <future>
#include <iostream>
#include <memory>
#include <stdexcept>

namespace {
void require(bool value, const char *message) {
  if (!value) throw std::runtime_error(message);
}
void requireNear(double actual, double expected, double tolerance, const char *message) {
  require(std::isfinite(actual) && std::abs(actual - expected) <= tolerance, message);
}
TDoubleParamP number(TFx &fx, const std::string &name) {
  return TParamP(fx.getParams()->getParam(name));
}
TPixelParamP color(TFx &fx, const std::string &name) {
  return TParamP(fx.getParams()->getParam(name));
}
void set(TFx &fx, const std::string &name, double value) {
  number(fx, name)->setDefaultValue(value);
}
void setColor(TFx &fx, const std::string &name, const TPixelD &value) {
  auto p = color(fx, name);
  p->getRed()->setDefaultValue(value.r);
  p->getGreen()->setDefaultValue(value.g);
  p->getBlue()->setDefaultValue(value.b);
}

class Source final : public TStandardRasterFx {
  TRasterP m_image;
  TPoint m_origin;
  int *m_cancel;
public:
  Source(const TRasterP &image, TPoint origin = TPoint(), int *cancel = nullptr)
      : m_image(image), m_origin(origin), m_cancel(cancel) {}
  const TPersistDeclaration *getDeclaration() const override { return nullptr; }
  bool canHandle(const TRenderSettings &, double) override { return true; }
  bool doGetBBox(double, TRectD &box, const TRenderSettings &) override {
    box = TRectD(m_origin.x, m_origin.y, m_origin.x + m_image->getLx(),
                 m_origin.y + m_image->getLy());
    return true;
  }
  std::string getAlias(double, const TRenderSettings &) const override {
    return "smoother2-test-source";
  }
  void compute(TTile &tile, double, const TRenderSettings &) override {
    tile.getRaster()->clear();
    tile.getRaster()->copy(m_image, TPoint(m_origin.x - int(tile.m_pos.x),
                                          m_origin.y - int(tile.m_pos.y)));
    if (m_cancel) *m_cancel = 1;
  }
  void doCompute(TTile &tile, double frame, const TRenderSettings &settings) override {
    compute(tile, frame, settings);
  }
};

class PartialSource final : public TStandardRasterFx {
public:
  const TPersistDeclaration *getDeclaration() const override { return nullptr; }
  bool canHandle(const TRenderSettings &, double) override { return true; }
  bool doGetBBox(double, TRectD &box, const TRenderSettings &) override {
    box = TRectD(0, 0, 3, 3);
    return true;
  }
  void compute(TTile &tile, double, const TRenderSettings &) override {
    TRasterFP raster = tile.getRaster();
    require(bool(raster), "Unexpected source precision");
    for (int y = 0; y < raster->getLy(); ++y)
      for (int x = 0; x < raster->getLx(); ++x)
        require(raster->pixels(y)[x] == TPixelF::Transparent,
                "Source scratch was not initialized before upstream rendering");
    raster->pixels(1 - int(tile.m_pos.y))[1 - int(tile.m_pos.x)] = TPixelF(1, 0, 0, 1);
  }
  void doCompute(TTile &tile, double frame, const TRenderSettings &settings) override {
    compute(tile, frame, settings);
  }
};

// A raster level supplies native pixels. Let the real host resample every
// other requested grid so a premature DPI reduction is visible to the test.
class NativeSource final : public TStandardRasterFx {
  TRasterFP m_image;
  TPointD m_origin;
public:
  explicit NativeSource(const TRasterFP &image, TPointD origin = TPointD(-12, -10))
      : m_image(image), m_origin(origin) {
    enableComputeInFloat(true);
  }
  const TPersistDeclaration *getDeclaration() const override { return nullptr; }
  bool canHandle(const TRenderSettings &, double) override { return false; }
  TAffine handledAffine(const TRenderSettings &, double) override {
    const double x = -m_origin.x, y = -m_origin.y;
    return TTranslation(x - std::floor(x), y - std::floor(y));
  }
  bool doGetBBox(double, TRectD &box, const TRenderSettings &) override {
    box = TRectD(m_origin.x, m_origin.y, m_origin.x + m_image->getLx(),
                 m_origin.y + m_image->getLy());
    return true;
  }
  std::string getAlias(double, const TRenderSettings &) const override {
    return "smoother2-native-source[" + std::to_string(m_image->getLx()) + "," +
           std::to_string(m_image->getLy()) + "," + std::to_string(m_origin.x) +
           "," + std::to_string(m_origin.y) + "]";
  }
  void doCompute(TTile &tile, double, const TRenderSettings &settings) override {
    tile.getRaster()->clear();
    TTile source(m_image, m_origin);
    TRasterFx::applyAffine(tile, source, settings);
  }
};

template <class Pixel>
TRasterPT<Pixel> render(CelSmoother2Fx &fx, int width, int height,
                       TPoint position = TPoint(), double frame = 0,
                       TRenderSettings settings = TRenderSettings()) {
  TRasterPT<Pixel> result(width, height);
  TTile tile;
  tile.m_pos = TPointD(position.x, position.y);
  tile.setRaster(result);
  fx.doCompute(tile, frame, settings);
  return result;
}

void testTransferAndGamma() {
  using namespace CelSmoother2;
  requireNear(linearize(0.04045), 0.0031308049535603713, 1e-14, "sRGB toe is incorrect");
  requireNear(encode(0.0031308), 0.040449936, 1e-14, "sRGB inverse toe is incorrect");
  for (double c : {-2.5, -0.2, 0.0, 0.001, 0.3, 1.0, 4.5})
    requireNear(encode(linearize(c)), c, 1e-12, "Transfer lost signed HDR values");
  Settings s;
  Pixel black{0, 0, 0, 1}, white{1, 1, 1, 1};
  s.version = 0;
  requireNear(mix(black, white, .5, s).r, .5, 1e-14, "v1 must blend encoded RGB");
  s.version = 1;
  requireNear(mix(black, white, .5, s).r, 0.7353569830524495, 1e-13,
       "v2 must blend in sRGB linear light");
  for (int version : {0, 1}) {
    s.version = version;
    for (int mode : {0, 1, 2}) {
      s.gammaMode = mode;
      s.gamma = 1;
      requireNear(mix(black, white, .5, s).r, version ? .7353569830524495 : .5,
           1e-13, "Gamma one changed the selected version");
      s.gamma = 2.4;
      const double result = mix(black, white, .5, s).r;
      if (mode) require(result < (version ? .735 : .5), "Gamma did not favor dark lines");
      else requireNear(result, version ? .7353569830524495 : .5, 1e-13, "None applied gamma");
    }
  }
  s.version = 0;
  s.gammaMode = 1;
  Pixel red{1, 0, 0, 1}, blue{0, 0, 1, 1};
  requireNear(mix(red, blue, .5, s).r, .5, 1e-13, "Black-only gamma affected red/blue");
  s.gammaMode = 2;
  requireNear(mix(red, blue, .5, s).r, std::pow(.5, 2.4), 1e-13, "Global gamma omitted red/blue");
  s.gammaMode = 1;
  s.gammaCount = 5;
  s.colors[4] = red;
  requireNear(mix(red, blue, .5, s).r, std::pow(.5, 2.4), 1e-13, "Fifth gamma slot is ignored");
  s.gammaCount = 4;
  requireNear(mix(red, blue, .5, s).r, .5, 1e-13, "Inactive gamma slot still matches");
  s.gammaCount = 0;
  requireNear(mix(black, white, .5, s).r, .5, 1e-13, "Zero gamma count applied correction");
  const double threshold = .5 / 255;
  require(matches(Pixel{std::nextafter(threshold, 0.0), 0, 0, 1}, black),
          "Gamma tolerance rejected its interior");
  require(!matches(Pixel{threshold, 0, 0, 1}, black), "Gamma tolerance included its tie");
  require(matches(Pixel{.25, 0, 0, .25}, red), "Partial alpha broadened selection");
  s.gammaMode = 0;
  const Pixel covered = mix(Pixel{.5, 0, 0, .5}, Pixel{0, 0, 0, 0}, .5, s);
  requireNear(covered.r, .25, 1e-14, "Transparent edge changed straight color");
  requireNear(covered.a, .25, 1e-14, "Gamma changed coverage");
}

void testAllocationBoundaries() {
  const int maximum = std::numeric_limits<int>::max();
  for (int bytes : {4, 8, 16}) {
    require(canAllocateSource(maximum / bytes, 1, bytes), "Valid byte limit was rejected");
    require(!canAllocateSource(maximum / bytes + 1, 1, bytes), "Byte limit overflow was accepted");
    require(!canAllocateSource(maximum / bytes, 2, bytes), "Area overflow was accepted");
  }
  require(!canAllocateSource(-1, 4, 16) && !canAllocateSource(4, -1, 16) &&
              !canAllocateSource(4, 4, 0), "Invalid allocation dimensions were accepted");

}

void testPartiallyWrittenSource() {
  CelSmoother2Fx fx;
  fx.getInputPort(0)->setFx(new PartialSource);
  set(fx, "smoothness", 0);
  auto result = render<TPixelF>(fx, 7, 7, TPoint(-2, -2));
  for (int y = 0; y < 7; ++y)
    for (int x = 0; x < 7; ++x)
      require(result->pixels(y)[x] ==
                  (x == 3 && y == 3 ? TPixelF(1, 0, 0, 1) : TPixelF::Transparent),
              "Unwritten upstream pixels leaked into smoothing");
}

template <class Pixel> TRasterPT<Pixel> fixture() {
  TRasterPT<Pixel> image(23, 19);
  const double maximum = pixelScale<Pixel>();
  for (int y = 0; y < 19; ++y)
    for (int x = 0; x < 23; ++x) {
      const bool line = x == y || x == y + 1;
      image->pixels(y)[x] = line ? Pixel(0, 0, 0, maximum) :
          (x > y ? Pixel(maximum, 0, 0, maximum) : Pixel(0, 0, maximum, maximum));
    }
  return image;
}

template <class Pixel> void testRendering() {
  CelSmoother2Fx fx;
  auto input = fixture<Pixel>();
  fx.getInputPort(0)->setFx(new Source(input, TPoint(-9, -7)));
  set(fx, "smoothness", 0);
  auto bypass = render<Pixel>(fx, 23, 19, TPoint(-9, -7));
  for (int y = 0; y < 19; ++y)
    for (int x = 0; x < 23; ++x)
      require(bypass->pixels(y)[x] == input->pixels(y)[x], "Zero strengths altered pixels");
  for (double strength : {0., 50., 100.}) {
    for (double extra : {0., 50., 100.}) {
      set(fx, "smoothness", strength);
      set(fx, "extraSmooth", extra);
      auto output = render<Pixel>(fx, 23, 19, TPoint(-9, -7));
      bool changed = false;
      for (int y = 0; y < 19; ++y)
        for (int x = 0; x < 23; ++x) {
          changed |= output->pixels(y)[x] != input->pixels(y)[x];
          require(std::isfinite(double(output->pixels(y)[x].r)), "Nonfinite smoothing output");
        }
      require(changed == (strength > 0 || extra > 0), "Smoothing or extra pass is ineffective");
    }
  }
  set(fx, "smoothRange", 255);
  auto rangeBypass = render<Pixel>(fx, 23, 19, TPoint(-9, -7));
  for (int y = 0; y < 19; ++y)
    for (int x = 0; x < 23; ++x)
      require(rangeBypass->pixels(y)[x] == input->pixels(y)[x], "Maximum range did not bypass");
  set(fx, "smoothRange", 2);
  number(fx, "gammaCorrection")->setValue(0, 0);
  number(fx, "gammaCorrection")->setValue(10, 2);
  number(fx, "smootherVersion")->setValue(0, 0);
  number(fx, "smootherVersion")->setValue(10, 1);
  std::vector<std::future<void>> jobs;
  for (int frame : {0, 10}) {
    auto full = render<Pixel>(fx, 25, 21, TPoint(-10, -8), frame);
    for (int y = 0; y < 21; y += 3)
      jobs.push_back(std::async(std::launch::async, [&, full, frame, y] {
        auto strip = render<Pixel>(fx, 25, 3, TPoint(-10, y - 8), frame);
        for (int row = 0; row < 3; ++row)
          for (int x = 0; x < 25; ++x)
            require(strip->pixels(row)[x] == full->pixels(y + row)[x],
                    "Concurrent frame or tile changed contour output");
      }));
  }
  for (auto &job : jobs) job.get();
  TRenderSettings settings;
  TRectD needed;
  TRenderSettings inputSettings;
  fx.transform(0, 0, TRectD(0, 0, 1, 1), settings, needed, inputSettings);
  require(needed == TRectD(-11, -9, 16, 14), "Source dependency is tile-relative");
  require(fx.getMemoryRequirement(needed, 0, settings) > 0, "Scratch memory is unreported");
  require(!fx.toBeComputedInLinearColorSpace(true, true), "Host will double-linearize the image");
  settings.m_affine = TScale(2);
  require(!fx.canHandle(settings, 0) && fx.handledAffine(settings, 0).isIdentity(),
          "Smoothing changed source resolution under affine transforms");
}

void testKeyHdrAndCancellation() {
  CelSmoother2Fx fx;
  TRasterFP input(5, 1);
  input->pixels(0)[0] = TPixelF(1, 1, 1, 1);
  input->pixels(0)[1] = TPixelF(.5, .5, .5, .5);
  input->pixels(0)[2] = TPixelF(2, -.5, .125, 1);
  input->pixels(0)[3] = TPixelF(1, 1, 1, 0);
  input->pixels(0)[4] = TPixelF(std::numeric_limits<float>::infinity(), 0, 0, 1);
  fx.getInputPort(0)->setFx(new Source(input));
  set(fx, "smoothness", 0);
  set(fx, "useColorKey", 1);
  auto output = render<TPixelF>(fx, 5, 1);
  for (int i : {0, 1, 3, 4})
    require(output->pixels(0)[i] == TPixelF::Transparent, "Key or invalid pixels retained RGB");
  require(output->pixels(0)[2] == input->pixels(0)[2], "Float HDR was clamped");
  set(fx, "invertColorKey", 1);
  output = render<TPixelF>(fx, 5, 1);
  for (int i : {0, 1})
    require(output->pixels(0)[i] == input->pixels(0)[i], "Inverted key removed selected color");
  require(output->pixels(0)[2] == TPixelF::Transparent, "Inverted key kept another color");
  int canceled = 0;
  TRenderSettings settings;
  settings.m_isCanceled = &canceled;
  fx.getInputPort(0)->setFx(new Source(input, TPoint(), &canceled));
  for (int initial : {0, 1}) {
    canceled = initial;
    output = render<TPixelF>(fx, 5, 1, TPoint(), 0, settings);
    for (int i = 0; i < 5; ++i)
      require(output->pixels(0)[i] == TPixelF::Transparent, "Cancellation exposed a partial image");
  }
}

TRasterFP hostRender(TRasterFx &fx, int strips, const TAffine &affine,
                    bool linear, double frame = 0) {
  constexpr int width = 48, height = 40;
  TRenderer renderer(1);
  const unsigned long id = TRenderer::buildRenderId();
  renderer.install(id);
  renderer.declareRenderStart(id);
  renderer.declareFrameStart(frame);
  TRasterFP output(width, height);
  output->clear();
  output->setLinear(linear);
  try {
    TRenderSettings settings;
    settings.m_bpp = 128;
    settings.m_affine = affine;
    settings.m_linearColorSpace = linear;
    settings.m_colorSpaceGamma = 2.2;
    settings.m_cameraBox = TRectD(-24, -20, 24, 20);
    for (int strip = 0; strip < strips; ++strip) {
      const int y0 = height * strip / strips;
      const int y1 = height * (strip + 1) / strips;
      TRasterFP part(width, y1 - y0);
      part->clear();
      part->setLinear(linear);
      TTile tile(part, TPointD(-24, -20 + y0));
      fx.compute(tile, frame, settings);
      output->copy(tile.getRaster(), TPoint(0, y0));
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
  return output;
}

void sameImage(const TRasterFP &a, const TRasterFP &b, const char *message) {
  bool visible = false;
  for (int y = 0; y < a->getLy(); ++y)
    for (int x = 0; x < a->getLx(); ++x) {
      const TPixelF &first = a->pixels(y)[x], &second = b->pixels(y)[x];
      visible |= first.m > .5f;
      require(std::max({std::abs(first.r - second.r), std::abs(first.g - second.g),
                        std::abs(first.b - second.b), std::abs(first.m - second.m)}) < 1e-6f,
              message);
    }
  require(visible, "Host rendering produced an empty image");
}

void testHostColorSpaceAndTransforms() {
  TFxP owner = new CelSmoother2Fx;
  auto &fx = *static_cast<CelSmoother2Fx *>(owner.getPointer());
  fx.getInputPort(0)->setFx(new Source(fixture<TPixelF>(), TPoint(-9, -7)));
  set(fx, "gammaCorrection", 1);
  set(fx, "extraSmooth", 60);
  for (int version : {0, 1}) {
    set(fx, "smootherVersion", version);
    auto encoded = hostRender(fx, 1, TAffine(), false);
    auto linear = hostRender(fx, 1, TAffine(), true);
    TRop::toLinearRGB(encoded, 2.2);
    sameImage(encoded, linear, "Working-space gamma was applied incorrectly");
    const TAffine affine = TTranslation(2.5, -3.25) * TRotation(21) * TScale(1.3, .7);
    for (bool workingLinear : {false, true})
      sameImage(hostRender(fx, 1, affine, workingLinear),
           hostRender(fx, 8, affine, workingLinear),
           "Affine or linear rendering created tile seams");
  }
}

void testNativeLevelGrid() {
  TRasterFP input(24, 20);
  input->clear();
  for (int y = 3; y < 17; ++y)
    for (int x = 3; x < 21; ++x)
      input->pixels(y)[x] = x > 5 + y / 2 ? TPixelF(.8f, .2f, .6f, 1)
                                         : TPixelF(.05f, .05f, .05f, 1);
  TFxP referenceOwner = new CelSmoother2Fx;
  auto &reference = *static_cast<CelSmoother2Fx *>(referenceOwner.getPointer());
  reference.getInputPort(0)->setFx(new NativeSource(input));
  set(reference, "extraSmooth", 70);
  set(reference, "gammaCorrection", 2);
  const auto smoothed = hostRender(reference, 1, TAffine(), false);
  set(reference, "smoothRange", 255);
  const auto bypass = hostRender(reference, 1, TAffine(), false);
  set(reference, "smoothRange", 2);
  bool filtered = false;
  for (int y = 0; y < smoothed->getLy(); ++y)
    for (int x = 0; x < smoothed->getLx(); ++x)
      filtered |= std::abs(smoothed->pixels(y)[x].r - bypass->pixels(y)[x].r) > .001f;
  require(filtered, "Native-grid fixture did not contain a smoothed contour");

  TFxP actualOwner = new CelSmoother2Fx;
  auto &actual = *static_cast<CelSmoother2Fx *>(actualOwner.getPointer());
  set(actual, "extraSmooth", 70);
  set(actual, "gammaCorrection", 2);
  TFxP dpiOwner = new NaAffineFx;
  auto *dpi = static_cast<NaAffineFx *>(dpiOwner.getPointer());
  dpi->setName(L"LevelColumn AffineFx");
  dpi->getInputPort(0)->setFx(new NativeSource(input));
  TFxP placementOwner = new NaAffineFx;
  auto *placement = static_cast<NaAffineFx *>(placementOwner.getPointer());
  placement->getInputPort(0)->setFx(dpi);
  TFxP upstreamOwner = new CelSmoother2Fx;
  auto *upstream = static_cast<CelSmoother2Fx *>(upstreamOwner.getPointer());
  set(*upstream, "smoothness", 0);
  upstream->getInputPort(0)->setFx(placement);
  actual.getInputPort(0)->setFx(upstream);
  for (const TAffine &scale : std::vector<TAffine>{TScale(.25), TScale(.25, .5)}) {
    dpi->setAffine(scale);
    for (const TAffine &position : std::vector<TAffine>{
             TTranslation(.25, -.75), TTranslation(2.25, -3.75) * TRotation(17)}) {
      placement->setAffine(position);
      const TAffine world = position * scale;
      const TAffine grid = actual.handledAffine(TRenderSettings(), 0);
      require(grid != TAffine() && grid.a13 >= 0 && grid.a13 < 1 &&
                  grid.a23 >= 0 && grid.a23 < 1, "Invalid native source grid");
      TRenderSettings predicted;
      TRectD request;
      actual.transform(0, 0, TRectD(0, 0, 1, 1), TRenderSettings(), request, predicted);
      require(predicted.m_affine == grid && request.getLx() >= 28 && request.getLy() >= 24,
              "Predictive source request lost native pixels or smoothing borders");
      for (int version : {0, 1}) {
        set(actual, "smootherVersion", version);
        set(reference, "smootherVersion", version);
        for (bool linear : {false, true})
          for (const TAffine &camera : std::vector<TAffine>{TAffine(), TScale(4)}) {
            const auto expected = hostRender(reference, 1, camera * world, linear);
            const auto full = hostRender(actual, 1, camera, linear);
            sameImage(full, expected, "Level DPI changed native contour and gamma blending");
            sameImage(full, hostRender(actual, 8, camera, linear),
                      "Native DPI rendering introduced strip seams");
          }
      }
    }
  }
  const std::string alias = actual.getAlias(0, TRenderSettings());
  dpi->setAffine(TScale(.250000000001, .5));
  require(actual.getAlias(0, TRenderSettings()) != alias,
          "Fine native grid changes reused the cached smoothing result");
  number(actual, "smoothness")->setValue(0, 20);
  number(actual, "smoothness")->setValue(10, 90);
  const auto first = hostRender(actual, 1, TScale(4), false, 0);
  const auto last = hostRender(actual, 1, TScale(4), false, 10);
  auto concurrentFirst = std::async(std::launch::async, [&] {
    return hostRender(actual, 8, TScale(4), false, 0);
  });
  auto concurrentLast = std::async(std::launch::async, [&] {
    return hostRender(actual, 8, TScale(4), false, 10);
  });
  sameImage(first, concurrentFirst.get(), "Concurrent native grid render changed frame zero");
  sameImage(last, concurrentLast.get(), "Concurrent native grid render changed frame ten");
  dpi->setName(L"Unmarked geometry");
  require(actual.handledAffine(TRenderSettings(), 0) == TAffine(),
          "Generic source geometry unexpectedly selected a raster-level grid");
}

void testOddLevelOrigin() {
  TRasterFP input(23, 17);
  input->clear();
  for (int y = 2; y < 15; ++y)
    for (int x = 2; x < 21; ++x)
      input->pixels(y)[x] = x > 4 + y / 2 ? TPixelF(.8f, .4f, .2f, 1)
                                         : TPixelF(0, 0, 0, 1);
  const TPointD origin(-11.5, -8.5);
  TFxP referenceOwner = new CelSmoother2Fx;
  auto &reference = *static_cast<CelSmoother2Fx *>(referenceOwner.getPointer());
  reference.getInputPort(0)->setFx(new NativeSource(input, TPointD()));
  TFxP actualOwner = new CelSmoother2Fx;
  auto &actual = *static_cast<CelSmoother2Fx *>(actualOwner.getPointer());
  TFxP dpiOwner = new NaAffineFx;
  auto *dpi = static_cast<NaAffineFx *>(dpiOwner.getPointer());
  dpi->setName(L"LevelColumn AffineFx");
  dpi->getInputPort(0)->setFx(new NativeSource(input, origin));
  actual.getInputPort(0)->setFx(dpi);
  for (auto *effect : {&actual, &reference}) {
    set(*effect, "extraSmooth", 55);
    set(*effect, "gammaCorrection", 1);
  }
  for (const TAffine &world : std::vector<TAffine>{
           TAffine(), TScale(.25),
           TTranslation(2.25, -1.75) * TRotation(13) * TScale(.5, .25)}) {
    dpi->setAffine(world);
    const TAffine grid = actual.handledAffine(TRenderSettings(), 0);
    const TPointD aligned = grid * world * origin;
    requireNear(aligned.x, std::round(aligned.x), 1e-12,
                "Odd level horizontal pixel phase was ignored");
    requireNear(aligned.y, std::round(aligned.y), 1e-12,
                "Odd level vertical pixel phase was ignored");
    for (bool linear : {false, true})
      for (const TAffine &camera : std::vector<TAffine>{TAffine(), TScale(4)}) {
        const auto expected = hostRender(reference, 1,
                                        camera * world * TTranslation(origin), linear);
        const auto full = hostRender(actual, 1, camera, linear);
        sameImage(full, expected, "Odd level pixels were resampled before smoothing");
        sameImage(full, hostRender(actual, 8, camera, linear),
                  "Odd level origin introduced strip seams");
      }
  }
  // Identity DPI omits the affine wrapper and marks the native source itself.
  auto *raw = new NativeSource(input, origin);
  raw->setName(L"LevelColumn AffineFx");
  actual.getInputPort(0)->setFx(raw);
  sameImage(hostRender(actual, 1, TAffine(), false),
            hostRender(reference, 1, TTranslation(origin), false),
            "Unwrapped identity-DPI source lost its native pixel phase");
}

void testPersistence(const QString &directory) {
  TFxP owner = new CelSmoother2Fx;
  auto &fx = *owner;
  fx.setFxId(L"CelSmoother2A");
  number(fx, "useColorKey")->setValue(3, 1);
  number(fx, "smootherVersion")->setValue(8, 0);
  number(fx, "gammaCorrection")->setValue(8, 1);
  number(fx, "numberOfGammaColors")->setValue(8, 5);
  color(fx, "gammaColor5")->setValueD(8, TPixelD(.123456789123, 2.5, -.25, 1));
  TFxP clone = fx.clone(false);
  require(color(*clone, "gammaColor5")->getValueD(8) == color(fx, "gammaColor5")->getValueD(8),
          "Clone lost final gamma color precision");
  for (bool compressed : {false, true}) {
    const TFilePath path((directory + (compressed ? "/compressed.fx" : "/scene.fx")).toStdWString());
    { TOStream stream(path, compressed); stream << owner.getPointer(); }
    TFxP restored;
    { TIStream stream(path); stream >> restored; }
    require(restored && restored->getFxType() == "STD_celSmoother2Fx", "Factory lost v2 effect identity");
    require(color(*restored, "gammaColor5")->getValueD(8) == color(fx, "gammaColor5")->getValueD(8),
            "Scene lost hidden gamma precision");
    require(number(*restored, "useColorKey")->isKeyframe(3) &&
                number(*restored, "smootherVersion")->getValue(8) == 0 &&
                number(*restored, "gammaCorrection")->getUIChoices().size() == 3,
            "Scene lost discrete keys or native choices");
  }
  const TFilePath preset((directory + "/preset.fx").toStdWString());
  { TOStream stream(preset); fx.savePreset(stream); }
  CelSmoother2Fx restoredPreset;
  { TIStream stream(preset); restoredPreset.loadPreset(stream); }
  require(color(restoredPreset, "gammaColor5")->getValueD(8) == color(fx, "gammaColor5")->getValueD(8),
          "Preset lost gamma color");
  TFxP second = new CelSmoother2Fx;
  second->setFxId(L"CelSmoother2B");
  second->getInputPort(0)->setFx(owner.getPointer());
  TFxP macroOwner = TMacroFx::create({owner, second});
  auto *macro = dynamic_cast<TMacroFx *>(macroOwner.getPointer());
  require(macro, "Cannot create native macro");
  macro->setParamExposed(owner.getPointer(), "gammaColor5", true);
  macro->setParamExposed(owner.getPointer(), "smootherVersion", true);
  const TFilePath path((directory + "/macro.fx").toStdWString());
  { TOStream stream(path); stream << macroOwner.getPointer(); }
  TFxP restored;
  { TIStream stream(path); stream >> restored; }
  auto *loaded = dynamic_cast<TMacroFx *>(restored.getPointer());
  require(loaded, "Macro reload failed");
  TFx *member = loaded->getFxById(L"CelSmoother2A");
  require(member && loaded->isParamExposed(member, "gammaColor5") &&
              loaded->isParamExposed(member, "smootherVersion"), "Macro exposure was lost");
  CelSmoother2Fx aliasFx;
  set(aliasFx, "gammaCorrection", 1);
  setColor(aliasFx, "gammaColor1", TPixelD(.12345671, 0, 0, 1));
  const std::string alias = aliasFx.getAlias(0, TRenderSettings());
  setColor(aliasFx, "gammaColor1", TPixelD(.12345672, 0, 0, 1));
  require(aliasFx.getAlias(0, TRenderSettings()) != alias, "Fine colors collided in cache");
}
}  // namespace

int main(int argc, char **argv) {
  QCoreApplication app(argc, argv);
  TThread::init();
  TRenderer::initialize();
  QTemporaryDir directory;
  int result = 1;
  try {
    require(directory.isValid(), "Cannot create fixture directory");
    std::cout << "Checking transfer functions and gamma selection" << std::endl;
    testTransferAndGamma();
    std::cout << "Checking allocation and partial-source boundaries" << std::endl;
    testAllocationBoundaries();
    testPartiallyWrittenSource();
    std::cout << "Checking precision, extra passes and concurrent tiles" << std::endl;
    testRendering<TPixel32>();
    testRendering<TPixel64>();
    testRendering<TPixelF>();
    testKeyHdrAndCancellation();
    std::cout << "Checking host color conversion and affine rendering" << std::endl;
    testHostColorSpaceAndTransforms();
    std::cout << "Checking native level DPI, gamma and concurrent frame grids" << std::endl;
    testNativeLevelGrid();
    testOddLevelOrigin();
    std::cout << "Checking scenes, presets and macros" << std::endl;
    testPersistence(directory.path());
    std::cout << "PASS: complete smoother v2 rendering, gamma, concurrency and persistence\n";
    result = 0;
  } catch (const TException &error) {
    std::cerr << QString::fromStdWString(error.getMessage()).toStdString() << '\n';
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
  }
  TThread::shutdown();
  return result;
}
