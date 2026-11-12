# Memory lifetime audit and measurement plan

Status: scoped source inventory and proposed CI work, following the user-requested
memory review across artist workflows. No memory workload, local application build,
leak detector, or baseline/mutation run has been executed by this lane. The repeated-memory
protocol remains proposed; the ledger separately records prepared ownership repairs below.
No memory-runtime or baseline/mutation evidence is claimed.

The objective is to distinguish live artwork, intentionally retained caches/history,
expired ownership, and allocator retention before changing memory policy. Each repair
must preserve drawing identity, undo/redo, saved scenes and rendered output. Existing
PR 133 cache-pressure work must be reviewed separately rather than duplicated.

## Memory ownership fixture additions

The memory follow-up has source/fixture diffs; runtime and baseline/mutation evidence remain
pending. F38 is merge-exception double deletion and F40 is array-deleter mismatch, not measured
normal-success growth. F36 releases builder metadata; F45/F46 retire progress UI, not the full
render graph. See the measurement protocol below,
[scene ownership review](audit_memory_ownership.md) and [LSan qualification](audit_image_cache_lsan.md).

| ID | Priority | Prepared behavior | Authored cases |
| --- | --- | --- | --- |
| F35 | P1 | ChildStack owned-root teardown: Recover the owned root before deleting ancestor records; only that root is cleared or released. | `destroyWhileNested`, `closeBeforeDestruction`, `clearNavigationWhileNested`, `clearSceneWhileNested`; CI pending |
| F36 | P2 | Compressed cache ImageBuilder metadata ownership: Both compressed-item destructors delete their uniquely owned builder before other owned metadata. | `testRepeatedCompressedRemoval`, `qualify`, `remove_builder_delete`, `require_result`; CI pending |
| F37 | P2 | Paste conversion temporary payload ownership: Scope-own conversion and external-image payloads across the helper/Selection call, reuse RasterSelection transferredData ownership, and use an intrusive palette owner during external preparation. | `checkStrokeConversionOwnership`, `undoConvertedRaster`, `checkRasterToVectorCycles`, `checkExternalRasterCycles`; CI pending |
| F38 | P1 | Null-target palette ownership on merge failure: Hold the temporary palette with TPaletteP throughout both raster getData paths, so normal return and merge exceptions use one reference-counting ownership model. | `checkNullTargetPaletteCopies`, `checkNullPaletteFailure`; CI pending |
| F39 | P2 | External-FX input-port graph ownership: A private unique_ptr vector owns every allocated input port, including allocations hidden by duplicate names. | `inputPortOwnership`, `failedLoadPortOwnership`; CI pending |
| F40 | P1 | Plugin enum and spectrum array deleters: Use typed delete[] deleters for both trait arrays while preserving SDK pointers, values and the current parameter publication behavior. | `setup`, `checkReplacedParameters`; CI pending |
| F41 | P2 | Custom-panel submenu retention during rebuild: Delete owned direct submenus before clearing/rebuilding actions. | `checkNestedMenus`, `checkMenuCycles`; CI pending |
| F42 | P2 | About dialog retention after closing: The About dialog has scoped lifetime and owns its child widgets; returning from the modal action destroys the dialog and descendants. | `checkAboutCycles`; CI pending |
| F43 | P2 | Failed and overlapping update-checker lifetime: Parent each checker to the window, resolve the actual sender on completion, copy its result, disconnect only that sender from the window and schedule deletion before either success or error return. | `checkUpdateCompletion`, `checkOverlappingUpdates`, `checkUpdateCycles`; CI pending |
| F44 | P2 | Drawing snapshot shared cache ownership: A private shared lease removes owned snapshot IDs after the last clone releases them. | `checkClipboardClones`, `checkUndoOwner`, `checkTransferredGenerations`, `checkReplacementAndFailure`, `checkConsumingPaste`; CI pending |
| F45 | P2 | Normal render progress startup and terminal ownership: Scope-own listener/build-progress objects through startup, then hand off only after start. | `checkNormalRenders`, `checkRealCancellation`, `checkNestedCancellation`, `checkHandoffAndLateMessage`, `checkNestedProgressDelivery`, `checkStartupFailure`; CI pending |
| F46 | P2 | Multimedia render progress scoped ownership: Own the multimedia progress dialog for the synchronous start call and destroy it afterward; queued messages use QPointer and ignore an already retired target. | `checkMultimedia`; CI pending |
| F47 | P2 | Floating-paste undo original-image cache ownership: Destroying the noncopyable undo owner also removes its unique old-floating-image key, leaving unrelated entries and live undo behavior intact. | `oldFloatingCacheKeys`, `checkStrokeConversionOwnership`, `undoConvertedRaster`; CI pending |
| F48 | P2 | Optional plugin destroy callback reference balance: Invoke an available destroy callback, then release the host-held metadata reference independently; preserve callback-before-release ordering. | `PluginHostLifetimeTest::check`, `checkReplacedParameters`; CI pending |

F37 owns returned conversions; exceptions inside factories remain outside scope. F38 drives a
real throwing merge; normal null-target copy was not a leak. F40 value checks need mismatch
instrumentation. F44 orphan snapshots were previously cleared at scene reset. F45 graph-build
exceptions are source-only; early cancel is covered, active-window shutdown is not. F47 checks
keys before undo/after reset, with pixel-checked undo/redo between, not each intermediate key.

`childstack_lifetime` is a required 60-second reliability CTest on all three platforms.
`timagecache` remains the existing functional target. Linux additionally links standalone
LSan and runs the fixed/mutated/restored qualifier plus eight harness contracts in the same
job. The mutation removes only the in-memory builder delete; require the functional sentinel,
leak exit 23 and direct builder stack, then restored success. All execution remains pending;
disk spill/constructor exceptions are not forced. No job or matrix axis was added.

`exerciseUiMemoryOwnership`, `runDrawingCacheIntegrationTest` and
`runRenderProgressNativeChecks` run through the existing native application driver under
BUILD_UI_PORT_TESTS. Earlier build/runtime failures can prevent their checkpoints.
F37/F39/F48 reference assertions and UI QPointers are owner evidence, not measured RSS.

## Existing measurements and limits

| Existing route | What it records | What it cannot establish |
| --- | --- | --- |
| [nativeprocessmemory.cpp](../toonz/sources/toonz/nativeprocessmemory.cpp), `nativeProcessPeakResidentBytes` | Windows `PeakWorkingSetSize`; macOS `getrusage(RUSAGE_SELF).ru_maxrss` in bytes; Linux `/proc/self/status` `VmHWM` converted from KiB to bytes | These are process lifetime high-water values. They do not decrease after cleanup, attribute a prior allocation to a workload, or measure simultaneous child-process memory. |
| [artistworkflowbenchmark.cpp](../toonz/sources/toonz/artistworkflowbenchmark.cpp) | One final peak-memory value/source alongside native gesture/frame-render/save timings and correctness checks | The report captures its memory value before the final undo reset and destruction of the local reloaded scene. It has no before/after cleanup series and cannot establish retained-memory growth per cycle. |
| [paired_benchmark.py](../ci-scripts/paired_benchmark.py) | Matched process duration and peak RSS from retained bundles; Windows queries the benchmark process handle, POSIX uses `wait4` usage | Windows excludes descendants; POSIX usage may contain waited-descendant high-water contributions. Neither is simultaneous process-tree RSS or an ownership leak diagnosis. |
| [TImageCache](../toonz/sources/common/tcache/timagecache.cpp), `getMemUsage` | Sum of compressed and uncompressed item sizes | Header units say KB, but raster item sizes return bytes and aggregation uses `int`/`UINT` without scaling. Treat this as a legacy payload estimate with inconsistent documentation and 32-bit accumulation, not a reliable process-memory total. It omits other owners, vector payloads and allocator overhead. |
| Same cache, `getDiskUsage` | Constant zero in both total and per-ID implementations | No disk-cache measurement exists through these functions. A zero result is not evidence that no swap files remain. |
| [TUndoManager](../toonz/sources/include/tundo.h) | History count/index and per-entry approximate `getSize` estimates; explicit reset | Undo estimates are policy inputs, not exact live heap bytes. Counts can prove retired history without measuring unrelated caches or shared resources. |
| Existing image/media/particle cache tests | Selected image identity, compression/aliasing, media lease/budget/retirement and particle resource contracts | These protect particular ownership transitions. Their presence or a prior pass is not an all-workflow memory plateau or long-session result. |

Additional existing ownership fixtures provide narrower evidence:

- [Icon lifecycle](../toonz/sources/toonz/iconlifecycleintegrationtest.cpp) checks level
  QPointers, worker TLS destruction and empty tracking after retirement. Terminal singleton
  shutdown needs a fresh process for each repetition.
- [Batch lifetime](../toonz/sources/toonz/batchtaskintegrationtest.cpp) counts runners and
  individual task children and reaps a real child. Group-root allocation cleanup and malformed
  nested extraction retain the limitations in [the task review](audit_task_lifetime.md).
- [Renderer startup](../toonz/sources/common/tfx/tests/trenderer_startup_test.cpp) reaches real
  success/failure/cancel and stop boundaries. The weak-owner check in
  [movie render queue](../toonz/sources/toonzlib/tests/movierenderqueue_test.cpp) uses a shared
  payload stand-in, so a real output-raster release assertion remains necessary.
- [Plugin Manager](../toonz/sources/toonz/tests/pluginmanager_test.cpp) freezes session policy:
  disabling a loaded plugin does not unload its library. Repetition should create/destroy
  effect instances or use fresh processes, without inventing hot-unload behavior.

The image cache deliberately preserves `$:` scene-independent IDs during
`clearSceneImages`. Their survival alone is not a leak. Prefer known-ID/owner checks until
cache telemetry units and range have their own reviewed correction.

No current-memory time series or process-wide live-allocation inventory is supplied by
these routes. Native object/handle counters and cache identities should accompany a
new series so an apparent resident-memory plateau does not hide an ownership leak.
Conversely, a high-water value that stays high after release is not itself a leak.

## Workload map

Each row is a bounded proposed workload, not implemented telemetry or a passing scenario.
Use the existing application/format fixture entry points where possible. Source findings
and fixes belong in their own reviewed records; do not infer a defect solely from growth.

| Boundary | Repeatable cycle and cleanup boundary | Correctness and ownership observations |
| --- | --- | --- |
| Scene and nested Xsheet ownership | Open a disposable scene with representative native levels and nested Xsheets, navigate child/parent, save/reload, close or replace the scene | Same saved drawing IDs, exposures, palette/FX/key state; retired scene and navigation owners release; external resources remain valid. |
| Drawing edits, undo/redo and clipboard | Draw/edit raster and vector content, copy/paste, undo/redo, discard redo with an intentional edit, replace clipboard, reset history and scene | Exact pixels/geometry/identity and expected history count; clipboard/undo-owned cache IDs disappear only after their last owner. Test Cancel without destroying redo. |
| Frame navigation, playback and preview | Scrub fixed frames, render preview, stop, invalidate by an edit, switch scene and retire preview | Same frame output; no pending preview callbacks or scene-specific image owners after retirement; intended reusable cache capacity is recorded separately. |
| Render, effects and export | Render a fixed RGBA8/RGBA16/float scene, export supported formats, then cancel/fail/retry in separate cycles | Baseline/patched pixels and files agree where behavior is unchanged; source rasters remain intact; renderer/cache leases and helper processes retire. Unsupported modes remain explicit. |
| Panels, Rooms and custom UI | Open/use/close representative panels, switch every preset, alternate eager/lazy initialization, load valid/missing/malformed custom panels | Correct current Room and latest pending switch, usable surviving panels, released old QObjects; no queued callbacks referencing retired panels. W1/W3 remain separate unresolved runtime cases. |
| Tasks and subprocesses | Submit queued/assigned work, stop root/child, wait for retirement, remove/load/clear tasks; include application shutdown with active work | Exact runner/task destruction and no leftover child process/handle; unaffected tasks/progress remain correct. Preserve F27/F28 parser/farm limits. |
| Plugins and external tools | Repeat supported probe/load/use/close or restart cycles and external-FX/audio/FFmpeg success/start-failure/cancel paths | Correct plugin results, output and audio; no live expired requests or helper process. Do not invent unload support or treat intentional process-lifetime modules as per-cycle leaks. |
| Media, particle and assisted workflows | Repeat representative media decode/lease cleanup, particle preview/bake/export and available assisted-tool sessions; close review, accept/cancel, replace scene | Respect active readers and pinned resources; check disk generations separately from RAM; preserve accepted output and Cancel state. Report worker memory separately from the UI process. |

All-workflow scope requires this map to expand when an active owner/resource boundary is
found outside the listed routes. It does not mean every effect, codec, device, panel or
workflow has already been executed. Large scenes and device-specific paths follow the
bounded smoke cases rather than replacing them. Explicit additional families include camera
and import formats, 3D/GLB assets, scripting objects, sound playback/recording, thumbnail
browsers/file watchers and GPU/driver allocations; CPU RSS alone cannot qualify them.

## Proposed repeat-cycle protocol

1. Use matched baseline and candidate builds with exact revision, architecture, Qt/backend,
   allocator/build configuration, workload data hashes and memory-relevant preferences.
   Use isolated portable settings and disposable artwork; keep each workload in its own
   process so earlier allocations cannot dominate its peak comparison.
2. Warm a fixed workload until lazy caches have been created. Record initialization as a
   separate phase. Start with two warm-up and five measured cycles in bounded PR qualification, and two
   warm-up and 25 measured cycles for a manual extended run. These are proposed workload
   bounds, not memory limits. Freeze counts before comparing builds;
   retain each cycle rather than only the last value or a whole-process maximum.
3. Sample current process memory at quiescent pre-cycle, post-work and post-cleanup points.
   Keep platform-native metrics and units explicit. Record parent/worker PIDs separately;
   do not sum shared resident pages and call the result unique process-tree memory.
4. Reach cleanup through real completion/retirement signals and bounded event-loop drains,
   not arbitrary sleeps or global cache purges. Capture history, task/process, QObject,
   cache-ID and disk-generation ownership observations at the same boundary. Compare both
   the ordinary retained-cache state and an explicit user action that clears its owner.
5. Record peak, raw current post-cleanup samples, adjacent/first-to-last deltas, trend against
   cycle number, and resource counts together. Investigate
   persistent growth after warm-up and teardown with allocation/lifetime instrumentation;
   separate intended bounded history/cache capacity from unreachable retained ownership.
   Do not force allocator trimming in the primary run or require resident bytes to return
   exactly to startup values.
6. Require the workload's saved-state/pixel/identity assertions before interpreting memory.
   A crash, timeout, skipped cleanup or incorrect output is a failed scenario, not an improved
   memory result. Preserve the original failure before a separate diagnostic run.
7. For each proposed fix, retain an exact baseline or narrow mutation that detects the
   ownership defect and an exact candidate run with unchanged correctness. Use repeated
   runs on matched hardware to establish an appropriate noise envelope before setting
   byte/percentage gates; no universal RAM threshold is established here.

The current-memory samples and combined report above are proposed additions, not existing
APIs or command flags. Keep telemetry limited to test/diagnostic paths and existing fixture
boundaries; source ownership fixes should remain narrow and separately reviewable.

## Proposed current-memory metrics

These additions need implementation and qualification. Missing or unsupported values must
report availability/error, not zero. Keep distinct OS meanings rather than normalizing unlike
metrics into one "RAM" value.

| Platform | Proposed current values, alongside the existing peak | Interpretation/source |
| --- | --- | --- |
| Windows | `PROCESS_MEMORY_COUNTERS_EX.WorkingSetSize` and `PrivateUsage` | Working set is current residency; private usage is committed private bytes, not resident bytes. [Microsoft structure contract](https://learn.microsoft.com/en-us/windows/win32/api/psapi/ns-psapi-process_memory_counters_ex). |
| Linux | One `/proc/self/smaps_rollup` read per phase: Rss, Pss, Private_Clean and Private_Dirty; swap separately if used | Pss apportions shared pages. Do not silently substitute VmRSS for an unavailable Pss measurement. [Kernel proc documentation](https://www.kernel.org/doc/html/latest/filesystems/proc.html). |
| macOS | Checked `task_info(TASK_VM_INFO)` resident_size and phys_footprint, with returned structure-count validation | Preserve the footprint name and ABI availability; it is not equivalent to Windows private commit or Linux private residency. [Apple task-info ABI](https://github.com/apple-oss-distributions/xnu/blob/main/osfmk/mach/task_info.h). |

## CI route and evidence record

The Linux, macOS and Windows application jobs already invoke the native driver and artist
benchmark. Add bounded memory-lifetime cases as steps within those existing jobs when
implemented, preserving the maximum of ten baseline PR validation jobs. Retain each scenario's
result and artifacts even if another step fails. A step added to a workflow is still pending
until its exact binary reaches the named checkpoint.

The current Linux SeExpr graph has ASan/UBSan with explicit leak detection; the standalone
GLB graph has sanitizer compilation. Neither instruments the full native application. Begin
with bounded ownership targets and their contributing implementation libraries, then qualify
any expanded Qt/GL graph separately. ASan/UBSan instruction checks require instrumentation
of the implementation code. Standalone LeakSanitizer instead intercepts allocations and
checks reachability at runtime; a fixture linked with LSan can detect allocations leaked by
uninstrumented libraries, subject to allocator-interception and reachability limits.
[GCC documents its allocator interception](https://gcc.gnu.org/onlinedocs/gcc/Instrumentation-Options.html).
[LeakSanitizer supports Linux and macOS](https://clang.llvm.org/docs/LeakSanitizer.html),
but the selected compiler/runtime must pass an exact leak-restoring mutation before relying
on that fixture's coverage. Windows PageHeap/CDB remains corruption diagnosis, not general
leak detection.

Retain symbolized reports and original failures. Do not add blanket suppressions for caches
or third-party allocations. A proposed negative control should retain one fixture payload or
skip one retirement and fail the relevant owner assertion/report; no such mutation has run.
Process-exit leak checks do not replace in-session checks of reachable global cache owners.

Use scheduled/manual qualification for longer sessions, additional Qt/backends/devices and
expensive heap instrumentation. Keep ordinary and instrumented results separate because
instrumentation changes allocation and timing. Existing paired benchmark bundles can provide
matched scene-IO peaks; they do not replace an in-process repeated teardown series.

An evidence record should include revision/build identity, platform/metric source and units,
fixture and input hashes, cycle/warm-up counts, preferences, process IDs, phase samples,
resource/ownership counts, output-correctness result, peak and post-cleanup series, exit status,
CI URL and retained artifact, baseline/mutation result, and remaining limitations. Omit local
user paths and private artwork. No numeric improvement, leak freedom, complete memory audit,
or release qualification is claimed before that evidence exists.

## Separate PR 133 pressure-policy review

[PR 133](https://github.com/OpenAnimationLibrary/groundzero/pull/133) was reviewed open and
unmerged at `5462fb53f6db6ce7c960186202974c67a59d8b20`. Its change is confined to
`common/tsystem/tsystempd.cpp`; macOS/Linux `memoryShortage()` still returns false in the
current audit branch. With the big-memory manager disabled, this boundary affects image-cache
compression/spill. It concerns pressure policy, not proof of ownership release or leak freedom.
No PR 133 production code is copied into this documentation patch.

Two source-review items require resolution before adoption:

- The proposed macOS path acquires `mach_host_self()` send rights twice and does not release
  them, including on host-statistics failure. Acquire one right, release it on every exit,
  check page-size retrieval and add a bounded repeated-call send-right count fixture. This
  is a resource-lifetime observation; no RAM growth was measured.
  [Apple documents the returned send right](https://github.com/apple-oss-distributions/xnu/blob/main/osfmk/man/mach_host_self.html).
- The Linux fallback `memAvailable ? memAvailable : memFree` conflates an absent field with a
  present zero. Parser cases should distinguish present-zero, missing, malformed/truncated
  input, field ordering and threshold edges. Inject readings at the OS boundary; do not exhaust
  a shared runner to simulate pressure. Then use a bounded spill/reload case preserving pixels
  and checked-out owners. Container limits and the macOS availability heuristic remain
  policy questions; the proposed 15% threshold is not a leak gate.

These are review-required items in a separate open PR, not prepared fixes or runtime findings
in this register. New ownership findings from the active memory source lanes must be
recorded with their actual repair/fixture status before they change that register.

Unfixed source boundaries are R17 plugin nested storage/per-host UI, R18 cache telemetry and
R20 loader controller retirement. F49 controller work is a later batch. Nested malformed
TIStream allocation remains F28's explicit residual. No all-workflow leak freedom is claimed.
