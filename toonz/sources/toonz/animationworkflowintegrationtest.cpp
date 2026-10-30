#include "animationworkflowintegrationtest.h"
#include "autosaverecovery.h"
#include "exportfidelity.h"
#include "filebrowserpopup.h"
#include "filebrowsermodel.h"
#include "dvdirtreeview.h"
#include "menubarcommandids.h"
#include "toonzqt/menubarcommand.h"
#include "iocommand.h"
#include "librarycommand.h"
#include "poselibrarycommand.h"
#include "posespane.h"
#include "scenecheckpane.h"
#include "storyboardtiming.h"
#include "tapp.h"
#include "viewerpane.h"
#include "tdoublekeyframe.h"
#include "tdoubleparam.h"
#include "tfx.h"
#include "tparamcontainer.h"
#include "tparamset.h"
#include "ext/plasticskeleton.h"
#include "ext/plasticskeletondeformation.h"
#include "tpalette.h"
#include "tstroke.h"
#include "tstream.h"
#include "tsound_io.h"
#include "tundo.h"
#include "tvectorimage.h"
#include "toonz/levelproperties.h"
#include "toonz/levelset.h"
#include "toonz/fxdag.h"
#include "toonz/navigationtags.h"
#include "toonz/preferences.h"
#include "toonz/scenechecker.h"
#include "toonz/tcolumnhandle.h"
#include "toonz/tcolumnfx.h"
#include "toonz/tcolumnfxset.h"
#include "toonz/tcamera.h"
#include "toonz/tframehandle.h"
#include "toonz/toonzscene.h"
#include "toonz/toonzfolders.h"
#include "toonz/tstageobject.h"
#include "toonz/tstageobjecttree.h"
#include "toonz/tstageobjectid.h"
#include "toonz/tpinnedrangeset.h"
#include "toonz/tscenehandle.h"
#include "toonz/txshchildlevel.h"
#include "toonz/txsheet.h"
#include "toonz/txsheethandle.h"
#include "toonz/txshlevelcolumn.h"
#include "toonz/txshleveltypes.h"
#include "toonz/txshsimplelevel.h"
#include "toonz/txshsoundcolumn.h"
#include "toonz/txshsoundlevel.h"
#include <QApplication>
#include <QDialog>
#include <QCheckBox>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QXmlStreamReader>
#include <QDir>
#include <QFileInfo>
#include <QLabel>
#include <QMainWindow>
#include <QPersistentModelIndex>
#include <QItemSelectionModel>
#include <QPushButton>
#include <QTemporaryDir>
#include <QTextEdit>
#include <QTimer>
#include <cmath>
#include <functional>
#include <iostream>
#include <stdexcept>

namespace {
void require(bool value, const char *message) {
  if (!value) throw std::runtime_error(message);
}
void requireNear(double actual, double expected, const char *message) {
  require(std::abs(actual - expected) < 1e-8, message);
}
bool sameKey(const TDoubleKeyframe &a, const TDoubleKeyframe &b) {
  return a.m_frame == b.m_frame && a.m_value == b.m_value && a.m_type == b.m_type &&
         a.m_prevType == b.m_prevType && a.m_isKeyframe == b.m_isKeyframe &&
         a.m_step == b.m_step && a.m_speedIn == b.m_speedIn && a.m_speedOut == b.m_speedOut &&
         a.m_linkedHandles == b.m_linkedHandles && a.m_expressionText == b.m_expressionText &&
         a.m_fileParams.m_path == b.m_fileParams.m_path &&
         a.m_fileParams.m_fieldIndex == b.m_fileParams.m_fieldIndex &&
         a.m_unitName == b.m_unitName && a.m_similarShapeOffset == b.m_similarShapeOffset;
}
// Drive actual native modal controls. No sleeps, screenshots or timing limits.
void dialogs(const QStringList &choices, const std::function<void()> &operation) {
  int handled = 0;
  bool matched = true;
  QTimer timer;
  QObject::connect(&timer, &QTimer::timeout, [&]() {
    auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget());
    if (!dialog) return;
    if (handled >= choices.size()) { matched = false; dialog->reject(); return; }
    const QString choice = choices[handled++];
    for (auto *button : dialog->findChildren<QPushButton *>())
      if (button->isVisible() && (choice == "*" || button->objectName() == choice ||
                                 button->text().remove('&') == choice)) {
        button->click();
        return;
      }
    matched = false;
    dialog->reject();
  });
  timer.start(0);
  operation();
  timer.stop();
  require(matched && handled == choices.size(), "native modal choices did not match fixture");
}
void writePoses(const TFilePath &file, const PoseLibrary &library) {
  TOStream stream(file);
  stream.openChild("poseSet", {{"version", "1"}});
  library.saveData(stream);
  stream.closeChild();
  require(stream.checkStatus(), "pose import fixture could not be written");
}
void exerciseDirectoryRefresh(ToonzScene *scene, const QString &folder) {
  const QString rootPath = QDir(folder).filePath("browser-refresh");
  QDir filesystem;
  require(filesystem.mkpath(rootPath + "/b_keep/nested") &&
          filesystem.mkpath(rootPath + "/c_remove/deep") &&
          filesystem.mkpath(rootPath + "/d_tail"), "directory refresh fixture setup");
  DvDirModel *model = DvDirModel::instance();
  QPersistentModelIndex root(model->getIndexByPath(TFilePath(rootPath)));
  require(root.isValid(), "native browser did not find fixture directory");
  QPersistentModelIndex keep(model->childByName(root, L"b_keep"));
  QPersistentModelIndex keepChild(model->childByName(keep, L"nested"));
  QPersistentModelIndex removed(model->childByName(root, L"c_remove"));
  QPersistentModelIndex removedChild(model->childByName(removed, L"deep"));
  require(keep.isValid() && keepChild.isValid() && removed.isValid() && removedChild.isValid(),
          "native browser child fixtures missing");
  DvDirModelNode *keptNode = model->getNode(keep);
  DvDirTreeView first, second;
  first.setRootIndex(root); second.setRootIndex(root);
  first.expand(keep); second.expand(removed);
  first.setCurrentIndex(keepChild); second.setCurrentIndex(removedChild);
  first.show(); second.show();
  QCoreApplication::processEvents();
  auto directoryChanged = [&] {
    // Exercise the real watcher signal and every attached native browser. OS
    // watcher delivery remains enabled; repeated notifications are intentional.
    MyFileSystemWatcher::instance()->directoryChanged(rootPath);
    QCoreApplication::processEvents();
  };
  require(QDir(rootPath + "/c_remove").removeRecursively() &&
          filesystem.mkpath(rootPath + "/a_new"), "directory refresh fixture mutation");
  directoryChanged();
  require(!removed.isValid() && !removedChild.isValid(),
          "directory refresh kept persistent indexes to deleted nodes");
  require(keep.isValid() && keepChild.isValid() && model->getNode(keep) == keptNode &&
          keep.row() == 1 && first.currentIndex() == keepChild && first.isExpanded(keep),
          "directory refresh lost surviving selection, expansion or reordered row");
  require(model->parent(keepChild) == keep, "directory refresh left an invalid parent index");
  // The reported crash occurred at the next layoutAboutToBeChanged, when Qt
  // consulted the stale indexes left behind by the previous refresh.
  directoryChanged();
  require(filesystem.mkpath(rootPath + "/c_remove/deep"), "recreate directory fixture");
  directoryChanged();
  require(!removed.isValid() && !removedChild.isValid(), "deleted directory indexes revived on recreation");

  auto *handle = TApp::instance()->getCurrentScene();
  const TFilePath originalPath = scene->getScenePath();
  const bool wasUntitled = scene->isUntitled();
  Preferences *preferences = Preferences::instance();
  const int originalPriority = int(preferences->getPathAliasPriority());
  struct RestoreSceneLocation {
    ToonzScene *scene;
    TSceneHandle *handle;
    Preferences *preferences;
    TFilePath path;
    bool untitled;
    int priority;
    ~RestoreSceneLocation() {
      if (untitled) scene->setUntitled();
      scene->setScenePath(path, !untitled);
      preferences->setValue(pathAliasPriority, priority);
      handle->notifySceneSwitched();
      handle->notifyPreferenceChanged("PathAliasPriority");
    }
  } restore{scene, handle, preferences, originalPath, wasUntitled, originalPriority};
  const QString sceneA = QDir(folder).filePath("scene-location-a");
  const QString sceneB = QDir(folder).filePath("scene-location-b");
  require(filesystem.mkpath(sceneA + "/shared/old") &&
          filesystem.mkpath(sceneB + "/shared/new"), "scene location fixture setup");
  preferences->setValue(pathAliasPriority, int(Preferences::SceneFolderAlias));
  handle->notifyPreferenceChanged("PathAliasPriority");
  scene->setScenePath(TFilePath(sceneA + "/scene.tnz"));
  handle->notifySceneSwitched();
  auto sceneFolderIndex = [&]() {
    for (int row = 0; row < model->rowCount(); ++row) {
      const auto index = model->index(row, 0, QModelIndex());
      if (dynamic_cast<DvDirModelSceneFolderNode *>(model->getNode(index))) return index;
    }
    return QModelIndex();
  };
  QPersistentModelIndex alias(sceneFolderIndex());
  QPersistentModelIndex aliasChild(model->childByName(alias, L"shared"));
  QPersistentModelIndex aliasGrandchild(model->childByName(aliasChild, L"old"));
  require(alias.isValid() && aliasChild.isValid() && aliasGrandchild.isValid(),
          "scene alias fixture indexes missing");
  second.setRootIndex(alias); second.expand(aliasChild); second.setCurrentIndex(aliasGrandchild);
  scene->setScenePath(TFilePath(sceneB + "/scene.tnz"));
  handle->notifySceneSwitched();
  require(!alias.isValid() && !aliasChild.isValid() && !aliasGrandchild.isValid(),
          "scene location change retained indexes into the old directory");
  const auto newAlias = sceneFolderIndex();
  const auto newShared = model->childByName(newAlias, L"shared");
  auto *newFolder = dynamic_cast<DvDirModelFileFolderNode *>(model->getNode(newShared));
  require(newFolder && newFolder->getPath() == TFilePath(sceneB + "/shared") &&
          model->childByName(newShared, L"new").isValid(),
          "scene location reused a same-named child from the previous directory");
  directoryChanged();
  require(keep.isValid() && keepChild.isValid() && first.currentIndex() == keepChild,
          "scene location update reset unrelated browser selection");

  // Closing the active browser must release its selection and command target;
  // closing another browser must leave that active selection alone. Repeat to
  // exercise a fresh browser after the previous selection has been destroyed.
  for (int reopen = 0; reopen != 2; ++reopen) {
    {
      DvDirTreeView active;
      active.setRootIndex(root);
      active.setCurrentIndex(keep);
      active.setCurrentIndex(keepChild);
      require(TSelection::getCurrent() == &active,
              "native browser navigation did not activate global selection");
      {
        DvDirTreeView inactive;
        require(TSelection::getCurrent() == &active,
                "constructing inactive browser replaced global selection");
      }
      require(TSelection::getCurrent() == &active,
              "closing inactive browser cleared another browser selection");
    }
    require(TSelection::getCurrent() == nullptr,
            "closing active browser retained a destroyed global selection");
  }
}
void exercisePoses(ToonzScene *scene, const QString &folder) {
  PoseLibrary first;
  first.setBase({{TStageObjectId::ColumnId(0), "x", 3.0}});
  first.addPose({"Turn", {{TStageObjectId::ColumnId(0), "x", 2.0}}});
  const TFilePath file(QDir(folder).filePath("poses.tposes"));
  writePoses(file, first);
  require(LibraryCmd::loadPoseSet(file), "native pose import failed");
  require(scene->getPoseLibrary()->getPoseCount() == 1, "pose import missing");
  requireNear(scene->getPoseLibrary()->getBase().front().m_value, 3.0, "imported base missing");
  require(TUndoManager::manager()->undo(), "pose import undo missing");
  require(scene->getPoseLibrary()->isEmpty(), "import undo did not restore empty base/library");
  require(TUndoManager::manager()->redo(), "pose import redo missing");

  PoseLibrary replacement;
  replacement.setBase({{TStageObjectId::ColumnId(0), "x", 99.0}});
  replacement.addPose({"Turn", {{TStageObjectId::ColumnId(0), "x", 8.0}}});
  writePoses(file, replacement);
  const int history = TUndoManager::manager()->getHistoryCount();
  dialogs({QObject::tr("Cancel")}, [&]() { require(!LibraryCmd::loadPoseSet(file), "cancelled pose import succeeded"); });
  require(TUndoManager::manager()->getHistoryCount() == history, "cancelled pose import changed undo history");
  dialogs({QObject::tr("Replace")}, [&]() { require(LibraryCmd::loadPoseSet(file), "pose replace failed"); });
  requireNear(scene->getPoseLibrary()->getPose(0).m_entries.front().m_value, 8.0, "pose replace value missing");
  requireNear(scene->getPoseLibrary()->getBase().front().m_value, 3.0, "replace changed existing base");
  require(TUndoManager::manager()->undo(), "replace undo missing");
  requireNear(scene->getPoseLibrary()->getPose(0).m_entries.front().m_value, 2.0, "replace undo did not restore pose");
  require(TUndoManager::manager()->redo(), "replace redo missing");

  PosesPane pane;
  pane.show();
  auto *remove = pane.findChild<QPushButton *>("deletePose0");
  require(remove, "native pose delete control missing");
  dialogs({PosesPane::tr("Delete")}, [&]() { remove->click(); });
  require(scene->getPoseLibrary()->getPoseCount() == 0, "native delete left pose");
  require(TUndoManager::manager()->undo(), "pose deletion undo missing");
  require(scene->getPoseLibrary()->getPoseCount() == 1, "pose deletion undo lost pose");
  pane.hide();

  const TFilePath sceneFile(QDir(folder).filePath("pose-scene.tnz"));
  scene->saveCopy(sceneFile);
  ToonzScene loaded;
  loaded.setProject(scene->getProject());
  loaded.load(sceneFile);
  require(loaded.getPoseLibrary()->getPoseCount() == 1, "pose library did not persist");
  requireNear(loaded.getPoseLibrary()->getBase().front().m_value, 3.0, "base did not persist");
}

void exerciseRecovery(ToonzScene *scene) {
  auto *preferences = Preferences::instance();
  const bool oldEnabled = preferences->isAutosaveEnabled();
  const bool oldRecovery = preferences->isAutosaveToRecoveryFolderEnabled();
  struct Restore {
    Preferences *preferences; bool enabled, recovery;
    ~Restore() {
      preferences->setValue(autosaveEnabled, enabled);
      preferences->setValue(autosaveToRecoveryFolder, recovery);
    }
  } restore{preferences, oldEnabled, oldRecovery};
  preferences->setValue(autosaveEnabled, false);
  require(AutosaveRecovery::status().state == AutosaveRecovery::State::Disabled, "disabled recovery status wrong");
  require(!AutosaveRecovery::retryRecoveryCopy(), "retry wrote while recovery disabled");
  bool inspected = false;
  QTimer::singleShot(0, [&]() {
    auto *dialog = QApplication::activeModalWidget();
    if (dialog) {
      auto *retry = dialog->findChild<QPushButton *>("retryRecovery");
      auto *details = dialog->findChild<QLabel *>("recoveryStatusDetails");
      inspected = retry && !retry->isEnabled() && details && !details->text().isEmpty();
      dialog->close();
    }
  });
  AutosaveRecovery::showStatusDialog();
  require(inspected, "native recovery status controls missing or disabled state wrong");
  preferences->setValue(autosaveToRecoveryFolder, true);
  preferences->setValue(autosaveEnabled, true);
  require(AutosaveRecovery::retryRecoveryCopy(), "empty scene recovery failed");
  auto complete = AutosaveRecovery::status();
  require(complete.state == AutosaveRecovery::State::Complete &&
          QFileInfo::exists(QDir(complete.folder).filePath("recovery.ini")), "complete recovery manifest/status missing");
  auto *sound = new TXshSoundLevel(L"MissingRecoveryAudio");
  sound->setType(SND_XSHLEVEL);
  sound->setScene(scene);
  scene->getLevelSet()->insertLevel(sound);
  require(!AutosaveRecovery::retryRecoveryCopy(), "missing recovery resource claimed complete");
  auto partial = AutosaveRecovery::status();
  require(partial.state == AutosaveRecovery::State::Partial && !partial.missingResources.isEmpty(), "partial recovery details missing");
  scene->getLevelSet()->removeLevel(sound);
  require(AutosaveRecovery::retryRecoveryCopy(), "recovery retry did not complete after resource fix");
  require(AutosaveRecovery::status().folder != complete.folder, "repeated recovery overwrote its previous version");
}

void exerciseTiming(ToonzScene *scene, QMainWindow *window, const QString &folder) {
  auto *app = TApp::instance();
  TXsheet *sheet = scene->getXsheet();
  auto *level = scene->createNewLevel(PLI_XSHLEVEL, L"StoryboardFixture")->getSimpleLevel();
  for (int row = 0; row < 6; ++row) {
    level->setFrame(TFrameId(row + 1), TVectorImageP(new TVectorImage));
    require(sheet->setCell(row, 0, TXshCell(level, TFrameId(row + 1))), "storyboard exposure fixture failed");
  }
  sheet->getNavigationTags()->addTag(0, "First");
  sheet->getNavigationTags()->addTag(2, "Second");
  auto *camera = sheet->getStageObject(TStageObjectId::CameraId(0));
  auto *curve = camera->getParam(TStageObject::T_X);
  TDoubleKeyframe start(0, 0), end(5, 50);
  start.m_type = end.m_type = TDoubleKeyframe::Linear;
  curve->setKeyframe(start); curve->setKeyframe(end);
  auto *rigObject = sheet->getStageObject(TStageObjectId::ColumnId(0));
  rigObject->getPinnedRangeSet()->setRange(0, 1);
  auto *rig = rigObject->getParam(TStageObject::T_Angle);
  TDoubleKeyframe hold(0, 20), last(5, 40);
  hold.m_type = last.m_type = TDoubleKeyframe::Constant;
  rig->setKeyframe(hold); rig->setKeyframe(last);
  auto *sound = new TXshSoundLevel(L"StoryboardAudio");
  sound->setType(SND_XSHLEVEL);
  sound->setScene(scene);
  sound->setFrameRate(24.0);
  const TFilePath audioPath(QDir(folder).filePath("storyboard.wav"));
  require(TSoundTrackWriter::save(audioPath,
      TSoundTrack::create(24000, 16, 1, 6000, TSound::INT)), "audio fixture file write failed");
  sound->setPath(audioPath);
  sound->loadSoundTrack(audioPath);
  require(sound->getFrameCount() == 6, "audio fixture duration wrong");
  scene->getLevelSet()->insertLevel(sound);
  for (int row = 0; row < 6; ++row)
    require(sheet->setCell(row, 1, TXshCell(sound, TFrameId(row))), "storyboard audio fixture failed");
  app->getCurrentColumn()->setColumnIndex(0);
  app->getCurrentFrame()->setFrame(3);
  QString error;
  require(moveStoryboardTiming(sheet, 0, 1, 6, &error), "native storyboard move failed");
  require(curve->getKeyframe(0).m_frame == 0, "frame-zero move introduced negative keys");
  const int source[] = {2, 3, 4, 5, 0, 1};
  for (int row = 0; row < 6; ++row) {
    require(sheet->getCell(row, 0).getFrameId().getNumber() == source[row] + 1, "storyboard picture permutation wrong");
    require(sheet->getCell(row, 1).getFrameId().getNumber() == source[row], "storyboard sound source offset wrong");
    requireNear(curve->getValue(row), source[row] * 10.0, "storyboard camera timing wrong");
    requireNear(rig->getValue(row), source[row] == 5 ? 40 : 20, "storyboard rig hold wrong");
  }
  require(rigObject->getPinnedRangeSet()->isPinned(4) &&
          !rigObject->getPinnedRangeSet()->isPinned(0), "rig pin ranges did not follow storyboard timing");
  requireNear(curve->getValue(0.5), 25, "native interpolation was lost inside panel");
  requireNear(curve->getValue(3.5), 50, "panel boundary must hold final frame");
  require(app->getCurrentFrame()->getFrame() == 3 && app->getCurrentColumn()->getColumnIndex() == 0,
          "storyboard move changed active selection");
  require(sheet->getNavigationTags()->getTagLabel(4) == "First", "storyboard tag timing wrong");
  require(TUndoManager::manager()->undo(), "storyboard undo missing");
  require(rigObject->getPinnedRangeSet()->isPinned(0) &&
          !rigObject->getPinnedRangeSet()->isPinned(4), "rig pin undo failed");
  require(curve->getKeyframeCount() == 2 && curve->getKeyframe(0).m_type == TDoubleKeyframe::Linear,
          "storyboard undo did not restore original curve");
  for (int row = 0; row < 6; ++row)
    require(sheet->getCell(row, 1).getFrameId().getNumber() == row, "storyboard undo did not restore audio clip");
  require(TUndoManager::manager()->redo() && TUndoManager::manager()->undo(), "repeated storyboard undo/redo failed");
  sheet->getColumn(0)->lock(true);
  const int history = TUndoManager::manager()->getCurrentHistoryIndex();
  require(!moveStoryboardTiming(sheet, 0, 1, 6, &error) && !error.isEmpty(), "locked storyboard move was not blocked");
  require(TUndoManager::manager()->getCurrentHistoryIndex() == history, "failed move changed history");
  sheet->getColumn(0)->lock(false);

  auto *column = sheet->getColumn(0)->getLevelColumn();
  column->setTimeRemapEnabled(true);
  column->getTimeRemapParam()->setValue(0, 6);
  column->getTimeRemapParam()->setValue(5, 1);
  app->getCurrentFrame()->setFrame(0);
  app->getCurrentXsheet()->notifyXsheetChanged();
  const auto viewers = window->findChildren<BaseViewerPanel *>();
  require(!viewers.isEmpty(), "native viewer is missing");
  viewers.front()->changeWindowTitle();
  auto *label = viewers.front()->findChild<QLabel *>("viewerTimeRemapStatus");
  require(label && !label->isHidden() && label->text().contains("6"), "source/playback timing readback missing");
  require(sheet->getCell(0, 0).getFrameId() == TFrameId(1), "timing readback changed editable source");
  require(!moveStoryboardTiming(sheet, 0, 1, 6, &error), "native mode silently baked remapped timing");
  std::vector<TFrameId> playback;
  for (int row = 0; row < 6; ++row) playback.push_back(column->getRemappedCell(row).getFrameId());
  require(moveStoryboardTiming(sheet, 0, 1, 6, &error, true), "opt-in remapped storyboard move failed");
  for (int row = 0; row < 6; ++row)
    require(column->getRemappedCell(row).getFrameId() == playback[source[row]],
            "rebased time remap lost picture/source correspondence");
  require(sheet->getFrameCount() == 6, "rebased time remap changed scene duration");
  require(TUndoManager::manager()->undo(), "remapped timing undo missing");
  require(column->isTimeRemapEnabled() && column->getTimeRemapParam()->getKeyframeCount() == 2,
          "remapped timing undo lost original curve");
  column->setTimeRemapEnabled(false);
  viewers.front()->changeWindowTitle();
  require(label->isHidden(), "timing readback remains after remap disabled");

  curve->deleteKeyframe(5);
  TDoubleKeyframe cycleEnd(2, 20);
  cycleEnd.m_type = TDoubleKeyframe::Linear;
  curve->setKeyframe(cycleEnd);
  camera->enableCycle(true);
  std::vector<double> cycled;
  for (int row = 0; row < 6; ++row) cycled.push_back(curve->getValue(camera->paramsTime(row)));
  require(!moveStoryboardTiming(sheet, 0, 1, 6, &error), "native mode silently baked stage cycle");
  require(moveStoryboardTiming(sheet, 0, 1, 6, &error, true), "opt-in stage cycle move failed");
  require(!camera->isCycleEnabled(), "baked stage cycle still wraps sampled timing");
  for (int row = 0; row < 6; ++row)
    requireNear(curve->getValue(row), cycled[source[row]], "stage cycle bake changed scene-frame output");
  require(TUndoManager::manager()->undo() && camera->isCycleEnabled() && curve->getKeyframeCount() == 2,
          "stage cycle undo did not restore cycle and original keys");
  camera->enableCycle(false);
  curve->enableCycle(true);
  cycled.clear();
  for (int row = 0; row < 6; ++row) cycled.push_back(curve->getValue(row));
  require(moveStoryboardTiming(sheet, 0, 1, 6, &error, true), "parameter cycle bake move failed");
  require(!curve->isCycleEnabled(), "baked parameter cycle still enabled");
  for (int row = 0; row < 6; ++row)
    requireNear(curve->getValue(row), cycled[source[row]], "parameter cycle bake changed scene-frame output");
  require(TUndoManager::manager()->undo() && curve->isCycleEnabled(), "parameter cycle undo failed");
  curve->enableCycle(false);
}

void exportThroughNativeCommand(const char *command, const QString &destination,
                                const QStringList &choices) {
  int handled = 0;
  bool selectedFile = false, matched = true;
  QString mismatch;
  const QString outputName = QFileInfo(destination).fileName();
  std::cerr << "animation-workflow: exporter " << command << " destination="
            << outputName.toStdString() << std::endl;
  auto describe = [&](QDialog *dialog, const QString &expected) {
    QStringList details;
    details << QString("dialog=%1 object=%2 title=%3 expected=%4")
                   .arg(dialog->metaObject()->className(), dialog->objectName(),
                        dialog->windowTitle(), expected);
    for (auto *button : dialog->findChildren<QPushButton *>())
      details << QString("button object=%1 text=%2 visible=%3 enabled=%4")
                     .arg(button->objectName(), button->text())
                     .arg(button->isVisible()).arg(button->isEnabled());
    for (auto *label : dialog->findChildren<QLabel *>())
      if (!label->text().isEmpty()) details << label->text().left(400);
    for (auto *text : dialog->findChildren<QTextEdit *>())
      details << text->toPlainText().left(400);
    return details.join("; ").replace(QFileInfo(destination).absolutePath(), "<fixture>")
        .replace('\n', ' ').left(1800);
  };
  QTimer timer;
  QObject::connect(&timer, &QTimer::timeout, [&]() {
    QWidget *modal = QApplication::activeModalWidget();
    if (!modal || dynamic_cast<DVGui::ProgressDialog *>(modal)) return;
    if (auto *file = dynamic_cast<GenericSaveFilePopup *>(modal)) {
      if (selectedFile) return;
      selectedFile = true;
      file->setFolder(TFilePath(QFileInfo(destination).absolutePath()));
      file->setFilename(TFilePath(QFileInfo(destination).fileName()));
      if (auto *check = file->findChild<QCheckBox *>("svgRenderScene")) check->setChecked(false);
      if (auto *check = file->findChild<QCheckBox *>("svgEmbedImages")) check->setChecked(true);
      if (auto *check = file->findChild<QCheckBox *>("svgOpenBrowser")) check->setChecked(false);
      if (auto *check = file->findChild<QCheckBox *>("svgRasterizeVectors")) check->setChecked(false);
      QTimer::singleShot(0, file, &FileBrowserPopup::onOkPressed);
      return;
    }
    auto *dialog = qobject_cast<QDialog *>(modal);
    if (!dialog) return;
    if (handled >= choices.size()) {
      matched = false;
      mismatch = describe(dialog, "no further dialog");
      dialog->reject();
      return;
    }
    const QString choice = choices[handled++];
    for (auto *button : dialog->findChildren<QPushButton *>())
      if (button->isVisible() && (button->objectName() == choice || button->text().remove('&') == choice)) {
        button->click();
        return;
      }
    matched = false;
    mismatch = describe(dialog, choice);
    dialog->reject();
  });
  timer.start(0);
  CommandManager::instance()->execute(command);
  timer.stop();
  if (!selectedFile || !matched || handled != choices.size())
    throw std::runtime_error(QString(
        "native exporter workflow controls did not match: command=%1 output=%2 "
        "fileSelected=%3 choices=%4/%5; %6")
        .arg(command, outputName).arg(selectedFile).arg(handled).arg(choices.size())
        .arg(mismatch).toStdString());
}
QByteArray fileBytes(const QString &path) {
  QFile file(path);
  require(file.open(QIODevice::ReadOnly), "native exporter did not write destination");
  return file.readAll();
}
void exerciseExports(ToonzScene *scene, const QString &folder) {
  scene->getCurrentCamera()->setRes(TDimension(32, 32));
  auto *level = scene->getXsheet()->getCell(0, 0).m_level->getSimpleLevel();
  require(level && level->getPalette(), "export fixture vector palette missing");
  for (const auto &fid : level->getFids()) {
    TVectorImageP image = level->getFrame(fid, true);
    require(bool(image), "export fixture drawing missing");
    image->setPalette(level->getPalette());
    std::vector<TThickPoint> points{TThickPoint(-8, 0, 2), TThickPoint(0, 0, 2), TThickPoint(8, 0, 2)};
    auto *stroke = new TStroke(points);
    stroke->setStyle(1);
    image->addStroke(stroke);
  }
  // The real exporter reports the fixture's sound track as unsupported.
  const QString cancelJson = QDir(folder).filePath("cancel-export.json");
  exportThroughNativeCommand(MI_ExportLottie, cancelJson, {"cancelExportFidelity"});
  require(!QFileInfo::exists(cancelJson) && !QFileInfo::exists(QDir(folder).filePath("cancel-export_images")),
          "Lottie cancel created output files");
  const QString json = QDir(folder).filePath("animation.json");
  exportThroughNativeCommand(MI_ExportLottie, json, {"exportSupportedContent", QObject::tr("OK")});
  const QByteArray jsonBytes = fileBytes(json);
  const auto parsed = QJsonDocument::fromJson(jsonBytes);
  require(parsed.isObject() && !parsed.object().value("layers").toArray().isEmpty(),
          "Lottie consent did not write animation layers");
  exportThroughNativeCommand(MI_ExportLottie, json,
      {QObject::tr("Overwrite"), "cancelExportFidelity"});
  require(fileBytes(json) == jsonBytes, "Lottie cancellation overwrote an existing destination");

  const QString cancelSvg = QDir(folder).filePath("cancel-export.svg");
  exportThroughNativeCommand(MI_ExportAnimatedSVG, cancelSvg, {"cancelExportFidelity"});
  require(!QFileInfo::exists(cancelSvg) && !QFileInfo::exists(QDir(folder).filePath("cancel-export_files")),
          "SVG cancel created output files");
  const QString svg = QDir(folder).filePath("rendered-animation.svg");
  exportThroughNativeCommand(MI_ExportAnimatedSVG, svg, {"renderSceneForExport"});
  const QByteArray svgBytes = fileBytes(svg);
  require(svgBytes.contains("data:image/png;base64,"), "rendered SVG fallback did not embed rendered frames");
  QXmlStreamReader xml(svgBytes);
  while (!xml.atEnd()) xml.readNext();
  require(!xml.hasError(), "rendered SVG output is not well-formed XML");
  const QString assets = QDir(folder).filePath("asset-animation.svg");
  exportThroughNativeCommand(MI_ExportAnimatedSVG, assets, {"exportSupportedContent"});
  require(fileBytes(assets).contains("<svg"), "supported SVG asset export did not write animation");
}

void exerciseNativeTimingBoundaries(ToonzScene *scene) {
  auto *sheet = scene->getXsheet();
  auto *curve = sheet->getStageObject(TStageObjectId::CameraId(0))->getParam(TStageObject::T_Y);
  for (int frame : {-4, -1, 0, 1, 2, 3, 4, 8, 10}) {
    TDoubleKeyframe key(frame, frame * 3.0);
    key.m_type = (frame == -4 || frame == -1 || frame == 8 || frame == 10)
                     ? TDoubleKeyframe::SpeedInOut : TDoubleKeyframe::Linear;
    key.m_speedIn = TPointD(-0.3, -0.6);
    key.m_speedOut = TPointD(0.4, 0.8);
    key.m_linkedHandles = true;
    curve->setKeyframe(key);
  }
  std::vector<TDoubleKeyframe> originals;
  for (int i = 0; i < curve->getKeyframeCount(); ++i) originals.push_back(curve->getKeyframe(i));
  const auto outsideBefore = curve->getKeyframeAt(-4);
  const auto outsideAfter = curve->getKeyframeAt(8);
  const auto outsideLast = curve->getKeyframeAt(10);
  const double beforeSample = curve->getValue(-2.5), afterSample = curve->getValue(9.0);
  QString error;
  require(moveStoryboardTiming(sheet, 1, 2, 5, &error), "native interior storyboard move failed");
  require(sameKey(curve->getKeyframeAt(-4), outsideBefore) &&
          sameKey(curve->getKeyframeAt(8), outsideAfter) &&
          sameKey(curve->getKeyframeAt(10), outsideLast),
          "native storyboard move changed untouched keys or handle links");
  requireNear(curve->getValue(-2.5), beforeSample, "native move changed samples before cut neighbours");
  requireNear(curve->getValue(9.0), afterSample, "native move changed samples after cut neighbours");
  require(TUndoManager::manager()->undo(), "interior timing undo failed");
  require(curve->getKeyframeCount() == int(originals.size()), "interior timing undo changed key count");
  for (size_t i = 0; i < originals.size(); ++i)
    require(sameKey(curve->getKeyframe(int(i)), originals[i]), "interior timing undo changed original key fields");
  curve->clearKeyframes();
}

void exerciseCompatibility(ToonzScene *scene) {
  auto *child = new TXshChildLevel;
  child->setName(L"CompatibilityChild");
  TXsheetP nested = new TXsheet;
  nested->setScene(scene);
  child->setXsheet(nested.getPointer());
  scene->getLevelSet()->insertLevel(child);
  for (int row = 0; row < 4000; ++row)
    scene->getXsheet()->setCell(row, 2, TXshCell(child, TFrameId(1)));
  nested->getNavigationTags()->addTag(0, "Note");
  nested->getNavigationTags()->m_tags.front().m_notes = "Compatibility note";
  const int history = TUndoManager::manager()->getHistoryCount();
  const auto issues = SceneChecker::compatibilityReport(scene);
  int noteCount = 0;
  for (const auto &issue : issues)
    if (issue.xsheet == nested.getPointer() && issue.kind == SceneChecker::Compatibility) ++noteCount;
  require(noteCount == 1, "compatibility nested sheet traversal duplicates or loses report");
  require(nested->getNavigationTags()->m_tags.front().m_notes == "Compatibility note" &&
          TUndoManager::manager()->getHistoryCount() == history, "read-only compatibility report mutated scene");
  auto *level = scene->getXsheet()->getCell(0, 0).m_level.getPointer();
  nested->setCell(0, 0, TXshCell(level, TFrameId(1)));
  nested->setCell(1, 0, TXshCell(level, TFrameId(2)));
  auto *handle = TApp::instance()->getCurrentXsheet();
  handle->setXsheet(nested.getPointer());
  QString error;
  const bool moved = moveStoryboardTiming(nested.getPointer(), 0, 0, 2, &error);
  const bool reordered = nested->getCell(0, 0).getFrameId() == TFrameId(2);
  const bool undone = moved && TUndoManager::manager()->undo();
  handle->setXsheet(scene->getXsheet());
  require(moved && reordered && undone && nested->getCell(0, 0).getFrameId() == TFrameId(1),
          "nested storyboard timing move/undo failed");
  require(scene->getXsheet()->getCell(0, 2).getFrameId() == TFrameId(1),
          "nested timing move changed parent exposure timing");

  auto *unused = new TXshChildLevel;
  unused->setName(L"UnusedCompatibilityChild");
  scene->getLevelSet()->insertLevel(unused);
  unused->setScene(scene);
  unused->getXsheet()->getNavigationTags()->addTag(0, "Unused note");
  unused->getXsheet()->getNavigationTags()->m_tags.front().m_notes = "Saved without exposure";
  int unusedNotes = 0;
  for (const auto &issue : SceneChecker::compatibilityReport(scene))
    if (issue.xsheet == unused->getXsheet() && issue.text.contains("Storyboard notes")) ++unusedNotes;
  require(unusedNotes == 1, "compatibility omitted an unused child xsheet saved in the Scene Cast");

  // Exercise the same numeric parameters that the native editors expose,
  // including containers hidden from the normal FX settings UI. Each family
  // is checked separately so one discovered expression cannot mask omissions.
  auto *object = nested->getStageObject(TStageObjectId::PegbarId(7));
  object->insertOffsetLayer(0, object->createOffsetLayer(L"Compatibility offset"));
  PlasticSkeletonP skeleton = new PlasticSkeleton;
  const int root = skeleton->addVertex(PlasticSkeletonVertex(TPointD(0, 0)), -1);
  const int tip = skeleton->addVertex(PlasticSkeletonVertex(TPointD(0, 100)), root);
  require(skeleton->setVertexName(tip, "CompatibilityTip"), "compatibility Plastic vertex setup failed");
  PlasticSkeletonDeformationP deformation = new PlasticSkeletonDeformation;
  deformation->attach(1, skeleton.getPointer());
  object->setPlasticSkeletonDeformation(deformation);
  auto *vertex = deformation->vertexDeformation(1, tip);
  auto *columnFx = dynamic_cast<TLevelColumnFx *>(nested->getColumn(0)->getFx());
  require(vertex && columnFx, "compatibility native channel owners missing");
  TFxP probe(TFx::create("STD_blurFx"));
  require(bool(probe), "compatibility ordinary FX factory failed");
  TParamSetP container(new TParamSet), inner(new TParamSet);
  TDoubleParamP leaf(new TDoubleParam);
  leaf->setName("compatibilityExpression");
  inner->addParam(leaf, "expression");
  container->addParam(TParamP(inner.getPointer()), "nested");
  probe->getParams()->add(new TParamVarT<TParamP>(
      "compatibilityHidden", nullptr, TParamP(container.getPointer()), true));
  nested->getFxDag()->assignUniqueId(probe.getPointer());
  nested->getFxDag()->getInternalFxs()->addFx(probe.getPointer());
  const std::vector<std::pair<const char *, TDoubleParam *>> curves{
      {"stage", object->getParam(TStageObject::T_X)},
      {"offset channel", object->getOffsetParam(0, TStageObject::T_X)},
      {"offset weight", object->getOffsetLayer(0).m_weight.getPointer()},
      {"Plastic skeleton", deformation->skeletonIdsParam().getPointer()},
      {"Plastic vertex", vertex->m_params[SkVD::ANGLE].getPointer()},
      {"time remap", columnFx->getTimeRemapParam()},
      {"column color", columnFx->getColorEffectParam(ColumnColorEffect::Brightness)},
      {"hidden nested FX", leaf.getPointer()}};
  auto expressionWarnings = [&]() {
    int count = 0;
    for (const auto &issue : SceneChecker::compatibilityReport(scene))
      if (issue.xsheet == nested.getPointer() && issue.text.contains("SeExpr animation")) ++count;
    return count;
  };
  for (const auto &entry : curves) {
    auto *curve = entry.second;
    require(curve, "compatibility numeric channel missing");
    curve->clearKeyframes();
    curve->setGrammar(nested->getStageObjectTree()->getGrammar());
    TDoubleKeyframe first(0, 1), last(3, 1);
    first.m_type = TDoubleKeyframe::Expression;
    first.m_expressionText = "seexpr1:1";
    last.m_prevType = TDoubleKeyframe::Expression;
    last.m_type = TDoubleKeyframe::Linear;
    curve->setKeyframe(first);
    curve->setKeyframe(last);
    const auto before = curve->getKeyframe(0);
    const bool dirty = TApp::instance()->getCurrentScene()->getDirtyFlag();
    const int currentHistory = TUndoManager::manager()->getHistoryCount();
    require(expressionWarnings() == 1,
            (std::string("compatibility omitted or duplicated SeExpr in ") + entry.first).c_str());
    require(sameKey(before, curve->getKeyframe(0)) && curve->getKeyframeCount() == 2 &&
                dirty == TApp::instance()->getCurrentScene()->getDirtyFlag() &&
                currentHistory == TUndoManager::manager()->getHistoryCount(),
            "compatibility expression inspection changed animation or authoring state");
    first.m_type = TDoubleKeyframe::Linear;
    curve->setKeyframe(first);
    require(expressionWarnings() == 0, "compatibility treated inactive expression text as active animation");
    curve->clearKeyframes();
  }
  for (const char *type : {"STD_seexprGeneratorFx", "STD_particleCacheFx", "STD_deformerStackFx",
                          "STD_transform3DFx", "STD_lut3DBakeFx", "STD_neuralStyleFx"}) {
    TFxP effect(TFx::create(type));
    require(bool(effect), (std::string("compatibility FX factory unavailable: ") + type).c_str());
    nested->getFxDag()->assignUniqueId(effect.getPointer());
    nested->getFxDag()->getInternalFxs()->addFx(effect.getPointer());
    int found = 0;
    for (const auto &issue : SceneChecker::compatibilityReport(scene))
      if (issue.xsheet == nested.getPointer() && issue.text.contains(type) &&
          issue.text.contains("requires the matching Groundzero effect")) ++found;
    require(found == 1, (std::string("compatibility omitted or duplicated registered FX: ") + type).c_str());
  }

  SceneCheckPane pane;
  auto *compatibility = pane.findChild<QCheckBox *>("sceneCompatibilityCheck");
  auto *scope = pane.findChild<QLabel *>("sceneCompatibilityScope");
  require(compatibility && scope && !compatibility->isChecked() && scope->isHidden(),
          "native compatibility inventory did not start opt-in");
  pane.show();
  QApplication::processEvents();
  compatibility->setChecked(true);
  require(!scope->isHidden() && scope->text().contains("no historical version") &&
              scope->text().contains("empty report"),
          "native compatibility scope was not visible when enabled");
  compatibility->setChecked(false);
  require(scope->isHidden(), "native compatibility scope did not follow the option");
  pane.hide();
}

void cleanup() {
  auto *app = TApp::instance();
  std::vector<TXshLevel *> levels;
  app->getCurrentScene()->getScene()->getLevelSet()->listLevels(levels);
  for (auto *level : levels) if (auto *simple = level->getSimpleLevel()) {
    simple->getProperties()->setDirtyFlag(false);
    if (simple->getPalette()) simple->getPalette()->setDirtyFlag(false);
  }
  app->getCurrentScene()->setDirtyFlag(false);
  TUndoManager::manager()->reset();
  IoCmd::newScene();
}
}

bool runAnimationWorkflowIntegrationTest(QMainWindow *window, QString *error) {
  try {
    auto checkpoint = [](const char *stage) {
      std::cerr << "animation-workflow: " << stage << std::endl;
    };
    auto *scene = TApp::instance()->getCurrentScene()->getScene();
    require(scene && scene->getXsheet()->getFrameCount() == 0, "animation workflow fixture needs a fresh scene");
    // Use an explicit native browser root: macOS system temporary directories
    // may sit below hidden folders or symlinks that its file tree omits.
    // Library is part of the disposable portable data prepared by the harness.
    const TFilePath libraryPath = ToonzFolder::getLibraryFolder();
    require(QDir().mkpath(libraryPath.getQString()),
            "animation workflow fixture Library unavailable");
    auto *directoryModel = DvDirModel::instance();
    const auto libraryIndex = directoryModel->getIndexByPath(libraryPath);
    require(libraryIndex.isValid(), "native browser Library root unavailable");
    QTemporaryDir temporary(QDir(libraryPath.getQString()).filePath(
        "animation-workflow-XXXXXX"));
    require(temporary.isValid(), "animation workflow fixture directory unavailable");
    directoryModel->refresh(libraryIndex);
    checkpoint("directory refresh and persistent indexes");
    exerciseDirectoryRefresh(scene, temporary.path());
    checkpoint("recovery");
    exerciseRecovery(scene);
    checkpoint("poses");
    exercisePoses(scene, temporary.path());
    checkpoint("timing");
    exerciseTiming(scene, window, temporary.path());
    checkpoint("timing boundaries");
    exerciseNativeTimingBoundaries(scene);
    checkpoint("export files");
    exerciseExports(scene, temporary.path());
    checkpoint("compatibility and nested timing");
    exerciseCompatibility(scene);
    dialogs({"cancelExportFidelity"}, [&]() { require(!ExportFidelity::confirmLottie({"fixture unsupported effect"}), "Lottie cancel accepted export"); });
    dialogs({"exportSupportedContent"}, [&]() { require(ExportFidelity::confirmLottie({"fixture unsupported effect"}), "Lottie explicit consent failed"); });
    dialogs({"cancelExportFidelity"}, [&]() { require(ExportFidelity::chooseAnimatedSvg() == ExportFidelity::Cancel, "SVG cancel accepted export"); });
    dialogs({"renderSceneForExport"}, [&]() { require(ExportFidelity::chooseAnimatedSvg() == ExportFidelity::RenderedScene, "SVG rendered fallback choice failed"); });
    checkpoint("restoring empty scene");
    cleanup();
    require(TApp::instance()->getCurrentScene()->getScene()->getXsheet()->getFrameCount() == 0,
            "animation workflow fixture did not leave a fresh scene");
    checkpoint("PASS");
    return true;
  } catch (const std::exception &failure) {
    if (error) *error = QString::fromUtf8(failure.what());
  } catch (...) {
    if (error) *error = "Unexpected native animation workflow exception";
  }
  return false;
}
