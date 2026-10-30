#include "libraryprovenanceintegrationtest.h"
#include "librarycommand.h"
#include "iocommand.h"
#include "nativeuiintegrationtest.h"
#include "tapp.h"
#include "toonz/levelset.h"
#include "toonz/sceneresources.h"
#include "toonz/toonzscene.h"
#include "toonz/tproject.h"
#include "toonz/tscenehandle.h"
#include "toonz/txshchildlevel.h"
#include "toonz/txshsimplelevel.h"
#include "toonz/txshleveltypes.h"
#include "toonz/txsheet.h"
#include "toonz/txshcell.h"
#include "toonz/levelproperties.h"
#include "tundo.h"
#include <QAbstractButton>
#include <QApplication>
#include <QColor>
#include <QDialog>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QImage>
#include <QTemporaryDir>
#include <QTimer>
#include <set>
#include <iostream>
#include <stdexcept>

namespace {
using Origin = TXshLevel::LibraryProvenance;
void require(bool condition, const char *message) {
  if (!condition) throw std::runtime_error(message);
}
void reset() {
  QString error;
  if (!resetNativeUiIntegrationScene(&error)) throw std::runtime_error(error.toStdString());
}
ToonzScene *scene() { return TApp::instance()->getCurrentScene()->getScene(); }
TXshLevel *byId(ToonzScene *source, const QString &id) {
  std::vector<TXshLevel *> levels;
  source->getLevelSet()->listLevels(levels);
  for (auto *level : levels) if (level->getCastId() == id) return level;
  return nullptr;
}
TXshLevel *withRelation(ToonzScene *source, const QString &itemId, Origin::Relationship relation) {
  std::vector<TXshLevel *> levels;
  source->getLevelSet()->listLevels(levels);
  for (auto *level : levels)
    if (level->getSimpleLevel() && level->getLibraryProvenance().itemId == itemId &&
        level->getLibraryRelationship() == relation) return level;
  return nullptr;
}
void uniqueIds(ToonzScene *source) {
  std::set<QString> ids;
  std::vector<TXshLevel *> levels;
  source->getLevelSet()->listLevels(levels);
  for (auto *level : levels)
    require(!level->getCastId().isEmpty() && ids.insert(level->getCastId()).second,
            "Library insertion produced duplicate Cast UUIDs");
}
IoCmd::LoadResourceArguments arguments(const TFilePath &item, IoCmd::LoadResourceArguments::ImportPolicy policy, int column) {
  IoCmd::LoadResourceArguments result(item);
  result.importPolicy = policy;
  result.renamePolicy = IoCmd::LoadResourceArguments::RenamePolicy::NEVER;
  result.convertPolicy = IoCmd::LoadResourceArguments::ConvertPolicy::NEVER;
  result.cachingBehavior = IoCmd::LoadResourceArguments::ON_DEMAND;
  result.row0 = 0; result.col0 = column; result.expose = true;
  return result;
}
struct ItemFiles {
  TFilePath item;
  ~ItemFiles() {
    QFile::remove(item.getQString());
    QFile::remove(LibraryCmd::getItemInfoPath(item).getQString());
  }
};
void roundTrip(const TFilePath &path, const QString &id, const Origin &origin,
               Origin::Relationship relationship, bool expected) {
  scene()->saveCopy(path);
  ToonzScene restored;
  TProjectManager::instance()->initializeScene(&restored);
  restored.setProject(scene()->getProject());
  restored.loadTnzFile(path);
  restored.loadResources(false);
  auto *level = byId(&restored, id);
  require(level && level->getLibraryProvenance() == origin &&
              level->getLibraryRelationship() == relationship && level->sourceExpectedOnDisk() == expected,
          "Library provenance, ownership, UUID or expected-source state changed on native reload");
  if (level->getSimpleLevel()) {
    const QImage image(restored.decodeFilePath(level->getPath()).getQString());
    require(!image.isNull() && image.size() == QSize(23, 11) &&
                image.pixelColor(5, 5) == QColor(20, 100, 190, 255),
            "Library copied drawing did not survive reload as the original pixel content");
  }
  uniqueIds(&restored);
}
}  // namespace

void runLibraryProvenanceIntegrationTest() {
  reset();
  const QString templates = LibraryCmd::getTemplatesFolder().getQString();
  const QString root = QFileInfo(QDir::currentPath()).canonicalFilePath() + '/';
  require(QDir::cleanPath(QFileInfo(templates).absoluteFilePath()).startsWith(root),
          "Refusing to write Library fixtures outside the prepared disposable root");
  if (QFileInfo(templates).exists())
    require(QFileInfo(templates).canonicalFilePath().startsWith(root),
            "Refusing a Library fixture directory outside disposable storage");
  require(QDir().mkpath(templates), "Cannot prepare disposable Library templates folder");
  QTemporaryDir assets(QDir(templates).filePath("provenance-XXXXXX"));
  require(assets.isValid(), "Cannot prepare disposable Library item assets");
  const QString itemName = QFileInfo(assets.path()).fileName();
  ItemFiles files{TFilePath(QDir(templates).filePath(itemName + ".tnz"))};
  const QString imagePath = QDir(assets.path()).filePath("ProvenanceDrawing.png");
  QImage picture(23, 11, QImage::Format_ARGB32);
  picture.fill(QColor(20, 100, 190, 255));
  require(picture.save(imagePath), "Cannot write Library source drawing");
  auto *seed = scene()->loadLevel(TFilePath(imagePath));
  require(seed && seed->getSimpleLevel(), "Cannot load Library source drawing");
  const QString seedId = seed->getCastId();
  auto *nested = scene()->createNewLevel(CHILD_XSHLEVEL, L"Provenance nested")->getChildLevel();
  nested->getXsheet()->setCell(0, 0, TXshCell(seed, seed->getSimpleLevel()->getFirstFid()));
  scene()->getTopXsheet()->setCell(0, 0, TXshCell(nested, TFrameId(1)));
  scene()->saveCopy(files.item);
  LibraryCmd::writeItemInfo(files.item, LibraryCmd::RigKind, false, itemName.toStdWString());
  const auto authoritative = LibraryCmd::templateProvenance(files.item);
  require(authoritative.relationship == Origin::Snapshot && !authoritative.itemId.isEmpty() &&
              authoritative.revision == 1, "Real Library sidecar did not establish item provenance");
  // Neither a plain external file nor a copied .libinfo outside the registered
  // Library templates directory establishes a new Library relationship.
  const TFilePath external(QDir(assets.path()).filePath("external.tnz"));
  require(QFile::copy(files.item.getQString(), external.getQString()) &&
              QFile::copy(LibraryCmd::getItemInfoPath(files.item).getQString(),
                          LibraryCmd::getItemInfoPath(external).getQString()),
          "Cannot prepare external scene attribution control");
  require(LibraryCmd::templateProvenance(external).relationship == Origin::NotRecorded &&
              seed->getLibraryRelationship() == Origin::NotRecorded,
          "External path or copied sidecar was mistaken for an actual Library item");

  reset();
  auto plainArgs = arguments(external, IoCmd::LoadResourceArguments::LOAD, 0);
  require(IoCmd::loadResources(plainArgs, false) == 1 &&
              plainArgs.loadedLevels.front()->getLibraryRelationship() == Origin::NotRecorded,
          "Loading an ordinary external scene claimed Library ownership");
  std::vector<TXshLevel *> plainLevels;
  scene()->getLevelSet()->listLevels(plainLevels);
  for (auto *level : plainLevels)
    require(level->getLibraryRelationship() == Origin::NotRecorded,
            "Ordinary imported dependencies gained inferred Library provenance");
  reset();
  bool canceled = false;
  QTimer cancel;
  QObject::connect(&cancel, &QTimer::timeout, [&]() {
    if (auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget())) {
      canceled = true;
      dialog->reject();
    }
  });
  cancel.start(20);
  auto canceledArgs = arguments(files.item, IoCmd::LoadResourceArguments::ASK_USER, 0);
  require(IoCmd::loadResources(canceledArgs, false) == 0, "Canceled Library import reported a loaded item");
  cancel.stop();
  require(canceled && scene()->getLevelSet()->getLevelCount() == 0 &&
              TUndoManager::manager()->getHistoryCount() == 0 && canceledArgs.loadedLevels.empty(),
          "Canceled Library insertion changed Cast, exposures or undo history");

  auto linkedArgs = arguments(files.item, IoCmd::LoadResourceArguments::LOAD, 0);
  require(IoCmd::loadResources(linkedArgs, false) == 1 && linkedArgs.loadedLevels.size() == 1,
          "Library Load did not insert its native sub-xsheet");
  auto *linked = withRelation(scene(), authoritative.itemId, Origin::Linked);
  auto *snapshot = linkedArgs.loadedLevels.front();
  require(linked && linked->getCastId() != seedId && linked->sourceExpectedOnDisk() &&
              snapshot->getChildLevel() && snapshot->getLibraryRelationship() == Origin::Snapshot,
          "Library Load did not distinguish linked drawing from embedded rig snapshot");
  const QString linkedId = linked->getCastId(), snapshotId = snapshot->getCastId();
  const Origin linkedOrigin = linked->getLibraryProvenance();
  const int count = scene()->getLevelSet()->getLevelCount();
  uniqueIds(scene());
  TUndoManager::manager()->undo();
  require(!byId(scene(), linkedId) && !byId(scene(), snapshotId), "Library insertion undo retained imported Cast entries");
  TUndoManager::manager()->redo();
  require(byId(scene(), linkedId) == linked && byId(scene(), snapshotId) == snapshot &&
              linked->getLibraryProvenance() == linkedOrigin,
          "Library insertion redo changed UUID or provenance");
  roundTrip(TFilePath(QDir(assets.path()).filePath("linked.tnz")), linkedId, linkedOrigin, Origin::Linked, true);
  roundTrip(TFilePath(QDir(assets.path()).filePath("snapshot.tnz")), snapshotId,
            snapshot->getLibraryProvenance(), Origin::Snapshot, snapshot->sourceExpectedOnDisk());
  LibraryCmd::writeItemInfo(files.item, LibraryCmd::RigKind, true, itemName.toStdWString());
  const auto revised = LibraryCmd::templateProvenance(files.item);
  require(revised.itemId == authoritative.itemId && revised.revision == 2 &&
              linked->getLibraryProvenance().revision == 1,
          "Library overwrite rewrote existing scene attribution instead of preserving inserted revision");

  auto repeated = arguments(files.item, IoCmd::LoadResourceArguments::LOAD, 1);
  require(IoCmd::loadResources(repeated, false) == 1 && byId(scene(), linkedId) == linked &&
              scene()->getLevelSet()->getLevelCount() == count + 2 &&
              repeated.loadedLevels.front()->getLibraryProvenance().revision == 2 &&
              linked->getLibraryProvenance().revision == 1,
          "Repeated Library Load failed to reuse the same linked drawing");
  uniqueIds(scene());
  TUndoManager::manager()->undo();
  require(scene()->getLevelSet()->getLevelCount() == count && byId(scene(), linkedId) == linked,
          "Repeated Library Load undo removed a reused drawing");

  // Import must still copy when a previously loaded drawing shares the source.
  auto imported = arguments(files.item, IoCmd::LoadResourceArguments::IMPORT, 1);
  require(IoCmd::loadResources(imported, false) == 1, "Library Import failed");
  auto *copy = withRelation(scene(), authoritative.itemId, Origin::LocalCopy);
  require(copy && copy != linked && copy->getCastId() != linkedId &&
              copy->getLibraryProvenance().sourceAssetPath == linkedOrigin.sourceAssetPath &&
              scene()->decodeFilePath(copy->getPath()) != TFilePath(linkedOrigin.sourceAssetPath),
          "Library Import reused the live source or lost origin attribution");
  const QString copiedId = copy->getCastId();
  uniqueIds(scene());
  roundTrip(TFilePath(QDir(assets.path()).filePath("imported.tnz")), copiedId,
            copy->getLibraryProvenance(), Origin::LocalCopy, true);
  TUndoManager::manager()->undo();
  require(!byId(scene(), copiedId) && byId(scene(), linkedId) == linked,
          "Library Import undo disturbed the original linked asset");

  const TFilePath linkedPath = linked->getPath();
  const QString unavailablePath = QDir(assets.path()).filePath("UnavailableDependency.png");
  require(picture.save(unavailablePath), "Cannot prepare collection interruption source");
  TXshLevelP unavailable = scene()->loadLevel(TFilePath(unavailablePath));
  require(unavailable && QFile::rename(unavailablePath, unavailablePath + ".hidden"),
          "Cannot interrupt collection with a missing source");
  {
    ResourceCollector collector(scene());
    SceneResources transaction(scene(), nullptr);
    transaction.accept(&collector);
    require(collector.aborted() && linked->getPath() == linkedPath &&
                linked->getLibraryProvenance() == linkedOrigin &&
                linked->getLibraryRelationship() == Origin::Linked &&
                unavailable->sourceExpectedOnDisk(),
            "Failed collection changed live links/provenance or falsely marked a missing source as new");
  }
  scene()->getLevelSet()->removeLevel(unavailable.getPointer());
  unavailable = TXshLevelP();
  require(QFile::rename(unavailablePath + ".hidden", unavailablePath),
          "Cannot restore collection interruption source");
  {
    ResourceCollector collector(scene());
    SceneResources transaction(scene(), scene()->getTopXsheet());
    transaction.accept(&collector, false);
    require(!collector.aborted() && linked->getLibraryRelationship() == Origin::LocalCopy &&
                linked->getCastId() == linkedId,
            "Collect did not detach file ownership while retaining attribution and UUID");
    transaction.rollbackPaths();
    require(linked->getPath() == linkedPath && linked->getLibraryProvenance() == linkedOrigin &&
                linked->getLibraryRelationship() == Origin::Linked && linked->sourceExpectedOnDisk(),
            "Collection transaction rollback failed to restore exact source metadata");
  }
  {
    ResourceCollector collector(scene());
    SceneResources transaction(scene(), scene()->getTopXsheet());
    transaction.accept(&collector);
    require(!collector.aborted() && linked->getLibraryRelationship() == Origin::LocalCopy &&
                linked->getLibraryProvenance().itemId == authoritative.itemId,
            "Completed collection lost source attribution or still claimed live Library linkage");
  }
  const Origin collectedOrigin = linked->getLibraryProvenance();
  const QString hiddenSource = imagePath + ".unavailable";
  require(QFile::rename(imagePath, hiddenSource), "Cannot hide original Library drawing");
  roundTrip(TFilePath(QDir(assets.path()).filePath("collected.tnz")), linkedId,
            collectedOrigin, Origin::LocalCopy, true);
  require(QFile::rename(hiddenSource, imagePath), "Cannot restore Library drawing");
  reset();
  std::cout << "native-ui-integration: PASS: Library provenance, ownership, UUID, import undo and collection rollback\n";
}
