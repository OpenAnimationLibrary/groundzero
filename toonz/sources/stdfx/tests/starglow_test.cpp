#include "../starglowfx.cpp"
#include "tparamcontainer.h"
#include "tmacrofx.h"
#include "tstream.h"
#include <QCoreApplication>
#include <QTemporaryDir>
#include <cmath>
#include <future>
#include <iostream>
#include <memory>
#include <stdexcept>

namespace {
void require(bool value, const char *message) { if (!value) throw std::runtime_error(message); }
TDoubleParamP parameter(TFx &fx, const char *name) { return TParamP(fx.getParams()->getParam(name)); }
class Source final : public TStandardRasterFx {
  TRasterP m_image;
  TPointD m_origin;
public:
  explicit Source(const TRasterP &image, TPointD origin = TPointD()) : m_image(image), m_origin(origin) {}
  const TPersistDeclaration *getDeclaration() const override { return nullptr; }
  std::string getAlias(double, const TRenderSettings &) const override { return "star-test-source"; }
  bool canHandle(const TRenderSettings &, double) override { return true; }
  bool doGetBBox(double, TRectD &box, const TRenderSettings &) override { box = TRectD(m_origin, TDimensionD(m_image->getLx(), m_image->getLy())); return true; }
  void compute(TTile &tile, double, const TRenderSettings &) override {
    tile.getRaster()->clear();
    tile.getRaster()->copy(m_image, TPoint(int(std::llround(m_origin.x - tile.m_pos.x)),
                                           int(std::llround(m_origin.y - tile.m_pos.y))));
  }
  void doCompute(TTile &tile, double frame, const TRenderSettings &settings) override { compute(tile, frame, settings); }
};
class HugeSource final : public TStandardRasterFx {
public:
  const TPersistDeclaration *getDeclaration() const override { return nullptr; }
  bool canHandle(const TRenderSettings &, double) override { return true; }
  bool doGetBBox(double, TRectD &box, const TRenderSettings &) override { box = TRectD(0, 0, 1000000, 1000000); return true; }
  void doCompute(TTile &tile, double, const TRenderSettings &) override { tile.getRaster()->clear(); }
};
// Some upstream effects write only their intersection and do not clear the
// rest of a requested tile.
class PartialSource final : public TStandardRasterFx {
public:
  const TPersistDeclaration *getDeclaration() const override { return nullptr; }
  bool canHandle(const TRenderSettings &, double) override { return true; }
  bool doGetBBox(double, TRectD &box, const TRenderSettings &) override { box = TRectD(0, 0, 1, 1); return true; }
  void compute(TTile &tile, double, const TRenderSettings &) override {
    TRasterFP raster = tile.getRaster();
    const int x = -int(tile.m_pos.x), y = -int(tile.m_pos.y);
    if (x >= 0 && y >= 0 && x < raster->getLx() && y < raster->getLy()) raster->pixels(y)[x] = TPixelF::White;
  }
  void doCompute(TTile &tile, double frame, const TRenderSettings &settings) override { compute(tile, frame, settings); }
};
template <class Pixel>
TRasterPT<Pixel> renderFx(StarGlowFx &fx, int width, int height, TPointD origin = TPointD(), double frame = 0) {
  TRasterPT<Pixel> out(width, height); TTile tile; tile.setRaster(out); tile.m_pos = origin;
  fx.doCompute(tile, frame, TRenderSettings()); return out;
}
void settings(StarGlowFx &fx) {
  parameter(fx, "verticalLength")->setDefaultValue(0);
  parameter(fx, "horizontalLength")->setDefaultValue(0);
  parameter(fx, "diagonalLength")->setDefaultValue(0);
  parameter(fx, "highlightRadius")->setDefaultValue(0);
}
template <class Pixel>
void testDepth() {
  StarGlowFx fx; settings(fx);
  TRasterPT<Pixel> input(3, 3); input->clear(); input->pixels(1)[1] = Pixel::White;
  fx.getInputPort(0)->setFx(new Source(input));
  TRectD bounds;
  require(fx.doGetBBox(0, bounds, TRenderSettings()) && bounds == TRectD(0, 0, 3, 3),
          "Zero effects expanded the source bounds");
  auto plain = renderFx<Pixel>(fx, 3, 3);
  require(plain->pixels(1)[1] == Pixel::White && plain->pixels(0)[0] == Pixel::Transparent, "Zero effects did not preserve source");
  parameter(fx, "sourceOpacity")->setDefaultValue(0);
  parameter(fx, "verticalLength")->setDefaultValue(1);
  parameter(fx, "profile")->setDefaultValue(0);
  auto result = renderFx<Pixel>(fx, 3, 3);
  const TPixelD center = toPixelD(result->pixels(1)[1]);
  if (!(std::abs(center.r - 1.0 / 3.0) < 0.005 && std::abs(center.m - 1.0 / 3.0) < 0.005)) {
    std::cerr << "Box center for " << sizeof(Pixel) * 8 << "-bit pixel: r=" << center.r
              << ", g=" << center.g << ", b=" << center.b << ", alpha=" << center.m << '\n';
    throw std::runtime_error("Box streak has incorrect weight");
  }
  require(result->pixels(1)[0] == Pixel::Transparent && result->pixels(0)[1] != Pixel::Transparent, "Vertical streak direction is wrong");
  parameter(fx, "threshold")->setDefaultValue(100);
  result = renderFx<Pixel>(fx, 3, 3);
  require(result->pixels(1)[1] == Pixel::Transparent, "Threshold did not remove the highlight");
  parameter(fx, "threshold")->setDefaultValue(0);
  for (int channel = 0; channel < 4; ++channel) {
    parameter(fx, "channel")->setDefaultValue(channel);
    for (int profile = 0; profile < 4; ++profile) {
      parameter(fx, "profile")->setDefaultValue(profile);
      for (int merge = 0; merge < 2; ++merge) {
        parameter(fx, "merge")->setDefaultValue(merge);
        result = renderFx<Pixel>(fx, 3, 3);
        require(toPixelD(result->pixels(1)[1]).m > 0, "A mode lost a white highlight");
      }
    }
  }
  parameter(fx, "channel")->setDefaultValue(0); parameter(fx, "profile")->setDefaultValue(0);
  parameter(fx, "merge")->setDefaultValue(0); parameter(fx, "rotation")->setDefaultValue(90);
  result = renderFx<Pixel>(fx, 3, 3);
  require(result->pixels(1)[0] != Pixel::Transparent && result->pixels(0)[1] == Pixel::Transparent, "Rotation did not rotate the streak");
}
void testFloatAndTiles() {
  StarGlowFx fx; settings(fx);
  TRasterFP input(7, 5); input->clear(); input->pixels(2)[3] = TPixelF(2, 1, 0.5f, 1);
  fx.getInputPort(0)->setFx(new Source(input));
  parameter(fx, "channel")->setDefaultValue(2); parameter(fx, "sourceOpacity")->setDefaultValue(0);
  parameter(fx, "verticalLength")->setDefaultValue(2); parameter(fx, "horizontalLength")->setDefaultValue(3);
  parameter(fx, "diagonalLength")->setDefaultValue(1); parameter(fx, "highlightRadius")->setDefaultValue(1);
  parameter(fx, "rotation")->setDefaultValue(31); parameter(fx, "approximate")->setDefaultValue(1);
  auto full = renderFx<TPixelF>(fx, 7, 5);
  for (int y = 0; y < 5; ++y) {
    auto row = renderFx<TPixelF>(fx, 7, 1, TPointD(0, y));
    for (int x = 0; x < 7; ++x) {
      const TPixelF a = full->pixels(y)[x], b = row->pixels(0)[x];
      require(std::abs(a.r-b.r) < 1e-5 && std::abs(a.g-b.g) < 1e-5 && std::abs(a.m-b.m) < 1e-5, "Tiled glow differs from full image");
    }
  }
  std::vector<std::future<void>> jobs;
  for (int y = 0; y < 5; ++y) jobs.push_back(std::async(std::launch::async, [&, y] {
    auto row = renderFx<TPixelF>(fx, 7, 1, TPointD(0, y));
    for (int x = 0; x < 7; ++x) {
      const TPixelF a = full->pixels(y)[x], b = row->pixels(0)[x];
      require(std::abs(a.r-b.r) < 1e-5 && std::abs(a.g-b.g) < 1e-5 && std::abs(a.m-b.m) < 1e-5,
              "Concurrent tiles differ from full image");
    }
  }));
  for (auto &job : jobs) job.get();
  require(full->pixels(2)[3].r > full->pixels(2)[3].g, "RGB highlight lost color");
  auto ramp = TSpectrumParamP(TParamP(fx.getParams()->getParam("verticalRamp")));
  const auto before = fx.getAlias(0, TRenderSettings());
  ramp->getColor(0)->getRed()->setDefaultValue(0.123456789);
  require(before != fx.getAlias(0, TRenderSettings()), "Fine ramp edits share a cache alias");
  TFxP copy = fx.clone(false);
  auto copied = TSpectrumParamP(TParamP(copy->getParams()->getParam("verticalRamp")));
  require(copied->getColor(0)->getValueD(0).r == 0.123456789, "Clone lost ramp precision");
  // The synthetic source is intentionally not registered for persistence.
  // Serialize the effect itself after the rendering checks have finished.
  fx.getInputPort(0)->setFx(nullptr);
  QTemporaryDir temp; require(temp.isValid(), "Temporary directory unavailable");
  const TFilePath path(temp.filePath("star.fx").toStdWString());
  { TOStream stream(path); stream << &fx; }
  TIStream stream(path); TPersist *loaded = nullptr; stream >> loaded; std::unique_ptr<TPersist> owner(loaded);
  auto *restored = dynamic_cast<StarGlowFx *>(loaded); require(restored, "Factory did not restore glow");
  auto saved = TSpectrumParamP(TParamP(restored->getParams()->getParam("verticalRamp")));
  require(saved->getColor(0)->getValueD(0).r == 0.123456789, "Scene lost ramp precision");
  const TFilePath presetPath(temp.filePath("star.preset").toStdWString());
  { TOStream preset(presetPath); fx.savePreset(preset); }
  StarGlowFx restoredPreset;
  { TIStream preset(presetPath); restoredPreset.loadPreset(preset); }
  auto presetRamp = TSpectrumParamP(TParamP(restoredPreset.getParams()->getParam("verticalRamp")));
  require(presetRamp->getColor(0)->getValueD(0).r == 0.123456789, "Preset lost ramp precision");
  TFxP first = new StarGlowFx, second = new StarGlowFx;
  first->setFxId(L"StarGlow1"); second->setFxId(L"StarGlow2");
  auto firstRamp = TSpectrumParamP(TParamP(first->getParams()->getParam("verticalRamp")));
  firstRamp->getColor(0)->getRed()->setDefaultValue(0.123456789);
  second->getInputPort(0)->setFx(first.getPointer());
  TFxP macroOwner = TMacroFx::create({first, second});
  auto *macro = dynamic_cast<TMacroFx *>(macroOwner.getPointer());
  require(macro, "Could not construct macro");
  macro->setParamExposed(first.getPointer(), "verticalRamp", true);
  const TFilePath macroPath(temp.filePath("star.macro").toStdWString());
  { TOStream macroStream(macroPath); macroStream << macro; }
  TFxP restoredMacro;
  { TIStream macroStream(macroPath); macroStream >> restoredMacro; }
  auto *loadedMacro = dynamic_cast<TMacroFx *>(restoredMacro.getPointer());
  require(loadedMacro, "Macro failed to reload");
  TFx *member = loadedMacro->getFxById(L"StarGlow1");
  require(member && loadedMacro->isParamExposed(member, "verticalRamp"), "Macro lost exposed ramp");
  auto macroRamp = TSpectrumParamP(TParamP(member->getParams()->getParam("verticalRamp")));
  require(macroRamp->getColor(0)->getValueD(0).r == 0.123456789, "Macro lost ramp precision");
}

void testHighlightAndMerge() {
  require(rasterSizeSupported(3, 3, 16) && !rasterSizeSupported(100000, 100000, 16) &&
              !rasterSizeSupported(0, 1, 4), "Oversized raster arithmetic was not rejected");
  StarGlowFx large; settings(large); large.getInputPort(0)->setFx(new HugeSource);
  require(large.getMemoryRequirement(TRectD(0, 0, 100000, 100000), 0, TRenderSettings()) > 900000,
          "Large memory estimates overflowed");
  StarGlowFx fx; settings(fx);
  TRasterFP input(3, 3); input->clear(); input->pixels(1)[1] = TPixelF(1, 1, 1, 1);
  fx.getInputPort(0)->setFx(new Source(input));
  parameter(fx, "sourceOpacity")->setDefaultValue(0);
  parameter(fx, "highlightRadius")->setDefaultValue(1);
  parameter(fx, "profile")->setDefaultValue(0);
  auto output = renderFx<TPixelF>(fx, 3, 3);
  require(std::abs(output->pixels(1)[1].r - 1.0 / 9.0) < 1e-6, "Highlight blur has incorrect weight");
  require(output->pixels(0)[0].m > 0, "Highlight did not spread in two axes");
  parameter(fx, "highlightRadius")->setDefaultValue(0);
  parameter(fx, "verticalLength")->setDefaultValue(1);
  parameter(fx, "sourceOpacity")->setDefaultValue(100);
  parameter(fx, "merge")->setDefaultValue(0);
  const auto added = renderFx<TPixelF>(fx, 3, 3);
  parameter(fx, "merge")->setDefaultValue(1);
  const auto over = renderFx<TPixelF>(fx, 3, 3);
  require(std::abs(added->pixels(1)[1].r - 4.0 / 3.0) < 1e-6, "Add merge did not retain HDR glow");
  require(std::abs(over->pixels(1)[1].r - 1.0) < 1e-6, "Premultiply merge is not alpha weighted");
  parameter(fx, "merge")->setDefaultValue(0);
  parameter(fx, "sourceOpacity")->setDefaultValue(0);
  parameter(fx, "threshold")->setDefaultValue(50);
  parameter(fx, "approximate")->setDefaultValue(0);
  const auto fullInput = renderFx<TPixelF>(fx, 3, 3);
  require(fullInput->pixels(1)[1].m > 0, "Full-resolution highlight was unexpectedly removed");
  parameter(fx, "approximate")->setDefaultValue(1);
  const auto reducedInput = renderFx<TPixelF>(fx, 3, 3);
  require(reducedInput->pixels(1)[1] == TPixelF::Transparent,
          "Approximated input was not reduced before thresholding");
  int canceled = 1; TRenderSettings info; info.m_isCanceled = &canceled;
  TRasterFP canceledOutput(3, 3); canceledOutput->fill(TPixelF::White);
  TTile tile; tile.setRaster(canceledOutput); fx.doCompute(tile, 0, info);
  require(canceledOutput->pixels(1)[1] == TPixelF::Transparent, "Canceled render was not cleared");
}
// Expected values below follow the documented native operations for a single
// sample in the center of a three-tap, normalized Box streak. They do not
// claim equivalence with another host's implementation.
void testChannelAndCompositingValues() {
  StarGlowFx fx; settings(fx);
  TRasterFP input(3, 3); input->clear();
  input->pixels(1)[1] = TPixelF(0.2f, 0.1f, 0.05f, 0.5f);
  fx.getInputPort(0)->setFx(new Source(input));
  parameter(fx, "verticalLength")->setDefaultValue(1);
  parameter(fx, "profile")->setDefaultValue(0);
  parameter(fx, "sourceOpacity")->setDefaultValue(0);
  auto center = [&]() { return renderFx<TPixelF>(fx, 3, 3)->pixels(1)[1]; };
  auto closeTo = [](float value, double expected) { return std::abs(value - expected) < 1e-6; };

  // Straight RGB is (0.4, 0.2, 0.1); Luminance uses the documented
  // native coefficients, and non-alpha modes are premultiplied by 0.5.
  const double luma = 0.2126 * 0.4 + 0.7152 * 0.2 + 0.0722 * 0.1;
  const double expected[4][4] = {
      {0.5 / 3, 0.5 / 3, 0.5 / 3, 0.5 / 3},
      {0.5 * luma / 3, 0.5 * luma / 3, 0.5 * luma / 3, 0.5 * luma / 3},
      {0.2 / 3, 0.1 / 3, 0.05 / 3, 0.2 / 3},
      {0.2 / 3, 0.2 / 3, 0.2 / 3, 0.2 / 3}};
  for (int mode = 0; mode < 4; ++mode) {
    parameter(fx, "channel")->setDefaultValue(mode);
    const TPixelF p = center();
    require(closeTo(p.r, expected[mode][0]) && closeTo(p.g, expected[mode][1]) &&
                closeTo(p.b, expected[mode][2]) && closeTo(p.m, expected[mode][3]),
            "Partially transparent color produced the wrong channel extraction");
  }

  // Threshold 10%, fade 20%, gain 2: luminance is in the fade interval,
  // brightness exceeds it, and Alpha/RGB do not apply the fade control.
  parameter(fx, "gain")->setDefaultValue(2);
  parameter(fx, "threshold")->setDefaultValue(10);
  parameter(fx, "fade")->setDefaultValue(20);
  parameter(fx, "channel")->setDefaultValue(1);
  const double excess = luma - 0.1;
  const double fadedLuma = excess * (excess / 0.2) * 2 * 0.5 / 3;
  require(closeTo(center().r, fadedLuma), "Luminance threshold and fade have incorrect weight");
  parameter(fx, "channel")->setDefaultValue(3);
  require(closeTo(center().r, 0.1), "Brightness threshold and fade have incorrect weight");
  parameter(fx, "channel")->setDefaultValue(0);
  require(closeTo(center().m, 0.8 / 3), "Alpha channel incorrectly applies fade");
  parameter(fx, "channel")->setDefaultValue(2);
  const TPixelF rgb = center();
  require(closeTo(rgb.r, 0.1) && closeTo(rgb.g, 0.05) && closeTo(rgb.b, 0.025) && closeTo(rgb.m, 0.1),
          "RGB channel incorrectly applies fade or loses color");

  // The source contribution is 75%; the glow contribution and its alpha
  // are 60% of the normalized alpha streak. Premultiply attenuates source
  // by one minus that glow coverage; Add keeps the full source contribution.
  parameter(fx, "gain")->setDefaultValue(1);
  parameter(fx, "threshold")->setDefaultValue(0);
  parameter(fx, "fade")->setDefaultValue(0);
  parameter(fx, "channel")->setDefaultValue(0);
  parameter(fx, "glowOpacity")->setDefaultValue(60);
  parameter(fx, "sourceOpacity")->setDefaultValue(75);
  parameter(fx, "merge")->setDefaultValue(0);
  const TPixelF add = center();
  require(closeTo(add.r, 0.25) && closeTo(add.g, 0.175) && closeTo(add.b, 0.1375) && closeTo(add.m, 0.475),
          "Add did not apply independent source and glow opacity");
  parameter(fx, "merge")->setDefaultValue(1);
  const TPixelF over = center();
  require(closeTo(over.r, 0.235) && closeTo(over.g, 0.1675) && closeTo(over.b, 0.13375) && closeTo(over.m, 0.4375),
          "Premultiply did not apply glow coverage and opacity");
}

void testExponentialSupport() {
  StarGlowFx fx; settings(fx);
  TRasterFP input(7, 7); input->clear(); input->pixels(3)[3] = TPixelF::White;
  fx.getInputPort(0)->setFx(new Source(input));
  parameter(fx, "sourceOpacity")->setDefaultValue(0);
  parameter(fx, "verticalLength")->setDefaultValue(2);
  parameter(fx, "profile")->setDefaultValue(3);
  const auto result = renderFx<TPixelF>(fx, 7, 7);
  // At length two, the discrete taps are exp(-4*|i|/2) for i=-2..2.
  const double normalization = 1 + 2 * std::exp(-2.0) + 2 * std::exp(-4.0);
  require(std::abs(result->pixels(3)[3].m - 1.0 / normalization) < 1e-6 &&
              std::abs(result->pixels(4)[3].m - std::exp(-2.0) / normalization) < 1e-6 &&
              std::abs(result->pixels(5)[3].m - std::exp(-4.0) / normalization) < 1e-6,
          "Exponential taps have incorrect normalized weights");
  require(result->pixels(6)[3] == TPixelF::Transparent,
          "Exponential streak continued beyond its documented finite support");
}
void testPartialUpstream() {
  StarGlowFx fx; settings(fx);
  fx.getInputPort(0)->setFx(new PartialSource);
  parameter(fx, "sourceOpacity")->setDefaultValue(0);
  parameter(fx, "verticalLength")->setDefaultValue(1);
  parameter(fx, "profile")->setDefaultValue(0);
  auto output = renderFx<TPixelF>(fx, 3, 3, TPointD(-1, -1));
  for (int y = 0; y < 3; ++y) {
    require(output->pixels(y)[0] == TPixelF::Transparent && output->pixels(y)[2] == TPixelF::Transparent,
            "Unwritten upstream pixels affected glow");
    require(std::abs(output->pixels(y)[1].m - 1.0 / 3.0) < 1e-6,
            "Partial upstream highlight was not rendered correctly");
  }
  parameter(fx, "verticalLength")->setDefaultValue(0);
  parameter(fx, "highlightRadius")->setDefaultValue(3);
  auto outside = renderFx<TPixelF>(fx, 7, 7, TPointD(-3, -3));
  require(std::abs(outside->pixels(3)[0].m - 1.0 / 49.0) < 1e-6,
          "Highlight did not reach outside the source bounding box");
}
void testFractionalOrigins() {
  StarGlowFx fx; settings(fx);
  TRasterFP input(3, 3); input->clear(); input->pixels(1)[1] = TPixelF::White;
  fx.getInputPort(0)->setFx(new Source(input, TPointD(0.1, 0.2)));
  parameter(fx, "sourceOpacity")->setDefaultValue(0);
  parameter(fx, "verticalLength")->setDefaultValue(1);
  parameter(fx, "profile")->setDefaultValue(0);
  const auto full = renderFx<TPixelF>(fx, 3, 3, TPointD(0.1, 0.2));
  for (int y = 0; y < 3; ++y) {
    auto row = renderFx<TPixelF>(fx, 3, 1, TPointD(0.1, 0.2 + y));
    for (int x = 0; x < 3; ++x)
      require(std::abs(full->pixels(y)[x].m - row->pixels(0)[x].m) < 1e-6,
              "Fractional-origin tiles differ from the full image");
  }
}
void testAnimatedRampPrecision() {
  StarGlowFx fx; settings(fx);
  TRasterFP input(3, 3); input->clear(); input->pixels(1)[1] = TPixelF::White;
  fx.getInputPort(0)->setFx(new Source(input));
  parameter(fx, "sourceOpacity")->setDefaultValue(0);
  parameter(fx, "verticalLength")->setDefaultValue(1);
  parameter(fx, "verticalUseRamp")->setDefaultValue(1);
  parameter(fx, "profile")->setDefaultValue(0);
  auto ramp = TSpectrumParamP(TParamP(fx.getParams()->getParam("verticalRamp")));
  ramp->getColor(0)->getRed()->setValue(0, 0.1234567);
  ramp->getColor(0)->getRed()->setValue(10, 0.1234571);
  const auto first = renderFx<TPixelF>(fx, 3, 3, TPointD(), 0);
  const auto last = renderFx<TPixelF>(fx, 3, 3, TPointD(), 10);
  require(std::abs(first->pixels(1)[1].r - 0.1234567 / 3.0) < 1e-7,
          "Float glow did not evaluate the precise first ramp color");
  require(last->pixels(1)[1].r > first->pixels(1)[1].r,
          "Animated sub-8-bit ramp colors produced identical float glow");
  ramp->getColor(0)->getRed()->setValue(0, 1e300);
  require(!fx.getAlias(0, TRenderSettings()).empty(), "Extreme HDR alias failed");
  const auto hdr = renderFx<TPixelF>(fx, 3, 3);
  require(std::isfinite(hdr->pixels(1)[1].r) && hdr->pixels(1)[1].r > 1e30f,
          "Extreme HDR output was not safely saturated");
}
}
int main(int argc, char **argv) {
  QCoreApplication app(argc, argv);
  bool passed = true;
  auto run = [&](const char *name, const auto &test) {
    std::cerr << "Running " << name << '\n';
    try { test(); }
    catch (const std::exception &e) { std::cerr << name << ": " << e.what() << '\n'; passed = false; }
    catch (const TException &e) { std::cerr << name << ": " << QString::fromStdWString(e.getMessage()).toStdString() << '\n'; passed = false; }
  };
  run("8-bit", testDepth<TPixel32>); run("16-bit", testDepth<TPixel64>); run("float", testDepth<TPixelF>);
  run("tiles", testFloatAndTiles); run("highlight and merge", testHighlightAndMerge);
  run("channel, fade and compositing values", testChannelAndCompositingValues);
  run("exponential support", testExponentialSupport);
  run("partial upstream", testPartialUpstream); run("fractional origins", testFractionalOrigins);
  run("animated ramp", testAnimatedRampPrecision);
  if (!passed) return 1;
  std::cout << "PASS: channels, profiles, alpha, rotation, tiles and precise ramps\n";
  return 0;
}
