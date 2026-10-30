#include "functiontimingintegrationtest.h"
#include "nativeuiintegrationtest.h"
#include "storyboardtiming.h"
#include "iocommand.h"
#include "tapp.h"
#include "ext/plasticskeleton.h"
#include "ext/plasticskeletondeformation.h"
#include "toonz/navigationtags.h"
#include "toonz/toonzscene.h"
#include "toonz/tstageobject.h"
#include "toonz/tstageobjecttree.h"
#include "toonz/tscenehandle.h"
#include "toonz/txsheet.h"
#include "toonz/txsheethandle.h"
#include "toonz/txshcell.h"
#include "toonz/txshchildlevel.h"
#include "toonz/txshleveltypes.h"
#include "toonz/txshsimplelevel.h"
#include "toonzqt/functioncurveoperations.h"
#include "tdoubleparam.h"
#include "tdoublekeyframe.h"
#include "tundo.h"
#include "tvectorimage.h"
#include <QApplication>
#include <QEventLoop>
#include <QTemporaryDir>
#include <array>
#include <cmath>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
namespace Ops = FunctionCurveOperations;
constexpr int frameCount = 6;
const std::array<int, frameCount> movedRows{{2, 3, 4, 5, 0, 1}};

void require(bool value, const char *message) {
  if (!value) throw std::runtime_error(message);
}
void near(double actual, double expected) {
  require(std::isfinite(actual) && std::abs(actual - expected) < 1.e-8,
          "Storyboard/function timing differs from the original frame oracle");
}
void checkpoint(const char *message) {
  std::cout << "function-timing-native: " << message << std::endl;
}
void settle() {
  QApplication::sendPostedEvents();
  QApplication::processEvents(QEventLoop::ExcludeUserInputEvents);
}
void keys(TDoubleParam *curve, double initial, double increment,
          const std::string &identity) {
  require(curve, "Native timing fixture channel is missing");
  curve->clearKeyframes();
  for (const int frame : {0, 2, 5}) {
    TDoubleKeyframe key(frame, initial + frame * increment);
    key.m_type = key.m_prevType = TDoubleKeyframe::Linear;
    curve->setKeyframe(key);
  }
  curve->setWorkspaceId(identity);
}
using Cells = std::vector<TXshCell>;
Cells cells(TXsheet *sheet) {
  Cells result;
  for (int column = 0; column < sheet->getColumnCount(); ++column)
    for (int frame = 0; frame < frameCount; ++frame)
      result.push_back(sheet->getCell(frame, column));
  return result;
}
void checkCells(TXsheet *sheet, const Cells &before, bool moved) {
  require(sheet->getFrameCount() == frameCount,
          "Storyboard timing changed the parent/child duration");
  require(before.size() == size_t(sheet->getColumnCount() * frameCount),
          "Storyboard timing changed the number of columns");
  for (int column = 0; column < sheet->getColumnCount(); ++column)
    for (int frame = 0; frame < frameCount; ++frame) {
      const int source = moved ? movedRows[frame] : frame;
      const auto &expected = before[column * frameCount + source];
      const auto &actual = sheet->getCell(frame, column);
      require(actual.m_level == expected.m_level &&
                  actual.getFrameId() == expected.getFrameId(),
              "Storyboard timing changed an exposure's level or source frame");
    }
}
struct Channel {
  TDoubleParam *curve;
  TDoubleParamP before;
  std::string identity;
  explicit Channel(TDoubleParam *value)
      : curve(value), before(new TDoubleParam(*value)),
        identity(value->getWorkspaceId()) {}
  void check(bool moved, bool locked) const {
    require(curve->getWorkspaceId() == identity,
            "Storyboard move/undo/redo changed a channel-set identity");
    require(curve->isEditorLocked() == locked,
            "Storyboard move/undo/redo changed a session channel lock");
    if (!moved)
      require(Ops::matchesSnapshot(curve, before.getPointer()),
              "Rejected/undone storyboard move changed native key fields");
    for (int frame = 0; frame < frameCount; ++frame)
      near(curve->getValue(frame),
           before->getValue(moved ? movedRows[frame] : frame));
  }
};

void checkReloaded(TDoubleParam *loaded, const Channel &source, bool moved) {
  require(loaded && loaded->getWorkspaceId() == source.identity,
          "Scene roundtrip lost a storyboard channel's stable identity");
  require(!loaded->isEditorLocked(),
          "Session-only editor lock was persisted into a scene");
  for (int frame = 0; frame < frameCount; ++frame)
    near(loaded->getValue(frame),
         source.before->getValue(moved ? movedRows[frame] : frame));
}
}

bool runFunctionTimingIntegrationTest(QMainWindow *, QString *error) {
  auto *app = TApp::instance();
  auto *undo = TUndoManager::manager();
  if (error) error->clear();
  try {
    checkpoint("setup begins");
    QString resetError;
    if (!resetNativeUiIntegrationScene(&resetError))
      throw std::runtime_error(resetError.toStdString());
    settle();
    auto *scene = app->getCurrentScene()->getScene();
    auto *sheet = app->getCurrentXsheet()->getXsheet();
    require(scene && sheet && sheet->getFrameCount() == 0,
            "Timing fixture requires an empty disposable application scene");
    QTemporaryDir directory;
    require(directory.isValid(), "Timing fixture storage is unavailable");
    auto *level = scene->createNewLevel(PLI_XSHLEVEL, L"TimingDrawings")->getSimpleLevel();
    auto *child = scene->createNewLevel(CHILD_XSHLEVEL, L"TimingChild")->getChildLevel();
    require(level && child, "Cannot create native timing fixture levels");
    auto *nested = child->getXsheet();
    for (int frame = 0; frame < frameCount; ++frame) {
      TVectorImageP image = new TVectorImage;
      image->setPalette(level->getPalette());
      level->setFrame(TFrameId(frame + 1), image);
      require(sheet->setCell(frame, 0, TXshCell(level, TFrameId(frame + 1))) &&
                  sheet->setCell(frame, 1, TXshCell(child, TFrameId(frame + 1))) &&
                  nested->setCell(frame, 0, TXshCell(level, TFrameId(frameCount - frame))),
              "Cannot expose native parent/child timing fixture frames");
    }
    const TFilePath levelPath(directory.filePath("timing.pli"));
    level->setPath(levelPath);
    level->save(levelPath);
    auto *camera = sheet->getStageObject(TStageObjectId::CameraId(0))
                       ->getParam(TStageObject::T_X);
    auto *object = sheet->getStageObject(TStageObjectId::ColumnId(0));
    object->insertOffsetLayer(0, object->createOffsetLayer(L"Storyboard offset"));
    auto *offset = object->getOffsetParam(0, TStageObject::T_Y);
    PlasticSkeletonP skeleton = new PlasticSkeleton;
    const int root = skeleton->addVertex(PlasticSkeletonVertex(TPointD(0, 0)), -1);
    const int tip = skeleton->addVertex(PlasticSkeletonVertex(TPointD(0, 100)), root);
    require(skeleton->setVertexName(tip, "TimingTip"), "Cannot name Plastic timing fixture vertex");
    PlasticSkeletonDeformationP deformation = new PlasticSkeletonDeformation;
    deformation->attach(1, skeleton.getPointer());
    object->setPlasticSkeletonDeformation(deformation);
    auto *vertex = deformation->vertexDeformation(1, tip);
    require(vertex, "Native Plastic timing fixture vertex is missing");
    auto *angle = vertex->m_params[SkVD::ANGLE].getPointer();
    auto *local = nested->getStageObject(TStageObjectId::ColumnId(0))
                      ->getParam(TStageObject::T_X);
    keys(camera, 10, 3, "timing-camera-channel");
    keys(offset, 20, 5, "timing-offset-channel");
    keys(angle, 30, 7, "timing-plastic-channel");
    keys(local, 100, 11, "timing-child-channel");
    std::array<Channel, 3> channels{{Channel(camera), Channel(offset), Channel(angle)}};
    const Channel localChannel(local);
    // A parent move reorders the child exposure. Its local animation and locks
    // are outside this timing domain and must neither change nor block the move.
    local->setEditorLocked(true);
    sheet->getNavigationTags()->addTag(0, "Parent start");
    nested->getNavigationTags()->addTag(1, "Child local");
    const Cells parentCells = cells(sheet), localCells = cells(nested);
    app->getCurrentXsheet()->notifyXsheetChanged();
    settle();
    undo->reset();
    const int history = undo->getHistoryCount();
    const int historyIndex = undo->getCurrentHistoryIndex();

    checkpoint("locked camera/offset/Plastic rejection begins");
    for (auto &locked : channels) {
      locked.curve->setEditorLocked(true);
      for (const bool dirty : {false, true}) {
        app->getCurrentScene()->setDirtyFlag(dirty);
        for (const bool bake : {false, true}) {
          QString reason;
          require(!moveStoryboardTiming(sheet, 0, 1, frameCount, &reason, bake) &&
                      !reason.isEmpty(),
                  "Storyboard timing accepted a locked Function Editor channel");
          require(undo->getHistoryCount() == history &&
                      undo->getCurrentHistoryIndex() == historyIndex &&
                      app->getCurrentScene()->getDirtyFlag() == dirty,
                  "Rejected locked timing changed undo history or dirty state");
          for (const auto &channel : channels)
            channel.check(false, channel.curve == locked.curve);
          checkCells(sheet, parentCells, false);
          checkCells(nested, localCells, false);
          localChannel.check(false, true);
          require(sheet->getNavigationTags()->getTagLabel(0) == "Parent start" &&
                      nested->getNavigationTags()->getTagLabel(1) == "Child local",
                  "Rejected timing changed parent or child navigation tags");
        }
      }
      locked.curve->setEditorLocked(false);
    }

    checkpoint("unlocked move and locked-history restoration begin");
    app->getCurrentScene()->setDirtyFlag(false);
    QString reason;
    require(moveStoryboardTiming(sheet, 0, 1, frameCount, &reason),
            "Unlocked native storyboard/function timing move failed");
    require(app->getCurrentScene()->getDirtyFlag() &&
                undo->getHistoryCount() == history + 1 &&
                undo->getCurrentHistoryIndex() == historyIndex + 1,
            "Storyboard move did not publish one dirty undo transaction");
    for (const auto &channel : channels) channel.check(true, false);
    checkCells(sheet, parentCells, true);
    checkCells(nested, localCells, false);
    localChannel.check(false, true);
    require(sheet->getNavigationTags()->getTagLabel(4) == "Parent start",
            "Parent storyboard tag did not follow the moved panel");
    // A lock protects new edits, not restoration of existing history. Copying
    // curve snapshots must retain the owner's current session lock and UUID.
    for (auto &channel : channels) channel.curve->setEditorLocked(true);
    require(undo->undo(), "Storyboard/function timing undo is missing");
    for (const auto &channel : channels) channel.check(false, true);
    checkCells(sheet, parentCells, false);
    checkCells(nested, localCells, false);
    localChannel.check(false, true);
    require(undo->redo(), "Storyboard/function timing redo is missing");
    for (const auto &channel : channels) channel.check(true, true);
    checkCells(sheet, parentCells, true);
    checkCells(nested, localCells, false);
    localChannel.check(false, true);
    require(undo->getHistoryCount() == history + 1 &&
                undo->getCurrentHistoryIndex() == historyIndex + 1,
            "Timing undo/redo changed the transaction count");

    checkpoint("scene roundtrip begins");
    object->setName("Renamed timing rig");
    const TFilePath scenePath(directory.filePath("function-timing.tnz"));
    scene->saveCopy(scenePath);
    ToonzScene reloaded;
    reloaded.loadTnzFile(scenePath);
    auto *loadedSheet = reloaded.getXsheet();
    auto *loadedObject = loadedSheet->getStageObject(TStageObjectId::ColumnId(0));
    auto loadedDeformation = loadedObject->getPlasticSkeletonDeformation();
    require(loadedDeformation, "Scene roundtrip lost the timed Plastic skeleton");
    auto *loadedVertex = loadedDeformation->vertexDeformation("TimingTip");
    require(loadedVertex, "Scene roundtrip lost the timed Plastic vertex");
    checkReloaded(loadedSheet->getStageObject(TStageObjectId::CameraId(0))
                      ->getParam(TStageObject::T_X), channels[0], true);
    checkReloaded(loadedObject->getOffsetParam(0, TStageObject::T_Y), channels[1], true);
    checkReloaded(loadedVertex->m_params[SkVD::ANGLE].getPointer(), channels[2], true);
    const auto &loadedChildCell = loadedSheet->getCell(0, 1);
    require(loadedChildCell.m_level, "Scene roundtrip lost the child exposure");
    auto *loadedChild = loadedChildCell.m_level->getChildLevel();
    require(loadedChild, "Scene roundtrip lost the nested timing domain");
    auto *loadedNested = loadedChild->getXsheet();
    checkReloaded(loadedNested->getStageObject(TStageObjectId::ColumnId(0))
                      ->getParam(TStageObject::T_X), localChannel, false);
    for (int frame = 0; frame < frameCount; ++frame) {
      require(loadedSheet->getCell(frame, 0).getFrameId() == TFrameId(movedRows[frame] + 1) &&
                  loadedSheet->getCell(frame, 1).getFrameId() == TFrameId(movedRows[frame] + 1) &&
                  loadedNested->getCell(frame, 0).getFrameId() == TFrameId(frameCount - frame),
              "Scene roundtrip changed parent exposures or nested local timing");
    }
    require(loadedSheet->getFrameCount() == frameCount &&
                loadedNested->getFrameCount() == frameCount &&
                loadedSheet->getNavigationTags()->getTagLabel(4) == "Parent start" &&
                loadedNested->getNavigationTags()->getTagLabel(1) == "Child local",
            "Scene roundtrip changed parent/child duration or navigation timing");
    for (const auto &channel : channels) channel.check(true, true);
    localChannel.check(false, true);
    app->getCurrentScene()->setDirtyFlag(false);
    undo->reset();
    checkpoint("PASS (camera/offset/Plastic locks, atomic rejection, undo/redo, stable IDs, session locks, nested timing, scene roundtrip)");
    return true;
  } catch (const std::exception &problem) {
    if (error) *error = QString::fromUtf8(problem.what());
  } catch (...) {
    if (error) *error = "Unexpected storyboard/function timing exception";
  }
  app->getCurrentScene()->setDirtyFlag(false);
  undo->reset();
  return false;
}
