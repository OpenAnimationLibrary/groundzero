# Scene, undo, and clipboard memory ownership audit

This review traces allocation, ownership, and release in the current source.
It does not establish a measured RAM slope, a leak-detector result, or coverage
of every application workflow. No local builds or tests were run. The proposed
repeat-cycle measurements are separate from source-confirmed ownership defects.

## F35: sub-xsheet navigation changes which sheet teardown releases

`ChildStack` owns the reference acquired when its constructor allocates the root
`TXsheet` and calls `addRef()` (`toonzlib/childstack.cpp:14`). Entering a child
stores the previous sheet as the raw `AncestorNode::m_xsheet` pointer and the
child level as a smart pointer, then assigns the child's sheet to `m_xsheet`
without acquiring a sheet reference (`openChild`, line 37).

Before the repair, the destructor calls `m_xsheet->release()` and then deletes the
ancestor records (line 21). When navigation is still inside a child, that
releases the borrowed child sheet rather than the owned root. The root's
reference is lost with the ancestor records. The root can retain its columns,
FX, child levels, and other scene state; the child level still expects to own
the sheet reference that teardown consumed. This combines retained ownership
with a potential later invalid release. `clear()` has the corresponding problem:
it clears the current child and discards the only route back to the root.

This path is reachable from scene replacement: `IoCmd::newSceneImpl()` creates
a replacement scene and calls `TSceneHandle::setScene()` without first closing
the old child navigation. `TApp::onSceneSwitched()` replaces the non-owning
`TXsheetHandle` pointer. Delayed retirement eventually destroys the old scene;
delaying destruction does not repair which sheet `ChildStack` releases.

The prepared repair restores the owned root through `getTopXsheet()` before
discarding navigation records, and clears/releases that root. It must preserve
child-level ownership and ordinary open/close behavior. No public navigation
policy needs to change. Verify teardown with external observer references and
level destruction sentinels, including two nested children, explicit close,
clear while nested, and repeated scene destruction. The new
`toonzlib/tests/childstack_lifetime_test.cpp` exercises 32 cycles of those cases
with observer references and `QPointer` level destruction sentinels. Independent
source review and static formatting/patch checks passed. The `childstack_lifetime`
CTest passed on all three platforms at merge f8a1680; baseline/mutation evidence remains
pending. See the [qualification record](audit_ci_2026_11_12.md). The legacy
public `setXsheet()` escape hatch has no production caller in the searched tree
and is outside the fixture's scope.

## F44: drawing snapshots outlive the last clipboard and undo owner

Before the repair, `DrawingData::setLevelFrames()` clones each copied frame into
`TImageCache` under a unique `IMAGECACHEUNIQUEID…` key. `clone()` copies those keys
so clipboard content and undo entries can share the snapshots. Both the
`DrawingData` destructor and `releaseData()` contain disabled cleanup.
`PasteFramesUndo` owns its data with `unique_ptr`, but its call to `releaseData()`
therefore cannot remove the cached images.

Repeated copy/clipboard replacement or copy/paste/undo/history-reset cycles
within one scene can leave snapshots with no remaining logical owner. They are
not needed to preserve undo once all copies of that `DrawingData` are gone.
Removing every key in each destructor would instead break live copies.

The prepared repair adds a private shared cache-entry lease. Clones retain that
lease; `releaseData()` and destruction retire the caller's ownership, and the
last lease removes its keys. Existing public signatures and the frame-to-key map
remain intact. The existing explicit consumption when `getLevelFrames()` receives
`cloneImages=false` is preserved.

All three production `setFrames()` handoffs were traced in `filmstripcommand.cpp`:
cut, clear, and remove create temporary cached snapshots and transfer their ID
maps into clipboard or undo `DrawingData`. Their producer keys can be reused by
later operations. The repair prepares fresh cache aliases for each adoption,
preserves repeated references to the same source key, and only removes producer
keys after the new snapshot commits. Reusing an object's own snapshot map also
preserves keys still owned by a live clone. This avoids deleting an older undo
snapshot when a producer reuses its temporary key for different pixels.

Cache entries and maps are staged before replacement. A missing producer entry
raises an actionable error without replacing the prior snapshot or consuming
other producer entries. Registered staging keys are released during unwinding.
The existing `HookSet` copy/assignment implementation is not strongly exception
safe under allocation failure; this repair does not claim to resolve that
separate boundary.

The native `runDrawingCacheIntegrationTest()` fixture checks actual cache keys,
pixels, and frame identities:

- Sixteen real Filmstrip Copy/clipboard-replacement cycles and clone retention
  through clipboard clearing, followed by last-owner removal.
- Four real copy/paste, clipboard-clear, undo/redo, and history-reset cycles;
  the drawing remains editable after its temporary snapshot keys disappear.
- Eight cycles adopting a reused producer key with different pixels, releasing
  old/new generations in both orders, preserving aliases and self-transfers,
  replacing an existing snapshot, rejecting a missing source, and preserving
  the existing consuming paste behavior.

The regression invokes Filmstrip copy/paste and public snapshot adoption. The
raw cut/clear/remove command entry points are source-traced rather than each
separately invoked. The failure test proves prior snapshot/producer preservation;
it does not enumerate transient staging keys. Independent source/API review and
formatting/patch checks passed. The native runner includes the fixture; compilation
completed on all three platforms, but the preceding paste-cleanup crash prevented this
fixture from running. No runtime pass is claimed. No local builds or tests were run.

This retention has a boundary: `ImageManager::clear()` calls
`TImageCache::clearSceneImages()`, which removes these non-`$:` keys during
New/Open Scene. It is orphan cache retention during a scene session, not proof
that these same snapshots survive every scene replacement. Memory pressure may
compress or spill an orphan; that is not equivalent to releasing its now-unneeded
cache entry. The separate [memory measurement plan](audit_memory_lifetime.md)
tracks settled repeat-cycle measurements without treating RSS alone as proof.

## Ownership that is intentionally retained or already released

| Path | Source evidence and interpretation |
| --- | --- |
| Scene retirement | `TSceneHandle::setScene()` retains an old scene through a three-second timer; reader leases can extend that lifetime. The record is retired by timer destruction and released after the last lease. Growth inside that window is expected. Existing `sceneRetentionChecks()` tests reader pins and reinstatement, but does not cover nested-sheet teardown. |
| Current scene handle | `TSceneHandle` does not own arbitrary current scenes; existing tests deliberately use stack-owned scenes. Adding unconditional deletion to its destructor would violate that contract. |
| Undo history | `TUndoManager::reset()` deletes completed undo entries. Those entries may intentionally own images, palettes, levels, and cache leases until reset/eviction. Clearing their containers is insufficient if their destructors fail to release a separate allocation. |
| Level frames | `TXshSimpleLevel::~TXshSimpleLevel()` calls `clearFrames()`, which unbinds image builders, removes icons, and invalidates textures; the level palette is a smart pointer. A level retained by a live exposure/undo entry is not automatically a leak. |
| Vector conversion | The normal conversion/cancel path closes a `WA_DeleteOnClose` progress dialog and deletes `Vectorizer`; its overwrite dialog is a `unique_ptr`. Partial converted levels can remain as visible scene output. Their existence alone does not show an ownerless allocation. |
| Peak RSS | A process high-water mark cannot decrease after cleanup. Allocator reuse and OS page retention can also keep current RSS above startup while live object counts are bounded. Match measurements to object ownership and settled cleanup boundaries. |

## Existing proposals and historical reports

[Groundzero PR 133](https://github.com/OpenAnimationLibrary/groundzero/pull/133)
merged as `efe357ec2` on `edition` and is included in the tested merge f8a1680.
It replaces the macOS/Linux `TSystem::memoryShortage()` stubs with available-memory
measurements so the existing image cache can compress/spill under pressure.
The audit keeps that pressure policy separate from ownership repairs.
It addresses a different boundary from releasing allocations after their last
logical owner. F58/F59 separately prepare macOS host send-right balance and Linux
present-zero availability handling in this merged code. Their bounded query/parser fixtures
postdate head `376d2e6ce` and await CI; they do not establish pressure-policy accuracy or
application-wide memory reduction. See the [pressure review](audit_memory_lifetime.md#separate-pr-133-pressure-policy-review).

Historical GitHub reports were checked for state and follow-up evidence:

| Report | Verified follow-up and audit use |
| --- | --- |
| [#4125: convert to vectors](https://github.com/opentoonz/opentoonz/issues/4125) | Closed in November 2025 after a current-build reproduction attempt did not reproduce it. Keep repeated convert/cancel in the measurement matrix; do not claim the old leak remains. |
| [#2359: mesh rendering](https://github.com/opentoonz/opentoonz/issues/2359) | Closed in October 2025 after a Linux retest reported the problem gone. The thread distinguishes render-time growth from memory retained after stopping and links a historical Tahoma2D fix. |
| [#4890: Plastic on Windows 11](https://github.com/opentoonz/opentoonz/issues/4890) | Closed in November 2025; comments reference recent mesh changes but lack a detailed reproduction. A matched Windows workload is still needed for new claims. |
| [#5728: alleged leak](https://github.com/opentoonz/opentoonz/issues/5728) | Closed in September 2025. Its access-violation report was explicitly distinguished from evidence of a leak. Do not classify a crash as a leak solely from its title. |
| [#5089: TWAIN allocation failure](https://github.com/opentoonz/opentoonz/issues/5089) | Closed in September 2025. The report concerns a failure-path allocation and discusses whether the scanning path is active; it is not evidence for normal scene-session RAM growth. |

## Qualification still required

Use the real New/Open Scene, clipboard, edit, undo/redo, and history-reset paths
in the isolated native runner. Separate warm-up from repeated cycles, wait for
scene retirement and queued deletion, and record logical owner/cache counts
beside current and peak process memory. No exact RSS-return-to-startup threshold
is justified by this source review. Validate fixes with the corresponding old
ownership operation restored so that the targeted assertion or leak detector
fails. Current bounded ownership and cache-leak evidence is recorded in the
[November 12 qualification](audit_ci_2026_11_12.md); unexecuted workflows, the full native
run and process-memory measurements remain pending.

## Plugin settings borrowers

F61 removes redundant per-host layout allocation, and F62 owns immutable descriptor
generations across hosts, clones and actual cached settings pages. These repairs are
source-prepared after head376d2e6ce and have no runtime evidence yet. Visited generations
remain intentionally retained until cached page/viewer and other host owners retire.
R17 keeps partial-construction and shared-state qualifications open; R22 separately tracks
histogram controllers, queued widget borrowers and retained FX owners during page retirement.
