# Aspect ratio guides, letterbox and pegbar guides

A separate project from the master plan. It builds on the 4:3 Center-Cut
Guide: more aspect ratio guides, a letterbox, an outside-camera colour, pegbar
alignment guides, and exporting to an aspect ratio. It ships as five pull
requests; display-only parts come first, the render change last.

| Part | What | Status |
|---|---|---|
| 1 | Aspect ratio guides in the Viewer and the Flipbook | This pull request |
| 2 | Letterbox overlay outside the guides | This pull request |
| 3 | Outside-camera colour and opacity | Separate pull request |
| 4 | Pegbar alignment guides from the `.fdg` peg-hole files | Pull request on top of parts 1 and 2 |
| 5 | Export to an aspect ratio: letterbox or crop | Pull request on top of part 4 |

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

## 4: pegbar alignment guides

**View > Pegbar Guide** draws, on the table like the Field Guide:

- the field boundary: the field size of the file (`fld_12_...` is 12 field)
  or, for a file without one, the Field Guide size of the Scene Settings, at
  the Field Guide aspect ratio;
- the registration holes to size: the round centre hole and the two slots;
- the paper edge behind the pegs, dashed;
- the name of the file by the centre hole.

**View > Pegbar Guide Settings...** picks the field guide file (every `.fdg`
in `stuff/config/fdg`: Acme and Oxberry 10, 12, 15 and 16 field, Japanese TV
and film; a file a studio adds shows up too), the side of the pegs (top,
bottom, left, right) and the colour. They are kept in the user's settings;
scenes are unchanged.

The holes come from the same files and values as the clean-up autocenter,
so the guide matches what the clean-up registers to. Toonz 4.6 (on SGI IRIX)
used these Acme and Oxberry standards. The layout math
(`include/toonz/pegbarguide.h`) has a regression test, `pegbarguide_test`.

## 5: export to an aspect ratio

Output Settings > More Settings:

- **Aspect Ratio**: None (the default), 4:3, 1.66:1, 16:9, 1.85:1, 2.39:1,
  2.40:1 or Custom (typed as `2.2`, `2.2:1` or `21:9`).
- **Fit**: **Letterbox** keeps the frame size and fills outside the ratio
  with the **Bar Color** (with opacity, so the bars can be transparent in a
  PNG); **Crop** writes only the part inside the ratio, centred, its sizes
  rounded down to even numbers for the movie encoders (1920x1080 at 1.85:1
  gives 1920x1038, at 2.39:1 1920x802).

The camera and the framing do not change: the frames are rendered as before
and fitted when they are written, after the color space conversion and before
the frame numbering, so the numbers stay in the picture. It applies to every
render written to a file (Render, Fast Render, batch and farm renders through
`tcomposer`), to both eyes of a stereoscopic render, and the clapperboard is
drawn at the written size. The Preview (to the Flipbook) is not fitted; use
the guides and the letterbox overlay of parts 1 and 2 to see the result.

The setting is saved in the scene, in the output settings, **only when it is
used**: scenes that do not use it are saved exactly as before. A scene that
uses it opens only in builds that have it; OpenToonz builds without it reject
the unknown tag.

Tests: `aspectexport_test` (the written size, the copy of the settings, the
scene file with and without the setting) and the pixel rect cases of
`cameraguide_test`.
