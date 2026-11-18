#include "automation_shutdown.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <thread>

#ifdef _WIN32
#include <io.h>
#else
#include <unistd.h>
#endif

namespace GroundzeroAutomation {
namespace {

using Clock = std::chrono::steady_clock;

std::atomic<Clock::rep> armedAt{0};

void announceDeadline() {
  static const char message[] =
      "automation-worker: shutdown deadline passed; ending the process\n";
#ifdef _WIN32
  (void)_write(2, message, unsigned(sizeof message - 1));
#else
  (void)::write(2, message, sizeof message - 1);
#endif
}

}  // namespace

void armShutdownDeadline() {
  Clock::rep expected = 0;
  const Clock::rep now =
      std::max<Clock::rep>(1, Clock::now().time_since_epoch().count());
  if (!armedAt.compare_exchange_strong(expected, now)) return;
  std::thread([] {
    std::this_thread::sleep_for(std::chrono::milliseconds(ShutdownDeadlineMs));
    // Low-level write: a stuck thread may hold the stdio locks.
    announceDeadline();
    std::_Exit(ShutdownDeadlineExitCode);
  }).detach();
}

int shutdownRemainingMs() {
  const Clock::rep armed = armedAt.load();
  if (armed == 0) return ShutdownDeadlineMs;
  const auto elapsed = Clock::now() - Clock::time_point(Clock::duration(armed));
  const auto left =
      std::chrono::milliseconds(ShutdownDeadlineMs) -
      std::chrono::duration_cast<std::chrono::milliseconds>(elapsed);
  return int(std::max<std::chrono::milliseconds::rep>(0, left.count()));
}

}  // namespace GroundzeroAutomation
