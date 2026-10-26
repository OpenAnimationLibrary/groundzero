# Storyboard pane

The Storyboard pane (Tier 6) shows the navigation tags of the xsheet as panel
cards: each tag starts a panel that lasts until the next tag (the last one
until the end of the scene) and its label is the panel caption. This page
describes the follow-ups of master plan item B8.

## Use

- **Notes**: the Notes field under Caption holds a free one-line text for the
  current panel. The notes are shown in the tooltip of the card.
- **Play Animatic** starts at the first frame of the current panel and pauses
  at its last frame. It uses the same mechanism as Short Play (the viewer's
  stop-at frame, `FlipConsole::setStopAt`). With no current panel it plays
  from the first frame to the end, as before.
- **Reorder**: drag a card and drop it on the left half of another card to
  move the panel before it, or on the right half to move it after it. The
  cells of the panel (every column) move together with its navigation tag, so
  the panel keeps its caption, color and notes. The move is a single undo
  step ("Move Storyboard Panel"). Notes edits are undoable too.
- **Camera Thumbnails** (on by default, not saved): each card shows a small
  camera render of the first frame of the panel, made by the icon generator
  (`IconGenerator::getXsheetFrameIcon`, the same renderer as the sub-xsheet
  icons, drawn by the camera stand with the scene background). The renders are
  cached and regenerated 500 ms after the last change of the xsheet or of a
  level. Turning the option off shows the drawing of the first frame as
  before.

## File format and compatibility

The notes are stored in the navigation tags of the xsheet, as an optional last
value of a `<tag>` element, written only when the notes are not empty:

    <tag> 12 "label" 255 0 255 "notes" </tag>

Scenes with no notes are saved exactly as before, and scenes saved by older
builds load as before. **Older builds cannot open a scene that uses notes**:
their loader reads the five values of a tag and then expects the end of the
tag, so it throws on the extra value.

## Limitations

- Reordering moves the cells and the tags only. Sound columns, locked columns
  and the animation keyframes of the columns, pegbars and camera are not
  moved: the keyframes stay on their rows.
- The tag of a removed xsheet frame restored by the undo of "Remove Frame"
  does not get its notes back (it only restores the label, as before).
- Notes are a single line.
- The camera thumbnails are rendered on the icon worker thread from the live
  xsheet, like the sub-xsheet icons: a render that overlaps an edit can show
  a mixed state until the next regeneration.
