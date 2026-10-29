#include "cadmiumtypes.h"
#include "trasterimage.h"

#include <algorithm>
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
  require(Cadmium::matchesFullColor(TImageP(image), frame, TImageP(), error),
          "unchanged source failed the freshness check");
  raster->pixels(4)[4] = black;
  require(!Cadmium::matchesFullColor(TImageP(image), frame, TImageP(), error),
          "edited source passed the freshness check");
  raster->pixels(4)[4] = transparent;
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

template <class Raster, class Pixel>
void exactReferenceTransfer(const Pixel &first, const Pixel &second, bool linear) {
  Raster target(13, 9), reference(13, 9);
  target->clear(); reference->clear();
  target->setLinear(linear); reference->setLinear(linear);
  TRaster32P cleanLines(13, 9);
  cleanLines->clear();
  for (int left : {1, 7})
    for (int y = 2; y <= 6; ++y)
      for (int x = left; x <= left + 4; ++x) {
        if (x == left || x == left + 4 || y == 2 || y == 6) {
          cleanLines->pixels(y)[x] = TPixel32::Black;
          target->pixels(y)[x] = Pixel::Black;
          reference->pixels(y)[x] = Pixel::Black;
        } else reference->pixels(y)[x] = left == 1 ? first : second;
      }
  target->pixels(0)[0] = first;
  reference->pixels(0)[0] = second;
  TRasterImageP targetImage(target), referenceImage(reference), cleanImage(cleanLines);
  Cadmium::Frame targetFrame, referenceFrame;
  for (Cadmium::Frame *frame : {&targetFrame, &referenceFrame}) {
    frame->rasterSource = Cadmium::RasterSource::LinkedLines;
    frame->lineSource = TImageP(cleanImage);
  }
  QString error;
  require(Cadmium::snapshotFullColor(TImageP(targetImage), targetFrame, error) &&
              Cadmium::snapshotFullColor(TImageP(referenceImage), referenceFrame, error),
          "native reference fill snapshot failed");
  const int firstLabel = targetFrame.labels[4 * 13 + 3];
  const int secondLabel = targetFrame.labels[4 * 13 + 9];
  require(firstLabel > 0 && secondLabel > 0 && firstLabel != secondLabel &&
              referenceFrame.labels[4 * 13 + 3] == firstLabel &&
              referenceFrame.labels[4 * 13 + 9] == secondLabel,
          "native reference fixture lost distinct region identities");
  require(referenceFrame.regionColors[firstLabel] ==
              referenceFrame.regionColors[secondLabel],
          "native reference fixture must have colliding display proxies");
  require(referenceFrame.nativeRegionColors[firstLabel].valid &&
              referenceFrame.nativeRegionColors[secondLabel].valid &&
              referenceFrame.nativeRegionColors[firstLabel] !=
                  referenceFrame.nativeRegionColors[secondLabel],
          "distinct native fills collapsed into one display proxy color");
  TRasterImageP output = Cadmium::applyFullColor(
      targetFrame, {}, referenceFrame.regionColors, false, error,
      referenceFrame.nativeRegionColors);
  require(bool(output), "exact native reference application failed");
  Raster result = output->getRaster();
  require(bool(result) && result->isLinear() == linear,
          "native reference application changed sample format or space");
  for (int y = 0; y < 9; ++y)
    for (int x = 0; x < 13; ++x) {
      const bool firstInterior = x > 1 && x < 5 && y > 2 && y < 6;
      const bool secondInterior = x > 7 && x < 11 && y > 2 && y < 6;
      const Pixel expected = firstInterior ? first :
          secondInterior ? second : target->pixels(y)[x];
      require(result->pixels(y)[x] == expected,
              "transferred native reference fill lost precision or changed protected pixels");
      require(target->pixels(y)[x] ==
                  (firstInterior || secondInterior ? Pixel::Transparent : expected),
              "native reference application modified the source drawing");
      if (firstInterior || secondInterior)
        require(reference->pixels(y)[x] == (firstInterior ? first : second),
                "native reference application modified the painted reference");
    }
  QVector<Cadmium::NativeColor> missing = referenceFrame.nativeRegionColors;
  missing[firstLabel] = Cadmium::NativeColor();
  require(!Cadmium::applyFullColor(targetFrame, {}, referenceFrame.regionColors,
                                  false, error, missing),
          "an accepted proxy without an exact native fill was painted");
  missing.removeLast();
  require(!Cadmium::applyFullColor(targetFrame, {}, referenceFrame.regionColors,
                                  false, error, missing),
          "a mismatched native color table was accepted");
  // Several distinct native fills may share a flat-looking display proxy.
  // That proxy must never stand in for a nonexistent dominant native fill.
  reference->pixels(3)[2] = second;
  reference->pixels(3)[3] = second;
  reference->pixels(3)[4] = second;
  reference->pixels(4)[2] = second;
  Cadmium::Frame nonflat = referenceFrame;
  require(Cadmium::snapshotFullColor(TImageP(referenceImage), nonflat, error),
          "nonflat native reference snapshot failed");
  require(nonflat.regionColors[firstLabel].isValid() &&
              !nonflat.nativeRegionColors[firstLabel].valid &&
              !nonflat.warnings.isEmpty(),
          "a display proxy concealed the lack of an exact native majority");
  require(!Cadmium::applyFullColor(targetFrame, {}, nonflat.regionColors,
                                  false, error, nonflat.nativeRegionColors),
          "a nonflat native reference was reduced to its display proxy");
}

template <class Raster, class Pixel>
void paperChannels(const Pixel &redLow, const Pixel &greenLow,
                   const Pixel &blueLow, const Pixel &paper, bool linear) {
  Raster raster(9, 9);
  raster->fill(Pixel::White); raster->setLinear(linear);
  for (int y = 2; y <= 6; ++y)
    for (int x = 2; x <= 6; ++x)
      if (x == 2 || x == 6 || y == 2 || y == 6)
        raster->pixels(y)[x] = Pixel::Black;
  raster->pixels(3)[3] = redLow;
  raster->pixels(3)[4] = greenLow;
  raster->pixels(3)[5] = blueLow;
  raster->pixels(4)[4] = paper;
  TRasterImageP image(raster);
  Cadmium::Frame frame;
  QString error;
  // Default TransparentLines must detect that every native alpha is opaque.
  require(!Cadmium::snapshotFullColor(TImageP(image), frame, error) &&
              error.contains(QStringLiteral("approve")),
          "opaque native paper bypassed extraction approval");
  frame.extractionApproved = true;
  require(Cadmium::snapshotFullColor(TImageP(image), frame, error),
          "approved native paper channel fixture failed");
  require(frame.regionStyles.size() == 2 && frame.labels[4 * 9 + 4] == 1,
          "paper channel thresholds changed the enclosed region");
  // The input display channels are permutations of (250, 247, 232)/255.
  // Removing white paper leaves alpha 23/255 and straight RGB ratios
  // 18/23, 15/23, 0, which round to these independent byte expectations.
  require(frame.lines.pixel(3, 5) == qRgba(0, 200, 166, 23) &&
              frame.lines.pixel(4, 5) == qRgba(200, 0, 166, 23) &&
              frame.lines.pixel(5, 5) == qRgba(200, 166, 0, 23),
          "paper extraction changed a channel threshold, gamma conversion, or rounding");
  require(frame.lines.pixel(4, 4) == qRgba(0, 0, 0, 0) &&
              frame.lines.pixel(2, 6) == qRgba(0, 0, 0, 255),
          "paper removal or opaque ink changed");
  require(raster->pixels(3)[3] == redLow && raster->pixels(4)[4] == paper,
          "paper analysis changed the native source");
}

double linearChannel(double display) {
  return display <= 0.04045 ? display / 12.92
      : std::pow((display + 0.055) / 1.055, 2.4);
}
double displayChannel(double linear) {
  return linear <= 0.0031308 ? linear * 12.92
      : 1.055 * std::pow(linear, 1.0 / 2.4) - 0.055;
}

template <class Pixel> Pixel nativePixel(double r, double g, double b, double a);
template <> TPixel32 nativePixel<TPixel32>(double r, double g, double b, double a) {
  const auto channel = [](double value) {
    return int(std::floor(std::max(0.0, std::min(1.0, value)) * 255.0 + 0.5));
  };
  return TPixel32(channel(r), channel(g), channel(b), channel(a));
}
template <> TPixel64 nativePixel<TPixel64>(double r, double g, double b, double a) {
  const auto channel = [](double value) {
    return int(std::floor(std::max(0.0, std::min(1.0, value)) * 65535.0 + 0.5));
  };
  return TPixel64(channel(r), channel(g), channel(b), channel(a));
}
template <> TPixelF nativePixel<TPixelF>(double r, double g, double b, double a) {
  return TPixelF(float(r), float(g), float(b), float(a));
}

template <class Raster, class Pixel>
void convertedRegionFills(bool linear) {
  Raster raster(6, 2);
  raster->clear(); raster->setLinear(linear);
  raster->pixels(1)[0] = Pixel::White;
  const Pixel marker = nativePixel<Pixel>(0.25, 0.125, 0.375, 0.5);
  raster->pixels(0)[5] = marker;
  TRasterFP ink(6, 2);
  ink->clear(); ink->setLinear(!linear);
  ink->pixels(1)[1] = TPixelF(0.125f, 0.25f, 0.375f, 0.5f);
  Cadmium::Frame frame;
  frame.kind = Cadmium::Kind::FullColor;
  frame.original = TImageP(new TRasterImage(raster));
  frame.rasterSource = Cadmium::RasterSource::LinkedLines;
  frame.lineSource = TImageP(new TRasterImage(ink));
  frame.lines = QImage(6, 2, QImage::Format_ARGB32);
  frame.lines.fill(Qt::transparent);
  frame.lines.setPixel(1, 0, qRgba(64, 128, 191, 128));
  frame.labels = {1, 1, 1, 2, 2, 0, 0, 0, 0, 0, 0, 0};
  frame.regionStyles = {-1, -1, -1};
  frame.protectionMask = QImage(6, 2, QImage::Format_ARGB32);
  frame.protectionMask.fill(Qt::black);
  frame.protectionMask.setPixel(2, 0, qRgb(255, 255, 255));
  const QVector<QColor> colors{QColor(), QColor(Qt::red), QColor(Qt::blue)};
  const QVector<Cadmium::NativeColor> native{
      {}, {0.25, 0.5, 0.75, 0.5, !linear, true},
      {0.75, 0.25, 0.5, 0.75, !linear, true}};
  const auto convert = [linear](double value) {
    return linear ? linearChannel(value) : displayChannel(value);
  };
  // A half-covered line over a half-alpha fill has alpha 3/4. Both have
  // identical straight channels here, so each output channel is 3/4 of the
  // converted straight value. The second region has no line contribution.
  const Pixel first = nativePixel<Pixel>(0.5 * convert(0.25),
      0.5 * convert(0.5), 0.5 * convert(0.75), 0.5);
  const Pixel edge = nativePixel<Pixel>(0.75 * convert(0.25),
      0.75 * convert(0.5), 0.75 * convert(0.75), 0.75);
  const Pixel second = nativePixel<Pixel>(0.75 * convert(0.75),
      0.75 * convert(0.25), 0.75 * convert(0.5), 0.75);
  for (bool repaint : {false, true}) {
    QString error;
    TRasterImageP output = Cadmium::applyFullColor(frame, {}, colors, repaint, error, native);
    require(bool(output), "mixed-space native region application failed");
    Raster result = output->getRaster();
    require(bool(result) && result->isLinear() == linear,
            "region conversion changed native format or color space");
    for (int y = 0; y < 2; ++y)
      for (int x = 0; x < 6; ++x) {
        Pixel expected = raster->pixels(y)[x];
        if (y == 1 && x == 0 && repaint) expected = first;
        if (y == 1 && x == 1) expected = edge;
        if (y == 1 && (x == 3 || x == 4)) expected = second;
        require(result->pixels(y)[x] == expected,
                "per-region fill reuse changed native samples, alpha, protection, or repaint policy");
      }
    require(raster->pixels(1)[0] == Pixel::White && raster->pixels(0)[5] == marker,
            "region fill application changed its source");
  }
}
}

int main() {
  nativePrecision<TRaster32P>(TPixel32::Black, TPixel32::Transparent);
  nativePrecision<TRaster64P>(TPixel64::Black, TPixel64::Transparent);
  nativePrecision<TRasterFP>(TPixelF::Black, TPixelF::Transparent);
  exactReferenceTransfer<TRaster64P>(TPixel64(12345, 23456, 34567, 65535),
                                    TPixel64(12346, 23456, 34567, 65535), false);
  exactReferenceTransfer<TRasterFP>(TPixelF(-0.1234567f, 1.234567f, 2.345678f, 0.456789f),
                                   TPixelF(-0.1234566f, 1.234566f, 2.345677f, 0.456789f), true);
  paperChannels<TRaster32P>(TPixel32(232, 250, 247), TPixel32(250, 232, 247),
      TPixel32(250, 247, 232), TPixel32(250, 249, 247), false);
  paperChannels<TRaster64P>(TPixel64(232 * 257, 250 * 257, 247 * 257),
      TPixel64(250 * 257, 232 * 257, 247 * 257),
      TPixel64(250 * 257, 247 * 257, 232 * 257),
      TPixel64(250 * 257, 249 * 257, 247 * 257), false);
  for (bool linear : {false, true}) {
    const auto channel = [linear](int value) {
      const double display = value / 255.0;
      return float(linear ? linearChannel(display) : display);
    };
    paperChannels<TRasterFP>(TPixelF(channel(232), channel(250), channel(247), 1),
        TPixelF(channel(250), channel(232), channel(247), 1),
        TPixelF(channel(250), channel(247), channel(232), 1),
        TPixelF(channel(250), channel(249), channel(247), 1), linear);
    convertedRegionFills<TRaster32P, TPixel32>(linear);
    convertedRegionFills<TRaster64P, TPixel64>(linear);
    convertedRegionFills<TRasterFP, TPixelF>(linear);
  }

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

  TRaster32P cleanLines(9, 9);
  cleanLines->clear();
  for (int y = 2; y <= 6; ++y)
    for (int x = 2; x <= 6; ++x)
      if (x == 2 || x == 6 || y == 2 || y == 6)
        cleanLines->pixels(y)[x] = TPixel32::Black;
  TRasterImageP cleanImage(cleanLines);
  Cadmium::Frame linked;
  linked.rasterSource = Cadmium::RasterSource::LinkedLines;
  linked.lineSource = TImageP(cleanImage);
  require(Cadmium::snapshotFullColor(TImageP(paperImage), linked, error),
          "linked clean-line snapshot failed");
  require(Cadmium::matchesFullColor(TImageP(paperImage), linked,
                                   TImageP(cleanImage), error),
          "unchanged linked drawing failed freshness check");
  cleanLines->pixels(0)[0] = TPixel32::Black;
  require(!Cadmium::matchesFullColor(TImageP(paperImage), linked,
                                    TImageP(cleanImage), error),
          "edited linked lines passed freshness check");
  cleanLines->pixels(0)[0] = TPixel32::Transparent;
  cleanImage->setDpi(144, 144);
  require(!Cadmium::matchesFullColor(TImageP(paperImage), linked,
                                    TImageP(cleanImage), error),
          "linked image metadata edit passed freshness check");
  cleanImage->setDpi(0, 0);
  Cadmium::Frame changedOptions = linked;
  changedOptions.paperThreshold = 0.5;
  require(!Cadmium::matchesFullColor(TImageP(paperImage), changedOptions, linked, error),
          "changed extraction settings passed freshness check");

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
