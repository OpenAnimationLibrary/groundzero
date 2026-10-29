// SPDX-License-Identifier: BSD-3-Clause

// Exercise real save failures and retries using replacement collisions and
// serialization failures, without relying on permissions or a full test disk.
#include "toonz/toonzscene.h"
#include "toonz/tproject.h"
#include "toonz/txsheet.h"
#include "toonz/txshsimplelevel.h"
#include "toonz/txshleveltypes.h"
#include "toonz/levelproperties.h"
#include "toonz/preferences.h"
#include "toonz/studiopalette.h"
#include "tenv.h"
#include "texception.h"
#include "timagecache.h"
#include "tnzimage.h"
#include "tpalette.h"
#include "tcolorstyles.h"
#include "tsimplecolorstyles.h"
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

void paletteFailureAndRetry(const QDir &dir, bool studio) {
  ToonzScene scene;
  TProjectManager::instance()->initializeScene(&scene);
  const TFilePath path = pathOf(dir.filePath(
      studio ? "studio-palette-save.tlv" : "palette-save.tlv"));
  const QString palettePath = path.withType("tpl").getQString();
  TXshSimpleLevelP level =
      new TXshSimpleLevel(studio ? L"studio-palette-save" : L"palette-save");
  level->setScene(&scene);
  level->setType(TZP_XSHLEVEL);
  level->setPath(path, true);
  level->setPalette(new TPalette);
  if (studio) level->getPalette()->setGlobalName(L"save-transaction-studio");
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
  TPaletteP reopened;
  if (studio) {
    reopened = StudioPalette::instance()->load(path.withType("tpl"));
  } else {
    TIStream input(path.withType("tpl"));
    TPersist *persist = nullptr;
    input >> persist;
    reopened = dynamic_cast<TPalette *>(persist);
    if (!reopened) delete persist;
  }
  require(!!reopened, "palette retry did not write a palette");
  require(reopened->getStyleCount() == level->getPalette()->getStyleCount(),
          "palette retry produced an incomplete palette");
  require(reopened->getStyle(1)->getMainColor() == TPixel32(120, 30, 220),
          "palette retry did not persist the edited color");
  if (studio)
    require(reopened->getGlobalName() == level->getPalette()->getGlobalName(),
            "palette retry lost the studio palette identity");
}

class CountedStyle final : public TSolidColorStyle {
  int &m_destroyed;

public:
  explicit CountedStyle(int &destroyed) : m_destroyed(destroyed) {}
  ~CountedStyle() override { ++m_destroyed; }
};

void paletteStyleOwnership() {
  int destroyed = 0;
  {
    TPaletteP palette = new TPalette;
    TColorStyle *owned = new CountedStyle(destroyed);
    palette->setStyle(1, owned);
    TColorStyle *originalZero = palette->getStyle(0);
    for (int target : {1, 0, -1, palette->getStyleCount()}) {
      palette->setStyle(target, owned);
      require(destroyed == 0, "aliased palette style was deleted");
      require(palette->getStyle(1) == owned &&
                  palette->getStyle(0) == originalZero,
              "aliased style assignment changed the palette");
    }
    palette->setStyle(-1, new CountedStyle(destroyed));
    palette->setStyle(palette->getStyleCount(), new CountedStyle(destroyed));
    require(destroyed == 2, "rejected new palette styles were not released");
  }
  require(destroyed == 3, "accepted palette style was not released exactly once");
}

void paletteAddStyleOwnership() {
  int destroyed = 0;
  {
    TPaletteP palette = new TPalette;
    const int styleId = palette->addStyle(new CountedStyle(destroyed));
    require(styleId == 2, "new palette style was not appended");
    TColorStyle *owned = palette->getStyle(styleId);
    require(palette->addStyle(owned) == -1 && destroyed == 0,
            "duplicate add deleted a palette-owned style");
    require(palette->getStyleCount() == 3 && palette->getStyle(styleId) == owned,
            "duplicate add changed the palette");

    TPalette::Page *page = palette->getPage(0);
    page->addStyle(styleId);
    while (page->getStyleCount() < 8)
      require(page->addStyle(TPixel32::Black) >= 0,
              "cannot fill the cleanup palette fixture");
    palette->setIsCleanupPalette(true);
    require(palette->addStyle(owned) == -1 && destroyed == 0,
            "cleanup limit deleted a borrowed style");
    require(palette->addStyle(new CountedStyle(destroyed)) == -1 && destroyed == 1,
            "cleanup limit did not release the rejected new style");
  }
  require(destroyed == 2, "appended palette style was not released exactly once");
}

class FailingStyle final : public TSolidColorStyle {
  void saveData(TOutputStreamInterface &os) const override {
    TSolidColorStyle::saveData(os);
    throw TException("Intentional style serialization failure");
  }
};

void studioPaletteSerializationFailure(const QDir &dir) {
  const TFilePath path = pathOf(dir.filePath("studio-transaction.tpl"));
  TPaletteP palette = new TPalette;
  palette->setGlobalName(L"save-transaction-studio");
  StudioPalette *studio = StudioPalette::instance();
  studio->save(path, palette.getPointer());
  const QByteArray original = bytesOf(path.getQString());
  const QFile::Permissions permissions = QFile::permissions(path.getQString());
  const QStringList files = dir.entryList(QDir::Files);

  palette->setStyle(1, new FailingStyle);
  palette->setDirtyFlag(true);
  bool rejected = false;
  try {
    studio->save(path, palette.getPointer());
  } catch (const TException &) {
    rejected = true;
  }
  require(rejected, "studio palette serialization failure was not reported");
  require(bytesOf(path.getQString()) == original,
          "failed serialization truncated the last studio palette");
  require(palette->getDirtyFlag(), "failed studio save cleared dirty state");
  require(dir.entryList(QDir::Files) == files,
          "failed studio save left a temporary palette behind");

  palette->setStyle(1, TPixel32(10, 90, 180));
  studio->save(path, palette.getPointer());
  TPaletteP reopened = studio->load(path);
  require(!!reopened, "studio palette retry could not be read");
  require(reopened->getGlobalName() == palette->getGlobalName(),
          "studio palette retry lost the palette identity");
  require(reopened->getStyle(1)->getMainColor() == TPixel32(10, 90, 180),
          "studio palette retry did not persist the edited color");
  require(QFile::permissions(path.getQString()) == permissions,
          "studio palette retry changed existing file permissions");
  require(dir.entryList(QDir::Files) == files,
          "studio palette retry left a temporary palette behind");
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
    for (bool studio : {false, true}) paletteFailureAndRetry(dir, studio);
    paletteStyleOwnership();
    paletteAddStyleOwnership();
    studioPaletteSerializationFailure(dir);
    std::cout << "Scene and palette save transactions passed\n";
    return 0;
  } catch (const TException &) {
    std::cerr << "Unexpected save exception\n";
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
  }
  return 1;
}
