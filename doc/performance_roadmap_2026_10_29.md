# Groundzero reliability and performance roadmap

Review date: October 29, 2026. Reviewed source: `edition` commit
`6ba3cc31da7022472e0d6a982f03c1348de69b1f`.

This roadmap follows the [codebase audit](codebase_audit_2026_10_29.md).
It covers the application broadly, with **Windows first and macOS second**.
Linux remains useful for existing regression, sanitizer and compatibility jobs.
The phases below describe the full program. The implementation table records
the work in [PR #103](https://github.com/OpenAnimationLibrary/groundzero/pull/103);
unmeasured and unvalidated work is identified explicitly.

## Implementation progress

The `improve/app-reliability-performance` branch starts this plan from integration
commit `3a2b9f6dc56a55a9f04f432dc00241020397b6e4`. That base already includes
the PDF/LUT responsiveness changes associated with A19. The original audit remains
a record of its earlier pinned snapshot.

| Work | Implementation in this batch | Validation status |
| --- | --- | --- |
| UI scaling | A17 gathers used levels once per rebuild; A18 hashes selected level identity | Identity/ownership regression added; interactive timing still required |
| Cadmium | A07 includes outline attributes in freshness checks; A20 updates only the reference overlay; pixel-type dispatch moves outside pixel loops | Existing vector/review tests expanded; native CI execution required |
| Save and recovery | A01 preserves parent dirty state; A02 uses checked temporary palette replacement; A03 restores failed-save identity; A04 shares compressed-format limits; A05 retries partial recovery with bounded retention | Native save-failure/retry and stream cases added; child-export/recovery UI scenarios still required |
| Navigation | A06 records the actual matched child-sheet coordinates | Application undo/redo scenario remains required |
| Cache and resources | A08 removes the correct raster identity; A10 gives imported media full content identities and complete extraction manifests; A15 manages PSD/video/export owners | Address-reuse, same-path replacement, interrupted-cache, overlapping-reader and ownership regressions added |
| Export completion | A09 protects shared FFmpeg metadata; A11 records actual failed frames and delivers terminal status; A12 explicitly checks finalization and discards aborted FFmpeg output | Concurrent export, ordered failure, updater lifecycle and failure/abort regressions added; public export/decode smoke required on all three platforms |
| PSD fidelity | A14 expands bitmap bits and preserves gray16 alpha; sampled crop/shrink coordinates and partial-parser cleanup are corrected | Independent native sample fixtures required on Windows, macOS and Linux |
| Cadmium memory | A16 accounts retained payloads, reserves analysis headroom and evicts inactive inspection caches while preserving artist choices | Small-budget admission/eviction tests added; mandatory recovery metadata and transient/native/worker allocations are not a hard RSS cap |
| Raster safety | A13 rejects invalid/overflowing storage and fails allocation explicitly; Canvas Resize prepares replacements before edits | Boundary/stride/view tests added; allocator fault injection and UI scenarios remain |
| Test reliability | V01–V04 tighten exception/golden/graphics checks; required subsets run on ordinary Linux, Windows and macOS workflows | Registration runner rejects missing/skipped expected cases; CI results must be reviewed |

The next performance batch is included in the same PR:

| Path | Additional implementation | Evidence and remaining validation |
| --- | --- | --- |
| Sparse scene saves | Compute the trailing-empty-column boundary once per save and reuse it for columns, pegbars and FX | Sparse plain/compressed fixtures check identical serialized content and round trips; paired timing remains required |
| Image cache | Use the recorded history key for explicit compression; discover resident icon IDs through their ordered prefix range under the cache lock | Address reuse, refreshed/remapped history, prefix boundaries and aliases are covered; alias updates still scan the alias map |
| Media import and cleanup | Combine metadata probes, reuse codec metadata, avoid repeated validation/locking for complete generations, remove filename sorting and a full-image mirror pass, reduce file-system probes | Probe-count, metadata fallback, source-switch, cache-lock and exact-pixel cases are added; full source hashing and manifest validation remain |
| Cadmium full-color paint | Read typed alpha directly, reuse paper/display samples and source reads, and convert accepted fill colors once per region | Independent 8/16/float fixtures cover thresholds, alpha and color spaces; transient fill storage adds 32 bytes per region |
| Render admission | Admit contiguous output windows in one renderer lifecycle; split aliases across windows and preserve queued/running task ownership when submission fails | Budget arithmetic, executor failure boundaries, ordering and native render lifecycle regressions are required; profile the throughput cost of recomputing aliases across windows |
| Performance measurement | Versioned sparse/dense scene metadata workloads, immutable benchmark bundles and an opt-in paired Windows/macOS harness | Raw samples, median/p95 and qualified native memory counters are recorded; compatible baseline/candidate runs and relocated bundle execution remain required |

See [paired measurements](paired_benchmarks.md) for workload matching, provenance,
native memory-counter scope and workflow availability. The harness does not
measure interactive drawing, playback or rendering, and ordinary PR checks do
not impose timing thresholds. The linked PR records current CI results; added
fixtures and static review alone are not execution evidence.

This PR does not complete all phases or establish a measured speedup. CI results
and remaining application validation are tracked in
the linked PR. Windows/macOS reference-machine profiles remain outstanding.
Canvas Resize preparation improves failure handling
but pins replacement drawings until commit and can raise peak residency; measure
large multi-drawing resizes before release. Render budget tuning and measurement,
budgeted disk-cache eviction, Cadmium peak-memory instrumentation, and native
interactive failure/cancellation scenarios remain follow-up work.

MovieRenderer opts into a 256 MiB admission target for pooled render outputs and
their handoff clones; other TRenderer consumers retain unrestricted dispatch by
default. One oversized frame is admitted alone. FX intermediates, predictive
caches, postprocessing, writer/encoder buffers and total process RSS are outside
this target. Alias clusters cannot retain a previous window's output while
waiting for later-window frames, but splitting them can increase repeated
computation. Measure long sequences, repeated exposures, stereo and fields before
tuning the default. Native thread-start failures and total-process allocation
failure still need broader platform qualification.

The FFmpeg import cache remains shared across readers; its files are not deleted
when one reader closes. This preserves concurrent-reader correctness while owner
objects are now released. Full-source hashing prevents same-path/same-timestamp
stale reuse, but adds sequential input reads. Export publication stages output
and copies it once before replacement, increasing disk traffic and temporary
space. Profile both costs; do not count these correctness changes as throughput
improvements. Budgeted disk-cache eviction remains Phase 2 work. Before enabling
deletion, add reader leases that cover in-flight frame loads across processes and
atomic retirement of unleased generations. A valid-generation fast path alone
does not make concurrent eviction safe.

Movie output preservation applies per file before its commit. Stereo and
multimedia jobs are not one atomic transaction across all outputs: if one file
has committed before a later file fails, it cannot be rolled back by aborting the
remaining writers. Image sequences can retain completed frames on cancellation.
End-to-end UI/CLI cancellation, including renderer startup and finalization,
still needs native smoke qualification in addition to the focused regressions.

Additional review fixed truncated compressed-header reads, transactional studio
palette saving, a studio-palette load exception leak, and palette style-alias
ownership. Focused regressions cover malformed headers, serialization failure,
retry/preservation and exactly-once style destruction. Windows test consumers
now import the library's DLL symbols instead of inheriting its export definition.

Cadmium's 512 MiB admission target protects retained browsing/analysis payloads;
it is not a total-process memory guarantee. Fresh recovery state remains
mandatory even when metadata exceeds that target. Preserving artist decisions
takes priority over refusing an autosave snapshot. Parse/flush temporaries,
native caches, GL storage and inference workers need separate measurement.

The audit was a source review. No local builds, application tests or performance
measurements were run: [CLAUDE.md](../CLAUDE.md) directs builds and tests through
CI. Source evidence establishes defects and candidate costs; it does not establish
a measured speedup. Reproduce each finding in CI or an application test before
calling its fix validated. Existing documents may describe earlier branches, so
use the pinned source and current results when implementing this plan.

## Objectives and decision rules

- Preserve drawings, palettes, animation, scene compatibility and rendered output.
- Make failures recoverable: cancellation, allocation failure, failed writes,
  worker exits and device loss must leave documents usable.
- Reduce latency, repeated work and peak memory in representative artist workflows.
- Optimize measured costs first. Compare candidate and baseline on the same
  hardware, fixture and settings; include cold and warm results.
- Prefer focused changes with regression evidence. Keep optional format changes,
  GPU redesigns and broad threading changes behind separate design decisions.

There is no universal speed multiplier or completion date. Each phase exits when
its evidence is sufficient, and its measurements determine the next priorities.

| Phase | Audit findings to close or measure | Primary deliverable |
| --- | --- | --- |
| 0 | V01–V05 | Reliable correctness gates and Windows/macOS baseline |
| 1 | A01–A07, A09, A11–A12, A14 | Preserved edits and truthful save/export outcomes |
| 2 | A08, A10, A13, A15–A16 | Correct cache identity, allocation, ownership and budgets |
| 3 | A17–A20 | Responsive UI and removal of demonstrated repeated work |
| 4 | Render queues, cache locks, video probing and cleanup candidates | Measured render/media throughput within memory budgets |
| 5 | Drawing, pixel and remaining Cadmium profiling candidates | Measured improvements in artist workflows |
| 6 | Supported-platform evidence for all selected changes | Qualified Windows/macOS packages |

## Phase 0 — Trustworthy gates and a reproducible baseline

**Dependencies:** none. Establish this foundation before performance changes.

Make the scene benchmark fail on construction, save and load exceptions, and on
missing required comparisons. Strengthen its round-trip checks beyond counts and
frame-number sums. Make missing FX golden images fail regression mode; generate
candidate references separately and review them before acceptance. Require a
working OpenGL context and an expected image manifest in rendering compatibility
jobs. Keep serialization-only checks explicitly separate.

Run fast regression tests on ordinary PRs. Add affected expensive suites and a
scheduled/release full suite: the current broad Linux workflow only runs manually
or on selected branches. Pin the compatibility baseline commit. Extend Windows
coverage beyond the existing cel/Cadmium tests and add macOS runtime tests.

Build a versioned fixture corpus containing real drawings, nested xsheets, audio,
palette and Plastic content. The existing `scenebench` deliberately omits drawing
files; retain it for serialization diagnosis, then add end-to-end measurements.
Archive traces, configuration, output hashes and machine-readable timings.

**Exit gate:** deliberately missing goldens, failed save/load, missing graphics
context and absent required tests each fail their intended job. Record a complete
baseline on Windows and macOS, with all skipped capabilities explicitly listed.

## Phase 1 — Document, undo and output integrity

**Dependencies:** the relevant Phase 0 correctness gates; establish focused
reproductions first. Urgent integrity fixes can proceed while the broader fixture
corpus and performance baseline are being assembled.

Address the audit's save, undo, export and Cadmium stale-vector findings before
speed work in those paths. Make writes report failure through final flush/close
and replacement; exercise compressed and plain scenes. Keep the previous valid
document/output recoverable when storage fills or a write is interrupted.

Preserve the parent scene's dirty flag when exporting a child; retain palette
dirty flags on failure; restore live path/title state after failed Save As.
Enforce the same compressed-scene limits when writing and reading, before replacing
the last good file. Retry incomplete recovery resources without requiring a new
edit. Correct the PSD bitmap and grayscale-alpha decoding cases with independent
pixel expectations.

Ensure undo targets the original document, level or xsheet after navigation and
scene changes. Balance edit transactions and restore selection/dirty state on
failure. Revalidate Cadmium targets against current geometry and drawing identity
before applying accepted results, including outline attributes such as opacity,
cap and join changed after generation. Store the actual matched child coordinates
for navigation redo. Eliminate shared-state races in FFmpeg multithread export;
track actual failed frames and propagate encoder finalization errors. Canceled or
failed exports must not be presented as completed output.

**Exit gate:** save/reopen, nested-xsheet undo/redo, stale-proposal rejection and
export cancellation cases pass. Fault injection covers short writes, disk-full,
failed replacement and worker termination. Older-scene fixtures retain their
expected animation and images. Explicitly document any accepted output change.

## Phase 2 — Ownership, allocation and cache correctness

**Dependencies:** Phase 0; coordinate transactional behavior with Phase 1.

Centralize checked raster dimensions and byte counts. Reject unsupported sizes
before allocation or pixel access; propagate allocation failure without creating
a usable-looking raster with a null buffer. Audit downstream strides and byte
counts as part of the same contract.

Repair non-Windows cache identity removal consistently across compression paths.
Test address reuse and compressed/uncompressed transitions so different drawings
cannot alias accidentally. Resolve confirmed ownership leaks, then establish
bounded cache policies and correct memory accounting. Fingerprint media source
revisions and require extraction-completeness manifests so same-path replacements
cannot reuse old frames. Apply Cadmium byte reservations to preview inspection as
well as generation, with eviction or spill after use. Include image buffers,
undo history, thumbnails, render intermediates and asynchronous worker resources.

**Exit gate:** boundary-size and allocation-failure tests fail safely; sanitizer
checks cover affected code; cache round trips preserve image identity and pixels.
Repeated import/edit/undo/close cycles show no unexplained upward trend in live
allocations, handles or threads after cleanup. Track RSS separately because an
allocator may retain reusable memory without leaking live objects.

## Phase 3 — Interactive responsiveness and scaling

**Dependencies:** stable transactions and ownership from Phases 1–2.

Profile startup, scene switching, timeline scrolling, scrubbing, selection and
stroke finalization on Windows first. Fix demonstrated repeated scans, unnecessary
copies, duplicate invalidations and work performed per event instead of per edit.
Batch notifications and cache derived values with explicit invalidation rules.

Start with one used-level traversal per filmstrip rebuild, pointer-identity hashing
for Level Settings, asynchronous PDF/LUT jobs, and before/after preview reuse when
only Cadmium's reference overlay changes. These have concrete source evidence;
measure each independently before broader UI changes.

Move expensive independent work off the UI thread only when the data ownership,
snapshot, cancellation and result-validation contracts are defined. Preserve Qt
and graphics-thread restrictions. Measure UI stalls while autosave, thumbnail
generation, rendering and Cadmium jobs run together.

**Exit gate:** representative interactions meet the agreed latency budgets below;
trace evidence attributes improvements to the changed path. Selection, undo,
redraw and cancellation tests pass. Large-scene scaling curves demonstrate that
fixed workloads no longer trigger the identified redundant work.

## Phase 4 — Rendering, export and media I/O throughput

**Dependencies:** Phase 2 memory safety and Phase 3 responsiveness instrumentation.

Measure single-frame and sequence workloads separately. Examine render scheduling,
tile sizes, shared locks, intermediate image lifetime, codec handoff and redundant
conversions. Compare 1, 2, 4 and available-core worker counts before changing
parallelism. Avoid multiplying memory use or oversubscribing libraries that
already use their own threads.

Profile FFmpeg piping/backpressure, PSD/EXR import and export, and scene/resource
I/O. Reduce proven copies and repeated decoding; bound queues and retain prompt
cancellation. Qualify bounded output-window scheduling for out-of-order completions;
profile remaining video cleanup and file-system costs; measure time to first usable video
frame before choosing on-demand decode and prefetch. Require explicit validated
tile/thread-safety behavior before extending frame-strip parallelism.
Evaluate compiler/LTO/PGO experiments separately so their gains and
platform costs remain attributable.

**Exit gate:** paired runs demonstrate throughput improvements beyond noise at
documented memory cost. Required output comparisons pass at supported bit depths.
Cancellation, encoder failure, missing codecs and partial-output handling pass on
Windows and macOS. No unvalidated renderer rewrite becomes a prerequisite.

## Phase 5 — Drawing, pixel operations and Cadmium

**Dependencies:** the earlier correctness gates and measured hotspot ranking.

Optimize the drawing and pixel kernels that dominate real profiles: stroke
matching, region queries, brush/fill processing, masks and image conversion.
Use spatial indexing, incremental computation, reusable scratch storage or
vectorization only where measurements justify them. Include dense vector art,
large raster brushes, high-bit-depth data and repeated exposures.

For Cadmium, measure preprocessing, serialization, worker startup, inference,
review and native application independently. Reuse immutable results only with
complete content identities and bounded memory. Preserve artist review and exact
native paint semantics; stale jobs must not apply to a changed drawing or scene.

**Exit gate:** output/undo tests and stale-result tests pass; measured end-to-end
latency improves rather than merely shifting cost to another stage. Validate
pressure, tilt, eraser and stroke appearance on Windows tablet hardware and the
supported macOS input path.

## Phase 6 — Platform soak and controlled rollout

**Dependencies:** completed evidence for the changes selected for release.

Test packaged Windows builds on supported versions, graphics drivers, tablet
paths, display scaling and memory-constrained machines. Test macOS packaging and
runtime separately; include Apple Silicon if shipping it, since current CI builds
Intel. Exercise audio, camera/stop-motion, fonts, translations and optional Python
or Cadmium components as applicable to the package.

Run sustained production-style sessions, repeated document lifecycle cycles and
concurrent background work. Keep benchmark fixtures, artifacts and traces for
future comparisons. Roll out risky optimizations behind reversible settings when
practical, with an explicit fallback and regression reproduction instructions.

**Exit gate:** supported packages launch and complete representative workflows;
there are no unresolved blocking integrity defects. Memory and latency remain
within agreed budgets throughout the soak. Every remaining limitation is named.

## Benchmark matrix and proposed budgets

Windows release configuration is the primary measurement target; macOS is the
second. Pin machine, CPU/GPU, driver, RAM, disk, compiler, thread count and scene
settings. Use warmup plus at least ten measured repetitions where practical;
retain individual samples and report median/p95 rather than only averages.

| Workflow | Required variation | Measurements |
| --- | --- | --- |
| Launch/open/save/autosave | Small/large real scenes; nested xsheets; compressed/plain; cold/warm | Time to usable UI, total time, UI stalls, peak RSS, bytes read/written |
| Draw/select/fill | Dense vectors; large brushes; 1080p/4K; tablet input | Input-to-display latency, stroke completion, allocations, output correctness |
| Playback/scrub | Repeated/unique drawings; onion skin; FX; cold/warm caches | Frame-time distribution, dropped frames, cache size/hits |
| Render/export | Heavy frame/long sequence; worker counts; 8/16/float; codecs | Throughput, CPU time, peak memory, cancellation latency |
| Lifecycle/undo | Repeated import/edit/undo/close; background jobs | Live allocations, handles, threads, correctness after navigation |
| Cadmium | Drawing size/reference count/batch size; cold/warm worker | Stage timings, total latency, UI stalls, RSS/VRAM, stale-result rejection |

The following are **proposed targets, not measured results**. Ratify them against
the baseline and reference hardware: p95 direct-interaction latency below 50 ms;
warm playback sustaining the selected 24 fps workload (41.7 ms/frame); cancellation
acknowledgment within 250 ms with separately measured time to release resources.
Accept a performance change only when its benefit exceeds measurement noise,
correctness remains unchanged, and its memory tradeoff is explicit. Establish
fixture-specific regression thresholds after measuring normal variance.

## Implementation order and next work

PR #103 combines the initial implementation sequence into one review:

1. Repair benchmark/golden/context gates and preserve diagnostic artifacts.
2. Preserve parent dirty state, palette-save retries and failed Save As metadata.
3. Correct child-sheet navigation redo and Cadmium stale-vector validation.
4. Check raster allocation and repair cache identity and resource ownership.
5. Handle export completion/cancellation and compressed-save/recovery failures.

The implementation table above distinguishes focused regression coverage from
the application scenarios still required. Next, complete the Windows/macOS
baseline corpus and interactive failure/undo/cancellation qualification; measure
memory and latency before selecting further Phase 3–5 optimizations. Render
budget qualification, disk-cache eviction and allocator fault injection remain open.
Link subsequent work to its audit finding, regression case, baseline/candidate
results and remaining application testing. Build success alone is not a runtime
or performance result.
