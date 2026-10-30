#include "functionworkflowintegrationtest.h"
#include "functionrigbenchmarkintegrationtest.h"
#include "nativeuiintegrationtest.h"
#include "iocommand.h"
#include "menubarcommandids.h"
#include "tapp.h"
#include "ext/plasticskeleton.h"
#include "ext/plasticskeletondeformation.h"
#include "toonz/tstageobject.h"
#include "toonz/tcolumnfx.h"
#include "toonz/tcolumnfxset.h"
#include "toonz/columncoloreffect.h"
#include "toonz/fxdag.h"
#include "toonz/doubleparamcmd.h"
#include "toonz/tstageobjecttree.h"
#include "toonz/toonzscene.h"
#include "toonz/toonzfolders.h"
#include "toonz/tscenehandle.h"
#include "toonz/sceneproperties.h"
#include "toonz/txsheet.h"
#include "toonz/txshcell.h"
#include "toonz/txshcolumn.h"
#include "toonz/txsheethandle.h"
#include "toonz/txshleveltypes.h"
#include "toonz/txshsimplelevel.h"
#include "toonzqt/functioncurveoperations.h"
#include "toonzqt/functionpanel.h"
#include "toonzqt/functionselection.h"
#include "toonzqt/functiontreeviewer.h"
#include "toonzqt/functionviewer.h"
#include "toonzqt/menubarcommand.h"
#include "texpression.h"
#include "tfx.h"
#include "tparamcontainer.h"
#include "tparamset.h"
#include "tstream.h"
#include "tundo.h"
#include "tvectorimage.h"
#include <QAction>
#include <QApplication>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QDoubleSpinBox>
#include <QEventLoop>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QInputDialog>
#include <QImage>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMainWindow>
#include <QMenu>
#include <QMouseEvent>
#include <QMessageBox>
#include <QPushButton>
#include <QSettings>
#include <QSlider>
#include <QTemporaryDir>
#include <QTimer>
#include <QToolButton>
#include <cmath>
#include <algorithm>
#include <functional>
#include <sstream>
#include <stdexcept>
#include <iostream>

namespace {
namespace Ops = FunctionCurveOperations;
void checkpoint(const char *message) {
  std::cerr << "function-workflow-native: " << message << std::endl;
}
void require(bool value, const char *message) { if (!value) throw std::runtime_error(message); }
void requireNear(double actual, double expected) { require(std::isfinite(actual) && std::abs(actual - expected) < 1.e-7, "Native Function Editor value differs from its oracle"); }
void requireUnkeyedIdentity(TDoubleParam *curve, const std::string &identity,
                            double value = 0.0) {
  if (!curve) throw std::runtime_error("Unkeyed channel is missing: " + identity);
  const double defaultValue = curve->getDefaultValue();
  const double frameZero = curve->getValue(0), frameTen = curve->getValue(10);
  const auto matchesValue = [value](double actual) {
    return std::isfinite(actual) && std::abs(actual - value) < 1.e-7;
  };
  if (curve->getKeyframeCount() != 0 || curve->getWorkspaceId() != identity ||
      !matchesValue(defaultValue) || !matchesValue(frameZero) ||
      !matchesValue(frameTen)) {
    std::ostringstream detail;
    detail.precision(17);
    detail << "Unkeyed channel " << identity << ": expected keys=0, value="
           << value << "; actual identity=" << curve->getWorkspaceId()
           << ", keys=" << curve->getKeyframeCount()
           << ", default=" << defaultValue << ", frame0=" << frameZero
           << ", frame10=" << frameTen;
    throw std::runtime_error(detail.str());
  }
}
void settle() { QApplication::sendPostedEvents(); QApplication::processEvents(QEventLoop::ExcludeUserInputEvents); }
void keys(TDoubleParam *curve, double from, double to, const std::string &formula = {}) {
  curve->clearKeyframes();
  TDoubleKeyframe a(0, from), b(20, to);
  a.m_type = formula.empty() ? TDoubleKeyframe::Linear : TDoubleKeyframe::Expression;
  a.m_expressionText = formula; b.m_prevType = a.m_type;
  curve->setKeyframe(a); curve->setKeyframe(b);
}
FunctionTreeModel::Channel *find(FunctionTreeView *tree, TDoubleParam *curve) {
  for (auto *channel : tree->channels()) if (channel->getParam() == curve) return channel;
  throw std::runtime_error("Native Function Editor omitted a scene/Plastic/offset channel");
}
void paint(FunctionPanel *graph) {
  QImage image(graph->size(), QImage::Format_ARGB32_Premultiplied); image.fill(Qt::transparent); graph->render(&image);
}
void mouse(FunctionPanel *graph, QEvent::Type type, const QPoint &point, Qt::MouseButton button,
           Qt::MouseButtons buttons, Qt::KeyboardModifiers modifiers = {}) {
  QMouseEvent event(type, QPointF(point), button, buttons, modifiers); QApplication::sendEvent(graph, &event);
}
void drag(FunctionPanel *graph, const QPoint &from, const QPoint &to, Qt::KeyboardModifiers modifiers = {}) {
  mouse(graph, QEvent::MouseMove, from, Qt::NoButton, Qt::NoButton, modifiers);
  mouse(graph, QEvent::MouseButtonPress, from, Qt::LeftButton, Qt::LeftButton, modifiers);
  mouse(graph, QEvent::MouseMove, to, Qt::NoButton, Qt::LeftButton, modifiers);
  mouse(graph, QEvent::MouseButtonRelease, to, Qt::LeftButton, Qt::NoButton, modifiers);
}
void modal(FunctionViewer *viewer, const char *dialogName, const std::function<void(QDialog *)> &inspect,
           const std::function<void()> &open) {
  QString failure; bool visited = false;
  // A disabled action can return without entering a modal loop. Tie pending
  // delivery to this call's lifetime rather than leaving its captures queued
  // against the longer-lived editor.
  QObject delivery;
  QTimer deadline;
  deadline.setSingleShot(true);
  QObject::connect(&deadline, &QTimer::timeout, &delivery, [dialogName]() {
    std::cerr << "function-workflow-native: expected modal: " << dialogName << std::endl;
    if (auto *modal = QApplication::activeModalWidget())
      std::cerr << "function-workflow-native: blocked modal: "
                << modal->objectName().toStdString() << " ("
                << modal->windowTitle().toStdString() << ")" << std::endl;
    qFatal("function-workflow-native: modal fixture exceeded its 15-second deadline");
  });
  QTimer::singleShot(0, &delivery, [&]() {
    auto *dialog = viewer->findChild<QDialog *>(dialogName);
    try { require(dialog, "Native function dialog did not open"); visited = true; inspect(dialog); }
    catch (const std::exception &error) { failure = QString::fromUtf8(error.what()); }
    if (dialog) dialog->reject();
  });
  checkpoint(dialogName);
  deadline.start(15000);
  open();
  deadline.stop();
  require(visited, "Native modal callback was not reached");
  if (!failure.isEmpty()) throw std::runtime_error(failure.toStdString());
}
void ownerRemovalDialogs(FunctionViewer *viewer, FunctionTreeView *tree,
                         FunctionPanel *graph, TXsheet *sheet,
                         TXshSimpleLevel *level, TDoubleParam *stable,
                         const QTemporaryDir &directory) {
  checkpoint("rejecting pending edits after native column/offset/FX owner removal");
  auto *app = TApp::instance();
  auto *undo = TUndoManager::manager();
  auto *edit = viewer->findChild<QToolButton *>("FunctionEditKeys");
  auto *presets = viewer->findChild<QAction *>("FunctionMotionPresetsAction");
  require(edit && presets, "Owner-removal fixture actions are missing");
  const QString library = (ToonzFolder::getMyModuleDir() + TFilePath("function-presets")).getQString();
  require(QDir().mkpath(library), "Owner-removal preset fixture folder is unavailable");
  const QString presetPath = QDir(library).filePath("Owner guard fixture.fcurve");
  const QString forbiddenPath = QDir(library).filePath("Removed owner must not publish.fcurve");
  require(!QFileInfo::exists(presetPath) && !QFileInfo::exists(forbiddenPath),
          "Owner-removal preset names are not isolated");
  enum Route { Edit, Preset, SaveName, ImportFile, ImportName, EmptyCells };
  const std::vector<std::pair<int, Route>> cases{{0, Edit}, {1, Edit}, {2, Edit},
      {0, Preset}, {1, Preset}, {2, Preset}, {2, SaveName}, {0, ImportFile}, {1, ImportName},
      {0, EmptyCells}, {1, EmptyCells}, {2, EmptyCells}};
  for (const auto &test : cases) {
    const int column = sheet->getColumnCount();
    require(sheet->setCell(0, column, TXshCell(level, TFrameId(1))),
            "Cannot create disposable owner-removal column");
    TStageObjectP object = sheet->getStageObject(TStageObjectId::ColumnId(column));
    TFxP fx;
    TDoubleParamP target;
    if (test.first == 0) target = object->getParam(TStageObject::T_X);
    else if (test.first == 1) {
      object->insertOffsetLayer(0, object->createOffsetLayer(L"Pending edit owner"));
      target = object->getOffsetParam(0, TStageObject::T_X);
    } else {
      fx = TFx::create("STD_blurFx");
      require(fx, "Cannot create owner-removal effect");
      sheet->getFxDag()->assignUniqueId(fx.getPointer());
      sheet->getFxDag()->getInternalFxs()->addFx(fx.getPointer());
      target = dynamic_cast<TDoubleParam *>(fx->getParams()->getParam("value"));
    }
    require(target, "Owner-removal fixture parameter is missing");
    keys(target.getPointer(), 1, 9);
    TDoubleParamP before = new TDoubleParam(*target), stableBefore = new TDoubleParam(*stable);
    const QString rawPath = directory.filePath("owner-guard.curve");
    { TOStream output{TFilePath(rawPath)}; before->saveData(output); }
    { TOStream output{TFilePath(presetPath)};
      output.openChild("functionPreset", {{"measure", target->getMeasureName()}});
      before->saveData(output); output.closeChild(); }
    app->getCurrentXsheet()->notifyXsheetChanged(); settle();
    auto *channel = find(tree, target.getPointer());
    channel->setIsActive(true); channel->setIsCurrent(true);
    viewer->getSelection()->selectNone();
    viewer->getSelection()->select(target.getPointer(), 0);
    if (test.second == Edit) viewer->getSelection()->select(stable, 0);
    if (test.second == EmptyCells) {
      viewer->getSelection()->selectCells(QRect(0, 5, 1, 2), {target.getPointer()});
      require(viewer->getSelection()->getSelectedKeyframeCount() == 0 && !viewer->getSelection()->isEmpty(),
              "Between-key cell fixture did not retain its owner without selected keys");
    }
    undo->reset(); app->getCurrentScene()->setDirtyFlag(false);
    bool removed = false;
    int historyAfterRemoval = -1, indexAfterRemoval = -1;
    bool dirtyAfterRemoval = false;
    auto removeOwner = [&]() {
      require(!removed, "Native fixture attempted duplicate owner removal");
      removed = true;
      if (test.first == 0) sheet->removeColumn(column);
      else if (test.first == 1) object->removeOffsetLayer(0);
      else require(sheet->getFxDag()->getInternalFxs()->removeFx(fx.getPointer()),
                   "Could not remove pending effect owner");
      app->getCurrentXsheet()->notifyXsheetChanged();
      for (auto *remaining : tree->channels())
        require(remaining->getParam() != target.getPointer(), "Removed owner remains in native Function model");
      require(!viewer->getSelection()->isSelected(target.getPointer(), 0),
              "Removed owner remains available to selection commands");
      if (test.second == Edit)
        require(viewer->getSelection()->isSelected(stable, 0),
                "Owner removal discarded surviving selected keys");
      historyAfterRemoval = undo->getHistoryCount(); indexAfterRemoval = undo->getCurrentHistoryIndex();
      dirtyAfterRemoval = app->getCurrentScene()->getDirtyFlag();
    };
    if (test.second == EmptyCells) {
      removeOwner();
      require(viewer->getSelection()->isEmpty() && viewer->getSelection()->getSelectedCells().isEmpty(),
              "Removed owner retained a between-key cell range");
      viewer->getSelection()->setStep(3, true);
      viewer->getSelection()->doCut(); viewer->getSelection()->doDelete();
    } else modal(viewer, test.second == Edit ? "FunctionKeyEditor" : "FunctionMotionPresets",
          [&](QDialog *dialog) {
      auto *apply = dialog->findChild<QDialogButtonBox *>()->button(QDialogButtonBox::Apply);
      auto *previewButton = dialog->findChild<QPushButton *>(test.second == Edit ? "FunctionEditPreview" : "FunctionPresetPreview");
      require(apply && previewButton, "Owner-removal modal controls are missing");
      if (test.second == Edit) dialog->findChild<QDoubleSpinBox *>("FunctionEditAmount")->setValue(99);
      else {
        auto *list = dialog->findChild<QListWidget *>("FunctionPresetList");
        require(list, "Owner-removal preset browser is missing");
        int row = -1;
        for (int i = 0; i < list->count(); ++i)
          if (list->item(i)->data(Qt::UserRole).toString() == "Owner guard fixture.fcurve") row = i;
        require(row >= 0, "Owner-removal native preset is missing"); list->setCurrentRow(row);
        dialog->findChild<QDoubleSpinBox *>("FunctionPresetOffset")->setValue(99);
      }
      app->getCurrentXsheet()->notifyXsheetChanged();
      require(dialog->isVisible(), "Harmless native owner refresh closed an edit dialog");
      previewButton->click(); require(!graph->comparison().empty(), "Owner-removal preview did not prepare changes");
      if (test.second == Edit || test.second == Preset) {
        removeOwner(); require(!dialog->isVisible(), "Removed native owner left its pending dialog open");
        // Already-queued callbacks must also revalidate, even after rejection.
        previewButton->click(); apply->click();
      } else {
        QTimer driver; QString failure; bool fileChosen = false, nestedVisited = false;
        QObject::connect(&driver, &QTimer::timeout, dialog, [&]() {
          try {
            if (auto *file = qobject_cast<QFileDialog *>(QApplication::activeModalWidget())) {
              if (fileChosen) return;
              require(file->objectName() == "FunctionPresetImportFile", "Unexpected owner-removal chooser");
              fileChosen = true;
              if (test.second == ImportFile) {
                nestedVisited = true; removeOwner();
                require(!file->isVisible() && !dialog->isVisible(), "Owner removal retained the import file chooser");
                return;
              }
              auto *name = file->findChild<QLineEdit *>("fileNameEdit");
              auto *buttons = file->findChild<QDialogButtonBox *>();
              auto *open = buttons ? buttons->button(QDialogButtonBox::Open) : nullptr;
              require(name && open, "Native import chooser controls are missing");
              name->setText(QFileInfo(rawPath).absoluteFilePath());
              require(file->selectedFiles().size() == 1 && open->isEnabled() &&
                          QFileInfo(file->selectedFiles().front()).canonicalFilePath() == QFileInfo(rawPath).canonicalFilePath(),
                      "Owner-removal chooser did not select its exact native curve");
              QTimer::singleShot(0, file, [open]() { open->click(); });
            } else if (auto *name = qobject_cast<QInputDialog *>(QApplication::activeModalWidget())) {
              if (nestedVisited) return;
              require(name->objectName() == (test.second == SaveName ? "FunctionPresetSaveName" : "FunctionPresetImportName"),
                      "Unexpected owner-removal name prompt");
              nestedVisited = true; name->setTextValue("Removed owner must not publish"); removeOwner();
              require(!name->isVisible() && !dialog->isVisible(), "Owner removal retained the nested preset name prompt");
              name->accept(); // Late delivery cannot publish the detached curve.
            }
          } catch (const std::exception &problem) {
            failure = QString::fromUtf8(problem.what());
            if (auto *active = qobject_cast<QDialog *>(QApplication::activeModalWidget())) active->reject();
          }
        });
        driver.start(5);
        dialog->findChild<QPushButton *>(test.second == SaveName ? "FunctionPresetSave" : "FunctionPresetImport")->click();
        driver.stop(); require(failure.isEmpty() && nestedVisited && removed, qPrintable(failure));
      }
    }, [&]() { if (test.second == Edit) edit->click(); else presets->trigger(); });
    require(removed && Ops::matchesSnapshot(target.getPointer(), before.getPointer()) &&
                Ops::matchesSnapshot(stable, stableBefore.getPointer()),
            "Owner removal allowed a detached or surviving channel to be edited");
    require(undo->getHistoryCount() == historyAfterRemoval && undo->getCurrentHistoryIndex() == indexAfterRemoval &&
                app->getCurrentScene()->getDirtyFlag() == dirtyAfterRemoval && !QFileInfo::exists(forbiddenPath),
            "Rejected detached target changed history, dirty state or preset library");
    require(graph->comparison().empty(), "Rejected detached target retained its preview");
    viewer->getSelection()->selectNone();
    if (test.first != 0) sheet->removeColumn(column);
    require(QFile::remove(presetPath), "Owner-removal preset cleanup failed");
    app->getCurrentXsheet()->notifyXsheetChanged(); settle();
  }
  undo->reset(); app->getCurrentScene()->setDirtyFlag(false);
  checkpoint("native owner removal canceled edit/preset/Save/Import without phantom history or writes");
}
void dependencies(FunctionViewer *viewer, FunctionTreeView *tree, TDoubleParam *target,
                  TDoubleParam *source, int expectedCount) {
  find(tree, target)->setIsCurrent(true);
  auto *action = viewer->findChild<QAction *>("FunctionDependenciesAction"); require(action, "Dependency action missing");
  modal(viewer, "FunctionDependenciesDialog", [&](QDialog *dialog) {
    auto *list = dialog->findChild<QListWidget *>("FunctionDependenciesList"); require(list && list->count() == expectedCount, "Native resolved dependency list has unexpected bindings");
    require(list->item(0)->data(Qt::UserRole).isValid(), "Resolved dependency cannot be navigated");
    list->itemDoubleClicked(list->item(0));
    require(viewer->getCurrentChannelCurve() == source, "Dependency navigation selected a different channel");
  }, [&]() { action->trigger(); });
}

void frozenComparison(FunctionViewer *viewer, FunctionTreeView *tree,
                      FunctionPanel *graph, TDoubleParam *source,
                      TDoubleParam *native, TDoubleParam *seexpr, TDoubleParam *offset) {
  checkpoint("frozen native and SeExpr comparison");
  for (auto *channel : tree->channels())
    channel->setIsActive(channel->getParam() == source || channel->getParam() == native ||
                         channel->getParam() == seexpr);
  find(tree, source)->setIsCurrent(true);
  // Keep the sampled visible domain inside the two driven endpoint keys.
  graph->fitRegion(1, Ops::displayValue(source, -5), 19, Ops::displayValue(source, 40));
  auto *capture = viewer->findChild<QAction *>("FunctionCaptureComparison");
  auto *toggle = viewer->findChild<QAction *>("FunctionToggleComparison");
  auto *restore = viewer->findChild<QAction *>("FunctionRestoreComparison");
  require(capture && toggle && restore, "Comparison controls are missing");
  TDoubleParamP original = new TDoubleParam(*source);
  TDoubleParamP originalOffset = new TDoubleParam(*offset);
  capture->trigger();
  const auto before = graph->comparisonState();
  require(before.frozen.size() == 3 && before.intervals >= 64 && before.last > before.first,
          "Comparison did not capture every visible native/driven channel");
  auto *info = viewer->findChild<QLabel *>("FunctionComparisonInfo");
  require(info && info->isVisible() && info->text().contains("frames/sample"),
          "Comparison does not disclose its sampled domain and resolution");
  for (const auto &curve : before.frozen) for (const auto &point : curve.samples) {
    const double factor = curve.owner.getPointer() == native ? 3 : curve.owner.getPointer() == seexpr ? 2 : 1;
    const double effectiveSource = original->getValue(point.x()) +
        (factor == 1 ? 0 : originalOffset->getValue(point.x()));
    requireNear(point.y(), factor * effectiveSource);
  }
  auto assertFrozen = [&]() {
    const auto actual = graph->comparisonState();
    require(actual.frozen.size() == before.frozen.size() && actual.intervals == before.intervals,
            "Comparison lost its frozen display");
    for (size_t i = 0; i < before.frozen.size(); ++i)
      require(actual.frozen[i].owner == before.frozen[i].owner &&
                  actual.frozen[i].samples == before.frozen[i].samples,
              "Comparison reevaluated a captured native/SeExpr dependency");
  };
  QString error;
  require(Ops::apply(Ops::planValues({{source, {0, 1, 2}}}, Ops::ValueMode::Offset,
                                    Ops::displayValue(source, 2) - Ops::displayValue(source, 0)),
                     "Comparison fixture source edit", error), qPrintable(error));
  settle(); paint(graph);
  requireNear(native->getValue(10), 21); requireNear(seexpr->getValue(10), 14);
  assertFrozen(); toggle->trigger(); require(graph->comparison().empty(), "Hide retained comparison display");
  toggle->trigger(); assertFrozen();
  viewer->getSelection()->selectNone(); viewer->getSelection()->select(source, 1);
  modal(viewer, "FunctionKeyEditor", [&](QDialog *dialog) {
    dialog->findChild<QComboBox *>("FunctionEditOperation")->setCurrentIndex(1);
    dialog->findChild<QDoubleSpinBox *>("FunctionEditAmount")->setValue(999);
    dialog->findChild<QPushButton *>("FunctionEditPreview")->click();
    require(graph->comparisonState().intervals == 0, "Preview did not temporarily replace the captured overlay");
  }, [&]() { viewer->findChild<QToolButton *>("FunctionEditKeys")->click(); });
  assertFrozen(); requireNear(source->getValue(10), 6);
  auto confirmation = [&](QMessageBox::StandardButton answer) {
    QObject delivery; QString failure; bool visited = false;
    QTimer deadline; deadline.setSingleShot(true);
    QObject::connect(&deadline, &QTimer::timeout, &delivery, []() { qFatal("Comparison confirmation fixture timed out"); });
    QTimer::singleShot(0, &delivery, [&]() {
      auto *box = qobject_cast<QMessageBox *>(QApplication::activeModalWidget());
      try {
        // QMessageBox deliberately ignores window titles on macOS. Identify
        // this native confirmation by its owner, prompt and real answer buttons.
        require(box, "Expected comparison restore message box was not shown");
        require(box->parentWidget() == viewer, "Comparison confirmation has the wrong owner");
        require(box->icon() == QMessageBox::Question && box->text() ==
                    "Restore all captured channels? Edits since capture will be replaced as one undo step.",
                "Comparison confirmation has the wrong question");
        require(box->standardButtons() == (QMessageBox::Yes | QMessageBox::No) &&
                    box->button(answer) && box->button(answer)->isEnabled(),
                "Comparison confirmation has missing or disabled answer buttons");
        visited = true; box->button(answer)->click();
      } catch (const std::exception &problem) { failure = QString::fromUtf8(problem.what()); if (box) box->reject(); }
    });
    deadline.start(15000); restore->trigger(); deadline.stop();
    require(visited && failure.isEmpty(), qPrintable(failure));
  };
  const int history = TUndoManager::manager()->getHistoryCount();
  confirmation(QMessageBox::No);
  requireNear(source->getValue(10), 6); assertFrozen();
  require(TUndoManager::manager()->getHistoryCount() == history, "Canceled restore changed history");
  confirmation(QMessageBox::Yes);
  require(Ops::matchesSnapshot(source, original.getPointer()), "Restore did not recover exact native source data");
  require(TUndoManager::manager()->getHistoryCount() == history + 1, "Restore was not one transaction");
  require(TUndoManager::manager()->undo(), "Comparison restore cannot be undone"); requireNear(source->getValue(10), 6);
  require(TUndoManager::manager()->redo(), "Comparison restore cannot be redone");
  require(Ops::matchesSnapshot(source, original.getPointer()) &&
              native->getKeyframe(0).m_expressionText == "col1.x*3" &&
              seexpr->getKeyframe(0).m_expressionText == TExpression::makeSeExprText("channel(\"col1.x\")*2"),
          "Comparison restore baked or rebound native/SeExpr owners");
  TUndoManager::manager()->reset(); viewer->getSelection()->selectNone(); toggle->trigger();
}
void renameNativeSet(FunctionViewer *viewer, FunctionTreeView *tree,
                     const std::vector<TDoubleParam *> &channels) {
  checkpoint("renaming saved channel set with native scene identities");
  const QString path = (ToonzFolder::getMyModuleDir() + TFilePath("function-editor.ini")).getQString();
  QSettings settings(path, QSettings::IniFormat);
  const QVariant previous = settings.value("channelSets");
  struct RestoreSettings {
    QString path; QVariant previous;
    ~RestoreSettings() {
      QSettings restore(path, QSettings::IniFormat);
      if (previous.isValid()) restore.setValue("channelSets", previous);
      else restore.remove("channelSets");
      restore.sync();
    }
  } restore{path, previous};
  QJsonArray ids;
  for (auto *curve : channels) {
    require(!curve->getWorkspaceId().empty(), "Native set fixture has an unidentified channel");
    ids.append(QString::fromStdString(curve->getWorkspaceId()));
  }
  QJsonArray expected{QJsonObject{{"name", "Native rig channels"}, {"ids", ids}, {"extension", "preserved"}}};
  settings.setValue("channelSets", QJsonDocument(expected).toJson(QJsonDocument::Compact)); settings.sync();
  require(settings.status() == QSettings::NoError, "Cannot prepare native saved set");
  auto *action = viewer->findChild<QAction *>("FunctionChannelSetsAction");
  require(action, "Native Channel Sets action missing");
  auto *sceneHandle = TApp::instance()->getCurrentScene();
  auto *undo = TUndoManager::manager();
  const int history = undo->getHistoryCount(), historyIndex = undo->getCurrentHistoryIndex();
  sceneHandle->setDirtyFlag(false);
  modal(viewer, "FunctionChannelSets", [&](QDialog *dialog) {
    auto *name = dialog->findChild<QLineEdit *>("FunctionChannelSetName");
    auto *rename = dialog->findChild<QPushButton *>("FunctionChannelSetRename");
    require(name && rename, "Native saved-set rename controls missing");
    name->setText("Native rig timing"); rename->click();
    QJsonObject entry = expected[0].toObject(); entry["name"] = "Native rig timing"; expected[0] = entry;
    QSettings observed(path, QSettings::IniFormat); observed.sync();
    require(QJsonDocument::fromJson(observed.value("channelSets").toByteArray()).array() == expected,
            "Native rename changed channel identities or failed to persist");
    name->setText("Uncommitted native name");
  }, [&]() { action->trigger(); });
  QSettings reopened(path, QSettings::IniFormat); reopened.sync();
  require(QJsonDocument::fromJson(reopened.value("channelSets").toByteArray()).array() == expected &&
              !sceneHandle->getDirtyFlag() && undo->getHistoryCount() == history &&
              undo->getCurrentHistoryIndex() == historyIndex,
          "Native rename cancel changed scene/history or committed the unfinished name");
  modal(viewer, "FunctionChannelSets", [&](QDialog *dialog) {
    auto *list = dialog->findChild<QListWidget *>("FunctionChannelSetList");
    auto *show = dialog->findChild<QPushButton *>("FunctionChannelSetShow");
    require(list && show && list->count() == 1 && list->item(0)->text() == "Native rig timing",
            "Renamed native channel set did not reopen");
    show->click();
    for (auto *channel : tree->channels()) {
      bool wanted = std::find(channels.begin(), channels.end(), channel->getParam()) != channels.end();
      require(channel->isActive() == wanted, "Renamed set recalled the wrong native channel membership");
    }
  }, [&]() { action->trigger(); });
}
void reportNumericRetimeCurve(TDoubleParam *curve, const QTemporaryDir &directory,
                             const char *phase) {
  // This fixture owns these three-key numeric channels. Retain their native
  // resource representation in the report, never the scene's temporary paths.
  try {
    if (!curve || curve->getKeyframeCount() != 3) return;
    for (int key = 0; key < curve->getKeyframeCount(); ++key) {
      const auto type = curve->getKeyframe(key).m_type;
      if (type != TDoubleKeyframe::Constant && type != TDoubleKeyframe::Linear &&
          type != TDoubleKeyframe::Exponential && type != TDoubleKeyframe::SpeedInOut &&
          type != TDoubleKeyframe::EaseInOut && type != TDoubleKeyframe::EaseInOutPercentage)
        return;  // Driven/file data could contain unrelated paths or text.
    }
    TDoubleParam snapshot(*curve); snapshot.setWorkspaceId("");
    const QString path = directory.filePath(QString::fromLatin1(phase) + ".curve");
    { TOStream output{TFilePath(path)}; if (!output) return; snapshot.saveData(output); }
    QFile persisted(path);
    if (!persisted.open(QIODevice::ReadOnly)) return;
    const QByteArray bytes = persisted.read(4097);
    if (bytes.size() > 4096) return;  // At most 8 KiB for the expected/reopened pair.
    std::cerr << "function-retime-serialization: " << phase << '\n'
              << bytes.constData() << "\nfunction-retime-serialization: end" << std::endl;
  } catch (...) {
    std::cerr << "function-retime-serialization: unavailable" << std::endl;
  }
}
void retimeNativeRange(FunctionViewer *viewer, ToonzScene *scene,
                       const std::vector<TDoubleParam *> &channels,
                       const QTemporaryDir &directory) {
  checkpoint("retiming native multi-channel selection to a fractional target range");
  auto *selection = viewer->getSelection(); selection->selectNone();
  std::vector<TDoubleParamP> before;
  for (auto *curve : channels) {
    require(curve->getKeyframeCount() == 3, "Native range fixture expected three keys per channel");
    before.emplace_back(new TDoubleParam(*curve));
    for (int k = 0; k < curve->getKeyframeCount(); ++k) selection->select(curve, k);
  }
  auto *undo = TUndoManager::manager(); undo->reset();
  const double targetFirst = 1.123456789, targetMiddle = 16.555555555,
               targetLast = 31.987654321;
  const auto exactFrame = [](double actual, double expected) {
    if (!std::isfinite(actual) || std::abs(actual - expected) >= 1.e-11) {
      std::ostringstream message; message.precision(17);
      message << "Fractional retime frame: expected " << expected << ", observed " << actual;
      throw std::runtime_error(message.str());
    }
  };
  auto *edit = viewer->findChild<QToolButton *>("FunctionEditKeys");
  require(edit, "Native target-range action missing");
  modal(viewer, "FunctionKeyEditor", [&](QDialog *dialog) {
    auto *mode = dialog->findChild<QComboBox *>("FunctionEditOperation");
    auto *first = dialog->findChild<QDoubleSpinBox *>("FunctionEditFirstFrame");
    auto *last = dialog->findChild<QDoubleSpinBox *>("FunctionEditLastFrame");
    auto *preview = dialog->findChild<QPushButton *>("FunctionEditPreview");
    auto *buttons = dialog->findChild<QDialogButtonBox *>();
    require(mode && first && last && preview && buttons, "Native target-range controls missing");
    mode->setCurrentIndex(8); first->setValue(targetFirst + 1); last->setValue(targetLast + 1);
    preview->click();
    for (size_t i = 0; i < channels.size(); ++i)
      require(Ops::matchesSnapshot(channels[i], before[i].getPointer()), "Native target preview changed curve data");
    require(undo->getHistoryCount() == 0, "Native target preview changed undo history");
    buttons->button(QDialogButtonBox::Apply)->click();
    for (auto *curve : channels) {
      exactFrame(curve->keyframeIndexToFrame(0), targetFirst);
      exactFrame(curve->keyframeIndexToFrame(1), targetMiddle);
      exactFrame(curve->keyframeIndexToFrame(2), targetLast);
    }
    require(undo->getHistoryCount() == 1 && selection->getSelectedKeyframeCount() == 9,
            "Native target range lost selection or created multiple undo steps");
    buttons->button(QDialogButtonBox::Apply)->click();
    require(undo->getHistoryCount() == 1, "Native repeated target range created an extra undo step");
    first->setValue(4); last->setValue(51); preview->click();
  }, [&]() { edit->click(); });
  for (auto *curve : channels) exactFrame(curve->keyframeIndexToFrame(0), targetFirst);
  for (auto *curve : channels) curve->setEditorLocked(true);
  require(undo->undo(), "Native target range has no undo");
  for (size_t i = 0; i < channels.size(); ++i)
    require(Ops::matchesSnapshot(channels[i], before[i].getPointer()) && channels[i]->isEditorLocked(),
            "Native target-range undo lost original data or session locks");
  require(undo->redo(), "Native target range has no redo");
  for (auto *curve : channels) {
    require(curve->isEditorLocked(), "Native target-range redo cleared a session lock");
    exactFrame(curve->keyframeIndexToFrame(2), targetLast);
  }
  const TFilePath path(directory.filePath("function-target-range.tnz")); scene->saveCopy(path);
  ToonzScene loaded; loaded.loadTnzFile(path);
  auto *object = loaded.getXsheet()->getStageObject(TStageObjectId::ColumnId(0));
  auto deformation = object->getPlasticSkeletonDeformation();
  require(deformation && deformation->vertexDeformation("FunctionTip"), "Retime scene lost its Plastic rig");
  const std::vector<TDoubleParam *> reopened{
      object->getParam(TStageObject::T_X), object->getOffsetParam(0, TStageObject::T_X),
      deformation->vertexDeformation("FunctionTip")->m_params[SkVD::ANGLE].getPointer()};
  for (size_t i = 0; i < reopened.size(); ++i) {
    require(reopened[i] && reopened[i]->getKeyframeCount() == 3 && !reopened[i]->isEditorLocked() &&
                reopened[i]->getWorkspaceId() == channels[i]->getWorkspaceId(),
            "Native retime scene lost channel identity/count or persisted session locks");
    exactFrame(reopened[i]->keyframeIndexToFrame(0), targetFirst);
    exactFrame(reopened[i]->keyframeIndexToFrame(1), targetMiddle);
    exactFrame(reopened[i]->keyframeIndexToFrame(2), targetLast);
    for (int k = 0; k < 3; ++k) {
      const auto actual = reopened[i]->getKeyframe(k), expected = channels[i]->getKeyframe(k);
      const bool exact = actual.m_frame == expected.m_frame && actual.m_value == expected.m_value &&
                  actual.m_type == expected.m_type && actual.m_prevType == expected.m_prevType &&
                  actual.m_step == expected.m_step && actual.m_speedIn == expected.m_speedIn &&
                  actual.m_speedOut == expected.m_speedOut && actual.m_linkedHandles == expected.m_linkedHandles &&
                  actual.m_expressionText == expected.m_expressionText;
      if (!exact) {
        std::ostringstream detail; detail.precision(17);
        detail << "Native retime roundtrip changed authored frame/value/handles/interpolation"
               << "; channel=" << i << ", key=" << k;
        const auto mismatch = [&](const char *field, const auto &authored, const auto &observed) {
          if (authored != observed)
            detail << "; " << field << " expected=" << authored << ", observed=" << observed;
        };
        mismatch("frame", expected.m_frame, actual.m_frame);
        mismatch("value", expected.m_value, actual.m_value);
        mismatch("type", int(expected.m_type), int(actual.m_type));
        mismatch("prevType", int(expected.m_prevType), int(actual.m_prevType));
        mismatch("step", expected.m_step, actual.m_step);
        mismatch("speedIn.x", expected.m_speedIn.x, actual.m_speedIn.x);
        mismatch("speedIn.y", expected.m_speedIn.y, actual.m_speedIn.y);
        mismatch("speedOut.x", expected.m_speedOut.x, actual.m_speedOut.x);
        mismatch("speedOut.y", expected.m_speedOut.y, actual.m_speedOut.y);
        mismatch("linkedHandles", expected.m_linkedHandles, actual.m_linkedHandles);
        mismatch("expression", expected.m_expressionText, actual.m_expressionText);
        detail << "; authored interpolation=" << int(expected.m_type)
               << ", preceding interpolation=" << int(expected.m_prevType);
        reportNumericRetimeCurve(channels[i], directory, "expected");
        reportNumericRetimeCurve(reopened[i], directory, "reopened");
        throw std::runtime_error(detail.str());
      }
    }
  }
  require(undo->undo(), "Native retime cleanup could not restore original curves");
  for (auto *curve : channels) curve->setEditorLocked(false);
  undo->reset();
  checkpoint("native target-range precision, cancel, locked undo/redo and scene reload passed");
}
}

bool runFunctionWorkflowIntegrationTest(QMainWindow *, QString *error) {
  auto *app = TApp::instance();
  try {
    // Called only by the isolated native application runner, after its earlier
    // fixture has discarded dirty state. Use the real New Scene command/handles.
    checkpoint("resetting disposable scene");
    QString resetError;
    require(resetNativeUiIntegrationScene(&resetError), qPrintable(resetError));
    settle();
    checkpoint("constructing native scene channels");
    auto *scene = app->getCurrentScene()->getScene();
    auto *xsheet = app->getCurrentXsheet()->getXsheet();
    require(scene && xsheet && xsheet->getFrameCount() == 0, "Function fixture requires an empty disposable scene");
    QTemporaryDir directory; require(directory.isValid(), "Function fixture storage unavailable");
    auto *level = scene->createNewLevel(PLI_XSHLEVEL, L"FunctionWorkflowFixture")->getSimpleLevel();
    require(level, "Cannot create native function fixture level");
    TVectorImageP image = new TVectorImage; image->setPalette(level->getPalette()); level->setFrame(TFrameId(1), image);
    TFilePath levelPath(directory.filePath("fixture.pli")); level->setPath(levelPath); level->save(levelPath);
    for (int c = 0; c < 3; ++c) require(xsheet->setCell(0, c, TXshCell(level, TFrameId(1))), "Cannot expose native function fixture column");
    auto *object = xsheet->getStageObject(TStageObjectId::ColumnId(0));
    auto *source = object->getParam(TStageObject::T_X);
    auto *native = xsheet->getStageObject(TStageObjectId::ColumnId(1))->getParam(TStageObject::T_X);
    auto *seexpr = xsheet->getStageObject(TStageObjectId::ColumnId(2))->getParam(TStageObject::T_X);
    const std::string nativeText = "col1.x*3";
    const std::string seexprText = TExpression::makeSeExprText("channel(\"col1.x\")*2");
    keys(source, 0, 8); source->setKeyframe(TDoubleKeyframe(10, 4));
    keys(native, 0, 0, nativeText); keys(seexpr, 0, 0, seexprText);
    object->insertOffsetLayer(0, object->createOffsetLayer(L"Performance offset"));
    auto *offset = object->getOffsetParam(0, TStageObject::T_X); require(offset, "Native offset layer missing"); keys(offset, 0, 2);
    PlasticSkeletonP skeleton = new PlasticSkeleton;
    int root = skeleton->addVertex(PlasticSkeletonVertex(TPointD(0, 0)), -1);
    int tip = skeleton->addVertex(PlasticSkeletonVertex(TPointD(0, 100)), root);
    require(skeleton->setVertexName(tip, "FunctionTip"), "Cannot name native Plastic vertex");
    PlasticSkeletonDeformationP deformation = new PlasticSkeletonDeformation;
    deformation->attach(1, skeleton.getPointer()); object->setPlasticSkeletonDeformation(deformation);
    auto *vertex = deformation->vertexDeformation(1, tip); require(vertex, "Native Plastic deformation vertex missing");
    auto *angle = vertex->m_params[SkVD::ANGLE].getPointer(); keys(angle, 0, 40);
    app->getCurrentXsheet()->notifyXsheetChanged(); settle();
    checkpoint("opening native Function Editor");
    auto *open = CommandManager::instance()->getAction(MI_OpenFunctionEditor); require(open && open->isEnabled(), "Native Function Editor menu action unavailable"); open->trigger(); settle();
    FunctionViewer *viewer = nullptr;
    for (auto *widget : QApplication::allWidgets()) if (auto *candidate = qobject_cast<FunctionViewer *>(widget)) if (candidate->isVisible()) { viewer = candidate; break; }
    require(viewer && viewer->getXsheetHandle() == app->getCurrentXsheet(), "Native Function Editor did not attach application handles");
    auto *tree = viewer->findChild<FunctionTreeView *>(); auto *graph = viewer->findChild<FunctionPanel *>();
    require(tree && graph, "Native Function Editor tree/graph missing"); graph->show(); graph->resize(800, 400); settle();
    find(tree, angle)->setIsActive(true); find(tree, offset)->setIsActive(true);
    checkpoint("checking native and SeExpr dependency navigation");
    dependencies(viewer, tree, native, source, 1); dependencies(viewer, tree, seexpr, source, 1);
    frozenComparison(viewer, tree, graph, source, native, seexpr, offset);
    viewer->getSelection()->selectNone(); viewer->getSelection()->select(angle, 0); viewer->getSelection()->select(offset, 0);
    auto *edit = viewer->findChild<QToolButton *>("FunctionEditKeys"); require(edit, "Native numeric editor action missing");
    modal(viewer, "FunctionKeyEditor", [&](QDialog *dialog) {
      auto *mode = dialog->findChild<QComboBox *>("FunctionEditOperation"); auto *frame = dialog->findChild<QDoubleSpinBox *>("FunctionEditFirstFrame");
      auto *favour = dialog->findChild<QSlider *>("FunctionEditFavour"); auto *buttons = dialog->findChild<QDialogButtonBox *>();
      require(mode && frame && favour && buttons, "Native breakdown controls missing"); mode->setCurrentIndex(6); frame->setValue(11); favour->setValue(150);
      auto *preview = dialog->findChild<QPushButton *>("FunctionEditPreview"); require(preview, "Native breakdown Preview missing"); preview->click();
      require(angle->getKeyframeCount() == 2 && offset->getKeyframeCount() == 2, "Breakdown preview changed native rig curves");
      buttons->button(QDialogButtonBox::Apply)->click();
      requireNear(angle->getValue(10), 60); requireNear(offset->getValue(10), 3);
    }, [&]() { edit->click(); });
    require(TUndoManager::manager()->undo(), "Native rig breakdown has no undo"); require(angle->getKeyframeCount() == 2 && offset->getKeyframeCount() == 2, "Native rig undo missed a channel");
    require(TUndoManager::manager()->redo(), "Native rig breakdown has no redo"); requireNear(angle->getValue(10), 60); requireNear(offset->getValue(10), 3);
    checkpoint("checking native graph drag and snapping");
    // Native mouse routes use display coordinates and an off-center hit, proving
    // grid/key snapping is anchored to the selected key, not the click pixel.
    for (auto *channel : tree->channels()) channel->setIsActive(channel->getParam() == source);
    find(tree, source)->setIsCurrent(true); viewer->getSelection()->selectNone(); viewer->getSelection()->select(source, 1);
    graph->setNormalized(false); graph->fitRegion(0, Ops::displayValue(source, 0), 24, Ops::displayValue(source, 10)); graph->setSnap(4, false); paint(graph);
    auto point = [&]() { return QPoint(qRound(graph->frameToX(10)) + 3, qRound(graph->valueToY(source, 4))); };
    const int dx = qRound(graph->frameToX(2.2) - graph->frameToX(0));
    drag(graph, point(), point() + QPoint(dx, 0)); requireNear(source->keyframeIndexToFrame(1), 14);
    require(TUndoManager::manager()->undo(), "Native snapped drag has no undo"); requireNear(source->keyframeIndexToFrame(1), 10); paint(graph);
    drag(graph, point(), point() + QPoint(dx, 0), Qt::AltModifier); requireNear(source->keyframeIndexToFrame(1), 12);
    require(TUndoManager::manager()->undo(), "Native snap bypass has no undo"); paint(graph);
    auto *nearby = object->getParam(TStageObject::T_Y); nearby->setKeyframe(TDoubleKeyframe(13, 0)); nearby->setKeyframe(TDoubleKeyframe(30, 1));
    find(tree, nearby)->setIsActive(true); graph->setSnap(4, true); paint(graph);
    int nearDx = qRound(graph->frameToX(2.9) - graph->frameToX(0));
    drag(graph, point(), point() + QPoint(nearDx, 0)); requireNear(source->keyframeIndexToFrame(1), 13);
    require(TUndoManager::manager()->undo(), "Native key snapping has no undo");
    find(tree, nearby)->setIsActive(false); graph->setSnap(4, false);
    scene->getProperties()->setMarkers(6, 1); viewer->refreshModel();
    find(tree, source)->setIsCurrent(true); viewer->getSelection()->selectNone(); viewer->getSelection()->select(source, 1);
    graph->setSnapMarkers(true); paint(graph);
    drag(graph, point(), point() + QPoint(nearDx, 0)); requireNear(source->keyframeIndexToFrame(1), 13);
    require(TUndoManager::manager()->undo(), "Native scene-marker snapping has no undo"); graph->setSnapMarkers(false); paint(graph);
    TDoubleParamP before = new TDoubleParam(*source); source->setEditorLocked(true);
    drag(graph, point(), point() + QPoint(dx, -20)); require(Ops::matchesSnapshot(source, before.getPointer()), "Locked native drag mutated the curve"); source->setEditorLocked(false);
    checkpoint("checking interpolation and handle routes");
    auto *interpolation = viewer->findChild<QToolButton *>("FunctionInterpolation"); require(interpolation && interpolation->menu(), "Interpolation toolbar unavailable");
    for (auto *action : interpolation->menu()->actions()) if (action->data().isValid() && action->data().toInt() == TDoubleKeyframe::SpeedInOut) { action->trigger(); break; }
    require(source->getKeyframe(1).m_type == TDoubleKeyframe::SpeedInOut, "Native interpolation menu did not edit the selected segment");
    {
      KeyframeSetter handles(source, 1); handles.unlinkHandles(); handles.setSpeedOut(TPointD(2, 2));
    }
    TDoubleParamP beforeHandle = new TDoubleParam(*source); paint(graph);
    auto speed = source->getSpeedOut(1);
    QPoint handle(qRound(graph->frameToX(10 + speed.x)), qRound(graph->valueToY(source, 4 + speed.y)));
    drag(graph, handle, handle + QPoint(0, -20));
    require(!Ops::matchesSnapshot(source, beforeHandle.getPointer()), "Native handle mouse drag did not edit its tangent");
    require(TUndoManager::manager()->undo() && Ops::matchesSnapshot(source, beforeHandle.getPointer()), "Native handle drag undo lost its tangent");
    auto *reset = viewer->findChild<QAction *>("FunctionResetHandles"); require(reset, "Native handle reset action missing");
    reset->trigger(); requireNear(source->getKeyframe(1).m_speedOut.y, 0);
    require(TUndoManager::manager()->undo() && Ops::matchesSnapshot(source, beforeHandle.getPointer()), "Native handle reset undo lost its tangent");
    source->setEditorLocked(true); reset->trigger(); require(Ops::matchesSnapshot(source, beforeHandle.getPointer()), "Locked toolbar reset changed a tangent"); source->setEditorLocked(false);
    require(TUndoManager::manager()->undo(), "Native handle fixture setup has no undo");
    require(TUndoManager::manager()->undo(), "Native interpolation menu has no undo");
    require(Ops::matchesSnapshot(source, before.getPointer()),
            "Native interpolation undo did not restore both segment endpoints exactly");
    modal(viewer, "FunctionKeyEditor", [&](QDialog *dialog) {
      auto *mode = dialog->findChild<QComboBox *>("FunctionEditOperation");
      auto *interval = dialog->findChild<QDoubleSpinBox *>("FunctionEditInterval");
      auto *pivot = dialog->findChild<QDoubleSpinBox *>("FunctionEditPivot");
      auto *buttons = dialog->findChild<QDialogButtonBox *>();
      require(mode && interval && pivot && buttons, "Native quantization controls missing");
      mode->setCurrentIndex(7); interval->setValue(4); pivot->setValue(1);
      auto *preview = dialog->findChild<QPushButton *>("FunctionEditPreview"); require(preview, "Native quantization Preview missing"); preview->click(); requireNear(source->keyframeIndexToFrame(1), 10);
      buttons->button(QDialogButtonBox::Apply)->click(); requireNear(source->keyframeIndexToFrame(1), 12);
    }, [&]() { edit->click(); });
    require(TUndoManager::manager()->undo(), "Native quantization has no undo"); requireNear(source->keyframeIndexToFrame(1), 10);
    checkpoint("checking scene and channel identity roundtrip");
    source->setWorkspaceId("native-source-stable-id"); offset->setWorkspaceId("native-offset-stable-id"); angle->setWorkspaceId("native-plastic-stable-id");
    // Y retains the authored nearby-key snapping fixture; Z is still untouched.
    object->getParam(TStageObject::T_Z)->setWorkspaceId("native-unkeyed-stage");
    object->getOffsetParam(0, TStageObject::T_Y)->setWorkspaceId("native-unkeyed-offset");
    vertex->m_params[SkVD::DISTANCE]->setWorkspaceId("native-unkeyed-plastic");
    for (auto channel : {TStageObject::T_RotX, TStageObject::T_RotY,
                         TStageObject::T_FocusZ, TStageObject::T_Aperture}) {
      auto *curve = object->getParam(channel);
      curve->setWorkspaceId("native-unkeyed-3d-" + std::to_string(int(channel)));
      require(curve->isDefault() && !object->isFullKeyframeChannel(channel),
              "Editor identity activated an unused 3D animation channel");
    }
    auto *columnFx = dynamic_cast<TLevelColumnFx *>(xsheet->getColumn(0)->getFx());
    require(columnFx, "Native identity fixture has no column effect");
    columnFx->getTimeRemapParam()->setWorkspaceId("native-unkeyed-remap");
    columnFx->getColorEffectParam(ColumnColorEffect::Brightness)->setWorkspaceId("native-unkeyed-brightness");
    columnFx->getColorEffectParam(ColumnColorEffect::Alpha)->setWorkspaceId("native-unkeyed-alpha");
    columnFx->getTintColorParam()->getRed()->setWorkspaceId("native-unkeyed-tint-red");
    require(!columnFx->isTimeRemapUsed() && !columnFx->isTimeRemapActive() &&
                !columnFx->isColorEffectUsed(),
            "Editor identity activated untouched column effects");
    TFxP identityFx(TFx::create("STD_blurFx"));
    require(identityFx, "Cannot create native identity effect fixture");
    auto *fxCurve = dynamic_cast<TDoubleParam *>(identityFx->getParams()->getParam("value"));
    require(fxCurve && !fxCurve->hasKeyframes(), "Native effect fixture is not unkeyed");
    const double fxDefault = fxCurve->getDefaultValue();
    fxCurve->setWorkspaceId("native-unkeyed-fx");
    xsheet->getFxDag()->assignUniqueId(identityFx.getPointer());
    identityFx->getInputPort(0)->setFx(columnFx);
    xsheet->getFxDag()->getInternalFxs()->addFx(identityFx.getPointer());
    checkpoint("checking unkeyed defaults before scene save");
    requireUnkeyedIdentity(object->getParam(TStageObject::T_Z), "native-unkeyed-stage");
    requireUnkeyedIdentity(object->getOffsetParam(0, TStageObject::T_Y), "native-unkeyed-offset");
    requireUnkeyedIdentity(vertex->m_params[SkVD::DISTANCE].getPointer(), "native-unkeyed-plastic");
    object->setName("Renamed rig");
    const double nativeValue = native->getValue(7), seexprValue = seexpr->getValue(7);
    const TFilePath scenePath(directory.filePath("function-workflow.tnz")); scene->saveCopy(scenePath);
    ToonzScene reloaded; reloaded.loadTnzFile(scenePath);
    auto *loadedObject = reloaded.getXsheet()->getStageObject(TStageObjectId::ColumnId(0));
    auto *loadedOffset = loadedObject->getOffsetParam(0, TStageObject::T_X);
    auto loadedDeformation = loadedObject->getPlasticSkeletonDeformation();
    require(loadedOffset && loadedDeformation, "Saved native rig structure did not reload");
    auto *loadedVertex = loadedDeformation->vertexDeformation("FunctionTip"); require(loadedVertex, "Reloaded Plastic vertex missing");
    auto *loadedAngle = loadedVertex->m_params[SkVD::ANGLE].getPointer();
    requireNear(loadedAngle->getValue(10), 60); requireNear(loadedOffset->getValue(10), 3);
    require(loadedObject->getParam(TStageObject::T_X)->getWorkspaceId() == "native-source-stable-id" && loadedOffset->getWorkspaceId() == "native-offset-stable-id" && loadedAngle->getWorkspaceId() == "native-plastic-stable-id", "Scene save/reload lost renamed channel identities");
    checkpoint("checking unkeyed defaults after scene reload");
    requireUnkeyedIdentity(loadedObject->getParam(TStageObject::T_Z), "native-unkeyed-stage");
    requireUnkeyedIdentity(loadedObject->getOffsetParam(0, TStageObject::T_Y), "native-unkeyed-offset");
    requireUnkeyedIdentity(loadedVertex->m_params[SkVD::DISTANCE].getPointer(), "native-unkeyed-plastic");
    for (auto channel : {TStageObject::T_RotX, TStageObject::T_RotY,
                         TStageObject::T_FocusZ, TStageObject::T_Aperture}) {
      requireUnkeyedIdentity(loadedObject->getParam(channel),
                            "native-unkeyed-3d-" + std::to_string(int(channel)));
      require(!loadedObject->isFullKeyframeChannel(channel),
              "Reloaded editor identity activated an unused 3D animation channel");
    }
    auto *loadedColumnFx = dynamic_cast<TLevelColumnFx *>(reloaded.getXsheet()->getColumn(0)->getFx());
    require(loadedColumnFx, "Reloaded identity fixture has no column effect");
    requireUnkeyedIdentity(loadedColumnFx->getTimeRemapParam(), "native-unkeyed-remap", 1.0);
    requireUnkeyedIdentity(loadedColumnFx->getColorEffectParam(ColumnColorEffect::Brightness), "native-unkeyed-brightness");
    requireUnkeyedIdentity(loadedColumnFx->getColorEffectParam(ColumnColorEffect::Alpha), "native-unkeyed-alpha", 100.0);
    requireUnkeyedIdentity(loadedColumnFx->getTintColorParam()->getRed().getPointer(), "native-unkeyed-tint-red");
    require(!loadedColumnFx->isTimeRemapUsed() && !loadedColumnFx->isTimeRemapActive() &&
                !loadedColumnFx->isColorEffectUsed(),
            "Reloaded editor identity activated untouched column effects");
    TFx *loadedFx = nullptr;
    auto *loadedFxs = reloaded.getXsheet()->getFxDag()->getInternalFxs();
    for (int i = 0; i < loadedFxs->getFxCount(); ++i) {
      TFx *candidate = loadedFxs->getFx(i);
      if (candidate->getFxId() == identityFx->getFxId()) loadedFx = candidate;
    }
    require(loadedFx, "Scene roundtrip lost the unkeyed native effect");
    requireUnkeyedIdentity(dynamic_cast<TDoubleParam *>(loadedFx->getParams()->getParam("value")),
                          "native-unkeyed-fx", fxDefault);
    auto *loadedNearby = loadedObject->getParam(TStageObject::T_Y);
    require(nearby->getKeyframeCount() == 2 && loadedNearby->getKeyframeCount() == 2 &&
                loadedNearby->isKeyframe(13) && loadedNearby->isKeyframe(30),
            "Scene roundtrip changed authored nearby-key snapping frames");
    requireNear(loadedNearby->getValue(13), 0);
    requireNear(loadedNearby->getValue(30), 1);
    auto *loadedNative = reloaded.getXsheet()->getStageObject(TStageObjectId::ColumnId(1))->getParam(TStageObject::T_X);
    auto *loadedSeexpr = reloaded.getXsheet()->getStageObject(TStageObjectId::ColumnId(2))->getParam(TStageObject::T_X);
    require(loadedNative->getKeyframe(0).m_expressionText == nativeText && loadedSeexpr->getKeyframe(0).m_expressionText == seexprText, "Native workflow changed formula source while saving");
    requireNear(loadedNative->getValue(7), nativeValue); requireNear(loadedSeexpr->getValue(7), seexprValue);
    renameNativeSet(viewer, tree, {source, offset, angle});
    retimeNativeRange(viewer, scene, {source, offset, angle}, directory);
    ownerRemovalDialogs(viewer, tree, graph, xsheet, level, source, directory);
    runFunctionRigBenchmarkIntegrationTest(viewer);
    app->getCurrentScene()->setDirtyFlag(false); TUndoManager::manager()->reset(); viewer->hide();
    std::cout << "function-workflow-native: PASS (Plastic, offset, native/SeExpr bindings, preview/undo, mouse routes, saved-set rename, target-range retiming, scene roundtrip)\n";
    return true;
  } catch (const std::exception &problem) { if (error) *error = QString::fromUtf8(problem.what()); }
  catch (...) { if (error) *error = "Unexpected native Function Editor exception"; }
  app->getCurrentScene()->setDirtyFlag(false); TUndoManager::manager()->reset(); return false;
}
