#pragma once

#include <QImage>
#include <QPointF>
#include <QRectF>
#include <QSet>
#include <QVector>
#include <QWidget>

class QComboBox;
class QLabel;
class QSlider;
class QToolButton;

class CadmiumReviewView final : public QWidget {
  Q_OBJECT

public:
  enum class Mode { SideBySide, Wipe, ReferenceOverlay, Before, After };

  explicit CadmiumReviewView(QWidget *parent = nullptr);
  void setImages(const QImage &before, const QImage &after,
                 const QImage &reference = QImage());
  void setRegions(const QSize &size, const QVector<int> &labels);
  void setSelectedRegion(int region);
  void setProtectedRegions(const QSet<int> &regions);
  void clear(const QString &message = QString());

  void setMode(Mode mode);
  Mode mode() const;
  void setZoom(double scale);
  double zoom() const;
  void fitToWindow();
  void setPan(const QPointF &offset);
  QPointF pan() const { return m_pan; }
  void setWipePosition(double fraction);
  double wipePosition() const { return m_wipe; }
  void setOverlayOpacity(double fraction);
  double overlayOpacity() const { return m_opacity; }

  // Positions use this widget's coordinates, including its toolbar offset.
  int regionAt(const QPoint &position) const;
  QPoint viewportPointForImage(const QPointF &position, int pane = 0) const;
  QRect canvasRect() const;

signals:
  void regionActivated(int region);
  void regionContextMenuRequested(int region, const QPoint &globalPosition);
  void zoomChanged(double scale);

protected:
  bool eventFilter(QObject *object, QEvent *event) override;
  void changeEvent(QEvent *event) override;

private:
  QSize imageSize() const;
  QRectF paneRect(int pane) const;
  int paneAt(const QPointF &position) const;
  double fitScale() const;
  QPointF imageOrigin(int pane) const;
  QPointF imagePosition(const QPointF &position, int pane) const;
  void changeZoom(double scale, const QPointF &anchor);
  void updateControls();
  void retranslateUi();
  void paintCanvas();
  bool nearWipe(const QPointF &position) const;

  QWidget *m_canvas;
  QComboBox *m_modes;
  QSlider *m_overlay;
  QLabel *m_zoomLabel;
  QToolButton *m_fitButton, *m_actualButton, *m_zoomOutButton, *m_zoomInButton;
  QImage m_before, m_after, m_reference;
  QSize m_regionSize;
  QVector<int> m_labels;
  QSet<int> m_protected;
  QString m_message;
  QPointF m_pan, m_press, m_last;
  double m_scale = 1.0;
  double m_wipe = 0.5;
  double m_opacity = 0.5;
  int m_selected = -1;
  bool m_fit = true;
  bool m_pressed = false;
  bool m_panning = false;
  bool m_wiping = false;
  bool m_space = false;
};
