# Native UI integration validation

The `BUILD_UI_PORT_TESTS` build includes a bounded `--test-native-ui` mode in the
actual application executable. It uses normal native startup, the command
registry, room factories, Preferences controls and scene selection handlers.
Builds and execution belong in CI, as required by the repository instructions.

`ci-scripts/run_native_ui_test.py` creates and removes the fixture. Its arguments
are the built executable, the built `stuff` tree and `--output` followed by the
CI result filename. Linux wraps this runner in `xvfb-run -a` and selects the
`xcb` Qt platform; Windows supplies the build's executable and Qt runtime paths.

The runner must create a fresh temporary directory using the platform temporary
directory API, copy a clean installed `stuff` tree into its `portablestuff`
subdirectory, and write `.groundzero-native-ui-test` containing exactly
`groundzero-native-ui-test` followed by a newline. Run the application with that
temporary directory as its working directory and `--test-native-ui` as its only
argument. Apply a process timeout of 180 seconds and delete the temporary tree
after the application exits. Linux requires a virtual display with working
software OpenGL; this is an application test rather than an offscreen QWidget
test. Windows runs the same executable mode on the CI desktop.

The copied data must include compiled themes, including Synthwave, and the
Japanese `toonz`, `toonzqt`, `tnzcore`, `toonzlib`, `colorfx`, `tnztools` and `image`
translation catalogs under `config/loc/日本語`. Missing required data fails the
test rather than silently skipping appearance coverage. The runner must use
fresh repository/build data, never an existing personal portable profile.

Preparation rejects missing markers, directories outside the platform temporary
directory and symbolic links in the copied portable tree. Native configuration
and preferences paths are checked again after initialization. The test sets
QSettings paths inside the disposable directory; the portable startup path
avoids registry and personal stuff initialization. Startup update checks,
autosave/recovery, startup popups and model preloading are suppressed in this
mode. Normal application startup and the normal event loop still run.

The fixture checks:

- Native Command Palette registration, opening, instance reuse and current
  Library, Motion Presets and Width/Pump command identities.
- Context-only commands remaining unavailable without a valid native target.
- The real Xsheet column selection surviving palette opening and Escape.
- Enter triggering the real Outline command once, affecting selected columns
  instead of the differently focused current column, with working undo/redo.
- Japanese switching through the actual Preferences language control,
  persistence in the isolated profile and translation of an existing native
  action without replacing its pointer or shortcut.
- Live composite Xsheet captions updating while the document name, room name
  and a custom panel caption matching a translated source remain unchanged.
- Repeated English/Japanese switches in real Inbetween and Convert dialogs,
  preserving interpolation keys, custom strengths, TLV conversion tokens and
  selected indices without emitting editing signals or starting conversion.
- A real Drawing Substitution pane switching from an empty-cell caption to an
  identically named level, preserving that literal level name through language
  changes. Only this unfinished catalog source uses a scoped fixture translator
  with a distinct alternate caption; other translations use the native catalogs.
- Synthwave switching through the actual Preferences theme control, with the
  native selection, scene dirty state and undo history preserved.
- Native Widget Inspector registration, inspecting a real palette field and
  cancelling its picker without affecting the scene.

The fixture creates three in-memory exposures of one disposable vector drawing.
It saves no scene or drawing and logs only fixed assertion descriptions, never
document data or resolved file paths. Success prints
`native-ui-integration: PASS` and exits with status zero. An assertion or native
exception returns a nonzero status; a startup hang is caught by the CI timeout.

This coverage does not certify tablet drivers, rendering output, every tool's
selection mode, graphics hardware or a complete interactive animation session.
CI results must be reported separately from manual application validation.
