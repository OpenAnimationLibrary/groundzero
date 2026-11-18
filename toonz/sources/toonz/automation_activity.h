#pragma once

namespace GroundzeroAutomation {

// Make this windowless automation process behave as one, before Qt starts.
//
// A worker, scene host or parse helper is an application without a window.
// - It runs background-only: no Dock icon, no menu bar, never activated, so it
//   cannot take focus from the application in front.
// - About 30 seconds after it goes idle, App Nap lowers every one of its
//   threads to background priority 4, throttles its I/O and coalesces its
//   timers; on a loaded machine it then reads the end of its input, and runs
//   the teardown after it, seconds late. A user-initiated activity, held for
//   the process's life, keeps its normal priorities.
// - After a process with this application's identifier was killed, AppKit asks
//   in a modal alert whether to reopen its windows, and the process waits for a
//   click. Ignoring persistent state, which it has none of, turns that off.
// Does nothing on other systems.
void prepareHeadlessProcess();

}  // namespace GroundzeroAutomation
