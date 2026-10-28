#include "../celradialblurfx.cpp"
#include "tparamcontainer.h"
#include "tstream.h"
#include "tmacrofx.h"

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
TDoubleParamP number(TFx &fx, const std::string &name) {
  return TParamP(fx.getParams()->getParam(name));
}
void set(TFx &fx, const std::string &name, double value) {
  auto p = number(fx, name); require(bool(p), "Missing numeric control"); p->setDefaultValue(value);
}
class Source final : public TStandardRasterFx {
  TRasterP m_raster;
  TPointD m_origin;
  int *m_cancel;
  bool m_expectCleared;
public:
  explicit Source(const TRasterP &raster, TPointD origin = TPointD(), int *cancel = nullptr,
                  bool expectCleared = false)
      : m_raster(raster), m_origin(origin), m_cancel(cancel), m_expectCleared(expectCleared) {}
  const TPersistDeclaration *getDeclaration() const override { return nullptr; }
  bool canHandle(const TRenderSettings &, double) override { return true; }
  bool doGetBBox(double, TRectD &box, const TRenderSettings &) override {
    box = TRectD(m_origin, TDimensionD(m_raster->getLx(), m_raster->getLy())); return true;
  }
  std::string getAlias(double, const TRenderSettings &) const override { return "radial-test-source"; }
  void compute(TTile &tile, double, const TRenderSettings &) override {
    if (m_expectCleared) {
      TRaster32P raster = tile.getRaster();
      require(bool(raster), "Expected an 8-bit initialization fixture");
      for (int y = 0; y < raster->getLy(); ++y)
        for (int x = 0; x < raster->getLx(); ++x)
          require(raster->pixels(y)[x] == TPixel32::Transparent,
                  "Upstream raster was not cleared before partial computation");
    }
    tile.getRaster()->clear();
    tile.getRaster()->copy(m_raster, TPoint(-int(tile.m_pos.x - m_origin.x), -int(tile.m_pos.y - m_origin.y)));
    if (m_cancel) ++*m_cancel;
  }
  void doCompute(TTile &tile, double frame, const TRenderSettings &settings) override { compute(tile, frame, settings); }
};
class HugeBoundsSource final : public TStandardRasterFx {
public:
  const TPersistDeclaration *getDeclaration() const override { return nullptr; }
  bool canHandle(const TRenderSettings &, double) override { return true; }
  bool doGetBBox(double, TRectD &box, const TRenderSettings &) override {
    box = TRectD(0, 0, 100000000, 100000000); return true;
  }
  std::string getAlias(double, const TRenderSettings &) const override { return "large-radial-source"; }
  void compute(TTile &tile, double, const TRenderSettings &) override {
    TRaster32P raster = tile.getRaster(); require(bool(raster), "Expected 8-bit tile");
    raster->fill(TPixel32::White);
  }
  void doCompute(TTile &tile, double frame, const TRenderSettings &settings) override {
    compute(tile, frame, settings);
  }
};

template <class Pixel>
TRasterPT<Pixel> render(CelRadialBlurFx &fx, int width, int height, TPointD origin = TPointD()) {
  TRasterPT<Pixel> output(width, height); TTile tile; tile.setRaster(output); tile.m_pos = origin;
  fx.doCompute(tile, 0, TRenderSettings()); return output;
}
void geometry() {
  const double pi = 3.14159265358979323846;
  require(CelRadial::fitsRasterAllocation(32768, 4095, 16),
          "Safe large raster was rejected");
  require(!CelRadial::fitsRasterAllocation(32768, 4096, 16) &&
          !CelRadial::fitsRasterAllocation(65536, 65536, 1) &&
          !CelRadial::fitsRasterAllocation(1, 0, 4),
          "Unsafe raster multiplication was accepted");
  const CelRadial::Point p{10, 0}, center{0, 0};
  auto outward = CelRadial::trajectory(p, center, 1, 0, 4, false, true);
  auto inward = CelRadial::trajectory(p, center, 1, 0, 4, false, false);
  require(std::abs(outward.x - 6) < 1e-12 && std::abs(inward.x - 14) < 1e-12, "Zoom directions are wrong");
  auto clockwise = CelRadial::trajectory(p, center, 1, 0, 5 * pi, true, true);
  auto counter = CelRadial::trajectory(p, center, 1, 0, 5 * pi, true, false);
  require(std::abs(clockwise.x) < 1e-12 && std::abs(clockwise.y - 10) < 1e-12 &&
          std::abs(counter.x) < 1e-12 && std::abs(counter.y + 10) < 1e-12,
          "Rotation directions are wrong");
  auto centerPoint = CelRadial::trajectory(center, center, 0, 45, 100, true, true);
  require(centerPoint.x == 0 && centerPoint.y == 0, "Center singularity changed the center");
  auto ellipse = CelRadial::trajectory(p, center, 2, 0, 4, false, true);
  auto rotatedEllipse = CelRadial::trajectory(p, center, 2, 90, 4, false, true);
  require(std::abs(ellipse.x - 8) < 1e-12 && std::abs(rotatedEllipse.x - 6) < 1e-12,
          "Ellipse ratio or angle did not affect zoom");
  for (double ratio : {0.0, 0.01, 1.0, 5.0})
    for (double angle : {0.0, 45.0, 90.0}) {
      auto q = CelRadial::trajectory({19, -11}, center, ratio, angle, 37, true, true);
      require(std::isfinite(q.x) && std::isfinite(q.y) &&
              std::hypot(q.x - 19, q.y + 11) <= 37.000001,
              "Elliptic rotation exceeded its halo or produced invalid coordinates");
    }
}

template <class Pixel>
void directionsAndTiles() {
  TRasterPT<Pixel> source(9, 1); source->clear();
  source->pixels(0)[2] = PixelConverter<Pixel>::from(TPixel32::White);
  CelRadialBlurFx fx; fx.getInputPort(0)->setFx(new Source(source));
  set(fx, "quality", 4); set(fx, "outerStrength", 2); set(fx, "repeatBorder", 0);
  auto outer = render<Pixel>(fx, 9, 1);
  require(outer->pixels(0)[3].m > 0 && outer->pixels(0)[1].m == 0,
          "Outward zoom did not move in the expected direction");
  set(fx, "outerFade", 100);
  auto fadedOuter = render<Pixel>(fx, 9, 1);
  require(fadedOuter->pixels(0)[3].m > 0 && fadedOuter->pixels(0)[3].m < outer->pixels(0)[3].m,
          "Outer Edge Fade did not soften the transparent border");
  set(fx, "outerFade", 0);
  set(fx, "quality", 1);
  require(render<Pixel>(fx, 9, 1)->pixels(0)[3].m == 0, "Quality did not change trajectory sampling");
  set(fx, "quality", 4);
  set(fx, "outerStrength", 0); set(fx, "innerStrength", 2);
  auto inner = render<Pixel>(fx, 9, 1);
  require(inner->pixels(0)[1].m > 0 && inner->pixels(0)[3].m == 0,
          "Inward zoom did not move in the expected direction");
  set(fx, "innerFade", 100);
  auto fadedInner = render<Pixel>(fx, 9, 1);
  require(fadedInner->pixels(0)[1].m > 0 && fadedInner->pixels(0)[1].m < inner->pixels(0)[1].m,
          "Inner Edge Fade did not soften the transparent border");
  set(fx, "innerFade", 0);
  set(fx, "outerStrength", 2);
  auto both = render<Pixel>(fx, 9, 1);
  require(both->pixels(0)[1].m > 0 && both->pixels(0)[3].m > 0,
          "Independent inner and outer controls cannot work together");
  std::vector<std::future<void>> jobs;
  for (int i = 0; i < 4; ++i) jobs.push_back(std::async(std::launch::async, [&] {
    auto concurrent = render<Pixel>(fx, 9, 1);
    for (int x = 0; x < 9; ++x)
      require(concurrent->pixels(0)[x] == both->pixels(0)[x], "Concurrent frames changed output");
  }));
  for (auto &job : jobs) job.get();
  auto left = render<Pixel>(fx, 4, 1, TPointD(0, 0));
  auto right = render<Pixel>(fx, 5, 1, TPointD(4, 0));
  for (int x = 0; x < 9; ++x)
    require(both->pixels(0)[x] == (x < 4 ? left->pixels(0)[x] : right->pixels(0)[x - 4]),
            "Tile boundaries changed radial output");
  auto padded = render<Pixel>(fx, 12, 3, TPointD(-2, -1));
  for (int x = 0; x < 9; ++x)
    require(padded->pixels(1)[x + 2] == both->pixels(0)[x], "Negative origin changed output");
}

template <class Pixel>
void rotationAndBorders() {
  TRasterPT<Pixel> source(7, 7); source->clear();
  source->pixels(3)[5] = PixelConverter<Pixel>::from(TPixel32::White);
  CelRadialBlurFx fx; fx.getInputPort(0)->setFx(new Source(source, TPointD(-3, -3)));
  set(fx, "blurType", 1); set(fx, "quality", 1); set(fx, "repeatBorder", 0);
  set(fx, "outerStrength", 3.14159265358979323846);
  auto outer = render<Pixel>(fx, 7, 7, TPointD(-3, -3));
  require(outer->pixels(1)[3].m > 0 && outer->pixels(5)[3].m == 0,
          "Clockwise rotation did not reach the expected side");
  set(fx, "outerStrength", 0); set(fx, "innerStrength", 3.14159265358979323846);
  auto inner = render<Pixel>(fx, 7, 7, TPointD(-3, -3));
  require(inner->pixels(5)[3].m > 0 && inner->pixels(1)[3].m == 0,
          "Counterclockwise rotation did not reach the expected side");

  TRasterPT<Pixel> edge(2, 1); edge->fill(PixelConverter<Pixel>::from(TPixel32::White));
  CelRadialBlurFx border; border.getInputPort(0)->setFx(new Source(edge));
  set(border, "outerStrength", 2); set(border, "quality", 2); set(border, "outerFade", 100);
  set(border, "repeatBorder", 0);
  auto noRepeat = render<Pixel>(border, 4, 1);
  set(border, "repeatBorder", 1);
  auto repeat = render<Pixel>(border, 4, 1);
  require(repeat->pixels(0)[3].m > noRepeat->pixels(0)[3].m,
          "Repeat Border did not prevent boundary fading");
  require(render<Pixel>(border, 1, 1, TPointD(20, 0))->pixels(0)[0] == Pixel::Transparent,
          "Repeat Border extended output past its support");
  set(border, "outerFade", 0); set(border, "repeatBorder", 0);
  auto noFade = render<Pixel>(border, 4, 1);
  set(border, "repeatBorder", 1);
  auto noFadeRepeat = render<Pixel>(border, 4, 1);
  for (int x = 0; x < 4; ++x)
    require(noFade->pixels(0)[x] == noFadeRepeat->pixels(0)[x],
            "Repeat Border affected output without edge fading");
}

template <class Pixel>
void integerGainPremultiplication() {
  const int half = Pixel::maxChannelValue / 2;
  TRasterPT<Pixel> input(1, 1); input->pixels(0)[0] = Pixel(half, 0, 0, half);
  CelRadialBlurFx fx; fx.getInputPort(0)->setFx(new Source(input));
  set(fx, "brightness", 2);
  const Pixel output = render<Pixel>(fx, 1, 1)->pixels(0)[0];
  require(output.m == half && output.r == half && output.r <= output.m,
          "Brightness gain produced an invalid premultiplied integer pixel");
}

void shapeAndNoise() {
  TRaster32P source(12, 1); source->clear(); source->pixels(0)[2] = TPixel32::Red;
  for (int x = 6; x <= 8; ++x) source->pixels(0)[x] = TPixel32::White;
  CelRadialBlurFx fx; fx.getInputPort(0)->setFx(new Source(source));
  set(fx, "outerStrength", 2); set(fx, "quality", 4); set(fx, "repeatBorder", 0);
  auto all = render<TPixel32>(fx, 12, 1);
  require(all->pixels(0)[3].m > 0 && all->pixels(0)[9].m > 0, "Baseline shapes did not blur");
  set(fx, "sizeVariation", 100);
  auto sizes = render<TPixel32>(fx, 12, 1);
  require(sizes->pixels(0)[3].m == 0 && sizes->pixels(0)[9].m > 0, "Shape-size variation was ignored");
  set(fx, "outerFade", 100);
  auto fadedSizes = render<TPixel32>(fx, 12, 1);
  require(fadedSizes->pixels(0)[2] == source->pixels(0)[2],
          "Edge Fade changed a group whose size variation removed blur");
  set(fx, "outerFade", 0);
  set(fx, "sizeVariation", 0); set(fx, "noiseVariation", 100); set(fx, "noiseType", 2);
  TRaster32P black(12, 1); black->fill(TPixel32::Black);
  fx.getInputPort(1)->setFx(new Source(black));
  require(render<TPixel32>(fx, 12, 1)->pixels(0)[3].m == 0, "Black noise layer did not suppress blur");
  set(fx, "outerFade", 100);
  require(render<TPixel32>(fx, 12, 1)->pixels(0)[2] == source->pixels(0)[2],
          "Edge Fade changed a pixel whose noise removed blur");
  set(fx, "outerFade", 0);
  TRaster32P white(12, 1); white->fill(TPixel32::White);
  fx.getInputPort(1)->setFx(new Source(white));
  require(render<TPixel32>(fx, 12, 1)->pixels(0)[3].m > 0, "White noise layer did not retain blur");
  set(fx, "noiseVariation", 0); set(fx, "outerStrength", 1); set(fx, "outerOffset", 2);
  set(fx, "outerOffsetMode", 0);
  require(render<TPixel32>(fx, 12, 1)->pixels(0)[5].m > 0, "Add offset mode was ignored");
  set(fx, "outerOffsetMode", 1);
  require(render<TPixel32>(fx, 12, 1)->pixels(0)[5].m == 0, "Max offset mode was ignored");
}

void noiseModes() {
  CelNoise::Settings noise; noise.seed = 7; noise.thickness = 10;
  noise.type = CelNoise::Smooth;
  const double first = CelNoise::sample(noise, 3.25, -4.5);
  require(first >= 0 && first <= 1 && first == CelNoise::sample(noise, 3.25, -4.5),
          "Smooth noise is not deterministic");
  noise.seed = 8;
  require(CelNoise::sample(noise, 3.25, -4.5) != first, "Noise seed had no effect");
  noise.seed = 7; noise.offset = 0.5;
  require(CelNoise::sample(noise, 3.25, -4.5) != first, "Noise offset had no effect");
  noise.offset = 0; noise.type = CelNoise::Block;
  require(CelNoise::sample(noise, 1, 1) == CelNoise::sample(noise, 8, 8),
          "Block noise varied within a cell");
  require(CelNoise::sample(noise, 1, 1) != CelNoise::sample(noise, 11, 1),
          "Block noise did not vary between cells");
  noise.type = CelNoise::Layer;
  require(CelNoise::sample(noise, 1, 1, 0.25) == 0.25, "Layer noise did not use luminance");
}
void presetAndMacro(const QString &presetPath, const QString &macroPath) {
  TFxP first = new CelRadialBlurFx, second = new CelRadialBlurFx;
  first->setFxId(L"Radial1"); second->setFxId(L"Radial2");
  number(*first, "brightness")->setValue(8, 1.234567891234);
  number(*first, "blurType")->setValue(8, 1);
  { TOStream stream(TFilePath(presetPath.toStdWString())); first->savePreset(stream); }
  CelRadialBlurFx preset;
  { TIStream stream(TFilePath(presetPath.toStdWString())); preset.loadPreset(stream); }
  require(number(preset, "brightness")->getValue(8) == 1.234567891234 &&
          number(preset, "blurType")->getValue(8) == 1, "Preset lost animated controls");
  second->getInputPort(0)->setFx(first.getPointer());
  TFxP macroOwner = TMacroFx::create({first, second});
  auto *macro = dynamic_cast<TMacroFx *>(macroOwner.getPointer());
  require(macro != nullptr, "Cannot build macro");
  macro->setParamExposed(first.getPointer(), "blurType", true);
  { TOStream stream(TFilePath(macroPath.toStdWString())); stream << macro; }
  TIStream stream(TFilePath(macroPath.toStdWString())); TFxP restored; stream >> restored;
  auto *loaded = dynamic_cast<TMacroFx *>(restored.getPointer());
  require(loaded != nullptr, "Cannot load macro");
  TFx *member = loaded->getFxById(L"Radial1");
  require(member && loaded->isParamExposed(member, "blurType") &&
          number(*member, "brightness")->getValue(8) == 1.234567891234,
          "Macro lost an exposed choice or precise value");
}

void precisionAndPersistence(const QString &path) {
  CelRadialBlurFx fx;
  TRasterFP input(1, 1); input->pixels(0)[0] = TPixelF(2.5f, -0.5f, 0.125f, 0.5f);
  fx.getInputPort(0)->setFx(new Source(input));
  auto pixel = render<TPixelF>(fx, 1, 1)->pixels(0)[0];
  require(pixel == input->pixels(0)[0], "Float HDR identity lost precision");
  set(fx, "brightness", 2); pixel = render<TPixelF>(fx, 1, 1)->pixels(0)[0];
  require(pixel.r == 5 && pixel.g == -1 && pixel.m == 0.5f, "Brightness gain changed alpha or clipped HDR");
  input->pixels(0)[0] = TPixelF(std::numeric_limits<float>::max(), 0, 0, 1);
  pixel = render<TPixelF>(fx, 1, 1)->pixels(0)[0];
  require(std::isfinite(pixel.r) && pixel.r == std::numeric_limits<float>::max(),
          "Extreme gain produced nonfinite output");
  const std::string alias = fx.getAlias(0, TRenderSettings());
  set(fx, "brightness", 2.0000001);
  require(fx.getAlias(0, TRenderSettings()) != alias, "Fine control changes share a render alias");
  fx.getInputPort(0)->setFx(nullptr);
  number(fx, "outerStrength")->setValue(10, 15.123456789123);
  { TOStream stream(TFilePath(path.toStdWString())); stream << &fx; }
  TIStream stream(TFilePath(path.toStdWString())); TPersist *loaded = nullptr; stream >> loaded;
  std::unique_ptr<TPersist> owner(loaded);
  auto *restored = dynamic_cast<CelRadialBlurFx *>(loaded);
  require(restored && number(*restored, "outerStrength")->getValue(10) == 15.123456789123,
          "Scene serialization lost the animated value");
  require(number(*restored, "blurType")->getUIChoices().size() == 2,
          "Scene loading lost animated choice metadata");
}

void zeroBlurDoesNotAllocateDomain() {
  CelRadialBlurFx fx; fx.getInputPort(0)->setFx(new HugeBoundsSource);
  TRenderSettings settings;
  const TRectD output(12, 20, 16, 21);
  TRectD request; TRenderSettings inputSettings;
  fx.transform(0, 0, output, settings, request, inputSettings);
  require(request == output && fx.getMemoryRequirement(output, 0, settings) == 0,
          "Zero blur requested the whole source domain");
  require(render<TPixel32>(fx, 4, 1, TPointD(12, 20))->pixels(0)[0] == TPixel32::White,
          "Zero blur did not pass through a large source");
  set(fx, "outerStrength", 1);
  require(fx.getMemoryRequirement(output, 0, settings) == std::numeric_limits<int>::max(),
          "Large-domain memory estimate wrapped instead of saturating");
}

void initializedInputs() {
  TRaster32P source(5, 1); source->fill(TPixel32::White);
  TRaster32P noise(5, 1); noise->fill(TPixel32::White);
  CelRadialBlurFx fx;
  fx.getInputPort(0)->setFx(new Source(source, TPointD(), nullptr, true));
  int noiseComputes = 0;
  fx.getInputPort(1)->setFx(new Source(noise, TPointD(), &noiseComputes, true));
  set(fx, "noiseType", 2); set(fx, "outerStrength", 1);
  require(render<TPixel32>(fx, 5, 1)->pixels(0)[0] == TPixel32::White && noiseComputes == 0,
          "Zero noise variation requested an unused noise layer");
  set(fx, "noiseVariation", 100);
  require(render<TPixel32>(fx, 5, 1)->pixels(0)[0] == TPixel32::White && noiseComputes == 1,
          "Cleared upstream inputs did not render correctly");
}

void cancellation() {
  TRaster32P input(2, 2); input->fill(TPixel32::White);
  int canceled = 1; CelRadialBlurFx fx; fx.getInputPort(0)->setFx(new Source(input, TPointD(), &canceled));
  TRaster32P output(2, 2); TTile tile; tile.setRaster(output);
  TRenderSettings settings; settings.m_isCanceled = &canceled;
  for (bool blurred : {false, true}) {
    set(fx, "outerStrength", blurred ? 2 : 0);
    for (int initiallyCanceled : {1, 0}) {
      canceled = initiallyCanceled; output->fill(TPixel32::White); fx.doCompute(tile, 0, settings);
      require(canceled == 1, "Canceled render requested its source");
      require(output->pixels(0)[0] == TPixel32::Transparent, "Canceled render left partial output");
    }
  }
}
}

int main(int argc, char **argv) {
  QCoreApplication app(argc, argv);
  try {
    geometry(); directionsAndTiles<TPixel32>(); directionsAndTiles<TPixel64>(); directionsAndTiles<TPixelF>();
    rotationAndBorders<TPixel32>(); rotationAndBorders<TPixel64>(); rotationAndBorders<TPixelF>();
    integerGainPremultiplication<TPixel32>(); integerGainPremultiplication<TPixel64>();
    shapeAndNoise(); noiseModes(); QTemporaryDir temp; require(temp.isValid(), "Cannot create scene fixture");
    precisionAndPersistence(temp.filePath("radial.fx"));
    presetAndMacro(temp.filePath("radial.preset"), temp.filePath("radial.macro"));
    zeroBlurDoesNotAllocateDomain(); initializedInputs(); cancellation();
    std::cout << "PASS: radial geometry, controls, precision, tiles and cancellation\n"; return 0;
  } catch (const TException &error) {
    std::cerr << QString::fromStdWString(error.getMessage()).toStdString() << '\n';
  } catch (const std::exception &error) { std::cerr << error.what() << '\n'; }
  return 1;
}
