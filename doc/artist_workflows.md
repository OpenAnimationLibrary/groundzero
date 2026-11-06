# Artist workflow guide

These guides describe the Function Editor, vector and Scene Cast workflows in
this branch. For release and platform readiness, use the
[acceptance ledger](artist_workflow_acceptance.md) and the checks on the relevant
pull request. A source guide does not imply that every platform has qualified.

## Find the right workflow

| Task | Start here |
| --- | --- |
| Find rig channels, fit selected keys, edit several values or retime a selection | [Function Editor tools](function_editor_usability.md) |
| Compare animation, save channel sets, reuse motion curves or inspect dependencies | [Function Editor tools](function_editor_usability.md) |
| Read stroke opacity, manage width profiles or make an animated gradient independent | [Vector selection, width and gradients](vector_usability.md) |
| Align artwork across selected drawings or a whole level | [Vector alignment](vector_alignment.md) |
| Review fill changes before reshaping a stroke | [Vector fill review](vector_fill_review.md) |
| Turn strokes into editable outlines, including loops and holes | [Contour Editor](contour_editor.md) |
| Paint inside filled regions or the current selection | [Vector brush painting](vector_brush_painting.md) |
| Preview stroke correspondence and choose how unmatched strokes appear or disappear | [Morphing](morphing.md) |
| Vary opacity along a stroke and preserve it through edits and export | [Stroke opacity envelopes](stroke_opacity_envelopes.md) |
| Search assets, navigate their exposures, inspect drawings and audio, or relink files | [Scene Cast workflows](scene_cast_workflows.md) |
| Organize collections, inspect duplicate assets, or prepare a project to move | [Scene Cast workflows](scene_cast_workflows.md) |
| Inspect recovery status or understand timing, pose-library and export choices | [Animation workflow behavior](animation_workflow_acceptance.md) |
| Set a media-cache budget or remove unused cached frames | [Media cache management](artist_workload_and_media_cache.md#media-cache-workflow-60) |

## Review an edit before applying it

In the Function Editor, select keys in the graph or spreadsheet, open **Edit
Keys**, choose the operation, then use **Preview**. Each **Apply** that changes
keys commits one undo step. Closing the dialog discards its uncommitted preview;
an earlier Apply remains an edit and can be undone normally. Target-range retiming accepts exact
first and last frames, including fractional values.

Vector operations use their selected drawing or level scope. Check that scope
before accepting the review. Groups, painted regions and locked resources can
affect which changes are valid; the detailed guides explain those boundaries.
Changing the reviewed scene, drawing or selection can cancel a pending operation.
Reopen the command after changing context so it reviews the intended artwork.

Scene Cast searches and filters help select assets without changing exposures.
Use **Where Used** to locate actual instances, including nested child sheets.
Review relink candidates and duplicate groups before committing an asset edit.
Undo restores the document changes; it does not delete a published local-copy
directory or recover a manually removed source file.

## Know what is saved

- Animation keys, vector artwork and asset metadata belong to the scene or its
  level resources. Save those resources along with the scene.
- Named channel sets, motion presets and width-profile libraries are local
  workspace resources. Use each workflow's library controls separately from
  saving animation. Channel-set membership uses identities stored in the scene;
  older builds may discard those optional identities when saving.
- Scene Cast collections store asset identities in the scene. Renaming an asset
  does not rename or replace its identity, and relinking keeps that identity.
- Media cleanup removes regenerable cached frames. It leaves source media
  unchanged and has no document undo. Open readers can keep usage above the
  chosen cache budget.

For portability, use the Cast collection report and the existing **Collect
Assets** workflow, then verify the saved scene at its relocated destination.
The Scene Check compatibility inventory reports known feature requirements;
it does not certify an arbitrary older application version. Export dialogs
explain known omissions and available alternatives before writing output.

The benchmark reports measure synthetic application work on the recorded
hardware and renderer. They do not measure physical tablet latency or establish
real-time playback performance on an artist's workstation.
