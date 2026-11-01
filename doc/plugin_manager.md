# Plugin Manager

Open **Preferences > Addons > Plugin Manager** to manage third-party OpenToonz
FX plugins. The Addons page is available on Windows, macOS, and Linux.

The manager lists `.plugin` libraries recursively inside the current stuff
directory's `plugins` folder. A library supplying several effects is enabled or
disabled as a unit. Checking **Enable at startup** enables that library for the
next launch; unchecking it blocks loading without moving or deleting files.
Current-session status and next-startup behavior are shown separately, with
loading failures available as tooltips. Refresh rescans filenames and settings
without opening plugin libraries.

**Bypass all plugins** is the same saved setting in Preferences and the manager.
It skips all third-party FX libraries at the next startup and preserves every
individual selection. Turning it off restores those selections. Groundzero and
the separate `tcomposer` renderer read the same settings. Qt platform plugins,
image-format plugins, built-in effects, and external applications are unaffected.

Changes require restarting Groundzero. Existing loaded effects continue working
in the current process. Child render processes inherit the current session's
activation policy, including when next-startup changes have been saved. A
separately launched renderer reads the saved settings. Disabled
effects are unavailable after restart; re-enable required libraries before
opening or rendering scenes that use them. Missing-effect placeholders and live
library unloading are outside this initial implementation.

## Installation

**Install File** copies a `.plugin` library into the plugin folder. For a plugin
distributed with dependencies or resources, use **Install Folder** to copy its
complete unpacked package. Files retain their layout and existing destinations
are never overwritten. Installation does not load the library. New plugins are
enabled by default unless their path is already blocked. External runtime
requirements still need to be installed according to the plugin's instructions.

Package installation rejects symbolic links and stages outside the scanned
plugin tree before publishing the complete package. Manually installed files
remain supported. There is no special meaning assigned to an `inactive` folder;
use the blocklist to disable its libraries.

## Plain-text configuration and recovery

The file is `stuff/config/plugin-blocklist.txt`, using the active stuff directory.
Paths are relative to `stuff/plugins`, with `/` separating subfolders. The file
uses UTF-8 and accepts blank lines and full-line `#` comments. Listed libraries
are disabled. The optional `@bypass-all` directive enables the global override:

```text
# Individual disabled libraries
ExperimentalBlur.plugin
OldEffects/LegacyGlow.plugin

# Remove this directive to restore individual activation settings
@bypass-all
```

A missing file means all plugins are enabled. Read errors or malformed entries
cause all FX plugins to be skipped for that startup, with the configuration
error shown in the manager. Correct the file and restart. The manager preserves
comments and entries for absent plugins when editing it, uses atomic saves, and
reports conflicting writes rather than overwriting another instance's changes.
Use **Open Configuration Folder** to locate the file for manual editing.

Launch Groundzero or `tcomposer` with **`--no-plugins`** to bypass all third-party
FX plugins for that launch without changing saved settings. This option can be
used alongside a scene filename. Child render processes inherit this temporary
bypass. It is useful when a plugin prevents Groundzero from starting.

Two builds sharing the same stuff directory also share this configuration.
