# Filmstrip Swing undo audit

This follow-up addresses filmstrip Swing on the prepared Groundzero undo audit.
The source trace confirms a P1 artwork-identity defect. A patch and behavioral
regressions are prepared; compilation and runtime results remain unverified.

## Defects and changes

`performSwing` calls `makeSpaceForFids`, which can move existing drawings to
new frame IDs. The former `FilmstripSwingUndo` only removed appended drawings.
It left those original drawings shifted and, when synchronization was enabled,
left their Xsheet references shifted too.

Swing now retains the inverse frame-ID order returned by `makeSpaceForFids`.
Undo removes appended drawings, restores synchronized exposure IDs when that
mode was enabled for the most recent application, and renumbers surviving
drawings back to their original IDs. The mapping follows drawing identity even when a lettered frame
crosses a numeric frame in the sorted level strip.

The previous code also cloned every selected drawing but allocated only
`selection size - 1` insertion IDs. Its equality assertion therefore failed in
debug builds for every selection containing at least two drawings. The clone
loop now produces exactly the images consumed by the insertion loop. Empty and
single-drawing selections return without creating undo history or dirtying the
scene.

## Compatibility boundary

The filmstrip's existing release-build return sequence is preserved. For a
selection `1, 2, 3`, it appends copies of `3, 2`. Xsheet cell Swing currently
appends `2, 1` instead. Aligning these endpoint conventions would change visible
animation timing and is a separate workflow decision; this patch does not change
Xsheet Swing or add a preference.

Initial insertion locations, sparse selection handling, cloned image content,
and the enabled/disabled Xsheet synchronization modes retain their behavior.
Subsequence and read-only level guards remain in place.

## Baseline and patched source traces

The fixture uses original drawing IDs `1, 1a, 1b, 2, 2a, 3, 3a, 5, 7`, each with
a distinct image marker. Each original has an Xsheet exposure separated by an
empty cell. These traces describe the source behavior, not executed test results.

- Selection `1, 2` copies drawing `2` into `3` and moves original `3` to `4`.
  Former undo deletes `3` but leaves the original at `4`. Patched undo restores
  the original drawing and its applicable exposure to `3`.
- Selection `1, 1a` copies `1a` into `2a` and moves originals `2a` to `3a` and
  `3a` to `4a`. Original `3` stays at `3`. Former undo leaves both lettered
  originals shifted. Patched undo restores both without swapping numeric `3`.
- Selection `1, 2, 3a` copies `3a, 2` into `4a, 5a`. Existing numeric `5` and
  `7` stay put. Former release-build undo removes the inserted drawings, while
  the debug count assertion fails first. Patched behavior preserves the same
  return sequence and reverses it in both build configurations.
- Selection `1, 3, 5` copies `5, 3` into `6, 7` and moves original `7` to `8`.
  Former undo leaves that original at `8`. Patched undo restores the drawing
  and its applicable exposure to `7`.

## Regression coverage and limits

The new cases were written before the production fix in the existing vector
workflow fixture. They check initial output, two undo/redo cycles, final undo,
all original image markers, drawing counts, unchanged input selections, exposure
IDs, and empty Xsheet cells. Every selection runs with synchronization disabled
and enabled, toggling the preference before undo so undo must reverse the mode
actually applied, then redoing with that new preference. Empty and single-drawing selections
also have no-op assertions.

The pre-fix source fails the clone-count assertion in debug builds. In release
builds, cases with shifted originals fail the restored-image/ID assertions.
These are source-derived failure expectations; no local test or mutation run was
performed. CI must compile and execute the existing vector workflow fixture.

Changed C++ regions were formatted, and incremental whitespace and repository
privacy checks passed. This is not release clearance or upstream approval.

Prepared with Codex assistance. Design summary: preserve current filmstrip Swing
output while restoring original drawing and exposure identity across undo/redo;
exercise numeric, lettered, sparse, and colliding later-frame cases.
