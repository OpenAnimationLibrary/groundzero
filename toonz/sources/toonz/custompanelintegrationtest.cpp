#include "custompanelintegrationtest.h"

#include "custompaneleditorpopup.h"
#include "mainwindow.h"
#include "menubarcommandids.h"
#include "pane.h"
#include "tenv.h"
#include "toonz/toonzfolders.h"
#include "toonzqt/menubarcommand.h"

#include <QApplication>
#include <QDialog>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QImage>
#include <QLabel>
#include <QPointer>
#include <QSettings>
#include <QTemporaryDir>
#include <QTimer>
#include <QToolButton>
#include <QTreeWidgetItemIterator>

#include <iostream>
#include <memory>
#include <stdexcept>

namespace {

void require(bool condition, const char* message) {
  if (!condition) throw std::runtime_error(message);
}

void writeFile(const QString& path, const QByteArray& contents) {
  QFile file(path);
  require(file.open(QIODevice::WriteOnly),
          "custom panel fixture could not open its input file");
  require(file.write(contents) == contents.size(),
          "custom panel fixture could not write its input file");
}

TPanel* panelInRoom(Room& room) {
  require(room.dockLayout()->count() == 1,
          "custom panel room did not restore exactly one panel");
  auto* panel = dynamic_cast<TPanel*>(room.dockLayout()->itemAt(0)->widget());
  require(panel && panel->widget(),
          "custom panel room restored no usable panel content");
  return panel;
}

std::unique_ptr<Room> loadRoom(const QTemporaryDir& directory,
                               const QString& name) {
  const QString panelType =
      "Custom_" + QFileInfo(directory.path()).fileName() + "__" + name;
  const QString path = directory.filePath(name + ".ini");
  QSettings settings(path, QSettings::IniFormat);
  settings.setValue("room/name", "Custom panel loading fixture");
  settings.setValue("room/pane_0/name", panelType);
  settings.setValue("room/pane_0/geometry", QRect(0, 0, 320, 200));
  settings.setValue("room/hierarchy", "-1 0 [ 0 ] ");
  settings.sync();
  require(settings.status() == QSettings::NoError,
          "custom panel fixture could not save its room input");
  auto room = std::make_unique<Room>();
  room->resize(320, 200);
  Room::RoomLoadParams params;
  params.forceBuildGui = true;
  room->load(TFilePath(path), params);
  require(!room->notInitialized(), "custom panel room failed to initialize");
  TPanel* panel = panelInRoom(*room);
  require(panel->objectName() == panelType &&
              panel->getPanelType() == panelType.toStdString() &&
              panel->windowTitle() == name,
          "custom panel restoration lost the panel identity or title");
  require(panel->parentWidget() == room.get() &&
              panel->isAncestorOf(panel->widget()),
          "custom panel restoration lost parent ownership");
  return room;
}

void retireRoom(std::unique_ptr<Room>& room) {
  QPointer<TPanel> panel     = panelInRoom(*room);
  QPointer<QWidget> contents = panel->widget();
  room.reset();
  require(panel.isNull() && contents.isNull(),
          "custom panel content survived its owning room");
}

void checkFallback(const QTemporaryDir& directory, const QString& name) {
  auto room      = loadRoom(directory, name);
  auto* fallback = qobject_cast<QLabel*>(panelInRoom(*room)->widget());
  require(fallback && fallback->textFormat() == Qt::PlainText &&
              fallback->wordWrap() &&
              QDir::fromNativeSeparators(fallback->text())
                  .contains(directory.filePath(name + ".ui")),
          "custom panel file failure did not show its plain-text error");
  retireRoom(room);
}

void checkValidPanel(const QTemporaryDir& directory) {
  writeFile(directory.filePath("valid.ui"), R"ui(<?xml version="1.0"?>
<ui version="4.0">
 <class>NativeCustomPanel</class>
 <widget class="QWidget" name="NativeCustomPanel">
  <layout class="QVBoxLayout" name="panelLayout">
   <item><widget class="QToolButton" name="MI_Undo"/></item>
   <item><widget class="QLabel" name="fixtureCaption">
    <property name="text"><string>Loaded custom panel</string></property>
   </widget></item>
  </layout>
 </widget>
 <resources/>
 <connections/>
</ui>)ui");
  auto room         = loadRoom(directory, "valid");
  QWidget* contents = panelInRoom(*room)->widget();
  auto* button      = contents->findChild<QToolButton*>();
  auto* label       = contents->findChild<QLabel*>("fixtureCaption");
  QAction* undo     = CommandManager::instance()->getAction(MI_Undo, false);
  require(undo && button && button->defaultAction() == undo,
          "valid custom panel lost its native command binding");
  require(label && label->text() == "Loaded custom panel",
          "valid custom panel content was replaced by an error");
  retireRoom(room);
}

void selectTemplate(CustomPanelEditorPopup& editor, const QString& path) {
  auto* browser = editor.findChild<QWidget*>("TemplateSelectorBrowser");
  require(browser, "custom panel editor has no template browser");
  auto* tree = browser->findChild<QTreeWidget*>();
  require(tree, "custom panel editor has no template tree");
  for (QTreeWidgetItemIterator item(tree); *item; ++item) {
    if (TFilePath((*item)->data(0, Qt::UserRole).toString()) != TFilePath(path))
      continue;
    require(QMetaObject::invokeMethod(
                &editor, "onTemplateItemClicked", Qt::DirectConnection,
                Q_ARG(QTreeWidgetItem*, *item), Q_ARG(int, 0)),
            "custom panel editor did not select its real template item");
    return;
  }
  throw std::runtime_error(
      "custom panel editor did not list the fixture template");
}

void changeEditorCommand(CustomPanelEditorPopup& editor, const QString& before,
                         const QString& after) {
  for (auto* field : editor.findChildren<CustomPanelUIField*>()) {
    if (field->commandId() != before) continue;
    require(field->setCommand(after),
            "custom panel fixture could not change its command field");
    field->notifyCommandChanged(before, after);
    return;
  }
  throw std::runtime_error("custom panel fixture lost its command field");
}

void checkCorruptedTemplate(CustomPanelEditorPopup& editor,
                            const QTemporaryDir& directory, int& warningCount,
                            const QString& warning) {
  auto* area = editor.findChild<UiPreviewArea*>();
  require(area, "custom panel editor has no preview area");
  QPointer<UiPreviewWidget> preview =
      qobject_cast<UiPreviewWidget*>(area->widget());
  require(preview, "custom panel editor lost its valid preview");
  const QImage before = preview->uiPixmapForTest().toImage();
  require(!before.isNull(), "custom panel fixture captured an empty preview");
  writeFile(directory.filePath("valid.ui"),
            "<ui version=\"4.0\"><widget class=\"QWidget\"></ui>");
  const int previousWarnings = warningCount;
  changeEditorCommand(editor, MI_Redo, MI_Undo);
  require(warningCount == previousWarnings + 1 &&
              QDir::fromNativeSeparators(warning).contains(
                  directory.filePath("valid.ui")),
          "externally corrupted template did not report its loading error");
  require(preview && area->widget() == preview.data() &&
              preview->uiPixmapForTest().toImage() == before,
          "externally corrupted template changed the last valid preview");
}

void checkEditorFailure(const QTemporaryDir& directory) {
  require(!QApplication::activeModalWidget(),
          "custom panel editor fixture started with an unexpected modal");
  int warningCount = 0;
  QString warning;
  QTimer response;
  QObject::connect(&response, &QTimer::timeout, &response, [&]() {
    auto* dialog = qobject_cast<QDialog*>(QApplication::activeModalWidget());
    if (!dialog) return;
    ++warningCount;
    for (QLabel* label : dialog->findChildren<QLabel*>())
      warning += label->text();
    dialog->reject();
  });
  response.start(10);
  CustomPanelEditorPopup editor;
  selectTemplate(editor, directory.filePath("valid.ui"));
  require(warningCount == 0,
          "custom panel editor could not load a valid fixture template");
  auto* area = editor.findChild<UiPreviewArea*>();
  require(area, "custom panel editor has no preview area");
  QPointer<QWidget> preview = area->widget();
  const auto fields         = editor.findChildren<CustomPanelUIField*>();
  require(preview && !fields.isEmpty(),
          "valid custom panel template produced no preview or command fields");
  selectTemplate(editor, directory.filePath("malformed.ui"));
  require(warningCount == 1 && QDir::fromNativeSeparators(warning).contains(
                                   directory.filePath("malformed.ui")),
          "malformed custom panel template did not report its loading error");
  require(preview && area->widget() == preview.data() &&
              editor.findChildren<CustomPanelUIField*>() == fields,
          "malformed custom panel template replaced the previous editor state");
  changeEditorCommand(editor, MI_Undo, MI_Redo);
  require(warningCount == 1,
          "failed template selection replaced the path used by field edits");
  checkCorruptedTemplate(editor, directory, warningCount, warning);
  response.stop();
}

}  // namespace

void exerciseCustomPanelLoading() {
  require(TEnv::getIsPortable(),
          "custom panel fixture requires isolated portable settings");
  const QString root =
      (ToonzFolder::getMyModuleDir() + "custompanels").getQString();
  require(QDir().mkpath(root),
          "custom panel fixture could not create its input directory");
  QTemporaryDir directory(QDir(root).filePath("native-loading-XXXXXX"));
  require(directory.isValid(),
          "custom panel fixture could not create temporary storage");
  checkFallback(directory, "missing");
  writeFile(directory.filePath("malformed.ui"),
            "<ui version=\"4.0\"><widget class=\"QWidget\"></ui>");
  checkFallback(directory, "malformed");
  checkValidPanel(directory);
  checkEditorFailure(directory);
  std::cerr << "native-ui-integration: checkpoint: custom panel file loading "
               "and ownership passed"
            << std::endl;
}
