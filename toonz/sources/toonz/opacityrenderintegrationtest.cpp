#include "opacityrenderintegrationtest.h"
#include "nativeprocessmemory.h"
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
#include "tools/vectoralignmentpreview.h"
#include <QApplication>
#include <QTemporaryDir>
#include <QDir>
#include <QFile>
#include <QImage>
#include <QPainter>
#include <QSvgRenderer>
#include <QOpenGLContext>
#include <QOpenGLFramebufferObject>
#include <QOpenGLFunctions>
#include <QOpenGLShaderProgram>
#include <QElapsedTimer>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSysInfo>
#include <QThread>
#include <QLabel>
#include <QPaintEvent>
#include <atomic>
#include <algorithm>
#include <future>
#include <cmath>
#include <iomanip>
#include <iostream>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <utility>
#include <vector>

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

constexpr int depthWidth = 256, depthHeight = 128;
struct DepthAttachments {
  std::vector<GLfloat> color;
  std::vector<GLuint> depth, stencil;
};
DepthAttachments depthAttachments() {
  const size_t count = size_t(depthWidth) * depthHeight;
  DepthAttachments result{std::vector<GLfloat>(count * 4),
                          std::vector<GLuint>(count), std::vector<GLuint>(count)};
  glPushClientAttrib(GL_CLIENT_PIXEL_STORE_BIT);
  glPixelStorei(GL_PACK_ALIGNMENT, 1); glPixelStorei(GL_PACK_ROW_LENGTH, 0);
  glPixelStorei(GL_PACK_SKIP_PIXELS, 0); glPixelStorei(GL_PACK_SKIP_ROWS, 0);
  glReadPixels(0,0,depthWidth,depthHeight,GL_RGBA,GL_FLOAT,result.color.data());
  glReadPixels(0,0,depthWidth,depthHeight,GL_DEPTH_COMPONENT,GL_UNSIGNED_INT,result.depth.data());
  GLint stencilBits = 0; glGetIntegerv(GL_STENCIL_BITS,&stencilBits);
  if (stencilBits)
    glReadPixels(0,0,depthWidth,depthHeight,GL_STENCIL_INDEX,GL_UNSIGNED_INT,result.stencil.data());
  glPopClientAttrib();
  require(glGetError() == GL_NO_ERROR, "native depth fixture could not read attachments");
  return result;
}
struct DepthState {
  std::vector<GLint> integers;
  std::vector<GLdouble> real;
  std::vector<std::pair<GLenum,int>> integerFields, realFields;
  bool operator==(const DepthState &other) const {
    return integers == other.integers && real == other.real;
  }
  void requireEqual(const DepthState &actual) const {
    if (*this == actual) return;
    std::ostringstream message;
    message << "native depth opacity changed caller state or matrix stacks:";
    int differences = 0;
    for (size_t i = 0; i < integers.size() && differences < 8; ++i)
      if (integers[i] != actual.integers[i]) {
        message << " integer 0x" << std::hex << integerFields[i].first << std::dec
                << '[' << integerFields[i].second << "]=" << integers[i]
                << "->" << actual.integers[i];
        ++differences;
      }
    for (size_t i = 0; i < real.size() && differences < 8; ++i)
      if (real[i] != actual.real[i]) {
        message << " real 0x" << std::hex << realFields[i].first << std::dec
                << '[' << realFields[i].second << "]=" << std::setprecision(17)
                << real[i] << "->" << actual.real[i];
        ++differences;
      }
    throw std::runtime_error(message.str());
  }
};
bool separateFramebuffers() {
  auto *context = QOpenGLContext::currentContext();
  return context && (context->format().majorVersion() >= 3 ||
      context->hasExtension("GL_EXT_framebuffer_blit") || context->hasExtension("GL_ARB_framebuffer_object"));
}
bool srgbFramebuffers() {
  auto *context = QOpenGLContext::currentContext();
  return context && (context->format().majorVersion() >= 3 ||
                      context->hasExtension("GL_EXT_framebuffer_sRGB") ||
                      context->hasExtension("GL_ARB_framebuffer_sRGB"));
}
bool floatingColorClamp() {
  auto *context = QOpenGLContext::currentContext();
  return context && (context->format().majorVersion() >= 3 ||
                      context->hasExtension("GL_ARB_color_buffer_float"));
}
DepthState depthState() {
  DepthState result;
  auto integer = [&](GLenum name, int count = 1) {
    const size_t offset = result.integers.size(); result.integers.resize(offset + count);
    for (int i = 0; i < count; ++i) result.integerFields.emplace_back(name, i);
    glGetIntegerv(name, result.integers.data() + offset);
  };
  for (GLenum name : {GL_FRAMEBUFFER_BINDING,GL_READ_BUFFER,GL_DRAW_BUFFER,GL_CURRENT_PROGRAM,
      GL_ATTRIB_STACK_DEPTH,GL_CLIENT_ATTRIB_STACK_DEPTH,GL_MODELVIEW_STACK_DEPTH,
      GL_PROJECTION_STACK_DEPTH,GL_TEXTURE_STACK_DEPTH,GL_MATRIX_MODE,
      GL_DEPTH_TEST,GL_DEPTH_FUNC,GL_DEPTH_WRITEMASK,GL_STENCIL_TEST,GL_STENCIL_FUNC,
      GL_STENCIL_REF,GL_STENCIL_VALUE_MASK,GL_STENCIL_WRITEMASK,GL_STENCIL_FAIL,
      GL_STENCIL_PASS_DEPTH_FAIL,GL_STENCIL_PASS_DEPTH_PASS,GL_SCISSOR_TEST,
      GL_BLEND,GL_BLEND_SRC,GL_BLEND_DST,GL_COLOR_SUM,GL_PACK_ALIGNMENT,GL_PACK_ROW_LENGTH,
      GL_PACK_SKIP_PIXELS,GL_PACK_SKIP_ROWS,GL_UNPACK_ALIGNMENT,GL_UNPACK_ROW_LENGTH,
      GL_UNPACK_SKIP_PIXELS,GL_UNPACK_SKIP_ROWS}) integer(name);
  integer(GL_VIEWPORT,4); integer(GL_SCISSOR_BOX,4); integer(GL_COLOR_WRITEMASK,4);
  if (separateFramebuffers()) integer(GL_READ_FRAMEBUFFER_BINDING);
  if (srgbFramebuffers()) integer(GL_FRAMEBUFFER_SRGB);
  if (floatingColorClamp()) { integer(0x891b); integer(0x891c); } // Fragment/read color clamp.
  auto real = [&](GLenum name, int count) {
    const size_t offset = result.real.size(); result.real.resize(offset + count);
    for (int i = 0; i < count; ++i) result.realFields.emplace_back(name, i);
    glGetDoublev(name, result.real.data() + offset);
  };
  real(GL_MODELVIEW_MATRIX,16); real(GL_PROJECTION_MATRIX,16); real(GL_TEXTURE_MATRIX,16);
  real(GL_DEPTH_RANGE,2); real(GL_ZOOM_X,1); real(GL_ZOOM_Y,1);
  real(GL_CURRENT_SECONDARY_COLOR,4);
  return result;
}
void prepareDepthTarget(GLenum function, bool writeDepth = true,
                        bool restricted = false, double rotation = 25.,
                        bool highPrecision = false) {
  glViewport(0,0,depthWidth,depthHeight);
  glDisable(GL_SCISSOR_TEST); glDisable(GL_STENCIL_TEST); glDisable(GL_BLEND);
  glDisable(GL_ALPHA_TEST); glDisable(GL_TEXTURE_2D); glDisable(GL_LIGHTING);
  glColorMask(GL_TRUE,GL_TRUE,GL_TRUE,GL_TRUE);
  glDepthMask(GL_TRUE); glStencilMask(~GLuint(0)); glClearDepth(1.); glClearStencil(3);
  glClearColor(0,0,1,1); glClear(GL_COLOR_BUFFER_BIT|GL_DEPTH_BUFFER_BIT|GL_STENCIL_BUFFER_BIT);
  glMatrixMode(GL_PROJECTION); glLoadIdentity(); glOrtho(0,depthWidth,0,depthHeight,-256,256);
  glMatrixMode(GL_MODELVIEW); glLoadIdentity();
  glEnable(GL_DEPTH_TEST); glDepthFunc(GL_ALWAYS);
  if (highPrecision) glColor4d(.121,.233,.477,1.); else glColor4d(0,0,1,1);
  glBegin(GL_QUADS);
  glVertex3d(0,0,0); glVertex3d(depthWidth,0,0);
  glVertex3d(depthWidth,depthHeight,0); glVertex3d(0,depthHeight,0);
  glEnd();
  glTranslated(128,64,20); glRotated(rotation,0,1,0);
  glDepthFunc(function); glDepthMask(writeDepth ? GL_TRUE : GL_FALSE);
  glColorMask(GL_TRUE, restricted ? GL_FALSE : GL_TRUE,GL_TRUE,GL_FALSE);
  glPixelStorei(GL_PACK_ALIGNMENT,8); glPixelStorei(GL_PACK_ROW_LENGTH,269);
  glPixelStorei(GL_PACK_SKIP_PIXELS,1); glPixelStorei(GL_PACK_SKIP_ROWS,2);
  glPixelStorei(GL_UNPACK_ALIGNMENT,8); glPixelStorei(GL_UNPACK_ROW_LENGTH,271);
  glPixelStorei(GL_UNPACK_SKIP_PIXELS,2); glPixelStorei(GL_UNPACK_SKIP_ROWS,1);
  glPixelZoom(1.5f,.75f);
  if (restricted) {
    glEnable(GL_SCISSOR_TEST); glScissor(100,32,60,64);
    glEnable(GL_STENCIL_TEST); glStencilFunc(GL_EQUAL,3,0xff);
    glStencilMask(0x0f); glStencilOp(GL_KEEP,GL_KEEP,GL_INCR);
  }
}
void drawDepthImage(const TVectorImageP &image) {
  TVectorRenderData data(TVectorRenderData::ViewerSettings(), TAffine(),
      TRect(0,0,depthWidth-1,depthHeight-1), image->getPalette());
  data.m_is3dView = true; data.m_drawRegions = false;
  tglDraw(data,image.getPointer());
}
void drawPatternedDepth() {
  glPushAttrib(GL_ALL_ATTRIB_BITS);
  glDisable(GL_ALPHA_TEST); glDisable(GL_STENCIL_TEST); glDisable(GL_SCISSOR_TEST);
  glDisable(GL_BLEND); glDisable(GL_TEXTURE_2D); glDisable(GL_CULL_FACE);
  glColorMask(GL_FALSE,GL_FALSE,GL_FALSE,GL_FALSE);
  glEnable(GL_DEPTH_TEST); glDepthFunc(GL_ALWAYS); glDepthMask(GL_TRUE);
  glPolygonMode(GL_FRONT_AND_BACK,GL_FILL);
  GLint matrixMode = 0; glGetIntegerv(GL_MATRIX_MODE,&matrixMode);
  glMatrixMode(GL_MODELVIEW); glPushMatrix(); glLoadIdentity();
  auto vertex = [](int x, int y) {
    const double z = ((x*37+y*19)%251)-125.;
    glVertex3d(x,y,z);
  };
  glBegin(GL_TRIANGLES);
  for (int y = 0; y < depthHeight; y += 16) for (int x = 0; x < depthWidth; x += 16) {
    vertex(x,y); vertex(x+16,y); vertex(x+16,y+16);
    vertex(x,y); vertex(x+16,y+16); vertex(x,y+16);
  }
  glEnd(); glPopMatrix(); glMatrixMode(GLenum(matrixMode)); glPopAttrib();
}
void requireAttachments(const DepthAttachments &expected,
                        const DepthAttachments &actual, const char *phase,
                        double tolerance = 2. / 255.) {
  size_t mismatches = 0;
  std::ostringstream detail;
  for (size_t i = 0; i < expected.depth.size(); ++i) {
    double delta = 0.;
    for (int c = 0; c < 4; ++c) {
      const double value = actual.color[4*i+c];
      delta = std::isfinite(value) ? std::max(delta,std::abs(double(expected.color[4*i+c])-value))
                                   : std::numeric_limits<double>::infinity();
    }
    if (delta > tolerance || expected.depth[i] != actual.depth[i] ||
        expected.stencil[i] != actual.stencil[i]) {
      if (mismatches < 8) {
        detail << "\npixel(" << i % depthWidth << ',' << i / depthWidth << ") color delta=" << delta
            << " depth=" << expected.depth[i] << ':' << actual.depth[i]
            << " stencil=" << expected.stencil[i] << ':' << actual.stencil[i];
      }
      ++mismatches;
    }
  }
  if (mismatches)
    throw std::runtime_error(std::string(phase) + ": attachment mismatches=" +
        std::to_string(mismatches) + detail.str() + (mismatches > 8 ? "\n[remaining pixels omitted]" : ""));
}
DepthAttachments mixedAttachments(const DepthAttachments &before,
                                  const DepthAttachments &native, double opacity) {
  DepthAttachments expected = opacity == 0. ? before : native;
  for (size_t i = 0; i < expected.color.size(); ++i)
    expected.color[i] = GLfloat(before.color[i] + opacity * (native.color[i] - before.color[i]));
  return expected;
}

void nativeDepthCropTransactions(TPalette *palette, int solidStyle, int chainStyle) {
  const auto image = imageWith(palette,solidStyle);
  auto *stroke = image->getStroke(0);
  struct ResetCapture {
    ~ResetCapture() {
      tglSetOpacityViewportCaptureForTest(false);
      tglSetOpacityFramebufferFailureForTest(0);
    }
  } reset;
  const unsigned long long targetPixels = size_t(depthWidth)*depthHeight;
  // Read every attachment after both production paths. The native draw keeps
  // its original scissor, including scratch-mask clears outside the color ROI.
  auto equivalent = [&](int scenario, bool expectCrop) {
    auto prepare = [&] {
      prepareDepthTarget(scenario == 1 ? GL_ALWAYS : GL_LEQUAL,
                         scenario != 2, scenario == 3, scenario == 4 ? 0. : 25.);
      glClearStencil(0); // Native scratch clear policy, on a nonzero sentinel.
      if (scenario == 4) glViewport(32,16,128,64);
      if (scenario == 5 || scenario == 6) drawPatternedDepth();
      if (scenario == 7) { glEnable(GL_SCISSOR_TEST); glScissor(0,0,8,8); }
    };
    DepthAttachments reference;
    TOpacityTransferStats stats[2];
    for (int mode = 0; mode < 2; ++mode) {
      prepare();
      const auto before = depthAttachments();
      const auto state = depthState();
      tglSetOpacityViewportCaptureForTest(mode == 0);
      tglSetOpacityFramebufferFailureForTest(scenario == 6 ? 4 : 0);
      tglResetOpacityTransferStatsForTest();
      bool rejected = false;
      try { drawDepthImage(image); } catch (const std::runtime_error &) { rejected = true; }
      require(rejected == (scenario == 6),"cropped/full transaction changed failure behavior");
      stats[mode] = tglOpacityTransferStatsForTest();
      state.requireEqual(depthState());
      const auto result = depthAttachments();
      if (scenario == 6)
        requireAttachments(before,result,"cropped/full rollback changed complete attachments",0.);
      if (!mode) reference = result;
      else requireAttachments(reference,result,"cropped transaction differs from forced-full capture",0.);
    }
    const unsigned long long transfers = scenario == 6 ? 2 : 3;
    require(stats[0].transactions == 1 && stats[1].transactions == 1 &&
                stats[0].croppedTransactions == 0 &&
                stats[0].colorPixels == transfers*targetPixels &&
                stats[0].depthPixels == stats[0].colorPixels &&
                stats[1].depthPixels == stats[1].colorPixels &&
                stats[0].stencilPixels == transfers*targetPixels &&
                stats[1].stencilPixels == stats[0].stencilPixels,
            "actual transfer counters omitted color/depth or full stencil work");
    require(expectCrop ? stats[1].croppedTransactions == 1 &&
                            stats[1].colorPixels < stats[0].colorPixels &&
                            stats[1].peakSnapshotBytes < stats[0].peakSnapshotBytes
                       : stats[1].croppedTransactions == 0 &&
                            stats[1].colorPixels == stats[0].colorPixels &&
                            stats[1].peakSnapshotBytes == stats[0].peakSnapshotBytes,
            "certified ROI did not reduce actual transfers, or fallback was cropped");
    std::cout << "vector-opacity-depth-roi: scenario=" << scenario
              << " style=" << stroke->getStyle() << " crop=" << expectCrop
              << " full_color_pixels=" << stats[0].colorPixels
              << " actual_color_pixels=" << stats[1].colorPixels
              << " stencil_pixels=" << stats[1].stencilPixels
              << " snapshot_bytes=" << stats[1].peakSnapshotBytes << std::endl;
  };
  for (int scenario = 0; scenario <= 7; ++scenario) {
    stroke->setOpacity(.5); stroke->setOpacityProfile({});
    equivalent(scenario,true);
    for (const StrokeOpacity::Profile &profile : {
        StrokeOpacity::Profile{{0,0},{.5,0},{.5,.5},{1,.5}},
        StrokeOpacity::Profile{{0,0},{1,0}}, StrokeOpacity::Profile{{0,1},{1,1}}}) {
      stroke->setOpacity(1.); stroke->setOpacityProfile(profile);
      equivalent(scenario,true);
    }
  }
  // An uncertified procedural style and an actual GLSL caller both retain the
  // previous full-target transaction, even when their visible artwork is small.
  stroke->setStyle(chainStyle); stroke->setOpacity(.5); stroke->setOpacityProfile({});
  equivalent(0,false);
  stroke->setStyle(solidStyle);
  QOpenGLShaderProgram program;
  require(program.addShaderFromSourceCode(QOpenGLShader::Vertex,
              "#version 120\nvoid main(){gl_Position=ftransform();gl_FrontColor=gl_Color;}") &&
              program.addShaderFromSourceCode(QOpenGLShader::Fragment,
              "#version 120\nvoid main(){gl_FragColor=gl_Color;}") && program.link() && program.bind(),
          "native shader fallback fixture could not create a compatibility program");
  struct ReleaseProgram {
    QOpenGLShaderProgram &program;
    ~ReleaseProgram() { program.release(); }
  } release{program};
  equivalent(0,false);
}

void nativeDepthTransactions(TPalette *palette, int chainStyle, int solidStyle) {
  std::cout << "vector-opacity-depth: native color/depth/stencil transactions" << std::endl;
  require(tglOpacitySnapshotFitsForTest(3840,2160,true) &&
              !tglOpacitySnapshotFitsForTest(8192,8192,true) &&
              !tglOpacitySnapshotFitsForTest(0,128,true) &&
              !tglOpacitySnapshotFitsForTest(256,-1,false),
          "native snapshot budget rejected 4K or accepted oversized/invalid dimensions");
  TOfflineGL context(TDimension(depthWidth,depthHeight)); context.makeCurrent(); context.initMatrix();
  nativeDepthCropTransactions(palette,solidStyle,chainStyle);
  const auto image = imageWith(palette,chainStyle);
  auto *stroke = image->getStroke(0);
  auto compareMix = [&](GLenum function, bool writeDepth, bool restricted,
                        double opacity, bool profile, bool highPrecision = false) {
    std::cout << "vector-opacity-depth: mix style=" << stroke->getStyle() << " function=" << function
              << " depth_write=" << writeDepth << " restricted=" << restricted << " opacity=" << opacity
              << " profile=" << profile << " high_bit=" << highPrecision << std::endl;
    stroke->setOpacity(1.); stroke->setOpacityProfile({});
    prepareDepthTarget(function,writeDepth,restricted,25.,highPrecision);
    const auto before = depthAttachments();
    drawDepthImage(image); const auto native = depthAttachments();
    if (restricted)
      require(native.stencil != before.stencil,
              "native depth fixture did not exercise stencil writes");
    prepareDepthTarget(function,writeDepth,restricted,25.,highPrecision);
    stroke->setOpacity(profile ? 1. : opacity);
    stroke->setOpacityProfile(profile ? StrokeOpacity::Profile{{0,opacity},{1,opacity}} : StrokeOpacity::Profile{});
    const auto state = depthState();
    drawDepthImage(image);
    state.requireEqual(depthState());
    requireAttachments(mixedAttachments(before,native,opacity),depthAttachments(),
        "native depth opacity differs from destination-aware color mix", highPrecision ? 1.1/1023. : 2./255.);
    if (!writeDepth) require(before.depth == native.depth, "depth-mask fixture wrote depth");
  };
  for (GLenum function : {GL_LEQUAL,GL_ALWAYS}) {
    std::cout << "vector-opacity-depth: scalar/profile mix depth function=" << function << std::endl;
    for (bool profile : {false,true}) {
      compareMix(function,true,false,.5,profile);
      compareMix(function,false,false,.5,profile);
      compareMix(function,true,true,.5,profile);
    }
    compareMix(function,true,false,.00001,true);
    compareMix(function,true,true,0.,true);
  }
  {
    using SecondaryColor = void (QOPENGLF_APIENTRYP)(GLfloat,GLfloat,GLfloat);
    auto *currentContext = QOpenGLContext::currentContext();
    auto address = currentContext->getProcAddress("glSecondaryColor3f");
    if (!address) address = currentContext->getProcAddress("glSecondaryColor3fEXT");
    require(address,"compatibility context did not expose secondary color");
    const auto secondary = reinterpret_cast<SecondaryColor>(address);
    const GLboolean originalColorSum = glIsEnabled(GL_COLOR_SUM);
    glPushAttrib(GL_ALL_ATTRIB_BITS);
    struct RestoreAttributes {
      GLboolean colorSum;
      ~RestoreAttributes() {
        glPopAttrib();
        if (colorSum) glEnable(GL_COLOR_SUM); else glDisable(GL_COLOR_SUM);
      }
    } restore{originalColorSum};
    secondary(.2f,.1f,.3f); glEnable(GL_COLOR_SUM);
    std::cout << "vector-opacity-depth: nonzero secondary color caller state" << std::endl;
    compareMix(GL_ALWAYS,true,false,.5,true);
    compareMix(GL_ALWAYS,true,false,.5,false);
    compareMix(GL_ALWAYS,true,false,1.,true);
    compareMix(GL_ALWAYS,true,true,0.,true);
    prepareDepthTarget(GL_LEQUAL,true,true);
    stroke->setOpacity(.5); stroke->setOpacityProfile({});
    const auto before = depthAttachments(); const auto state = depthState();
    bool rejected = false;
    tglSetOpacityFramebufferFailureForTest(4);
    struct ClearFailure { ~ClearFailure() { tglSetOpacityFramebufferFailureForTest(0); } } clear;
    try { drawDepthImage(image); } catch (const std::runtime_error &) { rejected = true; }
    tglSetOpacityFramebufferFailureForTest(0);
    require(rejected, "secondary-color transaction failure was hidden");
    state.requireEqual(depthState());
    requireAttachments(before,depthAttachments(),"secondary-color transaction rollback changed attachments",0.);
  }
  {
    auto *currentContext = QOpenGLContext::currentContext();
    const bool rectangle = currentContext->format().majorVersion() >= 3 ||
        currentContext->hasExtension("GL_ARB_texture_rectangle") ||
        currentContext->hasExtension("GL_EXT_texture_rectangle") ||
        currentContext->hasExtension("GL_NV_texture_rectangle");
    if (rectangle) {
      constexpr GLenum target = 0x84f5; // GL_TEXTURE_RECTANGLE.
      GLuint texture = 0; glGenTextures(1,&texture); glPushAttrib(GL_ALL_ATTRIB_BITS);
      struct RestoreTexture {
        GLuint texture;
        ~RestoreTexture() { glPopAttrib(); glDeleteTextures(1,&texture); }
      } restore{texture};
      glBindTexture(target,texture);
      glTexParameteri(target,GL_TEXTURE_MIN_FILTER,GL_NEAREST);
      glTexParameteri(target,GL_TEXTURE_MAG_FILTER,GL_NEAREST);
      glTexParameteri(target,GL_TEXTURE_WRAP_S,GL_CLAMP_TO_EDGE);
      glTexParameteri(target,GL_TEXTURE_WRAP_T,GL_CLAMP_TO_EDGE);
      const unsigned char pixels[] = {64,200,128,255,64,200,128,255,64,200,128,255,64,200,128,255};
      glPushClientAttrib(GL_CLIENT_PIXEL_STORE_BIT);
      glPixelStorei(GL_UNPACK_ALIGNMENT,1); glPixelStorei(GL_UNPACK_ROW_LENGTH,0);
      glPixelStorei(GL_UNPACK_SKIP_PIXELS,0); glPixelStorei(GL_UNPACK_SKIP_ROWS,0);
      glTexImage2D(target,0,GL_RGBA8,2,2,0,GL_RGBA,GL_UNSIGNED_BYTE,pixels);
      glPopClientAttrib(); glEnable(target); glTexCoord2f(.5f,.5f);
      glTexEnvf(GL_TEXTURE_ENV,GL_TEXTURE_ENV_MODE,GL_MODULATE);
      require(glGetError() == GL_NO_ERROR,"rectangle texture fixture setup failed");
      std::cout << "vector-opacity-depth: enabled rectangle texture caller state" << std::endl;
      compareMix(GL_ALWAYS,true,false,.5,true);
      GLint binding = 0; glGetIntegerv(0x84f6,&binding); // GL_TEXTURE_BINDING_RECTANGLE.
      require(glIsEnabled(target) && binding == GLint(texture),
              "opacity replacement did not restore rectangle texture state");
    } else std::cout << "vector-opacity-depth: rectangle textures unavailable on this driver" << std::endl;
  }

  // A blue foreground has no color delta on the blue canvas, but must still
  // write native depth. The zero half of a profile must not occlude a later
  // green probe placed between that foreground and the original canvas.
  const int blue = palette->addStyle(TPixel32::Blue);
  const auto sameColor = imageWith(palette,blue);
  auto *blueStroke = sameColor->getStroke(0);
  blueStroke->setOpacityProfile({{0,0},{.5,0},{.5,.5},{1,.5}});
  prepareDepthTarget(GL_LEQUAL,true,false,0.);
  const auto background = depthAttachments();
  drawDepthImage(sameColor); const auto painted = depthAttachments();
  const size_t left = size_t(64)*depthWidth+104, right = size_t(64)*depthWidth+152;
  require(painted.depth[left] == background.depth[left] && painted.depth[right] < background.depth[right],
          "zero profile or same-color foreground used color delta as depth coverage");
  for (int channel = 0; channel < 4; ++channel)
    require(painted.color[4*right+channel] == background.color[4*right+channel],
            "same-color native depth fixture changed canvas color");
  glMatrixMode(GL_MODELVIEW); glLoadIdentity(); glColor4d(0,1,0,1);
  glBegin(GL_QUADS); glVertex3d(96,60,10); glVertex3d(160,60,10);
  glVertex3d(160,68,10); glVertex3d(96,68,10); glEnd();
  const auto probed = depthAttachments();
  require(probed.color[4*left+1] > .99f && probed.color[4*left+2] < .01f &&
              probed.color[4*right+2] > .99f && probed.color[4*right+1] < .01f,
          "later behind probe was blocked by zero opacity or passed a same-color foreground");

  // Failure after the native draw must restore attachments, not only GL state.
  // Forced multisample and memory-budget refusals must occur before drawing.
  stroke->setOpacity(.5); stroke->setOpacityProfile({});
  for (int stage : {4,5,6}) {
    std::cout << "vector-opacity-depth: injected refusal/rollback stage=" << stage << std::endl;
    prepareDepthTarget(GL_LEQUAL,true,true);
    const auto before = depthAttachments(); const auto state = depthState();
    tglSetOpacityFramebufferFailureForTest(stage);
    struct ClearFailure { ~ClearFailure() { tglSetOpacityFramebufferFailureForTest(0); } } clearFailure;
    bool rejected = false;
    try { drawDepthImage(image); } catch (const std::runtime_error &) { rejected = true; }
    tglSetOpacityFramebufferFailureForTest(0);
    require(rejected && depthState() == state,
            "depth transaction failure was hidden or leaked caller state");
    requireAttachments(before,depthAttachments(),"failed depth transaction did not roll back",0.);
    compareMix(GL_LEQUAL,true,true,.5,false);
  }

  // Solid-style masking clears stencil bits over the caller's viewport, even
  // though its colored geometry has bounded extents. A cropped transaction
  // must not leave those clears outside its geometry snapshot after rollback.
  stroke->setStyle(solidStyle);
  compareMix(GL_LEQUAL,true,false,1.,true);
  compareMix(GL_LEQUAL,true,false,0.,true);
  prepareDepthTarget(GL_LEQUAL);
  const auto solidBefore = depthAttachments(); const auto solidState = depthState();
  stroke->setOpacity(.5); stroke->setOpacityProfile({});
  bool solidRejected = false;
  {
    tglSetOpacityFramebufferFailureForTest(4);
    struct ClearFailure { ~ClearFailure() { tglSetOpacityFramebufferFailureForTest(0); } } clearFailure;
    try { drawDepthImage(image); } catch (const std::runtime_error &) { solidRejected = true; }
  }
  require(solidRejected && depthState() == solidState,"bounded solid failure leaked caller state");
  requireAttachments(solidBefore,depthAttachments(),"bounded solid rollback changed attachments outside geometry",0.);
  // glClear in the native solid mask affects the full 256x128 attachment,
  // including pixels outside this reduced, offset 128x64 viewport.
  for (bool failAfterDraw : {false,true}) {
    std::cout << "vector-opacity-depth: offset viewport solid rollback=" << failAfterDraw << std::endl;
    prepareDepthTarget(GL_LEQUAL); glViewport(32,16,128,64);
    const auto before = depthAttachments(); const auto state = depthState();
    stroke->setOpacity(failAfterDraw ? .5 : 1.);
    stroke->setOpacityProfile(failAfterDraw ? StrokeOpacity::Profile{} : StrokeOpacity::Profile{{0,0},{1,0}});
    bool rejected = false;
    {
      tglSetOpacityFramebufferFailureForTest(failAfterDraw ? 4 : 0);
      struct ClearFailure { ~ClearFailure() { tglSetOpacityFramebufferFailureForTest(0); } } clearFailure;
      try { drawDepthImage(image); } catch (const std::runtime_error &) { rejected = true; }
    }
    require(rejected == failAfterDraw && depthState() == state,
            "offset-viewport solid changed state or failure behavior");
    requireAttachments(before,depthAttachments(),"offset viewport left changes elsewhere in its attachment",0.);
  }
  // A constant clear-depth value cannot detect conversion rounding. Produce
  // varied D24 values through actual triangle rasterization before restoring.
  for (bool failAfterDraw : {false,true}) {
    std::cout << "vector-opacity-depth: patterned native depth rollback=" << failAfterDraw << std::endl;
    prepareDepthTarget(GL_LEQUAL); drawPatternedDepth();
    const auto before = depthAttachments(); const auto state = depthState();
    auto distinct = before.depth; std::sort(distinct.begin(),distinct.end());
    require(std::unique(distinct.begin(),distinct.end())-distinct.begin() > 256,
            "patterned native fixture did not generate varied depth values");
    stroke->setOpacity(failAfterDraw ? .5 : 1.);
    stroke->setOpacityProfile(failAfterDraw ? StrokeOpacity::Profile{} : StrokeOpacity::Profile{{0,0},{1,0}});
    bool rejected = false;
    {
      tglSetOpacityFramebufferFailureForTest(failAfterDraw ? 4 : 0);
      struct ClearFailure { ~ClearFailure() { tglSetOpacityFramebufferFailureForTest(0); } } clearFailure;
      try { drawDepthImage(image); } catch (const std::runtime_error &) { rejected = true; }
    }
    require(rejected == failAfterDraw && depthState() == state,
            "patterned native depth changed failure behavior or caller state");
    requireAttachments(before,depthAttachments(),"patterned native depth was not restored exactly",0.);
  }
  stroke->setStyle(chainStyle);

  auto *functions = QOpenGLContext::currentContext()->functions();
  GLint originalFramebuffer = 0; glGetIntegerv(GL_FRAMEBUFFER_BINDING,&originalFramebuffer);
  {
    struct RestoreFramebuffer {
      QOpenGLFunctions *functions; GLint framebuffer;
      ~RestoreFramebuffer() { functions->glBindFramebuffer(GL_FRAMEBUFFER,GLuint(framebuffer)); }
    } restore{functions,originalFramebuffer};
    QOpenGLFramebufferObjectFormat format;
    format.setAttachment(QOpenGLFramebufferObject::CombinedDepthStencil);
    format.setInternalTextureFormat(0x8059); // GL_RGB10_A2, available in GL 2.1.
    QOpenGLFramebufferObject highBit(depthWidth,depthHeight,format);
    if (highBit.isValid() && highBit.bind()) {
      GLint redBits = 0; glGetIntegerv(GL_RED_BITS,&redBits);
      require(redBits >= 10,"requested high-bit fixture silently became RGBA8");
      compareMix(GL_ALWAYS,true,false,.5,true,true);
      std::cout << "vector-opacity-depth: RGB10_A2 destination passed" << std::endl;
    } else {
      while (glGetError() != GL_NO_ERROR) {}
      std::cout << "vector-opacity-depth: RGB10_A2 unavailable on this driver" << std::endl;
    }
  }
  if (separateFramebuffers()) {
    std::cout << "vector-opacity-depth: independent read framebuffer" << std::endl;
    struct RestoreFramebuffer {
      QOpenGLFunctions *functions; GLint framebuffer;
      ~RestoreFramebuffer() { functions->glBindFramebuffer(GL_FRAMEBUFFER,GLuint(framebuffer)); }
    } restore{functions,originalFramebuffer};
    QOpenGLFramebufferObjectFormat format;
    format.setAttachment(QOpenGLFramebufferObject::CombinedDepthStencil);
    QOpenGLFramebufferObject readTarget(depthWidth,depthHeight,format);
    require(readTarget.isValid() && readTarget.bind(),"separate read fixture framebuffer unavailable");
    prepareDepthTarget(GL_ALWAYS); glColorMask(GL_TRUE,GL_TRUE,GL_TRUE,GL_TRUE);
    glClearColor(0,1,0,1); glClearDepth(.25); glClearStencil(11);
    glClear(GL_COLOR_BUFFER_BIT|GL_DEPTH_BUFFER_BIT|GL_STENCIL_BUFFER_BIT);
    const auto readBefore = depthAttachments();
    functions->glBindFramebuffer(GL_FRAMEBUFFER,GLuint(originalFramebuffer));
    prepareDepthTarget(GL_ALWAYS); const auto before = depthAttachments();
    stroke->setOpacity(1.); stroke->setOpacityProfile({});
    drawDepthImage(image); const auto native = depthAttachments();
    prepareDepthTarget(GL_ALWAYS); stroke->setOpacity(.5);
    functions->glBindFramebuffer(GL_READ_FRAMEBUFFER,readTarget.handle());
    const auto state = depthState(); drawDepthImage(image);
    require(depthState() == state,"opacity changed independently bound read framebuffer state");
    functions->glBindFramebuffer(GL_READ_FRAMEBUFFER,GLuint(originalFramebuffer));
    requireAttachments(mixedAttachments(before,native,.5),depthAttachments(),
                       "opacity read pixels from the separate read target");
    require(readTarget.bind(),"could not rebind separate read fixture");
    requireAttachments(readBefore,depthAttachments(),"opacity modified separate read attachments",0.);
  } else std::cout << "vector-opacity-depth: separate read bindings unavailable on this driver" << std::endl;

  if (srgbFramebuffers()) {
    struct RestoreFramebuffer {
      QOpenGLFunctions *functions; GLint framebuffer; GLboolean srgb;
      ~RestoreFramebuffer() {
        if (srgb) glEnable(GL_FRAMEBUFFER_SRGB); else glDisable(GL_FRAMEBUFFER_SRGB);
        functions->glBindFramebuffer(GL_FRAMEBUFFER,GLuint(framebuffer));
      }
    } restore{functions,originalFramebuffer,glIsEnabled(GL_FRAMEBUFFER_SRGB)};
    QOpenGLFramebufferObjectFormat format;
    format.setAttachment(QOpenGLFramebufferObject::CombinedDepthStencil);
    format.setInternalTextureFormat(0x8c43); // GL_SRGB8_ALPHA8.
    QOpenGLFramebufferObject srgb(depthWidth,depthHeight,format);
    if (srgb.isValid() && srgb.bind()) {
      auto *currentContext = QOpenGLContext::currentContext();
      bool convertsSrgb = true;
      if (currentContext->format().majorVersion() >= 3 ||
          currentContext->hasExtension("GL_ARB_framebuffer_object")) {
        GLint encoding = 0;
        functions->glGetFramebufferAttachmentParameteriv(GL_FRAMEBUFFER,GL_COLOR_ATTACHMENT0,
            GL_FRAMEBUFFER_ATTACHMENT_COLOR_ENCODING,&encoding);
        require(encoding == GL_SRGB,"sRGB fixture did not retain its requested color encoding");
      } else {
        // EXT_framebuffer_sRGB on GL 2.1 exposes the current target's
        // capability, not the later core attachment-encoding query.
        // A valid sRGB texture FBO may legitimately be non-converting:
        // https://registry.khronos.org/OpenGL/extensions/EXT/EXT_framebuffer_sRGB.txt
        GLboolean capable = GL_FALSE;
        glGetBooleanv(0x8dba,&capable); // GL_FRAMEBUFFER_SRGB_CAPABLE_EXT.
        convertsSrgb = capable == GL_TRUE;
      }
      std::cout << "vector-opacity-depth: sRGB rendering capability=" << convertsSrgb
                << " expected_mix=" << (convertsSrgb ? "linear-light" : "stored-values") << std::endl;
      glEnable(GL_FRAMEBUFFER_SRGB);
      prepareDepthTarget(GL_ALWAYS,true,true,25.,true);
      const auto before = depthAttachments();
      stroke->setOpacity(1.); stroke->setOpacityProfile({});
      drawDepthImage(image); const auto native = depthAttachments();
      auto linear = [](double encoded) {
        return encoded <= .04045 ? encoded/12.92 : std::pow((encoded+.055)/1.055,2.4);
      };
      auto encoded = [](double linear) {
        return linear <= .0031308 ? linear*12.92 : 1.055*std::pow(linear,1./2.4)-.055;
      };
      bool distinguishesLinearMix = false;
      for (bool profile : {false,true}) for (double amount : {1.,.5,0.}) {
        prepareDepthTarget(GL_ALWAYS,true,true,25.,true);
        stroke->setOpacity(profile ? 1. : amount);
        stroke->setOpacityProfile(profile ? StrokeOpacity::Profile{{0,amount},{1,amount}} : StrokeOpacity::Profile{});
        const auto state = depthState(); drawDepthImage(image);
        require(depthState() == state,"sRGB opacity leaked caller state");
        auto expected = mixedAttachments(before,native,amount);
        for (size_t i = 0; convertsSrgb && i < expected.color.size(); ++i) {
          if (i%4 == 3 || before.color[i] == native.color[i] || amount == 0. || amount == 1.) continue;
          const double mixed = encoded((1.-amount)*linear(before.color[i]) + amount*linear(native.color[i]));
          distinguishesLinearMix |= std::abs(mixed-expected.color[i]) > 3./255.;
          expected.color[i] = GLfloat(mixed);
        }
        requireAttachments(expected,depthAttachments(),convertsSrgb
            ? "sRGB opacity mixed encoded values instead of linear light"
            : "non-converting sRGB target changed native stored-value mixing");
      }
      if (convertsSrgb)
        require(distinguishesLinearMix,"sRGB fixture did not distinguish linear and encoded mixing");
      std::cout << "vector-opacity-depth: actual sRGB identity/scalar/profile/zero passed" << std::endl;
    } else {
      while (glGetError() != GL_NO_ERROR) {}
      std::cout << "vector-opacity-depth: sRGB attachment unavailable on this driver" << std::endl;
    }
  } else std::cout << "vector-opacity-depth: sRGB framebuffer support unavailable on this driver" << std::endl;

  if (floatingColorClamp()) {
    using ClampColor = void (QOPENGLF_APIENTRYP)(GLenum,GLenum);
    auto *currentContext = QOpenGLContext::currentContext();
    auto address = currentContext->getProcAddress("glClampColor");
    if (!address) address = currentContext->getProcAddress("glClampColorARB");
    require(address,"float-color context did not expose glClampColor");
    const auto clamp = reinterpret_cast<ClampColor>(address);
    constexpr GLenum fragmentClamp = 0x891b, readClamp = 0x891c;
    GLint oldFragment = 0, oldRead = 0;
    glGetIntegerv(fragmentClamp,&oldFragment); glGetIntegerv(readClamp,&oldRead);
    struct RestoreFloatTarget {
      QOpenGLFunctions *functions; GLint framebuffer; ClampColor clamp; GLint fragment, read;
      ~RestoreFloatTarget() {
        clamp(0x891b,GLenum(fragment)); clamp(0x891c,GLenum(read));
        functions->glBindFramebuffer(GL_FRAMEBUFFER,GLuint(framebuffer));
      }
    } restore{functions,originalFramebuffer,clamp,oldFragment,oldRead};
    QOpenGLFramebufferObjectFormat format;
    format.setAttachment(QOpenGLFramebufferObject::CombinedDepthStencil);
    format.setInternalTextureFormat(0x8814); // GL_RGBA32F.
    QOpenGLFramebufferObject hdr(depthWidth,depthHeight,format);
    if (hdr.isValid() && hdr.bind()) {
      std::cout << "vector-opacity-depth: HDR target with enabled caller color clamps" << std::endl;
      GLint redBits = 0; glGetIntegerv(GL_RED_BITS,&redBits);
      require(redBits == 32,"HDR fixture silently changed float component precision");
      auto readHdr = [&]() {
        GLint original = 0; glGetIntegerv(readClamp,&original);
        clamp(readClamp,GL_FALSE);
        struct RestoreClamp {
          ClampColor clamp; GLint previous;
          ~RestoreClamp() { clamp(0x891c,GLenum(previous)); }
        } restoreClamp{clamp,original};
        return depthAttachments();
      };
      auto prepareHdr = [&]() {
        clamp(fragmentClamp,GL_FALSE); clamp(readClamp,GL_FALSE);
        prepareDepthTarget(GL_ALWAYS,true,false,0.);
        // Seed the float attachment independently of DrawPixels, which is the
        // production replacement path under test. ARB_texture_float permits
        // out-of-range components in uploads to a floating internal format:
        // https://registry.khronos.org/OpenGL/extensions/ARB/ARB_texture_float.txt
        require(glGetError() == GL_NO_ERROR,"GL error before HDR texture seed");
        glPushAttrib(GL_ALL_ATTRIB_BITS); glPushClientAttrib(GL_CLIENT_PIXEL_STORE_BIT);
        GLint unpackBuffer = 0;
        glGetIntegerv(GL_PIXEL_UNPACK_BUFFER_BINDING,&unpackBuffer);
        functions->glBindBuffer(GL_PIXEL_UNPACK_BUFFER,0);
        glBindTexture(GL_TEXTURE_2D,hdr.texture());
        glPixelStorei(GL_UNPACK_ALIGNMENT,1); glPixelStorei(GL_UNPACK_ROW_LENGTH,0);
        glPixelStorei(GL_UNPACK_SKIP_PIXELS,0); glPixelStorei(GL_UNPACK_SKIP_ROWS,0);
        glPixelStorei(GL_UNPACK_SWAP_BYTES,GL_FALSE);
        glPixelTransferi(GL_MAP_COLOR,GL_FALSE);
        for (GLenum parameter : {GL_RED_SCALE,GL_GREEN_SCALE,GL_BLUE_SCALE,GL_ALPHA_SCALE})
          glPixelTransferf(parameter,1.f);
        for (GLenum parameter : {GL_RED_BIAS,GL_GREEN_BIAS,GL_BLUE_BIAS,GL_ALPHA_BIAS})
          glPixelTransferf(parameter,0.f);
        GLint internalFormat = 0;
        glGetTexLevelParameteriv(GL_TEXTURE_2D,0,GL_TEXTURE_INTERNAL_FORMAT,&internalFormat);
        const GLfloat hdrPixel[] = {2.f,-.5f,.25f,1.f};
        for (const TPoint &point : {TPoint(8,8),TPoint(104,64)}) {
          glTexSubImage2D(GL_TEXTURE_2D,0,point.x,point.y,1,1,GL_RGBA,GL_FLOAT,hdrPixel);
        }
        const GLenum seedError = glGetError();
        functions->glBindBuffer(GL_PIXEL_UNPACK_BUFFER,GLuint(unpackBuffer));
        glPopClientAttrib(); glPopAttrib();
        const auto seeded = readHdr();
        for (const TPoint &point : {TPoint(8,8),TPoint(104,64)}) {
          const size_t pixel = size_t(point.y)*depthWidth+point.x;
          if (seedError != GL_NO_ERROR || internalFormat != 0x8814 ||
              !std::equal(hdrPixel,hdrPixel+4,seeded.color.begin()+4*pixel)) {
            std::ostringstream detail;
            detail << "HDR fixture did not retain values above one and below zero"
                << ": texture_format=" << internalFormat << " GL_error=" << seedError
                << " pixel(" << point.x << ',' << point.y << ")=";
            for (int channel = 0; channel < 4; ++channel)
              detail << seeded.color[4*pixel+channel] << ',';
            throw std::runtime_error(detail.str());
          }
        }
        clamp(fragmentClamp,GL_TRUE); clamp(readClamp,GL_TRUE);
      };
      stroke->setStyle(solidStyle); stroke->setOpacity(1.); stroke->setOpacityProfile({});
      prepareHdr(); const auto before = readHdr(); drawDepthImage(image); const auto native = readHdr();
      for (bool profile : {false,true}) for (double amount : {1.,.5}) {
        std::cout << "vector-opacity-depth: HDR opacity=" << amount << " profile=" << profile << std::endl;
        prepareHdr(); stroke->setOpacity(profile ? 1. : amount);
        stroke->setOpacityProfile(profile ? StrokeOpacity::Profile{{0,amount},{1,amount}} : StrokeOpacity::Profile{});
        const auto state = depthState(); drawDepthImage(image);
        require(depthState() == state,"HDR opacity failed to restore enabled caller color clamps");
        requireAttachments(mixedAttachments(before,native,amount),readHdr(),
                           "HDR opacity clamped attachment values",1.e-5);
      }
      prepareHdr(); const auto state = depthState(); stroke->setOpacity(.5); stroke->setOpacityProfile({});
      bool rejected = false;
      {
        tglSetOpacityFramebufferFailureForTest(4);
        struct ClearFailure { ~ClearFailure() { tglSetOpacityFramebufferFailureForTest(0); } } clearFailure;
        try { drawDepthImage(image); } catch (const std::runtime_error &) { rejected = true; }
      }
      require(rejected && depthState() == state,"HDR rollback changed clamp state or hid failure");
      requireAttachments(before,readHdr(),"HDR rollback clamped original attachment values",0.);
      stroke->setStyle(chainStyle);
      std::cout << "vector-opacity-depth: RGBA32F HDR/negative identity/mix/rollback passed" << std::endl;
    } else {
      while (glGetError() != GL_NO_ERROR) {}
      std::cout << "vector-opacity-depth: RGBA32F target unavailable on this driver" << std::endl;
    }
  } else std::cout << "vector-opacity-depth: floating color clamp support unavailable on this driver" << std::endl;

  {
    constexpr int width = 4096, height = 4096;
    GLint maxTexture = 0, maxRenderbuffer = 0;
    glGetIntegerv(GL_MAX_TEXTURE_SIZE,&maxTexture); glGetIntegerv(GL_MAX_RENDERBUFFER_SIZE,&maxRenderbuffer);
    if (maxTexture >= width && maxRenderbuffer >= width) {
      std::cout << "vector-opacity-depth: preparing actual 4096x4096 budget-refusal target" << std::endl;
      struct RestoreFramebuffer {
        QOpenGLFunctions *functions; GLint framebuffer;
        ~RestoreFramebuffer() { functions->glBindFramebuffer(GL_FRAMEBUFFER,GLuint(framebuffer)); }
      } restore{functions,originalFramebuffer};
      QOpenGLFramebufferObjectFormat format;
      format.setAttachment(QOpenGLFramebufferObject::CombinedDepthStencil);
      QOpenGLFramebufferObject oversized(width,height,format);
      if (oversized.isValid() && oversized.bind()) {
        prepareDepthTarget(GL_ALWAYS); glViewport(0,0,width,height);
        glEnable(GL_SCISSOR_TEST); glScissor(2000,1000,32,32);
        glClearColor(.2f,.4f,.6f,1.f); glClearDepth(.375); glClearStencil(13);
        glClear(GL_COLOR_BUFFER_BIT|GL_DEPTH_BUFFER_BIT|GL_STENCIL_BUFFER_BIT); glDisable(GL_SCISSOR_TEST);
        auto samples = [&]() {
          std::vector<GLfloat> colors(16); std::vector<GLuint> depth(4),stencil(4);
          glPushClientAttrib(GL_CLIENT_PIXEL_STORE_BIT);
          glPixelStorei(GL_PACK_ALIGNMENT,1); glPixelStorei(GL_PACK_ROW_LENGTH,0);
          glPixelStorei(GL_PACK_SKIP_PIXELS,0); glPixelStorei(GL_PACK_SKIP_ROWS,0);
          int i = 0;
          for (const TPointD &point : {TPointD(8,8),TPointD(2010,1010),TPointD(3000,1500),TPointD(width-1,height-1)}) {
            glReadPixels(int(point.x),int(point.y),1,1,GL_RGBA,GL_FLOAT,colors.data()+4*i);
            glReadPixels(int(point.x),int(point.y),1,1,GL_DEPTH_COMPONENT,GL_UNSIGNED_INT,depth.data()+i);
            glReadPixels(int(point.x),int(point.y),1,1,GL_STENCIL_INDEX,GL_UNSIGNED_INT,stencil.data()+i);
            ++i;
          }
          glPopClientAttrib();
          require(glGetError() == GL_NO_ERROR,"oversized target sentinel read failed");
          return std::make_pair(colors,std::make_pair(depth,stencil));
        };
        const auto before = samples(); const auto state = depthState();
        // This remains a genuine full-fallback workload after solid ROI
        // optimization; a small certified solid no longer needs 48*F bytes.
        stroke->setStyle(chainStyle);
        stroke->setOpacity(.5); stroke->setOpacityProfile({}); bool rejected = false;
        try { drawDepthImage(image); }
        catch (const std::runtime_error &error) {
          rejected = std::string(error.what()).find("512 MiB snapshot limit") != std::string::npos;
        }
        require(rejected && depthState() == state && samples() == before,
                "actual oversized target did not reject before changing attachments/state");
        std::cout << "vector-opacity-depth: actual 4096x4096 budget refusal passed; sampled four sentinels" << std::endl;
      } else {
        while (glGetError() != GL_NO_ERROR) {}
        std::cout << "vector-opacity-depth: oversized target unavailable; forced budget refusal checked" << std::endl;
      }
    } else std::cout << "vector-opacity-depth: driver target limit below4096; forced budget refusal checked" << std::endl;
  }

  for (GLenum requestedDepth : {GLenum(0x81a5),GLenum(0x81a6),GLenum(0x81a7),GLenum(0x8cac)}) {
    // Texture depth attachments expose the same complete-target contract as
    // renderbuffers. Exercise D16, D24, fixed D32 and D32F separately, without
    // assuming that a stencil attachment also exists.
    struct RestoreFramebuffer {
      QOpenGLFunctions *functions; GLint framebuffer;
      ~RestoreFramebuffer() { functions->glBindFramebuffer(GL_FRAMEBUFFER,GLuint(framebuffer)); }
    } restore{functions,originalFramebuffer};
    QOpenGLFramebufferObject depthTextureTarget(depthWidth,depthHeight);
    require(depthTextureTarget.isValid() && depthTextureTarget.bind(),"depth-texture color target unavailable");
    GLint previousTexture = 0; glGetIntegerv(GL_TEXTURE_BINDING_2D,&previousTexture);
    GLuint texture = 0; glGenTextures(1,&texture); glBindTexture(GL_TEXTURE_2D,texture);
    struct DeleteTexture {
      GLuint texture; GLint previous;
      ~DeleteTexture() { glBindTexture(GL_TEXTURE_2D,GLuint(previous)); glDeleteTextures(1,&texture); }
    } deleteTexture{texture,previousTexture};
    glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAG_FILTER,GL_NEAREST);
    glTexImage2D(GL_TEXTURE_2D,0,GLint(requestedDepth),depthWidth,depthHeight,0,GL_DEPTH_COMPONENT,
                 requestedDepth == 0x8cac ? GL_FLOAT : GL_UNSIGNED_INT,nullptr);
    functions->glFramebufferTexture2D(GL_FRAMEBUFFER,GL_DEPTH_ATTACHMENT,GL_TEXTURE_2D,texture,0);
    if (functions->glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE) {
      GLint depthBits = 0, format = 0;
      glGetIntegerv(GL_DEPTH_BITS,&depthBits);
      glGetTexLevelParameteriv(GL_TEXTURE_2D,0,GL_TEXTURE_INTERNAL_FORMAT,&format);
      std::cout << "vector-opacity-depth: depth texture requested=" << requestedDepth
                << " actual=" << format << " bits=" << depthBits << std::endl;
      const bool floatingDepth = format == 0x8cac || format == 0x8cad;
      if (depthBits > 24 && !floatingDepth) {
        prepareDepthTarget(GL_ALWAYS); drawPatternedDepth();
        const auto before = depthAttachments(); const auto state = depthState();
        stroke->setOpacity(.5); stroke->setOpacityProfile({}); bool rejected = false;
        try { drawDepthImage(image); }
        catch (const std::runtime_error &error) {
          rejected = std::string(error.what()).find("fixed or unclassified 32-bit depth") != std::string::npos;
        }
        require(rejected && depthState() == state,"fixed D32 target was accepted or changed state");
        requireAttachments(before,depthAttachments(),"fixed D32 refusal changed native attachments",0.);
        std::cout << "vector-opacity-depth: actual fixed D32 refusal passed" << std::endl;
      } else {
        compareMix(GL_ALWAYS,true,false,1.,true);
        compareMix(GL_ALWAYS,true,false,.5,false);
        for (bool failAfterDraw : {false,true}) {
          prepareDepthTarget(GL_ALWAYS); drawPatternedDepth();
          const auto before = depthAttachments(); const auto state = depthState();
          stroke->setOpacity(failAfterDraw ? .5 : 1.);
          stroke->setOpacityProfile(failAfterDraw ? StrokeOpacity::Profile{} : StrokeOpacity::Profile{{0,0},{1,0}});
          bool rejected = false;
          {
            tglSetOpacityFramebufferFailureForTest(failAfterDraw ? 4 : 0);
            struct ClearFailure { ~ClearFailure() { tglSetOpacityFramebufferFailureForTest(0); } } clearFailure;
            try { drawDepthImage(image); } catch (const std::runtime_error &) { rejected = true; }
          }
          require(rejected == failAfterDraw && depthState() == state,
                  "patterned texture depth restoration changed state or failure behavior");
          requireAttachments(before,depthAttachments(),"patterned texture depth did not restore exactly",0.);
        }
        std::cout << "vector-opacity-depth: actual depth-texture identity/mix/patterned rollback passed" << std::endl;
      }
    } else {
      while (glGetError() != GL_NO_ERROR) {}
      std::cout << "vector-opacity-depth: depth-texture format " << requestedDepth << " unavailable on this driver" << std::endl;
    }
  }
  {
    struct RestoreFramebuffer {
      QOpenGLFunctions *functions; GLint framebuffer;
      ~RestoreFramebuffer() { functions->glBindFramebuffer(GL_FRAMEBUFFER,GLuint(framebuffer)); }
    } restore{functions,originalFramebuffer};
    QOpenGLFramebufferObjectFormat format;
    format.setAttachment(QOpenGLFramebufferObject::CombinedDepthStencil); format.setSamples(4);
    QOpenGLFramebufferObject multisample(depthWidth,depthHeight,format);
    if (multisample.isValid() && multisample.format().samples() > 0 && multisample.bind()) {
      auto resolvedImage = [&]() {
        glPushClientAttrib(GL_CLIENT_PIXEL_STORE_BIT);
        glPixelStorei(GL_PACK_ALIGNMENT,1); glPixelStorei(GL_PACK_ROW_LENGTH,0);
        glPixelStorei(GL_PACK_SKIP_PIXELS,0); glPixelStorei(GL_PACK_SKIP_ROWS,0);
        const QImage pixels = multisample.toImage();
        glPopClientAttrib();
        return pixels;
      };
      prepareDepthTarget(GL_LEQUAL); const QImage before = resolvedImage();
      require(!before.isNull(),"could not resolve multisample baseline pixels");
      require(multisample.bind(),"could not rebind multisample target");
      const auto state = depthState(); bool rejected = false;
      try { drawDepthImage(image); }
      catch (const std::runtime_error &error) {
        rejected = std::string(error.what()).find("multisampl") != std::string::npos;
      }
      require(rejected && depthState() == state,
              "actual multisample target was modified or accepted silently");
      require(resolvedImage() == before,"multisample refusal changed destination pixels");
      std::cout << "vector-opacity-depth: actual multisample refusal passed samples="
                << multisample.format().samples() << std::endl;
    } else {
      while (glGetError() != GL_NO_ERROR) {}
      std::cout << "vector-opacity-depth: actual multisample target unavailable; forced refusal checked" << std::endl;
    }
  }
  // Bounded telemetry is descriptive, never a machine-dependent timing gate.
  QElapsedTimer elapsed; elapsed.start();
  for (int repeat = 0; repeat < 3; ++repeat) compareMix(GL_ALWAYS,true,false,.5,true);
  std::cout << "vector-opacity-depth: repeated opacity_draws=3 reference_draws=3 viewport=" << depthWidth << 'x' << depthHeight
            << " elapsed_ms=" << elapsed.elapsed() << " fixture_snapshot_bytes="
            << size_t(depthWidth)*depthHeight*(4*sizeof(GLfloat)+2*sizeof(GLuint))
            << " (attachment payload, not process peak)" << std::endl;
  context.doneCurrent();
}

void nativeDepthTiming(TPalette *palette, int solidStyle) {
  constexpr int width = 1920, height = 1080, strokes = 8, frames = 3;
  TOfflineGL context(TDimension(width,height)); context.makeCurrent(); context.initMatrix();
  TVectorImageP image = new TVectorImage; image->setPalette(palette);
  for (int i = 0; i < strokes; ++i) {
    const double y = 100. + i*120.;
    auto *stroke = new TStroke(std::vector<TThickPoint>{{100.,y,12.},{960.,y,12.},{1820.,y,12.}});
    stroke->setStyle(solidStyle); image->addStroke(stroke,false);
  }
  TVectorRenderData data(TVectorRenderData::ViewerSettings(),TAffine(),
      TRect(0,0,width-1,height-1),palette);
  data.m_is3dView = true; data.m_drawRegions = false;
  auto prepare = [&]() {
    glViewport(0,0,width,height); glDisable(GL_SCISSOR_TEST); glDisable(GL_STENCIL_TEST);
    glDisable(GL_BLEND); glDisable(GL_ALPHA_TEST); glDisable(GL_TEXTURE_2D);
    glColorMask(GL_TRUE,GL_TRUE,GL_TRUE,GL_TRUE); glDepthMask(GL_TRUE); glStencilMask(~GLuint(0));
    // Native translucent solid strokes clear scratch stencil bit zero, then
    // draw where that bit is zero. Unlike the transactional sentinel fixture,
    // this visible workload must retain the viewer's default clear value.
    glClearColor(0,0,1,1); glClearDepth(1.); glClearStencil(0);
    glClear(GL_COLOR_BUFFER_BIT|GL_DEPTH_BUFFER_BIT|GL_STENCIL_BUFFER_BIT);
    glEnable(GL_DEPTH_TEST); glDepthFunc(GL_ALWAYS); glColorMask(GL_TRUE,GL_TRUE,GL_TRUE,GL_FALSE);
    glMatrixMode(GL_PROJECTION); glLoadIdentity(); glOrtho(0,width,0,height,-256,256);
    glMatrixMode(GL_MODELVIEW); glLoadIdentity(); glTranslated(0,0,20);
  };
  // Initialize native geometry/style caches before measuring comparable work.
  prepare(); tglDraw(data,image.getPointer()); glFinish();
  GLint depthBits = 0, stencilBits = 0, stencilClear = 0, sampleBuffers = 0, samples = 0;
  GLint framebuffer = 0, viewport[4] = {}, colorBits[4] = {};
  glGetIntegerv(GL_DEPTH_BITS,&depthBits);
  glGetIntegerv(GL_STENCIL_BITS,&stencilBits);
  glGetIntegerv(GL_STENCIL_CLEAR_VALUE,&stencilClear);
  glGetIntegerv(GL_SAMPLE_BUFFERS,&sampleBuffers);
  glGetIntegerv(GL_SAMPLES,&samples);
  glGetIntegerv(GL_FRAMEBUFFER_BINDING,&framebuffer);
  glGetIntegerv(GL_VIEWPORT,viewport);
  glGetIntegerv(GL_RED_BITS,&colorBits[0]); glGetIntegerv(GL_GREEN_BITS,&colorBits[1]);
  glGetIntegerv(GL_BLUE_BITS,&colorBits[2]); glGetIntegerv(GL_ALPHA_BITS,&colorBits[3]);
  require(depthBits > 0 && glIsEnabled(GL_DEPTH_TEST) && sampleBuffers == 0 &&
              viewport[2] == width && viewport[3] == height,
          "1080p opacity workload requires an actual single-sample depth target");
  const size_t bytesPerPixel = 2*(4*sizeof(GLfloat)+sizeof(GLfloat)+(stencilBits ? sizeof(GLuint) : 0));
  const auto glText = [](GLenum name) {
    const GLubyte *value = glGetString(name);
    return value ? QString::fromLatin1(reinterpret_cast<const char *>(value)) : QString();
  };
  const QJsonObject provenance{
      {"platform",QSysInfo::productType()}, {"architecture",QSysInfo::currentCpuArchitecture()},
      {"qt",qVersion()}, {"gl_vendor",glText(GL_VENDOR)},
      {"gl_renderer",glText(GL_RENDERER)}, {"gl_version",glText(GL_VERSION)},
      {"target",framebuffer ? "framebuffer object" : "default framebuffer"},
      {"width",width}, {"height",height}, {"depth_bits",depthBits},
      {"stencil_bits",stencilBits}, {"stencil_clear",stencilClear},
      {"color_bits",QJsonArray{colorBits[0],colorBits[1],colorBits[2],colorBits[3]}},
      {"sample_buffers",sampleBuffers}, {"samples",samples}, {"depth_function","GL_ALWAYS"},
      {"strokes",strokes}, {"frames_per_mode",frames},
      {"full_fallback_snapshot_bytes_per_stroke",QString::number(qulonglong(size_t(width)*height*bytesPerPixel))},
      {"peak_resident_source",nativeProcessPeakResidentSource()},
      {"measurement","Default TOfflineGL; per-frame clear, native vector draw and glFinish; includes allocation, synchronous readback and replacement for modulated strokes"},
      {"memory_scope","Cumulative process peak before/after each mode; earlier fixtures may dominate. Not isolated workload RSS or process-tree memory. Actual snapshot payload and transferred pixels are recorded per mode separately from full-fallback payload."}};
  require(glGetError() == GL_NO_ERROR,"1080p opacity workload target query failed");
  std::cout << "vector-opacity-depth-workload-provenance: "
            << QJsonDocument(provenance).toJson(QJsonDocument::Compact).constData() << std::endl;
  qint64 times[3] = {};
  TOpacityTransferStats transferStats[3];
  std::vector<std::vector<GLubyte>> witnesses(3,std::vector<GLubyte>(strokes*4));
  for (int mode = 0; mode < 3; ++mode) {
    const char *name = mode == 0 ? "native" : mode == 1 ? "scalar" : "profile";
    for (int i = 0; i < strokes; ++i) {
      auto *stroke = image->getStroke(i);
      stroke->setOpacity(mode == 1 ? .5 : 1.);
      stroke->setOpacityProfile(mode == 2 ? StrokeOpacity::Profile{{0,.2},{1,.8}} : StrokeOpacity::Profile{});
    }
    glFinish();
    const qint64 peakBefore = nativeProcessPeakResidentBytes();
    tglResetOpacityTransferStatsForTest();
    std::cout << "vector-opacity-depth-workload-mode: begin " << name << std::endl;
    QJsonArray frameMilliseconds;
    for (int frame = 0; frame < frames; ++frame) {
      QElapsedTimer elapsed; elapsed.start();
      prepare(); tglDraw(data,image.getPointer()); glFinish();
      const qint64 frameNanoseconds = elapsed.nsecsElapsed();
      times[mode] += frameNanoseconds;
      frameMilliseconds.append(frameNanoseconds/1.e6);
    }
    const qint64 peakAfter = nativeProcessPeakResidentBytes();
    transferStats[mode] = tglOpacityTransferStatsForTest();
    const auto &transfers = transferStats[mode];
    require(mode == 0 ? transfers.transactions == 0
                     : transfers.transactions == strokes*frames &&
                       transfers.croppedTransactions == transfers.transactions &&
                       transfers.colorPixels < size_t(width)*height*3*strokes*frames &&
                       transfers.depthPixels == transfers.colorPixels &&
                       transfers.stencilPixels == size_t(width)*height*3*strokes*frames,
            "1080p workload did not retain full stencil and reduce actual solid color/depth transfers");
    // These untimed witnesses verify that each measured stroke actually drew;
    // scalar/profile at its midpoint must halve the native destination change.
    for (int i = 0; i < strokes; ++i)
      glReadPixels(960,100+i*120,1,1,GL_RGBA,GL_UNSIGNED_BYTE,witnesses[mode].data()+i*4);
    require(glGetError() == GL_NO_ERROR,"1080p native opacity workload left an OpenGL error");
    const QJsonObject measurement{
        {"mode",name}, {"frame_ms",frameMilliseconds}, {"total_ms",times[mode]/1.e6},
        {"color_pixels_transferred",QString::number(transfers.colorPixels)},
        {"depth_pixels_transferred",QString::number(transfers.depthPixels)},
        {"stencil_pixels_transferred",QString::number(transfers.stencilPixels)},
        {"peak_snapshot_payload_bytes",QString::number(transfers.peakSnapshotBytes)},
        {"cropped_transactions",QString::number(transfers.croppedTransactions)},
        {"process_peak_resident_before_bytes",QString::number(peakBefore)},
        {"process_peak_resident_after_bytes",QString::number(peakAfter)}};
    std::cout << "vector-opacity-depth-workload-mode: "
              << QJsonDocument(measurement).toJson(QJsonDocument::Compact).constData() << std::endl;
  }
  for (int i = 0; i < strokes; ++i) {
    QJsonArray nativePixel, scalarPixel, profilePixel;
    for (int channel = 0; channel < 4; ++channel) {
      nativePixel.append(int(witnesses[0][i*4+channel]));
      scalarPixel.append(int(witnesses[1][i*4+channel]));
      profilePixel.append(int(witnesses[2][i*4+channel]));
    }
    const QJsonObject witness{{"stroke",i}, {"x",960}, {"y",100+i*120},
        {"native_rgba",nativePixel}, {"scalar_rgba",scalarPixel}, {"profile_rgba",profilePixel}};
    std::cout << "vector-opacity-depth-workload-witness: "
              << QJsonDocument(witness).toJson(QJsonDocument::Compact).constData() << std::endl;
    require(witnesses[0][i*4] > 0,"1080p native workload stroke was not visible");
    for (int channel = 0; channel < 4; ++channel) {
      const double background = channel >= 2 ? 255. : 0.;
      const double expected = (witnesses[0][i*4+channel]+background)/2.;
      require(std::abs(witnesses[1][i*4+channel]-expected) <= 2. &&
                  std::abs(witnesses[2][i*4+channel]-expected) <= 2.,
              "1080p measured opacity strokes differ from native midpoint color mix");
    }
  }
  std::cout << "vector-opacity-depth-workload: viewport=" << width << 'x' << height
            << " strokes=" << strokes << " frames_per_mode=" << frames
            << " native_ms=" << times[0]/1.e6 << " scalar_ms=" << times[1]/1.e6
            << " profile_ms=" << times[2]/1.e6
            << " scalar_peak_snapshot_bytes=" << transferStats[1].peakSnapshotBytes
            << " profile_peak_snapshot_bytes=" << transferStats[2].peakSnapshotBytes
            << " full_fallback_snapshot_bytes=" << size_t(width)*height*bytesPerPixel
            << " (known renderer attachment payloads; not process peak; no timing gate)" << std::endl;
  context.doneCurrent();
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

void nativePlanarOpacity(TPalette *palette, int style) {
  std::cout << "vector-opacity-plane: native PlaneViewer scalar/profile visibility" << std::endl;
  auto image = imageWith(palette,style);
  auto *stroke = image->getStroke(0);
  TVectorImageP empty = new TVectorImage; empty->setPalette(palette);
  VectorAlignmentPreview::Swatch swatch(nullptr);
  swatch.resize(320,200);
  const TRectD bounds(-64,-32,64,32);
  auto capture = [&](const TVectorImageP &drawing) {
    swatch.setImage(drawing,bounds);
    swatch.show(); QCoreApplication::processEvents();
    const auto pixels = swatch.grabFramebuffer().convertToFormat(QImage::Format_RGBA8888);
    require(!pixels.isNull(),"native planar preview returned no pixels");
    return pixels;
  };
  const QImage background = capture(empty), ordinary = capture(image);
  int visible = 0;
  for (int y = 0; y < ordinary.height(); ++y)
    for (int x = 0; x < ordinary.width(); ++x)
      if (ordinary.pixel(x,y) != background.pixel(x,y)) ++visible;
  require(visible > 100,"native planar reference did not display the stroke");
  for (bool profile : {false,true}) for (double amount : {1.,.5,0.}) {
    stroke->setOpacity(profile ? 1. : amount);
    stroke->setOpacityProfile(profile ? StrokeOpacity::Profile{{0,amount},{1,amount}}
                                     : StrokeOpacity::Profile{});
    const QImage actual = capture(image);
    require(actual.size() == ordinary.size(),"native planar preview changed capture dimensions");
    for (int y = 0; y < actual.height(); ++y)
      for (int x = 0; x < actual.width()*4; ++x) {
        const double expected = background.constScanLine(y)[x] + amount *
            (double(ordinary.constScanLine(y)[x])-background.constScanLine(y)[x]);
        require(std::abs(actual.constScanLine(y)[x]-expected) <= 2.,
                "flattened native PlaneViewer lost or miscomposited stroke opacity");
      }
  }
  stroke->setOpacity(1.); stroke->setOpacityProfile({{0,0},{1,1}});
  const QImage envelope = capture(image);
  const double previewScale = std::min((envelope.width()-20.)/bounds.getLx(),
                                        (envelope.height()-20.)/bounds.getLy());
  for (double position : {-.5,.5}) {
    const int x = int(envelope.width()*.5+48.*position*previewScale);
    const int y = envelope.height()/2;
    const double amount = (position+1.)*.5;
    for (int channel = 0; channel < 4; ++channel) {
      const int byte = 4*x+channel;
      const double expected = background.constScanLine(y)[byte] + amount *
          (double(ordinary.constScanLine(y)[byte])-background.constScanLine(y)[byte]);
      require(std::abs(envelope.constScanLine(y)[byte]-expected) <= 2.,
              "flattened PlaneViewer sampled the envelope at the wrong stroke position");
    }
  }
  swatch.hide();

  // Exercise the same rank-three transform with perspective, including a
  // source stroke crossing w=0. Native clipping decides visible geometry;
  // the opacity texture must preserve it without inverse-projecting corners.
  TOfflineGL context(TDimension(256,128)); context.makeCurrent(); context.initMatrix();
  TVectorRenderData data(TVectorRenderData::ProductionSettings(),TAffine(),TRect(),palette);
  auto prepare = [&](double perspective, bool degenerate = false) {
    context.makeCurrent(); context.initMatrix(); context.clear(TPixel32::Transparent);
    glDisable(GL_DEPTH_TEST); glDisable(GL_STENCIL_TEST); glDisable(GL_SCISSOR_TEST);
    glColorMask(GL_TRUE,GL_TRUE,GL_TRUE,GL_TRUE);
    glMatrixMode(GL_MODELVIEW); glLoadIdentity(); glTranslated(128,64,0);
    glScaled(degenerate ? 0. : 1.,1.,0.);
    GLdouble projective[] = {1,0,0,perspective, 0,1,0,0, 0,0,1,0, 0,0,0,1};
    glMultMatrixd(projective);
  };
  auto pixels = [&]() {
    TRaster32P raster(256,128); context.getRaster(raster); return raster;
  };
  for (double perspective : {0.,.005,.025}) {
    stroke->setOpacity(1.); stroke->setOpacityProfile({});
    prepare(perspective); tglDraw(data,image.getPointer()); const auto ordinaryPixels = pixels();
    for (bool profile : {false,true}) for (double amount : {1.,.5}) {
      stroke->setOpacity(profile ? 1. : amount);
      stroke->setOpacityProfile(profile ? StrokeOpacity::Profile{{0,amount},{1,amount}}
                                       : StrokeOpacity::Profile{});
      prepare(perspective); tglDraw(data,image.getPointer());
      scaledPixels(ordinaryPixels,pixels(),amount,"planar perspective opacity changed native clipping");
    }
  }
  prepare(0.,true); const auto original = pixels(); const auto state = depthState();
  bool rejected = false;
  try { tglDraw(data,image.getPointer()); }
  catch (const std::runtime_error &error) {
    rejected = std::string(error.what()).find("degenerate drawing-plane") != std::string::npos;
  }
  require(rejected,"degenerate planar opacity projection was silently omitted");
  state.requireEqual(depthState());
  scaledPixels(original,pixels(),1.,"degenerate planar refusal changed the framebuffer",false);
  context.doneCurrent();
  std::cout << "vector-opacity-plane: native previews and projective clipping passed" << std::endl;
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
  nativePlanarOpacity(palette.getPointer(),styles.front());
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
  nativeDepthTransactions(palette.getPointer(),styles[1],styles.front());
  nativeDepthTiming(palette.getPointer(),styles.front());
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
