# Groundzero correctness and performance audit

Reviewed snapshot: `6ba3cc31da7022472e0d6a982f03c1348de69b1f` on `edition`, October 29, 2026. Windows is the primary target, macOS second; Linux remains useful for automated checks and sanitizers. The companion [phased roadmap](performance_roadmap_2026_10_29.md) turns these findings into implementation work.

The highest priority is preventing loss of edits and incorrect output. Save-state handling, sub-xsheet redo, vector paint freshness, raster allocation, media export, and the macOS image cache have actionable defects. Several performance costs are also avoidable: repeated scene traversal, colliding level hashes, synchronous external processes, redundant paint previews, and unbounded retained work. Optimizing those paths must preserve image output, undo behavior, and existing scene compatibility.

This is a source and control-flow audit, not a completed optimization or a measured performance result. Findings marked **source-confirmed** have an identifiable defective operation and a traced caller or invariant; their reproduction recipes have not been executed. **Profiling candidates** identify plausible costs without asserting a measured bottleneck or speedup. P1 means prioritize before performance refactoring because of crash, edit-loss, or incorrect-output risk; P2 means a narrower correctness, resource, responsiveness, or validation issue. These are engineering priorities, not externally assigned vulnerability ratings.

## Scope and validation

Six review tracks covered scene/resource persistence and recovery; undo and editing/UI paths; rendering/cache/export; Cadmium adapters and lifecycle; image allocation/PSD loading; and CI/test/benchmark reliability. Review included callers and existing guards, not just suspicious statements. The snapshot contains 2,553 C/C++ source/header files under `toonz/sources`; this review does not claim line-by-line coverage of all of them, every FX, bundled third-party code, or every platform path.

No application builds, tests, profilers, or artist workflows were run locally. [CLAUDE.md](../CLAUDE.md) directs builds and tests through CI. The inspected [Linux Build for this exact commit](https://github.com/OpenAnimationLibrary/groundzero/actions/runs/40808010682) completed successfully. That existing result does not reproduce the failure cases below, cover the whole optional test suite, or establish Windows/macOS runtime readiness. The remote branch advanced during the audit; conclusions remain pinned to the reviewed commit.

Code references below use repository-relative paths and line numbers from the reviewed snapshot. Reproduction steps are proposed regression specifications, not executed test results. Some issues are already acknowledged in repository notes; those are identified rather than presented as new discoveries.

## Findings at a glance

| ID | Priority | Finding | Main platform scope | Roadmap work |
| --- | --- | --- | --- | --- |
| A01 | P1 | Child export clears parent scene dirty state | All | Save transactions |
| A02 | P1 | Failed palette save clears dirty flags before throwing | All | Save transactions |
| A03 | P1 | Failed Save As changes live scene path/title state | All | Save transactions |
| A04 | P1 | Compressed writer can save a scene the reader rejects | All, large scenes | Serialization contracts |
| A05 | P2 | Incomplete recovery snapshots suppress resource retries | All | Recovery completeness |
| A06 | P1 | Sub-xsheet redo records the wrong coordinates | All | Undo correctness |
| A07 | P1 | Vector paint proposal can overwrite newer stroke attributes | All with Cadmium | Native edit freshness |
| A08 | P1 | Cache compression leaves stale raster identities | macOS/Linux | Cache correctness |
| A09 | P1 | FFmpeg multithread export mutates shared state unsafely | All using that preference | Export concurrency |
| A10 | P1 | Replaced videos can reuse obsolete frames/metadata | All | Media cache identity |
| A11 | P1 | Failed-frame sentinel blocks ordered export | All, sequential output | Export completion |
| A12 | P1 | Encoder finalization errors do not fail the sequence | All | Export completion |
| A13 | P1 | Allocation failure creates usable-looking null rasters | All | Allocation invariants |
| A14 | P2 | PSD bitmap and gray-alpha pixels decode incorrectly | All | Import fidelity |
| A15 | P2 | PSD/video/export ownership leaks accumulate | All | Memory ownership |
| A16 | P2 | Cadmium preview browsing bypasses aggregate memory limit | All with Cadmium | Bounded residency |
| A17 | P2 | Filmstrip rebuild repeats whole-scene traversal | All | UI scalability |
| A18 | P2 | Level Settings hashes all live levels identically | All | UI scalability |
| A19 | P2 | PDF and LUT jobs synchronously block the GUI | All with external tools | Async jobs |
| A20 | P2 | Reference-overlay changes rebuild unchanged paint previews | All with Cadmium | Preview reuse |
| V01 | P1 | Scene benchmark can pass after exceptions/data loss | CI | Trustworthy gates |
| V02 | P1 | Missing FX goldens pass and can be auto-promoted | CI | Trustworthy gates |
| V03 | P1 | Routine PR checks omit most optional regression suites | CI | Coverage |
| V04 | P2 | Graphics compatibility can pass without rendered pixels | CI | Coverage |
| V05 | P2 | macOS runtime/architecture coverage is incomplete | macOS/CI | Platform qualification |

## Save, recovery, undo, and native edits

**A01 — Child export clears the parent scene's unsaved state. Source-confirmed.**

`SaveSubSceneAsPopup::execute()` in `toonz/sources/toonz/filebrowserpopup.cpp:649` invokes `IoCmd::saveScene(..., SAVE_SUBXSHEET)`. The scene writer restores the parent path after writing the child, but `toonz/sources/toonz/iocommand.cpp:2072` unconditionally clears the active scene dirty flag. `saveSceneIfNeeded()` checks that flag at line 1723. Parent-only timeline/key edits can therefore lose their close/open save prompt after a child export; separately dirty drawings do not protect those parent-only changes.

Fix by preserving parent dirty state for child exports and clearing it only after a successful full-scene save. Regression: save a parent, edit only its timeline, export a child, then attempt to open another scene. Verify the parent remains dirty and prompts; also cover an initially clean parent.

**A02 — Failed palette persistence clears the dirty flags before reporting failure. Source-confirmed.**

The existing-TLV, palette-only branch at `toonz/sources/toonzlib/txshsimplelevel.cpp:1755` sets `paletteNotSaved` when opening the `.tpl` fails. Lines 1856–1859 clear level and palette dirty flags before the exception at 1862–1864. `SceneLevel::save()` at `toonz/sources/toonzlib/sceneresources.cpp:344` uses those flags to decide whether an existing same-path resource needs saving. A retry can skip the unsaved palette. The branch also checks stream status before serialization, not after write/flush completion.

Use a checked temporary-file/replace transaction and clear dirty flags only after success. Regression: inject open, write, flush, and replace failures for a palette-only edit; retain old file bytes and dirty flags, then verify a retry persists the new colors.

**A03 — Failed Save As changes the active document's path and untitled state. Source-confirmed.**

`toonz/sources/toonzlib/toonzscene.cpp:759` calls `setScenePath(newScenePath)` before read-only checks, temporary-file creation, serialization, and replacement. `setScenePath()` clears untitled state at line 386. The restoration at line 788 is only reached after a successful child save. `IoCmd` catches the exception and returns failure without rolling those fields back (`iocommand.cpp:2035–2058`). Subsequent saves and `$scenefolder` resource resolution can use the failed destination.

Guard path/title/project-related metadata transactionally: restore on failure and on every child export; commit only after successful full-scene replacement. Regression: titled and untitled scenes, full and child saves, failures at each I/O stage, and assertions about alias resolution and the next ordinary save destination.

**A04 — A successfully saved compressed scene can exceed its reader's limits. Source-confirmed.**

The writer in `toonz/sources/common/tstream/tstream.cpp:290–312` compresses and writes sizes without the reader's bounds, narrowing sizes into `TINT32`. The reader at lines 896–907 rejects compressed payloads above 100,000,000 bytes and uncompressed scene text above 256 MiB. `checkCompressedSceneFile()` at `toonz/sources/toonzlib/toonzscene.cpp:197` checks header/physical length but not those limits; the old scene may then be replaced at line 786. A legitimate scene crossing a limit can save successfully and fail to reopen.

Share format limits and validate before replacement. Reject safely or use a supported uncompressed fallback; do not simply remove loader allocation limits. Regression: boundary cases with injectable small limits, preservation of the previous file, and the invariant that every successful save can reopen.

**A05 — Partial recovery snapshots stop retrying failed drawing copies. Source-confirmed.**

`toonz/sources/toonz/autosaverecovery.cpp:550–556` records an uncopied resource but, if scene and manifest writes succeed, lines 581–583 clear `s_changed` and return success. The next timer save exits at line 487 until a new edit arrives. A transient drawing-copy failure that later clears is never retried if the artist makes no further changes, leaving recovery with an older drawing.

Track pending resources independently from scene edits and report partial versus complete snapshots. Regression: first resource-copy attempt fails, next interval succeeds without an intervening edit; the next recovery snapshot must include the unsaved drawing.

**A06 — Redo can fail to reopen a child sheet and direct later edits into the parent. Source-confirmed.**

`OpenChildUndo` captures the playhead/current column (`toonz/sources/toonz/subscenecommand.cpp:1018–1019`). Opening may instead find a child elsewhere through the column fallback at 1115–1118 or selection search at 1141–1147. The matched coordinates are not passed to the undo entry at 1164. Redo at 1036 tries the stored, potentially empty cell and ignores failure. Later current-xsheet undo entries, such as `drawingsubstitutionundo.cpp:59`, can operate on the parent.

Store the actual matched coordinates and validate sheet identity/reopen success. Regression: child at frame 10, playhead at empty frame 1, select column, open child, edit, undo edit/navigation, redo navigation/edit. Assert child navigation and unchanged parent cells. Cover multiple children and selection-range opening.

**A07 — Applying a vector paint proposal can revert intervening stroke attributes. Source-confirmed.**

Cadmium's vector fingerprint at `toonz/sources/toonz/cadmiumvector.cpp:45–79` omits stroke outline options, including opacity, cap/join, and miter values. Freshness checks at `cadmiumpopup.cpp:868–870` compare that fingerprint. `applyVector()` clones the old proposal snapshot at `cadmiumvector.cpp:314`, and application replaces the drawing (`cadmiumpopup.cpp:895–924`). Pump opacity edits mutate the native image without replacing the builder identity (`tnztools/pumptool.cpp:1151`, `toonzlib/txshsimplelevel.cpp:314`). Thus an opacity edit between generation and application passes freshness checks but can be overwritten by the old snapshot.

Include the complete relevant native state in revision/fingerprint checks and prefer applying fills to a validated current image. Regression: generate, edit opacity/cap/join, then apply; reject stale proposals or preserve the edit. Reference changes must also invalidate analysis.

## Rendering, caching, import, and memory

**A08 — Non-Windows cache compression erases the wrong identity key. Source-confirmed and independently reviewed.**

`getPointer()` at `toonz/sources/common/tcache/timagecache.cpp:769` returns the raster address for raster images; insertion uses that key at 1270. Three non-Windows paths erase the image-object address instead, at lines 840, 928, and 1047. The Windows branches use the correct helper. Once the old raster is released, its stale reverse-map entry survives. If its address is reused, `add()` at 1242–1247 aliases the new drawing to the old cache ID instead of retaining the supplied image. The result can be wrong or missing artwork. This depends on compression and allocator address reuse; frequency is not measured.

Use one insert/erase identity convention across all branches and assert reverse-index invariants. Regression: full-color and Toonz Raster compress/remove/reallocate/get cycles plus direct invariant checks. Qualify on macOS as well as Linux before release.

**A09 — FFmpeg multithread export reaches unsynchronized shared mutation. Source-confirmed.**

Rendering workers deliver completion directly (`common/tfx/trenderer.cpp:1048,1222`). With the multithread FFmpeg preference, `toonzlib/movierenderer.cpp:549–550` uses the format's sequential requirement; MP4/WebM/APNG are not in the sequential-only list (`include/tlevel_io.h:261`). The lock is released around `saveFrame()` at `movierenderer.cpp:652`. The fallback FFmpeg writer bypasses its pipe mutex (`image/ffmpeg/tiio_ffmpeg.cpp:546–551`) and concurrently updates counters/dimensions and its cleanup `QVector` (`:102–104,198`).

Make per-frame values local and synchronize shared state; design an ordered, bounded output stage before expanding concurrency. Regression: concurrent MP4 exports with this preference enabled, ThreadSanitizer where supported, and decoded frame-count/content checks. Preserve Windows native stress coverage even when the sanitizer run uses Linux.

**A10 — Media cache identity ignores source changes and has only 24 hash bits. Source-confirmed.**

`image/ffmpeg/tiio_ffmpeg.cpp:89–98` derives its prefix from six hex digits of a path-only hash. `getInfo()` trusts cached metadata at 730–743 and extraction checks a cached first frame at 949–959. Replacing a movie at the same path can reuse old dimensions, duration, and images. Unrelated paths can also collide; extraction completeness is not established by one frame's existence. MP4 extraction cleanup is disabled at `image/ffmpeg/tiio_mp4.cpp:200`.

Use versioned source identities, freshness checks, extraction manifests, atomic publication, and separate per-export temporary namespaces. Regression: same-path replacement with different pixels/resolution/duration, interrupted extraction, deliberate key collision, and simultaneous readers.

**A11 — Render failure inserts a frame-zero sentinel regardless of the failed frame. Source-confirmed.**

`toonzlib/movierenderer.cpp:785` inserts an empty raster at key `0.0`. The sequential flush at 791–793 stops if that is not the next expected frame; the success path's corresponding check at 616–618 also stops. A later-frame failure or a nonzero render range can block subsequent writes and retain completed rasters. Terminal completion is suppressed when `m_failure` is set (845–849), while the normal dialog closes through completion (`toonz/rendercommand.cpp:455`).

Represent real completed/failed/canceled frame states and always deliver one terminal sequence result. Regression: inject failure at first/middle/last frames for zero and nonzero ranges, check bounded memory, written-frame policy, progress closure, and cancellation.

**A12 — Encoder finalization failure is not part of sequence success. Source-confirmed.**

`Ffmpeg::finishPipe()` warns after failure/timeout but returns true meaning that a pipe was used (`image/ffmpeg/tiio_ffmpeg.cpp:630–650`). MP4 finalizes in its destructor (`tiio_mp4.cpp:60–69`). `MovieRenderer::onRenderFinished()` destroys writers, then calls sequence completion unless an earlier rendering failure was recorded (`toonzlib/movierenderer.cpp:842–849`). The legacy intermediate-file path likewise lacks a returned encoder-success status. A warning can coexist with a successful completion notification and a missing/truncated output.

Add explicit fallible finalization and structured terminal errors; publish a temporary output only after successful encoder exit and validation. Regression: failed startup, nonzero exit, timeout, disk-full, cancel, and preservation of an existing destination.

**A13 — Raster allocation failure violates the object invariant; size arithmetic can overflow. Source-confirmed and independently reviewed.**

`TBigMemoryManager::putRaster()` computes bytes by multiplying three `int` values into a `TUINT32` at `common/tsystem/tbigmemorymanager.cpp:260`. This can overflow before conversion. Separately, ordinary allocation failure is returned at 303, but `TRaster` ignores that result and returns from construction with a null buffer (`common/traster/traster.cpp:41–55`). The smart-pointer/template wrappers do not reject the object. Canvas resize creates and immediately clears/copies it (`toonz/canvassizepopup.cpp:1693–1711`); `TRaster::clear()` writes through the buffer at `common/traster/traster.cpp:410`. The Windows out-of-memory callback only warns and returns (`toonz/main.cpp:136–145`).

Use checked size arithmetic, explicit supported limits, and a fail-fast allocation contract compatible with existing exception recovery. Validate row strides and downstream copy/clear byte arithmetic too. Regression: allocation-failure injection during valid canvas resize, negative/zero/overflowing dimensions, and preservation of original artwork after failure. Some loaders already bound dimensions; do not claim every oversized-file path reaches the allocator unchecked.

**A14 — PSD bitmap expansion and 16-bit grayscale alpha produce incorrect pixels. Source-confirmed; previously acknowledged.**

At `common/psdlib/psd.cpp:791–799`, each inverted packed bitmap byte is copied as one grayscale value into up to eight pixels instead of expanding its bits. For example, a packed alternating byte produces identical gray pixels rather than alternating black/white. At 883–890, the 16-bit two-channel grayscale branch assigns the alpha sample, then the separate four-channel condition's `else` overwrites alpha with `0xffff`.

Expand bitmap bits with correct offsets/shrink semantics; preserve gray-alpha independently from RGB-without-alpha. Regression: alternating and partial final bytes, cropped/shrunk bitmaps, and 16-bit grayscale with varying alpha, compared to independent expected native samples. These correct existing decoding errors and should receive explicit output review. See the acknowledged backlog in [loader known issues](loader_known_issues.md).

**A15 — PSD and media/export ownership leaks accumulate. Source-confirmed; PSD issues previously acknowledged.**

`image/psd/tiio_psd.cpp:92` allocates a parser that `loadInfo()` never deletes. `common/psdlib/psd.cpp:100` has no effective destructor cleanup for allocated layer/channel tables and buffers. Separately, each export frame allocates a capability-probe writer at `toonzlib/movierenderer.cpp:466` without deletion; the factory returns an allocated object. MP4 allocates raw `Ffmpeg` owners at `image/ffmpeg/tiio_mp4.cpp:52,178`, while its destructors at 60 and 200 do not delete them; similar ownership occurs in other format adapters.

Introduce explicit RAII ownership, including exception paths, and resolve invariant writer capabilities once per job. Keep reusable cache lifetime explicit rather than relying on leaks. Regression: repeated PSD/video import-close, failed imports, and long image/movie exports under leak/handle instrumentation. Require stable retained allocations after cleanup; exact operating-system RSS need not return immediately to its initial value.

**A16 — Cadmium inspection bypasses the aggregate cache limit. Source-confirmed.**

The 512 MiB admission check is in `cadmiumpopup.cpp:540–569` inside `prepareDrawing()`. Preview row changes instead call `inspectDrawing()` (`cadmiumpopupui.cpp:317,475`; `cadmiumpopup.cpp:433–479`), which stores analyzed snapshots without aggregate accounting/eviction. The per-source check at 1208 cannot bound all retained clones, label maps, previews, and metadata. Applying only marks a drawing applied at 939 and does not release these snapshots, despite advice to apply results to free capacity.

Centralize reservations/accounting across inspect/generate/restore/preview paths, include temporary work, and evict/spill inactive snapshots while preserving decisions. Regression: inject a small budget, browse many drawings, apply, revisit, and verify bounded residency plus preserved decisions. Actual memory-pressure/crash thresholds remain to be measured.

## Source-confirmed responsiveness and scalability costs

The unnecessary work below is visible in source. Its contribution to real user latency and the size of any speedup have not been measured.

**A17 — Filmstrip rebuild traverses the scene once per cast level.** `toonz/filmstrip.cpp:1716–1720,2359–2364` calls `TXsheet::isLevelUsed()` inside level loops. Each call rebuilds a used-level set by traversing exposures and child sheets (`toonzlib/txsheet.cpp:489–537`). Cost grows roughly with levels times traversed exposure span. Compute that set once per rebuild, following the existing pattern at `toonz/levelcommand.cpp:71`. Compare menu contents/order and benchmark increasing level, exposure, and nesting counts.

**A18 — Level Settings puts every non-null level into one hash bucket.** `toonz/levelsettingspopup.h:70` uses `QSet<TXshLevelP>`. The smart pointer's implicit `operator bool()` (`include/tsmartpointer.h:158`) supplies scalar hashing; no pointer-identity hash overload is present. Rebuild loops (`levelsettingspopup.cpp:820–850`) can take quadratic work for distinct levels, or O(exposures × distinct levels). Add a narrowly scoped identity hash while preserving smart-pointer ownership. Verify Qt 5/6 identity semantics and scaling. This is already documented in [Qt 6 port status](qt6_port_status.md).

**A19 — PDF and LUT subprocesses block the GUI.** PDF loading synchronously enters `convertPdfToPngLevel()` (`toonz/iocommand.cpp:3036`) and waits indefinitely at `toonz/importpdf.cpp:146`. LUT browser slots wait up to 120 seconds (`toonz/filebrowser.cpp:1167–1174`, `toonz/scenebrowser.cpp:1025–1032`). Use shared lifetime-managed asynchronous jobs with cancel, termination deadlines, retained temporary directories, and atomic result publication. Stub subprocesses should test success, failure, stall, and ignored termination while an event-loop heartbeat remains responsive.

**A20 — Changing a reference overlay rebuilds unchanged Cadmium results.** `cadmiumpopupui.cpp:324–325` calls `updateProposalPreview()`, which rebuilds before/result/after at 523–542. Vector display invokes `snapshotVector()` (`cadmiumpopup.cpp:181–185`), with clones, region analysis, hashing, and line/label renders (`cadmiumvector.cpp:143–225`). A ready proposal can require four GL rasterization passes merely to change the reference overlay. Cache before/after by source and decision revisions, separate display from segmentation, and change only the reference image on overlay selection. Instrument analysis/composition counts; overlay changes should trigger no new analysis, with equivalent displayed results.

## Validation defects and release blind spots

**V01 — Scene benchmark exceptions and incomplete round trips can pass.** `toonzlib/tests/scenebench_test.cpp:142–158,409–416,639–651` prints/catches failures without reliably setting `g_broken`, allowing exit zero. `checkLoaded()` at 214–234 uses counts and a weak frame checksum; missing animation keys/FX can be notices rather than failures. Make construction/save/load failures fatal to the test and compare canonical cell identities, keys, FX connections/parameters, and child sheets. Inject exceptions and remove keys/FX to prove the gate fails. Separate timing output from correctness assertions.

**V02 — Missing FX references are accepted and may be promoted automatically.** `stdfx/tests/fxrender_test.cpp:223–228,254` counts absent references without failing. `.github/workflows/tests_linux.yml:133–150` can copy and commit missing images under an `always()` condition on pushes. Normal comparison must fail on absent/unreadable goldens; explicit reference generation should produce review artifacts and never silently bless candidate output. Deleting one expected golden must fail the comparison.

**V03 — Most optional suites are absent from routine PR gates.** `tests_linux.yml:16–21` enables the broad suite only manually or on `tier12`/`ci-tests/**` pushes. Normal Linux enables cel/Cadmium and separately runs sanitized GLB (`workflow_linux.yml:81,89,95,130`); normal Windows enables/runs cel/Cadmium (`workflow_windows.yml:169,239,251`). `BUILD_TESTS` defaults off (`toonz/sources/CMakeLists.txt:848`). Add fast regression coverage per PR, affected expensive suites, full scheduled/release runs, a required-test manifest, and compatibility against a pinned baseline. This gap does not establish that omitted tests currently fail.

**V04 — Compatibility can pass when neither build renders pixels.** Missing GL causes skips in `toonzlib/tests/compat_regression.cpp:685–710`; the workflow only warns (`tests_linux.yml:396–398`). Require a working context and expected image manifest for the graphics gate; name a separate serialization-only mode explicitly. Forcing GL failure must fail graphics validation.

**V05 — macOS lacks comparable runtime gates.** `workflow_macos.yml:21` uses an Intel runner and its build at 84–91 enables no regression suite; there is no CTest step. Add macOS application/runtime tests and Apple Silicon qualification if shipping native arm64. Windows release qualification also needs real tablet/input, DPI, audio, process cancellation, packaging startup, and long-session tests beyond compilation. Experimental Qt 6 jobs are not evidence of supported-platform runtime readiness.

Two apparent issues were rejected after checking context: the full-suite `continue-on-error` step is paired with an explicit failing exit step, and current PLI fuzz tests do not activate the driver's known-case skip callback. Generic undo use of the current sheet is not itself a bug because navigation is undoable; A06 identifies the actual coordinate mismatch.

## Performance work that requires profiling

| Candidate | Source evidence | Measurement and safe direction |
| --- | --- | --- |
| Render reorder-buffer growth | `toonzlib/movierenderer.cpp:591–618` retains completed rasters behind the next ordered frame | Peak resident bytes versus early-frame skew; budget scheduling and backpressure. One 3840×2160 float RGBA raster alone is about 126.6 MiB. |
| Global image-cache contention | `common/tcache/timagecache.cpp:813,1703,1745,1772` holds metadata locks across expensive work | Lock wait/hold time, compression/decompression and disk latency; move work outside locks only with pinned entries and generation validation after A08. |
| Video time to first frame | `image/ffmpeg/tiio_ffmpeg.cpp:888,910,980` can count/decode whole clips | Cold/warm first usable frame, probe time, disk writes; structured metadata probing, on-demand decode, bounded prefetch. |
| Quadratic video cleanup tracking | `image/ffmpeg/tiio_ffmpeg.cpp:995–1000` performs linear `contains()` per frame | Scale frame counts; use a set plus ordered list or per-job directory ownership. |
| Drawing finalization and retouch | `tnztools/toolutils.cpp:84`, `toonzrasterbrushtool.cpp:1665`, `pumptool.cpp:1081` | Input-to-display and stroke-finalization p95/p99, savebox/region/stroke scans; consider dirty regions and spatial indexing after correctness checks. |
| Cadmium repeated encoding/reference analysis | `toonz/cadmiumpopup.cpp:583–588,694–718` repeats invariant work | Encode immutable inputs once, share analysis by full revision, and account retained bytes. |
| Cadmium pixel dispatch/conversion | `toonz/cadmiumfullcolor.cpp:25–29,75–85,201–204` repeats type/conversion work in pixel loops | Typed scanline dispatch and reused color conversions; preserve native precision, alpha, HDR, and exact fill identity. |
| Compressed scene peak memory | `common/tstream/tstream.cpp:290–299` materializes full text and compression buffers | Peak allocations and UI stall for real large scenes; bounded/streaming serialization with shared format limits. |
| Frame-strip safety | `common/tfx/trenderer.cpp:111–158` uses an unsafe-effect denylist | Whole-frame versus tiled output across effects, margins, animated inputs, and macros; explicit tested capability before more parallelism. |

Shared rasters across equivalent output frames and destructive postprocessing deserve focused concurrency tests before removing copies. They are a follow-up hypothesis, not an additional confirmed defect here. Playback, brushes, GLB, and every individual FX need representative traces; changing compiler flags or adding threads without those traces is not evidence of an application-wide speedup.

## Implementation handoff

The first deliverable should be trustworthy regression gates and a reproducible Windows/macOS baseline, followed immediately by narrowly scoped save/undo/data-integrity fixes. Memory and cache ownership precede broader concurrency. Simple algorithmic/UI improvements can then be measured independently. Each accepted optimization needs an unchanged-correctness result, a paired baseline/candidate measurement, and a supported-platform check. Detailed sequencing, workload coverage, and release gates are in the [roadmap](performance_roadmap_2026_10_29.md).
