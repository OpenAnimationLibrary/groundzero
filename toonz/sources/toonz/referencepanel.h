#pragma once

#ifndef REFERENCEPANEL_H
#define REFERENCEPANEL_H

#include <QFrame>
#include <QImage>
#include <QList>
#include <QPair>
#include <QPointF>
#include <QString>
#include <QStringList>
#include <QWidget>

#include <vector>

class QToolButton;
class QComboBox;
class QSlider;

//=============================================================================
// ReferenceViewState
//-----------------------------------------------------------------------------
/*! The view of a reference image: what the panel remembers for each image. */

struct ReferenceViewState {
  double zoom;
  QPointF pan;   // image center offset from the widget center, in pixels
  int rotation;  // in quarter turns, 0..3
  bool flipH, flipV;
  bool autoFit;    // refit on resize until the user pans or zooms
  double opacity;  // 0..1

  ReferenceViewState()
      : zoom(1.0)
      , rotation(0)
      , flipH(false)
      , flipV(false)
      , autoFit(true)
      , opacity(1.0) {}
};

//=============================================================================
// ReferenceImageView
//-----------------------------------------------------------------------------
/*! Shows a single image with pan (drag), zoom (wheel), mirror, 90 degree
    rotation and opacity. View state only; nothing is stored in the scene. */

class ReferenceImageView final : public QWidget {
  Q_OBJECT

  QImage m_image;
  QPointF m_pan;  // image center offset from the widget center, in pixels
  double m_zoom;
  int m_rotation;  // in quarter turns, 0..3
  bool m_flipH, m_flipV;
  bool m_autoFit;  // refit on resize until the user pans or zooms
  bool m_panning;
  double m_opacity;  // 0..1
  QPoint m_lastPos;

public:
  ReferenceImageView(QWidget *parent = 0);

  void setImage(const QImage &image);
  // Shows the image with the given view; a null state fits it to the view.
  void setImage(const QImage &image, const ReferenceViewState *state);
  bool hasImage() const { return !m_image.isNull(); }
  QImage image() const { return m_image; }

  ReferenceViewState getViewState() const;

  void setOpacity(double opacity);
  void fitToView();
  void setFlipH(bool on);
  void setFlipV(bool on);
  void rotate(int quarterTurns);

signals:
  // The user changed the view (pan, zoom, mirror, rotation, opacity, fit).
  void viewStateChanged();

protected:
  void paintEvent(QPaintEvent *) override;
  void resizeEvent(QResizeEvent *) override;
  void mousePressEvent(QMouseEvent *) override;
  void mouseMoveEvent(QMouseEvent *) override;
  void mouseReleaseEvent(QMouseEvent *) override;
  void mouseDoubleClickEvent(QMouseEvent *) override;
  void wheelEvent(QWheelEvent *) override;

private:
  void applyFit();
};

//=============================================================================
// ReferencePanel
//-----------------------------------------------------------------------------
/*! Dockable panel showing reference images loaded from file or dropped onto
    it. Several images can be loaded and are listed in a combo box; the view
    (zoom, pan, mirror, rotation, opacity) is remembered for each of them. A
    PSD file offers its layers as selectable entries, besides the flattened
    image. The list and the views are remembered in the user environment. */

class ReferencePanel final : public QFrame {
  Q_OBJECT

  struct Entry {
    QString path;
    int layerId;        // PSD layer, 0 for the whole (flattened) image
    QString boardItem;  // PureRef export filename; empty means board overview
    ReferenceViewState state;
    bool hasState;
    Entry() : layerId(0), hasState(false) {}
  };

  ReferenceImageView *m_view;
  QToolButton *m_flipHButton, *m_flipVButton;
  QComboBox *m_imageCombo, *m_layerCombo;
  QSlider *m_opacitySlider;
  QWidget *m_boardControls;
  QStringList m_boardItems;

  std::vector<Entry> m_entries;
  int m_current;                        // index in m_entries, -1 if none
  QList<QPair<int, QString>> m_layers;  // layers of the current PSD
  bool m_updating;  // true while the widgets are set from the state

public:
  ReferencePanel(QWidget *parent = 0);

  bool loadImage(const QString &path);

protected:
  void dragEnterEvent(QDragEnterEvent *) override;
  void dropEvent(QDropEvent *) override;

private:
  bool showEntry(int index);
  void storeCurrentState();
  void updateCombos();
  void saveEntries() const;
  void loadEntries();
  bool importPureRefBoard(const QString &path, bool refresh = false);
  QString pureRefExecutable();

protected slots:
  void onLoadImage();
  void onRemoveImage();
  void onImageComboChanged(int index);
  void onLayerComboChanged(int index);
  void onOpacityChanged(int value);
  void onViewStateChanged();
  void onImportPureRefBoard();
  void onRefreshPureRefBoard();
  void onOpenInPureRef();
  void onConfigurePureRef();
};

#endif  // REFERENCEPANEL_H
