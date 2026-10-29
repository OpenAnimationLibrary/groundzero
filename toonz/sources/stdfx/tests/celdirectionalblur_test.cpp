#include "../celdirectionalblurfx.cpp"
#include "celcamerabox.h"
#include "tmacrofx.h"
#include "tparamcontainer.h"
#include "tstream.h"
#include "celmotion_host_fixture.h"
#include "tfxutil.h"
#include "toonz/toonzscene.h"
#include "toonz/txshcell.h"
#include "toonz/txshlevelcolumn.h"
#include "toonz/txshleveltypes.h"
#include "toonz/txshsimplelevel.h"
#include "trasterimage.h"

#include <QCoreApplication>
#include <QTemporaryDir>
#include <chrono>
#include <future>
#include <iostream>
#include <stdexcept>

namespace {
void require(bool value, const char *message) {
  if (!value) throw std::runtime_error(message);
}
TDoubleParamP param(TFx &fx, const std::string &name) {
  return TParamP(fx.getParams()->getParam(name));
}
void value(TFx &fx, const std::string &name, double number) { param(fx, name)->setDefaultValue(number); }
CelMotion::Image image(int width, int height) {
  CelMotion::Image result; result.width = width; result.height = height;
  result.pixels.resize(std::size_t(width) * height); return result;
}
CelMotion::Image motion(const CelMotion::Image &input, CelDirectional::Settings settings,
                       int width = 12, int height = 8, const CelMotion::Image *noise = nullptr) {
  CelMotion::Shapes shapes;
  require(CelMotion::analyze(input, shapes), "Shape analysis canceled");
  auto output = image(width, height); output.originX = -2;
  require(CelDirectional::render(input, shapes, noise, output, settings), "Motion render canceled");
  return output;
}

void mathTests() {
  auto input = image(1, 1); input.pixels[0] = {{0.5, 0, 0, 0.5}};
  CelDirectional::Settings settings; settings.front.strength = 2; settings.back.strength = 1;
  settings.gain = 2;
  auto output = motion(input, settings);
  // Independent closed-form single-pixel trajectory: center occurs once,
  // front weights are 2/3 and 1/3, and the back weight is 1/2.
  require(std::abs(output.pixels[2][3] - 0.5) < 1e-12 &&
              std::abs(output.pixels[3][3] - 1.0 / 3) < 1e-12 &&
              std::abs(output.pixels[4][3] - 1.0 / 6) < 1e-12 &&
              std::abs(output.pixels[1][3] - 0.25) < 1e-12,
          "Asymmetric single-pixel tail disagrees with analytical weights");
  require(output.pixels[0] == CelMotion::Pixel{} &&
              std::abs(output.pixels[3][0] - 2.0 / 3) < 1e-12,
          "Gain or trajectory support is wrong");
  settings.back.strength = 0; settings.front.edgeFade = 1;
  auto faded = motion(input, settings);
  require(std::abs(faded.pixels[2][3] - 0.25) < 1e-12, "Opposite-edge fade not applied");
  settings.front.edgeFade = 0; settings.noiseVariation = 1; settings.noise.type = CelNoise::Layer;
  const auto missing = motion(input, settings);
  require(missing.pixels[3][3] == 0 && missing.pixels[2][3] == 0.5,
          "Missing noise layer is not black");
  auto layer = image(1, 1); layer.pixels[0] = {{1, 1, 1, 1}};
  const auto white = motion(input, settings, 12, 8, &layer);
  require(std::abs(white.pixels[3][3] - 1.0 / 3) < 1e-12, "White layer did not retain full strength");
  layer.pixels[0] = {{1, 1, 1, 0}};
  const auto hidden = motion(input, settings, 12, 8, &layer);
  require(hidden.pixels[3][3] == 0, "Transparent noise RGB affected the blur");

  auto shapesInput = image(3, 7);
  shapesInput.pixels[0] = {{1, 0, 0, 1}};
  for (int y = 4; y < 7; ++y) shapesInput.pixels[y * 3] = {{0, 1, 0, 1}};
  CelMotion::Shapes shapes;
  require(CelMotion::analyze(shapesInput, shapes) && shapes.groups.size() == 2,
          "Separated shape groups were not detected");
  require(CelMotion::sizeFactor(shapes.groups[0], shapes, 1) == 0 &&
              CelMotion::sizeFactor(shapes.groups[1], shapes, 1) == 1,
          "Size variation did not remove the smallest group's blur");
  settings = CelDirectional::Settings(); settings.front.strength = 4; settings.sizeVariation = 1;
  const auto varied = motion(shapesInput, settings);
  require(varied.pixels[4][3] == 0 && varied.pixels[5 * 12 + 4][3] > 0,
          "Group-sized strength was not applied");
  settings.front.sharpTail = 1;
  const auto sharp = motion(shapesInput, settings);
  require(sharp.pixels[4 * 12 + 4][3] == 0 && sharp.pixels[5 * 12 + 4][3] > 0,
          "Sharp tails did not taper the component's transverse edges");
  auto diagonal = image(2, 2);
  diagonal.pixels[0] = diagonal.pixels[3] = {{1, 0, 0, 1}};
  require(CelMotion::analyze(diagonal, shapes) && shapes.groups.size() == 1 && shapes.groups[0].count == 2,
          "Diagonal or overlapping coverage must form one eight-connected group");
  int canceled = 1;
  require(!CelMotion::analyze(input, shapes, &canceled), "Shape cancellation ignored");
  require(CelMotion::checkedArea(4096, 4096) == std::size_t(4096) * 4096,
          "4K square canonical image rejected");
  require(directionalMemoryMB(TRectD(0, 0, 4096, 4096), 160) == 2560 &&
              directionalMemoryMB(TRectD(0, 0, 1e12, 1e12), 160) == std::numeric_limits<int>::max(),
          "Canonical memory estimate overflowed before allocation");
  bool rejected = false;
  try { CelMotion::checkedArea(134217728, 1); }
  catch (const std::runtime_error &) { rejected = true; }
  require(rejected, "Signed-int host allocation boundary accepted");

  // The documented maximum is exercised with both long tails. Record time
  // without using machine-dependent thresholds as correctness assertions.
  settings = CelDirectional::Settings(); settings.front.strength = settings.back.strength = 4000;
  auto longOutput = image(8003, 1); longOutput.originX = -4001;
  CelMotion::analyze(input, shapes);
  const auto start = std::chrono::steady_clock::now();
  require(CelDirectional::render(input, shapes, nullptr, longOutput, settings), "Maximum-length render failed");
  require(longOutput.pixels[1][3] > 0 && longOutput.pixels[8001][3] > 0 &&
              longOutput.pixels[0][3] == 0 && longOutput.pixels[8002][3] == 0,
          "Maximum strength support was truncated");
  std::cout << "Maximum two-sided trajectory milliseconds: "
            << std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count() << '\n';
}

void noiseTests() {
  CelNoise::Settings settings; settings.seed = 7; settings.type = CelNoise::Block;
  const double a = CelNoise::sample(settings, 1, 1);
  require(a == CelNoise::sample(settings, 9, 9), "Block noise varies within a block");
  settings.seed = 8;
  require(a != CelNoise::sample(settings, 1, 1), "Seed does not change the noise");
  settings.type = CelNoise::Smooth;
  require(CelNoise::sample(settings, 1, 1) != CelNoise::sample(settings, 9, 9),
          "Smooth noise does not interpolate spatially");
  const double base = CelNoise::sample(settings, -17.25, 12.75);
  settings.offset = 0.00001;
  require(std::abs(base - CelNoise::sample(settings, -17.25, 12.75)) < 1e-7,
          "Animated noise offset is discontinuous");
  settings.offset = 2.5;
  const double later = CelNoise::sample(settings, -17.25, 12.75);
  settings.offset = 0;
  require(base == CelNoise::sample(settings, -17.25, 12.75), "Noise depends on frame evaluation order");
  settings.offset = 2.5;
  require(later == CelNoise::sample(settings, -17.25, 12.75), "Noise is not repeatable");
  settings.thickness = 100;
  require(later != CelNoise::sample(settings, -17.25, 12.75), "Thickness has no effect");
  settings.type = CelNoise::Layer;
  require(CelNoise::sample(settings, 0, 0, -1) == 0 && CelNoise::sample(settings, 0, 0, 2) == 1,
          "Layer intensity is not bounded");
}

void productionTransformTests() {
  HostMotionContext context;
  for (const TAffine &affine : {TAffine(), TAffine(TScale(2.0, 1.0))}) {
    auto run = [&](int noiseMode, bool strips) {
      TFxP owner = new CelDirectionalBlurFx;
      auto *fx = dynamic_cast<CelDirectionalBlurFx *>(owner.getPointer());
      fx->getInputPort(0)->setFx(new HostMotionPixel(TPointD(2, 2)));
      if (noiseMode == 0)
        fx->getInputPort(1)->setFx(new HostMotionShift(new HostMotionPixel(TPointD(1, 2)), TPointD(1, 0)));
      else
        fx->getInputPort(1)->setFx(new HostMotionPixel(TPointD(2, 2), noiseMode == 1));
      value(*fx, "frontStrength", 4); value(*fx, "noiseType", 2); value(*fx, "noiseVariation", 100);
      TRenderSettings info; info.m_affine = affine;
      TRaster32P output(24, 16); output->clear();
      if (!strips) {
        TTile tile(output, TPointD(-4, -4)); fx->compute(tile, 0, info);
      } else {
        for (int y = 0; y < 16; ++y) {
          TRaster32P row(24, 1); row->clear();
          TTile tile(row, TPointD(-4, y - 4)); fx->compute(tile, 0, info);
          output->copy(row, TPoint(0, y));
        }
      }
      return output;
    };
    auto shifted = run(0, false), direct = run(1, false), black = run(2, false);
    auto strips = run(0, true);
    int shiftedAlpha = 0, blackAlpha = 0;
    for (int y = 0; y < 16; ++y) for (int x = 0; x < 24; ++x) {
      require(shifted->pixels(y)[x] == direct->pixels(y)[x],
              "Transformed noise does not match a pretranslated layer");
      require(shifted->pixels(y)[x] == strips->pixels(y)[x],
              "Host affine changed output between frame and strips");
      shiftedAlpha += shifted->pixels(y)[x].m; blackAlpha += black->pixels(y)[x].m;
    }
    require(shiftedAlpha > blackAlpha, "Transformed bright noise did not produce a directional trail");
  }
}

void rasterLevelDpiTests() {
  HostMotionContext context;
  for (int width : {8, 9}) {
    const int height = width == 8 ? 8 : 7;
    ToonzScene scene;
    TXshLevelP level = new TXshSimpleLevel(L"directional-dpi-fixture");
    auto *simple = level->getSimpleLevel();
    simple->setType(OVL_XSHLEVEL); simple->setScene(&scene);
    TRaster32P raster(width, height); raster->clear();
    const int px = width / 2 - 1, py = height / 2;
    raster->pixels(py)[px] = TPixel32::White;
    simple->setFrame(TFrameId(1), TRasterImageP(raster));
    TXshLevelColumn column;
    require(column.setCell(0, TXshCell(level, TFrameId(1))), "Cannot create directional raster level");
    CelMotion::Image source = image(width, height), output = image(width, height);
    source.pixels[size_t(py) * width + px] = {{1, 1, 1, 1}};
    CelMotion::Shapes shapes; require(CelMotion::analyze(source, shapes), "Cannot analyze directional level");
    CelDirectional::Settings values; values.front.strength = 2;
    require(CelDirectional::render(source, shapes, nullptr, output, values), "Cannot render native direction");
    TRaster32P expectedNative(width, height);
    for (int y = 0; y < height; ++y) for (int x = 0; x < width; ++x) {
      const int alpha = int(std::round(output.pixels[size_t(y) * width + x][3] * 255));
      expectedNative->pixels(y)[x] = TPixel32(alpha, alpha, alpha, alpha);
    }
    const int resultWidth = width + width % 2, resultHeight = height + height % 2;
    const TPointD origin(std::floor(-0.5 * width), std::floor(-0.5 * height));
    TRaster32P expected(resultWidth, resultHeight); expected->clear();
    TTile native(expectedNative, TPointD(-0.5 * width, -0.5 * height));
    TTile expectedTile(expected, origin); TRasterFx::applyAffine(expectedTile, native, TRenderSettings());
    for (const TAffine &placement : {TAffine(TScale(.25)), TAffine(TScale(.25, .5))}) {
      TFxP dpi = TFxUtil::makeAffine(TFxP(column.getFx()), placement);
      dpi->setName(L"LevelColumn AffineFx");
      TFxP owner = new CelDirectionalBlurFx;
      auto &fx = *static_cast<CelDirectionalBlurFx *>(owner.getPointer());
      fx.getInputPort(0)->setFx(dpi.getPointer()); value(fx, "frontStrength", 2);
      TRenderSettings info; info.m_affine = placement.inv();
      TRaster32P actual(resultWidth, resultHeight); actual->clear(); TTile tile(actual, origin);
      fx.compute(tile, 0, info);
      for (int y = 0; y < resultHeight; ++y) for (int x = 0; x < resultWidth; ++x) {
        const auto &a = actual->pixels(y)[x], &b = expected->pixels(y)[x];
        require(std::abs(int(a.m) - int(b.m)) <= 2 && std::abs(int(a.r) - int(b.r)) <= 2,
                "Directional blur changed source-pixel strength across raster DPI or odd canvas");
      }
    }
  }
}

class MotionSource final : public TStandardRasterFx {
  TRasterP m_image;
  TPointD m_origin;
  bool m_clear;
  int *m_cancel;
public:
  TRectD dryRequest;
  MotionSource(TRasterP raster, TPointD origin = TPointD(), bool clear = true, int *cancel = nullptr)
      : m_image(raster), m_origin(origin), m_clear(clear), m_cancel(cancel) {}
  const TPersistDeclaration *getDeclaration() const override { return nullptr; }
  bool canHandle(const TRenderSettings &, double) override { return true; }
  bool doGetBBox(double, TRectD &box, const TRenderSettings &) override {
    box = TRectD(m_origin, TDimensionD(m_image->getLx() + (m_clear ? 0 : 2), m_image->getLy() + (m_clear ? 0 : 2)));
    return true;
  }
  std::string getAlias(double, const TRenderSettings &) const override { return "directional-source"; }
  void compute(TTile &tile, double, const TRenderSettings &) override {
    if (m_clear) tile.getRaster()->clear();
    tile.getRaster()->copy(m_image, TPoint(int(m_origin.x - tile.m_pos.x), int(m_origin.y - tile.m_pos.y)));
    if (m_cancel) *m_cancel = 1;
  }
  void doCompute(TTile &tile, double frame, const TRenderSettings &info) override { compute(tile, frame, info); }
  void dryCompute(TRectD &rect, double, const TRenderSettings &) override { dryRequest = rect; }
};
template <class Pixel>
TRasterPT<Pixel> render(CelDirectionalBlurFx &fx, int width, int height,
    TPointD origin = TPointD(), double frame = 0, TRenderSettings info = TRenderSettings()) {
  TRasterPT<Pixel> result(width, height);
  TTile tile; tile.setRaster(result); tile.m_pos = origin;
  fx.doCompute(tile, frame, info); return result;
}
template <class Pixel>
void rendererTests() {
  const double scale = directionalChannelScale<Pixel>();
  TRasterPT<Pixel> source(7, 6), layer(7, 6);
  source->clear(); layer->fill(Pixel::White);
  for (int y = 1; y < 5; ++y)
    for (int x = 1; x < 6; ++x)
      if ((x + y) % 3) source->pixels(y)[x] = Pixel(scale, 0, 0, scale);
  CelDirectionalBlurFx fx;
  const TPointD sourceOrigin(-3, -2);
  auto *input = new MotionSource(source, sourceOrigin);
  fx.getInputPort(0)->setFx(input);
  auto identity = render<Pixel>(fx, 7, 6, sourceOrigin);
  for (int y = 0; y < 6; ++y)
    for (int x = 0; x < 7; ++x)
      require(identity->pixels(y)[x] == source->pixels(y)[x], "Zero strengths are not identity");
  value(fx, "frontStrength", 5.25); value(fx, "backStrength", 2.5);
  value(fx, "sizeVariation", 35); value(fx, "frontFade", 40);
  value(fx, "backFade", 75); value(fx, "frontSharp", 60); value(fx, "backSharp", 25);
  value(fx, "noiseVariation", 70); value(fx, "noiseSeed", 13);
  value(fx, "noiseOffset", 142.5); value(fx, "noiseThickness", 3.2);
  fx.getInputPort(1)->setFx(new MotionSource(layer, TPointD(-2, -2)));
  for (int type : {0, 1, 2}) {
    value(fx, "noiseType", type);
    for (double angle : {0.0, 37.0, 90.0, -125.0}) {
      value(fx, "angle", angle);
      const auto full = render<Pixel>(fx, 19, 16, TPointD(-9, -7));
      std::vector<std::future<void>> jobs;
      for (int y = 0; y < 16; ++y)
        jobs.push_back(std::async(std::launch::async, [&, y] {
          const auto strip = render<Pixel>(fx, 19, 1, TPointD(-9, y - 7));
          for (int x = 0; x < 19; ++x) {
            const auto &a = full->pixels(y)[x], &b = strip->pixels(0)[x];
            require(a == b, "Whole/strip/concurrent output differs");
          }
        }));
      for (auto &job : jobs) job.get();
    }
  }
  TRenderSettings inputInfo, info; TRectD request;
  fx.transform(0, 0, TRectD(10, 10, 11, 11), info, request, inputInfo);
  require(request == TRectD(-3, -2, 4, 4), "Analysis domain depends on output tile");
  TRectD outputRect(10, 10, 11, 11); fx.doDryCompute(outputRect, 0, info);
  require(input->dryRequest == request, "Predictive request differs from canonical source request");
  info.m_affine = TScale(2);
  fx.transform(0, 0, outputRect, info, request, inputInfo);
  require(request == TRectD(-6, -4, 8, 8), "Scaled canonical domain is wrong");
  value(fx, "angle", 0); value(fx, "frontStrength", 4); value(fx, "backStrength", 2);
  TRectD box; fx.doGetBBox(0, box, info);
  require(box == TRectD(-6, -3, 9, 5), "Output bounds applied render affine twice");
  int canceled = 1; TRenderSettings canceledInfo; canceledInfo.m_isCanceled = &canceled;
  for (int initial : {1, 0}) {
    canceled = initial;
    fx.getInputPort(0)->setFx(new MotionSource(source, sourceOrigin, true, &canceled));
    const auto result = render<Pixel>(fx, 3, 3, TPointD(), 0, canceledInfo);
    for (int y = 0; y < 3; ++y)
      for (int x = 0; x < 3; ++x)
        require(result->pixels(y)[x] == Pixel::Transparent, "Canceled output is not clear");
  }
  TRasterPT<Pixel> partial(1, 1); partial->fill(Pixel::Black);
  fx.getInputPort(0)->setFx(new MotionSource(partial, TPointD(), false));
  value(fx, "angle", 0); value(fx, "noiseVariation", 0); value(fx, "sizeVariation", 0);
  const auto cleared = render<Pixel>(fx, 3, 3);
  require(cleared->pixels(2)[2] == Pixel::Transparent, "Unwritten source samples became shapes");
  partial->fill(Pixel::White);
  fx.getInputPort(0)->setFx(new MotionSource(partial));
  fx.getInputPort(1)->setFx(new MotionSource(partial, TPointD(1, 0), false));
  value(fx, "noiseType", 2); value(fx, "noiseVariation", 100);
  const auto noNoise = render<Pixel>(fx, 3, 1);
  require(noNoise->pixels(0)[1] == Pixel::Transparent,
          "Unwritten transformed noise samples generated a streak");
  TRasterPT<Pixel> bright(1, 1);
  const double alpha = scale == 1 ? 0.5 : std::floor(scale / 2);
  bright->pixels(0)[0] = Pixel(alpha, alpha, alpha, alpha);
  CelDirectionalBlurFx gain; gain.getInputPort(0)->setFx(new MotionSource(bright));
  value(gain, "gain", 10);
  const auto gained = render<Pixel>(gain, 1, 1)->pixels(0)[0];
  require(gained.m == alpha && (scale == 1 || gained.r <= gained.m),
          "Integer gain violated premultiplied coverage");
  CelDirectionalBlurFx disconnected;
  require(render<Pixel>(disconnected, 1, 1)->pixels(0)[0] == Pixel::Transparent, "Disconnected output not clear");
}

void persistenceTests(const QTemporaryDir &temp) {
  TFxP fx = new CelDirectionalBlurFx;
  for (const char *name : directionalNames) {
    param(*fx, name)->setValue(0, 1);
    param(*fx, name)->setValue(10, 2);
  }
  auto clone = fx->clone(false);
  for (const char *name : directionalNames)
    require(param(*clone, name)->isKeyframe(10), "Clone lost control animation");
  const TFilePath file(temp.filePath("directional.fx").toStdWString());
  { TOStream stream(file); stream << fx.getPointer(); }
  TFxP restored;
  { TIStream stream(file); stream >> restored; }
  require(restored->getFxType() == "STD_celDirectionalBlurFx", "Registration did not restore effect");
  for (const char *name : directionalNames)
    require(param(*restored, name)->getValue(10) == 2, "Scene lost a parameter");
  require(param(*restored, "noiseType")->getUIChoices().size() == 3, "Noise choices did not restore");
  auto *native = dynamic_cast<CelDirectionalBlurFx *>(fx.getPointer());
  param(*fx, "frontStrength")->setValue(0, 1.123456789);
  const auto alias = native->getAlias(0, TRenderSettings());
  param(*fx, "frontStrength")->setValue(0, 1.123456788);
  require(alias != native->getAlias(0, TRenderSettings()), "Fine strength shares cache alias");
  const TFilePath presetFile(temp.filePath("directional.preset").toStdWString());
  { TOStream stream(presetFile); fx->savePreset(stream); }
  CelDirectionalBlurFx preset;
  { TIStream stream(presetFile); preset.loadPreset(stream); }
  require(param(preset, "frontStrength")->getValue(0) == 1.123456788, "Preset lost precision");
  fx->setFxId(L"Motion1"); TFxP second = new CelDirectionalBlurFx; second->setFxId(L"Motion2");
  second->getInputPort(0)->setFx(fx.getPointer());
  TFxP owner = TMacroFx::create({fx, second});
  auto *macro = dynamic_cast<TMacroFx *>(owner.getPointer());
  require(macro != nullptr, "Macro creation failed");
  macro->setParamExposed(fx.getPointer(), "noiseType", true);
  const TFilePath macroFile(temp.filePath("directional.macro").toStdWString());
  { TOStream stream(macroFile); stream << macro; }
  TFxP loaded; { TIStream stream(macroFile); stream >> loaded; }
  auto *loadedMacro = dynamic_cast<TMacroFx *>(loaded.getPointer());
  TFx *member = loadedMacro ? loadedMacro->getFxById(L"Motion1") : nullptr;
  require(member && loadedMacro->isParamExposed(member, "noiseType") &&
              param(*member, "noiseType")->isKeyframe(10), "Macro lost exposed noise animation");
  TRasterFP hdr(1, 1); hdr->pixels(0)[0] = TPixelF(1, -0.25f, 0.125f, 0.5f);
  preset.getInputPort(0)->setFx(new MotionSource(hdr));
  value(preset, "frontStrength", 0); value(preset, "backStrength", 0);
  // Use a fresh effect so the animated preset parameters do not override defaults.
  CelDirectionalBlurFx gain; gain.getInputPort(0)->setFx(new MotionSource(hdr)); value(gain, "gain", 2);
  require(render<TPixelF>(gain, 1, 1)->pixels(0)[0] == TPixelF(2, -0.5f, 0.25f, 0.5f),
          "Float gain clipped HDR colors or changed alpha");
}
}  // namespace

int main(int argc, char **argv) {
  QCoreApplication app(argc, argv); TThread::init(); TRenderer::initialize(); QTemporaryDir temp;
  int result = 1;
  try {
    require(temp.isValid(), "Temporary directory unavailable");
    std::cout << "Checking directional kernel and noise" << std::endl;
    mathTests(); noiseTests();
    rendererTests<TPixel32>(); rendererTests<TPixel64>(); rendererTests<TPixelF>();
    productionTransformTests();
    rasterLevelDpiTests();
    std::cout << "Checking directional persistence" << std::endl;
    persistenceTests(temp);
    {
      TFxP fx = new CelDirectionalBlurFx;
      CelCameraBoxTest::setParam(*fx, "frontStrength", 3);
      CelCameraBoxTest::checkInfiniteSourceCoversCamera(
          static_cast<TRasterFx &>(*fx), "Cel Directional Blur");
    }
    std::cout << "PASS: directional lobes, shapes, fades, tails, noise, precision, tiles and persistence\n";
    result = 0;
  } catch (const TException &error) {
    std::cerr << QString::fromStdWString(error.getMessage()).toStdString() << '\n';
  } catch (const std::exception &error) { std::cerr << error.what() << '\n'; }
  TThread::shutdown(); return result;
}
