// SPDX-License-Identifier: BSD-3-Clause

// A sub-xsheet whose file has no <output> entry used to load with no output
// node, and building the scene FX then read the first element of an empty
// list and crashed. The loader now restores the default output node, wired to
// the sheet's xsheet node as a new sheet is, so the sub-xsheet renders like
// any other. This test saves a scene with a sub-xsheet, removes the
// sub-xsheet's <output> entries from the file, loads it and builds the FX
// tree.

#include "toonz/fxdag.h"
#include "toonz/levelset.h"
#include "toonz/preferences.h"
#include "toonz/tcolumnfx.h"
#include "toonz/scenefx.h"
#include "toonz/toonzscene.h"
#include "toonz/tproject.h"
#include "toonz/txshcell.h"
#include "toonz/txshchildlevel.h"
#include "toonz/txshleveltypes.h"
#include "toonz/txshsimplelevel.h"
#include "toonz/txsheet.h"
#include "tenv.h"
#include "texception.h"
#include "tfilepath.h"
#include "tfx.h"
#include "tconvert.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QRegularExpression>
#include <QTemporaryDir>

#include <iostream>
#include <stdexcept>
#include <string>

namespace {

void require(bool condition, const std::string& message) {
  if (!condition) throw std::runtime_error(message);
}

// A scene whose first column exposes a sub-xsheet holding one vector level
QByteArray writeSceneText(const QString& dir) {
  ToonzScene scene;
  TProjectManager::instance()->initializeScene(&scene);

  auto* child = new TXshChildLevel(L"Sub");
  child->setScene(&scene);
  scene.getLevelSet()->insertLevel(child);
  scene.getXsheet()->setCell(0, 0, TXshCell(child, TFrameId(1)));

  TXshLevel* drawing = scene.createNewLevel(PLI_XSHLEVEL, L"Drawing");
  require(drawing, "cannot create a vector level");
  child->getXsheet()->setCell(0, 0, TXshCell(drawing, TFrameId(1)));
  scene.getXsheet()->updateFrameCount();

  const QString path = QDir(dir).filePath("with-output.tnz");
  scene.saveCopy(TFilePath(path.toStdWString()));
  QFile file(path);
  require(file.open(QIODevice::ReadOnly), "cannot read the saved scene");
  return file.readAll();
}

// Removes every <output> entry that follows the sub-xsheet's <fxnodes> tag,
// leaving the top sheet's own entry alone. Fails when the writer's layout
// changes, so the test can't pass on the unedited scene.
QByteArray dropChildOutputs(const QByteArray& text) {
  const int childAt = text.indexOf("<childLevel");
  require(childAt >= 0, "the saved scene has no sub-xsheet");
  const int childEnd = text.indexOf("</childLevel>", childAt);
  require(childEnd > childAt, "the sub-xsheet is not closed");
  QString child = QString::fromUtf8(text.mid(childAt, childEnd - childAt));
  const QRegularExpression output(
      "<output>.*?</output>", QRegularExpression::DotMatchesEverythingOption);
  const int before = child.count("<output>");
  require(before > 0, "the sub-xsheet was saved without an output entry");
  child.remove(output);
  require(!child.contains("<output>"), "an output entry is left");
  return text.left(childAt) + child.toUtf8() + text.mid(childEnd);
}

int outputCount(TXsheet* xsheet) {
  return xsheet->getFxDag()->getOutputFxCount();
}

void loadAndBuild(const QString& dir) {
  const QByteArray edited = dropChildOutputs(writeSceneText(dir));
  const QString path      = QDir(dir).filePath("without-output.tnz");
  QFile file(path);
  require(file.open(QIODevice::WriteOnly | QIODevice::Truncate) &&
              file.write(edited) == edited.size(),
          "cannot write the edited scene");
  file.close();

  ToonzScene scene;
  scene.loadTnzFile(TFilePath(path.toStdWString()));
  TXsheet* top          = scene.getXsheet();
  TXshChildLevel* child = top->getCell(0, 0).getChildLevel();
  require(child, "the top sheet lost its sub-xsheet");
  TXsheet* sub = child->getXsheet();

  require(outputCount(sub) == 1,
          "the sub-xsheet has " + std::to_string(outputCount(sub)) +
              " output nodes; the loader must restore the default one");
  FxDag* dag = sub->getFxDag();
  require(
      dag->getCurrentOutputFx()->getInputPort(0)->getFx() == dag->getXsheetFx(),
      "the restored output is not wired to the sub-xsheet's xsheet node");
  require(outputCount(top) == 1, "the top sheet's own output was changed");

  TFxP subFx = buildSceneFx(&scene, sub, 0, 1, false);
  require(subFx.getPointer() != nullptr,
          "the sub-xsheet builds no FX tree from its restored output");
  TFxP sceneFx = buildSceneFx(&scene, 0, 1, false);
  require(sceneFx.getPointer() != nullptr,
          "the scene builds no FX tree around the sub-xsheet");
  std::cout << "  sub-xsheet without <output>: loaded with a default output "
               "and built its FX tree"
            << std::endl;
}

}  // namespace

int main(int argc, char* argv[]) {
  QCoreApplication application(argc, argv);
  try {
    QTemporaryDir storage;
    require(storage.isValid(), "cannot create a temporary directory");
    const QString stuff = QDir(storage.path()).filePath("stuff");
    require(
        QDir().mkpath(QDir(stuff).filePath("profiles/layouts/rooms/Default")),
        "cannot create the stuff directory");
    TEnv::setStuffDir(TFilePath(stuff.toStdWString()));
    Preferences::instance()->setValue(compressSceneFiles, false);
    loadAndBuild(storage.path());
  } catch (const std::exception& error) {
    std::cerr << "FAIL: " << error.what() << std::endl;
    return 1;
  } catch (const TException& error) {
    std::cerr << "FAIL: " << ::to_string(error.getMessage()) << std::endl;
    return 1;
  }
  std::cout << "subxsheet_output: PASS" << std::endl;
  return 0;
}
