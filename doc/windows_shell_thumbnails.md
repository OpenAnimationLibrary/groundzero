# Windows Shell thumbnails

Explorer's thumbnail provider loads no Qt or Toonz rendering libraries. It
resolves the installed `tconverter.exe` beside `toonzpreview.dll` and runs one
disposable renderer process per request. The renderer constructs its own
`QGuiApplication` on its main thread, so the Qt OpenGL backend works for ordinary
PLI drawings, stroke opacity envelopes and vector artwork inside TNZ scenes.
The provider never upgrades an existing `QCoreApplication` or creates a GUI
application on a COM worker.

The helper entry point is:

```
tconverter --shell-thumbnail ABSOLUTE_INPUT SIZE ABSOLUTE_OUTPUT
```

This private mode uses the existing `IconGenerator` implementations for PLI,
TNZ, TLV and TGA, including first-drawing selection and the scene thumbnail's
normal background and aspect ratio. Existing conversion arguments keep their
previous behavior. The output is a private, versioned bitmap exchange file,
not a project or image interchange format. Its explicit little-endian header
describes dimensions followed by bottom-up, premultiplied BGRA bytes. It is
written atomically only after rendering succeeds, without an overwrite prompt.

The Shell provider caps output at 1024 pixels per side and checks the header,
dimensions, exact byte count and premultiplied channels before creating a
32-bit DIB. Small requests render with enough room for the icon renderer's
fitting margin and are reduced to the requested size afterward. Unicode paths
and spaces are passed through Windows command-line quoting and Qt's Unicode
argument handling.

Requests have a 20-second rendering deadline. The child starts suspended and is
placed in a kill-on-close Windows Job before execution. COM cancellation,
timeout and process failures terminate the renderer and its descendants. The
provider waits at most two additional seconds after termination and removes
its private request output files and directory; it never follows directory
junctions during cleanup. Missing helpers, malformed results and unsupported
input return a failing HRESULT with no bitmap. The renderer executable is
selected by its absolute installed sibling path, without a PATH search.

Static palette-icon registration uses the existing Windows configuration
location (`HKLM\SOFTWARE\OpenToonz\OpenToonz`, `TOONZCONFIG` or `TOONZROOT`),
or the installed sibling `portablestuff` tree, without loading Qt in Explorer.

## CI coverage

The Windows `ui_shellthumbnail` fixture creates real ordinary and envelope PLI
files and TNZ scenes that reference them. It compares the actual thumbnail CLI
output with bitmaps from the DLL's declared COM class factory and
`IThumbnailProvider::GetThumbnail`, invoked on a separate STA worker while the
host owns only a `QCoreApplication`. It verifies visible artwork and envelope
differences through RGB; normal icon backgrounds may be opaque. It also checks
the Shell ARGB contract, Unicode paths, a one-pixel request, timeout,
cancellation, malformed output, missing executables and request-directory
cleanup. Timeout and corrupt-output children belong only to the test fixture.

The launcher uses disposable application settings and the existing pinned
Windows Mesa test renderer. `ui_shellthumbnail_test.exe` is a CI-only artifact;
the installed application needs only the normal `tconverter.exe` and
`toonzpreview.dll`. The fixture and renderer changes await supported CI;
builds and tests were not run locally.
