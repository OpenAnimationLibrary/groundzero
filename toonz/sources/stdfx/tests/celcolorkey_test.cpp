#include "../celcolorkeyfx.cpp"
#include "tparamcontainer.h"
#include "tstream.h"
#include <QCoreApplication>
#include <QTemporaryDir>
#include <iostream>
#include <memory>
#include <stdexcept>

namespace {
void require(bool condition, const char *message) {
  if (!condition) throw std::runtime_error(message);
}
TDoubleParamP number(TFx &fx, const std::string &name) {
  return TParamP(fx.getParams()->getParam(name));
}
TPixelParamP color(TFx &fx, const std::string &name) {
  return TParamP(fx.getParams()->getParam(name));
}
class Source final : public TStandardRasterFx {
  TRasterP m_raster;
public:
  explicit Source(const TRasterP &raster) : m_raster(raster) {}
  const TPersistDeclaration *getDeclaration() const override { return nullptr; }
  bool canHandle(const TRenderSettings &, double) override { return true; }
  bool doGetBBox(double, TRectD &box, const TRenderSettings &) override {
    box = TRectD(0, 0, m_raster->getLx(), m_raster->getLy()); return true;
  }
  std::string getAlias(double, const TRenderSettings &) const override { return "key-test-source"; }
  void compute(TTile &tile, double, const TRenderSettings &) override {
    tile.getRaster()->clear();
    tile.getRaster()->copy(m_raster, TPoint(-int(tile.m_pos.x), -int(tile.m_pos.y)));
  }
  void doCompute(TTile &tile, double frame, const TRenderSettings &settings) override { compute(tile, frame, settings); }
};
template <class Pixel>
TRasterPT<Pixel> render(CelColorKeyFx &fx, int width, TPointD position = TPointD()) {
  TRasterPT<Pixel> result(width, 1); TTile tile; tile.setRaster(result); tile.m_pos = position;
  fx.doCompute(tile, 0, TRenderSettings()); return result;
}
template <class Pixel>
void testRaster() {
  CelColorKeyFx fx;
  TRasterPT<Pixel> source(3, 1);
  source->pixels(0)[0] = PixelConverter<Pixel>::from(TPixelD(1, 0, 0, 1));
  source->pixels(0)[1] = PixelConverter<Pixel>::from(TPixelD(0, 1, 0, 1));
  source->pixels(0)[2] = PixelConverter<Pixel>::from(TPixelD(1, 0, 0, 1));
  fx.getInputPort(0)->setFx(new Source(source));
  number(fx, "use001")->setDefaultValue(1);
  color(fx, "key001")->setDefaultValue(TPixel32(0, 255, 0));
  auto keyed = render<Pixel>(fx, 3);
  require(keyed->pixels(0)[0] == source->pixels(0)[0] && keyed->pixels(0)[1] == Pixel::Transparent,
          "Remove mode did not remove the selected green");
  number(fx, "keep")->setDefaultValue(1);
  auto kept = render<Pixel>(fx, 3);
  require(kept->pixels(0)[0] == Pixel::Transparent && kept->pixels(0)[1] == source->pixels(0)[1],
          "Keep mode selected the wrong pixels");
  number(fx, "enableReplace")->setDefaultValue(1);
  number(fx, "replace001")->setDefaultValue(1);
  color(fx, "replacement001")->setDefaultValue(TPixel32(255, 255, 255));
  auto replaced = render<Pixel>(fx, 3);
  require(replaced->pixels(0)[1] == Pixel::White, "Replacement color was not applied");
  number(fx, "enableReplace")->setDefaultValue(0);
  number(fx, "thin")->setDefaultValue(1);
  auto expanded = render<Pixel>(fx, 3);
  require(expanded->pixels(0)[0] == source->pixels(0)[0] && expanded->pixels(0)[2] == source->pixels(0)[2],
          "Positive edge thin did not expand the selected region");
  number(fx, "thin")->setDefaultValue(-1);
  auto contracted = render<Pixel>(fx, 3);
  require(contracted->pixels(0)[1] == Pixel::Transparent, "Negative edge thin did not contract the selected region");
  number(fx, "thin")->setDefaultValue(0);
  number(fx, "enabledColors")->setDefaultValue(25);
  number(fx, "use001")->setDefaultValue(0);
  number(fx, "use025")->setDefaultValue(1);
  color(fx, "key025")->setDefaultValue(TPixel32(255, 0, 0));
  auto last = render<Pixel>(fx, 3);
  require(last->pixels(0)[0] == source->pixels(0)[0] && last->pixels(0)[1] == Pixel::Transparent,
          "The last color entry did not select red");
  auto padded = render<Pixel>(fx, 5, TPointD(-1, 0));
  require(padded->pixels(0)[0] == Pixel::Transparent && padded->pixels(0)[1] == last->pixels(0)[0] &&
              padded->pixels(0)[3] == last->pixels(0)[2] && padded->pixels(0)[4] == Pixel::Transparent,
          "Negative-origin tile differed from the full result");
  int canceled = 1; TRenderSettings settings; settings.m_isCanceled = &canceled;
  TTile tile; TRasterPT<Pixel> output(3, 1); output->fill(Pixel::White); tile.setRaster(output);
  fx.doCompute(tile, 0, settings);
  require(output->pixels(0)[0] == Pixel::Transparent, "Canceled output was not cleared");
}
void testPersistence(const QString &path) {
  CelColorKeyFx fx;
  number(fx, "enabledColors")->setDefaultValue(25);
  number(fx, "use025")->setValue(3, 1);
  color(fx, "key025")->getRed()->setDefaultValue(0.123456789);
  { TOStream stream(TFilePath(path.toStdWString())); stream << &fx; }
  TIStream stream(TFilePath(path.toStdWString())); TPersist *saved = nullptr; stream >> saved;
  std::unique_ptr<TPersist> owner(saved);
  auto *loaded = dynamic_cast<CelColorKeyFx *>(saved);
  require(loaded && number(*loaded, "use025")->isKeyframe(3), "Entry animation was not restored");
  require(color(*loaded, "key025")->getValueD(0).r == 0.123456789, "Entry precision was not restored");
  require(number(*loaded, "use025")->isUICheckbox(), "Animated choice metadata was not restored");
}
}
int main(int argc, char **argv) {
  QCoreApplication app(argc, argv);
  try {
    testRaster<TPixel32>(); testRaster<TPixel64>(); testRaster<TPixelF>();
    checkInputSize(2, 3, 16);
    bool tooLarge = false;
    try { checkInputSize(16005, 16005, 16); }
    catch (const TException &) { tooLarge = true; }
    require(tooLarge, "Oversized raster dimensions were not rejected");
    QTemporaryDir directory; require(directory.isValid(), "Cannot create temporary files");
    testPersistence(directory.filePath("color-key.fx"));
    std::cout << "PASS: color key raster modes, entries and persistence\n";
    return 0;
  } catch (const TException &e) {
    std::cerr << QString::fromStdWString(e.getMessage()).toStdString() << '\n';
  } catch (const std::exception &e) { std::cerr << e.what() << '\n'; }
  return 1;
}
