# Workflow workspaces

The Workflow menu switches **Room Sets** without restarting Groundzero. A room
set is a collection of rooms. The menu and Preferences use the same existing
`CurrentRoomChoice` preference and room-set folders; no separate workflow state
is introduced. Default and Studio Ghibli remain the existing room sets, alongside
four new sets arranged for particular tasks. The menu offers:

- **2D Tradigital Mode**: drawing, timing, FX and browsing.
- **Cutout Digital Mode**: drawing, FX/rigging, cutout animation, timing and export preparation.
- **Stop-Motion Mode**: capture, camera, audio/lip sync, lighting, motion path and browsing.
- **Character Mode**: cutout rooms with Groundzero's Rig Library, Poses and Drawing Substitution panels in a dedicated Character room.
- **Default Rooms** and **Studio Ghibli Rooms**: return to the existing shipped profiles.

Preferences labels the selector **Room Sets / Workflows**, without a restart
asterisk.
Both controls use the same deferred switch and update immediately. An open
Preferences selector follows menu changes; an unavailable or rejected switch
keeps the loaded set selected and does not save a different startup choice.

The active choice has a checkmark. Commands can be assigned shortcuts or added
to command bars using the existing command registry. Stop-Motion Mode requires
a build with the existing Stop Motion controls. Stop playback before switching;
an active capture live view is stopped before its panels are retired.

Each room has the Workflow menu, including rooms whose saved menu predates this
feature. Existing room menu overrides take precedence; new profiles inherit
Groundzero's standard menu. The existing room loader and settings continue to
remember the profile and room arrangements. No personal-profile management or
migration is added.

Switching is deferred until the invoking menu or panel has returned. Room/tab
callbacks are blocked while rooms and their menus are replaced; the old stack
is hidden throughout teardown. Viewer references are cleared when their viewer
is destroyed, toolbar separators are owned per toolbar, and the Flipbook pool
is reloaded before new panels process events.
Lazy and eager room loading use the same switching path.

Workspace selection does not assign a scene role, convert a scene, restructure
cells, or change animation timing. This port adds no storyboard/animatic system,
companion metadata format, scene-save persistence changes, or editing/undo commands.
Character Mode uses Groundzero's existing character tools; it does not import
Ztoryc's separate character or production-management systems.

## Provenance

Adapted from Ztoryc's non-storyboard Workflow commands, switching safeguards and
shipped room templates at
[`53cf5860c8aa64f53908b5fbea608f6be795fd21`](https://github.com/matitanimata/ztoryc/tree/53cf5860c8aa64f53908b5fbea608f6be795fd21).
The original copyright and BSD-3-Clause notices are retained in
[`stuff/doc/Ztoryc-LICENSE.txt`](../stuff/doc/Ztoryc-LICENSE.txt).
Templates are adapted to Groundzero's registered panels. The duplicate capture
room is omitted from the 2D profile; export-preparation rooms use Scene Check
instead of Ztoryc's unsupported Output Settings panel.

AI-assisted implementation: OpenAI Codex.

## Validation

The existing native UI CI fixture exercises both lazy and eager room loading,
all available presets and their room tabs, menu/action alignment and checkmarks,
deferred teardown, outgoing panel/menu destruction, repeated Flipbook use,
no-op selection, queued requests and bidirectional Preferences/menu updates. It also checks that scene identity,
frame/column navigation, dirty state and undo history remain unchanged.

Interactive acceptance: switch profiles with a scene open, reopen floating
panels, customize a room menu and switch away/back, restart in each profile,
and test Stop Motion live view on supported hardware.
