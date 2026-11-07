# Upstream bug inventory and historical audit reconciliation

Snapshot date: 2026-11-07. Source review baseline:
`2e52fb78b70915925393290f32dc09a95a3927ec` on Groundzero's `edition` branch.

This review inventories all **132 open issues carrying the upstream bug label** in the
captured GitHub response. It connects selected reports to Groundzero source and
rechecks the 25 findings from the
[October 29 audit](codebase_audit_2026_10_29.md).
An open upstream issue is a report, not proof that current Groundzero reproduces it.
No application builds, tests, or artist reproductions ran in this lane; repository
policy requires those in CI. No upstream issues were closed or changed.

## Evidence and limits

The issue snapshot came from both pages of the
[upstream open-bug API](https://api.github.com/repos/opentoonz/opentoonz/issues?state=open&labels=bug&per_page=100).
Only issue numbers and public report titles are reproduced below; contributor details,
attachments, machine information, and private data are excluded. Counts describe this
snapshot and can change as GitHub issues are updated.

The inventory uses these statuses:

| Code | Meaning |
| --- | --- |
| R | Report catalogued; not reproduced or confirmed in current Groundzero. |
| C | Corresponding defective source path confirmed at the baseline; runtime reproduction pending. |
| S | Selected source path reviewed; reported outcome or exploitability remains unproven. |
| M | Current source differs from the alleged defect; targeted regression still needed. |

There are **124 R, 3 C, 4 S, and 1 M** records. These are evidence states, not closure
decisions. Severity recommendations below are engineering priorities based on possible
artwork loss, incorrect output, or crashes; they are not vulnerability ratings.

The Groundzero tracker separately had four open issues:
[65](https://github.com/OpenAnimationLibrary/groundzero/issues/65) concerns storyboard
integration conflicts;
[53](https://github.com/OpenAnimationLibrary/groundzero/issues/53) collects feature requests;
[52](https://github.com/OpenAnimationLibrary/groundzero/issues/52) links the upstream bug list;
[8](https://github.com/OpenAnimationLibrary/groundzero/issues/8) concerns contributions.
They are not four independent, reproduced product defects. Open proposals
[141](https://github.com/OpenAnimationLibrary/groundzero/pull/141) and
[133](https://github.com/OpenAnimationLibrary/groundzero/pull/133) already cover GL/brush
ownership and memory-shortage work respectively. Review them before duplicating fixes;
an open proposal is not an integrated remedy.

## Prioritized source triage

### P1: batch movie chunks overwrite the same output

Upstream [4629][u4629] describes batch MP4 rendering leaving only the final chunk.
At the baseline, `toonz/sources/toonz/batches.cpp:299` defines a private
`isMovieType()` accepting only MOV, AVI, and 3GP. `addRenderTask()` at line 376
therefore applies the normal chunk preference to MP4, WebM, and APNG. The resulting
`TFarmTaskGroup` receives one shared output path at line 390.

The shared helper in `toonz/sources/include/tlevel_io.h:247` already recognizes all
six formats. The task editor uses that helper, explaining why editing task output
can change chunk behavior. This is a source-confirmed inconsistency and a credible
cause of incomplete movies; it is not a measured reproduction of every symptom in
the issue. Use the shared format classification, then verify one movie task spans
the requested range while image sequences still split. Check existing task-list
loading separately, since correcting new task creation does not repair saved lists.

The task reliability lane was notified to prepare the fix and regression.

### P1: frame renumbering and grouped undo

Upstream [2426][u2426] matches the filmstrip undo path that restores drawings by
their sorted positions after `makeSpaceForFids()`. A frame such as `2a` can move
past `3`, changing order and restoring the wrong image. Duplicate drawing cache
keys also used only the numeric frame component. The undo lane confirmed this at
`toonz/sources/toonz/filmstripcommand.cpp` and prepared explicit inverse frame
mappings and cache identities scoped to each undo entry.

Upstream [1840][u1840] matches `UngroupUndo::undo()` in
`toonz/sources/tnztools/imagegrouping.cpp`: regrouping the entire selected range
merges originally separate groups. Preserve individual group ranges, including
nested groups, and exercise undo and redo. Candidate fixes and their regression
fixtures still need CI execution.

### P1 investigation: scene-controlled resource destinations

Upstream [7195][u7195] identifies alias paths containing parent-directory segments.
Current `ToonzScene::decodeFilePath()` still joins alias directories and tails
without canonical containment checks at
`toonz/sources/toonzlib/toonzscene.cpp:1464`. This establishes the path-resolution
behavior, not an end-to-end exploit against the reviewed build.

Groundzero intentionally supports resources outside a project and configurable
project folders. Rejecting all paths outside a project would break those workflows.
Before changing policy, trace the exact save/rename/collect operation, ownership of
the destination, symlinks, existing-file prompts, and a malicious fixture. Decide
where untrusted scene input requires confirmation or write restrictions while
preserving explicitly chosen external resource paths. No standalone code execution,
CVSS score, or arbitrary-file overwrite was independently demonstrated here.

### P1 reproduction queue: lost edits and invalid saves

| Issue | Reason for early reproduction | Required distinction |
| --- | --- | --- |
| [6923][u6923] | Alt-drag reportedly loses mesh keys beyond undo recovery. | Review selected and unselected plastic keys, overwrite, insert, undo, and redo. |
| [4530][u4530] | Creating a suffixed raster level reportedly removes existing images. | Test underscore recognition, standard/custom frame formats, existing-file guard, and backup state. |
| [6156][u6156] | TLV merge reportedly damages merge and later editing behavior. | Preserve a minimal affected TLV fixture and compare palette/index/transparency data. |
| [5313][u5313] | Rotation expressions reportedly prevent saving. | Reproduce with a minimal project and compare rotation with other expression channels. |
| [3772][u3772], [2430][u2430] | Column undo and macro persistence reportedly alter FX connections. | Compare ordered and dynamic port topology before/after undo and save/load. |
| [4286][u4286], [3486][u3486], [3070][u3070] | Explode, insert, and range undo reportedly change unrelated animation state. | Verify parent camera, function curves, frame identities, and redo. |
| [3524][u3524], [3294][u3294], [2926][u2926] | Project expansion or file opening reportedly crashes/hangs. | Test current build, empty project folders, startup versus in-app open, and native graphics. |

Source tracing for 6923 found that `KeyframeMover::setKeyframes()`
(`toonz/sources/toonz/keyframemover.cpp:46`) removes and restores all keys on touched
objects for overwrite/insert moves. The current `TStageObject::Keyframe` includes
plastic keyframe data, so a simplistic claim that the snapshot omits all plastic
keys is unsupported. The exact lost-key case remains for a fixture.

Source tracing for 4530 found that `LevelCreatePopup::apply()` checks the resolved
destination before creation (`levelcreatepopup.cpp:571`), while
`ToonzScene::getDefaultLevelPath()` builds a filename and then applies the frame
template (`toonzscene.cpp:1604`). Frame parsing preferences can affect that name.
The destructive outcome has not been established through the current guard.

### P2: navigation, input, output fidelity, and platform behavior

[5190][u5190] includes exposure/frame shifts during vector drawing undo; the
commented navigation call in `UndoPencil` alone does not establish its cause.
A narrower source-confirmed variant is now F31: with Brush Break enabled and Auto Group
disabled, every split undo record claimed frame creation, but only the first retained
renumber metadata. Reverse undo removed the frame before original IDs/exposures could be
restored. The prepared fix gives creation ownership to the first record. Actual gesture
fixtures cover blank and held exposures with auto-create/stretch/hold/renumber and three
undo/redo cycles. They have not run, and the exact reporter settings/video, new-level
membership and lettered-ID variants remain unverified. Navigation is unchanged; the
inventory retains S and R14 remains open.

[2445][u2445] remains an unverified destination/type policy report. F32/F33 address two
narrower defects in `TCellSelection::pasteCells`: native `RasterImageData` must take precedence
over its external-image preview, and external-image Cancel must preserve redo. The prepared
MI_Paste fixture checks six indexed/full-color targets plus Cancel/history, not mixed-column,
gap-lookup, keyboard activation, cross-process MIME or all conversion behavior. New-level
size/DPI still follows preferences. The existing
[OT-Dev PR280](https://github.com/OpenAnimationLibrary/opentoonz-dev/pull/280) proposes broader
destination/PasteAs behavior; it was not imported here. R13 and this report remain open.

For [6972][u6972], the file-safety lane found that active BMP decoding already
differs from the older, unbuilt implementation implicated by the report. Do not
patch the dead implementation and claim an application fix. Keep grayscale-pixel
and short-name filename cases in the runtime queue.

Remaining drawing, effects, display, device, packaging, and platform reports below
need representative fixtures or hardware. Visual differences require pixel/output
comparison under defined color and alpha settings; they cannot be resolved from
titles alone. Audio and task problems require process-lifetime and cancellation
evidence. Antivirus reports require the exact signed/distributed artifact rather
than a source-only conclusion.

## Reconciliation of the October 29 audit

The older audit reviewed `6ba3cc31da7022472e0d6a982f03c1348de69b1f`, not this
baseline. All 25 entries were revisited in current source. **Present mitigation**
means the original defective operation has a corresponding change in source;
it does not mean regression execution, supported-platform qualification, or formal
closure. **Partial** means a narrower fix exists while part of the original
validation or design recommendation remains.

Paths below are relative to `toonz/sources/` unless prefixed with `.github/`.

| ID | Current disposition | Current source evidence and remaining qualification |
| --- | --- | --- |
| A01 | Present mitigation | `toonz/iocommand.cpp:2210` clears scene dirty state only for a full save. Verify dirty and initially clean parents after child export. |
| A02 | Present mitigation | `toonzlib/txshsimplelevel.cpp:1819` writes palette to a private temporary directory, checks the stream, then replaces. Dirty flags clear later; `common/tstream/tstream.cpp:616` flushes during status checking. Failure injection remains required. |
| A03 | Present mitigation | `toonzlib/toonzscene.cpp:749` restores path, untitled, and standalone state unless a full save commits. Project-manager side effects and all alias workflows need qualification. |
| A04 | Present mitigation | Writer/reader use `TStreamLimits::validCompressedSizes()`; `toonzscene.cpp:198` validates the temporary file before replacement. Boundary and I/O failures need CI. |
| A05 | Present mitigation | `toonz/autosaverecovery.cpp:638` retains the changed state when snapshot resources are incomplete. Validate retry without another edit. |
| A06 | Partial | `toonz/subscenecommand.cpp:1166` stores actual matched coordinates. Redo at line 1031 still ignores `openChild()` failure; sheet identity and failed-reopen handling need review. |
| A07 | Present mitigation | `toonz/cadmiumvector.cpp:61` includes cap/join, miter, pattern, and opacity in the fingerprint. Validate pending proposal versus each native edit. |
| A08 | Present mitigation | `common/tcache/timagecache.cpp:836`, 923, and 1041 erase via `getPointer()`. Allocation-address reuse and compression still need native stress coverage. |
| A09 | Present mitigation | `image/ffmpeg/tiio_ffmpeg.cpp:230` and 236 lock intermediate cleanup/bookkeeping; conversion dimensions are local. This does not prove whole-export race freedom; shared input mutation is a separate current finding. |
| A10 | Present mitigation | `tiio_ffmpeg.cpp:822` hashes canonical path plus source content, uses versioned metadata/manifests, and holds a cache lease. Replace-during-read and interrupted extraction remain regression cases. |
| A11 | Present mitigation | `toonzlib/movierenderer.cpp:736` fails actual frame IDs through the render queue and supplies terminal completion state. Test early/middle/late failure and nonzero ranges. |
| A12 | Present mitigation | `movierenderer.cpp:761` explicitly closes writers and records finalization failure; canceled/failed work aborts. Verify disk-full, encoder failure, and old destination preservation. |
| A13 | Present mitigation | `common/tsystem/tbigmemorymanager.cpp:249` uses checked byte counts; `common/traster/traster.cpp:41` throws on allocation failure. Audit debug counters separately and run failure injection. |
| A14 | Present mitigation | `common/psdlib/psd.cpp:697` expands bitmap bits; line 766 retains two-channel 16-bit alpha. Compare decoded samples, crops, and shrinking. |
| A15 | Present mitigation | PSD parser is stack-owned, PSD reader is a unique owner, `psd.cpp:97` frees layer data, MP4 uses unique FFmpeg owners, and renderer capability probe is RAII-owned. Leak/handle qualification remains. |
| A16 | Present mitigation | `toonz/cadmiumpopup.cpp:534` accounts admission and eviction, including preview/session costs. Mandatory recovery data can exceed budget rather than be discarded; measure bounded heavy caches and metadata growth separately. |
| A17 | Present mitigation | `toonz/filmstrip.cpp:1744` and 2509 compute used levels once per rebuild. No latency improvement was measured in this audit. |
| A18 | Present mitigation | `toonz/levelselection.h:13` hashes `TXshLevelP` by pointer identity for Qt 5 and 6. Verify semantics and scaling in CI. |
| A19 | Partial | PDF/LUT flows use `toonz/processprogress.h` with a modal event loop and cancel. This addresses the unresponsive wait; it is not a fully asynchronous job design. Startup and termination waits remain. |
| A20 | Present mitigation | `toonz/cadmiumpopupui.cpp:397` routes overlay changes to `updateReferencePreview()`; line 678 changes only the reference image. Verify no analysis reruns and unchanged output. |
| V01 | Partial | `toonzlib/tests/scenebench_test.cpp:147` makes exceptions fail; line 227 compares drawings and structure counts. Keyframe values, FX parameters/connections, and nested-sheet canonical equality are not fully compared by `SceneStats`. |
| V02 | Present mitigation | `stdfx/tests/fxrender_test.cpp:275` treats missing references as failures; explicit generation is separate. Baseline full-suite workflow no longer silently commits candidate images. |
| V03 | Partial | Routine platform jobs now enable reliability, stream, scene, FX, PSD, EXR, and FFmpeg suites and run native UI checks. Full optional matrices remain separate; see the new CI topology report for consolidated coverage. |
| V04 | Present mitigation | `toonzlib/tests/compat_regression.cpp:696` fails without GL when rendering is required; baseline `.github/workflows/tests_linux.yml:283` supplies that flag for both builds. Verify rendered manifest completeness. |
| V05 | Partial | `.github/workflows/workflow_macos.yml:143` enables regression targets and runs native checks. The runner remains Intel; native Apple Silicon, tablets, DPI, audio, and long sessions remain unqualified. |

This is a mitigation register, not a claim that the historical audit is closed.
Existing test names and code assertions are evidence of intended coverage only.
Required closure evidence is a passing targeted regression at the exact candidate
revision, with a failing baseline or failure injection where practical.

## Complete upstream inventory

Every row links to its original report. Area assignment is for triage ownership;
many reports cross subsystem boundaries. R is intentionally retained until evidence
supports a stronger state.

### Persistence and media (19)

| Issue | Report | State |
| --- | --- | --- |
| [7195][u7195] | Scene-file path traversal in ToonzScene::decodeFilePath allows writes outside the project folder | S |
| [6972][u6972] | Invalid 8bit Grayscale and 8+3 (DOS compatible) BMP filenames | M |
| [6884][u6884] | XDTS doesn't populate tracks inside sub-folders | R |
| [6156][u6156] | Some frames have broken editing (TLV Merge) | R |
| [5313][u5313] | Can't save scene after entering any Expression into Rotation property of a column | R |
| [4690][u4690] | Import scene as sub-xhseet creates an own pallette for the imported content | R |
| [4530][u4530] | Creating a level named 'foo_1' removes images files from level named 'foo' without warning | S |
| [4451][u4451] | wrong project pallete | R |
| [4085][u4085] | Images added to the sequence don't appear in the Level Strip after applying Reload Level. | R |
| [3525][u3525] | OpenToonz Incorrectly assign relative path on imported files | R |
| [3524][u3524] | Crash occur when assign $scenepath variable in Project folders | R |
| [3279][u3279] | Multiple styles are created when merging levels. | R |
| [3091][u3091] | Save All and Save All Levels commands don't always create level backups | R |
| [2926][u2926] | Opening a scene (.tnz) file from the File Explorer causes OT to hang on launch [Windows 10] | R |
| [2637][u2637] | Subsampling option resizes mp4 levels on the viewer | R |
| [2430][u2430] | Issue saving/loading macroFx input ports | R |
| [1992][u1992] | Symlink loading issues | R |
| [356][u356] | (PSD) UTF-8 encoded metadata cannot be loaded correctly | R |
| [119][u119] | (SVG) Missing fills / Fill tool not working properly | R |

### Undo and timeline (16)

| Issue | Report | State |
| --- | --- | --- |
| [6923][u6923] | Mesh Key Deletion and Undo Bug | S |
| [5190][u5190] | (Undo / History) Undo from vector level fails to go to previous frame | S |
| [4518][u4518] | Shift+dragging last drawing of an exposed level, don't work. | R |
| [4286][u4286] | Exploding subxsheet reverts parent Camera to Table | R |
| [4092][u4092] | An "Expose Loaded Levels in Xsheet" option bug or feature? | R |
| [3923][u3923] | Changing interpolation type with a shortcut doesn't contextually adapt to function editor | R |
| [3921][u3921] | Shift Keys Up/Down with contiguous keys | R |
| [3772][u3772] | Undo of deleting column fails to reproduce fx/stage schematic connection | R |
| [3486][u3486] | After "Insert frame", frames go out of sync with values of Function/Transparency | R |
| [3408][u3408] | Bug with displaying drawing created by "Create Blanc Drawing" command + "inbetweens" in deformed column. | R |
| [3092][u3092] | Removing key doesn't reset original size value for Z position | R |
| [3070][u3070] | Vector Brush Range undo issue with creating new frames | R |
| [2445][u2445] | New level created by pasting does not match level type with copied image | R |
| [2426][u2426] | Undo Failure of Duplicate Drawing Command on Number+Letter Frames | C |
| [2297][u2297] | Pasting doesn't paste as a selection, ready to edit | R |
| [1840][u1840] | (Undo / History) Undoing ungroup of several groups groups all into one | C |

### Drawing and rigging (31)

| Issue | Report | State |
| --- | --- | --- |
| [6850][u6850] | Vector brush thickness depends on DPI | R |
| [5878][u5878] | Vector Area lost after moving strokes across a stroke group | R |
| [5661][u5661] | Skeleton Tool Not Working After Flipping | R |
| [5365][u5365] | Selection tool: Shift+Drag does not inhibit snapped dimension's value changing | R |
| [5364][u5364] | Selection tool: Transform's X/Y coordinates scaled incorrectly | R |
| [5339][u5339] | Angular, jagged, choppy textured Raster brush issue | R |
| [4476][u4476] | Pen stroke and geometry Glitch in Opentoonz | R |
| [4295][u4295] | Control Point Editor Tool. Incorrect behaviour after using Tape Tool | R |
| [4061][u4061] | control point editor pinches off new points when dragging other points | R |
| [3870][u3870] | Assert fault using plastic tool with sub-xsheet | R |
| [3852][u3852] | Weird line glitches and lag when doing fast lines | R |
| [3851][u3851] | Vector level doesn't work with number pad | R |
| [3567][u3567] | Convert to vector : non-black color line recognition issue | R |
| [3511][u3511] | Opentoonz Pen Display Brush and erasor Tool Issue | R |
| [3441][u3441] | Raster strokes pixelated after zoom | R |
| [3280][u3280] | Toonz Raster Fill bug while holding Shift key. | R |
| [3255][u3255] | Pinch tool endpoint issue | R |
| [3180][u3180] | color fill may fail if some unrelated strokes are grouped | R |
| [3158][u3158] | Tools are not usable at certain times when they should be usable | R |
| [3115][u3115] | intersection is not correctly detected | R |
| [3090][u3090] | ToonzVector Tape Tool - Rectangular mode does not close gap if line has 0 thickness | R |
| [3075][u3075] | Cutter Tool generates improper control points | R |
| [2708][u2708] | Motion Path - Center editing not working. | R |
| [2488][u2488] | Paint Brush Tool + Lines Mode + Selective Shortcut | R |
| [2376][u2376] | MyPaint Brushes ignore alpha channel on Raster levels | R |
| [2368][u2368] | Issues with Inverse kinematics and Pinned center | R |
| [2291][u2291] | Resizing a drawing in shift and trace edit mode doesn't work as expected (center point not correct) | R |
| [2250][u2250] | Texture Style + Geometric Tool Lines Get Fainter on Toonz Raster Level | R |
| [1636][u1636] | (Vector) Line thickness affects fill gap | R |
| [1361][u1361] | Skeleton IK tool broken | R |
| [1318][u1318] | (Vectors) Unfilling a space randomly autofills later | R |

### Rendering and effects (24)

| Issue | Report | State |
| --- | --- | --- |
| [6592][u6592] | Antialiasing: Color blending is not gamma-correct | R |
| [5408][u5408] | Layer Blending Effects don't work as expected with partially transparent pixels | R |
| [5407][u5407] | Noise Effect does not work as expected and changes behavior at bottom of camera view | R |
| [4855][u4855] | Collapsing a column that has a motion path to a subXsheet has the motion path both inside AND outside the subXsheet | R |
| [4617][u4617] | `Fx Intensity` slider does not work inside macro FX | R |
| [4504][u4504] | Z property not respected on scenes loaded inside another scene as level | R |
| [4461][u4461] | Adjust Levels - Controls reversed when using 16-bit Channel Width | R |
| [4349][u4349] | White pixels along .tga files when move camera? | R |
| [4136][u4136] | Rasters looks jagged when you are not in 100% zoom. | R |
| [3906][u3906] | Calligraphic line Fx does not work correctly with smooth lines? | R |
| [3830][u3830] | semi-transparent color in toonz vector has weird border when rendered | R |
| [3655][u3655] | An artifact in the rendering of a mesh section line when the drawing contains a style with transparency. | R |
| [3490][u3490] | Several swatches viewers do not update at the same time. | R |
| [3313][u3313] | Color changes with keys don't show up with "vector as raster" | R |
| [3294][u3294] | Crashes when opening a file while "vector as raster" is checked | R |
| [3227][u3227] | Curves FX: Slowdown with FX Settings open during playback | R |
| [3201][u3201] | animate tool in "all" mode slow down OT using plastic skeletons (only in windows version) | R |
| [3150][u3150] | Multitone - Positions of colors not saved if moved to the left or right ends | R |
| [3100][u3100] | Preview Cleanup stops automatically updating | R |
| [2471][u2471] | Edit In Place fails to show the scene properly when the camera moves | R |
| [2429][u2429] | ColorBlending FX makes zero-thickness lines visible in renders | R |
| [2329][u2329] | Exploding Macro Screws Up "OverFX" order | R |
| [2145][u2145] | Outline FX only works on larger objects | R |
| [1837][u1837] | Special FX "Art Contour" is restless | R |

### UI and input (30)

| Issue | Report | State |
| --- | --- | --- |
| [6426][u6426] | [Linux/KDE Plasma Wayland] Copy & Paste does not work when the clipboard is enabled | R |
| [5273][u5273] | The scene is loaded with all closed groups in Stage Schematic. | R |
| [4538][u4538] | Function Editor toggle-state not saved in room | R |
| [4316][u4316] | Command/Xsheet bar icons disappear on 2nd monitor | R |
| [4311][u4311] | Changing rooms creates drawing offset in undocked viewers | R |
| [4219][u4219] | Schematic input issues with display tablets/touchscreen monitors | R |
| [4113][u4113] | Zoom tool fails to zoom out when using stylus while dragging slowly | R |
| [4101][u4101] | Inconsistency on zoom limit | R |
| [4064][u4064] | tool selection vanishes after group selection on fx schematic | R |
| [3794][u3794] | link all selected elements | R |
| [3632][u3632] | Onion Skin Behavior in Different Autocreation Modes | R |
| [3547][u3547] | Print XSheet (.html) columns labels off by one | R |
| [3542][u3542] | [GNU/Linux ] HiDPI Monitor Support | R |
| [3338][u3338] | Display "Global  Key" option at the start of Tool Option Bar | R |
| [3299][u3299] | Link Flipbooks option does not work for pausing by Play/Loop button | R |
| [3254][u3254] | Changing the default Units needs a restart | R |
| [3130][u3130] | File > Configure Shortcuts... Shows Alt on Mac | R |
| [3077][u3077] | Windows Scaling settings breaks Viewer | R |
| [2833][u2833] | Input issues with mac Sidecar | R |
| [2828][u2828] | BadPegbar appears on the Function Editor | R |
| [2760][u2760] | Narrower layout of FX Settings for Particles | R |
| [2617][u2617] | Minor visual bug with column nodes | R |
| [2338][u2338] | Touch gesture overrides some double-click actions on touchpad | R |
| [1786][u1786] | (Huion) Hang when windows switched using stylus button | R |
| [1769][u1769] | Onion skin:  Additive tinting doesn't apply to brighter lines | R |
| [1503][u1503] | Surface Pro 3:  Scaling, Pressure and UI issues | R |
| [1281][u1281] | Schematic:  Node paths can’t be changed inside group | R |
| [1216][u1216] | High dpi scale issues | R |
| [1078][u1078] | (UI) Buttons and fields still needing minimum height | R |
| [681][u681] | (Onion Skin) Vector levels with vector brush applied stay fully opaque | R |

### Platform and distribution (5)

| Issue | Report | State |
| --- | --- | --- |
| [6721][u6721] | [bug] Toonzpreview.dll keeps being caught by antivirus | R |
| [6700][u6700] | Potential DLLs to be blacklisted | R |
| [4482][u4482] | Multiple macOS installer issues | R |
| [3592][u3592] | error: too many arguments provided to function-like macro invocation | R |
| [3522][u3522] | Fix weblate components | R |

### Tasks and audio (6)

| Issue | Report | State |
| --- | --- | --- |
| [5770][u5770] | Task Rendering:  Rendering image sequence returns clip with no movement | R |
| [4629][u4629] | Batch Task is Writing and Deleting Chunk | C |
| [4091][u4091] | 3 audio management problems. | R |
| [3860][u3860] | Stop Motion controls: Opacity value decreases when changing tabs. | R |
| [3806][u3806] | Undocked tasks window disappears while rendering | R |
| [1739][u1739] | Audio not updating properly in Viewer | R |

### Version control (1)

| Issue | Report | State |
| --- | --- | --- |
| [6920][u6920] | SVN repositories/projects get duplicated in File Browser on click | R |

## Qualification and ownership

1. Run focused regression fixtures for confirmed defects first, preserving original
   scene resources and output files. Require fail-before/pass-after evidence.
2. Reproduce the P1 queue with minimal, shareable fixtures on the reported platform
   and the current candidate; record exact revision, preferences, and action sequence.
3. Assign remaining R records to subsystem owners. Separate obsolete reports,
   intentional behavior, duplicate causes, current defects, and environment-specific
   failures only after checking evidence.
4. Review path and external-command trust policy with maintainers before introducing
   restrictions that change established artist workflows.
5. Use the main audit report and CI coverage map to record current patch status.
   Do not close an upstream issue solely because a similar Groundzero fix exists.

Source inspection and document integrity checks were performed. No application
builds, runtime tests, benchmark measurements, exploit reproductions, or GitHub
writes were performed by this lane.

[u7195]: https://github.com/opentoonz/opentoonz/issues/7195
[u6972]: https://github.com/opentoonz/opentoonz/issues/6972
[u6923]: https://github.com/opentoonz/opentoonz/issues/6923
[u6920]: https://github.com/opentoonz/opentoonz/issues/6920
[u6884]: https://github.com/opentoonz/opentoonz/issues/6884
[u6850]: https://github.com/opentoonz/opentoonz/issues/6850
[u6721]: https://github.com/opentoonz/opentoonz/issues/6721
[u6700]: https://github.com/opentoonz/opentoonz/issues/6700
[u6592]: https://github.com/opentoonz/opentoonz/issues/6592
[u6426]: https://github.com/opentoonz/opentoonz/issues/6426
[u6156]: https://github.com/opentoonz/opentoonz/issues/6156
[u5878]: https://github.com/opentoonz/opentoonz/issues/5878
[u5770]: https://github.com/opentoonz/opentoonz/issues/5770
[u5661]: https://github.com/opentoonz/opentoonz/issues/5661
[u5408]: https://github.com/opentoonz/opentoonz/issues/5408
[u5407]: https://github.com/opentoonz/opentoonz/issues/5407
[u5365]: https://github.com/opentoonz/opentoonz/issues/5365
[u5364]: https://github.com/opentoonz/opentoonz/issues/5364
[u5339]: https://github.com/opentoonz/opentoonz/issues/5339
[u5313]: https://github.com/opentoonz/opentoonz/issues/5313
[u5273]: https://github.com/opentoonz/opentoonz/issues/5273
[u5190]: https://github.com/opentoonz/opentoonz/issues/5190
[u4855]: https://github.com/opentoonz/opentoonz/issues/4855
[u4690]: https://github.com/opentoonz/opentoonz/issues/4690
[u4629]: https://github.com/opentoonz/opentoonz/issues/4629
[u4617]: https://github.com/opentoonz/opentoonz/issues/4617
[u4538]: https://github.com/opentoonz/opentoonz/issues/4538
[u4530]: https://github.com/opentoonz/opentoonz/issues/4530
[u4518]: https://github.com/opentoonz/opentoonz/issues/4518
[u4504]: https://github.com/opentoonz/opentoonz/issues/4504
[u4482]: https://github.com/opentoonz/opentoonz/issues/4482
[u4476]: https://github.com/opentoonz/opentoonz/issues/4476
[u4461]: https://github.com/opentoonz/opentoonz/issues/4461
[u4451]: https://github.com/opentoonz/opentoonz/issues/4451
[u4349]: https://github.com/opentoonz/opentoonz/issues/4349
[u4316]: https://github.com/opentoonz/opentoonz/issues/4316
[u4311]: https://github.com/opentoonz/opentoonz/issues/4311
[u4295]: https://github.com/opentoonz/opentoonz/issues/4295
[u4286]: https://github.com/opentoonz/opentoonz/issues/4286
[u4219]: https://github.com/opentoonz/opentoonz/issues/4219
[u4136]: https://github.com/opentoonz/opentoonz/issues/4136
[u4113]: https://github.com/opentoonz/opentoonz/issues/4113
[u4101]: https://github.com/opentoonz/opentoonz/issues/4101
[u4092]: https://github.com/opentoonz/opentoonz/issues/4092
[u4091]: https://github.com/opentoonz/opentoonz/issues/4091
[u4085]: https://github.com/opentoonz/opentoonz/issues/4085
[u4064]: https://github.com/opentoonz/opentoonz/issues/4064
[u4061]: https://github.com/opentoonz/opentoonz/issues/4061
[u3923]: https://github.com/opentoonz/opentoonz/issues/3923
[u3921]: https://github.com/opentoonz/opentoonz/issues/3921
[u3906]: https://github.com/opentoonz/opentoonz/issues/3906
[u3870]: https://github.com/opentoonz/opentoonz/issues/3870
[u3860]: https://github.com/opentoonz/opentoonz/issues/3860
[u3852]: https://github.com/opentoonz/opentoonz/issues/3852
[u3851]: https://github.com/opentoonz/opentoonz/issues/3851
[u3830]: https://github.com/opentoonz/opentoonz/issues/3830
[u3806]: https://github.com/opentoonz/opentoonz/issues/3806
[u3794]: https://github.com/opentoonz/opentoonz/issues/3794
[u3772]: https://github.com/opentoonz/opentoonz/issues/3772
[u3655]: https://github.com/opentoonz/opentoonz/issues/3655
[u3632]: https://github.com/opentoonz/opentoonz/issues/3632
[u3592]: https://github.com/opentoonz/opentoonz/issues/3592
[u3567]: https://github.com/opentoonz/opentoonz/issues/3567
[u3547]: https://github.com/opentoonz/opentoonz/issues/3547
[u3542]: https://github.com/opentoonz/opentoonz/issues/3542
[u3525]: https://github.com/opentoonz/opentoonz/issues/3525
[u3524]: https://github.com/opentoonz/opentoonz/issues/3524
[u3522]: https://github.com/opentoonz/opentoonz/issues/3522
[u3511]: https://github.com/opentoonz/opentoonz/issues/3511
[u3490]: https://github.com/opentoonz/opentoonz/issues/3490
[u3486]: https://github.com/opentoonz/opentoonz/issues/3486
[u3441]: https://github.com/opentoonz/opentoonz/issues/3441
[u3408]: https://github.com/opentoonz/opentoonz/issues/3408
[u3338]: https://github.com/opentoonz/opentoonz/issues/3338
[u3313]: https://github.com/opentoonz/opentoonz/issues/3313
[u3299]: https://github.com/opentoonz/opentoonz/issues/3299
[u3294]: https://github.com/opentoonz/opentoonz/issues/3294
[u3280]: https://github.com/opentoonz/opentoonz/issues/3280
[u3279]: https://github.com/opentoonz/opentoonz/issues/3279
[u3255]: https://github.com/opentoonz/opentoonz/issues/3255
[u3254]: https://github.com/opentoonz/opentoonz/issues/3254
[u3227]: https://github.com/opentoonz/opentoonz/issues/3227
[u3201]: https://github.com/opentoonz/opentoonz/issues/3201
[u3180]: https://github.com/opentoonz/opentoonz/issues/3180
[u3158]: https://github.com/opentoonz/opentoonz/issues/3158
[u3150]: https://github.com/opentoonz/opentoonz/issues/3150
[u3130]: https://github.com/opentoonz/opentoonz/issues/3130
[u3115]: https://github.com/opentoonz/opentoonz/issues/3115
[u3100]: https://github.com/opentoonz/opentoonz/issues/3100
[u3092]: https://github.com/opentoonz/opentoonz/issues/3092
[u3091]: https://github.com/opentoonz/opentoonz/issues/3091
[u3090]: https://github.com/opentoonz/opentoonz/issues/3090
[u3077]: https://github.com/opentoonz/opentoonz/issues/3077
[u3075]: https://github.com/opentoonz/opentoonz/issues/3075
[u3070]: https://github.com/opentoonz/opentoonz/issues/3070
[u2926]: https://github.com/opentoonz/opentoonz/issues/2926
[u2833]: https://github.com/opentoonz/opentoonz/issues/2833
[u2828]: https://github.com/opentoonz/opentoonz/issues/2828
[u2760]: https://github.com/opentoonz/opentoonz/issues/2760
[u2708]: https://github.com/opentoonz/opentoonz/issues/2708
[u2637]: https://github.com/opentoonz/opentoonz/issues/2637
[u2617]: https://github.com/opentoonz/opentoonz/issues/2617
[u2488]: https://github.com/opentoonz/opentoonz/issues/2488
[u2471]: https://github.com/opentoonz/opentoonz/issues/2471
[u2445]: https://github.com/opentoonz/opentoonz/issues/2445
[u2430]: https://github.com/opentoonz/opentoonz/issues/2430
[u2429]: https://github.com/opentoonz/opentoonz/issues/2429
[u2426]: https://github.com/opentoonz/opentoonz/issues/2426
[u2376]: https://github.com/opentoonz/opentoonz/issues/2376
[u2368]: https://github.com/opentoonz/opentoonz/issues/2368
[u2338]: https://github.com/opentoonz/opentoonz/issues/2338
[u2329]: https://github.com/opentoonz/opentoonz/issues/2329
[u2297]: https://github.com/opentoonz/opentoonz/issues/2297
[u2291]: https://github.com/opentoonz/opentoonz/issues/2291
[u2250]: https://github.com/opentoonz/opentoonz/issues/2250
[u2145]: https://github.com/opentoonz/opentoonz/issues/2145
[u1992]: https://github.com/opentoonz/opentoonz/issues/1992
[u1840]: https://github.com/opentoonz/opentoonz/issues/1840
[u1837]: https://github.com/opentoonz/opentoonz/issues/1837
[u1786]: https://github.com/opentoonz/opentoonz/issues/1786
[u1769]: https://github.com/opentoonz/opentoonz/issues/1769
[u1739]: https://github.com/opentoonz/opentoonz/issues/1739
[u1636]: https://github.com/opentoonz/opentoonz/issues/1636
[u1503]: https://github.com/opentoonz/opentoonz/issues/1503
[u1361]: https://github.com/opentoonz/opentoonz/issues/1361
[u1318]: https://github.com/opentoonz/opentoonz/issues/1318
[u1281]: https://github.com/opentoonz/opentoonz/issues/1281
[u1216]: https://github.com/opentoonz/opentoonz/issues/1216
[u1078]: https://github.com/opentoonz/opentoonz/issues/1078
[u681]: https://github.com/opentoonz/opentoonz/issues/681
[u356]: https://github.com/opentoonz/opentoonz/issues/356
[u119]: https://github.com/opentoonz/opentoonz/issues/119
