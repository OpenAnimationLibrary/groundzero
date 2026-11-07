# Native crash investigation, 2026-11-07

## Scope and evidence

This investigation separates the integration branch's workflow teardown crash,
an independent alignment timeout, and a newer test-fixture crash in PR141.
They require different fixes. A process exit code of 1 alone does not identify
which failure occurred: the Windows crash reporter catches access violations
and the test launcher subsequently reports that status.

Full job logs were read through the GitHub connector. The small diagnostic
artifacts listed below were downloaded and their SHA-256 digests verified
against GitHub metadata. Executables and crash dumps were not executed locally.
No local build or test was run, following `CLAUDE.md`.

| Evidence | Revision or retained runtime | Observation |
| --- | --- | --- |
| [Edition Windows job](https://github.com/OpenAnimationLibrary/groundzero/actions/runs/42297300171/job/128167284089) | `2e52fb78b70915925393290f32dc09a95a3927ec` | Original access violation in Qt text-layout destruction; independent workflow-switch has the same failing Qt function through another line-edit owner. |
| [Edition diagnostic artifact](https://github.com/OpenAnimationLibrary/groundzero/actions/runs/42297300171/artifacts/13546987383) | Same revision | Original output, independent workflow-switch and workflow-ui transcripts. |
| [Retained-runtime debugger job](https://github.com/OpenAnimationLibrary/groundzero/actions/runs/42330745601/job/128276455179) | Retained runtime `08a4b6fd12860cfb64c0845ac3d601cd77ab6d1f`, not the diagnostic branch's source revision | First-chance access violation at `QTextEngine::~QTextEngine+0xb2`; a null internal member is dereferenced. |
| [Earlier retained-runtime debugger job](https://github.com/OpenAnimationLibrary/groundzero/actions/runs/42330337243/job/128275178234) | Same retained artifact `13555851183` | Same Qt destructor signature. |
| [Later retained-runtime debugger job](https://github.com/OpenAnimationLibrary/groundzero/actions/runs/42332081822/job/128280648806) | Same retained runtime | Timed out after 900 seconds at initial debugger commands; provides no completed verifier diagnosis. |
| [PR141 Windows job](https://github.com/OpenAnimationLibrary/groundzero/actions/runs/42342872309/job/128314993429) | `a0406efc4a8f90a74d77bb5ddbab1d0d8aeabeeb` | Original crash in `BaseViewerPanel::getPreviewButtonStates` while showing a newly constructed fixture viewer. |
| [PR141 second Windows job](https://github.com/OpenAnimationLibrary/groundzero/actions/runs/42342857539/job/128315200193) | Same PR141 revision | Native integration fails; its status is not evidence that the edition teardown crash has been fixed. |
| [PR141 macOS job](https://github.com/OpenAnimationLibrary/groundzero/actions/runs/42342857600/job/128314492835) | Same PR141 revision | Original-process IPS report matches executable UUID and PID; `EXC_BAD_ACCESS`, address `0x9d`, same preview-button function. |
| [PR141 Linux job](https://github.com/OpenAnimationLibrary/groundzero/actions/runs/42342857517/job/128314174502) | Same PR141 revision | Original status -11; independent debugger replay stops in the same preview-button function. Original core metadata did not identify the exact executable. |
| [PR141 CDB artifact](https://github.com/OpenAnimationLibrary/groundzero/actions/runs/42342872309/artifacts/13563883652) | Same PR141 revision | First-chance access violation reads an uninitialized preview-button pointer. |

The artifact digests are:

| Artifact | SHA-256 |
| --- | --- |
| Edition artist-workflow diagnostics, `13546987383` | `aa26b099b8e20cca323f50cf0697353cd371b5a0f6b9753251bfc511c297dd99` |
| PR141 page-heap/CDB transcript, `13563883652` | `d4e2d45bc1efbb2c8c8e58f9010c68305bd457b3ee1aa34d17c7d5a4959bc89e` |
| PR141 artist-workflow diagnostics, `13564073082` | `14988917ba6fdb6055b77e5a43eda40cd6469d22c047686a817036979f4c5507` |

## W1: edition workflow teardown access violation

**Priority:** P1, observed application crash. **Root cause:** unresolved.

The original Windows transcript reports `EXCEPTION_ACCESS_VIOLATION` after
visiting Character rooms and requesting `Default`, with lazy loading disabled.
Its stack is:

```text
QTextEngine::~QTextEngine
QTextLayout::~QTextLayout
QWidgetLineControl::~QWidgetLineControl
DVGui::LineEdit destructor
DvItemViewerPanel / DvItemViewer / FileBrowser destruction
TPanel / Room destruction
MainWindow::clearWorkflowRooms
MainWindow::switchWorkflow
exerciseWorkflowSwitching
```

The independent workflow-switch diagnostic reaches the same Qt destructor
through `XsheetGUI::RenameColumnField`, `ColumnArea`, and `XsheetViewer`.
The source trigger is the explicit room deletion in
[clearWorkflowRooms at the tested revision](https://github.com/OpenAnimationLibrary/groundzero/blob/2e52fb78b70915925393290f32dc09a95a3927ec/toonz/sources/toonz/mainwindow.cpp#L1284),
called by
[switchWorkflow](https://github.com/OpenAnimationLibrary/groundzero/blob/2e52fb78b70915925393290f32dc09a95a3927ec/toonz/sources/toonz/mainwindow.cpp#L1366).
The stack identifies where invalid state is consumed, not where it was created.

The retained-runtime CDB trace adds a useful boundary: at
`QTextEngine::~QTextEngine+0xb2`, the instruction reads through `RDX == 0`.
The preceding instruction loaded that pointer from the text-engine object's
offset `0xa0`. This trace reaches the same destructor through an `IntLineEdit`
inside `ToolBarContainer`. The responsible object field and earlier write have
not been verified against matching private Qt symbols. Calling this a proven
font-cache, double-delete, or OpenGL corruption bug would exceed the evidence.

[Qt 5.15.2's destructor source](https://github.com/qt/qtbase/blob/v5.15.2/src/gui/text/qtextengine.cpp#L1826)
releases layout data, special data, cached font engines, and subsequently its
members. The crash instruction is therefore useful for narrowing a debugger
watchpoint, but the public function name alone does not establish which member
or owner first became invalid.

The edition page-heap diagnostic is separate from the original process. It
ends while loading Character with `3221226529 (0xC0000421)` and explicitly
reports that no crash report was available. It supplies no offending write
stack. The large original failure bundle, artifact `13546751245`, was listed
as available until 2026-11-09; it was not downloaded during this investigation.

PR141 already changes SceneViewer context retirement and display-list proxy
ownership. Those source defects are relevant to repeated viewer destruction,
but no trace reviewed here establishes that they wrote the invalid Qt state.
Do not duplicate that patch or describe it as a verified W1 fix.

## W2: PR141 constructs an incomplete viewer fixture

**Priority:** P2, blocks native acceptance coverage on three platforms.
**Root cause:** confirmed by source and remote debugger evidence.

PR141's
[exerciseSceneViewerContextOwners](https://github.com/OpenAnimationLibrary/groundzero/blob/a0406efc4a8f90a74d77bb5ddbab1d0d8aeabeeb/toonz/sources/toonz/nativeuiintegrationtest.cpp#L411)
constructs `SceneViewerPanel` directly, then shows it. Showing the contained
SceneViewer changes the active viewer and reaches
[getPreviewButtonStates](https://github.com/OpenAnimationLibrary/groundzero/blob/a0406efc4a8f90a74d77bb5ddbab1d0d8aeabeeb/toonz/sources/toonz/viewerpane.cpp#L516).

The two preview-button pointers are declared without initializers in
[BaseViewerPanel](https://github.com/OpenAnimationLibrary/groundzero/blob/a0406efc4a8f90a74d77bb5ddbab1d0d8aeabeeb/toonz/sources/toonz/viewerpane.h#L58).
Their initialization belongs to the real
[SceneViewerPanelContainer construction path](https://github.com/OpenAnimationLibrary/groundzero/blob/a0406efc4a8f90a74d77bb5ddbab1d0d8aeabeeb/toonz/sources/toonz/tpanels.cpp#L1534),
which invokes `initializeTitleBar`. The fixture bypasses that path.

The PR141 CDB transcript reads `m_previewButton` into
`RAX = 0xc0c0c0c0c0c0c0c0`, then faults reading `RAX + 0x9d` at
`getPreviewButtonStates+4`. macOS independently records invalid address `0x9d`
in the same function. These observations support uninitialized fixture state;
they do not demonstrate a failure of the new GL ownership assertions.

A separate four-line-addition, one-line-deletion follow-up patch was prepared
against exact PR141 head `a0406efc4`. It creates the viewer through
`TPanelFactory::createPanel(window, "SceneViewer")`, verifies the contained
SceneViewer, and retains all existing context-retirement, surviving-viewer,
framebuffer, and reparenting checks. It changes no production GL code and is
not included silently in the edition audit branch.

The proposed replacement for the direct `SceneViewerPanel` construction is:

```cpp
auto panel = std::unique_ptr<TPanel>(
    TPanelFactory::createPanel(window, QStringLiteral("SceneViewer")));
require(panel->findChild<SceneViewer*>(),
        "native SceneViewer factory did not create its viewer");
```

Static formatting, whitespace, privacy, and patch checks passed. The corrected
fixture has not been built or executed. Its next CI run must reach the existing
GL assertions and then the workflow teardown scenarios; a passing constructor
alone is insufficient.

## W3: independent alignment diagnostic timeout

**Priority:** P2, incomplete independent diagnostic. **Root cause:** unresolved.

The edition `ui-integration-output-workflow-ui.txt` stops at alignment review 6,
Align Left, two drawings, accepted. Both preview comparisons pass; the log
then reaches the intervening callback, action click, and command return.
The diagnostic is terminated after 120 seconds. This is neither evidence of a
network failure nor the W1 text-destructor exception.

The next investigation should retain the exact UI-thread stack when this
deadline is reached and identify any open modal widget and active callback.
Do not increase the timeout or skip the changed-source scenarios without that
evidence. Earlier review 5 correctly rejects changed source drawings and
displays its explanatory warning, so removing that review behavior would
weaken the tested contract.

## Differential CI verification

Use separate disposable fixture state for each scenario and retain the exact
revision, renderer, Qt runtime, lazy-loading mode, and original result.
Additional diagnostics must not replace or turn a failing original result green.

| Scenario | Comparison | Required evidence |
| --- | --- | --- |
| Workflow replacement | All-workflows and isolated workflow-switch; eager and lazy rooms | Original failure or pass, last profile/room, original exception stack; complete every preset transition. |
| Text-editor teardown | FileBrowser rename field, Xsheet rename field, toolbar numeric field | Object lifetime and thread evidence; first invalid write or free if the Qt member is corrupted. |
| Viewer context ownership | Two real factory-created viewers; retire one, use survivor, reparent survivor | Preview buttons initialized; each live context remains registered; original pixel/context assertions retained. |
| GL change isolation | Edition versus the existing PR141 ownership change with its fixture corrected | Determine whether the former W1 teardown point is reached and whether its signature persists. Do not infer causation from different early failures. |
| Alignment review | Unchanged source, source mutation, frame switch, cancel, accept | Deadline stack and modal state; rejected reviews leave artwork/history unchanged; accepted preview matches applied output. |
| Windows heap diagnosis | Normal run versus separately identified instrumented diagnostic | Confirm runtime instrumentation, capture first exception/verifier stop, preserve registry state, and report debugger timeouts explicitly. |

No new application crash fix is claimed by this document. W2 has a concrete
isolated fixture correction awaiting CI; W1 and W3 remain acceptance blockers
requiring further runtime evidence.
