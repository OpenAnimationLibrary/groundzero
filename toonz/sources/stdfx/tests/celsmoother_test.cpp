#include "../celsmootherfx.cpp"
#include "tparamcontainer.h"
#include "tdoublekeyframe.h"
#include "tmacrofx.h"
#include "tstream.h"
#include "trenderer.h"
#include "tthread.h"

#include <QCoreApplication>
#include <QTemporaryDir>
#include <chrono>
#include <future>
#include <iostream>
#include <stdexcept>

namespace {
void require(bool condition, const char *message) {
  if (!condition) throw std::runtime_error(message);
}
TDoubleParamP number(TFx &fx, const char *name) {
  return TParamP(fx.getParams()->getParam(name));
}
TPixelParamP color(TFx &fx) {
  return TParamP(fx.getParams()->getParam("colorKey"));
}
class Source final : public TStandardRasterFx {
  TRasterP m_image;
  TPoint m_origin;
  bool m_infinite;
  int *m_cancel;
public:
  Source(const TRasterP &image, TPoint origin = TPoint(), bool infinite = false,
         int *cancel = nullptr)
      : m_image(image), m_origin(origin), m_infinite(infinite), m_cancel(cancel) {}
  const TPersistDeclaration *getDeclaration() const override { return nullptr; }
  bool canHandle(const TRenderSettings &, double) override { return true; }
  bool doGetBBox(double, TRectD &box, const TRenderSettings &) override {
    box = m_infinite ? TConsts::infiniteRectD
                    : TRectD(m_origin.x, m_origin.y,
                             m_origin.x + m_image->getLx(),
                             m_origin.y + m_image->getLy());
    return true;
  }
  std::string getAlias(double, const TRenderSettings &) const override {
    return "cel-smoother-fixture";
  }
  void compute(TTile &tile, double, const TRenderSettings &) override {
    tile.getRaster()->clear();
    tile.getRaster()->copy(m_image,
        TPoint(m_origin.x - int(tile.m_pos.x), m_origin.y - int(tile.m_pos.y)));
    if (m_cancel) *m_cancel = 1;
  }
  void doCompute(TTile &tile, double frame, const TRenderSettings &settings) override {
    compute(tile, frame, settings);
  }
};
template <class Pixel>
TRasterPT<Pixel> render(CelSmootherFx &fx, int width, int height,
                       TPointD origin = TPointD(), double frame = 0,
                       TRenderSettings settings = TRenderSettings()) {
  TRasterPT<Pixel> raster(width, height);
  TTile tile;
  tile.setRaster(raster);
  tile.m_pos = origin;
  fx.doCompute(tile, frame, settings);
  return raster;
}
template <class Pixel> void testPrecision() {
  const double max = CelSmootherLocal::maximum<Pixel>();
  TRasterPT<Pixel> input(3, 3);
  input->fill(Pixel(0, 0, 0, max));
  for (int y = 1; y < 3; ++y)
    for (int x = 1; x < 3; ++x) input->pixels(y)[x] = Pixel(max, max, max, max);
  CelSmootherFx fx;
  fx.getInputPort(0)->setFx(new Source(input));
  auto output = render<Pixel>(fx, 3, 3);
  // A two-pixel corner has triangular coverage 3/8 in its first cell and
  // 1/8 in its second. The crossing cell receives both directional passes.
  const auto expected = [max](double v) {
    return max == 1 ? v : std::floor(v * max + .5);
  };
  require(output->pixels(1)[1].r == expected(25.0 / 64), "Corner area is wrong");
  require(output->pixels(1)[2].r == expected(7.0 / 8), "Horizontal area is wrong");
  require(output->pixels(2)[1].r == expected(7.0 / 8), "Vertical area is wrong");
  require(output->pixels(2)[2] == input->pixels(2)[2], "Flat region changed");
  require(output->pixels(1)[1].m == max, "Opaque smoothing lost alpha");
  number(fx, "smoothRange")->setDefaultValue(255);
  output = render<Pixel>(fx, 3, 3);
  for (int y = 0; y < 3; ++y)
    for (int x = 0; x < 3; ++x)
      require(output->pixels(y)[x] == input->pixels(y)[x], "Range bypass changed data");
  number(fx, "smoothRange")->setDefaultValue(0);
  output = render<Pixel>(fx, 3, 3);
  require(output->pixels(1)[1].r == expected(25.0 / 64), "Zero range missed exact flats");
  number(fx, "useColorKey")->setDefaultValue(1);
  output = render<Pixel>(fx, 3, 3);
  require(output->pixels(2)[2] == Pixel::Transparent, "White key was not removed");
  require(output->pixels(1)[1].m == expected(39.0 / 64), "Key was applied after smoothing");
  require(output->pixels(1)[1].r == 0, "Keyed background RGB leaked");
  number(fx, "smoothRange")->setDefaultValue(255);
  const double alpha = max == 1 ? .5 : std::floor(max / 2);
  input->pixels(0)[0] = Pixel(alpha, alpha, alpha, alpha);
  output = render<Pixel>(fx, 3, 3);
  require(output->pixels(0)[0] == Pixel::Transparent, "Partial coverage key failed");
}

void testKernel() {
  using namespace CelSmoothing;
  std::vector<Pixel> source(9, Pixel{0, 0, 0, 1}), output;
  source[4] = source[5] = source[7] = source[8] = {.01, .01, .01, 1};
  require(smooth(3, 3, source, output, 6, 100), "Kernel failed");
  require(output[4].r == .01, "Range did not group a shallow gradient");
  smooth(3, 3, source, output, 0, 100);
  require(std::abs(output[4].r - .01 * 25 / 64) < 1e-15,
          "Sub-8-bit values were quantized");
  source[4] = source[5] = source[7] = source[8] = {4, -2, .12345678, .5};
  source[0] = source[1] = source[2] = source[3] = source[6] = {0, 0, 0, 0};
  smooth(3, 3, source, output, 0, 100);
  require(output[4].r == 4 * 25.0 / 64 && output[4].g == -2 * 25.0 / 64,
          "HDR smoothing clipped values");
  require(output[4].a == .5 * 25.0 / 64, "Premultiplied coverage is wrong");
  require(smooth(0, 0, {}, output, 0, 100) && output.empty(), "Empty kernel failed");
  for (const auto size : {std::pair<int,int>{1,1}, {1,17}, {23,1}, {13,17}}) {
    source.assign(std::size_t(size.first) * size.second, Pixel{.25,.5,.75,1});
    smooth(size.first, size.second, source, output, 0, 100);
    for (const auto &p : output) require(p.r == .25 && p.a == 1, "Flat narrow image changed");
  }
  // Intersections and one-pixel lines retain finite, bounded coverage.
  source.assign(121, Pixel{1,1,1,1});
  for (int y = 0; y < 11; ++y)
    for (int x = 0; x < 11; ++x)
      if (x == y || x + y == 10 || x == 5) source[y * 11 + x] = {0,0,0,1};
  smooth(11, 11, source, output, 0, 100);
  require(output[60].r < .5, "Intersection erased the thin line");
  for (const auto &p : output)
    require(std::isfinite(p.r) && p.r >= 0 && p.r <= 1 && p.a == 1,
            "Thin line generated invalid coverage");
  int canceled = 1;
  require(!smooth(11, 11, source, output, 0, 100, &canceled) && output.empty(),
          "Canceled kernel published data");
  canceled = 0;
  const Mix cancelBlend = [&canceled](const Pixel &a, const Pixel &b, double w) {
    canceled = 1; return mix(a, b, w);
  };
  require(!smooth(11, 11, source, output, 0, 100, &canceled, cancelBlend) && output.empty(),
          "Mid-pass cancellation published partial output");
}

void testTilesAndDomain() {
  TRasterFP input(23, 17);
  for (int y = 0; y < 17; ++y)
    for (int x = 0; x < 23; ++x)
      input->pixels(y)[x] = x > y ? TPixelF(2, -.25f, .12345f, 1)
                                 : TPixelF(0, 0, 0, 1);
  CelSmootherFx fx;
  fx.getInputPort(0)->setFx(new Source(input, TPoint(-7, -5)));
  auto full = render<TPixelF>(fx, 23, 17, TPointD(-7, -5));
  std::vector<std::future<void>> jobs;
  for (int row = 0; row < 17; ++row)
    jobs.push_back(std::async(std::launch::async, [&, row] {
      const auto strip = render<TPixelF>(fx, 29, 1, TPointD(-10, row - 5), row % 3);
      for (int x = 0; x < 29; ++x) {
        const TPixelF expected = x >= 3 && x < 26 ? full->pixels(row)[x - 3]
                                                  : TPixelF::Transparent;
        require(strip->pixels(0)[x] == expected, "Full/strip/thread mismatch");
      }
    }));
  for (auto &job : jobs) job.get();
  TRenderSettings settings, inputSettings;
  TRectD request;
  fx.transform(0, 0, TRectD(0, 0, 1, 1), settings, request, inputSettings);
  require(request == TRectD(-7,-5,16,12), "Predictive request missed full source");
  settings.m_affine = TTranslation(2.5, -3.25) * TRotation(21) * TScale(1.3, .7);
  require(fx.handledAffine(settings, 0) == TAffine(), "Transform is applied before smoothing");
  require(fx.getMemoryRequirement(TRectD(0,0,1,1), 0, TRenderSettings()) > 0,
          "Whole-source memory was not reported");
  fx.getInputPort(0)->setFx(new Source(input, TPoint(-7,-5), true));
  settings = TRenderSettings();
  settings.m_cameraBox = TRectD(-7,-5,16,12);
  auto infinite = render<TPixelF>(fx, 23, 17, TPointD(-7,-5), 0, settings);
  for (int y = 0; y < 17; ++y)
    for (int x = 0; x < 23; ++x)
      require(infinite->pixels(y)[x] == full->pixels(y)[x], "Camera domain changed output");
  int canceled = 0;
  fx.getInputPort(0)->setFx(new Source(input, TPoint(), false, &canceled));
  settings.m_isCanceled = &canceled;
  const auto empty = render<TPixelF>(fx, 23, 17, TPointD(), 0, settings);
  require(empty->pixels(1)[1] == TPixelF::Transparent, "Canceled source escaped");
  CelSmootherFx missing;
  require(render<TPixelF>(missing, 1, 1)->pixels(0)[0] == TPixelF::Transparent,
          "Missing source was not transparent");
}

TRasterFP renderThroughHost(CelSmootherFx &fx, int strips, const TAffine &affine) {
  constexpr int width = 48, height = 40;
  TRenderer renderer(1);
  const unsigned long renderId = TRenderer::buildRenderId();
  renderer.install(renderId);
  renderer.declareRenderStart(renderId);
  renderer.declareFrameStart(0);
  TRasterFP output(width,height);
  output->clear();
  try {
    TRenderSettings settings;
    settings.m_bpp = 128;
    settings.m_affine = affine;
    settings.m_cameraBox = TRectD(-24,-20,24,20);
    for (int s = 0; s < strips; ++s) {
      const int y0 = height * s / strips, y1 = height * (s + 1) / strips;
      TRasterFP part(width,y1-y0);
      part->clear();
      TTile tile(part,TPointD(-24.25,-20.5+y0));
      fx.compute(tile,0,settings);
      output->copy(tile.getRaster(),TPoint(0,y0));
    }
  } catch (...) {
    renderer.declareFrameEnd(0); renderer.declareRenderEnd(renderId);
    renderer.uninstall(); throw;
  }
  renderer.declareFrameEnd(0); renderer.declareRenderEnd(renderId);
  renderer.uninstall();
  return output;
}
void testHostTransforms() {
  TRasterFP input(23,17);
  input->clear();
  for (int y = 2; y < 15; ++y)
    for (int x = y / 2; x < 20; ++x)
      input->pixels(y)[x] = TPixelF(.6f,.2f,.8f,1);
  CelSmootherFx fx;
  fx.getInputPort(0)->setFx(new Source(input,TPoint(-7,-5)));
  const TAffine affine = TTranslation(2.5,-3.25) * TRotation(21) * TScale(1.3,.7);
  const auto full = renderThroughHost(fx,1,affine);
  const auto strips = renderThroughHost(fx,8,affine);
  bool visible = false;
  for (int y = 0; y < full->getLy(); ++y)
    for (int x = 0; x < full->getLx(); ++x) {
      const TPixelF &a = full->pixels(y)[x], &b = strips->pixels(y)[x];
      visible = visible || a.m > .5f;
      require(std::max({std::abs(a.r-b.r),std::abs(a.g-b.g),
                        std::abs(a.b-b.b),std::abs(a.m-b.m)}) <= .000001f,
              "Host rotation, scale or fractional origin created strip seams");
    }
  require(visible,"Transformed rendering produced an empty image");
}

void testPersistence(const QString &path, bool compressed) {
  TFxP owner = new CelSmootherFx;
  auto &fx = *owner;
  fx.setFxId(L"CelSmoother1");
  number(fx, "useColorKey")->setValue(0, 0);
  number(fx, "useColorKey")->setValue(10, 1);
  number(fx, "smoothRange")->setValue(0, 0);
  number(fx, "smoothRange")->setValue(10, 255);
  color(fx)->setValueD(0, TPixelD(.123456789123, 2.5, -.3, 1));
  color(fx)->setValueD(10, TPixelD(.234567891234, 3.5, -.4, 1));
  TRenderSettings settings;
  const auto before = fx.getAlias(0, settings);
  color(fx)->getRed()->setValue(0, .123456789124);
  require(fx.getAlias(0, settings) != before, "Precise key shares cache alias");
  TFxP clone = fx.clone(false);
  require(color(*clone)->getValueD(0) == color(fx)->getValueD(0), "Clone lost color precision");
  require(number(*clone,"useColorKey")->isUICheckbox(), "Clone lost checkbox editor");
  { TOStream stream(TFilePath(path.toStdWString()), compressed); stream << owner; }
  TFxP loaded;
  { TIStream stream(TFilePath(path.toStdWString())); stream >> loaded; }
  require(dynamic_cast<CelSmootherFx *>(loaded.getPointer()), "Factory failed");
  require(color(*loaded)->getValueD(10) == color(fx)->getValueD(10), "Scene lost precise key");
  require(number(*loaded,"useColorKey")->getValue(10) == 1 &&
              number(*loaded,"smoothRange")->getValue(10) == 255,
          "Scene lost animated controls");
  { TOStream stream(TFilePath((path + ".preset").toStdWString())); fx.savePreset(stream); }
  CelSmootherFx preset;
  { TIStream stream(TFilePath((path + ".preset").toStdWString())); preset.loadPreset(stream); }
  require(color(preset)->getValueD(10) == color(fx)->getValueD(10), "Preset lost key");
  TFxP second = new CelSmootherFx;
  second->setFxId(L"CelSmoother2");
  second->getInputPort(0)->setFx(owner.getPointer());
  TFxP macroOwner = TMacroFx::create({owner,second});
  auto *macro = dynamic_cast<TMacroFx *>(macroOwner.getPointer());
  require(macro, "Macro creation failed");
  for (const char *name : {"useColorKey","colorKey","smoothRange"})
    macro->setParamExposed(owner.getPointer(), name, true);
  { TOStream stream(TFilePath((path + ".macro").toStdWString())); stream << macroOwner; }
  TFxP macroLoaded;
  { TIStream stream(TFilePath((path + ".macro").toStdWString())); stream >> macroLoaded; }
  auto *restored = dynamic_cast<TMacroFx *>(macroLoaded.getPointer());
  require(restored, "Macro reload failed");
  TFx *member = restored->getFxById(L"CelSmoother1");
  require(member, "Macro member missing");
  for (const char *name : {"useColorKey","colorKey","smoothRange"})
    require(restored->isParamExposed(member,name), "Macro exposure lost");
}
void reportCost() {
  constexpr int width = 1920, height = 1080;
  std::vector<CelSmoothing::Pixel> input(std::size_t(width) * height), output;
  for (int y = 0; y < height; ++y)
    for (int x = 0; x < width; ++x)
      input[std::size_t(y) * width + x] = x > y ? CelSmoothing::Pixel{1,1,1,1}
                                               : CelSmoothing::Pixel{0,0,0,1};
  const auto start = std::chrono::steady_clock::now();
  require(CelSmoothing::smooth(width,height,input,output,6,100), "Large image failed");
  const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::steady_clock::now() - start).count();
  std::cout << "1080p kernel: " << ms << " ms, working buffers "
            << (input.capacity() + output.capacity()) * sizeof(CelSmoothing::Pixel)
            << " bytes\n";
}
}  // namespace
int main(int argc, char **argv) {
  QCoreApplication app(argc, argv);
  TThread::init();
  QTemporaryDir temp;
  int result = 1;
  try {
    require(temp.isValid(), "Temporary storage unavailable");
    testPrecision<TPixel32>(); testPrecision<TPixel64>(); testPrecision<TPixelF>();
    testKernel(); testTilesAndDomain(); testHostTransforms();
    testPersistence(temp.filePath("smoother.fx"), false);
    testPersistence(temp.filePath("smoother-compressed.fx"), true);
    reportCost();
    std::cout << "PASS: smoothing precision, coverage, tiles, animation and persistence\n";
    result = 0;
  } catch (const TException &e) {
    std::cerr << QString::fromStdWString(e.getMessage()).toStdString() << '\n';
  } catch (const std::exception &e) { std::cerr << e.what() << '\n'; }
  TThread::shutdown();
  return result;
}
