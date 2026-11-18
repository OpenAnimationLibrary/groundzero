#pragma once

namespace GroundzeroAutomation {

// Keep macOS from napping this process for the rest of its life.
//
// A worker, scene host or parse helper is an application without a window.
// About 30 seconds after it goes idle, App Nap lowers every one of its threads
// to background priority 4, throttles its I/O and coalesces its timers. On a
// loaded machine such a process then reads the end of its input, and runs the
// quit request that follows, seconds late, so it misses its exit deadline.
// A user-initiated, latency-critical activity keeps its normal priorities.
// Does nothing on other systems.
void preventAppNap();

}  // namespace GroundzeroAutomation
