#include "../celsmootherfx.cpp"
#include "celcamerabox.h"
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
  TPointD m_origin;
  bool m_infinite;
  int *m_cancel;
  bool m_partial;
  bool m_transform;
public:
  Source(const TRasterP &image, TPoint origin = TPoint(), bool infinite = false,
         int *cancel = nullptr, bool partial = false, bool transform = false)
      : Source(image,convert(origin),infinite,cancel,partial,transform) {}
  Source(const TRasterP &image, TPointD origin, bool infinite,
         int *cancel, bool partial, bool transform)
      : m_image(image), m_origin(origin), m_infinite(infinite), m_cancel(cancel),
        m_partial(partial), m_transform(transform) {}
  const TPersistDeclaration *getDeclaration() const override { return nullptr; }
  bool canHandle(const TRenderSettings &, double) override { return true; }
  TAffine handledAffine(const TRenderSettings &, double) override {
    return TTranslation(-m_origin.x-std::floor(-m_origin.x),
                        -m_origin.y-std::floor(-m_origin.y));
  }
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
  void compute(TTile &tile, double, const TRenderSettings &settings) override {
    if (m_partial) {
      const TRasterP raster = tile.getRaster();
      for (int y = 0; y < raster->getLy(); ++y) {
        const UCHAR *row = raster->getRawData() +
            std::size_t(y) * raster->getWrap() * raster->getPixelSize();
        require(std::all_of(row, row + raster->getLx() * raster->getPixelSize(),
                            [](UCHAR c) { return c == 0; }),
                "Upstream received uninitialized source storage");
      }
      TRect sample(2,2,2,2);
      raster->copy(m_image->extract(sample),
                   TPoint(2-int(tile.m_pos.x),2-int(tile.m_pos.y)));
      return;
    }
    tile.getRaster()->clear();
    if (m_transform) {
      TTile source(m_image, m_origin);
      TRasterFx::applyAffine(tile, source, settings);
      return;
    }
    tile.getRaster()->copy(m_image,
        TPoint(int(m_origin.x) - int(tile.m_pos.x), int(m_origin.y) - int(tile.m_pos.y)));
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
  color(fx)->setDefaultValue(TPixel32(64,128,192,255));
  const TPixelD coloredKey = color(fx)->getValueD(0);
  Pixel colored = CelSmootherLocal::encode<Pixel>(
      {coloredKey.r,coloredKey.g,coloredKey.b,1});
  colored.m = alpha;
  colored = premultiply(colored);
  input->pixels(0)[0] = colored;
  input->pixels(0)[1] = Pixel(alpha,0,0,alpha);
  output = render<Pixel>(fx,3,3);
  require(output->pixels(0)[0] == Pixel::Transparent &&
              output->pixels(0)[1] == input->pixels(0)[1],
          "Colored partial-coverage key removed the wrong region");
}

template <class Pixel> void testPartialSource() {
  TRasterPT<Pixel> source(5,5);
  source->fill(Pixel::White);
  CelSmootherFx fx;
  number(fx,"smoothRange")->setDefaultValue(255);
  fx.getInputPort(0)->setFx(new Source(source,TPoint(),false,nullptr,true));
  const auto output = render<Pixel>(fx,5,5);
  for (int y = 0; y < 5; ++y)
    for (int x = 0; x < 5; ++x)
      require(output->pixels(y)[x] == (x == 2 && y == 2 ? Pixel::White : Pixel::Transparent),
              "Partially rendered source leaked uninitialized pixels");
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
  source.assign(9, Pixel{244.0/255,244.0/255,244.0/255,1});
  source[4] = source[5] = source[7] = source[8] = {250.0/255,250.0/255,250.0/255,1};
  smooth(3,3,source,output,6,100);
  require(output[4].r == 250.0/255,"Inclusive integer range rounded away a boundary");
  for (int y = 0; y < 3; ++y)
    for (int x = 0; x < 3; ++x) {
      const double value = .1 + .01 * x + (x > 0 && y > 0 ? .7 : 0);
      source[y*3+x] = {value,value,value,1};
    }
  smooth(3,3,source,output,6,100);
  // Neighboring gradient samples group into the same contour run while
  // retaining their actual values during the two triangular coverage blends.
  const double gradientCorner = (.81 * .625 + .11 * .375) * .625 + .1 * .375;
  require(std::abs(output[4].r-gradientCorner) < 1e-14,
          "Range failed to smooth a high-contrast contour over a gradient");
  require(output[0].r == source[0].r && output[2].r == source[2].r,
          "Smoothing flattened an untouched gradient region");
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

void testAllocationBounds() {
  // Exercise the exact signed-int product boundary without allocating images.
  for (int bytes : {4,8,16}) {
    const int last = std::numeric_limits<int>::max() / bytes;
    CelSmootherLocal::checkInputSize(last,1,bytes);
    bool rejected = false;
    try { CelSmootherLocal::checkInputSize(last+1,1,bytes); }
    catch (const TException &) { rejected = true; }
    require(rejected,"Raster product overflow was not rejected before allocation");
  }
  CelSmootherLocal::checkInputSize(16383,8192,16);
  bool rejected = false;
  try { CelSmootherLocal::checkInputSize(16384,8192,16); }
  catch (const TException &) { rejected = true; }
  require(rejected,"Two-dimensional raster byte overflow was not rejected");
  rejected = false;
  try { CelSmootherLocal::checkInputSize(1,1,0); }
  catch (const TException &) { rejected = true; }
  require(rejected,"Invalid pixel size reached allocation");
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

TRasterFP renderThroughHost(TRasterFx &fx, int strips, const TAffine &affine,
                           bool linear = false, double frame = 0,
                           TPointD origin = TPointD(-24.25,-20.5)) {
  constexpr int width = 48, height = 40;
  TRenderer renderer(1);
  const unsigned long renderId = TRenderer::buildRenderId();
  renderer.install(renderId);
  renderer.declareRenderStart(renderId);
  renderer.declareFrameStart(frame);
  TRasterFP output(width,height);
  output->setLinear(linear);
  output->clear();
  try {
    TRenderSettings settings;
    settings.m_bpp = 128;
    settings.m_linearColorSpace = linear;
    settings.m_affine = affine;
    settings.m_cameraBox = TRectD(-24,-20,24,20);
    for (int s = 0; s < strips; ++s) {
      const int y0 = height * s / strips, y1 = height * (s + 1) / strips;
      TRasterFP part(width,y1-y0);
      part->setLinear(linear);
      part->clear();
      TTile tile(part,TPointD(origin.x,origin.y+y0));
      fx.compute(tile,frame,settings);
      output->copy(tile.getRaster(),TPoint(0,y0));
    }
  } catch (...) {
    renderer.declareFrameEnd(frame); renderer.declareRenderEnd(renderId);
    renderer.uninstall(); throw;
  }
  renderer.declareFrameEnd(frame); renderer.declareRenderEnd(renderId);
  renderer.uninstall();
  return output;
}
void testHostTransforms() {
  TRasterFP input(23,17);
  input->clear();
  for (int y = 2; y < 15; ++y)
    for (int x = y / 2; x < 20; ++x)
      input->pixels(y)[x] = TPixelF(.6f,.2f,.8f,1);
  // The production cache retains smart references to effects. Match scene
  // ownership here rather than allowing it to acquire a stack object.
  TFxP owner = new CelSmootherFx;
  auto &fx = *static_cast<CelSmootherFx *>(owner.getPointer());
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

void sameImage(const TRasterFP &a, const TRasterFP &b, const char *message) {
  require(a->getSize() == b->getSize(), "Image dimensions differ");
  for (int y = 0; y < a->getLy(); ++y)
    for (int x = 0; x < a->getLx(); ++x) {
      const TPixelF &p = a->pixels(y)[x], &q = b->pixels(y)[x];
      require(std::max({std::abs(p.r-q.r),std::abs(p.g-q.g),
                        std::abs(p.b-q.b),std::abs(p.m-q.m)}) <= .00001f,
              message);
    }
}

void testLevelDpi() {
  // Scene construction places this actual geometry wrapper between native
  // raster pixels and ordinary image effects. A camera zoom must not undo
  // sampling damage from smoothing a DPI-reduced intermediate image.
  TRasterFP input(24,20);
  input->clear();
  for (int y = 3; y < 17; ++y)
    for (int x = 3; x < 21; ++x)
      input->pixels(y)[x] = x > 5 + y / 2 ? TPixelF(.8f,.2f,.6f,1)
                                         : TPixelF(.05f,.05f,.05f,1);
  TFxP referenceOwner = new CelSmootherFx;
  auto &reference = *static_cast<CelSmootherFx *>(referenceOwner.getPointer());
  reference.getInputPort(0)->setFx(
      new Source(input,TPoint(-12,-10),false,nullptr,false,true));
  const auto native = renderThroughHost(reference,1,TAffine(),false,0,TPointD(-24,-20));
  number(reference,"smoothRange")->setDefaultValue(255);
  const auto bypass = renderThroughHost(reference,1,TAffine(),false,0,TPointD(-24,-20));
  number(reference,"smoothRange")->setDefaultValue(6);
  bool filtered = false;
  for (int y = 0; y < native->getLy(); ++y)
    for (int x = 0; x < native->getLx(); ++x)
      filtered = filtered || std::abs(native->pixels(y)[x].r-bypass->pixels(y)[x].r) > .001f;
  require(filtered,"Level DPI fixture did not contain a smoothed contour");
  TFxP actualOwner = new CelSmootherFx;
  auto &actual = *static_cast<CelSmootherFx *>(actualOwner.getPointer());
  TFxP dpiOwner = new NaAffineFx;
  auto *dpi = static_cast<NaAffineFx *>(dpiOwner.getPointer());
  dpi->setName(L"LevelColumn AffineFx");
  dpi->getInputPort(0)->setFx(
      new Source(input,TPoint(-12,-10),false,nullptr,false,true));
  TFxP placementOwner = new NaAffineFx;
  auto *placement = static_cast<NaAffineFx *>(placementOwner.getPointer());
  placement->getInputPort(0)->setFx(dpi);
  actual.getInputPort(0)->setFx(placement);

  for (const TAffine &scale : {TAffine(TScale(.25)), TAffine(TScale(.25,.5))}) {
    dpi->setAffine(scale);
    for (const TAffine &position : {
             TAffine(TTranslation(.25,-.75)),
             TTranslation(2.25,-3.75) * TRotation(17)}) {
      placement->setAffine(position);
      const TAffine world = position * scale;
      const TAffine grid = actual.handledAffine(TRenderSettings(),0);
      require(grid != TAffine(), "Level DPI was ignored");
      require(grid.a13 >= 0 && grid.a13 < 1 && grid.a23 >= 0 && grid.a23 < 1,
              "Native grid translation was not canonicalized");
      TRenderSettings settings, sourceSettings;
      TRectD request;
      actual.transform(0,0,TRectD(0,0,1,1),settings,request,sourceSettings);
      require(sourceSettings.m_affine == grid && request.getLx() >= 24 &&
                  request.getLy() >= 20,
              "Predictive DPI request did not retain native source resolution");
      for (bool linear : {false,true})
        for (const TAffine &camera : {TAffine(), TAffine(TScale(4))}) {
          // Integer tile origins exercise identity-affine host routing directly.
          const TPointD origin(-24,-20);
          const auto expected = renderThroughHost(reference,1,camera*world,linear,0,origin);
          const auto full = renderThroughHost(actual,1,camera,linear,0,origin);
          sameImage(full,expected,"Level DPI changed native contour smoothing");
          const auto strips = renderThroughHost(actual,8,camera,linear,0,origin);
          sameImage(full,strips,"DPI rendering introduced strip seams");
        }
    }
  }
  const std::string alias = actual.getAlias(0,TRenderSettings());
  dpi->setAffine(TScale(.250000000001,.5));
  require(actual.getAlias(0,TRenderSettings()) != alias,
          "Fine level-grid changes reused the cached contour result");
  // A generic affine is not proof of a raster-level pixel grid. Preserve the
  // existing identity-grid semantics when the native scene marker is absent.
  dpi->setName(L"Unmarked geometry");
  require(actual.handledAffine(TRenderSettings(),0) == TAffine(),
          "Generic source geometry unexpectedly selected a native level grid");
  // With identity DPI the scene builder names the level source directly and
  // omits the otherwise redundant affine wrapper.
  TFxP identitySource = new Source(input,TPoint(-12,-10),false,nullptr,false,true);
  identitySource->setName(L"LevelColumn AffineFx");
  placement->getInputPort(0)->setFx(identitySource.getPointer());
  for (bool linear : {false,true}) {
    const TPointD origin(-24,-20);
    const auto expected = renderThroughHost(reference,1,placement->getPlacement(0),
                                            linear,0,origin);
    const auto full = renderThroughHost(actual,1,TAffine(),linear,0,origin);
    sameImage(full,expected,"Identity-DPI level placement lost the native grid");
  }
}

void testOddLevelPhase() {
  TRasterFP input(23,17);
  input->clear();
  for (int y = 3; y < 14; ++y)
    for (int x = 3; x < 20; ++x)
      input->pixels(y)[x] = x > 5+y/2 ? TPixelF(.8f,.2f,.6f,1)
                                      : TPixelF(.05f,.05f,.05f,1);
  const TPointD sourceOrigin(-11.5,-8.5);
  const TPointD outputOrigin(-24,-20);
  TFxP referenceOwner = new CelSmootherFx;
  auto &reference = *static_cast<CelSmootherFx *>(referenceOwner.getPointer());
  reference.getInputPort(0)->setFx(
      new Source(input,TPoint(),false,nullptr,false,true));
  TFxP actualOwner = new CelSmootherFx;
  auto &actual = *static_cast<CelSmootherFx *>(actualOwner.getPointer());
  TFxP nativeSource = new Source(input,sourceOrigin,false,nullptr,false,true);
  nativeSource->setName(L"LevelColumn AffineFx");
  TFxP dpiOwner = new NaAffineFx;
  auto *dpi = static_cast<NaAffineFx *>(dpiOwner.getPointer());
  dpi->setName(L"LevelColumn AffineFx");
  dpi->getInputPort(0)->setFx(nativeSource.getPointer());
  TFxP placementOwner = new NaAffineFx;
  auto *placement = static_cast<NaAffineFx *>(placementOwner.getPointer());
  const TAffine position = TTranslation(2.25,-3.75) * TRotation(17);
  placement->setAffine(position);
  actual.getInputPort(0)->setFx(placement);
  for (const TAffine &scale : {TAffine(), TAffine(TScale(.25)), TAffine(TScale(.25,.5))}) {
    if (scale.isIdentity()) placement->getInputPort(0)->setFx(nativeSource.getPointer());
    else {
      dpi->setAffine(scale);
      placement->getInputPort(0)->setFx(dpi);
    }
    for (bool linear : {false,true})
      for (const TAffine &camera : {TAffine(), TAffine(TScale(4))}) {
        // The reference filters integer-origin samples first. Shifting an
        // already resampled reference would conceal a half-pixel phase defect.
        const TAffine expectedPlacement = camera * position * scale * TTranslation(sourceOrigin);
        const auto expected = renderThroughHost(reference,1,expectedPlacement,linear,0,outputOrigin);
        const auto full = renderThroughHost(actual,1,camera,linear,0,outputOrigin);
        sameImage(full,expected,"Odd-sized level was resampled before smoothing");
        const auto strips = renderThroughHost(actual,8,camera,linear,0,outputOrigin);
        sameImage(full,strips,"Odd-sized level rendering introduced strip seams");
      }
  }
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
  { TOStream stream(TFilePath(path.toStdWString()), compressed); stream << owner.getPointer(); }
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
  { TOStream stream(TFilePath((path + ".macro").toStdWString())); stream << macroOwner.getPointer(); }
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
    std::cout << "Checking all raster precisions and keyed coverage" << std::endl;
    testPrecision<TPixel32>(); testPrecision<TPixel64>(); testPrecision<TPixelF>();
    std::cout << "Checking partial upstream rendering" << std::endl;
    testPartialSource<TPixel32>(); testPartialSource<TPixel64>(); testPartialSource<TPixelF>();
    std::cout << "Checking contour math, allocation boundaries and concurrent tiles" << std::endl;
    testKernel(); testAllocationBounds(); testTilesAndDomain();
    std::cout << "Checking transformed host rendering" << std::endl;
    testHostTransforms();
    std::cout << "Checking native level DPI and host color conversion" << std::endl;
    testLevelDpi(); testOddLevelPhase();
    std::cout << "Checking scene, preset and macro persistence" << std::endl;
    testPersistence(temp.filePath("smoother.fx"), false);
    testPersistence(temp.filePath("smoother-compressed.fx"), true);
    reportCost();
    {
      TFxP fx = new CelSmootherFx;
      CelCameraBoxTest::checkInfiniteSourceCoversCamera(
          static_cast<TRasterFx &>(*fx), "Cel Smoother");
    }
    std::cout << "PASS: smoothing precision, coverage, tiles, animation and persistence\n";
    result = 0;
  } catch (const TException &e) {
    std::cerr << QString::fromStdWString(e.getMessage()).toStdString() << '\n';
  } catch (const std::exception &e) { std::cerr << e.what() << '\n'; }
  TThread::shutdown();
  return result;
}
