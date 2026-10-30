

#include "tgl.h"
#include "tvectorgl.h"
#include "tregion.h"
#include "tregionprop.h"
#include "tstrokeutil.h"
#include "tvectorrenderdata.h"
#include "tvectorimage.h"
#include "tpalette.h"
#include "tcolorfunctions.h"
#include "tsimplecolorstyles.h"
#include "tthreadmessage.h"
#include "tstrokeprop.h"
#include "tstrokeopacityfield.h"
#include "tstrokeopacitybounds.h"
#include "tstencilcontrol.h"

#include "tconvert.h"
#include "tcurves.h"
#include "tstrokeoutline.h"
#include <QTime>
#include <QOpenGLContext>
#include <QOpenGLFunctions>
#include <QOpenGLFramebufferObject>
#include <QMatrix4x4>
#include <QVector4D>
#include <stdexcept>

#ifndef _WIN32
#define CALLBACK
#endif

#ifndef checkErrorsByGL
#define checkErrorsByGL                                                        \
  {                                                                            \
    GLenum err = glGetError();                                                 \
    assert(err != GL_INVALID_ENUM);                                            \
    assert(err != GL_INVALID_VALUE);                                           \
    assert(err != GL_INVALID_OPERATION);                                       \
    assert(err != GL_STACK_OVERFLOW);                                          \
    assert(err != GL_STACK_UNDERFLOW);                                         \
    assert(err != GL_OUT_OF_MEMORY);                                           \
    assert(err == GL_NO_ERROR);                                                \
  }
#endif

#undef checkErrorsByGL
#define checkErrorsByGL

namespace {
thread_local TViewerOpacityErrorScope *viewerOpacityErrors = nullptr;
// An intermediate style framebuffer must fail as a whole. Its nested strokes
// cannot report success merely because the outer operation is a widget paint.
class SuspendViewerOpacityErrors {
  TViewerOpacityErrorScope *m_previous = viewerOpacityErrors;
  bool m_suspended;
public:
  explicit SuspendViewerOpacityErrors(bool suspend = true) : m_suspended(suspend) {
    if (m_suspended) viewerOpacityErrors = nullptr;
  }
  ~SuspendViewerOpacityErrors() { if (m_suspended) viewerOpacityErrors = m_previous; }
};
class OpacityRenderError : public std::runtime_error {
public:
  using std::runtime_error::runtime_error;
};
class GLAttributeScope {
public:
  explicit GLAttributeScope(GLbitfield bits = GL_ALL_ATTRIB_BITS) { glPushAttrib(bits); }
  ~GLAttributeScope() { glPopAttrib(); }
  GLAttributeScope(const GLAttributeScope &) = delete;
  GLAttributeScope &operator=(const GLAttributeScope &) = delete;
};
#ifdef BUILD_UI_PORT_TESTS
thread_local int opacityFramebufferFailure = 0;
thread_local bool opacityFullViewportCapture = false;
thread_local int opacityCaptureDepth = 0;
#endif
}

TViewerOpacityErrorScope::TViewerOpacityErrorScope()
    : m_previous(viewerOpacityErrors) { viewerOpacityErrors = this; }
TViewerOpacityErrorScope::~TViewerOpacityErrorScope() { viewerOpacityErrors = m_previous; }
void TViewerOpacityErrorScope::record(const char *message) {
  if (m_error.empty()) m_error = message;
}
bool TViewerOpacityErrorScope::currentFailed() {
  return viewerOpacityErrors && !viewerOpacityErrors->error().empty();
}
#ifdef BUILD_UI_PORT_TESTS
void tglSetOpacityFramebufferFailureForTest(int stage) { opacityFramebufferFailure = stage; }
void tglSetOpacityViewportCaptureForTest(bool full) { opacityFullViewportCapture = full; }
#endif

//-----------------------------------------------------------------------------
/*
DV_EXPORT_API void mylog(std::string s)
{
  static TThread::Mutex mutex;
  QMutexLocker sl(mutex);
  std::ofstream os("C:\\gmt\\buttami\\bu.txt", std::ios::app);

  LARGE_INTEGER ticksPerSecond;
  LARGE_INTEGER tick;
  static LARGE_INTEGER firstTick;
  static bool firstTime = true;
  long dt = 0;

  QueryPerformanceFrequency(&ticksPerSecond);
  QueryPerformanceCounter(&tick);
  if(firstTime) {firstTick = tick;firstTime=false;}
  else
  {
    dt =
(long)(1000000*(tick.QuadPart-firstTick.QuadPart)/ticksPerSecond.QuadPart);
  }
  os << dt << ":" << s << std::endl;
}
*/

//=============================================================================
#ifdef _DEBUG

bool checkQuadraticDistance(TStroke *stroke, bool checkThickness) {
  UINT i, qCount = stroke->getChunkCount();
  const TThickQuadratic *q;
  TThickPoint p1, p2, p3;

  // if the points coincide it's a point-like stroke and it's allowed
  if (qCount == 1) return true;

  for (i = 0; i != qCount; i++) {
    q  = stroke->getChunk(i);
    p1 = q->getThickP0();
    p2 = q->getThickP1();
    p3 = q->getThickP2();

    if (areAlmostEqual(p1.x, p2.x) && areAlmostEqual(p2.x, p3.x) &&
        areAlmostEqual(p1.y, p2.y) && areAlmostEqual(p2.y, p3.y) &&
        (!checkThickness || (areAlmostEqual(p1.thick, p2.thick) &&
                             areAlmostEqual(p2.thick, p3.thick))))
      return false;
  }
  return true;
}

//-----------------------------------------------------------------------------

void drawControlPoints(const TVectorRenderData &rd, TStroke *stroke,
                       double pixelSize, bool allPoints = true) {
  int i;
  TPointD p;
  glPushMatrix();

  tglMultMatrix(rd.m_aff);

  glPointSize(2.0);
  glBegin(GL_POINTS);

  if (allPoints) {
    int n = stroke->getControlPointCount();
    for (i = 0; i < n; ++i) {
      p = stroke->getControlPoint(i);
      glColor3d((i + 1) & 1, i & 1, 0.0);
      glVertex2d(p.x, p.y);
    }
  } else {
    int n = stroke->getChunkCount();
    for (i = 0; i < n; ++i) {
      const TThickQuadratic *chunk = stroke->getChunk(i);
      p                            = chunk->getP0();
      glColor3d(1.0, 0.0, 0.0);
      glVertex2d(p.x, p.y);
    }
    const TThickQuadratic *chunk = stroke->getChunk(n - 1);
    glColor3d(1.0, 0.0, 0.0);
    p = chunk->getP2();
    glVertex2d(p.x, p.y);
  }

  glEnd();
  glPopMatrix();
}

#endif

//-----------------------------------------------------------------------------

static void drawArrows(TStroke *stroke, bool onlyFirstPoint) {
  double length = stroke->getLength(0.0, 1.0);
  int points    = length / 20;
  if (points < 2) points += 1;
  double currentPosition = 0.0;

  TPointD prePoint, point, postPoint;
  glColor3d(1.0, 0.0, 0.0);
  for (int i = 0; i <= points; i++) {
    currentPosition = i / (double)points;
    point           = stroke->getPointAtLength(length * currentPosition);
    prePoint =
        (i == 0) ? point
                 : stroke->getPointAtLength(length * (currentPosition - 0.02));
    postPoint =
        (i == points)
            ? point
            : stroke->getPointAtLength(length * (currentPosition + 0.02));

    if (prePoint == postPoint) continue;

    double radian =
        std::atan2(postPoint.y - prePoint.y, postPoint.x - prePoint.x);
    double degree = radian * 180.0 / 3.14159265;

    glPushMatrix();
    glTranslated(point.x, point.y, 0);
    glRotated(degree, 0, 0, 1);
    glBegin(GL_LINES);
    glVertex2d(0, 0);
    glVertex2d(-3, -3);
    glVertex2d(0, 0);
    glVertex2d(-3, 3);
    glEnd();
    glPopMatrix();

    if (onlyFirstPoint) break;
    // make the arrow blue from the second one
    glColor3d(0.0, 0.0, 1.0);
  }
}

//-----------------------------------------------------------------------------
// Used for Guided Drawing
static void drawFirstControlPoint(const TVectorRenderData &rd,
                                  TStroke *stroke) {
  TPointD p          = stroke->getPoint(0.0);
  double length      = stroke->getLength(0.0, 1.0);
  int msecs          = QTime::currentTime().msec();
  double modifier    = (msecs / 100) * 0.1;
  TPointD startPoint = stroke->getPointAtLength(length * modifier);
  TPointD endPoint   = stroke->getPointAtLength(length * 0.10);
  double j           = 0.025;

  glPushMatrix();
  tglMultMatrix(rd.m_aff);
  if (!rd.m_animatedGuidedDrawing) glLineWidth(2.0f);
  glColor3d(0.0, 1.0, 0.0);
  if (!rd.m_animatedGuidedDrawing) {
    drawArrows(stroke, false);
  }

  if (rd.m_animatedGuidedDrawing) {
    drawArrows(stroke, true);
    glColor3d(0.0, 1.0, 0.0);
    j = 0.025 + modifier;
    glBegin(GL_LINES);
    // draw the first animated section
    for (int i = 0; i < 8; i++) {
      endPoint = stroke->getPointAtLength(length * j);
      glVertex2d(startPoint.x, startPoint.y);
      glVertex2d(endPoint.x, endPoint.y);
      startPoint = endPoint;
      j += 0.025;
      if (j > 1) {
        j -= 1;
        startPoint = stroke->getPointAtLength(length * j);
      }
    }

    modifier = modifier + 0.5;
    if (modifier >= 1) modifier -= 1;
    startPoint = stroke->getPointAtLength(length * modifier);
    j          = 0.025 + modifier;

    // draw another animated section
    for (int i = 0; i < 8; i++) {
      endPoint = stroke->getPointAtLength(length * j);
      glVertex2d(startPoint.x, startPoint.y);
      glVertex2d(endPoint.x, endPoint.y);
      startPoint = endPoint;
      j += 0.025;
      if (j > 1) {
        j -= 1;
        startPoint = stroke->getPointAtLength(length * j);
      }
    }
    glEnd();
  }

  glLineWidth(1.0f);
  glPopMatrix();
}

//=============================================================================

/*TPixel TransparencyCheckBlackBgInk = TPixel(255,255,255);
TPixel TransparencyCheckWhiteBgInk = TPixel(0,0,0);
TPixel TransparencyCheckPaint = TPixel(127,127,127);*/
static int Index = 0;
void tglDraw(const TVectorRenderData &rd, TRegion *r, bool pushAttribs) {
  checkErrorsByGL;
  assert(r);
  checkErrorsByGL;
  if (!r) return;
  bool alphaChannel = rd.m_alphaChannel;
  checkErrorsByGL;

  int j          = 0;
  bool visible   = false;
  int colorCount = 0;

  TColorStyleP style;
  if (rd.m_paintCheckEnabled && r->getStyle() == rd.m_paintIndex) {
    TSolidColorStyle *colorStyle = new TSolidColorStyle();
    colorStyle->setMainColor(rd.m_paintCheckColor);
    style = colorStyle;
  } else if (rd.m_tcheckEnabled) {
    static TSolidColorStyle *color = new TSolidColorStyle();
    color->addRef();
    color->setMainColor(rd.m_tCheckPaint);
    style = color;
  } else
    style = rd.m_palette->getStyle(r->getStyle());

  colorCount = style->getColorParamCount();
  if (colorCount == 0) {  // for example texture
    visible = true;
  } else {
    visible = false;
    for (j = 0; j < colorCount && !visible; j++) {
      TPixel32 color = style->getColorParamValue(j);
      if (rd.m_cf) color = (*(rd.m_cf))(color);
      if (color.m != 0) visible = true;
    }
  }
  if (visible) {
    TRegionProp *prop = r->getProp(/*rd.m_palette*/);
    /// this code was inside tregion::getprop/////
    int styleId = r->getStyle();
    if (styleId) {
      // TColorStyle * style = rd.m_palette->getStyle(styleId);
      if (!style->isRegionStyle() || style->isEnabled() == false) {
        prop = 0;
      } else {
        // Warning: The same remark of stroke props holds here.
        if (!prop || style.getPointer() != prop->getColorStyle()) {
          r->setProp(style->makeRegionProp(r));
          prop = r->getProp();
        }
      }
    }

    ////// draw
    if (prop) {
      if (pushAttribs) glPushAttrib(GL_ALL_ATTRIB_BITS);

      tglEnableLineSmooth(true);
// #define DRAW_EDGE_NUMBERS
#ifdef DRAW_EDGE_NUMBERS
      glPushMatrix();
      tglMultMatrix(rd.m_aff);
      switch (Index % 7) {
      case 0:
        tglColor(TPixel::Red);
        break;
      case 1:
        tglColor(TPixel::Green);
        break;
      case 2:
        tglColor(TPixel::Blue);
        break;
      case 3:
        tglColor(TPixel::Cyan);
        break;
      case 4:
        tglColor(TPixel::Magenta);
        break;
      case 5:
        tglColor(TPixel::Yellow);
        break;
      case 6:
        tglColor(TPixel::Black);
        break;
      default:
        tglColor(TPixel::Red);
        break;
      }

      Index++;
      if (rIndex == 2) {
        double y = r->getEdge(0)
                       ->m_s
                       ->getThickPoint(
                           (r->getEdge(0)->m_w0 + r->getEdge(0)->m_w1) / 2.0)
                       .y;
        tglDrawSegment(TPointD(-1000, y), TPointD(1000, y));
      }

      for (int i = 0; i < (int)r->getEdgeCount(); i++) {
        TEdge *e  = r->getEdge(i);
        TPointD p = e->m_s->getPoint(0.8 * e->m_w0 + 0.2 * e->m_w1);
        if (i == 0)
          tglDrawText(p,
                      (QString::number(rIndex) + QString("-0")).toStdString());
        else
          tglDrawText(p, QString::number(i).toStdString());
        if (e->m_index == 3) {
          tglColor(TPixel::Black);
          TStroke *s = e->m_s;
          drawPoint(s->getChunk(0)->getP0(), .3);
          tglColor(TPixel::Red);
          tglDrawText(s->getChunk(0)->getP0(),
                      QString::number(0).toStdString());
          for (int ii = 0; ii < s->getChunkCount(); ii++) {
            drawPoint(s->getChunk(ii)->getP2(), .3);
            if (ii < s->getChunkCount() - 1) {
              tglColor(TPixel::Red);
              tglDrawText(s->getChunk(ii)->getP2(),
                          QString::number(ii + 1).toStdString());
            }
          }
        }
      }
      glPopMatrix();
#endif

      if (alphaChannel) {
        GLboolean red, green, blue, alpha;
        tglGetColorMask(red, green, blue, alpha);

        // Draw RGB channels
        tglEnableBlending(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
        glColorMask(red, green, blue, GL_FALSE);
        prop->draw(rd);

        // Draw Matte channel
        tglEnableBlending(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
        glColorMask(GL_FALSE, GL_FALSE, GL_FALSE, alpha);
        prop->draw(rd);

        glColorMask(red, green, blue, alpha);
      } else {
        // workaround: in render, the filled areas of custom styles disappeared.
        if (!rd.m_isOfflineRender || !rd.m_isImagePattern)
          tglRgbOnlyColorMask();  // RGB components only

        prop->draw(rd);
      }

      if (pushAttribs) glPopAttrib();
    }
  }

  for (UINT i = 0; i < r->getSubregionCount(); i++)
    tglDraw(rd, r->getSubregion(i), pushAttribs);
  checkErrorsByGL;
}

//-----------------------------------------------------------------------------

void tglDrawMask(const TVectorRenderData &rd1, const TVectorImage *vim) {
  UINT i;
  assert(vim);
  if (!vim) return;
  TVectorRenderData rd(rd1);

  GLAttributeScope attributes;

  if (!rd.m_palette) {
    TPalette *vPalette = vim->getPalette();
    assert(vPalette);
    rd.m_palette = vPalette;
  }
  for (i = 0; i < vim->getRegionCount(); i++)
    tglDraw(rd, vim->getRegion(i), false);

}

//-----------------------------------------------------------------------------

namespace {
bool isOThick(const TStroke *s) {
  int i;
  for (i = 0; i < s->getControlPointCount(); i++)
    if (s->getControlPoint(i).thick != 0) return false;
  return true;
}
}  // namespace

namespace {
// Viewer and production contexts share this framebuffer adapter. The native
// address fallback also supports externally supplied WGL contexts.
class OpacityFramebuffer {
  using Gen = void (QOPENGLF_APIENTRYP)(GLsizei, GLuint *);
  using Delete = void (QOPENGLF_APIENTRYP)(GLsizei, const GLuint *);
  using Bind = void (QOPENGLF_APIENTRYP)(GLenum, GLuint);
  using AttachTexture = void (QOPENGLF_APIENTRYP)(GLenum, GLenum, GLenum, GLuint, GLint);
  using Storage = void (QOPENGLF_APIENTRYP)(GLenum, GLenum, GLsizei, GLsizei);
  using AttachRenderbuffer = void (QOPENGLF_APIENTRYP)(GLenum, GLenum, GLenum, GLuint);
  using Check = GLenum (QOPENGLF_APIENTRYP)(GLenum);
  Gen m_genBuffers, m_genRenderbuffers;
  Delete m_deleteBuffers, m_deleteRenderbuffers;
  Bind m_bindBuffer, m_bindRenderbuffer;
  AttachTexture m_attachTexture;
  Storage m_storage;
  AttachRenderbuffer m_attachRenderbuffer;
  Check m_check;
  GLuint m_framebuffer = 0, m_texture = 0, m_depthStencil = 0;
  GLint m_previous = 0;
  static QFunctionPointer address(const char *name) {
    auto lookup = [](const char *entry) {
      QFunctionPointer result = nullptr;
      if (auto *context = QOpenGLContext::currentContext())
        result = context->getProcAddress(entry);
#ifdef _WIN32
      if (!result) {
        auto pointer = wglGetProcAddress(entry);
        if (pointer && pointer != reinterpret_cast<PROC>(1) && pointer != reinterpret_cast<PROC>(2) &&
            pointer != reinterpret_cast<PROC>(3) && pointer != reinterpret_cast<PROC>(-1))
          result = reinterpret_cast<QFunctionPointer>(pointer);
      }
#endif
      return result;
    };
    QFunctionPointer result = lookup(name);
    // Qt's supported compatibility contexts can expose the EXT spellings
    // instead of the OpenGL 3 core entry points. Signatures and enums match.
    if (!result) result = lookup((QByteArray(name) + "EXT").constData());
    if (!result) throw std::runtime_error("Stroke opacity envelopes require OpenGL framebuffer support");
    return result;
  }

public:
  OpacityFramebuffer(int width, int height) {
#ifdef BUILD_UI_PORT_TESTS
    if (opacityFramebufferFailure == 1 ||
        (opacityFramebufferFailure == 3 && opacityCaptureDepth > 0))
      throw std::runtime_error("Stroke opacity envelopes require OpenGL framebuffer support");
#endif
    m_genBuffers = reinterpret_cast<Gen>(address("glGenFramebuffers"));
    m_deleteBuffers = reinterpret_cast<Delete>(address("glDeleteFramebuffers"));
    m_bindBuffer = reinterpret_cast<Bind>(address("glBindFramebuffer"));
    m_attachTexture = reinterpret_cast<AttachTexture>(address("glFramebufferTexture2D"));
    m_genRenderbuffers = reinterpret_cast<Gen>(address("glGenRenderbuffers"));
    m_deleteRenderbuffers = reinterpret_cast<Delete>(address("glDeleteRenderbuffers"));
    m_bindRenderbuffer = reinterpret_cast<Bind>(address("glBindRenderbuffer"));
    m_storage = reinterpret_cast<Storage>(address("glRenderbufferStorage"));
    m_attachRenderbuffer = reinterpret_cast<AttachRenderbuffer>(address("glFramebufferRenderbuffer"));
    m_check = reinterpret_cast<Check>(address("glCheckFramebufferStatus"));
    GLint texture = 0, renderbuffer = 0;
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &m_previous);
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &texture);
    glGetIntegerv(GL_RENDERBUFFER_BINDING, &renderbuffer);
    m_genBuffers(1, &m_framebuffer); m_bindBuffer(GL_FRAMEBUFFER, m_framebuffer);
    glGenTextures(1, &m_texture); glBindTexture(GL_TEXTURE_2D, m_texture);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    m_attachTexture(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, m_texture, 0);
    m_genRenderbuffers(1, &m_depthStencil); m_bindRenderbuffer(GL_RENDERBUFFER, m_depthStencil);
    m_storage(GL_RENDERBUFFER, GL_DEPTH24_STENCIL8, width, height);
    m_attachRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, m_depthStencil);
    m_attachRenderbuffer(GL_FRAMEBUFFER, GL_STENCIL_ATTACHMENT, GL_RENDERBUFFER, m_depthStencil);
    bool complete = m_check(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
#ifdef BUILD_UI_PORT_TESTS
    if (opacityFramebufferFailure == 2) complete = false;
#endif
    glBindTexture(GL_TEXTURE_2D, texture); m_bindRenderbuffer(GL_RENDERBUFFER, renderbuffer);
    restore();
    if (!complete) {
      release();
      throw std::runtime_error("Cannot allocate stroke opacity framebuffer");
    }
  }
  GLuint texture() const { return m_texture; }
  void bind() { m_bindBuffer(GL_FRAMEBUFFER, m_framebuffer); }
  void restore() { m_bindBuffer(GL_FRAMEBUFFER, m_previous); }
  void release() {
    if (m_texture) glDeleteTextures(1, &m_texture), m_texture = 0;
    if (m_depthStencil) m_deleteRenderbuffers(1, &m_depthStencil), m_depthStencil = 0;
    if (m_framebuffer) m_deleteBuffers(1, &m_framebuffer), m_framebuffer = 0;
  }
  ~OpacityFramebuffer() { release(); }
};

// Composite the native style once, then modulate its complete premultiplied
// result. This path applies to solid, procedural, raster and vector patterns
// equally; splitting the geometry into translucent sub-strokes would alter
// pattern spacing and darken overlapping joins.
void drawOpacityEnvelope(const TVectorRenderData &rd, const TStroke &stroke) {
  GLint viewport[4];
  glGetIntegerv(GL_VIEWPORT, viewport);
  if (viewport[2] <= 0 || viewport[3] <= 0) return;
  GLdouble model[16], projection[16];
  glPushMatrix();
  tglMultMatrix(rd.m_aff);
  glGetDoublev(GL_MODELVIEW_MATRIX, model);
  glGetDoublev(GL_PROJECTION_MATRIX, projection);
  glPopMatrix();
  QMatrix4x4 modelMatrix, projectionMatrix;
  for (int i = 0; i < 16; ++i) {
    modelMatrix.data()[i] = float(model[i]);
    projectionMatrix.data()[i] = float(projection[i]);
  }
  const QMatrix4x4 mapping = projectionMatrix * modelMatrix;
  bool invertible = false;
  const QMatrix4x4 inverse = mapping.inverted(&invertible);
  if (!invertible) return;
  // Unknown procedural styles can draw outside their legacy geometry bbox.
  // Their fallback is the full output viewport, never a guessed crop.
  const auto *style = rd.m_palette->getStyle(stroke.getStyle());
  TRectD box;
  int x0 = viewport[0], y0 = viewport[1];
  int x1 = viewport[0] + viewport[2], y1 = viewport[1] + viewport[3];
  bool bounded = style && StrokeOpacity::renderBounds(*style, stroke, box);
#ifdef BUILD_UI_PORT_TESTS
  bounded = bounded && !opacityFullViewportCapture;
#endif
  if (bounded) {
    double left = x1, right = x0, bottom = y1, top = y0;
    bool finiteProjection = true;
    double firstW = 0.;
    for (const TPointD &point : {box.getP00(), box.getP01(), box.getP10(), box.getP11()}) {
      QVector4D projected = mapping * QVector4D(float(point.x), float(point.y), 0.f, 1.f);
      if (std::abs(projected.w()) < 1e-10 || (firstW && firstW * projected.w() < 0.)) {
        finiteProjection = false; break;
      }
      firstW = projected.w();
      const double x = viewport[0] + (.5 + .5 * projected.x() / projected.w()) * viewport[2];
      const double y = viewport[1] + (.5 + .5 * projected.y() / projected.w()) * viewport[3];
      if (!std::isfinite(x) || !std::isfinite(y)) { finiteProjection = false; break; }
      left = std::min(left, x); right = std::max(right, x);
      bottom = std::min(bottom, y); top = std::max(top, y);
    }
    if (finiteProjection) {
      // Circlets have a minimum screen-space radius; the pad also contains
      // line antialiasing at any viewer zoom.
      const double pad = style->getTagId() == 106 ? 4. : 2.;
      x0 = int(std::clamp(std::floor(left) - pad, double(viewport[0]), double(x1)));
      y0 = int(std::clamp(std::floor(bottom) - pad, double(viewport[1]), double(y1)));
      x1 = int(std::clamp(std::ceil(right) + pad, double(viewport[0]), double(x1)));
      y1 = int(std::clamp(std::ceil(top) + pad, double(viewport[1]), double(y1)));
    }
  }
  if (glIsEnabled(GL_SCISSOR_TEST)) {
    GLint scissor[4]; glGetIntegerv(GL_SCISSOR_BOX, scissor);
    x0 = std::max(x0, scissor[0]); y0 = std::max(y0, scissor[1]);
    x1 = std::min(x1, scissor[0] + scissor[2]); y1 = std::min(y1, scissor[1] + scissor[3]);
  }
  if (x1 <= x0 || y1 <= y0) return;
  const int width = x1 - x0, height = y1 - y0;
  OpacityFramebuffer buffer(width, height);
  const bool hasEnvelope = !stroke.getOpacityProfile().empty();
  std::vector<unsigned char> pixels;
  if (hasEnvelope) pixels.resize(size_t(width) * size_t(height) * 4);
  glPushAttrib(GL_ALL_ATTRIB_BITS);
  glPushClientAttrib(GL_CLIENT_PIXEL_STORE_BIT);
  struct Restore {
    OpacityFramebuffer &buffer;
    bool active = true;
    void restore() {
      if (!active) return;
      buffer.restore();
      glPopClientAttrib(); glPopAttrib(); active = false;
    }
    ~Restore() { restore(); }
  } restore{buffer};
  buffer.bind();
  glViewport(viewport[0] - x0, viewport[1] - y0, viewport[2], viewport[3]);
  glDisable(GL_SCISSOR_TEST); glDisable(GL_STENCIL_TEST); glDisable(GL_DEPTH_TEST);
  glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
  glDepthMask(GL_TRUE); glStencilMask(~GLuint(0));
  glClearColor(0, 0, 0, 0); glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);
  TStroke unmodulated(stroke);
  unmodulated.setOpacityProfile({});
  unmodulated.setOpacity(1.);
  TVectorRenderData data(rd);
  data.m_alphaChannel = true;
  // Legacy procedural bounds may exclude ornaments. The FBO/viewport already
  // clips actual pixels, so do not discard a style using its geometry bbox.
  data.m_clippingRect = TRect();
  {
    TStencilControl::FramebufferScope stencilScope;
    SuspendViewerOpacityErrors suspendViewerErrors;
#ifdef BUILD_UI_PORT_TESTS
    struct CaptureDepth {
      CaptureDepth() { ++opacityCaptureDepth; }
      ~CaptureDepth() { --opacityCaptureDepth; }
    } captureDepth;
#endif
    tglDraw(data, &unmodulated, true);
  }
  if (hasEnvelope) {
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glPixelStorei(GL_PACK_ROW_LENGTH, 0);
    glPixelStorei(GL_PACK_SKIP_ROWS, 0); glPixelStorei(GL_PACK_SKIP_PIXELS, 0);
    glReadPixels(0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data());
  }
  restore.restore();

  const double projectedScale = std::max(
      std::hypot(double(mapping(0,0)) * viewport[2], double(mapping(1,0)) * viewport[3]),
      std::hypot(double(mapping(0,1)) * viewport[2], double(mapping(1,1)) * viewport[3])) * .5;
  std::unique_ptr<StrokeOpacity::Field> field;
  if (!stroke.getOpacityProfile().empty())
    field.reset(new StrokeOpacity::Field(stroke, projectedScale > 0. ? .125 / projectedScale : .05));
  auto planePoint = [&](double x, double y) {
    const float nx = float(2. * (x - viewport[0]) / viewport[2] - 1.);
    const float ny = float(2. * (y - viewport[1]) / viewport[3] - 1.);
    QVector4D nearPoint = inverse * QVector4D(nx, ny, -1, 1);
    QVector4D farPoint = inverse * QVector4D(nx, ny, 1, 1);
    if (nearPoint.w() == 0 || farPoint.w() == 0) return TPointD();
    nearPoint /= nearPoint.w(); farPoint /= farPoint.w();
    const double dz = farPoint.z() - nearPoint.z();
    const double z = std::abs(dz) > 1e-12 ? -nearPoint.z() / dz : 0.;
    return TPointD(nearPoint.x() + z * (farPoint.x() - nearPoint.x()),
                   nearPoint.y() + z * (farPoint.y() - nearPoint.y()));
  };
  for (int y = 0; hasEnvelope && y < height; ++y) for (int x = 0; x < width; ++x) {
    auto *pixel = &pixels[(size_t(y) * width + x) * 4];
    if (!pixel[3]) continue;
    const double opacity = stroke.getOpacity() * (field ? field->opacity(planePoint(x + x0 + .5, y + y0 + .5)) : 1.);
    for (int channel = 0; channel < 4; ++channel)
      pixel[channel] = static_cast<unsigned char>(std::lround(pixel[channel] * opacity));
  }
  // The parent stencil, scissor and write masks apply to the final composite.
  glPushAttrib(GL_ALL_ATTRIB_BITS);
  glPushClientAttrib(GL_CLIENT_PIXEL_STORE_BIT);
  glMatrixMode(GL_PROJECTION); glPushMatrix(); glLoadIdentity();
  glOrtho(viewport[0], viewport[0] + viewport[2], viewport[1], viewport[1] + viewport[3], -1, 1);
  glMatrixMode(GL_MODELVIEW); glPushMatrix(); glLoadIdentity();
  glMatrixMode(GL_TEXTURE); glPushMatrix(); glLoadIdentity();
  glMatrixMode(GL_MODELVIEW);
  glDisable(GL_TEXTURE_GEN_S); glDisable(GL_TEXTURE_GEN_T);
  glDisable(GL_LIGHTING);
  glEnable(GL_BLEND); glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
  if (!glIsEnabled(GL_ALPHA_TEST)) { glEnable(GL_ALPHA_TEST); glAlphaFunc(GL_GREATER, 0); }
  glEnable(GL_TEXTURE_2D); glBindTexture(GL_TEXTURE_2D, buffer.texture());
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
  if (hasEnvelope) {
    glTexEnvf(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_REPLACE);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1); glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
    glPixelStorei(GL_UNPACK_SKIP_ROWS, 0); glPixelStorei(GL_UNPACK_SKIP_PIXELS, 0);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data());
  } else {
    // Uniform opacity stays entirely on the GPU: no readback, field lookup or
    // texture re-upload for ordinary translucent pencil/brush drawings.
    glTexEnvf(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE);
    const double opacity = stroke.getOpacity();
    glColor4d(opacity, opacity, opacity, opacity);
  }
  glDisable(GL_CULL_FACE); glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);
  // A plane's projected depth is linear in screen coordinates. Reconstruct
  // each corner depth so 3D viewer occlusion remains identical to the original
  // stroke; a constant raster Z would incorrectly place it at the near plane.
  auto vertex = [&](double x, double y, double u, double v) {
    const TPointD point = planePoint(x, y);
    const QVector4D projected = mapping * QVector4D(float(point.x), float(point.y), 0, 1);
    const double depth = projected.w() != 0 ? projected.z() / projected.w() : 0.;
    glTexCoord2d(u, v); glVertex3d(x, y, -depth);
  };
  glBegin(GL_QUADS);
  vertex(x0,y0,0,0); vertex(x1,y0,1,0); vertex(x1,y1,1,1); vertex(x0,y1,0,1);
  glEnd();
  glPopClientAttrib();
  glMatrixMode(GL_TEXTURE); glPopMatrix();
  glMatrixMode(GL_MODELVIEW); glPopMatrix(); glMatrixMode(GL_PROJECTION); glPopMatrix(); glMatrixMode(GL_MODELVIEW);
  glPopAttrib();
}
} // namespace

void tglDraw(const TVectorRenderData &rd, const TStroke *s, bool pushAttribs) {
  SuspendViewerOpacityErrors strictRender(rd.m_isOfflineRender || rd.m_isIcon);
  assert(s);
  if (!s) return;
  if (s->getOpacity() <= 0.) return;
  if (s->getOpacity() < 1. || !s->getOpacityProfile().empty()) {
    GLint renderMode = GL_RENDER;
    glGetIntegerv(GL_RENDER_MODE, &renderMode);
    if (renderMode == GL_SELECT) {
      // Native picking records geometric hits, not framebuffer pixels. The
      // opacity composite rectangle must never replace the stroke's hit area.
      TStroke geometry(*s);
      geometry.setOpacity(1.);
      geometry.setOpacityProfile({});
      tglDraw(rd, &geometry, pushAttribs);
      return;
    }
    try { drawOpacityEnvelope(rd, *s); }
    catch (const OpacityRenderError &error) {
      if (viewerOpacityErrors && !rd.m_isOfflineRender && !rd.m_isIcon)
        viewerOpacityErrors->record(error.what());
      else throw;
    }
    catch (const std::exception &error) {
      const std::string message = std::string("Cannot render stroke opacity: ") + error.what();
      if (viewerOpacityErrors && !rd.m_isOfflineRender && !rd.m_isIcon)
        viewerOpacityErrors->record(message.c_str());
      else throw OpacityRenderError(message);
    }
    return;
  }

  TStrokeProp *prop  = 0;
  bool pushedAttribs = false;

  try {
    TColorStyleP style;
    TStroke *stroke = const_cast<TStroke *>(s);
    if (rd.m_inkCheckEnabled && s->getStyle() == rd.m_colorCheckIndex) {
      TSolidColorStyle *colorStyle = new TSolidColorStyle();
      colorStyle->setMainColor(rd.m_inkCheckColor);
      style = colorStyle;
    } else if (rd.m_ink1CheckEnabled && s->getStyle() == 1) {
      TSolidColorStyle *colorStyle = new TSolidColorStyle();
      colorStyle->setMainColor(rd.m_ink1CheckColor);
      style = colorStyle;
    } else if (rd.m_tcheckEnabled) {
      static TSolidColorStyle *color = new TSolidColorStyle();
      color->addRef();
      color->setMainColor(rd.m_tCheckInk);
      style = color;
    } else
      style = rd.m_palette->getStyle(stroke->getStyle());

    if (!rd.m_show0ThickStrokes && isOThick(s) &&
        dynamic_cast<TSolidColorStyle *>(
            style.getPointer())  // This is probably to exclude
                                 // TCenterlineStrokeStyle-like styles
        && !rd.m_tcheckEnabled)  // I wonder why this?
      return;

    // const TStroke& stroke = *s;   // is it needed?

    assert(rd.m_palette);

    prop = s->getProp(/*rd.m_palette*/);
    /////this code was inside tstroke::getprop/////////
    if (prop) prop->getMutex()->lock();

    if (!style->isStrokeStyle() || style->isEnabled() == false) {
      if (prop) prop->getMutex()->unlock();

      prop = 0;
    } else {
      // Warning: the following pointers check is conceptually wrong - we
      // keep it because the props maintain SMART POINTER-like reference to
      // the associated style. This prevents the style from being destroyed
      // while still referenced by the prop.
      if (!prop || style.getPointer() != prop->getColorStyle()) {
        if (prop) prop->getMutex()->unlock();

        stroke->setProp(style->makeStrokeProp(stroke));
        prop = stroke->getProp();
        if (prop) prop->getMutex()->lock();
      }
    }

    //--------- draw ------------
    if (!prop) return;

    if (pushAttribs) glPushAttrib(GL_ALL_ATTRIB_BITS), pushedAttribs = true;

    bool alphaChannel = rd.m_alphaChannel, antialias = rd.m_antiAliasing;
    TVectorImagePatternStrokeProp *aux =
        dynamic_cast<TVectorImagePatternStrokeProp *>(prop);
    if (aux)  // the vector image patterns return to this function....by not
              // executing the else body, we avoid drawing them twice!
      prop->draw(rd);
    else {
      if (antialias)
        tglEnableLineSmooth(true);
      else
        tglEnableLineSmooth(false);

      if (alphaChannel) {
        GLboolean red, green, blue, alpha;
        tglGetColorMask(red, green, blue, alpha);

        // Draw RGB channels
        tglEnableBlending(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
        glColorMask(red, green, blue, GL_FALSE);
        prop->draw(rd);

        // Draw Matte channel
        tglEnableBlending(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
        glColorMask(GL_FALSE, GL_FALSE, GL_FALSE, alpha);
        prop->draw(rd);

        glColorMask(red, green, blue, alpha);
      } else {
        tglEnableBlending(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
        prop->draw(rd);
      }
    }

    if (pushAttribs) glPopAttrib(), pushedAttribs = false;

    prop->getMutex()->unlock();
    //---------------------
  } catch (const OpacityRenderError &) {
    // Nested vector brush rendering must preserve the actionable opacity
    // failure instead of exporting a partially painted brush silently.
    if (prop) prop->getMutex()->unlock();
    if (pushedAttribs) glPopAttrib();
    throw;
  } catch (...) {
    if (prop) prop->getMutex()->unlock();
    if (pushedAttribs) glPopAttrib();
  }
}

//------------------------------------------------------------------------------------

static void tglDoDraw(const TVectorRenderData &rd, TRegion *r) {
  bool visible   = false;
  int colorCount = 0;
  if (!r) return;

  TColorStyleP style = rd.m_palette->getStyle(r->getStyle());
  colorCount         = style->getColorParamCount();
  if (colorCount == 0)  // for example texture
    visible = true;
  else {
    visible = false;
    for (int j = 0; j < colorCount && !visible; j++) {
      TPixel32 color = style->getColorParamValue(j);
      if (rd.m_cf) color = (*(rd.m_cf))(color);
      if (color.m != 0) visible = true;
    }
  }
  if (visible)
    tglDraw(rd, r, false);
  else
    for (UINT j = 0; j < r->getSubregionCount(); j++)
      tglDraw(rd, r->getSubregion(j), false);
}

//------------------------------------------------------------------------------------

static bool tglDoDraw(const TVectorRenderData &rd, const TStroke *s) {
  bool visible   = false;
  int colorCount = 0;

  const TPalette *palette = rd.m_palette;

  int styleId = s->getStyle();
  // assert(0<=styleId && styleId<stylesCount);
  TColorStyleP style = palette->getStyle(styleId);
  assert(style);
  colorCount = style->getColorParamCount();
  if (colorCount == 0)
    visible = true;
  else {
    visible = false;
    for (int j = 0; j < style->getColorParamCount() && !visible; j++) {
      TPixel32 color = style->getColorParamValue(j);
      if (rd.m_cf) color = (*(rd.m_cf))(color);
      if (color.m != 0) visible = true;
    }
  }

  bool ret = false;

  if (visible) {
    // Change stroke color to blue if guided drawing
    if (rd.m_showGuidedDrawing && rd.m_highLightNow) {
      TVectorRenderData newRd(rd, rd.m_aff, rd.m_clippingRect, rd.m_palette, rd.m_guidedCf);
      tglDraw(newRd, s, false);
      TStroke *new_s = (TStroke *)s;
      drawFirstControlPoint(rd, new_s);
      ret = rd.m_animatedGuidedDrawing;
    } else {
      tglDraw(rd, s, false);
    }
  }
#ifdef _DEBUG
// drawControlPoints(rd, vim->getStroke(i), sqrt(tglGetPixelSize2()), true);
// assert(checkQuadraticDistance(vim->getStroke(i),true));
#endif
  return ret;
}

//------------------------------------------------------------------------------------

namespace {

void doDraw(const TVectorImage *vim, const TVectorRenderData &_rd,
            bool drawEnteredGroup, TStroke **guidedStroke = 0) {
  static TOnionFader *fade = new TOnionFader(TPixel::White, 0.5);

  TVectorRenderData rd(_rd);

  if (!rd.m_palette) {
    TPalette *vPalette = vim->getPalette();
    rd.m_palette       = vPalette;
    if (!vPalette) return;
  }

  if (!drawEnteredGroup && !rd.m_isIcon && vim->isInsideGroup() > 0)
    rd.m_cf = fade;

  TVectorRenderData rdRegions = rd;

  // Strokes of hidden art layers (Art Layer Manager): the stacking order tells
  // the Underlay and Overlay strokes from the Line Art ones
  int firstColour = -1, lastColour = -1;
  if (rd.m_hiddenRoleMask)
    for (int i = 0; i < (int)vim->getStrokeCount(); i++)
      if (isOThick(vim->getStroke(i))) {
        if (firstColour < 0) firstColour = i;
        lastColour = i;
      }
  // As ArtLayer::colourRange(): without Line Art between the Colour Art
  // strokes, there is no Underlay and Overlay
  bool lineArtBetween = false;
  for (int i = firstColour + 1; i < lastColour && !lineArtBetween; i++)
    lineArtBetween = !isOThick(vim->getStroke(i));
  if (!lineArtBetween) firstColour = lastColour = -1;

  /*if (rd.m_drawRegions && rd.m_isImagePattern)// the image patterns need
antialiasing for lines, but on the areas there would be a lot of asserts
rdRegions.m_alphaChannel = rdRegions.m_antiAliasing = false;*/
  UINT strokeIndex = 0;
  Index            = 0;

  while (strokeIndex < vim->getStrokeCount())  // each while loop draws a group
  {
    int currStrokeIndex = strokeIndex;
    if (!rd.m_isIcon && vim->isInsideGroup() > 0 &&
        ((drawEnteredGroup && !vim->isEnteredGroupStroke(strokeIndex)) ||
         (!drawEnteredGroup && vim->isEnteredGroupStroke(strokeIndex)))) {
      while (strokeIndex < vim->getStrokeCount() &&
             vim->sameGroup(strokeIndex, currStrokeIndex))
        strokeIndex++;
      continue;
    }

    if (rd.m_drawRegions)
      for (UINT regionIndex = 0; regionIndex < vim->getRegionCount();
           regionIndex++)
        if (vim->sameGroupStrokeAndRegion(currStrokeIndex, regionIndex))
          tglDoDraw(rdRegions, vim->getRegion(regionIndex));
    while (strokeIndex < vim->getStrokeCount() &&
           vim->sameGroup(strokeIndex, currStrokeIndex)) {
      if (rd.m_indexToHighlight != strokeIndex) {
        rd.m_highLightNow = false;
      } else {
        rd.m_highLightNow = true;
      }
#if DISEGNO_OUTLINE == 1
      CurrStrokeIndex = strokeIndex;
      CurrVimg        = vim;
#endif
      const TStroke *stroke = vim->getStroke(strokeIndex);
      if (rd.m_hiddenRoleMask) {
        int role = 0;  // Line Art
        if (isOThick(stroke))
          role = 1;  // Colour Art
        else if (firstColour >= 0 && (int)strokeIndex < firstColour)
          role = 2;  // Underlay
        else if (firstColour >= 0 && (int)strokeIndex > lastColour)
          role = 3;  // Overlay
        if (rd.m_hiddenRoleMask & (1 << role)) {
          strokeIndex++;
          continue;
        }
      }
      bool isGuided;
      if ((rd.m_fadeThickStrokes || rd.m_fade0ThickStrokes) &&
          (isOThick(stroke) ? rd.m_fade0ThickStrokes : rd.m_fadeThickStrokes)) {
        TVectorRenderData rdFaded(rd);
        rdFaded.m_cf = fade;
        isGuided     = tglDoDraw(rdFaded, stroke);
      } else
        isGuided = tglDoDraw(rd, stroke);
      if (isGuided && guidedStroke) *guidedStroke = vim->getStroke(strokeIndex);
      strokeIndex++;
    }
  }
}
}  // namespace

//------------------------------------------------------------------------------------

void tglDraw(const TVectorRenderData &rd, const TVectorImage *vim,
             TStroke **guidedStroke) {
  SuspendViewerOpacityErrors strictRender(rd.m_isOfflineRender || rd.m_isIcon);
  assert(vim);
  if (!vim) return;

  QMutexLocker sl(vim->getMutex());

  checkErrorsByGL;
  checkErrorsByGL;

  GLAttributeScope attributes;

  // if(!rd.m_palette) rd.m_palette = vim->getPalette();
  // mylog("tglDraw start; mutex=" + toString((unsigned long)&vim->getMutex()));

  glEnable(GL_ALPHA_TEST);
  glAlphaFunc(GL_GREATER, 0);

  doDraw(vim, rd, false, guidedStroke);
  if (!rd.m_isIcon && vim->isInsideGroup() > 0)
    doDraw(vim, rd, true, guidedStroke);

  glDisable(GL_ALPHA_TEST);


#ifdef _DEBUG
  vim->drawAutocloses(rd);
#endif
  checkErrorsByGL;
  // mylog("tglDraw stop");
}
