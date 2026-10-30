# Shared Library

Rigs, symbols and pose sets stored outside any scene, shown in one panel and
inserted into any scene, like the Library that Adobe Animate shares between
documents. Inserting a rig or symbol takes a snapshot of its structure; its
level files can be copied or linked. Scenes retain recorded Library origin,
but do not automatically follow later structural changes to the item (see
"Not in this version" at the end).

## The Library panel

**Windows > Library** opens the panel (it was called Rig Library; rooms
saved with the Rig Library keep it). It has three tabs:

| Tab | Shows | Double-click |
|---|---|---|
| Rigs & Symbols | the templates of the Library: rigs saved with **Save Selection as Template** and symbols saved with **Save Symbol to Library**, with their thumbnail and a "Rig" or "Symbol" badge | inserts it |
| Pose Sets | the pose sets of the Library, with the number of poses of each | loads it into the scene |
| Scene Poses | the poses of the current scene (as before) | applies the pose |

- **Rigs & Symbols**: **Insert**, **Rename...**, **Delete** and
  **Refresh**. A rig or symbol can also be dragged into the xsheet cells.
- **Pose Sets**: **Load into Scene**, **Save Scene Poses...**,
  **Rename...**, **Delete** and **Refresh**.
- **Scene Poses**: **Apply** and **Apply as Offset Layer**, unchanged.
- The same commands are in the right-click menu of the lists, and the
  tooltip of an item shows its kind, its revision and the scene it was saved
  from.
- The lists follow the Library folder: items saved, renamed or deleted, here
  or by hand in the folder, show up after a moment. **Refresh** reads the
  folder again.

## Saving to the Library

- **Save Selection as Template** (columns selected, Xsheet menu or column
  header menu) works as before: the columns, their pegbars, fxs and cameras
  become a template.
- **Save Symbol to Library**: right-click a symbol in the Symbols panel and
  choose **Save to Library...**, or use **Xsheet > Save Symbol to Library...**
  (also in the Sub-xsheet menu). The command saves the sub-xsheet of the
  first selected column showing one; with no such column selected, the symbol
  selected in the Symbols panel; otherwise the sub-xsheet in the current
  cell. The symbol is saved the same way as Save Selection as Template saves
  columns.
- **Save Pose Set to Library**: **Save Pose Set...** in the Poses panel, or
  **Save Scene Poses...** in the Pose Sets tab, saves all the poses of the
  current scene and its base pose.

The name is asked, proposing the symbol name (or the scene name for a pose
set). If the Library already has an item with that name, you choose
**Overwrite**, **Keep Both** (the new item is saved as `name_2`, `name_3`...)
or **Cancel**. Names are made safe as file names on every system: `/ \ : * ?
" < > |` and control characters become `_`, dots at the start and dots and
spaces at the end are removed, reserved Windows names get a `_` (`CON` becomes
`CON_`), and the name is at most 120 characters long.

A rig or symbol is packaged with copies of its level files, so they must be
saved on disk: if the scene is untitled or a level has unsaved changes, you
are asked to **Save All and Continue** first.

## Where the items are stored

Everything is in the Library folder (the **Library** node of the File
Browser: the `library` folder of the stuff folder, unless the LIBRARY path is
set):

| Item | Files |
|---|---|
| Rig or symbol | `templates/<name>.tnz`, the copies of its level files in `templates/<name>/`, its thumbnail `templates/sceneIcons/<name> .png`, and `templates/<name>.libinfo` |
| Pose set | `poses/<name>.tposes` and `poses/<name>.libinfo` |

The `.tnz` of a template refers to its level files as
`$scenefolder/<name>/...`, so it can be inserted from any project.

A `.tposes` file holds the poses in the format the scene uses for them,
inside a `<poseSet version="1">` tag.

The `.libinfo` file next to each item records its identity and revision. When
a rig or symbol is inserted from the registered Library templates folder,
valid metadata from this file supplies its recorded origin in the scene:

```xml
<?xml version="1.0" encoding="UTF-8"?>
<libraryItem version="1">
    <id>8b0e5d0c-3f3e-4f0e-9b8e-2d6f1b7c1a90</id>
    <revision>2</revision>
    <kind>symbol</kind>
    <sourceScene>sc010</sourceScene>
    <levelFolder>Hero</levelFolder>
</libraryItem>
```

- `id` is random; it stays the same when the item is overwritten or renamed.
- `revision` is 1 for a new item and goes up by one at each overwrite.
- `kind` is `rig`, `symbol` or `poseSet`.
- `sourceScene` is only the name of the scene the item was saved from
  (empty for an untitled scene): never a path or a user name.
- `levelFolder` (templates only) is the folder of its level files.

Older templates without `.libinfo` get one the next time they are overwritten
or renamed. An insertion without valid Library metadata does not infer an
origin from the file's location. Scene files now preserve the recorded item
identity, revision and ownership relationship on inserted assets, along with
their own distinct Cast identities. Existing scenes are not retroactively
attributed to a Library item.

## Inserting a rig or a symbol

**Insert**, a double-click or a drag into the xsheet loads the template as a
sub-xsheet at the current frame (or where it is dropped), as **Load As
Sub-xsheet** does:

- If the camera of the template differs from the camera of the scene, you
  choose to keep the template camera or to use the scene camera.
- You choose how its level files are used (unless the Default File Import
  Behavior preference already decides it):
  - **Import** copies them into the current project (into a sub-folder named
    after the item if Create Sub-folder when Importing Sub-Xsheet is on). The
    scene then has its own copies and does not depend on the Library.
  - **Load** links them: the scene uses the files in the Library folder. If
    the item is later overwritten or deleted, the scene sees the new files,
    or misses them.
- A level already in the scene with the same name and file is reused; the
  other levels are added to the cast, renamed if their name is taken.

Scene Cast distinguishes the inserted **Library snapshot** from its
**Library-linked file** or **Local copy from Library** assets. A reused level
keeps its existing attribution. A linked-file label means its current path
still matches the recorded source; missing-file status is separate. Relinking
to another source retains the origin but reports **Library origin; source
replaced**. See [Library ownership in Scene Cast](scene_cast_workflows.md#understand-library-ownership)
for inspecting these relationships and making local copies.

The insertion is **one undo step**: Undo removes the new column (or the cells
put in an empty column), the levels added to the cast and the cast folder
made for them; Redo puts them back. Undo does not delete the files that
Import copied into the project. Every scene loaded as a sub-xsheet is now
undoable this way, also with Load As Sub-xsheet and when a `.tnz` is dropped
from the File Browser.

## Loading a pose set

**Load into Scene** (or a double-click in the Pose Sets tab, or **Load Pose
Set...** in the Poses panel, which also opens `.tposes` files from anywhere)
adds the poses of the set to the poses of the scene:

- Poses with new names are added after the scene poses.
- If some poses of the set have the same name as poses of the scene, they are
  listed and you choose **Replace** (the set poses take their place),
  **Keep the Scene Poses** (those set poses are skipped) or **Cancel**. When
  a name appears several times, the first pose of the set with that name goes
  with the first scene pose with that name, the second with the second, and
  so on; the extra ones are added.
- A scene without a base pose takes the base pose of the set, so new poses can
  be added to it; a scene with a base pose keeps it.
- Poses refer to the columns and pegbars by their position (Col1, Peg2...),
  so a set applies to scenes where the rig has the same layout.

Loading a pose set is **one undo step** when it changes the scene library.
Undo restores the complete previous pose order, values and base pose; Redo
restores the imported result. **Cancel** leaves the library and undo history
unchanged, and an identical import does not add an undo step. **Set Base**,
**Add Pose** and deleting a scene pose are also undoable. These operations
change the scene's pose library, not the saved `.tposes` file.

## Renaming and deleting

- **Rename...** renames the item file, its thumbnail and its `.libinfo`. The
  level files of a template stay in their folder, since the template refers to
  them there; the `.libinfo` records that folder, so that a new item with the
  old name gets another one (`name_2`...) instead of overwriting them.
- **Delete** asks first, then deletes the item, its thumbnail, its `.libinfo`
  and, for a template, the folder of its level files unless another template
  uses it. Scenes where the item was inserted with Import keep their copies;
  scenes where it was inserted with Load lose its level files. It cannot be
  undone.

## Not in this version

- **Update Uses / live structural synchronization**: recorded origin and
  revision identify the inserted snapshot; changing a Library item's rig or
  symbol structure does not update existing scenes. **Load** still references
  the Library's level files, so replacing or deleting those files can affect
  scenes that use them. Recorded origin is not a live synchronization system.
- **No Levels or Palettes tabs**: levels are found in the File Browser, and
  palettes stay in the Studio Palette, which already shares them between
  scenes.
- Renaming a template does not rename the folder of its level files.

## Code

- `toonz/librarycommand.h/.cpp`: the Library folders, the `.libinfo` files,
  Save Symbol to Library (`MI_SaveSymbolToLibrary`), pose sets, rename and
  delete.
- `toonz/subscenecommand.cpp`: `SubsceneCmd::saveXsheetAsTemplate()`, the
  part of Save Selection as Template that copies the level files, recodes
  their paths, saves and restores them; it is used by both commands.
- `toonz/iocommand.cpp`: `LoadSubSceneUndo`, recorded around
  `loadChildLevel()` in `IoCmd::loadResources()`, and import ownership recording.
- `toonz/poselibrarycommand.cpp`: complete scene pose-library undo transactions.
- `toonzlib/txshlevel.cpp`: Cast identity and Library-origin persistence.
- `toonz/riglibrarypane.h/.cpp`: the Library panel.
- `include/toonz/libraryitems.h`: the file name rules and the pose set
  merge, tested by `toonzlib/tests/libraryitems_test.cpp`.
