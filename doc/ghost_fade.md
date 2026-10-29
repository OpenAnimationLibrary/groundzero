# Ghost Fade for the onion skin

How strong the onion skin ghosts look is set today by the **Paper Thickness**
preference and, one marker at a time, by the opacity popup of a marker. The
**Ghost Fade** adds three viewer settings for all the ghosts at once: an
opacity multiplier, the tint, and a step for long holds.

They are the user's settings, kept with the other viewer settings, not in the
scene. With the defaults (100%, tinted, every frame) the ghosts look exactly as
before.

## Where

Right-click the viewer (or the frame column of the Xsheet) with the onion skin
active: **Ghost Fade** is a submenu under the onion skin commands.

| Setting | What it does | Default |
|---|---|---|
| **Opacity** | Multiplies the opacity of every ghost, from 0% (hidden) to 200%. | 100% |
| **Tint Ghosts** | Colours the ghosts with the front and back onion skin colours of the preferences. Off, the ghosts keep their own colours and are only faded. | On |
| **Ghost Every** | Draws the relative ghosts only every 2, 3, 4, 6, 8 or 12 frames from the current frame. | Every Frame |
| **Reset Ghost Fade** | Back to the defaults. | |

A change is shown at once in every viewer.

## How the opacity is worked out

For each ghost the onion skin first gives an opacity as before: the opacity of
its marker when one is set, otherwise the automatic fade of the paper
thickness (`1 - fade`, the fade growing with the distance from the current
frame). The Ghost Fade then multiplies it:

```
opacity = clamp(marker or automatic opacity × Opacity, 0, 1)
```

At 100% the value is the same, bit for bit, as without the Ghost Fade. Above
100% the far ghosts get stronger until they are opaque. The current frame is
not a ghost and is not changed.

## The step

With **Ghost Every 4 Frames**, relative markers at -12 to +12 frames show the
ghosts at -12, -8, -4, +4, +8 and +12 only: with an animation on fours, or a
long hold, the ghosts that would repeat the same drawing are skipped. Fixed
markers are always drawn, since they were placed one by one. The skipped
ghosts do not count in the fade, so the nearest ghost drawn fades as the
nearest one.

## What it does not change

- **Shift and Trace** ghosts keep their own look (and their own preference for
  the onion skin colours).
- The markers in the Xsheet and their opacity popup still show the marker and
  automatic values, before the Ghost Fade.
- The Fill tool and the other tools that work on the onion-skinned frames
  still see every marker: the step only changes what is drawn.
- Renders and previews have no onion skin, so they are not affected.

## How it works

- `toonz/ghostfade.h` holds the pure math (the settings, the opacity and the
  step), tested by `ghostfade_test`.
- `OnionSkinMask::getGhostFade()` / `setGhostFade()` keep the settings, read
  once from the user's settings file (`OnionSkinGhostOpacity`,
  `OnionSkinGhostTint`, `OnionSkinGhostStep`) and written back when changed.
- The stage builder (`toonzlib/stage.cpp`) skips the relative ghosts off the
  step; the stage visitor (`toonzlib/stagevisitor.cpp`) multiplies the opacity
  in `getOnionSkinOpacity` and leaves out the onion colours, for vector,
  raster, Toonz raster and mesh drawings, on the CPU and GPU paths.
- The submenu is built in `toonz/onionskinmaskgui.cpp`.
