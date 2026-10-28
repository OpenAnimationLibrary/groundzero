#include "../cadmiumtypes.h"
#include "ttoonzimage.h"
#include <QCoreApplication>
#include <algorithm>
#include <iostream>
#include <stdexcept>

namespace {
void require(bool value, const char *message) {
  if (!value) throw std::runtime_error(message);
}

TToonzImageP drawing(int width, int height) {
  TRasterCM32P raster(width, height);
  for (int y = 0; y < height; ++y)
    for (int x = 0; x < width; ++x)
      raster->pixels(y)[x] = TPixelCM32(0, 0, 255);
  TToonzImageP image(raster, raster->getBounds());
  TPaletteP palette = new TPalette;
  palette->addStyle(TPixel32(240, 20, 10));
  palette->addStyle(TPixel32(10, 40, 230));
  image->setPalette(palette.getPointer());
  image->setOffset(TPoint(7, -9));
  image->setDpi(120.0, 144.0);
  return image;
}

void orientationAndPreservation() {
  TToonzImageP image = drawing(7, 9);
  TRasterCM32P raster = image->getRaster();
  // A closed native box whose top and bottom are deliberately asymmetric.
  for (int y = 2; y <= 6; ++y)
    for (int x = 1; x <= 5; ++x)
      if (x == 1 || x == 5 || y == 2 || y == 6)
        raster->pixels(y)[x] = TPixelCM32(1, 0, 0);
  raster->pixels(3)[2] = TPixelCM32(0, 2, 255);
  raster->pixels(5)[2] = TPixelCM32(1, 0, 64);
  Cadmium::Frame frame;
  QString error;
  require(Cadmium::snapshotToonzRaster(image, frame, error), "Snapshot failed");
  require(frame.lines.pixelColor(2, 3).alpha() == 191,
          "The line image must use top-left coordinates and native ink coverage");
  require(frame.labels[3 * 7 + 2] > 0,
          "Antialias paint pixels must inherit an enclosed fill label");
  require(TToonzImageP(frame.original)->getOffset() == image->getOffset(),
          "Snapshot lost the native image offset");
  QVector<int> styles(frame.regionStyles.size(), 3);
  styles[0] = -100;
  TToonzImageP result(Cadmium::applyToonzRaster(frame, styles, {}, false, error));
  require(bool(result), "Apply failed");
  require(result->getRaster()->pixels(3)[2].getPaint() == 2,
          "Protected existing paint was overwritten");
  require(result->getRaster()->pixels(4)[3].getPaint() == 3,
          "An enclosed unpainted region was not painted");
  require(result->getRaster()->pixels(5)[2].getPaint() == 3,
          "Partial-coverage line paint was not updated");
  for (int y = 0; y < 9; ++y)
    for (int x = 0; x < 7; ++x) {
      const TPixelCM32 before = raster->pixels(y)[x];
      const TPixelCM32 after = result->getRaster()->pixels(y)[x];
      require(before.getInk() == after.getInk() && before.getTone() == after.getTone(),
              "Applying paint changed ink or tone");
    }
  require(raster->pixels(4)[3].getPaint() == 0,
          "Apply changed the original source image");
  require(result->getOffset() == image->getOffset(), "Apply lost image offset");
  double x, y;
  result->getDpi(x, y);
  require(x == 120.0 && y == 144.0, "Apply changed image DPI");
  TToonzImageP repainted(Cadmium::applyToonzRaster(frame, styles, {}, true, error));
  require(repainted->getRaster()->pixels(3)[2].getPaint() == 3,
          "Explicit repaint did not replace existing paint");
  styles[1] = 4096;
  require(!Cadmium::applyToonzRaster(frame, styles, {}, true, error),
          "An out-of-range native style was accepted");
  require(!error.isEmpty(), "Invalid input needs an actionable error");
}

void losslessRegionLabels() {
  TToonzImageP image = drawing(35, 35);
  TRasterCM32P raster = image->getRaster();
  for (int y = 0; y < 35; ++y)
    for (int x = 0; x < 35; ++x)
      if (!(x % 2) || !(y % 2)) raster->pixels(y)[x] = TPixelCM32(1, 0, 0);
  Cadmium::Frame frame;
  QString error;
  require(Cadmium::snapshotToonzRaster(image, frame, error), "Grid snapshot failed");
  require(frame.regionStyles.size() == 290, "The native region count was truncated");
  int maximum = 0;
  for (int label : frame.labels) maximum = std::max(maximum, label);
  require(maximum == 289, "Region labels wrapped at eight bits");
  require(Cadmium::matchesToonzRaster(image, frame, error),
          "An unchanged source failed the fast freshness check");
  const QByteArray savedFingerprint = frame.fingerprint;
  Cadmium::Frame again;
  require(Cadmium::snapshotToonzRaster(image, again, error), "Repeat snapshot failed");
  require(again.fingerprint == frame.fingerprint, "Fingerprint is not deterministic");
  raster->pixels(1)[1].setPaint(2);
  require(!Cadmium::matchesToonzRaster(image, frame, error),
          "The fast freshness check accepted a pixel edit");
  require(Cadmium::snapshotToonzRaster(image, again, error), "Edited snapshot failed");
  require(again.fingerprint != frame.fingerprint, "Pixel edit did not invalidate snapshot");
  QVector<int> styles(again.regionStyles.size(), -100);
  again.labels[1] = styles.size();
  require(!Cadmium::applyToonzRaster(again, styles, {}, false, error),
          "An invalid region label was accepted");
  raster->pixels(1)[1].setPaint(0);
  require(Cadmium::matchesToonzRaster(image, frame, error),
          "Restoring native pixels did not restore freshness");
  image->getPalette()->setStyle(2, TPixel32(140, 25, 90));
  require(!Cadmium::matchesToonzRaster(image, frame, error),
          "The fast freshness check accepted a palette color edit");
  require(frame.fingerprint == savedFingerprint,
          "Freshness validation modified the saved proposal fingerprint");
}
}  // namespace

int main(int argc, char **argv) {
  QCoreApplication application(argc, argv);
  try {
    orientationAndPreservation();
    losslessRegionLabels();
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
  return 0;
}
