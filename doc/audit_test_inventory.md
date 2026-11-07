# Audit regression inventory and lifecycle decisions

This inventory reviews the working-tree audit changes against
`2e52fb78b70915925393290f32dc09a95a3927ec`, on November 7, 2026. It covers the
changed C++ fixtures, the new Python CI-auditor contracts, the new Ruby CI-budget
contracts, their registration, and their workflow callers. It is a source review,
not a measured coverage report or evidence that CI has passed. Builds, tests,
fault injection, and differential runs were not performed locally, as required by
[CLAUDE.md](../CLAUDE.md).

## Registration and configured execution

"Required" below means a workflow names the test or invokes its driver. It does
not mean that the workflow has executed successfully. The application workflows
also have build/dependency conditions that can prevent later test steps from
running after a failure.

| Regression family | Registration or entry point | Configured CI execution |
| --- | --- | --- |
| PSD persistence and independent readers | `tiio_psd`, under `BUILD_PSD_TESTS` in [image CMake](../toonz/sources/image/CMakeLists.txt) | Required by Linux, macOS, and Windows PR regression manifests. Write-failure injection is compiled only on POSIX. |
| Public FFmpeg export, HDR preservation, helper paths, audio lifecycle | `ffmpeg_public`, under `BUILD_FFMPEG_TESTS`, conditional on `find_program(ffmpeg)` | Required by all three platform PR manifests. Missing registration fails the named manifest. |
| Direct FFmpeg export/import/cache lifecycle | `tiio_ffmpeg`, same option, additionally excluded on MSVC | Required on Linux and macOS; intentionally absent from the Windows list because direct writer classes are not exported there. |
| Painted deformer weights and existing deformer behavior | `deformer`, under `BUILD_DEFORMER_TESTS` in [toonzlib CMake](../toonz/sources/toonzlib/CMakeLists.txt) | Explicitly enabled and required in all three platform PR manifests. |
| Auto-Patch support dimensions and outline math | `autopatchoutline`, under `BUILD_AUTOPATCH_TESTS` in [stdfx CMake](../toonz/sources/stdfx/CMakeLists.txt) | Explicitly enabled and required in all three platform PR manifests. This is a standalone helper fixture, not a rendered-FX fixture. |
| Native plugin ABI probe ranges | `plugin_probe_single`, `plugin_probe_empty`, under `BUILD_RELIABILITY_TESTS` in [toonzqt CMake](../toonz/sources/toonzqt/CMakeLists.txt) | Required in all three platform PR manifests; each mode has its own process and loads a real fixture module. |
| External-effect process and output contracts | `externfx_process`, under `BUILD_RELIABILITY_TESTS` in [tnzbase CMake](../toonz/sources/tnzbase/CMakeLists.txt) | Required in all three platform PR manifests; Windows and POSIX execute different process branches. |
| Task removal, farm IDs, active children, movie batch setup | `exerciseWorkflowSwitching()` in [native UI integration](../toonz/sources/toonz/nativeuiintegrationtest.cpp), compiled into `OpenToonz` with `BUILD_UI_PORT_TESTS` | The platform workflows invoke `run_native_ui_test.py` explicitly. These cases are not independent CTest registrations. |
| Filmstrip insertion/duplicate/Each/Swing undo and ungroup undo | `runVectorWorkflowNativeChecks()` in [vector integration](../toonz/sources/toonz/vectorworkflowintegrationtest.cpp), same application build option | Explicit native application driver on all three platforms, with `--require-vector-workflows`. A CTest-only run does not execute these cases. |
| CI diagnostic classification and API pagination | [test_ci_audit.py](../ci-scripts/tests/test_ci_audit.py), Python `unittest` discovery | Explicit Linux PR step with `-p test_ci_audit.py -v`; not a CTest target. |
| Automatic-check budget and workflow parsing | [test_ci_budget.rb](../ci-scripts/tests/test_ci_budget.rb), Ruby Minitest | Explicit Linux PR step after the read-only budget checker; not a CTest target or a pre-commit test-suite invocation. |

The required platform lists are in [Linux Build](../.github/workflows/workflow_linux.yml),
[MacOS Build](../.github/workflows/workflow_macos.yml), and
[Windows Build](../.github/workflows/workflow_windows.yml).
[run_regression_tests.py](../ci-scripts/run_regression_tests.py) enumerates CTest
registrations first, fails for missing required names, and checks JUnit for
missing, failed, or skipped cases. Keep these checks: a broad regex or successful
build alone would not establish execution of the required fixtures.

The nightly/manual [Linux Tests](../.github/workflows/tests_linux.yml) workflow
configures `BUILD_TESTS=ON` for Qt 5 and Qt 6 and runs all registered CTest cases.
Its Qt 6 dependency installation does not explicitly install FFmpeg, so FFmpeg
registration depends on runner availability. A green full CTest run cannot prove
that a conditionally unregistered suite existed. Neither full CTest invocation
substitutes for the platform workflows' explicit native application driver.
The separate [experimental Qt 6 workflow](../.github/workflows/workflow_linux_qt6.yml)
is manual and build-only; it is not runtime qualification.

## Keep, extend, replace, and remove

| Family | Decision | Behavior protected and reason |
| --- | --- | --- |
| Existing PSD format fixtures | **Keep** | Eight- and sixteen-bit pixels, Unicode layer names, single-pixel images, bitmap/gray imports, malformed files, and layered output remain distinct from transactional saving. |
| New PSD transaction/reader cases | **Keep and extend later** | Interleaved readers retain their own layer identity; failed overwrites preserve prior bytes and remove temporary files; retries persist edited pixels. Both small and larger payload cases remain. They do not independently inject every open/write/flush/rename failure. |
| New duplicate HDR case in the direct FFmpeg suite | **Replace and remove that function only** | Its partial-alpha and repeated-source invariants now live in the public fixture described below. No established direct-suite case or target is removed. |
| Existing direct FFmpeg suite | **Keep** | MP4 tolerance, concurrent completion, cache leases, metadata batching, replacement/incomplete-cache handling, finalization errors, cancellation, abort, cleanup, and retry cases protect different observable outcomes. |
| Public FFmpeg fixture | **Extend** | Retains public factory construction, row/channel/frame ordering, abort behavior, helper-directory resolution, and alpha values above one. Adds fractional alpha and repeated export of one source raster before removing duplicate HDR coverage. |
| Existing deformer geometry/render tests | **Keep** | Identity, inverse, raster warp, stroke thickness, source-image immutability, stack order, and serialized painting round trips do not duplicate new invalid-input checks. |
| New weight-grid cases | **Keep** | Large brushes and erasers must affect all intended cells; remote/invalid brushes must not alter weights; invalid geometry/counts must not construct usable grids. Assertions inspect values and validity, not a chosen clipping implementation. |
| Auto-Patch math and sizing | **Keep; extend at the FX boundary** | Brute-force distance comparisons, outline falloff, and seam/detail separation protect pixels. Boundary predicates protect conversions and allocations. They do not establish correct placement of the checks in the predictive render/cache lifecycle. |
| Native plugin probe fixture versus Plugin Manager tests | **Keep both** | A real module exercises loader ABI entry ranges. The existing Qt 5/Qt 6 manager suites exercise policy/settings. One cannot replace the other. Single-entry acceptance and empty-range rejection are distinct. |
| External-effect fixture | **Keep; extend failure branches later** | Uses real child processes, arguments, output files, and actionable failures. Windows handle-count and environment-expansion checks have no equivalent in the POSIX shell branch. |
| Native task and undo fixtures | **Keep** | Multiple panels, parent/child selections, retired farm IDs, active-child rejection, preserved drawing IDs/content/exposures, independent inserted rasters, and nested groups exercise different state transitions. Repeated undo/redo cycles are intentional. |
| Python CI-auditor tests | **Keep** | Positive crash/error cases and benign checkpoint/cancellation-word controls prevent misleading diagnoses. Pagination and endpoint assertions concern the external GitHub boundary; they also prevent bounded requests from fetching unrelated history. |
| Ruby CI-budget tests | **Keep and extend** | Includes, excludes, reusable nesting, event multiplicity, malformed/dynamic input, and cycle rejection are separate contracts. The CLI boundary case now accepts nine and ten checks and rejects eleven. |

No legacy test has been classified as obsolete solely because it is old, lengthy,
or resembles a newer case. Retiring standalone workflow files during CI
consolidation is not removal of the suites moved into Linux Build.

### HDR replacement evidence

Before consolidation, the new direct test checked floating pixels with alpha
`0.5`, exported the same raster twice, compared decoded rows, and compared pipe
versus intermediate-image output. The new public fixture initially used alpha
greater than one. Deleting either case without moving those inputs would have
lost a distinct boundary.

The public fixture now retains its original above-one-alpha cases in frames zero
and one. Frames two and three use the direct fixture's exact inputs:

| Region | Floating RGBA | Expected decoded RGBA |
| --- | --- | --- |
| Top | `(2, -0.25, 0.25, 1)` | `(255, 0, 64, 255)` |
| Bottom | `(0.5, 0.25, 2, 0.5)` | `(128, 64, 255, 128)` |

The last frame reuses the preceding raster without repainting it. After
finalization, every source pixel must still equal its original floating value.
Both export paths independently decode to the same expected pixels at every
position, which also checks parity, frame order, and orientation. The public
factory makes these assertions available on Windows as well as Linux/macOS.

The expected alpha is `128`, not a rescaled or unpremultiplied channel value:
[TRop conversion](../toonz/sources/common/trop/tconvert.cpp) clamps each channel
and rounds `value * 255 + 0.5`; the existing FFmpeg pipe and ARGB32 intermediate
paths pass those channel values through. No production pixel conversion was
changed by this test consolidation. Runtime equivalence remains pending in CI.

## Differential proof plan

Use the same tests and runner/toolchain on the fixed tree and a disposable tree
with one production defect restored. Keep fixture registration and dependencies
identical. Record the expected assertion or sanitizer report, exit status, and
logs for each tree. Compilation failure, an unrelated early crash, or a missing
test is not proof that a regression test catches its intended defect.

| Scenario | Defect to restore in the disposable comparison | Expected distinguishing evidence |
| --- | --- | --- |
| HDR export | Restore one in-place float-clamp path at a time | Source-preservation assertion fails for the affected path; fixed output still matches the expected decoded pixels. |
| PSD overwrite failure | Restore truncating destination writes | POSIX size-limit case detects changed original bytes or incorrectly reported success; fixed tree preserves bytes and retry succeeds. |
| PSD interleaved readers | Restore shared mutable frame/layer selection | First reader returns another layer after second-reader construction; fixed readers retain their colors through repeated metadata reads. |
| Painted weights | Restore cast-before-clipping and unchecked grid geometry separately | Large brush fails to cover the grid, invalid geometry is accepted, or sanitizer reports conversion/count overflow; fixed cases retain the ordinary round-trip behavior. |
| Auto-Patch size guards | Disable a predicate check while retaining the new helper API | Existing helper boundary cases detect accepted impossible dimensions. Do not count failure to compile a new helper against an older header as behavioral proof. |
| Auto-Patch predictive tiling | Add an actual FX/cache integration scenario before claiming this boundary is covered | A large predictive region that is later tiled must survive dry computation; each actual oversized tile must still fail before allocation. The helper-only fixture does not prove this lifecycle contract. |
| Plugin function probes | Restore the `begin == end - 1` rejection | The valid single-entry function probe is rejected; the fixed tree loads it. The empty-range case must remain rejected in both trees. |
| External effect | Restore ignored child exit/output checks separately | Nonzero exit or missing/corrupt output fails to produce the required actionable error; fixed process cases report it. |
| Task/undo state | Restore one ownership/cache-ID defect per run | The corresponding task retirement, drawing identity, exposure, aliasing, or group-depth assertion fails. Earlier crashes must not be mistaken for execution of later cases. |
| CI auditor | Restore single-page reads or broad keyword classification separately | Second-page failure or benign checkpoint controls distinguish the changed behavior without live API calls. |
| CI budget | Mutate matrix/event/reusable expansion or limit comparison | Synthetic workflow fixtures and CLI status identify incorrect totals/rejection; the new guard has no pre-existing production implementation to run as a historical baseline. |

The Python tests mock GitHub/subprocess boundaries rather than replacing the
classification logic. Ruby tests write temporary workflow documents and include
a real CLI process. Native plugin and external-effect tests load/run real fixture
artifacts. The farm controller is an external-service fake; native application
state transitions and undo operations remain real.

## CI commands and outstanding qualification

Run these only in configured CI builds, preserving the platform workflow's Qt,
library-path, display, and debugger environment. The commands below are a focused
subset; they do not replace the existing platform manifests.

```sh
cd toonz/build
python3 ../../ci-scripts/run_regression_tests.py \
  deformer autopatchoutline tiio_psd ffmpeg_public \
  externfx_process plugin_probe_single plugin_probe_empty
```

Linux uses its Xvfb/software-GL wrapper. Windows uses `python` with
`--config RelWithDebInfo`. Run the retained `tiio_ffmpeg` target on non-MSVC
builds. Native task and undo cases require the existing platform
`run_native_ui_test.py` step with its required-workflow flags; they are not covered
by the subset above.

```sh
python3 -m unittest discover -s ci-scripts/tests -p test_ci_audit.py -v
ruby ci-scripts/tests/test_ci_budget.rb
```

Outstanding evidence and follow-up work:

- Execute the consolidated HDR case on all three platforms, retaining decoded
  output and original-raster failure diagnostics. Tests have not established the
  replacement's runtime equivalence yet.
- Add actual Auto-Patch dry/compute integration coverage for the predictive-cache
  subdivision case. Old-scene image comparisons remain necessary; helper tests
  and generic Compat scenes cannot establish behavior for an unrepresented FX.
- Add Windows PSD write/commit failure injection and explicit rename/flush
  failure scenarios. POSIX `RLIMIT_FSIZE` is not proof of Windows filesystem
  behavior or every transaction failure stage.
- Qualify full native UI task/undo workflows under Qt 6. The configured Qt 6
  CTest jobs alone do not run this application driver. The macOS PR application
  job is Intel; unrelated ARM runtime matrices do not qualify these C++ changes.
- The external-effect fixture checks Windows process-handle stability but does
  not prove cleanup of arbitrarily long-running child/grandchild trees. Its
  command/output cases should remain separate from future timeout coverage.
- Plugin single/empty probe tests do not cover every malformed ABI descriptor,
  stale parameter handle, or nonzero-offset raster. Do not claim those paths
  are tested merely because the probe executable passes.
- Execute the expanded CLI boundary case accepting nine and ten checks and
  rejecting eleven. Keep malformed-input tests; parser success does not subsume them.
- Preserve first-run failing CTest logs. The nightly workflow currently reruns
  failed tests to gather annotations; a later diagnostic result must not replace
  the original failure evidence or be treated as a retry that qualified the code.

Follow-up fixtures retain the existing execution paths: `verifyAudioLifecycle`
and `verifyAudioDecode` run within `ffmpeg_public`; `swingUndoChecks` runs within
`runVectorWorkflowNativeChecks`. Their process, timeout, drawing, and exposure
scenarios are recorded in the scenario ledger and Swing report.
`testProbeTimeoutConversion` extends the retained direct FFmpeg suite with a
delayed helper, large timeout conversion, default fallback, and short-timeout rejection.
It is configured for Linux/macOS; an equivalent Windows public case remains missing.
The eight page-heap contracts run early in the existing Windows application job,
before native compilation. They mock the registry/process boundaries and do not prove
that CDB or runtime heap instrumentation works on the runner.

At draft head `2758123e81050f45aaec2c105bb24a729cfd0f27`, the Linux CI steps for
the 20 auditor tests and 15 budget tests passed in
[job 128529701035](https://github.com/OpenAnimationLibrary/groundzero/actions/runs/42414551454/job/128529701035).
That run predates the expanded budget boundary and the follow-up C++ fixtures.
No numeric coverage, mutation score, cross-platform pass, or application rendering
result is asserted here.
