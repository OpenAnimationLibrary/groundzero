#include "cadmiumpopup.h"
#include "cadmiumruntime.h"
#include "cadmiumsource.h"
#include "cadmiumdestination.h"
#include "cadmiumcodec.h"
#include "cadmiumcolor.h"
#include "trop.h"
#include "tropcm.h"
#include <cmath>
#include "tapp.h"
#include "tundo.h"
#include "ttoonzimage.h"
#include "trasterimage.h"
#include "tcolorstyles.h"
#include "toonz/toonzscene.h"
#include "toonz/levelset.h"
#include "toonz/levelproperties.h"
#include "toonz/tscenehandle.h"
#include "toonz/txshlevelhandle.h"
#include "toonzqt/icongenerator.h"
#include <QBuffer>
#include <QApplication>
#include <QCheckBox>
#include <QCloseEvent>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QFileDialog>
#include <QFormLayout>
#include <QHeaderView>
#include <QHash>
#include <QJsonArray>
#include <QJsonObject>
#include <QLabel>
#include <QListWidget>
#include <QMessageBox>
#include <QPainter>
#include <QPointer>
#include <QPushButton>
#include <QSet>
#include <QSplitter>
#include <QTableWidget>
#include <QTimer>
#include <QThread>
#include <QVBoxLayout>
#include <algorithm>
#include <climits>
#include <memory>

namespace {
QString frameName(const TFrameId &fid) {
  return QString::number(fid.getNumber()) + fid.getLetter();
}

using Cadmium::labelUri;

TImageP captureImage(const TImageP &source) {
  if (!source) return TImageP();
  TImageP captured(source->cloneImage());
  TToonzImageP from(source), to(captured);
  if (from && to) {
    to->setOffset(from->getOffset());
    to->setSubsampling(from->getSubsampling());
  }
  if (source->getPalette()) captured->setPalette(source->getPalette()->clone());
  return captured;
}

QImage displayImage(const TImageP &, const Cadmium::Frame &);

class CadmiumRasterTask final : public QThread {
public:
  Cadmium::Frame target, reference;
  QString error;
  bool success = false;
  bool single = false;
  QImage before;
  void run() override {
    try {
      auto prepare = [this](Cadmium::Frame &frame) {
        const TImageP image = frame.original;
        if (frame.kind == Cadmium::Kind::ToonzRaster)
          return Cadmium::snapshotToonzRaster(image, frame, error);
        return Cadmium::snapshotFullColor(image, frame, error);
      };
      success = prepare(target) && (single || prepare(reference));
      if (success && single)
        before = displayImage(target.original, target).scaled(320, 260, Qt::KeepAspectRatio, Qt::SmoothTransformation);
    } catch (...) {
      error = QObject::tr("The drawing analysis could not be completed. Try a smaller batch.");
    }
  }
};

QImage preview(const Cadmium::Frame &frame, const QVector<QColor> &colors) {
  if (frame.lines.isNull() ||
      frame.labels.size() != frame.lines.width() * frame.lines.height())
    return QImage();
  QImage image(frame.lines.size(), QImage::Format_ARGB32_Premultiplied);
  if (image.isNull()) return image;
  image.fill(Qt::transparent);
  for (int y = 0; y < image.height(); ++y) {
    QRgb *row = reinterpret_cast<QRgb *>(image.scanLine(y));
    for (int x = 0; x < image.width(); ++x) {
      const int label = frame.labels[y * image.width() + x];
      QColor color;
      if (label >= 0 && label < colors.size()) color = colors[label];
      if (!color.isValid() && label >= 0 && label < frame.regionColors.size())
        color = frame.regionColors[label];
      row[x] = color.isValid() ? qPremultiply(color.rgba()) : 0;
    }
  }
  QPainter painter(&image);
  painter.drawImage(0, 0, frame.lines);
  return image;
}

QVector<int> acceptedStyles(QVector<int> styles) {
  for (int &style : styles) if (style == -2) style = -1;
  return styles;
}

QImage displayImage(const TImageP &image, const Cadmium::Frame &analysis) {
  TRasterP raster;
  TRasterImageP fullColor(image);
  if (fullColor) raster = fullColor->getRaster();
  TToonzImageP native(image);
  if (native) {
    TRaster32P converted(native->getRaster()->getSize());
    TRop::convert(converted, native->getRaster(), native->getPalette(), TRect());
    raster = converted;
  }
  if (!raster) {
    Cadmium::Frame painted;
    QString error;
    if (Cadmium::snapshotVector(image, painted, error))
      return preview(painted, painted.regionColors);
    return preview(analysis, analysis.regionColors);
  }
  QImage result(raster->getLx(), raster->getLy(), QImage::Format_ARGB32_Premultiplied);
  if (result.isNull()) return result;
  raster->lock();
  for (int y = 0; y < result.height(); ++y) {
    QRgb *row = reinterpret_cast<QRgb *>(result.scanLine(y));
    const int sy = result.height() - 1 - y;
    for (int x = 0; x < result.width(); ++x) {
      double r, g, b, a;
      if (TRaster32P ras = raster) {
        const TPixel32 p = ras->pixels(sy)[x];
        r = p.r / 255.0; g = p.g / 255.0; b = p.b / 255.0; a = p.m / 255.0;
      } else if (TRaster64P ras = raster) {
        const TPixel64 p = ras->pixels(sy)[x];
        r = p.r / 65535.0; g = p.g / 65535.0; b = p.b / 65535.0; a = p.m / 65535.0;
      } else if (TRasterFP ras = raster) {
        const TPixelF p = ras->pixels(sy)[x];
        r = p.r; g = p.g; b = p.b; a = p.m;
      } else { row[x] = 0; continue; }
      if (raster->isLinear() && a > 0.0) {
        auto encode = [a](double channel) {
          const double v = std::max(0.0, channel / a);
          return a * (v <= 0.0031308 ? 12.92 * v : 1.055 * std::pow(v, 1.0 / 2.4) - 0.055);
        };
        r = encode(r); g = encode(g); b = encode(b);
      }
      auto byte = [](double value) { return int(std::round(std::max(0.0, std::min(1.0, value)) * 255.0)); };
      row[x] = qRgba(byte(r), byte(g), byte(b), byte(a));
    }
  }
  raster->unlock();
  return result;
}

void notifyDrawings(TXshSimpleLevel *level,
                    const std::map<TFrameId, TImageP> &images) {
  std::map<TFrameId, TImageP> clones;
  std::map<TFrameId, TImageP> previous;
  const bool wasDirty = level->getDirtyFlag();
  for (const auto &entry : images) {
    TImageP clone(entry.second->cloneImage());
    TToonzImageP source(entry.second), nativeClone(clone);
    if (source && nativeClone) {
      nativeClone->setOffset(source->getOffset());
      nativeClone->setSubsampling(source->getSubsampling());
    }
    clone->setPalette(level->getPalette());
    clones[entry.first] = clone;
    if (level->fid2index(entry.first) >= 0)
      previous[entry.first] = Cadmium::loadDrawing(level, entry.first);
    else previous[entry.first] = TImageP();
  }
  try {
    for (const auto &entry : clones) {
      level->setFrame(entry.first, entry.second);
      level->touchFrame(entry.first);
    }
  } catch (...) {
    for (const auto &entry : previous) {
      if (entry.second) level->setFrame(entry.first, entry.second);
      else if (level->fid2index(entry.first) >= 0) level->eraseFrame(entry.first);
    }
    level->setDirtyFlag(wasDirty);
    throw;
  }
  for (const auto &entry : clones) {
    IconGenerator::instance()->invalidate(level, entry.first);
  }
  level->setDirtyFlag(true);
  TApp::instance()->getCurrentScene()->setDirtyFlag(true);
  TApp::instance()->getCurrentLevel()->notifyLevelChange();
  TApp::instance()->getCurrentScene()->notifyCastChange();
}

class CadmiumPaintUndo final : public TUndo {
  TXshSimpleLevelP m_level;
  TXshSimpleLevelP m_source;
  ToonzScene *m_scene;
  std::map<TFrameId, TImageP> m_before, m_after;
  bool m_newLevel;
public:
  CadmiumPaintUndo(TXshSimpleLevel *level,
                   const std::map<TFrameId, TImageP> &before,
                   const std::map<TFrameId, TImageP> &after, bool newLevel,
                   TXshSimpleLevel *source, ToonzScene *scene)
      : m_level(level), m_source(source), m_scene(scene),
        m_before(before), m_after(after), m_newLevel(newLevel) {}
  int getSize() const override {
    TINT64 bytes = sizeof(*this);
    for (const auto &entry : m_before)
      bytes += getImageMemorySize(entry.second.getPointer());
    for (const auto &entry : m_after)
      bytes += getImageMemorySize(entry.second.getPointer());
    return clampSize(bytes);
  }
  void undo() const override {
    if (TApp::instance()->getCurrentScene()->getScene() != m_scene) return;
    if (m_newLevel) {
      ToonzScene *scene = TApp::instance()->getCurrentScene()->getScene();
      if (TApp::instance()->getCurrentLevel()->getLevel() == m_level.getPointer())
        TApp::instance()->getCurrentLevel()->setLevel(m_source.getPointer());
      if (scene) scene->getLevelSet()->removeLevel(m_level.getPointer());
      TApp::instance()->getCurrentScene()->notifyCastChange();
      TApp::instance()->getCurrentScene()->setDirtyFlag(true);
    } else notifyDrawings(m_level.getPointer(), m_before);
  }
  void redo() const override {
    if (TApp::instance()->getCurrentScene()->getScene() != m_scene) return;
    if (m_newLevel) {
      ToonzScene *scene = TApp::instance()->getCurrentScene()->getScene();
      if (!scene) return;
      scene->getLevelSet()->insertLevel(m_level.getPointer());
    }
    notifyDrawings(m_level.getPointer(), m_after);
  }
  QString getHistoryString() override {
    return QObject::tr("Apply Cadmium Paint");
  }
};
}

void CadmiumPopup::open(TXshSimpleLevel *level,
                        const std::set<TFrameId> &selection, QWidget *parent) {
  static QPointer<CadmiumPopup> popup;
  if (!popup) popup = new CadmiumPopup(parent);
  if (popup->m_generating) {
    popup->show(); popup->raise(); popup->activateWindow();
    return;
  }
  popup->initialize(level, selection);
  popup->show(); popup->raise(); popup->activateWindow();
}

CadmiumPopup::CadmiumPopup(QWidget *parent) : QDialog(parent) {
  setWindowTitle(tr("Cadmium Assisted Paint"));
  resize(940, 720);
  auto *layout = new QVBoxLayout(this);
  auto *intro = new QLabel(tr("Mark painted references in the Level Strip, then select target drawings. "
                              "Review proposals before applying. Full-color results are created as a new level."), this);
  intro->setWordWrap(true);
  layout->addWidget(intro);
  auto *setup = new QHBoxLayout;
  auto *executable = new QPushButton(tr("Choose Worker…"), this);
  auto *models = new QPushButton(tr("Choose Models Folder…"), this);
  auto *unload = new QPushButton(tr("Unload Models"), this);
  setup->addWidget(executable); setup->addWidget(models); setup->addWidget(unload);
  layout->addLayout(setup);
  connect(executable, &QPushButton::clicked, this, [this] {
    const QString path = QFileDialog::getOpenFileName(this, tr("Choose Cadmium Worker"));
    if (!path.isEmpty()) CadmiumRuntime::instance()->setExecutablePath(path);
  });
  connect(models, &QPushButton::clicked, this, [this] {
    const QString path = QFileDialog::getExistingDirectory(this, tr("Choose Model Folder"));
    if (!path.isEmpty()) CadmiumRuntime::instance()->setModelsDirectory(path);
  });
  connect(unload, &QPushButton::clicked, CadmiumRuntime::instance(),
          &CadmiumRuntime::stop);

  auto *splitter = new QSplitter(this);
  m_list = new QListWidget(splitter);
  auto *details = new QWidget(splitter);
  auto *detailLayout = new QVBoxLayout(details);
  auto *form = new QFormLayout;
  m_reference = new QComboBox(details);
  form->addRow(tr("Reference drawing:"), m_reference);
  m_rasterSource = new QComboBox(details);
  m_rasterSource->addItems({tr("Transparent line artwork"), tr("Opaque paper scan"), tr("Linked clean-line level")});
  form->addRow(tr("Raster source:"), m_rasterSource);
  m_lineLevel = new QComboBox(details);
  form->addRow(tr("Clean-line level:"), m_lineLevel);
  m_paperThreshold = new QDoubleSpinBox(details);
  m_paperThreshold->setRange(0.0, 1.0); m_paperThreshold->setSingleStep(0.01);
  m_paperThreshold->setValue(0.92);
  form->addRow(tr("Paper threshold:"), m_paperThreshold);
  m_lineThreshold = new QDoubleSpinBox(details);
  m_lineThreshold->setRange(0.0, 1.0); m_lineThreshold->setSingleStep(0.01);
  m_lineThreshold->setValue(0.2);
  form->addRow(tr("Line threshold:"), m_lineThreshold);
  detailLayout->addLayout(form);
  m_extraction = new QCheckBox(tr("I reviewed and approve the extracted line mask"), details);
  detailLayout->addWidget(m_extraction);
  m_repaint = new QCheckBox(tr("Replace existing paint on selected targets"), details);
  detailLayout->addWidget(m_repaint);
  auto *previews = new QHBoxLayout;
  m_before = new QLabel(details); m_after = new QLabel(details);
  for (QLabel *label : {m_before, m_after}) {
    label->setMinimumSize(200, 200); label->setAlignment(Qt::AlignCenter);
    label->setStyleSheet(QStringLiteral("background-color: #808080;"));
    previews->addWidget(label);
  }
  detailLayout->addLayout(previews);
  m_regions = new QTableWidget(details);
  m_regions->setColumnCount(3);
  m_regions->setHorizontalHeaderLabels({tr("Region"), tr("Prediction"), tr("Native style / decision")});
  m_regions->horizontalHeader()->setStretchLastSection(true);
  detailLayout->addWidget(m_regions);
  splitter->setStretchFactor(1, 1);
  layout->addWidget(splitter, 1);
  m_status = new QLabel(this); m_status->setWordWrap(true);
  layout->addWidget(m_status);
  auto *buttons = new QHBoxLayout;
  m_generate = new QPushButton(tr("Generate Selected Drawings"), this);
  m_apply = new QPushButton(tr("Apply Drawing"), this);
  m_applyAll = new QPushButton(tr("Apply Reviewed Batch"), this);
  m_review = new QPushButton(tr("Include Drawing in Reviewed Batch"), this);
  m_cancel = new QPushButton(tr("Cancel Processing"), this);
  auto *discard = new QPushButton(tr("Discard Proposals"), this);
  buttons->addWidget(m_generate); buttons->addWidget(m_apply);
  buttons->addWidget(m_review);
  buttons->addWidget(m_applyAll); buttons->addWidget(m_cancel); buttons->addWidget(discard);
  layout->addLayout(buttons);
  connect(m_generate, &QPushButton::clicked, this, &CadmiumPopup::generate);
  connect(m_apply, &QPushButton::clicked, this, [this] { apply(false); });
  connect(m_applyAll, &QPushButton::clicked, this, [this] { apply(true); });
  connect(m_review, &QPushButton::clicked, this, [this] {
    const int row = m_list->currentRow();
    if (row >= 0 && row < int(m_drawings.size()) && m_drawings[row].ready) {
      m_drawings[row].reviewed = true; refresh();
    }
  });
  connect(m_cancel, &QPushButton::clicked, this, &CadmiumPopup::cancel);
  connect(discard, &QPushButton::clicked, this, [this] {
    cancel();
    for (Drawing &drawing : m_drawings) {
      drawing.ready = false; drawing.colors.clear(); drawing.styles.clear();
      drawing.nativeColors.clear();
      drawing.target = Cadmium::Frame(); drawing.reference = Cadmium::Frame();
    }
    refresh(); refreshPreview();
  });
  connect(m_list, &QListWidget::currentRowChanged, this, [this] { refreshPreview(); });
  connect(m_reference, QOverload<int>::of(&QComboBox::activated), this,
          [this] { assignReference(); });
  connect(m_rasterSource, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
          [this] {
    ++m_snapshotEpoch;
    m_extraction->setChecked(false);
    for (Drawing &drawing : m_drawings) { drawing.ready = false; drawing.reviewed = false; drawing.target = Cadmium::Frame(); }
    refresh(); refreshPreview();
  });
  connect(m_lineLevel, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
          [this] {
    ++m_snapshotEpoch;
    m_extraction->setChecked(false);
    for (Drawing &drawing : m_drawings) { drawing.ready = false; drawing.reviewed = false; drawing.target = Cadmium::Frame(); }
    refresh(); refreshPreview();
  });
  auto thresholdChanged = [this] {
    ++m_snapshotEpoch;
    m_extraction->setChecked(false);
    for (Drawing &drawing : m_drawings) { drawing.ready = false; drawing.reviewed = false; drawing.target = Cadmium::Frame(); }
    refresh(); refreshPreview();
  };
  connect(m_paperThreshold, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this, thresholdChanged);
  connect(m_lineThreshold, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this, thresholdChanged);
  connect(m_repaint, &QCheckBox::toggled, this, [this] {
    for (Drawing &drawing : m_drawings) drawing.reviewed = false;
    updateProposalPreview(); refresh();
  });
  CadmiumRuntime *runtime = CadmiumRuntime::instance();
  connect(runtime, &CadmiumRuntime::resultReady, this, &CadmiumPopup::received);
  connect(runtime, &CadmiumRuntime::failed, this, &CadmiumPopup::failed);
  connect(runtime, &CadmiumRuntime::workerStopped, this, [this] {
    if (!m_generating && !m_backgroundTasks) return;
    ++m_snapshotEpoch;
    m_cancelled = true;
    m_generating = false;
    m_running = -1;
    refresh();
    m_status->setText(tr("The worker was unloaded. Pending processing was cancelled."));
  });
  connect(runtime, &CadmiumRuntime::statusChanged, this, [this] {
    m_status->setText(CadmiumRuntime::instance()->statusText());
  });
}

bool CadmiumPopup::currentScene() const {
  return m_scene && TApp::instance()->getCurrentScene()->getScene() == m_scene &&
         m_level && m_scene->getLevelSet()->getLevel(m_level->getName()) == m_level.getPointer();
}

void CadmiumPopup::initialize(TXshSimpleLevel *level,
                              const std::set<TFrameId> &selection) {
  m_level = level;
  m_selectionRenumberCount = level ? level->getRenumberCount() : -1;
  ++m_snapshotEpoch;
  m_scene = TApp::instance()->getCurrentScene()->getScene();
  m_drawings.clear(); m_references.clear(); m_list->clear();
  m_reference->clear(); m_lineLevel->clear();
  m_running = -1; m_generating = false; m_cancelled = false;
  if (!level || !currentScene()) { refresh(); return; }
  std::vector<TFrameId> ordered;
  level->getFids(ordered);
  for (const TFrameId &fid : ordered)
    if (level->getCadmiumReferences().count(fid)) {
      m_references.push_back(fid);
      m_reference->addItem(frameName(fid));
    }
  std::vector<TXshLevel *> levels;
  m_scene->getLevelSet()->listLevels(levels);
  for (TXshLevel *candidate : levels)
    if (candidate->getSimpleLevel() && candidate != level &&
        candidate->getType() == OVL_XSHLEVEL)
      m_lineLevel->addItem(QString::fromStdWString(candidate->getName()),
                          QVariant::fromValue<qulonglong>(reinterpret_cast<qulonglong>(candidate)));
  for (const TFrameId &fid : selection) {
    if (level->getCadmiumReferences().count(fid) || level->fid2index(fid) < 0) continue;
    Drawing drawing; drawing.fid = fid;
    int bestDistance = INT_MAX;
    for (const TFrameId &reference : m_references) {
      const int distance = std::abs(level->fid2index(fid) - level->fid2index(reference));
      if (distance < bestDistance) {
        drawing.referenceId = reference; bestDistance = distance;
      }
    }
    m_drawings.push_back(drawing);
    m_list->addItem(frameName(fid));
  }
  const bool fullColor = level->getType() == OVL_XSHLEVEL;
  m_rasterSource->setVisible(fullColor); m_lineLevel->setVisible(fullColor);
  m_extraction->setVisible(fullColor);
  m_paperThreshold->setVisible(fullColor); m_lineThreshold->setVisible(fullColor);
  m_extraction->setChecked(false); m_repaint->setChecked(false);
  refresh();
  if (!m_drawings.empty()) m_list->setCurrentRow(0);
  m_status->setText(m_references.empty()
      ? tr("Mark at least one painted reference drawing in the Level Strip.")
      : tr("Ready to generate proposals for %1 unique drawings.").arg(m_drawings.size()));
}

bool CadmiumPopup::snapshot(const TFrameId &fid, Cadmium::Frame &frame, QString &error, bool captureOnly) {
  if (!currentScene() || m_level->fid2index(fid) < 0) {
    error = tr("The drawing or scene is no longer available."); return false;
  }
  const TImageP image = Cadmium::loadDrawing(m_level.getPointer(), fid);
  if (!image) { error = tr("The drawing could not be loaded."); return false; }
  if (captureOnly && TUndo::getImageMemorySize(image.getPointer()) > 128LL * 1024 * 1024) {
    error = tr("This drawing exceeds the analysis memory budget."); return false;
  }
  switch (m_level->getType()) {
  case TZP_XSHLEVEL:
    if (captureOnly) {
      frame.kind = Cadmium::Kind::ToonzRaster;
      frame.original = captureImage(image); return true;
    }
    return Cadmium::snapshotToonzRaster(image, frame, error);
  case PLI_XSHLEVEL: return Cadmium::snapshotVector(image, frame, error);
  case OVL_XSHLEVEL:
    frame.rasterSource = static_cast<Cadmium::RasterSource>(m_rasterSource->currentIndex());
    frame.extractionApproved = m_extraction->isChecked() || m_inspecting;
    frame.paperThreshold = m_paperThreshold->value();
    frame.lineThreshold = m_lineThreshold->value();
    if (frame.rasterSource == Cadmium::RasterSource::LinkedLines) {
      auto *line = reinterpret_cast<TXshSimpleLevel *>(m_lineLevel->currentData().toULongLong());
      bool found = false;
      std::vector<TXshLevel *> levels; m_scene->getLevelSet()->listLevels(levels);
      for (TXshLevel *candidate : levels) if (candidate == line) found = true;
      if (!found || !line || line->fid2index(fid) < 0) {
        error = tr("The clean-line level must contain the matching drawing ID."); return false;
      }
      frame.lineSource = Cadmium::loadDrawing(line, fid);
      if (captureOnly &&
          TUndo::getImageMemorySize(image.getPointer()) +
          TUndo::getImageMemorySize(frame.lineSource.getPointer()) > 128LL * 1024 * 1024) {
        error = tr("The drawing and linked line source exceed the analysis memory budget.");
        return false;
      }
      if (captureOnly) frame.lineSource = captureImage(frame.lineSource);
    }
    if (captureOnly) {
      frame.kind = Cadmium::Kind::FullColor;
      frame.original = captureImage(image);
      frame.protectionMask = frame.protectionMask.copy();
      return true;
    }
    return Cadmium::snapshotFullColor(image, frame, error);
  default: error = tr("This level does not support cel painting."); return false;
  }
}

void CadmiumPopup::generate() {
  if (m_generating || m_backgroundTasks || m_drawings.empty() || m_references.empty() || !currentScene()) return;
  if (m_level->getRenumberCount() != m_selectionRenumberCount) {
    QMessageBox::information(this, tr("Cadmium"), tr("The drawings were renumbered. Reopen Assisted Paint with the intended selection.")); return;
  }
  if (CadmiumRuntime::instance()->startupMode() == CadmiumRuntime::Off) {
    QMessageBox::information(this, tr("Cadmium"), tr("Enable Cadmium in Preferences before generating paint.")); return;
  }
  if (CadmiumRuntime::instance()->busy()) {
    QMessageBox::information(this, tr("Cadmium"), tr("The worker is already processing another request.")); return;
  }
  if (m_level->getType() == OVL_XSHLEVEL &&
      m_rasterSource->currentIndex() == int(Cadmium::RasterSource::PaperScan) &&
      !m_extraction->isChecked()) {
    refreshPreview();
    QMessageBox::information(this, tr("Cadmium"),
      tr("Review the extracted line mask in the preview, then approve extraction before generating."));
    return;
  }
  ++m_snapshotEpoch;
  m_cancelled = false; m_generating = true; m_running = -1;
  m_regions->setRowCount(0);
  m_regions->setEnabled(false);
  for (Drawing &drawing : m_drawings) {
    drawing.ready = false; drawing.applied = false; drawing.reviewed = false; drawing.error.clear();
    drawing.target = Cadmium::Frame(); drawing.reference = Cadmium::Frame();
    drawing.colors.clear(); drawing.styles.clear(); drawing.nativeColors.clear();
  }
  next();
}

void CadmiumPopup::next() {
  if (!m_generating) return;
  if (m_cancelled || !currentScene()) { cancel(); return; }
  if (m_level->getRenumberCount() != m_selectionRenumberCount) {
    cancel();
    m_status->setText(tr("The drawings were renumbered. Reopen Assisted Paint with the intended selection."));
    return;
  }
  if (++m_running >= int(m_drawings.size())) {
    m_running = -1; m_generating = false;
    m_status->setText(tr("Generation finished. Review each result before applying."));
    refresh(); refreshPreview(); return;
  }
  Drawing &drawing = m_drawings[m_running];
  QString error;
  constexpr qint64 budget = 512LL * 1024 * 1024;
  auto memory = [](const Cadmium::Frame &frame) {
    return TUndo::getImageMemorySize(frame.original.getPointer()) +
           TUndo::getImageMemorySize(frame.lineSource.getPointer()) +
           qint64(frame.lines.sizeInBytes()) +
           qint64(frame.labels.size() + frame.regionStyles.size() +
                  frame.nativeRegionIndices.size()) * qint64(sizeof(int)) +
           qint64(frame.regionColors.size()) * qint64(sizeof(QColor)) +
           qint64(frame.nativeRegionColors.size()) * qint64(sizeof(Cadmium::NativeColor));
  };
  qint64 held = 0;
  for (const Drawing &cached : m_drawings)
    held += memory(cached.target) + memory(cached.reference) +
            qint64(cached.styles.size()) * qint64(sizeof(int)) +
            qint64(cached.colors.size()) * qint64(sizeof(QColor)) +
            qint64(cached.nativeColors.size()) * qint64(sizeof(Cadmium::NativeColor));
  for (const TFrameId &fid : {drawing.fid, drawing.referenceId}) {
    const TImageP image = Cadmium::loadDrawing(m_level.getPointer(), fid);
    held += std::max<qint64>(16LL * 1024 * 1024,
                            TUndo::getImageMemorySize(image.getPointer()) * 4);
    if (m_level->getType() == OVL_XSHLEVEL &&
        m_rasterSource->currentIndex() == int(Cadmium::RasterSource::LinkedLines)) {
      auto *line = reinterpret_cast<TXshSimpleLevel *>(m_lineLevel->currentData().toULongLong());
      std::vector<TXshLevel *> levels;
      m_scene->getLevelSet()->listLevels(levels);
      if (line && std::find(levels.begin(), levels.end(), line) != levels.end()) {
        const TImageP lineImage = Cadmium::loadDrawing(line, fid);
        held += TUndo::getImageMemorySize(lineImage.getPointer()) * 4;
      }
    }
  }
  if (held > budget) {
    drawing.error = tr("The proposal cache reached its memory budget. Apply or discard completed results, then select a smaller batch.");
    m_generating = false; m_running = -1;
    m_status->setText(drawing.error); refresh(); return;
  }
  drawing.renumberCount = m_level->getRenumberCount();
  drawing.targetBuilder = ImageManager::instance()->getBuilder(m_level->getImageId(drawing.fid));
  drawing.referenceBuilder = ImageManager::instance()->getBuilder(m_level->getImageId(drawing.referenceId));
  if (m_level->getType() != PLI_XSHLEVEL) {
    auto *task = new CadmiumRasterTask;
    if (!snapshot(drawing.fid, task->target, error, true) ||
        !snapshot(drawing.referenceId, task->reference, error, true)) {
      delete task; drawing.error = error; refresh();
      scheduleNext(); return;
    }
    const quint64 epoch = m_snapshotEpoch;
    const int index = m_running;
    ++m_backgroundTasks;
    connect(task, &QThread::finished, this, [this, task, epoch, index] {
      --m_backgroundTasks;
      if (epoch != m_snapshotEpoch || m_cancelled || !m_generating ||
          m_running != index || !currentScene()) { refresh(); return; }
      Drawing &completed = m_drawings[index];
      if (task->success) {
        completed.target = task->target; completed.reference = task->reference;
        submit();
      } else {
        completed.error = task->error; refresh();
        scheduleNext();
      }
    });
    connect(task, &QThread::finished, task, &QObject::deleteLater);
    connect(qApp, &QCoreApplication::aboutToQuit, task, [task] { task->wait(); });
    m_status->setText(tr("Analyzing drawing %1 of %2…").arg(m_running + 1).arg(m_drawings.size()));
    refresh(); task->start(); return;
  }
  if (!snapshot(drawing.fid, drawing.target, error) ||
      !snapshot(drawing.referenceId, drawing.reference, error)) {
    drawing.error = error; refresh();
    scheduleNext(); return;
  }
  submit();
}

void CadmiumPopup::scheduleNext() {
  const quint64 epoch = m_snapshotEpoch;
  const int index = m_running;
  QTimer::singleShot(0, this, [this, epoch, index] {
    if (epoch != m_snapshotEpoch || index != m_running ||
        !m_generating || m_cancelled) return;
    next();
  });
}

void CadmiumPopup::submit() {
  if (!m_generating || m_cancelled || !currentScene() || m_running < 0 ||
      m_running >= int(m_drawings.size())) return;
  Drawing &drawing = m_drawings[m_running];
  if (drawing.target.lines.size() != drawing.reference.lines.size()) {
    drawing.error = tr("Reference and target analysis canvases must have matching dimensions.");
    refresh(); scheduleNext(); return;
  }
  const QString targetLabels = labelUri(drawing.target), refLabels = labelUri(drawing.reference);
  if (targetLabels.isEmpty() || refLabels.isEmpty()) {
    drawing.error = tr("Invalid or incomplete native region labels.");
    refresh(); scheduleNext(); return;
  }
  QJsonArray palette;
  QSet<QRgb> uniqueColors;
  bool hasNullColor = false;
  for (const QColor &color : drawing.reference.regionColors) {
    if (color.isValid()) {
      uniqueColors.insert(color.rgba());
      palette.append(QJsonArray{color.red(), color.green(), color.blue(), color.alpha()});
    } else {
      hasNullColor = true; palette.append(QJsonArray{-100, 0, 0, 0});
    }
  }
  if (uniqueColors.isEmpty() || uniqueColors.size() + int(hasNullColor) > 512) {
    drawing.error = tr("The reference needs between 1 and 512 model palette entries.");
    refresh(); scheduleNext(); return;
  }
  QJsonObject reference{{"seg_map_uri", refLabels},
                        {"line_image_uri", Cadmium::lineImageUri(drawing.reference.lines)},
                        {"colors_rgba", palette}};
  QJsonObject request{{"target_seg_map_uri", targetLabels},
                      {"target_line_image_uri", Cadmium::lineImageUri(drawing.target.lines)},
                      {"references", QJsonArray{reference}},
                      {"return_colorized", false}, {"verbose", false}};
  refresh();
  m_status->setText(tr("Processing drawing %1 of %2…").arg(m_running + 1).arg(m_drawings.size()));
  CadmiumRuntime::instance()->colorize(request);
}

void CadmiumPopup::received(const QJsonObject &response) {
  if (!m_generating || m_cancelled || m_running < 0 || m_running >= int(m_drawings.size())) return;
  Drawing &drawing = m_drawings[m_running];
  const QJsonArray colors = response.value("target_colors_rgba").toArray();
  const QJsonArray ids = response.value("target_color_ids").toArray();
  if (colors.size() != drawing.target.regionColors.size() || ids.size() != colors.size()) {
    failed(tr("Worker returned the wrong number of region predictions.")); return;
  }
  drawing.styles.fill(-1, colors.size()); drawing.colors.resize(colors.size());
  drawing.nativeColors.fill(Cadmium::NativeColor(), colors.size());
  QVector<QColor> palette;
  for (const QColor &color : drawing.reference.regionColors)
    if (!palette.contains(color)) palette.append(color);
  QHash<QRgb, QVector<int>> nativeChoices;
  for (int i = 0; i < colors.size(); ++i) {
    if (!ids[i].isDouble() || ids[i].toDouble() != ids[i].toInt()) {
      failed(tr("Worker returned a malformed palette index.")); return;
    }
    const QJsonArray rgba = colors[i].toArray();
    if (rgba.size() != 4) { failed(tr("Worker returned malformed region colors.")); return; }
    bool nullColor = true;
    for (const QJsonValue &channel : rgba) if (channel.toInt() != -100) nullColor = false;
    if (ids[i].toInt() == -100 || nullColor) continue;
    const int predictedId = ids[i].toInt();
    if (predictedId < 0 || predictedId >= palette.size()) {
      failed(tr("Worker returned an out-of-range palette index.")); return;
    }
    for (const QJsonValue &channel : rgba)
      if (!channel.isDouble() || channel.toDouble() != channel.toInt() ||
          channel.toInt() < 0 || channel.toInt() > 255) {
        failed(tr("Worker returned a color outside the supported range.")); return;
      }
    const QColor color(rgba[0].toInt(), rgba[1].toInt(), rgba[2].toInt(), rgba[3].toInt());
    if (color != palette[predictedId]) {
      failed(tr("Worker palette indices and predicted colors do not agree.")); return;
    }
    drawing.colors[i] = color;
    if (drawing.target.kind == Cadmium::Kind::FullColor) {
      if (!nativeChoices.contains(color.rgba()))
        nativeChoices.insert(color.rgba(), Cadmium::matchingNativeRegions(drawing.reference, color));
      const QVector<int> &candidates = nativeChoices[color.rgba()];
      if (candidates.size() == 1) {
        drawing.styles[i] = candidates.front();
        drawing.nativeColors[i] = drawing.reference.nativeRegionColors[candidates.front()];
      }
      continue;
    }
    QSet<int> styles;
    for (int region = 0; region < drawing.reference.regionColors.size(); ++region)
      if (drawing.reference.regionColors[region] == color &&
          region < drawing.reference.regionStyles.size() && drawing.reference.regionStyles[region] >= 0)
        styles.insert(drawing.reference.regionStyles[region]);
    if (styles.size() == 1) drawing.styles[i] = *styles.begin();
  }
  drawing.ready = true;
  drawing.reference.original = TImageP();
  drawing.reference.lineSource = TImageP();
  drawing.reference.lines = QImage();
  drawing.reference.labels.clear();
  refresh(); refreshPreview();
  scheduleNext();
}

void CadmiumPopup::failed(const QString &error) {
  if (!m_generating || m_running < 0 || m_running >= int(m_drawings.size())) return;
  m_drawings[m_running].error = error;
  m_drawings[m_running].ready = false;
  if (!CadmiumRuntime::instance()->isReady()) {
    m_generating = false;
    m_running = -1;
    m_status->setText(error);
    refresh();
    return;
  }
  refresh();
  scheduleNext();
}

void CadmiumPopup::refresh() {
  bool ready = false;
  for (int i = 0; i < int(m_drawings.size()); ++i) {
    const Drawing &drawing = m_drawings[i];
    ready |= drawing.ready && !drawing.applied;
    const QString state = drawing.applied ? tr("Applied") : !drawing.error.isEmpty() ? tr("Failed") :
        drawing.ready ? (drawing.reviewed ? tr("Reviewed") : tr("Review")) : m_generating && i == m_running ? tr("Processing") : tr("Queued");
    m_list->item(i)->setText(tr("%1 — %2").arg(frameName(drawing.fid), state));
    m_list->item(i)->setToolTip(drawing.error);
  }
  m_generate->setEnabled(!m_generating && !m_backgroundTasks && !m_drawings.empty() && !m_references.empty());
  m_apply->setEnabled(!m_generating && ready);
  m_applyAll->setEnabled(!m_generating && ready);
  m_review->setEnabled(!m_generating && ready);
  m_cancel->setEnabled(m_generating);
  m_reference->setEnabled(!m_generating);
  m_rasterSource->setEnabled(!m_generating);
  m_lineLevel->setEnabled(!m_generating);
  m_extraction->setEnabled(!m_generating);
  m_paperThreshold->setEnabled(!m_generating);
  m_lineThreshold->setEnabled(!m_generating);
  m_repaint->setEnabled(!m_generating);
  m_regions->setEnabled(!m_generating);
}

void CadmiumPopup::refreshPreview() {
  m_regions->setRowCount(0); m_before->clear(); m_after->clear();
  const int index = m_list->currentRow();
  if (index < 0 || index >= int(m_drawings.size())) return;
  Drawing &drawing = m_drawings[index];
  const auto found = std::find(m_references.begin(), m_references.end(), drawing.referenceId);
  m_reference->setCurrentIndex(found == m_references.end() ? -1 : int(found - m_references.begin()));
  if (drawing.target.lines.isNull()) {
    if (m_level && m_level->getType() != PLI_XSHLEVEL) {
      if (m_backgroundTasks) { m_after->setText(tr("Analyzing line mask…")); return; }
      auto *task = new CadmiumRasterTask;
      QString error;
      m_inspecting = true;
      const bool captured = snapshot(drawing.fid, task->target, error, true);
      m_inspecting = false;
      if (!captured) { delete task; m_before->setText(error); return; }
      task->single = true;
      const quint64 epoch = m_snapshotEpoch;
      ++m_backgroundTasks;
      connect(task, &QThread::finished, this, [this, task, epoch, index] {
        --m_backgroundTasks;
        if (!currentScene() || !isVisible()) { refresh(); return; }
        if (epoch != m_snapshotEpoch || index != m_list->currentRow()) {
          refresh(); refreshPreview(); return;
        }
        if (task->success) {
          m_before->setPixmap(QPixmap::fromImage(task->before));
          m_after->setPixmap(QPixmap::fromImage(task->target.lines).scaled(320, 260, Qt::KeepAspectRatio, Qt::SmoothTransformation));
          m_after->setToolTip(tr("Extracted line mask for inspection"));
        } else m_before->setText(task->error);
        refresh();
      });
      connect(task, &QThread::finished, task, &QObject::deleteLater);
      connect(qApp, &QCoreApplication::aboutToQuit, task, [task] { task->wait(); });
      m_after->setText(tr("Analyzing line mask…"));
      refresh(); task->start(); return;
    }
    Cadmium::Frame probe; QString error;
    m_inspecting = true;
    const bool inspected = snapshot(drawing.fid, probe, error);
    m_inspecting = false;
    if (inspected) {
      m_before->setPixmap(QPixmap::fromImage(preview(probe, probe.regionColors)).scaled(320, 260, Qt::KeepAspectRatio, Qt::SmoothTransformation));
      m_after->setPixmap(QPixmap::fromImage(probe.lines).scaled(320, 260, Qt::KeepAspectRatio, Qt::SmoothTransformation));
      m_after->setToolTip(tr("Extracted line mask for inspection"));
    }
    else m_before->setText(error);
    return;
  }
  m_before->setPixmap(QPixmap::fromImage(displayImage(drawing.target.original, drawing.target)).scaled(320, 260, Qt::KeepAspectRatio, Qt::SmoothTransformation));
  if (!drawing.ready) { m_after->setText(drawing.error); return; }
  updateProposalPreview();
  if (!drawing.target.warnings.isEmpty())
    m_status->setText(drawing.target.warnings.join(QStringLiteral("\n")));
  const bool fullColor = drawing.target.kind == Cadmium::Kind::FullColor;
  QHash<QRgb, QVector<int>> nativeChoices;
  m_regions->setRowCount(drawing.colors.size());
  for (int region = 0; region < drawing.colors.size(); ++region) {
    m_regions->setItem(region, 0, new QTableWidgetItem(QString::number(region)));
    const QColor color = drawing.colors[region];
    auto *prediction = new QTableWidgetItem(color.isValid() ? color.name(QColor::HexArgb) : tr("Unmatched"));
    if (color.isValid()) prediction->setBackground(color);
    m_regions->setItem(region, 1, prediction);
    auto *decision = new QComboBox(m_regions);
    decision->addItem(tr("Unresolved"), -1);
    decision->addItem(tr("Skip region"), -2);
    if (fullColor) {
      if (color.isValid() && !nativeChoices.contains(color.rgba()))
        nativeChoices.insert(color.rgba(), Cadmium::matchingNativeRegions(drawing.reference, color));
      const QVector<int> candidates = color.isValid() ? nativeChoices.value(color.rgba()) : QVector<int>();
      for (int candidate : candidates) {
        const Cadmium::NativeColor &native = drawing.reference.nativeRegionColors[candidate];
        decision->addItem(tr("Reference fill %1: %2, %3, %4; alpha %5")
            .arg(candidate).arg(native.r, 0, 'g', 9).arg(native.g, 0, 'g', 9)
            .arg(native.b, 0, 'g', 9).arg(native.a, 0, 'g', 9), candidate);
      }
      if (color.isValid() && candidates.isEmpty())
        prediction->setToolTip(tr("No exact flat native reference fill is available. Skip this region or use another reference."));
      else if (candidates.size() > 1)
        prediction->setToolTip(tr("Different native reference fills share this model color. Choose the exact reference fill or skip the region."));
    } else if (drawing.target.palette) {
      for (int style = 0; style < drawing.target.palette->getStyleCount(); ++style)
        decision->addItem(tr("Style %1").arg(style), style);
    }
    decision->setCurrentIndex(decision->findData(drawing.styles.value(region, -1)));
    m_regions->setCellWidget(region, 2, decision);
    if (region == 0) {
      decision->setEnabled(false);
      decision->setToolTip(tr("Background and line pixels are protected."));
    }
    connect(decision, QOverload<int>::of(&QComboBox::activated), this,
            [this, index, region, decision, fullColor] {
      if (m_generating || index < 0 || index >= int(m_drawings.size())) return;
      Drawing &selected = m_drawings[index];
      if (!selected.ready || region < 0 || region >= selected.styles.size() ||
          region >= selected.colors.size()) return;
      const int style = decision->currentData().toInt();
      selected.styles[region] = style;
      selected.reviewed = false;
      if (fullColor) {
        selected.nativeColors[region] = style >= 0 &&
            style < selected.reference.nativeRegionColors.size()
            ? selected.reference.nativeRegionColors[style] : Cadmium::NativeColor();
      } else if (selected.target.palette && style >= 0) {
        const TPixel32 pixel = selected.target.palette->getStyle(style)->getMainColor();
        selected.colors[region] = QColor(pixel.r, pixel.g, pixel.b, pixel.m);
      }
      updateProposalPreview(); refresh();
    });
  }
}

void CadmiumPopup::updateProposalPreview() {
  const int row = m_list->currentRow();
  if (row < 0 || row >= int(m_drawings.size())) return;
  const Drawing &drawing = m_drawings[row];
  if (!drawing.ready) return;
  QVector<QColor> colors = drawing.colors;
  for (int region = 0; region < colors.size(); ++region)
    if (drawing.styles.value(region, -1) < 0) colors[region] = QColor();
  const QVector<int> styles = acceptedStyles(drawing.styles);
  QString error;
  TImageP result;
  switch (drawing.target.kind) {
  case Cadmium::Kind::ToonzRaster:
    result = Cadmium::applyToonzRaster(drawing.target, styles, colors, m_repaint->isChecked(), error); break;
  case Cadmium::Kind::Vector:
    result = Cadmium::applyVector(drawing.target, styles, colors, m_repaint->isChecked(), error); break;
  case Cadmium::Kind::FullColor:
    result = Cadmium::applyFullColor(drawing.target, styles, colors, m_repaint->isChecked(), error, drawing.nativeColors); break;
  }
  if (!result) { m_after->setText(error); return; }
  m_after->setPixmap(QPixmap::fromImage(displayImage(result, drawing.target)).scaled(320, 260, Qt::KeepAspectRatio, Qt::SmoothTransformation));
}

void CadmiumPopup::assignReference() {
  const int row = m_list->currentRow(), reference = m_reference->currentIndex();
  if (m_generating || row < 0 || row >= int(m_drawings.size()) ||
      reference < 0 || reference >= int(m_references.size())) return;
  Drawing &drawing = m_drawings[row];
  drawing.referenceId = m_references[reference]; drawing.ready = false;
  drawing.colors.clear(); drawing.styles.clear(); drawing.nativeColors.clear();
  refresh(); refreshPreview();
}

void CadmiumPopup::apply(bool all) {
  if (m_generating || !currentScene() || !Cadmium::canPaintLevel(m_level.getPointer())) return;
  std::map<TFrameId, TImageP> before, after;
  QString error;
  std::vector<int> accepted;
  for (int i = 0; i < int(m_drawings.size()); ++i) {
    Drawing &drawing = m_drawings[i];
    if ((!all && i != m_list->currentRow()) || (all && !drawing.reviewed) || !drawing.ready || drawing.applied) continue;
    if ((m_level->getType() != OVL_XSHLEVEL && m_level->isFrameReadOnly(drawing.fid)) ||
        m_level->getCadmiumReferences().count(drawing.fid) ||
        !m_level->getCadmiumReferences().count(drawing.referenceId)) {
      error = tr("A target became read-only or its reference assignment changed."); break;
    }
    if (m_level->getRenumberCount() != drawing.renumberCount ||
        !drawing.targetBuilder || !drawing.referenceBuilder ||
        ImageManager::instance()->getBuilder(m_level->getImageId(drawing.fid)) != drawing.targetBuilder.getPointer() ||
        ImageManager::instance()->getBuilder(m_level->getImageId(drawing.referenceId)) != drawing.referenceBuilder.getPointer()) {
      error = tr("A drawing was replaced or renumbered. Generate a new proposal before applying."); break;
    }
    Cadmium::Frame targetNow, referenceNow;
    auto fresh = [this, &error](const TFrameId &fid, const Cadmium::Frame &saved,
                                Cadmium::Frame &now) {
      if (m_level->getType() == PLI_XSHLEVEL) {
        return snapshot(fid, now, error) && now.fingerprint == saved.fingerprint;
      }
      if (!snapshot(fid, now, error, true)) return false;
      if (saved.kind == Cadmium::Kind::ToonzRaster)
        return Cadmium::matchesToonzRaster(now.original, saved, error);
      return Cadmium::matchesFullColor(now.original, now, saved, error);
    };
    if (!fresh(drawing.fid, drawing.target, targetNow) ||
        !fresh(drawing.referenceId, drawing.reference, referenceNow)) {
      if (error.isEmpty())
        error = tr("A drawing, palette, or input changed. Generate a new proposal before applying.");
      break;
    }
    QVector<QColor> colors = drawing.colors;
    for (int r = 1; r < colors.size(); ++r) {
      if (colors[r].isValid() && drawing.styles.value(r, -1) == -1) {
        error = tr("Resolve or explicitly skip ambiguous regions before applying."); break;
      }
    }
    if (!error.isEmpty()) break;
    for (int r = 0; r < colors.size(); ++r)
      if (drawing.styles.value(r, -1) == -2 ||
          (drawing.target.kind != Cadmium::Kind::FullColor && drawing.styles.value(r, -1) < 0))
        colors[r] = QColor();
    TImageP result;
    const QVector<int> styles = acceptedStyles(drawing.styles);
    switch (drawing.target.kind) {
    case Cadmium::Kind::ToonzRaster:
      result = Cadmium::applyToonzRaster(drawing.target, styles, colors, m_repaint->isChecked(), error); break;
    case Cadmium::Kind::Vector:
      result = Cadmium::applyVector(drawing.target, styles, colors, m_repaint->isChecked(), error); break;
    case Cadmium::Kind::FullColor:
      result = Cadmium::applyFullColor(drawing.target, styles, colors, m_repaint->isChecked(), error, drawing.nativeColors); break;
    }
    if (!result) { if (error.isEmpty()) error = tr("The proposal could not be applied."); break; }
    before[drawing.fid] = targetNow.original;
    after[drawing.fid] = result;
    accepted.push_back(i);
  }
  if (!error.isEmpty()) {
    QMessageBox::warning(this, tr("Cadmium"), error); return;
  }
  if (after.empty()) return;
  TXshSimpleLevelP destination = m_level;
  const bool sourceWasDirty = m_level->getDirtyFlag();
  const bool newLevel = m_level->getType() == OVL_XSHLEVEL;
  TXshLevel *created = nullptr;
  try {
  if (newLevel) {
    created = Cadmium::createResultLevel(m_scene, m_level.getPointer(), error);
    if (!created || !created->getSimpleLevel()) {
      if (created) m_scene->getLevelSet()->removeLevel(created);
      QMessageBox::warning(this, tr("Cadmium"), error.isEmpty() ? tr("Could not create the result level.") : error); return;
    }
    destination = created->getSimpleLevel();
    destination->setCadmiumSourceLevelId(m_level->cadmiumLevelId());
    destination->set16BitChannelLevel(m_level->is16BitChannelLevel());
    destination->setFloatChannelLevel(m_level->isFloatChannelLevel());
    destination->getProperties()->options() = m_level->getProperties()->options();
    destination->getProperties()->setIsStopMotion(false);
    destination->getProperties()->setImageRes(m_level->getProperties()->getImageRes());
    destination->getProperties()->setImageDpi(m_level->getProperties()->getImageDpi());
    destination->getProperties()->setBpp(m_level->getProperties()->getBpp());
  }
    auto undo = std::unique_ptr<CadmiumPaintUndo>(new CadmiumPaintUndo(
        destination.getPointer(), before, after, newLevel, m_level.getPointer(), m_scene));
    notifyDrawings(destination.getPointer(), after);
    TUndoManager::manager()->add(undo.release());
  } catch (...) {
    if (newLevel) {
      if (created) m_scene->getLevelSet()->removeLevel(created);
    }
    else {
      for (const auto &entry : before) {
        destination->setFrame(entry.first, entry.second);
        IconGenerator::instance()->invalidate(destination.getPointer(), entry.first);
      }
      destination->setDirtyFlag(sourceWasDirty);
      TApp::instance()->getCurrentLevel()->notifyLevelChange();
    }
    QMessageBox::warning(this, tr("Cadmium"),
      tr("The paint batch could not be committed. Its drawings were restored."));
    return;
  }
  for (int index : accepted) m_drawings[index].applied = true;
  refresh();
  m_status->setText(newLevel
      ? tr("Created result level %1. Original drawings were preserved.").arg(QString::fromStdWString(destination->getName()))
      : tr("Applied %1 drawings. Undo restores their original paint.").arg(after.size()));
}

void CadmiumPopup::cancel() {
  ++m_snapshotEpoch;
  m_cancelled = true;
  if (m_generating) CadmiumRuntime::instance()->stop();
  m_generating = false; m_running = -1;
  refresh();
  m_status->setText(tr("Processing stopped. Accepted drawings are unchanged."));
}

void CadmiumPopup::closeEvent(QCloseEvent *event) {
  if (m_generating) cancel();
  QDialog::closeEvent(event);
}
