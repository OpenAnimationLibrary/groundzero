#include "cadmiumpopup.h"
#include "cadmiumruntime.h"
#include "cadmiumsource.h"
#include "cadmiumdestination.h"
#include "cadmiumcodec.h"
#include "cadmiumcolor.h"
#include "cadmiumreviewview.h"
#include <QFileInfo>
#include <QStandardPaths>
#include <QSignalBlocker>
#include <QItemSelectionModel>
#include <QCryptographicHash>
#include <QDir>
#include "trop.h"
#include "tropcm.h"
#include <cmath>
#include "tapp.h"
#include "tundo.h"
#include "ttoonzimage.h"
#include "trasterimage.h"
#include "tcolorstyles.h"
#include "tsimplecolorstyles.h"
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

QByteArray stableSourceKey(ToonzScene *scene, TXshSimpleLevel *level) {
  QString resource = QDir::cleanPath(QFileInfo(scene->decodeFilePath(level->getPath()).getQString()).absoluteFilePath());
#ifdef Q_OS_WIN
  resource = resource.toCaseFolded();
#endif
  return QCryptographicHash::hash((resource + QChar(0) +
      QString::fromStdWString(level->getName())).toUtf8(), QCryptographicHash::Sha256);
}

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

Cadmium::RegionProposal manualDecision(const Cadmium::RegionProposal &previous,
                                       const Cadmium::Frame &target) {
  Cadmium::RegionProposal result;
  result.protectedRegion = previous.protectedRegion;
  if (!previous.locked) return result;
  if (previous.selected == -2) {
    result.selected = -2; result.locked = true; return result;
  }
  if (previous.selected < 0 || previous.selected >= previous.candidates.size()) return result;
  Cadmium::ProposalCandidate candidate = previous.candidates[previous.selected];
  bool valid = target.kind == Cadmium::Kind::FullColor;
  if (!valid && target.palette && candidate.style >= 0 && candidate.style < target.palette->getStyleCount()) {
    const TPixel32 color = target.palette->getStyle(candidate.style)->getMainColor();
    valid = candidate.color == QColor(color.r, color.g, color.b, color.m);
  }
  if (!valid) return result;
  candidate.referenceIndex = -1; candidate.referenceRegion = -1; candidate.supportingReferences.clear();
  result.candidates.append(candidate); result.selected = 0; result.locked = true;
  return result;
}

QImage displayImage(const TImageP &, const Cadmium::Frame &);

class CadmiumRasterTask final : public QThread {
public:
  Cadmium::Frame target;
  QVector<Cadmium::Frame> references;
  QVector<QImage> previews;
  QString error;
  bool success = false;
  void run() override {
    try {
      auto prepare = [this](Cadmium::Frame &frame) {
        const TImageP image = frame.original;
        return frame.kind == Cadmium::Kind::ToonzRaster
            ? Cadmium::snapshotToonzRaster(image, frame, error)
            : Cadmium::snapshotFullColor(image, frame, error);
      };
      if (!prepare(target)) return;
      for (Cadmium::Frame &reference : references) {
        if (!prepare(reference)) return;
        previews.append(displayImage(reference.original, reference));
      }
      success = true;
    } catch (...) {
      error = QObject::tr("The drawing analysis could not be completed. Try a smaller batch.");
    }
  }
};

class CadmiumSessionTask final : public QThread {
public:
  QString cacheRoot, scene, error;
  QByteArray sourceKey;
  Cadmium::Session session;
  bool success = false;
  void run() override {
    success = Cadmium::saveSessionWithAlias(cacheRoot, scene, sourceKey, session, error);
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
  if (!popup->m_generating && !popup->m_restoring)
    popup->initialize(level, selection);
  popup->show(); popup->raise(); popup->activateWindow();
}

QImage CadmiumPopup::imagePreview(const TImageP &image, const Cadmium::Frame &frame) {
  return displayImage(image, frame);
}

bool CadmiumPopup::currentScene() const {
  return m_scene && TApp::instance()->getCurrentScene()->getScene() == m_scene &&
      m_level && m_scene->getLevelSet()->getLevel(m_level->getName()) == m_level.getPointer();
}

void CadmiumPopup::initialize(TXshSimpleLevel *level,
                              const std::set<TFrameId> &selection) {
  if (m_sessionDirty) flushSession();
  m_saveTimer.stop();
  ++m_snapshotEpoch;
  m_refreshing = true;
  m_level = level;
  m_scene = TApp::instance()->getCurrentScene()->getScene();
  m_selectionRenumberCount = level ? level->getRenumberCount() : -1;
  m_drawings.clear(); m_references.clear(); m_jobs.clear(); m_recovery.clear();
  m_savedSession = Cadmium::Session();
  m_pendingSession = Cadmium::Session(); m_pendingScene.clear(); ++m_sessionRevision;
  m_list->clear(); m_referenceList->clear(); m_lineLevel->clear();
  m_running = -1; m_jobPosition = -1; m_runningReference = 0;
  m_generating = false; m_restoring = false; m_cancelled = false;
  m_sessionDirty = false;
  if (!level || !currentScene()) {
    m_refreshing = false; refresh(); return;
  }
  m_level->cadmiumLevelId();
  TApp::instance()->getCurrentScene()->setDirtyFlag(true);
  m_sessionScene = sceneIdentity();
  m_sessionSourceKey = stableSourceKey(m_scene, level);
  std::vector<TFrameId> ordered;
  level->getFids(ordered);
  for (const TFrameId &fid : ordered)
    if (level->getCadmiumReferences().count(fid)) m_references.push_back(fid);
  m_lineLevel->addItem(tr("Choose a clean-line level"), QVariant::fromValue<qulonglong>(0));
  std::vector<TXshLevel *> levels;
  m_scene->getLevelSet()->listLevels(levels);
  for (TXshLevel *candidate : levels)
    if (candidate != level && candidate->getSimpleLevel() && candidate->getType() == OVL_XSHLEVEL)
      m_lineLevel->addItem(QString::fromStdWString(candidate->getName()),
          QVariant::fromValue<qulonglong>(reinterpret_cast<qulonglong>(candidate)));
  for (const TFrameId &fid : selection) {
    if (level->getCadmiumReferences().count(fid) || !level->isFid(fid)) continue;
    Drawing drawing; drawing.fid = fid;
    int bestDistance = INT_MAX;
    for (const TFrameId &reference : m_references) {
      const int distance = std::abs(level->fid2index(fid) - level->fid2index(reference));
      if (distance < bestDistance) {
        drawing.referenceIds = QVector<TFrameId>{reference}; bestDistance = distance;
      }
    }
    m_drawings.push_back(drawing); m_list->addItem(frameName(fid));
  }
  m_extraction->setChecked(false); m_repaint->setChecked(false);
  if (!m_drawings.empty()) {
    m_list->selectAll(); m_list->setCurrentRow(0, QItemSelectionModel::NoUpdate);
  }
  m_refreshing = false;
  refresh();
  m_status->setText(m_references.empty()
      ? tr("Mark at least one painted reference drawing in the Level Strip.")
      : tr("Choose references, generate proposals, and review native region decisions."));
  if (!sceneIdentity().isEmpty() &&
      (QFileInfo::exists(Cadmium::sessionFilePath(sessionCacheRoot(), sceneIdentity(), level->cadmiumLevelId())) ||
       QFileInfo::exists(Cadmium::sessionAliasPath(sessionCacheRoot(), sceneIdentity(), m_sessionSourceKey))))
    QTimer::singleShot(0, this, [this, epoch = m_snapshotEpoch] {
      if (epoch == m_snapshotEpoch && currentScene() && !m_generating && !m_backgroundTasks) {
        restoreSession();
        if (!m_restoring) refreshPreview();
      }
    });
  else refreshPreview();
}

void CadmiumPopup::optionsChanged() {
  if (m_refreshing || m_generating || m_restoring) return;
  ++m_snapshotEpoch;
  for (Drawing &drawing : m_drawings) {
    drawing.ready = false; drawing.reviewed = false;
    drawing.target = Cadmium::Frame(); drawing.proposals.clear();
    drawing.labelsFingerprint.clear();
    drawing.references.clear(); drawing.referencePreviews.clear();
    drawing.selectedRegions.clear();
  }
  autosave(); refresh(); refreshPreview();
  m_status->setText(tr("Input settings changed. Inspect the new line mask and regenerate decisions before applying."));
}

void CadmiumPopup::assignReferences() {
  if (m_refreshing || m_generating || m_restoring) return;
  const int row = m_list->currentRow();
  if (row < 0 || row >= int(m_drawings.size())) return;
  QVector<TFrameId> references;
  for (int i = 0; i < m_referenceList->count() && i < int(m_references.size()); ++i)
    if (m_referenceList->item(i)->checkState() == Qt::Checked)
      references.append(m_references[i]);
  if (references.size() > Cadmium::MaximumProposalReferences) {
    m_status->setText(tr("Choose at most %1 references per drawing.").arg(Cadmium::MaximumProposalReferences));
    refreshPreview(); return;
  }
  Drawing &drawing = m_drawings[row];
  if (references.size() == drawing.referenceIds.size() &&
      std::equal(references.cbegin(), references.cend(), drawing.referenceIds.cbegin())) return;
  drawing.referenceIds = references;
  drawing.references.clear(); drawing.referenceBuilders.clear(); drawing.referencePreviews.clear();
  // Reference indices in candidates are local to the checked reference list.
  // A changed assignment cannot reuse old provenance.
  for (int region = 1; region < drawing.proposals.size(); ++region)
    drawing.proposals[region] = manualDecision(drawing.proposals[region], drawing.target);
  if (!drawing.proposals.isEmpty()) drawing.proposals[0].selected = -2;
  drawing.ready = false; drawing.reviewed = false; drawing.error.clear();
  autosave(); refresh(); refreshPreview();
}

void CadmiumPopup::inspectDrawing() {
  const int row = m_list->currentRow();
  if (m_generating || m_restoring || m_backgroundTasks || !currentScene() ||
      row < 0 || row >= int(m_drawings.size()) || !m_drawings[row].target.lines.isNull()) return;
  auto *task = new CadmiumRasterTask;
  QString error;
  m_inspecting = true;
  const bool captured = snapshot(m_drawings[row].fid, task->target, error,
                                 m_level->getType() != PLI_XSHLEVEL);
  m_inspecting = false;
  if (!captured) { delete task; m_status->setText(error); return; }
  const quint64 epoch = m_snapshotEpoch;
  if (m_level->getType() == PLI_XSHLEVEL) {
    m_drawings[row].target = task->target; delete task;
    m_drawings[row].labelsFingerprint = Cadmium::sessionLabelsDigest(m_drawings[row].target);
    m_drawings[row].proposals.resize(m_drawings[row].target.regionColors.size());
    if (!m_drawings[row].proposals.isEmpty()) {
      m_drawings[row].proposals[0].protectedRegion = true;
      m_drawings[row].proposals[0].selected = -2;
    }
    refreshPreview(); return;
  }
  ++m_backgroundTasks;
  connect(task, &QThread::finished, this, [this, task, row, epoch] {
    --m_backgroundTasks;
    if (epoch != m_snapshotEpoch || !currentScene() || row >= int(m_drawings.size())) {
      refresh();
      if (currentScene() && isVisible() && !m_cancelled) refreshPreview();
      return;
    }
    if (task->success) {
      m_drawings[row].target = task->target;
      m_drawings[row].labelsFingerprint = Cadmium::sessionLabelsDigest(task->target);
      m_drawings[row].proposals.resize(task->target.regionColors.size());
      if (!m_drawings[row].proposals.isEmpty()) {
        m_drawings[row].proposals[0].protectedRegion = true;
        m_drawings[row].proposals[0].selected = -2;
      }
    } else m_drawings[row].error = task->error;
    refresh();
    if (row == m_list->currentRow()) {
      if (task->success) refreshPreview(); else m_view->clear(task->error);
    }
  });
  connect(task, &QThread::finished, task, &QObject::deleteLater);
  connect(qApp, &QCoreApplication::aboutToQuit, task, [task] { task->wait(); });
  task->start();
}

void CadmiumPopup::generate() {
  if (m_generating || m_restoring || m_backgroundTasks || !currentScene() || m_drawings.empty()) return;
  if (m_selectionRenumberCount != m_level->getRenumberCount()) {
    QMessageBox::warning(this, tr("Cadmium"), tr("The drawings were renumbered. Reopen Assisted Paint with the intended selection.")); return;
  }
  if (CadmiumRuntime::instance()->startupMode() == CadmiumRuntime::Off || CadmiumRuntime::instance()->busy()) {
    QMessageBox::information(this, tr("Cadmium"), tr("Enable an available Cadmium worker in Preferences before generating.")); return;
  }
  if (m_level->getType() == OVL_XSHLEVEL &&
      m_rasterSource->currentIndex() == int(Cadmium::RasterSource::PaperScan) && !m_extraction->isChecked()) {
    QMessageBox::information(this, tr("Cadmium"), tr("Inspect the extracted line mask and approve extraction before generating.")); return;
  }
  m_generation = static_cast<Cadmium::RegenerationMode>(m_generationMode->currentData().toInt());
  m_jobs.clear();
  const bool currentOnly = m_generation == Cadmium::RegenerationMode::CurrentDrawing ||
                           m_generation == Cadmium::RegenerationMode::SelectedRegions;
  for (int i = 0; i < int(m_drawings.size()); ++i) {
    if (m_drawings[i].applied) continue;
    if (currentOnly ? i != m_list->currentRow() :
        (!m_list->selectedItems().isEmpty() && !m_list->item(i)->isSelected())) continue;
    if (m_drawings[i].referenceIds.isEmpty()) {
      m_status->setText(tr("Choose at least one reference for every drawing to generate.")); return;
    }
    m_jobs.append(i);
  }
  if (m_jobs.isEmpty()) { m_status->setText(tr("Select an unapplied drawing to regenerate.")); return; }
  ++m_snapshotEpoch; m_cancelled = false; m_generating = true; m_jobPosition = -1;
  m_running = -1; m_runningReference = 0;
  for (int index : m_jobs) m_drawings[index].error.clear();
  next();
}

void CadmiumPopup::next() {
  if (!m_generating || m_cancelled || !currentScene()) { if (m_generating) cancel(); return; }
  if (++m_jobPosition >= m_jobs.size()) {
    m_running = -1; m_generating = false;
    autosave(); refresh(); refreshPreview();
    m_status->setText(tr("Generation finished. Review agreements, conflicts, and protected regions before applying."));
    return;
  }
  m_running = m_jobs[m_jobPosition];
  prepareDrawing(m_running, false);
}

void CadmiumPopup::scheduleNext() {
  const quint64 epoch = m_snapshotEpoch;
  QTimer::singleShot(0, this, [this, epoch] {
    if (epoch == m_snapshotEpoch && m_generating && !m_cancelled) next();
  });
}

void CadmiumPopup::prepareDrawing(int index, bool restoring) {
  if (!currentScene() || index < 0 || index >= int(m_drawings.size())) return;
  Drawing &drawing = m_drawings[index];
  QString error;
  if (m_level->getRenumberCount() != m_selectionRenumberCount) {
    error = tr("The drawings were renumbered. Reopen Assisted Paint before continuing.");
  }
  const qint64 budget = 512LL * 1024 * 1024;
  auto memory = [](const Cadmium::Frame &frame) {
    return TUndo::getImageMemorySize(frame.original.getPointer()) +
        TUndo::getImageMemorySize(frame.lineSource.getPointer()) + qint64(frame.lines.sizeInBytes()) +
        qint64(frame.labels.size()) * qint64(sizeof(int)) +
        qint64(frame.nativeRegionColors.size()) * qint64(sizeof(Cadmium::NativeColor));
  };
  qint64 bytes = 0;
  for (const Drawing &cached : m_drawings) {
    bytes += memory(cached.target) + Cadmium::proposalMemoryBytes(cached.proposals);
    for (const Cadmium::Frame &reference : cached.references) bytes += memory(reference);
    for (const QImage &image : cached.referencePreviews) bytes += image.sizeInBytes();
  }
  QVector<TFrameId> captureIds = drawing.referenceIds;
  captureIds.prepend(drawing.fid);
  for (const TFrameId &fid : captureIds) {
    if (!m_level->isFid(fid) || (fid != drawing.fid && !m_level->getCadmiumReferences().count(fid))) {
      error = tr("A target or marked reference is no longer available."); break;
    }
    TImageP image = Cadmium::loadDrawing(m_level.getPointer(), fid);
    bytes += std::max<qint64>(16LL * 1024 * 1024, TUndo::getImageMemorySize(image.getPointer()) * 4);
    if (m_level->getType() == OVL_XSHLEVEL &&
        m_rasterSource->currentIndex() == int(Cadmium::RasterSource::LinkedLines)) {
      auto *line = reinterpret_cast<TXshSimpleLevel *>(m_lineLevel->currentData().toULongLong());
      std::vector<TXshLevel *> levels; m_scene->getLevelSet()->listLevels(levels);
      if (line && std::find(levels.begin(), levels.end(), line) != levels.end())
        bytes += TUndo::getImageMemorySize(Cadmium::loadDrawing(line, fid).getPointer()) * 4;
    }
  }
  if (bytes > budget) error = tr("The proposal cache reached its memory budget. Apply or discard results and use a smaller batch or fewer references.");
  if (!error.isEmpty()) {
    drawing.error = error;
    if (restoring) { refresh(); QTimer::singleShot(0, this, &CadmiumPopup::restoreNext); }
    else { refresh(); scheduleNext(); }
    return;
  }
  drawing.pendingRenumberCount = m_level->getRenumberCount();
  drawing.pendingTargetBuilder = ImageManager::instance()->getBuilder(m_level->getImageId(drawing.fid));
  drawing.pendingReferenceBuilders.clear();
  for (const TFrameId &fid : drawing.referenceIds)
    drawing.pendingReferenceBuilders.append(ImageManager::instance()->getBuilder(m_level->getImageId(fid)));
  auto *task = new CadmiumRasterTask;
  const bool raster = m_level->getType() != PLI_XSHLEVEL;
  bool captured = snapshot(drawing.fid, task->target, error, raster);
  if (captured) for (const TFrameId &fid : drawing.referenceIds) {
    Cadmium::Frame reference;
    if (!snapshot(fid, reference, error, raster)) { captured = false; break; }
    task->references.append(reference);
    if (!raster) task->previews.append(imagePreview(reference.original, reference));
  }
  const quint64 epoch = m_snapshotEpoch;
  if (!captured || !raster) {
    analysisFinished(index, task->target, task->references, task->previews,
                     captured ? QString() : error, restoring, epoch);
    delete task; return;
  }
  ++m_backgroundTasks;
  connect(task, &QThread::finished, this, [this, task, index, restoring, epoch] {
    --m_backgroundTasks;
    analysisFinished(index, task->target, task->references, task->previews,
        task->success ? QString() : task->error, restoring, epoch);
  });
  connect(task, &QThread::finished, task, &QObject::deleteLater);
  connect(qApp, &QCoreApplication::aboutToQuit, task, [task] { task->wait(); });
  m_status->setText(restoring ? tr("Validating saved drawing and reference fingerprints…") : tr("Analyzing target and reference regions…"));
  refresh(); task->start();
}

void CadmiumPopup::analysisFinished(int index, Cadmium::Frame target,
    QVector<Cadmium::Frame> references, QVector<QImage> previews, QString error,
    bool restoring, quint64 epoch) {
  if (epoch != m_snapshotEpoch || !currentScene() || m_cancelled ||
      index < 0 || index >= int(m_drawings.size()) ||
      (restoring ? !m_restoring : !m_generating)) { refresh(); return; }
  Drawing &drawing = m_drawings[index];
  if (error.isEmpty())
    for (const Cadmium::Frame &reference : references)
      if (reference.lines.size() != target.lines.size()) {
        error = tr("Reference and target analysis canvases must have matching dimensions."); break;
      }
  if (!error.isEmpty()) {
    drawing.error = error;
    if (restoring) QTimer::singleShot(0, this, &CadmiumPopup::restoreNext);
    else scheduleNext();
    refresh(); return;
  }
  if (restoring) {
    const Cadmium::SessionDrawing saved = m_recovery.value(drawing.fid);
    drawing.target = target; drawing.references = references; drawing.referencePreviews = previews;
    drawing.labelsFingerprint = Cadmium::sessionLabelsDigest(target);
    drawing.targetBuilder = drawing.pendingTargetBuilder;
    drawing.referenceBuilders = drawing.pendingReferenceBuilders;
    drawing.renumberCount = drawing.pendingRenumberCount;
    drawing.selectedRegions.clear();
    if (saved.applied) {
      drawing.applied = true; drawing.ready = false; drawing.reviewed = false;
      drawing.proposals.clear();
      drawing.error = tr("Previously applied session entry. Clear the saved session to start a new proposal after scene undo or output changes.");
    } else if (saved.targetFingerprint.isEmpty()) {
      drawing.proposals.clear(); drawing.proposals.resize(target.regionColors.size());
      if (!drawing.proposals.isEmpty()) {
        drawing.proposals[0].protectedRegion = true; drawing.proposals[0].selected = -2;
      }
      drawing.ready = false; drawing.reviewed = false; drawing.applied = false; drawing.error.clear();
    } else if (Cadmium::validateSessionAgainstFrames(saved, target, references, target.kind, error)) {
      drawing.proposals = saved.regions;
      drawing.ready = saved.ready; drawing.reviewed = saved.ready && saved.reviewed;
      drawing.applied = false; drawing.error.clear();
    } else {
      drawing.proposals.clear(); drawing.proposals.resize(target.regionColors.size());
      if (!drawing.proposals.isEmpty()) {
        drawing.proposals[0].protectedRegion = true; drawing.proposals[0].selected = -2;
      }
      drawing.ready = false; drawing.reviewed = false; drawing.error = error;
    }
    for (Cadmium::Frame &reference : drawing.references) {
      reference.original = TImageP(); reference.lineSource = TImageP();
      reference.labels.clear();
    }
    refresh(); refreshPreview();
    QTimer::singleShot(0, this, &CadmiumPopup::restoreNext);
    return;
  }
  bool sameReferences = drawing.references.size() == references.size();
  if (sameReferences) for (int ref = 0; ref < references.size(); ++ref)
    if (drawing.references[ref].fingerprint != references[ref].fingerprint) sameReferences = false;
  QVector<Cadmium::RegionProposal> previous = drawing.proposals;
  if (!sameReferences) for (int region = 1; region < previous.size(); ++region)
    previous[region] = manualDecision(previous[region], target);
  Cadmium::invalidateIncompleteConsensus(previous, references.size());
  const Cadmium::RegenerationPlan plan = Cadmium::regenerationPlan(
      drawing.target, target, previous, drawing.selectedRegions, m_generation, drawing.applied);
  if (!plan.valid()) { drawing.error = plan.error; refresh(); scheduleNext(); return; }
  drawing.targetBuilder = drawing.pendingTargetBuilder;
  drawing.referenceBuilders = drawing.pendingReferenceBuilders;
  drawing.renumberCount = drawing.pendingRenumberCount;
  drawing.target = target; drawing.references = references; drawing.referencePreviews = previews;
  drawing.labelsFingerprint = Cadmium::sessionLabelsDigest(target);
  drawing.proposals = plan.regions; drawing.requestedRegions = plan.requested;
  QSet<int> mappedSelection;
  for (int selected : drawing.selectedRegions)
    if (plan.previousToCurrent.contains(selected)) mappedSelection.insert(plan.previousToCurrent.value(selected));
  drawing.selectedRegions = mappedSelection;
  drawing.ready = false;
  if (plan.invalidateReview) drawing.reviewed = false;
  drawing.error.clear();
  m_runningReference = 0;
  autosave();
  if (drawing.requestedRegions.isEmpty()) { finishDrawing(); return; }
  submit();
}

void CadmiumPopup::submit() {
  if (!m_generating || m_cancelled || !currentScene() || m_running < 0) return;
  Drawing &drawing = m_drawings[m_running];
  if (m_runningReference >= drawing.references.size()) { finishDrawing(); return; }
  const Cadmium::Frame &reference = drawing.references[m_runningReference];
  const QString targetLabels = Cadmium::labelUri(drawing.target), refLabels = Cadmium::labelUri(reference);
  if (targetLabels.isEmpty() || refLabels.isEmpty()) { failed(tr("The native region labels are incomplete.")); return; }
  QJsonArray palette;
  QSet<QRgb> colors;
  bool nullColor = false;
  for (const QColor &color : reference.regionColors) {
    if (color.isValid()) {
      palette.append(QJsonArray{color.red(), color.green(), color.blue(), color.alpha()}); colors.insert(color.rgba());
    } else { palette.append(QJsonArray{-100, 0, 0, 0}); nullColor = true; }
  }
  if (colors.isEmpty() || colors.size() + int(nullColor) > 512) {
    failed(tr("Every reference needs between 1 and 512 model palette entries.")); return;
  }
  QJsonObject ref{{"seg_map_uri", refLabels}, {"line_image_uri", Cadmium::lineImageUri(reference.lines)}, {"colors_rgba", palette}};
  QJsonObject request{{"target_seg_map_uri", targetLabels},
      {"target_line_image_uri", Cadmium::lineImageUri(drawing.target.lines)},
      {"references", QJsonArray{ref}}, {"return_colorized", false}, {"verbose", false}};
  refresh();
  m_status->setText(tr("Drawing %1 of %2 — reference %3 of %4")
      .arg(m_jobPosition + 1).arg(m_jobs.size()).arg(m_runningReference + 1).arg(drawing.references.size()));
  CadmiumRuntime::instance()->colorize(request);
}

void CadmiumPopup::received(const QJsonObject &response) {
  if (!m_generating || m_cancelled || m_running < 0 || !currentScene()) return;
  Drawing &drawing = m_drawings[m_running];
  if (m_runningReference < 0 || m_runningReference >= drawing.references.size()) return;
  QString error;
  QVector<Cadmium::RegionProposal> incoming;
  if (!Cadmium::parseReferenceProposals(response, drawing.references[m_runningReference],
      drawing.target, m_runningReference, incoming, error)) { failed(error); return; }
  QVector<Cadmium::RegionProposal> merged = drawing.proposals;
  // Unrequested automatic regions already contain prior reference votes. Shield
  // them during this merge and restore their exact state afterwards.
  for (int region = 1; region < merged.size(); ++region)
    if (!drawing.requestedRegions.contains(region)) merged[region].protectedRegion = true;
  if (!Cadmium::mergeReferenceProposals(merged, incoming, drawing.target.kind, error)) { failed(error); return; }
  for (int region = 1; region < merged.size(); ++region)
    if (!drawing.requestedRegions.contains(region)) merged[region] = drawing.proposals[region];
  drawing.proposals = merged;
  ++m_runningReference;
  autosave();
  if (m_runningReference == drawing.references.size()) finishDrawing(); else submit();
}

void CadmiumPopup::finishDrawing() {
  if (m_running < 0 || m_running >= int(m_drawings.size())) return;
  Drawing &drawing = m_drawings[m_running];
  Cadmium::invalidateIncompleteConsensus(drawing.proposals, drawing.references.size());
  drawing.ready = true; drawing.error.clear();
  for (Cadmium::Frame &reference : drawing.references) {
    reference.original = TImageP(); reference.lineSource = TImageP();
    reference.labels.clear();
  }
  autosave(); refresh(); refreshPreview(); scheduleNext();
}

void CadmiumPopup::failed(const QString &error) {
  if (!m_generating || m_running < 0 || m_running >= int(m_drawings.size())) return;
  m_drawings[m_running].error = error; m_drawings[m_running].ready = false;
  autosave();
  if (!CadmiumRuntime::instance()->isReady()) {
    m_generating = false; m_running = -1; m_status->setText(error); refresh(); return;
  }
  refresh(); scheduleNext();
}

TImageP CadmiumPopup::composeProposal(const Drawing &drawing, QString &error) const {
  if (!Cadmium::validateProposals(drawing.proposals, drawing.target.kind, error)) return TImageP();
  QVector<int> styles(drawing.proposals.size(), -1);
  QVector<QColor> colors(drawing.proposals.size());
  QVector<Cadmium::NativeColor> native(drawing.proposals.size());
  for (int region = 1; region < drawing.proposals.size(); ++region) {
    const Cadmium::RegionProposal &proposal = drawing.proposals[region];
    if (proposal.protectedRegion || proposal.selected < 0) continue;
    const Cadmium::ProposalCandidate &candidate = proposal.candidates[proposal.selected];
    styles[region] = candidate.style; colors[region] = candidate.color; native[region] = candidate.nativeColor;
  }
  switch (drawing.target.kind) {
  case Cadmium::Kind::ToonzRaster: return Cadmium::applyToonzRaster(drawing.target, styles, colors, m_repaint->isChecked(), error);
  case Cadmium::Kind::Vector: return Cadmium::applyVector(drawing.target, styles, colors, m_repaint->isChecked(), error);
  case Cadmium::Kind::FullColor: return Cadmium::applyFullColor(drawing.target, styles, colors, m_repaint->isChecked(), error, native);
  }
  error = tr("The drawing format is unsupported."); return TImageP();
}

void CadmiumPopup::setRegionDecision(int region, int candidate) {
  const int row = m_list->currentRow();
  if (m_generating || m_restoring || row < 0 || row >= int(m_drawings.size())) return;
  Drawing &drawing = m_drawings[row];
  if (!drawing.ready || region <= 0 || region >= drawing.proposals.size()) return;
  if (!Cadmium::chooseProposal(drawing.proposals[region], candidate)) return;
  drawing.reviewed = false; autosave(); refresh(); refreshPreview();
}

void CadmiumPopup::setRegionProtected(int region, bool protect) {
  setRegionsProtected(QSet<int>{region}, protect);
}

void CadmiumPopup::setRegionLocked(int region, bool lock) {
  const int row = m_list->currentRow();
  if (m_generating || m_restoring || row < 0 || row >= int(m_drawings.size())) return;
  Drawing &drawing = m_drawings[row];
  if (region <= 0 || region >= drawing.proposals.size()) return;
  Cadmium::RegionProposal &proposal = drawing.proposals[region];
  if (lock && proposal.selected == -1) return;
  proposal.locked = lock; drawing.reviewed = false;
  autosave(); refresh(); refreshPreview();
}

void CadmiumPopup::setRegionsSkipped(const QSet<int> &regions) {
  const int row = m_list->currentRow();
  if (m_generating || m_restoring || row < 0 || row >= int(m_drawings.size())) return;
  Drawing &drawing = m_drawings[row];
  for (int region : regions)
    if (region > 0 && region < drawing.proposals.size()) Cadmium::chooseProposal(drawing.proposals[region], -2);
  drawing.reviewed = false; autosave(); refresh(); refreshPreview();
}

void CadmiumPopup::setRegionsProtected(const QSet<int> &regions, bool protect) {
  const int row = m_list->currentRow();
  if (m_generating || m_restoring || row < 0 || row >= int(m_drawings.size())) return;
  Drawing &drawing = m_drawings[row];
  for (int region : regions)
    if (region > 0 && region < drawing.proposals.size()) Cadmium::protectProposal(drawing.proposals[region], protect);
  drawing.reviewed = false; autosave(); refresh(); refreshPreview();
}

void CadmiumPopup::addPaletteDecision(int region, int style) {
  const int row = m_list->currentRow();
  if (m_generating || m_restoring || row < 0 || row >= int(m_drawings.size())) return;
  Drawing &drawing = m_drawings[row];
  if (!drawing.ready || drawing.target.kind == Cadmium::Kind::FullColor || !drawing.target.palette ||
      region <= 0 || region >= drawing.proposals.size() || style < 0 ||
      style >= drawing.target.palette->getStyleCount() || drawing.proposals[region].protectedRegion) return;
  const TColorStyle *colorStyle = drawing.target.palette->getStyle(style);
  if (!dynamic_cast<const TSolidColorStyle *>(colorStyle)) {
    m_status->setText(tr("Choose a solid native palette style.")); return;
  }
  const TPixel32 pixel = colorStyle->getMainColor();
  Cadmium::ProposalCandidate candidate;
  candidate.style = style; candidate.color = QColor(pixel.r, pixel.g, pixel.b, pixel.m);
  Cadmium::RegionProposal &proposal = drawing.proposals[region];
  int selected = -1;
  for (int i = 0; i < proposal.candidates.size(); ++i)
    if (Cadmium::sameCandidate(candidate, proposal.candidates[i], drawing.target.kind)) selected = i;
  if (selected < 0) {
    if (proposal.candidates.size() >= Cadmium::MaximumRegionCandidates) return;
    proposal.candidates.append(candidate); selected = proposal.candidates.size() - 1;
  }
  Cadmium::chooseProposal(proposal, selected);
  drawing.reviewed = false; autosave(); refresh(); refreshPreview();
}

void CadmiumPopup::apply(bool all) {
  if (m_generating || m_restoring || m_backgroundTasks || !currentScene() ||
      !Cadmium::canPaintLevel(m_level.getPointer())) return;
  std::map<TFrameId, TImageP> before, after;
  QVector<int> accepted;
  QString error;
  for (int index = 0; index < int(m_drawings.size()); ++index) {
    Drawing &drawing = m_drawings[index];
    if ((all ? !drawing.reviewed : index != m_list->currentRow()) || !drawing.ready || drawing.applied) continue;
    if ((m_level->getType() != OVL_XSHLEVEL && m_level->isFrameReadOnly(drawing.fid)) ||
        m_level->getCadmiumReferences().count(drawing.fid) ||
        drawing.references.size() != drawing.referenceIds.size() ||
        drawing.referenceBuilders.size() != drawing.referenceIds.size() ||
        m_level->getRenumberCount() != drawing.renumberCount ||
        !drawing.targetBuilder || ImageManager::instance()->getBuilder(m_level->getImageId(drawing.fid)) != drawing.targetBuilder.getPointer()) {
      error = tr("A drawing was replaced, renumbered, protected, or changed. Regenerate before applying."); break;
    }
    auto fresh = [this, &error](const TFrameId &fid, const Cadmium::Frame &saved, Cadmium::Frame &now) {
      if (m_level->getType() == PLI_XSHLEVEL)
        return snapshot(fid, now, error) && now.fingerprint == saved.fingerprint;
      if (!snapshot(fid, now, error, true)) return false;
      return saved.kind == Cadmium::Kind::ToonzRaster
          ? Cadmium::matchesToonzRaster(now.original, saved, error)
          : Cadmium::matchesFullColor(now.original, now, saved, error);
    };
    Cadmium::Frame targetNow;
    if (!fresh(drawing.fid, drawing.target, targetNow)) {
      if (error.isEmpty()) error = tr("The target or its input settings changed. Regenerate before applying."); break;
    }
    for (int ref = 0; ref < drawing.referenceIds.size(); ++ref) {
      Cadmium::Frame referenceNow;
      const TFrameId &fid = drawing.referenceIds[ref];
      if (!m_level->getCadmiumReferences().count(fid) || !drawing.referenceBuilders[ref] ||
          ImageManager::instance()->getBuilder(m_level->getImageId(fid)) != drawing.referenceBuilders[ref].getPointer() ||
          !fresh(fid, drawing.references[ref], referenceNow)) {
        if (error.isEmpty()) error = tr("A chosen reference changed or was replaced. Regenerate before applying."); break;
      }
    }
    if (!error.isEmpty()) break;
    for (int region = 1; region < drawing.proposals.size(); ++region)
      if (!drawing.proposals[region].protectedRegion && drawing.proposals[region].selected == -1) {
        error = tr("Choose a fill, explicitly skip, or protect every unresolved region before applying."); break;
      }
    if (!error.isEmpty()) break;
    TImageP result = composeProposal(drawing, error);
    if (!result) { if (error.isEmpty()) error = tr("The native proposal could not be prepared."); break; }
    before[drawing.fid] = targetNow.original; after[drawing.fid] = result; accepted.append(index);
  }
  if (!error.isEmpty()) { QMessageBox::warning(this, tr("Cadmium"), error); return; }
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
    if (newLevel) { if (created) m_scene->getLevelSet()->removeLevel(created); }
    else {
      for (const auto &entry : before) {
        destination->setFrame(entry.first, entry.second);
        IconGenerator::instance()->invalidate(destination.getPointer(), entry.first);
      }
      destination->setDirtyFlag(sourceWasDirty);
      TApp::instance()->getCurrentLevel()->notifyLevelChange();
    }
    QMessageBox::warning(this, tr("Cadmium"), tr("The paint batch could not be committed. Its original drawings were restored."));
    return;
  }
  for (int index : accepted) m_drawings[index].applied = true;
  autosave(); refresh(); refreshPreview();
  m_status->setText(newLevel
      ? tr("Created result level %1. Original drawings were preserved.").arg(QString::fromStdWString(destination->getName()))
      : tr("Applied %1 drawings. Undo restores their original paint.").arg(after.size()));
}

QString CadmiumPopup::sessionCacheRoot() const {
  return QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation);
}

QString CadmiumPopup::sceneIdentity() const {
  return currentScene() && !m_scene->isUntitled() ? m_scene->getScenePath().getQString() : QString();
}

Cadmium::SessionOptions CadmiumPopup::sessionOptions() const {
  Cadmium::SessionOptions options;
  options.source = static_cast<Cadmium::RasterSource>(m_rasterSource->currentIndex());
  options.paperThreshold = m_paperThreshold->value(); options.lineThreshold = m_lineThreshold->value();
  options.extractionApproved = m_extraction->isChecked(); options.repaint = m_repaint->isChecked();
  auto *line = reinterpret_cast<TXshSimpleLevel *>(m_lineLevel->currentData().toULongLong());
  if (currentScene() && line && options.source == Cadmium::RasterSource::LinkedLines) {
    std::vector<TXshLevel *> levels; m_scene->getLevelSet()->listLevels(levels);
    if (std::find(levels.begin(), levels.end(), line) != levels.end()) {
      options.lineLevelId = line->cadmiumLevelId(); options.lineSourceKey = stableSourceKey(m_scene, line);
    }
  }
  return options;
}

Cadmium::Session CadmiumPopup::makeSession() const {
  Cadmium::Session session;
  if (!currentScene()) return session;
  session.levelId = m_level->cadmiumLevelId();
  session.kind = m_level->getType() == PLI_XSHLEVEL ? Cadmium::Kind::Vector :
      m_level->getType() == OVL_XSHLEVEL ? Cadmium::Kind::FullColor : Cadmium::Kind::ToonzRaster;
  session.options = sessionOptions();
  QMap<TFrameId, Cadmium::SessionDrawing> records;
  if (m_savedSession.levelId == session.levelId &&
      Cadmium::sessionOptionsDigest(m_savedSession.options) == Cadmium::sessionOptionsDigest(session.options))
    for (const Cadmium::SessionDrawing &drawing : m_savedSession.drawings)
      if (m_level->isFid(drawing.fid)) records.insert(drawing.fid, drawing);
  const TFrameId selected = m_list->currentRow() >= 0 && m_list->currentRow() < int(m_drawings.size())
      ? m_drawings[m_list->currentRow()].fid : TFrameId();
  for (const Drawing &drawing : m_drawings) {
    Cadmium::SessionDrawing record;
    record.fid = drawing.fid; record.ready = drawing.ready; record.applied = drawing.applied;
    record.reviewed = drawing.reviewed && drawing.ready;
    if (drawing.applied) {
      record.ready = false; record.reviewed = false;
      for (const TFrameId &fid : drawing.referenceIds) {
        Cadmium::SessionReference reference; reference.fid = fid;
        record.references.append(reference);
      }
      records.insert(record.fid, record);
      continue;
    }
    record.targetFingerprint = drawing.target.fingerprint;
    if (!drawing.target.lines.isNull()) {
      record.canvas = drawing.target.lines.size(); record.labelsFingerprint = drawing.labelsFingerprint;
    }
    record.regions = drawing.proposals;
    for (int ref = 0; ref < drawing.referenceIds.size(); ++ref) {
      Cadmium::SessionReference reference; reference.fid = drawing.referenceIds[ref];
      if (ref < drawing.references.size()) {
        reference.fingerprint = drawing.references[ref].fingerprint;
        reference.regionCount = drawing.references[ref].regionColors.size();
      }
      record.references.append(reference);
    }
    records.insert(record.fid, record);
  }
  for (auto record = records.cbegin(); record != records.cend(); ++record) {
    if (record.key() == selected) session.selectedDrawing = session.drawings.size();
    session.drawings.append(record.value());
  }
  return session;
}

void CadmiumPopup::autosave() {
  if (m_refreshing || !currentScene()) return;
  ++m_sessionRevision; m_sessionDirty = true;
  m_pendingSession = makeSession(); m_pendingScene = sceneIdentity();
  m_saveTimer.start();
}

void CadmiumPopup::saveSession(bool explicitSave) {
  const QString scene = currentScene() ? sceneIdentity() : m_pendingScene;
  if (scene.isEmpty()) {
    if (explicitSave) m_status->setText(tr("Save the scene first to keep this painting session across restarts."));
    return;
  }
  if (m_sessionWrites) { m_saveTimer.start(); return; }
  if (!m_sessionDirty && !explicitSave) return;
  Cadmium::Session session = currentScene() ? makeSession() : m_pendingSession;
  QString error;
  if (!Cadmium::validateSession(session, error)) { m_status->setText(error); return; }
  if (!m_sessionScene.isEmpty() && m_sessionScene != scene) {
    if (!Cadmium::copySession(sessionCacheRoot(), m_sessionScene, scene, session.levelId, error))
      error.clear(); // A new complete snapshot can create the Save As entry.
  }
  m_sessionScene = scene;
  if (currentScene()) m_savedSession = session;
  m_pendingSession = session; m_pendingScene = scene;
  auto *task = new CadmiumSessionTask;
  task->cacheRoot = sessionCacheRoot(); task->scene = scene; task->session = session;
  task->sourceKey = m_sessionSourceKey;
  const quint64 revision = m_sessionRevision;
  const quint64 epoch = m_snapshotEpoch;
  const QString levelId = session.levelId;
  ++m_sessionWrites; m_sessionWriter = task;
  connect(task, &QThread::finished, this, [this, task, revision, epoch, levelId, scene, explicitSave] {
    --m_sessionWrites;
    if (m_sessionWriter == task) m_sessionWriter = nullptr;
    if (task->success && revision == m_sessionRevision &&
        m_pendingSession.levelId == levelId && m_pendingScene == scene) m_sessionDirty = false;
    if (currentScene() && m_level->cadmiumLevelId() == levelId && sceneIdentity() == scene && epoch == m_snapshotEpoch) {
      if (!task->success) m_status->setText(task->error);
      else if (explicitSave) m_status->setText(tr("Painting session saved. Source drawings were not modified."));
    }
    if (m_sessionDirty) m_saveTimer.start();
    refresh();
  });
  connect(task, &QThread::finished, task, &QObject::deleteLater);
  task->start();
}

void CadmiumPopup::flushSession() {
  m_saveTimer.stop();
  if (m_sessionWriter) m_sessionWriter->wait();
  if (!m_sessionDirty) return;
  const QString scene = currentScene() ? sceneIdentity() : m_pendingScene;
  if (scene.isEmpty()) return;
  QString error;
  const Cadmium::Session session = currentScene() ? makeSession() : m_pendingSession;
  if (Cadmium::saveSessionWithAlias(sessionCacheRoot(), scene, m_sessionSourceKey, session, error)) m_sessionDirty = false;
}

void CadmiumPopup::restoreSession() {
  if (m_generating || m_restoring || m_backgroundTasks || !currentScene()) return;
  if (m_sessionWriter) m_sessionWriter->wait();
  Cadmium::Session saved;
  QString error;
  if (!Cadmium::loadSessionWithAlias(sessionCacheRoot(), sceneIdentity(), m_level->cadmiumLevelId(), m_sessionSourceKey, saved, error)) {
    m_status->setText(error); return;
  }
  const Cadmium::Kind kind = m_level->getType() == PLI_XSHLEVEL ? Cadmium::Kind::Vector :
      m_level->getType() == OVL_XSHLEVEL ? Cadmium::Kind::FullColor : Cadmium::Kind::ToonzRaster;
  if (saved.kind != kind) { m_status->setText(tr("The saved session belongs to another native drawing format.")); return; }
  const Cadmium::SessionOptions previousOptions = sessionOptions();
  const int previousLine = m_lineLevel->currentIndex();
  m_refreshing = true;
  m_rasterSource->setCurrentIndex(int(saved.options.source));
  m_paperThreshold->setValue(saved.options.paperThreshold); m_lineThreshold->setValue(saved.options.lineThreshold);
  m_extraction->setChecked(saved.options.extractionApproved); m_repaint->setChecked(saved.options.repaint);
  m_lineLevel->setCurrentIndex(0);
  if (!saved.options.lineLevelId.isEmpty()) for (int i = 1; i < m_lineLevel->count(); ++i) {
    auto *line = reinterpret_cast<TXshSimpleLevel *>(m_lineLevel->itemData(i).toULongLong());
    std::vector<TXshLevel *> levels; m_scene->getLevelSet()->listLevels(levels);
    if (std::find(levels.begin(), levels.end(), line) != levels.end() &&
        (line->cadmiumLevelId() == saved.options.lineLevelId ||
         (!saved.options.lineSourceKey.isEmpty() && stableSourceKey(m_scene, line) == saved.options.lineSourceKey))) {
      m_lineLevel->setCurrentIndex(i); saved.options.lineLevelId = line->cadmiumLevelId();
    }
  }
  m_refreshing = false;
  if (Cadmium::sessionOptionsDigest(saved.options) != Cadmium::sessionOptionsDigest(sessionOptions())) {
    // An unavailable source must not change the repaint policy or extraction
    // settings underneath proposals that the artist has already reviewed.
    m_refreshing = true;
    m_rasterSource->setCurrentIndex(int(previousOptions.source));
    m_paperThreshold->setValue(previousOptions.paperThreshold);
    m_lineThreshold->setValue(previousOptions.lineThreshold);
    m_extraction->setChecked(previousOptions.extractionApproved);
    m_repaint->setChecked(previousOptions.repaint);
    m_lineLevel->setCurrentIndex(previousLine);
    m_refreshing = false;
    m_status->setText(tr("The saved clean-line source or painting settings are no longer available.")); return;
  }
  m_saveTimer.stop();
  // Resume replaces the current review state. Only successfully validated saved
  // entries may become applicable again, including when repaint mode changed.
  for (Drawing &drawing : m_drawings) {
    drawing.ready = false; drawing.reviewed = false; drawing.error.clear();
    drawing.target = Cadmium::Frame(); drawing.labelsFingerprint.clear();
    drawing.proposals.clear(); drawing.selectedRegions.clear();
    drawing.references.clear(); drawing.referencePreviews.clear();
    drawing.targetBuilder = ImageBuilderP(); drawing.referenceBuilders.clear();
    drawing.pendingTargetBuilder = ImageBuilderP(); drawing.pendingReferenceBuilders.clear();
    drawing.requestedRegions.clear();
  }
  m_jobs.clear(); m_recovery.clear();
  for (const Cadmium::SessionDrawing &record : saved.drawings) {
    for (int index = 0; index < int(m_drawings.size()); ++index) if (m_drawings[index].fid == record.fid) {
      bool validReferences = true;
      for (const Cadmium::SessionReference &reference : record.references)
        if (!m_level->isFid(reference.fid) || !m_level->getCadmiumReferences().count(reference.fid)) validReferences = false;
      if (!validReferences) { m_drawings[index].error = tr("A saved marked reference is no longer available."); continue; }
      Drawing &drawing = m_drawings[index];
      drawing.referenceIds.clear();
      for (const Cadmium::SessionReference &reference : record.references) drawing.referenceIds.append(reference.fid);
      drawing.ready = false; drawing.reviewed = false; drawing.error.clear();
      m_recovery.insert(record.fid, record); m_jobs.append(index);
    }
  }
  m_savedSession = saved; m_sessionScene = sceneIdentity(); m_sessionDirty = false;
  m_pendingSession = saved; m_pendingScene = sceneIdentity(); ++m_sessionRevision;
  ++m_snapshotEpoch; m_cancelled = false; m_restoring = true; m_jobPosition = -1; m_running = -1;
  refresh(); restoreNext();
}

void CadmiumPopup::restoreNext() {
  if (!m_restoring || m_cancelled || !currentScene()) return;
  if (++m_jobPosition >= m_jobs.size()) {
    m_restoring = false; m_running = -1;
    if (m_savedSession.selectedDrawing >= 0 && m_savedSession.selectedDrawing < m_savedSession.drawings.size()) {
      const TFrameId selected = m_savedSession.drawings[m_savedSession.selectedDrawing].fid;
      for (int i = 0; i < int(m_drawings.size()); ++i)
        if (m_drawings[i].fid == selected) m_list->setCurrentRow(i, QItemSelectionModel::NoUpdate);
    }
    refresh(); refreshPreview();
    m_status->setText(tr("Saved session checked against current artwork. Stale entries require regeneration; previously applied entries remain history."));
    return;
  }
  m_running = m_jobs[m_jobPosition]; prepareDrawing(m_running, true);
}

void CadmiumPopup::clearSession() {
  if (m_generating || m_restoring || !currentScene()) return;
  m_saveTimer.stop();
  if (m_sessionWriter) m_sessionWriter->wait();
  ++m_sessionRevision; m_sessionDirty = false;
  QString error;
  if (!sceneIdentity().isEmpty() && !Cadmium::removeSessionWithAlias(sessionCacheRoot(), sceneIdentity(), m_level->cadmiumLevelId(), m_sessionSourceKey, error)) {
    m_status->setText(error); return;
  }
  m_savedSession = Cadmium::Session(); m_recovery.clear();
  m_pendingSession = Cadmium::Session(); m_pendingScene.clear();
  for (Drawing &drawing : m_drawings) {
    drawing.ready = false; drawing.reviewed = false; drawing.applied = false;
    drawing.proposals.clear(); drawing.target = Cadmium::Frame();
    drawing.labelsFingerprint.clear();
    drawing.references.clear(); drawing.referencePreviews.clear(); drawing.selectedRegions.clear(); drawing.error.clear();
  }
  ++m_snapshotEpoch; refresh(); refreshPreview();
  m_status->setText(tr("Saved proposals and review decisions cleared. Scene artwork and undo history were preserved."));
}

void CadmiumPopup::cancel() {
  ++m_snapshotEpoch; m_cancelled = true;
  const bool processing = m_generating;
  m_generating = false; m_restoring = false; m_running = -1;
  if (processing) CadmiumRuntime::instance()->stop();
  autosave(); refresh();
  m_status->setText(tr("Processing stopped. Completed proposals and manual decisions are retained."));
}

void CadmiumPopup::closeEvent(QCloseEvent *event) {
  if (m_generating || m_restoring) cancel();
  if (m_sessionDirty) saveSession();
  QDialog::closeEvent(event);
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
