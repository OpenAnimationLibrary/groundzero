# Script API depth

Master plan item D4. The scripting API (JavaScript in the Script Console and
"Run Script", Python in the Python Console when the build has `WITH_PYTHON`)
gets more on images, levels, the xsheet, rendering and the scene that is open
in the application. This page describes only what D4 added. The earlier API
(`Image`, `Level`, `Scene`, `StageObject`, `Renderer`, `ImageBuilder`,
`FilePath`, the vectorizers) works as before.

Nothing here changes how scenes are saved or rendered, and no new data is
written to scenes. Scripts that do not use the new calls behave as before.

## Conventions

- Rows, frames and columns of the scene are numbered from 0. The frame ids of
  levels are the ones of the level (`1`, `"2a"`), as before.
- A script `Scene` is not the scene open in the application, and its changes
  are not undoable. To work on the open scene, save it and open the file in a
  script (see "The scene open in the application").
- Colors are strings of QColor: `"red"`, `"#ff8800"`, and `"#80ff8800"` for a
  color with alpha (the order is `#AARRGGBB`). Colors are returned as
  `"#AARRGGBB"`, always with the alpha.
- Pixel coordinates have (0, 0) at the lower left corner of the image, as the
  rasters of OpenToonz.
- In JavaScript, a call with bad arguments throws an `Error`; in Python it
  raises `ValueError`, `TypeError`, `IndexError` or `RuntimeError`
  (`OSError` for files).

## Image

Pixels can be read and written in Raster (full color) images. ToonzRaster and
Vector images raise an error: use `Level` or the vectorizers for them.

| JavaScript | Python | |
|---|---|---|
| `img.getPixel(x, y)` | `img.getPixel(x, y)` | The color of the pixel, as `"#AARRGGBB"`. |
| `img.setPixel(x, y, color)` | same | Sets one pixel; returns the image. |
| `img.fill(color)` | same | Fills the whole image; returns the image. |
| `img.clone()` | same | A copy, with its own pixels. |
| `img.dpi` (read and write) | same | The dpi of Raster and ToonzRaster images; must be > 0 when set. |
| | `Image.create(w, h, type="Raster")` | A new transparent image (`"Raster"` or `"ToonzRaster"`). JavaScript has `ImageBuilder` for this. |
| | `Image()`, `Image(path)`, `img.load(path)`, `img.save(path)`, `img.type`, `img.width`, `img.height` | The Python `Image` class has the same meaning as the JavaScript one. |

The pixel values are the ones of the raster: `getPixel` and `setPixel` do not
convert them. The colors of a rendered image can be premultiplied by its
alpha, as the renderer produces them.

```js
var img = new ImageBuilder(64, 64, 'Raster').fill('white').image;
img.setPixel(10, 20, 'red');
print(img.getPixel(10, 20));     // #ffff0000
var copy = img.clone();
copy.fill('#80000000');          // 50% black
copy.dpi = 120;
```

```python
import opentoonz as ot
img = ot.Image.create(64, 64)
img.fill('white').setPixel(10, 20, 'red')
print(img.getPixel(10, 20))      # #ffff0000
```

## Level

A level also has these, new in D4 (JavaScript and Python):

| Call | |
|---|---|
| `level.hasFrame(fid)` | True if the level has the frame. |
| `level.removeFrame(fid)` | Removes the frame if the level has it (nothing happens if not); returns the level. The cells of the scenes that use the frame are not changed. |
| `level.appendFrame(image)` | Adds the image after the last frame (frame number = last + 1, or 1 for an empty level); returns the new frame id as a string, as `getFrameIds()` gives them. |

These existed in JavaScript and are now also in Python: `getFrame(fid)`,
`getFrameByIndex(index)` and `setFrame(fid, image)`. `getFrame` returns the
image of the level itself, not a copy: after changing its pixels, call
`setFrame(fid, image)` so that the level is marked as modified (and saved by
`level.save()`).

An image can be added only to a level of the same type (Raster to Raster,
ToonzRaster to ToonzRaster, Vector to Vector). In JavaScript an empty
`new Level()` takes the type of the first image. In Python the level has to
come from a scene (`scene.newLevel(...)`): `Level().setFrame(...)` raises an
error.

```js
var scene = new Scene();
var level = scene.newLevel('Raster', 'BG');
var fid = level.appendFrame(new ImageBuilder(32, 32, 'Raster').fill('blue').image);
level.getFrame(fid).setPixel(0, 0, 'red');
level.setFrame(fid, level.getFrame(fid));     // marks the level as modified
level.removeFrame(fid);
```

## Scene

| Call | |
|---|---|
| `scene.getColumns()` | An array (list in Python) of objects (dicts) with `index`, `id` (`Col1`...), `name`, `type` (`Level`, `Sound`, `SoundText`, `Zerary`, `Palette`, `Mesh` or `Empty`), `firstRow` and `lastRow` (the first and last cell; -1 and -1 if the column has no cells). |
| `scene.renderFrame(frame)` | Renders the row `frame` with the output settings of the scene (camera, resolution, shrink...) and returns an `Image` (Raster, with the dpi of the camera). The frame must be in 0 to `frameCount - 1`. |

`renderFrame` renders the scene as the `Renderer` class does. To choose columns or several frames in JavaScript, keep using
`new Renderer()`: its `columns` and `frames` arrays and its `renderFrame()` and
`renderScene()` methods work as before.

D4 fixed the render completion of `Renderer`: its port did not override
`onRenderFinished(bool)`, so a render could wait forever.

```js
var scene = new Scene('shots/sh010.tnz');
scene.getColumns().forEach(function (c) { print(c.id, c.type, c.firstRow, c.lastRow); });
var frame = scene.renderFrame(0);
frame.save('/tmp/sh010_0001.png');
```

```python
scene = ot.Scene('shots/sh010.tnz')
for c in scene.getColumns():
    print(c['id'], c['type'], c['firstRow'], c['lastRow'])
scene.renderFrame(0).save('/tmp/sh010_0001.png')
```

## Scene check and palettes

Added with the scene checker (plan item T6, see `doc/scene_checker.md`), in
JavaScript and Python:

| JavaScript | Python | |
|---|---|---|
| `scene.check()`, `scene.check({unusedStyles: true})` | `scene.check()`, `scene.check(unusedStyles=True)` | The issues of the scene, errors first: an array (list) of objects (dicts) with `severity` (`error`, `warning` or `info`), `kind` (`missingDrawing`, `missingFile`, `unsavedLevel`, `emptyColumn`, `unusedLevel`, `unusedStyle`, `duplicateStyle`, `levelDpi`, `frameRate` or `resolution`), `title` (the group of the Scene Check panel, such as `Missing Drawings`), `text`, `column`, `row` and `rowCount` (from 0; -1, -1 and 0 when the issue is not about cells), `level` and `subXsheet` (names, `""` for none) and `styleId` (-1 for none). `unusedStyles` also looks for unused styles, which loads every drawing. |
| `level.palette` | same | The level's palette, read only: an object (dict) with `name` and `styles`, the styles of its pages in page order, each with `id`, `name`, `color` (`"#AARRGGBB"`, the main color), `page` (from 0) and `used` (a drawing of the level uses it; this loads the drawings). `null` (`None`) for a level without a palette. It is a copy: changing it changes nothing. |

```js
var scene = new Scene('shots/sh010.tnz');
scene.check().forEach(function (i) {
  if (i.severity == 'error') print(i.title + ': ' + i.text);
});
var pal = scene.getLevel('A').palette;
if (pal) pal.styles.filter(function (s) { return !s.used; })
               .forEach(function (s) { print('unused', s.id, s.name, s.color); });
```

```python
scene = ot.Scene('shots/sh010.tnz')
errors = [i for i in scene.check() if i['severity'] == 'error']
for s in scene.getLevel('A').palette['styles']:
    print(s['id'], s['name'], s['color'], s['used'])
```

## The scene open in the application

Three functions, in the Script Console (JavaScript, global functions) and in
the Python Console (functions of the `opentoonz` module). They use the
application, so they work only in the application, not in the `-script`
command line and not in the tests.

| Call | |
|---|---|
| `currentSceneInfo()` | An object (dict) with `name`, `path` (absolute), `untitled`, `dirty`, `frameCount`, `columnCount`, `currentFrame` and `currentColumn`. Empty if there is no scene. It was already in Python; D4 added it to JavaScript. |
| `setCurrentFrame(frame)` | Makes the frame (row, from 0) current in the application. |
| `setCurrentColumn(column)` | Makes the column (from 0) current. It must exist in the current xsheet. |

The scene is read only. To change it from a script, save it in the
application, open the file in a script, change it, save it, and reload it:

```js
var info = currentSceneInfo();
if (!info.untitled && !info.dirty) {
  var scene = new Scene(info.path);
  print(scene.frameCount, 'frames');
  setCurrentFrame(scene.frameCount - 1);
}
```

## Compatibility

- New API only: no existing call changes behavior. The one fix is the render
  completion of `Renderer` mentioned above.
- `Image.dpi` was read only in JavaScript and is now writable.
- Python: all of it needs `WITH_PYTHON`, as before. No dependency was added to
  the default build.
- Scenes and levels saved by scripts are the ones of the existing API.

## Limitations

- Pixels are for Raster images only (32 and 64 bit rasters). There is no
  pixel API for ToonzRaster ink/paint/tone or for vector strokes.
- `getFrame` shares the image with the level (see above); there is no
  `getFrameCopy`, use `getFrame(fid).clone()`.
- `Scene.renderFrame` renders one frame at a time with the scene's settings.
  It blocks the script, and in Python the application, while it renders:
  the application repaints, but keyboard and mouse input (in the Python
  Console too) waits until the render ends.
- The scene open in the application cannot be changed by scripts, only
  inspected, and its current frame and column set. Python scripts block the
  application while they run.
- Python `Image` has no `ImageBuilder`: use `Image.create`, `fill`, `setPixel`
  and `Level` frames.
- Fx, node, Plastic and toolbar-script bindings are not part of D4.
- The tests that need rendering or the UI are not in `scriptengine_test`:
  they cover pixels, frames and columns, and the out-of-range errors.
