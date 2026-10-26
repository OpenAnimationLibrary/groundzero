# Translations status (master-plan item D5)

This records the result of master-plan item D5: the strings added by the
features of Tiers 2 to 6 (and the Tier 10 rig commands) were added to the `.ts`
catalogs by reading the sources. `lupdate` was not run (nothing was built or
run when this was written), so the entries were typed from the `tr()`,
`QObject::tr()`, `QT_TR_NOOP()` and `QCoreApplication::translate()` calls.

## What was added

The catalogs are in `toonz/sources/translations/<language>/<module>.ts`. The
same entries were added to every language directory (chinese, czech, french,
german, italian, japanese, korean, norwegian_bokmal, portuguese_brazil,
russian, spanish) so that translators see them. Each entry has an empty
translation marked `type="unfinished"`; nothing was translated. An entry that
a catalog already had in the same context was not added again, and the files
were not otherwise reformatted.

Per language: 270 entries, in these contexts (the number of entries is in
brackets).

| Catalog | Contexts |
|---|---|
| `toonz.ts` (218) | MasterControllersPane (57), MainWindow (37), QObject (30), OffsetLayersPane (19), ReferencePanel (17), RigLibraryPane (16), StoryboardPane (14), PosesPane (7), RigEaseCommand (5), DrawingSubstitutionPane (5), SubstitutionSliderPane (3), PreferencesPopup (3), StackedMenuBar (2), CellArea (2), ReferenceImageView (1) |
| `toonzqt.ts` (35) | StageSchematicScene (30), SchematicViewer (4), FunctionTreeModel (1) |
| `tnztools.ts` (6) | LassoCutterTool (6) |
| `toonzlib.ts` (11) | QObject (11) |

By feature:

- Panels: Reference (`referencepanel.cpp`), Drawing Substitution,
  Substitution Slider, Storyboard, Master Controllers, Offset Layers and Rig
  Library. The panel titles and the Windows menu commands are the `QObject`
  entries of `toonz.ts` (titles, from `tpanels.cpp`) and the `MainWindow`
  entries (menu commands). The buttons, tooltips, dialogs and messages are in
  the class of each panel.
- Menu commands in `MainWindow`: the rig breakdown and rig ease presets (from
  the `QT_TRANSLATE_NOOP` tables in `rigeasecommand.h`), Include Child Objects
  in Rig Commands, Bake Envelope into Vector Level, Add Tone / Highlight
  Shading, New Annotation Column, the Light Table include and exclude commands,
  Lasso Cutter Tool, Insert Blank Drawing, Previous and Next Frame Past End,
  Convert Lines to Areas and Areas to Lines, the 4:3 Center-Cut Guide and Save
  As Default Raster Palette. The Rig Breakdown and Rig Ease sub-menus are in
  `StackedMenuBar` (menu bar) and `CellArea` (cell menu).
- Preferences: Light Table Opacity, Table Color and the Lasso Cutter Tool entry
  of the toolbar list (`PreferencesPopup`).
- Tools: the Lasso Cutter tool options (`LassoCutterTool` in `tnztools.ts`).
- Rig view and published channels in the Stage Schematic (`StageSchematicScene`,
  `SchematicViewer`, `FunctionTreeModel` in `toonzqt.ts`).
- Undo and history texts of the new commands (`QObject`: storyboard panel,
  drawing substitution, offset layer, published channel, bake envelope and bake
  drawing swap; the offset layer and published channel ones are in
  `toonzlib.ts`).
- Messages of the Tone / Highlight setup and the envelope bake commands.

## Not covered

- **FX names and parameter names** (Envelope Deformer, Envelope Deformer Plus,
  Deformer Stack, Bone Deformer, Free-Form Deformer, Curve Deformer, Colour
  Override, Auto-Patch). These are not in the `.ts` files: their English names
  are in `stuff/config/current.txt`, and the repository has no per-language copy
  of that file to extend. They are left as they are.
- **`MasterControllerPad`**: the class has no `Q_OBJECT`, so its one tooltip
  ("Drag to set the X and Y controllers, from 0 to 1") is looked up in the
  `QWidget` context at run time, and `lupdate` would file it under
  `MasterControllerPad`. It was left out; add `Q_OBJECT` (or a
  `Q_DECLARE_TR_FUNCTIONS`) to the class first.
- The strings of the script bindings and the Spine export, and most of the
  other changes made since the last catalog update, were not read through; only
  the features of the list above were.
- The entries came from reading the code, so they may differ in small ways from
  what `lupdate` would produce (for example, the context of a `QT_TR_NOOP` used
  outside a class). A `lupdate` run will keep the matching entries and mark any
  that are no longer used as obsolete.
- No `.qm` file was built; the new entries show in English until they are
  translated and compiled.

## How to regenerate with lupdate later

Run `lupdate` for the catalogs, as described in
`toonz/sources/translations/README.md`:

```sh
bash toonz/sources/translations/update.sh <language>      # one language
cmake --build toonz/build --target translate_update_toonz # all languages, one module
```

The other targets are `translate_update_tnztools`, `translate_update_toonzqt`,
`translate_update_toonzlib`, `translate_update_tnzcore`,
`translate_update_colorfx` and `translate_update_image`. Review the diff: the
entries added here should stay (now with their source locations), and any
that `lupdate` does not find again are marked obsolete or dropped, which
shows where this list was wrong.
