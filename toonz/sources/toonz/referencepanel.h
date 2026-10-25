#pragma once

#ifndef REFERENCEPANEL_H
#define REFERENCEPANEL_H

#include <QFrame>
#include <QImage>
#include <QPointF>
#include <QWidget>

class QToolButton;

//=============================================================================
// ReferenceImageView
//-----------------------------------------------------------------------------
/*! Shows a single image with pan (drag), zoom (wheel), mirror and 90 degree
    rotation. View state only; nothing is stored in the scene.           */

class ReferenceImageView final : public QWidget {
  Q_OBJECT

  QImage m_image;
  QPointF m_pan;  // image center offset from the widget center, in pixels
  double m_zoom;
  int m_rotation;  // in quarter turns, 0..3
  bool m_flipH, m_flipV;
  bool m_autoFit;  // refit on resize until the user pans or zooms
  bool m_panning;
  QPoint m_lastPos;

public:
  ReferenceImageView(QWidget *parent = 0);

  void setImage(const QImage &image);
  bool hasImage() const { return !m_image.isNull(); }

  void fitToView();
  void setFlipH(bool on);
  void setFlipV(bool on);
  void rotate(int quarterTurns);

protected:
  void paintEvent(QPaintEvent *) override;
  void resizeEvent(QResizeEvent *) override;
  void mousePressEvent(QMouseEvent *) override;
  void mouseMoveEvent(QMouseEvent *) override;
  void mouseReleaseEvent(QMouseEvent *) override;
  void mouseDoubleClickEvent(QMouseEvent *) override;
  void wheelEvent(QWheelEvent *) override;
};

//=============================================================================
// ReferencePanel
//-----------------------------------------------------------------------------
/*! Dockable panel showing a reference image loaded from file or dropped
    onto it. The last image path is remembered in the user environment. */

class ReferencePanel final : public QFrame {
  Q_OBJECT

  ReferenceImageView *m_view;
  QToolButton *m_flipHButton, *m_flipVButton;

public:
  ReferencePanel(QWidget *parent = 0);

  bool loadImage(const QString &path);

protected:
  void dragEnterEvent(QDragEnterEvent *) override;
  void dropEvent(QDropEvent *) override;

protected slots:
  void onLoadImage();
};

#endif  // REFERENCEPANEL_H
