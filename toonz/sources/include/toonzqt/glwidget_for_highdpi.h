#pragma once

#ifndef GLWIDGET_FOR_HIGHDPI_H
#define GLWIDGET_FOR_HIGHDPI_H

#include <QOpenGLWidget>
#include <QApplication>
#include <QCoreApplication>
#include <QOpenGLFunctions>
#include "toonzqt/gutil.h"

#undef DVAPI
#ifdef TOONZQT_EXPORTS
#define DVAPI DV_EXPORT_API
#else
#define DVAPI DV_IMPORT_API
#endif

class QLabel;

class DVAPI GLWidgetForHighDpi : public QOpenGLWidget, protected QOpenGLFunctions {
  Q_DECLARE_TR_FUNCTIONS(GLWidgetForHighDpi)
  QLabel *m_opacityErrorOverlay = nullptr;
public:
  GLWidgetForHighDpi(QWidget *parent   = nullptr,
                     Qt::WindowFlags f = Qt::WindowFlags())
      : QOpenGLWidget(parent, f) {}

  //  modify sizes for high DPI monitors
  int width() const { return QOpenGLWidget::width() * getDevPixRatio(); }
  int height() const { return QOpenGLWidget::height() * getDevPixRatio(); }
  QRect rect() const { return QRect(0, 0, width(), height()); }
  int getDevPixRatio() const { return getDevicePixelRatio(this); }
  // Finish Qt's framebuffer operation before reporting a failed viewer
  // capture. A partially rendered drawing must not become a saved image.
  QImage grabFramebuffer();
protected:
  void paintEvent(QPaintEvent *event) override;
};

#endif
