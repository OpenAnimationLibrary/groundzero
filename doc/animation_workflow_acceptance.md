# Animation workflow reliability acceptance

Items 53–58 were reconciled against edition `aa01975f38d6226156e5b07df7271b8fc199d003`.
The current integration baseline is edition `b584755dca36e9cc5da1c325a2f11c8b83cd2aa2`.
This ledger describes implemented source and prepared acceptance coverage. Builds
and tests run in CI; no local build or test was executed. A passing CI result is
required before these changes are described as verified.

| Item | Source status and behavior | Prepared native acceptance |
| --- | --- | --- |
| 53 — Undoable pose-library changes | Implemented. `poselibrarycommand.cpp` snapshots the complete ordered library and base pose. Poses Set Base/Add/Delete and Library import/replace use one native undo step. Name-conflict Cancel does not change history; identical imports do not add an undo. | `exercisePoses`: actual `.tposes` file import, conflict Cancel/Replace controls, base preservation, native Delete control, undo/redo, scene save/load. |
| 54 — Storyboard timing | Implemented with explicit native and bake choices. `storyboardtiming.cpp` moves exposure cells, sound slices, navigation tags, camera/rig/Plastic/offset/FX numeric curves, rig pin ranges and audio gain together. Native curves retain their interpolation where cuts are representable; incompatible curves require opt-in baking. | `exerciseTiming`: exposure/audio offset permutation, camera interpolation, rig holds and pin ranges, cut holds, repeated undo/redo, untouched-key field equality and outside-interval samples, selected frame/column preservation, locked-track failure, time-remap rebasing, stage cycles and parameter cycles. `exerciseCompatibility` also exercises a nested-sheet timing move and undo without changing parent exposures. |
| 55 — Source/playback timing | Implemented. The viewer shows the selected active time-remapped column's editable exposure, output frame, looked-up source row/drawing and original-editing versus preview mode. It updates with frame, column, scene and preview changes and disappears when remapping is disabled. | Actual native viewer label checks, original source-cell invariance, clear-on-disable. |
| 56 — Older-build compatibility | Implemented as an optional read-only **known-extension inventory** in Scene Check. `scenecompatibility.cpp` visits each stored sheet once, including unexposed child levels, and reports pose libraries, storyboard notes, published channels, offsets, 3D/depth channels, active SeExpr animation, selected new effects, time remapping, color effects and data already skipped by this reader. | Nested sheet referenced by 4,000 exposures produces one note report; unexposed child notes are included. Each stage/offset/Plastic/column/hidden nested FX expression family is checked independently, with key/history/dirty-state preservation. Actual registered SeExpr, particle-cache, deformer, 3D-transform, LUT and neural-style effects require matching-reader warnings. The native option reveals its explicit scope label. |
| 57 — Export fidelity choices | Implemented. Lottie uses the real exporter's loss list before writing output. Animated SVG offers Cancel, supported asset mode, or the existing full-scene renderer. `exportfidelity.cpp` supplies named accessible native controls; closing cancels. | Actual File > Export commands through their native filename pickers: Lottie cancel leaves no files, consent writes parsed JSON layers, overwrite-then-cancel preserves bytes; SVG cancel leaves no files, rendered fallback embeds PNG frames in valid XML, and supported asset export writes SVG. |
| 58 — Recovery status/retry | Implemented. File > Recovery Status shows disabled/pending/writing/complete/partial/failed state, attempt/copy times, missing resources, errors, last copy location and retry. Recovery detects unnamed resources, missing external audio/palettes, project-link failures and unsupported full-color palette changes. Partial copies keep automatic retry pending. Disabled mode writes nothing. | Actual status dialog disabled controls, disabled retry, real recovery manifests, missing-resource partial status, resource removal and repeated successful retry with a distinct version folder. |

## Storyboard boundary contract

The move is a permutation of scene rows. All ordinary exposure columns,
including sound text and child-xsheet exposures, participate. Child xsheets keep
their own local timing. Sound clips crossing a cut split while retaining source
sample offsets; undo restores the original clips. A native segment moves intact
when it has exact boundary keys or a linear/constant segment that can be split
without changing its shape. Each panel's last source frame holds until the next
panel begins. Untouched keys retain their complete native fields, including handle links.
Only moved keys and the immediate cut neighbours are normalized to preserve
effective handles. Boundary keys isolate the edit; frame-zero moves create no
negative key unless the source already contains negative timing.

The separate **Bake Unsupported Timing** choice samples otherwise unsupported
channels before changing any track. Sampling includes expression/file-driven
channels, stepped nonlinear segments, parameter cycles and stage cycling. Stage
cycles become finite keys over the scene and existing key range; original cycle
flags return on undo. Active time remapping is sampled and its looked-up source
rows are rebased through the same exposure permutation, retaining the original
exposure-based duration. Other representable channels remain native.

Baked curves lose their original expressions, cycle behavior outside the sampled
range, and subframe interpolation. This is disclosed before consent. Undo restores
full original curves, flags, clips and tags. Locked tracks, invalid ranges,
nonfinite samples and snapshots exceeding two million keys or exposure cells
fail before any live track is changed. The artist can unlock tracks, correct
expressions or divide a large scene and retry.

## Deliberate report limits

The compatibility option has no target-version selector. It identifies known
extensions that a reader lacking the corresponding feature may reject, omit or
render differently; it does not certify any historical release or third-party
plugin. A visible explanation appears while the option is enabled, including
that an empty inventory is not proof an older reader can open the scene. It does
not rewrite or strip feature data. Compression and external level formats need
their own compatibility checks.

The current built-in effect inventory recognizes registered `STD_` names for
SeExpr effects, particle caches, deformer stacks, 3D transforms, LUT baking and
neural style. Animation expressions are inspected recursively in numeric
parameter sets, including hidden FX parameters, column remap/color parameters,
offset channels and weights, and Plastic skeleton/vertex curves. Inactive
expression text on a non-expression key is not reported as evaluated animation.
The report inspects the serialized authoring data, including disconnected FX and
unused child levels; it does not evaluate expressions, load inference models, or
infer third-party capabilities.

The export preflight does not introduce a new renderer or promise editable Lottie
support for skipped effects. SVG's rendered choice produces PNG frames inside the
existing SVG playback export. Audio is not carried by either SVG mode.

A complete recovery copy includes the scene and recoverable unsaved supported
assets. Clean external assets still depend on their original files. Unsupported
full-color palette changes are explicitly reported as incomplete. This is not an
archive/collect-assets operation.

## CI registration and remaining validation

`animationworkflowintegrationtest.cpp` is compiled with the existing
`BUILD_UI_PORT_TESTS` native application fixture, enabled by Linux, Windows and
macOS workflows. Its runner is invoked before the existing native UI fixture,
requires an empty startup scene and explicitly creates a fresh scene afterward.
It uses the existing isolated portable profile and real native dialogs/commands;
it does not reproduce production algorithms in test-only helpers.
The directory-refresh fixture creates a temporary child of the isolated Library,
verifies that explicit browser root and refreshes it before lookup. It does not
require hidden or symlinked system temporary ancestors to appear in the browser.

Execution remains pending CI. Platform-specific filesystem failures and preview/render pixel fidelity continue to require
the existing integration/render suites and manual acceptance where no fixture is
present. No passing build, test, or performance timing is asserted by this ledger.

The concurrent vector-opacity-profile feature (item 36, PLI 150.1 for profiled
strokes) requires an additional profile-specific compatibility/export finding
once its production API is integrated. That cross-feature check remains pending;
the known-extension inventory above is not a claim that every newer vector
feature is currently covered.
