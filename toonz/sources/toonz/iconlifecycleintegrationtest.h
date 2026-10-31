#pragma once

// Called only by the isolated native UI runner, after icon workers have
// quiesced. Uses the production Stage visitor on an actual worker thread.
void runIconWorkerSettingsNativeChecks();

enum class NativeIconQuitScenario { PreserveReplacement, ClearReviewedScene };

// Native acceptance: cancel and accept real New/Load transitions, prove worker
// retirement and resumed rendering, then exercise one terminal Quit and its
// late completions. The two scenarios require separate application processes.
void runIconLifecycleNativeChecks(NativeIconQuitScenario scenario);
