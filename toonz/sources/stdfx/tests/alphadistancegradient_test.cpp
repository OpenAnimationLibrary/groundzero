#include "../alphadistancegradientfx.cpp"
#include "tparamcontainer.h"
#include "tstream.h"
#include "tmacrofx.h"
#include <QCoreApplication>
#include <QTemporaryDir>
#include <future>
#include <iostream>
#include <stdexcept>
#include <type_traits>
namespace {
void require(bool value, const char *message) { if (!value) throw std::runtime_error(message); }
TDoubleParamP number(TFx &fx, const std::string &name) { return TParamP(fx.getParams()->getParam(name)); }
TPixelParamP color(TFx &fx, const std::string &name) { return TParamP(fx.getParams()->getParam(name)); }
void set(TFx &fx, const std::string &name, double value) { number(fx, name)->setDefaultValue(value); }
class Source final : public TStandardRasterFx {
  TRasterP m_image;
  bool m_infinite;
  int *m_cancel;
  bool m_partial;
public:
  explicit Source(const TRasterP &image, bool infinite = false, int *cancel = nullptr, bool partial = false)
      : m_image(image), m_infinite(infinite), m_cancel(cancel), m_partial(partial) {}
  const TPersistDeclaration *getDeclaration() const override { return nullptr; }
  bool canHandle(const TRenderSettings &, double) override { return true; }
  std::string getAlias(double, const TRenderSettings &) const override { return "distance-source"; }
  bool doGetBBox(double, TRectD &box, const TRenderSettings &) override {
    box = m_infinite ? TConsts::infiniteRectD : TRectD(0, 0, m_image->getLx(), m_image->getLy());
    return true;
  }
  void compute(TTile &tile, double, const TRenderSettings &) override {
    if (m_partial) {
      const TRasterP raster = tile.getRaster();
      for (int y = 0; y < raster->getLy(); ++y)
        for (int x = 0; x < raster->getLx() * raster->getPixelSize(); ++x)
          require(raster->getRawData()[size_t(y) * raster->getWrap() * raster->getPixelSize() + x] == 0,
                  "Gradient source raster was not cleared before rendering");
    } else tile.getRaster()->clear();
    if (m_infinite) {
      if (TRaster32P raster = tile.getRaster()) raster->fill(TPixel32::White);
      else if (TRaster64P raster = tile.getRaster()) raster->fill(TPixel64::White);
      else if (TRasterFP raster = tile.getRaster()) raster->fill(TPixelF::White);
    }
    else tile.getRaster()->copy(m_image, TPoint(-int(tile.m_pos.x), -int(tile.m_pos.y)));
    if (m_cancel) *m_cancel = 1;
  }
  void doCompute(TTile &tile, double frame, const TRenderSettings &settings) override {
    compute(tile, frame, settings);
  }
};
template <class Pixel>
TRasterPT<Pixel> render(AlphaDistanceGradientFx &fx, int w, int h,
                       TPointD pos = TPointD(), const TRenderSettings &settings = TRenderSettings()) {
  TRasterPT<Pixel> result(w, h); result->fill(Pixel::White);
  TTile tile; tile.setRaster(result); tile.m_pos = pos; fx.doCompute(tile, 0, settings);
  return result;
}
template <class Pixel> void expect(const Pixel &pixel, double r, double g, double b, double alpha) {
  const double maximum = Pixel::maxChannelValue;
  const double tolerance = std::is_same<Pixel, TPixelF>::value ? 0.000002 : 1.5 / maximum;
  require(std::abs(pixel.r / maximum - r) <= tolerance &&
              std::abs(pixel.g / maximum - g) <= tolerance &&
              std::abs(pixel.b / maximum - b) <= tolerance &&
              std::abs(pixel.m / maximum - alpha) <= tolerance, "Gradient pixel differs from analytic fixture");
}
template <class Pixel> void checkModes() {
  const double maximum = Pixel::maxChannelValue;
  TRasterPT<Pixel> input(7, 7); input->clear();
  const Pixel source(maximum / 4, maximum / 8, 0, maximum / 2);
  for (int y = 1; y < 6; ++y) for (int x = 1; x < 6; ++x) input->pixels(y)[x] = source;
  AlphaDistanceGradientFx fx; fx.getInputPort(0)->setFx(new Source(input, false, nullptr, true));
  set(fx, "insideThreshold", 4); set(fx, "outsideThreshold", 4); set(fx, "power", 2);
  color(fx, "gradationColor")->setDefaultValue(TPixel32::Red);
  color(fx, "backgroundColor")->setDefaultValue(TPixel32::Blue);
  const double weights[] = {0.5, std::sqrt(0.75), 0.25, 1};
  for (int region = 0; region < 3; ++region)
    for (int curve = 0; curve < 4; ++curve)
      for (int mode = 0; mode < 2; ++mode)
        for (int background = 0; background < 2; ++background)
          for (int invert = 0; invert < 2; ++invert) {
            set(fx, "region", region); set(fx, "interpolation", curve);
            set(fx, "renderMode", mode); set(fx, "useBackground", background); set(fx, "invert", invert);
            const auto out = render<Pixel>(fx, 1, 1, TPointD(3, 3));
            double weight = region == 1 ? 0 : weights[curve];
            if (invert) weight = 1 - weight;
            const double red = mode ? double(source.r) / source.m : 1;
            const double green = mode ? double(source.g) / source.m : 0;
            expect(out->pixels(0)[0], red * weight, green * weight,
                   background ? 1 - weight : 0, background ? 1 : weight);
            if (region != 0) {
              auto outside = render<Pixel>(fx, 1, 1, TPointD(8, 3));
              weight = invert ? 1 - weights[curve] : weights[curve];
              expect(outside->pixels(0)[0], red * weight, green * weight,
                     background ? 1 - weight : 0, background ? 1 : weight);
            }
          }
  set(fx, "region", 0); set(fx, "renderMode", 0); set(fx, "invert", 0);
  set(fx, "useBackground", 0); set(fx, "interpolation", 0); set(fx, "insideThreshold", 0);
  expect(render<Pixel>(fx, 1, 1, TPointD(1, 3))->pixels(0)[0], 1, 0, 0, 1);
  expect(render<Pixel>(fx, 1, 1, TPointD(3, 3))->pixels(0)[0], 0, 0, 0, 0);
  input->pixels(3)[3] = Pixel::Transparent;
  set(fx, "insideThreshold", 4);
  expect(render<Pixel>(fx, 1, 1, TPointD(2, 3))->pixels(0)[0], 1, 0, 0, 1);
  input->clear();
  expect(render<Pixel>(fx, 1, 1)->pixels(0)[0], 0, 0, 0, 0);
  set(fx, "useBackground", 1);
  expect(render<Pixel>(fx, 1, 1)->pixels(0)[0], 0, 0, 1, 1);
  set(fx, "invert", 1);
  expect(render<Pixel>(fx, 1, 1)->pixels(0)[0], 1, 0, 0, 1);
  input->fill(source); set(fx, "invert", 0); set(fx, "useBackground", 0);
  expect(render<Pixel>(fx, 1, 1)->pixels(0)[0], 1, 0, 0, 1);
}
void checkBlur() {
  std::vector<double> impulse(25 * 25, 0); impulse[12 * 25 + 12] = 1;
  auto plain = impulse, scaled = impulse;
  require(gradientBlur(plain, 25, 25, 3, false, nullptr) &&
              gradientBlur(scaled, 25, 25, 3, true, nullptr), "Blur failed");
  require(std::abs(plain[12 * 25 + 12] - 49.0 / 729) < 1e-12,
          "Three-pass blur differs from analytic kernel");
  require(scaled[12 * 25 + 12] == 1 && plain[12 * 25 + 12] < 0.1,
          "Blur modes do not preserve distinct peak behavior");
  double sum = 0; for (double value : plain) sum += value;
  require(std::abs(sum - 1) < 1e-12 && plain[12 * 25 + 8] < 1e-15,
          "Blur lost energy or exceeded support");
  require(gradientBlur(impulse, 25, 25, 0, true, nullptr) && impulse[12 * 25 + 12] == 1,
          "Zero blur changed the profile");
  int canceled = 1;
  require(!gradientBlur(impulse, 25, 25, 1, false, &canceled), "Blur ignored cancellation");
}
template <class Pixel> void checkTilesAndCancellation() {
  TRasterPT<Pixel> input(9, 5); input->clear();
  input->pixels(2)[3] = Pixel::White; input->pixels(1)[6] = Pixel::Black;
  AlphaDistanceGradientFx fx; fx.getInputPort(0)->setFx(new Source(input, false, nullptr, true));
  set(fx, "region", 2); set(fx, "insideThreshold", 2); set(fx, "outsideThreshold", 3);
  set(fx, "blurSize", 2);
  for (int blur = 0; blur < 3; ++blur) {
    set(fx, "blurMode", blur);
    const auto full = render<Pixel>(fx, 23, 19, TPointD(-7, -7));
    std::vector<std::future<void>> jobs;
    for (int y = 0; y < 19; ++y) jobs.emplace_back(std::async(std::launch::async, [&, y] {
      const auto strip = render<Pixel>(fx, 23, 1, TPointD(-7, y - 7));
      for (int x = 0; x < 23; ++x)
        require(strip->pixels(0)[x] == full->pixels(y)[x], "Canvas varies with tile or thread");
    }));
    for (auto &job : jobs) job.get();
  }
  int canceled = 1; TRenderSettings settings; settings.m_isCanceled = &canceled;
  fx.getInputPort(0)->setFx(new Source(input, false, &canceled));
  for (int initial : {1, 0}) {
    canceled = initial; const auto output = render<Pixel>(fx, 2, 2, TPointD(), settings);
    for (int y = 0; y < 2; ++y) for (int x = 0; x < 2; ++x)
      require(output->pixels(y)[x] == Pixel::Transparent, "Cancellation left output");
  }
}
void checkCanvasAndFloat() {
  const TPixelD huge(std::numeric_limits<double>::max(), -100, 0.25, 0.5);
  const auto integer8 = gradientPixel(huge, static_cast<TPixel32 *>(nullptr));
  const auto integer16 = gradientPixel(huge, static_cast<TPixel64 *>(nullptr));
  require(integer8.r == integer8.m && integer8.g == 0 &&
              integer16.r == integer16.m && integer16.g == 0,
          "Integer HDR conversion violated premultiplied coverage");
  const auto floating = gradientPixel(huge, static_cast<TPixelF *>(nullptr));
  require(std::isfinite(floating.r) && floating.r == std::numeric_limits<float>::max() &&
              floating.g == -100, "Float output overflowed or clamped signed HDR");
  TRasterFP input(1, 1); input->pixels(0)[0] = TPixelF(1, -0.25f, 0.125f, 0.5f);
  AlphaDistanceGradientFx fx; fx.getInputPort(0)->setFx(new Source(input, false, nullptr, true));
  set(fx, "renderMode", 1);
  expect(render<TPixelF>(fx, 1, 1)->pixels(0)[0], 2, -0.5, 0.25, 1);
  TRenderSettings settings; settings.m_affine = TScale(2);
  TRectD box;
  require(fx.doGetBBox(0, box, settings) && box == TRectD(0, 0, 1, 1),
          "Bounds apply affine twice");
  settings.m_affine = TScale(2, 1);
  require(!fx.canHandle(settings, 0), "Nonuniform transform bypasses adapter");
  TRaster32P finite(3, 3); finite->fill(TPixel32::White);
  fx.getInputPort(0)->setFx(new Source(finite, true));
  settings = TRenderSettings(); settings.m_cameraBox = TRectD(-2, -2, 3, 3);
  require(fx.doGetBBox(0, box, settings) && box == settings.m_cameraBox,
          "Infinite source ignored finite camera");
  auto image = render<TPixel32>(fx, 7, 7, TPointD(-3, -3), settings);
  require(image->pixels(0)[0] == TPixel32::Transparent && image->pixels(1)[1].m == 255,
          "Camera boundary was not finite");
  settings.m_cameraBox = TRectD(); bool threw = false;
  try { render<TPixel32>(fx, 1, 1, TPointD(), settings); }
  catch (const TException &) { threw = true; }
  require(threw, "Unbounded canvas silently depends on tile size");
  threw = false;
  try { gradientSize(TRectD(0, 0, 1000000, 1000000)); }
  catch (const TException &) { threw = true; }
  require(threw, "Canvas index overflow was accepted");
  for (int bytes : {4, 8, 16}) {
    const int width = std::numeric_limits<int>::max() / bytes;
    require(gradientSize(TRectD(0, 0, width, 1), bytes).lx == width,
            "Safe canvas byte-count boundary was rejected");
    bool overflow = false;
    try { gradientSize(TRectD(0, 0, width + 1, 1), bytes); }
    catch (const TException &) { overflow = true; }
    require(overflow, "Canvas allocation would overflow signed byte count");
  }
  for (double scale : {0.25, 0.5, 1.1, 2.0}) {
    settings.m_affine = TScale(scale);
    require(!fx.canHandle(settings, 0) && fx.handledAffine(settings, 0) == TAffine(),
            "Preview must transform one canonical native-space result");
  }
  AlphaDistanceGradientFx missing;
  require(render<TPixel32>(missing, 1, 1)->pixels(0)[0] == TPixel32::Transparent,
          "Missing source retained output");
}
const std::array<const char *, 10> numbers = {{"region", "insideThreshold", "outsideThreshold",
  "renderMode", "useBackground", "invert", "interpolation", "power", "blurMode", "blurSize"}};
void checkPersistence(const QString &directory) {
  TFxP first = new AlphaDistanceGradientFx, second = new AlphaDistanceGradientFx;
  first->setFxId(L"Distance1"); second->setFxId(L"Distance2");
  const double values[] = {2, 1000, 0, 1, 1, 1, 3, 0.01, 2, 4096};
  for (size_t i = 0; i < numbers.size(); ++i) number(*first, numbers[i])->setValue(10, values[i]);
  color(*first, "gradationColor")->setValueD(10, TPixelD(1.12345678901, 0.5, -0.25, 1));
  color(*first, "backgroundColor")->setValueD(10, TPixelD(0.000000000123456, 0.25, 0.75, 1));
  TFxP clone = first->clone(false);
  for (size_t i = 0; i < numbers.size(); ++i)
    require(number(*clone, numbers[i])->getValue(10) == values[i], "Clone lost a control");
  TFxP linked = new AlphaDistanceGradientFx; linked->linkParams(first.getPointer());
  require(number(*linked, "region").getPointer() == number(*first, "region").getPointer(), "Parameter link copied values");
  linked->unlinkParams();
  const std::string before = first->getAlias(10, TRenderSettings());
  number(*first, "insideThreshold")->setValue(10, 999.99999999);
  require(before != first->getAlias(10, TRenderSettings()), "Fine controls share cache aliases");
  for (bool compressed : {false, true}) {
    const TFilePath path((directory + (compressed ? "/compressed.fx" : "/plain.fx")).toStdWString());
    { TOStream stream(path, compressed); stream << first; }
    TFxP restored; { TIStream stream(path); stream >> restored; }
    require(restored && color(*restored, "backgroundColor")->getValueD(10) ==
                color(*first, "backgroundColor")->getValueD(10), "Scene lost color precision");
    for (const char *name : numbers)
      require(number(*restored, name)->isKeyframe(10) && number(*restored, name)->getValue(10) ==
                  number(*first, name)->getValue(10), "Scene lost an animated control");
    require(number(*restored, "interpolation")->getUIChoices().size() == 4,
            "Restored choice control lost metadata");
  }
  const TFilePath preset((directory + "/distance.preset").toStdWString());
  { TOStream stream(preset); first->savePreset(stream); }
  { TIStream stream(preset); second->loadPreset(stream); }
  for (const char *name : numbers)
    require(number(*second, name)->getValue(10) == number(*first, name)->getValue(10), "Preset lost a control");
  second->getInputPort(0)->setFx(first.getPointer());
  TFxP owner = TMacroFx::create({first, second});
  auto *macro = dynamic_cast<TMacroFx *>(owner.getPointer());
  require(macro != nullptr, "Macro creation failed");
  for (const char *name : numbers) macro->setParamExposed(first.getPointer(), name, true);
  macro->setParamExposed(first.getPointer(), "gradationColor", true);
  const TFilePath path((directory + "/distance.macro").toStdWString());
  { TOStream stream(path); stream << macro; }
  TFxP restored; { TIStream stream(path); stream >> restored; }
  auto *loaded = dynamic_cast<TMacroFx *>(restored.getPointer());
  require(loaded && loaded->getFxById(L"Distance1"), "Macro did not restore effect");
  for (const char *name : numbers)
    require(loaded->isParamExposed(loaded->getFxById(L"Distance1"), name), "Macro lost exposure");
}
}
int main(int argc, char **argv) {
  QCoreApplication app(argc, argv); QTemporaryDir temp;
  try {
    require(temp.isValid(), "Temporary directory unavailable");
    checkModes<TPixel32>(); checkModes<TPixel64>(); checkModes<TPixelF>(); checkBlur();
    checkTilesAndCancellation<TPixel32>(); checkTilesAndCancellation<TPixel64>(); checkTilesAndCancellation<TPixelF>();
    checkCanvasAndFloat(); checkPersistence(temp.path());
    std::cout << "PASS: every mode, all depths, blur, finite canvas, tiles and persistence\n"; return 0;
  } catch (const TException &error) {
    std::cerr << QString::fromStdWString(error.getMessage()).toStdString() << '\n';
  } catch (const std::exception &error) { std::cerr << error.what() << '\n'; }
  return 1;
}
