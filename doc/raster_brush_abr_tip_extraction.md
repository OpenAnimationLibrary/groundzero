# Legacy ABR sampled-tip extraction

The `Extract Sampled Tips from ABR` action extracts original sampled mask
images into the legacy tip library. It does not apply Photoshop brush presets,
dynamics or computed-tip rasterization, and it does not claim source appearance
or behavior. The native raster-brush importer remains a separate action.

## Isolation and identity

The GUI sends an explicit trusted sampled-tip operation to the same bounded
worker used by native brush import. Linux restrictions, the Windows zero-
capability AppContainer and Job, and the independently signed macOS helper with
descriptor transport remain shared. No ABR decoding occurs in the GUI process.
The response uses the existing validated, bounded native-record/raw-RGBA8
protocol, avoiding an image decoder in the receiving parent.

Each original sampled tip occurs once in file order, with its original name,
dimensions and coverage bytes. Modern descriptor aliases neither duplicate nor
rename that sampled image. Duplicate source names remain duplicate in transport;
the legacy filesystem naming policy adds a safe ordinal/collision suffix only
when saving a PNG. Names are bounded to 120 UTF-8 bytes at a complete code-point
boundary, and collision identity includes canonical Unicode spelling and case.
Every staged PNG is opened with create-if-absent semantics before encoding,
flushed and closed; an equivalent filename cannot truncate an earlier tip.
Coverage is inverted for the established legacy grayscale
PNG representation: full coverage is black, zero coverage is white. The ABR
parser's declared 16-bit interpretation remains its original high-byte mask.

`AbrParser::Result::sampledDataComplete` distinguishes structural completeness
from descriptor/pattern source-behavior warnings. Skipped, malformed, unknown or
truncated records/section boundaries cannot produce an installed subset. Valid
semantic warnings are retained without pretending that descriptor decoding
applies source dynamics. Computed records have no sampled image and are reported
separately; a computed-only file yields no extracted tips.

The operation adopts the existing worker's 64 MiB input and 256-record protocol
limits and aggregate decoded-resource budget. Oversized resources are rejected,
not silently clamped or sampled down.

## Cancellation and publication

A modal cancellable command keeps the UI responsive while a background thread
owns decoding, staging and PNG encoding. The GUI and worker communicate only
through an atomic cancellation flag; no background callback reads a GUI object.
The stack progress dialog is parentless so main-window destruction cannot delete
it. Owner destruction, application quit or an external modal-loop exit requests
cancellation and joins the worker before returning; shutdown does not show a new
summary dialog. A finished callback distinguishes completed work from thread
bookkeeping, so an ordinary finish is not mistaken for cancellation.
Cancellation during foreign decoding kills/reaps the isolated child through the
existing hard-cancellation path. Staging checks cancellation between tips and
rows. A bounded in-progress PNG encoding completes before its staging directory
is retired; it does not become a visible brush.

Each ABR file stages one complete collection beneath a reserved nonselectable
`.abr-import-` prefix, then uses the shared native atomic no-replace publication
helper. Name collisions choose another numbered collection; existing packs are
never overwritten. Failure or cancellation before publication removes the
private stage. A late cancellation cannot erase or deny a complete collection
already published. Cancelling a multi-file selection preserves collections
completed earlier in that selection while discarding the current partial stage.

## Validation status

Regressions are implemented for portable Unicode filename bounds/collision
identity, Unicode/duplicate source names, descriptor aliases,
exact mask dimensions/values, retained-mask allocation bounds, malformed second
records, computed-only/mixed files, isolated response roundtrips and cancellation
at the prepared worker boundary. Final builds, automated tests and installed-
application validation remain deferred until the implementation plan is fulfilled.
No source parity, physical-tablet or complete RCP1 qualification follows from
this extraction path.
