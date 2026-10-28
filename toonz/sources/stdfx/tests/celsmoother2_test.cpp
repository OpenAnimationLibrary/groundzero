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
void near(double actual, double expected, double tolerance, const char *message) {
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

class OversizedSource final : public TStandardRasterFx {
public:
  bool computed = false;
  const TPersistDeclaration *getDeclaration() const override { return nullptr; }
  bool canHandle(const TRenderSettings &, double) override { return true; }
  bool doGetBBox(double, TRectD &box, const TRenderSettings &) override {
    box = TRectD(0, 0, std::numeric_limits<int>::max() / 16, 2);
    return true;
  }
  void doCompute(TTile &, double, const TRenderSettings &) override { computed = true; }
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
  near(linearize(0.04045), 0.0031308049535603713, 1e-14, "sRGB toe is incorrect");
  near(encode(0.0031308), 0.040449936, 1e-14, "sRGB inverse toe is incorrect");
  for (double c : {-2.5, -0.2, 0.0, 0.001, 0.3, 1.0, 4.5})
    near(encode(linearize(c)), c, 1e-12, "Transfer lost signed HDR values");
  Settings s;
  Pixel black{0, 0, 0, 1}, white{1, 1, 1, 1};
  s.version = 0;
  near(mix(black, white, .5, s).r, .5, 1e-14, "v1 must blend encoded RGB");
  s.version = 1;
  near(mix(black, white, .5, s).r, 0.7353569830524495, 1e-13,
       "v2 must blend in sRGB linear light");
  for (int version : {0, 1}) {
    s.version = version;
    for (int mode : {0, 1, 2}) {
      s.gammaMode = mode;
      s.gamma = 1;
      near(mix(black, white, .5, s).r, version ? .7353569830524495 : .5,
           1e-13, "Gamma one changed the selected version");
      s.gamma = 2.4;
      const double result = mix(black, white, .5, s).r;
      if (mode) require(result < (version ? .735 : .5), "Gamma did not favor dark lines");
      else near(result, version ? .7353569830524495 : .5, 1e-13, "None applied gamma");
    }
  }
  s.version = 0;
  s.gammaMode = 1;
  Pixel red{1, 0, 0, 1}, blue{0, 0, 1, 1};
  near(mix(red, blue, .5, s).r, .5, 1e-13, "Black-only gamma affected red/blue");
  s.gammaMode = 2;
  near(mix(red, blue, .5, s).r, std::pow(.5, 2.4), 1e-13, "Global gamma omitted red/blue");
  s.gammaMode = 1;
  s.gammaCount = 5;
  s.colors[4] = red;
  near(mix(red, blue, .5, s).r, std::pow(.5, 2.4), 1e-13, "Fifth gamma slot is ignored");
  s.gammaCount = 4;
  near(mix(red, blue, .5, s).r, .5, 1e-13, "Inactive gamma slot still matches");
  s.gammaCount = 0;
  near(mix(black, white, .5, s).r, .5, 1e-13, "Zero gamma count applied correction");
  const double threshold = .5 / 255;
  require(matches(Pixel{std::nextafter(threshold, 0.0), 0, 0, 1}, black),
          "Gamma tolerance rejected its interior");
  require(!matches(Pixel{threshold, 0, 0, 1}, black), "Gamma tolerance included its tie");
  require(matches(Pixel{.25, 0, 0, .25}, red), "Partial alpha broadened selection");
  s.gammaMode = 0;
  const Pixel covered = mix(Pixel{.5, 0, 0, .5}, Pixel{0, 0, 0, 0}, .5, s);
  near(covered.r, .25, 1e-14, "Transparent edge changed straight color");
  near(covered.a, .25, 1e-14, "Gamma changed coverage");
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
  CelSmoother2Fx fx;
  auto *source = new OversizedSource;
  fx.getInputPort(0)->setFx(source);
  bool rejected = false;
  try { render<TPixelF>(fx, 1, 1); }
  catch (const TException &) { rejected = true; }
  require(rejected && !source->computed, "Oversized source reached the host allocator");
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

TRasterFP hostRender(CelSmoother2Fx &fx, int strips, const TAffine &affine,
                    bool linear) {
  constexpr int width = 48, height = 40;
  TRenderer renderer(1);
  const unsigned long id = TRenderer::buildRenderId();
  renderer.install(id);
  renderer.declareRenderStart(id);
  renderer.declareFrameStart(0);
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
      fx.compute(tile, 0, settings);
      output->copy(tile.getRaster(), TPoint(0, y0));
    }
  } catch (...) {
    renderer.declareFrameEnd(0);
    renderer.declareRenderEnd(id);
    renderer.uninstall();
    throw;
  }
  renderer.declareFrameEnd(0);
  renderer.declareRenderEnd(id);
  renderer.uninstall();
  return output;
}

void testHostColorSpaceAndTransforms() {
  CelSmoother2Fx fx;
  fx.getInputPort(0)->setFx(new Source(fixture<TPixelF>(), TPoint(-9, -7)));
  set(fx, "gammaCorrection", 1);
  set(fx, "extraSmooth", 60);
  const auto same = [](const TRasterFP &a, const TRasterFP &b, const char *message) {
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
  };
  for (int version : {0, 1}) {
    set(fx, "smootherVersion", version);
    auto encoded = hostRender(fx, 1, TAffine(), false);
    auto linear = hostRender(fx, 1, TAffine(), true);
    TRop::toLinearRGB(encoded, 2.2);
    same(encoded, linear, "Working-space gamma was applied incorrectly");
    const TAffine affine = TTranslation(2.5, -3.25) * TRotation(21) * TScale(1.3, .7);
    for (bool workingLinear : {false, true})
      same(hostRender(fx, 1, affine, workingLinear),
           hostRender(fx, 8, affine, workingLinear),
           "Affine or linear rendering created tile seams");
  }
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
    { TOStream stream(path, compressed); stream << owner; }
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
  { TOStream stream(path); stream << macroOwner; }
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
  QTemporaryDir directory;
  int result = 1;
  try {
    require(directory.isValid(), "Cannot create fixture directory");
    testTransferAndGamma();
    testAllocationBoundaries();
    testRendering<TPixel32>();
    testRendering<TPixel64>();
    testRendering<TPixelF>();
    testKeyHdrAndCancellation();
    testHostColorSpaceAndTransforms();
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
