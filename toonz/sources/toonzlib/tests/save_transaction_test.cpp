// SPDX-License-Identifier: BSD-3-Clause

// Exercise real save failures and retries. Directory collisions make writes
// fail on every platform, without relying on permissions or a full test disk.
#include "toonz/toonzscene.h"
#include "toonz/tproject.h"
#include "toonz/txsheet.h"
#include "toonz/txshsimplelevel.h"
#include "toonz/txshleveltypes.h"
#include "toonz/levelproperties.h"
#include "toonz/preferences.h"
#include "tenv.h"
#include "texception.h"
#include "timagecache.h"
#include "tnzimage.h"
#include "tpalette.h"
#include "tcolorstyles.h"
#include "tstream.h"
#include "tthread.h"
#include "ttoonzimage.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QTemporaryDir>

#include <iostream>
#include <stdexcept>

namespace {

void require(bool value, const char *message) {
  if (!value) throw std::runtime_error(message);
}

TFilePath pathOf(const QString &path) { return TFilePath(path); }

QByteArray bytesOf(const QString &path) {
  QFile file(path);
  require(file.open(QIODevice::ReadOnly), "cannot read test file");
  return file.readAll();
}

void blockDestination(const QString &path) {
  require(QDir().mkpath(path), "cannot create blocking directory");
  QFile marker(QDir(path).filePath("keep"));
  require(marker.open(QIODevice::WriteOnly), "cannot create directory marker");
  require(marker.write("keep") == 4, "cannot write directory marker");
}

void sceneFailureAndRetry(const QDir &dir, bool untitled, bool child) {
  ToonzScene scene;
  TProjectManager::instance()->initializeScene(&scene);
  if (!untitled) scene.setScenePath(pathOf(dir.filePath("original.tnz")));
  const TFilePath original = scene.getScenePath();
  const TFilePath alias("$scenefolder/levels/drawing.tlv");
  const TFilePath originalResource = scene.decodeFilePath(alias);
  const QString name = QString("scene-%1-%2.tnz").arg(untitled).arg(child);
  TFilePath destination = pathOf(dir.filePath(name));
  blockDestination(destination.getQString());

  bool rejected = false;
  try {
    // Skip icons: this checks persistence without an OpenGL context.
    scene.save(destination, child ? scene.getTopXsheet() : nullptr, false);
  } catch (const TException &) {
    rejected = true;
  }
  require(rejected, "scene replacement failure was not reported");
  require(scene.getScenePath() == original, "failed save changed scene path");
  require(scene.isUntitled() == untitled, "failed save changed untitled state");
  require(scene.decodeFilePath(alias) == originalResource,
          "failed save redirected scene-folder resources");
  require(bytesOf(QDir(destination.getQString()).filePath("keep")) == "keep",
          "failed replacement changed the existing destination");

  require(QDir(destination.getQString()).removeRecursively(),
          "cannot remove blocking directory");
  scene.save(destination, child ? scene.getTopXsheet() : nullptr, false);
  require(scene.getScenePath() == (child ? original : destination),
          "successful save committed the wrong scene path");
  require(scene.isUntitled() == (child && untitled),
          "successful save committed the wrong untitled state");
  ToonzScene reopened;
  reopened.loadTnzFile(destination);
}

void paletteFailureAndRetry(const QDir &dir) {
  ToonzScene scene;
  TProjectManager::instance()->initializeScene(&scene);
  const TFilePath path = pathOf(dir.filePath("palette-save.tlv"));
  const QString palettePath = path.withType("tpl").getQString();
  TXshSimpleLevelP level = new TXshSimpleLevel(L"palette-save");
  level->setScene(&scene);
  level->setType(TZP_XSHLEVEL);
  level->setPath(path, true);
  level->setPalette(new TPalette);
  level->getProperties()->setImageRes(TDimension(2, 2));
  TRasterCM32P raster(2, 2);
  raster->clear();
  TToonzImageP drawing(raster, raster->getBounds());
  drawing->setPalette(level->getPalette());
  drawing->setDpi(72, 72);
  level->setFrame(TFrameId(1), drawing);
  level->setDirtyFlag(true);
  level->save();
  require(!level->getProperties()->getDirtyFlag(), "initial level save failed");

  const QByteArray oldPalette = bytesOf(palettePath);
  const QByteArray oldLevel   = bytesOf(path.getQString());
  require(QFile::rename(palettePath, palettePath + ".saved"),
          "cannot move palette fixture");
  blockDestination(palettePath);
  level->getPalette()->setStyle(1, TPixel32(120, 30, 220));
  level->getPalette()->setDirtyFlag(true);

  bool rejected = false;
  try {
    level->save();
  } catch (const TException &) {
    rejected = true;
  }
  require(rejected, "palette replacement failure was not reported");
  require(level->getPalette()->getDirtyFlag(), "failed palette save cleared dirty state");
  require(bytesOf(path.getQString()) == oldLevel,
          "palette-only failure changed level pixels");
  require(bytesOf(palettePath + ".saved") == oldPalette,
          "palette failure changed the original palette fixture");

  require(QDir(palettePath).removeRecursively(), "cannot unblock palette path");
  require(QFile::rename(palettePath + ".saved", palettePath),
          "cannot restore palette fixture");
  level->save();
  require(!level->getPalette()->getDirtyFlag(), "palette retry did not clear dirty state");
  require(bytesOf(palettePath) != oldPalette, "palette retry skipped changed colors");
  require(bytesOf(path.getQString()) == oldLevel, "palette retry changed level pixels");
  // Confirm that the replacement can be parsed, not just that bytes changed.
  TIStream input(path.withType("tpl"));
  TPersist *persist = nullptr;
  input >> persist;
  TPaletteP reopened = dynamic_cast<TPalette *>(persist);
  if (!reopened) {
    delete persist;
    throw std::runtime_error("palette retry did not write a palette");
  }
  require(reopened->getStyleCount() == level->getPalette()->getStyleCount(),
          "palette retry produced an incomplete palette");
  require(reopened->getStyle(1)->getMainColor() == TPixel32(120, 30, 220),
          "palette retry did not persist the edited color");
}

}  // namespace

int main(int argc, char **argv) {
  QCoreApplication application(argc, argv);
  try {
    QTemporaryDir storage;
    require(storage.isValid(), "cannot create test storage");
    QDir dir(storage.path());
    const QString stuff = dir.filePath("stuff");
    QDir().mkpath(QDir(stuff).filePath("profiles/layouts/rooms/Default"));
    QDir().mkpath(QDir(stuff).filePath("library"));
    TEnv::setStuffDir(pathOf(stuff));
    TEnv::setApplicationFullName("save_transaction_test");
    Preferences::instance()->setValue(backupEnabled, false);
    Preferences::instance()->setValue(compressSceneFiles, false);
    QDir().mkpath(dir.filePath("cache"));
    TImageCache::instance()->setRootDir(pathOf(dir.filePath("cache")));
    TThread::init();
    initImageIo();
    for (bool untitled : {false, true})
      for (bool child : {false, true}) sceneFailureAndRetry(dir, untitled, child);
    paletteFailureAndRetry(dir);
    std::cout << "Scene and palette save transactions passed\n";
    return 0;
  } catch (const TException &) {
    std::cerr << "Unexpected save exception\n";
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
  }
  return 1;
}
