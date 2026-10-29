#include "nativeuiintegrationtest.h"

#include "columnselection.h"
#include "commandpalette.h"
#include "languagemanager.h"
#include "mainwindow.h"
#include "menubarcommandids.h"
#include "pane.h"
#include "tapp.h"
#include "themeapplication.h"
#include "widgetinspector.h"
#include "xsheetviewer.h"

#include "tenv.h"
#include "tundo.h"
#include "tvectorimage.h"
#include "toonz/preferences.h"
#include "toonz/tcolumnhandle.h"
#include "toonz/tframehandle.h"
#include "toonz/toonzfolders.h"
#include "toonz/toonzscene.h"
#include "toonz/tscenehandle.h"
#include "toonz/txshcell.h"
#include "toonz/txshcolumn.h"
#include "toonz/txsheet.h"
#include "toonz/txsheethandle.h"
#include "toonz/txshleveltypes.h"
#include "toonz/txshsimplelevel.h"
#include "toonzqt/menubarcommand.h"
#include "toonzqt/tselectionhandle.h"

#include <QApplication>
#include <QComboBox>
#include <QDir>
#include <QDirIterator>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QKeyEvent>
#include <QLineEdit>
#include <QMainWindow>
#include <QPointer>
#include <QSettings>
#include <QTreeWidget>

#include <iostream>
#include <memory>

namespace {
QString testRoot;

struct Failure {
  const char *message;
};

void require(bool condition, const char *message) {
  if (!condition) throw Failure{message};
}

void settle() {
  // Give posted native selection, translation and layout updates a turn. There
  // is no animation playback, external work, waiting for input or polling loop.
  QCoreApplication::sendPostedEvents();
  QCoreApplication::processEvents(QEventLoop::ExcludeUserInputEvents, 20);
}

bool below(const QString &path, const QString &root) {
  const QString resolved = QFileInfo(path).canonicalFilePath();
  return !resolved.isEmpty() && resolved.startsWith(root + '/');
}

QAction *nativeAction(const char *id) {
  QAction *action = CommandManager::instance()->getAction(id);
  require(action, "native action registration missing");
  return action;
}

void trigger(const char *id) {
  QAction *action = nativeAction(id);
  require(action->isEnabled(), "required native action is disabled");
  action->trigger();
  settle();
}

CommandPalette *openPalette() {
  trigger(MI_OpenCommandPalette);
  auto *room = TApp::instance()->getCurrentRoom();
  require(room, "native room missing");
  const auto palettes = room->findChildren<CommandPalette *>();
  require(palettes.size() == 1, "palette factory did not reuse one instance");
  CommandPalette *palette = palettes.front();
  require(palette->isVisible(), "registered palette did not become visible");
  return palette;
}

QTreeWidgetItem *search(CommandPalette *palette, const char *id) {
  auto *edit = palette->findChild<QLineEdit *>("commandPaletteSearch");
  auto *results = palette->findChild<QTreeWidget *>("commandPaletteResults");
  require(edit && results, "native palette controls missing");
  edit->setText(QString::fromLatin1(id));
  palette->refreshCommands();
  for (int i = 0; i < results->topLevelItemCount(); ++i) {
    auto *item = results->topLevelItem(i);
    if (item->data(0, Qt::UserRole).toString() == QLatin1String(id)) {
      results->setCurrentItem(item);
      return item;
    }
  }
  return nullptr;
}

bool runnable(CommandPalette *palette, const char *id) {
  auto *item = search(palette, id);
  return item && (item->flags() & Qt::ItemIsEnabled);
}

void sendKey(CommandPalette *palette, int key) {
  auto *edit = palette->findChild<QLineEdit *>("commandPaletteSearch");
  require(edit, "palette search field missing");
  QKeyEvent press(QEvent::KeyPress, key, Qt::NoModifier);
  QApplication::sendEvent(edit, &press);
  settle();
}

QWidget *preferencesPopup() {
  trigger(MI_Preferences);
  for (QWidget *widget : QApplication::allWidgets())
    if (widget->objectName() == QLatin1String("PreferencesPopup") &&
        widget->isVisible())
      return widget;
  throw Failure{"native Preferences popup missing"};
}

QComboBox *comboWithValue(QWidget *parent, const QString &value) {
  QComboBox *found = nullptr;
  for (auto *combo : parent->findChildren<QComboBox *>()) {
    if (combo->findData(value) < 0) continue;
    require(!found, "ambiguous native Preferences control");
    found = combo;
  }
  require(found, "required native Preferences choice missing");
  return found;
}

void choose(QComboBox *combo, const QString &value) {
  const int index = combo->findData(value);
  require(index >= 0, "native Preferences value unavailable");
  combo->setCurrentIndex(index);
  settle();
}

void exerciseNativeUi(QMainWindow *window) {
  require(window && window->isVisible(), "application window is not visible");
  require(TEnv::getIsPortable(), "test application is not portable");
  require(QFileInfo(TEnv::getStuffDir().getQString()).canonicalFilePath() ==
              testRoot + "/portablestuff",
          "native stuff root escaped isolation");
  require(below(ToonzFolder::getMyModuleDir().getQString(), testRoot),
          "native preferences escaped isolation");
  require(below(TEnv::getConfigDir().getQString(), testRoot),
          "native configuration escaped isolation");
  require(below(ToonzFolder::getCacheRootFolder().getQString(), testRoot),
          "native cache escaped isolation");
  require(below(ToonzFolder::getCrashReportFolder().getQString(), testRoot),
          "native crash reports escaped isolation");
  require(!QApplication::activeModalWidget(), "unexpected startup modal dialog");

  TApp *app = TApp::instance();
  ToonzScene *scene = app->getCurrentScene()->getScene();
  TXsheet *xsheet = app->getCurrentXsheet()->getXsheet();
  require(scene && xsheet, "native startup did not create a disposable scene");
  require(xsheet->getFrameCount() == 0, "test scene is not empty");

  // Test the real action registry and factory, including current Animate IDs.
  auto *palette = openPalette();
  require(!runnable(palette, MI_ToggleColumnOutline),
          "outline command was runnable without a drawing target");
  require(!runnable(palette, MI_MakeUniqueStyle),
          "unique style command was runnable without a picked gradient");
  require(search(palette, MI_OpenRigLibrary), "native Library action missing");
  require(search(palette, MI_OpenMotionPresets),
          "native Motion Presets action missing");
  require(search(palette, "T_Pump"), "native Width/Pump action missing");
  require(openPalette() == palette, "reopening replaced the native palette");
  sendKey(palette, Qt::Key_Escape);

  // Use the Xsheet's own selection, not a stand-in QObject or fake handler.
  trigger(MI_OpenXshView);
  XsheetViewer *viewer = nullptr;
  for (auto *candidate : app->getCurrentRoom()->findChildren<XsheetViewer *>())
    if (candidate->isVisible()) {
      viewer = candidate;
      break;
    }
  require(viewer, "native Xsheet viewer is missing");
  viewer->setFocus(Qt::OtherFocusReason);
  settle();
  scene->setSceneName(L"NativeUiFixtureScene");
  auto *level = scene->createNewLevel(PLI_XSHLEVEL, L"NativeUiFixture")
                    ->getSimpleLevel();
  require(level, "disposable vector level was not created");
  level->setFrame(TFrameId(1), TVectorImageP(new TVectorImage));
  const TXshCell cell(level, TFrameId(1));
  for (int col = 0; col < 3; ++col)
    require(xsheet->setCell(0, col, cell), "disposable cell was not inserted");
  app->getCurrentXsheet()->notifyXsheetChanged();
  app->getCurrentColumn()->setColumnIndex(1);
  TColumnSelection *selection = viewer->getColumnSelection();
  selection->selectNone();
  selection->selectColumn(0);
  selection->selectColumn(2);
  selection->makeCurrent();
  const auto selectedColumns = selection->getIndices();
  const int currentFrame = app->getCurrentFrame()->getFrame();
  TUndoManager *undo = TUndoManager::manager();
  undo->reset();
  app->getCurrentScene()->setDirtyFlag(false);

  const auto selectionUnchanged = [&]() {
    require(app->getCurrentSelection()->getSelection() == selection &&
                selection->getIndices() == selectedColumns &&
                app->getCurrentColumn()->getColumnIndex() == 1 &&
                app->getCurrentFrame()->getFrame() == currentFrame &&
                app->getCurrentScene()->getScene() == scene &&
                app->getCurrentXsheet()->getXsheet() == xsheet,
            "palette or appearance change altered native selection context");
  };
  palette = openPalette();
  selectionUnchanged();
  sendKey(palette, Qt::Key_Escape);
  selectionUnchanged();
  require(undo->getHistoryCount() == 0 &&
              !app->getCurrentScene()->getDirtyFlag(),
          "opening and cancelling a palette modified the document");

  palette = openPalette();
  require(runnable(palette, MI_ToggleColumnOutline),
          "valid native outline selection was not runnable");
  int invocations = 0;
  QObject observer;
  QObject::connect(nativeAction(MI_ToggleColumnOutline), &QAction::triggered,
                   &observer, [&]() { ++invocations; });
  sendKey(palette, Qt::Key_Return);
  selectionUnchanged();
  require(invocations == 1 && undo->getHistoryCount() == 1,
          "palette did not execute exactly one native undoable command");
  require(xsheet->getColumn(0)->isOutlineView() &&
              !xsheet->getColumn(1)->isOutlineView() &&
              xsheet->getColumn(2)->isOutlineView(),
          "outline action used focus/current column instead of selected columns");
  undo->undo();
  require(!xsheet->getColumn(0)->isOutlineView() &&
              !xsheet->getColumn(2)->isOutlineView(),
          "native outline undo failed");
  undo->redo();
  require(xsheet->getColumn(0)->isOutlineView() &&
              xsheet->getColumn(2)->isOutlineView(),
          "native outline redo failed");

  // Run the actual Preferences controls and persistence adapter. Packaged
  // Japanese catalogs are required: a missing catalog is a failure, not a skip.
  const QString originalLanguage = UiLanguageManager::instance()->currentLanguage();
  const QString originalTheme = NativeTheme::instance().currentTheme();
  QWidget *preferences = preferencesPopup();
  const QString japanese = QString::fromUtf8("日本語");
  auto *language = comboWithValue(preferences, japanese);
  auto *theme = comboWithValue(preferences, QStringLiteral("Synthwave"));
  choose(language, "English");
  QPointer<QAction> preferencesAction = nativeAction(MI_Preferences);
  const QString englishText = preferencesAction->text();
  viewer->changeWindowTitle();
  const QString englishXsheetTitle = viewer->parentWidget()->windowTitle();
  require(englishXsheetTitle.contains("NativeUiFixtureScene"),
          "native Xsheet title did not include the document name");
  auto *room = dynamic_cast<Room *>(app->getCurrentRoom());
  require(room, "native room type is missing");
  const QString roomName = room->getName();
  std::unique_ptr<TPanel> customTitlePanel(new TPanel(room));
  // Deliberately matches an existing translated caption but has no UI source
  // identity: a custom caption must never be translated by its spelling.
  customTitlePanel->setWindowTitle(QString::fromUtf8("Schematic"));
  const QKeySequence shortcut = preferencesAction->shortcut();
  const int historyCount = undo->getHistoryCount();
  const int historyIndex = undo->getCurrentHistoryIndex();
  const bool dirty = app->getCurrentScene()->getDirtyFlag();
  choose(language, japanese);
  require(UiLanguageManager::instance()->currentLanguage() == japanese &&
              Preferences::instance()->getCurrentLanguage() == japanese,
          "Preferences language choice did not update native persisted state");
  QSettings savedPreferences(
      (ToonzFolder::getMyModuleDir() + TFilePath("preferences.ini")).getQString(),
      QSettings::IniFormat);
  savedPreferences.sync();
  require(savedPreferences.status() == QSettings::NoError &&
              savedPreferences.value("CurrentLanguageName").toString() == japanese,
          "native language preference was not saved in the isolated profile");
  require(preferencesAction &&
              nativeAction(MI_Preferences) == preferencesAction.data() &&
              preferencesAction->text() != englishText &&
              preferencesAction->shortcut() == shortcut,
          "live native action translation failed or replaced its identity");
  require(viewer->parentWidget()->windowTitle() != englishXsheetTitle &&
              viewer->parentWidget()->windowTitle().contains("NativeUiFixtureScene") &&
              scene->getSceneName() == L"NativeUiFixtureScene" &&
              room->getName() == roomName &&
              customTitlePanel->windowTitle() == "Schematic",
          "live native panel title failed or changed a document, room or custom name");
  selectionUnchanged();
  choose(theme, "Synthwave");
  require(NativeTheme::instance().currentTheme() == "Synthwave" &&
              !qApp->styleSheet().isEmpty(),
          "Preferences did not apply the packaged native Synthwave theme");
  savedPreferences.sync();
  require(savedPreferences.status() == QSettings::NoError &&
              savedPreferences.value("CurrentStyleSheetName").toString() == "Synthwave",
          "native theme preference was not saved in the isolated profile");
  selectionUnchanged();
  choose(language, originalLanguage);
  choose(theme, originalTheme);
  preferences->hide();
  require(undo->getHistoryCount() == historyCount &&
              undo->getCurrentHistoryIndex() == historyIndex &&
              app->getCurrentScene()->getDirtyFlag() == dirty,
          "appearance changes modified the document or its undo history");

  palette = openPalette();
  auto *edit = palette->findChild<QLineEdit *>("commandPaletteSearch");
  trigger(MI_OpenWidgetInspector);
  auto inspectors = app->getCurrentRoom()->findChildren<WidgetInspector *>();
  require(inspectors.size() == 1 && inspectors.front()->isVisible(),
          "registered native inspector factory failed");
  WidgetInspector *inspector = inspectors.front();
  inspector->inspectWidget(edit);
  require(inspector->inspectedWidget() == edit,
          "native inspector did not inspect a real palette control");
  inspector->beginPicking();
  require(inspector->isPicking(), "native inspector did not start its picker");
  inspector->cancelPicking();
  require(!inspector->isPicking(), "native inspector left a picker active");
  selectionUnchanged();
  require(undo->getHistoryCount() == historyCount,
          "native inspector changed document undo history");
  inspector->clearInspection();
  // No close/save prompts or fixture files: normal application teardown follows.
  selection->makeNotCurrent();
  app->getCurrentScene()->setDirtyFlag(false);
  undo->reset();
}
}  // namespace

bool prepareNativeUiIntegrationTest(QString *error) {
  const auto reject = [error](const char *reason) {
    if (error) *error = QString::fromLatin1(reason);
    return false;
  };
  const QString root = QFileInfo(QDir::currentPath()).canonicalFilePath();
  const QString temporary = QFileInfo(QDir::tempPath()).canonicalFilePath();
  if (root.isEmpty() || temporary.isEmpty() ||
      !root.startsWith(temporary + '/'))
    return reject("native UI test requires a disposable temporary directory");
  const QFileInfo marker(QDir(root).filePath(".groundzero-native-ui-test"));
  QFile markerFile(marker.filePath());
  if (marker.isSymLink() || !markerFile.open(QIODevice::ReadOnly) ||
      markerFile.readAll() != QByteArray("groundzero-native-ui-test\n"))
    return reject("native UI test isolation marker is missing or invalid");
  const QString stuff = QDir(root).filePath("portablestuff");
  if (!QFileInfo(stuff).isDir() || QFileInfo(stuff).isSymLink())
    return reject("native UI test needs a copied portable stuff directory");
  QDirIterator entries(stuff, QDir::AllEntries | QDir::Hidden |
                                 QDir::System | QDir::NoDotAndDotDot,
                       QDirIterator::Subdirectories);
  while (entries.hasNext()) {
    entries.next();
    if (entries.fileInfo().isSymLink())
      return reject("native UI test portable data cannot contain symlinks");
  }
  for (const QString &part : {QString("config"), QString("profiles"),
                             QString("projects")})
    if (!QFileInfo(QDir(stuff).filePath(part)).isDir())
      return reject("native UI test portable data is incomplete");
  const QString settings = QDir(root).filePath("qt-settings");
  if (!QDir().mkpath(settings))
    return reject("native UI test cannot create isolated settings");
  QSettings::setDefaultFormat(QSettings::IniFormat);
  QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settings);
  QSettings::setPath(QSettings::IniFormat, QSettings::SystemScope, settings);
  ToonzFolder::setTestStorageRoot(TFilePath(root));
  testRoot = root;
  if (error) error->clear();
  return true;
}

int runNativeUiIntegrationTest(QMainWindow *window) {
  try {
    require(!testRoot.isEmpty(), "native UI test was not prepared before startup");
    exerciseNativeUi(window);
    std::cout << "native-ui-integration: PASS\n";
    return 0;
  } catch (const Failure &failure) {
    std::cerr << "native-ui-integration: FAIL: " << failure.message << '\n';
  } catch (...) {
    std::cerr << "native-ui-integration: FAIL: unexpected native exception\n";
  }
  return 1;
}
