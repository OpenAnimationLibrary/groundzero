A day-by-day list of everything merged from my pull requests, 25 to 29 October 2026 (UTC). Each line says what changed, in plain words; the numbers at the end are the pull requests, if you want the details.

---

## Saturday 25 October

**New**
- Selection Lock: stops picking things in the viewer, nothing else. (#4)
- Function Editor: ease presets for selected segments, and breakdown keys in the spreadsheet. (#5, #11)
- Sound: levels shown next to the Soundtrack button, and the volume of a sound column can be animated. (#6, #19)
- Light Table view toggle. (#12)
- PSD export. (#16)
- Plastic tool: inverse kinematics in Animate mode, columns that follow skeleton bones, and optional curve bending. (#17, #22, #39)
- Tone / Highlight FX for cel-style shading. (#18)
- Pump tool: thicken and thin brush modes. (#20)
- Animate tool: Show Trajectory option. (#21)
- Library: Save Selection as Template. (#26)
- Xsheet: column groups that can be nested and folded. (#29)
- Poses panel for weighted rig poses. (#28)
- Tilt (X/Y rotation) channels for stage objects. (#37)
- Vector in-betweening: stroke pairing hints. (#38)
- Experimental GPU compositing of raster levels in the viewer. (#43)
- Optional Line Art / Colour Art editing for Toonz Vector levels. (#45)
- Several projected GLB scenes per model FX. (#14)
- Harmony-style features: Reference panel, Annotation column, Drawing Substitution panel, Storyboard panel, Lasso Cutter tool, Master Controllers panel, Colour Override FX, Auto-Patch FX and Envelope Deformer FX. (#55)
- Rigging and deformation: Deformer Stack, Rig View in the Stage Schematic, offset layers, Curve / Free-Form / Bone deformers, Envelope Deformer Plus, Substitution Slider, Rig Library, and meshes in the Spine export. (#57)
- Through the first snapshot merge: exports stream frames straight to ffmpeg, single frames render in parallel strips on idle threads, and scripting moved from QtScript to QJSEngine with access to stage objects and keyframes. (#23, #24, #25, #30)

**Fixed**
- A failed cell paste no longer breaks undo, and undo sizes count the image data they hold. (#2, #15)
- Crash reports on Linux and macOS are written safely and shown at the next launch. (#13)
- Screen handling moved from the removed QDesktopWidget to QScreen. (#3)

**Behind the scenes**
- Pulled in the latest OpenToonz changes (palette tools, WAV fix, Windows scanning and more). (#54)
- The plan for the work that followed. (#56)
- Optional extra compiler warnings, the test switch for all regression tests, FX render tests, and removal of unused sources. (#1, #9, #10, #34, #35)

---

## Monday 27 October

**New**
- Drawing, timing and pipeline: Line Art / Colour Art layers with an Art Layer Manager, per-stroke pencil opacity, Photoshop ABR brush import, smarter morphing, Colour Override swatches and modes, Light Shading FX, per-column Retime / Hold / Loop, Storyboard notes and reordering, and a Lasso Cutter that keeps fills. (#58)
- FX Schematic port filter, optional compressed scene files, Convert Brush Strokes to Outline Shapes, and the Shape-Aware Deformer FX. (#59)
- Scripting (JavaScript and Python) can read image pixels, level frames and scene columns, and render frames. Also Rig Breakdown / Rig Ease, Layered PSD export, Light Table opacity, a Reference panel with several images, and 270 new translation entries in 11 languages. (#60)
- Faster scene saving and loading, more deformer types in the Deformer Stack, Bake Deformers into Vector Level, and a port filter in the Rig View. (#69)

**Fixed**
- Damaged PSD, PLI and EXR files no longer crash or hang the loaders. (#60, #69)
- A placement bug in the Free-Form deformer. (#69)
- The Windows build. (#71)

**Behind the scenes**
- Tests now run in CI, and the whole project builds with Qt 6 on Linux. (#69)
- The Linux build avoids a package mirror that kept stalling. (#70)
- A rule, and checks, against publishing personal data or session links. (#72)

---

## Tuesday 28 October

**New**
- Viewer: aspect ratio guides with a letterbox overlay, a custom outside-camera colour, and a Pegbar Guide. (#74, #75, #76)
- Export to an aspect ratio, letterboxed or cropped. (#77)
- Ten new cel effects: Cel Color Keep, Cel Color Key, Cel Color Expand, Cel Boundary Blur, Cel Smoother, Cel Smoother v2, Alpha Distance Gradient, Cel Radial Blur, Cel Directional Blur and Star Glow. (#78, #80, #82, #84, #85, #86, #87, #88, #89, #83)
- Assisted cel painting that runs locally, with region correction, side-by-side comparison and session recovery. (#95)

**Fixed**
- Code audit, round 1: crashes and data loss in the schematic, autosave recovery, GLB poses, Poses panel, Spine export, art layers, exporters, undo, and the EXR, PSD and compressed scene readers. (#94)
- Code audit, round 2: scripting crashes and hanging renders, the Type tool's Down arrow and vertical text, 24-bit WAV playback, cancelled strip renders, hooks and sub-xsheets with time remap, and the release step. (#96)
- Image bounds in ToonzRaster scripting. (#73)

**Behind the scenes**
- CI checks no longer fail for reasons outside the change being tested. (#79)

---

## Wednesday 29 October

**New**
- Adobe Animate-style features: Motion Presets panel, per-column Color Effect, a Library shared across scenes, Lottie export, Width tool, Gradient Transform tool, Free Deform and Edit Multiple Frames, Distribute to Layers and Convert to Symbol, Envelope modifier, Rig Mapping, Paint Fills / Paint Behind / Fluid Brush, Texture Atlas export, and Outline View. (#93)
- Three new themes: Animate, ToonBoom and Premiere. (#99)

**Fixed**
- Code audit, round 3: four cel effects now fill the whole frame over generated sources at any preview size, `view(level)` in scripts no longer crashes, sound volume works in expressions, Plastic vertex rename keeps its links, plus Qt 6 audio recording, Qt 6 colour picking on other monitors, and fewer Function Editor rebuilds. (#97)
- Code audit, round 4: raster text keeps the edges of letters like "j", Create 3D LUT and Import PDF no longer freeze the window and can be cancelled, and a crash on machines without OpenGL. (#98)

**Behind the scenes**
- Tools that read CI failures and group them by cause. (#90)
- The release step reports what failed and retries uploads. (#100)

---

**Closed without merging:** #33, #36 and #48 (CI workflow changes) and #81 (a written audit of missing Animate features, which the work in #93 followed). The work of #40, #42, #44, #46, #47 and #49 (vertex-array drawing, Spine export, the Qt 6 build, Harmony-style symbols, Python scripting and autosave recovery) is in the app through later pull requests.
