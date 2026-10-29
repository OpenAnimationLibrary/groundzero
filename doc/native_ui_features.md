# Native interface features

These features use the existing application commands, rooms, panels and
preferences. They do not require a separate template application or manual
configuration-file editing.

**Command Palette.** Open Windows > Command Palette and type part of a command
name, identifier or category. The results show its current shortcut and category.
Use the arrow keys to select a result and Enter to run it, or activate a result
with the mouse. Escape returns to the previous editing context. Unavailable
commands remain unavailable, and commands retain their ordinary confirmation
dialogs, checked state and undo behavior. The panel can be docked or floated.

**Synthwave appearance.** Synthwave adds a purple and cyan appearance alongside
the existing themes. It uses the native stylesheet and icon system. Select it
from the Theme menu or Preferences > Interface > Theme. Additional user styles
continue to take precedence over the selected theme.

**Quick theme selection.** Each room menu provides a Theme menu, including rooms
with saved custom menus. Choices follow the installed theme directories; the
menu and an open Preferences dialog stay synchronized. Theme changes refresh
open windows and floating panels. Reload QSS uses the same application path.
Missing stylesheets, incomplete style rules and unreadable local images are
reported before a new selection is saved. An invalid additional stylesheet
remains in the editor for correction and does not replace the applied style.

**Message Center history.** Message Center retains timestamped entries in
arrival order and keeps its existing severity filters. Its history is bounded,
with a default of 1,000 entries. Preferences > Diagnostics changes the entry
limit. Old entries are discarded when the configured limits are reached, with a
count displayed in the view. Reading older messages does not force scrolling
back to the newest entry. Clear removes the in-memory history; it does not
delete saved diagnostic files.

**Optional local diagnostic files.** Preferences > Diagnostics contains Save
local diagnostic logs, minimum severity, status, Open Log Folder and Delete
Saved Logs. File logging starts disabled. The saved checkbox controls whether
it is enabled on subsequent application starts. Retention defaults to one
5 MiB file and three backups, for 20 MiB total. Logs stay on the local computer.
The application records diagnostics through a bounded queue and reports dropped
entries and write failures in the settings page. Retry is available after a
folder or ownership problem is resolved. File logging and Message Center use
separate retention; turning off file logging leaves Message Center available.

**Widget Inspector.** Open Windows > Widget Inspector, choose Pick Widget, then
select an application widget. The selection is consumed so it does not activate
the selected control or draw on the canvas. The read-only snapshot shows the
widget class, object identifier, geometry, visibility, enabled state and panel
type where available. Refresh updates the snapshot; Clear releases it. Escape
or right-click cancels picking. Closing or hiding the panel also cancels picking.
The inspector does not edit properties, retain widget ownership, save its target
or automatically copy inspected metadata into diagnostic logs.

**Interface language.** Preferences > Interface > Language selects an installed
catalog set during the current session. The manager validates the required
catalogs before replacing the active language and saving the choice. Missing
translations can display their English source text. User-authored names and
document content remain unchanged. The integration retains existing controls
and command identities while refreshing their presentation; it does not rebuild
the workspace to change its language.

**Validation status.** The implementation includes automated tests for command
dispatch, theme resources and synchronization, diagnostic retention, inspector
input handling, catalog replacement and presentation updates. Builds and tests
run in CI. Source inspection alone does not establish a passing build or desktop
compatibility. Full language-owner coverage and representative packaged-app
checks remain acceptance requirements until their results are recorded; this
guide does not claim that validation has already completed.
