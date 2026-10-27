# Light Table extras, Tone / Highlight setup and Reference panel

Three small Tier 11 items (master plan E3, E4, E5; parity plan Q7, Q8 and the
Reference view row). Everything is opt-in: with the defaults, a scene that does
not use them looks, saves and renders exactly as before.

## E3: Light Table opacity and per-column exclusion

The Light Table (View > Light Table) draws every column except the current one
faded, and only while drawing (not during playback).

- **Opacity.** Preferences > Onion Skin > **Light Table Opacity**, 0 to 100,
  default 30 (the former fixed value). It is a user preference, not a scene
  property. The faded columns keep their relative opacity: a column at 50%
  shows at 50% of the preference. The value reaches the stage builder through
  `Stage::VisitArgs::m_lightTableOpacity` (`toonzlib/stage.cpp`,
  `toonz/sceneviewer.cpp`).
- **Exclusion.** A column can be excluded from the Light Table: it is then
  drawn at its own opacity even when it is not the current column (for example
  a background to keep at full strength). Right-click a column header and use
  **Exclude from Light Table**, **Include in Light Table** (both act on the
  selected columns, or the current one) or **Include All in Light Table**.
  The commands are `MI_LightTableExcludeSelectedColumns`,
  `MI_LightTableIncludeSelectedColumns` and `MI_LightTableIncludeAllColumns`
  (`toonz/columncommand.cpp`). They are not undoable, like the Selection Lock
  commands they follow. The camera column is never faded.

The flag is `TXshColumn::isLightTableExcluded()`, a bit (0x80) of the column
status word that is already saved with every column, like the Selection Lock
bit. The word is unchanged for a column that is not excluded, so **older
builds cannot open scenes that use it** as the bit means nothing to them: they
ignore it and fade the column (they still open the scene).

## E4: Tone / Highlight setup

**Xsheet > Add Tone / Highlight Shading** (`MI_ToneHighlightSetup`,
`toonz/tonehighlightcommand.cpp`) works on the selected level columns, or the
current one. For each column it inserts two Tone / Highlight Fxs
(`toneHighlightFx`) after the column, with the usual Insert FX rules: first a
**Tone** (multiply) and then a **Highlight** (screen), so the output of the
column goes through both.

| | Tone | Highlight |
|---|---|---|
| Mode | Tone | Highlight |
| Color | 90, 80, 140 | 255, 244, 214 |
| Intensity | 0.6 | 0.5 |
| Other | defaults (protect lines on, softness 0) | defaults |

The parameters are the defaults of the new Fxs, not keys. Each column gets its
own Fxs with unlinked parameters. All the Fxs of one command are one undo step
(`TUndoScopedBlock`). Nothing is stored beyond the Fxs themselves, which are
the existing ones.

Limitation: the **Matte** ports are left free. The matte (the drawn shapes of
the shadows and highlights) comes from another column, which you connect in the
FX Schematic. The Fxs do nothing until a matte is connected; an automatic
"line-art matte" of the column itself is not built, as the Fx has no access to
the Line Art of a drawing.

## E5: Reference panel

**Windows > Reference** (`toonz/referencepanel.*`):

- **Several images.** Load Image... takes several files, and dropping files on
  the panel adds them. The loaded images are listed in a combo box (up to 20;
  the oldest is dropped beyond that) and **Remove** takes the current one out
  of the list. Loading (or dropping) an image already in the list reads the
  file again, so that changes made to it are shown, and keeps its view.
- **Remembered view.** The zoom, pan, flips, rotation and a new **opacity**
  slider (0 to 100%) are stored for each image, and restored when the image is
  selected and when the application starts. Images that no longer exist are
  forgotten. A new image continues the current flips, rotation and opacity
  (the former behaviour) and is fitted to the panel.
- **PSD layers.** For a `.psd` file a second combo box lists **Flattened
  image** and the layers that have pixels (folder markers are left out),
  topmost first. A layer is shown at the size of the canvas, and keeps the view
  of the file, so layers can be compared in place. The reader is `TPSDReader`
  (`common/psdlib`). Layer names are read in the local 8-bit encoding.

State is kept in the user environment (`ReferencePanelImages` and
`ReferencePanelCurrentImage` in the env file, as one percent-encoded line;
`ReferencePanelLastImage` is still written for older builds). It is never
stored in a scene.

Limitations: for 16-bit and CMYK PSDs the layers are listed but cannot be
shown (`TPSDReader::load` returns nothing for them), and a warning appears;
the flattened image of such a file is still shown through the fallback
readers. Layer visibility, blend modes and masks of the PSD are
not applied; each layer is shown alone.
