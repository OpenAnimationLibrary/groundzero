#include "../seexprfx.cpp"
#include "tmacrofx.h"
#include <QCoreApplication>
#include <QTemporaryDir>
#include <future>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <type_traits>
#ifdef _WIN32
#include <windows.h>
#include <psapi.h>
#else
#include <sys/resource.h>
#endif

namespace {
void require(bool condition, const char *message) {
  if (!condition) throw std::runtime_error(message);
}
TDoubleParamP numeric(TFx &fx, const std::string &name) {
  TParam *parameter = fx.getParams()->getParam(name);
  require(parameter && dynamic_cast<TDoubleParam *>(parameter), "numeric fixture parameter is missing");
  return TParamP(parameter);
}
void set(TFx &fx, const std::string &name, double value) { numeric(fx, name)->setDefaultValue(value); }
void choice(TFx &fx, const std::string &name, int value) {
  auto *parameter = dynamic_cast<TIntEnumParam *>(fx.getParams()->getParam(name));
  require(parameter, "choice fixture parameter is missing"); parameter->setValue(value);
}
QJsonObject scalar(const char *id, double value, bool integer = false) {
  return {{"id", id}, {"name", id}, {"type", "scalar"}, {"value", value},
          {"min", -20}, {"max", 20}, {"integer", integer}};
}
void document(TFx &fx, const QString &rgb, const QString &alpha = "1", const QJsonArray &controls = {}) {
  auto *native = dynamic_cast<TFxSeExprSource *>(&fx);
  require(native, "native expression interface is missing");
  const QJsonObject object{{"version", 1}, {"rgb", rgb}, {"alpha", alpha}, {"controls", controls}};
  native->seExprDocumentParam()->setValue(QString::fromUtf8(QJsonDocument(object).toJson(QJsonDocument::Compact)).toStdWString());
}
template <typename Pixel> TRasterPT<Pixel> rendered(
    TRasterFx &fx, int width, int height, TPointD position = TPointD(), double frame = 0,
    const TRenderSettings &settings = TRenderSettings()) {
  TRasterPT<Pixel> image(width, height); image->fill(Pixel::White);
  TTile tile; tile.setRaster(image); tile.m_pos = position;
  fx.doCompute(tile, frame, settings);
  return image;
}
template <typename Pixel> void expect(const Pixel &pixel, double r, double g, double b, double a) {
  const double maximum = Pixel::maxChannelValue;
  const double epsilon = std::is_same<Pixel, TPixelF>::value ? 0.000003 : 1.5 / maximum;
  require(std::abs(double(pixel.r) / maximum - r) < epsilon &&
              std::abs(double(pixel.g) / maximum - g) < epsilon &&
              std::abs(double(pixel.b) / maximum - b) < epsilon &&
              std::abs(double(pixel.m) / maximum - a) < epsilon,
          "rendered pixel differs from the analytic result");
}
class Source final : public TStandardRasterFx {
  TRasterP m_image;
public:
  explicit Source(const TRasterP &image) : m_image(image) {}
  const TPersistDeclaration *getDeclaration() const override { return nullptr; }
  bool canHandle(const TRenderSettings &, double) override { return true; }
  std::string getAlias(double, const TRenderSettings &) const override { return "seexpr-test-source"; }
  bool doGetBBox(double, TRectD &box, const TRenderSettings &) override {
    box = TRectD(0, 0, m_image->getLx(), m_image->getLy()); return true;
  }
  void compute(TTile &tile, double, const TRenderSettings &) override {
    tile.getRaster()->clear();
    tile.getRaster()->copy(m_image, TPoint(-int(tile.m_pos.x), -int(tile.m_pos.y)));
  }
  void doCompute(TTile &tile, double frame, const TRenderSettings &settings) override { compute(tile, frame, settings); }
};

template <typename Pixel> void checkGenerator() {
  SeExprGeneratorFx fx;
  set(fx, "width", 8); set(fx, "height", 4);
  document(fx, "[$u,$v,0.25]", "0.5");
  const auto image = rendered<Pixel>(fx, 8, 4, TPointD(-4, -2));
  for (int y = 0; y < 4; ++y) for (int x = 0; x < 8; ++x)
    expect(image->pixels(y)[x], ((x + .5) / 8) * .5, ((y + .5) / 4) * .5, .125, .5);
  for (int origin = 0; origin < 8; origin += 2) {
    const auto split = rendered<Pixel>(fx, 2, 4, TPointD(-4 + origin, -2));
    for (int y = 0; y < 4; ++y) for (int x = 0; x < 2; ++x)
      require(split->pixels(y)[x] == image->pixels(y)[origin + x], "generator has a tile seam");
  }
  expect(rendered<Pixel>(fx, 1, 1, TPointD(9, 9))->pixels(0)[0], 0, 0, 0, 0);
  TRenderSettings scaled; scaled.m_affine = TScale(2);
  expect(rendered<Pixel>(fx, 1, 1, TPointD(-8, -4), 0, scaled)->pixels(0)[0], .015625, .03125, .125, .5);
  document(fx, "[$time/2,0,0]");
  set(fx, "fps", 12);
  expect(rendered<Pixel>(fx, 1, 1, TPointD(), 12)->pixels(0)[0], .5, 0, 0, 1);
  int stop = 1; TRenderSettings canceledSettings; canceledSettings.m_isCanceled = &stop;
  expect(rendered<Pixel>(fx, 2, 2, TPointD(), 0, canceledSettings)->pixels(0)[0], 0, 0, 0, 0);
}

template <typename Pixel> void checkColorAndSampling() {
  const double maximum = Pixel::maxChannelValue;
  TRasterPT<Pixel> input(3, 1);
  input->pixels(0)[0] = Pixel(maximum * .25, 0, 0, maximum * .5);
  input->pixels(0)[1] = Pixel(0, maximum * .5, 0, maximum * .5);
  input->pixels(0)[2] = Pixel(0, 0, maximum, maximum);
  SeExprColorFx fx; fx.getInputPort("Source")->setFx(new Source(input));
  document(fx, "$src", "$a");
  const auto identity = rendered<Pixel>(fx, 3, 1);
  for (int x = 0; x < 3; ++x) require(identity->pixels(0)[x] == input->pixels(0)[x], "identity changed premultiplied input");
  document(fx, "sample_rgb(1,0)", "sample_alpha(1,0)"); set(fx, "samplingRadius", 1);
  choice(fx, "samplingFilter", 0);
  const auto shift = rendered<Pixel>(fx, 3, 1);
  require(shift->pixels(0)[0] == input->pixels(0)[1], "bounded neighbor sampler did not shift Source");
  expect(shift->pixels(0)[2], 0, 0, 0, 0);
  choice(fx, "edgeMode", 2);
  require(rendered<Pixel>(fx, 3, 1)->pixels(0)[2] == input->pixels(0)[0], "repeat sampling did not wrap");
  document(fx, "sample_rgb(0.5,0)", "sample_alpha(0.5,0)");
  choice(fx, "samplingFilter", 1);
  const auto wrapped = rendered<Pixel>(fx, 3, 1);
  expect(wrapped->pixels(0)[2], double(input->pixels(0)[0].r) / maximum * .5, 0, .5,
         (1 + double(input->pixels(0)[0].m) / maximum) * .5);
  const auto split = rendered<Pixel>(fx, 1, 1, TPointD(2, 0));
  require(split->pixels(0)[0] == wrapped->pixels(0)[2], "repeat bilinear sampling has a tile seam");
  document(fx, "[1,1,1]", "1");
  TRasterPT<Pixel> mask(3, 1); mask->clear(); mask->pixels(0)[1] = Pixel::White;
  fx.getInputPort("Mask")->setFx(new Source(mask));
  set(fx, "mix", .5);
  const auto mixed = rendered<Pixel>(fx, 3, 1);
  require(mixed->pixels(0)[0] == input->pixels(0)[0], "zero mask did not retain Source");
  expect(mixed->pixels(0)[1], .5, .5 + double(input->pixels(0)[1].g) / maximum * .5,
         .5, .5 + double(input->pixels(0)[1].m) / maximum * .5);
  document(fx, "sample_rgb(2,0)", "1");
  bool rejected = false; try { rendered<Pixel>(fx, 1, 1); } catch (const TException &) { rejected = true; }
  require(rejected, "sampler silently exceeded its declared radius");
}

void checkNamedControlsAndPersistence(const QString &directory) {
  TFxP owner = new SeExprGeneratorFx;
  auto &fx = *dynamic_cast<SeExprGeneratorFx *>(owner.getPointer());
  QJsonObject tint{{"id", "tint"}, {"name", "tint"}, {"type", "color"}, {"value", QJsonArray{3.5, -.25, 1.123456789}}};
  document(fx, "$tint", "1", {tint});
  expect(rendered<TPixelF>(fx, 1, 1)->pixels(0)[0], 3.5, -.25, 1.123456789, 1);
  QJsonObject colors{{"id", "colors"}, {"name", "colors"}, {"type", "colorcurve"},
      {"points", QJsonArray{QJsonArray{0, QJsonArray{4, -.5, .123456789}, 1},
                             QJsonArray{1, QJsonArray{2, .5, .987654321}, 1}}}};
  document(fx, "colors(0.25)", "1", {colors});
  expect(rendered<TPixelF>(fx, 1, 1)->pixels(0)[0], 3.5, -.25,
         .123456789 * .75 + .987654321 * .25, 1);
  TRenderSettings linear; linear.m_linearColorSpace = true; linear.m_colorSpaceGamma = 2;
  expect(rendered<TPixelF>(fx, 1, 1, TPointD(), 0, linear)->pixels(0)[0], 13, -.3125,
         .123456789 * .123456789 * .75 + .987654321 * .987654321 * .25, 1);
  QJsonObject spectrum{{"id", "ocean"}, {"name", "ocean"}, {"type", "curve"},
      {"presentation", "spectrum"}, {"spectrum", QJsonObject()},
      {"points", QJsonArray{QJsonArray{0, 0}, QJsonArray{1, 1}}}};
  document(fx, "[ocean(0.5),0,0]", "1", {spectrum});
  expect(rendered<TPixelF>(fx, 1, 1)->pixels(0)[0], TSeExprSpectrum::value(TSeExprSpectrum::Parameters(), .5), 0, 0, 1);
  set(fx, "seexprControl_ocean__s_amplitude", 0);
  expect(rendered<TPixelF>(fx, 1, 1)->pixels(0)[0], 0, 0, 0, 1);
  QJsonObject steps = scalar("steps", 2.7, true);
  document(fx, "[$steps,0,0]", "1", {steps});
  expect(rendered<TPixelF>(fx, 1, 1)->pixels(0)[0], 3, 0, 0, 1);
  numeric(fx, "seexprControl_steps__value")->setValue(1, 4.4);
  expect(rendered<TPixelF>(fx, 1, 1, TPointD(), 1)->pixels(0)[0], 4, 0, 0, 1);
  const auto before = fx.getAlias(1, TRenderSettings());
  numeric(fx, "seexprControl_steps__value")->setValue(1, 4.40000001);
  require(before != fx.getAlias(1, TRenderSettings()), "cache key lost control precision");
  auto renamed = steps; renamed["label"] = "Renamed Steps";
  document(fx, "[$steps,0,0]", "1", {renamed});
  require(numeric(fx, "seexprControl_steps__value")->hasKeyframes(), "display rename deleted animation");
  QJsonObject curve{{"id", "ramp"}, {"name", "ramp"}, {"type", "curve"},
                    {"points", QJsonArray{QJsonArray{0, 0, 0}, QJsonArray{1, 1, 1}}}};
  document(fx, "[ramp(0.5),$steps,0]", "1", {renamed, curve});
  expect(rendered<TPixelF>(fx, 1, 1, TPointD(), 1)->pixels(0)[0], 0, 4, 0, 1);
  curve["points"] = QJsonArray{QJsonArray{0, 0, 1}, QJsonArray{.5, 1, 1}, QJsonArray{1, 0, 1}};
  document(fx, "[ramp(0.25),$steps,0]", "1", {curve, renamed});
  expect(rendered<TPixelF>(fx, 1, 1, TPointD(), 1)->pixels(0)[0], .5, 4, 0, 1);
  TFxP copy = fx.clone(false);
  require(copy->getParams()->getParamCount() == fx.getParams()->getParamCount(), "cloning lost archived controls or curve stops");
  expect(rendered<TPixelF>(*dynamic_cast<TRasterFx *>(copy.getPointer()), 1, 1, TPointD(), 1)->pixels(0)[0], .5, 4, 0, 1);
  for (bool compressed : {false, true}) {
    const TFilePath path((directory + (compressed ? "/fx-compressed.tnz" : "/fx.tnz")).toStdWString());
    { TOStream stream(path, compressed); stream << owner.getPointer(); }
    TFxP restored; { TIStream stream(path); stream >> restored; }
    require(restored && restored->getParams()->getParam("seexprControl_steps__value"), "scene loading looked up controls before their schema");
    expect(rendered<TPixelF>(*dynamic_cast<TRasterFx *>(restored.getPointer()), 1, 1, TPointD(), 1)->pixels(0)[0], .5, 4, 0, 1);
  }
  const TFilePath preset((directory + "/formula.preset").toStdWString());
  { TOStream stream(preset); fx.savePreset(stream); }
  SeExprGeneratorFx presetFx; { TIStream stream(preset); presetFx.loadPreset(stream); }
  expect(rendered<TPixelF>(presetFx, 1, 1, TPointD(), 1)->pixels(0)[0], .5, 4, 0, 1);
  owner->setFxId(L"Expression1");
  TFxP macroOwner = TMacroFx::create({owner});
  auto *macro = dynamic_cast<TMacroFx *>(macroOwner.getPointer());
  require(macro, "native expression macro creation failed");
  macro->setParamExposed(owner.getPointer(), "seexprControl_steps__value", true);
  const TFilePath macroPath((directory + "/formula.macro").toStdWString());
  { TOStream stream(macroPath); stream << macroOwner.getPointer(); }
  TFxP loadedMacro; { TIStream stream(macroPath); stream >> loadedMacro; }
  auto *restoredMacro = dynamic_cast<TMacroFx *>(loadedMacro.getPointer());
  require(restoredMacro && restoredMacro->getFxById(L"Expression1") &&
              restoredMacro->isParamExposed(restoredMacro->getFxById(L"Expression1"), "seexprControl_steps__value"),
          "macro save/reopen lost the stable expression-control exposure");
  SeExprGeneratorFx linked;
  document(linked, "[$extra,0,0]", "1", {scalar("extra", .2)});
  linked.linkParams(&fx);
  numeric(fx, "seexprControl_steps__value")->setValue(1, 6);
  expect(rendered<TPixelF>(linked, 1, 1, TPointD(), 1)->pixels(0)[0], .5, 6, 0, 1);
  auto expanded = scalar("another", .75);
  document(fx, "[$another,$steps,0]", "1", {renamed, expanded});
  require(linked.getParams()->getParam("seexprControl_another__value") == fx.getParams()->getParam("seexprControl_another__value"),
          "growing a linked schema created independent controls");
  linked.unlinkParams();
  document(linked, "[$solo,0,0]", "1", {scalar("solo", .125)});
  expect(rendered<TPixelF>(linked, 1, 1)->pixels(0)[0], .125, 0, 0, 1);
  require(!fx.getParams()->getParam("seexprControl_solo__value"), "unlinked schema edits changed the former source");
  document(fx, "[0,0,0]");
  require(fx.getParams()->getParam("seexprControl_steps__value") &&
              numeric(fx, "seexprControl_steps__value")->hasKeyframes(), "removing a control deleted its archived animation");
}

void checkFailuresAndWorkers() {
  SeExprGeneratorFx fx;
  document(fx, "[broken,");
  bool failed = false; try { rendered<TPixelF>(fx, 1, 1); } catch (const TException &) { failed = true; }
  require(failed, "invalid formula rendered a success-looking blank image");
  document(fx, "[1/0,0,0]"); failed = false;
  try { rendered<TPixelF>(fx, 1, 1); } catch (const TException &) { failed = true; }
  require(failed, "nonfinite output was accepted");
  auto worker = [] {
    SeExprGeneratorFx effect; set(effect, "width", 16); set(effect, "height", 16);
    document(effect, "def FLOAT square(FLOAT a){a*a} [square($u),square($v),noise([$x,$y,$seed])]");
    return rendered<TPixelF>(effect, 16, 16, TPointD(-8,-8));
  };
  auto a = std::async(std::launch::async, worker), b = std::async(std::launch::async, worker);
  const auto first = a.get(), second = b.get();
  for (int y = 0; y < 16; ++y) for (int x = 0; x < 16; ++x)
    require(first->pixels(y)[x] == second->pixels(y)[x], "parallel workers shared mutable interpreter state");
}

std::uint64_t checksum(const TRasterFP &image, int rowStart, int fullWidth) {
  std::uint64_t result = 0;
  for (int y = 0; y < image->getLy(); ++y) for (int x = 0; x < image->getLx(); ++x) {
    const TPixelF &p = image->pixels(y)[x];
    const float channels[] = {p.r, p.g, p.b, p.m};
    for (int c = 0; c < 4; ++c) {
      std::uint32_t bits = 0; std::memcpy(&bits, channels + c, sizeof(bits));
      std::uint64_t h = std::uint64_t(bits) ^
          ((std::uint64_t(rowStart + y) * fullWidth + x) * 4 + c) * 0x9e3779b97f4a7c15ULL;
      h ^= h >> 30; h *= 0xbf58476d1ce4e5b9ULL;
      h ^= h >> 27; h *= 0x94d049bb133111ebULL;
      result += h ^ (h >> 31);
    }
  }
  return result;
}
std::uint64_t peakMemory() {
#ifdef _WIN32
  PROCESS_MEMORY_COUNTERS counters{};
  require(GetProcessMemoryInfo(GetCurrentProcess(), &counters, sizeof(counters)) != 0,
          "peak working-set measurement failed");
  return counters.PeakWorkingSetSize;
#else
  struct rusage usage{};
  require(getrusage(RUSAGE_SELF, &usage) == 0, "peak resident-memory measurement failed");
#ifdef __APPLE__
  return std::uint64_t(usage.ru_maxrss);
#else
  return std::uint64_t(usage.ru_maxrss) * 1024;
#endif
#endif
}
void benchmark() {
  struct Work { double cold = 0, warm = 0; std::uint64_t first = 0, second = 0; };
  for (const auto dimensions : {TDimension(1920,1080), TDimension(3840,2160)}) {
    std::uint64_t referenceFirst = 0, referenceSecond = 0;
    for (int workers : {1, 4}) {
      std::vector<std::future<Work>> tasks;
      const auto wall = std::chrono::steady_clock::now();
      for (int worker = 0; worker < workers; ++worker) {
        const int start = dimensions.ly * worker / workers;
        const int end = dimensions.ly * (worker + 1) / workers;
        tasks.push_back(std::async(std::launch::async, [=]() {
          SeExprGeneratorFx effect;
          set(effect, "width", dimensions.lx); set(effect, "height", dimensions.ly);
          document(effect, "[$u,$v,0.5+0.25*noise([$x/64,$y/64,$time])]");
          Work result;
          for (int frame = 0; frame < 2; ++frame) {
            const auto began = std::chrono::steady_clock::now();
            const auto image = rendered<TPixelF>(effect, dimensions.lx, end - start,
                TPointD(-dimensions.lx * .5, start - dimensions.ly * .5), frame);
            const double elapsed = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - began).count();
            const auto hash = checksum(image, start, dimensions.lx);
            if (frame == 0) { result.cold = elapsed; result.first = hash; }
            else { result.warm = elapsed; result.second = hash; }
          }
          return result;
        }));
      }
      Work total;
      for (auto &task : tasks) {
        const auto result = task.get();
        total.cold = std::max(total.cold, result.cold); total.warm = std::max(total.warm, result.warm);
        total.first += result.first; total.second += result.second;
      }
      if (workers == 1) { referenceFirst = total.first; referenceSecond = total.second; }
      else require(referenceFirst == total.first && referenceSecond == total.second,
                   "full-resolution worker-count checksums differ");
      std::cout << "seexpr-benchmark: width=" << dimensions.lx << " height=" << dimensions.ly
                << " workers=" << workers << " cold_max_worker_ms=" << total.cold
                << " warm_max_worker_ms=" << total.warm
                << " total_wall_ms=" << std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - wall).count()
                << " checksum_frame0=" << total.first << " checksum_frame1=" << total.second
                << " peak_resident_bytes=" << peakMemory() << std::endl;
    }
  }
}
}  // namespace

int main(int argc, char **argv) {
  QCoreApplication application(argc, argv);
  try {
    if (argc == 2 && std::string(argv[1]) == "--benchmark") { benchmark(); return 0; }
    QTemporaryDir temporary; require(temporary.isValid(), "temporary fixture directory is unavailable");
    checkGenerator<TPixel32>(); checkGenerator<TPixel64>(); checkGenerator<TPixelF>();
    checkColorAndSampling<TPixel32>(); checkColorAndSampling<TPixel64>(); checkColorAndSampling<TPixelF>();
    checkNamedControlsAndPersistence(temporary.path()); checkFailuresAndWorkers();
    std::cout << "seexpr-fx: PASS\n"; return 0;
  } catch (const TException &error) {
    std::cerr << QString::fromStdWString(error.getMessage()).toStdString() << '\n';
  } catch (const std::exception &error) { std::cerr << error.what() << '\n'; }
  return 1;
}
