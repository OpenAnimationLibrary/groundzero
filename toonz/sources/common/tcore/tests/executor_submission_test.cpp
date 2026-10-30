#include "tthread.h"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QPointer>
#include <QSemaphore>
#include <QThreadStorage>

#include <atomic>
#include <cstdlib>
#include <exception>
#include <iostream>
#include <stdexcept>

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

struct RetirementState {
  QSemaphore entered, release, storageEntered, storageRelease;
  std::atomic<int> finished{0}, canceled{0}, storageDestroyed{0};
  std::atomic<QThread *> worker{nullptr};
};

struct RetirementStorage {
  RetirementState &state;
  explicit RetirementStorage(RetirementState &value) : state(value) {}
  ~RetirementStorage() {
    state.storageEntered.release();
    state.storageRelease.acquire();
    ++state.storageDestroyed;
  }
};

class RetirementRunnable final : public TThread::Runnable {
  RetirementState &m_state;
  QThreadStorage<RetirementStorage *> &m_storage;

public:
  RetirementRunnable(RetirementState &state,
                     QThreadStorage<RetirementStorage *> &storage)
      : m_state(state), m_storage(storage) {
    QObject::connect(this, &TThread::Runnable::finished, this,
                     [this](TThread::RunnableP) { ++m_state.finished; },
                     Qt::DirectConnection);
    QObject::connect(this, &TThread::Runnable::canceled, this,
                     [this](TThread::RunnableP) { ++m_state.canceled; },
                     Qt::DirectConnection);
  }
  void run() override {
    m_state.worker = QThread::currentThread();
    m_storage.setLocalData(new RetirementStorage(m_state));
    m_state.entered.release();
    m_state.release.acquire();
  }
};

void waitForRetirement(TThread::Executor &executor) {
  QElapsedTimer deadline;
  deadline.start();
  while (!executor.isRetired() && deadline.elapsed() < 5000) {
    QCoreApplication::processEvents();
    QThread::msleep(1);
  }
  require(executor.isRetired(), "dedicated executor did not retire its workers");
}

void retirementCancelsQueueAndWaitsForThreadStorage() {
  TThread::Executor executor, unrelated;
  executor.setDedicatedThreads(true);
  unrelated.setDedicatedThreads(true);
  RetirementState live;
  QThreadStorage<RetirementStorage *> storage;
  TThread::RunnableP active = new RetirementRunnable(live, storage);
  executor.addTask(active);
  require(live.entered.tryAcquire(1, 5000), "retirement task did not start");
  QPointer<QThread> worker(live.worker.load());
  QObject observer;
  bool destroyedCallback = false, retiredAtDestruction = false;
  QObject::connect(worker.data(), &QObject::destroyed, &observer, [&]() {
    destroyedCallback = true;
    retiredAtDestruction = executor.isRetired();
  }, Qt::DirectConnection);

  State pending, external;
  TThread::RunnableP queued = new ProbeRunnable(pending, Behavior::Normal);
  TThread::RunnableP other = new ProbeRunnable(external, Behavior::Gated);
  executor.addTask(queued);
  unrelated.addTask(other);
  require(external.entered.tryAcquire(1, 5000), "unrelated task did not start");
  executor.beginRetirement();
  executor.beginRetirement();
  require(!executor.isRetired() && live.canceled == 1 && pending.canceled == 1 &&
              pending.runs == 0 && external.canceled == 0,
          "retirement missed queue cancellation, repeated it, or touched another executor");

  State rejectedState;
  TThread::RunnableP rejected = new ProbeRunnable(rejectedState, Behavior::Normal);
  bool accepted = true, rejectedAdmission = false;
  try {
    executor.addTask(rejected, accepted);
  } catch (const std::runtime_error &) {
    rejectedAdmission = true;
  }
  require(rejectedAdmission && !accepted &&
              !executor.withdrawQueuedTask(rejected),
          "retired executor accepted a new task");
  checkNotExecuted(rejectedState);

  live.release.release();
  require(live.storageEntered.tryAcquire(1, 5000),
          "retiring worker did not reach its actual QThreadStorage destructor");
  // Runnable::finished and QThread::finished have both been emitted, but Qt's
  // storage cleanup is deliberately still blocked. Neither signal proves drain.
  QCoreApplication::processEvents();
  require(live.finished == 1 && live.storageDestroyed == 0 &&
              !executor.isRetired(),
          "executor reported retirement before thread-local cleanup completed");
  live.storageRelease.release();
  waitForRetirement(executor);
  require(live.storageDestroyed == 1 && pending.runs == 0 &&
              external.runs == 1 && external.finished == 0 && external.canceled == 0,
          "retirement lost TLS cleanup or interfered with unrelated work");
  require(destroyedCallback && retiredAtDestruction && !worker,
          "worker QObject destruction observed an incomplete or locked retirement");
  executor.beginRetirement();
  require(executor.isRetired(), "repeated retirement revived a worker");

  external.release.release();
  require(external.completed.tryAcquire(1, 5000),
          "unrelated executor stopped completing work after retirement");
  unrelated.beginRetirement();
  waitForRetirement(unrelated);
}

void retirementRejectsNonDedicatedHistory() {
  TThread::Executor executor;
  bool refused = false;
  try {
    executor.beginRetirement();
  } catch (const std::logic_error &) {
    refused = true;
  }
  require(refused && !executor.isRetired(),
          "non-dedicated executor promised private worker retirement");
  // A rejected precondition must leave ordinary submission usable.
  executor.setDedicatedThreads(true);
  State state;
  TThread::RunnableP task = new ProbeRunnable(state, Behavior::Gated);
  executor.addTask(task);
  require(state.entered.tryAcquire(1, 5000), "precondition failure closed admission");
  executor.setDedicatedThreads(false);
  executor.setDedicatedThreads(true);
  refused = false;
  try {
    executor.beginRetirement();
  } catch (const std::logic_error &) {
    refused = true;
  }
  require(refused && state.canceled == 0,
          "retirement accepted a worker that could have served unrelated work");
  // This executor no longer has a dedicated-history guarantee. Retire its
  // ordinary worker through the existing nonpersistent path instead.
  executor.setDedicatedThreads(true, false);
  task = TThread::RunnableP();
  state.release.release();
  require(state.deleted.tryAcquire(1, 5000),
          "dedication-history fixture retained its completed task");
  QCoreApplication::processEvents();
}

}  // namespace

int main(int argc, char **argv) {
  QCoreApplication application(argc, argv);
  TThread::init();
  failureBeforeAndAfterPublication();
  failedSubmissionPreservesLiveTask();
  retirementCancelsQueueAndWaitsForThreadStorage();
  retirementRejectsNonDedicatedHistory();
  TThread::shutdown();
  std::cout << "executor submission ownership and retirement checks passed\n";
  return 0;
}
