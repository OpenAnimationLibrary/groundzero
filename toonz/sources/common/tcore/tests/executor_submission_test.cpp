#include "tthread.h"

#include <QCoreApplication>
#include <QSemaphore>

#include <atomic>
#include <cstdlib>
#include <exception>
#include <iostream>

namespace {

void require(bool condition, const char *message) {
  if (condition) return;
  std::cerr << message << '\n';
  std::exit(1);
}

class SubmissionFailure final : public std::exception {};

struct State {
  std::atomic<int> priorities{0};
  std::atomic<int> runs{0};
  std::atomic<int> finished{0};
  std::atomic<int> canceled{0};
  std::atomic<int> exceptions{0};
  std::atomic<int> destroyed{0};
  QSemaphore entered;
  QSemaphore release;
  QSemaphore completed;
  QSemaphore deleted;
};

enum class Behavior { Normal, FailPriority, FailLoad, Gated };

class ProbeRunnable final : public TThread::Runnable {
  State &m_state;
  Behavior m_behavior;

public:
  ProbeRunnable(State &state, Behavior behavior)
      : m_state(state), m_behavior(behavior) {
    QObject::connect(this, &TThread::Runnable::finished, this,
                     [this](TThread::RunnableP) {
                       ++m_state.finished;
                       m_state.completed.release();
                     },
                     Qt::DirectConnection);
    QObject::connect(this, &TThread::Runnable::canceled, this,
                     [this](TThread::RunnableP) { ++m_state.canceled; },
                     Qt::DirectConnection);
    QObject::connect(this, &TThread::Runnable::exception, this,
                     [this](TThread::RunnableP) { ++m_state.exceptions; },
                     Qt::DirectConnection);
  }

  ~ProbeRunnable() override {
    ++m_state.destroyed;
    m_state.deleted.release();
  }

  int schedulingPriority() override {
    ++m_state.priorities;
    if (m_behavior == Behavior::FailPriority) throw SubmissionFailure();
    return 5;
  }

  int taskLoad() override {
    if (m_behavior == Behavior::FailLoad) throw SubmissionFailure();
    return 0;
  }

  void run() override {
    ++m_state.runs;
    if (m_behavior == Behavior::Gated) {
      m_state.entered.release();
      m_state.release.acquire();
    }
  }
};

template <class F>
void fails(F operation) {
  try {
    operation();
    require(false, "submission failure did not propagate to its caller");
  } catch (const SubmissionFailure &) {
  }
}

void checkNotExecuted(const State &state) {
  require(state.runs == 0 && state.finished == 0 && state.canceled == 0 &&
              state.exceptions == 0,
          "unpublished or withdrawn task executed or emitted a callback");
}

void failureBeforeAndAfterPublication() {
  TThread::Executor executor;
  State before;
  TThread::RunnableP rejected =
      new ProbeRunnable(before, Behavior::FailPriority);
  bool accepted = true;
  fails([&] { executor.addTask(rejected, accepted); });
  require(!accepted, "priority failure was reported as accepted");
  require(!executor.withdrawQueuedTask(rejected),
          "unpublished task appeared in the queue");
  require(before.priorities == 1, "submission evaluated priority twice");
  rejected = TThread::RunnableP();
  require(before.destroyed == 1, "rejected task retained executor ownership");
  checkNotExecuted(before);

  State after;
  TThread::RunnableP queued = new ProbeRunnable(after, Behavior::FailLoad);
  accepted                 = false;
  fails([&] { executor.addTask(queued, accepted); });
  require(accepted, "refresh failure hid the already-published task");
  require(executor.withdrawQueuedTask(queued),
          "refresh failure lost the queued task");
  require(!executor.withdrawQueuedTask(queued),
          "withdrawal removed the same task twice");
  require(after.priorities == 1, "refresh failure reevaluated priority");
  queued = TThread::RunnableP();
  require(after.destroyed == 1, "withdrawn task retained queue ownership");
  QCoreApplication::processEvents();
  checkNotExecuted(after);

  State legacy;
  TThread::RunnableP oldApi = new ProbeRunnable(legacy, Behavior::FailLoad);
  fails([&] { executor.addTask(oldApi); });
  require(executor.withdrawQueuedTask(oldApi),
          "legacy overload swallowed publication or scheduling failure");
  oldApi = TThread::RunnableP();
  require(legacy.destroyed == 1, "legacy submission retained a withdrawn task");
  checkNotExecuted(legacy);
}

void failedSubmissionPreservesLiveTask() {
  TThread::Executor executor;
  executor.setMaxActiveTasks(1);
  State live;
  TThread::RunnableP active = new ProbeRunnable(live, Behavior::Gated);
  bool accepted            = false;
  executor.addTask(active, accepted);
  require(accepted, "live task was not accepted");
  require(live.entered.tryAcquire(1, 5000), "accepted task did not start");
  require(!executor.withdrawQueuedTask(active),
          "queued-only withdrawal removed an active task");
  active = TThread::RunnableP();

  State before;
  TThread::RunnableP rejected =
      new ProbeRunnable(before, Behavior::FailPriority);
  accepted = true;
  fails([&] { executor.addTask(rejected, accepted); });
  require(!accepted, "failed priority inherited live task acceptance");
  rejected = TThread::RunnableP();
  checkNotExecuted(before);

  State after;
  TThread::RunnableP failed = new ProbeRunnable(after, Behavior::FailLoad);
  accepted                 = false;
  fails([&] { executor.addTask(failed, accepted); });
  require(accepted, "failed refresh forgot its queued task");
  require(executor.withdrawQueuedTask(failed),
          "failed refresh task could not be withdrawn");
  failed = TThread::RunnableP();
  checkNotExecuted(after);

  State pending;
  TThread::RunnableP queued = new ProbeRunnable(pending, Behavior::Normal);
  executor.addTask(queued);
  TThread::Executor unrelated;
  require(!unrelated.withdrawQueuedTask(queued),
          "another executor withdrew a task it does not own");
  require(executor.withdrawQueuedTask(queued),
          "normally queued task was not withdrawn");
  queued = TThread::RunnableP();
  require(pending.destroyed == 1, "withdrawn normal task retained ownership");
  checkNotExecuted(pending);

  require(live.runs == 1 && live.finished == 0 && live.canceled == 0 &&
              live.exceptions == 0 && live.destroyed == 0,
          "failed submission altered or destroyed a live task");
  live.release.release();
  require(live.completed.tryAcquire(1, 5000),
          "live task failed to complete after another submission failed");
  require(live.deleted.tryAcquire(1, 5000),
          "completed live task retained executor ownership");
  QCoreApplication::processEvents();
  require(live.runs == 1 && live.finished == 1 && live.canceled == 0 &&
              live.exceptions == 0 && live.destroyed == 1,
          "live task did not execute and complete exactly once");
  checkNotExecuted(pending);
}

}  // namespace

int main(int argc, char **argv) {
  QCoreApplication application(argc, argv);
  TThread::init();
  failureBeforeAndAfterPublication();
  failedSubmissionPreservesLiveTask();
  TThread::shutdown();
  std::cout << "executor submission ownership checks passed\n";
  return 0;
}
