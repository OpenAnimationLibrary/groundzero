# Width tool and width profiles

Two ways to shape the thickness of a vector line, inspired by the Width tool
of Adobe Animate:

- **Width mode of the Pump tool**: drag on a stroke to make it thicker or
  thinner at that spot.
- **Width profiles**: give whole strokes a thickness shape (tapered ends, a
  swelling middle) in one command, and save the shape of a stroke you like to
  use again.

Nothing here changes the level or scene file format. A width is just the
thickness stored in the control points of a stroke, which every vector level
already has. A drawing that does not use the feature is saved exactly as
before.

## Width mode (Pump tool)

1. Choose the **Pump tool** and set **Mode** to **Width**.
2. Press on a vector stroke, close to its line. You can press on the line
   itself or a few pixels outside its edge.
3. Keep the button down and drag. The circle drawn on the stroke is the new
   thickness at that spot, and the edge of the line follows the pointer: drag
   away from the line to make it thicker, towards the line to make it
   thinner.
4. Release. The whole drag is one undo step.

Details:

- A control point is added where you pressed, unless one is already within
  about two pixels. Adding a point does not change the shape of the line.
- The change fades out smoothly along the stroke on both sides, so no spike
  appears. **Width Falloff** (1 to 100, in screen pixels) sets how far it
  reaches. A larger value makes a broad swelling, a smaller one a narrow
  bump. Where there is no control point at the end of the fade, one is added
  so that the fade ends cleanly. The value is remembered between sessions.
- A press that does not move (less than about two pixels) changes nothing and
  adds no undo step.
- Thickness is kept between a hairline (0.05) and 250 (both are the radius of
  the line, in drawing units), so a line cannot vanish and cannot grow
  without end.
- Strokes with no thickness (the invisible boundaries used for fills) cannot
  be picked. When strokes overlap, the one drawn on top is picked. Only
  strokes of the group you are inside can be edited, as with the other Pump
  modes.
- The other Pump modes (Pump, Thicken Brush, Thin Brush, Opacity Brush) work
  as before.

## Width profiles

A width profile is a list of points: a position along the stroke (0 at the
start, 1 at the end, measured by length) and a thickness ratio (0 to 1) of
the stroke's largest thickness. Applying a profile to a stroke keeps its
shape and its largest thickness and replaces the thickness along it.

The commands are in the **Level > Width Profile** menu. They work on the
strokes selected with the **Selection tool** (vector drawing):

| Command | Result |
|---|---|
| Uniform Width | The same thickness all along (the stroke's largest) |
| Taper Start | Starts as a point and reaches full thickness over the first third |
| Taper End | Full thickness, then tapers to a point over the last third |
| Taper Both Ends | Pointed at both ends, full thickness in the middle |
| Pulse | Thin at both ends (a quarter of full), swelling to full in the middle |
| Apply Saved Width Profile... | Choose one of your saved profiles from a list |
| Save Width Profile From Selected Stroke... | Name and save the thickness shape of the selected stroke |

Each command is a single undo step, whatever the number of selected strokes.
All of them appear in the shortcut editor, so you can give them shortcuts or
put them on a command bar.

Notes:

- The tips of a taper are a very thin line (0.05), not zero, so the stroke
  never disappears.
- Strokes with no thickness are skipped. If none of the selected strokes has
  thickness you get a message and nothing changes.
- Saving a profile asks for exactly one selected stroke. The thickness is
  sampled along its length and simplified to a few points. The ratio is
  relative to that stroke's largest thickness, so a profile saved from a thin
  stroke can be applied to a thick one.
- Saved profiles are kept for the user, not in the scene, in the text file
  `width_profiles.txt` in the configuration folder of the OpenToonz stuff
  folder. Each line is a name, a tab, then `position:ratio` pairs separated
  by commas. You can edit the file in a text editor, for instance to rename
  or delete a profile (lines starting with `#` and unreadable lines are
  ignored). A profile saved with an existing name replaces it.
- The Selection tool context menu does not list the profile commands; use
  the Level menu or a shortcut.
- Menus saved in an existing room are copies of the menu template, so the new
  Width Profile submenu appears there only when you add it with the menu bar
  customization, or after resetting the menus. The commands are available in
  the shortcut editor right away.

## Limits

- Width mode edits one stroke per drag. It changes the thickness of the
  control points near the pressed point; it does not offer a different width
  on each side of the line (a stroke has a single thickness per point).
- A drag adds up to three control points to the stroke. Dragging many times in
  the same area does not add more once points are close enough to each other.
- Closed strokes (circles, shapes) are handled with the fade wrapping around
  the closing point, but a taper profile applied to a closed stroke makes the
  stroke thin at the point where it closes.
- Profiles are positions by length, not by control point, so a stroke with
  evenly or unevenly spaced points gets the same shape. A profile cannot be
  applied to a selection of several frames or a whole level, only to the
  current drawing.
- A profile replaces the thickness the stroke had (for example the variation
  that came from pen pressure); only its largest thickness is kept.

## How it is built

The arithmetic (evaluating a profile, the built-in profiles, building a
profile from thickness samples, mapping a profile to control points, the
falloff of a width change, and the text format of saved profiles) is in
`toonz/sources/include/tools/widthprofile.h`, which has no dependency on
strokes or the interface. The Width mode is in
`toonz/sources/tnztools/pumptool.cpp`, and the selection commands are in
`toonz/sources/tnztools/widthprofilecommands.cpp`.

The standalone test `toonz/sources/tnztools/tests/widthprofile_test.cpp` covers
profile evaluation (end points, middle, clamping outside 0 to 1), the built-in
profiles, building a profile from a thickness list and evaluating it again,
mapping onto control points (same count, monotone positions, scaled to the
maximum, minimum thickness), the falloff and thickness limits, the text
format, and degenerate input (empty profile, one point, no length). It
builds with the other tnztools tests (`BUILD_TNZTOOLS_TESTS`) and can also be
compiled on its own from the repo root:

```sh
g++ -std=c++17 -Itoonz/sources/include \
  toonz/sources/tnztools/tests/widthprofile_test.cpp \
  -o widthprofile_test
./widthprofile_test
```
