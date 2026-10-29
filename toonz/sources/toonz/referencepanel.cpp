#include "toonzqt/uitranslations.h"
#include "referencepanel.h"

// TnzQt includes
#include "toonzqt/gutil.h"

// TnzCore includes
#include "tenv.h"
#include "tmsgcore.h"
#include "tfilepath.h"
#include "timage_io.h"
#include "traster.h"
#include "trasterimage.h"
#include "trop.h"
#include "../common/psdlib/psd.h"

// Qt includes
#include <QComboBox>
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
#include <QSlider>
#include <QStringList>
#include <QToolButton>
#include <QUrl>
#include <QVBoxLayout>
#include <QWheelEvent>

#include <algorithm>
#include <cmath>

namespace {
// AdjustToMinimumContentsLength is gone in Qt 6; its replacement also
// reserves room for an icon, so Qt 5 keeps the original value
const QComboBox::SizeAdjustPolicy c_comboSizePolicy =
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
    QComboBox::AdjustToMinimumContentsLengthWithIcon;
#else
    QComboBox::AdjustToMinimumContentsLength;
#endif
}  // namespace

// The path of the image shown last, kept for the older builds.
TEnv::FilePathVar ReferencePanelLastImage("ReferencePanelLastImage",
                                          TFilePath());
// The loaded images with their views, see ReferencePanel::saveEntries(), and
// the index of the one shown.
TEnv::StringVar ReferencePanelImages("ReferencePanelImages", "");
TEnv::IntVar ReferencePanelCurrentImage("ReferencePanelCurrentImage", 0);

namespace {

const double minZoom = 0.01, maxZoom = 64.0;
// The oldest images are dropped from the list beyond this number.
const int maxEntries = 20;

//-----------------------------------------------------------------------------

QImage rasterToImage(TRasterP ras) {
  if (!ras) return QImage();
  if (!TRaster32P(ras) && !TRasterGR8P(ras)) {
    TRaster32P ras32(ras->getLx(), ras->getLy());
    TRop::convert(ras32, ras);
    ras = ras32;
  }
  // rasterToQImage() returns a deep copy when mirroring
  return rasterToQImage(ras);
}

//-----------------------------------------------------------------------------

bool isPsd(const QString &path) {
  return QFileInfo(path).suffix().toLower() == "psd";
}

//-----------------------------------------------------------------------------

// A single layer of a PSD file, as large as the canvas.
QImage readPsdLayer(const QString &path, int layerId) {
  try {
    TPSDReader reader{TFilePath(path)};
    TRasterImageP img;
    reader.load(img, layerId);
    if (!img) return QImage();
    return rasterToImage(img->getRaster());
  } catch (...) {
  }
  return QImage();
}

//-----------------------------------------------------------------------------

// The layers of a PSD file that have pixels (the folder markers are left out),
// the topmost first, as pairs of layer id and name.
QList<QPair<int, QString>> readPsdLayers(const QString &path) {
  QList<QPair<int, QString>> layers;
  try {
    TPSDReader reader{TFilePath(path)};
    TPSDHeaderInfo header = reader.getPSDHeaderInfo();
    for (int i = header.layersCount - 1; i >= 0; --i) {
      TPSDLayerInfo *li = reader.getLayerInfo(i);
      if (!li || li->layerId == 0) continue;
      if (li->right <= li->left || li->bottom <= li->top) continue;
      QString name = li->name ? QString::fromLocal8Bit(li->name) : QString();
      if (name.isEmpty()) name = UiTranslations::text(QObject::tr("Layer %1")).arg(li->layerId);
      layers.append(qMakePair((int)li->layerId, name));
    }
  } catch (...) {
    layers.clear();
  }
  return layers;
}

//-----------------------------------------------------------------------------

// layerId is the layer of a PSD file; 0 reads the whole (flattened) image.
QImage readImageFile(const QString &path, int layerId = 0) {
  if (layerId > 0) return readPsdLayer(path, layerId);

  QImageReader reader(path);
  reader.setAutoTransform(true);
  QImage image = reader.read();
  if (!image.isNull()) return image;

  // Fall back to the OpenToonz image readers (tga, psd, sgi, ...)
  try {
    TRasterP ras;
    if (!TImageReader::load(TFilePath(path), ras) || !ras) return QImage();
    return rasterToImage(ras);
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
    , m_panning(false)
    , m_opacity(1.0) {
  setMinimumSize(50, 50);
  setFocusPolicy(Qt::ClickFocus);
}

//-----------------------------------------------------------------------------

void ReferenceImageView::setImage(const QImage &image) {
  setImage(image, nullptr);
}

//-----------------------------------------------------------------------------

void ReferenceImageView::setImage(const QImage &image,
                                  const ReferenceViewState *state) {
  m_image = image.convertToFormat(QImage::Format_ARGB32_Premultiplied);
  if (!state) {
    applyFit();
    return;
  }
  m_rotation = ((state->rotation % 4) + 4) % 4;
  m_flipH    = state->flipH;
  m_flipV    = state->flipV;
  m_opacity  = std::max(0.0, std::min(1.0, state->opacity));
  if (state->autoFit)
    applyFit();
  else {
    m_autoFit = false;
    m_zoom    = std::max(minZoom, std::min(maxZoom, state->zoom));
    m_pan     = state->pan;
    update();
  }
}

//-----------------------------------------------------------------------------

ReferenceViewState ReferenceImageView::getViewState() const {
  ReferenceViewState state;
  state.zoom     = m_zoom;
  state.pan      = m_pan;
  state.rotation = m_rotation;
  state.flipH    = m_flipH;
  state.flipV    = m_flipV;
  state.autoFit  = m_autoFit;
  state.opacity  = m_opacity;
  return state;
}

//-----------------------------------------------------------------------------

void ReferenceImageView::setOpacity(double opacity) {
  m_opacity = std::max(0.0, std::min(1.0, opacity));
  update();
  emit viewStateChanged();
}

//-----------------------------------------------------------------------------

void ReferenceImageView::applyFit() {
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

void ReferenceImageView::fitToView() {
  applyFit();
  emit viewStateChanged();
}

//-----------------------------------------------------------------------------

void ReferenceImageView::setFlipH(bool on) {
  m_flipH = on;
  update();
  emit viewStateChanged();
}

//-----------------------------------------------------------------------------

void ReferenceImageView::setFlipV(bool on) {
  m_flipV = on;
  update();
  emit viewStateChanged();
}

//-----------------------------------------------------------------------------

void ReferenceImageView::rotate(int quarterTurns) {
  m_rotation = ((m_rotation + quarterTurns) % 4 + 4) % 4;
  if (m_autoFit)
    applyFit();
  else
    update();
  emit viewStateChanged();
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
  p.setOpacity(m_opacity);
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
  if (m_autoFit) applyFit();
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
  emit viewStateChanged();
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
  emit viewStateChanged();
}

//=============================================================================
// ReferencePanel
//-----------------------------------------------------------------------------

ReferencePanel::ReferencePanel(QWidget *parent)
    : QFrame(parent), m_current(-1), m_updating(false) {
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

  // The list of the loaded images, and the layers of a PSD file
  m_imageCombo = new QComboBox(this);
  m_imageCombo->setSizeAdjustPolicy(c_comboSizePolicy);
  m_imageCombo->setMinimumContentsLength(8);
  m_imageCombo->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
  m_imageCombo->setToolTip(tr("Loaded Images"));
  QToolButton *removeButton = new QToolButton(this);
  removeButton->setText(tr("Remove"));
  removeButton->setToolTip(tr("Remove the Image from the List"));
  removeButton->setAutoRaise(true);

  m_layerCombo = new QComboBox(this);
  m_layerCombo->setSizeAdjustPolicy(c_comboSizePolicy);
  m_layerCombo->setMinimumContentsLength(8);
  m_layerCombo->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
  m_layerCombo->setToolTip(tr("PSD Layer"));
  m_layerCombo->hide();

  m_opacitySlider = new QSlider(Qt::Horizontal, this);
  m_opacitySlider->setRange(0, 100);
  m_opacitySlider->setValue(100);
  m_opacitySlider->setFixedWidth(80);
  m_opacitySlider->setToolTip(tr("Opacity"));

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

  QHBoxLayout *imageLay = new QHBoxLayout();
  imageLay->setContentsMargins(2, 0, 2, 2);
  imageLay->setSpacing(2);
  {
    imageLay->addWidget(m_imageCombo, 1);
    imageLay->addWidget(removeButton);
    imageLay->addWidget(m_layerCombo, 1);
    imageLay->addWidget(m_opacitySlider);
  }

  QVBoxLayout *mainLay = new QVBoxLayout();
  mainLay->setContentsMargins(0, 0, 0, 0);
  mainLay->setSpacing(0);
  {
    mainLay->addLayout(buttonLay);
    mainLay->addLayout(imageLay);
    mainLay->addWidget(m_view, 1);
  }
  setLayout(mainLay);

  connect(loadButton, &QToolButton::clicked, this,
          &ReferencePanel::onLoadImage);
  connect(removeButton, &QToolButton::clicked, this,
          &ReferencePanel::onRemoveImage);
  connect(m_imageCombo, QOverload<int>::of(&QComboBox::currentIndexChanged),
          this, &ReferencePanel::onImageComboChanged);
  connect(m_layerCombo, QOverload<int>::of(&QComboBox::currentIndexChanged),
          this, &ReferencePanel::onLayerComboChanged);
  connect(m_opacitySlider, &QSlider::valueChanged, this,
          &ReferencePanel::onOpacityChanged);
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
  connect(m_view, &ReferenceImageView::viewStateChanged, this,
          &ReferencePanel::onViewStateChanged);

  loadEntries();
}

//-----------------------------------------------------------------------------

// Shows the image of the entry with its remembered view, or a view that
// continues the current one (mirror, rotation, opacity) for a new image.
bool ReferencePanel::showEntry(int index) {
  if (index < 0 || index >= (int)m_entries.size()) return false;

  storeCurrentState();

  Entry &entry = m_entries[index];
  QImage image = readImageFile(entry.path, entry.layerId);
  if (image.isNull()) return false;

  ReferenceViewState state =
      entry.hasState ? entry.state : m_view->getViewState();
  if (!entry.hasState) state.autoFit = true;

  m_layers.clear();
  if (isPsd(entry.path)) m_layers = readPsdLayers(entry.path);

  m_updating = true;
  m_view->setImage(image, &state);
  entry.state    = m_view->getViewState();
  entry.hasState = true;
  m_current      = index;
  m_flipHButton->setChecked(entry.state.flipH);
  m_flipVButton->setChecked(entry.state.flipV);
  m_opacitySlider->setValue(tround(entry.state.opacity * 100.0));
  m_updating = false;

  updateCombos();
  ReferencePanelLastImage = TFilePath(entry.path);
  saveEntries();
  return true;
}

//-----------------------------------------------------------------------------

void ReferencePanel::storeCurrentState() {
  if (m_current < 0 || m_current >= (int)m_entries.size()) return;
  m_entries[m_current].state    = m_view->getViewState();
  m_entries[m_current].hasState = true;
}

//-----------------------------------------------------------------------------

void ReferencePanel::updateCombos() {
  bool wasUpdating = m_updating;
  m_updating       = true;

  m_imageCombo->clear();
  for (int i = 0; i < (int)m_entries.size(); i++) {
    m_imageCombo->addItem(QFileInfo(m_entries[i].path).fileName());
    m_imageCombo->setItemData(i, m_entries[i].path, Qt::ToolTipRole);
  }
  m_imageCombo->setCurrentIndex(m_current);

  // Only a PSD file with layers offers them
  m_layerCombo->clear();
  if (m_current >= 0 && !m_layers.isEmpty()) {
    m_layerCombo->addItem(tr("Flattened image"), QVariant(0));
    for (const QPair<int, QString> &layer : m_layers)
      m_layerCombo->addItem(layer.second, layer.first);
    int layerIndex = m_layerCombo->findData(m_entries[m_current].layerId);
    m_layerCombo->setCurrentIndex(std::max(0, layerIndex));
    m_layerCombo->show();
  } else
    m_layerCombo->hide();

  m_updating = wasUpdating;
}

//-----------------------------------------------------------------------------

// The list is stored as one line: the entries are separated by '|', their
// fields by ';', and the path is percent-encoded.
void ReferencePanel::saveEntries() const {
  QStringList items;
  for (const Entry &entry : m_entries) {
    const ReferenceViewState &s = entry.state;
    QStringList fields;
    fields << QString::fromLatin1(QUrl::toPercentEncoding(entry.path))
           << QString::number(entry.layerId) << QString::number(s.zoom, 'g', 9)
           << QString::number(s.pan.x(), 'g', 9)
           << QString::number(s.pan.y(), 'g', 9) << QString::number(s.rotation)
           << QString::number(s.flipH ? 1 : 0)
           << QString::number(s.flipV ? 1 : 0)
           << QString::number(s.autoFit ? 1 : 0)
           << QString::number(s.opacity, 'g', 4);
    items << fields.join(";");
  }
  ReferencePanelImages       = items.join("|").toStdString();
  ReferencePanelCurrentImage = m_current;
}

//-----------------------------------------------------------------------------

void ReferencePanel::loadEntries() {
  QString text = QString::fromStdString(ReferencePanelImages);
  for (const QString &item : text.split('|', Qt::SkipEmptyParts)) {
    QStringList fields = item.split(';');
    if (fields.size() < 10) continue;
    Entry entry;
    entry.path = QUrl::fromPercentEncoding(fields[0].toLatin1());
    if (!QFileInfo(entry.path).isFile()) continue;
    entry.layerId         = std::max(0, fields[1].toInt());
    ReferenceViewState &s = entry.state;
    s.zoom                = fields[2].toDouble();
    s.pan                 = QPointF(fields[3].toDouble(), fields[4].toDouble());
    s.rotation            = fields[5].toInt();
    s.flipH               = fields[6].toInt() != 0;
    s.flipV               = fields[7].toInt() != 0;
    s.autoFit             = fields[8].toInt() != 0;
    s.opacity             = fields[9].toDouble();
    if (!(s.zoom > 0.0)) s.autoFit = true;
    entry.hasState = true;
    m_entries.push_back(entry);
  }

  // Older builds only remembered the last image
  if (m_entries.empty()) {
    TFilePath lastPath = ReferencePanelLastImage;
    if (!lastPath.isEmpty() && QFileInfo(toQString(lastPath)).isFile()) {
      Entry entry;
      entry.path = toQString(lastPath);
      m_entries.push_back(entry);
    }
  }
  if (m_entries.empty()) return;

  // Show the remembered image, or the first one that can be read
  int start =
      std::min((int)ReferencePanelCurrentImage, (int)m_entries.size() - 1);
  start = std::max(0, start);
  while (!m_entries.empty()) {
    if (showEntry(start)) return;
    // not readable: forget it
    m_entries.erase(m_entries.begin() + start);
    start = std::min(start, (int)m_entries.size() - 1);
  }
  m_current = -1;
  updateCombos();
}

//-----------------------------------------------------------------------------

bool ReferencePanel::loadImage(const QString &path) {
  // An image already in the list is read again (the file may have changed) and
  // shown with its view
  for (int i = 0; i < (int)m_entries.size(); i++) {
    if (m_entries[i].path != path) continue;
    if (showEntry(i)) return true;
    DVGui::warning(UiTranslations::text(tr("Unable to load the image %1")).arg(path));
    return false;
  }

  QImage image = readImageFile(path);
  if (image.isNull()) {
    DVGui::warning(UiTranslations::text(tr("Unable to load the image %1")).arg(path));
    return false;
  }

  // Keep the current index valid when the oldest image is dropped
  if ((int)m_entries.size() >= maxEntries) {
    storeCurrentState();
    int removed = (m_current == 0) ? 1 : 0;
    m_entries.erase(m_entries.begin() + removed);
    if (m_current > removed) m_current--;
  }

  Entry entry;
  entry.path = path;
  m_entries.push_back(entry);
  if (!showEntry((int)m_entries.size() - 1)) {
    m_entries.pop_back();
    updateCombos();
    return false;
  }
  return true;
}

//-----------------------------------------------------------------------------

void ReferencePanel::onLoadImage() {
  QString dir;
  TFilePath lastPath = ReferencePanelLastImage;
  if (!lastPath.isEmpty()) dir = QFileInfo(toQString(lastPath)).absolutePath();

  QStringList paths = QFileDialog::getOpenFileNames(
      this, tr("Load Reference Image"), dir,
      tr("Images") +
          " (*.png *.jpg *.jpeg *.bmp *.gif *.tif "
          "*.tiff *.tga *.psd *.webp);;" +
          tr("All Files") + " (*)");
  for (const QString &path : paths) loadImage(path);
}

//-----------------------------------------------------------------------------

void ReferencePanel::onRemoveImage() {
  if (m_current < 0 || m_current >= (int)m_entries.size()) return;
  int removed = m_current;
  m_entries.erase(m_entries.begin() + removed);
  m_current = -1;  // its state is gone

  while (!m_entries.empty()) {
    int index = std::min(removed, (int)m_entries.size() - 1);
    if (showEntry(index)) return;
    m_entries.erase(m_entries.begin() + index);
  }
  m_updating = true;
  m_view->setImage(QImage(), nullptr);
  m_updating = false;
  m_layers.clear();
  updateCombos();
  ReferencePanelLastImage = TFilePath();
  saveEntries();
}

//-----------------------------------------------------------------------------

void ReferencePanel::onImageComboChanged(int index) {
  if (m_updating || index < 0 || index == m_current) return;
  if (!showEntry(index)) {
    DVGui::warning(UiTranslations::text(tr("Unable to load the image %1"))
                       .arg(index < (int)m_entries.size()
                                ? m_entries[index].path
                                : QString()));
    updateCombos();  // back to the current image
  }
}

//-----------------------------------------------------------------------------

// Shows another layer of the PSD file, with the same view.
void ReferencePanel::onLayerComboChanged(int index) {
  if (m_updating || index < 0 || m_current < 0) return;
  Entry &entry = m_entries[m_current];
  int layerId  = m_layerCombo->itemData(index).toInt();
  if (layerId == entry.layerId) return;

  QImage image = readImageFile(entry.path, layerId);
  if (image.isNull()) {
    DVGui::warning(UiTranslations::text(tr("Unable to load the layer of %1")).arg(entry.path));
    updateCombos();  // back to the current layer
    return;
  }
  entry.layerId = layerId;
  storeCurrentState();
  m_updating = true;
  m_view->setImage(image, &entry.state);
  m_updating = false;
  saveEntries();
}

//-----------------------------------------------------------------------------

void ReferencePanel::onOpacityChanged(int value) {
  if (m_updating) return;
  m_view->setOpacity(value / 100.0);
}

//-----------------------------------------------------------------------------

void ReferencePanel::onViewStateChanged() {
  if (m_updating) return;
  storeCurrentState();
  saveEntries();
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
  bool loaded = false;
  for (const QUrl &url : mimeData->urls()) {
    if (!url.isLocalFile()) continue;
    if (loadImage(url.toLocalFile())) loaded = true;
  }
  if (loaded) e->acceptProposedAction();
}
