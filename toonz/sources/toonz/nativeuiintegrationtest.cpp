#include "nativeuiintegrationtest.h"

#include "columnselection.h"
#include "commandpalette.h"
#include "convertpopup.h"
#include "drawingsubstitutionpane.h"
#include "filmstrip.h"
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
#include "toonz/levelset.h"
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
#include "toonzqt/uitranslations.h"

#include <QApplication>
#include <QComboBox>
#include <QDir>
#include <QDirIterator>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QImage>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QMainWindow>
#include <QPointer>
#include <QSettings>
#include <QTemporaryDir>
#include <QTreeWidget>
#include <QTranslator>

#include <iostream>
#include <memory>

namespace {
QString testRoot;

void checkpoint(const char *stage) {
  // Fixed labels only: flush each stage even if a native callback terminates
  // the process before the fixture can report its final result.
  std::cerr << "native-ui-integration: checkpoint: " << stage << std::endl;
}

// This recently added source is unfinished in the shipped catalogs. Supply one
// distinct test caption for this exact native source so equal-text provenance
// is observable; every other caption still uses the installed real catalogs.
class EmptyCellCaptionTranslator final : public QTranslator {
public:
  bool isEmpty() const override { return false; }
  QString translate(const char *context, const char *source,
                    const char *comment = nullptr, int plural = -1) const override {
    if (qstrcmp(context, "DrawingSubstitutionPane") ||
        qstrcmp(source, "No level in the current cell"))
      return QString();
    const QString value = UiLanguageManager::instance()->currentLanguage() == "English"
                              ? QString::fromUtf8(source)
                              : QStringLiteral("Fixture translated empty cell");
    return UiTranslations::record(context, source, comment, plural, value);
  }
};

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
  const QString parent = QFileInfo(root).canonicalFilePath();
  if (resolved.isEmpty() || parent.isEmpty()) return false;
  // QDir handles platform path casing and drive roots. Canonical paths also
  // resolve links before checking containment; equality and siblings fail.
  const QString relative = QDir::fromNativeSeparators(
      QDir(parent).relativeFilePath(resolved));
  return !relative.isEmpty() && relative != "." && relative != ".." &&
         !relative.startsWith("../") && !QDir::isAbsolutePath(relative);
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
  checkpoint("checking initialized application isolation");
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
  checkpoint("opening and searching native command palette");
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
  checkpoint("creating disposable native scene and selection");
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
  app->getCurrentFrame()->setFrame(0);

  EmptyCellCaptionTranslator emptyCellCaptionTranslator;
  checkpoint("checking native Drawing Substitution captions");
  require(QCoreApplication::installTranslator(&emptyCellCaptionTranslator),
          "empty-cell fixture translator could not be installed");
  trigger(MI_OpenDrawingSubstitutionPanel);
  const auto substitutionPanes =
      app->getCurrentRoom()->findChildren<DrawingSubstitutionPane *>();
  require(!substitutionPanes.isEmpty(), "native Drawing Substitution pane missing");
  DrawingSubstitutionPane *substitutionPane = nullptr;
  for (auto *candidate : substitutionPanes)
    if (candidate->isVisible()) {
      substitutionPane = candidate;
      break;
    }
  require(substitutionPane, "native Drawing Substitution pane is not visible");
  QLabel *substitutionName = substitutionPane->findChild<QLabel *>(
      "drawingSubstitutionLevelName");
  require(substitutionName && substitutionName->text() == "NativeUiFixture",
          "native Drawing Substitution did not show the fixture level");
  app->getCurrentFrame()->setFrame(1);
  settle();
  const QString literalLevelName = substitutionName->text();
  require(literalLevelName == "No level in the current cell",
          "native empty-cell fixture did not acquire its source caption");
  UiTranslations::captureAll();
  require(scene->getLevelSet()->renameLevel(level, literalLevelName.toStdWString()),
          "native equal-caption level rename failed");
  app->getCurrentFrame()->setFrame(0);
  settle();
  require(substitutionName->text() == literalLevelName,
          "native pane did not display the equal-text literal level name");

  TColumnSelection *selection = viewer->getColumnSelection();
  checkpoint("checking palette context preservation");
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
                app->getCurrentXsheet()->getXsheet() == xsheet &&
                substitutionName->text() == literalLevelName &&
                QString::fromStdWString(level->getName()) == literalLevelName,
            "palette or appearance change altered native selection context");
  };
  palette = openPalette();
  selectionUnchanged();
  sendKey(palette, Qt::Key_Escape);
  selectionUnchanged();
  require(undo->getHistoryCount() == 0 &&
              !app->getCurrentScene()->getDirtyFlag(),
          "opening and cancelling a palette modified the document");

  checkpoint("executing native outline command and undo");
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
  checkpoint("opening native Preferences");
  const QString originalLanguage = UiLanguageManager::instance()->currentLanguage();
  const QString originalTheme = NativeTheme::instance().currentTheme();
  QWidget *preferences = preferencesPopup();
  const QString japanese = QString::fromUtf8("日本語");
  auto *language = comboWithValue(preferences, japanese);
  auto *theme = comboWithValue(preferences, QStringLiteral("Synthwave"));
  choose(language, "English");

  // Exercise the real owners of translated choices. Keep these dialogs hidden:
  // no inbetween command or conversion is requested.
  checkpoint("initializing native interpolation dialog");
  InbetweenDialog interpolation(window);
  interpolation.setValue("Ease Out");
  interpolation.setEase(23, 71);
  interpolation.setAutoMatch(true);
  QComboBox *interpolationMode = comboWithValue(&interpolation, "Ease Out");
  const int interpolationIndex = interpolation.getIndex("Ease Out");
  const QString englishInterpolation = interpolationMode->currentText();
  require(interpolationIndex == 2 && interpolation.getValue() == "Ease Out",
          "native interpolation fixture did not select Ease Out");

  // TLV is an input-dependent choice: native callers setFiles() before
  // setFormat(). Supply a real, disposable full-color image so the owner can
  // classify it and read its metadata through the normal initialization path.
  checkpoint("initializing idle native conversion dialog");
  QTemporaryDir conversionInputs(QDir(testRoot).filePath("conversion-input-XXXXXX"));
  require(conversionInputs.isValid(), "conversion input fixture directory failed");
  const QString conversionInput = conversionInputs.filePath("fixture.png");
  QImage conversionImage(2, 2, QImage::Format_ARGB32);
  conversionImage.fill(Qt::white);
  require(conversionImage.save(conversionInput, "PNG"),
          "conversion input fixture image could not be written");
  ConvertPopup conversion(false);
  conversion.setFiles({TFilePath(conversionInput.toStdWString())});
  conversion.setFormat("tlv");
  const QString conversionToken = conversion.TlvMode_PaintedFromTwoImages;
  QComboBox *conversionMode = comboWithValue(&conversion, conversionToken);
  choose(conversionMode, conversionToken);
  const int conversionIndex = conversionMode->currentIndex();
  const QString englishConversion = conversionMode->currentText();
  require(conversion.getDestinationType() == "tlv",
          "native conversion fixture did not enable TLV for its raster input");
  require(conversion.getTlvMode() == conversionToken,
          "native conversion fixture did not select the two-image painting mode");
  require(!conversion.isConverting(),
          "native conversion fixture unexpectedly started conversion");

  int modeChanges = 0;
  QObject modeObserver;
  for (QComboBox *combo : {interpolationMode, conversionMode}) {
    QObject::connect(combo, QOverload<int>::of(&QComboBox::currentIndexChanged),
                     &modeObserver, [&](int) { ++modeChanges; });
    QObject::connect(combo, &QComboBox::currentTextChanged, &modeObserver,
                     [&](const QString &) { ++modeChanges; });
  }
  const auto checkNativeModes = [&](bool isJapanese) {
    const QString emptyCellCaption = QCoreApplication::translate(
        "DrawingSubstitutionPane", "No level in the current cell");
    require((emptyCellCaption != literalLevelName) == isJapanese,
            "equal-text fixture did not provide a distinct translated caption");
    require(interpolation.getValue() == "Ease Out" &&
                interpolation.getIndex("Ease Out") == interpolationIndex &&
                interpolationMode->currentIndex() == interpolationIndex &&
                interpolation.slowStart() == 23 && interpolation.slowEnd() == 71 &&
                interpolation.autoMatch(),
            "language refresh changed native interpolation semantics or options");
    require(conversion.getTlvMode() == conversionToken &&
                conversion.TlvMode_PaintedFromTwoImages == conversionToken &&
                conversionMode->currentIndex() == conversionIndex &&
                conversion.getDestinationType() == "tlv" &&
                !conversion.isConverting(),
            "language refresh changed native conversion semantics or started work");
    require((interpolationMode->currentText() != englishInterpolation) == isJapanese &&
                (conversionMode->currentText() != englishConversion) == isJapanese,
            "native interpolation or conversion captions did not follow language");
    require(modeChanges == 0,
            "native caption refresh emitted an editing signal for a mode choice");
    selectionUnchanged();
  };

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
  checkpoint("switching native UI to Japanese");
  choose(language, japanese);
  checkNativeModes(true);
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
  checkpoint("applying native Synthwave theme");
  choose(theme, "Synthwave");
  require(NativeTheme::instance().currentTheme() == "Synthwave" &&
              !qApp->styleSheet().isEmpty(),
          "Preferences did not apply the packaged native Synthwave theme");
  savedPreferences.sync();
  require(savedPreferences.status() == QSettings::NoError &&
              savedPreferences.value("CurrentStyleSheetName").toString() == "Synthwave",
          "native theme preference was not saved in the isolated profile");
  selectionUnchanged();
  checkpoint("repeating native language switches");
  for (const QString &nextLanguage : {QString("English"), japanese,
                                      QString("English")}) {
    choose(language, nextLanguage);
    checkNativeModes(nextLanguage == japanese);
  }
  checkpoint("restoring native appearance preferences");
  choose(language, originalLanguage);
  choose(theme, originalTheme);
  preferences->hide();
  require(undo->getHistoryCount() == historyCount &&
              undo->getCurrentHistoryIndex() == historyIndex &&
              app->getCurrentScene()->getDirtyFlag() == dirty,
          "appearance changes modified the document or its undo history");

  checkpoint("checking native widget inspector");
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
  // No close/save prompts; the temporary input is removed on return.
  checkpoint("clearing native fixture selection and history");
  selection->makeNotCurrent();
  app->getCurrentScene()->setDirtyFlag(false);
  undo->reset();
  checkpoint("destroying native fixture dialogs");
}
}  // namespace

bool prepareNativeUiIntegrationTest(QString *error) {
  checkpoint("validating disposable fixture directory");
  const auto reject = [error](const char *reason) {
    if (error) *error = QString::fromLatin1(reason);
    return false;
  };
  const QString root = QFileInfo(QDir::currentPath()).canonicalFilePath();
  const QString temporary = QFileInfo(QDir::tempPath()).canonicalFilePath();
  if (!below(root, temporary))
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
  checkpoint("disposable storage isolation prepared");
  return true;
}

int runNativeUiIntegrationTest(QMainWindow *window) {
  checkpoint("entered native fixture callback");
  try {
    require(!testRoot.isEmpty(), "native UI test was not prepared before startup");
    exerciseNativeUi(window);
    std::cout << "native-ui-integration: PASS" << std::endl;
    return 0;
  } catch (const Failure &failure) {
    std::cerr << "native-ui-integration: FAIL: " << failure.message << std::endl;
  } catch (...) {
    std::cerr << "native-ui-integration: FAIL: unexpected native exception"
              << std::endl;
  }
  return 1;
}
