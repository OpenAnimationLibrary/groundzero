// Regression test of the export to an aspect ratio (Output Settings > Aspect
// Ratio): the size of the written frames, the copy of the output settings,
// and the scene file, which carries the setting only when it is used (older
// OpenToonz builds reject the tag).

#include "toonz/sceneproperties.h"
#include "toonz/toonzscene.h"
#include "toonz/tproject.h"
#include "toonz/preferences.h"
#include "toutputproperties.h"
#include "tstream.h"
#include "tfilepath.h"
#include "tenv.h"
#include "texception.h"
#include "tconvert.h"
#include "toonz/preferencesitemids.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QTemporaryDir>

#ifdef __linux__
#include <execinfo.h>
#endif

#include <cmath>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>

namespace {

void require(bool condition, const char *message) {
  if (!condition) throw std::runtime_error(message);
}

// Progress lines, so that a crash shows how far the test got
void step(const char *what) { std::cout << "ok: " << what << std::endl; }

#ifdef __linux__
void onCrash(int sig) {
  void *frames[64];
  const int count = backtrace(frames, 64);
  std::fprintf(stderr, "aspectexport_test: signal %d, backtrace:\n", sig);
  backtrace_symbols_fd(frames, count, 2);
  std::_Exit(128 + sig);
}
#endif

//------------------------------------------------------------------

void testOutputSize() {
  TOutputProperties out;
  const TDimension hd(1920, 1080);
  require(out.getOutputSize(hd) == hd, "size: unchanged by default");

  out.setAspectMode(TOutputProperties::AspectLetterbox);
  out.setAspectRatio(2.39);
  require(out.getOutputSize(hd) == hd, "size: letterbox keeps the size");

  out.setAspectMode(TOutputProperties::AspectCrop);
  out.setAspectRatio(1.85);
  require(out.getOutputSize(hd) == TDimension(1920, 1038), "size: 1.85 crop");
  out.setAspectRatio(2.39);
  require(out.getOutputSize(hd) == TDimension(1920, 802),
          "size: 2.39 crop, rounded down to even");
  out.setAspectRatio(4.0 / 3.0);
  require(out.getOutputSize(hd) == TDimension(1440, 1080), "size: 4:3 crop");

  // Bad values are ignored
  out.setAspectRatio(-1);
  require(std::abs(out.getAspectRatio() - 4.0 / 3.0) < 1e-12,
          "ratio: negative ignored");
  out.setAspectMode(7);
  require(out.getAspectMode() == TOutputProperties::NoAspectExport,
          "mode: unknown is none");
}

void testCopy() {
  TOutputProperties out;
  out.setAspectMode(TOutputProperties::AspectLetterbox);
  out.setAspectRatio(2.40);
  out.setAspectColor(TPixel32(10, 20, 30, 128));

  const TOutputProperties copy(out);
  require(copy.getAspectMode() == TOutputProperties::AspectLetterbox,
          "copy: mode");
  require(copy.getAspectRatio() == 2.40, "copy: ratio");
  require(copy.getAspectColor() == TPixel32(10, 20, 30, 128), "copy: color");

  TOutputProperties assigned;
  assigned = out;
  require(assigned.getAspectMode() == TOutputProperties::AspectLetterbox,
          "assign: mode");
  require(assigned.getAspectColor() == TPixel32(10, 20, 30, 128),
          "assign: color");
}

//------------------------------------------------------------------

TFilePath toPath(const QString &path) { return TFilePath(path.toStdWString()); }

QByteArray contents(const QString &path) {
  QFile file(path);
  require(file.open(QIODevice::ReadOnly), "read: open");
  return file.readAll();
}

// A scene saved and loaded as the application does (ToonzScene::saveCopy and
// loadTnzFile, as scenebench_test does).
void testSceneFile(const QString &dir) {
  Preferences::instance()->setValue(compressSceneFiles, false, false);

  // Not used: nothing written, so the scene opens in any build
  const QString plain = QDir(dir).filePath("plain.tnz");
  {
    ToonzScene scene;
    TProjectManager::instance()->initializeScene(&scene);
    scene.saveCopy(toPath(plain));
  }
  require(!contents(plain).contains("aspectExport"),
          "scene: no tag when not used");
  step("scene without the setting saved");

  // Used: written, and read back
  const QString used = QDir(dir).filePath("used.tnz");
  {
    ToonzScene scene;
    TProjectManager::instance()->initializeScene(&scene);
    TOutputProperties &out = *scene.getProperties()->getOutputProperties();
    out.setAspectMode(TOutputProperties::AspectCrop);
    out.setAspectRatio(2.39);
    out.setAspectColor(TPixel32(255, 0, 0, 200));
    scene.saveCopy(toPath(used));
  }
  require(contents(used).contains("aspectExport"), "scene: tag when used");
  step("scene with the setting saved");

  {
    std::unique_ptr<ToonzScene> loaded(new ToonzScene());
    loaded->loadTnzFile(toPath(used));
    const TOutputProperties &back =
        *loaded->getProperties()->getOutputProperties();
    require(back.getAspectMode() == TOutputProperties::AspectCrop,
            "scene: mode read back");
    require(std::abs(back.getAspectRatio() - 2.39) < 1e-9,
            "scene: ratio read back");
    require(back.getAspectColor() == TPixel32(255, 0, 0, 200),
            "scene: color read back");
  }
  {
    std::unique_ptr<ToonzScene> loaded(new ToonzScene());
    loaded->loadTnzFile(toPath(plain));
    require(loaded->getProperties()->getOutputProperties()->getAspectMode() ==
                TOutputProperties::NoAspectExport,
            "scene: none when absent");
  }
  step("scenes read back");
}

}  // namespace

int main(int argc, char *argv[]) {
#ifdef __linux__
  std::signal(SIGSEGV, onCrash);
  std::signal(SIGABRT, onCrash);
#endif
  QCoreApplication app(argc, argv);
  try {
    QTemporaryDir dir;
    require(dir.isValid(), "no temporary directory");
    // An empty stuff folder, so that the defaults are used
    const QString stuffPath = QDir(dir.path()).filePath("stuff");
    QDir().mkpath(QDir(stuffPath).filePath("profiles/layouts/rooms/Default"));
    TEnv::setStuffDir(TFilePath(stuffPath.toStdWString()));
    Preferences::instance();
    step("environment set up");

    testOutputSize();
    step("written sizes");
    testCopy();
    step("copies");
    testSceneFile(dir.path());
  } catch (const TException &e) {
    std::cerr << "aspectexport_test failed: " << ::to_string(e.getMessage())
              << std::endl;
    return 1;
  } catch (const std::exception &e) {
    std::cerr << "aspectexport_test failed: " << e.what() << std::endl;
    return 1;
  }
  std::cout << "aspectexport_test passed" << std::endl;
  return 0;
}
