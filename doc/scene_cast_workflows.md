# Working with Scene Cast

Open **Scene Cast** from **Windows**; some room layouts put it under
**Other Windows**. Select assets before using **Asset Actions…**. Save the scene
to retain tags, covers, named collections and recorded Library origins.

## Find and organize assets

Choose **All scene folders** to search the whole scene's Cast, or **Current
folder** to stay in the selected Cast folder. **Search names, paths and tags…**
matches case-insensitive words: `hero walk` requires both words somewhere in an
asset's name, resolved path or tags. This searches scene assets, not the disk or
Shared Library.

Type, status, collection and search filters work together. To find an asset
hidden by another filter, clear the search and choose **All types**, **All
statuses**, **All assets** and **All scene folders**. Sorting and refreshing
retain selected asset identities; assets hidden by a filter are not targets
for selection-based actions.

Useful status choices:

| Filter | What it means |
|---|---|
| **Used in scene** / **Unused** | Whether exposures in the scene and its reachable nested sub-xsheets use the asset. An incomplete usage scan cannot establish that an asset is unused. |
| **Modified assets** | Drawing or palette changes that have not been saved. |
| **New / not saved** | No source exists on disk, and the asset is not recorded as previously saved. |
| **Missing saved assets** | A source expected on disk is absent. **Absent on disk (new or missing)** combines both absent-source cases. |
| **External links** | File links outside the project folders and saved scene folder. This does not identify Library ownership. |
| **Decoded drawings cached** | At least one decoded drawing is in the image cache, including its swap storage. This is not a saved-file or rendered-FX status. |

After changing files outside the application, use **Asset Actions… > Refresh
Disk Status**.

For tags, select assets and choose **Edit Tags…**. Enter comma-separated tags;
this **replaces the tags on every selected asset**. Cancel leaves them unchanged,
and Undo restores the previous tags.

For a reusable group, select assets and choose **Collections… > New Collection
from Selected…**. Recall it from the collection selector. These are explicit
asset memberships, not saved searches; the other filters still apply.
**Add Selected to Collection…**, **Remove Selected from Collection**, **Rename
Collection…** and **Delete Collection…** are undoable. Deleting a collection
does not delete its assets or files. Membership follows the same asset through
renaming and relinking.

## Find an exposure in a nested scene

Select one or more assets and choose **Asset Actions… > Where Used…**. Each
result identifies a **Sheet instance**, **Column** and **Frames**. Frame numbers
belong to the named sheet. Select a result and choose **Go To Exposure**, or
double-click it, to open that particular nested instance at its first listed
exposure.

Navigation leaves exposure contents unchanged and uses normal child-sheet
navigation, which can add navigation entries to Undo. Canceled or limited
searches are labeled partial; cyclic references are not followed. If the scene
changes while results are open, close the dialog and search again.

## Inspect drawings and audition audio

Choose **Inspect / Preview / Audition…** or **Show Inspector** from **Asset
Actions…**. The **Asset Inspector** stays beside the Cast: resize its divider,
or use **Hide Inspector**. It follows the Cast selection. With several assets
selected, choose a row to inspect one of them.

The table shows drawing ranges and gaps, resolution/DPI/depth, palette details,
paths, tags and Library provenance. Scrub the drawing slider to preview actual
drawing IDs without changing exposures. **Use Preview Drawing as Cover** saves
that drawing as the Cast thumbnail; **Use First Drawing as Cover** restores the
default. Cover changes support Undo.

For audio, inspect the waveform, click it or move the seek slider to choose a
start position, then press **Audition Audio**. **Stop Audio** stops playback.
Seeking, changing targets or hiding the inspector stops the current audition;
auditioning does not insert an exposure. Playback requires an available audio
device; an unreadable or unplayable sound reports an error.

## Repair links or make local copies

Save modified drawings and palettes before either operation.

1. Select assets and choose **Relink Selected…**.
2. Choose a folder in **Search for Replacement Assets**. The search looks
   recursively for matching names.
3. In **Review Batch Relink**, inspect each replacement. A unique match is
   preselected; choose among multiple matches, use **Browse…** for another
   file, or leave **Skip**.
4. Choose **Relink Chosen**. Replacements must have a compatible type and all
   required drawing IDs. Failed or canceled validation leaves links unchanged.

Relinking changes sources without copying or deleting files. Exposures retain
their drawing IDs, and one Undo restores the batch's previous sources.

To copy selected external sources into the project instead, choose **Make
Local Copy…**, choose a project folder or the saved scene folder, and review
the confirmation. Copies are prepared in a new folder before links change;
existing files are not overwritten. Cancel leaves the original links intact.
Undo restores those links and **keeps the published copied files** for Redo.
Internal file links are skipped. Asset identities and collection memberships
remain intact.

## Review unused and duplicate assets

**Review Unused Levels…** examines the whole Cast. Review the checked candidates
before choosing **Remove Checked**; assets tagged `keep` start unchecked.
Removal changes Cast membership, not disk files, and supports Undo. An
incomplete usage scan prevents this removal.

**Review Duplicate Assets…** also examines the whole Cast. It compares saved
content, skips assets modified in memory, and offers **Save Report…**. Matching
bytes identify candidates, not interchangeable assets.

To consolidate explicitly, choose **Review Consolidation…**, check the groups
you want, choose each **Keep asset**, and use **Validate Chosen Groups**. Review
the final confirmation. Validation checks drawing IDs, palettes, native
metadata and affected columns. Successful consolidation redirects exposures,
combines tags and moves collection memberships to the chosen survivor. Its
name, folder, cover and Library ownership remain. Undo restores the original
assets and exposures; files stay on disk. Cancel before confirmation leaves the
scene unchanged. Nothing consolidates automatically.

## Prepare a portable scene

**Collection Report…** is a read-only file inventory, separate from named Cast
collections. Check missing sources, sidecars, caches and destination-name
warnings; **Save Report…** exports CSV. A canceled scan is labeled partial.

Save All before packaging. In the **File Browser**, select the saved scene
file and use **Collect Assets** from its context menu. This existing command
imports external resources into the scene's project and saves the scene when
resources were collected. It is a file operation, not a Cast Undo action;
aborting a multi-scene collection does not undo scenes already processed.

After copying or relocating the project, return to **Collection Report…** and
choose **Verify Relocated Scene…**. Select the relocated `.tnz`, then its
project root. Verification opens a separate scene, checks that discovered
dependencies resolve inside that root, and reads exposed drawings without
changing the working scene. Inspect every reported issue; this does not certify
compatibility with other applications. The [Scene checker](scene_checker.md)
provides complementary exposure and render-setting checks.

## Understand Library ownership

The [Library panel](animate_library.md#the-library-panel) inserts shared rigs
and symbols. Recorded Library origin is separate from file location:

| Inspector label | Meaning |
|---|---|
| **Library-linked file** | The current source path still matches the source recorded during Library insertion. Missing-file status is separate. |
| **Local copy from Library** | The asset retains its recorded origin and uses a copied source. |
| **Library snapshot** | Rig or symbol structure was inserted as a snapshot. |
| **Library origin; source replaced** | Origin is retained, but the current source has been replaced. |

These labels come from recorded Library metadata, not guesses based on an
external path. They do not provide automatic rig or symbol synchronization.
**Make Local Copy…** changes selected external Library-linked files to local
copies; it does not synchronize or replace the entire rig. Copying an asset
already marked source-replaced keeps that distinction. Undo restores the
previous links and provenance, and saving the scene preserves the recorded
origin for reopening.
