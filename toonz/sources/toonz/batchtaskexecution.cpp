#include "batchtaskexecution.h"

#include "tsystem.h"

#include <QProcess>
#include <stdexcept>

namespace BatchTaskExecution {

Control::Control(size_t count) {
  for (size_t i = 0; i < count; ++i)
    nodes.push_back(std::make_shared<std::atomic<bool>>(false));
}

bool Control::isCanceled(size_t node) const {
  return canceled.load() || nodes[node]->load();
}

void Control::complete() {
  if (!finished.exchange(true)) completion.release();
}

void Control::wait() {
  if (!finished.load()) completion.acquire();
}

namespace {

bool processFailed(const QProcess& process, bool ioFailed) {
  const auto error = process.error();
  return ioFailed || process.exitStatus() != QProcess::NormalExit ||
         (error != QProcess::UnknownError && error != QProcess::Timedout);
}

#ifdef BUILD_UI_PORT_TESTS
void reportProcessStarted(const Plan& plan, QProcess& process, bool& reported) {
  if (reported || process.state() != QProcess::Running) return;
  reported = true;
  if (plan.hooks && plan.hooks->processStarted)
    plan.hooks->processStarted(process.processId());
}

void reportProcessTimeout(const Plan& plan, QProcess& process) {
  if (process.state() == QProcess::Running &&
      process.error() == QProcess::Timedout && plan.hooks &&
      plan.hooks->processPollTimedOut)
    plan.hooks->processPollTimedOut(process);
}

void reportProcessStopped(const Plan& plan, QProcess& process) {
  if (plan.hooks && plan.hooks->processStopped)
    plan.hooks->processStopped(process.state() == QProcess::NotRunning);
}
#endif

ProcessResult executeProcess(const Command& command,
                             const std::function<bool()>& canceled,
                             const Plan& plan) {
  if (canceled()) return {0, false, true};
  bool ioFailed = false;
  QProcess process;
  QObject::connect(
      &process, &QProcess::errorOccurred, &process,
      [&ioFailed](QProcess::ProcessError error) {
        if (error != QProcess::Timedout && error != QProcess::UnknownError)
          ioFailed = true;
      });
  process.setProgram(command.program);
  process.setArguments(command.arguments);
  process.start();
  bool stopped = false;
#ifdef BUILD_UI_PORT_TESTS
  bool reportedStarted = false;
#else
  (void)plan;
#endif
  while (process.state() != QProcess::NotRunning) {
#ifdef BUILD_UI_PORT_TESTS
    reportProcessStarted(plan, process, reportedStarted);
#endif
    if (canceled()) {
      stopped = true;
      process.kill();
    }
    if (process.waitForFinished(50)) break;
#ifdef BUILD_UI_PORT_TESTS
    reportProcessTimeout(plan, process);
#endif
  }
  // A polling timeout is not a failed command after the process has exited.
  const bool failed = processFailed(process, ioFailed);
#ifdef BUILD_UI_PORT_TESTS
  reportProcessStopped(plan, process);
#endif
  return {process.exitCode(), failed, stopped};
}

class Runner final : public TThread::Runnable {
  std::shared_ptr<const Plan> m_plan;
  std::shared_ptr<Control> m_control;
  std::function<void(const Outcome&)> m_publish;

  void publish(Outcome::Kind kind, size_t node, ProcessResult process = {}) {
    Outcome outcome;
    outcome.kind    = kind;
    outcome.node    = node;
    outcome.time    = QDateTime::currentDateTime();
    outcome.host    = TSystem::getHostName();
    outcome.process = process;
    m_publish(outcome);
  }

  void execute(size_t index) {
    const Node& node = m_plan->nodes[index];
    if (!node.eligible || m_control->isCanceled(index)) return;
    if (!node.dependencies.empty())
      publish(Outcome::WaitingForDependencies, index);
    for (size_t dependency : node.dependencies) execute(dependency);
    if (m_control->isCanceled(index)) return;
    publish(Outcome::Started, index);
    if (!node.children.empty()) {
      executeChildren(index);
      return;
    }
    executeCommand(index);
  }

  void executeChildren(size_t index) {
    for (size_t child : m_plan->nodes[index].children) {
      if (m_control->isCanceled(index)) break;
      execute(child);
    }
    publish(Outcome::GroupFinished, index,
            {0, false, m_control->isCanceled(index)});
  }

  void executeCommand(size_t index) {
    const auto canceled = [control = m_control, index] {
      return control->isCanceled(index);
    };
    ProcessResult result;
    Command command = m_plan->nodes[index].command;
#ifdef BUILD_UI_PORT_TESTS
    if (m_plan->hooks && m_plan->hooks->prepareCommand)
      m_plan->hooks->prepareCommand(command);
    if (m_plan->hooks && m_plan->hooks->execute)
      result = m_plan->hooks->execute(command, canceled);
    else
#endif
      result = executeProcess(command, canceled, *m_plan);
    publish(Outcome::ProcessFinished, index, result);
  }

public:
  Runner(std::shared_ptr<const Plan> plan, std::shared_ptr<Control> control,
         std::function<void(const Outcome&)> publish)
      : m_plan(std::move(plan))
      , m_control(std::move(control))
      , m_publish(std::move(publish)) {}

  ~Runner() override {
#ifdef BUILD_UI_PORT_TESTS
    if (m_plan->hooks && m_plan->hooks->destroyed)
      m_plan->hooks->destroyed(m_plan->submission);
#endif
  }

  void run() override {
#ifdef BUILD_UI_PORT_TESTS
    if (m_plan->hooks && m_plan->hooks->runEntered) m_plan->hooks->runEntered();
#endif
    execute(m_plan->root);
#ifdef BUILD_UI_PORT_TESTS
    if (m_plan->hooks && m_plan->hooks->terminalPublished)
      m_plan->hooks->terminalPublished();
#endif
  }

  int schedulingPriority() override {
#ifdef BUILD_UI_PORT_TESTS
    if (m_plan->hooks && m_plan->hooks->throwBeforeAcceptance)
      throw std::runtime_error(
          "Batch fixture: failure before queue publication");
#endif
    return TThread::Runnable::schedulingPriority();
  }

  int taskLoad() override {
#ifdef BUILD_UI_PORT_TESTS
    if (m_plan->hooks && m_plan->hooks->throwAfterAcceptance)
      throw std::runtime_error(
          "Batch fixture: failure after queue publication");
#endif
    return 0;
  }
};

}  // namespace

TThread::RunnableP makeRunner(std::shared_ptr<const Plan> plan,
                              std::shared_ptr<Control> control,
                              std::function<void(const Outcome&)> publish) {
  return new Runner(std::move(plan), std::move(control), std::move(publish));
}

}  // namespace BatchTaskExecution
