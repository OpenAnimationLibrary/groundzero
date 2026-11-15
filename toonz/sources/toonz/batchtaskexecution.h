#pragma once

#include "tthread.h"

#include <QDateTime>
#include <QSemaphore>
#include <QStringList>
#include <atomic>
#include <functional>
#include <memory>
#include <vector>

class QProcess;

namespace BatchTaskExecution {

struct Identity {
  QString id;
  quint64 generation  = 0;
  quint64 incarnation = 0;
};

struct Command {
  QString taskId;
  QString program;
  QStringList arguments;
};

struct ProcessResult {
  int exitCode  = 0;
  bool failed   = false;
  bool canceled = false;
};

#ifdef BUILD_UI_PORT_TESTS
struct TestHooks {
  std::function<void()> runEntered;
  std::function<void()> terminalPublished;
  std::function<void(std::function<void()>)> terminalPosted;
  std::function<void(Command&)> prepareCommand;
  std::function<void(qint64)> processStarted;
  std::function<void(bool)> processStopped;
  std::function<void(QProcess&)> processPollTimedOut;
  std::function<ProcessResult(const Command&, const std::function<bool()>&)>
      execute;
  std::function<void(quint64)> retired;
  std::function<void(quint64)> destroyed;
  bool throwBeforeAcceptance = false;
  bool throwAfterAcceptance  = false;
};
#endif

struct Node {
  Identity identity;
  Command command;
  std::vector<size_t> dependencies;
  std::vector<size_t> children;
  int frameCount = 0;
  int stepCount  = 0;
  bool eligible  = false;
};

struct Plan {
  quint64 submission = 0;
  size_t root        = 0;
  std::vector<Node> nodes;
  std::vector<Identity> affected;
#ifdef BUILD_UI_PORT_TESTS
  std::shared_ptr<TestHooks> hooks;
#endif
};

struct Control {
  explicit Control(size_t count);
  bool isCanceled(size_t node) const;
  void complete();
  void wait();

  std::atomic<bool> canceled{false};
  std::vector<std::shared_ptr<std::atomic<bool>>> nodes;
  std::atomic<bool> finished{false};
  QSemaphore completion;
};

struct Outcome {
  enum Kind {
    WaitingForDependencies,
    Started,
    ProcessFinished,
    GroupFinished
  } kind      = Started;
  size_t node = 0;
  QDateTime time;
  QString host;
  ProcessResult process;
};

TThread::RunnableP makeRunner(std::shared_ptr<const Plan> plan,
                              std::shared_ptr<Control> control,
                              std::function<void(const Outcome&)> publish);

}  // namespace BatchTaskExecution
