# Batch task lifetime repair and evidence

Status: implementation and fixtures prepared for F27/F28; source reviewed, with
runtime and differential evidence pending. The approved edit policy freezes only
objects affected by pending local submissions. No local builds or tests were run,
following [CLAUDE.md](../CLAUDE.md). This repair does not establish the cause or
resolution of the separate Windows workflow PageHeap failure.

## Baseline defects

At the audit baseline, `TaskRunner` in `batches.cpp` retained a raw `TFarmTask*`,
resolved dependencies from the live controller map, and traversed live children.
Stopping queued work changed its display state without withdrawing its runner.
Removal then accepted that suspended task even though the runner could still
read it. A worker already assigned a task, or publishing terminal status before
its final metadata writes, had the same lifetime gap. F06's active-child status
check covered only the narrower parent-removal case.

`removeAllTasks` and `doLoad` also cleared raw-pointer indexes without releasing
ownership roots. Groups own and delete their children, so deleting every indexed
pointer would double-delete those children. These findings remain traceable as
R01 → F27 and R02 → F28 in the [ledger](audit_findings.json).

## Prepared ownership and execution boundary

The implementation is in [batches.cpp](../toonz/sources/toonz/batches.cpp),
[batches.h](../toonz/sources/toonz/batches.h), and the internal
[batchtaskexecution helper](../toonz/sources/toonz/batchtaskexecution.cpp).
`TFarmTask` ownership and task mutation stay on the application thread. Each
accepted local submission retains an immutable execution plan, cancellation
control, and `RunnableP` in a controller record. The worker holds values rather
than task pointers, controller-map references, or mutable dependency containers.

`PlanBuilder` captures actual command-builder program/arguments, frame counts,
ordered dependency/child indices, eligibility, and task identity. It does not use
`TFarmTask` copy construction, which omits command fields. Capture rejects
executable dependency cycles while retaining suspended-node early return,
repeated dependency execution, missing-dependency handling, and the existing
zero/one/multiple-child command distinction. `startAll` stages whole-list
eligibility before capturing or publishing its plans. Unpublished states are
restored after capture/submission failure without restoring objects still held
by another accepted record.

The affected-object index holds a set of submission IDs per task incarnation.
[Task-sheet handlers](../toonz/sources/toonz/tasksviewer.cpp) check that index
before execution-affecting edits. Unrelated idle tasks remain editable and
removable. Checked chunk replacement rejects the whole affected group before
mutation and replaces the former unbounded remove-until-empty UI loop.

## Executor retirement, cancellation, and shutdown

The existing [executor](../toonz/sources/common/tcore/tthread.cpp) reports queue
publication through `addTask(task, accepted)`. Publication and ownership are
handled separately from the task's displayed status:

| Event | Prepared behavior |
| --- | --- |
| Failure before acceptance | Retire the unpublished record and restore eligible prior display state. |
| Failure after acceptance | Keep the record, request cancellation, and attempt queued withdrawal. |
| Successful queued withdrawal | Retire explicitly; `withdrawQueuedTask` emits no completion signal. |
| Failed queued withdrawal | Keep the assigned/running record until completion. |
| `canceled` signal | Set the atomic cancellation token; do not treat it as completion. |
| `finished` or `exception` | Queue application-thread retirement after `run` returns or unwinds. |
| Terminal display outcome | Update display data while the submission remains protected. |

Executor signals run under its transition mutex. Direct connections perform only
atomic cancellation or semaphore completion; they do not enter the executor or
mutate task/UI state. Retirement uses explicit queued connections. A completed
runner may still be retained by the executor when its controller record retires;
its remaining members contain no live task borrow.

Each command constructs, uses, and destroys its `QProcess` on the worker thread.
Cancellation is observed at entry, between graph commands, and in 50 ms process
waits. A root stop cancels that submission's current/future commands; an explicit
child/dependency stop targets matching nodes. Another submission sharing a
dependency retains its own cancellation control and membership entry.

The process loop kills canceled children and waits until they are no longer
running before returning. It accepts a normal exit after polling timeouts while
latching other process errors, so a later timeout cannot erase an earlier I/O
failure. The application-thread outcome preserves the completed-step fallback
for a nonzero exit and group child-state aggregation. These details follow Qt's
[wait implementations][qt-unix] and [process error handling][qt-process]; the
Windows [wait implementation][qt-windows] has the same retained-timeout behavior.

`aboutToQuit` invokes `shutdownExecution`: it marks the controller closing,
snapshots records, requests cancellation/withdrawal, and waits for remaining
completion tokens outside executor locks. Completion tokens are signaled after
`run` returns/unwinds, so the wait does not depend on GUI event delivery. This is
a local-submission drain; it does not replace global executor shutdown or retire
the separate legacy local-controller listener. Active-process application exit
still needs runtime qualification.

## Progress and list ownership

Outcomes carry submission and node values. Application checks require a live
record, matching list generation, and matching task incarnation before mutation.
Internal identities are not serialized or added to farm wire messages.

`postTaskProgress` atomically reads an immutable ID-to-identity snapshot from the
server thread. The queued callback performs task lookup and counter updates on
the application thread after matching generation/incarnation. Adding or removing
an unrelated idle task preserves pending progress for unchanged identities.
Previously queued progress cannot target a replacement object with the same ID.
A message first arriving after ID reuse still lacks a wire-level run identity;
that limitation is retained below.

Clear/load is refused while affected work remains pending or active. Once safe,
lookup indexes and selections are detached before distinct ownership roots are
destroyed. Actual group membership determines ownership; editable `m_parentId`
strings do not determine which objects are deleted independently.

`TaskListReader` stages replacement objects under temporary ownership, accepts
historical same-pointer aliases, rejects distinct objects with conflicting IDs,
and publishes only a prepared replacement. Open/format/validation failure keeps
the current list and its identity. This covers normal owner cleanup and visible
list transactionality; it does not establish cleanup of every exception inside
the shared persistence parser.

## Authored regression evidence

[batchtaskintegrationtest.cpp](../toonz/sources/toonz/batchtaskintegrationtest.cpp)
uses the actual controller and executor. Gates run inside `run` or after outcome
publication, outside the executor transition lock. External execution is replaced
only where deterministic state transitions need it; separate cases launch the
application's minimal stdin-waiting child mode. No timing sleep is needed to
exercise successful completion after a polling timeout.

The gated affected-edit case also calls `exerciseBatchTaskViewGuards` from
[batchtaskviewintegrationtest.cpp](../toonz/sources/toonz/batchtaskviewintegrationtest.cpp).
It invokes actual TaskSheet name, dependency, chunk, and thread slots, checks
restored editor values and child identities after refusal, and accepts/restores
unrelated idle name and leaf command-field edits. It does not cover all edit
handlers, mouse/focus delivery, tree removal, visual themes/DPI, or remote farm
execution. Hidden leaf controls are exercised through their existing callable
slots; the repair does not expose new UI controls.

| Cases | Observable behavior |
| --- | --- |
| `exerciseQueuedWithdrawal`, `exerciseAssignedCancellation` | Withdrawn work never launches; assigned work remains protected until retirement. |
| `exerciseTerminalRetention`, `exerciseStopAll` | Terminal display does not permit early removal; queued and assigned cancellation retire separately. |
| `exerciseAffectedEdits` | Affected dependency/group edits are refused atomically while unrelated idle removal succeeds. |
| `exerciseSubmissionFailure`, `exerciseWorkerException` | Real pre/post-publication failures and a worker-boundary throw release the tracked submission. |
| `exerciseMultipleSelectedTasks` | Actual tree start/stop handles two selected roots while leaving an unselected task untouched; checks use a pre-mutation selection snapshot. |
| `exerciseTaskTreeSubmissionFailure` | A standard exception from submission is reported through the actual tree start slot and leaves no pending submission. |
| `exerciseChunkCollisionEditor` | A real chunk editor collision reports a warning and refreshes/restores the editor without replacing original children. |
| `exerciseClearAndLoad`, `exerciseOwnershipRoots` | Active clear/load is refused; failed replacement preserves the list; counted task owners/children release once. |
| `exerciseLoadAliasesAndDuplicates` | Historical flat child aliases retain one identity; conflicting duplicate IDs cannot replace the list. |
| `exerciseInvalidSerializedOwnership` | Serialized self-cycle, duplicate-child and shared-child graphs are rejected while prior list identity, dirty state and owners remain intact. |
| `exerciseRetiredOutcomes` | Replayed terminal outcomes from a retired submission cannot modify a replacement task. |
| `exerciseQueuedProgressIdentity` | Queued progress survives unrelated membership edits and is rejected after same-ID replacement or list replacement. |
| `exerciseSharedDependencyCancellation`, `exerciseChildCancellation` | Root cancellation does not cancel another owner; individual child cancellation preserves sibling execution. |
| `exerciseCapturedCommands`, `exerciseDependencyOrder`, `exerciseGroupCardinality` | Composer/cleanup arguments, repeated/diamond/suspended dependencies, and child cardinality remain compatible. |
| `exerciseStartAllEligibility`, `exerciseRejectedCycles`, `exerciseCompletedStepFallback` | Whole-batch eligibility, rejection before publication, and nonzero-exit progress fallback are preserved. |
| `exerciseRealProcessStartFailure`, `exerciseRealProcessAfterPoll`, `exerciseRealProcessCancellation` | Missing executable, later successful exit after a poll, and cancellation/reaping use real QProcess execution. |

The selected-tree case checks behavior; it does not reproduce a TreeModel use-after-free.
Selection items can be reused during refresh. Start/stop/remove snapshot IDs before mutation,
and standard exceptions from start/stop/chunk edits now reach existing error reporting.

Runner retirement/destruction and individual `CountedTask` destruction are counted.
`TFarmTaskGroup` is final: its own destructor is not directly counted; group
coverage uses lookup emptiness and owned-child counts. Repeated valid loads need
leak-detector evidence for their freshly deserialized objects.
The serialized ownership rejection case checks retained list state; it does not
prove per-allocation cleanup or cover partial nested extraction failures.

Retired-outcome replay returns at the absent-record check. It does not separately
prove generation/incarnation rejection or duplicate retirement delivery. The
progress fixture independently exercises task-incarnation rejection, while its
whole-list case changes both generation and incarnation. A targeted mutation or
additional independent evidence is still needed for each separate predicate.

## Remaining scope and qualification

- **Malformed nested persistence, P2:** `TFarmTaskGroup::loadData` extracts a child
  before adopting it. In `tstream.cpp`, a new object is assigned before its load
  or end-tag validation can throw. That unattached child can escape group cleanup.
  The alias branch also assigns an existing pointer before some format errors,
  so blanket deletion on extraction failure could delete an object another root
  owns. A separate repair needs explicit new-object versus alias provenance and
  fixtures for both failures. F28 does not claim nested malformed-load leak freedom.
- **Legacy progress correlation:** external `taskProgress` carries a textual ID
  without submission identity. A late message arriving after ID reuse can match
  the new snapshot. The local launch still does not add `-farm`/`-id` wiring.
  Local queue identity checks do not authenticate that external message.
- **Remote configuration restart:** an existing farm mapping still calls
  `restartTask(farmTaskId)` without resending edited local command configuration.
  This local-execution repair does not qualify remote reconfiguration/restart.
- **Runtime evidence:** compile and run the exact integrated revision on supported
  Windows/Linux/macOS configurations; retain native checkpoints and sanitizer or
  PageHeap logs. Run targeted baseline/mutation comparisons. Active-process quit,
  non-timeout I/O-error latching, separate generation checks, and duplicate
  retirement injection are not established by the authored cases above.

All these qualifications remain open. Passing older Tasks fixtures or unrelated
CTest suites does not establish this repair's runtime result or a Windows crash
root cause.

[qt-unix]: https://github.com/qt/qtbase/blob/5.15/src/corelib/io/qprocess_unix.cpp#L775-L815
[qt-windows]: https://github.com/qt/qtbase/blob/5.15/src/corelib/io/qprocess_win.cpp#L737-L770
[qt-process]: https://github.com/qt/qtbase/blob/5.15/src/corelib/io/qprocess.cpp#L879-L1095

At `5e9c2f00ccabd48f8f6b80748b924e1f9ea4a23b`, the
[Linux build](https://github.com/OpenAnimationLibrary/groundzero/actions/runs/42422881641/job/128554892608)
compiled `tasksviewer.cpp`, `batchtaskexecution.cpp`, `batches.cpp` and both batch fixture
sources without task-file diagnostics, and linked OpenToonz. The external-FX fixture target
then left the overall build failed, so no task checkpoint ran. The matching macOS job also
failed that fixture; task compile/link completion was not established there. This is bounded
compilation evidence, not validation of plan execution, UI guards or lifetime behavior.
