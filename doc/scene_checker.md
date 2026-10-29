# Scene checker

The scene checker lists what is wrong, or worth a look, in a scene: cells that
show drawings their level does not have, level files that are missing, empty
columns, levels and styles nobody uses, look-alike styles, and settings that
do not agree with each other (DPI, frame rates, resolutions). It is meant to be
run before a render, or before a scene is handed on.

It only reads the scene: it changes nothing in it, and nothing is saved in the
scene. A scene that is never checked behaves exactly as before.

## What is checked

Each issue has a severity (error, warning or info), a kind, and where it is: a
column and rows, a level, a style.

| Kind | Severity | Found when |
|---|---|---|
| Missing Drawings | error | A cell shows a drawing its level does not have (the cell the Xsheet draws in red), or a frame past the end of a sub-xsheet. Consecutive cells showing the same missing drawing are one issue. |
| Missing Files | error | The file of a saved level, palette level or sound is not on disk. Paths are decoded first (`+drawings/...`, `$scenefolder`...). The cells of such a level are not listed as missing drawings: the file is the issue. |
| Unsaved Levels | info | A level was never saved: its drawings are only in memory. |
| Empty Columns | info | A column has no cells. |
| Unused Levels | info | A level of the cast is used in no xsheet (sub-xsheets included). |
| Unused Styles | info | No drawing of the levels in the xsheets uses a style of their palette (ink or paint of a Toonz Raster drawing, a stroke or a region of a vector drawing). As Erase Unused Styles, the first two styles of the first page are never listed. This one loads every drawing, so it is an option, off unless asked. |
| Duplicate Styles | info | A style looks the same as an earlier one of its palette: same type, same main colour, same colour and other parameters. The names are not compared. |
| Level DPI | warning | A Raster or Toonz Raster level used in an xsheet has a DPI (custom, or of its images, as its DPI policy says) more than 1% away from the camera's, so it does not show at its size in pixels, or it has no DPI. Vector levels have no DPI and are not checked. |
| Frame Rates | warning | A sound used in a sound column is laid out at another frame rate than the output's (it drifts out of sync), or a movie level's file runs at another frame rate (one frame per cell, it plays at another speed). |
| Resolution | error, warning or info | The camera has no resolution (error); its pixels are not square (warning); the output is shrunk, or the output aspect ratio changes the frame size (info); an mp4, webm or mov output would have an odd width or height (warning); the preview camera has another resolution than the output camera (info). |

The top xsheet and every sub-xsheet used in it are walked, each once. An issue
inside a sub-xsheet gives its column and rows in that sub-xsheet, and the
sub-xsheet's name.

Issues are sorted with the errors first, then by kind.

## The Scene Check panel

**Render > Check Scene...** opens the Scene Check panel (floating, unless one is
already shown in the room) and checks the current scene. The panel can also be
docked in a room like any other.

- At the top, the scene's **statistics**: the drawings of the levels used in the
  xsheets, the levels of the cast, the non-empty columns of the top xsheet, the
  frames, the styles of the palettes (each palette once) and the size of the
  scene file ("not saved" for an untitled scene).
- **Check** runs the check again; it also runs when the panel opens and when
  another scene is opened. **Unused Styles** adds the slow check of the unused
  styles.
- The issues are grouped by kind, with their count, and the panel's title gives
  the counts by severity ("Scene Check: 2 errors, 1 warning, 3 info"). The
  combo box shows all issues, the warnings and errors, or the errors only, and
  the filter field keeps the issues whose text or kind contains what is typed.
- A **double-click** on an issue makes its column and frame current (for an
  issue inside a sub-xsheet that is not open, the column of the sub-xsheet and
  its first frame), its level current, and its style current in the level's
  palette.

Saved menus of earlier versions get the command added after Output Settings
(or at the end of the Render menu) when they are loaded.

## Checking before rendering

**Preferences > Preview > Render > Check the Scene before Rendering and Ask
when It Has Errors** is off by default. When it is on, Render (and Save and
Render, Fast Render) first checks the scene. If the check finds errors, the
Scene Check panel shows them and a dialog asks whether to **Continue** the
render or **Cancel** it (the default). Warnings and info do not stop the
render. The preview is not checked.

## How it works

`SceneChecker::check()` (`toonz/scenechecker.h`, `toonzlib/scenechecker.cpp`)
takes a `ToonzScene` and options (`unusedStyles`, and the function that reads a
movie's frame rate, which the tests replace) and returns the issues. The
levels of the cast come from the `SceneResources` visitor, the levels used from
`TXsheet::getUsedLevels()`. `SceneChecker::statistics()` counts the drawings,
levels, columns, frames and palette styles of the scene and the size of its
file. `SceneChecker::usedStyleIds()` lists the styles a level's drawings use.

The panel is `SceneCheckPane` (`toonz/scenecheckpane.cpp`, panel type
`SceneCheck`); the command is `MI_CheckScene`; the preference is
`checkSceneBeforeRendering`, read in `RenderCommand::init()`
(`toonz/rendercommand.cpp`) for the full render.

Tested by `toonzlib/tests/scenechecker_test.cpp`, on scenes built in memory:
each check once with the issue and once without.
