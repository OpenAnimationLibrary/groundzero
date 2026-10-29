#include "seexprnativeintegrationtest.h"

#include "columncommand.h"
#include "expressionreferencemanager.h"
#include "subscenecommand.h"
#include "tapp.h"

#include "tdoubleparam.h"
#include "tdoublekeyframe.h"
#include "tenv.h"
#include "texpression.h"
#include "tundo.h"
#include "tvectorimage.h"
#include "toonz/preferences.h"
#include "toonz/expressionreferencemonitor.h"
#include "toonz/tcolumnhandle.h"
#include "toonz/tframehandle.h"
#include "toonz/tstageobject.h"
#include "toonz/toonzfolders.h"
#include "toonz/toonzscene.h"
#include "toonz/tscenehandle.h"
#include "toonz/txshcell.h"
#include "toonz/txshchildlevel.h"
#include "toonz/txshcolumn.h"
#include "toonz/txsheet.h"
#include "toonz/txsheethandle.h"
#include "toonz/txshleveltypes.h"
#include "toonz/txshsimplelevel.h"
#include "toonzqt/dvdialog.h"
#include "toonzqt/stageobjectsdata.h"

#include <QApplication>
#include <QClipboard>
#include <QDir>
#include <QDirIterator>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QMainWindow>
#include <QPushButton>
#include <QRadioButton>
#include <QSettings>
#include <QTimer>

#include <cmath>
#include <iostream>
#include <set>

namespace {
QString testRoot;

struct Failure { const char *message; };
void require(bool condition, const char *message) {
  if (!condition) throw Failure{message};
}
void checkpoint(const char *stage) {
  std::cerr << "seexpr-native-integration: " << stage << std::endl;
}
void near(double actual, double expected, const char *message) {
  require(std::isfinite(actual) && std::abs(actual - expected) < 1e-8, message);
}
void settle() {
  QCoreApplication::sendPostedEvents();
  QCoreApplication::processEvents(QEventLoop::ExcludeUserInputEvents, 20);
}
bool below(const QString &path, const QString &root) {
  const QString resolved = QFileInfo(path).canonicalFilePath();
  const QString parent = QFileInfo(root).canonicalFilePath();
  if (resolved.isEmpty() || parent.isEmpty()) return false;
  const QString relative = QDir::fromNativeSeparators(QDir(parent).relativeFilePath(resolved));
  return !relative.isEmpty() && relative != "." && relative != ".." &&
         !relative.startsWith("../") && !QDir::isAbsolutePath(relative);
}

TDoubleParam *channel(TXsheet *xsheet, int column) {
  return xsheet->getStageObject(TStageObjectId::ColumnId(column))
      ->getParam(TStageObject::T_X);
}
void keys(TDoubleParam *curve, const std::string &expression,
          double start = 0, double end = 0) {
  // Keep the native stage-channel measure. Scene loading reconstructs those
  // units, so the fixture must exercise the same conversions before and after
  // serialization instead of relying on an unsaved measure-name override.
  TDoubleKeyframe first(0, start), last(3, end);
  first.m_prevType = TDoubleKeyframe::None;
  first.m_type = expression.empty() ? TDoubleKeyframe::Linear : TDoubleKeyframe::Expression;
  first.m_expressionText = expression;
  last.m_prevType = first.m_type;
  last.m_type = TDoubleKeyframe::Linear;
  curve->setKeyframe(first);
  curve->setKeyframe(last);
}

const std::string seExprSource =
    "seexpr1:$caption=\"col1.x\"; channel(\"col1.x\") * 2";

void checkValues(TXsheet *xsheet, int input, int expression, int dependent) {
  near(channel(xsheet, expression)->getValue(1), channel(xsheet, input)->getValue(1) * 2,
       "SeExpr reference changed its value during native column operation");
  near(channel(xsheet, dependent)->getValue(1), channel(xsheet, expression)->getValue(1) * 3,
       "mixed native/SeExpr dependency changed its value during native operation");
}
void checkOriginal(TXsheet *xsheet) {
  require(xsheet->getColumnCount() == 3, "native undo did not restore three original columns");
  require(channel(xsheet, 1)->getKeyframe(0).m_expressionText == seExprSource,
          "native operation did not preserve or restore original SeExpr source");
  require(channel(xsheet, 2)->getKeyframe(0).m_expressionText == "col2.x*3",
          "native operation did not restore the original native reference");
  checkValues(xsheet, 0, 1, 2);
}
void undo(TUndoManager *manager) {
  require(manager->undo(), "native operation did not provide an undo transaction");
  settle();
}
void redo(TUndoManager *manager) {
  require(manager->redo(), "native operation did not provide a redo transaction");
  settle();
}

void exerciseNativeReferenceLifecycle(QMainWindow *window) {
  require(window && window->isVisible(), "native main window is not visible");
  require(TEnv::getIsPortable(), "native fixture did not start in portable mode");
  require(below(TEnv::getStuffDir().getQString(), testRoot) &&
              below(ToonzFolder::getMyModuleDir().getQString(), testRoot) &&
              below(TEnv::getConfigDir().getQString(), testRoot) &&
              below(ToonzFolder::getCacheRootFolder().getQString(), testRoot) &&
              below(ToonzFolder::getCrashReportFolder().getQString(), testRoot),
          "native application storage escaped the disposable fixture");
  require(!QApplication::activeModalWidget(), "unexpected startup modal dialog");
  TApp *app = TApp::instance();
  ToonzScene *scene = app->getCurrentScene()->getScene();
  TXsheet *xsheet = app->getCurrentXsheet()->getXsheet();
  require(scene && xsheet && xsheet->getFrameCount() == 0,
          "native test did not start with an empty disposable scene");
  checkpoint("creating actual vector columns and mixed expression curves");
  scene->setSceneName(L"SeExprNativeFixture");
  TXshSimpleLevel *level = scene->createNewLevel(PLI_XSHLEVEL, L"SeExprNativeDrawing")
                              ->getSimpleLevel();
  require(level, "native fixture could not create its disposable vector level");
  level->setFrame(TFrameId(1), TVectorImageP(new TVectorImage));
  const TXshCell cell(level, TFrameId(1));
  for (int c = 0; c < 3; ++c)
    for (int row = 0; row < 4; ++row)
      require(xsheet->setCell(row, c, cell), "native fixture could not insert an exposure");
  keys(channel(xsheet, 0), "", 2, 8);
  keys(channel(xsheet, 1), seExprSource);
  keys(channel(xsheet, 2), "col2.x*3");
  Preferences::instance()->setValue(modifyExpressionOnMovingReferences, true, false);
  app->getCurrentScene()->notifyPreferenceChanged("modifyExpressionOnMovingReferences");
  app->getCurrentXsheet()->notifyXsheetChanged();
  app->getCurrentFrame()->setFrame(0);
  app->getCurrentColumn()->setColumnIndex(0);
  settle();
  auto *monitor = ExpressionReferenceManager::instance();
  monitor->refreshXsheetRefInfo(xsheet);
  TUndoManager *history = TUndoManager::manager();
  history->reset();
  checkOriginal(xsheet);

  checkpoint("moving actual columns through the native reference observer");
  xsheet->moveColumn(0, 2);
  app->getCurrentXsheet()->notifyXsheetChanged();
  settle();
  const std::string moved = channel(xsheet, 0)->getKeyframe(0).m_expressionText;
  require(moved.find("channel(\"col3.x\")") != std::string::npos,
          "native column movement did not rewrite the SeExpr reference literal");
  require(moved.find("$caption=\"col1.x\"") != std::string::npos,
          "native column movement changed unrelated SeExpr string data");
  checkValues(xsheet, 2, 0, 1);
  xsheet->moveColumn(2, 0);
  app->getCurrentXsheet()->notifyXsheetChanged();
  settle();
  checkOriginal(xsheet);

  checkpoint("inserting columns with native undo and redo");
  ColumnCmd::insertEmptyColumn(0);
  settle();
  require(xsheet->getColumnCount() == 4, "native insert command did not add a column");
  require(channel(xsheet, 2)->getKeyframe(0).m_expressionText.find("channel(\"col2.x\")") != std::string::npos,
          "native insertion did not update the SeExpr channel index");
  checkValues(xsheet, 1, 2, 3);
  undo(history); checkOriginal(xsheet);
  redo(history); checkValues(xsheet, 1, 2, 3);
  undo(history); checkOriginal(xsheet);

  checkpoint("deleting a referenced column and restoring external formulas with undo");
  ColumnCmd::deleteColumn(0, true);
  settle();
  require(xsheet->getColumnCount() == 2, "native delete command did not remove its target");
  require(monitor->isIgnored(channel(xsheet, 0)) ||
              channel(xsheet, 0)->getKeyframe(0).m_expressionText.find('?') != std::string::npos,
          "deleted reference was silently rebound without a repair diagnostic");
  undo(history); checkOriginal(xsheet);
  redo(history);
  require(xsheet->getColumnCount() == 2, "native deletion redo did not remove its target");
  undo(history); checkOriginal(xsheet);

  checkpoint("copying and pasting real columns through the native clipboard");
  ColumnCmd::copyColumns({0, 1, 2});
  require(dynamic_cast<const StageObjectsData *>(QApplication::clipboard()->mimeData()),
          "native copy command did not provide native column clipboard data");
  std::set<int> destination{3};
  ColumnCmd::pasteColumns(destination);
  settle();
  require(xsheet->getColumnCount() == 6, "native clipboard paste did not duplicate all columns");
  require(channel(xsheet, 4)->getKeyframe(0).m_expressionText.find("channel(\"col4.x\")") != std::string::npos,
          "native pasted SeExpr formula retained its original source-column binding");
  checkValues(xsheet, 3, 4, 5);
  undo(history); checkOriginal(xsheet);
  redo(history); checkValues(xsheet, 3, 4, 5);
  undo(history); checkOriginal(xsheet);
  QApplication::clipboard()->clear();

  checkpoint("collapsing actual mixed-expression columns into a sub-xsheet");
  std::set<int> collapse{0, 1, 2};
  SubsceneCmd::collapse(collapse, L"SeExprNativeChild");
  settle();
  TXshChildLevel *child = xsheet->getCell(0, 0).getChildLevel();
  require(child && child->getXsheet(), "native collapse did not create a sub-xsheet level");
  checkValues(child->getXsheet(), 0, 1, 2);
  require(child->getXsheet()->getExpRefMonitor()->info().contains(channel(child->getXsheet(), 1)) &&
              !xsheet->getExpRefMonitor()->info().contains(channel(child->getXsheet(), 1)),
          "native collapse registered a child formula in the parent monitor");
  require(channel(child->getXsheet(), 1)->getKeyframe(0).m_expressionText == seExprSource,
          "native sub-xsheet collapse changed literal data or expression source");
  undo(history); checkOriginal(xsheet);
  redo(history);
  child = xsheet->getCell(0, 0).getChildLevel();
  require(child, "native collapse redo did not restore the sub-xsheet");
  checkValues(child->getXsheet(), 0, 1, 2);

  checkpoint("exploding the real sub-xsheet through its confirmation dialog");
  int confirmed = 0;
  QTimer confirmation;
  confirmation.setInterval(5);
  QObject::connect(&confirmation, &QTimer::timeout, window, [&]() {
    for (QWidget *widget : QApplication::topLevelWidgets()) {
      auto *dialog = qobject_cast<DVGui::RadioButtonDialog *>(widget);
      if (!dialog || !dialog->isVisible()) continue;
      for (QPushButton *button : dialog->findChildren<QPushButton *>()) {
        if (button->text() == QObject::tr("Apply")) {
          ++confirmed;
          button->click();
          return;
        }
      }
    }
  });
  confirmation.start();
  SubsceneCmd::explode(0);
  confirmation.stop();
  settle();
  require(confirmed == 1, "native explode confirmation was not exercised exactly once");
  checkOriginal(xsheet);
  require(xsheet->getExpRefMonitor()->info().contains(channel(xsheet, 1)),
          "native explode did not register its restored formula in the parent monitor");
  undo(history);
  child = xsheet->getCell(0, 0).getChildLevel();
  require(child, "native explode undo did not restore the sub-xsheet");
  checkValues(child->getXsheet(), 0, 1, 2);
  redo(history); checkOriginal(xsheet);

  checkpoint("saving and reopening the native mixed-reference scene data");
  const TFilePath scenePath(QDir(testRoot).filePath("native-references.tnz"));
  const bool compressed = Preferences::instance()->getBoolValue(compressSceneFiles);
  Preferences::instance()->setValue(compressSceneFiles, false, false);
  scene->saveCopy(scenePath);
  Preferences::instance()->setValue(compressSceneFiles, compressed, false);
  QFile saved(scenePath.getQString());
  require(saved.open(QIODevice::ReadOnly), "native reference scene was not saved");
  require(saved.readAll().contains("SeExpr1"),
          "native reference scene did not preserve the versioned SeExpr dialect");
  saved.close();
  {
    // Read the real scene graph and curves, without loading drawing resources
    // or replacing the active GUI document. The native batch fixture separately
    // exercises saved effect resources and final image output.
    ToonzScene reopened;
    reopened.loadTnzFile(scenePath);
    checkOriginal(reopened.getXsheet());
  }

  checkpoint("clearing disposable history and verifying document identity");
  require(app->getCurrentScene()->getScene() == scene &&
              app->getCurrentXsheet()->getXsheet() == xsheet &&
              xsheet->getCell(0, 0).m_level.getPointer() == level,
          "native reference operations changed the active document or original drawing");
  Preferences::instance()->setValue(modifyExpressionOnMovingReferences, false, false);
  app->getCurrentScene()->notifyPreferenceChanged("modifyExpressionOnMovingReferences");
  scene->setSceneName(L"SeExprNativeFixture");
  app->getCurrentScene()->setDirtyFlag(false);
  history->reset();
}
}  // namespace

bool prepareSeExprNativeIntegrationTest(QString *error) {
  const auto reject = [error](const char *message) {
    if (error) *error = QString::fromLatin1(message);
    return false;
  };
  const QString root = QFileInfo(QDir::currentPath()).canonicalFilePath();
  if (!below(root, QFileInfo(QDir::tempPath()).canonicalFilePath()))
    return reject("SeExpr native test requires a disposable temporary directory");
  QFileInfo marker(QDir(root).filePath(".groundzero-seexpr-native-test"));
  QFile file(marker.filePath());
  if (marker.isSymLink() || !file.open(QIODevice::ReadOnly) ||
      file.readAll() != QByteArray("groundzero-seexpr-native-test\n"))
    return reject("SeExpr native test isolation marker is missing or invalid");
  const QString stuff = QDir(root).filePath("portablestuff");
  if (!QFileInfo(stuff).isDir() || QFileInfo(stuff).isSymLink())
    return reject("SeExpr native test requires a copied portable stuff directory");
  QDirIterator entries(stuff, QDir::AllEntries | QDir::Hidden | QDir::System |
                                 QDir::NoDotAndDotDot, QDirIterator::Subdirectories);
  while (entries.hasNext()) {
    entries.next();
    if (entries.fileInfo().isSymLink())
      return reject("SeExpr native test portable data cannot contain symbolic links");
  }
  for (const QString &name : {QString("config"), QString("profiles"), QString("projects")})
    if (!QFileInfo(QDir(stuff).filePath(name)).isDir())
      return reject("SeExpr native test portable data is incomplete");
  const QString settings = QDir(root).filePath("qt-settings");
  if (!QDir().mkpath(settings)) return reject("SeExpr native test cannot create isolated settings");
  QSettings::setDefaultFormat(QSettings::IniFormat);
  QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settings);
  QSettings::setPath(QSettings::IniFormat, QSettings::SystemScope, settings);
  ToonzFolder::setSeExprTestStorageRoot(TFilePath(root));
  testRoot = root;
  checkpoint("disposable native application storage prepared");
  if (error) error->clear();
  return true;
}

int runSeExprNativeIntegrationTest(QMainWindow *window) {
  try {
    require(!testRoot.isEmpty(), "native SeExpr fixture was not prepared before startup");
    exerciseNativeReferenceLifecycle(window);
    std::cout << "seexpr-native-integration: PASS" << std::endl;
    return 0;
  } catch (const Failure &failure) {
    std::cerr << "seexpr-native-integration: FAIL: " << failure.message << std::endl;
  } catch (...) {
    std::cerr << "seexpr-native-integration: FAIL: unexpected native exception" << std::endl;
  }
  return 1;
}
