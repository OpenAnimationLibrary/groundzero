# Vector workflow acceptance

Implementation base: `edition` `aa01975f38d6226156e5b07df7271b8fc199d003`.
Current integration base: `edition` `d510cf089e03e8887f315bb6ff8b88d985687539`,
preserving the merged native UI, particle-cache safety, Qt 6 build and
SeExpr/neural state fixes through #126.
All eighteen rows have implementation and acceptance source. **Execution is
pending CI**; source review and a clean diff check are not passing test results.
Builds and tests are not run locally. Raster brush compatibility and neural
styling are outside this branch.

| Item | Implemented behavior | Acceptance source and remaining qualification |
|---|---|---|
| 19 | Selection opacity displays current, mixed or empty values; readback cannot edit a drawing | `selectionopacity_test.cpp`; actual field selection/frame/readback/apply/undo in `vectorworkflowintegrationtest.cpp` |
| 20 | Scalar opacity composites the complete native style once, including procedural, vector brush, Matrioska and translucent raster Trail output | `opacityrenderintegrationtest.cpp`: representative real styles, scalar 0/.4/.5/1, palette alpha, identity envelopes, native default production GL; all three platform results pending |
| 21 | SVG centerline and outline exports retain palette × stroke opacity; profiled strokes explicitly embed native-rendered PNGs | `svg_opacity_test.cpp` independent SVG renderer; native profile export/reimport rejection and atomic unsupported-style rejection in `opacityrenderintegrationtest.cpp` |
| 22 | Visual width-profile manager supports capture, built-ins, rename, delete, import, export, preview and atomic library save; drawing Apply is bound to its original context even across nested Capture prompts | `widthprofilemanager_test.cpp`: real widgets, cancellation, storage and parsing errors. Native manager context-latch/source-snapshot coverage in `vectorworkflowintegrationtest.cpp` is prepared; fresh CI execution pending. |
| 23 | Width profiles apply once per selected drawing, with art and active-group filters, read-only preflight and one undo; intervening source edits or context/scope changes reject stale Apply | Native fixture covers repeated exposure, selected/unselected drawings, nested groups, queued Apply after level/frame/selection/scene switches, unchanged replacement targets, direct off-frame geometry/fill/profile edits and repeated undo/redo. New stale-source fixtures await CI. |
| 24 | Replace or multiply pressure with adjustable strength; preview uses a captured stroke or uniform reference and Apply revalidates every intended drawing | `widthprofile_test.cpp` plus native manager Cancel/zero-strength/multiply-pressure actions, exact resulting control points, repeated undo/redo and native PLI reload. New modal-context acceptance awaits CI. |
| 25 | Opt-in Review Fills shows original/proposed native drawings after supported reshaping; Apply or Restore Original; context rejection stays latched even if a queued Apply follows a frame round trip | `fillworkflowintegrationtest.cpp`: actual control-point/segment/tangent/Selection gestures, topology, cancel, tool switch, multiple drawings and undo; repeated actual same-drawing frame-switch/queued-Apply recovery fixture prepared, fresh CI pending |
| 26 | Versioned scene-owned morph hints retain drawing/stroke anchors; renumber and stale references handled explicitly | `morphworkflow_test.cpp` and native scene save/reopen, renumber and stale-anchor fixtures |
| 27 | Filmstrip Inbetween offers source/result/target correspondence preview and selectable match rows; Apply validates the reviewed source/destination drawings, hints and editing context | Actual Filmstrip hit-target caller in `fillworkflowintegrationtest.cpp`: accepted silent changes reach source rejection; signaled context rejection clears preview sources and keeps a queued Apply rejected. Scene/level/selection changes, strip deletion and fresh Apply/undo/reload fixtures are source-reviewed; execution pending. |
| 28 | Explicit omit, hold, collapse/grow, or authored-opacity Fade for unmatched morph strokes; Fade rejects unmatched painted-region boundaries (including nested fills/holes) before preview/apply. Independent region opacity remains unsupported. Intervening endpoint/intermediate/hint edits are preserved by stale-source rejection. | `morphworkflow_test.cpp`; actual dialog preview pixels, Cancel/Apply/repeat/undo, scalar/profile/group/PLI preservation, nested topology and failure atomicity. New actual-caller mutation/interruption coverage is source-reviewed; fresh CI execution pending. |
| 29 | Make Unique explicitly duplicates a gradient even when only one current-drawing use is visible | Native gradient action across drawings, repeated duplication and one-step undo/redo |
| 30 | Make Unique offers Copy Animation or Current Appearance; copied keys remain independent of the original; Cancel keeps native image/cache state clean | `styleanimationcopy_test.cpp`; actual native choices, repeated Cancel on saved clean drawings, exact keys/interpolation/unlinking, same-palette modal edits, undo and PLI reload in `vectorworkflowintegrationtest.cpp`. New cancellation/persistence coverage awaits CI. |
| 31 | Outline conversion stages native contours/fills in original stroke order and retains group ancestry | `contour_outline_test.cpp`; real registered command plus native rendered overlap/fill/group/undo comparisons in `contourworkflowintegrationtest.cpp` |
| 32 | Closed-loop outline conversion produces independent inner/outer boundaries without a cap seam | Native loop/hole fixture plus geometric contour test; unsupported animated/procedural/profiled selections rejected before mutation |
| 33 | Align/distribute previews every changed drawing with native original/proposed swatches at a common scale, then applies reviewed positions in one undo; Cancel and stale-source rejection preserve originals | `vectorworkflowintegrationtest.cpp`: actual modal preview pixels versus applied/reopened PLI output; current, selected-drawing and whole-level scopes; cancel/no-op, off-frame source changes, same-drawing frame switches, nested groups and repeated undo/redo. New preview qualification pending CI |
| 34 | Paint Inside and Paint Selection clip native brush output to filled regions or a captured selection mask, retaining holes | `brushworkflowintegrationtest.cpp`: actual gestures, holes/outside bounds, mask capture/staleness, art filters, locked drawings and undo |
| 35 | Guided Drawing and range painting preserve the chosen paint mode and preflight every destination mask | Native brush fixture exercises guide/range output and undo; explicit Current Drawing or Same Area on All Drawings selection-mask policy |
| 36 | Normalized arc-length opacity envelope with native editor, rendering, PLI 150.1, split/join/reverse/Pump/Tape/morph preservation and explicit SVG fidelity contract; editor Apply requires unchanged source contents and a latched original context | `pli_opacityprofile_test.cpp`, editor/morph native fixtures, `strokeopacityeditintegrationtest.cpp`, production renderer and CLI fixtures. New real queued-Apply geometry/envelope/palette and context-change rejection, fresh Cancel/Apply/repeated undo/native reload coverage is source-reviewed; fresh execution and complete platform qualification pending. |

## Test integration

### Repeatable morph preview background

Vector checkpoint `c267c4bcf79b9093b72d6aa06c76783db96aa521` passed Linux
application and installed-package checks. The combined original process at
`3d4309a9a1a4de181a36577939c6649da175b516` then failed the exact repeated-Fade
framebuffer comparison; its six fresh isolated processes passed, which does not
replace the failed original sequence.

Source review found that the morph swatch computed its checkerboard using the
previous view transform, before establishing the current artwork transform.
The swatch now resets the checkerboard transform exactly as the alignment
preview does. This fixes a concrete history-dependent rendering path; it does
not yet establish the cause of the recorded combined failure. Fade interpolation
and its authored opacity remain unchanged.

The native regression deliberately changes the previous affine transform and
requires an identical actual repaint. It retains the exact 50/25/75/50-percent
pixel sequence, Cancel/Apply and undo checks, and independently inspects generated
geometry, opacity, profile, style, groups and unchanged source/palette contents.
Bounded dimensions, pixel differences and transform diagnostics distinguish a
rendering mismatch from altered artwork. The corrected supplemental Linux
application and installed package passed at
`68b56d40978113ea597c598892e44b9b55e1977a`; later owning heads still require
their own qualification. No comparison or original failure was waived.

### Atomic deformation Undo (pending CI)

The original Windows application at `1f504560` timed out between its final
Envelope Undo/history-reset checkpoints after earlier geometry and region/style
checks completed. Source review found a distinct renderer race: live control
points were changed outside the image mutex, and notifications preceded fill
restoration. Geometry, rebuilt regions and fills now restore under one recursive
image lock; UI and icon notifications follow unlock and name the undo record's
owned frame/level. The native regression exercises a real concurrent GL reader,
complete fills at the first notification, off-frame Undo and untouched foreign
level geometry, cache and dirty state. Separate Undo/reset checkpoints identify
any remaining stall. These source-backed corrections do not by themselves
establish the original timeout's cause or passing platform qualification.

### Original-window lifetime qualification (pending CI)

Rows 22–25, 27–30, 33 and 36 include source-reviewed original-window
cancellation work; fresh platform execution remains pending. Stack reviews are
parentless QObject objects, with native transient placement instead of QWidget
ownership. Hiding, closing or destroying the original window (including an
ancestor of the Filmstrip) permanently cancels the current review and its nested
prompts. Showing the owner again cannot revive it. Reopening a fresh review
retains ordinary Apply/Cancel behavior.

The native command fixtures hide/show or delete the actual active owner during
width, fill, alignment, gradient and opacity review, and hide the actual morph
Filmstrip or its containing area. Receiver-queued callbacks explicitly execute
the real button click after cancellation; a disabled Apply remains Rejected.
This strengthens the previous width context oracle, which allowed a queued
button to mark the dialog Accepted and relied on the caller's later source
check. Drawing, palette, image identity, history and dirty-state checks remain.
The standalone manager also cancels naming, New, Delete, Import and nested
Export overwrite prompts, preserving exact library and destination bytes.
Library drafts and independent exports completed before cancellation are not
rolled back. The scope was established by source ownership analysis, not a
reproduced crash; it is separate from the still-unlocalized native timeout.

The first execution of these cases found two fixture setup errors. Native owner
setup now requests platform activation and waits, with a five-second bound, for
the exposed window to be active; each review also proves its transient owner is
that exact window before cancellation. Merely calling Qt's active-window setter
does not request native focus. Manager cancellation compares all loaded draft
samples exactly, using the library's existing five-significant-digit format as
its initial state, rather than comparing decoded samples to unsaved sine-based
built-in values. Original byte, queued-action, drawing and history oracles stay
strict. These fixture corrections still require fresh CI qualification.

Linux qualification at `ed247a3255816f50c6531c4c2cc8e5f67f7dfd90`
passed the standalone width manager and the complete native width-context
workflow, including both exact post-save Undo/Redo cycles and alignment.
The later actual Filmstrip Apply, three Undo/Redo cycles, and live-save
invariance also passed, but the original full-double PLI reload comparison
failed: control point `(11.902937812669542, 1.614491414073373, 2)` reopened as
`(11.8984375, 1.609375, 2)`, with matching style, scalar opacity, envelope and
palette color. The unchanged native reader calls `findRegions`, which truncates
new non-point strokes to its 1/128 region grid after PLI's finer 1/16384
absolute/delta coordinate encoding. This is existing edition behavior, not a
new morph or opacity data-loss finding. The reload fixture now predicts both
stages and native byte-width encoding for every control point, compares them
exactly, and rejects unexpected topology or degenerate-handle repair. Live
Apply/Undo geometry and persisted scalar/profile/style/palette checks remain
exact. This corrected persistence oracle still awaits fresh CI execution;
these component passes do not qualify the full branch.

Linux `6392d44` subsequently passed that exact persistence oracle, then the
original process crashed during the following strip-replacement phase; its GDB
replay completed. Source review identified a pre-existing viewport-owned resize
filter retaining a raw pointer to a deleted Filmstrip. The filter now belongs
to the strip, is removed before selection-detachment signals can reenter layout,
ignores callbacks after `takeWidget` detaches the still-live strip, and protects
its own callback against synchronous destruction. Native coverage repeats real
strip detachment/deletion, reentrant and late viewport Resize events, and
responsive replacement layout while retaining drawing/history and actual
modal-cancellation checks. Fresh execution remains pending; without the
original crash stack this source-proven lifetime defect is not yet a confirmed
attribution of that process failure.

The existing `BUILD_UI_PORT_TESTS` gate builds the standalone tests and the real
application fixtures. `nativeuiintegrationtest.cpp` calls the vector, fill,
contour, brush, morph, opacity editor, renderer, native stroke editing and
production-context/CLI fixtures. The application depends on `tconverter` so the
native acceptance run can invoke actual ordinary/profiled PLI conversion and
verify raster-only conversion without a GUI platform.

Added CTest names are `ui_vector_opacity`, `ui_vector_profilemanager`,
`ui_vector_gradientanimation`, `ui_vector_svgopacity`, `ui_vector_pliprofile`,
`ui_vector_morphworkflow`, and `ui_vector_contour_outline`. Windows additionally
requires `ui_shellthumbnail`, which exercises the actual installed DLL provider
and renderer subprocess from a COM worker without a GUI application in the host.
The repository's
three-platform CI registration and native-launcher checks remain the authority
for executed results.

## Deliberate format and workflow boundaries

- Morph hints belong to the scene's level metadata. Saving a standalone PLI
  drawing does not carry scene-level correspondence hints.
- Opacity envelopes require PLI 150.1. Uniform strokes keep the existing native
  format-selection rules. Older readers reject the new format explicitly.
- SVG has no portable editable along-path alpha primitive. Profiled strokes
  with verified bounds (solid, native raster/vector Trail, Circlets) become
  embedded PNGs at the chosen export scale. Other paths remain vectors. The
  native SVG vector importer rejects this tagged mixed output with an
  instruction to open the original PLI, before importing partial contents.
- Profiled styles without verified finite export bounds fail before creating or
  replacing the SVG. Use the scene export's rendered full-camera SVG option
  instead. Viewer and frame rendering still support these styles by capturing
  their complete output viewport; geometry-only style bounds are not guessed.
- Outline conversion supports static solid strokes, including scalar opacity.
  Animated/procedural styles and along-stroke envelopes require a different
  representation and are rejected explicitly before any mutation.
- Shape-clipped painting refuses unsupported guide-stroke scalar/envelope
  opacity instead of silently converting it to a different alpha shape.
- With depth testing disabled, uniform opacity remains on the GPU after native-style capture. Envelopes add
  pixel readback and a bounded spatial lookup. Unverified procedural styles
  capture a full viewport. Performance and memory costs are measured in CI;
  this branch makes no unmeasured speed claim.

## Depth-tested opacity and resource cost

Native 3D drawing has fragment-level depth and stencil behavior, including
antialiased Chain lines that occlude one another. A flattened style texture
cannot reconstruct those decisions from the stroke plane. The depth-enabled
path therefore draws the unmodulated native style into the original target and
interpolates its actual color change by scalar opacity and the arc-length
envelope. It retains native depth and stencil wherever the effective opacity is
positive, including changes that leave the color identical to the background.
Exactly zero opacity restores the original color, depth and stencil. Errors
after native drawing restore all saved attachments before propagating.

PlaneViewer intentionally flattens the Z coordinate. Opacity previews invert
the drawing plane's XY/W projection when that makes the full 3D matrix singular;
ordinary 3D matrices keep their existing inverse path. An actually degenerate
drawing plane is reported before drawing, and an undefined projective-horizon
sample reports an error instead of silently omitting the stroke. Native checks
cover widget scalar/profile visibility, sampled fade positions, and projected
clipping across the near plane; the existing 3D depth oracles remain unchanged.

Stencil snapshots cover the full attachment because native fill-mask clears can
affect pixels outside both the stroke and viewport. Color and depth snapshots
use a conservative projected rectangle only for the exact solid style with a
finite, positive-W projection and fixed-pipeline filled polygons. The rectangle
includes authored miters and the native antialias fringe, and is intersected
with the target, viewport and existing scissor. The native draw and its scissor
are unchanged. Other styles, active shader/assembly programs, wide-line or
non-filled rasterization, and uncertain projections retain full color/depth
snapshots. Float color preserves high-bit
targets. Fixed depth of at most 24 bits and native 32-bit floating depth use FLOAT
transfers to avoid normalized UINT32 sampling loss; exact native depth oracles
remain mandatory. Fixed or unclassified depth formats above 24 bits are refused
before drawing because this compatibility path cannot restore them exactly.
sRGB targets interpolate RGB in linear light. Floating-color targets temporarily
disable read/fragment clamping during snapshot/replace, preserving untouched
HDR values while native drawing retains the caller's original clamp policy.
Pixel transfer, program, texture,
channel-mask, scissor and separate read-framebuffer state are restored. Field
lookups are skipped for unchanged color/depth and separately for unchanged
stencil; zero-opacity stencil restoration still examines the whole attachment.

The supported depth targets are non-multisample Qt windows and FBOs with a depth
renderbuffer, 2D texture, or cube-map face within those precision limits. Multisample attachments (including a
multisample attachment with one sample) and other texture depth targets are
rejected before drawing. The application's Qt production backend requests a
combined depth/stencil FBO with no samples; the ordinary 2D opacity path is
unchanged by these depth-specific restrictions. This is not a per-sample GPU
renderer redesign.

This 3D path adds readback, native drawing and attachment replacement per stroke.
With stencil present, the full fallback's two snapshots contain 48 bytes per
target pixel: 1,572,864 bytes at 256×128 and 99,532,800 bytes at 1920×1080. For a
certified solid crop of A pixels in a target of F pixels, the actual snapshot
payload is 40×A + 8×F bytes, before stroke/field objects and existing framebuffer
storage. Full stencil readback, comparison and replacement remain a per-stroke
cost. Buffers are released
after each stroke. Aggregate snapshots are capped at 512 MiB before allocation;
larger targets are refused before drawing with an instruction to reduce the
viewer framebuffer size. With stencil, the full fallback allows up to 11,184,810 target pixels;
full 3840×2160 depth-tested targets fit (398,131,200 snapshot bytes). Cropped
transactions are checked against their actual combined color/depth/stencil
payload before any allocation or native drawing. This limit does not apply
to ordinary depth-disabled 2D drawing or export. Acceptance records native, scalar and envelope frame timings
for bounded small and 1920×1080 multi-stroke workloads; these are descriptive
measurements rather than a machine-dependent pass threshold. Usable performance
and complete platform execution remain pending CI; no performance-neutrality
claim is made.

The `vector-opacity-depth-workload-provenance` and
`vector-opacity-depth-workload-mode` records identify the actual GL renderer,
target/depth/sample formats, and each synchronized frame time in the 1080p
eight-stroke workload. Timings include allocation, readback and replacement;
untimed midpoint pixels verify that all eight strokes drew with the expected
native/scalar/envelope result. Per-mode memory records use the process peak
resident set before and after rendering (`PeakWorkingSetSize` on Windows,
`ru_maxrss` on macOS, `VmHWM` on Linux). This cumulative high-water mark may be
dominated by earlier fixtures; it is not an isolated allocation delta or
simultaneous process-tree RSS. Actual color/depth/stencil transfer pixel counts
and the peak snapshot payload are reported separately from the full fallback's
payload. The earlier valid llvmpipe run measured roughly 8 ms native versus
382–687 ms scalar/profile per eight-stroke frame across the mandatory and isolated
samples, with a cumulative process peak
around 0.8 GB. Those are software-renderer measurements before cropping, not
hardware-GPU estimates. The cropped path's timing and memory results remain
pending fresh CI; no latency threshold or responsiveness claim replaces the
pixel oracles.

The native fixture retains the strict Chain `GL_LEQUAL` comparison and adds the
scene viewer's `GL_ALWAYS` mode, transparent holes and later layers, same-color
depth writes, channel/depth/stencil masks, scissor, reduced viewports, independent
read FBOs, high-bit and sRGB color, supported depth textures, preflight refusal,
failure rollback and recovery.
Forced-full versus cropped native transactions compare every color, depth and
stencil pixel for scalar/profile/zero, tilted and offset views, masks, patterned
depth and injected rollback. Actual transfer counters require reduced
color/depth work and unchanged full stencil work; a real shader and Chain style
require full fallback. The actual oversized-target refusal uses Chain so it
continues to exercise a genuinely over-budget transaction after optimization.

## Production caller preservation

The default offscreen backend uses Qt compatibility contexts on every desktop
platform. Native acceptance covers context nesting, sharing, GUI/background
creation overlap, release and transfer between render threads, and ordinary and
profiled PLI command-line conversion. Raster-only `tconverter` retains a
QCoreApplication path that works without a display plugin.

Windows Explorer's QCore/COM worker environment uses a bounded renderer
subprocess, described in [Windows Shell Thumbnails](windows_shell_thumbnails.md).
Its native CTest covers ordinary/profiled PLI and TNZ output through the real
DLL factory, Unicode paths, small dimensions, cancellation, timeout, malformed
payloads and temporary-file cleanup. The provider loads only Windows system
libraries; GUI, GL and image-format dependencies stay in the installed helper.
All of these execution results remain pending supported CI.

## Recoverable viewer failures

Native GL widgets collect opacity failures only during their actual paint
events. A mouse-transparent, accessible notice stays above later drawing layers
and tool overlays; repeated repaint reuses it, framebuffer reuse retains it,
and successful rendering clears it. Intermediate style captures suspend this
collection so a failed nested render cannot appear complete. Production,
thumbnail and file-export renders retain explicit failures.

Explicit viewer framebuffer capture collects errors while Qt completes its
framebuffer operation, then throws instead of returning a partial image. The
scene viewer does not freeze an incomplete frame. Native acceptance uses real
PlaneViewer paint/repaint events, nested widget painting, later opaque layers,
disabled updates, strict capture and recovery. Results remain pending CI.
Native selection mode uses the original style geometry without an opacity
framebuffer; its real selection-buffer tests retain hit names, reject points
inside only the composite rectangle, and retain zero-scalar-opacity behavior.

Clipped Paint Inside/Selection objects are prepared with their fills before
insertion. They remain directly inside the entered group, above its existing
children and below any outside layers. Native brush acceptance checks nested
group ancestry, overlapping siblings and outer layers, and repeated undo/redo.
Its phase-specific pixel, contour and region diagnostics retain the original
rendering assertions. Linux at `bc9ca867f6cd2fa6c719f6159a9de3411b44b76e` passed
fill, contour, Brush, morph and opacity-editor native checks; the renderer then
identified the Chain depth mismatch addressed above. The new depth correction
and remaining platform qualification still await CI.
