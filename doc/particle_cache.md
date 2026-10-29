# Particle caches and OT artwork

The Particle Cache FX brings recorded particle motion into the FX Schematic.
Connect OT drawings or animated artwork to its `Artwork` inputs, then composite
the result with ordinary effects. The companion bake command records OT's
Particles and Tiled Particles effects into a reusable cache package.

This integrates the complete Partio core library, including its readers,
writers, attribute data model and spatial-query implementation. Partio does not
simulate fluids, smoke, cloth or particles, and it does not render artwork.
Native simulation and sprite rendering remain OT functionality.

## Import motion and draw the particles

For falling leaves prepared in another application:

1. Export the motion in a Partio-supported format. Include a stable particle ID,
   position and, where available, size, rotation, velocity and age.
2. Choose **File > Particle Caches > Import Particle Cache**. Select a cache file
   or the `cache.otpc` manifest of an OT cache package. A numbered cache file can
   identify its sibling sequence.
3. Inspect the attribute list and use **Attribute Mapping** to match the source
   names. Position is required. Optional attributes have defaults. Set units
   and axes so the imported motion fits the drawing plane.
4. Connect leaf drawings or an animated leaf level to `Artwork1`, `Artwork2`,
   and further inputs of the new Particle Cache FX. Artwork is rendered through
   OT's existing FX inputs, including raster, Toonz Raster and vector sources.
5. Set the cache and scene frame rates in FX Settings. `Cache FPS` equal to zero
   uses the package's rate; a bare sequence starts with the library adapter's
   default metadata. The import command initializes `Scene FPS` from the scene.
   If the scene rate changes later, update this control explicitly.
6. Choose cache artwork indices, stable-ID random assignment, or the other
   available assignment modes. Choose a fixed drawing, the cached drawing frame,
   cycling by particle age, or cycling by scene frame.
7. Adjust size, tint, opacity and ordering, then connect downstream FX or render
   through the Xsheet as usual.

The imported sequence is exposed at the current scene frame. Its initial column
duration follows the sequence's metadata frame rate; a static file uses ordinary
zerary-FX exposure behavior. Timing controls can hold, loop, reverse or retime
playback. Edit the column exposure when extending a loop or changing duration.

The inspector can reopen or relink a selected cache, inspect attributes and
change mappings with undo. Its mapping is saved on the FX and does not modify
the input file. To enter attribute names directly in FX Settings, turn off
**Use Manifest Mapping**. In that mode the inspector displays the active mapping
read-only and can relink the cache without changing animated mapping controls.
Turn Use Manifest Mapping back on to edit mappings in the inspector.

Stable IDs identify particles across frames. Particle row order is not an
identity. Hold Samples is the default; Interpolate Stable IDs requires valid
identities and handles births/deaths explicitly. Missing files and missing
attributes are different errors. Missing-frame behavior is selectable, so an
intentional gap can hold or become transparent without hiding a corrupt file.

## Bake native snow or sparks

1. Build the effect with OT's **Particles** or **Tiled Particles** generator and
   connect artwork and control images as usual.
2. Select that generator in the FX Schematic and choose **File > Particle Caches
   > Bake Particle Effect**.
3. Set the first and last frames, choose 8-bit or 16-bit artwork precision, and
   choose a new destination folder. Existing folders are never overwritten.
4. Choose the capture area. **Camera framing** is the default: it captures the
   camera's output pixel area at every frame, following animated column, parent
   and camera placement, including rotation and scale. **Custom FX area** lets
   you enter bounds that stay fixed in the particle effect's coordinates. Its
   initial bounds enclose the camera area at the first frame when the source is
   visible; otherwise they start at the camera's physical size. After you edit
   them, changing the first frame preserves your edits. Expand or reposition
   those bounds when controls or tiled particles need a larger evaluation
   region. Sampling density follows the camera's output resolution.
5. Leave **Use the completed cache in this scene** enabled to switch playback
   after a successful bake. The original generator and its inputs remain in the
   scene as an editable, disconnected schematic node. For a zerary column, its
   exposure and stage placement stay on the cached playback. Undo restores the
   procedural source; undo does not delete exported files.
6. Save and reopen the scene, or collect the project to move it to another
   machine. The package includes the recorded appearance required for playback.

Select a generator belonging to the current Xsheet. If a source is inside a
macro, explode the macro before baking; the command does not rewrite macro
membership or its exposed parameter interface. Gaps in the generator's column
exposure and frames where its column is behind the camera are recorded as empty
frames. Singular or invalid transforms report an error.

Native generators evaluate at whole frames. The command records every selected
frame rather than pretending that their integer simulations support arbitrary
subframe evaluation. Native trails and motion-blur contributions are captured
as ordered draw records.

Procedural artwork inputs with unlimited duration are sampled over a fixed
range: frame 1 through the later of the bake's last frame and the scene's last
frame. This range stays the same for every captured sample, so random or cycling
artwork selection does not change its frame count during the bake. Finite artwork
inputs keep their own duration. The package records the procedural range's last
frame as the zero-based `artworkLastFrame` metadata value.

Prepared appearance is **individual particle artwork with transforms and draw
order**, not a flattened image of the whole effect. It preserves native color,
opacity, trails, blur and Tiled Particles' prepared image fragments. Sprite
assets use a versioned lossless channel format. Native sampling is nonlinear
8/16-bit; the cache renderer converts for linear output and can produce
8/16-bit or floating-point rasters. Selecting a higher output precision cannot
recover detail absent from the recorded artwork.

The captured artwork has finite resolution. Enlarging it greatly after baking
can reveal resampling. Rebake at a suitable camera resolution when higher
detail is needed. The capture region is also part of the result: camera framing
keeps the output viewport fixed while the local evaluation area follows the
scene transforms; a custom area keeps the local rectangle fixed instead. A
cache does not rerun an infinite control image when you change the camera after
baking. Custom areas under rotation or shear evaluate the enclosing output
pixel rectangle.

Native tiled-particle initialization depends on the evaluation viewport. The
bake uses a fresh simulation with the chosen viewport for each frame. A preview
that was warmed with a different viewport or simulation history can therefore
differ; comparison must use the same fresh evaluation and capture framing.
Ordinary animated column and camera transforms are sampled per frame, rather
than freezing their placement at the first frame.

To reuse the motion with different drawings, turn off **Use Baked Appearance**,
connect replacement artwork and choose the appropriate size and artwork modes.
For native scale values, **Artwork Scale** interprets the size as an artwork
multiplier. This is a new appearance built from the recorded particle data;
native prepared fragments and recorded trails are used only in baked-appearance
mode. Generic trail and motion-blur controls apply to the replacement-artwork
path.

## Interchange and formats

Select a Particle Cache FX and choose **Export Particle Cache** to export its
original particle data into a new directory. This does not bake downstream FX,
timing controls or the entire OT scene into another application's renderer.
Collect the OT project to share artwork and native effects.

The writer menu follows Partio's actual format registry:

| Capability | Extensions (including aliases) |
|---|---|
| Read and write | `bgeo`, `bhclassic`, `geo`, `hclassic`, `pdb`, `pdb32`, `pdb64`, `pda`, `ptc`, `ptf`, `pdc`, `prt`, `bin`, `itbl`, `atbl` |
| Read only | `mc`, `pts` |
| Write only | `rib` |

The dispatcher also accepts trailing `.gz`. Reader/writer support is specific
to the implementations in the pinned Partio version. In particular, BGEO is
**legacy version 5**; modern binary-JSON BGEO and `.bgeo.sc` are unsupported.
For Houdini interchange, use a compatible legacy export such as `.bhclassic`.

Formats do not all store the same attributes or types. Export checks for data
loss and fails unless the user explicitly allows unsupported attributes to be
omitted. RIB is write-only and cannot be validated by reimport through Partio;
it requires that explicit opt-in and produces output frame files, not a playable
OT cache package. Readable exports can include an OT manifest. Generic data
exports do not claim to retain native prepared sprite appearance.

## Packages, projects and failures

An OT package contains `cache.otpc`, particle frame files, and any native sprite
assets referenced by the manifest. Keep the package together. Scene references
use project-aware paths; collection handles package contents and relinking is
available through the inspector. Native input and control image levels remain
ordinary scene resources, because the original generator stays editable.

Writes use a temporary staging location and publish only after validation.
Cancellation, read failure or write failure must leave the scene and existing
destination untouched. A successful bake can therefore be kept without switching
the scene, or used immediately with a scene undo operation.

The implementation imposes explicit memory and input limits rather than loading
unbounded data: particle frames are limited to ten million particles and 1 GiB
of decoded attribute data, and the manifest supports up to 100,000 samples.
Native capture limits the raster dimension to 16,384, artwork memory to 512 MiB
per capture and draw count to one million. Oversized, malformed or unsupported
inputs report errors. The shared particle-frame and sprite caches have bounded
memory and reload changed files.

## Implementation and validation

- `toonz/cmake/Partio.cmake` pins and builds the complete upstream core, with
  zlib required. Separate Partio applications and Python bindings are not needed
  by OT's native C++ workflow.
- `toonzlib/particlecache.cpp` owns generic data, mappings, format capabilities,
  package parsing, playback and verified interchange writes.
- `stdfx/particlecachefx.cpp` owns deterministic sprite rendering and FX controls.
- `stdfx/particlebake.cpp` and the capture hooks evaluate native simulation in
  isolated state; `particlebakeio.cpp` stores and reloads native draw packages.
- `toonz/particlecachepopup.cpp` owns native import, inspection, export and bake
  commands. Baking expands the full scene FX tree, extracts its outer affine
  transforms, and passes the detached native generator with that frame's complete
  render affine and viewport to the capture service. Scene-resource discovery
  tracks cache assets separately from levels.

CI regression targets cover core I/O, generic rendering, native capture/playback
and project resources. Required checks include image comparisons for native
effects; reordered IDs and births/deaths; tiled and out-of-order rendering;
precision and alpha handling; format-direction and attribute-loss checks;
failed/cancelled exports; save/reload and project relocation; and existing-scene
compatibility. These are validation requirements, not a claim that all checks
have passed. Consult the PR's current CI results and application-testing notes.

Upstream: [Partio](https://github.com/wdas/partio),
[license](https://github.com/wdas/partio/blob/main/LICENSE).
