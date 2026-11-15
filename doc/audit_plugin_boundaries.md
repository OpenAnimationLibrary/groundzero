# Plugin and external-process audit

This review inspected the plugin manager, native FX loader, PureRef importer,
related-application launchers, PDF and OTLUT process calls, ZIP export, particle
resource packaging, and legacy external-program effects. It is a source review,
not a claim that every extension or external application is safe.

## Fixes and regression coverage

- **Medium: external-effect launch failures.** In
  `toonz/sources/tnzbase/texternfx.cpp`, POSIX execution previously concatenated an
  uninitialized Windows-only buffer instead of the prepared arguments. Windows
  execution used a fixed 1,024-byte expansion buffer, ignored failed process
  creation before using its handles, and leaked successful process handles.
  Expansion now sizes its buffer, failed launches report context, and successful
  launches close both handles. Nonzero process exits and unreadable output images
  report errors after temporary-file cleanup. Existing shell and environment
  expansion semantics are retained.
- **Medium: single-effect function probes.** In
  `toonz/sources/toonzqt/pluginhost.cpp`, a function probe with one effect was
  rejected as empty, although the SDK's end pointer identifies the zeroed
  sentinel after the last effect. The redundant rejection was removed; an empty
  range is still rejected.
- **Medium: queued plugin publication on Qt 5.** `PluginInformation*` was sent
  from the loader thread to the application thread without a meta-type
  declaration or registration. Both are now provided before the worker starts,
  as required for custom queued arguments by
  [Qt's meta-type documentation](https://doc.qt.io/archives/qt-5.15/qmetatype.html).

`externfx_process` exercises quoted arguments, successful PNG decoding, nonzero
exits, missing output, and malformed output. Its Windows cases additionally
cover expansion beyond 1,024 bytes, a missing executable, an oversized command,
and handle counts across repeated launches. The added public-render cases connect
a real raster source and apply an asymmetric PNG output to an extracted tile.
They check exact colors, premultiplied alpha, orientation, destination storage,
tile position and untouched surrounding pixels. Smaller/larger images and a TGA decoded
as grayscale must report errors without publishing pixels. Grayscale PNG expands to
RGBA32 through the production reader and is accepted by the revised fixture.

`plugin_probe_single` and `plugin_probe_empty` use a real native module through
the public loader. They check both the recorded load result and whether the
effect is available for creation. The fixture module is built beneath a private
test directory and has no installation rule.

The process and loader tests passed [macOS CI at `c20884ffb`](https://github.com/OpenAnimationLibrary/groundzero/actions/runs/42419847532/job/128545825336).
The revised F29 public-render cases pass on Linux/macOS at merge ebe1a05 (CI33/CI37);
required Windows and baseline/mutation qualification remain open. The repository prohibits
local builds/tests; static source/format/whitespace checks are local validation.

## Prepared output repair and remaining external-effect limits

**F29: 32-bit decoded output now updates the existing render tile.**
`TImageReader::load` replaces its raster argument, so loading into a local pointer
previously discarded the output pixels. The effect now checks that the decoded
raster is 32-bit RGBA and exactly matches the requested tile dimensions, then
copies it into the existing destination. This preserves the parent raster's
storage when the renderer passes an extracted tile. It does not resize, crop,
shift or replace the tile. Other decoded pixel types and mismatched dimensions
produce explicit errors after temporary-file cleanup.

This changes scenes whose external effects previously retained input pixels
instead of the program's output. Existing external-effect scenes still need
before/after render comparison. Unsupported output dimensions or grayscale
images now fail instead of silently discarding valid decoded images. The
existing early return for 64-bit and floating-point destination tiles remains
unresolved; this change does not add support for those rendering modes.
F29 retains R08 as a stable alias in the [open finding ledger](audit_findings.json);
the earlier process-test pass does not validate these new pixel assertions.

At exact head `5e9c2f00ccabd48f8f6b80748b924e1f9ea4a23b`, the
[Linux](https://github.com/OpenAnimationLibrary/groundzero/actions/runs/42422881641/job/128554892608) and
[macOS](https://github.com/OpenAnimationLibrary/groundzero/actions/runs/42422881533/job/128554992210)
builds rejected the new `BlueSource` fixture because it lacked the pure virtual
`TFx::getPluginId() const` override. That fixture correction is now prepared; the
failure prevented execution of the public pixel cases. It supplies no behavioral
result for F29, and the earlier process-test pass still predates these cases.

**Configuration trust needs a separate decision.** Scene deserialization accepts
an external effect's executable path, argument string, and configuration name.
Ordinary scene rendering clones the effect; `TExternalProgramFx::clone` rebuilds
it through `initialize(name)`, which reads
`getBinDir()/plugins/externFxs/(name + ".xml")`. The name is not checked for
containment, and the selected XML supplies executable commands. Therefore the
serialized executable and arguments alone do not demonstrate ordinary-render
code execution. The availability of attacker-controlled XML and the complete
rendering path still need validation. Restricting configuration locations or
introducing execution consent would affect existing workflows and requires a
maintainer decision. No trust policy was changed in this patch.

## Prepared ownership follow-up and residuals

F39 owns external-FX input ports, including duplicate-name allocations; sixteen multi-port
cycles and eight malformed loads assert upstream reference balance and final destruction.
F40 corrects enum/spectrum array deleters; real sixteen-replacement setup/value checks still
need allocation-mismatch instrumentation. F48 balances metadata references across eight real
hosts with a destroy callback and eight without it. Plugin probe CTests passed on all three
platforms at merge f8a1680. The F39 owner checks returned before the later external-FX
pixel failures in that older run. The whole Linux/macOS external-effect targets now pass
at ebe1a05 (CI33/CI37). Targeted ownership/deleter mutations remain pending. See the
[November 14 qualification](audit_ci_2026_11_14.md).

F61/F62 now implement the approved direct-settings and immutable-generation design for
those separate allocations and borrowers. Their source/fixtures postdate ebe1a05 and await
CI. R17 remains open for partial factory/clone construction and shared-state qualifications;
R22 separately tracks active histogram/controller retirement. Process-lived registration and
visited cached viewer generations remain intentional owners.

R20 now aliases prepared F49. Both discovery branches notify controller completion.
Startup polling deletes its controller only after wait succeeds; listener controllers retire
after thread completion independently of listener survival. The actual loader/probe fixture
observes controller, worker and thread retirement, with four normal/early-close menu pairs.
Linux/macOS probe targets pass at ebe1a05, with no individual case success markers.
Required platform/mutation qualification and any RAM measurement remain open.

F51 removes menu destruction as an owner of the global plugin registry. Persistent FX
declarations borrow that process-lived metadata; Room/SchematicViewer destruction must not
release it. The fixture creates hosts through both PluginLoader and the registered TFx
factory before and after menu/last-host retirement. Intentional process-lifetime registry and
library retention remains; F61/F62 separately own the descriptor/layout repair, and R17
keeps its unresolved subcases.

F52 deletes owned root menus and old submenu trees, parents the repeat action, and replaces
only the named plugin group when overlapping discovery completes. Preset-save/remove and
rapid-refresh cases require retired QPointers, one plugin entry per menu, and survival of
externally owned actions and unrelated groups. Linux/macOS probe targets pass at ebe1a05;
case execution follows source ordering, with required platform/mutation evidence still open.

F57 prefixes the selected executable as quoted Windows argv[0] and shell-quotes the POSIX
executable path. Its child reports the actual argv[0]; the POSIX link includes spaces, an
apostrophe and a dollar sign. Legacy argument-shell semantics and Windows ANSI launch remain
unchanged. Linux/macOS launch/output validation passes at ebe1a05 (CI33/CI37);
Windows identity and defect-restored qualification remain open.

At tested merge `ebe1a05083f3e33ecdb1ab15fb4ccaafd2fef240`, Windows passed 21 of 22
reliability tests; `externfx_process` stopped at its executable-identity assertion.
The fixture compared Qt's forward-slash application path with native `argv[0]`, but the
selected API takes a `TFilePath`, which normalizes Windows separators before launch.
The corrected fixture requires byte-exact equality with that selected native path and
retains POSIX link spelling. Empty, truncated, or substituted executable arguments still
fail. Failure output reports only lengths, the first differing byte offset, and byte
values; it does not publish paths, other arguments, or environment values. Launch code
is unchanged. This source-confirmed fixture correction has not yet run in CI.

## Other inspected boundaries and limits

Plugin package installation rejects symlinks, stages outside the scanned tree,
and refuses existing destinations. The plugin inventory reads filenames without
loading libraries. Native libraries remain trusted executable extensions once
the user installs and enables them.

PureRef imports reject command-separator characters in relevant paths, launch
with separate process arguments and private settings, validate image headers
and decoding, and publish cache generations transactionally. Cache manifests
limit generation names and image paths. No confirmed remote-input escape was
identified in these inspected paths; hostile code running as the same local
user was not treated as an isolated attacker.

The inspected application code writes ZIP archives rather than extracting them.
Cross-platform interpretation of archive filenames remains unverified. Particle
package import preserves valid external read references while generating or
checking destination-relative paths. These observations are not a full audit of
all image decoders, system loaders, third-party tools, or archive consumers.

## Approved storage follow-up

The user approved both stages in [the plugin storage design](audit_plugin_storage_design.md):
direct settings construction and immutable descriptor generations. F61/F62 implement the approved stages in commit `259f9ac45`, with CI pending. R17
remains open for partial factory/clone construction, user-data aliases and shared-state
qualifications. The Linux/macOS probe passes at head `376d2e6ce` qualify earlier
controller/registry/menu repairs, not these later storage changes. R22 separately tracks
active histogram ownership and borrowed widgets; visited cached generations remain
intentional owners until their viewers/pages retire.
