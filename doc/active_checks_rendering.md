# Active Checks Rendering

**Render Active Checks** includes supported View checks in rendered images.
It is off by default and is available under **File Settings** in both
**Render > Output Settings** and **Render > Preview Settings**. The two options
are independent.

## Use

1. Select the column to check and the palette style to highlight.
2. Enable the desired checks in the **View** menu.
3. Enable **Render Active Checks** in Output Settings or Preview Settings.
4. Render or preview the scene normally.

The render uses a snapshot of the active checks, current column, selected style
and check colors. Changing the Viewer while a render is running does not change
that job. Disabling the option restores ordinary rendering.

## Supported checks

- Ink Check
- Ink#1 Check
- Paint Check
- Transparency Check
- Fill Check
- Inks Only
- Black BG Check

Source checks, including Inks Only, use the selected Toonz Raster or Vector
column. Full-color raster images do not contain separate palette ink and paint
identifiers. Black BG Check supplies the render background for the whole scene.

Gap Check, camera and field guides, rulers, table decorations, onion skin,
Shift and Trace, and other Viewer editing aids are not included.

### Combining checks

All supported active checks are captured together. Ink Check highlights the
selected ink style, Ink#1 Check highlights style 1, and Paint Check highlights
the selected paint style. These highlights take precedence over Transparency
Check colors. If Ink Check and Ink#1 Check both match an ink, Ink Check takes
precedence. Inks Only hides paint even when Paint Check or Fill Check is active.
Fill Check temporarily fills closed unpainted regions and uses the transparency
diagnostic colors. Black BG Check changes the output backing and selects the
corresponding transparency ink color.

Temporary fills and check colors do not change the source drawings or palettes.

### Effects and palette styles

Checks are applied before ordinary compositing effects, so those effects can
change their appearance. Toonz Raster check colors follow the Viewer's
diagnostic display, including average colors for procedural palette styles.
Some Toonz Raster effects, including Palette Filter in Lines & Areas mode,
Blend Tz, Calligraphic, Out Border and Art at Contour, require the original
indexed ink and paint data. When these effects conflict with an active check,
rendering reports the unsupported combination. Other columns and Black BG
Check alone do not cause this conflict.

## Saved scenes and presets

Scenes save the option and captured check settings for batch rendering.
Saving a scene refreshes the capture from its current View state. Output
presets save whether the option is enabled and capture the current scene's
View state when applied.

Older scenes and presets leave the option off. Older applications can open
scenes containing the new settings, but ignore Active Checks Rendering.

## Rendered exports

The script renderer and rendered-frame Animated SVG export use the output
check settings. Native vector SVG export does not run the scene renderer.
Layered PSD export applies source checks to the selected column's layer and
includes Black BG in the merged preview. Its source layers remain transparent,
as with the existing scene-background handling; no background layer is added.

## Application review

The automated tests cover combined check colors, source preservation, snapshot
serialization and column rendering. Application review should also compare a
Vector and a Toonz Raster scene with the Viewer, including:

- Ink + Paint + Transparency, Fill + Inks Only, and Black BG combinations;
- a shared drawing in two columns, with only the selected column checked;
- a selected column inside a sub-xsheet, including saving that sub-xsheet;
- changing checks, styles and check colors while previewing;
- rendering a saved scene through the batch renderer;
- toggling the option off and comparing with the original render;
- an unsupported Toonz Raster effect producing the documented error.
