#pragma once

#ifndef QTOFFLINEGL_H
#define QTOFFLINEGL_H

#include <memory>

#include <QtOpenGL>
#if QT_VERSION < QT_VERSION_CHECK(6, 0, 0)
#include <QGLFormat>
#include <QGLContext>
#include <QGLPixelBuffer>
#endif
#include <QOpenGLFramebufferObject>
#include <QPointer>

#include "tofflinegl.h"

class QtOfflineGL final : public TOfflineGL::Imp {
public:
  std::shared_ptr<QOpenGLContext> m_context;
  QPointer<QOpenGLContext> m_oldContext;
  QSurface *m_oldSurface = nullptr;
  QPointer<QObject> m_oldSurfaceObject;
  std::shared_ptr<QOffscreenSurface> m_surface;
  std::shared_ptr<QOpenGLFramebufferObject> m_fbo;

  QtOfflineGL(TDimension rasterSize, std::shared_ptr<TOfflineGL::Imp> shared);
  ~QtOfflineGL();

  void createContext(TDimension rasterSize,
                     std::shared_ptr<TOfflineGL::Imp> shared) override;
  void makeCurrent() override;
  void doneCurrent() override;

  void saveCurrentContext();
  void restoreCurrentContext();

  void getRaster(TRaster32P raster) override;
};

//-----------------------------------------------------------------------------

// Unused. Qt 6 has no QGLPixelBuffer, and no replacement is needed: QtOfflineGL
// already renders to a QOpenGLFramebufferObject.
#if QT_VERSION < QT_VERSION_CHECK(6, 0, 0)
class QtOfflineGLPBuffer final : public TOfflineGL::Imp {
public:
  std::shared_ptr<QGLPixelBuffer> m_context;

  QtOfflineGLPBuffer(TDimension rasterSize);
  ~QtOfflineGLPBuffer();

  void createContext(TDimension rasterSize);
  void makeCurrent() override;
  void doneCurrent() override;

  void getRaster(TRaster32P raster) override;
};
#endif

#endif
