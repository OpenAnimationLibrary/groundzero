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

## Proposed follow-up design

Keep submission lifetime separate from the task's displayed status. Track each
submitted `RunnableP` before publication and retain its task borrows until the
runner has made its last task access. Handle accepted-submission exceptions,
queued withdrawal, successful completion, and exceptional completion explicitly.
Cancellation notification alone must not release an active borrow.

Protect the complete graph a runner can access, including group children and
dependencies. `doRun` resolves dependency IDs dynamically through `getTask`;
therefore a root-only pending-runner counter does not protect dependency reads.
Dependency lookup and borrow acquisition must be atomic with removal, and chunk
or dependency edits must not invalidate a runner's execution graph. Preserve
unrelated-task editing/removal rather than blocking the whole batch list whenever
any runner exists. Whether to snapshot an execution graph or coordinate live
edits needs a focused compatibility review.

For stop/remove, withdraw queued work through the executor's atomic API. If a
worker already owns the submission, retain the borrows and reject removal until
retirement is observed. Release locks before executor calls, modal warnings,
and destruction of runnable records that may invoke callbacks. Extend the same
protocol to bulk clear/load before adding owner deletion there.

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
