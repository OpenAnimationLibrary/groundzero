# Fluid Width (speed sensitive Brush)

The vector Brush has a **Fluid Width** slider (0 to 100, default 0). Above 0
the line gets thinner where the pen moves fast and stays full width where it
moves slowly, like the speed sensitive brushes in other animation programs.
It works with a mouse as well as with a tablet.

At 0 the Brush behaves exactly as before.

## Where it is

- The Brush tool options bar, next to **Pressure** (vector levels only).
- The Tool Properties panel, with the other Brush options.

The other brushes (Toonz Raster, Raster, MyPaint) do not have it.

## What it does

- **Size is the maximum.** A slow stroke has the width you set with Size (and
  Pressure, if it is on). A fast stroke is made thinner than that, never
  wider.
- **Strength.** At 100 a very fast stroke is about a fifth of the normal
  width; at 50 about 60 percent. The width never drops below a tenth of the
  normal width, so a quick flick still leaves a visible line.
- **Pressure.** The speed effect is multiplied with the pressure width. With
  Pressure off (for instance with a mouse) the line is the fixed Size width
  made thinner by speed.
- **Starts and ends.** A stroke starts at full width and eases thinner as the
  pen picks up speed. If the pen slows down at the end of a stroke the line
  swells again; a quick flick off the paper ends thin.
- **Smoothed.** The speed is averaged over a few hundredths of a second, so a
  shaky mouse or uneven input events do not make the width jitter.
- **Zoom.** Speed is measured in screen pixels, so the same hand movement
  gives the same look whatever the zoom level.

## Works with the other Brush options

- **Smooth** and **Assistants** (rulers, perspective, mirror and the like) are
  applied first; the width follows the speed along the final line.
- **Shift** straight lines are drawn at one width, because they only have two
  end points.
- Motion paths and Colour Art strokes have no thickness, so they ignore it.

## Presets and saved settings

- The slider value is part of a Brush preset, like Pressure and Smooth. A
  preset only stores it when it is above 0, so existing presets are not
  changed, and presets saved before this option existed load with it off.
- The last value is kept in the user settings (`VectorBrushFluidWidth`), not
  in scenes. The strokes you draw are ordinary strokes with a thickness per
  control point, so scenes and levels open in older versions unchanged.

## Notes for developers

- The maths is in `toonz/sources/include/tools/fluidwidth.h` and has no GUI
  dependency; its test is `toonz/sources/tnztools/tests/fluidwidth_test.cpp`.
  The speed reference, limits and smoothing time are constants at the top of
  the header.
- The factor is applied in `ToonzVectorBrushTool::inputPaintTracks`, from the
  time and length that every point of the final input track already carries,
  so it sees the result of the stabiliser and the assistants. The speed history
  is kept point by point so the tool can take preview points back.
