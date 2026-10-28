# Motion presets

A motion preset is a saved animation (a bounce, a fly-in, a shake, a spin...)
that you apply to the selected columns or pegbars in one click. It starts at the
current frame and works from where the object is at that frame, so the same
preset suits any object, in any place. It is the equivalent of Adobe Animate's
Motion Presets.

Nothing is added to the scene file for the feature itself: applying a preset
only sets keyframes (or fills a new offset layer), which scenes already save.

## Using the panel

Open **Windows > Motion Presets**. The list shows the stock presets and, below
them, your own. Each one has a small plot of its curves; click one to see its
description, its length in frames and the channels it animates.

1. Select the columns (or the cells of the columns, or the pegbars in the stage
   schematic) to animate. With no selection the current object is used, the one
   the Animate tool edits.
2. Go to the frame where the animation should start.
3. Double-click the preset, or select it and click **Apply**.

One apply is one undo step. Locked columns are left alone and named in a
message. A channel that cannot take the keys is skipped and named too (see
Limits); the other channels of the preset are applied.

**Apply as offset layer** puts the keys in a new offset layer named after the
preset (one layer per object and per apply) instead of the base keys of the
object. The base keys stay as they were and the layer can be switched off,
weighted or baked later (see `offset_layers.md`).

### Saving your own preset

Select the keyframes of an object in the xsheet and choose **Save Selection as
Preset...** in the panel, or **Xsheet > Save Keys as Motion Preset...** (the
command can be given a shortcut). It stores the keys of every animated channel of
the current object between the first and the last selected frame, asks for a
name and writes the preset into the `user` folder (see Files). If one key is
selected, the preset runs from it to the last key of the object. If no
keyframe is selected, the whole animation of the object is saved.

Channels with fewer than two keys in the range, or whose keys never change the
value, are not stored. Channels with an expression, a file or a similar shape in
the range are not stored either, and the message says so.

**Delete** removes the selected preset of the user. The stock presets are not
deleted from the panel (remove their files if you want to).

## What the stock presets do

| Preset | Channels | Frames | Notes |
|---|---|---|---|
| Bounce | y | 24 | three hops in place, each lower than the last |
| Drop | y | 22 | falls from 10 inches above the object and bounces twice (ends at the object) |
| Fly In From Left / Right | x | 12 | slides in and settles (ends at the object) |
| Fly Out To Left / Right | x | 12 | speeds up and leaves |
| Shake | x, y | 18 | damped jitter |
| Spin 360 | rotation | 24 | one full turn, eased |
| Pulse | scale | 18 | a heartbeat: +25%, then +12% |
| Swing | rotation | 24 | a pendulum that slows down |
| Wobble | rotation, scale H and V | 16 | jelly: wobble with squash and stretch |

There is no fade preset: a stage object has no opacity channel, so presets
only animate the transform channels.

## How a preset is applied

A preset stores, for each channel, a list of keys relative to the first key of
the channel. Applying it at the frame F:

| Channel | Rule |
|---|---|
| X, Y (inches), Z, SO | the key gets `S + d`, `S` being the value of the curve at the first key and `d` the stored difference |
| Rotation, tilt X and Y (degrees) | the same: angles add. A stored 360 makes the object turn once more, whatever its angle was |
| Shear H and V | the same |
| Scale, Scale H, Scale V | the key gets `S * r`, `r` being the stored ratio. A pulse of 1.25 makes any object 25% bigger than it was |

Where the first key of the channel is at the frame `F + offset` (nearly always
`F`), `S` is read there before anything is changed. So the object does not
jump at the first key, and applying the same preset twice at the same frame
gives the same result (not so for the presets that end at the current position,
see below: they read the position again, so undo before applying one again).

* The keys that the curve already has between the first and the last key of the
  preset are replaced. The other keys are kept. The segment after the last key
  of the preset is linear.
* Interpolation types and speed handles are copied as stored. In a scale
  channel the value part of a handle is a ratio too and is multiplied like the
  keys.
* **Ends at the current position** (the preset's anchor is *end*, as in the fly-in
  presets and Drop): the keys are shifted so that the *last* key gets `S`
  instead of the first one. The object arrives where it is. Before the first
  key the object holds the value of the first key, so after "Fly In From Left"
  at frame 10 the object waits off screen until frame 10, then comes in. For a
  scale channel the keys are divided by the last ratio.
* With **Apply as offset layer**, the layer curves of the position, angle and
  shear channels get the same differences from their value at the first key
  (0 in a new layer). The layer curves of the scales are added to the base scale
  (see `offset_layers.md`), so they get `R * (r - 1)` where `R` is the base
  scale at the first key. That keeps the result equal to what the base keys would
  give, as long as the base scale does not change afterwards.
* The frame numbers of a preset are offsets, so it can be applied at any frame,
  also in a sub-xsheet.

## Limits

* A curve is skipped, and named in the message, when it is **cycled**, or when it
  has an **expression**, a **file** or a **similar shape** segment where the
  keys would go. Nothing is changed in that case.
* The path position channel is not part of presets.
* Presets store the stage object channels only: no fx parameters, no plastic
  skeleton, no drawing numbers.
* A scale that starts from 0 is stored as a difference, not as a ratio, because a
  ratio to 0 means nothing.
* The values are stored in the internal units (inches for X and Y), so a preset
  keeps its size when the unit preferences change. The 18 inch flights of the
  stock presets are meant for the default camera, 16 inches wide.
* The step ("on twos") of the keys of the preset is not stored; new keys have step 1.

## Files

The library is the `motionpresets` folder of the Library folder of the stuff
directory: the stock presets are the `*.tnzmotion` files in it
(`stuff/library/motionpresets` in the source tree, shipped with the rest of
`stuff`), and the ones you save go to its `user` subfolder, which is created when
the first one is saved. The panel watches both folders and also has a Refresh
button.

A `.tnzmotion` file is text in the syntax of the other Toonz files:

```
<motionpreset>
  <name>
    "Fly In From Left"
  </name>
  <description>
    "Slides in from the left and settles at the current position."
  </description>
  <anchor>
    end
  </anchor>
  <channel name="x" mode="add">
    0 0 S 0 0 4 14 0
    12 18 L -4 0 0 0 0
  </channel>
</motionpreset>
```

* `name` is shown in the panel. `description` is optional. `anchor` is optional:
  `start` (the default) or `end`.
* A `channel` has a `name`: `x`, `y`, `z`, `so`, `rot`, `rotx`, `roty`, `sx`, `sy`,
  `sc`, `shx` or `shy` (the names of the scene files), and a `mode`: `add` or
  `multiply` (optional; `multiply` for `sx`, `sy` and `sc`, `add` for the
  others).
* Each key is a line of 8 numbers and words: `frame value type speedInX
  speedInY speedOutX speedOutY linkedHandles`.
  * `frame` is the offset from the start of the preset. The earliest key of
    the preset is at 0, frames increase along a channel.
  * `value` is the difference from the first key of the channel (`add`, so 0 for
    the first key) or the ratio to it (`multiply`, so 1 for the first key).
  * `type` is the interpolation of the segment that starts at the key, with the
    codes of the scene files: `C` constant, `L` linear, `S` speed in/out, `E` ease
    in/out, `Ep` ease in/out in percent, `Exp` exponential, `n` none.
  * The speeds are the handles of the key as in the Function Editor, as offsets
    from the key: in is (frame, value) of the incoming handle, out of the
    outgoing one. The `S` type uses both ends, `E` and `Ep` use the x of
    each. They can be 0 for `C`, `L` and `Exp`. In a `multiply` channel the value
    parts are ratios.
  * `linkedHandles` is 0 or 1.
* Unknown tags are ignored, so a later version can add to the format.

A file that cannot be read, or whose channels are not valid (unknown channel
names, frames that do not increase, fewer than two keys in a channel, a null
scale ratio), is not listed in the panel.

## Code and tests

* `toonzlib/motionpreset.cpp`, `include/toonz/motionpreset.h`: the model,
  loading and saving, `captureFromKeys` and `apply`. It has no GUI code. The undo
  of an apply is one undo entry per curve that gets keys, restoring the
  curve's keys; the panel puts them in one block.
* `toonz/motionpresetspane.cpp`: the panel and the two commands
  (`MI_OpenMotionPresets`, `MI_SaveMotionPreset`).
* `toonzlib/tests/motionpreset_test.cpp` (ctest name `motionpreset`) checks the
  save and load round trip, the hand-written syntax, the keys that applying
  sets for each rule above (including offset layers and the skipped curves), undo
  and redo, that capturing the keys of an object and applying them to another
  gives the same deltas, and that every stock preset of
  `stuff/library/motionpresets` loads, is valid, animates something and starts
  from a null difference.
