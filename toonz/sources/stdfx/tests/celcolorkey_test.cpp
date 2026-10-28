#include "../celcolorkeyfx.cpp"
#include "tparamcontainer.h"
#include "tfxutil.h"
#include "tmacrofx.h"
#include "trenderer.h"
#include "tstream.h"
#include "tthread.h"
#include <QCoreApplication>
#include <QTemporaryDir>
#include <iostream>
#include <future>
#include <memory>
#include <stdexcept>

namespace {
void require(bool condition, const char *message) {
  if (!condition) throw std::runtime_error(message);
}
TDoubleParamP number(TFx &fx, const std::string &name) {
  return TParamP(fx.getParams()->getParam(name));
}
TPixelParamP color(TFx &fx, const std::string &name) {
  return TParamP(fx.getParams()->getParam(name));
}
class Source final : public TStandardRasterFx {
  TRasterP m_raster;
  bool *m_observedClean;
  bool m_affineAware;
public:
  explicit Source(const TRasterP &raster, bool *observedClean = nullptr,
                  bool affineAware = false)
      : m_raster(raster), m_observedClean(observedClean),
        m_affineAware(affineAware) {}
  const TPersistDeclaration *getDeclaration() const override { return nullptr; }
  bool canHandle(const TRenderSettings &, double) override { return true; }
  bool doGetBBox(double, TRectD &box, const TRenderSettings &) override {
    box = TRectD(0, 0, m_raster->getLx(), m_raster->getLy()); return true;
  }
  std::string getAlias(double, const TRenderSettings &) const override { return "key-test-source"; }
  void compute(TTile &tile, double, const TRenderSettings &settings) override {
    if (m_affineAware) {
      tile.getRaster()->clear();
      TTile source(m_raster, TPointD());
      TRasterFx::applyAffine(tile, source, settings);
      return;
    }
    if (m_observedClean) {
      TRaster32P raster = tile.getRaster();
      *m_observedClean = bool(raster);
      if (raster)
        for (int y = 0; y < raster->getLy(); ++y)
          for (int x = 0; x < raster->getLx(); ++x)
            *m_observedClean = *m_observedClean && raster->pixels(y)[x] == TPixel32::Transparent;
    } else tile.getRaster()->clear();
    tile.getRaster()->copy(m_raster, TPoint(-int(tile.m_pos.x), -int(tile.m_pos.y)));
  }
  void doCompute(TTile &tile, double frame, const TRenderSettings &settings) override { compute(tile, frame, settings); }
};
template <class Pixel>
TRasterPT<Pixel> render(CelColorKeyFx &fx, int width, TPointD position = TPointD()) {
  TRasterPT<Pixel> result(width, 1); TTile tile; tile.setRaster(result); tile.m_pos = position;
  fx.doCompute(tile, 0, TRenderSettings()); return result;
}

template <class Pixel>
TRasterPT<Pixel> renderRect(CelColorKeyFx &fx, int width, int height,
                           TPointD position = TPointD()) {
  TRasterPT<Pixel> result(width, height);
  TTile tile(result, position);
  fx.doCompute(tile, 0, TRenderSettings());
  return result;
}

template <class Pixel>
void testTwoDimensionalMasks() {
  constexpr int width = 19, height = 13;
  CelColorKeyFx fx;
  TRasterPT<Pixel> source(width, height);
  const Pixel red = PixelConverter<Pixel>::from(TPixelD(1, 0, 0, 1));
  const Pixel green = PixelConverter<Pixel>::from(TPixelD(0, 1, 0, 1));
  for (int y = 0; y < height; ++y)
    for (int x = 0; x < width; ++x)
      source->pixels(y)[x] = x >= 6 && x <= 12 && y >= 4 && y <= 8 ? green : red;
  fx.getInputPort(0)->setFx(new Source(source));
  number(fx, "use001")->setDefaultValue(1);
  color(fx, "key001")->setDefaultValue(TPixel32(0, 255, 0));
  number(fx, "blur")->setDefaultValue(2.0);
  const double tolerance = std::is_same<Pixel, TPixelF>::value ? 1e-6 :
      (std::is_same<Pixel, TPixel32>::value ? 1.0 / 255 : 1.0 / 65535);
  for (int metric : {0, 1, 2}) {
    number(fx, "blurMetric")->setDefaultValue(metric);
    for (int direction : {0, 1, 2}) {
      number(fx, "blurDirection")->setDefaultValue(direction);
      for (int keep : {0, 1}) {
        number(fx, "keep")->setDefaultValue(keep);
        const auto output = renderRect<Pixel>(fx, width, height);
        for (int y = 0; y < height; ++y)
          for (int x = 0; x < width; ++x) {
            const bool inside = x >= 6 && x <= 12 && y >= 4 && y <= 8;
            const int dx = std::max({6 - x, 0, x - 12});
            const int dy = std::max({4 - y, 0, y - 8});
            // Closed-form distances to an axis-aligned rectangle, independent
            // of the production distance transform and its tile domain.
            const double distance = inside ? std::min({x - 5, 13 - x, y - 3, 9 - y}) :
                metric == 0 ? std::max(dx, dy) :
                metric == 1 ? std::max(dx, dy) + std::min(dx, dy) / 3.0 :
                              std::hypot(double(dx), double(dy));
            double coverage;
            if (inside)
              coverage = direction == 2 ? 1.0 : direction == 0 ? distance / 3.0 :
                         0.5 + (distance - 0.5) / 5.0;
            else
              coverage = direction == 0 ? 0.0 : direction == 2 ? 1.0 - distance / 3.0 :
                         0.5 - (distance - 0.5) / 5.0;
            coverage = std::max(0.0, std::min(1.0, coverage));
            const double alpha = keep ? coverage : 1.0 - coverage;
            const TPixelD pixel = toPixelD(output->pixels(y)[x]);
            require(std::abs(pixel.m - alpha) <= tolerance && pixel.b == 0.0 &&
                        std::abs(pixel.r - (inside ? 0.0 : alpha)) <= tolerance &&
                        std::abs(pixel.g - (inside ? alpha : 0.0)) <= tolerance,
                    "Two-dimensional feather differs from rectangle geometry");
          }
      }
    }
    number(fx, "thinMetric")->setDefaultValue(metric);
    number(fx, "thin")->setDefaultValue(1.5);
    number(fx, "keep")->setDefaultValue(1);
    const auto full = renderRect<Pixel>(fx, width + 4, height + 4, TPointD(-2, -2));
    std::vector<std::future<void>> jobs;
    for (int row = 0; row < 4; ++row)
      jobs.push_back(std::async(std::launch::async, [&, row] {
        const int y0 = (height + 4) * row / 4;
        const int y1 = (height + 4) * (row + 1) / 4;
        const auto part = renderRect<Pixel>(fx, width + 4, y1 - y0,
                                            TPointD(-2, y0 - 2));
        for (int y = y0; y < y1; ++y)
          for (int x = 0; x < width + 4; ++x)
            require(part->pixels(y - y0)[x] == full->pixels(y)[x],
                    "Composed thinning and feathering changed across concurrent tiles");
      }));
    for (auto &job : jobs) job.get();
    number(fx, "blur")->setDefaultValue(0);
    for (double amount : {-4000.0, 4000.0}) {
      number(fx, "thin")->setDefaultValue(amount);
      const auto large = renderRect<Pixel>(fx, width, height);
      for (int y = 0; y < height; ++y)
        for (int x = 0; x < width; ++x)
          require(large->pixels(y)[x] ==
                      (amount > 0 ? source->pixels(y)[x] : Pixel::Transparent),
                  "Large typed thinning radius changed finite-domain coverage");
    }
    number(fx, "thin")->setDefaultValue(0);
    number(fx, "blur")->setDefaultValue(2);
  }
}

struct RenderContext {
  TRenderer renderer;
  unsigned long id;
  RenderContext() : renderer(1), id(TRenderer::buildRenderId()) {
    renderer.install(id);
    renderer.declareRenderStart(id);
    renderer.declareFrameStart(0);
  }
  ~RenderContext() {
    renderer.declareFrameEnd(0);
    renderer.declareRenderEnd(id);
    renderer.uninstall();
  }
};

template <class Pixel>
TRasterPT<Pixel> hostRender(TRasterFx &fx, const TAffine &affine, int strips,
                           bool linear = false) {
  RenderContext context;
  constexpr int width = 48, height = 36;
  TRasterPT<Pixel> output(width, height);
  output->clear(); output->setLinear(linear);
  TRenderSettings settings;
  settings.m_bpp = std::is_same<Pixel, TPixelF>::value ? 128 :
                  std::is_same<Pixel, TPixel32>::value ? 32 : 64;
  settings.m_affine = affine;
  settings.m_linearColorSpace = linear;
  settings.m_cameraBox = TRectD(-8, -6, 40, 30);
  for (int strip = 0; strip < strips; ++strip) {
    const int y0 = height * strip / strips, y1 = height * (strip + 1) / strips;
    TRasterPT<Pixel> part(width, y1 - y0);
    part->clear(); part->setLinear(linear);
    TTile tile(part, TPointD(-8.25, -6.5 + y0));
    fx.compute(tile, 0, settings);
    output->copy(tile.getRaster(), TPoint(0, y0));
  }
  return output;
}

template <class Pixel>
void compareImages(const TRasterPT<Pixel> &left, const TRasterPT<Pixel> &right,
                   const char *message) {
  const double tolerance = std::is_same<Pixel, TPixelF>::value ? 2e-5 :
      (std::is_same<Pixel, TPixel32>::value ? 1.0 / 255 : 1.0 / 65535);
  bool visible = false;
  for (int y = 0; y < left->getLy(); ++y)
    for (int x = 0; x < left->getLx(); ++x) {
      const TPixelD a = toPixelD(left->pixels(y)[x]);
      const TPixelD b = toPixelD(right->pixels(y)[x]);
      visible = visible || a.m > 0.1;
      require(std::max({std::abs(a.r-b.r), std::abs(a.g-b.g),
                        std::abs(a.b-b.b), std::abs(a.m-b.m)}) <= tolerance, message);
    }
  require(visible, "Host comparison rendered an empty fixture");
}

template <class Pixel>
void testHostTransforms() {
  TRasterPT<Pixel> image(17, 13);
  for (int y = 0; y < 13; ++y)
    for (int x = 0; x < 17; ++x) {
      const double alpha = (x + y) % 3 == 0 ? 0.5 : 1.0;
      image->pixels(y)[x] = PixelConverter<Pixel>::from(
          x >= 5 && x <= 11 && y >= 3 && y <= 9 ? TPixelD(0, alpha, 0, alpha) :
                                                               TPixelD(alpha, 0, 0, alpha));
    }
  TFxP owner = new CelColorKeyFx;
  auto &fx = *static_cast<CelColorKeyFx *>(owner.getPointer());
  fx.getInputPort(0)->setFx(new Source(image, nullptr, true));
  number(fx, "use001")->setDefaultValue(1);
  number(fx, "keep")->setDefaultValue(1);
  color(fx, "key001")->setDefaultValue(TPixel32(0, 255, 0));
  number(fx, "thin")->setDefaultValue(1.5);
  number(fx, "blur")->setDefaultValue(2.25);
  const std::vector<TAffine> affines{
      TTranslation(2.5, -1.25) * TScale(1.25),
      TTranslation(3.5, 1.25) * TRotation(27) * TScale(1.4, 0.7),
      TTranslation(18.5, 1.25) * TScale(-1.2, 0.8),
      TAffine(1.1, 0.35, 2.5, -0.2, 0.9, 1.75)};
  for (const auto &affine : affines)
    for (bool linear : {false, true})
      compareImages(hostRender<Pixel>(fx, affine, 1, linear),
                    hostRender<Pixel>(fx, affine, 6, linear),
                    "Transformed Color Key introduced strip seams");
}

template <class Pixel>
void testLevelDpiGrid() {
  TRasterPT<Pixel> image(17, 13);
  for (int y = 0; y < 13; ++y)
    for (int x = 0; x < 17; ++x)
      image->pixels(y)[x] = PixelConverter<Pixel>::from(
          (x == 8 || (x >= 5 && x <= 11 && y >= 5 && y <= 7)) ?
              TPixelD(0, 1, 0, 1) : TPixelD(1, 0, 0, 1));
  TFxP source = new Source(image, nullptr, true);
  for (const TAffine &placement : std::vector<TAffine>{TScale(0.25),
                                  TTranslation(1.125, -0.375) * TScale(0.25, 0.5)}) {
    TFxP dpi = TFxUtil::makeAffine(source, placement);
    dpi->setName(L"LevelColumn AffineFx");
    TFxP nativeOwner = new CelColorKeyFx, dpiOwner = new CelColorKeyFx;
    auto &native = *static_cast<CelColorKeyFx *>(nativeOwner.getPointer());
    auto &keyed = *static_cast<CelColorKeyFx *>(dpiOwner.getPointer());
    native.getInputPort(0)->setFx(source.getPointer());
    keyed.getInputPort(0)->setFx(dpi.getPointer());
    for (TFx *effect : {nativeOwner.getPointer(), dpiOwner.getPointer()}) {
      number(*effect, "use001")->setDefaultValue(1);
      number(*effect, "keep")->setDefaultValue(1);
      color(*effect, "key001")->setDefaultValue(TPixel32(0, 255, 0));
    }
    for (bool edges : {false, true}) {
      for (TFx *effect : {nativeOwner.getPointer(), dpiOwner.getPointer()}) {
        number(*effect, "thin")->setDefaultValue(edges ? 1.5 : 0.0);
        number(*effect, "blur")->setDefaultValue(edges ? 1.25 : 0.0);
      }
      for (const TAffine &camera : {placement.inv(), TAffine(),
                                    TTranslation(2.5, 1.25) * TRotation(17) * TScale(3)}) {
        // Moving the DPI placement after the pixel operation is an independent
        // graph equivalence: classify and modify native pixels, then resample.
        compareImages(hostRender<Pixel>(native, camera * placement, 1),
                      hostRender<Pixel>(keyed, camera, 1),
                      "Level DPI resampled colors before Color Key");
        compareImages(hostRender<Pixel>(keyed, camera, 1),
                      hostRender<Pixel>(keyed, camera, 4),
                      "Level DPI introduced Color Key tile seams");
      }
    }
  }
}
template <class Pixel>
void testRaster() {
  CelColorKeyFx fx;
  TRasterPT<Pixel> source(3, 1);
  source->pixels(0)[0] = PixelConverter<Pixel>::from(TPixelD(1, 0, 0, 1));
  source->pixels(0)[1] = PixelConverter<Pixel>::from(TPixelD(0, 1, 0, 1));
  source->pixels(0)[2] = PixelConverter<Pixel>::from(TPixelD(1, 0, 0, 1));
  fx.getInputPort(0)->setFx(new Source(source));
  number(fx, "use001")->setDefaultValue(1);
  color(fx, "key001")->setDefaultValue(TPixel32(0, 255, 0));
  auto keyed = render<Pixel>(fx, 3);
  require(keyed->pixels(0)[0] == source->pixels(0)[0] && keyed->pixels(0)[1] == Pixel::Transparent,
          "Remove mode did not remove the selected green");
  number(fx, "keep")->setDefaultValue(1);
  auto kept = render<Pixel>(fx, 3);
  require(kept->pixels(0)[0] == Pixel::Transparent && kept->pixels(0)[1] == source->pixels(0)[1],
          "Keep mode selected the wrong pixels");
  number(fx, "enableReplace")->setDefaultValue(1);
  number(fx, "replace001")->setDefaultValue(1);
  color(fx, "replacement001")->setDefaultValue(TPixel32(255, 255, 255));
  auto replaced = render<Pixel>(fx, 3);
  require(replaced->pixels(0)[1] == Pixel::White, "Replacement color was not applied");
  source->pixels(0)[1] = PixelConverter<Pixel>::from(TPixelD(0, 0.5, 0, 0.5));
  color(fx, "replacement001")->getRed()->setDefaultValue(2.0);
  color(fx, "replacement001")->getGreen()->setDefaultValue(0.0);
  color(fx, "replacement001")->getBlue()->setDefaultValue(0.0);
  const TPixelD highReplacement = toPixelD(render<Pixel>(fx, 3)->pixels(0)[1]);
  if (std::is_same<Pixel, TPixelF>::value)
    require(highReplacement.r > highReplacement.m, "Float replacement lost HDR range");
  else
    require(highReplacement.r <= highReplacement.m, "Integer replacement exceeds alpha");
  source->pixels(0)[1] = PixelConverter<Pixel>::from(TPixelD(0, 1, 0, 1));
  number(fx, "enableReplace")->setDefaultValue(0);
  number(fx, "thin")->setDefaultValue(1);
  auto expanded = render<Pixel>(fx, 3);
  require(expanded->pixels(0)[0] == source->pixels(0)[0] && expanded->pixels(0)[2] == source->pixels(0)[2],
          "Positive edge thin did not expand the selected region");
  number(fx, "thin")->setDefaultValue(-1);
  auto contracted = render<Pixel>(fx, 3);
  require(contracted->pixels(0)[1] == Pixel::Transparent, "Negative edge thin did not contract the selected region");
  number(fx, "thin")->setDefaultValue(0);
  number(fx, "enabledColors")->setDefaultValue(25);
  number(fx, "use001")->setDefaultValue(0);
  number(fx, "use025")->setDefaultValue(1);
  color(fx, "key025")->setDefaultValue(TPixel32(255, 0, 0));
  auto last = render<Pixel>(fx, 3);
  require(last->pixels(0)[0] == source->pixels(0)[0] && last->pixels(0)[1] == Pixel::Transparent,
          "The last color entry did not select red");
  auto padded = render<Pixel>(fx, 5, TPointD(-1, 0));
  require(padded->pixels(0)[0] == Pixel::Transparent && padded->pixels(0)[1] == last->pixels(0)[0] &&
              padded->pixels(0)[3] == last->pixels(0)[2] && padded->pixels(0)[4] == Pixel::Transparent,
          "Negative-origin tile differed from the full result");
  int canceled = 1; TRenderSettings settings; settings.m_isCanceled = &canceled;
  TTile tile; TRasterPT<Pixel> output(3, 1); output->fill(Pixel::White); tile.setRaster(output);
  fx.doCompute(tile, 0, settings);
  require(output->pixels(0)[0] == Pixel::Transparent, "Canceled output was not cleared");
}
void testPartiallyWrittenSource() {
  CelColorKeyFx fx;
  TRaster32P pixels(1, 1); pixels->pixels(0)[0] = TPixel32::White;
  bool observedClean = false;
  fx.getInputPort(0)->setFx(new Source(pixels, &observedClean));
  auto output = render<TPixel32>(fx, 3, TPointD(-1, 0));
  require(observedClean, "Source tile was not initialized before upstream rendering");
  require(output->pixels(0)[0] == TPixel32::Transparent &&
              output->pixels(0)[1] == TPixel32::White &&
              output->pixels(0)[2] == TPixel32::Transparent,
          "Partial upstream output changed transparent pixels");
}
void verifyStoredParameters(TFx &restored, TFx &original) {
  for (const char *name : {"keep", "enabledColors", "perColor", "perComponent",
                          "colorSpace", "precision", "enableReplace", "thin", "blur"})
    for (double frame : {0.0, 5.0, 10.0})
      require(number(restored, name)->getValue(frame) == number(original, name)->getValue(frame),
              "Persistence changed a global mode or edge parameter");
  for (int i = 0; i < 25; ++i) {
    for (const char *prefix : {"use", "replace", "threshold", "component1", "component2", "component3"})
      for (double frame : {0.0, 5.0, 10.0})
        require(number(restored, slotName(prefix, i))->getValue(frame) ==
                    number(original, slotName(prefix, i))->getValue(frame),
                "Persistence changed an indexed parameter");
    for (const char *prefix : {"key", "replacement"})
      require(color(restored, slotName(prefix, i))->getValueD(10) ==
                  color(original, slotName(prefix, i))->getValueD(10),
              "Persistence changed a sampled or replacement color");
  }
  require(number(restored, "enableReplace")->isKeyframe(10) &&
              number(restored, "component3025")->isKeyframe(10) &&
              color(restored, "replacement025")->isKeyframe(10),
          "Persistence dropped hidden entry animation");
  double minimum, maximum;
  require(number(restored, "thin")->getUISliderRange(minimum, maximum) &&
              minimum == -100 && maximum == 100 &&
              number(restored, "blur")->getUISliderRange(minimum, maximum) &&
              minimum == 0 && maximum == 100 &&
              number(restored, "enableReplace")->isUICheckbox(),
          "Persistence dropped native editor metadata");
}

void configureStoredParameters(TFx &fx) {
  number(fx, "keep")->setDefaultValue(1);
  number(fx, "enabledColors")->setDefaultValue(1);
  number(fx, "perColor")->setDefaultValue(1);
  number(fx, "perComponent")->setDefaultValue(1);
  number(fx, "colorSpace")->setDefaultValue(3);
  number(fx, "precision")->setDefaultValue(2);
  number(fx, "enableReplace")->setValue(0, 0);
  number(fx, "enableReplace")->setValue(10, 1);
  number(fx, "thin")->setValue(0, -250);
  number(fx, "thin")->setValue(10, 4000);
  number(fx, "blur")->setDefaultValue(250);
  for (int i = 0; i < 25; ++i) {
    number(fx, slotName("use", i))->setDefaultValue(i % 2);
    number(fx, slotName("replace", i))->setDefaultValue((i + 1) % 2);
    for (const char *prefix : {"threshold", "component1", "component2", "component3"}) {
      number(fx, slotName(prefix, i))->setValue(0, (i + 1) * 0.0000000123456789);
      number(fx, slotName(prefix, i))->setValue(10, (i + 1) / 25.0);
    }
    color(fx, slotName("key", i))->setValueD(10, TPixelD((i + 1) * 0.0123456789123, .5, .75, 1));
    color(fx, slotName("replacement", i))->setValueD(10, TPixelD(1.23456789e-12, i + .123456789, -.25, 1));
  }
}

void testPresetAndMacro(const QString &presetPath, const QString &macroPath) {
  TFxP first = new CelColorKeyFx, second = new CelColorKeyFx;
  first->setFxId(L"CelColorKey1"); second->setFxId(L"CelColorKey2");
  configureStoredParameters(*first);
  TFxP clone = first->clone(false);
  verifyStoredParameters(*clone, *first);
  {
    TOStream stream(TFilePath(presetPath.toStdWString()));
    first->savePreset(stream);
    require(!stream.setFullPrecision(false), "Preset leaked its precision mode");
  }
  CelColorKeyFx preset;
  { TIStream stream(TFilePath(presetPath.toStdWString())); preset.loadPreset(stream); }
  verifyStoredParameters(preset, *first);
  second->getInputPort(0)->setFx(first.getPointer());
  TFxP owner = TMacroFx::create({first, second});
  auto *macro = dynamic_cast<TMacroFx *>(owner.getPointer());
  require(macro != nullptr, "Cannot create Color Key macro");
  for (const char *name : {"thin", "enableReplace", "component3025", "replacement025"})
    macro->setParamExposed(first.getPointer(), name, true);
  { TOStream stream(TFilePath(macroPath.toStdWString())); stream << macro; }
  TIStream stream(TFilePath(macroPath.toStdWString()));
  TFxP restored; stream >> restored;
  auto *savedMacro = dynamic_cast<TMacroFx *>(restored.getPointer());
  TFx *member = savedMacro ? savedMacro->getFxById(L"CelColorKey1") : nullptr;
  require(member != nullptr, "Macro lost its Color Key member");
  verifyStoredParameters(*member, *first);
  for (const char *name : {"thin", "enableReplace", "component3025", "replacement025"})
    require(savedMacro->isParamExposed(member, name), "Macro lost an exposed Color Key control");
}

void testPersistence(const QString &path, bool compressed = false) {
  CelColorKeyFx fx;
  configureStoredParameters(fx);
  number(fx, "enabledColors")->setDefaultValue(25);
  number(fx, "use025")->setValue(3, 1);
  color(fx, "key025")->getRed()->setValue(0, 0.123456789);
  { TOStream stream(TFilePath(path.toStdWString()), compressed); stream << &fx; }
  TIStream stream(TFilePath(path.toStdWString())); TPersist *saved = nullptr; stream >> saved;
  std::unique_ptr<TPersist> owner(saved);
  auto *loaded = dynamic_cast<CelColorKeyFx *>(saved);
  require(loaded && number(*loaded, "use025")->isKeyframe(3), "Entry animation was not restored");
  verifyStoredParameters(*loaded, fx);
  require(color(*loaded, "key025")->getValueD(0).r == 0.123456789, "Entry precision was not restored");
  require(number(*loaded, "use025")->isUICheckbox(), "Animated choice metadata was not restored");
  double sliderMin, sliderMax;
  require(number(*loaded, "thin")->getUISliderRange(sliderMin, sliderMax) &&
              sliderMin == -100.0 && sliderMax == 100.0,
          "Scene restore lost edge thin slider bounds");
  require(number(*loaded, "blur")->getUISliderRange(sliderMin, sliderMax) &&
              sliderMin == 0.0 && sliderMax == 100.0,
          "Scene restore lost edge blur slider bounds");
}
}
int main(int argc, char **argv) {
  QCoreApplication app(argc, argv);
  TThread::init();
  TRenderer::initialize();
  int result = 1;
  try {
    testRaster<TPixel32>(); testRaster<TPixel64>(); testRaster<TPixelF>();
    std::cout << "Checking two-dimensional masks and concurrent tiles" << std::endl;
    testTwoDimensionalMasks<TPixel32>();
    testTwoDimensionalMasks<TPixel64>();
    testTwoDimensionalMasks<TPixelF>();
    std::cout << "Checking transformed production rendering" << std::endl;
    testHostTransforms<TPixel32>(); testHostTransforms<TPixel64>(); testHostTransforms<TPixelF>();
    std::cout << "Checking native pixels across level DPI placement" << std::endl;
    testLevelDpiGrid<TPixel32>(); testLevelDpiGrid<TPixel64>(); testLevelDpiGrid<TPixelF>();
    testPartiallyWrittenSource();
    checkInputSize(2, 3, 16);
    bool tooLarge = false;
    try { checkInputSize(16005, 16005, 16); }
    catch (const TException &) { tooLarge = true; }
    require(tooLarge, "Oversized raster dimensions were not rejected");
    QTemporaryDir directory; require(directory.isValid(), "Cannot create temporary files");
    testPersistence(directory.filePath("color-key.fx"));
    testPersistence(directory.filePath("color-key-compressed.fx"), true);
    testPresetAndMacro(directory.filePath("color-key.preset"), directory.filePath("color-key.macro"));
    std::cout << "PASS: color key raster modes, entries and persistence\n";
    result = 0;
  } catch (const TException &e) {
    std::cerr << QString::fromStdWString(e.getMessage()).toStdString() << '\n';
  } catch (const std::exception &e) { std::cerr << e.what() << '\n'; }
  TThread::shutdown();
  return result;
}
