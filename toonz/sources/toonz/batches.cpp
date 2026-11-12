

#ifdef _WIN32
#define _WIN32_WINNT 0x0500  // per CreateJobObject e affini
#endif

#include "tsystem.h"
#include "mainwindow.h"
#include "batches.h"
#include "batchtaskexecution.h"
#include "tthreadmessage.h"
#include "tfarmexecutor.h"
#include "tstream.h"
#include "tfarmstuff.h"
#include "tenv.h"
#include "toutputproperties.h"
#include "tlevel_io.h"
#include "trasterfx.h"
#include "tasksviewer.h"
#include "tapp.h"
#include "filebrowserpopup.h"
#include "tmsgcore.h"
#include "toonz/toonzscene.h"
#include "toonz/sceneproperties.h"
#include "toonz/preferences.h"

#include "toonzqt/gutil.h"

#include <QString>
#include <QCoreApplication>
#include <QPointer>
#include <QThread>
#include <climits>
#include <functional>
#include <memory>
#include <stdexcept>
#include <QHostInfo>

namespace {

class BatchCallbackMessage final : public TThread::Message {
  std::function<void()> m_callback;

public:
  explicit BatchCallbackMessage(std::function<void()> callback)
      : m_callback(std::move(callback)) {}
  void onDeliver() override { m_callback(); }
  TThread::Message* clone() const override {
    return new BatchCallbackMessage(*this);
  }
};

}  // namespace

//------------------------------------------------------------------------------------------------------------------
//------------------------------------------------------------------------------------------------------------------
//----------------------------------------------------------------------------------------------------------------
//----------------------------------------------------------------------------------------------------------------

LoadTaskListPopup::LoadTaskListPopup()
    : FileBrowserPopup(tr("Load Task List")) {
  addFilterType("tnzbat");
  setOkText(tr("Load"));
}

bool LoadTaskListPopup::execute() {
  if (m_selectedPaths.empty()) return false;

  const TFilePath &fp = *m_selectedPaths.begin();

  if (!TSystem::doesExistFileOrLevel(fp)) {
    DVGui::error(toQString(fp) + tr(" does not exist."));
    return false;
  } else if (fp.getType() != "tnzbat") {
    DVGui::error(toQString(fp) +
                 tr("It is possible to load only TNZBAT files."));
    return false;
  }

  BatchesController::instance()->doLoad(fp);
  return true;
}

//----------------------------------------------------------------------------------------------------------------

LoadTaskPopup::LoadTaskPopup() : FileBrowserPopup(""), m_isRenderTask(true) {
  addFilterType("tnz");
  setOkText(tr("Add"));
}

void LoadTaskPopup::open(bool isRenderTask) {
  m_isRenderTask = isRenderTask;
  setWindowTitle(isRenderTask ? tr("Add Render Task to Batch List")
                              : tr("Add Cleanup Task to Batch List"));

  if (isRenderTask)
    removeFilterType("cln");
  else
    addFilterType("cln");

  exec();
}

bool LoadTaskPopup::execute() {
  if (m_selectedPaths.empty()) return false;

  const TFilePath &fp = *m_selectedPaths.begin();

  if (!TSystem::doesExistFileOrLevel(fp)) {
    DVGui::error(toQString(fp) + tr(" does not exist."));
    return false;
  } else if (m_isRenderTask && fp.getType() != "tnz") {
    DVGui::error(toQString(fp) +
                 tr(" you can load only TNZ files for render task."));
    return false;
  } else if (!m_isRenderTask && fp.getType() != "tnz" &&
             fp.getType() != "cln") {
    DVGui::error(toQString(fp) +
                 tr(" you can load only TNZ or CLN files for cleanup task."));
    return false;
  }

  if (m_isRenderTask)
    BatchesController::instance()->addComposerTask(fp);
  else
    BatchesController::instance()->addCleanupTask(fp);

  return true;
}

//=============================================================================

SaveTaskListPopup::SaveTaskListPopup()
    : FileBrowserPopup(tr("Save Task List")) {
  setOkText(tr("Save"));
}

bool SaveTaskListPopup::execute() {
  if (m_selectedPaths.empty()) return false;

  const TFilePath &fp = *m_selectedPaths.begin();

  BatchesController::instance()->doSave(fp.withType("tnzbat"));
  return true;
}

//------------------------------------------------------------------------------
struct BatchExecutionRecord {
  std::shared_ptr<const BatchTaskExecution::Plan> plan;
  std::shared_ptr<BatchTaskExecution::Control> control;
  TThread::RunnableP runnable;
};

struct BatchExecutionState {
  quint64 generation      = 1;
  quint64 nextIncarnation = 1;
  quint64 nextSubmission  = 1;
  std::shared_ptr<const std::map<QString, BatchTaskExecution::Identity>>
      progressIdentities = std::make_shared<
          const std::map<QString, BatchTaskExecution::Identity>>();
  std::map<QString, quint64> incarnations;
  std::map<quint64, BatchExecutionRecord> records;
  std::map<quint64, std::set<quint64>> members;
  bool closing = false;
#ifdef BUILD_UI_PORT_TESTS
  std::shared_ptr<BatchTaskExecution::TestHooks> hooks;
#endif
};

namespace {
using BatchTaskExecution::Identity;
using BatchTaskExecution::Node;
using BatchTaskExecution::Outcome;
using BatchTaskExecution::Plan;

std::shared_ptr<const std::map<QString, Identity>> taskIdentities(
    const std::map<QString, quint64>& incarnations, quint64 generation) {
  auto identities = std::make_shared<std::map<QString, Identity>>();
  for (const auto& entry : incarnations)
    identities->emplace(entry.first,
                        Identity{entry.first, generation, entry.second});
  return identities;
}

void collectTasks(TFarmTask* task, std::map<QString, TFarmTask*>& tasks) {
  if (!task || task->m_id.isEmpty() || !tasks.emplace(task->m_id, task).second)
    throw TException("Task list contains an empty or duplicate task ID");
  if (auto* group = dynamic_cast<TFarmTaskGroup*>(task))
    for (int i = 0; i < group->getTaskCount(); ++i)
      collectTasks(group->getTask(i), tasks);
}

std::vector<TFarmTask*> taskRoots(const std::map<QString, TFarmTask*>& tasks) {
  std::set<TFarmTask*> children;
  for (const auto& entry : tasks)
    if (auto* group = dynamic_cast<TFarmTaskGroup*>(entry.second))
      for (int i = 0; i < group->getTaskCount(); ++i)
        children.insert(group->getTask(i));
  std::vector<TFarmTask*> roots;
  roots.reserve(tasks.size());
  for (const auto& entry : tasks)
    if (!children.count(entry.second)) roots.push_back(entry.second);
  return roots;
}

std::vector<std::unique_ptr<TFarmTask>> taskOwners(
    const std::map<QString, TFarmTask*>& tasks) {
  const auto roots = taskRoots(tasks);
  std::vector<std::unique_ptr<TFarmTask>> owners;
  owners.reserve(roots.size());
  for (auto* root : roots) owners.emplace_back(root);
  return owners;
}

struct PreparedTask {
  QString id;
  TaskState status;
  int successful;
  int failed;
  QDateTime start;
  QDateTime completion;
  QString caller;
  explicit PreparedTask(const TFarmTask& task)
      : id(task.m_id)
      , status(task.m_status)
      , successful(task.m_successfullSteps)
      , failed(task.m_failedSteps)
      , start(task.m_startDate)
      , completion(task.m_completionDate)
      , caller(task.m_callerMachineName) {}
  void restore(TFarmTask& task) const {
    task.m_status            = status;
    task.m_successfullSteps  = successful;
    task.m_failedSteps       = failed;
    task.m_startDate         = start;
    task.m_completionDate    = completion;
    task.m_callerMachineName = caller;
  }
};

class PlanBuilder {
  BatchesController& m_controller;
  const BatchExecutionState& m_state;
  std::shared_ptr<Plan> m_plan = std::make_shared<Plan>();
  std::map<TFarmTask*, size_t> m_indices;
  std::set<TFarmTask*> m_visiting;
  std::set<TFarmTask*> m_protected;

  Identity identity(TFarmTask* task) const {
    return {task->m_id, m_state.generation,
            m_state.incarnations.at(task->m_id)};
  }
  void protect(TFarmTask* task) {
    if (!m_protected.insert(task).second) return;
    m_plan->affected.push_back(identity(task));
    if (auto* group = dynamic_cast<TFarmTaskGroup*>(task))
      for (int i = 0; i < group->getTaskCount(); ++i)
        protect(group->getTask(i));
  }
  void captureEdges(TFarmTask* task, size_t index) {
    if (task->m_dependencies)
      for (int i = 0; i < task->m_dependencies->getTaskCount(); ++i)
        if (auto* dependency =
                m_controller.getTask(task->m_dependencies->getTaskId(i))) {
          const size_t child = capture(dependency);
          m_plan->nodes[index].dependencies.push_back(child);
        }
    if (task->getTaskCount() > 1) {
      for (int i = 0; i < task->getTaskCount(); ++i) {
        const size_t child = capture(task->getTask(i));
        m_plan->nodes[index].children.push_back(child);
      }
    } else {
      m_plan->nodes[index].command = {task->m_id, task->getCommandLinePrgName(),
                                      task->getCommandLineArgumentsList()};
    }
  }

  size_t capture(TFarmTask* task) {
    if (m_visiting.count(task))
      throw TException("Batch task dependencies contain a cycle");
    auto found = m_indices.find(task);
    if (found != m_indices.end()) return found->second;
    protect(task);
    Node node;
    node.identity       = identity(task);
    node.eligible       = task->m_status != Suspended;
    const qint64 frames = qint64(task->m_to) - task->m_from + 1;
    if (node.eligible && (frames < 1 || frames > INT_MAX))
      throw TException("Batch task frame range is invalid");
    node.frameCount    = node.eligible ? int(frames) : 0;
    node.stepCount     = task->m_stepCount;
    const size_t index = m_plan->nodes.size();
    m_indices.emplace(task, index);
    m_plan->nodes.push_back(node);
    if (!node.eligible) return index;
    m_visiting.insert(task);
    captureEdges(task, index);
    m_visiting.erase(task);
    return index;
  }

public:
  PlanBuilder(BatchesController& controller, const BatchExecutionState& state)
      : m_controller(controller), m_state(state) {}
  std::shared_ptr<Plan> build(TFarmTask* root) {
    m_plan->root = capture(root);
    return m_plan;
  }
};

std::vector<std::unique_ptr<TFarmTask>> prepareChunks(const TFarmTask& task,
                                                      int from, int to,
                                                      int size,
                                                      const TFilePath& output) {
  std::vector<std::unique_ptr<TFarmTask>> children;
  const qint64 frames = qint64(to) - from + 1;
  const qint64 count  = (frames + size - 1) / size;
  if (count <= 1) return children;
  children.reserve(size_t(count));
  qint64 first = from;
  for (qint64 i = 1; i <= count; ++i) {
    const int last     = int(std::min(first + size - 1, qint64(to)));
    const QString name = task.m_name + " " +
                         QString::number(first).rightJustified(2, QChar('0')) +
                         "-" +
                         QString::number(last).rightJustified(2, QChar('0'));
    auto child         = std::make_unique<TFarmTask>(
        task.m_id + "." + QString::number(i).rightJustified(2, QChar('0')),
        name, true, task.m_user, task.m_hostName, int(last - first + 1),
        task.m_priority, task.m_taskFilePath, output, int(first), last,
        task.m_step, task.m_shrink, task.m_multimedia, size,
        task.m_threadsIndex, task.m_maxTileSizeIndex, Overwrite_Off, false);
    child->m_parentId       = task.m_id;
    child->m_status         = Suspended;
    child->m_submissionDate = QDateTime::currentDateTime();
    children.push_back(std::move(child));
    first = qint64(last) + 1;
  }
  return children;
}

std::vector<std::unique_ptr<TFarmTask>> replaceGroupChildren(
    TFarmTaskGroup& group, std::vector<std::unique_ptr<TFarmTask>>& children) {
  const int oldCount = group.getTaskCount();
  std::vector<std::unique_ptr<TFarmTask>> retired;
  retired.reserve(size_t(oldCount));
  size_t attached = 0;
  try {
    for (const auto& child : children) {
      group.addTask(child.get());
      ++attached;
    }
  } catch (...) {
    for (size_t i = 0; i < attached; ++i) group.removeTask(children[i].get());
    throw;
  }
  for (int i = 0; i < oldCount; ++i) {
    TFarmTask* child = group.getTask(0);
    group.removeTask(child);
    retired.emplace_back(child);
  }
  for (auto& child : children) child.release();
  return retired;
}

bool validChunkRange(int from, int to, int chunkSize) {
  const qint64 frames = qint64(to) - from + 1;
  return chunkSize > 0 && frames >= 1 && frames <= INT_MAX;
}

bool chunkSettingsChanged(const TFarmTask& task, int from, int to,
                          int chunkSize) {
  return task.m_from != from || task.m_to != to ||
         task.m_chunkSize != chunkSize;
}

class TaskListReader {
  std::map<TFarmTask*, std::unique_ptr<TFarmTask>> m_roots;
  std::set<TFarmTask*> m_seen;
  std::set<TFarmTask*> m_visiting;
  std::map<TFarmTask*, TFarmTaskGroup*> m_parents;
  std::map<QString, TFarmTask*> m_tasks;

  bool claimChild(TFarmTaskGroup& group, TFarmTask* child) {
    if (!child || m_visiting.count(child) || m_parents.count(child)) {
      group.removeTask(child);
      return false;
    }
    m_parents.emplace(child, &group);
    auto root = m_roots.find(child);
    if (root != m_roots.end()) {
      root->second.release();
      m_roots.erase(root);
    }
    return true;
  }

  bool scanOwners(TFarmTask* task) {
    if (!m_seen.insert(task).second) return true;
    m_visiting.insert(task);
    bool valid = true;
    if (auto* group = dynamic_cast<TFarmTaskGroup*>(task)) {
      int i = 0;
      while (i < group->getTaskCount()) {
        auto* child = group->getTask(i);
        if (!claimChild(*group, child)) {
          valid = false;
          continue;
        }
        valid = scanOwners(child) && valid;
        ++i;
      }
    }
    m_visiting.erase(task);
    return valid;
  }

  void index(TFarmTask* task) {
    if (task->m_id.isEmpty())
      throw TException("Task list contains an empty task ID");
    auto found = m_tasks.find(task->m_id);
    if (found != m_tasks.end()) {
      if (found->second != task)
        throw TException("Task list contains conflicting duplicate task IDs");
      return;
    }
    m_tasks.emplace(task->m_id, task);
    if (auto* group = dynamic_cast<TFarmTaskGroup*>(task))
      for (int i = 0; i < group->getTaskCount(); ++i) index(group->getTask(i));
  }

  void adopt(TPersist* value) {
    if (!value) return;
    auto* task = dynamic_cast<TFarmTask*>(value);
    if (!task) {
      delete value;
      throw TException("Task list contains an unexpected object type");
    }
    if (m_seen.count(task))
      return;  // Historical files repeat TPersist pointer references.
    m_roots.emplace(task, std::unique_ptr<TFarmTask>(task));
    if (!scanOwners(task))
      throw TException("Task list contains repeated or cyclic ownership");
    index(task);
  }

public:
  void read(TIStream& stream) {
    TPersist* value = nullptr;
    try {
      stream >> value;
    } catch (...) {
      adopt(value);
      throw;
    }
    adopt(value);
  }
  const std::map<QString, TFarmTask*>& tasks() const { return m_tasks; }
  void release() {
    for (auto& root : m_roots) root.second.release();
    m_roots.clear();
  }
};

void readTaskListContents(TIStream& stream, TaskListReader& tasks) {
  std::string tag;
  while (stream.matchTag(tag)) {
    if (tag == "generator")
      stream.getString();
    else if (tag == "batch")
      while (!stream.eos()) tasks.read(stream);
    else
      throw TException(tag + ": unexpected task list tag");
    if (!stream.matchEndTag()) throw TException(tag + ": missing end tag");
  }
  if (!stream.matchEndTag()) throw TException("tnzbatches: missing end tag");
}

void readTaskList(const TFilePath& path, TaskListReader& tasks) {
  TIStream stream(path);
  if (!stream)
    throw TException(path.getWideString() + L": Cannot open task list");
  std::string tag;
  if (!stream.matchTag(tag) || tag != "tnzbatches")
    throw TException("Bad task list format");
  readTaskListContents(stream, tasks);
}

std::vector<PreparedTask> prepareStart(TFarmTask& task) {
  std::vector<TFarmTask*> prepared{&task};
  if (task.getTaskCount() > 1)
    for (int i = 0; i < task.getTaskCount(); ++i)
      prepared.push_back(task.getTask(i));
  std::vector<PreparedTask> previous;
  for (auto* entry : prepared) previous.emplace_back(*entry);
  for (auto* entry : prepared) {
    entry->m_status      = Waiting;
    entry->m_failedSteps = entry->m_successfullSteps = 0;
    entry->m_completionDate = entry->m_startDate = QDateTime();
    entry->m_callerMachineName                   = QHostInfo().localHostName();
  }
  return previous;
}

std::vector<PreparedTask> prepareAllTasks(
    const std::map<QString, TFarmTask*>& tasks) {
  std::vector<PreparedTask> previous;
  for (const auto& entry : tasks) previous.emplace_back(*entry.second);
  for (const auto& entry : tasks) entry.second->m_status = Waiting;
  return previous;
}

void cancelNodes(const BatchExecutionRecord& record,
                 const std::map<QString, TFarmTask*>& tasks) {
  for (size_t i = 0; i < record.plan->nodes.size(); ++i)
    if (tasks.count(record.plan->nodes[i].identity.id))
      record.control->nodes[i]->store(true);
}

void retireMembership(BatchesController& controller, BatchExecutionState& state,
                      quint64 submission, const Identity& identity,
                      bool failed) {
  auto members = state.members.find(identity.incarnation);
  if (members != state.members.end()) {
    members->second.erase(submission);
    if (members->second.empty()) state.members.erase(members);
  }
  auto* task = controller.getTask(identity.id);
  if (failed && task &&
      (task->m_status == Waiting || task->m_status == Running)) {
    task->m_status         = Aborted;
    task->m_completionDate = QDateTime::currentDateTime();
  }
}

void recordProgress(TFarmTask& task, bool successful) {
  if (successful)
    ++task.m_successfullSteps;
  else
    ++task.m_failedSteps;
}

void restorePrepared(BatchesController& controller,
                     const BatchExecutionState& state,
                     const std::vector<PreparedTask>& previous) {
  for (const auto& saved : previous) {
    auto incarnation = state.incarnations.find(saved.id);
    if (incarnation != state.incarnations.end() &&
        !state.members.count(incarnation->second))
      saved.restore(*controller.getTask(saved.id));
  }
}

void setRunning(TFarmTask& task, const Outcome& outcome) {
  task.m_successfullSteps = task.m_failedSteps = 0;
  task.m_completionDate                        = QDateTime();
  task.m_status                                = Running;
  task.m_server                                = outcome.host;
  task.m_startDate                             = outcome.time;
}

bool successfulOutcome(BatchesController& controller, const TFarmTask& task,
                       const Plan& plan, const Outcome& outcome) {
  if (outcome.process.failed || outcome.process.canceled) return false;
  const Node& node = plan.nodes[outcome.node];
  if (outcome.kind == Outcome::ProcessFinished)
    return outcome.process.exitCode == 0 ||
           task.m_successfullSteps == node.stepCount;
  for (size_t child : node.children) {
    auto* childTask = controller.getTask(plan.nodes[child].identity.id);
    if (childTask && childTask->m_status == Aborted) return false;
  }
  return true;
}

void finishTask(BatchesController& controller, TFarmTask& task,
                const Plan& plan, const Outcome& outcome) {
  const bool success = successfulOutcome(controller, task, plan, outcome);
  task.m_status      = success ? Completed : Aborted;
  if (success || outcome.kind == Outcome::GroupFinished)
    task.m_successfullSteps = plan.nodes[outcome.node].frameCount;
  else
    task.m_failedSteps = plan.nodes[outcome.node].frameCount;
  task.m_completionDate = outcome.time;
}

}  // namespace

BatchesController::BatchesController()
    : m_dirtyFlag(false)
    , m_localControllerPortNumber(0)
    , m_controller(nullptr)
    , m_execution(new BatchExecutionState) {
  connect(QCoreApplication::instance(), &QCoreApplication::aboutToQuit, this,
          &BatchesController::shutdownExecution);
}

//------------------------------------------------------------------------------

BatchesController::~BatchesController() {
  shutdownExecution();
  auto owners = taskOwners(m_tasks);
  m_tasks.clear();
}

std::shared_ptr<Plan> BatchesController::captureTask(TFarmTask* task) {
  auto plan        = PlanBuilder(*this, *m_execution).build(task);
  plan->submission = m_execution->nextSubmission++;
#ifdef BUILD_UI_PORT_TESTS
  if (m_execution->hooks)
    plan->hooks =
        std::make_shared<BatchTaskExecution::TestHooks>(*m_execution->hooks);
#endif
  return plan;
}

void BatchesController::submitTask(std::shared_ptr<Plan> plan) {
  const quint64 id = plan->submission;
  QPointer<BatchesController> receiver(this);
  auto publish = [receiver, id
#ifdef BUILD_UI_PORT_TESTS
                  ,
                  hooks = plan->hooks
#endif
  ](const Outcome& outcome) {
    auto replay = [receiver, id, outcome] {
      BatchCallbackMessage([receiver, id, outcome] {
        if (receiver) receiver->applyOutcome(id, outcome);
      }).send();
    };
    replay();
#ifdef BUILD_UI_PORT_TESTS
    if (hooks && hooks->terminalPosted &&
        (outcome.kind == Outcome::ProcessFinished ||
         outcome.kind == Outcome::GroupFinished))
      hooks->terminalPosted(replay);
#endif
  };
  auto control =
      std::make_shared<BatchTaskExecution::Control>(plan->nodes.size());
  auto runnable =
      BatchTaskExecution::makeRunner(plan, control, std::move(publish));
  m_execution->records.emplace(id,
                               BatchExecutionRecord{plan, control, runnable});
  bool accepted = false;
  try {
    for (const auto& task : plan->affected)
      m_execution->members[task.incarnation].insert(id);
    connect(
        runnable.getPointer(), &TThread::Runnable::finished, this,
        [control] { control->complete(); }, Qt::DirectConnection);
    connect(
        runnable.getPointer(), &TThread::Runnable::exception, this,
        [control] { control->complete(); }, Qt::DirectConnection);
    connect(
        runnable.getPointer(), &TThread::Runnable::finished, this,
        [this, id] { retireTask(id, false); }, Qt::QueuedConnection);
    connect(
        runnable.getPointer(), &TThread::Runnable::exception, this,
        [this, id] { retireTask(id, true); }, Qt::QueuedConnection);
    connect(
        runnable.getPointer(), &TThread::Runnable::canceled, this,
        [control] { control->canceled.store(true); }, Qt::DirectConnection);
    m_localExecutor.addTask(runnable, accepted);
  } catch (...) {
    if (accepted)
      stopSubmission(id);
    else
      retireTask(id, false);
    throw;
  }
}

TFarmTask* BatchesController::liveTask(const Identity& identity) const {
  if (identity.generation != m_execution->generation) return nullptr;
  const auto incarnation = m_execution->incarnations.find(identity.id);
  if (incarnation == m_execution->incarnations.end()) return nullptr;
  if (incarnation->second != identity.incarnation) return nullptr;
  return getTask(identity.id);
}

void BatchesController::applyOutcome(quint64 submission,
                                     const Outcome& outcome) {
  auto found = m_execution->records.find(submission);
  if (found == m_execution->records.end()) return;
  const auto& plan = *found->second.plan;
  if (outcome.node >= plan.nodes.size()) return;
  TFarmTask* task = liveTask(plan.nodes[outcome.node].identity);
  if (!task) return;
  applyTaskOutcome(*task, found->second, outcome);
  notify();
}

void BatchesController::applyTaskOutcome(TFarmTask& task,
                                         const BatchExecutionRecord& record,
                                         const Outcome& outcome) {
  switch (outcome.kind) {
  case Outcome::WaitingForDependencies:
    if (!record.control->isCanceled(outcome.node)) task.m_status = Waiting;
    break;
  case Outcome::Started:
    if (!record.control->isCanceled(outcome.node)) setRunning(task, outcome);
    break;
  case Outcome::ProcessFinished:
  case Outcome::GroupFinished:
    finishTask(*this, task, *record.plan, outcome);
    break;
  }
}

void BatchesController::retireTask(quint64 submission, bool failed) {
  auto found = m_execution->records.find(submission);
  if (found == m_execution->records.end()) return;
  auto record = std::move(found->second);
  m_execution->records.erase(found);
  for (const auto& identity : record.plan->affected)
    retireMembership(*this, *m_execution, submission, identity, failed);
#ifdef BUILD_UI_PORT_TESTS
  if (record.plan->hooks && record.plan->hooks->retired)
    record.plan->hooks->retired(submission);
#endif
  notify();
}

void BatchesController::stopSubmission(quint64 submission) {
  auto found = m_execution->records.find(submission);
  if (found == m_execution->records.end()) return;
  auto record = found->second;
  record.control->canceled.store(true);
  for (const auto& identity : record.plan->affected)
    if (auto* task = getTask(identity.id))
      if (task->m_status == Waiting) task->m_status = Suspended;
  if (m_localExecutor.withdrawQueuedTask(record.runnable))
    retireTask(submission, false);
}

void BatchesController::shutdownExecution() {
  m_execution->closing = true;
  const auto records   = m_execution->records;
  for (const auto& record : records) stopSubmission(record.first);
  for (const auto& record : records) {
    if (!m_execution->records.count(record.first)) continue;
    record.second.control->wait();
    retireTask(record.first, true);
  }
}

void BatchesController::postTaskProgress(const QString& id, bool successful) {
  const auto identities = std::atomic_load(&m_execution->progressIdentities);
  const auto found      = identities->find(id);
  if (found == identities->end()) return;
  const Identity identity = found->second;
  QPointer<BatchesController> receiver(this);
  BatchCallbackMessage([receiver, identity, successful] {
    if (!receiver || receiver->m_execution->closing) return;
    auto* task = receiver->liveTask(identity);
    if (!task) return;
    recordProgress(*task, successful);
    if (auto* parent = receiver->getTask(task->m_parentId))
      recordProgress(*parent, successful);
    receiver->notify();
  }).send();
}

#ifdef BUILD_UI_PORT_TESTS
void BatchesController::setExecutionTestHooks(
    std::shared_ptr<BatchTaskExecution::TestHooks> hooks) {
  m_execution->hooks = std::move(hooks);
}
size_t BatchesController::pendingExecutionCount() const {
  return m_execution->records.size();
}
quint64 BatchesController::taskGeneration() const {
  return m_execution->generation;
}
#endif

//------------------------------------------------------------------------------

void BatchesController::registerTasksTree(TaskTreeModel* tree) {
  m_tasksTrees.insert(tree);
}

//------------------------------------------------------------------------------

void BatchesController::unregisterTasksTree(TaskTreeModel *tree) {
  m_tasksTrees.erase(tree);
}

//------------------------------------------------------------------------------

static int TaskCounter = 1;

//------------------------------------------------------------------------------

void BatchesController::addCleanupTask(const TFilePath &taskFilePath) {
  setDirtyFlag(true);
  QString id = QString::number(TaskCounter++);
  while (BatchesController::instance()->getTask(id))
    id = QString::number(TaskCounter++);

  TFarmTaskGroup *taskGroup = new TFarmTaskGroup(
      id,                                              // id
      QString::fromStdString(taskFilePath.getName()),  // name
      TSystem::getUserName(),                          // user
      TSystem::getHostName(),                          // host
      1,                                               // stepCount
      50,                                              // priority
      taskFilePath,                                    // taskFilePath
      Overwrite_Off,                                   // Overwrite
      false);                                          // onlyvisible

  try {
    BatchesController::instance()->addTask(id, taskGroup);
  } catch (TException &) {
  }
}

//------------------------------------------------------------------------------

void BatchesController::addComposerTask(const TFilePath &_taskFilePath) {
  setDirtyFlag(true);
  TFilePath taskFilePath;
  try {
    taskFilePath = TSystem::toUNC(_taskFilePath);
  } catch (TException &) {
  }

  ToonzScene scene;
  try {
    scene.loadNoResources(taskFilePath);
  } catch (...) {
    return;
  }

  TSceneProperties *sprop      = scene.getProperties();
  const TOutputProperties &out = *sprop->getOutputProperties();
  const TRenderSettings &rs    = out.getRenderSettings();

  QString name = QString::fromStdString(taskFilePath.getName());
  int r0, r1, step;
  out.getRange(r0, r1, step);

  int sceneFrameCount = scene.getFrameCount();
  if (r0 < 0) r0 = 0;
  if (r1 >= sceneFrameCount)
    r1 = sceneFrameCount - 1;
  else if (r1 < r0)
    r1 = sceneFrameCount - 1;

  r0++, r1++;
  int shrink = rs.m_shrinkX;

  int multimedia       = out.getMultimediaRendering();
  int threadsIndex     = out.getThreadIndex();
  int maxTileSizeIndex = out.getMaxTileSizeIndex();

  TFilePath outputPath =
      scene.decodeFilePath(sprop->getOutputProperties()->getPath());
  if (outputPath.getWideName() == L"")
    outputPath = outputPath.withName(scene.getScenePath().getName());
  int taskChunkSize;
  if (isMovieType(outputPath.getType()))
    taskChunkSize = sceneFrameCount;
  else {
    int size = Preferences::instance()->getDefaultTaskChunkSize();
    if (r1 - r0 + 1 < size)
      taskChunkSize = r1 - r0 + 1;
    else
      taskChunkSize = size;
  }

  QString id = QString::number(TaskCounter++);
  while (BatchesController::instance()->getTask(id))
    id = QString::number(TaskCounter++);

  TFarmTaskGroup *taskGroup = new TFarmTaskGroup(
      id, name, TSystem::getUserName(), TSystem::getHostName(), sceneFrameCount,
      50, taskFilePath, outputPath, r0, r1, step, shrink, multimedia,
      taskChunkSize, threadsIndex, maxTileSizeIndex);

  try {
    BatchesController::instance()->addTask(id, taskGroup);
  } catch (TException &) {
    // TMessage::error(toString(e.getMessage()));
  }
  // m_data->m_scene.setProject( mainprogramProj);
  // TModalPopup::closePopup();
}

//-------------------------------------------------------------------------------------------------------

/*--- id はタスクを追加するごとにインクリメントされる数字 ---*/
void BatchesController::addTask(const QString &id, TFarmTask *task,
                                bool doUpdate) {
  if (task->m_id.isEmpty()) task->m_id = id;
  std::map<QString, TFarmTask*> added;
  collectTasks(task, added);
  for (const auto& entry : added)
    if (m_tasks.count(entry.first))
      throw TException("Task ID already exists in the batch list");
  auto tasks        = m_tasks;
  auto incarnations = m_execution->incarnations;
  for (const auto& entry : added) {
    tasks.emplace(entry);
    incarnations.emplace(entry.first, m_execution->nextIncarnation++);
  }
  auto identities = taskIdentities(incarnations, m_execution->generation);
  for (const auto& entry : added) {
    entry.second->m_status         = Suspended;
    entry.second->m_submissionDate = QDateTime::currentDateTime();
  }
  m_tasks.swap(tasks);
  m_execution->incarnations.swap(incarnations);
  std::atomic_store(&m_execution->progressIdentities, std::move(identities));
  if (doUpdate) update();
}

//------------------------------------------------------------------------------

QString BatchesController::taskBusyStr() {
  return tr(
      "The %1 task is currently active.\nStop it or wait for its completion "
      "before removing it.");
}

//------------------------------------------------------------------------------

namespace {
TFarmTask* activeTask(TFarmTask* task) {
  if (task->m_status == Running || task->m_status == Waiting) return task;
  auto* group = dynamic_cast<TFarmTaskGroup*>(task);
  if (!group) return nullptr;
  for (int i = 0; i < group->getTaskCount(); ++i)
    if (TFarmTask* active = activeTask(group->getTask(i))) return active;
  return nullptr;
}
}  // namespace

void BatchesController::suspendFarmTask(const QString& id) {
  if (!m_controller) return;
  auto found = m_farmIdsTable.find(id);
  if (found != m_farmIdsTable.end()) m_controller->suspendTask(found->second);
}

void BatchesController::removeTask(const QString& id) {
  TFarmTask* task = getTask(id);
  if (!task) return;
  if (!canEditTask(id)) {
    TFarmTask* active = activeTask(task);
    DVGui::warning(taskBusyStr().arg(active ? active->m_name : task->m_name));
    return;
  }
  std::map<QString, TFarmTask*> removed;
  collectTasks(task, removed);
  auto incarnations = m_execution->incarnations;
  for (const auto& entry : removed) incarnations.erase(entry.first);
  auto identities = taskIdentities(incarnations, m_execution->generation);
  if (dynamic_cast<TFarmTaskGroup*>(task)) suspendFarmTask(id);
  for (const auto& entry : m_tasks)
    if (auto* group = dynamic_cast<TFarmTaskGroup*>(entry.second))
      group->removeTask(task);
  for (const auto& entry : removed) {
    m_tasks.erase(entry.first);
    m_farmIdsTable.erase(entry.first);
  }
  m_execution->incarnations.swap(incarnations);
  std::atomic_store(&m_execution->progressIdentities, std::move(identities));
  std::unique_ptr<TFarmTask> owner(task);
  m_dirtyFlag = true;
  update();
}

//------------------------------------------------------------------------------

void BatchesController::releaseTasks() {
  auto identities = taskIdentities({}, m_execution->generation + 1);
  auto owners     = taskOwners(m_tasks);
  m_tasks.clear();
  m_farmIdsTable.clear();
  m_execution->incarnations.clear();
  ++m_execution->generation;
  std::atomic_store(&m_execution->progressIdentities, std::move(identities));
  m_filepath  = TFilePath();
  m_dirtyFlag = false;
  notify();
}

bool BatchesController::tasksAreEditable() const {
  if (!m_execution->records.empty()) return false;
  for (const auto& entry : m_tasks)
    if (activeTask(entry.second)) return false;
  return true;
}

bool BatchesController::canEditTask(const QString& id) const {
  auto* task = getTask(id);
  if (!task || m_execution->closing || activeTask(task)) return false;
  std::map<QString, TFarmTask*> affected;
  collectTasks(task, affected);
  for (const auto& entry : affected) {
    auto incarnation = m_execution->incarnations.find(entry.first);
    if (incarnation != m_execution->incarnations.end() &&
        m_execution->members.count(incarnation->second))
      return false;
  }
  return true;
}

bool BatchesController::setTaskChunkSize(const QString& id, int chunkSize) {
  auto* task = getTask(id);
  return task && setTaskChunkSettings(id, task->m_from, task->m_to, chunkSize,
                                      task->m_outputPath);
}

std::vector<std::unique_ptr<TFarmTask>> BatchesController::replaceChunks(
    TFarmTaskGroup& group, int from, int to, int chunkSize,
    const TFilePath& outputPath) {
  auto children     = prepareChunks(group, from, to, chunkSize, outputPath);
  auto tasks        = m_tasks;
  auto incarnations = m_execution->incarnations;
  std::map<QString, TFarmTask*> removed;
  for (int i = 0; i < group.getTaskCount(); ++i)
    collectTasks(group.getTask(i), removed);
  for (const auto& entry : removed) {
    tasks.erase(entry.first);
    incarnations.erase(entry.first);
  }
  for (const auto& child : children) {
    if (!tasks.emplace(child->m_id, child.get()).second)
      throw TException(
          "A replacement chunk ID is already used by another task");
    incarnations.emplace(child->m_id, m_execution->nextIncarnation++);
  }
  auto identities = taskIdentities(incarnations, m_execution->generation);
  auto retired    = replaceGroupChildren(group, children);
  m_tasks.swap(tasks);
  m_execution->incarnations.swap(incarnations);
  for (const auto& entry : removed) m_farmIdsTable.erase(entry.first);
  std::atomic_store(&m_execution->progressIdentities, std::move(identities));
  return retired;
}

bool BatchesController::setTaskChunkSettings(const QString& id, int from,
                                             int to, int chunkSize,
                                             const TFilePath& outputPath) {
  TFarmTask* task = getTask(id);
  if (!task || !canEditTask(id) || !validChunkRange(from, to, chunkSize))
    return false;
  std::vector<std::unique_ptr<TFarmTask>> retired;
  if (auto* group = dynamic_cast<TFarmTaskGroup*>(task)) {
    if (chunkSettingsChanged(*task, from, to, chunkSize))
      retired = replaceChunks(*group, from, to, chunkSize, outputPath);
    for (int i = 0; i < group->getTaskCount(); ++i)
      group->getTask(i)->m_outputPath = outputPath;
  }
  task->m_from       = from;
  task->m_to         = to;
  task->m_chunkSize  = chunkSize;
  task->m_outputPath = outputPath;
  m_dirtyFlag        = true;
  notify();
  return true;
}

void BatchesController::removeAllTasks() {
  if (!tasksAreEditable()) {
    DVGui::warning(
        tr("Stop submitted tasks and wait for them to finish before clearing "
           "the list."));
    return;
  }
  releaseTasks();
}

//------------------------------------------------------------------------------

int BatchesController::getTaskCount() const { return m_tasks.size(); }

//------------------------------------------------------------------------------

QString BatchesController::getTaskId(int index) const {
  assert(index >= 0 && index < (int)m_tasks.size());
  std::map<QString, TFarmTask *>::const_iterator it = m_tasks.begin();
  std::advance(it, index);
  return it->first;
}

//------------------------------------------------------------------------------

TFarmTask *BatchesController::getTask(int index) const {
  assert(index >= 0 && index < (int)m_tasks.size());
  std::map<QString, TFarmTask *>::const_iterator it = m_tasks.begin();
  std::advance(it, index);
  return it->second;
}

//------------------------------------------------------------------------------

TFarmTask *BatchesController::getTask(const QString &id) const {
  std::map<QString, TFarmTask *>::const_iterator it = m_tasks.find(id);
  if (it != m_tasks.end()) return it->second;
  return 0;
}

//------------------------------------------------------------------------------

bool BatchesController::getTaskInfo(const QString &id, QString &parent,
                                    QString &name, TaskState &status) {
  TFarmTask *task = getTask(id);
  if (task) {
    parent = task->m_parentId;
    name   = task->m_name;
    status = task->m_status;
  }

  return task != 0;
}

//------------------------------------------------------------------------------

TaskState BatchesController::getTaskStatus(const QString &id) const {
  TFarmTask *task = getTask(id);
  if (task)
    return task->m_status;
  else
    return TaskUnknown;
}

void BatchesController::setDirtyFlag(bool state) {
  static bool FirstTime = true;

  if (FirstTime) {
    FirstTime = false;
    bool ret  = connect(TApp::instance()->getMainWindow(), SIGNAL(exit(bool &)),
                        SLOT(onExit(bool &)));
    assert(ret);
  }

  if (state == m_dirtyFlag) return;

  m_dirtyFlag = state;
  update();
}

//------------------------------------------------------------------------------

void BatchesController::getTasks(const QString &parentId,
                                 std::vector<QString> &tasks) const {
  std::map<QString, TFarmTask *>::const_iterator it = m_tasks.begin();
  for (; it != m_tasks.end(); ++it) {
    TFarmTask *task = it->second;
    assert(task);
    if (task->m_parentId == parentId) tasks.push_back(task->m_id);
  }
}

//------------------------------------------------------------------------------

void BatchesController::submitAllTasks() {
  std::multimap<int, TFarmTask*> ordered;
  for (auto it = m_tasks.rbegin(); it != m_tasks.rend(); ++it)
    ordered.emplace(it->second->m_priority, it->second);
  std::vector<std::shared_ptr<Plan>> plans;
  for (auto it = ordered.rbegin(); it != ordered.rend(); ++it)
    if (it->second->m_parentId.isEmpty())
      plans.push_back(captureTask(it->second));
  for (auto& plan : plans) submitTask(plan);
}

void BatchesController::startAll() {
  if (m_controller) {
    for (const auto& entry : m_tasks)
      if (entry.second->m_parentId.isEmpty()) start(entry.first);
    return;
  }
  if (!tasksAreEditable() || m_execution->closing) {
    DVGui::warning(
        tr("Stop submitted tasks and wait for them to finish before starting "
           "the list."));
    return;
  }
  const auto previous = prepareAllTasks(m_tasks);
  try {
    submitAllTasks();
  } catch (...) {
    restorePrepared(*this, *m_execution, previous);
    notify();
    throw;
  }
  notify();
}

//------------------------------------------------------------------------------

void BatchesController::startFarmTask(TFarmTask* task) {
  std::map<QString, QString>::iterator ktor = m_farmIdsTable.find(task->m_id);
  if (ktor != m_farmIdsTable.end()) {
    QString farmTaskId = ktor->second;
    m_controller->restartTask(farmTaskId);

    if (dynamic_cast<TFarmTaskGroup*>(task)) {
      std::vector<QString> subtasks;
      m_controller->getTasks(farmTaskId, subtasks);

      if (!subtasks.empty()) {
        assert((int)subtasks.size() == task->getTaskCount());

        std::vector<QString>::iterator jtor = subtasks.begin();
        for (int i = 0; i < task->getTaskCount(); ++i, ++jtor)
          m_farmIdsTable.insert(std::make_pair(task->getTask(i)->m_id, *jtor));
      }
    }
  } else {
    bool suspended = false;
    QString id     = m_controller->addTask(*task, suspended);
    m_farmIdsTable.insert(std::make_pair(task->m_id, id));

    if (dynamic_cast<TFarmTaskGroup*>(task)) {
      std::vector<QString> subtasks;
      m_controller->getTasks(id, subtasks);
      assert((int)subtasks.size() == task->getTaskCount());

      std::vector<QString>::iterator jtor = subtasks.begin();
      for (int i = 0; i < task->getTaskCount(); ++i, ++jtor)
        m_farmIdsTable.insert(std::make_pair(task->getTask(i)->m_id, *jtor));
    }
  }
}

void BatchesController::start(const QString& taskId) {
  TFarmTask* task = getTask(taskId);
  if (!task) return;
  if (!m_controller && !canEditTask(taskId)) {
    DVGui::warning(taskBusyStr().arg(task->m_name));
    return;
  }
  const auto previous = prepareStart(*task);
  try {
    if (m_controller)
      startFarmTask(task);
    else
      submitTask(captureTask(task));
  } catch (...) {
    restorePrepared(*this, *m_execution, previous);
    notify();
    throw;
  }
  notify();
}

//------------------------------------------------------------------------------

void BatchesController::stop(const QString& taskId) {
  auto* task = getTask(taskId);
  if (!task) return;
  std::map<QString, TFarmTask*> affected;
  collectTasks(task, affected);
  for (const auto& entry : affected)
    if (entry.second->m_status == Waiting) entry.second->m_status = Suspended;
  if (!m_controller) {
    const auto records = m_execution->records;
    for (const auto& entry : records) {
      const auto& plan = *entry.second.plan;
      if (plan.nodes[plan.root].identity.id == taskId) {
        stopSubmission(entry.first);
        continue;
      }
      cancelNodes(entry.second, affected);
    }
  } else {
    suspendFarmTask(taskId);
  }
  notify();
}

void BatchesController::stopAll() {
  if (!m_controller) {
    const auto records = m_execution->records;
    for (const auto& entry : records) stopSubmission(entry.first);
  } else {
    for (const auto& entry : m_farmIdsTable)
      m_controller->suspendTask(entry.second);
  }
  for (const auto& entry : m_tasks)
    if (entry.second->m_status == Waiting) entry.second->m_status = Suspended;
  notify();
}

//------------------------------------------------------------------------------

void BatchesController::onExit(bool& ret) {
  int answer = 0;

  if (m_dirtyFlag)
    answer =
        DVGui::MsgBox(QString(tr("The current task list has been modified.\nDo "
                                 "you want to save your changes?")),
                      tr("Save"), tr("Discard"), tr("Cancel"), 1);

  ret = true;
  if (answer == 3)
    ret = false;
  else if (answer == 1)
    save();
}

//------------------------------------------------------------------------------

void BatchesController::load() {
  static LoadTaskListPopup *popup = 0;

  if (!popup) popup = new LoadTaskListPopup();
  popup->exec();
}

//------------------------------------------------------------------------------
void BatchesController::loadTask(bool isRenderTask) {
  static LoadTaskPopup *popup = new LoadTaskPopup();
  popup->open(isRenderTask);
}

//------------------------------------------------------------------------------

void BatchesController::doLoad(const TFilePath& fp) {
  if (!tasksAreEditable()) {
    DVGui::warning(
        tr("Stop submitted tasks and wait for them to finish before loading "
           "another list."));
    return;
  }
  if (m_dirtyFlag) {
    const int answer =
        DVGui::MsgBox(tr("The current task list has been modified.\nDo you "
                         "want to save your changes?"),
                      tr("Save"), tr("Discard"), tr("Cancel"));
    if (answer == 1)
      save();
    else if (answer == 3 || answer == 0)
      return;
  }
  if (!tasksAreEditable()) {
    DVGui::warning(
        tr("Submitted tasks must finish before replacing the task list."));
    return;
  }
  TaskListReader loaded;
  readTaskList(fp, loaded);
  auto tasks = loaded.tasks();
  std::map<QString, quint64> incarnations;
  for (const auto& entry : tasks) {
    incarnations.emplace(entry.first, m_execution->nextIncarnation++);
    entry.second->m_status         = Suspended;
    entry.second->m_submissionDate = QDateTime::currentDateTime();
  }
  auto identities  = taskIdentities(incarnations, m_execution->generation + 1);
  const auto roots = taskRoots(m_tasks);
  std::vector<std::unique_ptr<TFarmTask>> previousOwners;
  previousOwners.reserve(roots.size());
  m_filepath = fp;
  for (auto* root : roots) previousOwners.emplace_back(root);
  loaded.release();
  m_tasks.swap(tasks);
  m_execution->incarnations.swap(incarnations);
  m_farmIdsTable.clear();
  ++m_execution->generation;
  std::atomic_store(&m_execution->progressIdentities, std::move(identities));
  m_dirtyFlag = false;
  notify();
}

//------------------------------------------------------------------------------

QString BatchesController::getListName() const {
  QString str = (m_filepath == TFilePath()
                     ? tr("Tasks")
                     : QString::fromStdString(m_filepath.getName()));
  return str + (m_dirtyFlag ? "*" : "");
}

//------------------------------------------------------------------------------

void BatchesController::saveas() {
  if (getTaskCount() == 0) {
    DVGui::warning(tr("The Task List is empty!"));
    return;
  }

  static SaveTaskListPopup *popup = 0;
  if (!popup) popup = new SaveTaskListPopup();

  popup->exec();
}

void BatchesController::save() {
  if (m_filepath.isEmpty())
    saveas();
  else
    doSave();
}

void BatchesController::doSave(const TFilePath &_fp) {
  setDirtyFlag(false);

  TFilePath fp;
  if (_fp == TFilePath())
    fp = m_filepath;
  else
    fp = m_filepath = _fp;

  TOStream os(fp);

  std::map<std::string, std::string> attr;
  attr["version"] = "1.0";

  os.openChild("tnzbatches", attr);
  os.child("generator") << TEnv::getApplicationFullName();

  os.openChild("batch");
  std::map<QString, TFarmTask *>::const_iterator it = m_tasks.begin();
  for (; it != m_tasks.end(); ++it) {
    TFarmTask *task = it->second;
    assert(task);
    os << task;
  }
  os.closeChild();  // "batch"
  os.closeChild();  // "tnzbatches"

  update();
}

//------------------------------------------------------------------------------

void BatchesController::attach(BatchesController::Observer *obs) {
  m_observers.insert(obs);
}

//------------------------------------------------------------------------------

void BatchesController::detach(BatchesController::Observer *obs) {
  m_observers.erase(obs);
}

//------------------------------------------------------------------------------

namespace {

void notifyObserver(BatchesController::Observer *obs) { obs->update(); }
}  // namespace

void BatchesController::notify() {
  for (TaskTreeModel *tree : m_tasksTrees) tree->setupModelData();
  std::for_each(m_observers.begin(), m_observers.end(), notifyObserver);
}

//------------------------------------------------------------------------------

void BatchesController::setController(TFarmController *controller) {
  m_controller = controller;
}

//------------------------------------------------------------------------------

TFarmController *BatchesController::getController() const {
  return m_controller;
}

//------------------------------------------------------------------------------

namespace {

class ControllerFailureMsg final : public TThread::Message {
public:
  ControllerFailureMsg(const TException &e) : m_e(e) {}

  void onDeliver() override {
    // throw m_e;
  }

  TThread::Message *clone() const override {
    return new ControllerFailureMsg(m_e);
  }

  TException m_e;
};

}  // namespace

//------------------------------------------------------------------------------

void BatchesController::update() {
  if (m_controller) {
    try {
      std::map<QString, QString>::iterator it = m_farmIdsTable.begin();
      for (; it != m_farmIdsTable.end(); ++it) {
        QString farmTaskId    = it->second;
        QString batchesTaskId = it->first;

        TFarmTask *batchesTask = getTask(batchesTaskId);

        if (batchesTask) {
          TFarmTask farmTask          = *batchesTask;
          QString batchesTaskParentId = batchesTask->m_parentId;
          m_controller->queryTaskInfo(farmTaskId, farmTask);
          int chunkSize            = batchesTask->m_chunkSize;
          *batchesTask             = farmTask;
          batchesTask->m_chunkSize = chunkSize;
          batchesTask->m_id        = batchesTaskId;
          batchesTask->m_parentId  = batchesTaskParentId;
        }
      }
    } catch (TException &e) {
      ControllerFailureMsg(e).send();
    }
  }

  std::map<QString, TFarmTask *>::iterator jtor = m_tasks.begin();
  for (; jtor != m_tasks.end(); ++jtor) {
    TFarmTask *task = jtor->second;
    assert(task);
    if (task->m_status == Completed) m_farmIdsTable.erase(task->m_id);
  }

  notify();
}

//------------------------------------------------------------------------------

BatchesController *BatchesController::m_instance;

//------------------------------------------------------------------------------
//------------------------------------------------------------------------------

BatchesController::Observer::~Observer() {}

//------------------------------------------------------------------------------
//------------------------------------------------------------------------------
//------------------------------------------------------------------------------
//------------------------------------------------------------------------------

namespace {

class MyLocalController final : public TFarmController, public TFarmExecutor {
public:
  MyLocalController(int port) : TFarmExecutor(port) {}

  QString execute(const std::vector<QString> &argv) override;

  QString addTask(const TFarmTask &task, bool suspended) override;
  void removeTask(const QString &id) override;
  void suspendTask(const QString &id) override;
  void activateTask(const QString &id) override;
  void restartTask(const QString &id) override;

  void getTasks(std::vector<QString> &tasks) override;
  void getTasks(const QString &parentId, std::vector<QString> &tasks) override;
  void getTasks(const QString &parentId,
                std::vector<TaskShortInfo> &tasks) override;

  void queryTaskInfo(const QString &id, TFarmTask &task) override;

  void queryTaskShortInfo(const QString &id, QString &parentId, QString &name,
                          TaskState &status) override;

  void attachServer(const QString &name, const QString &addr,
                    int port) override {
    assert(false);
  }

  void detachServer(const QString &name, const QString &addr,
                    int port) override {
    assert(false);
  }

  void taskSubmissionError(const QString &taskId, int errCode) override {
    assert(false);
  }

  void taskProgress(const QString &taskId, int step, int stepCount,
                    int frameNumber, FrameState state) override;

  void taskCompleted(const QString &taskId, int exitCode) override;

  void getServers(std::vector<ServerIdentity> &servers) override {
    assert(false);
  }

  ServerState queryServerState2(const QString &id) override {
    assert(false);
    return ServerUnknown;
  }

  void queryServerInfo(const QString &id, ServerInfo &info) override {
    assert(false);
  }

  void activateServer(const QString &id) override { assert(false); }

  void deactivateServer(const QString &id, bool completeRunningTasks) override {
    assert(false);
  }

private:
  std::map<QString, TFarmTask> m_tasks;
};

QString MyLocalController::execute(const std::vector<QString> &argv) {
  if (argv.size() > 5 && argv[0] == "taskProgress") {
    int step, stepCount, frameNumber;

    fromStr(step, argv[2]);
    fromStr(stepCount, argv[3]);
    fromStr(frameNumber, argv[4]);

    FrameState state;
    fromStr((int &)state, argv[5]);
    taskProgress(argv[1], step, stepCount, frameNumber, state);
    return "";
  } else if (argv.size() > 2 && argv[0] == "taskCompleted") {
    QString taskId = argv[1];
    int exitCode;
    fromStr(exitCode, argv[2]);

    taskCompleted(taskId, exitCode);
    return "";
  }

  return "";
}

QString MyLocalController::addTask(const TFarmTask &task, bool suspended) {
  assert(false);
  return "";
}

void MyLocalController::removeTask(const QString &id) { assert(false); }

void MyLocalController::suspendTask(const QString &id) { assert(false); }

void MyLocalController::activateTask(const QString &id) { assert(false); }

void MyLocalController::restartTask(const QString &id) { assert(false); }

void MyLocalController::getTasks(std::vector<QString> &tasks) { assert(false); }

void MyLocalController::getTasks(const QString &parentId,
                                 std::vector<QString> &tasks) {
  assert(false);
}

void MyLocalController::getTasks(const QString &parentId,
                                 std::vector<TaskShortInfo> &tasks) {
  assert(false);
}

void MyLocalController::queryTaskInfo(const QString &id, TFarmTask &task) {
  assert(false);
}

void MyLocalController::queryTaskShortInfo(const QString &id, QString &parentId,
                                           QString &name, TaskState &status) {
  assert(false);
}

void MyLocalController::taskProgress(const QString &taskId, int step,
                                     int stepCount, int frameNumber,
                                     FrameState state) {
  BatchesController::instance()->postTaskProgress(taskId, state == FrameDone);
}

void MyLocalController::taskCompleted(const QString &taskId, int exitCode) {}

class MyLocalControllerController final : public TThread::Runnable {
  MyLocalController *m_controller;

public:
  MyLocalControllerController(int port)
      : m_controller(new MyLocalController(port)) {
    TThread::Executor executor;
    executor.addTask(this);
    connect(this, SIGNAL(finished(TThread::RunnableP)), this,
            SLOT(onFinished(TThread::RunnableP)));
    connect(this, SIGNAL(exception(TThread::RunnableP)), this,
            SLOT(onFinished(TThread::RunnableP)));
  }

  void run() override { m_controller->run(); }

public slots:

  void onFinished(TThread::RunnableP thisTask) override {
    BatchesController::instance()->notify();
  }
};

}  // anonymous namespace

//------------------------------------------------------------------------------
//------------------------------------------------------------------------------

BatchesController *BatchesController::instance() {
  if (!m_instance) {
    m_instance = new BatchesController;

    // scandisce un range di porte fino a troverne una libera da utilizzare come
    // porta del local controller

    const int portRangeSize = 100;

    int i          = 0;
    int portNumber = cPortNumber;
    for (; i < portRangeSize; ++i, ++portNumber) {
      bool portBusy = TFarmStuff::testConnection("localhost", portNumber);
      if (!portBusy) break;
    }

    if (i < portRangeSize) {
      m_instance->m_localControllerPortNumber = portNumber;

      //      static MyLocalControllerController *TheLocalController =
      new MyLocalControllerController(portNumber);
    }
  }
  return m_instance;
}
