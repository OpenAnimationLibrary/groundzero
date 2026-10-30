# Shared offscreen rendering prerequisite

The workflow export changes require the same Qt offscreen context implementation
used by later vector editing changes. This prerequisite replaces the Windows
bitmap/GDI OpenGL context for application rendering. The existing production
implementation is shared; later vector work adds stroke features to this backend.

The Qt context owns a GUI-thread-created offscreen surface and a context used by
the requesting thread. The constructor does not hold the legacy Linux GLX mutex
while synchronously requesting that surface. GUI palette rendering and background
frame rendering can therefore overlap without the former lock inversion.

`tconverter` creates a GUI application for vector/scene input. Raster-only
conversion retains a core application and works without a display/platform
plugin. The Explorer thumbnail DLL keeps its host free of Qt GUI state and calls
the installed converter in a bounded child process. Size, payload, timeout,
cancellation and temporary-file checks apply before returning a thumbnail.
Scene thumbnails first use normal project loading. When no project is available
and the loader has not read a scene, the icon loader reads the standalone TNZ
using the existing scene-relative asset rules. It does not create a Sandbox or
write a project just to display a thumbnail.

## Acceptance

Source and native fixtures are available; execution of this prerequisite revision
is pending CI. Builds and tests are not run locally.

- The actual application creates overlapping GUI-palette and background contexts,
  checks their pixels, and uses an independent watchdog for a stalled GUI thread.
- The native fixture saves an ordinary PLI, reopens it to verify its displayed
  palette style, color, stroke geometry and scalar opacity, then launches the
  installed converter. An independent PNG reader requires opaque visible artwork.
- A raster-only child receives an invalid GUI platform and no display variables;
  its output must preserve the source pixels.
- Windows exercises the actual Shell COM provider on a worker thread for Unicode
  PLI and scene paths, compares every pixel with the real converter, verifies the
  host remains a core application, and covers tiny requests, cancellation,
  deadlines, malformed payloads and temporary-file cleanup. Saved TNZ inputs
  independently verify their camera, cell and vector resource; absolute and
  `$scenefolder` asset paths must render nonwhite artwork at the saved camera's
  aspect ratio. Standalone-scene correction qualification is pending fresh CI.
- All three native workflows require `offline-gl-workflow: PASS`. Windows also
  requires the registered Shell fixture. The common Mesa launcher gives the
  renderer and converter the same verified desktop GL and removes temporary DLLs
  and activation markers before packaging. Launcher contracts preserve child
  failures, timeout cleanup and raw Unicode diagnostics. Native, Shell and
  debugger launches stop the complete process tree on timeout before releasing
  fixture data or renderer files. If termination cannot be confirmed, those
  resources are retained and the check fails. A separate bounded read of the
  capture file preserves diagnostics without waiting for descendant pipe EOF.

Along-stroke opacity profiles and their newer native format are deliberately
covered by the later vector changes. They are not prerequisites for these
ordinary-image/context/CLI/Shell acceptance checks.
