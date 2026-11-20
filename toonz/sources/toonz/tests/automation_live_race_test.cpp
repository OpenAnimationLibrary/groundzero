#include "automation_live_protocol.h"
#include "automation_live_state.h"

#include <QCoreApplication>

#include <array>
#include <atomic>
#include <chrono>
#include <functional>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <vector>

namespace GroundzeroAutomation {
namespace Live {
namespace {

const QString instance = "00000000-0000-4000-8000-000000000002";

void require(bool condition, const char* message) {
  if (!condition) throw std::runtime_error(message);
}

QString operationId(int value) {
  return "00000000-0000-4000-8000-" +
         QString::number(value, 16).rightJustified(12, '0');
}

QJsonObject request(int value) {
  return {{"id", "concurrent"},
          {"method", "live.stage.set_key_value"},
          {"params", QJsonObject{{"instance_id", instance},
                                 {"scene_epoch", "1"},
                                 {"operation_id", operationId(value)},
                                 {"expected_fingerprint", QString(64, '0')},
                                 {"object_id", "concurrent_target"},
                                 {"channel", "x"},
                                 {"frame", 0},
                                 {"value", 1},
                                 {"unit", "inch"}}}};
}

QJsonObject outcome(const std::shared_ptr<Ticket>& ticket) {
  return {{"result", QJsonObject{{"instance_id", instance},
                                 {"operation_id", ticket->operationId},
                                 {"state", "completed"},
                                 {"changed", true}}}};
}

bool waitFor(const std::function<bool()>& predicate) {
  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(10);
  while (std::chrono::steady_clock::now() < deadline) {
    if (predicate()) return true;
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  return predicate();
}

class Threads final {
public:
  ~Threads() {
    start();
    joinAll();
  }
  void add(std::function<void()> operation) {
    m_threads.emplace_back([this, operation]() {
      while (!m_started.load()) std::this_thread::yield();
      try {
        operation();
      } catch (...) {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (!m_failure) m_failure = std::current_exception();
      }
    });
  }
  void start() { m_started.store(true); }
  void join() {
    joinAll();
    if (m_failure) std::rethrow_exception(m_failure);
  }

private:
  void joinAll() {
    for (auto& thread : m_threads)
      if (thread.joinable()) thread.join();
  }
  std::atomic<bool> m_started{false};
  std::mutex m_mutex;
  std::exception_ptr m_failure;
  std::vector<std::thread> m_threads;
};

class IncrementOnExit final {
public:
  explicit IncrementOnExit(std::atomic<int>& counter) : m_counter(counter) {}
  ~IncrementOnExit() { ++m_counter; }

private:
  std::atomic<int>& m_counter;
};

struct Load final {
  SharedState state{instance, "edit"};
  std::atomic<bool> seedStarted{false};
  std::atomic<bool> release{false};
  std::atomic<bool> done{false};
  std::atomic<int> prepared{0};
  std::atomic<int> finished{0};
  std::array<std::atomic<int>, 2> accepted{{0, 0}};
  std::atomic<int> polls{0};
  QHash<QString, int> commits;
};

void complete(Load& load, const std::shared_ptr<Ticket>& ticket) {
  ++load.commits[ticket->operationId];
  std::this_thread::yield();
  load.state.finish(ticket, outcome(ticket));
}

void consume(Load& load) {
  const auto seed = load.state.takeNext();
  require(bool(seed), "The concurrent consumer could not begin its seed edit");
  load.seedStarted.store(true);
  require(waitFor([&]() { return load.release.load(); }),
          "The seed edit was never released");
  complete(load, seed);
  while (!load.done.load()) {
    const auto ticket = load.state.takeNext();
    if (ticket)
      complete(load, ticket);
    else
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  while (const auto ticket = load.state.takeNext()) complete(load, ticket);
}

void fillHeldQueue(Load& load, int producer) {
  const QString client = producer % 2 ? "client-b" : "client-a";
  const auto duplicate = load.state.admit(client, request(1), true);
  require(duplicate.immediate.value("result").toObject().value("state") ==
              "pending",
          "A duplicate operation bypassed the started receipt");
  for (int index = 0; index < 16; ++index) {
    const auto admitted =
        load.state.admit(client, request(1000 + producer * 16 + index), true);
    if (admitted.ticket)
      ++load.accepted[producer % 2];
    else
      require(admitted.immediate.value("error").toObject().value("code") ==
                  "resource_limit",
              "The held concurrent queue returned an unexpected rejection");
  }
}

void conflictingReplay(SharedState& state, QJsonObject call) {
  QJsonObject params = call.value("params").toObject();
  params.insert("value", 2);
  call.insert("params", params);
  try {
    state.admit("client-b", call, true);
  } catch (const Error& error) {
    require(error.code == "operation_conflict",
            "A concurrent conflict changed its error");
    return;
  }
  throw std::runtime_error(
      "Concurrent arguments reused an admitted operation UUID");
}

void replayLoad(Load& load, int producer) {
  const QString client = producer % 2 ? "client-b" : "client-a";
  for (int index = 0; index < 500; ++index) {
    const int number       = 2000 + (index + producer * 37) % 256;
    const QJsonObject call = request(number);
    const auto admitted    = load.state.admit(client, call, true);
    if (admitted.ticket) load.state.poll(admitted.ticket);
    const QJsonObject read{
        {"id", "poll"},
        {"method", "live.receipt"},
        {"params", QJsonObject{{"operation_id", operationId(number)}}}};
    const auto receipt = load.state.admit(client, read, false)
                             .immediate.value("result")
                             .toObject();
    require(receipt.value("state") != "unknown",
            "An admitted concurrent receipt disappeared");
    if (index % 16 == 0) conflictingReplay(load.state, call);
    ++load.polls;
  }
}

void produce(Load& load, int index) {
  IncrementOnExit finished(load.finished);
  {
    IncrementOnExit prepared(load.prepared);
    require(waitFor([&]() { return load.seedStarted.load(); }),
            "The seed edit did not begin");
    fillHeldQueue(load, index);
  }
  require(waitFor([&]() { return load.release.load(); }),
          "The concurrent load was not released");
  replayLoad(load, index);
}

void verifyTerminal(Load& load, int number) {
  const QJsonObject first =
      load.state.admit("client-a", request(number), false).immediate;
  require(
      first == load.state.admit("client-b", request(number), false).immediate,
      "Two clients did not share the same final receipt");
  const int commits = load.commits.value(operationId(number));
  if (first.contains("result")) {
    require(first.value("result").toObject().value("state") == "completed",
            "The consumer left a receipt pending");
    require(commits == 1, "A completed UUID was not committed exactly once");
  } else
    require(commits == 0, "A rejected UUID was also committed");
}

void concurrentClients() {
  Load load;
  require(bool(load.state.admit("client-a", request(1), true).ticket),
          "Seed edit was rejected");
  Threads threads;
  threads.add([&]() { consume(load); });
  for (int index = 0; index < 6; ++index)
    threads.add([&, index]() { produce(load, index); });
  threads.start();
  const bool prepared = waitFor([&]() { return load.prepared.load() == 6; });
  const int heldA     = load.accepted[0].load() + 1;
  const int heldB     = load.accepted[1].load();
  load.release.store(true);
  const bool finished = waitFor([&]() { return load.finished.load() == 6; });
  load.done.store(true);
  threads.join();
  require(prepared && finished,
          "The concurrent producers exceeded their fixed deadline");
  require(heldA == 4 && heldB == 4,
          "Concurrent admission exceeded the 8/4 outstanding limits");
  verifyTerminal(load, 1);
  for (int index = 1000; index < 1096; ++index) verifyTerminal(load, index);
  for (int index = 2000; index < 2256; ++index) verifyTerminal(load, index);
  std::cout
      << "automation-live-race-test: six producers, two clients, held_pending="
      << heldA + heldB << ", receipt_polls=" << load.polls.load()
      << ", committed_UUIDs=" << load.commits.size() << " PASS\n";
}

void stopRace(int iteration) {
  SharedState state(instance, "edit");
  const auto first  = state.admit("client-a", request(3000), true);
  const auto second = state.admit("client-b", request(3001), true);
  std::atomic<bool> begun{false};
  std::atomic<bool> stopped{false};
  std::atomic<int> commits{0};
  Threads threads;
  threads.add([&]() {
    if (iteration == 1)
      require(waitFor([&]() { return stopped.load(); }), "Stop did not run");
    const auto ticket = state.takeNext();
    begun.store(true);
    if (!ticket) return;
    if (iteration == 0)
      require(waitFor([&]() { return stopped.load(); }),
              "Active Stop did not run");
    ++commits;
    std::this_thread::yield();
    state.finish(ticket, outcome(ticket));
  });
  threads.add([&]() {
    if (iteration == 0)
      require(waitFor([&]() { return begun.load(); }), "Edit did not begin");
    state.revoke();
    stopped.store(true);
  });
  threads.start();
  threads.join();
  const QJsonObject result = state.poll(first.ticket);
  require(commits.load() == (result.contains("result") ? 1 : 0),
          "Stop changed the started commit outcome or allowed a duplicate");
  require(result == state.poll(first.ticket),
          "Stop changed its retained outcome on lookup");
  require(state.poll(second.ticket).value("error").toObject().value("code") ==
              "gui_busy",
          "Stop allowed another queued edit to begin");
}

struct Deadline final {
  SharedState state{instance, "edit"};
  std::shared_ptr<Ticket> ticket;
  int commits = 0;
};

void deadlineConsumer(std::array<Deadline, 8>& cases) {
  std::array<std::shared_ptr<Ticket>, 8> started;
  started[0] = cases[0].state.takeNext();
  require(bool(started[0]), "The deadline seed did not begin");
  for (int index = 2; index < 8; ++index) {
    const auto offset = std::chrono::microseconds(index % 2 ? 500 : -500);
    std::this_thread::sleep_until(cases[index].ticket->deadline + offset);
    started[index] = cases[index].state.takeNext();
  }
  std::this_thread::sleep_until(cases.back().ticket->deadline +
                                std::chrono::milliseconds(5));
  for (int index = 0; index < 8; ++index) {
    if (!started[index]) continue;
    ++cases[index].commits;
    cases[index].state.finish(started[index], outcome(started[index]));
  }
}

void deadlineRace() {
  std::array<Deadline, 8> cases;
  for (int index = 0; index < 8; ++index)
    cases[index].ticket =
        cases[index].state.admit("client", request(4000 + index), true).ticket;
  Threads threads;
  threads.add([&]() { deadlineConsumer(cases); });
  for (int index = 0; index < 6; ++index)
    threads.add([&, index]() {
      std::this_thread::sleep_until(cases[index + 2].ticket->deadline);
      for (auto& item : cases) item.state.poll(item.ticket);
    });
  threads.start();
  threads.join();
  int committed = 0;
  for (auto& item : cases) {
    const QJsonObject result = item.state.poll(item.ticket);
    require(!result.isEmpty(), "The deadline race left a nonterminal receipt");
    require(
        item.commits == (result.contains("result") ? 1 : 0),
        "Deadline expiry overwrote a started outcome or allowed a late commit");
    if (result.contains("error"))
      require(
          result.value("error").toObject().value("code") == "deadline_exceeded",
          "The deadline race returned an unexpected rejection");
    require(result == item.state.poll(item.ticket),
            "The deadline outcome was not immutable");
    committed += item.commits;
  }
  require(
      cases[0].commits == 1 && cases[1].commits == 0,
      "The deadline fixture did not preserve both begun and unstarted cases");
  std::cout << "automation-live-race-test: deadline/begin committed="
            << committed << "/8 PASS\n";
}

}  // namespace

void runAutomationLiveRaceTests() {
  concurrentClients();
  for (int iteration = 0; iteration < 64; ++iteration) stopRace(iteration);
  std::cout << "automation-live-race-test: 64 Stop/begin races PASS\n";
  deadlineRace();
}

}  // namespace Live
}  // namespace GroundzeroAutomation

#ifdef AUTOMATION_LIVE_RACE_STANDALONE
int main(int argc, char* argv[]) {
  QCoreApplication application(argc, argv);
  try {
    GroundzeroAutomation::Live::runAutomationLiveRaceTests();
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "automation-live-race-test: " << error.what() << '\n';
    return 1;
  }
}
#endif
