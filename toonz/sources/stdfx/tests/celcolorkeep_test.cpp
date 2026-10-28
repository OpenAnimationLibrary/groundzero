// Compile the production node directly so registration, persistence and rendering
// are exercised together without loading unrelated effects.
#include "../celcolorkeepfx.cpp"
#include "tparamcontainer.h"
#include "tpixelutils.h"
#include "tstream.h"
#include "tmacrofx.h"

#include <QCoreApplication>
#include <QTemporaryDir>
#include <future>
#include <iostream>
#include <memory>
#include <stdexcept>

namespace {
void require(bool condition, const char *message) {
  if (!condition) throw std::runtime_error(message);
}

TPixelParamP color(TFx &fx, int index) {
  return TParamP(fx.getParams()->getParam(colorSlotName(index)));
}
TDoubleParamP count(TFx &fx) {
  return TParamP(fx.getParams()->getParam("enabledColors"));
}
void setColor(TFx &fx, int index, const TPixelD &value) {
  auto param = color(fx, index);
  param->getRed()->setDefaultValue(value.r);
  param->getGreen()->setDefaultValue(value.g);
  param->getBlue()->setDefaultValue(value.b);
  param->getMatte()->setDefaultValue(value.m);
}

class Source final : public TStandardRasterFx {
  TRasterP m_image;
  int *m_cancelOnCompute;
public:
  explicit Source(const TRasterP &image, int *cancelOnCompute = nullptr)
      : m_image(image), m_cancelOnCompute(cancelOnCompute) {}
  const TPersistDeclaration *getDeclaration() const override { return nullptr; }
  bool canHandle(const TRenderSettings &, double) override { return true; }
  bool doGetBBox(double, TRectD &box, const TRenderSettings &) override {
    box = TRectD(0, 0, m_image->getLx(), m_image->getLy());
    return true;
  }
  std::string getAlias(double, const TRenderSettings &) const override {
    return "cel-test-source";
  }
  void compute(TTile &tile, double, const TRenderSettings &) override {
    tile.getRaster()->clear();
    tile.getRaster()->copy(m_image, TPoint(-int(tile.m_pos.x), -int(tile.m_pos.y)));
    if (m_cancelOnCompute) ++*m_cancelOnCompute;
  }
  void doCompute(TTile &tile, double frame, const TRenderSettings &info) override {
    compute(tile, frame, info);
  }
};

template <class Pixel>
TRasterPT<Pixel> render(CelColorKeepFx &fx, int width, int height = 1,
                       double frame = 0, TPointD position = TPointD()) {
  TRasterPT<Pixel> output(width, height);
  TTile tile;
  tile.setRaster(output);
  tile.m_pos = position;
  fx.doCompute(tile, frame, TRenderSettings());
  return output;
}

template <class Pixel>
void testInteger() {
  const int max = Pixel::maxChannelValue;
  CelColorKeepFx fx;
  TRasterPT<Pixel> input(103, 1);
  count(fx)->setDefaultValue(100);
  for (int i = 0; i < 100; ++i) {
    // All 100 entries are distinct; this also reaches the final bound slot.
    input->pixels(0)[i] = Pixel(i, 100 - i, 31, max);
    setColor(fx, i, TPixelD(double(i) / max, double(100 - i) / max,
                            31.0 / max, 1));
  }
  input->pixels(0)[100] = Pixel(200, 200, 200, max);
  input->pixels(0)[101] = Pixel(42, 31, 24, 0);
  input->pixels(0)[102] = Pixel(0, 0, 0, max);
  fx.getInputPort(0)->setFx(new Source(input));
  auto output = render<Pixel>(fx, 103);
  for (int i = 0; i < 100; ++i)
    require(output->pixels(0)[i] == input->pixels(0)[i], "A selected color vanished");
  for (int i = 100; i < 103; ++i)
    require(output->pixels(0)[i] == Pixel::Transparent, "Unselected RGB leaked");
  count(fx)->setDefaultValue(99);
  output = render<Pixel>(fx, 103);
  require(output->pixels(0)[99] == Pixel::Transparent, "Inactive slot matched");
  count(fx)->setDefaultValue(100);
  output = render<Pixel>(fx, 103);
  require(output->pixels(0)[99] == input->pixels(0)[99], "Inactive value was lost");
  setColor(fx, 0, color(fx, 99)->getValueD(0));
  output = render<Pixel>(fx, 103);
  require(output->pixels(0)[99] == input->pixels(0)[99], "Duplicate changed output");
  count(fx)->setDefaultValue(0);
  output = render<Pixel>(fx, 103);
  for (int i = 0; i < 103; ++i)
    require(output->pixels(0)[i] == Pixel::Transparent, "Zero count kept a pixel");

  // Native premultiplied coverage: red keeps its exact source alpha and RGB.
  count(fx)->setDefaultValue(1);
  setColor(fx, 0, TPixelD(1, 0, 0, 1));
  const int alpha = max / 2;
  input->pixels(0)[0] = Pixel(alpha, 0, 0, alpha);
  input->pixels(0)[1] = Pixel(alpha - 1, 0, 0, alpha);
  output = render<Pixel>(fx, 2);
  require(output->pixels(0)[0] == input->pixels(0)[0], "Partial alpha changed");
  require(output->pixels(0)[1] == Pixel::Transparent, "Integer key gained tolerance");

  // A mixed color exercises the host's depth-specific coverage rounding.
  setColor(fx, 0, TPixelD(123.0 / 255, 65.0 / 255, 211.0 / 255, 1));
  const Pixel selected(max * 123 / 255, max * 65 / 255, max * 211 / 255, alpha);
  input->pixels(0)[0] = premultiply(selected);
  output = render<Pixel>(fx, 1);
  require(output->pixels(0)[0] == input->pixels(0)[0],
          "Native premultiplication did not match the selected color");
}

void testFloat() {
  CelColorKeepFx fx;
  TRasterFP input(7, 1);
  const float tolerance = 0.0001f;
  input->pixels(0)[0] = TPixelF(0, 0, 0, 1);
  input->pixels(0)[1] = TPixelF(tolerance, 0, 0, 1);
  input->pixels(0)[2] = TPixelF(std::nextafter(tolerance, 1.0f), 0, 0, 1);
  input->pixels(0)[3] = TPixelF(0, 0, 0, 0.5f);
  input->pixels(0)[4] = TPixelF(1, 1, 1, 0);
  input->pixels(0)[5] = TPixelF(std::numeric_limits<float>::quiet_NaN(), 0, 0, 1);
  input->pixels(0)[6] = TPixelF(std::numeric_limits<float>::infinity(), 0, 0, 1);
  fx.getInputPort(0)->setFx(new Source(input));
  auto output = render<TPixelF>(fx, 7);
  for (int i : {0, 1, 3})
    require(output->pixels(0)[i] == input->pixels(0)[i], "Float boundary rejected");
  for (int i : {2, 4, 5, 6})
    require(output->pixels(0)[i] == TPixelF::Transparent, "Invalid float matched");
  setColor(fx, 0, TPixelD(2, -0.5, 0.3, 1));
  input->pixels(0)[0] = TPixelF(1, -0.25f, 0.15f, 0.5f);
  output = render<TPixelF>(fx, 1);
  require(output->pixels(0)[0] == input->pixels(0)[0], "HDR key was clamped");

  setColor(fx, 0, TPixelD(0.1234, 0.5, 0.75, 1));
  input->pixels(0)[0] = TPixelF(0.1234f, 0.5f, 0.75f, 1);
  input->pixels(0)[1] = TPixelF(0.1238f, 0.5f, 0.75f, 1);
  output = render<TPixelF>(fx, 2);
  require(output->pixels(0)[0] == input->pixels(0)[0], "Float key was quantized");
  require(output->pixels(0)[1] == TPixelF::Transparent, "Sub-8-bit key merged");
}

void testPrecisionAndPersistence(const QString &path, bool compressed) {
  CelColorKeepFx fx;
  count(fx)->setValue(0, 1);
  count(fx)->setValue(10, 100);
  color(fx, 0)->setValueD(0, TPixelD(0.1234567, 0.5, 0.75, 1));
  color(fx, 99)->setValueD(10, TPixelD(0.7654321, 0.5, 0.75, 1));
  setColor(fx, 50, TPixelD(1.23456789123e-12, 3.5, -0.15, 1));
  TRenderSettings settings;
  const std::string before = fx.getAlias(0, settings);
  color(fx, 0)->getRed()->setValue(0, 0.1234568);
  require(fx.getAlias(0, settings) != before, "Fine colors share a cache alias");
  TFxP clone = fx.clone(false);
  require(color(*clone, 99)->getValueD(10) == color(fx, 99)->getValueD(10),
          "Clone lost the last color");
  require(count(*clone)->isKeyframe(10), "Clone lost count animation");
  {
    TOStream stream(TFilePath(path.toStdWString()), compressed);
    stream << &fx;
    require(!stream.setFullPrecision(false), "Effect leaked its stream mode");
  }
  TIStream stream(TFilePath(path.toStdWString()));
  TPersist *loaded = nullptr;
  stream >> loaded;
  std::unique_ptr<TPersist> owner(loaded);
  auto *restored = dynamic_cast<CelColorKeepFx *>(loaded);
  require(restored != nullptr, "Factory did not restore effect");
  for (int i = 0; i < 100; ++i) {
    require(color(*restored, i)->getValueD(10) == color(fx, i)->getValueD(10),
            "A serialized color changed");
  }
  require(color(*restored, 99)->isKeyframe(10), "Hidden color key was lost");
  require(count(*restored)->getValue(10) == 100, "Count did not persist");
  require(activeColorCount(1.49) == 1 && activeColorCount(1.5) == 2 &&
              activeColorCount(-1) == 0 && activeColorCount(101) == 100,
          "Animated count does not round and clamp");
}

void testTilesAndThreads() {
  CelColorKeepFx fx;
  TRaster32P input(17, 13);
  for (int y = 0; y < 13; ++y)
    for (int x = 0; x < 17; ++x)
      input->pixels(y)[x] = (x + y) % 2 ? TPixel32::Black : TPixel32::White;
  fx.getInputPort(0)->setFx(new Source(input));
  const auto full = render<TPixel32>(fx, 17, 13);
  std::vector<std::future<void>> jobs;
  for (int y = 0; y < 13; ++y)
    jobs.push_back(std::async(std::launch::async, [&, y] {
      auto strip = render<TPixel32>(fx, 17, 1, 0, TPointD(0, y));
      for (int x = 0; x < 17; ++x)
        require(strip->pixels(0)[x] == full->pixels(y)[x], "Tile/thread mismatch");
    }));
  for (auto &job : jobs) job.get();
  const auto padded = render<TPixel32>(fx, 23, 17, 0, TPointD(-3, -2));
  for (int y = 0; y < 17; ++y)
    for (int x = 0; x < 23; ++x) {
      const bool inside = x >= 3 && x < 20 && y >= 2 && y < 15;
      const TPixel32 expected = inside ? full->pixels(y - 2)[x - 3]
                                      : TPixel32::Transparent;
      require(padded->pixels(y)[x] == expected, "Negative-origin tile mismatch");
    }
  CelColorKeepFx disconnected;
  auto empty = render<TPixel32>(disconnected, 2);
  require(empty->pixels(0)[0] == TPixel32::Transparent, "Missing input not clear");
}

template <class Pixel>
void testCancellation() {
  CelColorKeepFx fx;
  TRasterPT<Pixel> input(2, 2), output(2, 2);
  input->fill(Pixel::Black);
  int canceled = 1;
  fx.getInputPort(0)->setFx(new Source(input, &canceled));
  TRenderSettings settings;
  settings.m_isCanceled = &canceled;
  TTile tile;
  tile.setRaster(output);
  for (int initiallyCanceled : {1, 0}) {
    canceled = initiallyCanceled;
    output->fill(Pixel::White);
    fx.doCompute(tile, 0, settings);
    require(canceled == 1, "Canceled render still requested its source");
    for (int y = 0; y < 2; ++y)
      for (int x = 0; x < 2; ++x)
        require(output->pixels(y)[x] == Pixel::Transparent,
                "Cancellation left a partially rendered tile");
  }
}

void testPresetAndMacro(const QString &presetPath, const QString &macroPath) {
  TFxP first = new CelColorKeepFx;
  TFxP second = new CelColorKeepFx;
  first->setFxId(L"CelColorKeep1");
  second->setFxId(L"CelColorKeep2");
  count(*first)->setDefaultValue(100);
  color(*first, 99)->setValueD(5, TPixelD(0.123456789123, 2.5, 0.25, 1));
  {
    TOStream stream(TFilePath(presetPath.toStdWString()));
    first->savePreset(stream);
    require(!stream.setFullPrecision(false), "Preset leaked its stream mode");
  }
  CelColorKeepFx restoredPreset;
  {
    TIStream stream(TFilePath(presetPath.toStdWString()));
    restoredPreset.loadPreset(stream);
  }
  require(color(restoredPreset, 99)->getValueD(5) == color(*first, 99)->getValueD(5),
          "Preset lost high-precision color");
  require(count(restoredPreset)->getValue(5) == 100, "Preset lost enabled count");
  second->getInputPort(0)->setFx(first.getPointer());
  TFxP macroOwner = TMacroFx::create({first, second});
  auto *macro = dynamic_cast<TMacroFx *>(macroOwner.getPointer());
  require(macro != nullptr, "Could not build macro");
  macro->setParamExposed(first.getPointer(), "color100", true);
  {
    TOStream stream(TFilePath(macroPath.toStdWString()));
    stream << macro;
  }
  TIStream stream(TFilePath(macroPath.toStdWString()));
  TFxP restored;
  stream >> restored;
  auto *loadedMacro = dynamic_cast<TMacroFx *>(restored.getPointer());
  require(loadedMacro != nullptr, "Macro failed to reload");
  TFx *member = loadedMacro->getFxById(L"CelColorKeep1");
  require(member && loadedMacro->isParamExposed(member, "color100"),
          "Macro lost exposed color");
  require(color(*member, 99)->getValueD(5) == color(*first, 99)->getValueD(5),
          "Macro changed saved color precision");
}
}  // namespace

int main(int argc, char **argv) {
  QCoreApplication app(argc, argv);
  QTemporaryDir temp;
  try {
    require(temp.isValid(), "Temporary directory unavailable");
    std::cout << "Checking 8-bit colors" << std::endl;
    testInteger<TPixel32>();
    std::cout << "Checking 16-bit colors" << std::endl;
    testInteger<TPixel64>();
    std::cout << "Checking float colors" << std::endl;
    testFloat();
    std::cout << "Checking scene precision" << std::endl;
    testPrecisionAndPersistence(temp.filePath("colors.fx"), false);
    std::cout << "Checking compressed scene precision" << std::endl;
    testPrecisionAndPersistence(temp.filePath("colors-compressed.fx"), true);
    std::cout << "Checking tiles and threads" << std::endl;
    testTilesAndThreads();
    std::cout << "Checking cancellation" << std::endl;
    testCancellation<TPixel32>();
    testCancellation<TPixel64>();
    testCancellation<TPixelF>();
    std::cout << "Checking presets and macros" << std::endl;
    testPresetAndMacro(temp.filePath("colors.preset"), temp.filePath("colors.macro"));
    std::cout << "PASS: color capacity, precision, alpha, persistence and tiles\n";
    return 0;
  } catch (const TException &error) {
    std::cerr << QString::fromStdWString(error.getMessage()).toStdString() << '\n';
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
  }
  return 1;
}
