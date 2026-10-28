#include "cadmiumtypes.h"
#include "trasterimage.h"

#include <cmath>
#include <cstdlib>
#include <iostream>
#include <limits>

namespace {
void require(bool condition, const char *message) {
  if (condition) return;
  std::cerr << message << '\n';
  std::exit(1);
}
template <class Raster, class Pixel>
void nativePrecision(const Pixel &black, const Pixel &transparent) {
  Raster raster(9, 9);
  raster->fill(transparent);
  for (int y = 2; y <= 6; ++y)
    for (int x = 2; x <= 6; ++x)
      if (x == 2 || x == 6 || y == 2 || y == 6)
        raster->pixels(y)[x] = black;
  TRasterImageP image(raster);
  image->setDpi(120.0, 96.0);
  image->setOffset(TPoint(7, -3));
  Cadmium::Frame frame;
  QString error;
  require(Cadmium::snapshotFullColor(TImageP(image), frame, error),
          "transparent native raster snapshot failed");
  require(frame.regionStyles.size() == 2, "expected one enclosed region");
  require(frame.labels[4 * 9 + 4] == 1 && frame.labels[0] == 0,
          "interior and exterior labels are incorrect");
  QVector<QColor> colors(2);
  colors[1] = QColor(64, 128, 255);
  TRasterImageP output = Cadmium::applyFullColor(frame, {}, colors, false, error);
  require(bool(output), "native raster application failed");
  Raster result = output->getRaster();
  require(bool(result), "application changed raster sample format");
  require(raster->pixels(4)[4] == transparent, "source raster was modified");
  require(result->pixels(2)[2] == black, "opaque ink was modified");
  require(result->pixels(0)[0] == transparent, "exterior was modified");
  require(result->pixels(4)[4] != transparent, "accepted interior was not painted");
  require(output->getOffset() == TPoint(7, -3), "image offset was lost");
  double x, y; output->getDpi(x, y);
  require(x == 120.0 && y == 96.0, "image DPI was lost");
  frame.protectionMask = QImage(9, 9, QImage::Format_ARGB32);
  frame.protectionMask.fill(Qt::black);
  frame.protectionMask.setPixel(4, 4, qRgb(255, 255, 255));
  output = Cadmium::applyFullColor(frame, {}, colors, true, error);
  result = output->getRaster();
  require(result->pixels(4)[4] == transparent, "protection mask was ignored");
  frame.labels[4 * 9 + 4] = 99;
  require(!Cadmium::applyFullColor(frame, {}, colors, true, error),
          "out-of-range label was accepted");
}
}

int main() {
  nativePrecision<TRaster32P>(TPixel32::Black, TPixel32::Transparent);
  nativePrecision<TRaster64P>(TPixel64::Black, TPixel64::Transparent);
  nativePrecision<TRasterFP>(TPixelF::Black, TPixelF::Transparent);

  TRaster32P paper(9, 9);
  paper->fill(TPixel32::White);
  for (int y = 2; y <= 6; ++y)
    for (int x = 2; x <= 6; ++x)
      if (x == 2 || x == 6 || y == 2 || y == 6)
        paper->pixels(y)[x] = TPixel32::Black;
  Cadmium::Frame frame;
  frame.rasterSource = Cadmium::RasterSource::PaperScan;
  QString error;
  TRasterImageP paperImage(paper);
  require(!Cadmium::snapshotFullColor(TImageP(paperImage), frame, error),
          "paper extraction was performed without approval");
  frame.extractionApproved = true;
  require(Cadmium::snapshotFullColor(TImageP(paperImage), frame, error),
          "approved paper extraction failed");
  QVector<QColor> colors(frame.regionStyles.size());
  colors[1] = QColor(Qt::red);
  TRasterImageP output = Cadmium::applyFullColor(frame, {}, colors, false, error);
  require(bool(output), "paper application failed");
  TRaster32P result = output->getRaster();
  require(result->pixels(4)[4] == TPixel32::Red, "paper interior was not colored");
  require(result->pixels(0)[0] == TPixel32::White, "paper exterior was modified");

  // Preserve meaningful float precision outside the accepted region and
  // perform accepted color conversion in the native linear sample space.
  TRasterFP linear(9, 9);
  linear->clear(); linear->setLinear(true);
  for (int y = 2; y <= 6; ++y)
    for (int x = 2; x <= 6; ++x)
      if (x == 2 || x == 6 || y == 2 || y == 6)
        linear->pixels(y)[x] = TPixelF::Black;
  linear->pixels(0)[0] = TPixelF(1.234567f, 0.345678f, 0.123456f, 0.5f);
  frame = Cadmium::Frame();
  TRasterImageP floatImage(linear);
  require(Cadmium::snapshotFullColor(TImageP(floatImage), frame, error),
          "linear float snapshot failed");
  colors = QVector<QColor>(frame.regionStyles.size());
  colors[1] = QColor(128, 128, 128);
  output = Cadmium::applyFullColor(frame, {}, colors, true, error);
  require(bool(output), "linear float application failed");
  TRasterFP fresult = output->getRaster();
  require(fresult->isLinear(), "linear sample-space flag was lost");
  require(fresult->pixels(0)[0] == linear->pixels(0)[0], "HDR sample was reduced");
  const double display = 128.0 / 255.0;
  const double expected = std::pow((display + 0.055) / 1.055, 2.4);
  require(std::abs(fresult->pixels(4)[4].r - expected) < 1e-6,
          "accepted display color was not converted to linear samples");

  frame = Cadmium::Frame();
  frame.rasterSource = Cadmium::RasterSource::LinkedLines;
  frame.lineSource = TImageP(new TRasterImage(TRaster32P(8, 9)));
  require(!Cadmium::snapshotFullColor(TImageP(floatImage), frame, error),
          "mismatched linked clean-line geometry was accepted");
  linear->pixels(4)[4].r = std::numeric_limits<float>::quiet_NaN();
  frame = Cadmium::Frame();
  require(!Cadmium::snapshotFullColor(TImageP(floatImage), frame, error),
          "nonfinite float input was accepted");
  return 0;
}
