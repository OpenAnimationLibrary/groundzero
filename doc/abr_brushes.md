# Photoshop brushes (ABR)

The full-colour Brush tool (the Brush tool on a raster level) can paint with
the sampled brush tips of Photoshop `.abr` files. This is master plan item B3
(parity plan D4).

## Using it

1. File > Import > Import Photoshop Brushes (ABR)... and choose one or more
   `.abr` files.
2. Each tip is written as a grayscale PNG into
   `<library folder>/brushtips/<name of the .abr file>/`. Tips are numbered in
   the order of the file (`001 name.png`; `tip_001.png` when the file has no
   names).
3. Choose the tip in the **Tip:** option of the Brush tool on a raster level
   (select the tool again if it is already active when you import). `Round`
   is the normal round brush and is the default.

Any PNG put by hand in `<library folder>/brushtips` (or a folder in it) is
listed too. The PNG of a tip is black where it paints and white where it does
not. A PNG with transparency is read by its alpha channel instead. The longer
side of the image spans the size of the brush.

## How it works

`AbrParser::parseAbr` (`include/toonz/abrparser.h`, `toonzlib/abrparser.cpp`)
turns the bytes of a file into tips (name, size, 8-bit coverage, 255 = full
paint). It has no dependency on Qt or Toonz and is tested by
`toonzlib/tests/abrparser_test.cpp` (CMake option `BUILD_ABR_TESTS`), which
builds ABR files in code.

The tip is stamped by the dabs of the MyPaint brush engine that the full
colour tool uses (`MyPaintTip` and `Raster32PMyPaintSurface::drawTipDab` in
`tnztools/mypainttoonzbrush.cpp`). libmypaint has no custom tip, so the
surface replaces the round dab by the tip: it is centred on the dab, scaled to
its diameter (so the Size, pressure and MyPaint "radius" dynamics work as
usual), squashed and turned by the dab's aspect ratio and angle, and blended
with the dab's color, opacity, eraser and lock alpha. Small dabs sample a
chain of half size copies of the tip. The classic brush uses a spacing of 20%
of the size when a tip is chosen. The choice of tip is kept in the user
settings (`FullcolorBrushTip`) and is not part of the brush presets or the
scene.

## Supported ABR files

- Versions 1 and 2: sampled brushes (type 2), 8 or 16 bit, raw or PackBits.
- Versions 6 to 10 (Photoshop CS2 and later): the `8BIM` `samp` section, sub
  versions 1 and 2, 8 or 16 bit, raw or PackBits.

## Not supported

- Computed brushes of versions 1 and 2 (round brushes described by
  parameters). They have no image; they are counted in the import message.
- The `desc` section of versions 6 and up, which holds the names, the spacing
  and the dynamics of the presets (shape dynamics, scattering, textures, dual
  brush, color dynamics, transfer). These are proprietary: a tip is imported
  as a plain stamp, with the spacing of the brush and none of its dynamics.
  Tips of versions 6 and up are named by their number.
- Pattern (`patt`) sections and anything else but sampled tips.
- Versions 3 to 5.
- Tips in the MyPaint-style brush on Toonz Raster levels and in the vector
  brush; the colorize blend mode of MyPaint dabs; the Hardness setting while a
  tip is chosen (the tip has its own edge).

The layout of the file was written from the GIMP and Krita readers; it was
not checked against real Photoshop files in this change (nothing could be
run), only against files built by the test.
