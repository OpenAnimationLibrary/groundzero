# PureRef reference boards

PureRef authors and edits the board; Groundzero displays the saved result in
**Windows > Reference**. PureRef is optional and is not bundled. This feature
uses the [documented command-line interface](https://www.pureref.com/handbook/automation/),
not a reader or writer for the `.pur` format. AI assistance: Codex.

## Import and use

1. Open the arrow menu beside **Load Image...**, then **Import PureRef Board...**.
   A `.pur` file can also be dropped on the panel.
2. Select the installed PureRef executable if it is not found automatically.
   **PureRef Application...** changes that selection later. On macOS select
   PureRef.app or its executable; on Linux an executable AppImage also works
   through the same application picker, subject to its runtime requirements.
3. Choose whether to include individual images. **No** (the default) imports
   only the overview. **Yes** imports the overview and the images exported by
   PureRef, with cropping enabled.
4. The board occupies one entry in **Loaded Images**. Its second selector
   contains **Board overview** and any imported images. The existing pan,
   zoom, mirror, rotation and opacity controls apply to the displayed image.
   Selection and the board's current viewing state are remembered across
   restarts, as with PSD layer selection. Views are per board, not per member.

There is no longer a 20-reference limit or automatic removal of older images.
Board members do not become separate top-level references. Only the selected
image is decoded for display; the list does not keep every image in memory.

**Open in PureRef** opens the original board for authoring. Save there, then
use **Refresh from PureRef** to update Groundzero's imported images. Refresh
retains the selected exported filename when it still exists and falls back
to the overview otherwise. Reordering or renaming images in PureRef can
change their exported filenames. The original `.pur` file is never saved,
overwritten or converted to a different version by Groundzero.

The overview is a flattened image, requested at 4096 by 4096 export resolution;
notes, drawings and layout are not editable in Groundzero. Individual images
are exported as cropped PNGs. They do not carry the board's item positions,
groups or editable annotations. GIF playback is not transferred. A user can
instead export images manually from PureRef and load those through the
existing image loader, without configuring an application path.

## Persistence and failure handling

The `.pur` source path, selected member and viewing state stay in the existing
user environment. Reference collections are **not yet scene properties** and
are not included in scene archives. Converted images and a versioned manifest
are kept in the application's local data directory, separately from the board.
Keep the `.pur` and its linked source images together when moving it; then
import it from its new location. Removing a reference removes its list entry,
not the original board or images. The most recent import is retained locally.

Reopening the panel reads these saved images without starting PureRef. They
remain viewable if PureRef or the original board is unavailable. The source
must be available for editing or refreshing. Unsaved PureRef edits are not
visible until saved and refreshed; there is no live synchronization.

Import runs a separate PureRef process using a private settings file. PureRef
may display a window during export; no headless operation is assumed. A busy
dialog keeps the application painting and provides Cancel. The export is
limited to two minutes. A canceled, timed-out, failed or unreadable export
keeps the previous successful import. A successful refresh publishes its new
manifest atomically and removes the replaced export directory. Imports of the
same board are locked against concurrent conversion by another instance.

Paths containing semicolons cannot be used for command-line import because
PureRef documents no escape for its command separator. Move/rename such a
board or use manual image export. Commas in board paths are escaped as PureRef
documents. Decoded exports are limited to 64 million pixels per image to avoid
unreasonable allocations; this is independent of the number of references.

## Validation

The focused CI test runs a controlled external exporter to cover 80 member
images, path escaping, restart/offline cache reads, overview-only refresh,
failed/incomplete/corrupt exports, concurrent source changes, cancellation,
timeout, missing executable, cache cleanup and invalid manifest paths. It does
not establish compatibility with an installed PureRef release.

Before merge, test with the supported PureRef release on each target platform:

- Import legacy and current-format boards, with embedded and linked images;
  compare overview aspect ratio, crops, rotations, text and annotations.
- Import more than 20 ordinary images and a board with more than 20 images;
  confirm no reference disappears and member selection works.
- Restart Groundzero, remove the source temporarily, and verify cached images.
- Edit/save in PureRef, refresh, and verify the image and panel view update.
- Keep an unsaved interactive PureRef board open during import; confirm it is
  neither changed nor closed by the export process. Test non-default PureRef
  single-window settings as well.
- Cancel an export and try missing linked files or a damaged board; verify the
  previous references remain available and the exporter does not stay running.
