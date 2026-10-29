# Particle Cache example project

Download the **particle-cache-examples-Linux-gcc** artifact from the Particle Cache CI run
for this change, then extract the entire archive. Use the OpenToonz build from
the same change, which includes the Particle Cache FX. These scenes do not work
in older builds without that effect.

Open a scene below with **File → Load Scene**. Keep the project file,
`scenes/scenes.xml`, `drawings`, and `extras` together when moving the examples.
OpenToonz discovers the project automatically from `scenes.xml`; no external
simulator, plugin installation, or absolute asset paths are needed. Each scene
is 24 frames at 24 fps, with a 256 × 256 camera. PNG previews are in `previews/`.

| Scene | What it demonstrates |
| --- | --- |
| `scenes/01-external-motion.tnz` | Particle motion from a Partio BGEO sequence, rendered using two editable OpenToonz star drawings. The cache is 12 fps and the scene is 24 fps. Stable IDs preserve particle identity while file order changes between samples. |
| `scenes/02-standard-particles.tnz` | Native Standard Particles captured to a cache, including trails, randomized scale/opacity, artwork animation and birth color. The original generator and its drawing/control inputs remain editable in the FX schematic. |
| `scenes/03-tiled-particles.tnz` | Native Tiled Particles captured with its finite base image, tile sampling and flap settings. Playback retains the native prepared artwork, while the original generator and input levels remain in the schematic. |

## Imported motion with OpenToonz artwork

Open `01-external-motion.tnz`, preview frames 1–24, then open the **Particle
Cache** FX settings on the playback column. Its cache path is
`+extras/external-motion/cache.otpc`. The package contains 13 samples, numbered
101–113, at 12 fps. Scene frame 1 starts at cache sample 101; the scene advances
half a cache sample per frame with **Interpolate Stable IDs** enabled. Frame 24
therefore evaluates sample 112.5. Position and velocity are in OpenToonz world
units and world units per second.

The manifest maps `P` to position, `v` to velocity, and `particleId` to identity.
It also contains radius (`pscale`), rotation, age, lifetime and drawing selection
(`ot_frame`). IDs 1001–1012 stay fixed even though particle array order reverses
on alternate samples. The motion is generated as interchange data; it does not
require a native OpenToonz particle simulator to play.

Edit the two drawings in the `external-star` level to change the particle look
without changing its motion. Change **Speed**, **Time Offset**, or artwork
settings to explore retiming and replacement artwork. **Inspect Particle
Cache** shows the source attributes and mapping. Exporting this cache can move
the motion to other supported particle formats; artwork rendering remains an
OpenToonz scene operation.

## Native simulation and baked playback

Both native scenes open on their **BAKED PLAYBACK** effect. In the FX schematic,
the disconnected **EDITABLE SOURCE** node retains the native generator settings
and links to its artwork and control columns. The visible output uses the
24-frame package in `+extras/standard-bake` or `+extras/tiled-bake`. The capture
uses the full camera domain, 32-bit RGBA (8 bits per channel), a fixed seed of
127, and one native simulation sample for each integer scene frame.

To compare the original, disconnect the baked playback column from **Xsheet**
in the FX schematic and connect **EDITABLE SOURCE** to **Xsheet**. Preview the
same frame range. Connect the playback column again to return to the cache.
Keep only one of those outputs connected when comparing; otherwise both will
be composited. To experiment, change the source parameters and use **Bake
Particle Effect** to create a new package. The existing bake remains a snapshot
of its captured simulation until you bake again.

The package contains live particle state plus per-frame draw recipes and native
sprite assets, rather than a flattened movie. This preserves trails, native
color treatment and tiled appearance while avoiding repeated simulation during
cache playback. The source drawings remain ordinary editable PNG levels. To
restyle the cached motion using the connected drawings, disable **Use Baked
Appearance** in the cache FX; that intentionally replaces the captured native
appearance with ordinary cache artwork rendering.

Native capture samples the existing generators at integer frames. Increasing
the output resolution cannot recover artwork detail beyond the baked sampling
resolution; rebake at the required production resolution. These examples are
small workflow demonstrations, not performance benchmarks. External caches can
use arbitrary source frame rates, stable-ID interpolation, trails and shutter
sampling through the cache renderer.

## How the bundle is produced and checked

CI builds `particlecache_demo` and runs it with a fresh output directory and this
README. The generator is
`toonz/sources/stdfx/tests/particlecache_demo.cpp` in the repository. It uses
`ToonzScene`, real raster levels, registered production FXs, and the public
particle cache/bake APIs. It does not hand-author scene XML or persist custom
test effects.

Before publishing `VALIDATED.txt`, generation saves all three scenes, moves the
whole project, reloads the scene and artwork files, checks every cache dependency
is inside the moved bundle, and compares rendered sample frames before and after
reopening. Native live and baked output must match exactly at frames 6, 13 and
24. The external scene checks frames 1, 12 and 24, including fractional cache
sampling. `previews/` contains one rendered PNG from each reopened scene.

The generated artwork and particle motion are original example content and may
be used or modified under the repository's BSD-3-Clause license.
