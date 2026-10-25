# Offset layers

An offset layer keeps animation that is added to the animation of a column,
pegbar, camera or table apart from its base keys. It is the runtime of the
Harmony Attribute Controller (master plan A3, parity plan F5): poses and
controllers can be stacked, scaled and removed without baking keys.

Nothing changes for a scene that does not use them: an object without layers
reads, places, renders and saves exactly as before.

## Model

A layer has a name, an animatable **weight** (default 1), an enable flag and an
additive curve for each of the channels X, Y, Z, SO, Rotation, Scale H, Scale V,
Scale, Shear H, Shear V, Rotation X and Rotation Y. The Path channel has no
curve. The curves are ordinary animatable parameters, zero by default.

```
effective = base + sum over the enabled layers of (weight * layer curve)
```

The scale channels are no exception: the offsets are added to the scale
factors (a layer of 0.5 over a base of 1 gives 1.5), and the effective Scale,
Scale H and Scale V are then multiplied as usual. The shear and the length
channels are added in their stored units (inches, the shear tangent), so a
layer is independent of the unit preferences.

The effective value is what the following read: the placement of the object
(viewers, renders, schematic), `getZ`, `getSO`, `TStageObject::getParam(channel,
frame)` and the expressions referring to the object's channels (`peg1.x`,
`col2.rot`, ...). The base curves, the keyframes of the object (the key icons of
the xsheet) and the tools that edit them do not see the layers: the Animate
tool and its fields edit the base keys, as deltas from the values they read
from them, while its handles are drawn at the effective placement.

The code is in `toonzlib/tstageobject.cpp` (`TStageObject::OffsetLayer`) and
`toonzlib/tstageobjectcmd.cpp` (the commands, all with undo).

## Panel and Function Editor

**Windows > Offset Layers** lists the layers of the current object: an enable
check box, the name (editable), the weight slider (-200% to 200%) and the
buttons **Bake into Base** and **Delete**. **New Layer** adds an empty layer.
The weight is the value of its curve when it has no keys, and a key at the
current frame when it has some.

The curves of a layer are listed in the Function Editor, under the object, in
the group **Offset Layers** as `<layer>/<channel>` (and `<layer>/Weight`), and
are keyed with the usual tools. They can be driven by expressions. The group
is not listed for objects without layers.

**Bake into Base** adds the weighted layer to the base curves at the keys of
the base, of the layer and of its weight, and clears the curves of the layer.
The result is exact at those keys only, as the sum of two interpolated curves
is not the interpolation of the sum.

**Train from Keys** adds a layer called "Trained", with weight 0, whose curves
are the animated base curves minus their value at the frame 0, so the existing
animation can be repeated scaled by raising the weight. Expression and file
keys become linear keys with their values at the keys.

In the **Poses** panel, **Apply as offset layer** applies a pose to the layer
with the given name (added to the objects that do not have it) instead of
the base keys: the layer curves get a key at the current frame with (current
layer value + weight * pose offset). Only the stage object channels are applied
to layers; Plastic vertex channels and Path are skipped.

## File format and compatibility

Objects with layers get a new child tag of the stage object in the scene file:

```xml
<offsetLayers>
  <layer>
    <name>wave</name>
    <enabled>0</enabled>        <!-- only if disabled -->
    <weight>...</weight>        <!-- a doubleParam -->
    <x>...</x> <rot>...</rot>   <!-- same tags as the base channels, only
                                     the ones that are not zero -->
  </layer>
</offsetLayers>
```

The tag is written only for objects with at least one layer. Builds without
this feature stop loading a scene with the tag ("unexpected tag"); scenes
without layers are the same as before and load anywhere. Layers are carried by
cloning, duplicating and copying objects, by the sub-xsheet commands, and by
undo.

## Limitations

- Expressions in a layer curve are saved as text and are not rewritten when
  the object is copied to another scene, as for the base ones.
- The layers do not take part in the keyframes of the object (xsheet key
  icons, moving or copying object keys, cycle range), but follow the frame
  insertion and removal of the xsheet. Cycling uses the range of the base keys.
- The effective scale can reach 0 or be negative; as for the base channels,
  nothing prevents a degenerate placement.
- Edits of the base through the Animate tool fields and viewer gizmos are
  relative: they change the base keys, not the effective value shown by the
  handles when layers are active.
- Layers are for stage object channels only: not for Plastic skeleton vertex
  parameters, the Path channel, the center or the sound volume.
- The scripting API (`getValue`/`setValue` of a stage object channel) reads and
  writes the base curve.
- Setup Mode (recording the channels owned by a controller) and fitting of
  weights to keys are not included.
