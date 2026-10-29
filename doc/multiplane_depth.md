# Multiplane Depth and the camera focus

Columns placed at different Z are already drawn smaller or larger by the camera,
like the planes of a multiplane camera. **Multiplane Depth** adds the rest of
the look: columns away from the plane in focus are blurred, and the ones behind
it fade into a haze and lose their saturation, as distant hills do.

It is a scene setting, off by default. A scene that does not use it renders
exactly as before.

## The camera's Focus Z and Aperture

Every camera has two more channels, shown in the Function Editor under the
camera only:

| Channel | Meaning | Default |
|---|---|---|
| **Focus Z** | The Z of the plane in focus, in the units of the Z channel. | 0, the table |
| **Aperture** | The depth blur in stops: each stop more doubles the blur, each stop less halves it. | 0, the blur as set in the scene |

They are animated like any channel. A key on Focus Z at one frame and another at
a later frame gives a **rack focus**: the sharp plane moves from one column to
another. In expressions they read as `camera1.focus` and `camera1.aperture`,
and scripts reach them as the channels `focus` and `aperture`.

Like the 3D tilt channels, they take part in a camera's keys only once they are
used (keyed or set away from the default), and they are saved in the scene only
then. Older builds ignore them.

## The scene setting

**Xsheet > Scene Settings... > Edit Multiplane Depth** opens the settings:

| Setting | What it does |
|---|---|
| Blur and Haze the Columns by Their Distance | Turns the depth on. |
| Blur per Unit | The blur for each unit of Z between a column and the Focus Z. |
| Maximum Blur | The most a column is blurred, before the aperture. |
| Haze Density | How fast the columns behind the focus fade into the haze colour. |
| Haze Color | The colour of the haze. |
| Desaturation per Unit | How fast the columns behind the focus lose their saturation. |

For a column at Z, with the camera's Focus Z and Aperture at the frame:

```
blur         = min(Maximum Blur, Blur per Unit × |Z − Focus Z|) × 2^Aperture
behind       = max(0, Focus Z − Z)
haze         = 1 − exp(−Haze Density × behind)
desaturation = min(1, Desaturation per Unit × behind)
```

The blur is in the units of the Blur FX's Value, as seen by the camera: the
render divides it by the column's scale on screen, so a column pushed back (and
so drawn smaller) is blurred as much as the formula says. Desaturation moves the
colours toward their grey, then the haze mixes them toward the haze colour; the
transparency of the column is unchanged.

The settings are saved in the scene only when they differ from the defaults, and
changes can be undone.

## Excluding a column

Right-click a column header and choose **Toggle Exclude from Depth** to keep a
column sharp and clear whatever its Z (a title, a character that must stay
crisp, a column used as a matte). It works on the selected columns, or the
current one, and can be undone. The flag is a bit of the column's status,
saved only when set.

## What is affected

- Level columns and FX columns (such as a Color Card) of the top xsheet.
- A sub-xsheet column is one plane, at the Z of its column: the columns inside
  it are not blurred one by one.
- The textures of particles are not: the particles place them themselves.
- Rendering a sub-xsheet on its own (as the current xsheet) does not apply the
  depth.
- The viewer does not show the depth; the preview and the render do.

## How it works

`FxBuilder` (`toonzlib/scenefx.cpp`) inserts, over a placed column, the standard
Blur FX with the value above and a hidden `ColumnDepthFx`
(`common/tfx/unaryFx.cpp`) for the haze and desaturation, which supports 8-bit,
16-bit and float renders. The math is in `toonz/multiplanedepth.h`, tested by
`toonzlib/tests/multiplanedepth_test.cpp`; the camera channels are tested by
`toonzlib/tests/cameradepth_test.cpp`.
