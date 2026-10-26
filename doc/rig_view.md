# Rig view

The Rig view is a display mode of the Stage Schematic (master plan A2, parity
plan F4). It shows, on the nodes, what is attached to the objects, lets the
user publish chosen channels of an object as named ports on its node, and
filters the nodes. The Stage Schematic behaves exactly as before while the
toggle is off (the default; the state is not saved).

Only publishing channels adds data to the scene (see "File format and
compatibility"); everything else reads the scene.

## Use

The bottom toolbar of the Stage Schematic has a **Rig View** toggle (left of
the New Pegbar button). When it is on, a **Filter nodes** field appears next to
it, and the nodes get small badges and port rows under them.

### Badges (column nodes)

- **Deformers**: the Deformer Stack, Envelope and Free-Form FXs found in the
  column's fx chain. The chain follows the output connections of the column's
  fx, and stops at the FXs that combine several inputs and at the xsheet node.
- **Offsets**: the offset layers of the column (doc/offset_layers.md); a
  disabled layer is marked "(off)".
- **Controllers**: the master controllers (pegbars named `ctrl_*`) that drive a
  channel of the column or the weight of one of its offset layers, found as
  `peg<N>.rot` in an expression key of those curves.

The tool tip of a badge has the whole text. The badges can be clicked (left
button) while the Rig view is on:

- **Deformers**: makes the deformer FX the current FX and opens the FX
  Settings (the FX Editor). If the chain has several deformers, a menu asks
  which one.
- **Offsets**: makes the column the current object and opens the Offset Layers
  panel.
- **Controllers**: opens the Master Controllers panel.

### Published channels and ports (any stage object)

A stage object can publish some of its channels, each with a name of the
user's choice, as ports of its node. This is the "group node with published
ports" of the master plan: the node of a rig part (a pegbar, a column, the
camera) lists the few channels the animator needs, in one block.

- **Publish Channel...** (context menu of the node, while the Rig view is on)
  asks the channel and the port name. The channels are the animatable ones of
  the object (X, Y, Z, SO, Rotation, Scale H/V, Scale, Shear H/V, Rotation
  X/Y; the 3D tilts not for the camera), and the weight of each offset layer
  of the object. The name proposed is the one of the channel; a name cannot be
  empty or used by another port of the object.
- **Unpublish Channel...** (the same menu, when the object has ports) removes
  one.
- Each port is a row under the node (below the badges): its name and its value
  at the current frame, in the units of the preferences. The value follows the
  frame changes, and the changes of the xsheet.
  - A click selects the channel in the Function Editor (it is opened if no
    Function Editor is shown), so it can be keyed there.
  - A double click asks the value, and keys the channel at the current frame
    with it (one undo step). The weight of an offset layer is keyed only if it
    is animated, as in the Offset Layers panel.
- The value is the one of the base curve of the channel, without the offset
  layers.
- Publishing and unpublishing are undoable. Renaming an offset layer updates
  the ports of its weight; a port whose layer is deleted shows "-" until the
  layer is back (undo), and does nothing.

### Filter

The filter hides the nodes (and their links) that do not match the text,
ignoring the case; an empty field shows everything. A node matches by its
name, or by the content of its badges and rows: the deformer labels, the
names of its offset layers (not their "(off)" mark), the names of the
controllers, and the names of its published channels. It is the "port
filtering" of master plan C2 in as far as the stage nodes are concerned.

## File format and compatibility

An object with published channels gets a new child tag of the stage object in
the scene file, written only for objects that have at least one:

```xml
<publishedChannels>
  <port>
    <name>smile</name>
    <channel>rot</channel>        <!-- the tag of the base channel: x, y, z,
                                       so, rot, sx, sy, sc, shx, shy, rotx,
                                       roty -->
  </port>
  <port>
    <name>wave</name>
    <layerWeight>wave</layerWeight>   <!-- instead of channel: the weight of
                                           the offset layer of that name -->
  </port>
</publishedChannels>
```

Scenes that do not use published channels are saved byte for byte as before.
**Older builds cannot open scenes that use published channels**: they stop
loading with "unexpected tag", as they do for offset layers. The ports are
carried by cloning, duplicating and copying objects, by the sub-xsheet
commands, and by undo, like the offset layers. The Rig view itself (toggle,
filter) is not saved.

The code is in `toonzlib/tstageobject.cpp` (`TStageObject::PublishedChannel`),
`toonzlib/tstageobjectcmd.cpp` (`publishChannel`, `unpublishChannel`) and
`toonzqt/stageschematicscene.cpp`.

## Partial

- Deformers are still not nodes of the graph, and the pose and deformer chains
  are shown as badges only: a deformer cannot be wired or published, it is
  edited in the FX Settings. A published port stands for a stage object
  channel (or a layer weight); the parameters of the FXs cannot be published.
- A port cannot be renamed (unpublish and publish it again), and the ports
  have no links: they are rows for reading and keying, not sockets to wire.
- Badges are shown on column nodes only. The ports are shown on every kind of
  object node.
- The values of the ports are refreshed when the frame or the xsheet changes;
  a change of the curve that does not notify the xsheet (for example while
  dragging in the Function Editor) shows after the next refresh.
- The badges and rows are drawn under the node and can overlap the nodes
  below it.
- A binding made with an expression that is not in a key of the curve, or with
  an expression that builds the reference otherwise (`peg3 .rot`, a different
  channel), is not found.
- The Rig view filter does not apply to the FX Schematic; it has its own port
  filter (see below).

## Port filter in the FX Schematic

The toolbar of the FX Schematic has a **Filter ports** field, next to the
Stage Schematic's Rig view filter. The text is matched without regard to case:

- a node stays visible when its name or the name of one of its input ports
  contains the text;
- a node that stays only because of its ports shows only the matching ports
  (the other port rows are hidden); a node that matches by its name keeps all
  its ports;
- the nodes that match neither are hidden, and so is every link that ends on
  a hidden node or a hidden port.

An empty field leaves the schematic exactly as before. The filter is in
memory only (not saved with the scene or the preferences). It is applied again
whenever the schematic is rebuilt, and clearing the text shows everything
back. The code is `FxSchematicScene::applyPortFilter` (`toonzqt/
fxschematicscene.cpp`), modeled on `StageSchematicScene::applyRigFilter`.

Limits: the rows of the hidden ports are not packed (a gap stays in the
node), the ports without a name (the Output node's) are never filtered, and
the output ports, the group editors and the macro editors are not filtered.

## Lock Timeline Ruler

Nothing was added: the Timeline already behaves like this. In the Timeline
orientation the frame ruler (`RowArea`) lives in its own scroll area
(`XsheetViewer::m_rowScrollArea`, positioned over the header-layer range and
the body-frame range in `XsheetViewer::positionSections`). Its vertical
scroll bar is not connected to the cells (`connectOrDisconnectScrollBars`
links the row area to the cells along the frame axis only, and the layer axis
goes to the column area), so scrolling the layers never moves the ruler; the
ruler follows the cells horizontally, which it must, to keep the frame
numbers aligned. The same holds in the Xsheet orientation, where the column
headers stay put when the rows scroll vertically. A separate "lock" toggle
would therefore have no behaviour to switch.
