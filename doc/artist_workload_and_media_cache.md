# Artist workload evidence and media cache management

This change covers improvement ledger items 59 and 60 on edition baseline
`aa01975f38d6226156e5b07df7271b8fc199d003`. Source implementation and CI
qualification are separate: no performance result or platform pass is implied
by this document. Builds and tests run in the repository CI workflows.

## Native artist workload benchmark (59)

The existing isolated native application runner has an `--artist-benchmark`
mode, compiled only with `BUILD_UI_PORT_TESTS`. It starts the real application
with a disposable copy of its data and an empty scene. It constructs 24 actual
vector drawings (96 strokes each) and 24 full-color raster drawings (512 square
pixels), then measures:

* Twelve pressure traces, each with 96 input points, through the native vector
  brush and its normal input modifiers, committing actual undoable strokes.
* Pen commit to completion of native viewer framebuffer rendering.
* A deterministic nonsequential frame-navigation pass and sequential frame
  stepping with the application's playback state enabled.
* A separate 50% scalar-opacity vector pass, with a render-change assertion and
  saved-opacity checks, to expose the cost of translucent compositing.
* A separate native opacity-envelope pass, with the same scene, geometry and
  scalar opacity, plus independent PLI envelope round-trip checks. This records
  the additional per-pixel compositing cost in the combined vector workflow.
* Three complete drawing-resource and scene saves. All drawings are marked
  modified before each sample; this cannot collapse into metadata-only saves.
* Process peak resident memory, with platform-specific counters.

The fixture independently reopens saved drawings through public image I/O,
verifies native brush stroke counts, raster pixels and scene exposure length,
and records a checksum of rendered framebuffers. It records raw samples,
median/p95, Qt/platform/architecture, actual GL renderer and viewport size.
Native brush option values are included so changed defaults are visible when
comparing runs. The centered artwork must change on every exposed frame in
both navigation passes; a blank or stale framebuffer cannot pass this check.
The runner adds build revision and CI run/attempt provenance. Every platform
uploads the JSON report, raw native output and workflow diagnostic output.

These are synthetic workloads using real application paths, not physical
tablet-to-display latency measurements. Sequential frame stepping measures
rendering capacity, not timer pacing or audio synchronization. CI software
rendering and hosted hardware are not representative of every artist's
workstation. No hard speed threshold is imposed on shared runners; correctness
failures and missing reports fail CI. Compare distributions only for the same
fixture version, workload, viewport, renderer and comparable hardware/build
settings. Use multiple runs and retain their revisions before claiming a speed
improvement. Framebuffer checksums are diagnostic, not cross-platform golden
images. Peak resident memory includes startup and is not proof against leaks.

## Media cache workflow (60)

File > Manage Media Cache shows cached bytes and protected in-use generations,
allows a persistent MiB budget, and offers explicit unused-media cleanup.
Zero disables automatic cleanup. Saving a positive budget also cleans toward
that budget; automatic cleanup runs in a coalesced background job after readers
close. Open readers may keep usage above the requested budget. Refresh and
cleanup run outside the UI thread. Cancellation prevents new retirements;
already retired files are removed, and a saved policy remains saved. Closing a
busy dialog requests cancellation and closes when the worker finishes.

Each FFmpeg reader registers an independent process lease before reading cache
metadata, validating images or starting extraction. The lease lives through
its final image read. A short shared management gate makes lease registration
and retirement mutually exclusive. Cleanup rechecks leases and extraction
locks under this gate, atomically renames an unused generation to a unique
retired directory, then removes only that directory outside the gate. A new
reader can recreate the original generation while retirement finishes.
Dead-process leases are recoverable; live leases never expire merely by age.
Cleaners serialize their inventories under a separate process lock so concurrent
budget passes cannot evict against stale totals. A reader-close request that
arrives during an automatic pass causes a fresh pass before the worker exits.
Interrupted retired directories are counted and retried on later cleanup.

Partial extraction directories count toward usage and remain protected while
their reader or extraction worker is alive. Abandoned partial directories are
eligible for the same atomic retirement after a crash. The manager ignores
source movies, export scratch files, unknown layouts and symlinks. Startup/shutdown no longer delete
the shared FFmpeg cache; the generic Clear Cache command routes this category
to the manager. Disk cleanup is intentionally not document undo: cached frames
are regenerated from unchanged sources, and active generations are protected.
This protocol is honored by current versions; older application versions that
delete the shared cache do not participate in it.

Coverage is registered as `ui_mediacache`, actual shared FFmpeg reader cleanup
and regeneration in `tiio_ffmpeg`, and real menu/dialog interactions in the
native application integration test. It includes concurrent readers, a second
process, a killed reader, stale-lease recovery, cancellation, repeated cleanup,
interrupted retirement, policy persistence, disabled automatic cleanup and
coalesced background cleanup. Native UI checks cover budget apply/reopen and
canceling destructive cleanup. All three platform workflows require the
registered test and retain normal existing UI/regression suites.

## Acceptance status

| Item | Source state | Qualification still required |
| --- | --- | --- |
| 59 | Native benchmark, correctness checks and provenance/artifacts implemented | Exact published-head CI on Linux, Windows and macOS; review metric limits against reports |
| 60 | Shared-reader protection, asynchronous manager and background budget implemented | Exact published-head protocol, real-reader and native-dialog tests on all three platforms |
