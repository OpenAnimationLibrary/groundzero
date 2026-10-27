// FX render regression test.
//
// Renders small FX graphs built from the standard FX with their default
// parameters, the way the FX Settings swatch does, and compares every result
// with a reference PNG in the reference folder (first argument). Results that
// are missing or differ are written to the output folder (second argument),
// so a CI run can upload them for review.
//
// To add or update a reference, copy the PNG from the output folder (for
// example from the "fxrender-output" CI artifact) into the reference folder.
// A case without a reference yet is written there and reported, but does not
// fail: the Linux Tests workflow commits it as the new reference. A render
// that differs from its reference fails.
// The PNG files store the raw premultiplied pixel values as if they were
// straight RGBA, so they compare exactly.

#include "tfx.h"
#include "trasterfx.h"
#include "trenderer.h"
#include "traster.h"
#include "tthread.h"

#include <QGuiApplication>
#include <QDir>
#include <QFileInfo>
#include <QImage>
#include <QString>

#include <algorithm>
#include <cstdlib>
#include <functional>
#include <iostream>
#include <string>
#include <vector>

DV_IMPORT_API void initStdFx();

namespace {

const TDimension cSize(160, 120);
// Allowed difference per channel, and share of pixels allowed beyond it, to
// absorb floating point differences between compilers and platforms.
const int cChannelTolerance  = 3;
const double cPixelTolerance = 0.002;

TFxP create(const std::string &id) {
  TFxP fx = TFx::create(id);
  if (!fx) {
    std::cerr << "Unknown FX identifier: " << id << std::endl;
    std::exit(1);
  }
  return fx;
}

void connect(const TFxP &fx, int port, const TFxP &input) {
  TFxPort *fxPort = fx->getInputPort(port);
  if (!fxPort) {
    std::cerr << "FX " << fx->getFxType() << " has no input port " << port
              << std::endl;
    std::exit(1);
  }
  fxPort->setFx(input.getPointer());
}

TFxP unary(const std::string &id, const TFxP &input) {
  TFxP fx = create(id);
  connect(fx, 0, input);
  return fx;
}

TFxP binary(const std::string &id, const TFxP &input0, const TFxP &input1) {
  TFxP fx = create(id);
  connect(fx, 0, input0);
  connect(fx, 1, input1);
  return fx;
}

struct Case {
  const char *name;
  std::function<TFxP()> build;
};

const std::vector<Case> &cases() {
  static const std::vector<Case> list = {
      {"linear_gradient", [] { return create("STD_linearGradientFx"); }},
      {"radial_gradient", [] { return create("STD_radialGradientFx"); }},
      {"spiral", [] { return create("STD_spiralFx"); }},
      {"checkerboard", [] { return create("checkBoardFx"); }},
      {"blur_checkerboard",
       [] { return unary("STD_blurFx", create("checkBoardFx")); }},
      {"posterize_gradient",
       [] { return unary("STD_posterizeFx", create("STD_linearGradientFx")); }},
      {"over_gradients",
       [] {
         return binary("overFx", create("STD_radialGradientFx"),
                       create("checkBoardFx"));
       }},
      {"glow_gradient", [] {
         return binary("STD_glowFx", create("STD_radialGradientFx"),
                       create("checkBoardFx"));
       }}};
  return list;
}

TRaster32P render(const TFxP &fx, TRenderer &renderer) {
  TRasterFxP rasterFx = fx;
  if (!rasterFx) return TRaster32P();

  // As in SwatchViewer::ContentRender::run().
  unsigned long renderId = TRenderer::buildRenderId();
  renderer.install(renderId);
  renderer.declareRenderStart(renderId);
  renderer.declareFrameStart(0);

  TRenderSettings info;
  info.m_bpp = 32;
  TTile tile;
  rasterFx->allocateAndCompute(tile, -0.5 * TPointD(cSize.lx, cSize.ly), cSize,
                               TRaster32P(1, 1), 0, info);

  renderer.declareFrameEnd(0);
  renderer.declareRenderEnd(renderId);
  renderer.uninstall();
  return tile.getRaster();
}

// Rasters are stored bottom row first; images top row first.
QImage toImage(const TRaster32P &ras) {
  QImage image(ras->getLx(), ras->getLy(), QImage::Format_ARGB32);
  ras->lock();
  for (int y = 0; y < ras->getLy(); ++y) {
    const TPixel32 *pix = ras->pixels(ras->getLy() - 1 - y);
    QRgb *line          = reinterpret_cast<QRgb *>(image.scanLine(y));
    for (int x = 0; x < ras->getLx(); ++x, ++pix)
      line[x] = qRgba(pix->r, pix->g, pix->b, pix->m);
  }
  ras->unlock();
  return image;
}

// Returns an empty string if the images match within the tolerances.
std::string compare(const QImage &actual, const QImage &reference) {
  if (actual.size() != reference.size())
    return "size differs from the reference";
  const QImage ref = reference.convertToFormat(QImage::Format_ARGB32);
  int maxDiff = 0, badPixels = 0;
  for (int y = 0; y < actual.height(); ++y) {
    const QRgb *a = reinterpret_cast<const QRgb *>(actual.constScanLine(y));
    const QRgb *r = reinterpret_cast<const QRgb *>(ref.constScanLine(y));
    for (int x = 0; x < actual.width(); ++x) {
      int diff = std::max(std::max(std::abs(qRed(a[x]) - qRed(r[x])),
                                   std::abs(qGreen(a[x]) - qGreen(r[x]))),
                          std::max(std::abs(qBlue(a[x]) - qBlue(r[x])),
                                   std::abs(qAlpha(a[x]) - qAlpha(r[x]))));
      maxDiff  = std::max(maxDiff, diff);
      if (diff > cChannelTolerance) ++badPixels;
    }
  }
  if (badPixels > cPixelTolerance * actual.width() * actual.height())
    return std::to_string(badPixels) +
           " pixels differ by more than the tolerance (max difference " +
           std::to_string(maxDiff) + ")";
  return std::string();
}

}  // namespace

int main(int argc, char **argv) {
  if (argc < 3) {
    std::cerr << "Usage: fxrender_test <reference folder> <output folder>"
              << std::endl;
    return 2;
  }
  // A QGuiApplication: some FX render through an offscreen OpenGL surface,
  // which needs a screen (QT_QPA_PLATFORM=offscreen when headless)
  QGuiApplication app(argc, argv);
  TThread::init();
  initStdFx();

  const QDir referenceDir(QString::fromLocal8Bit(argv[1]));
  QDir outputDir(QString::fromLocal8Bit(argv[2]));
  outputDir.mkpath(".");

  TRenderer renderer(1);
  int failures = 0, missing = 0;
  for (const Case &c : cases()) {
    const QString fileName = QString::fromLatin1(c.name) + ".png";
    TRaster32P ras         = render(c.build(), renderer);
    if (!ras) {
      std::cerr << c.name << ": the FX produced no 32-bit raster" << std::endl;
      ++failures;
      continue;
    }
    const QImage actual = toImage(ras);

    std::string problem;
    const QString referencePath = referenceDir.filePath(fileName);
    if (!QFileInfo::exists(referencePath)) {
      ++missing;
      actual.save(outputDir.filePath(fileName), "PNG");
      std::cout << c.name << ": no reference image yet, wrote "
                << outputDir.filePath(fileName).toStdString() << std::endl;
      continue;
    } else {
      QImage reference(referencePath);
      problem = reference.isNull() ? "the reference image cannot be read"
                                   : compare(actual, reference);
    }

    if (problem.empty()) {
      std::cout << c.name << ": ok" << std::endl;
      continue;
    }
    ++failures;
    actual.save(outputDir.filePath(fileName), "PNG");
    std::cerr << c.name << ": " << problem << "; wrote "
              << outputDir.filePath(fileName).toStdString() << std::endl;
  }

  if (missing)
    std::cout << missing << " of " << cases().size()
              << " FX renders have no reference image yet" << std::endl;
  if (failures)
    std::cerr << failures << " of " << cases().size()
              << " FX renders do not match their references" << std::endl;
  else
    std::cout << "All " << cases().size() - missing
              << " FX renders with a reference match" << std::endl;
  return failures ? 1 : 0;
}
