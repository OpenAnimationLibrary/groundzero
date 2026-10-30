

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
#include <QWindow>
#include <algorithm>
#include <cmath>
#include <limits>
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
constexpr size_t opacitySnapshotBudget = size_t(512) * 1024 * 1024;
bool opacitySnapshotFits(int width, int height, int stencilWidth, int stencilHeight,
                         size_t budget) {
  if (width < 0 || height < 0 || stencilWidth < 0 || stencilHeight < 0) return false;
  // Both snapshots: cropped FLOAT RGBA/depth, complete UINT stencil. Check
  // each product against the remaining budget before multiplying it.
  constexpr size_t stencilBytes = 2 * sizeof(GLuint);
  if (stencilHeight) {
    if (size_t(stencilWidth) > (budget / stencilBytes) / size_t(stencilHeight)) return false;
    budget -= size_t(stencilWidth) * size_t(stencilHeight) * stencilBytes;
  }
  constexpr size_t colorDepthBytes = 2 * (4 * sizeof(GLfloat) + sizeof(GLuint));
  return !height || size_t(width) <= (budget / colorDepthBytes) / size_t(height);
}
bool opacitySnapshotFits(int width, int height, bool stencil,
                         size_t budget = opacitySnapshotBudget) {
  if (width <= 0 || height <= 0) return false;
  return opacitySnapshotFits(width, height, stencil ? width : 0,
                             stencil ? height : 0, budget);
}
#ifdef BUILD_UI_PORT_TESTS
thread_local int opacityFramebufferFailure = 0;
thread_local bool opacityFullViewportCapture = false;
thread_local int opacityCaptureDepth = 0;
thread_local TOpacityTransferStats opacityTransferStats;
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
bool tglOpacitySnapshotFitsForTest(int width, int height, bool stencil) {
  return opacitySnapshotFits(width, height, stencil);
}
void tglResetOpacityTransferStatsForTest() { opacityTransferStats = {}; }
TOpacityTransferStats tglOpacityTransferStatsForTest() { return opacityTransferStats; }
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
public:
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
    if (!result) result = lookup((QByteArray(name) + "ARB").constData());
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

bool opacityExtension(const char *name) {
  if (auto *context = QOpenGLContext::currentContext())
    return context->hasExtension(name);
  const auto *extensions = glGetString(GL_EXTENSIONS);
  return extensions && QByteArray(reinterpret_cast<const char *>(extensions))
                           .split(' ').contains(name);
}

struct OpacityRectangle { GLint x = 0, y = 0, width = 0, height = 0; };

bool opacityDepthRectangle(const TVectorRenderData &rd, const TStroke &stroke,
                           const GLint *viewport, const GLdouble *model,
                           const GLdouble *projection, OpacityRectangle &rectangle) {
  // Only the exact solid implementation has certified native color/depth
  // bounds here. Procedural styles and programmable geometry retain the full
  // attachment transaction, independently of their 2D capture optimization.
  const auto *style = rd.m_palette->getStyle(stroke.getStyle());
  TRectD box;
  if (!style || style->getTagId() != 3 ||
      !StrokeOpacity::renderBounds(*style, stroke, box)) return false;
#ifdef BUILD_UI_PORT_TESTS
  if (opacityFullViewportCapture) return false;
#endif
  GLint program = 0, polygonMode[2] = {};
  GLfloat lineWidth = 0.f;
  glGetIntegerv(GL_CURRENT_PROGRAM, &program);
  glGetIntegerv(GL_POLYGON_MODE, polygonMode);
  glGetFloatv(GL_LINE_WIDTH, &lineWidth);
  if (program || polygonMode[0] != GL_FILL || polygonMode[1] != GL_FILL ||
      lineWidth > 1.f) return false;
  if ((opacityExtension("GL_ARB_vertex_program") || opacityExtension("GL_NV_vertex_program")) &&
      glIsEnabled(0x8620)) return false; // GL_VERTEX_PROGRAM_ARB/NV.
  if (opacityExtension("GL_ARB_fragment_program") && glIsEnabled(0x8804)) return false;
  if (opacityExtension("GL_NV_fragment_program") && glIsEnabled(0x8870)) return false;
  double left = std::numeric_limits<double>::infinity(), bottom = left;
  double right = -left, top = -left;
  for (const TPointD &point : {box.getP00(), box.getP01(), box.getP10(), box.getP11()}) {
    // Use the original double matrices, not the float inverse used for field
    // lookup: a distant drawing may cancel large world-coordinate offsets.
    GLdouble eye[4], clip[4];
    for (int i = 0; i < 4; ++i)
      eye[i] = model[i] * point.x + model[i+4] * point.y + model[i+12];
    for (int i = 0; i < 4; ++i)
      clip[i] = projection[i]*eye[0] + projection[i+4]*eye[1] +
                projection[i+8]*eye[2] + projection[i+12]*eye[3];
    // A positive W at every corner proves no drawing-plane horizon crosses
    // this convex bound. Uncertain or behind-eye projections use full capture.
    if (!std::isfinite(clip[3]) || clip[3] <= 1.e-10) return false;
    const double x = viewport[0] + (.5 + .5 * clip[0] / clip[3]) * viewport[2];
    const double y = viewport[1] + (.5 + .5 * clip[1] / clip[3]) * viewport[3];
    if (!std::isfinite(x) || !std::isfinite(y)) return false;
    left = std::min(left, x); right = std::max(right, x);
    bottom = std::min(bottom, y); top = std::max(top, y);
  }
  // Native OutlineBuilder expands one screen pixel with a miter factor at
  // most four (sqrt(17) total displacement); six contains that fringe and
  // pixel rasterization. Authored outline miters are already in renderBounds.
  constexpr double pad = 6.;
  const double vx1 = double(viewport[0]) + viewport[2];
  const double vy1 = double(viewport[1]) + viewport[3];
  int x0 = int(std::clamp(std::floor(left) - pad, double(viewport[0]), vx1));
  int y0 = int(std::clamp(std::floor(bottom) - pad, double(viewport[1]), vy1));
  int x1 = int(std::clamp(std::ceil(right) + pad, double(viewport[0]), vx1));
  int y1 = int(std::clamp(std::ceil(top) + pad, double(viewport[1]), vy1));
  if (glIsEnabled(GL_SCISSOR_TEST)) {
    GLint scissor[4]; glGetIntegerv(GL_SCISSOR_BOX, scissor);
    x0 = std::max(x0, scissor[0]); y0 = std::max(y0, scissor[1]);
    x1 = std::min(x1, scissor[0] + scissor[2]); y1 = std::min(y1, scissor[1] + scissor[3]);
  }
  rectangle = {x0, y0, std::max(0, x1-x0), std::max(0, y1-y0)};
  return true;
}

// Read and replace the native target without applying pixel-transfer settings
// a second time. Every scope ends before native style drawing, which must see
// the caller's original state. Program/PBO bindings are not attribute-stack
// state, so preserve those explicitly.
struct OpacityTransferFunctions {
  using BindBuffer = void (QOPENGLF_APIENTRYP)(GLenum, GLuint);
  using UseProgram = void (QOPENGLF_APIENTRYP)(GLuint);
  using ActiveTexture = void (QOPENGLF_APIENTRYP)(GLenum);
  using BindFramebuffer = void (QOPENGLF_APIENTRYP)(GLenum, GLuint);
  using AttachmentParameter = void (QOPENGLF_APIENTRYP)(GLenum, GLenum, GLenum, GLint *);
  using RenderbufferParameter = void (QOPENGLF_APIENTRYP)(GLenum, GLenum, GLint *);
  using ClampColor = void (QOPENGLF_APIENTRYP)(GLenum, GLenum);
  // address() rejects unavailable entry points before this aggregate finishes
  // construction, before any framebuffer binding or target mutation occurs.
  BindBuffer bindBuffer = reinterpret_cast<BindBuffer>(OpacityFramebuffer::address("glBindBuffer"));
  UseProgram useProgram = reinterpret_cast<UseProgram>(OpacityFramebuffer::address("glUseProgram"));
  ActiveTexture activeTexture = reinterpret_cast<ActiveTexture>(OpacityFramebuffer::address("glActiveTexture"));
  BindFramebuffer bindFramebuffer = reinterpret_cast<BindFramebuffer>(OpacityFramebuffer::address("glBindFramebuffer"));
  BindFramebuffer bindRenderbuffer = reinterpret_cast<BindFramebuffer>(OpacityFramebuffer::address("glBindRenderbuffer"));
  AttachmentParameter attachmentParameter = reinterpret_cast<AttachmentParameter>(OpacityFramebuffer::address("glGetFramebufferAttachmentParameteriv"));
  RenderbufferParameter renderbufferParameter = reinterpret_cast<RenderbufferParameter>(OpacityFramebuffer::address("glGetRenderbufferParameteriv"));
  bool imaging = opacityExtension("GL_ARB_imaging");
  bool modernContext = QOpenGLContext::currentContext() && QOpenGLContext::currentContext()->format().majorVersion() >= 3;
  bool arbFramebuffer = opacityExtension("GL_ARB_framebuffer_object");
  bool srgb = modernContext || opacityExtension("GL_EXT_framebuffer_sRGB") || opacityExtension("GL_ARB_framebuffer_sRGB");
  bool separate = modernContext || opacityExtension("GL_EXT_framebuffer_blit") || arbFramebuffer;
  bool textureRectangle = modernContext || opacityExtension("GL_ARB_texture_rectangle") ||
                          opacityExtension("GL_EXT_texture_rectangle") || opacityExtension("GL_NV_texture_rectangle");
  bool colorBufferFloat = modernContext || opacityExtension("GL_ARB_color_buffer_float");
  ClampColor clampColor = colorBufferFloat
      ? reinterpret_cast<ClampColor>(OpacityFramebuffer::address("glClampColor")) : nullptr;
};

class OpacityPixelTransfer {
  const OpacityTransferFunctions &m_functions;
  GLint m_pack = 0, m_unpack = 0, m_program = 0, m_matrixMode = GL_MODELVIEW;
  GLint m_readFramebuffer = 0;
  GLboolean m_colorSum = GL_FALSE;
  bool m_replace = false;
public:
  OpacityPixelTransfer(const OpacityTransferFunctions &functions,
                       const GLint *viewport, bool replace)
      : m_functions(functions), m_replace(replace) {
    if (replace) m_colorSum = glIsEnabled(GL_COLOR_SUM);
    glGetIntegerv(GL_PIXEL_PACK_BUFFER_BINDING, &m_pack);
    glGetIntegerv(GL_PIXEL_UNPACK_BUFFER_BINDING, &m_unpack);
    glGetIntegerv(GL_CURRENT_PROGRAM, &m_program);
    glGetIntegerv(GL_MATRIX_MODE, &m_matrixMode);
    if (m_functions.separate) {
      GLint draw = 0;
      glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &m_readFramebuffer);
      glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &draw);
      m_functions.bindFramebuffer(GL_READ_FRAMEBUFFER, GLuint(draw));
    }
    glPushAttrib(GL_ALL_ATTRIB_BITS);
    glPushClientAttrib(GL_CLIENT_PIXEL_STORE_BIT);
    if (m_functions.clampColor) {
      // Core and ARB use identical enums. macOS compatibility headers expose
      // the fragment-clamp token only under its ARB spelling.
      constexpr GLenum readClamp = 0x891C, fragmentClamp = 0x891B;
      m_functions.clampColor(readClamp, GL_FALSE);
      if (replace) m_functions.clampColor(fragmentClamp, GL_FALSE);
    }
    m_functions.bindBuffer(GL_PIXEL_PACK_BUFFER, 0); m_functions.bindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);
    for (GLenum parameter : {GL_PACK_ALIGNMENT, GL_UNPACK_ALIGNMENT}) glPixelStorei(parameter, 1);
    for (GLenum parameter : {GL_PACK_ROW_LENGTH, GL_PACK_SKIP_ROWS, GL_PACK_SKIP_PIXELS,
         GL_PACK_SWAP_BYTES, GL_PACK_LSB_FIRST, GL_UNPACK_ROW_LENGTH, GL_UNPACK_SKIP_ROWS,
         GL_UNPACK_SKIP_PIXELS, GL_UNPACK_SWAP_BYTES, GL_UNPACK_LSB_FIRST})
      glPixelStorei(parameter, 0);
    for (GLenum parameter : {GL_RED_SCALE, GL_GREEN_SCALE, GL_BLUE_SCALE, GL_ALPHA_SCALE,
                             GL_DEPTH_SCALE}) glPixelTransferf(parameter, 1.f);
    for (GLenum parameter : {GL_RED_BIAS, GL_GREEN_BIAS, GL_BLUE_BIAS, GL_ALPHA_BIAS,
                             GL_DEPTH_BIAS}) glPixelTransferf(parameter, 0.f);
    glPixelTransferi(GL_MAP_COLOR, GL_FALSE); glPixelTransferi(GL_MAP_STENCIL, GL_FALSE);
    glPixelTransferi(GL_INDEX_SHIFT, 0); glPixelTransferi(GL_INDEX_OFFSET, 0);
    if (m_functions.imaging) {
      for (GLenum capability : {GL_COLOR_TABLE, GL_POST_CONVOLUTION_COLOR_TABLE,
           GL_POST_COLOR_MATRIX_COLOR_TABLE, GL_CONVOLUTION_1D, GL_CONVOLUTION_2D,
           GL_SEPARABLE_2D, GL_HISTOGRAM, GL_MINMAX}) glDisable(capability);
      for (GLenum parameter : {GL_POST_CONVOLUTION_RED_SCALE, GL_POST_CONVOLUTION_GREEN_SCALE,
           GL_POST_CONVOLUTION_BLUE_SCALE, GL_POST_CONVOLUTION_ALPHA_SCALE,
           GL_POST_COLOR_MATRIX_RED_SCALE, GL_POST_COLOR_MATRIX_GREEN_SCALE,
           GL_POST_COLOR_MATRIX_BLUE_SCALE, GL_POST_COLOR_MATRIX_ALPHA_SCALE})
        glPixelTransferf(parameter, 1.f);
      for (GLenum parameter : {GL_POST_CONVOLUTION_RED_BIAS, GL_POST_CONVOLUTION_GREEN_BIAS,
           GL_POST_CONVOLUTION_BLUE_BIAS, GL_POST_CONVOLUTION_ALPHA_BIAS,
           GL_POST_COLOR_MATRIX_RED_BIAS, GL_POST_COLOR_MATRIX_GREEN_BIAS,
           GL_POST_COLOR_MATRIX_BLUE_BIAS, GL_POST_COLOR_MATRIX_ALPHA_BIAS})
        glPixelTransferf(parameter, 0.f);
      glMatrixMode(GL_COLOR); glPushMatrix(); glLoadIdentity();
      glMatrixMode(m_matrixMode);
    }
    // Snapshot the actual draw target, then restore its read-buffer state and
    // the caller's independently bound read FBO when this scope ends.
    GLint drawBuffer = 0; glGetIntegerv(GL_DRAW_BUFFER, &drawBuffer);
    glReadBuffer(drawBuffer);
    if (!replace) return;
    m_functions.useProgram(0);
    GLint units = 0; glGetIntegerv(GL_MAX_TEXTURE_UNITS, &units);
    for (int i = 0; i < units; ++i) {
      m_functions.activeTexture(GL_TEXTURE0 + i);
      for (GLenum capability : {GL_TEXTURE_1D, GL_TEXTURE_2D, GL_TEXTURE_3D,
           GL_TEXTURE_CUBE_MAP, GL_TEXTURE_GEN_S, GL_TEXTURE_GEN_T, GL_TEXTURE_GEN_R,
           GL_TEXTURE_GEN_Q}) glDisable(capability);
      if (m_functions.textureRectangle) glDisable(GL_TEXTURE_RECTANGLE);
    }
    for (GLenum capability : {GL_ALPHA_TEST, GL_STENCIL_TEST, GL_DEPTH_TEST, GL_BLEND,
         GL_FOG, GL_LIGHTING, GL_COLOR_SUM, GL_COLOR_LOGIC_OP, GL_DITHER, GL_SCISSOR_TEST})
      glDisable(capability);
    if (m_functions.srgb)
      glDisable(GL_FRAMEBUFFER_SRGB);
    GLint planes = 0; glGetIntegerv(GL_MAX_CLIP_PLANES, &planes);
    for (int i = 0; i < planes; ++i) glDisable(GL_CLIP_PLANE0 + i);
    glPixelZoom(1.f, 1.f);
    glViewport(viewport[0], viewport[1], viewport[2], viewport[3]);
    glMatrixMode(GL_PROJECTION); glPushMatrix(); glLoadIdentity();
    glOrtho(viewport[0], viewport[0] + viewport[2], viewport[1], viewport[1] + viewport[3], -1., 1.);
    glMatrixMode(GL_MODELVIEW); glPushMatrix(); glLoadIdentity();
  }
  ~OpacityPixelTransfer() {
    if (m_replace) {
      glMatrixMode(GL_MODELVIEW); glPopMatrix();
      glMatrixMode(GL_PROJECTION); glPopMatrix();
    }
    if (m_functions.imaging) { glMatrixMode(GL_COLOR); glPopMatrix(); }
    glMatrixMode(m_matrixMode);
    glPopClientAttrib(); glPopAttrib();
    // Some compatibility drivers omit COLOR_SUM from PopAttrib's enable/fog
    // restoration (Mesa 23.2.1 attrib.c). Restore it explicitly, including the
    // preflight scope, so the following native draw sees the original state.
    if (m_replace) {
      if (m_colorSum) glEnable(GL_COLOR_SUM);
      else glDisable(GL_COLOR_SUM);
    }
    if (m_functions.separate)
      m_functions.bindFramebuffer(GL_READ_FRAMEBUFFER, GLuint(m_readFramebuffer));
    m_functions.bindBuffer(GL_PIXEL_PACK_BUFFER, m_pack); m_functions.bindBuffer(GL_PIXEL_UNPACK_BUFFER, m_unpack);
    m_functions.useProgram(m_program);
  }
};

TDimension opacityTargetSize(const OpacityTransferFunctions &functions, GLenum &depthType) {
  GLint framebuffer = 0; glGetIntegerv(GL_FRAMEBUFFER_BINDING, &framebuffer);
  if (framebuffer) {
    GLint type = 0, object = 0;
    functions.attachmentParameter(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_FRAMEBUFFER_ATTACHMENT_OBJECT_TYPE, &type);
    functions.attachmentParameter(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_FRAMEBUFFER_ATTACHMENT_OBJECT_NAME, &object);
    if (type == GL_RENDERBUFFER && object) {
      GLint previous = 0, width = 0, height = 0, format = 0;
      glGetIntegerv(GL_RENDERBUFFER_BINDING, &previous);
      functions.bindRenderbuffer(GL_RENDERBUFFER, GLuint(object));
      functions.renderbufferParameter(GL_RENDERBUFFER, GL_RENDERBUFFER_WIDTH, &width);
      functions.renderbufferParameter(GL_RENDERBUFFER, GL_RENDERBUFFER_HEIGHT, &height);
      functions.renderbufferParameter(GL_RENDERBUFFER, GL_RENDERBUFFER_INTERNAL_FORMAT, &format);
      functions.bindRenderbuffer(GL_RENDERBUFFER, GLuint(previous));
      if (format == GL_DEPTH_COMPONENT32F || format == GL_DEPTH32F_STENCIL8)
        depthType = GL_FLOAT;
      if (width > 0 && height > 0) return TDimension(width, height);
    } else if (type == GL_TEXTURE && object) {
      GLint level = 0, face = 0, previous = 0, width = 0, height = 0, format = 0;
      functions.attachmentParameter(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_FRAMEBUFFER_ATTACHMENT_TEXTURE_LEVEL, &level);
      functions.attachmentParameter(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_FRAMEBUFFER_ATTACHMENT_TEXTURE_CUBE_MAP_FACE, &face);
      const GLenum target = face ? GL_TEXTURE_CUBE_MAP : GL_TEXTURE_2D;
      const GLenum binding = face ? GL_TEXTURE_BINDING_CUBE_MAP : GL_TEXTURE_BINDING_2D;
      glGetIntegerv(binding, &previous);
      if (glGetError() != GL_NO_ERROR)
        throw std::runtime_error("Cannot inspect the native 3D depth attachment");
      glBindTexture(target, GLuint(object));
      const GLenum bindError = glGetError();
      if (bindError == GL_NO_ERROR) {
        const GLenum image = face ? GLenum(face) : GL_TEXTURE_2D;
        glGetTexLevelParameteriv(image, level, GL_TEXTURE_WIDTH, &width);
        glGetTexLevelParameteriv(image, level, GL_TEXTURE_HEIGHT, &height);
        glGetTexLevelParameteriv(image, level, GL_TEXTURE_INTERNAL_FORMAT, &format);
      }
      glBindTexture(target, GLuint(previous));
      if (bindError != GL_NO_ERROR)
        throw std::runtime_error("Stroke opacity in a 3D view requires a renderbuffer, 2D texture, or cube-face depth attachment");
      if (format == GL_DEPTH_COMPONENT32F || format == GL_DEPTH32F_STENCIL8)
        depthType = GL_FLOAT;
      if (width > 0 && height > 0) return TDimension(width, height);
    }
  } else if (auto *context = QOpenGLContext::currentContext()) {
    if (functions.modernContext || functions.arbFramebuffer) {
      GLint component = 0;
      functions.attachmentParameter(GL_FRAMEBUFFER, GL_DEPTH, GL_FRAMEBUFFER_ATTACHMENT_COMPONENT_TYPE, &component);
      if (component == GL_FLOAT) depthType = GL_FLOAT;
    }
    auto *surface = context->surface();
    if (surface && surface->surfaceClass() == QSurface::Window) {
      const auto *window = static_cast<const QWindow *>(surface);
      const QSize size = window->size() * window->devicePixelRatio();
      return TDimension(size.width(), size.height());
    }
  }
  // Native stencil masks clear the attachment outside the viewport as well.
  // Guessing its dimensions would make rollback silently incomplete.
  throw std::runtime_error("Cannot determine the native 3D opacity framebuffer dimensions");
}

struct OpacityTargetPixels {
  static_assert(sizeof(GLfloat) == sizeof(GLuint), "GL attachment storage must preserve 32-bit depth values");
  std::vector<GLfloat> color;
  // Raw 32-bit FLOAT storage in the integer vector. Native float depth and
  // fixed depth up to 24 bits are transferred without a UINT32 texture stage.
  std::vector<GLuint> depth, stencil;
  OpacityTargetPixels(size_t count, size_t stencilCount)
      : color(count * 4), depth(count), stencil(stencilCount) {}
};

template <typename OpacityAt>
void drawDepthOpacity(const TVectorRenderData &rd, const TStroke &stroke,
                      const GLint *viewport, const OpacityRectangle *bounds,
                      const OpacityAt &opacityAt) {
  GLint sampleBuffers = 0, stencilBits = 0;
  glGetIntegerv(GL_SAMPLE_BUFFERS, &sampleBuffers);
  glGetIntegerv(GL_STENCIL_BITS, &stencilBits);
#ifdef BUILD_UI_PORT_TESTS
  if (opacityFramebufferFailure == 5) sampleBuffers = 1;
#endif
  if (sampleBuffers > 0)
    throw std::runtime_error("Stroke opacity in a 3D view requires a single-sample framebuffer; disable multisampling");
  const OpacityTransferFunctions functions;
  GLint originalRead = 0, originalDraw = 0;
  glGetIntegerv(GL_FRAMEBUFFER_BINDING, &originalDraw);
  if (functions.separate) glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &originalRead);
  struct RestoreFramebuffers {
    const OpacityTransferFunctions &functions;
    GLint read, draw;
    ~RestoreFramebuffers() {
      if (functions.separate) {
        functions.bindFramebuffer(GL_DRAW_FRAMEBUFFER, GLuint(draw));
        functions.bindFramebuffer(GL_READ_FRAMEBUFFER, GLuint(read));
      } else functions.bindFramebuffer(GL_FRAMEBUFFER, GLuint(draw));
    }
  } restoreFramebuffers{functions, originalRead, originalDraw};
  GLenum depthType = GL_UNSIGNED_INT;
  const TDimension target = opacityTargetSize(functions, depthType);
  GLint depthBits = 0; glGetIntegerv(GL_DEPTH_BITS, &depthBits);
  if (depthBits > 24 && depthType != GL_FLOAT)
    throw std::runtime_error("Stroke opacity in a 3D view requires fixed depth of at most 24 bits or known floating depth; fixed or unclassified 32-bit depth cannot be restored exactly");
  // UINT32 DrawPixels can pass through a normalized 32-bit temporary texture
  // whose float sampler loses a depth quantum. FLOAT preserves D24 and native
  // float-depth transfers without that intermediate integer normalization.
  depthType = GL_FLOAT;
  bool srgbColor = false;
  if (functions.srgb && glIsEnabled(GL_FRAMEBUFFER_SRGB)) {
    GLint framebuffer = 0, attachment = 0, encoding = 0;
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &framebuffer);
    glGetIntegerv(GL_DRAW_BUFFER, &attachment);
    if (!framebuffer) {
      if (attachment == GL_BACK) attachment = GL_BACK_LEFT;
      if (attachment == GL_FRONT) attachment = GL_FRONT_LEFT;
    }
    if (functions.modernContext || functions.arbFramebuffer) {
      functions.attachmentParameter(GL_FRAMEBUFFER, GLenum(attachment), GL_FRAMEBUFFER_ATTACHMENT_COLOR_ENCODING, &encoding);
      srgbColor = encoding == GL_SRGB;
    } else {
      // EXT_framebuffer_sRGB predates the per-attachment encoding query. Its
      // capability value follows the currently bound drawable/FBO instead.
      GLboolean capable = GL_FALSE;
      glGetBooleanv(GL_FRAMEBUFFER_SRGB_CAPABLE_EXT, &capable);
      srgbColor = capable;
    }
  }
  if (target.lx <= 0 || target.ly <= 0) return;
  const GLint targetViewport[4] = {0, 0, target.lx, target.ly};
  const int x0 = bounds ? std::clamp(bounds->x, 0, target.lx) : 0;
  const int y0 = bounds ? std::clamp(bounds->y, 0, target.ly) : 0;
  const int x1 = bounds ? std::clamp(bounds->x + bounds->width, x0, target.lx) : target.lx;
  const int y1 = bounds ? std::clamp(bounds->y + bounds->height, y0, target.ly) : target.ly;
  const int width = x1-x0, height = y1-y0;
  size_t snapshotBudget = opacitySnapshotBudget;
#ifdef BUILD_UI_PORT_TESTS
  if (opacityFramebufferFailure == 6) snapshotBudget = 1;
#endif
  // Check the aggregate before multiplication and allocation, including both
  // snapshots. Keep a bounded failure even for a very large valid GL target.
  if (!opacitySnapshotFits(width, height, stencilBits > 0 ? target.lx : 0,
                           stencilBits > 0 ? target.ly : 0, snapshotBudget))
    throw std::runtime_error("Stroke opacity in a 3D view exceeds the 512 MiB snapshot limit; reduce the viewer framebuffer size");
  const size_t count = size_t(width) * size_t(height);
  const size_t stencilCount = stencilBits > 0 ? size_t(target.lx) * size_t(target.ly) : 0;
  // Allocate everything before touching the target. The opacity field and
  // transform have already been built; their per-pixel lookup allocates nothing.
  // Float color preserves 10-bit displays. Native-compatible depth transfers
  // and integer stencil copies retain zero-opacity holes without a UINT32
  // normalized depth texture between readback and replacement.
  OpacityTargetPixels before(count, stencilCount), after(count, stencilCount);
#ifdef BUILD_UI_PORT_TESTS
  ++opacityTransferStats.transactions;
  if (bounds) ++opacityTransferStats.croppedTransactions;
  opacityTransferStats.peakSnapshotBytes = std::max(opacityTransferStats.peakSnapshotBytes,
      static_cast<unsigned long long>(2 * (count * (4*sizeof(GLfloat)+sizeof(GLuint)) +
                                          stencilCount*sizeof(GLuint))));
#endif
  TStroke unmodulated(stroke);
  unmodulated.setOpacityProfile({}); unmodulated.setOpacity(1.);
  auto read = [&](OpacityTargetPixels &pixels) {
    OpacityPixelTransfer transfer(functions, viewport, false);
    if (count) {
      glReadPixels(x0, y0, width, height, GL_RGBA, GL_FLOAT, pixels.color.data());
      glReadPixels(x0, y0, width, height, GL_DEPTH_COMPONENT, depthType, pixels.depth.data());
    }
    if (!pixels.stencil.empty())
      glReadPixels(0, 0, target.lx, target.ly, GL_STENCIL_INDEX, GL_UNSIGNED_INT, pixels.stencil.data());
#ifdef BUILD_UI_PORT_TESTS
    opacityTransferStats.colorPixels += count;
    opacityTransferStats.depthPixels += count;
    opacityTransferStats.stencilPixels += stencilCount;
#endif
    const GLenum error = glGetError();
    if (error != GL_NO_ERROR)
      throw std::runtime_error("Cannot read the native 3D opacity target (OpenGL " + std::to_string(error) + ")");
  };
  auto replace = [&](const OpacityTargetPixels &pixels) {
    OpacityPixelTransfer transfer(functions, targetViewport, true);
    if (count) {
      glRasterPos3d(x0, y0, 0.);
      glDepthMask(GL_FALSE); glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
      glDrawPixels(width, height, GL_RGBA, GL_FLOAT, pixels.color.data());
      glColorMask(GL_FALSE, GL_FALSE, GL_FALSE, GL_FALSE);
      glEnable(GL_DEPTH_TEST); glDepthFunc(GL_ALWAYS); glDepthMask(GL_TRUE);
      glDrawPixels(width, height, GL_DEPTH_COMPONENT, depthType, pixels.depth.data());
    }
    glColorMask(GL_FALSE, GL_FALSE, GL_FALSE, GL_FALSE);
    if (!pixels.stencil.empty()) {
      glRasterPos3d(0, 0, 0.);
      glStencilMask(~GLuint(0));
      glDrawPixels(target.lx, target.ly, GL_STENCIL_INDEX, GL_UNSIGNED_INT, pixels.stencil.data());
    }
#ifdef BUILD_UI_PORT_TESTS
    opacityTransferStats.colorPixels += count;
    opacityTransferStats.depthPixels += count;
    opacityTransferStats.stencilPixels += stencilCount;
#endif
    const GLenum error = glGetError();
    if (error != GL_NO_ERROR)
      throw std::runtime_error("Cannot replace the native 3D opacity target (OpenGL " + std::to_string(error) + ")");
  };
  read(before);
  // Preflight the replacement entry points/state before the first native draw.
  { OpacityPixelTransfer transfer(functions, targetViewport, true); }
  const GLenum error = glGetError();
  if (error != GL_NO_ERROR)
    throw std::runtime_error("Cannot prepare native 3D opacity compositing (OpenGL " + std::to_string(error) + ")");
  try {
    // The original context performs all actual depth/stencil decisions,
    // including antialiased line self-occlusion and native mask operations.
    SuspendViewerOpacityErrors strict;
    tglDraw(rd, &unmodulated, true);
#ifdef BUILD_UI_PORT_TESTS
    if (opacityFramebufferFailure == 4)
      throw std::runtime_error("Injected native 3D opacity composite failure");
#endif
    read(after);
    auto linear = [](double value) {
      return value <= .04045 ? value / 12.92 : std::pow((value + .055) / 1.055, 2.4);
    };
    auto encoded = [](double value) {
      return value <= .0031308 ? value * 12.92 : 1.055 * std::pow(value, 1. / 2.4) - .055;
    };
    for (size_t i = 0; i < count; ++i) {
      // Unchanged color/depth needs no field lookup. Stencil is handled over
      // the complete attachment below, including native clears outside bounds.
      if (before.depth[i] == after.depth[i] &&
          std::equal(before.color.begin() + i * 4, before.color.begin() + i * 4 + 4,
                     after.color.begin() + i * 4)) continue;
      const double amount = opacityAt(double(i % width) + x0 + .5,
                                      double(i / width) + y0 + .5);
      for (int channel = 0; channel < 4; ++channel) {
        const size_t index = i * 4 + channel;
        if (amount == 1. || before.color[index] == after.color[index]) continue;
        if (amount == 0.) after.color[index] = before.color[index];
        else if (srgbColor && channel < 3) {
          const double start = linear(before.color[index]);
          after.color[index] = GLfloat(encoded(start + amount * (linear(after.color[index]) - start)));
        } else after.color[index] = GLfloat(before.color[index] +
            amount * (double(after.color[index]) - before.color[index]));
      }
      // Do not infer coverage from color differences: same-colored artwork
      // still writes depth. Any positive opacity retains native attachments.
      if (amount == 0.) {
        after.depth[i] = before.depth[i];
      }
    }
    // Native mask allocation can clear anywhere in the target, even outside
    // the viewport. Do not change its scissor or draw behavior to fit the ROI.
    // Exact-zero opacity restores those stencil effects everywhere as before.
    for (size_t i = 0; i < stencilCount; ++i)
      if (before.stencil[i] != after.stencil[i] &&
          opacityAt(double(i % target.lx) + .5, double(i / target.lx) + .5) == 0.)
        after.stencil[i] = before.stencil[i];
    replace(after);
  } catch (...) {
    replace(before);
    throw;
  }
}

// Composite the native style once, then modulate its complete premultiplied
// result. This path applies to solid, procedural, raster and vector patterns
// equally; splitting the geometry into translucent sub-strokes would alter
// pattern spacing and darken overlapping joins.
void drawOpacityEnvelope(const TVectorRenderData &rd, const TStroke &stroke) {
  GLint viewport[4];
  glGetIntegerv(GL_VIEWPORT, viewport);
  if (viewport[2] <= 0 || viewport[3] <= 0) return;
  GLint depthBits = 0;
  if (glIsEnabled(GL_DEPTH_TEST)) glGetIntegerv(GL_DEPTH_BITS, &depthBits);
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
  double planeInverse[9] = {};
  if (!invertible) {
    // PlaneViewer deliberately flattens Z, so its 4D matrix is singular even
    // though the stroke's z=0 plane has a valid screen mapping. Invert that
    // plane's XY/W homography without changing the viewer's native geometry.
    const double a = mapping(0,0), b = mapping(0,1), c = mapping(0,3);
    const double d = mapping(1,0), e = mapping(1,1), f = mapping(1,3);
    const double g = mapping(3,0), h = mapping(3,1), i = mapping(3,3);
    const double cofactors[] = {e*i-f*h,c*h-b*i,b*f-c*e,
        f*g-d*i,a*i-c*g,c*d-a*f,d*h-e*g,b*g-a*h,a*e-b*d};
    const double determinant = a*cofactors[0]+b*cofactors[3]+c*cofactors[6];
    if (!std::isfinite(determinant) || determinant == 0.)
      throw OpacityRenderError("Cannot display stroke opacity with a degenerate drawing-plane projection; restore the viewer orientation");
    for (int j = 0; j < 9; ++j) {
      planeInverse[j] = cofactors[j]/determinant;
      if (!std::isfinite(planeInverse[j]))
        throw OpacityRenderError("Cannot display stroke opacity with an unbounded drawing-plane projection; adjust the viewer orientation");
    }
  }
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
  if (!depthBits && (x1 <= x0 || y1 <= y0)) return;
  const int width = x1 - x0, height = y1 - y0;
  const double projectedScale = std::max(
      std::hypot(double(mapping(0,0)) * viewport[2], double(mapping(1,0)) * viewport[3]),
      std::hypot(double(mapping(0,1)) * viewport[2], double(mapping(1,1)) * viewport[3])) * .5;
  std::unique_ptr<StrokeOpacity::Field> field;
  if (!stroke.getOpacityProfile().empty())
    field.reset(new StrokeOpacity::Field(stroke, projectedScale > 0. ? .125 / projectedScale : .05));
  auto planePoint = [&](double x, double y) {
    const float nx = float(2. * (x - viewport[0]) / viewport[2] - 1.);
    const float ny = float(2. * (y - viewport[1]) / viewport[3] - 1.);
    if (!invertible) {
      const double w = planeInverse[6]*nx+planeInverse[7]*ny+planeInverse[8];
      if (w == 0. || !std::isfinite(w))
        throw OpacityRenderError("Cannot display stroke opacity at the drawing-plane horizon; adjust the viewer orientation");
      const double px = (planeInverse[0]*nx+planeInverse[1]*ny+planeInverse[2])/w;
      const double py = (planeInverse[3]*nx+planeInverse[4]*ny+planeInverse[5])/w;
      if (!std::isfinite(px) || !std::isfinite(py))
        throw OpacityRenderError("Cannot display stroke opacity at the drawing-plane horizon; adjust the viewer orientation");
      return TPointD(px,py);
    }
    QVector4D nearPoint = inverse * QVector4D(nx, ny, -1, 1);
    QVector4D farPoint = inverse * QVector4D(nx, ny, 1, 1);
    if (nearPoint.w() == 0 || farPoint.w() == 0) return TPointD();
    nearPoint /= nearPoint.w(); farPoint /= farPoint.w();
    const double dz = farPoint.z() - nearPoint.z();
    const double z = std::abs(dz) > 1e-12 ? -nearPoint.z() / dz : 0.;
    return TPointD(nearPoint.x() + z * (farPoint.x() - nearPoint.x()),
                   nearPoint.y() + z * (farPoint.y() - nearPoint.y()));
  };
  if (depthBits > 0) {
    OpacityRectangle rectangle;
    const bool certified = opacityDepthRectangle(rd, stroke, viewport, model, projection, rectangle);
    drawDepthOpacity(rd, stroke, viewport, certified ? &rectangle : nullptr,
        [&](double x, double y) {
          return stroke.getOpacity() * (field ? field->opacity(planePoint(x, y)) : 1.);
        });
    return;
  }
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
  // Targets with depth attachments already use drawDepthOpacity above. Here
  // the capture contains the native clipping result and only needs a 2D quad;
  // inverse-projecting its corners would fail at a projective horizon even
  // when all captured artwork is valid and visible.
  auto vertex = [&](double x, double y, double u, double v) {
    glTexCoord2d(u, v); glVertex2d(x, y);
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
