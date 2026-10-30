#include "functionworkflowintegrationtest.h"
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
#include "tundo.h"
#include "tvectorimage.h"
#include <QAction>
#include <QApplication>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QEventLoop>
#include <QImage>
#include <QListWidget>
#include <QMainWindow>
#include <QMenu>
#include <QMouseEvent>
#include <QPushButton>
#include <QSlider>
#include <QTemporaryDir>
#include <QTimer>
#include <QToolButton>
#include <cmath>
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
    app->getCurrentScene()->setDirtyFlag(false); TUndoManager::manager()->reset(); viewer->hide();
    std::cout << "function-workflow-native: PASS (Plastic, offset, native/SeExpr bindings, preview/undo, mouse routes, scene roundtrip)\n";
    return true;
  } catch (const std::exception &problem) { if (error) *error = QString::fromUtf8(problem.what()); }
  catch (...) { if (error) *error = "Unexpected native Function Editor exception"; }
  app->getCurrentScene()->setDirtyFlag(false); TUndoManager::manager()->reset(); return false;
}
