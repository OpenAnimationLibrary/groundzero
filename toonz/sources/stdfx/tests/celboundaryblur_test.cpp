#include "../celboundaryblurfx.cpp"
#include "../celcolorkeepfx.cpp"
#include "tmacrofx.h"
#include "tparamcontainer.h"
#include "trenderer.h"
#include "tstream.h"
#include "tthread.h"

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
TDoubleParamP parameter(TFx &fx, const std::string &name) {
  return TParamP(fx.getParams()->getParam(name));
}
void value(TFx &fx, const std::string &name, double number) {
  parameter(fx, name)->setDefaultValue(number);
}

// Deliberately direct O(radius * pixels) reference, independent of the prefix
// sum implementation. The coefficients follow the documented native formula.
std::vector<CelBlur::Pixel> reference(std::vector<CelBlur::Pixel> pixels,
    int width, int height, const CelBlur::Settings &settings) {
  for (int repeat = 0; repeat < settings.repeats; ++repeat) {
    for (bool vertical : {!settings.vertical, settings.vertical}) {
      auto result = pixels;
      for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
          const int at = y * width + x;
          if (pixels[at][3] <= 0) continue;
          CelBlur::Pixel sum{};
          for (int step = -int(std::ceil(settings.radius));
               step <= int(std::ceil(settings.radius)); ++step) {
            const int sx = x + (vertical ? 0 : step);
            const int sy = y + (vertical ? step : 0);
            if (sx < 0 || sx >= width || sy < 0 || sy >= height) continue;
            auto pixel = pixels[sy * width + sx];
            if (settings.legacy && pixel[3] > 0) {
              for (int c = 0; c < 3; ++c) pixel[c] /= pixel[3];
              pixel[3] = 1;
            }
            const double weight = settings.legacy ? 1 :
                std::max(0.0, settings.radius + 1 - std::abs(step));
            for (int c = 0; c < 4; ++c) sum[c] += pixel[c] * weight;
          }
          for (int c = 0; c < 3; ++c)
            result[at][c] = pixels[at][c] * (1 - settings.blend) +
                (sum[c] / sum[3]) * pixels[at][3] * settings.blend;
        }
      }
      pixels.swap(result);
    }
  }
  return pixels;
}

void kernelTests() {
  std::vector<CelBlur::Pixel> image(63);
  for (int y = 0; y < 7; ++y)
    for (int x = 0; x < 9; ++x) {
      const double alpha = ((x * 7 + y * 3) % 5) * 0.25;
      image[y * 9 + x] = {{alpha * (x < 4 ? 2.0 : 0.1), alpha * y / 7,
                           alpha * -0.25, alpha}};
    }
  for (double radius : {1.0, 3.25, 100.0, 1000.0})
    for (double blend : {0.01, 1.0})
      for (int repeats : {0, 1, 10})
        for (bool vertical : {false, true})
          for (bool legacy : {false, true}) {
          CelBlur::Settings settings;
          settings.radius = radius;
          settings.blend = blend;
          settings.repeats = repeats;
          settings.vertical = vertical;
          settings.legacy = legacy;
          auto actual = image;
          const auto expected = reference(image, 9, 7, settings);
          require(CelBlur::filter(actual, 9, 7, settings), "Unexpected cancellation");
          for (std::size_t i = 0; i < actual.size(); ++i)
            for (int c = 0; c < 4; ++c)
              require(std::abs(actual[i][c] - expected[i][c]) < 1e-10,
                      "Optimized filter differs from independent reference");
        }
  CelBlur::Settings settings;
  settings.radius = 1;
  settings.blend = 1;
  std::vector<CelBlur::Pixel> edge{{{1, 0, 0, 1}}, {{0, 0, 0.5, 0.5}}, {{0, 0, 0, 0}}};
  require(CelBlur::filter(edge, 3, 1, settings), "Edge filter canceled");
  require(std::abs(edge[0][0] - 0.8) < 1e-12 &&
              std::abs(edge[0][2] - 0.2) < 1e-12,
          "Partial coverage introduced a dark fringe");
  require(edge[1][3] == 0.5 && edge[2] == CelBlur::Pixel{}, "Alpha silhouette expanded");
  auto horizontal = image, vertical = image;
  settings.vertical = true;
  CelBlur::filter(vertical, 9, 7, settings);
  settings.vertical = false;
  CelBlur::filter(horizontal, 9, 7, settings);
  double difference = 0;
  for (int i = 0; i < 63; ++i) difference += std::abs(horizontal[i][0] - vertical[i][0]);
  require(difference > 0.01, "Bias direction has no effect on an asymmetric mask");
  int canceled = 1;
  require(!CelBlur::filter(horizontal, 9, 7, settings, &canceled), "Cancellation ignored");
  require(CelBlur::checkedArea(4096, 4096) == std::size_t(4096) * 4096,
          "A 4K square image was rejected before allocation");
  require(blurMemoryMB(TRectD(0, 0, 4096, 4096), 160) == 2560 &&
              blurMemoryMB(TRectD(0, 0, 1e12, 1e12), 160) == std::numeric_limits<int>::max(),
          "Working-set estimate overflowed before allocation");
  bool limited = false;
  try { CelBlur::checkedArea(std::numeric_limits<int>::max(), 10); }
  catch (const std::runtime_error &) { limited = true; }
  require(limited, "Unchecked working-image allocation");
  limited = false;
  try { CelBlur::checkedArea(134217728, 1); }
  catch (const std::runtime_error &) { limited = true; }
  require(limited, "Signed-int raster allocation boundary was accepted");
}

class BlurSource final : public TStandardRasterFx {
  TRasterP m_image;
  int *m_cancel;
  bool m_clear;
  bool m_transform;
public:
  TRectD dryRequest;
  explicit BlurSource(TRasterP image, int *cancel = nullptr, bool clear = true,
                      bool transform = false)
      : m_image(image), m_cancel(cancel), m_clear(clear), m_transform(transform) {}
  const TPersistDeclaration *getDeclaration() const override { return nullptr; }
  bool canHandle(const TRenderSettings &, double) override { return true; }
  bool doGetBBox(double, TRectD &box, const TRenderSettings &) override {
    box = TRectD(0, 0, m_image->getLx() + (m_clear ? 0 : 2),
                  m_image->getLy() + (m_clear ? 0 : 2)); return true;
  }
  std::string getAlias(double, const TRenderSettings &) const override { return "boundary-source"; }
  void compute(TTile &tile, double, const TRenderSettings &settings) override {
    if (m_clear) tile.getRaster()->clear();
    if (m_transform) {
      TTile source(m_image, TPointD());
      TRasterFx::applyAffine(tile, source, settings);
    } else {
      tile.getRaster()->copy(m_image, TPoint(-int(tile.m_pos.x), -int(tile.m_pos.y)));
    }
    if (m_cancel) *m_cancel = 1;
  }
  void doCompute(TTile &tile, double frame, const TRenderSettings &info) override { compute(tile, frame, info); }
  void dryCompute(TRectD &rect, double, const TRenderSettings &) override { dryRequest = rect; }
};

template <class Pixel>
TRasterPT<Pixel> render(CelBoundaryBlurFx &fx, int width, int height,
                       TPointD position = TPointD(), double frame = 0,
                       TRenderSettings info = TRenderSettings()) {
  TRasterPT<Pixel> result(width, height);
  TTile tile; tile.setRaster(result); tile.m_pos = position;
  fx.doCompute(tile, frame, info);
  return result;
}

template <class Pixel>
void renderTests() {
  const double scale = blurChannelScale<Pixel>();
  TRasterPT<Pixel> image(19, 11);
  for (int y = 0; y < 11; ++y)
    for (int x = 0; x < 19; ++x) {
      const double alpha = (x + y) % 5 ? 1 : 0;
      image->pixels(y)[x] = Pixel(x < 9 ? scale * alpha : 0,
          y < 6 ? scale * alpha : 0, 0, scale * alpha);
    }
  CelBoundaryBlurFx fx;
  auto *source = new BlurSource(image);
  fx.getInputPort(0)->setFx(source);
  value(fx, "amount", 2.25); value(fx, "smoothness", 70); value(fx, "repeats", 3);
  for (int bias : {0, 1})
    for (int legacy : {0, 1}) {
    value(fx, "bias", bias); value(fx, "legacy", legacy);
    auto full = render<Pixel>(fx, 23, 15, TPointD(-2, -2));
    std::vector<std::future<void>> jobs;
    for (int y = 0; y < 15; ++y)
      jobs.push_back(std::async(std::launch::async, [&, y] {
        auto strip = render<Pixel>(fx, 23, 1, TPointD(-2, y - 2));
        for (int x = 0; x < 23; ++x) {
          const auto &a = full->pixels(y)[x], &b = strip->pixels(0)[x];
          require(std::abs(double(a.r) - b.r) / scale < 1e-5 &&
                      std::abs(double(a.g) - b.g) / scale < 1e-5 && a.m == b.m,
                  "Strip/concurrent render changed output");
        }
      }));
    for (auto &job : jobs) job.get();
  }
  TRectD box;
  fx.doGetBBox(0, box, TRenderSettings());
  require(box == TRectD(0, 0, 19, 11), "Alpha bounds changed");
  TRenderSettings inputInfo, scaled; scaled.m_affine = TScale(2);
  TRectD request;
  fx.transform(0, 0, TRectD(0, 0, 4, 3), scaled, request, inputInfo);
  require(request == TRectD(0, 0, 19, 18), "Repeated scaled support is wrong");
  TRectD outputRect(0, 0, 4, 3);
  fx.doDryCompute(outputRect, 0, scaled);
  require(source->dryRequest == request, "Predictive and actual input support differ");
  value(fx, "amount", 1000); value(fx, "repeats", 10);
  const auto maximum = render<Pixel>(fx, 19, 11);
  require(maximum->pixels(0)[1].m == image->pixels(0)[1].m,
          "Maximum typed amount and repeats changed coverage");
  value(fx, "repeats", 0);
  auto identity = render<Pixel>(fx, 19, 11);
  for (int y = 0; y < 11; ++y)
    for (int x = 0; x < 19; ++x)
      require(identity->pixels(y)[x] == image->pixels(y)[x], "Zero repeats is not identity");
  value(fx, "repeats", 1);
  int canceled = 1;
  TRenderSettings cancelInfo; cancelInfo.m_isCanceled = &canceled;
  for (int initial : {1, 0}) {
    canceled = initial;
    fx.getInputPort(0)->setFx(new BlurSource(image, &canceled));
    auto result = render<Pixel>(fx, 3, 3, TPointD(), 0, cancelInfo);
    for (int y = 0; y < 3; ++y)
      for (int x = 0; x < 3; ++x)
        require(result->pixels(y)[x] == Pixel::Transparent, "Canceled tile was partially written");
  }
  TRasterPT<Pixel> partial(1, 1); partial->fill(Pixel::Black);
  fx.getInputPort(0)->setFx(new BlurSource(partial, nullptr, false));
  const auto cleared = render<Pixel>(fx, 3, 3);
  for (int y = 0; y < 3; ++y)
    for (int x = 0; x < 3; ++x)
      if (x || y) require(cleared->pixels(y)[x] == Pixel::Transparent,
                          "Unwritten upstream samples were not transparent");
  CelBoundaryBlurFx empty;
  require(render<Pixel>(empty, 1, 1)->pixels(0)[0] == Pixel::Transparent,
          "Disconnected source did not clear output");
}

void persistenceTests(const QTemporaryDir &temp) {
  TFxP fx = new CelBoundaryBlurFx;
  for (const std::string name : {"amount", "smoothness", "repeats", "bias", "legacy"}) {
    auto p = parameter(*fx, name);
    p->setValue(0, (name == "bias" || name == "legacy") ? 0 : 1);
    p->setValue(10, (name == "bias" || name == "legacy") ? 1 : 2);
  }
  value(*fx, "amount", 12.34567890123);
  auto clone = fx->clone(false);
  for (const std::string name : {"amount", "smoothness", "repeats", "bias", "legacy"})
    require(parameter(*clone, name)->getValue(10) == parameter(*fx, name)->getValue(10),
            "Cloning lost an animated parameter");
  const TFilePath path(temp.filePath("boundary.fx").toStdWString());
  { TOStream stream(path); stream << fx.getPointer(); }
  TFxP restored;
  { TIStream stream(path); stream >> restored; }
  require(restored->getFxType() == "STD_celBoundaryBlurFx", "Effect registration failed");
  require(parameter(*restored, "bias")->isKeyframe(10), "Choice animation did not persist");
  require(parameter(*restored, "bias")->getUIChoices().size() == 2, "Choice editor metadata was lost");
  const TFilePath presetPath(temp.filePath("boundary.preset").toStdWString());
  { TOStream stream(presetPath); fx->savePreset(stream); }
  CelBoundaryBlurFx preset;
  { TIStream stream(presetPath); preset.loadPreset(stream); }
  require(parameter(preset, "repeats")->isKeyframe(10), "Preset lost repeat animation");
  auto *native = dynamic_cast<CelBoundaryBlurFx *>(fx.getPointer());
  parameter(*fx, "amount")->setValue(0, 1.123456789);
  const auto alias = native->getAlias(0, TRenderSettings());
  parameter(*fx, "amount")->setValue(0, 1.123456788);
  require(alias != native->getAlias(0, TRenderSettings()), "Fine amount shares a cache alias");
  fx->setFxId(L"Boundary1");
  TFxP second = new CelBoundaryBlurFx; second->setFxId(L"Boundary2");
  second->getInputPort(0)->setFx(fx.getPointer());
  TFxP macroOwner = TMacroFx::create({fx, second});
  auto *macro = dynamic_cast<TMacroFx *>(macroOwner.getPointer());
  require(macro != nullptr, "Macro creation failed");
  macro->setParamExposed(fx.getPointer(), "bias", true);
  const TFilePath macroPath(temp.filePath("boundary.macro").toStdWString());
  { TOStream stream(macroPath); stream << macro; }
  TFxP loaded;
  { TIStream stream(macroPath); stream >> loaded; }
  auto *loadedMacro = dynamic_cast<TMacroFx *>(loaded.getPointer());
  TFx *member = loadedMacro ? loadedMacro->getFxById(L"Boundary1") : nullptr;
  require(member && loadedMacro->isParamExposed(member, "bias") &&
              parameter(*member, "bias")->isKeyframe(10), "Macro lost exposed animated choice");
}

void workflowTest() {
  // Chaining production effects enters TRasterFx::compute and its resource
  // cache, unlike the direct source fixtures. Install the same render/frame
  // lifecycle as the application before evaluating the connected graph.
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
  } context;
  TRaster32P image(3, 1);
  image->pixels(0)[0] = TPixel32(255, 0, 0);
  image->pixels(0)[1] = TPixel32(0, 0, 255);
  image->pixels(0)[2] = TPixel32(0, 255, 0);
  auto *keep = new CelColorKeepFx;
  TDoubleParamP enabled = TParamP(keep->getParams()->getParam("enabledColors"));
  enabled->setDefaultValue(2);
  TPixelParamP red = TParamP(keep->getParams()->getParam("color001"));
  TPixelParamP blue = TParamP(keep->getParams()->getParam("color002"));
  red->setDefaultValue(TPixel32(255, 0, 0)); blue->setDefaultValue(TPixel32(0, 0, 255));
  keep->getInputPort(0)->setFx(new BlurSource(image));
  CelBoundaryBlurFx blur;
  blur.getInputPort(0)->setFx(keep);
  value(blur, "amount", 1); value(blur, "smoothness", 100);
  const auto output = render<TPixel32>(blur, 3, 1);
  require(output->pixels(0)[0].b > 0 && output->pixels(0)[1].r > 0 &&
              output->pixels(0)[2] == TPixel32::Transparent,
          "Color Keep followed by boundary blur did not isolate color transitions");
}

struct BlurRenderContext {
  TRenderer renderer;
  unsigned long id;
  BlurRenderContext() : renderer(1), id(TRenderer::buildRenderId()) {
    renderer.install(id);
    renderer.declareRenderStart(id);
    renderer.declareFrameStart(0);
  }
  ~BlurRenderContext() {
    renderer.declareFrameEnd(0);
    renderer.declareRenderEnd(id);
    renderer.uninstall();
  }
};

template <class Pixel> void keyWorkflowTest() {
  BlurRenderContext context;
  const double scale = blurChannelScale<Pixel>();
  const double half = scale == 1 ? .5 : std::floor(scale / 2);
  const double alpha = half / scale;
  TRasterPT<Pixel> image(3,1);
  image->pixels(0)[0] = Pixel(scale,0,0,scale);
  image->pixels(0)[1] = Pixel(0,0,half,half);
  image->pixels(0)[2] = Pixel(0,scale,0,scale);
  TFxP key = TFx::create("STD_celColorKeyFx");
  require(bool(key), "Color Key production registration is missing");
  key->getInputPort(0)->setFx(new BlurSource(image));
  auto keyColor = [&](const char *name, TPixel32 color) {
    TPixelParamP slot = TParamP(key->getParams()->getParam(name));
    slot->setDefaultValue(color);
  };
  CelBoundaryBlurFx blur;
  blur.getInputPort(0)->setFx(key.getPointer());
  value(blur,"amount",1); value(blur,"smoothness",100); value(blur,"repeats",1);
  TRenderSettings settings;
  settings.m_bpp = scale == 1 ? 128 : (scale == 255 ? 32 : 64);
  const double tolerance = scale == 1 ? 1e-6 : 1.0 / scale;
  // Removing green and keeping red/blue must produce the same coverage mask.
  // Check the native analytical weights, without using the optimized filter
  // or the original plugin as an expected-output generator.
  for (bool keep : {false,true}) {
    value(*key,"keep",keep ? 1 : 0);
    value(*key,"enabledColors",keep ? 2 : 1);
    value(*key,"use001",1); value(*key,"use002",keep ? 1 : 0);
    keyColor("key001", keep ? TPixel32(255,0,0) : TPixel32(0,255,0));
    keyColor("key002", TPixel32(0,0,255));
    for (bool legacy : {false,true}) {
      value(blur,"legacy",legacy ? 1 : 0);
      const auto output = render<Pixel>(blur,3,1,TPointD(),0,settings);
      const double red0 = legacy ? .5 : 2.0 / (2.0 + alpha);
      const double blue0 = legacy ? .5 : alpha / (2.0 + alpha);
      const double red1 = legacy ? .5 * alpha : alpha / (1 + 2 * alpha);
      const double blue1 = legacy ? .5 * alpha : 2 * alpha * alpha / (1 + 2 * alpha);
      require(std::abs(output->pixels(0)[0].r / scale - red0) <= tolerance &&
                  std::abs(output->pixels(0)[0].b / scale - blue0) <= tolerance &&
                  std::abs(output->pixels(0)[1].r / scale - red1) <= tolerance &&
                  std::abs(output->pixels(0)[1].b / scale - blue1) <= tolerance,
              "Color Key followed by Blur changed native coverage weights");
      require(output->pixels(0)[0].m == scale && output->pixels(0)[1].m == half &&
                  output->pixels(0)[2] == Pixel::Transparent,
              "Color Key followed by Blur changed the selected alpha mask");
    }
  }
}

TRasterFP hostRender(CelBoundaryBlurFx &fx, int strips,
                     const TAffine &affine, bool linear) {
  BlurRenderContext context;
  constexpr int width = 48, height = 36;
  TRasterFP output(width,height);
  output->clear(); output->setLinear(linear);
  TRenderSettings settings;
  settings.m_bpp = 128;
  settings.m_affine = affine;
  settings.m_linearColorSpace = linear;
  settings.m_cameraBox = TRectD(-16,-12,32,24);
  for (int strip = 0; strip < strips; ++strip) {
    const int y0 = height * strip / strips, y1 = height * (strip + 1) / strips;
    TRasterFP part(width,y1-y0);
    part->clear(); part->setLinear(linear);
    TTile tile(part,TPointD(-16.25,-12.5+y0));
    fx.compute(tile,0,settings);
    output->copy(tile.getRaster(),TPoint(0,y0));
  }
  return output;
}

void hostTransformTests() {
  TRasterFP image(17,13);
  for (int y = 0; y < 13; ++y)
    for (int x = 0; x < 17; ++x) {
      const float alpha = ((x * 3 + y) % 7 == 0) ? 0 : ((x + y) % 3 == 0 ? .4f : 1);
      image->pixels(y)[x] = TPixelF(alpha * (x < 8 ? 2 : .1f),
          alpha * (y < 6 ? .2f : .75f), alpha * .4f, alpha);
    }
  TFxP owner = new CelBoundaryBlurFx;
  auto &fx = *static_cast<CelBoundaryBlurFx *>(owner.getPointer());
  fx.getInputPort(0)->setFx(new BlurSource(image,nullptr,true,true));
  value(fx,"amount",2.25); value(fx,"smoothness",63);
  const std::vector<TAffine> transforms{
      TTranslation(2.5,-1.25) * TScale(1.25),
      TTranslation(3.5,1.25) * TRotation(27) * TScale(1.4,.7),
      TTranslation(18.5,1.25) * TScale(-1.2,.8),
      TAffine(1.1,.35,2.5,-.2,.9,1.75)};
  for (const auto &affine : transforms)
    for (bool linear : {false,true})
      for (int bias : {0,1})
        for (int legacy : {0,1}) {
          value(fx,"bias",bias); value(fx,"legacy",legacy); value(fx,"repeats",3);
          const auto full = hostRender(fx,1,affine,linear);
          const auto strips = hostRender(fx,6,affine,linear);
          value(fx,"repeats",0);
          const auto identity = hostRender(fx,1,affine,linear);
          bool visible = false;
          for (int y = 0; y < full->getLy(); ++y)
            for (int x = 0; x < full->getLx(); ++x) {
              const auto &a = full->pixels(y)[x], &b = strips->pixels(y)[x];
              visible = visible || a.m > .2f;
              require(std::max({std::abs(a.r-b.r),std::abs(a.g-b.g),
                                std::abs(a.b-b.b),std::abs(a.m-b.m)}) <= 2e-5f,
                      "Transformed host blur introduced strip seams");
              require(std::abs(a.m - identity->pixels(y)[x].m) <= 2e-5f,
                      "Transformed host blur changed the source alpha silhouette");
            }
          require(visible,"Transformed host blur produced no visible image");
        }
}
}  // namespace

int main(int argc, char **argv) {
  QCoreApplication app(argc, argv);
  TThread::init();
  TRenderer::initialize();
  QTemporaryDir temp;
  int result = 1;
  try {
    require(temp.isValid(), "Temporary directory unavailable");
    std::cout << "Checking boundary kernel" << std::endl;
    kernelTests();
    renderTests<TPixel32>(); renderTests<TPixel64>(); renderTests<TPixelF>();
    std::cout << "Checking boundary persistence" << std::endl;
    persistenceTests(temp);
    std::cout << "Checking color selection workflow" << std::endl;
    workflowTest();
    keyWorkflowTest<TPixel32>(); keyWorkflowTest<TPixel64>(); keyWorkflowTest<TPixelF>();
    std::cout << "Checking transformed host blur" << std::endl;
    hostTransformTests();
    std::cout << "PASS: boundary math, all controls, precision, support, tiles, cancellation and persistence\n";
    result = 0;
  } catch (const TException &error) {
    std::cerr << QString::fromStdWString(error.getMessage()).toStdString() << '\n';
  } catch (const std::exception &error) { std::cerr << error.what() << '\n'; }
  TThread::shutdown();
  return result;
}
