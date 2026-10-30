#pragma once

// Called only by the isolated native UI runner, after icon workers have
// quiesced. Uses the production Stage visitor on an actual worker thread.
void runIconWorkerSettingsNativeChecks();

// Native acceptance: cancel and accept real New/Load transitions, prove worker
// retirement and resumed rendering, then exercise terminal Quit and its late
// completions before GUI teardown.
void runIconLifecycleNativeChecks();
