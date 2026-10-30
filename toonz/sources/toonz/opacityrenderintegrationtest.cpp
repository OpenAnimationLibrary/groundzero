#include "opacityrenderintegrationtest.h"
#include "tstroke.h"
#include "tvectorgl.h"
#include "tstrokeopacitybounds.h"
#include "tvectorimage.h"
#include "tvectorrenderdata.h"
#include "tsimplecolorstyles.h"
#include "tvectorbrushstyle.h"
#include "tpalette.h"
#include "tcolorstyles.h"
#include "tregion.h"
#include "tofflinegl.h"
#include "qtofflinegl.h"
#include "tlevel_io.h"
#include "timage_io.h"
#include "tproperty.h"
#include "texception.h"
#include "toonzqt/planeviewer.h"
#include <QApplication>
#include <QTemporaryDir>
#include <QDir>
#include <QFile>
#include <QImage>
#include <QPainter>
#include <QSvgRenderer>
#include <QOpenGLContext>
#include <QElapsedTimer>
#include <QThread>
#include <QLabel>
#include <QPaintEvent>
#include <atomic>
#include <algorithm>
#include <future>
#include <cmath>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <stdexcept>

namespace {
void require(bool value, const char *message) { if (!value) throw std::runtime_error(message); }
TVectorImageP imageWith(TPalette *palette, int style) {
  TVectorImageP image = new TVectorImage;
  image->setPalette(palette);
  auto *stroke = new TStroke(std::vector<TThickPoint>{{-48,0,8},{0,0,8},{48,0,8}});
  stroke->setStyle(style); image->addStroke(stroke, false);
  return image;
}
void depthDiagnostics(const TVectorImageP &image, const char *phase) {
  GLint viewport[4], depthFunction = 0, depthBits = 0, stencilBits = 0;
  GLint sourceBlend = 0, destinationBlend = 0, matrixMode = 0;
  GLboolean depthWrite = GL_FALSE, colorWrite[4];
  GLdouble depthRange[2], model[16], projection[16];
  glGetIntegerv(GL_VIEWPORT, viewport);
  glGetIntegerv(GL_DEPTH_FUNC, &depthFunction);
  glGetIntegerv(GL_DEPTH_BITS, &depthBits);
  glGetIntegerv(GL_STENCIL_BITS, &stencilBits);
  glGetIntegerv(GL_BLEND_SRC, &sourceBlend);
  glGetIntegerv(GL_BLEND_DST, &destinationBlend);
  glGetIntegerv(GL_MATRIX_MODE, &matrixMode);
  glGetBooleanv(GL_DEPTH_WRITEMASK, &depthWrite);
  glGetBooleanv(GL_COLOR_WRITEMASK, colorWrite);
  glGetDoublev(GL_DEPTH_RANGE, depthRange);
  glGetDoublev(GL_MODELVIEW_MATRIX, model);
  glGetDoublev(GL_PROJECTION_MATRIX, projection);
  const auto *stroke = image->getStroke(0);
  std::ostringstream out;
  out << std::setprecision(17) << "vector-opacity-depth: " << phase
      << " style=" << stroke->getStyle()
      << " tag=" << image->getPalette()->getStyle(stroke->getStyle())->getTagId()
      << " opacity=" << stroke->getOpacity()
      << " knots=" << stroke->getOpacityProfile().size()
      << " depth=(enabled=" << int(glIsEnabled(GL_DEPTH_TEST))
      << ",function=" << depthFunction << ",bits=" << depthBits
      << ",write=" << int(depthWrite) << ",range=" << depthRange[0] << ':' << depthRange[1] << ')'
      << " stencil=(enabled=" << int(glIsEnabled(GL_STENCIL_TEST)) << ",bits=" << stencilBits << ')'
      << " blend=(enabled=" << int(glIsEnabled(GL_BLEND)) << ",source=" << sourceBlend
      << ",destination=" << destinationBlend << ") colorMask=";
  for (auto value : colorWrite) out << int(value);
  out << " viewport=";
  for (auto value : viewport) out << value << ',';
  out << " matrixMode=" << matrixMode << " model=";
  for (auto value : model) out << value << ',';
  out << " projection=";
  for (auto value : projection) out << value << ',';
  std::cout << out.str() << std::endl;
}
TRaster32P render(const TVectorImageP &image, bool depth = false,
                 TOfflineGL *reuse = nullptr) {
  std::unique_ptr<TOfflineGL> owned;
  if (!reuse) { owned.reset(new TOfflineGL(TDimension(256,128))); reuse = owned.get(); }
  reuse->makeCurrent(); reuse->initMatrix(); reuse->clear(TPixel32::Transparent);
  glClearDepth(1.); glDepthMask(GL_TRUE); glClear(GL_DEPTH_BUFFER_BIT);
  if (depth) {
    glMatrixMode(GL_PROJECTION); glLoadIdentity(); glOrtho(0,256,0,128,-256,256);
    glMatrixMode(GL_MODELVIEW); glLoadIdentity();
    // SceneViewer::drawCameraStand uses LEQUAL for coplanar vector layers.
    glEnable(GL_DEPTH_TEST); glDepthFunc(GL_LEQUAL);
    glColor4d(0,0,1,1); glBegin(GL_QUADS);
    glVertex3d(0,0,0); glVertex3d(256,0,0); glVertex3d(256,128,0); glVertex3d(0,128,0); glEnd();
    glTranslated(128,64,0); glRotated(25,0,1,0);
    // ViewerSettings targets an opaque display, not production alpha. Hold
    // the opaque canvas alpha while still comparing every output channel.
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_FALSE);
  } else { glDisable(GL_DEPTH_TEST); glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE); }
  TVectorRenderData data = depth
      ? TVectorRenderData(TVectorRenderData::ViewerSettings(), TAffine(), TRect(0,0,255,127), image->getPalette())
      : TVectorRenderData(TVectorRenderData::ProductionSettings(), TTranslation(128,64), TRect(0,0,255,127), image->getPalette());
  data.m_is3dView = depth; data.m_drawRegions = false;
  if (depth) depthDiagnostics(image, "before native draw");
  reuse->draw(image, data, false);
  if (depth) depthDiagnostics(image, "after native draw");
  auto pixels = reuse->getRaster()->clone(); reuse->doneCurrent();
  return pixels;
}
void scaledPixels(const TRaster32P &base, const TRaster32P &actual, double scale,
                  const char *message, bool requireArtwork = true) {
  int occupied = 0, mismatchCount = 0, maximumDifference = 0;
  int left = base->getLx(), bottom = base->getLy(), right = -1, top = -1;
  std::ostringstream mismatches;
  for (int y = 0; y < base->getLy(); ++y) for (int x = 0; x < base->getLx(); ++x) {
    const auto a = base->pixels(y)[x], b = actual->pixels(y)[x];
    if (a.m) ++occupied;
    const int delta = std::max({std::abs(int(std::lround(a.r * scale)) - int(b.r)),
        std::abs(int(std::lround(a.g * scale)) - int(b.g)),
        std::abs(int(std::lround(a.b * scale)) - int(b.b)),
        std::abs(int(std::lround(a.m * scale)) - int(b.m))});
    if (delta > 2) {
      if (mismatchCount < 12)
        mismatches << "\npixel(" << x << ',' << y << ") base RGBA=("
                   << int(a.r) << ',' << int(a.g) << ',' << int(a.b) << ',' << int(a.m)
                   << ") actual RGBA=(" << int(b.r) << ',' << int(b.g) << ','
                   << int(b.b) << ',' << int(b.m) << ')';
      ++mismatchCount;
      maximumDifference = std::max(maximumDifference, delta);
      left = std::min(left,x); right = std::max(right,x);
      bottom = std::min(bottom,y); top = std::max(top,y);
    }
  }
  if (mismatchCount) {
    std::ostringstream detail;
    detail << message << ": mismatched pixels=" << mismatchCount
           << " max channel difference=" << maximumDifference << " scale=" << scale
           << " bounds=(" << left << ',' << bottom << ',' << right << ',' << top << ')'
           << mismatches.str();
    if (mismatchCount > 12) detail << "\n[remaining pixel details omitted]";
    throw std::runtime_error(detail.str());
  }
  require(!requireArtwork || occupied > 20, "opacity style fixture produced no artwork");
}

class OpacityFailureViewer final : public PlaneViewer {
  TVectorImageP m_image;
public:
  int failureStage = 1, paintCount = 0;
  bool productionRejected = false, stateRestored = true, nestedScopeRestored = true;
  OpacityFailureViewer *nestedViewer = nullptr;
  explicit OpacityFailureViewer(const TVectorImageP &image)
      : PlaneViewer(nullptr), m_image(image) { resize(360, 180); }
protected:
  void paintGL() override {
    ++paintCount;
    productionRejected = false;
    tglSetOpacityFramebufferFailureForTest(failureStage);
    struct ResetFailure { ~ResetFailure() { tglSetOpacityFramebufferFailureForTest(0); } } reset;
    GLint stack = 0, framebuffer = 0, viewport[4], modelStack = 0;
    glGetIntegerv(GL_ATTRIB_STACK_DEPTH, &stack);
    glGetIntegerv(GL_MODELVIEW_STACK_DEPTH, &modelStack);
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &framebuffer);
    glGetIntegerv(GL_VIEWPORT, viewport);
    if (failureStage) {
      // A file/production render invoked during a widget paint must still fail.
      TVectorRenderData production(TVectorRenderData::ProductionSettings(),
          TTranslation(128,64), TRect(), m_image->getPalette());
      try { tglDraw(production, m_image.getPointer()); }
      catch (const std::runtime_error &) { productionRejected = true; }
    }
    pushGLWorldCoordinates();
    draw(m_image);  // Actual PlaneViewer draw path under a real Qt paint event.
    popGLCoordinates();
    GLint afterStack = 0, afterFramebuffer = 0, afterViewport[4], afterModelStack = 0;
    glGetIntegerv(GL_ATTRIB_STACK_DEPTH, &afterStack);
    glGetIntegerv(GL_MODELVIEW_STACK_DEPTH, &afterModelStack);
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &afterFramebuffer);
    glGetIntegerv(GL_VIEWPORT, afterViewport);
    stateRestored = stateRestored && stack == afterStack && modelStack == afterModelStack && framebuffer == afterFramebuffer &&
        std::equal(viewport, viewport + 4, afterViewport) && glGetError() == GL_NO_ERROR;
    if (nestedViewer && failureStage) {
      QPaintEvent nestedEvent(nestedViewer->QOpenGLWidget::rect());
      QApplication::sendEvent(nestedViewer, &nestedEvent);
      makeCurrent();
      nestedScopeRestored = nestedScopeRestored && TViewerOpacityErrorScope::currentFailed();
    }
    // A later opaque layer covers the entire failed drawing. The error must
    // remain visible above this layer, without being baked into GL output.
    glDisable(GL_SCISSOR_TEST); glColorMask(GL_TRUE,GL_TRUE,GL_TRUE,GL_TRUE);
    glClearColor(0, 0, 1, 1); glClear(GL_COLOR_BUFFER_BIT);
  }
};

void viewerFailureBoundary(const TVectorImageP &image, const std::vector<int> &stages = {1,2}) {
  OpacityFailureViewer nested(image);
  nested.failureStage = 0; nested.show();
  QApplication::processEvents();
  OpacityFailureViewer viewer(image);
  viewer.nestedViewer = &nested;
  viewer.show();
  QElapsedTimer initialDeadline; initialDeadline.start();
  while (viewer.paintCount == 0 && initialDeadline.elapsed() < 5000) {
    QApplication::processEvents(); QThread::msleep(1);
  }
  auto *initialOverlay = viewer.findChild<QLabel *>("vectorOpacityErrorOverlay");
  require(viewer.paintCount > 0 && initialOverlay && initialOverlay->isVisible(),
          "first native widget paint lost its opacity failure notice");
  auto repaint = [&] {
    const int previous = viewer.paintCount;
    viewer.update();
    QElapsedTimer deadline; deadline.start();
    while (viewer.paintCount == previous && deadline.elapsed() < 5000) {
      QApplication::processEvents(); QThread::msleep(1);
    }
    require(viewer.paintCount > previous, "native opacity viewer did not paint");
  };
  QLabel *overlay = nullptr;
  for (int stage : stages) {
    viewer.failureStage = stage; repaint();
    overlay = viewer.findChild<QLabel *>("vectorOpacityErrorOverlay");
    require(overlay && overlay->isVisible() && overlay->text().contains("Preview incomplete") &&
                !overlay->accessibleDescription().isEmpty() &&
                overlay->testAttribute(Qt::WA_TransparentForMouseEvents) &&
                overlay->focusPolicy() == Qt::NoFocus,
            "native viewer did not expose a nonblocking accessible opacity error");
    require(viewer.productionRejected && viewer.stateRestored && viewer.nestedScopeRestored,
            "viewer boundary swallowed a production error or leaked GL/error state");
    viewer.makeCurrent();
    unsigned char background[4] = {};
    glReadPixels(0,0,1,1,GL_RGBA,GL_UNSIGNED_BYTE,background);
    viewer.doneCurrent();
    require(background[2] == 255 && background[0] == 0,
            "later-layer coverage fixture did not cover the failed drawing");
    const QImage banner = overlay->grab().toImage();
    int colored = 0;
    for (int y = 0; y < banner.height(); ++y) for (int x = 0; x < banner.width(); ++x)
      if (qRed(banner.pixel(x,y)) > qGreen(banner.pixel(x,y)) * 1.5) ++colored;
    require(colored > 20, "native overlay did not paint a visible failure notice");
    repaint();
    require(viewer.findChildren<QLabel *>("vectorOpacityErrorOverlay").size() == 1,
            "repainting created duplicate opacity error notices");
  }
  viewer.setUpdatesEnabled(false);
  const int painted = viewer.paintCount;
  QPaintEvent reused(viewer.QOpenGLWidget::rect());
  QApplication::sendEvent(&viewer, &reused);
  require(viewer.paintCount == painted && overlay && !overlay->isHidden(),
          "reusing an old failed framebuffer incorrectly cleared its notice");
  bool reusedCaptureRejected = false;
  try { viewer.grabFramebuffer(); } catch (const std::runtime_error &) { reusedCaptureRejected = true; }
  require(reusedCaptureRejected, "capture reused a failed framebuffer while updates were disabled");
  viewer.setUpdatesEnabled(true);
  // Direct framebuffer capture is strict: finish Qt cleanup, then reject a
  // partial image outside the paint-event boundary. The same widget recovers.
  bool captureRejected = false;
  try { viewer.grabFramebuffer(); } catch (const std::runtime_error &) { captureRejected = true; }
  require(captureRejected && !TViewerOpacityErrorScope::currentFailed(),
          "failed framebuffer capture returned partial pixels or leaked its error scope");
  viewer.failureStage = 0; repaint();
  require(overlay && !overlay->isVisible(), "opacity error notice survived successful recovery");
  const QImage recovered = viewer.grabFramebuffer();
  require(!recovered.isNull() && qBlue(recovered.pixel(0,0)) == 255,
          "native widget could not render after an opacity failure");
  viewer.hide();
  nested.hide();
}

void nativeOpacityPicking(const TVectorImageP &image) {
  TOfflineGL context(TDimension(256,128));
  context.makeCurrent(); context.initMatrix();
  auto hit = [&](const TPointD &point) {
    GLuint selection[128] = {};
    GLint viewport[4]; glGetIntegerv(GL_VIEWPORT, viewport);
    glSelectBuffer(128, selection); glRenderMode(GL_SELECT);
    glInitNames(); glPushName(73);
    glMatrixMode(GL_PROJECTION); glPushMatrix(); glLoadIdentity();
    gluPickMatrix(point.x + 128, point.y + 64, .5, .5, viewport);
    gluOrtho2D(0,256,0,128);
    glMatrixMode(GL_MODELVIEW); glPushMatrix(); glLoadIdentity();
    struct RestorePicking {
      bool selected = true;
      ~RestorePicking() {
        glMatrixMode(GL_MODELVIEW); glPopMatrix();
        glMatrixMode(GL_PROJECTION); glPopMatrix(); glMatrixMode(GL_MODELVIEW);
        if (selected) glRenderMode(GL_RENDER);
      }
    } restore;
    TVectorRenderData data(TVectorRenderData::ViewerSettings(), TTranslation(128,64),
        TRect(), image->getPalette());
    data.m_drawRegions = false;
    tglDraw(data, image.getPointer());
    const GLint hits = glRenderMode(GL_RENDER); restore.selected = false;
    require(hits >= 0 && (!hits || (selection[0] == 1 && selection[3] == 73)),
            "native opacity selection buffer overflowed or lost its object name");
    return hits > 0;
  };
  auto *stroke = image->getStroke(0);
  stroke->setOpacity(1.); stroke->setOpacityProfile({});
  require(hit(TPointD()) && !hit(TPointD(54,7)),
          "native picking fixture did not distinguish stroke from bbox-only corner");
  tglSetOpacityFramebufferFailureForTest(1);
  struct ResetFailure { ~ResetFailure() { tglSetOpacityFramebufferFailureForTest(0); } } reset;
  stroke->setOpacity(.5);
  require(hit(TPointD()) && !hit(TPointD(54,7)),
          "scalar opacity replaced native geometric picking with a composite rectangle");
  stroke->setOpacityProfile({{0,0},{1,1}});
  require(hit(TPointD()) && !hit(TPointD(54,7)),
          "opacity envelope altered native selection hits or required a framebuffer");
  stroke->setOpacity(0.);
  require(!hit(TPointD()), "native picking changed zero-scalar-opacity semantics");
  context.doneCurrent();
}
}

void runOpacityRenderNativeChecks() {
  QTemporaryDir inputs;
  require(inputs.isValid(), "opacity render fixture directory");
  TPaletteP palette = new TPalette;
  std::vector<int> styles;
  // Solid, optimized procedural, outline procedural, Matrioska and native
  // vector MyPaint-style drawing all use the same whole-stroke contract.
  for (int tag : {3, 104, 106, 135, 141, 4001}) {
    auto *style = TColorStyle::create(tag);
    require(style && style->isStrokeStyle(), "representative native style unavailable");
    for (int color = 0; color < style->getColorParamCount(); ++color)
      style->setColorParamValue(color, color % 2 ? TPixel32(20,160,240,160) : TPixel32(220,80,30,192));
    if (style->hasMainColor()) style->setMainColor(TPixel32(220,80,30,192));
    if (tag == 106) { style->setParamValue(1, 35.); style->setParamValue(2, 1.); }
    styles.push_back(palette->addStyle(style));
  }
  nativeOpacityPicking(imageWith(palette.getPointer(), styles.front()));
  TPaletteP brushPalette = new TPalette;
  const int brushColor = brushPalette->addStyle(TPixel32(220,80,30,192));
  TVectorImageP brush = new TVectorImage; brush->setPalette(brushPalette.getPointer());
  auto *edge = new TStroke(std::vector<TThickPoint>{{-8,-4,0},{0,-4,0},{8,-4,0},{8,0,0},{8,4,0},{0,4,0},{-8,4,0},{-8,0,0},{-8,-4,0}});
  edge->setSelfLoop(true); edge->setStyle(brushColor); brush->addStroke(edge);
  brush->findRegions(); require(brush->getRegion(TPointD()), "vector brush fixture region");
  brush->getRegion(TPointD())->setStyle(brushColor);
  styles.push_back(palette->addStyle(new TVectorBrushStyle("OpacityFixture", brush)));

  const TFilePath oldRasterRoot = TRasterImagePatternStrokeStyle::getRootDir();
  struct RestoreRoot {
    TFilePath root;
    ~RestoreRoot() { TRasterImagePatternStrokeStyle::setRootDir(root.getParentDir()); }
  } restoreRoot{oldRasterRoot};
  require(QDir().mkpath(inputs.filePath("custom styles")), "raster Trail fixture folder");
  QImage stamp(16,16,QImage::Format_ARGB32); stamp.fill(qRgba(200,100,40,128));
  require(stamp.save(inputs.filePath("custom styles/OpacityTrail.png")), "raster Trail fixture save");
  TRasterImagePatternStrokeStyle::setRootDir(TFilePath(inputs.path()));
  auto *trail = new TRasterImagePatternStrokeStyle("OpacityTrail");
  require(trail->getLevelFrameCount() == 1, "native translucent raster Trail did not load");
  styles.push_back(palette->addStyle(trail));

  for (int style : styles) {
    std::cout << "vector-opacity-render: checking style=" << style << " tag="
              << palette->getStyle(style)->getTagId() << std::endl;
    auto image = imageWith(palette.getPointer(), style);
    const auto base = render(image);
    if (palette->getStyle(style)->getTagId() == 106) {
      bool overhang = false;
      for (int y = 0; y < 128; ++y) for (int x = 0; x < 256; ++x)
        if ((y < 54 || y > 74) && base->pixels(y)[x].m) overhang = true;
      require(overhang, "Circlets fixture did not exercise decoration beyond legacy bbox");
    }
    if (style == styles.back()) {
      int maximumAlpha = 0;
      for (int y = 0; y < base->getLy(); ++y) for (int x = 0; x < base->getLx(); ++x)
        maximumAlpha = std::max(maximumAlpha, int(base->pixels(y)[x].m));
      require(std::abs(maximumAlpha - 128) <= 1, "translucent full-color Trail alpha was multiplied twice");
    }
    auto *stroke = image->getStroke(0);
    stroke->setOpacity(.5);
    const auto half = render(image);
    scaledPixels(base, half, .5, "half-opacity production style differs");
    tglSetOpacityViewportCaptureForTest(true);
    const auto fullViewport = render(image);
    tglSetOpacityViewportCaptureForTest(false);
    scaledPixels(fullViewport, half, 1., "bounded opacity capture clipped native style decoration");
    stroke->setOpacity(.4);
    scaledPixels(base, render(image), .4, "style did not apply scalar opacity once");
    stroke->setOpacityProfile({{0,.5},{1,.5}});
    scaledPixels(base, render(image), .2, "style scalar/profile composition differs");
    stroke->setOpacity(0.);
    scaledPixels(base, render(image), 0., "zero-opacity native style still renders");
    stroke->setOpacity(1.); stroke->setOpacityProfile({{0,1},{1,1}});
    scaledPixels(base, render(image), 1., "identity envelope altered style spacing or alpha");
    stroke->setOpacityProfile({});
    std::cout << "vector-opacity-render: checking native 3D identity style=" << style
              << " tag=" << palette->getStyle(style)->getTagId() << std::endl;
    const auto ordinaryDepth = render(image, true);
    stroke->setOpacityProfile({{0,1},{1,1}});
    scaledPixels(ordinaryDepth, render(image, true), 1.,
        "style opacity identity changed native 3D viewer occlusion");
  }
  // Failures must remain observable and restore every caller GL stack/state.
  {
    auto failing = imageWith(palette.getPointer(), styles.front());
    failing->getStroke(0)->setOpacity(.5);
    TOfflineGL context(TDimension(256,128)); context.makeCurrent(); context.initMatrix();
    TVectorRenderData data(TVectorRenderData::ProductionSettings(), TTranslation(128,64), TRect(0,0,255,127), palette.getPointer());
    GLint originalStack = 0, originalFramebuffer = 0, originalViewport[4];
    glGetIntegerv(GL_ATTRIB_STACK_DEPTH, &originalStack);
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &originalFramebuffer);
    glGetIntegerv(GL_VIEWPORT, originalViewport);
    for (int stage : {1, 2}) {
      tglSetOpacityFramebufferFailureForTest(stage);
      bool rejected = false;
      try { tglDraw(data, failing.getPointer()); } catch (const std::runtime_error &) { rejected = true; }
      tglSetOpacityFramebufferFailureForTest(0);
      GLint stack = 0, framebuffer = 0, viewport[4];
      glGetIntegerv(GL_ATTRIB_STACK_DEPTH, &stack); glGetIntegerv(GL_FRAMEBUFFER_BINDING, &framebuffer); glGetIntegerv(GL_VIEWPORT, viewport);
      require(rejected && stack == originalStack && framebuffer == originalFramebuffer &&
          std::equal(viewport, viewport + 4, originalViewport) && glGetError() == GL_NO_ERROR,
          "opacity allocation/capability failure leaked GL state or was hidden");
    }
    context.doneCurrent();
    auto success = render(failing);
    require(success->pixels(64)[128].m > 0, "render did not recover after injected framebuffer failure");
    viewerFailureBoundary(failing);
  }
  // A real PLI-backed Vector Trail can fail inside the outer opacity capture.
  // Its nested matrix and strict production/error-collection scopes must all
  // unwind before the ordinary widget paint reports an incomplete preview.
  {
    const TFilePath oldRoot = TVectorImagePatternStrokeStyle::getRootDir();
    struct RestoreVectorRoot {
      TFilePath path;
      ~RestoreVectorRoot() { TVectorImagePatternStrokeStyle::setRootDir(path.getParentDir()); }
    } restoreVectorRoot{oldRoot};
    TVectorImagePatternStrokeStyle::setRootDir(TFilePath(inputs.path()));
    auto stampImage = imageWith(palette.getPointer(), styles.front());
    stampImage->getStroke(0)->setOpacityProfile({{0,.5},{1,.5}});
    const TFilePath stampPath(inputs.filePath("custom styles/OpacityVectorTrail.pli"));
    {
      TLevelWriterP writer(stampPath);
      writer->setPalette(palette.getPointer());
      writer->getFrameWriter(TFrameId(1))->save(stampImage);
    }
    auto *vectorTrail = new TVectorImagePatternStrokeStyle("OpacityVectorTrail");
    require(vectorTrail->getLevelFrameCount() == 1, "native Vector Trail did not load");
    auto nestedPattern = imageWith(palette.getPointer(), palette->addStyle(vectorTrail));
    nestedPattern->getStroke(0)->setOpacity(.5);
    viewerFailureBoundary(nestedPattern, {3});
  }
  auto image = imageWith(palette.getPointer(), styles.front());
  const auto ordinaryDepth = render(image, true);
  image->getStroke(0)->setOpacityProfile({{0,1},{1,1}});
  scaledPixels(ordinaryDepth, render(image, true), 1., "opacity identity changed 3D depth occlusion");
  image->getStroke(0)->setOpacityProfile({{0,0},{1,1}});
  auto fade = render(image);
  const int leftAlpha = fade->pixels(64)[96].m, rightAlpha = fade->pixels(64)[160].m;
  require(leftAlpha < rightAlpha && leftAlpha > 0 && rightAlpha < 192,
          "along-stroke envelope did not vary rendered opacity");

  // Real default production contexts, including nesting, sharing and release.
  TOfflineGL outer(TDimension(256,128)); outer.makeCurrent();
  auto *outerContext = QOpenGLContext::currentContext();
  require(outerContext, "production renderer did not use the supported Qt context");
  {
    TOfflineGL shared(TDimension(256,128), &outer);
    auto *sharedContext = QOpenGLContext::currentContext();
    require(sharedContext && QOpenGLContext::areSharing(outerContext, sharedContext), "production context sharing missing");
    scaledPixels(fade, render(image, false, &shared), 1., "shared-context repeated render changed pixels");
    require(QOpenGLContext::currentContext() == outerContext, "nested renderer failed to restore parent context");
  }
  require(QOpenGLContext::currentContext() == outerContext, "renderer destruction lost parent context");
  outer.doneCurrent();

  TOfflineGL transferred(TDimension(256,128));
  transferred.doneCurrent();
  std::atomic<bool> done{false};
  auto worker = std::async(std::launch::async, [&] {
    try {
      auto pixels = render(image);
      scaledPixels(pixels, render(image, false, &transferred), 1.,
          "released production context could not transfer to a render thread");
      done.store(true); return pixels;
    }
    catch (...) { done.store(true); throw; }
  });
  QElapsedTimer deadline; deadline.start();
  while (!done.load() && deadline.elapsed() < 30000) {
    QCoreApplication::processEvents(); QThread::msleep(1);
  }
  if (!done.load()) qFatal("Background production render timed out");
  scaledPixels(fade, worker.get(), 1., "background production render differs");
  scaledPixels(fade, render(image, false, &transferred), 1.,
      "released production context could not return to the GUI thread");

  TPropertyGroup properties;
  TEnumProperty mode("Stroke Mode"); mode.addValue(L"Outline"); properties.bind(mode);
  TEnumProperty quality("Outline Quality"); quality.addValue(L"High"); properties.bind(quality);
  TIntProperty scale("Opacity Raster Scale",1,16,4); properties.bind(scale);
  const TFilePath path(inputs.filePath("Profile..svg"));
  TFilePath written;
  {
    TLevelWriterP writer(path, &properties);
    auto frame = writer->getFrameWriter(TFrameId(1)); written = frame->getFilePath(); frame->save(TImageP(image));
  }
  QSvgRenderer svg(written.getQString());
  require(svg.isValid(), "independent SVG renderer rejected opacity export");
  QImage svgPixels(400,100,QImage::Format_ARGB32_Premultiplied); svgPixels.fill(Qt::transparent);
  QPainter painter(&svgPixels); svg.render(&painter, QRectF(0,0,400,100)); painter.end();
  int low = 0, high = 0;
  for (int y = 0; y < 100; ++y) {
    low = std::max(low, qAlpha(svgPixels.pixel(100,y)));
    high = std::max(high, qAlpha(svgPixels.pixel(300,y)));
  }
  require(low > 0 && high > low * 2, "independent SVG render lost native opacity envelope");
  bool rejected = false;
  try { TLevelReaderP reader(written); reader->loadInfo(); }
  catch (const TImageException &error) {
    rejected = error.getMessage().find(L"original PLI 150.1") != std::wstring::npos;
  }
  require(rejected, "tagged rasterized SVG was silently imported as partial editable vectors");

  // An unbounded procedural style must fail before replacing an existing file.
  auto unsupported = imageWith(palette.getPointer(), styles[1]);
  unsupported->getStroke(0)->setOpacityProfile({{0,0},{1,1}});
  const TFilePath rejectedPath(inputs.filePath("Unbounded..svg"));
  TLevelWriterP rejectedWriter(rejectedPath, &properties);
  auto rejectedFrame = rejectedWriter->getFrameWriter(TFrameId(1));
  const QString rejectedFile = rejectedFrame->getFilePath().getQString();
  QFile sentinel(rejectedFile); require(sentinel.open(QIODevice::WriteOnly), "SVG sentinel open");
  sentinel.write("existing SVG bytes"); sentinel.close();
  bool boundsRejected = false;
  try { rejectedFrame->save(TImageP(unsupported)); }
  catch (const TImageException &error) { boundsRejected = error.getMessage().find(L"verified SVG render bounds") != std::wstring::npos; }
  require(sentinel.open(QIODevice::ReadOnly), "SVG sentinel reopen");
  require(boundsRejected && sentinel.readAll() == QByteArray("existing SVG bytes"),
          "unbounded SVG export did not fail atomically with actionable message");
}
