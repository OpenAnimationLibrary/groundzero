#include "toonzqt/glwidget_for_highdpi.h"
#include "tvectorgl.h"

#include <QLabel>
#include <QImage>
#include <QPaintEvent>
#include <algorithm>
#include <stdexcept>

QImage GLWidgetForHighDpi::grabFramebuffer() {
  const bool priorFailure = m_opacityErrorOverlay && !m_opacityErrorOverlay->isHidden();
  if (TViewerOpacityErrorScope::currentFailed() ||
      (priorFailure && (!updatesEnabled() || !isValid() ||
          QOpenGLWidget::width() <= 0 || QOpenGLWidget::height() <= 0)))
    throw std::runtime_error("Cannot capture an incomplete opacity preview");
  TViewerOpacityErrorScope errors;
  const QImage image = QOpenGLWidget::grabFramebuffer();
  if (!errors.error().empty()) throw std::runtime_error(errors.error());
  if (image.isNull() && priorFailure)
    throw std::runtime_error("Cannot capture an incomplete opacity preview");
  return image;
}

void GLWidgetForHighDpi::paintEvent(QPaintEvent *event) {
  TViewerOpacityErrorScope errors;
  const bool canPaint = updatesEnabled() &&
      QOpenGLWidget::width() > 0 && QOpenGLWidget::height() > 0;
  QOpenGLWidget::paintEvent(event);
  // Qt may reuse the old framebuffer when updates are disabled or its surface
  // is unavailable. That does not establish recovery from the previous error.
  if (errors.error().empty()) {
    if (canPaint && isValid() && m_opacityErrorOverlay) m_opacityErrorOverlay->hide();
    return;
  }

  // A child widget is composited above every GL layer and tool overlay. It is
  // created only after a failure and never captures mouse/keyboard input.
  if (!m_opacityErrorOverlay) {
    m_opacityErrorOverlay = new QLabel(this);
    m_opacityErrorOverlay->setObjectName("vectorOpacityErrorOverlay");
    m_opacityErrorOverlay->setAttribute(Qt::WA_TransparentForMouseEvents);
    m_opacityErrorOverlay->setFocusPolicy(Qt::NoFocus);
    m_opacityErrorOverlay->setTextFormat(Qt::PlainText);
    m_opacityErrorOverlay->setWordWrap(true);
    m_opacityErrorOverlay->setMargin(8);
    m_opacityErrorOverlay->setStyleSheet(
        "QLabel { color: white; background: #602c24; border: 1px solid #eeaa77; }");
    m_opacityErrorOverlay->setAccessibleName(tr("Vector preview error"));
  }
  const QString message = tr("Preview incomplete\n%1")
                              .arg(QString::fromStdString(errors.error()));
  if (m_opacityErrorOverlay->text() != message) m_opacityErrorOverlay->setText(message);
  m_opacityErrorOverlay->setAccessibleDescription(message);
  m_opacityErrorOverlay->setToolTip(message);
  const int availableWidth = std::max(1, QOpenGLWidget::width() - 16);
  const int availableHeight = std::max(1, QOpenGLWidget::height() - 16);
  const int bannerHeight = std::min(availableHeight,
      std::max(1, m_opacityErrorOverlay->heightForWidth(availableWidth)));
  m_opacityErrorOverlay->setGeometry(8, 8, availableWidth, bannerHeight);
  m_opacityErrorOverlay->show();
  m_opacityErrorOverlay->raise();
}
