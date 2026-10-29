#include "cadmiumreviewview.h"

#include <QApplication>
#include <QComboBox>
#include <QEvent>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QMouseEvent>
#include <QPainter>
#include <QSlider>
#include <QToolButton>
#include <QVBoxLayout>
#include <QWheelEvent>

#include <algorithm>
#include <cmath>
#include <limits>

namespace {
constexpr double minimumScale = 0.001;
constexpr double maximumScale = 64.0;
double unit(double value) { return std::max(0.0, std::min(1.0, value)); }
}

CadmiumReviewView::CadmiumReviewView(QWidget *parent) : QWidget(parent) {
  auto *layout = new QVBoxLayout(this);
  layout->setContentsMargins(0, 0, 0, 0);
  auto *tools = new QHBoxLayout;
  m_modes = new QComboBox(this);
  m_modes->setObjectName(QStringLiteral("cadmiumReviewMode"));
  m_modes->addItems({tr("Before / After"), tr("Wipe"), tr("Reference overlay"),
                     tr("Before"), tr("After")});
  tools->addWidget(m_modes);
  const auto button = [&](const QString &text, const QString &description) {
    auto *control = new QToolButton(this);
    control->setText(text);
    control->setToolTip(description);
    control->setAccessibleName(description);
    tools->addWidget(control);
    return control;
  };
  auto *fit = button(tr("Fit"), tr("Fit artwork to the review canvas"));
  auto *actual = button(tr("100%"), tr("Show one image pixel per canvas pixel"));
  auto *out = button(QStringLiteral("−"), tr("Zoom out"));
  auto *in = button(QStringLiteral("+"), tr("Zoom in"));
  m_zoomLabel = new QLabel(this);
  m_zoomLabel->setObjectName(QStringLiteral("cadmiumReviewZoom"));
  m_zoomLabel->setMinimumWidth(45);
  tools->addWidget(m_zoomLabel);
  tools->addStretch();
  m_overlay = new QSlider(Qt::Horizontal, this);
  m_overlay->setObjectName(QStringLiteral("cadmiumReviewOpacity"));
  m_overlay->setRange(0, 100);
  m_overlay->setValue(50);
  m_overlay->setMaximumWidth(100);
  m_overlay->setToolTip(tr("Reference overlay opacity"));
  m_overlay->setAccessibleName(tr("Reference overlay opacity"));
  tools->addWidget(m_overlay);
  layout->addLayout(tools);

  m_canvas = new QWidget(this);
  m_canvas->setObjectName(QStringLiteral("cadmiumReviewCanvas"));
  m_canvas->setMinimumSize(180, 160);
  m_canvas->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
  m_canvas->setFocusPolicy(Qt::StrongFocus);
  m_canvas->setMouseTracking(true);
  m_canvas->setAccessibleName(tr("Painting review canvas"));
  m_canvas->setToolTip(tr("Click a region to select it. Right-click for decisions. "
                          "Drag to pan; use the wheel to zoom. Drag the divider in Wipe mode."));
  m_canvas->installEventFilter(this);
  layout->addWidget(m_canvas, 1);

  connect(m_modes, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
          [this] { updateControls(); m_canvas->update(); });
  connect(m_overlay, &QSlider::valueChanged, this,
          [this](int value) { m_opacity = value / 100.0; m_canvas->update(); });
  connect(fit, &QToolButton::clicked, this, &CadmiumReviewView::fitToWindow);
  connect(actual, &QToolButton::clicked, this, [this] { setZoom(1.0); });
  connect(out, &QToolButton::clicked, this, [this] { setZoom(zoom() / 1.25); });
  connect(in, &QToolButton::clicked, this, [this] { setZoom(zoom() * 1.25); });
  updateControls();
}

void CadmiumReviewView::setImages(const QImage &before, const QImage &after,
                                const QImage &reference) {
  const QSize previous = m_before.isNull() ? m_after.size() : m_before.size();
  m_before = before;
  m_after = after;
  m_reference = reference;
  m_message.clear();
  const QSize current = m_before.isNull() ? m_after.size() : m_before.size();
  if (previous != current) { m_fit = true; m_pan = QPointF(); }
  updateControls();
  m_canvas->update();
}

void CadmiumReviewView::setReferenceImage(const QImage &reference) {
  // Reference selection does not alter the proposal, region decisions, or view.
  // Retain the already rendered before/after images instead of rebuilding them.
  m_reference = reference;
  updateControls();
  m_canvas->update();
}

void CadmiumReviewView::setRegions(const QSize &size, const QVector<int> &labels) {
  if (size == m_regionSize && labels.size() == m_labels.size() &&
      labels.constData() == m_labels.constData()) return;
  const qint64 count = qint64(size.width()) * qint64(size.height());
  if (size.width() <= 0 || size.height() <= 0 || count > std::numeric_limits<int>::max() ||
      count != labels.size() ||
      std::any_of(labels.cbegin(), labels.cend(), [](int label) { return label < 0; })) {
    m_regionSize = QSize();
    m_labels.clear();
  } else {
    m_regionSize = size;
    m_labels = labels;
  }
  m_selected = -1;
  m_protected.clear();
  updateControls();
  m_canvas->update();
}

void CadmiumReviewView::setSelectedRegion(int region) {
  m_selected = region > 0 ? region : -1;
  m_canvas->update();
}

void CadmiumReviewView::setProtectedRegions(const QSet<int> &regions) {
  m_protected = regions;
  m_protected.remove(0);
  m_canvas->update();
}

void CadmiumReviewView::clear(const QString &message) {
  m_before = QImage(); m_after = QImage(); m_reference = QImage();
  m_labels.clear(); m_regionSize = QSize(); m_protected.clear();
  m_selected = -1;
  m_message = message;
  m_fit = true; m_pan = QPointF();
  m_pressed = m_panning = m_wiping = m_space = false;
  updateControls();
  m_canvas->update();
}

void CadmiumReviewView::setMode(Mode selected) {
  const int index = int(selected);
  if (index >= 0 && index < m_modes->count()) m_modes->setCurrentIndex(index);
}

CadmiumReviewView::Mode CadmiumReviewView::mode() const {
  return Mode(m_modes->currentIndex());
}

QSize CadmiumReviewView::imageSize() const {
  if (!m_regionSize.isEmpty()) return m_regionSize;
  if (!m_before.isNull()) return m_before.size();
  if (!m_after.isNull()) return m_after.size();
  return m_reference.size();
}

QRect CadmiumReviewView::canvasRect() const { return m_canvas->geometry(); }

QRectF CadmiumReviewView::paneRect(int pane) const {
  QRectF result(m_canvas->rect());
  if (mode() == Mode::SideBySide) {
    const double middle = result.width() / 2.0;
    if (pane == 0) result.setRight(middle - 4.0);
    else result.setLeft(middle + 4.0);
  }
  return result.adjusted(8.0, 27.0, -8.0, -8.0);
}

int CadmiumReviewView::paneAt(const QPointF &point) const {
  if (paneRect(0).contains(point)) return 0;
  if (mode() == Mode::SideBySide && paneRect(1).contains(point)) return 1;
  return -1;
}

double CadmiumReviewView::fitScale() const {
  const QSize size = imageSize();
  if (size.isEmpty()) return 1.0;
  const QRectF first = paneRect(0);
  double available = std::min(first.width() / size.width(), first.height() / size.height());
  if (mode() == Mode::SideBySide) {
    const QRectF second = paneRect(1);
    available = std::min(available, std::min(second.width() / size.width(),
                                           second.height() / size.height()));
  }
  return std::max(minimumScale, std::min(maximumScale, available));
}

double CadmiumReviewView::zoom() const { return m_fit ? fitScale() : m_scale; }

QPointF CadmiumReviewView::imageOrigin(int pane) const {
  const QSize size = imageSize();
  return paneRect(pane).center() + m_pan -
         QPointF(size.width() * zoom() / 2.0, size.height() * zoom() / 2.0);
}

QPointF CadmiumReviewView::imagePosition(const QPointF &position, int pane) const {
  return (position - imageOrigin(pane)) / zoom();
}

int CadmiumReviewView::regionAt(const QPoint &position) const {
  if (m_labels.isEmpty()) return -1;
  const QPointF canvas = m_canvas->mapFrom(this, position);
  const int pane = paneAt(canvas);
  if (pane < 0) return -1;
  const QPointF image = imagePosition(canvas, pane);
  const int x = int(std::floor(image.x())), y = int(std::floor(image.y()));
  if (x < 0 || y < 0 || x >= m_regionSize.width() || y >= m_regionSize.height()) return -1;
  return m_labels[y * m_regionSize.width() + x];
}

QPoint CadmiumReviewView::viewportPointForImage(const QPointF &position, int pane) const {
  if (mode() != Mode::SideBySide) pane = 0;
  pane = pane == 1 ? 1 : 0;
  const QPointF point = imageOrigin(pane) + position * zoom();
  return m_canvas->mapTo(this, point.toPoint());
}

void CadmiumReviewView::changeZoom(double scale, const QPointF &anchor) {
  if (!std::isfinite(scale)) return;
  int pane = paneAt(anchor);
  if (pane < 0) pane = 0;
  const QPointF image = imagePosition(anchor, pane);
  m_scale = std::max(minimumScale, std::min(maximumScale, scale));
  m_fit = false;
  const QSize size = imageSize();
  m_pan = anchor - paneRect(pane).center() -
          QPointF((image.x() - size.width() / 2.0) * m_scale,
                  (image.y() - size.height() / 2.0) * m_scale);
  updateControls(); m_canvas->update();
  emit zoomChanged(zoom());
}

void CadmiumReviewView::setZoom(double scale) {
  changeZoom(scale, paneRect(0).center());
}

void CadmiumReviewView::fitToWindow() {
  m_fit = true; m_pan = QPointF();
  updateControls(); m_canvas->update();
  emit zoomChanged(zoom());
}

void CadmiumReviewView::setPan(const QPointF &offset) {
  if (!std::isfinite(offset.x()) || !std::isfinite(offset.y())) return;
  m_pan = offset;
  m_canvas->update();
}

void CadmiumReviewView::setWipePosition(double fraction) {
  if (!std::isfinite(fraction)) return;
  m_wipe = unit(fraction);
  m_canvas->update();
}

void CadmiumReviewView::setOverlayOpacity(double fraction) {
  if (!std::isfinite(fraction)) return;
  m_opacity = unit(fraction);
  m_overlay->setValue(qRound(m_opacity * 100.0));
  m_canvas->update();
}

void CadmiumReviewView::updateControls() {
  m_overlay->setEnabled(mode() == Mode::ReferenceOverlay && !m_reference.isNull());
  m_zoomLabel->setText(tr("%1%").arg(zoom() * 100.0, 0, 'f', zoom() < 0.1 ? 1 : 0));
}

bool CadmiumReviewView::nearWipe(const QPointF &position) const {
  const QRectF pane = paneRect(0);
  return mode() == Mode::Wipe && pane.contains(position) &&
         std::abs(position.x() - (pane.left() + m_wipe * pane.width())) <= 8.0;
}

void CadmiumReviewView::paintCanvas() {
  QPainter painter(m_canvas);
  painter.fillRect(m_canvas->rect(), palette().brush(QPalette::Dark));
  const QSize size = imageSize();
  if (size.isEmpty() || (m_before.isNull() && m_after.isNull())) {
    painter.setPen(palette().color(QPalette::Text));
    painter.drawText(m_canvas->rect().adjusted(12, 12, -12, -12),
                     Qt::AlignCenter | Qt::TextWordWrap,
                     m_message.isEmpty() ? tr("Select a drawing to inspect its paint proposal.") : m_message);
    return;
  }
  const auto drawImage = [&](const QImage &image, int pane, double opacity) {
    if (image.isNull()) return;
    painter.save();
    painter.setClipRect(paneRect(pane), Qt::IntersectClip);
    painter.setOpacity(opacity);
    painter.translate(imageOrigin(pane));
    painter.scale(zoom(), zoom());
    painter.setRenderHint(QPainter::SmoothPixmapTransform, zoom() < 1.0);
    painter.drawImage(QRectF(0.0, 0.0, size.width(), size.height()), image);
    painter.restore();
  };
  const auto background = [&](int pane) {
    const QRectF clip = paneRect(pane);
    painter.save();
    painter.setClipRect(clip);
    painter.fillRect(clip, QColor(126, 126, 126));
    for (int y = int(clip.top()); y < int(clip.bottom()) + 1; y += 16)
      for (int x = int(clip.left()); x < int(clip.right()) + 1; x += 16)
        if (((x - int(clip.left())) / 16 + (y - int(clip.top())) / 16) % 2 == 0)
          painter.fillRect(QRect(x, y, 16, 16), QColor(151, 151, 151));
    painter.restore();
  };
  const auto title = [&](int pane, const QString &text) {
    const QRectF rect = paneRect(pane);
    painter.setPen(palette().color(QPalette::Text));
    painter.drawText(QRectF(rect.left(), 2.0, rect.width(), 23.0), Qt::AlignCenter, text);
  };
  const int panes = mode() == Mode::SideBySide ? 2 : 1;
  for (int pane = 0; pane < panes; ++pane) background(pane);
  switch (mode()) {
  case Mode::SideBySide:
    drawImage(m_before, 0, 1.0); drawImage(m_after, 1, 1.0);
    title(0, tr("Before")); title(1, tr("After"));
    break;
  case Mode::Wipe: {
    const QRectF rect = paneRect(0);
    const double divider = rect.left() + m_wipe * rect.width();
    drawImage(m_before, 0, 1.0);
    painter.save();
    painter.setClipRect(QRectF(divider, rect.top(), rect.right() - divider, rect.height()));
    drawImage(m_after, 0, 1.0);
    painter.restore();
    painter.setPen(QPen(QColor(245, 245, 245), 2.0));
    painter.drawLine(QPointF(divider, rect.top()), QPointF(divider, rect.bottom()));
    painter.setBrush(QColor(245, 245, 245));
    painter.drawEllipse(QPointF(divider, rect.top() + 10.0), 5.0, 5.0);
    title(0, tr("Before / After — drag the divider"));
    break;
  }
  case Mode::ReferenceOverlay:
    drawImage(m_after, 0, 1.0); drawImage(m_reference, 0, m_opacity);
    title(0, m_reference.isNull() ? tr("After — no reference preview available") : tr("Reference overlay"));
    break;
  case Mode::Before:
    drawImage(m_before, 0, 1.0); title(0, tr("Before")); break;
  case Mode::After:
    drawImage(m_after, 0, 1.0); title(0, tr("After")); break;
  }
  if (!m_labels.isEmpty() && (m_selected > 0 || !m_protected.isEmpty())) {
    // Build only a viewport-sized highlight. Zooming into large drawings must
    // not allocate a second full-resolution region image.
    for (int pane = 0; pane < panes; ++pane) {
      const QRect visible = paneRect(pane).toAlignedRect().intersected(m_canvas->rect());
      if (visible.isEmpty()) continue;
      QImage highlight(visible.size(), QImage::Format_ARGB32_Premultiplied);
      if (highlight.isNull()) continue;
      highlight.fill(Qt::transparent);
      QVector<int> columns(visible.width(), -1);
      for (int x = 0; x < columns.size(); ++x) {
        const int imageX = int(std::floor(imagePosition(
            QPointF(visible.left() + x + 0.5, visible.top()), pane).x()));
        if (imageX >= 0 && imageX < m_regionSize.width()) columns[x] = imageX;
      }
      for (int y = 0; y < highlight.height(); ++y) {
        const int imageY = int(std::floor(imagePosition(
            QPointF(visible.left(), visible.top() + y + 0.5), pane).y()));
        if (imageY < 0 || imageY >= m_regionSize.height()) continue;
        QRgb *row = reinterpret_cast<QRgb *>(highlight.scanLine(y));
        for (int x = 0; x < columns.size(); ++x) {
          const int imageX = columns[x];
          if (imageX < 0) continue;
          const int index = imageY * m_regionSize.width() + imageX;
          const int region = m_labels[index];
          if (region <= 0 || (region != m_selected && !m_protected.contains(region))) continue;
          const bool boundary = imageX == 0 || imageY == 0 ||
              imageX + 1 == m_regionSize.width() || imageY + 1 == m_regionSize.height() ||
              m_labels[index - 1] != region || m_labels[index + 1] != region ||
              m_labels[index - m_regionSize.width()] != region ||
              m_labels[index + m_regionSize.width()] != region;
          const QColor color = region == m_selected
              ? QColor(255, 221, 40, boundary ? 220 : 65)
              : QColor(50, 205, 255, boundary ? 195 : 45);
          row[x] = qPremultiply(color.rgba());
        }
      }
      painter.save();
      painter.setClipRect(paneRect(pane));
      painter.drawImage(visible.topLeft(), highlight);
      painter.restore();
    }
  }
}

bool CadmiumReviewView::eventFilter(QObject *object, QEvent *event) {
  if (object != m_canvas) return QWidget::eventFilter(object, event);
  switch (event->type()) {
  case QEvent::Paint:
    paintCanvas(); return true;
  case QEvent::Resize:
    updateControls(); m_canvas->update(); break;
  case QEvent::Wheel: {
    auto *wheel = static_cast<QWheelEvent *>(event);
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
    const QPointF position = wheel->position();
#else
    const QPointF position = wheel->posF();
#endif
    const int delta = wheel->angleDelta().y() != 0
        ? wheel->angleDelta().y() : wheel->pixelDelta().y();
    if (!imageSize().isEmpty() && delta != 0)
      changeZoom(zoom() * std::pow(1.25, delta / 120.0), position);
    wheel->accept(); return true;
  }
  case QEvent::MouseButtonPress: {
    auto *mouse = static_cast<QMouseEvent *>(event);
    m_canvas->setFocus(Qt::MouseFocusReason);
    if (mouse->button() == Qt::RightButton) {
      const int region = regionAt(m_canvas->mapTo(this, mouse->pos()));
      if (region >= 0) emit regionContextMenuRequested(region, mouse->globalPos());
      return true;
    }
    if (mouse->button() == Qt::LeftButton || mouse->button() == Qt::MiddleButton) {
      m_pressed = true; m_press = m_last = mouse->pos();
      m_wiping = mouse->button() == Qt::LeftButton && nearWipe(m_press) && !m_space;
      m_panning = mouse->button() == Qt::MiddleButton || m_space;
      m_canvas->setCursor(m_wiping ? Qt::SplitHCursor : m_panning ? Qt::ClosedHandCursor : Qt::ArrowCursor);
      return true;
    }
    break;
  }
  case QEvent::MouseMove: {
    auto *mouse = static_cast<QMouseEvent *>(event);
    const QPointF position = mouse->pos();
    if (m_pressed) {
      if (m_wiping) {
        const QRectF rect = paneRect(0);
        if (rect.width() > 0.0) setWipePosition((position.x() - rect.left()) / rect.width());
      } else {
        if (!m_panning && (position - m_press).manhattanLength() >= QApplication::startDragDistance())
          m_panning = true;
        if (m_panning) {
          m_pan += position - m_last;
          m_canvas->setCursor(Qt::ClosedHandCursor);
          m_canvas->update();
        }
      }
      m_last = position;
      return true;
    }
    m_canvas->setCursor(nearWipe(position) ? Qt::SplitHCursor : Qt::ArrowCursor);
    break;
  }
  case QEvent::MouseButtonRelease: {
    auto *mouse = static_cast<QMouseEvent *>(event);
    if (m_pressed && (mouse->button() == Qt::LeftButton || mouse->button() == Qt::MiddleButton)) {
      if (!m_panning && !m_wiping && mouse->button() == Qt::LeftButton) {
        const int region = regionAt(m_canvas->mapTo(this, mouse->pos()));
        if (region >= 0) emit regionActivated(region);
      }
      m_pressed = m_panning = m_wiping = false;
      m_canvas->setCursor(nearWipe(mouse->pos()) ? Qt::SplitHCursor : Qt::ArrowCursor);
      return true;
    }
    break;
  }
  case QEvent::KeyPress: {
    auto *key = static_cast<QKeyEvent *>(event);
    if (key->key() == Qt::Key_Space) { m_space = true; key->accept(); return true; }
    if (key->key() == Qt::Key_Plus || key->key() == Qt::Key_Equal) { setZoom(zoom() * 1.25); return true; }
    if (key->key() == Qt::Key_Minus) { setZoom(zoom() / 1.25); return true; }
    if (key->key() == Qt::Key_F) { fitToWindow(); return true; }
    if (key->key() == Qt::Key_0) { setZoom(1.0); return true; }
    break;
  }
  case QEvent::KeyRelease:
    if (static_cast<QKeyEvent *>(event)->key() == Qt::Key_Space) { m_space = false; return true; }
    break;
  case QEvent::FocusOut:
    m_pressed = m_panning = m_wiping = m_space = false;
    m_canvas->unsetCursor();
    break;
  default:
    break;
  }
  return QWidget::eventFilter(object, event);
}
