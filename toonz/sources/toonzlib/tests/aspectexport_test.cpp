// Regression test of the export to an aspect ratio (Output Settings > Aspect
// Ratio): the size of the written frames, the copy of the output settings,
// and the scene file, which carries the setting only when it is used (older
// OpenToonz builds reject the tag).

#include "toonz/sceneproperties.h"
#include "toonz/preferences.h"
#include "toutputproperties.h"
#include "tstream.h"
#include "tfilepath.h"
#include "tenv.h"
#include "texception.h"
#include "tconvert.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QTemporaryDir>

#include <cmath>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {

void require(bool condition, const char *message) {
  if (!condition) throw std::runtime_error(message);
}

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

void save(const TSceneProperties &props, const TFilePath &fp) {
  TOStream os(fp);
  os.openChild("properties");
  props.saveData(os);
  os.closeChild();
}

void load(TSceneProperties &props, const TFilePath &fp) {
  TIStream is(fp);
  std::string tag;
  require(is.matchTag(tag) && tag == "properties", "load: properties tag");
  props.loadData(is, false);
  require(is.matchEndTag(), "load: end tag");
}

QByteArray contents(const TFilePath &fp) {
  QFile file(toQString(fp));
  require(file.open(QIODevice::ReadOnly), "read: open");
  return file.readAll();
}

void testSceneFile(const QString &dir) {
  // Not used: nothing written, so the scene opens in any build
  const TFilePath plain(QDir(dir).filePath("plain.xml").toStdWString());
  TSceneProperties defaults;
  save(defaults, plain);
  require(!contents(plain).contains("aspectExport"),
          "scene: no tag when not used");

  // Used: written, and read back
  const TFilePath used(QDir(dir).filePath("used.xml").toStdWString());
  TSceneProperties props;
  TOutputProperties &out = *props.getOutputProperties();
  out.setAspectMode(TOutputProperties::AspectCrop);
  out.setAspectRatio(2.39);
  out.setAspectColor(TPixel32(255, 0, 0, 200));
  save(props, used);
  require(contents(used).contains("aspectExport"), "scene: tag when used");

  TSceneProperties loaded;
  load(loaded, used);
  const TOutputProperties &back = *loaded.getOutputProperties();
  require(back.getAspectMode() == TOutputProperties::AspectCrop,
          "scene: mode read back");
  require(std::abs(back.getAspectRatio() - 2.39) < 1e-9,
          "scene: ratio read back");
  require(back.getAspectColor() == TPixel32(255, 0, 0, 200),
          "scene: color read back");

  // Loading a scene without it resets it
  load(loaded, plain);
  require(loaded.getOutputProperties()->getAspectMode() ==
              TOutputProperties::NoAspectExport,
          "scene: reset when absent");
}

}  // namespace

int main(int argc, char *argv[]) {
  QCoreApplication app(argc, argv);
  try {
    QTemporaryDir dir;
    require(dir.isValid(), "no temporary directory");
    // An empty stuff folder, so that the defaults are used
    const QString stuffPath = QDir(dir.path()).filePath("stuff");
    QDir().mkpath(QDir(stuffPath).filePath("profiles/layouts/rooms/Default"));
    TEnv::setStuffDir(TFilePath(stuffPath.toStdWString()));
    Preferences::instance();

    testOutputSize();
    testCopy();
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
