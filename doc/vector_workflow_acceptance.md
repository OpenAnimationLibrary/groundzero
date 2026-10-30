# Vector workflow acceptance

Implementation base: `edition` `aa01975f38d6226156e5b07df7271b8fc199d003`.
All eighteen rows have implementation and acceptance source. **Execution is
pending CI**; source review and a clean diff check are not passing test results.
Builds and tests are not run locally. Raster brush compatibility and neural
styling are outside this branch.

| Item | Implemented behavior | Acceptance source and remaining qualification |
|---|---|---|
| 19 | Selection opacity displays current, mixed or empty values; readback cannot edit a drawing | `selectionopacity_test.cpp`; actual field selection/frame/readback/apply/undo in `vectorworkflowintegrationtest.cpp` |
| 20 | Scalar opacity composites the complete native style once, including procedural, vector brush, Matrioska and translucent raster Trail output | `opacityrenderintegrationtest.cpp`: representative real styles, scalar 0/.4/.5/1, palette alpha, identity envelopes, native default production GL; all three platform results pending |
| 21 | SVG centerline and outline exports retain palette × stroke opacity; profiled strokes explicitly embed native-rendered PNGs | `svg_opacity_test.cpp` independent SVG renderer; native profile export/reimport rejection and atomic unsupported-style rejection in `opacityrenderintegrationtest.cpp` |
| 22 | Visual width-profile manager supports capture, built-ins, rename, delete, import, export, preview and atomic library save | `widthprofilemanager_test.cpp`: real widgets, cancellation, storage and parsing errors; native manager command exercised in `vectorworkflowintegrationtest.cpp` |
| 23 | Width profiles apply once per selected drawing, with art and active-group filters, read-only preflight and one undo | Native fixture covers repeated exposure, selected/unselected drawings, nested groups and repeated undo/redo |
| 24 | Replace or multiply pressure with adjustable strength; preview uses a captured stroke or uniform reference | `widthprofile_test.cpp` plus native manager zero-strength and multiply-pressure actions and resulting control points |
| 25 | Opt-in Review Fills shows original/proposed native drawings after supported reshaping; Apply or Restore Original | `fillworkflowintegrationtest.cpp`: actual control-point/segment/tangent/Selection gestures, topology, cancel, tool switch, multiple drawings and undo |
| 26 | Versioned scene-owned morph hints retain drawing/stroke anchors; renumber and stale references handled explicitly | `morphworkflow_test.cpp` and native scene save/reopen, renumber and stale-anchor fixtures |
| 27 | Filmstrip Inbetween offers source/result/target correspondence preview and selectable match rows | Actual native dialog/command in `fillworkflowintegrationtest.cpp`, compared with generated frames |
| 28 | Explicit omit, hold, or collapse/grow policy for unmatched morph strokes | `morphworkflow_test.cpp`; native staged range generation and failure atomicity, endpoints, stacking and undo |
| 29 | Make Unique explicitly duplicates a gradient even when only one current-drawing use is visible | Native gradient action across drawings, repeated duplication and one-step undo/redo |
| 30 | Make Unique offers Copy Animation or Current Appearance; copied keys remain independent of the original | `styleanimationcopy_test.cpp`; actual native animation-choice dialog, keys/interpolation and undo in `vectorworkflowintegrationtest.cpp` |
| 31 | Outline conversion stages native contours/fills in original stroke order and retains group ancestry | `contour_outline_test.cpp`; real registered command plus native rendered overlap/fill/group/undo comparisons in `contourworkflowintegrationtest.cpp` |
| 32 | Closed-loop outline conversion produces independent inner/outer boundaries without a cap seam | Native loop/hole fixture plus geometric contour test; unsupported animated/procedural/profiled selections rejected before mutation |
| 33 | Align/distribute works independently in selected drawings, preserves fills and nested groups, and uses one undo | `vectorworkflowintegrationtest.cpp`: selected range, first/last/selection reference semantics, nested groups and repeated undo/redo |
| 34 | Paint Inside and Paint Selection clip native brush output to filled regions or a captured selection mask, retaining holes | `brushworkflowintegrationtest.cpp`: actual gestures, holes/outside bounds, mask capture/staleness, art filters, locked drawings and undo |
| 35 | Guided Drawing and range painting preserve the chosen paint mode and preflight every destination mask | Native brush fixture exercises guide/range output and undo; explicit Current Drawing or Same Area on All Drawings selection-mask policy |
| 36 | Normalized arc-length opacity envelope with native editor, rendering, PLI 150.1, split/join/reverse/Pump/Tape/morph preservation and explicit SVG fidelity contract | `pli_opacityprofile_test.cpp`, editor/morph native fixtures, `strokeopacityeditintegrationtest.cpp`, production renderer and CLI fixtures; complete platform qualification pending |

## Test integration

The existing `BUILD_UI_PORT_TESTS` gate builds the standalone tests and the real
application fixtures. `nativeuiintegrationtest.cpp` calls the vector, fill,
contour, brush, morph, opacity editor, renderer, native stroke editing and
production-context/CLI fixtures. The application depends on `tconverter` so the
native acceptance run can invoke actual ordinary/profiled PLI conversion and
verify raster-only conversion without a GUI platform.

Added CTest names are `ui_vector_opacity`, `ui_vector_profilemanager`,
`ui_vector_gradientanimation`, `ui_vector_svgopacity`, `ui_vector_pliprofile`,
`ui_vector_morphworkflow`, and `ui_vector_contour_outline`. The repository's
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
- Uniform opacity remains on the GPU after native-style capture. Envelopes add
  pixel readback and a bounded spatial lookup. Unverified procedural styles
  capture a full viewport. Performance and memory costs are measured in CI;
  this branch makes no unmeasured speed claim.
