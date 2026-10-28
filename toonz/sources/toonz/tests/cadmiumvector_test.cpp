#include "cadmiumtypes.h"
#include "tregion.h"
#include "tstroke.h"
#include "tvectorimage.h"

#include <QApplication>
#include <iostream>
#include <stdexcept>
#include <vector>

namespace {
void require(bool condition, const char *message) {
  if (!condition) throw std::runtime_error(message);
}

TVectorImageP drawing(int paint = 0) {
  TVectorImageP image = new TVectorImage;
  TPaletteP palette = new TPalette;
  palette->addStyle(TPixel32(230, 30, 20, 255));
  palette->addStyle(TPixel32(230, 30, 20, 255));
  image->setPalette(palette.getPointer());
  std::vector<TThickPoint> points = {
      {-100, -100, 2}, {0, -100, 2}, {100, -100, 2},
      {100, 0, 2}, {100, 100, 2}, {0, 100, 2},
      {-100, 100, 2}, {-100, 0, 2}, {-100, -100, 2}};
  TStroke *stroke = new TStroke(points);
  stroke->setSelfLoop(true);
  stroke->setStyle(1);
  image->addStroke(stroke);
  image->findRegions();
  require(image->getRegionCount() == 1, "fixture needs one closed region");
  image->getRegion(0U)->setStyle(paint);
  return image;
}

void geometryEqual(const TVectorImageP &a, const TVectorImageP &b) {
  require(a->getStrokeCount() == b->getStrokeCount(), "stroke count changed");
  for (UINT i = 0; i < a->getStrokeCount(); ++i) {
    const TStroke *left = a->getStroke(i), *right = b->getStroke(i);
    require(left->getId() == right->getId(), "stroke identity changed");
    require(left->getStyle() == right->getStyle(), "ink style changed");
    require(left->isSelfLoop() == right->isSelfLoop(), "closure changed");
    require(left->getControlPointCount() == right->getControlPointCount(),
            "control point count changed");
    for (int j = 0; j < left->getControlPointCount(); ++j) {
      const TThickPoint p = left->getControlPoint(j), q = right->getControlPoint(j);
      require(p.x == q.x && p.y == q.y && p.thick == q.thick,
              "native vector geometry changed");
    }
  }
}

void run() {
  QString error;
  Cadmium::Frame empty;
  require(!Cadmium::snapshotVector(TImageP(), empty, error),
          "empty drawing accepted");
  require(!error.isEmpty(), "empty drawing needs actionable failure");
  const TVectorImageP source = drawing();
  Cadmium::Frame frame;
  require(Cadmium::snapshotVector(source, frame, error),
          qPrintable(QString("snapshot failed: ") + error));
  require(frame.lines.size() == QSize(1024, 1024), "analysis canvas changed");
  require(frame.regionStyles.size() == 2, "wrong region slots");
  require(frame.nativeRegionIndices.size() == 2, "missing native mapping");
  QVector<int> styles(frame.regionStyles.size(), -100);
  styles[1] = 3;
  const TVectorImageP output(Cadmium::applyVector(frame, styles, {}, false, error));
  require(!!output, qPrintable(QString("apply failed: ") + error));
  require(output->getRegion(0U)->getStyle() == 3,
          "equal-colored native style identities were merged");
  require(source->getRegion(0U)->getStyle() == 0, "source drawing changed");
  geometryEqual(source, output);
  require(!Cadmium::applyVector(frame, QVector<int>(), {}, false, error),
          "incomplete proposal accepted");
  styles[1] = 100000;
  require(!Cadmium::applyVector(frame, styles, {}, false, error),
          "out-of-range style accepted");
  styles[1] = -2;
  require(!Cadmium::applyVector(frame, styles, {}, false, error),
          "unknown negative style accepted");
  styles[1] = -100;
  const TVectorImageP skipped(Cadmium::applyVector(frame, styles, {}, false, error));
  require(!!skipped && skipped->getRegion(0U)->getStyle() == 0,
          "unmatched prediction changed paint");

  const TVectorImageP painted = drawing(2);
  Cadmium::Frame paintedFrame;
  require(Cadmium::snapshotVector(painted, paintedFrame, error),
          "painted snapshot failed");
  styles[1] = 3;
  const TVectorImageP protectedResult(
      Cadmium::applyVector(paintedFrame, styles, {}, false, error));
  require(!!protectedResult && protectedResult->getRegion(0U)->getStyle() == 2,
          "existing paint was overwritten");
  const TVectorImageP repainted(
      Cadmium::applyVector(paintedFrame, styles, {}, true, error));
  require(!!repainted && repainted->getRegion(0U)->getStyle() == 3,
          "explicit repaint failed");
  geometryEqual(painted, repainted);

  const TVectorImageP changed(frame.original);
  changed->getStroke(0)->setControlPoint(1, TThickPoint(0, -80, 2));
  require(!Cadmium::applyVector(frame, styles, {}, true, error),
          "changed snapshot was accepted");
}
}  // namespace

int main(int argc, char **argv) {
  QApplication application(argc, argv);
  try {
    run();
    std::cout << "Cadmium vector adapter checks passed\n";
    return 0;
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
