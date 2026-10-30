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
#include <atomic>
#include <future>
#include <cmath>
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
TRaster32P render(const TVectorImageP &image, bool depth = false,
                 TOfflineGL *reuse = nullptr) {
  std::unique_ptr<TOfflineGL> owned;
  if (!reuse) { owned.reset(new TOfflineGL(TDimension(256,128))); reuse = owned.get(); }
  reuse->makeCurrent(); reuse->initMatrix(); reuse->clear(TPixel32::Transparent);
  glClearDepth(1.); glDepthMask(GL_TRUE); glClear(GL_DEPTH_BUFFER_BIT);
  if (depth) {
    glMatrixMode(GL_PROJECTION); glLoadIdentity(); glOrtho(0,256,0,128,-256,256);
    glMatrixMode(GL_MODELVIEW); glLoadIdentity();
    glEnable(GL_DEPTH_TEST); glDepthFunc(GL_LESS);
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
  reuse->draw(image, data, false);
  auto pixels = reuse->getRaster()->clone(); reuse->doneCurrent();
  return pixels;
}
void scaledPixels(const TRaster32P &base, const TRaster32P &actual, double scale,
                  const char *message, bool requireArtwork = true) {
  int occupied = 0;
  for (int y = 0; y < base->getLy(); ++y) for (int x = 0; x < base->getLx(); ++x) {
    const auto a = base->pixels(y)[x], b = actual->pixels(y)[x];
    if (a.m) ++occupied;
    require(std::abs(int(std::lround(a.r * scale)) - int(b.r)) <= 2 &&
            std::abs(int(std::lround(a.g * scale)) - int(b.g)) <= 2 &&
            std::abs(int(std::lround(a.b * scale)) - int(b.b)) <= 2 &&
            std::abs(int(std::lround(a.m * scale)) - int(b.m)) <= 2, message);
  }
  require(!requireArtwork || occupied > 20, "opacity style fixture produced no artwork");
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

  std::atomic<bool> done{false};
  auto worker = std::async(std::launch::async, [&] {
    try { auto pixels = render(image); done.store(true); return pixels; }
    catch (...) { done.store(true); throw; }
  });
  QElapsedTimer deadline; deadline.start();
  while (!done.load() && deadline.elapsed() < 30000) {
    QCoreApplication::processEvents(); QThread::msleep(1);
  }
  if (!done.load()) qFatal("Background production render timed out");
  scaledPixels(fade, worker.get(), 1., "background production render differs");

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
