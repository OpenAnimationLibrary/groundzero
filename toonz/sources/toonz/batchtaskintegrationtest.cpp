#include "batchtaskintegrationtest.h"

#include "batches.h"
#include "batchtaskexecution.h"
#include "batchtaskviewintegrationtest.h"
#include "texception.h"
#include "tfarmtask.h"
#include "tstream.h"

#include <QApplication>
#include <QByteArray>
#include <QDialog>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QHostInfo>
#include <QMutex>
#include <QMutexLocker>
#include <QProcess>
#include <QThread>
#include <QTimer>
#include <QWaitCondition>

#include <atomic>
#include <exception>
#include <functional>
#include <iostream>
#include <memory>
#include <mutex>
#include <set>
#include <stdexcept>
#include <thread>
#include <vector>

namespace {

using BatchTaskExecution::Command;
using BatchTaskExecution::ProcessResult;
using BatchTaskExecution::TestHooks;

void require(bool condition, const char* message) {
  if (!condition) throw std::runtime_error(message);
}

template <typename Predicate>
bool pumpUntil(Predicate ready) {
  QElapsedTimer deadline;
  deadline.start();
  while (!ready() && deadline.elapsed() < 10000) {
    QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
    QThread::yieldCurrentThread();
  }
  return ready();
}

void checkpoint(const char* message) {
  std::cerr << "native-ui-integration: checkpoint: " << message << std::endl;
}

class Gate final {
  QMutex m_mutex;
  QWaitCondition m_release;
  bool m_open = false;

public:
  std::atomic<bool> entered{false};

  void block() {
    QMutexLocker lock(&m_mutex);
    entered = true;
    while (!m_open) m_release.wait(&m_mutex);
  }

  void open() {
    QMutexLocker lock(&m_mutex);
    m_open = true;
    m_release.wakeAll();
  }

  void await() {
    require(pumpUntil([&] { return entered.load(); }),
            "batch worker did not reach its deterministic gate");
  }
};

struct Observations final {
  mutable std::mutex mutex;
  std::vector<Command> commands;
  std::vector<quint64> retired;
  std::vector<quint64> destroyed;
  std::vector<std::function<void()>> terminalReplays;
  std::atomic<int> tasksDestroyed{0};

  void launched(const Command& command) {
    std::lock_guard<std::mutex> lock(mutex);
    commands.push_back(command);
  }

  QStringList launchedIds() const {
    std::lock_guard<std::mutex> lock(mutex);
    QStringList ids;
    for (const auto& command : commands) ids.push_back(command.taskId);
    return ids;
  }

  bool released(int expected) const {
    std::lock_guard<std::mutex> lock(mutex);
    return retired.size() == size_t(expected) &&
           destroyed.size() == size_t(expected);
  }

  void requireReleased(int expected) const {
    std::lock_guard<std::mutex> lock(mutex);
    const std::set<quint64> uniqueRetired(retired.begin(), retired.end());
    const std::set<quint64> uniqueDestroyed(destroyed.begin(), destroyed.end());
    require(retired.size() == size_t(expected) &&
                destroyed.size() == size_t(expected) &&
                uniqueRetired.size() == size_t(expected) &&
                uniqueRetired == uniqueDestroyed,
            "batch submission was leaked, retired twice, or destroyed twice");
  }
};

class CountedTask final : public TFarmTask {
  std::shared_ptr<Observations> m_observations;

public:
  CountedTask(const QString& id, const QString& root, bool composer,
              const std::shared_ptr<Observations>& observations)
      : TFarmTask(id, id, composer, QString(), QString(), 1, 50,
                  TFilePath(root + "/input with spaces." +
                            (composer ? "tnz" : "cln")),
                  TFilePath(root + "/output with spaces..png"), 1, 4, 1, 1, 0,
                  4, 0, 0, Overwrite_Off, false)
      , m_observations(observations) {}

  ~CountedTask() override { ++m_observations->tasksDestroyed; }
};

class DismissWarnings final {
  QTimer m_timer;

public:
  DismissWarnings() {
    QObject::connect(&m_timer, &QTimer::timeout, [] {
      if (auto* dialog =
              qobject_cast<QDialog*>(QApplication::activeModalWidget()))
        dialog->reject();
    });
    m_timer.start(0);
  }
};

class Fixture final {
  const bool m_dirty;
  TFarmController* const m_farmController;
  const int m_maxActive;
  std::vector<std::shared_ptr<Gate>> m_gates;

public:
  BatchesController* const controller = BatchesController::instance();
  const QString root;
  const std::shared_ptr<Observations> observed =
      std::make_shared<Observations>();
  const std::shared_ptr<TestHooks> hooks = std::make_shared<TestHooks>();
  DismissWarnings warnings;

  explicit Fixture(const QString& testRoot)
      : m_dirty(BatchesController::instance()->getDirtyFlag())
      , m_farmController(BatchesController::instance()->getController())
      , m_maxActive(BatchesController::instance()
                        ->executionTestExecutor()
                        .maxActiveTasks())
      , root(testRoot) {
    require(controller->getTaskCount() == 0 &&
                controller->pendingExecutionCount() == 0,
            "batch lifetime fixture requires an empty task list");
    controller->setController(nullptr);
    controller->executionTestExecutor().setMaxActiveTasks(1);
    auto state     = observed;
    hooks->execute = [state](const Command& command,
                             const std::function<bool()>&) {
      state->launched(command);
      return ProcessResult();
    };
    hooks->retired = [state](quint64 id) {
      std::lock_guard<std::mutex> lock(state->mutex);
      state->retired.push_back(id);
    };
    hooks->destroyed = [state](quint64 id) {
      std::lock_guard<std::mutex> lock(state->mutex);
      state->destroyed.push_back(id);
    };
    hooks->terminalPosted = [state](std::function<void()> replay) {
      std::lock_guard<std::mutex> lock(state->mutex);
      state->terminalReplays.push_back(std::move(replay));
    };
    controller->setExecutionTestHooks(hooks);
  }

  ~Fixture() {
    for (const auto& gate : m_gates) gate->open();
    controller->stopAll();
    if (!pumpUntil([&] { return controller->pendingExecutionCount() == 0; })) {
      std::cerr << "native-ui-integration: FAIL: batch fixture cleanup did not "
                   "retire workers"
                << std::endl;
      std::terminate();
    }
    controller->setExecutionTestHooks(nullptr);
    controller->removeAllTasks();
    controller->executionTestExecutor().setMaxActiveTasks(m_maxActive);
    controller->setController(m_farmController);
    controller->setDirtyFlag(m_dirty);
  }

  std::shared_ptr<Gate> gate() {
    auto result = std::make_shared<Gate>();
    m_gates.push_back(result);
    return result;
  }

  TFarmTask* task(const QString& id, bool composer = false) {
    auto* result = new CountedTask(id, root, composer, observed);
    controller->addTask(id, result, false);
    return result;
  }

  TFarmTaskGroup* group(const QString& id, int children) {
    auto* result = new TFarmTaskGroup(
        id, id, QString(), QString(), 1, 50,
        TFilePath(root + "/group with spaces.cln"), Overwrite_Off, false);
    for (int index = 0; index < children; ++index) {
      auto* child = new CountedTask(id + "." + QString::number(index), root,
                                    false, observed);
      child->m_parentId = id;
      result->addTask(child);
    }
    controller->addTask(id, result, false);
    return result;
  }

  void drained(int submissions) {
    require(pumpUntil([&] {
              return controller->pendingExecutionCount() == 0 &&
                     observed->released(submissions);
            }),
            "batch submissions did not retire and release their runners");
    observed->requireReleased(submissions);
  }
};

void exerciseQueuedWithdrawal(const QString& root) {
  Fixture fixture(root);
  auto* controller          = fixture.controller;
  auto gate                 = fixture.gate();
  fixture.hooks->runEntered = [gate] { gate->block(); };
  fixture.task("batch-active");
  fixture.task("batch-queued");
  controller->start("batch-active");
  gate->await();
  controller->start("batch-queued");
  require(controller->pendingExecutionCount() == 2,
          "queued batch submission was not registered");
  controller->stop("batch-queued");
  require(controller->canEditTask("batch-queued"),
          "withdrawn batch task remained busy");
  controller->removeTask("batch-queued");
  require(!controller->getTask("batch-queued") &&
              fixture.observed->tasksDestroyed == 1,
          "withdrawn queued task was not destroyed exactly once");
  gate->open();
  fixture.drained(2);
  require(fixture.observed->launchedIds() == QStringList{"batch-active"},
          "withdrawn batch command reached the process boundary");
  controller->removeTask("batch-active");
  require(fixture.observed->tasksDestroyed == 2,
          "completed batch task was not destroyed exactly once");
  checkpoint("batch queued withdrawal and ownership passed");
}

void exerciseAssignedCancellation(const QString& root) {
  Fixture fixture(root);
  auto* controller          = fixture.controller;
  auto gate                 = fixture.gate();
  fixture.hooks->runEntered = [gate] { gate->block(); };
  auto* task                = fixture.task("batch-assigned");
  controller->start(task->m_id);
  gate->await();
  controller->stop(task->m_id);
  controller->removeTask(task->m_id);
  require(controller->getTask("batch-assigned") == task &&
              !controller->canEditTask("batch-assigned") &&
              controller->pendingExecutionCount() == 1 &&
              fixture.observed->tasksDestroyed == 0,
          "assigned canceled task was released before worker retirement");
  gate->open();
  fixture.drained(1);
  require(fixture.observed->launchedIds().isEmpty(),
          "assigned canceled command started after the worker gate opened");
  controller->removeTask("batch-assigned");
  require(!controller->getTask("batch-assigned") &&
              fixture.observed->tasksDestroyed == 1,
          "assigned task could not be removed after retirement");
  checkpoint("batch assigned cancellation and retirement passed");
}

void exerciseTerminalRetention(const QString& root) {
  Fixture fixture(root);
  auto* controller                 = fixture.controller;
  auto gate                        = fixture.gate();
  fixture.hooks->terminalPublished = [gate] { gate->block(); };
  auto* task                       = fixture.task("batch-terminal");
  controller->start(task->m_id);
  gate->await();
  require(pumpUntil([&] { return task->m_status == Completed; }),
          "batch terminal outcome was not applied while the worker was gated");
  controller->removeTask(task->m_id);
  require(controller->getTask("batch-terminal") == task &&
              !controller->canEditTask("batch-terminal") &&
              controller->pendingExecutionCount() == 1 &&
              fixture.observed->tasksDestroyed == 0,
          "terminal display status incorrectly permitted task destruction");
  gate->open();
  fixture.drained(1);
  controller->removeTask("batch-terminal");
  require(fixture.observed->tasksDestroyed == 1,
          "terminal task was not destroyed after worker retirement");
  checkpoint("batch terminal status retains worker ownership passed");
}

void exerciseAffectedEdits(const QString& root) {
  Fixture fixture(root);
  auto* controller            = fixture.controller;
  auto gate                   = fixture.gate();
  fixture.hooks->runEntered   = [gate] { gate->block(); };
  auto* group                 = fixture.group("batch-group", 2);
  group->m_isComposerTask     = true;
  group->m_from               = 1;
  group->m_to                 = 4;
  group->m_chunkSize          = 2;
  group->m_taskFilePath       = TFilePath(root + "/group with spaces.tnz");
  group->m_outputPath         = TFilePath(root + "/group with spaces..png");
  auto* dependency            = fixture.task("batch-dependency");
  auto* idle                  = fixture.group("batch-idle", 1);
  idle->m_name                = "Idle batch fixture";
  auto* idleChild             = idle->getTask(0);
  idleChild->m_isComposerTask = true;
  idleChild->m_from           = 1;
  idleChild->m_to = idleChild->m_stepCount = idleChild->m_chunkSize = 4;
  idleChild->m_taskFilePath = TFilePath(root + "/idle with spaces.tnz");
  idleChild->m_outputPath   = TFilePath(root + "/idle with spaces..png");
  dependency->m_status      = Completed;
  group->m_dependencies->add(dependency->m_id);
  controller->start(group->m_id);
  gate->await();
  require(!controller->canEditTask(group->m_id) &&
              !controller->canEditTask(dependency->m_id) &&
              !controller->canEditTask("batch-group.0") &&
              controller->canEditTask("batch-idle"),
          "batch edit protection did not match the affected execution graph");
  controller->setDirtyFlag(false);
  exerciseBatchTaskViewGuards(group->m_id, "batch-idle");
  TFarmTask* child = group->getTask(0);
  const int from = group->m_from, to = group->m_to;
  const int chunkSize    = group->m_chunkSize;
  const TFilePath output = group->m_outputPath;
  require(!controller->setTaskChunkSettings(
              group->m_id, 4, 8, 2, TFilePath(root + "/changed..png")) &&
              group->m_from == from && group->m_to == to &&
              group->m_chunkSize == chunkSize &&
              group->m_outputPath == output && group->getTaskCount() == 2 &&
              group->getTask(0) == child,
          "refused batch chunk replacement partially mutated the group");
  controller->removeTask(dependency->m_id);
  controller->removeTask(group->m_id);
  require(controller->getTask("batch-dependency") == dependency &&
              controller->getTask("batch-group") == group,
          "pending batch dependency or group was removed");
  controller->removeTask("batch-idle");
  require(!controller->getTask("batch-idle") &&
              fixture.observed->tasksDestroyed == 1,
          "unrelated idle task could not be removed during execution");
  gate->open();
  fixture.drained(1);
  require(fixture.observed->launchedIds() ==
              QStringList{"batch-dependency", "batch-group.0", "batch-group.1"},
          "captured dependency and chunk execution order changed");
  controller->removeAllTasks();
  require(
      fixture.observed->tasksDestroyed == 4,
      "group clear failed to destroy its children and dependency exactly once");
  checkpoint("batch affected edits and atomic chunk refusal passed");
}

void exerciseMultipleSelectedTasks(const QString& root) {
  Fixture fixture(root);
  auto* controller          = fixture.controller;
  auto gate                 = fixture.gate();
  fixture.hooks->runEntered = [gate] { gate->block(); };
  fixture.group("batch-selected-A", 1);
  fixture.group("batch-selected-B", 1);
  auto* idle = fixture.group("batch-unselected", 1);
  const QStringList selected{"batch-selected-A", "batch-selected-B"};
  startSelectedBatchTasks(selected);
  gate->await();
  require(
      controller->pendingExecutionCount() == 2 && idle->m_status == Suspended,
      "task tree start did not submit exactly the selected rows");
  stopSelectedBatchTasks(selected);
  require(
      controller->pendingExecutionCount() == 1 && idle->m_status == Suspended,
      "task tree stop did not cancel both selected rows across model "
      "refreshes");
  gate->open();
  fixture.drained(2);
  require(fixture.observed->launchedIds().isEmpty(),
          "selected task cancellation allowed a command to start");
  controller->removeAllTasks();
  require(fixture.observed->tasksDestroyed == 3,
          "multiple task selection retained an owned child");
  checkpoint("batch multiple selected task commands passed");
}

void exerciseTaskTreeSubmissionFailure(const QString& root) {
  Fixture fixture(root);
  auto* controller                     = fixture.controller;
  fixture.hooks->throwBeforeAcceptance = true;
  auto* task = fixture.group("batch-tree-submission-failure", 1);
  startSelectedBatchTasks(QStringList{task->m_id});
  fixture.drained(1);
  require(task->m_status == Suspended && controller->canEditTask(task->m_id) &&
              fixture.observed->launchedIds().isEmpty(),
          "task tree submission error escaped or left submitted work behind");
  checkpoint("batch task tree submission exception contained passed");
}

void exerciseChunkCollisionEditor(const QString& root) {
  Fixture fixture(root);
  auto* controller        = fixture.controller;
  auto* group             = fixture.group("batch-chunk-collision", 2);
  group->m_isComposerTask = true;
  group->m_from = group->m_step = group->m_shrink = 1;
  group->m_to = group->m_stepCount = 4;
  group->m_chunkSize               = 2;
  auto* collision                  = fixture.task("batch-chunk-collision.01");
  controller->setDirtyFlag(false);
  exerciseBatchTaskChunkCollision(group->m_id);
  require(controller->getTaskCount() == 4 &&
              controller->getTask("batch-chunk-collision.01") == collision,
          "chunk collision changed the unrelated conflicting task");
  controller->removeAllTasks();
  require(fixture.observed->tasksDestroyed == 3,
          "chunk collision retained or destroyed an existing task twice");
  checkpoint("batch chunk collision editor recovery passed");
}

void exerciseStopAll(const QString& root) {
  Fixture fixture(root);
  auto* controller          = fixture.controller;
  auto gate                 = fixture.gate();
  fixture.hooks->runEntered = [gate] { gate->block(); };
  fixture.task("batch-stop-active");
  fixture.task("batch-stop-queued");
  controller->start("batch-stop-active");
  gate->await();
  controller->start("batch-stop-queued");
  controller->stopAll();
  require(controller->pendingExecutionCount() == 1 &&
              !controller->canEditTask("batch-stop-active") &&
              controller->canEditTask("batch-stop-queued"),
          "stop-all conflated queued withdrawal with assigned cancellation");
  controller->removeTask("batch-stop-queued");
  gate->open();
  fixture.drained(2);
  require(fixture.observed->launchedIds().isEmpty(),
          "stop-all allowed a canceled batch command to start");
  controller->removeAllTasks();
  require(fixture.observed->tasksDestroyed == 2,
          "stop-all did not release each canceled task exactly once");
  checkpoint("batch stop-all retirement passed");
}

void exerciseSubmissionFailure(const QString& root, bool afterAcceptance) {
  Fixture fixture(root);
  auto* controller = fixture.controller;
  auto* task       = fixture.task("batch-submission-failure");
  fixture.hooks->throwBeforeAcceptance = !afterAcceptance;
  fixture.hooks->throwAfterAcceptance  = afterAcceptance;
  bool threw                           = false;
  try {
    controller->start(task->m_id);
  } catch (const std::runtime_error& error) {
    require(QString::fromLocal8Bit(error.what())
                .startsWith("Batch fixture: failure"),
            "batch submission threw an unexpected exception");
    threw = true;
  }
  fixture.drained(1);
  require(threw && fixture.observed->launchedIds().isEmpty() &&
              controller->canEditTask(task->m_id),
          "batch submission fault did not throw, launched work, or retained "
          "ownership");
  if (!afterAcceptance)
    require(task->m_status == Suspended,
            "unpublished batch submission changed the original task status");
  controller->removeTask(task->m_id);
  require(fixture.observed->tasksDestroyed == 1,
          "failed batch submission retained its task after retirement");
}

void exerciseWorkerException(const QString& root) {
  Fixture fixture(root);
  auto* controller       = fixture.controller;
  auto state             = fixture.observed;
  fixture.hooks->execute = [state](
                               const Command& command,
                               const std::function<bool()>&) -> ProcessResult {
    state->launched(command);
    throw std::runtime_error("Batch fixture: process boundary exception");
  };
  auto* task = fixture.task("batch-worker-exception");
  controller->start(task->m_id);
  fixture.drained(1);
  require(task->m_status == Aborted && controller->canEditTask(task->m_id),
          "batch exception failed to abort and release the affected task");
  controller->removeTask(task->m_id);
  require(fixture.observed->tasksDestroyed == 1,
          "batch exception retained its task after retirement");
  checkpoint("batch submission failures and worker exception passed");
}

void exerciseClearAndLoad(const QString& root) {
  Fixture fixture(root);
  auto* controller = fixture.controller;
  fixture.group("batch-saved", 0);
  const TFilePath saved(root + "/batch saved list.tnzbat");
  controller->doSave(saved);
  controller->removeAllTasks();

  auto gate                 = fixture.gate();
  fixture.hooks->runEntered = [gate] { gate->block(); };
  auto* active              = fixture.task("batch-load-active");
  fixture.task("batch-load-queued");
  controller->start(active->m_id);
  gate->await();
  controller->start("batch-load-queued");
  controller->setDirtyFlag(false);
  const quint64 generation = controller->taskGeneration();
  controller->removeAllTasks();
  controller->doLoad(saved);
  require(controller->getTaskCount() == 2 &&
              controller->getTask("batch-load-active") == active &&
              controller->taskGeneration() == generation,
          "clear or load replaced a list containing pending batch work");
  controller->stopAll();
  controller->removeAllTasks();
  controller->doLoad(saved);
  require(controller->getTaskCount() == 2 &&
              controller->getTask("batch-load-active") == active &&
              controller->taskGeneration() == generation,
          "suspended assigned work did not prevent list replacement");
  gate->open();
  fixture.drained(2);
  controller->removeAllTasks();
  require(
      controller->getTaskCount() == 0 && fixture.observed->tasksDestroyed == 2,
      "clear after retirement did not destroy all task owners");
  controller->doLoad(saved);
  TFarmTask* loaded = controller->getTask("batch-saved");
  require(loaded && controller->getTaskCount() == 1,
          "valid task list could not be loaded after retirement");
  const quint64 loadedGeneration = controller->taskGeneration();
  bool rejected                  = false;
  try {
    controller->doLoad(TFilePath(root + "/batch missing list.tnzbat"));
  } catch (const TException&) {
    rejected = true;
  }
  require(rejected && controller->getTask("batch-saved") == loaded &&
              controller->getTaskCount() == 1 &&
              controller->taskGeneration() == loadedGeneration,
          "failed task-list loading discarded the previous list");
  checkpoint("batch active clear and load refusal passed");
}

void replayTerminalFacts(const std::shared_ptr<Observations>& observed) {
  std::vector<std::function<void()>> replays;
  {
    std::lock_guard<std::mutex> lock(observed->mutex);
    replays = observed->terminalReplays;
  }
  require(!replays.empty(), "batch fixture did not capture a terminal fact");
  for (const auto& replay : replays) {
    replay();
    replay();
  }
}

void requireFreshTask(TFarmTask* task) {
  require(task && task->m_status == Suspended &&
              task->m_successfullSteps == 0 && task->m_failedSteps == 0 &&
              !task->m_startDate.isValid() && !task->m_completionDate.isValid(),
          "stale batch outcome modified a replacement task with the same ID");
}

void exerciseRetiredOutcomes(const QString& root) {
  Fixture fixture(root);
  auto* controller = fixture.controller;
  fixture.task("batch-reused-id");
  controller->start("batch-reused-id");
  fixture.drained(1);
  const quint64 generation = controller->taskGeneration();
  controller->removeTask("batch-reused-id");
  auto* replacement = fixture.task("batch-reused-id");
  require(controller->taskGeneration() == generation,
          "individual replacement unexpectedly replaced the whole task list");
  replayTerminalFacts(fixture.observed);
  requireFreshTask(replacement);
  fixture.observed->requireReleased(1);
  controller->removeAllTasks();
  require(controller->taskGeneration() != generation,
          "clearing the task list did not advance its identity generation");
  replacement = fixture.task("batch-reused-id");
  replayTerminalFacts(fixture.observed);
  requireFreshTask(replacement);
  fixture.observed->requireReleased(1);
  controller->removeAllTasks();
  require(fixture.observed->tasksDestroyed == 3,
          "replacing a task ID retained or double-destroyed an old task");
  checkpoint("batch retired outcomes cannot modify replacement tasks passed");
}

std::shared_ptr<std::atomic<bool>> queueProgress(BatchesController* controller,
                                                 const QString& id,
                                                 bool success) {
  auto delivered = std::make_shared<std::atomic<bool>>(false);
  std::thread producer([controller, id, success, delivered] {
    controller->postTaskProgress(id, success);
    QMetaObject::invokeMethod(
        controller, [delivered] { *delivered = true; }, Qt::QueuedConnection);
  });
  producer.join();
  return delivered;
}

void exerciseQueuedProgressIdentity(const QString& root) {
  Fixture fixture(root);
  auto* controller = fixture.controller;
  auto* task       = fixture.task("batch-progress-id");
  auto delivered   = queueProgress(controller, task->m_id, true);
  fixture.task("batch-progress-unrelated");
  controller->removeTask("batch-progress-unrelated");
  require(task->m_successfullSteps == 0,
          "batch progress mutated a task off the application thread");
  require(pumpUntil([&] { return delivered->load(); }) &&
              task->m_successfullSteps == 1,
          "unrelated idle membership changes discarded valid queued progress");

  delivered = queueProgress(controller, task->m_id, false);
  controller->removeTask("batch-progress-id");
  task = fixture.task("batch-progress-id");
  require(pumpUntil([&] { return delivered->load(); }),
          "batch progress callback did not reach its delivery boundary");
  requireFreshTask(task);

  delivered = queueProgress(controller, task->m_id, true);
  controller->removeAllTasks();
  task = fixture.task("batch-progress-id");
  require(
      pumpUntil([&] { return delivered->load(); }),
      "batch progress callback did not reach its replacement-list boundary");
  requireFreshTask(task);
  controller->removeAllTasks();
  require(fixture.observed->tasksDestroyed == 4,
          "queued progress retained a removed task incarnation");
  checkpoint("batch queued progress preserves per-task identity passed");
}

void exerciseOwnershipRoots(const QString& root) {
  Fixture fixture(root);
  auto* controller = fixture.controller;
  for (int iteration = 0; iteration < 3; ++iteration) {
    auto* group        = fixture.group("batch-owner", 2);
    auto* orphan       = fixture.task("batch-orphan");
    orphan->m_parentId = "missing-parent";
    TFarmTask* child   = group->getTask(0);
    child->m_parentId  = "incorrect-parent";
    controller->removeTask(child->m_id);
    require(group->getTaskCount() == 1 &&
                fixture.observed->tasksDestroyed == 3 * iteration + 1,
            "child removal used its parent label instead of actual membership");
    controller->removeAllTasks();
    require(
        controller->getTaskCount() == 0 &&
            fixture.observed->tasksDestroyed == 3 * (iteration + 1),
        "repeated clear leaked or double-deleted owned children or an orphan");
  }
  checkpoint("batch repeated clear and ownership roots passed");
}

void writeTaskList(const TFilePath& path, TFarmTask* first, TFarmTask* second) {
  TOStream output(path);
  output.openChild("tnzbatches");
  output.openChild("batch");
  output << first << second;
  output.closeChild();
  output.closeChild();
}

void exerciseLoadAliasesAndDuplicates(const QString& root) {
  Fixture fixture(root);
  auto* controller = fixture.controller;
  auto* group      = fixture.group("batch-legacy-group", 1);
  const TFilePath legacy(root + "/batch legacy aliases.tnzbat");
  writeTaskList(legacy, group, group->getTask(0));
  controller->removeAllTasks();
  require(fixture.observed->tasksDestroyed == 1,
          "saving a legacy task-list fixture retained its original child");
  for (int iteration = 0; iteration < 3; ++iteration) {
    controller->doLoad(legacy);
    auto* loaded = dynamic_cast<TFarmTaskGroup*>(
        controller->getTask("batch-legacy-group"));
    require(
        loaded && loaded->getTaskCount() == 1 &&
            loaded->getTask(0) == controller->getTask("batch-legacy-group.0") &&
            controller->getTaskCount() == 2,
        "legacy task-list aliases did not retain one child identity");
    controller->removeAllTasks();
    require(controller->getTaskCount() == 0,
            "repeated legacy loading and clearing retained a lookup entry");
  }
  auto* preserved = fixture.task("batch-preserved");
  auto first      = std::make_unique<TFarmTaskGroup>(
      "batch-duplicate", "first duplicate", QString(), QString(), 1, 50,
      TFilePath(root + "/duplicate.cln"), Overwrite_Off, false);
  auto second = std::make_unique<TFarmTaskGroup>(
      "batch-duplicate", "second duplicate", QString(), QString(), 1, 50,
      TFilePath(root + "/duplicate.cln"), Overwrite_Off, false);
  const TFilePath duplicates(root + "/batch conflicting duplicates.tnzbat");
  writeTaskList(duplicates, first.get(), second.get());
  const quint64 generation = controller->taskGeneration();
  controller->setDirtyFlag(false);
  bool rejected = false;
  try {
    controller->doLoad(duplicates);
  } catch (const TException&) {
    rejected = true;
  }
  require(rejected && controller->getTaskCount() == 1 &&
              controller->getTask("batch-preserved") == preserved &&
              controller->taskGeneration() == generation,
          "conflicting task IDs replaced the current task list");
  checkpoint("batch legacy aliases and conflicting IDs passed");
}

void requireOwnershipRejected(Fixture& fixture, const char* name,
                              const char* graph) {
  auto* controller         = fixture.controller;
  auto* preserved          = controller->getTask("batch-ownership-preserved");
  const quint64 generation = controller->taskGeneration();
  const QString listName   = controller->getListName();
  const QString path       = fixture.root + "/batch ownership " +
                             QString::fromLatin1(name) + ".tnzbat";
  const QByteArray contents =
      QByteArray("<tnzbatches><batch>\n") + graph + "\n</batch></tnzbatches>\n";
  QFile file(path);
  require(file.open(QIODevice::WriteOnly | QIODevice::Truncate),
          "batch ownership fixture file could not be created");
  require(file.write(contents) == contents.size() && file.flush(),
          "batch ownership fixture file could not be written completely");
  file.close();
  bool rejected = false;
  try {
    controller->doLoad(TFilePath(path));
  } catch (const TException& error) {
    require(QString::fromStdWString(error.getMessage()).contains("ownership"),
            "batch ownership fixture failed before ownership validation");
    rejected = true;
  }
  require(rejected && controller->getTaskCount() == 1 &&
              controller->getTask("batch-ownership-preserved") == preserved &&
              controller->taskGeneration() == generation &&
              controller->getListName() == listName &&
              !controller->getDirtyFlag() &&
              fixture.observed->tasksDestroyed == 0,
          "invalid serialized ownership replaced or destroyed the previous "
          "task list");
}

void exerciseInvalidSerializedOwnership(const QString& root) {
  Fixture fixture(root);
  fixture.task("batch-ownership-preserved");
  fixture.controller->setDirtyFlag(false);
  const struct {
    const char* name;
    const char* graph;
  } cases[] = {{"self-cycle", R"(
<ttaskgroup id="1">
  <info><taskId>"cycle-owner"</taskId></info>
  <tasks><ttaskgroup id="1"/></tasks>
</ttaskgroup>)"},
               {"duplicate-child", R"(
<ttaskgroup id="1">
  <info><taskId>"duplicate-child-owner"</taskId></info>
  <tasks>
    <ttask id="2"><taskId>"duplicate-child"</taskId></ttask>
    <ttask id="2"/>
  </tasks>
</ttaskgroup>)"},
               {"shared-child", R"(
<ttaskgroup id="1">
  <info><taskId>"first-owner"</taskId></info>
  <tasks><ttask id="2"><taskId>"shared-child"</taskId></ttask></tasks>
</ttaskgroup>
<ttaskgroup id="3">
  <info><taskId>"second-owner"</taskId></info>
  <tasks><ttask id="2"/></tasks>
</ttaskgroup>)"}};
  for (const auto& test : cases)
    requireOwnershipRejected(fixture, test.name, test.graph);
  fixture.controller->removeAllTasks();
  require(fixture.observed->tasksDestroyed == 1,
          "ownership rejection retained or destroyed the preserved task twice");
  checkpoint(
      "batch serialized ownership cycles and duplicate owners rejected passed");
}

void exerciseSharedDependencyCancellation(const QString& root) {
  Fixture fixture(root);
  auto* controller     = fixture.controller;
  auto firstGate       = fixture.gate();
  auto secondGate      = fixture.gate();
  auto* first          = fixture.task("batch-first-owner");
  auto* second         = fixture.task("batch-second-owner");
  auto* dependency     = fixture.task("batch-shared-dependency");
  dependency->m_status = Completed;
  first->m_dependencies->add(dependency->m_id);
  second->m_dependencies->add(dependency->m_id);
  auto state             = fixture.observed;
  fixture.hooks->execute = [state, firstGate](
                               const Command& command,
                               const std::function<bool()>& canceled) {
    state->launched(command);
    firstGate->block();
    ProcessResult result;
    result.canceled = canceled();
    return result;
  };
  controller->start(first->m_id);
  firstGate->await();
  auto secondHooks        = std::make_shared<TestHooks>(*fixture.hooks);
  secondHooks->runEntered = [secondGate] { secondGate->block(); };
  controller->setExecutionTestHooks(secondHooks);
  controller->start(second->m_id);
  controller->stop(first->m_id);
  firstGate->open();
  secondGate->await();
  require(pumpUntil([&] { return controller->pendingExecutionCount() == 1; }),
          "stopped batch owner did not retire independently");
  require(
      !controller->canEditTask(dependency->m_id),
      "shared dependency became editable while its other owner was pending");
  secondGate->open();
  fixture.drained(2);
  require(
      fixture.observed->launchedIds() == QStringList{"batch-shared-dependency",
                                                     "batch-shared-dependency",
                                                     "batch-second-owner"} &&
          second->m_status == Completed,
      "stopping one batch owner canceled another owner's shared dependency");
  checkpoint("batch shared dependency cancellation isolation passed");
}

void exerciseChildCancellation(const QString& root) {
  Fixture fixture(root);
  auto* controller       = fixture.controller;
  auto gate              = fixture.gate();
  auto* group            = fixture.group("batch-child-cancel", 2);
  auto state             = fixture.observed;
  fixture.hooks->execute = [state, gate](
                               const Command& command,
                               const std::function<bool()>& canceled) {
    state->launched(command);
    if (command.taskId == "batch-child-cancel.0") gate->block();
    ProcessResult result;
    result.canceled = canceled();
    return result;
  };
  controller->start(group->m_id);
  gate->await();
  controller->stop("batch-child-cancel.0");
  gate->open();
  fixture.drained(1);
  require(fixture.observed->launchedIds() ==
                  QStringList{"batch-child-cancel.0", "batch-child-cancel.1"} &&
              group->getTask(1)->m_status == Completed &&
              group->m_status == Aborted,
          "stopping a batch child canceled its sibling or completed the group");
  checkpoint("batch child cancellation preserves sibling execution passed");
}

void exerciseCapturedCommands(const QString& root) {
  Fixture fixture(root);
  auto* controller              = fixture.controller;
  auto* composer                = fixture.task("batch-composer", true);
  auto* cleanup                 = fixture.task("batch-cleanup");
  cleanup->m_overwrite          = Overwrite_NoPaint;
  cleanup->m_onlyVisible        = true;
  composer->m_callerMachineName = cleanup->m_callerMachineName =
      QHostInfo::localHostName();
  const Command composerCommand{composer->m_id,
                                composer->getCommandLinePrgName(),
                                composer->getCommandLineArgumentsList()};
  const Command cleanupCommand{cleanup->m_id, cleanup->getCommandLinePrgName(),
                               cleanup->getCommandLineArgumentsList()};
  controller->start(composer->m_id);
  fixture.drained(1);
  controller->start(cleanup->m_id);
  fixture.drained(2);
  std::lock_guard<std::mutex> lock(fixture.observed->mutex);
  const auto& commands = fixture.observed->commands;
  require(commands.size() == 2 &&
              commands[0].program == composerCommand.program &&
              commands[0].arguments == composerCommand.arguments &&
              commands[1].program == cleanupCommand.program &&
              commands[1].arguments == cleanupCommand.arguments,
          "batch capture changed composer or cleanup command-builder output");
  checkpoint("batch composer and cleanup command preservation passed");
}

void exerciseDependencyOrder(const QString& root) {
  Fixture fixture(root);
  auto* controller = fixture.controller;
  auto* a          = fixture.task("batch-A");
  auto* b          = fixture.task("batch-B");
  auto* c          = fixture.task("batch-C");
  auto* d          = fixture.task("batch-D");
  auto* suspended  = fixture.task("batch-suspended");
  b->m_status = c->m_status = d->m_status = Completed;
  a->m_dependencies->add(b->m_id);
  a->m_dependencies->add(c->m_id);
  a->m_dependencies->add("batch-missing");
  a->m_dependencies->add(suspended->m_id);
  b->m_dependencies->add(d->m_id);
  b->m_dependencies->add(d->m_id);
  c->m_dependencies->add(d->m_id);
  suspended->m_dependencies->add(suspended->m_id);
  auto state             = fixture.observed;
  fixture.hooks->execute = [state](const Command& command,
                                   const std::function<bool()>&) {
    state->launched(command);
    ProcessResult result;
    result.failed = command.taskId == "batch-D";
    return result;
  };
  controller->start(a->m_id);
  fixture.drained(1);
  require(fixture.observed->launchedIds() ==
                  QStringList{"batch-D", "batch-D", "batch-B", "batch-D",
                              "batch-C", "batch-A"} &&
              a->m_status == Completed && d->m_status == Aborted &&
              suspended->m_status == Suspended,
          "batch graph changed duplicate, diamond, missing, or suspended "
          "dependencies");
  checkpoint("batch dependency order and repeated execution passed");
}

void exerciseGroupCardinality(const QString& root) {
  Fixture fixture(root);
  auto* controller = fixture.controller;
  for (int children = 0; children < 3; ++children) {
    const QString id = "batch-children-" + QString::number(children);
    fixture.group(id, children);
    controller->start(id);
    fixture.drained(children + 1);
  }
  require(fixture.observed->launchedIds() ==
              QStringList{"batch-children-0", "batch-children-1",
                          "batch-children-2.0", "batch-children-2.1"},
          "batch zero, one, or multiple-child execution semantics changed");
  checkpoint("batch group cardinality compatibility passed");
}

void exerciseStartAllEligibility(const QString& root) {
  Fixture fixture(root);
  auto* controller          = fixture.controller;
  auto gate                 = fixture.gate();
  fixture.hooks->runEntered = [gate] { gate->block(); };
  auto* parent              = fixture.task("batch-all-parent");
  auto* dependency          = fixture.task("batch-all-dependency");
  parent->m_priority        = 100;
  parent->m_dependencies->add(dependency->m_id);
  controller->startAll();
  gate->await();
  gate->open();
  fixture.drained(2);
  require(fixture.observed->launchedIds() ==
              QStringList{"batch-all-dependency", "batch-all-parent",
                          "batch-all-dependency"},
          "start-all captured a dependency before staging its eligibility");
  checkpoint("batch start-all eligibility capture passed");
}

void exerciseRejectedCycles(const QString& root) {
  Fixture fixture(root);
  auto* controller = fixture.controller;
  auto* a          = fixture.task("batch-cycle-A");
  auto* b          = fixture.task("batch-cycle-B");
  b->m_status      = Completed;
  a->m_dependencies->add(b->m_id);
  b->m_dependencies->add(a->m_id);
  for (bool all : {false, true}) {
    bool rejected = false;
    try {
      if (all)
        controller->startAll();
      else
        controller->start(a->m_id);
    } catch (const TException&) {
      rejected = true;
    }
    require(rejected && controller->pendingExecutionCount() == 0 &&
                a->m_status == Suspended && b->m_status == Completed &&
                fixture.observed->launchedIds().isEmpty(),
            "cyclic batch graph was published or changed display state");
  }
  fixture.drained(0);
  checkpoint("batch executable cycle rejection passed");
}

void exerciseCompletedStepFallback(const QString& root) {
  Fixture fixture(root);
  auto* controller       = fixture.controller;
  auto state             = fixture.observed;
  fixture.hooks->execute = [state](const Command& command,
                                   const std::function<bool()>&) {
    state->launched(command);
    ProcessResult result;
    result.exitCode = 7;
    return result;
  };
  auto* zeroSteps        = fixture.task("batch-zero-steps");
  zeroSteps->m_stepCount = 0;
  auto* unfinished       = fixture.task("batch-unfinished-steps");
  controller->start(zeroSteps->m_id);
  fixture.drained(1);
  controller->start(unfinished->m_id);
  fixture.drained(2);
  require(zeroSteps->m_status == Completed && unfinished->m_status == Aborted,
          "batch nonzero-exit completed-step fallback changed");
  checkpoint("batch completed-step exit fallback passed");
}

void exerciseRealProcessStartFailure(const QString& root) {
  Fixture fixture(root);
  auto* controller              = fixture.controller;
  fixture.hooks->execute        = {};
  fixture.hooks->prepareCommand = [root](Command& command) {
    command.program = root + "/batch missing executable";
    command.arguments.clear();
  };
  auto stopped                  = std::make_shared<std::atomic<int>>(0);
  fixture.hooks->processStopped = [stopped](bool notRunning) {
    if (notRunning) ++*stopped;
  };
  auto* task = fixture.task("batch-process-start-failure");
  controller->start(task->m_id);
  fixture.drained(1);
  require(
      task->m_status == Aborted && stopped->load() == 1 &&
          controller->canEditTask(task->m_id),
      "real process start failure did not abort, close, and retire its task");
  controller->removeTask(task->m_id);
  require(fixture.observed->tasksDestroyed == 1,
          "real process start failure retained its task");
  checkpoint("batch real process start failure passed");
}

void exerciseRealProcessAfterPoll(const QString& root) {
  Fixture fixture(root);
  auto* controller              = fixture.controller;
  auto polls                    = std::make_shared<std::atomic<int>>(0);
  auto stopped                  = std::make_shared<std::atomic<int>>(0);
  fixture.hooks->execute        = {};
  fixture.hooks->prepareCommand = [](Command& command) {
    command.program   = QCoreApplication::applicationFilePath();
    command.arguments = QStringList{"--test-batch-task-child"};
  };
  fixture.hooks->processPollTimedOut = [polls](QProcess& process) {
    if (polls->fetch_add(1) == 0)
      require(process.write("x", 1) == 1,
              "batch child could not receive its deterministic release byte");
  };
  fixture.hooks->processStopped = [stopped](bool notRunning) {
    if (notRunning) ++*stopped;
  };
  auto* task = fixture.task("batch-process-poll-success");
  controller->start(task->m_id);
  fixture.drained(1);
  require(
      polls->load() >= 1 && stopped->load() == 1 && task->m_status == Completed,
      "a batch polling timeout changed a later successful child exit");
  checkpoint("batch real child success after polling timeout passed");
}

void exerciseRealProcessCancellation(const QString& root) {
  Fixture fixture(root);
  auto* controller              = fixture.controller;
  auto started                  = std::make_shared<std::atomic<qint64>>(0);
  auto stopped                  = std::make_shared<std::atomic<int>>(0);
  auto reaped                   = std::make_shared<std::atomic<bool>>(false);
  fixture.hooks->execute        = {};
  fixture.hooks->prepareCommand = [](Command& command) {
    command.program   = QCoreApplication::applicationFilePath();
    command.arguments = QStringList{"--test-batch-task-child"};
  };
  fixture.hooks->processStarted = [started](qint64 pid) { *started = pid; };
  fixture.hooks->processStopped = [stopped, reaped](bool notRunning) {
    *reaped = notRunning;
    ++*stopped;
  };
  const auto recordRetired = fixture.hooks->retired;
  auto retiredAfterReaping = std::make_shared<std::atomic<bool>>(false);
  fixture.hooks->retired   = [recordRetired, retiredAfterReaping, reaped,
                              stopped](quint64 id) {
    *retiredAfterReaping = reaped->load() && stopped->load() == 1;
    recordRetired(id);
  };
  auto* task = fixture.task("batch-real-child");
  controller->start("batch-real-child");
  require(pumpUntil([&] { return started->load() > 0; }),
          "real batch child did not start");
  controller->stop("batch-real-child");
  fixture.drained(1);
  require(retiredAfterReaping->load() && reaped->load() &&
              stopped->load() == 1 && task->m_status == Aborted,
          "batch child was not canceled and reaped before runner retirement");
  controller->removeTask("batch-real-child");
  require(fixture.observed->tasksDestroyed == 1,
          "real batch child cancellation retained its task");
  checkpoint("batch real child cancellation and reaping passed");
}

}  // namespace

int runBatchTaskFixtureChild() {
  std::cin.get();
  return 0;
}

void exerciseBatchTaskLifetime(const QString& testRoot) {
  exerciseQueuedWithdrawal(testRoot);
  exerciseAssignedCancellation(testRoot);
  exerciseTerminalRetention(testRoot);
  exerciseAffectedEdits(testRoot);
  exerciseMultipleSelectedTasks(testRoot);
  exerciseTaskTreeSubmissionFailure(testRoot);
  exerciseChunkCollisionEditor(testRoot);
  exerciseStopAll(testRoot);
  exerciseSubmissionFailure(testRoot, false);
  exerciseSubmissionFailure(testRoot, true);
  exerciseWorkerException(testRoot);
  exerciseClearAndLoad(testRoot);
  exerciseRetiredOutcomes(testRoot);
  exerciseQueuedProgressIdentity(testRoot);
  exerciseOwnershipRoots(testRoot);
  exerciseLoadAliasesAndDuplicates(testRoot);
  exerciseInvalidSerializedOwnership(testRoot);
  exerciseSharedDependencyCancellation(testRoot);
  exerciseChildCancellation(testRoot);
  exerciseCapturedCommands(testRoot);
  exerciseDependencyOrder(testRoot);
  exerciseGroupCardinality(testRoot);
  exerciseStartAllEligibility(testRoot);
  exerciseRejectedCycles(testRoot);
  exerciseCompletedStepFallback(testRoot);
  exerciseRealProcessStartFailure(testRoot);
  exerciseRealProcessAfterPoll(testRoot);
  exerciseRealProcessCancellation(testRoot);
}
