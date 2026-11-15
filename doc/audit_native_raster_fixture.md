# External raster retirement fixture

Run `43660089325`, tested merge `ebe1a05083f3e33ecdb1ab15fb4ccaafd2fef240`:
the original macOS process passed the indexed reset and all six native raster
targets, then failed the external-new-level fixture's exact red-pixel assertion.
Its cleanup returned normally and the process exited with status 1.

The fixture configured new levels at 96 DPI, while external clipboard payloads
use 120 DPI in `TCellSelection::pasteCells`. `ToonzScene::createNewLevel` initializes
resolution and DPI from preferences; the paste helper consequently scaled the
source by 96/120. A fully red destination was not a valid expectation for that
setup. The retirement fixture now scopes its preferences to 16×16 at 120 DPI,
checks the resulting DPI, and retains exact dimensions, every red pixel, selected
column, sound-column caption, drawing caption, and real New Scene assertions.
Pixel failures report their coordinates and RGBA values. Existing preference,
tool, and clipboard restoration also runs when an assertion throws.

This is a fixture correction, not evidence of a product rendering defect. The
current original Linux `-11` remains unresolved. The corrected fixture has only
been reviewed statically; CI execution and mutation qualification remain pending.
