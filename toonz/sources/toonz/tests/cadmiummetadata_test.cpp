#include "tenv.h"
#include "texception.h"
#include "tfilepath.h"
#include "timagecache.h"
#include "tpalette.h"
#include "trasterimage.h"
#include "tstream.h"
#include "tthread.h"
#include "toonz/levelset.h"
#include "toonz/preferences.h"
#include "toonz/toonzscene.h"
#include "toonz/tproject.h"
#include "toonz/txshcell.h"
#include "toonz/txsheet.h"
#include "toonz/txshleveltypes.h"
#include "toonz/txshsimplelevel.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QTemporaryDir>
#include <QUuid>
#include <iostream>
#include <map>
#include <set>
#include <stdexcept>
#include <vector>

namespace {
void require(bool value, const char *message) {
  if (!value) throw std::runtime_error(message);
}

TFilePath pathOf(const QString &path) {
  return TFilePath(path.toStdWString());
}

TImageP drawing() {
  TRaster32P raster(5, 3);
  raster->fill(TPixel32(12, 34, 56, 255));
  TRasterImageP image(raster);
  image->setDpi(96, 96);
  return TImageP(image.getPointer());
}

TXshSimpleLevel *addLevel(ToonzScene &scene, const QString &path,
                         const std::wstring &name) {
  TXshSimpleLevel *level = new TXshSimpleLevel(name);
  level->setScene(&scene);
  level->setType(OVL_XSHLEVEL);
  level->setPath(pathOf(path), true);
  level->setPalette(new TPalette);
  require(scene.getLevelSet()->insertLevel(level), "cannot insert test level");
  return level;
}

TXshSimpleLevel *findLevel(ToonzScene &scene, const std::wstring &name) {
  TXshLevel *level = scene.getLevelSet()->getLevel(name);
  require(level && level->getSimpleLevel(), "saved level is missing");
  return level->getSimpleLevel();
}

void verifyMetadata(ToonzScene &scene, const QString &id,
                    const QString &sourceId, const std::set<TFrameId> &marks) {
  TXshSimpleLevel *level = findLevel(scene, L"Paint");
  require(level->cadmiumLevelId() == id, "level identity changed on save/reopen");
  require(level->cadmiumSourceLevelId() == sourceId,
          "source association changed on save/reopen");
  require(level->getCadmiumReferences() == marks,
          "drawing reference marks changed on save/reopen");
}

void testSceneRoundtrip(const QDir &dir) {
  ToonzScene scene;
  TProjectManager::instance()->initializeScene(&scene);
  scene.setScenePath(pathOf(dir.filePath("original.tnz")));
  TXshSimpleLevel *source = addLevel(scene, dir.filePath("lines..png"), L"Lines");
  TXshSimpleLevel *paint = addLevel(scene, dir.filePath("paint..png"), L"Paint");
  const std::vector<TFrameId> fids = {
      TFrameId(1), TFrameId(2, QStringLiteral("A")), TFrameId(7)};
  for (const TFrameId &fid : fids) {
    source->setFrame(fid, drawing());
    paint->setFrame(fid, drawing());
  }
  const QString sourceId = source->cadmiumLevelId();
  const QString id = paint->cadmiumLevelId();
  require(!QUuid(id).isNull() && id != sourceId, "level identities are not unique");
  require(paint->cadmiumLevelId() == id, "level identity is not stable");
  paint->setCadmiumSourceLevelId(sourceId);
  paint->setCadmiumReference(fids[0], true);
  paint->setCadmiumReference(fids[1], true);
  paint->setCadmiumReference(TFrameId(2), true);
  const std::set<TFrameId> marks = {fids[0], fids[1]};
  require(paint->getCadmiumReferences() == marks,
          "nonexistent drawing was marked or letter suffix was lost");
  scene.getXsheet()->setCell(0, 0, TXshCell(paint, fids[0]));
  scene.getXsheet()->setCell(1, 0, TXshCell(paint, fids[1]));
  scene.getXsheet()->setCell(2, 0, TXshCell(paint, fids[1]));
  scene.getXsheet()->setCell(3, 1, TXshCell(source, fids[2]));

  for (bool compressed : {false, true}) {
    Preferences::instance()->setValue(compressSceneFiles, compressed, false);
    const TFilePath path = pathOf(dir.filePath(
        compressed ? "compressed.tnz" : "plain.tnz"));
    scene.saveCopy(path);
    ToonzScene loaded;
    loaded.loadTnzFile(path);
    verifyMetadata(loaded, id, sourceId, marks);
    require(!loaded.hasUnrecognizedSceneData(),
            "Cadmium metadata became unrecognized scene data");
    require(loaded.getXsheet()->getCell(1, 0).m_frameId == fids[1],
            "letter-suffixed exposure changed on scene reload");
  }

  // Exercise the real Save As path, without requesting an icon render.
  TFilePath saveAs = pathOf(dir.filePath("renamed.tnz"));
  scene.save(saveAs, nullptr, false);
  require(scene.getScenePath() == saveAs, "Save As did not switch scene path");
  ToonzScene reopened;
  reopened.loadTnzFile(saveAs);
  verifyMetadata(reopened, id, sourceId, marks);

  const std::vector<TFrameId> renamed = {
      TFrameId(4, QStringLiteral("B")), TFrameId(10),
      TFrameId(22, QStringLiteral("C"))};
  paint->renumber(renamed);
  require(paint->getCadmiumReferences() ==
              std::set<TFrameId>({renamed[0], renamed[1]}),
          "marks did not follow sparse drawing renumbering");
  paint->renumber(fids);
  require(paint->getCadmiumReferences() == marks,
          "inverse renumber did not restore drawing marks");

  const TImageP beforeDelete = paint->getFrame(fids[1], false);
  paint->eraseFrame(fids[1]);
  require(paint->getCadmiumReferences() == std::set<TFrameId>({fids[0]}),
          "deleted drawing retained a reference mark");
  paint->setFrame(fids[1], beforeDelete);
  require(!paint->getCadmiumReferences().count(fids[1]),
          "recreated drawing unexpectedly inherited a deleted mark");
  paint->setCadmiumReference(fids[1], true);
  require(paint->getCadmiumReferences() == marks,
          "reference state cannot be restored after drawing restoration");
  paint->clearFrames();
  require(paint->getCadmiumReferences().empty(), "cleared level retained marks");
  require(paint->cadmiumLevelId() == id, "clearing frames changed level identity");
}

void writeMetadata(const TFilePath &path, const std::string &encoded,
                   const std::string &id = std::string()) {
  TOStream out(path);
  require(out.checkStatus(), "cannot write metadata fixture");
  out.openChild("level");
  out << std::wstring(L"Paint");
  std::map<std::string, std::string> attributes;
  attributes["cadmiumReferencesV1"] = encoded;
  attributes["cadmiumLevelId"] = id;
  attributes["futureAttribute"] = "ignored";
  out.openCloseChild("info", attributes);
  out.child("path") << TFilePath("paint..png");
  out.closeChild();
}

void readMetadata(const TFilePath &path, TXshSimpleLevel &level) {
  TIStream input(path);
  require(bool(input), "cannot read metadata fixture");
  std::string tag;
  require(input.matchTag(tag) && tag == "level", "missing metadata wrapper");
  level.loadData(input);
  require(input.matchEndTag(), "metadata wrapper did not close");
}

void testMalformedMetadata(const QDir &dir) {
  ToonzScene scene;
  TProjectManager::instance()->initializeScene(&scene);
  TXshSimpleLevel *level = addLevel(scene, dir.filePath("validation..png"), L"Paint");
  const TFilePath path = pathOf(dir.filePath("metadata.xml"));
  const QString validId = QUuid::createUuid().toString(QUuid::WithoutBraces);
  QJsonArray entries;
  entries.append(QJsonArray{2, QStringLiteral("A")});
  entries.append(QJsonArray{7, QString()});
  entries.append(QJsonArray{-1, QStringLiteral("A")});
  entries.append(QJsonArray{3, QString(65, QLatin1Char('A'))});
  entries.append(QJsonArray{QStringLiteral("bad"), QStringLiteral("B")});
  entries.append(QJsonArray{9});
  entries.append(QJsonArray{2, QStringLiteral("A")});
  writeMetadata(path, QJsonDocument(entries).toJson(QJsonDocument::Compact)
                          .toBase64().toStdString(), validId.toStdString());
  readMetadata(path, *level);
  const std::set<TFrameId> expected = {TFrameId(2, QStringLiteral("A")), TFrameId(7)};
  require(level->getCadmiumReferences() == expected,
          "invalid reference entries were accepted or valid entries lost");
  require(level->cadmiumLevelId() == validId, "valid identity was not loaded");

  writeMetadata(path, "malformed", "invalid");
  readMetadata(path, *level);
  require(level->getCadmiumReferences().empty(),
          "malformed metadata preserved marks from a previous document");
  require(level->cadmiumLevelId() != validId &&
              !QUuid(level->cadmiumLevelId()).isNull(),
          "invalid identity was retained instead of replaced");

  writeMetadata(path, std::string(1048577, 'A'));
  readMetadata(path, *level);
  require(level->getCadmiumReferences().empty(),
          "oversized reference metadata was accepted");
}
}  // namespace

int main(int argc, char **argv) {
  QCoreApplication application(argc, argv);
  try {
    QTemporaryDir temporary;
    require(temporary.isValid(), "cannot create test storage");
    const QDir dir(temporary.path());
    const QString stuff = dir.filePath("stuff");
    QDir().mkpath(QDir(stuff).filePath("profiles/layouts/rooms/Default"));
    QDir().mkpath(QDir(stuff).filePath("library"));
    TEnv::setStuffDir(pathOf(stuff));
    Preferences::instance();
    TEnv::setApplicationFullName("cadmium_metadata_test");
    QDir().mkpath(dir.filePath("cache"));
    TImageCache::instance()->setRootDir(pathOf(dir.filePath("cache")));
    TThread::init();
    testSceneRoundtrip(dir);
    testMalformedMetadata(dir);
    std::cout << "Cadmium scene metadata checks passed\n";
    return 0;
  } catch (const TException &) {
    std::cerr << "Cadmium scene metadata operation failed\n";
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
  }
  return 1;
}
