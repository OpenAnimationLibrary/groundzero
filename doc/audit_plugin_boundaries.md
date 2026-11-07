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
and handle counts across repeated launches. It does not claim that the decoded
pixels are applied to the render tile.

`plugin_probe_single` and `plugin_probe_empty` use a real native module through
the public loader. They check both the recorded load result and whether the
effect is available for creation. The fixture module is built beneath a private
test directory and has no installation rule.

Builds, baseline-failure checks, and runtime tests remain pending in CI, following
the repository's prohibition on local builds and tests. Static formatting and
whitespace checks are the local validation performed.

## Unresolved external-effect behavior

**Decoded output is not applied to the render tile.**
`TExternalProgramFx::doCompute` passes a local `TRasterP` to
`TImageReader::load`. The implementation in
`toonz/sources/common/timage_io/timage_io.cpp` replaces that local pointer with
the decoded raster; it does not update the original tile's raster. The present
patch checks the decode result but deliberately leaves assignment unchanged.
Applying the output needs a separate rendering change, including the policy for
output dimensions and pixel types. It would change scenes whose external
effects currently leave their input pixels unchanged, so old-scene rendering
must be compared before acceptance.

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
