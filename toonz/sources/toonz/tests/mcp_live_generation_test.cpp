#include "tests/mcp_live_generation_test.h"

#include <QTemporaryDir>
#include <limits>
#include <stdexcept>

#include "tmutationgeneration.h"
#include "toonz/fxdag.h"
#include "toonz/levelset.h"
#include "toonz/tcolumnfx.h"
#include "toonz/tcolumnfxset.h"
#include "toonz/toonzscene.h"
#include "toonz/tscenehandle.h"
#include "toonz/tstageobjectspline.h"
#include "toonz/tstageobjecttree.h"
#include "toonz/txsheet.h"
#include "toonz/txshsimplelevel.h"
#include "tstream.h"
#include "tundo.h"

namespace {

void require(bool condition, const char* message) {
  if (!condition) throw std::runtime_error(message);
}

void sceneBindings() {
  TSceneHandle handle;
  auto* first = new ToonzScene;
  auto* second = new ToonzScene;
  int switchCount = 0;
  bool observed = true;
  std::uint64_t previous = handle.bindingGeneration();
  QObject::connect(&handle, &TSceneHandle::sceneSwitched, &handle, [&] {
    observed = observed && handle.bindingGeneration() > previous;
    ++switchCount;
  });
  handle.setScene(first);
  require(observed && switchCount == 1, "Scene binding was published after its signal");
  previous = handle.bindingGeneration();
  handle.setScene(first);
  require(handle.bindingGeneration() == previous && switchCount == 1,
          "Same-scene binding changed the generation");
  handle.setScene(nullptr);
  require(handle.bindingGeneration() > previous && switchCount == 1,
          "Silent null scene binding did not advance its generation");
  previous = handle.bindingGeneration();
  handle.setScene(first);
  require(observed && handle.bindingGeneration() > previous,
          "Reinstating the same scene revived an old binding generation");
  previous = handle.bindingGeneration();
  handle.setScene(second);
  require(observed && handle.bindingGeneration() > previous,
          "Scene replacement did not advance its generation");
  handle.setScene(nullptr);
  // Destroying this local handle retires its timers and detached scene records.
}

void stageMembership() {
  TStageObjectTree tree;
  const auto id = TStageObjectId::PegbarId(1);
  auto before = tree.membershipGeneration();
  TStageObjectP object(tree.getStageObject(id, true));
  require(tree.membershipGeneration() > before, "Created stage object was untracked");
  before = tree.membershipGeneration();
  require(
      tree.getStageObject(id, true) == object.getPointer() && tree.membershipGeneration() == before,
      "Existing stage-object lookup changed membership");
  tree.removeStageObject(id);
  require(tree.membershipGeneration() > before, "Removed stage object was untracked");
  before = tree.membershipGeneration();
  tree.insertStageObject(object.getPointer());
  require(tree.membershipGeneration() > before && tree.getStageObject(id) == object.getPointer(),
          "Reinserted stage object revived its prior membership");
  tree.insertColumn(0);
  tree.insertColumn(1);
  TStageObject* column = tree.getStageObject(TStageObjectId::ColumnId(0));
  before = tree.membershipGeneration();
  tree.swapColumns(0, 0);
  require(tree.membershipGeneration() == before, "No-op stage swap changed membership");
  tree.swapColumns(0, 1);
  require(tree.membershipGeneration() > before &&
              tree.getStageObject(TStageObjectId::ColumnId(1)) == column,
          "Column rebinding did not invalidate stage membership");
  before = tree.membershipGeneration();
  TSmartPointerT<TStageObjectSpline> spline(tree.createSpline());
  require(tree.membershipGeneration() > before, "Spline creation was untracked");
  before = tree.membershipGeneration();
  tree.removeSpline(spline.getPointer());
  require(tree.membershipGeneration() > before, "Spline removal was untracked");
  before = tree.membershipGeneration();
  tree.insertSpline(spline.getPointer());
  require(tree.membershipGeneration() > before, "Spline reinsertion was untracked");
  const auto camera = TStageObjectId::CameraId(1);
  tree.getStageObject(camera, true);
  before = tree.membershipGeneration();
  tree.setCurrentCameraId(camera);
  require(tree.membershipGeneration() > before, "Active camera rebinding was untracked");
  before = tree.membershipGeneration();
  tree.setCurrentPreviewCameraId(camera);
  require(tree.membershipGeneration() > before, "Preview camera rebinding was untracked");
  before = tree.membershipGeneration();
  tree.setCurrentCameraId(camera);
  tree.setCurrentPreviewCameraId(camera);
  require(tree.membershipGeneration() == before, "Same camera selection changed membership");
}

void stageColumnReindexing() {
  TStageObjectTree tree;
  TStageObjectP retained(tree.getStageObject(TStageObjectId::ColumnId(2), true));
  const auto before = tree.membershipGeneration();
  tree.removeColumn(0);
  require(tree.membershipGeneration() > before, "Native stage-column removal was untracked");
  require(tree.getStageObject(TStageObjectId::ColumnId(1), false) == retained.getPointer(),
          "Native stage-column removal did not rebind a later object's ID");
}

void effectMembership() {
  FxDag factory;
  TFxSet effects;
  TFxP effect(factory.addOutputFx());
  auto before = effects.membershipGeneration();
  effects.clear();
  require(effects.membershipGeneration() == before, "Empty FX clear changed membership");
  effects.addFx(effect.getPointer());
  require(effects.membershipGeneration() > before, "FX insertion was untracked");
  before = effects.membershipGeneration();
  effects.addFx(effect.getPointer());
  require(effects.membershipGeneration() == before, "Duplicate FX insertion changed membership");
  require(effects.removeFx(effect.getPointer()) && effects.membershipGeneration() > before,
          "FX removal was untracked");
  before = effects.membershipGeneration();
  effects.addFx(effect.getPointer());
  require(effects.membershipGeneration() > before, "FX reinsertion revived prior membership");
  before = effects.membershipGeneration();
  effects.clear();
  require(effects.membershipGeneration() > before, "Owned FX clear was untracked");

  FxDag dag;
  before = dag.outputGeneration();
  auto* native = dag.addOutputFx();
  TFxP output(native);
  require(dag.outputGeneration() > before, "Output insertion was untracked");
  before = dag.outputGeneration();
  dag.setCurrentOutputFx(native);
  require(dag.outputGeneration() > before, "Current output rebinding was untracked");
  before = dag.outputGeneration();
  dag.setCurrentOutputFx(native);
  require(dag.outputGeneration() == before, "Same current output changed its generation");
  dag.removeOutputFx(native);
  require(dag.outputGeneration() > before, "Output removal was untracked");
  before = dag.outputGeneration();
  dag.addOutputFx(native);
  require(dag.outputGeneration() > before, "Output reinsertion revived prior membership");
}

void levelMembership() {
  TLevelSet levels;
  TXshSimpleLevelP level(new TXshSimpleLevel(L"Generation fixture"));
  auto before = levels.membershipGeneration();
  levels.clear();
  require(levels.membershipGeneration() == before, "Empty level clear changed membership");
  require(levels.insertLevel(level.getPointer()) && levels.membershipGeneration() > before,
          "Level insertion was untracked");
  before = levels.membershipGeneration();
  levels.insertLevel(level.getPointer());
  require(levels.membershipGeneration() == before, "Duplicate level insertion changed membership");
  levels.removeLevel(level.getPointer());
  require(levels.membershipGeneration() > before, "Level removal was untracked");
  before = levels.membershipGeneration();
  levels.insertLevel(level.getPointer());
  require(levels.membershipGeneration() > before, "Level reinsertion revived prior membership");
  before = levels.membershipGeneration();
  levels.renameLevel(level.getPointer(), L"Renamed generation fixture");
  require(levels.membershipGeneration() > before, "Level name rebinding was untracked");
  before = levels.membershipGeneration();
  levels.clear();
  require(levels.membershipGeneration() > before, "Owned level clear was untracked");
}

void columnMembership() {
  ToonzScene scene;
  TXsheet* sheet = scene.getXsheet();
  auto before = sheet->columnGeneration();
  sheet->touchColumn(2, TXshColumn::eLevelType);
  require(sheet->getColumnCount() == 3 && sheet->columnGeneration() > before,
          "Automatically created columns were untracked");
  before = sheet->columnGeneration();
  TXshColumnP column(sheet->touchColumn(2, TXshColumn::eLevelType));
  require(sheet->columnGeneration() == before, "Existing column touch changed ownership");
  sheet->moveColumn(2, 0);
  require(sheet->columnGeneration() > before && sheet->getColumn(0) == column.getPointer(),
          "Column move did not invalidate index bindings");
  before = sheet->columnGeneration();
  sheet->removeColumn(0);
  require(sheet->columnGeneration() > before, "Column removal was untracked");
  before = sheet->columnGeneration();
  sheet->insertColumn(0, column.getPointer());
  require(sheet->columnGeneration() > before, "Column reinsertion revived prior ownership");
  before = sheet->columnGeneration();
  sheet->touchColumn(0, TXshColumn::eSoundType);
  require(sheet->columnGeneration() > before && sheet->getColumn(0) != column.getPointer(),
          "Empty-column type replacement was untracked");
  TStageObjectP oldStage(sheet->getStageObject(TStageObjectId::CameraId(0)));
  TFxP oldOutput(sheet->getFxDag()->getCurrentOutputFx());
  before = sheet->columnGeneration();
  sheet->clearAll();
  require(sheet->columnGeneration() > before, "Column/tree/DAG clear was untracked");
  require(sheet->getStageObject(TStageObjectId::CameraId(0)) != oldStage.getPointer(),
          "Stage replacement fixture did not retain a retired native object");
  require(sheet->getFxDag()->getCurrentOutputFx() != oldOutput.getPointer(),
          "DAG replacement fixture did not retain a retired native output");
  before = sheet->columnGeneration();
  sheet->clearAll();
  require(sheet->columnGeneration() > before,
          "Replacing empty stage/FX containers revived their generations");
  before = sheet->columnGeneration();
  sheet->setScene(nullptr);
  require(sheet->columnGeneration() > before, "Xsheet ownership rebinding was untracked");
  sheet->setScene(&scene);
}

void persistedMembership() {
  QTemporaryDir directory;
  require(directory.isValid(), "Cannot create generation persistence fixture");
  const TFilePath path((directory.path() + "/effects.xml").toStdWString());
  FxDag factory;
  TFxSet source;
  source.addFx(factory.addOutputFx());
  {
    TOStream output(path);
    require(bool(output), "Cannot write native membership fixture");
    source.saveData(output, 1);
  }
  TFxSet loaded;
  auto before = loaded.membershipGeneration();
  {
    TIStream input(path);
    require(bool(input), "Cannot read native membership fixture");
    loaded.loadData(input);
  }
  require(loaded.getFxCount() == 1 && loaded.membershipGeneration() > before,
          "Loading into an empty FX owner was untracked");
  TFxP pinned(loaded.getFx(0));
  before = loaded.membershipGeneration();
  {
    TIStream input(path);
    loaded.loadData(input);
  }
  require(loaded.getFxCount() == 1 && loaded.getFx(0) != pinned.getPointer() &&
              loaded.membershipGeneration() > before,
          "Native reload did not invalidate replaced effect ownership");
}

void persistedOutputBindings() {
  QTemporaryDir directory;
  require(directory.isValid(), "Cannot create output-binding persistence fixture");
  const TFilePath path((directory.path() + "/dag.xml").toStdWString());
  FxDag source;
  {
    TOStream output(path);
    require(bool(output), "Cannot write native DAG fixture");
    source.saveData(output, 0);
  }
  FxDag loaded;
  TFxP oldXsheet(loaded.getXsheetFx());
  TFxP oldOutput(loaded.getCurrentOutputFx());
  const auto before = loaded.outputGeneration();
  {
    TIStream input(path);
    require(bool(input), "Cannot read native DAG fixture");
    input.setVersion(VersionNumber(1, 16));
    loaded.loadData(input);
  }
  require(loaded.outputGeneration() > before, "Native DAG load did not invalidate output bindings");
  require(loaded.getXsheetFx() != oldXsheet.getPointer(),
          "Native DAG load did not replace the pinned xsheet effect");
  require(loaded.getCurrentOutputFx() != oldOutput.getPointer(),
          "Native DAG load did not replace the pinned output effect");
}

struct UndoObservations {
  bool busy = true;
  int added = 0;
  int undone = 0;
  int redone = 0;
  int destroyed = 0;
};

class ObservedUndo final : public TUndo {
  TUndoManager& m_manager;
  UndoObservations& m_observations;
  bool m_throwOnAdd;

 public:
  ObservedUndo(TUndoManager& manager, UndoObservations& observations, bool throwOnAdd = false)
      : m_manager(manager), m_observations(observations), m_throwOnAdd(throwOnAdd) {}
  ~ObservedUndo() override {
    m_observations.busy = m_observations.busy && m_manager.isBusy();
    ++m_observations.destroyed;
  }
  int getSize() const override {
    m_observations.busy = m_observations.busy && m_manager.isBusy();
    return 1;
  }
  void onAdd() override {
    m_observations.busy = m_observations.busy && m_manager.isBusy();
    ++m_observations.added;
    if (m_throwOnAdd) throw std::runtime_error("Native onAdd fixture failure");
  }
  void undo() const override {
    m_observations.busy = m_observations.busy && m_manager.isBusy();
    ++m_observations.undone;
  }
  void redo() const override {
    m_observations.busy = m_observations.busy && m_manager.isBusy();
    ++m_observations.redone;
  }
};

void undoIntervals() {
  UndoObservations observations;
  TUndoManager manager;
  manager.setUndoMemorySize(1);
  QObject::connect(&manager, &TUndoManager::historyChanged, &manager,
                   [&] { observations.busy = observations.busy && manager.isBusy(); });
  QObject::connect(&manager, &TUndoManager::somethingChanged, &manager,
                   [&] { observations.busy = observations.busy && manager.isBusy(); });
  auto before = manager.changeGeneration();
  require(!manager.undo(), "Empty history reported an undo");
  require(!manager.redo(), "Empty history reported a redo");
  require(!manager.isBusy(), "Empty history retained busy state");
  require(manager.changeGeneration() == before, "Empty history changed its generation");
  manager.beginBlock();
  manager.beginBlock();
  require(manager.isBusy(), "Nested undo block did not retain busy state");
  manager.add(new ObservedUndo(manager, observations));
  require(manager.changeGeneration() > before, "Adding to an undo block was untracked");
  require(manager.isBusy(), "Adding to an undo block released busy state");
  manager.endBlock();
  require(manager.isBusy(), "Inner undo block ended the outer busy interval");
  manager.endBlock();
  require(!manager.isBusy(), "Closing an undo block retained busy state");
  require(manager.getHistoryCount() == 1, "Closing an undo block lost its history");
  before = manager.changeGeneration();
  require(manager.undo(), "Native undo did not run");
  require(manager.changeGeneration() > before, "Undo cursor movement was untracked");
  require(!manager.isBusy(), "Undo retained busy state after its callback");
  before = manager.changeGeneration();
  require(manager.redo(), "Native redo did not run");
  require(manager.changeGeneration() > before, "Redo cursor movement was untracked");
  require(!manager.isBusy(), "Redo retained busy state after its callback");
  before = manager.changeGeneration();
  manager.popUndo(1);
  require(manager.changeGeneration() > before, "Explicit history removal was untracked");
  require(!manager.isBusy(), "Explicit history removal retained busy state");
  manager.add(new ObservedUndo(manager, observations));
  const auto reset = manager.resetGeneration();
  manager.reset();
  require(manager.resetGeneration() > reset, "Explicit reset did not invalidate its epoch");
  require(!manager.isBusy(), "History reset retained busy state");
  require(observations.busy, "A history callback escaped its busy interval");
  require(observations.added == 2, "Native history lost an onAdd callback");
  require(observations.undone == 1, "Native history lost its undo callback");
  require(observations.redone == 1, "Native history lost its redo callback");
  require(observations.destroyed == 2, "Native history did not destroy both removed entries");
  const auto emptyReset = manager.resetGeneration();
  manager.reset();
  require(manager.resetGeneration() > emptyReset, "Empty explicit reset revived an old epoch");
}

void saturation() {
  auto value = (std::numeric_limits<std::uint64_t>::max)() - 1;
  TMutation::advanceGeneration(value);
  require(value == (std::numeric_limits<std::uint64_t>::max)(),
          "Generation did not reach its exhaustion sentinel");
  TMutation::advanceGeneration(value);
  require(value == (std::numeric_limits<std::uint64_t>::max)(),
          "Exhausted generation wrapped into a reusable identity");
}

void historyDiscardIntervals() {
  UndoObservations observations;
  TUndoManager manager;
  manager.setUndoMemorySize(1);
  manager.add(new ObservedUndo(manager, observations));
  manager.add(new ObservedUndo(manager, observations));
  manager.undo();
  auto before = manager.changeGeneration();
  manager.beginBlock();
  require(manager.changeGeneration() > before, "Redo-branch discard was untracked");
  require(observations.destroyed == 1, "Redo-branch discard did not destroy its entry");
  require(manager.isBusy(), "Redo-branch discard released beginBlock's busy state");
  manager.endBlock();
  manager.add(new ObservedUndo(manager, observations));
  manager.setUndoMemorySize(0);
  manager.add(new ObservedUndo(manager, observations));
  require(observations.destroyed == 3, "History memory limit did not trim the old entries");
  require(observations.busy, "History memory trimming escaped add's busy interval");
  require(!manager.isBusy(), "History memory trimming retained busy state");
  manager.undo();
  before = manager.changeGeneration();
  manager.popUndo(1, true);
  require(manager.changeGeneration() > before, "Forward history removal was untracked");
  require(manager.getHistoryCount() == 0, "Forward history removal retained its entry");
  require(!manager.isBusy(), "Forward history removal retained busy state");
  manager.beginBlock();
  manager.add(new ObservedUndo(manager, observations));
  before = manager.changeGeneration();
  manager.popUndo(1);
  require(manager.changeGeneration() > before && manager.isBusy(),
          "Open-block history removal was untracked");
  manager.endBlock();
  require(observations.busy && !manager.isBusy(), "History deletion released busy state too early");
}

void throwingHistoryCallback() {
  UndoObservations observations;
  TUndoManager manager;
  manager.setUndoMemorySize(1);
  const auto before = manager.changeGeneration();
  bool threw = false;
  try {
    manager.add(new ObservedUndo(manager, observations, true));
  } catch (const std::runtime_error&) {
    threw = true;
  }
  require(threw && observations.busy && !manager.isBusy() && manager.changeGeneration() > before,
          "Throwing onAdd callback left a stale busy state or history generation");
  manager.reset();
}

}  // namespace

void runNativeGenerationTests() {
  sceneBindings();
  stageMembership();
  stageColumnReindexing();
  effectMembership();
  levelMembership();
  columnMembership();
  persistedMembership();
  persistedOutputBindings();
  undoIntervals();
  historyDiscardIntervals();
  throwingHistoryCallback();
  saturation();
}
