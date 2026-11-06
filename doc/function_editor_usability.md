# Function Editor workflow tools

The channel browser now has a search field and a short row of controls. Search
matches objects, channel names and expression references. It filters the browser
only: active graph curves, spreadsheet columns and selected keys stay visible.
Clearing search restores the previous browser expansion.

**Fit Selected** frames the selected keys across all selected channels. Use
**Tools → Fit all visible curves** to frame the complete active animation.

**Edit Keys** opens a numeric editor for the current graph or spreadsheet key
selection. It shows how many keys/channels are selected and whether values are
mixed. Choose an operation, use Preview to see dashed proposed curves, then
Apply. Closing cancels the uncommitted preview; each changed Apply is one undo step.
Repeated Apply preserves the original selected keys, with new baked/breakdown
keys selected when appropriate.

- Set or offset values uses each channel's current display unit.
- Offset frames or retime around a pivot preserves relative selected timing.
  Invalid frames, collisions, crossing unselected keys or invalid channel values
  produce an explanation without a partial edit. Quantize frame grid rounds
  selected times to the sample/grid interval relative to the pivot, with the
  same collision checks and one undo step.
- Retime to target range maps the first and last selected keys, across all
  selected channels, to the explicit Target first/last frames. Interior keys keep
  their relative timing, and timing handles follow the affected segments. The
  fields support fractional frames; native scene files preserve their precision.
  A selection at one frame needs equal destination bounds. Repeating Apply with
  unchanged bounds leaves keys and undo history unchanged.
- Insert breakdown uses the first-frame field and a favour slider. Values below
  0% or above 100% create anticipation or overshoot between surrounding keys.
- Bake evaluates the inclusive first/last frame range at the chosen interval and
  explicitly replaces driven segments with numeric samples. It is exact at those
  samples, not between them. Partial boundaries inside a curve must be existing
  keys; cyclic ranges stay within the original period. Cancel stops sampling
  without changing any curve.
- Reduce linear keys uses the amount field as maximum display-unit error against
  original keys and requested sample positions. Nonlinear, held, stepped and
  driven boundaries are preserved. The preview reports removed keys/error.

Expression, file and Similar Shape driven endpoints cannot be silently converted
by numeric value edits or imported presets. Bake them explicitly when numeric
conversion is intended. Native and SeExpr formulas remain editable in the
existing expression controls.

**Interpolation** exposes the existing native interpolation modes and selected
handle link, unlink and reset operations. **Tools** contains:

- **Channel Sets:** save currently active curves by name and restore their
  visibility later. Save the scene to retain newly assigned channel identities.
  Select a saved set and choose **Rename set**, or press F2, edit its name and
  press Enter. Rename preserves the set's channel membership and persists in
  local preferences without changing animation undo or scene dirty state.
  Empty names and duplicates are rejected. Closing or switching scenes discards
  unfinished name edits; completed renames remain saved.
  Renaming channels does not change these IDs; missing or duplicate identities
  are reported. Sets are local user preferences. Older builds can read the scene
  but may discard the optional IDs when saving it.
- **Solo current / restore visible curves:** temporarily isolate the current
  channel, then recover the previous active-channel list.
- **Lock / unlock selected channels:** protect the selected channels (or current
  channel when no keys are selected). A locked curve remains visible, and editor
  writes are rejected. Locks last for this session; ordinary undo history still
  works.
- **Normalize curve display:** compare channels in a shared 0–1 display range
  without changing stored values. Labels retain channel identity and locked state.
- **Capture / show / hide comparison curves:** compare against a snapshot;
  Restore applies that snapshot as one undo after confirmation. Changing the
  scene/Xsheet invalidates comparison buffers and cancels pending previews.
  The dashed display freezes evaluated values across the visible frame range,
  including native and SeExpr channel dependencies. Its caption states the
  captured range and sampling interval. Between samples the display is an
  approximation; recapture after zooming to extend its range or increase detail.
  Restore retains the exact original keys and formulas, independently of the
  sampled display.
- **Motion Presets:** browse saved numeric curves, inspect a thumbnail, mark
  favorites, search by name, and preview duration/amplitude/value-offset adaptation before
  replacing the current channel. Existing preset files are never overwritten.
  **Import existing curve** accepts native `.curve` files and `.fcurve` presets.
  Native `.curve` files do not store their measure, so import explains that it
  uses the current channel's measure. Import adds a numeric preset to your
  library without changing the channel; Preview and Apply remain separate.
- **Dependencies:** inspect actual resolved native/SeExpr bindings, formula
  errors and dependent channels. Double-click a channel to show it.

Frame dragging can snap to a chosen whole-frame increment or nearby unselected
keys or the scene's frame markers (including their configured offset). Alt
bypasses those extra snaps during ordinary moves; native mouse dragging remains
whole-frame. Alt retains the existing stretch shortcut for contiguous selections
of three or more keys. Use Shift+Alt for a constrained move with snapping bypassed.
Use Edit Keys for precise fractional timing. Shift-drag constrains an axis.
Left/Right nudge selected keys by the configured frame increment; Shift uses ten
increments. Up/Down use the value nudge. Tools also configures
the value nudge and handle hit radius in logical pixels.
