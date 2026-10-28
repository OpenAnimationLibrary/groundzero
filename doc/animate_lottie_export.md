# Lottie export

**File > Export > Export Lottie...** writes the current xsheet as a Lottie
animation: a `.json` file in the Bodymovin format that lottie-web, the iOS and
Android Lottie players and most Lottie tools play. Vector drawings stay
vectors (shapes), the column motion stays keyframes, sub-xsheets become
precompositions. Nothing is added to scenes and no file format changes.

This is a first version: what it cannot express is left out and listed at the
end of the export (see [What is not exported](#what-is-not-exported)).

## Use

The command opens the save dialog with a few options under the file list:

- **From Frame** / **To Frame**: the frames to export. They default to the
  output range of the scene (Output Settings), or to the whole xsheet when no
  range is set or when a sub-xsheet is open.
- **Frame Rate**: the frame rate of the animation; the scene's by default.
- **Embed Raster Levels as PNG Data**: checked, the images of the raster
  drawings are stored in the JSON file itself (as base64 PNG data); unchecked,
  they are saved as PNG files in a `<name>_images` folder next to the JSON
  file, which must then be copied with it.

When the file is written, a message says what was not exported, if anything,
and offers to open the folder.

## What is exported

The current camera is the composition: its resolution in pixels is the
width and height of the animation, and the exported frames are its frames
(the first exported frame is Lottie frame 0).

| In the scene | In the Lottie file |
| --- | --- |
| Each visible column (render toggle on), top column first | A null layer with the column placement, and the layers of its drawings, parented to it |
| A pegbar, the table or a column which is the parent of a column | A null layer with its placement, the column layers parented to it (left out when it never moves its children) |
| A moving camera | A root null layer with the inverse camera motion |
| A run of the same vector drawing in a column | A shape layer, visible from the first frame of the run to the end of its last one (`ip` / `op`); a drawing change starts a new layer |
| A raster drawing (Toonz Raster or Raster level) | An image layer showing an image asset, at the level resolution, placed and scaled as in the camera |
| A sub-xsheet column | A precomposition layer: the sub-xsheet is a composition asset built in the same way (its own camera is its frame), and the sub-xsheet frames shown are a time remap with hold keys |
| The column opacity | The opacity of the column's drawing layers (a static value) |
| The column time remap | Followed: the drawings shown are the remapped ones |

**Placements.** The placement of every stage object is baked at every
exported frame, relative to its parent (so hooks, parent handles and Plastic
skeleton handles are followed as rendered), and split into position,
rotation and scale. Keys that linear interpolation restores (runs of equal
values, constant speed moves, within 0.01 pixel, degree or percent) are
dropped. Lottie is y-down: a counterclockwise rotation of the stage becomes a
negative Lottie rotation, and the positions are measured from the top left
corner of the camera. Lengths are camera pixels.

**Vector drawings.** A shape layer holds the drawing in its stacking order
(as OpenToonz draws it: group by group, the filled areas of a group under its
strokes):

- A **filled area** (a region with a style) is a group with its outline as a
  path and a fill of the style color. The areas inside it (its subregions) are
  added to the group as more paths, and the fill uses the even-odd rule, so
  they are cut out as holes, then drawn on top with their own style. An area
  without a style is not drawn, but the areas inside it are.
- A **stroke** whose thickness is about constant (within 10%) is a Lottie
  stroke on its centerline, with the average width and the cap, join and
  miter limit of the stroke. A closed stroke is always a Lottie stroke.
- A **stroke of varying thickness** (pressure) is its outline shape, filled
  (the same conversion as the Contour Editor's, see
  [contour_editor.md](contour_editor.md)).
- Colors are the main color of the style; the alpha of the color and the
  stroke opacity give the opacity of the fill or the stroke. Strokes of zero
  thickness (the invisible region boundaries) are not drawn.

Quadratic curves are written as the equal cubic Bezier curves Lottie uses.

**Numbers.** Every number is checked: a value that is not a number or is
infinite is written as 0, so the file always parses.

## What is not exported

These are skipped, and listed in the message at the end of the export:

- **FX**: columns are exported without their effects; FX columns are skipped.
- **Column color filters** and **blend modes** (which come from FX).
- **Clipping masks**: a mask column is skipped, the masked columns are
  exported unclipped.
- **Z depth and perspective**, and **stacking order (SO)** changes: the column
  order of the xsheet is kept.
- **Shear**: Lottie layers are written without skew, so a sheared placement
  (shear channels, or the rotation of a child under a parent scaled
  unevenly) loses its shear.
- **Plastic deformations and mesh columns**: the drawings are exported
  undeformed, at the placement of their columns.
- **Special styles**: gradients, textures, patterns and brush styles are
  exported with their main color. Per-region style variants are not
  expressible either.
- **Sound** and sound text columns.

Raster drawings that could not be rendered are listed too.

## File size

- Each stroke of varying thickness becomes an outline with many points, so a
  drawing made with pressure is much larger than the same drawing made with a
  constant thickness.
- A drawing used several times (a cycle, for example) is written once for
  each run of frames it is shown in.
- Embedded images are base64 data: a third larger than the PNG files.
- The JSON is written compact (no indentation).

## Previewing the file

Any Lottie player shows it: drop the `.json` file on a Lottie web viewer, open
it in the LottieFiles apps or in Glaxnimate, or play it with lottie-web in a
page:

```html
<div id="anim" style="width: 960px; height: 540px"></div>
<script src="lottie.min.js"></script>
<script>
  lottie.loadAnimation({container: document.getElementById("anim"),
                        renderer: "svg", loop: true, autoplay: true,
                        path: "scene.json"});
</script>
```

A browser opens the file only from a web server (not as a local file) when
the images are not embedded, as they are loaded next to it.

## Code

- `toonz/sources/include/toonz/lottiemath.h`: the maths and JSON snippets
  (transform decomposition, the y flip, quadratic to cubic curves, the path
  builder, keyframe reduction, colors), header only. Tested by
  `toonzlib/tests/lottiemath_test.cpp`.
- `toonz/sources/include/toonz/lottieexporter.h`, `toonzlib/lottieexporter.cpp`:
  the export of an xsheet and of a vector drawing, without GUI. Tested on a
  scene built in memory by `toonzlib/tests/lottieexport_test.cpp`.
- `toonz/sources/toonz/lottieexport.cpp`: the command, its options, the PNG
  rendering of the raster drawings (as the Spine export renders them) and the
  files.
