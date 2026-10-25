# Rig view

The Rig view is a display mode of the Stage Schematic (master plan A2, parity
plan F4). It shows, on each column node, what is attached to the column, and
filters the nodes by name. It only reads the scene: nothing is added to the
scene format, and the Stage Schematic behaves exactly as before while the
toggle is off (the default; the state is not saved).

## Use

The bottom toolbar of the Stage Schematic has a **Rig View** toggle (left of
the New Pegbar button). When it is on, a **Filter nodes** field appears next to
it, and every column node gets up to three small badges under it:

- **Deformers**: the Deformer Stack, Envelope and Free-Form FXs found in the
  column's fx chain. The chain follows the output connections of the column's
  fx, and stops at the FXs that combine several inputs and at the xsheet node.
- **Offsets**: the offset layers of the column (doc/offset_layers.md); a
  disabled layer is marked "(off)".
- **Controllers**: the master controllers (pegbars named `ctrl_*`) that drive a
  channel of the column or the weight of one of its offset layers, found as
  `peg<N>.rot` in an expression key of those curves.

The tool tip of a badge has the whole text. The filter hides the nodes (and
their links) whose name does not contain the text, ignoring the case; an empty
field shows everything. It is the "port filtering" of master plan C2 in as far
as the stage nodes are concerned.

## Partial

- The badges are labels: they cannot be clicked or edited, and the deformers,
  layers and bindings are edited where they were (FX Schematic, Offset Layers
  panel, Master Controllers panel). Other stage objects than columns have no
  badges.
- Deformers are not nodes of the graph, and there is no group node.
- **Publish Channel...** and the group node with published ports are not
  implemented: a stage object has no existing data slot (notes, user data) to
  record a published channel, and a new format is out of scope here.
- The filter works on node names only, not on the badge contents or on ports
  of the FX Schematic.
- The badges are drawn under the node and can overlap the nodes below it.
- A binding made with an expression that is not in a key of the curve, or with
  an expression that builds the reference otherwise (`peg3 .rot`, a different
  channel), is not found.
