#pragma once

namespace GroundzeroAutomation {

// Once its input has ended, an automation process ends within this time.
constexpr int ShutdownDeadlineMs = 3000;
// Exit code of a process the deadline ended before its teardown finished.
constexpr int ShutdownDeadlineExitCode = 3;
// Teardown budgets inside the deadline: jobs on both lanes to end (render
// writers closed), then scene hosts to end after their input closes, then
// killed hosts to be reaped.
constexpr int JobShutdownBudgetMs    = 1500;
constexpr int HostCloseBudgetMs      = 800;
constexpr int HostKillBudgetMs       = 300;

// Start the shutdown deadline; later calls do nothing.
//
// A detached thread ends the process with ShutdownDeadlineExitCode when the
// deadline passes, whatever the other threads are doing. What is left then is
// what a killed process leaves, which the next worker's sweep of abandoned
// claims removes; scene hosts and parse helpers end with their worker. The
// OpenMP runtime's registration file outside the workspace is removed first.
void armShutdownDeadline();

// Milliseconds left before the deadline; the full deadline when not armed.
int shutdownRemainingMs();

}  // namespace GroundzeroAutomation
