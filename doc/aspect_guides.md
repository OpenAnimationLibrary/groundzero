# Aspect ratio guides, letterbox and pegbar guides

A separate project from the master plan. It builds on the 4:3 Center-Cut
Guide: more aspect ratio guides, a letterbox, an outside-camera colour, pegbar
alignment guides, and exporting to an aspect ratio. It ships as five pull
requests; display-only parts come first, the render change last.

| Part | What | Status |
|---|---|---|
| 1 | Aspect ratio guides in the Viewer and the Flipbook | This pull request |
| 2 | Letterbox overlay outside the guides | This pull request |
| 3 | Outside-camera colour and opacity | Planned |
| 4 | Pegbar alignment guides from the `.fdg` peg-hole files | Planned |
| 5 | Export to an aspect ratio: letterbox or crop | Planned |

## 1 and 2: guides and letterbox

**View > Aspect Ratio Guides** (also in the Viewer and Flipbook right-click
menus, the viewer title bar's show/hide menu, and the safe area button's menu)
holds one toggle per guide:

| Guide | Ratio | Colour |
|---|---|---|
| 4:3 Center-Cut Guide (unchanged command, so its shortcut keeps working) | 1.33:1 | cyan |
| 1.66:1 Guide (European Widescreen, the traditional animated feature ratio) | 1.66:1 | violet |
| 16:9 Guide | 1.78:1 | green |
| 1.85:1 Guide (Theatrical Widescreen) | 1.85:1 | yellow |
| 2.39:1 Guide (Scope, the DCI figure) | 2.39:1 | orange |
| 2.40:1 Guide (Anamorphic Scope, the figure usually quoted) | 2.40:1 | red |
| Custom Aspect Ratio Guide | typed in the settings | white |

- Any number of guides can be on at once. Each is centred in the camera (the
  image, in the Flipbook), drawn dashed in its colour, and labelled with its
  ratio in its top left corner.
- **Letterbox Outside Aspect Guides** shades the frame outside each active
  guide. With two guides on, the area outside both is shaded twice, so each
  band reads separately.
- **Aspect Ratio Guide Settings...** sets the custom ratio (`2.2`, `2.2:1`,
  `21:9` or `21/9`) and the letterbox colour and opacity (default black, 70%).
- Every toggle and setting is kept in the user's settings, not in the scene.
  Scenes are unchanged.
- Every command is in the shortcut editor, under View.

The guide math (`include/toonz/cameraguide.h`: the centred rect, the typed
ratio, the letterbox bars) has a regression test, `cameraguide_test`.

## 3: outside-camera colour (planned)

Today the area outside the camera, in Camera view, uses the Preview BG colour
at full opacity. A new **Outside Camera** colour and opacity in Preferences >
Colors, so the table and the art outside the frame can show through dimmed.
The table disc keeps its own **Table Color**.

## 4: pegbar alignment guides (planned)

A Viewer overlay of the field boundary and the registration holes (the centre
round hole and the two side slots), drawn to size from the peg-hole files the
clean-up already ships in `stuff/config/fdg` (Acme and Oxberry 10/12/15/16
field, Japanese TV and film). Fixed to the table like the Field Guide, top or
bottom pegs, any `.fdg` file added to the folder shows up in the list. Toonz
4.6 (on SGI IRIX) used these same Acme and Oxberry standards.

## 5: export to an aspect ratio (planned)

Output Settings gets **Aspect Ratio** (None, the presets above, Custom) and
**Mode**: **Letterbox** keeps the frame size and adds bars of a chosen colour;
**Crop** outputs only the guide area (1920x1080 at 1.85:1 gives 1920x1038),
rounded to even sizes for the movie encoders. The camera and the framing do
not change. It applies to Render, batch render and the Flipbook preview, and
is saved in the scene only when it is used. Tests: the size math, a save and
reload of the setting, a render reference image of each mode, and the
compatibility check of the existing scenes.
