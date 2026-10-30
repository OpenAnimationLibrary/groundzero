

#include "texception.h"
#include "qtofflinegl.h"
#include <traster.h>
#include <tconvert.h>
#include <QCoreApplication>
#include <QGuiApplication>
#include <QThread>
#include <QOpenGLFunctions>
#include <QDebug>

namespace {
void diagnoseOffscreenContext(const char *phase, QOpenGLContext *context,
                              QOpenGLFramebufferObject *framebuffer = nullptr) {
  if (!qEnvironmentVariableIsSet("GROUNDZERO_OFFSCREEN_GL_DIAGNOSTICS")) return;
  const bool current = context && QOpenGLContext::currentContext() == context;
  const auto nativeVersion = glGetString(GL_VERSION);
  const auto nativeRenderer = glGetString(GL_RENDERER);
  const auto qtVersion =
      current ? context->functions()->glGetString(GL_VERSION) : nullptr;
  const auto qtRenderer =
      current ? context->functions()->glGetString(GL_RENDERER) : nullptr;
  const auto string = [](const GLubyte *value) {
    return value ? QString::fromLatin1(reinterpret_cast<const char *>(value))
                 : QString("unavailable");
  };
  qWarning().noquote()
      << "offscreen-gl:" << phase << "current=" << current << "gles="
      << (context && context->isOpenGLES()) << "native-version="
      << string(nativeVersion) << "native-renderer=" << string(nativeRenderer)
      << "qt-version=" << string(qtVersion) << "qt-renderer=" << string(qtRenderer)
      << "fbo-valid=" << (framebuffer && framebuffer->isValid()) << "fbo-size="
      << (framebuffer ? framebuffer->size() : QSize());
}
}  // namespace

//-----------------------------------------------------------------------------

#if 0  // was _WIN32, this function not used

static void swapRedBlueChannels(
    void *buffer,
    int bufferSize)  // Flips The Red And Blue Bytes (WidthxHeight)
{
  void *b = buffer;  // Pointer To The Buffer

#ifdef x64
  int size   = bufferSize;
  UCHAR *pix = (UCHAR *)b;
  while (size > 0) {
    UCHAR r    = *pix;
    UCHAR b    = *(pix + 2);
    *pix       = b;
    *(pix + 2) = r;
    pix += 4;
    size--;
  }
#else
  __asm        // Assembler Code To Follow
  {
		mov ecx, bufferSize  // Counter Set To Dimensions Of Our Memory Block
			mov ebx, b  // Points ebx To Our Data (b)
			label:  // Label Used For Looping
			mov al,[ebx+0]  // Loads Value At ebx Into al
			mov ah,[ebx+2]  // Loads Value At ebx+2 Into ah
			mov [ebx+2],al  // Stores Value In al At ebx+2
			mov [ebx+0],ah  // Stores Value In ah At ebx

			add ebx,4  // Moves Through The Data By 4 Bytes
			dec ecx  // Decreases Our Loop Counter
			jnz label  // If Not Zero Jump Back To Label
  }
#endif
}

#endif

//-----------------------------------------------------------------------------

#if defined(MACOSX)
#if defined(powerpc)

void rightRotateBits(UCHAR *buf, int bufferSize) {
  UINT *buffer = (UINT *)buf;
  UINT app;
  for (int i = 0; i < bufferSize; i++, buffer++) {
    app     = *buffer;
    *buffer = app >> 8 | app << 24;
  }
}
#else
void rightRotateBits(UCHAR *buf, int bufferSize) {
  UINT *buffer = (UINT *)buf;
  UINT app;
  for (int i = 0; i < bufferSize; i++, buffer++) {
    app     = *buffer;
    *buffer = (app >> 16 & 0x000000ff) | (app << 16 & 0x00ff0000) |
              (app & 0xff00ff00);
  }
}
#endif
#endif

//-----------------------------------------------------------------------------

//=============================================================================
// QtOfflineGL : implem. offlineGL usando QT
//-----------------------------------------------------------------------------

//-----------------------------------------------------------------------------

QtOfflineGL::QtOfflineGL(TDimension rasterSize,
                         std::shared_ptr<TOfflineGL::Imp> shared)
    : TOfflineGL::Imp(rasterSize.lx, rasterSize.ly)
    , m_context(0)
    , m_oldContext(0) {
  createContext(rasterSize, std::move(shared));
  /*
makeCurrent();

glClearColor(0.0f,0.0f,0.0f,0.0f);
glClear(GL_COLOR_BUFFER_BIT);

doneCurrent();
*/
}

//-----------------------------------------------------------------------------

QtOfflineGL::~QtOfflineGL() {
  if (!m_context) return;
  // Framebuffer resources must be released in their owning context; leave the
  // caller's context current after destruction, including nested exports.
  try {
    makeCurrent();
    m_fbo.reset();
    doneCurrent();
  } catch (...) {
    // Qt defers framebuffer cleanup to the share group if activation failed.
    m_fbo.reset();
    restoreCurrentContext();
  }
  m_context.reset();
}

//-----------------------------------------------------------------------------

void QtOfflineGL::createContext(TDimension rasterSize,
                                std::shared_ptr<TOfflineGL::Imp> shared) {
  // Imposto il formato dei Pixel (pixelFormat)
  /*
   32,                    // 32-bit color depth
   0, 0, 0, 0, 0, 0,      // color bits ignored
   8,                     // no alpha buffer
   0,                     // shift bit ignored
   0,                     // no accumulation buffer
   0, 0, 0, 0,            // accum bits ignored
   32,                    // 32-bit z-buffer
   32,                    // max stencil buffer
   0,                     // no auxiliary buffer
   PFD_MAIN_PLANE,        // main layer
   0,                     // reserved
   0, 0, 0                // layer masks ignored

   ATTENZIONE !! SU MAC IL FORMATO E' DIVERSO (casomai possiamo mettere un
   ifdef)

   SPECIFICHE  MAC = depth_size 24, stencil_size 8, alpha_size 1

*/

  // A QGLFormat used to be set up here, but it was never used: the context
  // only gets the QSurfaceFormat below.
  auto *application = qobject_cast<QGuiApplication *>(QCoreApplication::instance());
  if (!application)
    throw TException("Offscreen OpenGL rendering requires a QGuiApplication");
  saveCurrentContext();
  QSurfaceFormat format;
  format.setRenderableType(QSurfaceFormat::OpenGL);
  format.setProfile(QSurfaceFormat::CompatibilityProfile);
  format.setVersion(2, 1);
  format.setDepthBufferSize(24);
  format.setStencilBufferSize(8);

  // Some platforms implement offscreen surfaces with native windows. Create
  // and destroy that surface on the GUI thread, while the render context and
  // all GL work remain on the calling render thread.
  auto createSurface = [&] {
    m_surface = std::shared_ptr<QOffscreenSurface>(new QOffscreenSurface,
        [](QOffscreenSurface *surface) {
          if (surface->thread() == QThread::currentThread()) delete surface;
          else surface->deleteLater();
        });
    m_surface->setFormat(format);
    m_surface->create();
  };
  if (application->thread() != QThread::currentThread()) {
    if (!QMetaObject::invokeMethod(application, createSurface, Qt::BlockingQueuedConnection))
      throw TException("Cannot create the offscreen surface on the GUI thread");
  } else createSurface();

  m_context = std::make_shared<QOpenGLContext>();
  m_context->setFormat(format);
  if (auto source = std::dynamic_pointer_cast<QtOfflineGL>(shared))
    if (source->m_context) m_context->setShareContext(source->m_context.get());
  // Without OpenGL (the offscreen platform on a headless machine) the
  // context is not created, and the framebuffer below crashed on the missing
  // current context. Fail here instead, where callers can report it.
  if (!m_surface->isValid() || !m_context->create() ||
      !m_context->makeCurrent(m_surface.get())) {
    diagnoseOffscreenContext("context-creation-failed", m_context.get());
    m_context.reset();
    m_surface.reset();
    restoreCurrentContext();
    throw TException("OpenGL is not available: no offscreen context");
  }

  QOpenGLFramebufferObjectFormat fbo_format;
  fbo_format.setAttachment(QOpenGLFramebufferObject::CombinedDepthStencil);
  m_fbo = std::make_shared<QOpenGLFramebufferObject>(rasterSize.lx,
                                                     rasterSize.ly, fbo_format);
  if (!m_fbo->isValid() || !m_fbo->bind()) {
    diagnoseOffscreenContext("framebuffer-creation-failed", m_context.get(),
                             m_fbo.get());
    m_fbo.reset();
    m_context->doneCurrent();
    m_context.reset();
    restoreCurrentContext();
    throw TException("OpenGL is not available: no framebuffer support");
  }
  diagnoseOffscreenContext("created", m_context.get(), m_fbo.get());

  // Creo il contesto OpenGL - assicurandomi che sia effettivamente creato
  // NOTA: Se il contesto non viene creato, di solito basta ritentare qualche
  // volta.
}
//-----------------------------------------------------------------------------

void QtOfflineGL::makeCurrent() {
  if (!m_context) return;
  saveCurrentContext();
  if (!m_context->thread()) m_context->moveToThread(QThread::currentThread());
  if (m_context->thread() != QThread::currentThread() ||
      !m_context->makeCurrent(m_surface.get()))
    throw TException("Cannot activate the offscreen OpenGL context on this thread");
  if (m_fbo && !m_fbo->bind()) throw TException("Cannot bind offscreen framebuffer");
}

void QtOfflineGL::doneCurrent() {
  if (!m_context) return;
  if (QOpenGLContext::currentContext() == m_context.get()) m_context->doneCurrent();
  if (m_context->thread() == QThread::currentThread()) m_context->moveToThread(nullptr);
  restoreCurrentContext();
}

void QtOfflineGL::saveCurrentContext() {
  auto *current = QOpenGLContext::currentContext();
  if (current == m_context.get()) return;
  m_oldContext = current;
  m_oldSurface = current ? current->surface() : nullptr;
  m_oldSurfaceObject = dynamic_cast<QObject *>(m_oldSurface);
}

void QtOfflineGL::restoreCurrentContext() {
  if (m_oldContext && m_oldSurface && m_oldSurfaceObject)
    m_oldContext->makeCurrent(m_oldSurface);
}

//-----------------------------------------------------------------------------

void QtOfflineGL::getRaster(TRaster32P raster) {
  makeCurrent();
  glFlush();

  int lx = raster->getLx();
  int ly = raster->getLy();
  QImage pixels = m_fbo->toImage(false);
  if (qEnvironmentVariableIsSet("GROUNDZERO_OFFSCREEN_GL_DIAGNOSTICS")) {
    diagnoseOffscreenContext("readback", m_context.get(), m_fbo.get());
    int visible = 0, dark = 0;
    for (int y = 0; y < pixels.height(); ++y)
      for (int x = 0; x < pixels.width(); ++x) {
        const QRgb value = pixels.pixel(x, y);
        if (qAlpha(value)) ++visible;
        if (qAlpha(value) && qRed(value) < 200 && qGreen(value) < 200 &&
            qBlue(value) < 200)
          ++dark;
      }
    qWarning() << "offscreen-gl: pixels" << pixels.size()
               << "format=" << int(pixels.format())
               << "requested=" << QSize(lx, ly) << "visible=" << visible
               << "dark=" << dark;
  }

  raster->lock();
  raster->copy(TRaster32P(lx, ly, m_fbo->width(),
                          (TPixelRGBM32 *)pixels.bits(), false));
  raster->unlock();
}

// QGLPixelBuffer::hasOpenGLPbuffers() (statica) -> true se la scheda supporta i
// PBuffer

//=============================================================================
// QtOfflineGLPBuffer : implem. offlineGL usando QT e PBuffer
//-----------------------------------------------------------------------------

#if QT_VERSION < QT_VERSION_CHECK(6, 0, 0)  // see qtofflinegl.h

QtOfflineGLPBuffer::QtOfflineGLPBuffer(TDimension rasterSize)
    : TOfflineGL::Imp(rasterSize.lx, rasterSize.ly), m_context(0) {
  createContext(rasterSize);

  makeCurrent();

  glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
  glClear(GL_COLOR_BUFFER_BIT);

  doneCurrent();
}

//-----------------------------------------------------------------------------

QtOfflineGLPBuffer::~QtOfflineGLPBuffer() {}

//-----------------------------------------------------------------------------

void QtOfflineGLPBuffer::createContext(TDimension rasterSize) {
  // Imposto il formato dei Pixel (pixelFormat)
  /*
32,                    // 32-bit color depth
0, 0, 0, 0, 0, 0,      // color bits ignored
8,                     // no alpha buffer
0,                     // shift bit ignored
0,                     // no accumulation buffer
0, 0, 0, 0,            // accum bits ignored
32,                    // 32-bit z-buffer
32,                    // max stencil buffer
0,                     // no auxiliary buffer
PFD_MAIN_PLANE,        // main layer
0,                     // reserved
0, 0, 0                // layer masks ignored

ATTENZIONE !! SU MAC IL FORMATO E' DIVERSO (casomai possiamo mettere un ifdef)

SPECIFICHE  MAC = depth_size 24, stencil_size 8, alpha_size 1

*/

  QGLFormat fmt;

#if defined(_WIN32)
  fmt.setAlphaBufferSize(8);
  fmt.setAlpha(false);
  fmt.setRgba(true);
  fmt.setDepthBufferSize(32);
  fmt.setDepth(true);
  fmt.setStencilBufferSize(32);
  fmt.setStencil(true);
  fmt.setAccum(false);
  fmt.setPlane(0);
#elif defined(MACOSX)
  fmt.setAlphaBufferSize(1);
  fmt.setAlpha(false);
  fmt.setRgba(true);
  fmt.setDepthBufferSize(24);
  fmt.setDepth(true);
  fmt.setStencilBufferSize(8);
  fmt.setStencil(true);
  fmt.setAccum(false);
  fmt.setPlane(0);
#elif defined(LINUX) || defined(FREEBSD)
  fmt.setAlphaBufferSize(1);
  fmt.setAlpha(false);
  fmt.setRgba(true);
  fmt.setDepthBufferSize(24);
  fmt.setDepth(true);
  fmt.setStencilBufferSize(8);
  fmt.setStencil(true);
  fmt.setAccum(false);
  fmt.setPlane(0);
#endif

  // Il PixelBuffer deve essere con width ed height potenze di 2

  int sizeMax = std::max(rasterSize.lx, rasterSize.ly);

  // trovo la potenza di 2 che "contiene" sizeMax e la utilizzo per il PBuffer
  int pBufferSize = 2;
  while (pBufferSize < sizeMax) pBufferSize *= 2;

  m_context =
      std::make_shared<QGLPixelBuffer>(QSize(pBufferSize, pBufferSize), fmt);
}

//-----------------------------------------------------------------------------

void QtOfflineGLPBuffer::makeCurrent() {
  if (m_context) {
    m_context->makeCurrent();
  }
}

//-----------------------------------------------------------------------------

void QtOfflineGLPBuffer::doneCurrent() {
  if (m_context) m_context->doneCurrent();
}

//-----------------------------------------------------------------------------

void QtOfflineGLPBuffer::getRaster(TRaster32P raster) {
  makeCurrent();
  glFlush();

  // The image is stored using a 32-bit ARGB format (0xAARRGGBB).
  QImage image = m_context->toImage();

  int lx = raster->getLx(), ly = raster->getLy();

  static const TRaster32P emptyRaster;
  if (image.height() == 0 || image.width() == 0) return;

  // devo iniziare a leggere la Y da un certo offset
  // dato dalla differenza tra la y della image e la y del raster
  int yOffset = image.height() - ly;
  raster->lock();

  for (int y = 0; y < ly; y++) {
    QRgb *inpPix = (QRgb *)image.scanLine(yOffset + y);

    TPixel32 *pix    = raster->pixels(ly - 1 - y);
    TPixel32 *endPix = pix + lx;

    for (; pix < endPix; ++pix) {
      pix->m = 255;
      pix->r = qRed(*inpPix);
      pix->g = qGreen(*inpPix);
      pix->b = qBlue(*inpPix);
      inpPix++;
    }
  }
  raster->unlock();
}

#endif  // QT_VERSION < 6
