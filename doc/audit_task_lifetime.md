# Batch task lifetime follow-up

Status: source-confirmed residual defects; runtime reproduction and a complete
retirement fix remain unverified. No local builds or tests were run, following
`CLAUDE.md`. This document does not attribute the existing Windows workflow
PageHeap failure to these paths.

## P1: a suspended task can still be borrowed by a worker

The relevant code is in `toonz/sources/toonz/batches.cpp`:

- `start` and `startAll` submit `TaskRunner` objects to `m_localExecutor`.
  Each runner stores a raw `TFarmTask*` in `m_task`.
- `stop` changes a `Waiting` task to `Suspended` and kills any published
  `QProcess`. It does not withdraw that task's queued runner.
- `removeTask` permits removal once the task and its descendants are no
  longer `Waiting` or `Running`, then deletes the owned task.
- `TaskRunner::run` subsequently calls `doRun(m_task)`. The first statement
  reads `task->m_status`, including when the task was suspended before running.

A deterministic regression should hold the executor's active worker behind a
gate, queue a second task, stop that second task, remove it, and then release the
gate. On the current implementation the queued runner can read the freed second
task. A task already handed to a worker but not yet inside `run` presents the
same ownership problem; an empty process map is not proof that the task is safe
to delete.

`TaskRunner::doRun` also marks a task `Completed` or `Aborted` before its last
metadata writes, and a group runner reads child states after their execution.
Consequently, terminal display status alone is not a worker-retirement signal.

The active-child case has a narrower guard in this patch: removing a suspended
group now checks its descendants and rejects removal while a child is `Waiting`
or `Running`. The native fixture covers both states and subsequent successful
removal. That guard does not solve suspended queued runners or terminal-status
publication before the worker's final access.

## P2: bulk clearing drops owners

`BatchesController::removeAllTasks` and `doLoad` clear `m_tasks`, a map of raw
task pointers, without deleting its owners. `TFarmTaskGroup::Imp::~Imp` in
`toonz/sources/toonzfarm/tfarm/tfarmtask.cpp` deletes the group's children when
the group itself is deleted. Repeated clear/load operations therefore retain
unreachable groups and their owned children.

Do not repair this by iterating the map and deleting every pointer: groups own
their children, so that can double-delete children or inspect them after their
owner is gone. Even a root-only deletion pass is unsafe while queued or active
runners retain raw pointers. Collect ownership roots before deleting anything,
and release them only after borrowing work has retired.

## Existing executor support and constraints

`toonz/sources/include/tthread.h` and
`toonz/sources/common/tcore/tthread.cpp` provide these relevant primitives:

- `addTask(task, accepted)` reports whether queue publication happened, even
  if a later synchronous scheduling refresh throws. An accepted submission may
  already be running and must remain tracked on that exception path.
- `withdrawQueuedTask(task)` atomically removes only queued work. A false
  result does not establish whether the task is running or already finished.
- `removeTask` and `cancelAll` emit cancellation for active workers but do not
  wait for their `run` methods to finish.
- `beginRetirement` is terminal and requires a dedicated executor used only
  for dedicated work. The batch executor currently uses the ordinary shared
  worker configuration, so that API cannot simply be inserted into `stop`.
- Runnable signals are emitted under the executor transition mutex. Their
  direct handlers must not block or call executor methods.

`BatchesController::stopAll` currently calls `cancelAll` while holding
`TasksMutex`. A runner destructor that also takes `TasksMutex` would introduce
a lock-order/deadlock risk when queued runners are released by cancellation.

## Additional source findings from the lifetime trace

The following observations narrow the implementation requirements. They do not
establish a connection to the Windows crash report.

- `batches.cpp:181,188,207,217`: a runner reads its raw task, resolves dependencies
  from the live map, and repeatedly traverses the live child vector. Retaining
  only the submitted root or only the running process is insufficient.
- `batches.cpp:264-271`: a leaf publishes `Completed`/`Aborted` before its final
  counters and completion date. A terminal display state is not a deletion
  boundary.
- `batches.cpp:234-254`: a worker allocates a `QProcess`, publishes its raw pointer,
  then removes the map entry without deleting the process. Use worker-owned RAII
  for the process; the registry is not its owner.
- `tasksviewer.cpp:248,265,277,289`: dependency handlers directly mutate the same
  dependency vector read by workers. Guarding removal alone does not stabilize
  the execution graph.
- `tasksviewer.cpp:391-401,487-509`: chunk rebuilding repeatedly calls removal
  until the child list empties. The timestamp-based edit guard admits queued
  tasks. A new busy-removal guard alone can produce a non-progressing loop and
  repeated warnings. Check the whole rebuild before changing anything.
- `tfarmtask.cpp:197-234`: copy construction delegates to assignment, which omits
  `m_isComposerTask`, `m_taskFilePath`, `m_outputPath`, `m_callerMachineName`, and
  `m_platform`. Do not construct execution snapshots by copying `TFarmTask`;
  that would change or lose command configuration.
- `batches.cpp:1160-1182`: the local controller's server runnable also looks up
  and mutates raw task pointers. Removing worker-side borrows includes this
  callback path, not only `TaskRunner`.
- `batches.cpp:611-619`: `startAll` changes each task to `Waiting` immediately
  before submitting a root. A worker can observe dependencies or children before
  the loop has marked them eligible. Capture the intended whole-batch eligibility
  before publishing any runner; do not freeze this scheduling race into a plan.

## Recommended internal repair, awaiting implementation review

Use a controller-owned submission record with a value-only execution plan.
Keep `TFarmTask` ownership and all live task mutations on the application thread.
This is the smallest recommended boundary that removes every demonstrated
worker borrow without rewriting the shared executor or changing the persisted
task format. A live-pointer lease design would still need to coordinate every
dependency, child-list, command-field, and status access spread across the task
sheet and worker. A root counter or a process-map lock does not do that.

The execution plan is an internal type, not a new task data model or serialized
format. Build it on the application thread before submission:

- Capture each node's actual program and argument list using the existing
  command builders. Capture the frame/count fields needed for result reporting.
- Capture ordered dependency and child edges as plan indices, with an explicit
  recursion stack to reject executable dependency cycles before publication.
  A suspended node returns before traversing its edges; preserve that behavior
  when choosing the captured execution closure. Preserve repeated dependency
  execution; do not silently introduce run-once semantics.
- Preserve the current zero-child, one-child, and multiple-child execution
  distinction. A leaf's `getTask(0) == this` is not a dependency cycle.
- Use existing task IDs for display, plus an internal list generation and task
  incarnation to identify the live object receiving results. Neither value is
  written to TNZBAT files or sent to a render-farm server.
- Retain initial suspension eligibility and per-node cancellation tokens in the
  submission record. Workers receive no `TFarmTask*`, controller map reference,
  live child vector, or live dependency container.

For `startAll`, prepare all intended `Waiting` states before capturing plans or
publishing work. For individual `start`, preserve its existing eligibility rules.
Capture actual command-builder output, including current argument order. Publish
process facts to the application thread and preserve the existing final status
rule, including its completed-step fallback for a nonzero exit, and child status
aggregation. Do not accidentally replace those rules with exit-code-only status.

The controller records the `RunnableP` and its affected task incarnations before
calling `addTask(runnable, accepted)`. The record follows this transition table:

| Event | Required transition |
| --- | --- |
| Throw with `accepted == false` | Release the unpublished record; restore display state. |
| Throw with `accepted == true` | Retain; cancel and attempt withdrawal outside locks. |
| Stop, withdrawal succeeds | Mark withdrawn; release once, outside executor locks. |
| Stop, withdrawal fails | Request cancellation; retain the assigned/running record. |
| `canceled` signal | Record cancellation only; do not release an active record. |
| `finished` or `exception` | Queue retirement after `run` returns or unwinds; retire once. |
| Terminal display state | Update display only; retain until the retirement callback. |

Use queued controller callbacks for executor signals; direct handlers under the
executor transition mutex must not block or re-enter the executor. Notifications
carry outcome values and task incarnation, never a live task pointer. Applying an
outcome requires both a matching list generation and matching incarnation. This
prevents an already-queued event from being applied to a replacement with the
same textual ID. Queued retirement callbacks must retain enough record identity
to ignore duplicate delivery without double-releasing ownership.

Each running command owns its `QProcess` on its worker thread. Cancellation is
requested through the record and performed on that same thread, using a worker
event loop or bounded waits that observe the cancellation token. Do not retain
the current cross-thread raw-process map. Check cancellation at entry and before
each command; keep assigned/running records active until they finish. Stopping
queued work must not start its process. An already-started process is killed and
reaped before the record retires.

### Mutations, clearing, and loading

Maintain an application-thread index from task incarnation to pending records.
Use it to reject removal and execution-affecting edits only for objects in a
pending plan, including dependencies and group descendants. An unrelated idle
task remains editable and removable. Multiple submissions may retain the same
dependency; this index is a set of records, not a single boolean or root count.

Move chunk replacement into one checked controller operation. Check the entire
affected group before removing any child or changing its chunk size. On refusal,
leave the group unchanged and refresh the editor once. Remove the task sheet's
unbounded removal loop. Dependency editing and command-field editing must use
the same affected-task check, so accepted submissions have stable configuration.

Retain the current refusal to remove active work. Extend that refusal to
assigned and suspended-but-unretired work, and apply the same rule to `doLoad`
before it discards the current list. The user can stop the work and retry after
retirement. Do not spin a modal event loop while holding controller state.

Once no affected record remains, collect all distinct ownership roots before
deleting anything. Determine owned children from actual group membership, not
only the editable `m_parentId` string. Detach lookup indexes and task selections,
then delete each root once; groups destroy their children. For load, deserialize
replacement roots into temporary RAII ownership, validate duplicate IDs, and
publish the replacement only after successful preparation. Failed loading must
release the temporary owners and retain the previous list.

### Exact proposed production edit area

Paths below are relative to `toonz/sources/toonz` unless otherwise noted.
The helper and test names describe proposed files, not implemented features.

| File or area | Proposed responsibility |
| --- | --- |
| `batches.h` | Private submission registry, task identity, and checked edits. |
| `batches.cpp:138-274` | Replace raw-task traversal with owned plans and worker-owned processes. |
| Submission/removal/load methods | Apply the transition table and release roots safely. |
| Local-controller callbacks | Marshal progress values to the application thread. |
| `tasksviewer.cpp` | Check dependency/command edits and replace chunks atomically. |
| Internal `batchtaskexecution` helper | Isolate per-run state and process/publication boundaries. |
| Native fixture and CMake/CI | Gate the protocol; coordinate shared build/workflow ownership. |

Preserve existing public start/stop/remove commands. The checked chunk operation
must replace the task sheet's partial, potentially non-progressing rebuild.
Extract a helper only if it makes this implementation clearer; no reusable task
framework is proposed. Process execution and value publication are the two
boundaries needed for deterministic fixtures.

No production edits are made at this design stage. No changes to `tthread.cpp`,
farm serialization, executor-wide cancellation, or dedicated-worker retirement
are proposed.

### Compatibility decisions requiring explicit review

1. Edits to a queued job currently reach live mutable state, but are unsafe.
   Recommendation: refuse execution-affecting edits from accepted submission
   through retirement; permit unrelated idle edits. The next submission captures
   the edited configuration. This is a deliberate restriction of the unsafe
   queued-edit path, without a new preference or file-format change.
2. `doLoad` currently discards the lookup map even while work exists.
   Recommendation: reject replacement until affected work retires, matching
   removal's existing active-work restriction. Preserve the prior list if the
   replacement file cannot be parsed.
3. A root stop during dependency execution currently does not reliably kill that
   dependency's process. Recommendation: cancel the current command and future
   commands owned by that submission, without canceling another submission that
   shares the dependency.
   Preserve the existing behavior when explicitly stopping an individual child
   or dependency, and specify this in the regression expectations.
4. Local server `taskProgress` messages contain only a textual task ID. The
   current `TaskRunner` launch has its `-farm`/`-id` argument wiring commented
   out. Generation checks can reject callbacks queued before list replacement;
   they cannot authenticate a late external message that arrives afterward with
   a reused ID. Do not claim that per-run wire correlation already exists or add
   that protocol behavior silently. This is separate from removing raw-pointer
   worker access and needs a documented policy if the callback path is retained.

## Required CI evidence

Add deterministic fixtures with gates, not timing sleeps, for these cases:

1. Stop and remove queued work before it starts; release the worker gate and
   verify no freed task is accessed and no canceled task launches a process.
2. Stop during the queue-to-worker handoff; verify removal remains unavailable
   until the worker's last access, then succeeds.
3. Publish a terminal task status before the last access; verify the object
   remains retained until retirement.
4. Remove an active child's parent, remove a dependency while borrowed, and
   edit a queued group's chunks; verify protected objects remain valid while
   unrelated idle tasks can still be removed.
5. Cancel all, throw before and after accepted submission, and throw inside
   `run`; verify every borrow is released exactly once without deadlock.
6. Clear or load over queued and running work; verify the chosen existing-work
   behavior, correct ownership release after retirement, and no stale callbacks
   applied to replacement task IDs.
7. Repeat clear/load and group/child removal under a leak detector; count owner
   and child destruction and verify there is neither leakage nor double-delete.

Run the relevant native suite in CI on Windows with PageHeap and on supported
Linux/macOS sanitizer builds. Demonstrate that targeted regressions detect the
unfixed paths, and distinguish these results from the separate pre-existing
workflow-panel failure. No such runtime evidence is claimed by this audit.

## Concrete gated regression construction

Run the fixture against the actual controller and its local executor. A test
adapter may access that executor and install narrowly scoped gates; it must not
replace task ownership, queue withdrawal, result application, or retirement
logic with a mock. Mock only the external process boundary where a real child
process would make the state transition nondeterministic. Include a real child
process cancellation/reaping case separately.

- **Queued removal:** occupy the single active slot with a semaphore-gated
  runnable, submit a second real batch task, then stop and remove it. Verify
  withdrawal succeeded, no command started, and destruction occurred exactly
  once. Release the blocker and verify no callback touches the removed task.
- **Assigned before first access:** gate the first line inside `TaskRunner::run`,
  after the executor has released its transition mutex but before execution-plan
  or task access. Stop the task; withdrawal must fail. Removal stays refused
  until that gate is released and retirement is delivered, then succeeds.
  Never block a direct `started` signal handler: that signal holds the executor
  transition mutex and would invalidate the test by deadlocking cancellation.
- **Terminal before retirement:** intercept the value-publication boundary,
  publish a terminal outcome, and gate its return outside controller/executor
  locks. Process application events so the terminal status is visible. Removal
  must still be refused until the worker returns and its retirement callback is
  delivered. This directly distinguishes display state from execution lifetime.
- **Graph and edit isolation:** queue a group with children and dependencies,
  attempt a chunk rebuild and dependency removal, and assert that both are
  refused without partial changes or repeated dialogs. Remove a separate idle
  root successfully. Cover missing dependencies, repeated dependency edges, and
  zero/one/multiple-child groups without changing valid command arguments. Cover
  a suspended dependency with outgoing edges and whole-batch eligibility under
  `startAll`, including a dependency that appears later in priority order.
- **Publication failures:** inject failure before queue acceptance and after
  actual accepted publication, using the existing executor submission fixture's
  patterns. Assert that the former releases immediately and the latter remains
  tracked until withdrawal or completion. Throw from the process boundary to
  exercise exceptional retirement. Count every record release once.
- **Replacement and delayed events:** hold work active, attempt clear/load, and
  assert no owners or list identity changed. Retire the work, load a fixture with
  reused IDs, then deliver a previously queued outcome carrying the old
  generation/incarnation. It must be discarded. Treat late external ID-only
  progress separately; this test does not demonstrate wire-level correlation.
- **Owner accounting:** instrument root and child destruction, repeatedly clear
  and load the same multi-chunk fixture, and require one deletion per object.
  Include malformed replacement input and duplicate IDs. Run this case with a
  leak detector in CI in addition to the explicit counters.

For differential evidence, the queued/assigned and terminal-status cases must
fail against the pre-repair ownership code with the same gates and inputs, then
pass with the lifecycle change. Gates must be released by fixture cleanup even
when an assertion fails. Record the failing phase, task ID, submission identity,
and owner counts so a timeout cannot be mistaken for a detected use-after-free.

The existing native Tasks cases exercise UI ownership and active child status;
they do not gate a real local submission. Their success alone cannot establish
worker lifetime safety or explain the separate Windows workflow-panel failure.
