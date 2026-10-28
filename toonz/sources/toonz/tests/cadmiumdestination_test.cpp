#include "cadmiumdestination.h"
#include "tenv.h"
#include "texception.h"
#include "timagecache.h"
#include "tnzimage.h"
#include "tpalette.h"
#include "tthread.h"
#include "toonz/levelset.h"
#include "toonz/preferences.h"
#include "toonz/toonzscene.h"
#include "toonz/tproject.h"
#include "toonz/txshleveltypes.h"
#include "toonz/txshsimplelevel.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>
#include <iostream>
#include <stdexcept>

namespace {
void require(bool condition, const char *message) {
  if (!condition) throw std::runtime_error(message);
}
TFilePath pathOf(const QString &path) {
  return TFilePath(path.toStdWString());
}
TXshSimpleLevel *addLevel(ToonzScene &scene, const std::wstring &name,
                          const TFilePath &path) {
  auto *level = new TXshSimpleLevel(name);
  level->setScene(&scene);
  level->setType(OVL_XSHLEVEL);
  level->setPath(path, true);
  level->setPalette(new TPalette);
  require(scene.getLevelSet()->insertLevel(level), "cannot insert fixture level");
  return level;
}

void destinations(const QDir &root, const QString &folder,
                   const std::string &extension) {
  const QDir dir(root.filePath(folder));
  require(QDir().mkpath(dir.path()), "cannot create fixture directory");
  ToonzScene scene;
  TProjectManager::instance()->initializeScene(&scene);
  scene.setScenePath(pathOf(dir.filePath("scene.tnz")));
  TXshSimpleLevel *source = addLevel(
      scene, L"Source", pathOf(dir.filePath("source..png")));
  source->set16BitChannelLevel(extension == "tif");
  source->setFloatChannelLevel(extension == "exr");

  const auto candidatePath = [&](const std::wstring &name) {
    return scene.getDefaultLevelPath(OVL_XSHLEVEL, name).withType(extension);
  };
  // The first collision exists only on disk and is intentionally not a valid
  // image: the destination chooser must not load, rewrite or remove it.
  const TFilePath occupiedPath = scene.decodeFilePath(
      candidatePath(L"Source_paint")).withFrame(TFrameId(1));
  const QString occupied = occupiedPath.getQString();
  require(QDir().mkpath(QFileInfo(occupied).absolutePath()),
          "cannot create fixture resource directory");
  QFile original(occupied);
  const QByteArray originalBytes("existing-resource-must-survive");
  require(original.open(QIODevice::WriteOnly) &&
              original.write(originalBytes) == originalBytes.size(),
          "cannot write collision fixture");
  original.close();

  // A different cast name can still own the next actual resource path.
  addLevel(scene, L"DifferentName", candidatePath(L"Source_paint_1"));
  // The third candidate is occupied by name even though its path differs.
  addLevel(scene, L"Source_paint_2", pathOf(dir.filePath("another..png")));

  QString error;
  TXshSimpleLevel *result = Cadmium::createResultLevel(&scene, source, error);
  require(result, "result creation failed after valid collisions");
  require(error.isEmpty() && result->getName() == L"Source_paint_3",
          "result did not skip disk, cast-path and name collisions");
  require(result->getPath().getType() == extension,
          "the actual output format was chosen after collision checking");
  require(scene.decodeFilePath(result->getPath()) ==
              scene.decodeFilePath(candidatePath(L"Source_paint_3")),
          "result resource path differs from the validated destination");
  require(original.open(QIODevice::ReadOnly) && original.readAll() == originalBytes,
          "existing resource bytes were altered");
  original.close();

  TXshSimpleLevel *second = Cadmium::createResultLevel(&scene, source, error);
  require(second && second != result && second->getName() == L"Source_paint_4",
          "a second result reused an occupied destination");
  require(second->getPath().getType() == extension,
          "a subsequent result lost its output format");
}
}

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
    Preferences::instance()->setValue(
        pathAliasPriority, int(Preferences::SceneFolderAlias), false);
    TEnv::setApplicationFullName("cadmium_destination_test");
    QDir().mkpath(dir.filePath("cache"));
    TImageCache::instance()->setRootDir(pathOf(dir.filePath("cache")));
    TThread::init();
    initImageIo();
    destinations(dir, QStringLiteral("byte"), "png");
    destinations(dir, QStringLiteral("sixteen"), "tif");
    destinations(dir, QStringLiteral("floating"), "exr");
    QString error;
    require(!Cadmium::createResultLevel(nullptr, nullptr, error) && !error.isEmpty(),
            "missing source and scene were accepted");
    std::cout << "Cadmium destination collision checks passed\n";
    return 0;
  } catch (const TException &) {
    std::cerr << "Cadmium destination operation failed\n";
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
  }
  return 1;
}
