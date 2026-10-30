# Native icon rendering and shutdown

The macOS job for `b00db72a35be48bf7852cbbc49a113622985761d` reproduced
a shutdown crash in both the isolated timing diagnostic and the separate
required timing lifecycle gate. The timing assertions and application cleanup
checkpoint passed, then the process exited with SIGSEGV. Both LLDB captures
stopped a background worker in `QAction::isChecked`, reached through
`StageBuilder::addFrame`, `ToonzScene::renderFrame`, and
`XsheetIconRenderer::run`. The main thread was destroying main-window widgets
and actions. This is distinct from the separately corrected Function segment
editor observer lifetime defect.

Evidence: [completed macOS job and LLDB output](https://github.com/OpenAnimationLibrary/groundzero/actions/runs/41075364273/job/124132160178).
The original failing process statuses remain authoritative; a diagnostic fixture
PASS does not qualify a crash during teardown.

## Rendering settings

`Stage::VisitArgs::m_cameraTest` is an explicit value, defaulting to false.
`StageBuilder` uses that captured value throughout a visit. The four native
Scene Viewer drawing and picking callers capture the GUI setting before
visiting. Thumbnail and other convenience-render callers do not consult a
`QAction` from their worker thread. Camera Test continues to restrict the
current column when explicitly requested by the viewer.

## Shutdown contract

`IconGenerator::shutdown()` is terminal, GUI-thread-only, and idempotent. It
closes request admission before servicing any callbacks and retires its own
executor. Every admitted generation remains tracked until completion, including
a superseded request that is still running. Queued work is canceled; an active
render is allowed to finish safely. Completion during shutdown resolves request
tokens without publishing a new cache entry or `iconGenerated` event.

Retirement includes a successful `QThread::wait` and Qt `QThreadStorage`
destructors, not only the `Runnable::finished` signal. GUI event service remains available for
offscreen-context creation and destruction. A reentrant shutdown call must also
observe complete retirement before returning. A previously completed task's
late termination notification does not start an unconditional nested wait.

Scene and Xsheet thumbnail tasks acquire source-scene leases at GUI admission,
before scheduling. The leases also protect readers whose scene was replaced
before shutdown began. Synchronous file-icon rendering owns its detached scene
and does not consult the GUI registry from a worker. A lease wrapper keeps the
registry record alive until GUI destruction, including the interval after a
worker's final release posts `DeferredDelete`. Retirement generations prevent a
stale timer from deleting a reinstated current scene. Current scenes retain the
existing caller ownership; destroying a handle does not delete its current
scene. Retired scenes are released after their timer (or handle) is destroyed
and all reader leases have finished.

The accepted main-window close path supplies a callback to
`IoCmd::saveSceneIfNeeded`: it drains after all save/discard decisions succeed
and before that command clears the scene. Cancel and failed saves never invoke
the callback, so icon generation remains usable. The close path also drains
before clearing image resources. A close-in-progress latch rejects reentrant
close events before prompting or emitting exit notifications.

A shared `SceneTransitionGuard` retains the reviewed source and blocks queued
New, checked/unchecked Load, direct save-and-clear, and close entrypoints during
the transition. The save helper checks the reviewed scene after each prompt and
save operation. If a low-level caller nevertheless replaces the scene during
accepted shutdown, the old reader source remains alive, Quit stays accepted,
and the unreviewed replacement is not cleared.
The common boundary after the application's event loop drains as well, covering
programmatic exit and the native runner that do not execute `closeEvent`.
Production shutdown never returns into widget destruction merely because a
timeout elapsed while workers remained active.

Ordinary New and Load also need reader quiescence: retaining a scene alone does
not prevent `ToonzScene::clear` from replacing properties and clearing assets.
After successful save/discard decisions, these commands close thumbnail
admission and retire the old executor before destructive changes. Admission
stays closed through the complete command, including unchecked Load and progress
dialog event service. A replacement executor is allocated and configured before
pausing; preparation failure leaves the original executor usable and aborts the
transition. The guard resumes by swapping the prepared executor without an
allocation or throwing destructor. A terminal shutdown during a pause never
reopens admission. A precreated deferred repaint notification refreshes consumers
that consumed paint events while requests were paused; it emits no request
completion token. Canceled prompts do not pause rendering.

## Prepared native acceptance

`iconlifecycleintegrationtest.cpp` is included by `BUILD_UI_PORT_TESTS`; its
task hooks are compiled into `toonzqt` only under that same flag. At the final
native application boundary it exercises:

- A real vector thumbnail with visible artwork, plus an injected renderer
  exception, with exactly one successful or unsuccessful token completion.
- Actual main-window Quit with the native Cancel choice, followed by successful
  icon generation; Discard acceptance keeps the source intact through drain.
- Deterministic native retirement timers, handle destruction, a current stack
  scene, and reinstatement before a worker's queued final lease release.
- A real Xsheet renderer whose scene is replaced and whose retirement timer
  expires while the worker is held; rendering must finish before its source
  is destroyed.
- Queued New, checked/unchecked Load, direct save-and-clear, and reentrant close
  during accepted Quit. A direct replacement survives until its own later
  explicit Discard decision.
- Canceled New and checked Load keep a real active Xsheet renderer and queued
  level renderer usable. Accepted New, checked Load, and unchecked Load retire
  those old readers before mutation, preserve saved source bytes, and generate
  visible thumbnails on the resumed executor.
- A real active worker held at its execution boundary, a superseded generation
  for the same drawing, and a separate queued drawing.
- Admission rejection before the drain services a queued GUI callback; that
  callback releases the active worker and reenters shutdown.
- Exact completion counts, no late cache or UI publication, cancellation before
  queued render execution, and actual `QThreadStorage` destructor completion.
- Repeated shutdown and deliberately late termination notifications for retained
  completed tasks.
- A two-column native Stage visit on a worker while GUI Camera Test is enabled:
  ordinary visits include both columns, and an explicit captured Camera Test
  includes only the selected column.

The test watchdogs release their own barriers and report failure. They do not
qualify performance or make a timed delay count as successful retirement.
The native runner requires the actual lifecycle PASS and worker-drained output
lines as well as a zero process status; echoed or missing lines cannot qualify
the full run, an isolated diagnostic, or the required timing gate. The timing
gate also retains its original fixture and cleanup checkpoints. No local build or test was run. These corrections and new
acceptance cases require fresh exact-head CI on every supported platform.
