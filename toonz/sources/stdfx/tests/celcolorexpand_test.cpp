#include "../celcolorexpandfx.cpp"
#include "tparamcontainer.h"
#include "tstream.h"
#include "tmacrofx.h"
#include "cellevelfixture.h"

#include <QCoreApplication>
#include <QTemporaryDir>
#include <future>
#include <iostream>
#include <stdexcept>
#include <type_traits>

namespace {
void require(bool value, const char *message) {
  if (!value) throw std::runtime_error(message);
}
TDoubleParamP radius(TFx &fx) {
  return TParamP(fx.getParams()->getParam("radius"));
}
class Source final : public TStandardRasterFx {
  TRasterP m_image;
  int *m_cancel;
  bool m_partial;
public:
  explicit Source(const TRasterP &image, int *cancel = nullptr, bool partial = false)
      : m_image(image), m_cancel(cancel), m_partial(partial) {}
  const TPersistDeclaration *getDeclaration() const override { return nullptr; }
  bool canHandle(const TRenderSettings &, double) override { return true; }
  std::string getAlias(double, const TRenderSettings &) const override { return "expand-source"; }
  bool doGetBBox(double, TRectD &box, const TRenderSettings &) override {
    box = TRectD(0, 0, m_image->getLx(), m_image->getLy()); return true;
  }
  void compute(TTile &tile, double, const TRenderSettings &) override {
    if (m_partial) {
      const TRasterP raster = tile.getRaster();
      for (int y = 0; y < raster->getLy(); ++y)
        for (int x = 0; x < raster->getLx() * raster->getPixelSize(); ++x)
          require(raster->getRawData()[size_t(y) * raster->getWrap() * raster->getPixelSize() + x] == 0,
                  "Input padding was not cleared before upstream rendering");
    } else tile.getRaster()->clear();
    tile.getRaster()->copy(m_image, TPoint(-int(tile.m_pos.x), -int(tile.m_pos.y)));
    if (m_cancel) *m_cancel = 1;
  }
  void doCompute(TTile &tile, double frame, const TRenderSettings &settings) override {
    compute(tile, frame, settings);
  }
};
template <class Pixel>
TRasterPT<Pixel> render(CelColorExpandFx &fx, int w, int h,
                       TPointD pos = TPointD(), double frame = 0,
                       const TRenderSettings &settings = TRenderSettings()) {
  TRasterPT<Pixel> result(w, h);
  result->fill(Pixel::White);
  TTile tile; tile.setRaster(result); tile.m_pos = pos;
  fx.doCompute(tile, frame, settings);
  return result;
}
template <class Pixel>
void checkDepth() {
  const double top = std::is_same<Pixel, TPixelF>::value ? 1 : Pixel::maxChannelValue;
  TRasterPT<Pixel> input(9, 7); input->clear();
  input->pixels(1)[2] = Pixel(top, 0, 0, top);
  input->pixels(1)[6] = Pixel(0, top, 0, top);
  input->pixels(5)[4] = Pixel(top / 4, 0, top / 2, top / 2);
  CelColorExpandFx fx; fx.getInputPort(0)->setFx(new Source(input, nullptr, true));
  for (double r : {0.0, 0.99, 1.0, 1.4142135623730951, 2.0, 3.25, 20.0}) {
    radius(fx)->setDefaultValue(r);
    const auto full = render<Pixel>(fx, 15, 13, TPointD(-3, -3));
    for (int y = 0; y < 13; ++y) for (int x = 0; x < 15; ++x) {
      double best = std::numeric_limits<double>::infinity();
      Pixel expected = Pixel::Transparent;
      // Exhaustive oracle, independent of the production distance transform.
      for (int sy = 0; sy < 7; ++sy) for (int sx = 0; sx < 9; ++sx) {
        const Pixel p = input->pixels(sy)[sx];
        if (p.m == 0) continue;
        const double d = std::hypot(double(sx - (x - 3)), double(sy - (y - 3)));
        if (d <= r && d < best) { best = d; expected = p; }
      }
      require(full->pixels(y)[x] == expected, "Whole-color radius or tie mismatch");
    }
    std::vector<std::future<void>> jobs;
    for (int y = 0; y < 13; ++y) jobs.emplace_back(std::async(std::launch::async, [&, y] {
      auto strip = render<Pixel>(fx, 15, 1, TPointD(-3, y - 3));
      for (int x = 0; x < 15; ++x)
        require(strip->pixels(0)[x] == full->pixels(y)[x], "Tile or thread seam");
    }));
    for (auto &job : jobs) job.get();
  }
  input->clear(); radius(fx)->setDefaultValue(10);
  require(render<Pixel>(fx, 1, 1)->pixels(0)[0] == Pixel::Transparent,
          "Empty source gained color");
  input->pixels(0)[0] = Pixel(top / 2, 0, 0, top / 2);
  require(render<Pixel>(fx, 1, 1)->pixels(0)[0] == input->pixels(0)[0],
          "One-pixel partial coverage changed");
  int canceled = 1; TRenderSettings settings; settings.m_isCanceled = &canceled;
  fx.getInputPort(0)->setFx(new Source(input, &canceled));
  for (int initial : {1, 0}) {
    canceled = initial;
    auto output = render<Pixel>(fx, 3, 2, TPointD(), 0, settings);
    for (int y = 0; y < 2; ++y) for (int x = 0; x < 3; ++x)
      require(output->pixels(y)[x] == Pixel::Transparent, "Canceled output not empty");
  }
}
void checkGeometry() {
  TRaster32P input(1, 1); input->fill(TPixel32::Red);
  CelColorExpandFx fx; fx.getInputPort(0)->setFx(new Source(input));
  radius(fx)->setDefaultValue(2.25);
  TRenderSettings settings; settings.m_affine = TScale(2.0);
  TRectD box;
  require(fx.doGetBBox(0, box, settings) && box == TRectD(-2.25, -2.25, 3.25, 3.25),
          "Bounds applied scale twice");
  auto image = render<TPixel32>(fx, 6, 1, TPointD(), 0, settings);
  require(image->pixels(0)[4] == TPixel32::Red && image->pixels(0)[5] == TPixel32::Transparent,
          "Preview radius did not scale");
  TRenderSettings transformed; TRectD request;
  fx.transform(0, 0, TRectD(0, 0, 3, 2), settings, request, transformed);
  require(request == TRectD(-5, -5, 8, 7), "Predictive halo differs from render");
  require(!fx.canHandle(settings, 0) && fx.handledAffine(settings, 0) == TAffine(),
          "Preview must resample the completed expansion");
  settings.m_affine = TScale(2.0, 1.0);
  require(!fx.canHandle(settings, 0), "Nonuniform transform bypasses host adapter");
  radius(fx)->setDefaultValue(0);
  require(fx.canHandle(settings, 0), "Identity should accept every affine");
  bool threw = false;
  try { expandSize(10, 10, std::numeric_limits<int>::max() / 2); }
  catch (const TException &) { threw = true; }
  require(threw, "Raster size overflow was accepted");
  for (int bytes : {4, 8, 16}) {
    const int width = std::numeric_limits<int>::max() / bytes;
    require(expandSize(width, 1, 0, bytes).lx == width,
            "Safe byte-count boundary was rejected");
    bool overflow = false;
    try { expandSize(width + 1, 1, 0, bytes); }
    catch (const TException &) { overflow = true; }
    require(overflow, "Signed raster byte-count overflow was accepted");
  }
  threw = false;
  try { expandHalo(std::numeric_limits<double>::infinity()); }
  catch (const TException &) { threw = true; }
  require(threw, "Infinite radius was accepted");
  CelColorExpandFx missing;
  require(render<TPixel32>(missing, 1, 1)->pixels(0)[0] == TPixel32::Transparent,
          "Missing source retained output");
}
void checkFloat() {
  TRasterFP input(3, 1); input->clear();
  input->pixels(0)[1] = TPixelF(2, -0.25f, 0.125f, 0.5f);
  CelColorExpandFx fx; fx.getInputPort(0)->setFx(new Source(input));
  auto output = render<TPixelF>(fx, 3, 1);
  for (int x = 0; x < 3; ++x)
    require(output->pixels(0)[x] == input->pixels(0)[1], "Float HDR was altered");
  input->pixels(0)[1].r = std::numeric_limits<float>::quiet_NaN();
  require(render<TPixelF>(fx, 3, 1)->pixels(0)[0] == TPixelF::Transparent,
          "Invalid float seeded expansion");
  radius(fx)->setDefaultValue(0);
  output = render<TPixelF>(fx, 3, 1);
  require(std::isnan(output->pixels(0)[1].r), "Radius zero is not exact identity");
}

template <class Pixel> void checkLevelDpi() {
  const double top = Pixel::maxChannelValue;
  for (const TDimension &size : {TDimension(10, 8), TDimension(9, 7)}) {
    TRasterPT<Pixel> image(size); image->clear();
    for (int y = 1; y < size.ly - 1; ++y)
      for (int x = 1; x < size.lx - 1; ++x)
        image->pixels(y)[x] = x < size.lx / 3 ? Pixel(top, 0, 0, top) :
                             x > 2 * size.lx / 3 ? Pixel(0, 0, top, top) :
                             Pixel(0, top / 2, 0, top / 2);
    image->pixels(size.ly / 2)[size.lx / 2] = Pixel::Transparent;
    CelLevelTest::Level original(image);
    for (double search : {0.0, 1.0, 2.5}) {
      const int halo = int(std::ceil(search));
      TRasterPT<Pixel> expected(size.lx + 2 * halo, size.ly + 2 * halo);
      expected->clear();
      for (int y = 0; y < expected->getLy(); ++y)
        for (int x = 0; x < expected->getLx(); ++x) {
          double best = std::numeric_limits<double>::infinity();
          for (int sy = 0; sy < size.ly; ++sy) for (int sx = 0; sx < size.lx; ++sx) {
            const Pixel pixel = image->pixels(sy)[sx];
            if (!pixel.m) continue;
            const double distance = std::hypot(double(sx - x + halo), double(sy - y + halo));
            if (distance <= search && distance < best) {
              best = distance; expected->pixels(y)[x] = pixel;
            }
          }
        }
      CelLevelTest::Level oracle(expected);
      for (const TAffine &placement : std::vector<TAffine>{TAffine(), TScale(.25),
              TTranslation(1.125, -.375) * TRotation(17) * TScale(.25, .5)}) {
        TFxP input = original.placed(placement), reference = oracle.placed(placement);
        TFxP owner = new CelColorExpandFx;
        auto &fx = *static_cast<CelColorExpandFx *>(owner.getPointer());
        fx.getInputPort(0)->setFx(input.getPointer()); radius(fx)->setDefaultValue(search);
        for (const TAffine &camera : {TAffine(), placement.inv(),
                                      TTranslation(2.5, -.25) * TScale(2)}) {
          std::ostringstream description;
          description << "Expansion level " << size.lx << 'x' << size.ly << " radius " << search
                      << " placement " << placement.a11 << ',' << placement.a12 << ',' << placement.a13
                      << ',' << placement.a21 << ',' << placement.a22 << ',' << placement.a23
                      << " camera " << camera.a11 << ',' << camera.a12 << ',' << camera.a13
                      << ',' << camera.a21 << ',' << camera.a22 << ',' << camera.a23;
          const std::string label = description.str();
          const auto result = CelLevelTest::render<Pixel>(fx, camera);
          CelLevelTest::compare(CelLevelTest::render<Pixel>(
              *static_cast<TRasterFx *>(reference.getPointer()), camera), result,
              label.c_str());
          try {
            CelLevelTest::compare(result, CelLevelTest::render<Pixel>(fx, camera, 4),
                                  (label + " changes across strips").c_str());
          } catch (const std::exception &) {
            if (search == 0) {
              auto &upstream = *static_cast<TRasterFx *>(input.getPointer());
              CelLevelTest::compare(CelLevelTest::render<Pixel>(upstream, camera),
                  CelLevelTest::render<Pixel>(upstream, camera, 4),
                  (label + " upstream level changes across strips").c_str());
            }
            throw;
          }
        }
      }
    }
  }
}

void checkPersistence(const QString &directory) {
  TFxP first = new CelColorExpandFx, second = new CelColorExpandFx;
  first->setFxId(L"Expand1"); second->setFxId(L"Expand2");
  radius(*first)->setValue(0, 2.123456789012345);
  radius(*first)->setValue(10, 8.987654321012345);
  TFxP clone = first->clone(false);
  TFxP linked = new CelColorExpandFx;
  linked->linkParams(first.getPointer());
  require(radius(*linked).getPointer() == radius(*first).getPointer(), "Linked radius was copied");
  linked->unlinkParams();
  require(radius(*clone)->getValue(10) == radius(*first)->getValue(10), "Clone lost radius keys");
  const std::string before = first->getAlias(0, TRenderSettings());
  radius(*first)->setValue(0, 2.123456789012346);
  require(first->getAlias(0, TRenderSettings()) != before, "Radius aliases collide");
  for (bool compressed : {false, true}) {
    const TFilePath path((directory + (compressed ? "/compressed.fx" : "/plain.fx")).toStdWString());
    { TOStream stream(path, compressed); stream << first.getPointer(); }
    TFxP restored; { TIStream stream(path); stream >> restored; }
    require(restored && radius(*restored)->isKeyframe(10) &&
                radius(*restored)->getValue(0) == radius(*first)->getValue(0),
            "Scene did not preserve animated radius");
  }
  const TFilePath preset((directory + "/expand.preset").toStdWString());
  { TOStream stream(preset); first->savePreset(stream); }
  { TIStream stream(preset); second->loadPreset(stream); }
  require(radius(*second)->getValue(10) == radius(*first)->getValue(10), "Preset lost radius");
  second->getInputPort(0)->setFx(first.getPointer());
  TFxP owner = TMacroFx::create({first, second});
  auto *macro = dynamic_cast<TMacroFx *>(owner.getPointer());
  require(macro != nullptr, "Macro creation failed");
  macro->setParamExposed(first.getPointer(), "radius", true);
  const TFilePath macroPath((directory + "/expand.macro").toStdWString());
  { TOStream stream(macroPath); stream << macro; }
  TFxP restored; { TIStream stream(macroPath); stream >> restored; }
  auto *loaded = dynamic_cast<TMacroFx *>(restored.getPointer());
  require(loaded && loaded->getFxById(L"Expand1") &&
              loaded->isParamExposed(loaded->getFxById(L"Expand1"), "radius"),
          "Macro lost radius exposure");
}
}  // namespace
int main(int argc, char **argv) {
  QCoreApplication app(argc, argv); QTemporaryDir temp;
  TThread::init(); TRenderer::initialize(); int result = 1;
  try {
    require(temp.isValid(), "Temporary directory unavailable");
    checkDepth<TPixel32>(); checkDepth<TPixel64>(); checkDepth<TPixelF>();
    checkGeometry(); checkFloat(); checkPersistence(temp.path());
    checkLevelDpi<TPixel32>(); checkLevelDpi<TPixel64>(); checkLevelDpi<TPixelF>();
    std::cout << "PASS: nearest colors, coverage, radii, tiles, transforms and persistence\n";
    result = 0;
  } catch (const TException &error) {
    std::cerr << QString::fromStdWString(error.getMessage()).toStdString() << '\n';
  } catch (const std::exception &error) { std::cerr << error.what() << '\n'; }
  TThread::shutdown(); return result;
}
