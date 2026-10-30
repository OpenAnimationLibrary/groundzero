#include "cadmiumtypes.h"
#include "tregion.h"
#include "tstroke.h"
#include "tvectorimage.h"

#include <QApplication>
#include <QOffscreenSurface>
#include <QOpenGLContext>
#include <functional>
#include <iostream>
#include <stdexcept>
#include <vector>

namespace {
void require(bool condition, const char *message) {
  if (!condition) throw std::runtime_error(message);
}

// The adapter draws with OpenGL. Without a context (a test runner with no
// display, for example) the test is skipped: ctest reads 77 as skipped.
bool openGLAvailable() {
  QOffscreenSurface surface;
  surface.create();
  QOpenGLContext context;
  if (!surface.isValid() || !context.create()) return false;
  const bool ok = context.makeCurrent(&surface);
  if (ok) context.doneCurrent();
  return ok;
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
    const auto &leftOutline = left->outlineOptions();
    const auto &rightOutline = right->outlineOptions();
    require(leftOutline.m_capStyle == rightOutline.m_capStyle &&
                leftOutline.m_joinStyle == rightOutline.m_joinStyle &&
                leftOutline.m_miterLower == rightOutline.m_miterLower &&
                leftOutline.m_miterUpper == rightOutline.m_miterUpper &&
                leftOutline.m_patternFrameOffset == rightOutline.m_patternFrameOffset &&
                leftOutline.m_patternFrameStep == rightOutline.m_patternFrameStep &&
                leftOutline.m_opacity == rightOutline.m_opacity,
            "native stroke outline changed");
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
  // The message is built before the call when it is an argument: it would
  // report the previous call's error
  const bool snapshot =
      Cadmium::snapshotVector(TImageP(source.getPointer()), frame, error);
  require(snapshot, qPrintable(QString("snapshot failed: ") + error));
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
  require(Cadmium::snapshotVector(TImageP(painted.getPointer()), paintedFrame, error),
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

  const auto outlineEditInvalidates = [&](const std::function<void(TStroke &)> &edit,
                                         const char *message) {
    const TVectorImageP edited = source->clone();
    edit(*edited->getStroke(0));
    Cadmium::Frame editedFrame;
    require(Cadmium::snapshotVector(TImageP(edited.getPointer()), editedFrame, error),
            "outline-edited snapshot failed");
    // This is the freshness comparison used before the popup applies a result.
    require(editedFrame.fingerprint != frame.fingerprint, message);
    Cadmium::Frame stale = frame;
    stale.original = editedFrame.original;
    require(!Cadmium::applyVector(stale, styles, {}, true, error),
            "an outline-edited snapshot accepted stale paint decisions");
    const TVectorImageP freshResult(
        Cadmium::applyVector(editedFrame, styles, {}, true, error));
    require(!!freshResult, "fresh outline-edited proposal was rejected");
    geometryEqual(edited, freshResult);
  };
  outlineEditInvalidates([](TStroke &stroke) { stroke.setOpacity(0.5); },
                        "opacity edit retained a stale fingerprint");
  outlineEditInvalidates([](TStroke &stroke) {
    auto &outline = stroke.outlineOptions();
    outline.m_capStyle = (outline.m_capStyle + 1) % 3;
  }, "cap edit retained a stale fingerprint");
  outlineEditInvalidates([](TStroke &stroke) {
    auto &outline = stroke.outlineOptions();
    outline.m_joinStyle = (outline.m_joinStyle + 1) % 3;
  }, "join edit retained a stale fingerprint");
  outlineEditInvalidates([](TStroke &stroke) { stroke.outlineOptions().m_miterLower += 1.0; },
                        "lower miter edit retained a stale fingerprint");
  outlineEditInvalidates([](TStroke &stroke) { stroke.outlineOptions().m_miterUpper += 1.0; },
                        "upper miter edit retained a stale fingerprint");
  outlineEditInvalidates([](TStroke &stroke) { ++stroke.outlineOptions().m_patternFrameOffset; },
                        "pattern offset edit retained a stale fingerprint");
  outlineEditInvalidates([](TStroke &stroke) { stroke.outlineOptions().m_patternFrameStep = -1; },
                        "pattern direction edit retained a stale fingerprint");

  const TVectorImageP changed(frame.original);
  changed->getStroke(0)->setControlPoint(1, TThickPoint(0, -80, 2));
  require(!Cadmium::applyVector(frame, styles, {}, true, error),
          "changed snapshot was accepted");
}
}  // namespace

int main(int argc, char **argv) {
  QApplication application(argc, argv);
  if (!openGLAvailable()) {
    if (qEnvironmentVariableIsSet("GROUNDZERO_REQUIRE_VECTOR_GL")) {
      std::cerr << "Required Cadmium vector rendering has no OpenGL context\n";
      return 1;
    }
    std::cout << "Cadmium vector adapter checks skipped: no OpenGL context\n";
    return 77;
  }
  try {
    run();
    std::cout << "Cadmium vector adapter checks passed\n";
    return 0;
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
