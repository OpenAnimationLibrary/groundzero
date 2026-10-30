#include "castbrowseractions.h"
#include "toonz/sceneproperties.h"
#include "tapp.h"
#include "toonz/levelset.h"
#include "toonz/levelproperties.h"
#include "toonz/toonzscene.h"
#include "toonz/tproject.h"
#include "toonz/sceneresources.h"
#include "toonz/particlecache.h"
#include "toonz/hook.h"
#include "toonz/tscenehandle.h"
#include "toonz/txsheethandle.h"
#include "toonz/txshlevelhandle.h"
#include "toonz/txshsimplelevel.h"
#include "toonz/txshexrlevel.h"
#include "toonz/txshpalettelevel.h"
#include "toonz/txshsoundlevel.h"
#include "toonzqt/icongenerator.h"
#include "tlevel_io.h"
#include "tlevel.h"
#include "tpalette.h"
#include "timagecache.h"
#include "tstream.h"
#include "tsystem.h"
#include "tsound_io.h"
#include "tundo.h"
#include <QApplication>
#include <QComboBox>
#include <QCryptographicHash>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDateTime>
#include <QDirIterator>
#include <QFileDialog>
#include <QFileInfo>
#include <QHeaderView>
#include <QCheckBox>
#include <QLabel>
#include <QMessageBox>
#include <QProgressDialog>
#include <QPushButton>
#include <QSaveFile>
#include <QTableWidget>
#include <QTemporaryDir>
#include <QVBoxLayout>
#include <QUuid>
#include <stdexcept>
#include <memory>

namespace {
QString tr(const char *text) { return QObject::tr(text); }
ToonzScene *scene() { return TApp::instance()->getCurrentScene()->getScene(); }
QString name(TXshLevel *level) { return QString::fromStdWString(level->getName()); }
bool dirty(TXshLevel *level) {
  if (auto *sl = level->getSimpleLevel())
    return sl->getProperties()->getDirtyFlag() ||
           (sl->getPalette() && sl->getPalette()->getDirtyFlag());
  if (auto *pl = level->getPaletteLevel())
    return pl->getPalette() && pl->getPalette()->getDirtyFlag();
  return false;
}
void notify() {
  TApp::instance()->getCurrentScene()->setDirtyFlag(true);
  TApp::instance()->getCurrentScene()->notifyCastChange();
  TApp::instance()->getCurrentXsheet()->notifyXsheetChanged();
  TApp::instance()->getCurrentXsheet()->notifyXsheetSoundChanged();
  TApp::instance()->getCurrentLevel()->notifyLevelChange();
}
QTableWidget *makeTable(QDialog &dialog, const QStringList &headers) {
  auto *table = new QTableWidget(&dialog);
  table->setColumnCount(headers.size()); table->setHorizontalHeaderLabels(headers);
  table->setEditTriggers(QAbstractItemView::NoEditTriggers);
  table->setSelectionBehavior(QAbstractItemView::SelectRows);
  table->horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
  table->horizontalHeader()->setStretchLastSection(true);
  return table;
}
int append(QTableWidget *table, const QStringList &values) {
  int r = table->rowCount(); table->insertRow(r);
  for (int c = 0; c < values.size(); ++c) {
    auto *item = new QTableWidgetItem(values[c]); item->setToolTip(values[c]);
    table->setItem(r, c, item);
  }
  return r;
}
struct FrameSnapshot {
  std::vector<std::pair<TFrameId, QString>> frames;
  TINT64 bytes = 0;
  ~FrameSnapshot() {
    for (const auto &frame : frames) TImageCache::instance()->remove(frame.second);
  }
  void capture(TXshSimpleLevel *level, const std::vector<TFrameId> &ids,
               const std::function<bool()> &cancel) {
    for (const auto &id : ids) {
      if (cancel && cancel()) throw std::runtime_error("Asset operation canceled. No links were changed.");
      TImageP image = level->getFullsampledFrame(id, ImageManager::none);
      if (!image) continue;  // A missing original file can still be relinked.
      QString key = "cast-path-undo-" + QUuid::createUuid().toString();
      TImageCache::instance()->add(key, image->cloneImage());
      frames.emplace_back(id, key);
      bytes += TUndo::getImageMemorySize(image.getPointer());
    }
  }
  void restore(TXshSimpleLevel *level) const {
    for (const auto &frame : frames) {
      TImageP image = TImageCache::instance()->get(frame.second, false);
      if (image) level->setFrame(frame.first, image->cloneImage());
    }
  }
};
struct PathChange {
  TXshLevelP level;
  TFilePath before, after, scanBefore, scanAfter;
  TSoundTrackP soundBefore, soundAfter;
  TPaletteP paletteBefore, paletteAfter;
  std::shared_ptr<FrameSnapshot> framesBefore, framesAfter;
  LevelProperties propertiesBefore, propertiesAfter;
  std::map<TFrameId, int> statusBefore;
  bool preserveStatus = false;
  bool sixteenBefore = false, sixteenAfter = false;
  bool floatBefore = false, floatAfter = false;
  TXshLevel::LibraryProvenance provenanceBefore, provenanceAfter;
  bool expectedBefore = false, expectedAfter = true;
};
class PreparationGuard {
  ToonzScene *m_scene;
  QObject m_lifetime;
  bool m_changed = false;
public:
  explicit PreparationGuard(ToonzScene *source) : m_scene(source) {
    auto mark = [this]() { m_changed = true; };
    QObject::connect(TApp::instance()->getCurrentScene(), &TSceneHandle::castChanged, &m_lifetime, mark);
    QObject::connect(TApp::instance()->getCurrentXsheet(), &TXsheetHandle::xsheetChanged, &m_lifetime, mark);
    QObject::connect(TApp::instance()->getCurrentLevel(), &TXshLevelHandle::xshLevelChanged, &m_lifetime, mark);
    QObject::connect(TApp::instance()->getCurrentScene(), &TSceneHandle::sceneSwitched, &m_lifetime, mark);
  }
  bool unchanged() const { return !m_changed && scene() == m_scene; }
  bool valid(const std::vector<PathChange> &changes) const {
    if (!unchanged()) return false;
    for (const auto &change : changes) {
      auto *level = change.level.getPointer();
      if (m_scene->getLevelSet()->getLevel(level->getName()) != level ||
          level->getPath() != change.before || dirty(level)) return false;
      if (level->getLibraryProvenance() != change.provenanceBefore ||
          level->sourceExpectedOnDisk() != change.expectedBefore) return false;
      if (auto *simple = level->getSimpleLevel()) {
        std::vector<TFrameId> ids; simple->getFids(ids);
        if (ids.size() != change.statusBefore.size() || simple->getScannedPath() != change.scanBefore ||
            simple->getPalette() != change.paletteBefore.getPointer() ||
            simple->getProperties()->options() != change.propertiesBefore.options() ||
            simple->getProperties()->getImageRes() != change.propertiesBefore.getImageRes() ||
            simple->getProperties()->getImageDpi() != change.propertiesBefore.getImageDpi() ||
            simple->getProperties()->getBpp() != change.propertiesBefore.getBpp()) return false;
        for (const auto &id : ids) {
          auto found = change.statusBefore.find(id);
          if (found == change.statusBefore.end() || simple->getFrameStatus(id) != found->second) return false;
        }
      } else if (auto *sound = level->getSoundLevel()) {
        if (sound->getSoundTrack() != change.soundBefore) return false;
      } else if (auto *palette = level->getPaletteLevel())
        if (palette->getPalette() != change.paletteBefore.getPointer()) return false;
    }
    return true;
  }
};
class PathsUndo final : public TUndo {
  std::vector<PathChange> m_changes;
  QString m_title;
  void apply(bool after) const {
    for (const auto &change : m_changes) {
      const TFilePath path = after ? change.after : change.before;
      if (auto *sl = change.level->getSimpleLevel()) {
        sl->setPath(path, true);
        sl->setScannedPath(after ? change.scanAfter : change.scanBefore);
        for (const auto &status : change.statusBefore)
          sl->setFrameStatus(status.first, after && !change.preserveStatus ? TXshSimpleLevel::Normal : status.second);
        if (after ? change.paletteAfter : change.paletteBefore)
          sl->setPalette((after ? change.paletteAfter : change.paletteBefore).getPointer());
        sl->rebindFrameSources();
        const auto &snapshot = after ? change.framesAfter : change.framesBefore;
        if (snapshot) snapshot->restore(sl);
        *sl->getProperties() = after ? change.propertiesAfter : change.propertiesBefore;
        sl->set16BitChannelLevel(after ? change.sixteenAfter : change.sixteenBefore);
        sl->setFloatChannelLevel(after ? change.floatAfter : change.floatBefore);
        sl->updateReadOnly();
        std::vector<TFrameId> fids; sl->getFids(fids);
        for (const auto &fid : fids) IconGenerator::instance()->invalidate(sl, fid);
      } else if (auto *sound = change.level->getSoundLevel()) {
        sound->setPath(path);
        sound->setSoundTrack(after ? change.soundAfter : change.soundBefore);
      } else if (auto *palette = change.level->getPaletteLevel()) {
        palette->setPath(path);
        palette->setPalette((after ? change.paletteAfter : change.paletteBefore).getPointer());
      }
      change.level->setLibraryProvenance(after ? change.provenanceAfter : change.provenanceBefore);
      change.level->setSourceExpectedOnDisk(after ? change.expectedAfter : change.expectedBefore);
    }
    notify();
  }
public:
  PathsUndo(std::vector<PathChange> changes, QString title)
      : m_changes(std::move(changes)), m_title(std::move(title)) {}
  void undo() const override { apply(false); }
  void redo() const override { apply(true); }
  int getSize() const override {
    TINT64 bytes = sizeof(*this) + TINT64(m_changes.size()) * sizeof(PathChange);
    for (const auto &change : m_changes) {
      if (change.framesBefore) bytes += change.framesBefore->bytes;
      if (change.framesAfter && change.framesAfter != change.framesBefore) bytes += change.framesAfter->bytes;
      if (change.soundBefore) bytes += TINT64(change.soundBefore->getSampleCount()) * change.soundBefore->getSampleSize();
      if (change.soundAfter) bytes += TINT64(change.soundAfter->getSampleCount()) * change.soundAfter->getSampleSize();
    }
    return clampSize(bytes);
  }
  QString getHistoryString() override { return m_title; }
};

// Decode before mutating the live level. Re-linking preserves the original frame
// identifiers, so every existing exposure must still resolve in the replacement.
PathChange prepare(TXshLevel *level, const TFilePath &path,
                   const std::function<bool()> &cancel = {}, bool identicalCopy = false) {
  if (dirty(level)) throw std::runtime_error("Save modified levels before relinking or copying.");
  TFilePath actual = scene()->decodeFilePath(path);
  TFilePath original = level->getPath();
  const std::string psdSuffix = ResourceImporter::extractPsdSuffix(original);
  ResourceImporter::extractPsdSuffix(actual);
  if (!TSystem::doesExistFileOrLevel(actual)) throw std::runtime_error("Replacement does not exist.");
  actual = ResourceImporter::buildPsd(actual, psdSuffix);
  if (QString::fromStdString(level->getPath().getType()).compare(QString::fromStdString(path.getType()), Qt::CaseInsensitive) != 0)
    throw std::runtime_error("Replacement must have the same file type.");
  PathChange change;
  change.level = TXshLevelP(level); change.before = level->getPath();
  change.after = scene()->codeFilePath(actual);
  change.provenanceBefore = change.provenanceAfter = level->getLibraryProvenance();
  change.expectedBefore = level->sourceExpectedOnDisk();
  if (level->getLibraryRelationship() == TXshLevel::LibraryProvenance::Replaced)
    change.provenanceAfter.relationship = TXshLevel::LibraryProvenance::Replaced;
  if (change.provenanceAfter.relationship != TXshLevel::LibraryProvenance::NotRecorded &&
      change.provenanceAfter.relationship != TXshLevel::LibraryProvenance::Replaced &&
      scene()->decodeFilePath(change.before) != actual)
    change.provenanceAfter.relationship = identicalCopy ? TXshLevel::LibraryProvenance::LocalCopy
                                                       : TXshLevel::LibraryProvenance::Replaced;
  if (auto *sound = level->getSoundLevel()) {
    change.soundBefore = sound->getSoundTrack();
    TSoundTrackReaderP reader(actual);
    if (!reader) throw std::runtime_error("No sound reader is available.");
    change.soundAfter = reader->load();
    if (!change.soundAfter) throw std::runtime_error("The replacement sound is unreadable.");
  } else if (auto *palette = level->getPaletteLevel()) {
    TIStream input(actual);
    std::string tag;
    if (!input || !input.matchTag(tag) || tag != "palette")
      throw std::runtime_error("The replacement palette is unreadable.");
    TPaletteP replacement = new TPalette;
    replacement->loadData(input);
    change.paletteBefore = palette->getPalette(); change.paletteAfter = replacement;
  } else if (auto *simple = level->getSimpleLevel()) {
    change.scanBefore = change.scanAfter = simple->getScannedPath();
    change.propertiesBefore = change.propertiesAfter = *simple->getProperties();
    change.sixteenBefore = change.sixteenAfter = simple->is16BitChannelLevel();
    change.floatBefore = change.floatAfter = simple->isFloatChannelLevel();
    std::vector<TFrameId> oldFrames, newFrames;
    simple->getFids(oldFrames);
    for (const auto &id : oldFrames) change.statusBefore[id] = simple->getFrameStatus(id);
    change.paletteBefore = change.paletteAfter = simple->getPalette();
    change.framesBefore = std::make_shared<FrameSnapshot>();
    change.framesBefore->capture(simple, oldFrames, cancel);
    if (identicalCopy) {
      change.framesAfter = change.framesBefore;
      change.preserveStatus = true;
      return change;
    }
    TXshSimpleLevelP probe;
    if (auto *exr = dynamic_cast<TXshExrLevel *>(simple)) {
      auto *retained = new TXshExrLevel;
      retained->setSelection(exr->getSelection(), exr->isPrimaryBinding());
      probe = retained;
    } else probe = new TXshSimpleLevel;
    probe->setScene(scene()); probe->setType(simple->getType()); probe->setPath(actual, true);
    *probe->getProperties() = *simple->getProperties();
    if (actual.getType() == "psd") {
      // Load the retained selector directly. The interactive PSD loader can
      // rename a live scene level while interpreting a layer id.
      TLevelReaderP reader(actual);
      if (!reader) throw std::runtime_error("No reader is available for this replacement.");
      TLevelP info = reader->loadInfo();
      if (!info) throw std::runtime_error("Replacement level information is unreadable.");
      probe->setPalette(info->getPalette());
      for (auto it = info->begin(); it != info->end(); ++it) {
        newFrames.push_back(it->first);
        if (std::find(oldFrames.begin(), oldFrames.end(), it->first) == oldFrames.end()) continue;
        if (cancel && cancel()) throw std::runtime_error("Relink canceled.");
        probe->setFrame(it->first, reader->getFrameReader(it->first)->load());
      }
    } else {
      TLevelReaderP reader(actual);
      if (!reader) throw std::runtime_error("No reader is available for this replacement.");
      probe->load(); probe->getFids(newFrames);
    }
    if (!std::includes(newFrames.begin(), newFrames.end(), oldFrames.begin(), oldFrames.end()))
      throw std::runtime_error("Replacement is missing drawing identifiers used by this level.");
    change.paletteAfter = probe->getPalette();
    change.framesAfter = std::make_shared<FrameSnapshot>();
    change.framesAfter->capture(probe.getPointer(), oldFrames, cancel);
    if (change.framesAfter->frames.size() != oldFrames.size())
      throw std::runtime_error("A replacement drawing is unreadable. No links were changed.");
    probe->setPath(actual, true);
    change.propertiesAfter = *probe->getProperties();
    change.propertiesAfter.setDirtyFlag(false);
    change.sixteenAfter = probe->is16BitChannelLevel();
    change.floatAfter = probe->isFloatChannelLevel();
  } else throw std::runtime_error("This asset has no relinkable file.");
  return change;
}

QStringList assetFiles(const TFilePath &storedPath, const std::function<bool()> &cancel = {},
                       std::set<QString> *seen = nullptr) {
  TFilePath path = storedPath;
  ResourceImporter::extractPsdSuffix(path);
  QStringList files;
  std::set<QString> localSeen;
  if (!seen) seen = &localSeen;
  if (path.isEmpty() || seen->size() >= 100000 ||
      !seen->insert(QDir::cleanPath(path.getQString())).second || (cancel && cancel())) return files;
  if (path.getType() == "otpc") {
    ParticleCache::Sequence sequence;
    QString error;
    if (ParticleCache::openSequence(path.getQString(), sequence, error)) {
      for (const QString &dependency : ParticleCache::sequenceFiles(sequence)) {
        if (dependency == path.getQString()) files << dependency;
        else files << assetFiles(TFilePath(dependency), cancel, seen);
      }
      files.removeDuplicates(); files.sort(); return files;
    }
  }
  QFileInfo direct(path.getQString());
  if (direct.isFile()) files << direct.absoluteFilePath();
  else if (direct.isDir()) {
    QDirIterator it(direct.absoluteFilePath(), QDir::Files, QDirIterator::Subdirectories);
    while (it.hasNext()) {
      if ((files.size() & 255) == 0 && cancel && cancel()) break;
      files << it.next();
    }
  } else {
    TFilePathSet grouped;
    if (QFileInfo(path.getParentDir().getQString()).isDir()) {
      try { TSystem::readDirectory(grouped, path.getParentDir(), false, true); } catch (...) {}
    }
    for (const auto &file : grouped)
      if (file.withNoFrame() == path.withNoFrame()) files << file.getQString();
  }
  TFilePathSet sidecars;
  TXshSimpleLevel::getFiles(path, sidecars);
  for (const auto &file : sidecars) files << file.getQString();
  if (path.getType() == "tzp" || path.getType() == "tzu") {
    const TFilePath legacyPalette = path.withType("plt");
    if (QFileInfo::exists(legacyPalette.getQString())) files << legacyPalette.getQString();
  }
  const QString auxiliary = (path.getParentDir() + (path.getName() + "_files")).getQString();
  if (QFileInfo(auxiliary).isDir()) {
    QDirIterator it(auxiliary, QDir::Files, QDirIterator::Subdirectories);
    while (it.hasNext()) {
      if ((files.size() & 255) == 0 && cancel && cancel()) break;
      files << it.next();
    }
  }
  files.removeDuplicates(); files.sort();
  return files;
}

void copyAsset(const TFilePath &destination, const TFilePath &source,
               const std::function<bool()> &cancel) {
  const auto files = assetFiles(source, cancel);
  if (files.isEmpty()) throw std::runtime_error("A source asset is missing.");
  const QDir sourceDir(source.getParentDir().getQString());
  const QDir targetDir(destination.getParentDir().getQString());
  for (const QString &path : files) {
    if (cancel && cancel()) throw std::runtime_error("Copy canceled. No links were changed.");
    const QString relative = sourceDir.relativeFilePath(path);
    if (relative == ".." || relative.startsWith("../") || QDir::isAbsolutePath(relative))
      throw std::runtime_error("An asset dependency is outside its source folder.");
    const QString target = targetDir.filePath(relative);
    if (!QDir().mkpath(QFileInfo(target).absolutePath()))
      throw std::runtime_error("Could not create an asset copy folder.");
    QFile input(path);
    QSaveFile output(target);
    if (!input.open(QIODevice::ReadOnly) || !output.open(QIODevice::WriteOnly))
      throw std::runtime_error("An asset file could not be opened for copying.");
    const QFileInfo before(path);
    while (!input.atEnd()) {
      if (cancel && cancel()) throw std::runtime_error("Copy canceled. No links were changed.");
      const QByteArray bytes = input.read(1024 * 1024);
      if ((bytes.isEmpty() && input.error() != QFile::NoError) || output.write(bytes) != bytes.size())
        throw std::runtime_error("An asset file could not be copied completely.");
    }
    const QFileInfo after(path);
    if (before.size() != after.size() || before.lastModified() != after.lastModified() || !output.commit())
      throw std::runtime_error("A source changed during copying or the copy could not be finalized.");
  }
}

void report(QWidget *parent, const QString &title, const QString &explanation,
            const QStringList &headers, const std::vector<QStringList> &rows,
            const std::function<bool(QWidget *)> &review = {},
            const QString &actionLabel = tr("Review Consolidation…")) {
  QDialog dialog(parent); dialog.setWindowTitle(title);
  auto *layout = new QVBoxLayout(&dialog);
  auto *label = new QLabel(explanation, &dialog); label->setWordWrap(true);
  layout->addWidget(label);
  auto *table = makeTable(dialog, headers);
  for (const auto &values : rows) append(table, values);
  layout->addWidget(table);
  auto *buttons = new QDialogButtonBox(QDialogButtonBox::Close, &dialog);
  auto *save = buttons->addButton(tr("Save Report…"), QDialogButtonBox::ActionRole);
  if (review) {
    auto *consolidate = buttons->addButton(actionLabel, QDialogButtonBox::ActionRole);
    consolidate->setObjectName("CastReviewConsolidation");
    QObject::connect(consolidate, &QPushButton::clicked, &dialog, [&]() {
      if (review(&dialog)) dialog.accept();
    });
  }
  QObject::connect(save, &QPushButton::clicked, &dialog, [&]() {
    QString path = QFileDialog::getSaveFileName(&dialog, tr("Save Asset Report"), QString(), tr("CSV (*.csv)"));
    if (path.isEmpty()) return;
    auto line = [](QStringList values) {
      for (QString &value : values) { value.replace('"', "\"\""); value = '"' + value + '"'; }
      return values.join(',').toUtf8() + '\n';
    };
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly)) { QMessageBox::warning(&dialog, title, file.errorString()); return; }
    QByteArray bytes = line(headers);
    bool ok = file.write(bytes) == bytes.size();
    for (const auto &values : rows) { bytes = line(values); ok = ok && file.write(bytes) == bytes.size(); }
    if (!ok || !file.commit()) QMessageBox::warning(&dialog, title, file.errorString());
  });
  QObject::connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
  layout->addWidget(buttons); dialog.resize(950, 500); dialog.exec();
}

QByteArray nativeMetadata(TXshLevel *level) {
  QTemporaryDir directory;
  if (!directory.isValid()) throw std::runtime_error("Cannot prepare metadata comparison.");
  const TFilePath path(QDir(directory.path()).filePath("comparison.xml"));
  { TOStream stream(path); stream << level; }
  TPersist *loaded = nullptr;
  { TIStream stream(path); stream >> loaded; }
  TXshLevelP clone = dynamic_cast<TXshLevel *>(loaded);
  if (!clone) throw std::runtime_error("Cannot read asset metadata for consolidation.");
  clone->setName(L"Cast comparison");
  clone->setCastTags({}); clone->clearCastCover();
  // These describe asset identity/ownership, not rendering compatibility. The
  // confirmed survivor keeps its own origin; Undo retains each removed level.
  clone->setLibraryProvenance(TXshLevel::LibraryProvenance());
  clone->setSourceExpectedOnDisk(true);
  const TFilePath neutral("asset." + level->getPath().getType());
  if (auto *simple = clone->getSimpleLevel()) simple->setPath(neutral, true);
  else if (auto *sound = clone->getSoundLevel()) sound->setPath(neutral);
  else if (auto *palette = clone->getPaletteLevel()) palette->setPath(neutral);
  { TOStream stream(path); stream << clone.getPointer();
    if (auto *simple = level->getSimpleLevel()) {
      if (simple->getPalette()) simple->getPalette()->saveData(stream);
      simple->getHookSet()->saveData(stream);
    } else if (auto *palette = level->getPaletteLevel()) {
      if (palette->getPalette()) palette->getPalette()->saveData(stream);
    }
  }
  QFile file(path.getQString());
  if (!file.open(QIODevice::ReadOnly)) throw std::runtime_error("Cannot read asset metadata comparison.");
  QByteArray serialized = file.readAll();
  // Only remove this clone's exact, validated UUID attribute. Never mutate a
  // live asset's identity or broadly strip authoring fields from the payload.
  const QByteArray identity = " castId=\"" + clone->getCastId().toUtf8() + "\"";
  if (serialized.count(identity) != 1)
    throw std::runtime_error("Cannot normalize asset identity for metadata comparison.");
  serialized.replace(identity, QByteArray());
  return serialized;
}

bool compatible(TXshLevel *a, TXshLevel *b) {
  if (a->getType() != b->getType() || dirty(a) || dirty(b)) return false;
  std::vector<TFrameId> af, bf; a->getFids(af); b->getFids(bf);
  if (af != bf) return false;
  if (auto *as = a->getSimpleLevel()) {
    auto *bs = b->getSimpleLevel();
    if (!bs || as->getProperties()->options() != bs->getProperties()->options() ||
        as->getDpi() != bs->getDpi() || as->getResolution() != bs->getResolution() ||
        as->is16BitChannelLevel() != bs->is16BitChannelLevel() ||
        as->isFloatChannelLevel() != bs->isFloatChannelLevel()) return false;
    TFilePath ap = a->getPath(), bp = b->getPath();
    if (ResourceImporter::extractPsdSuffix(ap) != ResourceImporter::extractPsdSuffix(bp)) return false;
    for (const auto &fid : af) if (as->getFrameStatus(fid) != bs->getFrameStatus(fid)) return false;
    if (bool(as->getPalette()) != bool(bs->getPalette())) return false;
    if (as->getPalette() && as->getPalette()->getGlobalName() != bs->getPalette()->getGlobalName()) return false;
  } else if (auto *sound = a->getSoundLevel()) {
    if (!b->getSoundLevel() || sound->getFrameRate() != b->getSoundLevel()->getFrameRate()) return false;
  }
  return nativeMetadata(a) == nativeMetadata(b);
}

QByteArray savedDigest(ToonzScene *source, TXshLevel *level, const std::function<bool()> &cancel) {
  if (dirty(level)) return {};
  QStringList files = assetFiles(source->decodeFilePath(level->getPath()), cancel);
  if (auto *simple = level->getSimpleLevel())
    if (!simple->getScannedPath().isEmpty()) files += assetFiles(source->decodeFilePath(simple->getScannedPath()), cancel);
  if (files.empty()) return {};
  QCryptographicHash hash(QCryptographicHash::Sha256);
  hash.addData(QByteArray::number(level->getType()) + ':' + QByteArray::number(level->getFrameCount()));
  for (const QString &path : files) {
    if (cancel && cancel()) return {};
    QFileInfo before(path);
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) return {};
    hash.addData(QByteArray::number(file.size()) + ':');
    while (!file.atEnd()) {
      QByteArray bytes = file.read(1024 * 1024);
      if (bytes.isEmpty() && file.error() != QFile::NoError) return {};
      hash.addData(bytes);
      if (cancel && cancel()) return {};
    }
    QFileInfo after(path);
    if (before.size() != after.size() || before.lastModified() != after.lastModified()) return {};
  }
  return hash.result();
}

struct ConsolidationCell {
  TXsheetP sheet;
  int row, column;
  TXshCell before, after;
};
struct ConsolidationLevel {
  TXshLevelP level, canonical;
  TFilePath folder;
};
class ConsolidationUndo final : public TUndo {
  std::vector<ConsolidationCell> m_cells;
  std::vector<ConsolidationLevel> m_levels;
  std::map<TXshLevelP, std::pair<QStringList, QStringList>> m_tags;
  TXshLevelP m_currentBefore, m_currentAfter;
  QList<TSceneProperties::CastCollection> m_collectionsBefore, m_collectionsAfter;
  void apply(bool after) const {
    auto *set = scene()->getLevelSet();
    if (!after) for (const auto &item : m_levels) {
      set->insertLevel(item.level.getPointer());
      set->moveLevelToFolder(item.folder, item.level.getPointer());
    }
    for (const auto &cell : m_cells) cell.sheet->setCell(cell.row, cell.column, after ? cell.after : cell.before);
    for (const auto &entry : m_tags) entry.first->setCastTags(after ? entry.second.second : entry.second.first);
    if (after) for (const auto &item : m_levels) set->removeLevel(item.level.getPointer());
    scene()->getProperties()->setCastCollections(after ? m_collectionsAfter : m_collectionsBefore);
    TApp::instance()->getCurrentLevel()->setLevel((after ? m_currentAfter : m_currentBefore).getPointer());
    notify();
  }
public:
  ConsolidationUndo(std::vector<ConsolidationCell> cells, std::vector<ConsolidationLevel> levels)
      : m_cells(std::move(cells)), m_levels(std::move(levels)),
        m_currentBefore(TApp::instance()->getCurrentLevel()->getLevel()), m_currentAfter(m_currentBefore) {
    m_collectionsBefore = m_collectionsAfter = scene()->getProperties()->getCastCollections();
    for (const auto &item : m_levels) {
      if (!m_tags.count(item.canonical)) m_tags[item.canonical] = {item.canonical->getCastTags(), item.canonical->getCastTags()};
      auto &tags = m_tags[item.canonical].second;
      for (const auto &tag : item.level->getCastTags()) if (!tags.contains(tag, Qt::CaseInsensitive)) tags.append(tag);
      if (m_currentBefore == item.level) m_currentAfter = item.canonical;
      for (auto &collection : m_collectionsAfter) {
        for (auto &member : collection.members)
          if (member == item.level->getCastId()) member = item.canonical->getCastId();
        collection.members.removeDuplicates();
      }
    }
  }
  void undo() const override { apply(false); }
  void redo() const override { apply(true); }
  int getSize() const override { return clampSize(sizeof(*this) + TINT64(m_cells.size()) * sizeof(ConsolidationCell) + TINT64(m_levels.size()) * 1024); }
  QString getHistoryString() override { return tr("Consolidate Reviewed Cast Duplicates"); }
};

bool reviewConsolidation(QWidget *parent, ToonzScene *source,
                         const std::map<QByteArray, std::vector<TXshLevel *>> &groups) {
  if (scene() != source) return false;
  QDialog dialog(parent); dialog.setWindowTitle(tr("Review Duplicate Consolidation"));
  dialog.setObjectName("CastConsolidationReview");
  auto *layout = new QVBoxLayout(&dialog);
  auto *label = new QLabel(tr("Check groups explicitly and choose the asset to keep. Every exposure in all scene and nested sheets will keep its frame identifier. Compatible drawing and rig metadata is required. Tags are combined; the chosen asset's name, folder, cover and Library ownership remain. Collection memberships move to the chosen asset. Files stay on disk. Undo restores original assets, memberships and exposures."), &dialog);
  label->setWordWrap(true); layout->addWidget(label);
  auto *table = makeTable(dialog, {tr("Consolidate"), tr("Keep asset"), tr("Matching assets")});
  std::vector<std::pair<QByteArray, std::vector<TXshLevel *>>> choices;
  for (const auto &group : groups) if (group.second.size() > 1) {
    choices.push_back(group);
    QStringList names; for (auto *level : group.second) names << name(level);
    const int row = append(table, {QString(), QString(), names.join(", ")});
    auto *check = table->item(row, 0); check->setFlags(check->flags() | Qt::ItemIsUserCheckable); check->setCheckState(Qt::Unchecked);
    auto *canonical = new QComboBox(table); canonical->addItems(names);
    canonical->setAccessibleName(tr("Asset to keep for group %1").arg(row + 1));
    table->setCellWidget(row, 1, canonical);
  }
  layout->addWidget(table);
  auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
  buttons->button(QDialogButtonBox::Ok)->setText(tr("Validate Chosen Groups"));
  QObject::connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
  QObject::connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
  QObject::connect(TApp::instance()->getCurrentScene(), &TSceneHandle::sceneSwitched, &dialog, &QDialog::reject);
  layout->addWidget(buttons); dialog.resize(850, 420);
  if (dialog.exec() != QDialog::Accepted || scene() != source) return false;
  PreparationGuard guard(source);
  QProgressDialog progress(tr("Validating consolidation and exposure references…"), tr("Cancel"), 0, 0, parent);
  progress.setWindowModality(Qt::WindowModal);
  auto cancel = [&]() { QApplication::processEvents(); return progress.wasCanceled() || scene() != source; };
  std::vector<ConsolidationLevel> levels;
  std::map<TXshLevel *, TXshLevel *> replace;
  std::map<TXshLevel *, QByteArray> validated;
  try {
    std::vector<TXshLevel *> identityLevels;
    source->getLevelSet()->listLevels(identityLevels);
    std::map<QString, int> identityCounts;
    for (auto *level : identityLevels) ++identityCounts[level->getCastId()];
    for (int row = 0; row < int(choices.size()); ++row) {
      if (table->item(row, 0)->checkState() != Qt::Checked) continue;
      const auto &group = choices[row];
      auto *combo = qobject_cast<QComboBox *>(table->cellWidget(row, 1));
      auto *canonical = group.second[combo->currentIndex()];
      QStringList combinedTags = canonical->getCastTags();
      for (auto *level : group.second) {
        if (level->getCastId().isEmpty() || identityCounts[level->getCastId()] != 1)
          throw std::runtime_error("A chosen asset has an ambiguous Cast identity. No assets or collection memberships were changed.");
        if (source->getLevelSet()->getLevel(level->getName()) != level ||
            savedDigest(source, level, cancel) != group.first || !compatible(canonical, level))
          throw std::runtime_error("A chosen asset changed or has different drawing, layer, palette, rig, or level metadata. No consolidation was applied.");
        validated[level] = group.first;
        if (level != canonical) {
          replace[level] = canonical;
          levels.push_back({TXshLevelP(level), TXshLevelP(canonical), source->getLevelSet()->getFolder(level)});
        }
        for (const auto &tag : level->getCastTags())
          if (!combinedTags.contains(tag, Qt::CaseInsensitive)) combinedTags.append(tag);
      }
      if (combinedTags.size() > 32)
        throw std::runtime_error("A chosen group's combined tags exceed 32. Reduce the tags before consolidation so none are lost.");
    }
    if (levels.empty() || cancel()) return false;
    std::vector<TXsheetP> sheets;
    std::set<TXsheet *> scheduled;
    auto enqueue = [&](TXsheet *sheet) { if (scheduled.insert(sheet).second) sheets.emplace_back(sheet); };
    enqueue(source->getTopXsheet());
    std::vector<TXshLevel *> all; source->getLevelSet()->listLevels(all);
    for (auto *level : all) if (auto *child = level->getChildLevel()) enqueue(child->getXsheet());
    std::set<TXsheet *> visited;
    std::vector<ConsolidationCell> cells;
    size_t inspected = 0;
    for (size_t next = 0; next < sheets.size(); ++next) {
      auto sheet = sheets[next];
      if (!visited.insert(sheet.getPointer()).second) continue;
      if (visited.size() > 10000) throw std::runtime_error("Consolidation sheet limit reached. No changes applied.");
      for (int column = 0; column < sheet->getColumnCount(); ++column) {
        int first, last; sheet->getCellRange(column, first, last);
        for (int row = first; row <= last; ++row) {
          if (++inspected > 2000000) throw std::runtime_error("Consolidation cell limit reached. No changes applied.");
          if ((inspected & 255) == 1 && cancel()) return false;
          const TXshCell before = sheet->getCell(row, column);
          if (auto *child = before.m_level ? before.m_level->getChildLevel() : nullptr)
            enqueue(child->getXsheet());
          auto found = replace.find(before.m_level.getPointer());
          if (found == replace.end()) continue;
          if (sheet->getColumn(column)->isLocked()) throw std::runtime_error("A duplicate is exposed in a locked column. Unlock it before consolidating.");
          cells.push_back({sheet, row, column, before, TXshCell(found->second, before.m_frameId)});
        }
      }
    }
    if (cancel()) return false;
    progress.reset();
    if (scene() != source || QMessageBox::question(parent, tr("Confirm Duplicate Consolidation"),
        tr("Replace %1 exposure cells and remove %2 duplicate Cast entries? Files remain on disk; Undo restores the exact assets and exposures.")
            .arg(qulonglong(cells.size())).arg(qulonglong(levels.size()))) != QMessageBox::Yes) return false;
    if (scene() != source) return false;
    progress.setLabelText(tr("Final content verification…")); progress.show();
    for (const auto &entry : validated) {
      if (!guard.unchanged() || source->getLevelSet()->getLevel(entry.first->getName()) != entry.first ||
          savedDigest(source, entry.first, cancel) != entry.second)
        throw std::runtime_error("An asset or scene changed during final review. No consolidation was applied.");
    }
    if (!guard.unchanged()) throw std::runtime_error("The scene changed during final review. No consolidation was applied.");
    progress.reset();
    for (const auto &cell : cells)
      if (cell.sheet->getCell(cell.row, cell.column) != cell.before)
        throw std::runtime_error("The scene changed while reviewing consolidation. Search again.");
    auto *undo = new ConsolidationUndo(std::move(cells), std::move(levels));
    undo->redo(); TUndoManager::manager()->add(undo);
    return true;
  } catch (const std::exception &error) {
    progress.reset(); QMessageBox::warning(parent, tr("Consolidation canceled"), QString::fromUtf8(error.what()));
  } catch (...) {
    progress.reset(); QMessageBox::warning(parent, tr("Consolidation canceled"), tr("A selected asset could not be validated. No changes were applied."));
  }
  return false;
}

}

namespace CastBrowserActions {
void relink(QWidget *parent, const std::vector<TXshLevel *> &selected) {
  if (selected.empty()) return;
  ToonzScene *source = scene();
  std::vector<TXshLevelP> retained;
  for (auto *level : selected) retained.emplace_back(level);
  QString folder = QFileDialog::getExistingDirectory(parent, tr("Search for Replacement Assets"));
  if (folder.isEmpty() || scene() != source) return;
  QProgressDialog progress(tr("Finding replacement files…"), tr("Cancel"), 0, 0, parent);
  progress.setWindowModality(Qt::WindowModal);
  std::map<QString, std::set<TFilePath>> matches;
  QDirIterator it(folder, QDir::Files, QDirIterator::Subdirectories);
  int visited = 0;
  while (it.hasNext()) {
    const TFilePath path(it.next());
    matches[QString::fromStdWString(path.getLevelNameW()).toCaseFolded()].insert(path.getParentDir() + TFilePath(path.getLevelNameW()));
    if ((++visited & 255) == 0) {
      QApplication::processEvents();
      if (progress.wasCanceled() || scene() != source) return;
    }
  }
  QApplication::processEvents();
  const bool searchCanceled = progress.wasCanceled() || scene() != source;
  progress.reset();
  if (searchCanceled) return;
  QDialog dialog(parent); dialog.setWindowTitle(tr("Review Batch Relink"));
  dialog.setObjectName("CastRelinkReview");
  QObject::connect(TApp::instance()->getCurrentScene(), &TSceneHandle::sceneSwitched, &dialog, &QDialog::reject);
  auto *layout = new QVBoxLayout(&dialog);
  auto *label = new QLabel(tr("Choose replacements explicitly when more than one match exists. Skip leaves the asset unchanged. Modified assets must be saved first."), &dialog);
  label->setWordWrap(true); layout->addWidget(label);
  auto *table = makeTable(dialog, {tr("Level"), tr("Current path"), tr("Replacement"), tr("Browse")});
  std::vector<QComboBox *> choices;
  std::vector<TXshLevelP> levels;
  for (auto *level : selected) {
    levels.push_back(TXshLevelP(level));
    int r = append(table, {name(level), level->getPath().getQString(), QString(), QString()});
    auto *combo = new QComboBox(table); combo->addItem(tr("Skip"), QString());
    combo->setAccessibleName(tr("Replacement for %1").arg(name(level)));
    TFilePath matchPath = level->getPath();
    ResourceImporter::extractPsdSuffix(matchPath);
    QString key = QString::fromStdWString(matchPath.getLevelNameW()).toCaseFolded();
    for (const auto &candidate : matches[key]) combo->addItem(candidate.getQString(), candidate.getQString());
    if (combo->count() == 2) combo->setCurrentIndex(1);
    choices.push_back(combo); table->setCellWidget(r, 2, combo);
    auto *browse = new QPushButton(tr("Browse…"), table);
    QObject::connect(browse, &QPushButton::clicked, &dialog, [=, &dialog]() {
      QString candidate = QFileDialog::getOpenFileName(&dialog, tr("Choose Replacement"), folder);
      if (!candidate.isEmpty()) {
        TFilePath path(candidate); path = path.getParentDir() + TFilePath(path.getLevelNameW());
        combo->addItem(path.getQString(), path.getQString()); combo->setCurrentIndex(combo->count() - 1);
      }
    });
    table->setCellWidget(r, 3, browse);
  }
  layout->addWidget(table);
  auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
  buttons->button(QDialogButtonBox::Ok)->setText(tr("Relink Chosen"));
  QObject::connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
  QObject::connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
  layout->addWidget(buttons); dialog.resize(1000, 480);
  if (dialog.exec() != QDialog::Accepted || scene() != source) return;
  std::vector<PathChange> changes;
  PreparationGuard guard(source);
  QProgressDialog preparing(tr("Validating replacement drawings…"), tr("Cancel"), 0, 0, parent);
  preparing.setWindowModality(Qt::WindowModal);
  auto canceled = [&]() { QApplication::processEvents(); return preparing.wasCanceled() || scene() != source; };
  try {
    for (int r = 0; r < int(levels.size()); ++r) {
      QString candidate = choices[r]->currentData().toString();
      if (candidate.isEmpty()) continue;
      changes.push_back(prepare(levels[r].getPointer(), TFilePath(candidate), canceled));
    }
  } catch (const std::exception &e) {
    QMessageBox::warning(parent, tr("Relink canceled"), QString::fromUtf8(e.what())); return;
  } catch (...) { QMessageBox::warning(parent, tr("Relink canceled"), tr("A replacement could not be read. No paths were changed.")); return; }
  const bool preparationCanceled = canceled();
  preparing.reset();
  if (changes.empty() || preparationCanceled) return;
  if (!guard.valid(changes)) {
    QMessageBox::warning(parent, tr("Relink canceled"), tr("An asset changed during preparation. No links were changed; review the selection again."));
    return;
  }
  auto *undo = new PathsUndo(std::move(changes), tr("Relink Scene Cast Assets"));
  undo->redo(); TUndoManager::manager()->add(undo);
}

void makeLocal(QWidget *parent, const std::vector<TXshLevel *> &levels) {
  if (levels.empty()) return;
  ToonzScene *source = scene();
  std::vector<TXshLevelP> retained;
  for (auto *level : levels) retained.emplace_back(level);
  const auto project = source->getProject();
  if (!project) return;
  QString destination = QFileDialog::getExistingDirectory(parent, tr("Choose a Project Folder for Local Copies"),
                                                          project->getProjectFolder().getQString());
  if (destination.isEmpty() || scene() != source) return;
  TFilePath dest(destination);
  TXshSimpleLevelP locationProbe = new TXshSimpleLevel;
  locationProbe->setPath(dest + "cast-copy-probe.pli", true);
  if (external(source, locationProbe.getPointer())) {
    QMessageBox::warning(parent, tr("Make Local Copy"), tr("Choose a folder inside this project or the saved scene folder."));
    return;
  }
  // Use a dedicated folder. No existing files or assets are overwritten.
  QString leaf = "cast-copy-" + QUuid::createUuid().toString(QUuid::WithoutBraces);
  QTemporaryDir staging(QDir(destination).filePath(".cast-copy-XXXXXX"));
  if (!staging.isValid()) { QMessageBox::warning(parent, tr("Make Local Copy"), tr("The destination is not writable.")); return; }
  std::vector<PathChange> changes;
  PreparationGuard guard(source);
  std::vector<std::pair<TXshLevel *, TFilePath>> copies;
  QProgressDialog preparing(tr("Preparing local asset copies…"), tr("Cancel"), 0, 0, parent);
  preparing.setWindowModality(Qt::WindowModal);
  auto canceled = [&]() { QApplication::processEvents(); return preparing.wasCanceled() || scene() != source; };
  try {
    for (auto *level : levels) {
      if (canceled()) return;
      if (!external(source, level)) continue;
      if (dirty(level)) throw std::runtime_error("Save modified levels before making local copies.");
      TFilePath input = source->decodeFilePath(level->getPath());
      const std::string suffix = ResourceImporter::extractPsdSuffix(input);
      const int index = int(copies.size());
      TFilePath relative = TFilePath(std::to_string(index + 1)) + TFilePath(input.getLevelNameW());
      TFilePath temporary = TFilePath(staging.path()) + relative;
      copyAsset(temporary, input, canceled);
      // Validate all copies before any live path is changed.
      PathChange change = prepare(level, temporary, canceled, true);
      change.after = source->codeFilePath(ResourceImporter::buildPsd(dest + TFilePath(leaf) + relative, suffix));
      if (auto *simple = level->getSimpleLevel()) {
        if (!simple->getScannedPath().isEmpty()) {
          TFilePath scan = source->decodeFilePath(simple->getScannedPath());
          TFilePath scanRelative = TFilePath(std::to_string(index + 1)) + "scan" + TFilePath(scan.getLevelNameW());
          copyAsset(TFilePath(staging.path()) + scanRelative, scan, canceled);
          change.scanAfter = source->codeFilePath(dest + TFilePath(leaf) + scanRelative);
        }
      }
      changes.push_back(change);
      copies.emplace_back(level, relative);
    }
    const bool preparationCanceled = canceled();
    preparing.reset();
    if (preparationCanceled) return;
    if (changes.empty()) { QMessageBox::information(parent, tr("Make Local Copy"), tr("The selection contains no external links.")); return; }
    if (QMessageBox::question(parent, tr("Make Local Copy"),
         tr("Create %1 local asset copies in %2? Undo restores the original links and retains these copies.")
             .arg(int(changes.size())).arg(QDir(destination).filePath(leaf))) != QMessageBox::Yes) return;
    if (!guard.valid(changes)) throw std::runtime_error("An asset changed during preparation. No links were changed; review the selection again.");
    if (!QDir().rename(staging.path(), QDir(destination).filePath(leaf)))
      throw std::runtime_error("Could not publish the prepared asset folder.");
    staging.setAutoRemove(false);
  } catch (const std::exception &e) { QMessageBox::warning(parent, tr("Copy canceled"), QString::fromUtf8(e.what())); return; }
    catch (...) { QMessageBox::warning(parent, tr("Copy canceled"), tr("A file could not be copied or validated. No links were changed.")); return; }
  auto *undo = new PathsUndo(std::move(changes), tr("Make Cast Assets Local"));
  undo->redo(); TUndoManager::manager()->add(undo);
}

void duplicates(QWidget *parent) {
  ToonzScene *source = scene();
  std::vector<TXshLevel *> levels; source->getLevelSet()->listLevels(levels);
  std::vector<TXshLevelP> retained;
  for (auto *level : levels) retained.emplace_back(level);
  std::map<QByteArray, std::vector<TXshLevel *>> groups;
  QProgressDialog progress(tr("Comparing saved asset contents…"), tr("Cancel"), 0, int(levels.size()), parent);
  progress.setWindowModality(Qt::WindowModal);
  std::vector<QStringList> rows;
  bool canceled = false;
  for (int i = 0; i < int(levels.size()); ++i) {
    progress.setValue(i); QApplication::processEvents();
    if (progress.wasCanceled() || scene() != source) { canceled = true; break; }
    auto *level = levels[i];
    if (dirty(level)) { rows.push_back({name(level), tr("Skipped: modified in memory"), level->getPath().getQString()}); continue; }
    const QByteArray digest = savedDigest(source, level, [&]() {
      QApplication::processEvents(); return progress.wasCanceled() || scene() != source;
    });
    if (progress.wasCanceled() || scene() != source) { canceled = true; break; }
    if (!digest.isEmpty()) groups[digest].push_back(level);
  }
  progress.reset();
  if (scene() != source) return;
  int groupNumber = 0;
  for (const auto &group : groups) if (group.second.size() > 1) {
    ++groupNumber;
    for (auto *level : group.second)
      rows.push_back({name(level), tr("Matching saved content — group %1").arg(groupNumber),
                      source->decodeFilePath(level->getPath()).getQString()});
  }
  report(parent, tr("Duplicate Asset Review"),
      (canceled ? tr("Comparison canceled; partial results. ") : QString()) +
      tr("Matching saved bytes are candidates for review. Per-level options, palettes, and frame identifiers may differ. No exposures, files, or levels are consolidated automatically. Review Consolidation is a separate explicit action with validation, confirmation and Undo."),
      {tr("Level"), tr("Finding"), tr("Path")}, rows,
      [&](QWidget *owner) { return reviewConsolidation(owner, source, groups); });
}

void collectionReport(QWidget *parent) {
  ToonzScene *source = scene();
  struct Inventory final : ResourceProcessor {
    std::vector<std::pair<QString, TFilePath>> paths;
    void process(TXshSimpleLevel *sl) override {
      paths.emplace_back(name(sl), sl->getPath());
      if (!sl->getScannedPath().isEmpty()) paths.emplace_back(name(sl) + " (scan)", sl->getScannedPath());
    }
    void process(TXshPaletteLevel *pl) override { paths.emplace_back(name(pl), pl->getPath()); }
    void process(TXshSoundLevel *sl) override { paths.emplace_back(name(sl), sl->getPath()); }
    void process(SceneFxResource *resource) override { paths.emplace_back(tr("FX resource"), resource->resourcePath()); }
  } inventory;
  {
    SceneResources resources(source, source->getTopXsheet());
    resources.accept(&inventory);
    resources.commit();  // Inventory never changes paths; avoid rollback setter side effects.
  }
  std::vector<QStringList> rows;
  std::map<QString, int> destinationNames;
  for (const auto &entry : inventory.paths)
    ++destinationNames[QString::fromStdWString(entry.second.getLevelNameW()).toCaseFolded()];
  QProgressDialog progress(tr("Inspecting collection dependencies…"), tr("Cancel"), 0, int(inventory.paths.size()), parent);
  progress.setWindowModality(Qt::WindowModal);
  bool canceled = false;
  for (int i = 0; i < int(inventory.paths.size()); ++i) {
    progress.setValue(i); QApplication::processEvents();
    if (progress.wasCanceled() || scene() != source) { canceled = true; break; }
    const auto &entry = inventory.paths[i];
    TFilePath actual = source->decodeFilePath(entry.second);
    QStringList files = assetFiles(actual, [&]() { QApplication::processEvents(); return progress.wasCanceled() || scene() != source; });
    if (progress.wasCanceled() || scene() != source) { canceled = true; break; }
    qint64 bytes = 0; for (const QString &file : files) bytes += QFileInfo(file).size();
    TFilePath physical = actual; ResourceImporter::extractPsdSuffix(physical);
    const bool missing = !TSystem::doesExistFileOrLevel(physical) ||
        std::any_of(files.begin(), files.end(), [](const QString &file) { return !QFileInfo::exists(file); });
    QString state = files.isEmpty() || missing ? tr("Missing source or dependency / not saved") : tr("Available");
    if (actual.getType() == "otpc") {
      ParticleCache::Sequence sequence; QString error;
      if (!ParticleCache::openSequence(actual.getQString(), sequence, error)) state = tr("Unreadable cache: %1").arg(error);
    }
    if (destinationNames[QString::fromStdWString(entry.second.getLevelNameW()).toCaseFolded()] > 1)
      state += tr("; repeated destination name — collector must resolve collision");
    rows.push_back({entry.first, entry.second.getQString(), actual.getQString(),
                   QString::number(files.size()), QString::number(bytes), state});
  }
  progress.reset();
  if (scene() != source) return;
  report(parent, tr("Scene Collection Report"),
      (canceled ? tr("Canceled; partial inventory. ") : QString()) +
      tr("Read-only dependency inventory using the scene resource visitor, including FX resources. Sizes include discovered files and level sidecars. Particle cache manifests include their frame files and referenced artwork; use Collect Assets and verify the relocated project. No files or paths were changed."),
      {tr("Resource"), tr("Stored path"), tr("Resolved source"), tr("Files"), tr("Bytes"), tr("Status")}, rows,
      [](QWidget *owner) {
        const QString path = QFileDialog::getOpenFileName(owner, tr("Choose Relocated Collected Scene"), QString(), tr("Scene (*.tnz)"));
        if (path.isEmpty()) return false;
        const QString root = QFileDialog::getExistingDirectory(owner, tr("Choose the Relocated Project Root"), QFileInfo(path).absolutePath());
        if (!root.isEmpty()) verifyCollectedScene(owner, TFilePath(path), TFilePath(root));
        return false;
      }, tr("Verify Relocated Scene…"));
}

void verifyCollectedScene(QWidget *parent, const TFilePath &path, const TFilePath &root) {
  std::vector<QStringList> rows;
  QProgressDialog progress(tr("Verifying relocated scene resources…"), tr("Cancel"), 0, 0, parent);
  progress.setWindowModality(Qt::WindowModal);
  auto cancel = [&]() { QApplication::processEvents(); return progress.wasCanceled(); };
  const QString canonicalRoot = QFileInfo(root.getQString()).canonicalFilePath();
  bool canceled = false;
  try {
    if (canonicalRoot.isEmpty()) throw std::runtime_error("The relocation root does not exist.");
    ToonzScene verified;
    verified.loadNoResources(path);
    if (!verified.getProject()) throw std::runtime_error("The relocated scene project could not be loaded.");
    struct References final : ResourceProcessor {
      std::vector<std::pair<QString, TFilePath>> paths;
      void process(TXshSimpleLevel *level) override {
        paths.emplace_back(name(level), level->getPath());
        if (!level->getScannedPath().isEmpty()) paths.emplace_back(name(level) + " (scan)", level->getScannedPath());
      }
      void process(TXshSoundLevel *level) override { paths.emplace_back(name(level), level->getPath()); }
      void process(TXshPaletteLevel *level) override { paths.emplace_back(name(level), level->getPath()); }
      void process(SceneFxResource *resource) override { paths.emplace_back(tr("FX resource"), resource->resourcePath()); }
    } references;
    { SceneResources resources(&verified, verified.getTopXsheet()); resources.accept(&references); resources.commit(); }
    for (const auto &entry : references.paths) {
      if (cancel()) { canceled = true; break; }
      TFilePath physical = verified.decodeFilePath(entry.second);
      ResourceImporter::extractPsdSuffix(physical);
      const auto files = assetFiles(physical, cancel);
      QStringList findings;
      if (!TSystem::doesExistFileOrLevel(physical) || files.empty()) findings << tr("Missing source");
      for (const auto &file : files) {
        const QString actual = QFileInfo(file).canonicalFilePath();
#ifdef _WIN32
        constexpr auto sensitivity = Qt::CaseInsensitive;
#else
        constexpr auto sensitivity = Qt::CaseSensitive;
#endif
        if (actual.isEmpty()) findings << tr("Missing dependency");
        else if (actual.compare(canonicalRoot, sensitivity) != 0 &&
                 !actual.startsWith(canonicalRoot + '/', sensitivity)) findings << tr("Dependency remains outside relocated root");
        QFile readable(file);
        if (!readable.open(QIODevice::ReadOnly)) findings << tr("Unreadable dependency");
      }
      if (physical.getType() == "otpc") {
        ParticleCache::Sequence cache; QString error;
        if (!ParticleCache::openSequence(physical.getQString(), cache, error)) findings << tr("Unreadable cache: %1").arg(error);
      }
      findings.removeDuplicates();
      rows.push_back({entry.first, findings.empty() ? tr("Verified: available inside relocated root") : findings.join("; "), physical.getQString()});
    }
    // Read every exposed drawing through a native reader, preserving PSD/EXR
    // selectors by loading the detached scene's actual level type.
    CastWorkflow::Limits limits; limits.expandInstances = false;
    const auto uses = CastWorkflow::index(verified.getTopXsheet(), {}, cancel, limits);
    if (!uses.complete || uses.cycle) rows.push_back({tr("Scene"), tr("Exposure validation incomplete or cyclic"), path.getQString()});
    std::map<TXshSimpleLevel *, std::set<TFrameId>> drawings;
    for (const auto &use : uses.occurrences)
      if (auto *level = use.level->getSimpleLevel())
        for (int row = use.first; row <= use.last; ++row)
          drawings[level].insert(use.sheet->getCell(row, use.column).m_frameId);
    for (const auto &entry : drawings) {
      if (cancel()) { canceled = true; break; }
      auto *level = entry.first;
      try {
        level->load();
        bool readable = true;
        for (const auto &fid : entry.second) {
          if (cancel()) { canceled = true; break; }
          if (!level->isFid(fid) || !level->getFullsampledFrame(fid, ImageManager::none)) readable = false;
        }
        rows.push_back({name(level), readable && !canceled ? tr("Verified: exposed drawings decode") : tr("Unreadable or missing exposed drawing"), level->getPath().getQString()});
      } catch (...) { rows.push_back({name(level), tr("Native drawing decode failed"), level->getPath().getQString()}); }
    }
  } catch (const std::exception &error) {
    rows.push_back({tr("Scene"), QString::fromUtf8(error.what()), path.getQString()});
  } catch (...) { rows.push_back({tr("Scene"), tr("The scene could not be read"), path.getQString()}); }
  progress.reset();
  report(parent, tr("Relocated Scene Verification"),
      (canceled ? tr("Canceled; results are partial. ") : QString()) +
      tr("Checks the selected saved scene in a detached scene object. Dependencies must resolve inside the chosen relocated root. Exposed drawings are decoded with native readers. The open working scene and files are unchanged."),
      {tr("Resource"), tr("Verification"), tr("Resolved path")}, rows);
}
}  // namespace CastBrowserActions
