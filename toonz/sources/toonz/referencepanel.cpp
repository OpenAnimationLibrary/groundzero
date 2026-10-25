#include "referencepanel.h"

// TnzQt includes
#include "toonzqt/gutil.h"

// TnzCore includes
#include "tenv.h"
#include "tmsgcore.h"
#include "tfilepath.h"
#include "timage_io.h"
#include "traster.h"
#include "trop.h"

// Qt includes
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QFileDialog>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QImageReader>
#include <QMimeData>
#include <QMouseEvent>
#include <QPainter>
#include <QResizeEvent>
#include <QToolButton>
#include <QUrl>
#include <QVBoxLayout>
#include <QWheelEvent>

#include <algorithm>
#include <cmath>

TEnv::FilePathVar ReferencePanelLastImage("ReferencePanelLastImage",
                                          TFilePath());

namespace {

const double minZoom = 0.01, maxZoom = 64.0;

//-----------------------------------------------------------------------------

QImage readImageFile(const QString &path) {
  QImageReader reader(path);
  reader.setAutoTransform(true);
  QImage image = reader.read();
  if (!image.isNull()) return image;

  // Fall back to the OpenToonz image readers (tga, psd, sgi, ...)
  try {
    TRasterP ras;
    if (!TImageReader::load(TFilePath(path), ras) || !ras) return QImage();
    if (!TRaster32P(ras) && !TRasterGR8P(ras)) {
      TRaster32P ras32(ras->getLx(), ras->getLy());
      TRop::convert(ras32, ras);
      ras = ras32;
    }
    // rasterToQImage() returns a deep copy when mirroring
    return rasterToQImage(ras);
  } catch (...) {
  }
  return QImage();
}

}  // namespace

//=============================================================================
// ReferenceImageView
//-----------------------------------------------------------------------------

ReferenceImageView::ReferenceImageView(QWidget *parent)
    : QWidget(parent)
    , m_zoom(1.0)
    , m_rotation(0)
    , m_flipH(false)
    , m_flipV(false)
    , m_autoFit(true)
    , m_panning(false) {
  setMinimumSize(50, 50);
  setFocusPolicy(Qt::ClickFocus);
}

//-----------------------------------------------------------------------------

void ReferenceImageView::setImage(const QImage &image) {
  m_image = image.convertToFormat(QImage::Format_ARGB32_Premultiplied);
  fitToView();
}

//-----------------------------------------------------------------------------

void ReferenceImageView::fitToView() {
  m_autoFit = true;
  m_pan     = QPointF();
  if (!m_image.isNull()) {
    double w = m_image.width(), h = m_image.height();
    if (m_rotation % 2) std::swap(w, h);
    m_zoom = std::min(width() / w, height() / h);
    m_zoom = std::max(minZoom, std::min(maxZoom, m_zoom));
  }
  update();
}

//-----------------------------------------------------------------------------

void ReferenceImageView::setFlipH(bool on) {
  m_flipH = on;
  update();
}

//-----------------------------------------------------------------------------

void ReferenceImageView::setFlipV(bool on) {
  m_flipV = on;
  update();
}

//-----------------------------------------------------------------------------

void ReferenceImageView::rotate(int quarterTurns) {
  m_rotation = ((m_rotation + quarterTurns) % 4 + 4) % 4;
  if (m_autoFit)
    fitToView();
  else
    update();
}

//-----------------------------------------------------------------------------

void ReferenceImageView::paintEvent(QPaintEvent *) {
  QPainter p(this);
  p.fillRect(rect(), palette().color(QPalette::Window).darker(115));

  if (m_image.isNull()) {
    p.setPen(palette().color(QPalette::WindowText));
    p.drawText(rect(), Qt::AlignCenter | Qt::TextWordWrap,
               tr("Load an image, or drop an image file here."));
    return;
  }

  // The mirror is applied after the rotation so that it acts on the screen
  // axes.
  p.translate(QPointF(width() * 0.5, height() * 0.5) + m_pan);
  p.scale(m_zoom, m_zoom);
  p.scale(m_flipH ? -1.0 : 1.0, m_flipV ? -1.0 : 1.0);
  p.rotate(90.0 * m_rotation);
  // Show crisp pixels when zoomed in
  p.setRenderHint(QPainter::SmoothPixmapTransform, m_zoom < 2.0);
  p.drawImage(QPointF(-m_image.width() * 0.5, -m_image.height() * 0.5),
              m_image);
}

//-----------------------------------------------------------------------------

void ReferenceImageView::resizeEvent(QResizeEvent *) {
  if (m_autoFit) fitToView();
}

//-----------------------------------------------------------------------------

void ReferenceImageView::mousePressEvent(QMouseEvent *e) {
  if (m_image.isNull()) return;
  if (e->button() == Qt::LeftButton || e->button() == Qt::MiddleButton) {
    m_panning = true;
    m_lastPos = e->pos();
    setCursor(Qt::ClosedHandCursor);
  }
}

//-----------------------------------------------------------------------------

void ReferenceImageView::mouseMoveEvent(QMouseEvent *e) {
  if (!m_panning) return;
  m_pan += QPointF(e->pos() - m_lastPos);
  m_lastPos = e->pos();
  m_autoFit = false;
  update();
}

//-----------------------------------------------------------------------------

void ReferenceImageView::mouseReleaseEvent(QMouseEvent *) {
  if (!m_panning) return;
  m_panning = false;
  unsetCursor();
}

//-----------------------------------------------------------------------------

void ReferenceImageView::mouseDoubleClickEvent(QMouseEvent *e) {
  if (e->button() == Qt::LeftButton) fitToView();
}

//-----------------------------------------------------------------------------

void ReferenceImageView::wheelEvent(QWheelEvent *e) {
  e->accept();
  if (m_image.isNull()) return;
  int delta = e->angleDelta().y();
  if (delta == 0) return;

  double newZoom = m_zoom * std::pow(2.0, delta / 480.0);
  newZoom        = std::max(minZoom, std::min(maxZoom, newZoom));
  double factor  = newZoom / m_zoom;

  // Keep the image point under the cursor in place
  QPointF center(width() * 0.5, height() * 0.5);
  QPointF anchor = e->position() - center;
  m_pan          = anchor - (anchor - m_pan) * factor;
  m_zoom         = newZoom;
  m_autoFit      = false;
  update();
}

//=============================================================================
// ReferencePanel
//-----------------------------------------------------------------------------

ReferencePanel::ReferencePanel(QWidget *parent) : QFrame(parent) {
  setAcceptDrops(true);

  m_view = new ReferenceImageView(this);

  QToolButton *loadButton = new QToolButton(this);
  loadButton->setText(tr("Load Image..."));
  loadButton->setIcon(createQIcon("load"));
  loadButton->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
  loadButton->setAutoRaise(true);

  auto makeButton = [this](const char *iconName, const QString &toolTip,
                           bool checkable) {
    QToolButton *button = new QToolButton(this);
    button->setIcon(createQIcon(iconName));
    button->setToolTip(toolTip);
    button->setCheckable(checkable);
    button->setAutoRaise(true);
    return button;
  };
  m_flipHButton = makeButton("fliphoriz", tr("Flip Horizontally"), true);
  m_flipVButton = makeButton("flipvert", tr("Flip Vertically"), true);
  QToolButton *rotLeftButton =
      makeButton("rotateleft", tr("Rotate Left"), false);
  QToolButton *rotRightButton =
      makeButton("rotateright", tr("Rotate Right"), false);
  QToolButton *fitButton =
      makeButton("fit_to_window", tr("Fit to Panel"), false);

  QHBoxLayout *buttonLay = new QHBoxLayout();
  buttonLay->setContentsMargins(2, 2, 2, 2);
  buttonLay->setSpacing(2);
  {
    buttonLay->addWidget(loadButton);
    buttonLay->addStretch(1);
    buttonLay->addWidget(m_flipHButton);
    buttonLay->addWidget(m_flipVButton);
    buttonLay->addWidget(rotLeftButton);
    buttonLay->addWidget(rotRightButton);
    buttonLay->addWidget(fitButton);
  }

  QVBoxLayout *mainLay = new QVBoxLayout();
  mainLay->setContentsMargins(0, 0, 0, 0);
  mainLay->setSpacing(0);
  {
    mainLay->addLayout(buttonLay);
    mainLay->addWidget(m_view, 1);
  }
  setLayout(mainLay);

  connect(loadButton, &QToolButton::clicked, this,
          &ReferencePanel::onLoadImage);
  connect(m_flipHButton, &QToolButton::toggled, m_view,
          &ReferenceImageView::setFlipH);
  connect(m_flipVButton, &QToolButton::toggled, m_view,
          &ReferenceImageView::setFlipV);
  connect(rotLeftButton, &QToolButton::clicked, this,
          [this]() { m_view->rotate(-1); });
  connect(rotRightButton, &QToolButton::clicked, this,
          [this]() { m_view->rotate(1); });
  connect(fitButton, &QToolButton::clicked, m_view,
          &ReferenceImageView::fitToView);

  TFilePath lastPath = ReferencePanelLastImage;
  if (!lastPath.isEmpty()) {
    QString path = toQString(lastPath);
    if (QFileInfo(path).isFile()) {
      QImage image = readImageFile(path);
      if (!image.isNull()) m_view->setImage(image);
    }
  }
}

//-----------------------------------------------------------------------------

bool ReferencePanel::loadImage(const QString &path) {
  QImage image = readImageFile(path);
  if (image.isNull()) {
    DVGui::warning(tr("Unable to load the image %1").arg(path));
    return false;
  }
  m_view->setImage(image);
  ReferencePanelLastImage = TFilePath(path);
  return true;
}

//-----------------------------------------------------------------------------

void ReferencePanel::onLoadImage() {
  QString dir;
  TFilePath lastPath = ReferencePanelLastImage;
  if (!lastPath.isEmpty()) dir = QFileInfo(toQString(lastPath)).absolutePath();

  QString path =
      QFileDialog::getOpenFileName(this, tr("Load Reference Image"), dir,
                                   tr("Images") +
                                       " (*.png *.jpg *.jpeg *.bmp *.gif *.tif "
                                       "*.tiff *.tga *.psd *.webp);;" +
                                       tr("All Files") + " (*)");
  if (!path.isEmpty()) loadImage(path);
}

//-----------------------------------------------------------------------------

void ReferencePanel::dragEnterEvent(QDragEnterEvent *e) {
  const QMimeData *mimeData = e->mimeData();
  if (mimeData->hasUrls() && !mimeData->urls().isEmpty() &&
      mimeData->urls().first().isLocalFile())
    e->acceptProposedAction();
}

//-----------------------------------------------------------------------------

void ReferencePanel::dropEvent(QDropEvent *e) {
  const QMimeData *mimeData = e->mimeData();
  if (!mimeData->hasUrls() || mimeData->urls().isEmpty()) return;
  QUrl url = mimeData->urls().first();
  if (!url.isLocalFile()) return;
  if (loadImage(url.toLocalFile())) e->acceptProposedAction();
}
