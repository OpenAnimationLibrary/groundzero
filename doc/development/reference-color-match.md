# Reference Color Match

## Acceptance checklist

Implementation, execution in CI, and artist validation are separate gates. This
draft separates authored coverage from CI execution and artist validation.
Checklist completion requires the final revision to pass the corresponding gate.

- [ ] Shared native, versioned color transform with bounded analysis and explicit
      color-space assumptions; no model, network, or training dependency.
- [ ] Non-destructive FX, exact alpha preservation, source/mask/ink-protection
      inputs, zero-strength identity, 8/16-bit and float rendering.
- [ ] Shot-locked profile embedded in scene, independent of frame order, tiles,
      missing original images, and machine-local paths.
- [ ] Reusable reference editor with source/reference crops, sampling masks,
      before/after preview, cancellation and bounded memory.
- [ ] Reuse Color Model and Reference Panel; one undoable palette operation,
      stable style IDs, explicit style selection and protection of locked,
      linked, animated or unsupported styles.
- [ ] Import/export profiles and export compatible creative 3D LUTs. Replacing
      a source/reference is explicit; unavailable files cannot silently change
      an existing scene's look.
- [ ] Deterministic tests for color math, malformed profiles, masks, alpha,
      cancellation, serialization and palette undo; CI execution pending.
- [ ] Document limitations and a concrete extension contract for neural and
      temporal processing without shipping placeholder controls.

## Design decision

The core transform uses alpha-weighted statistics in OKLab, with a bounded
per-channel contrast ratio and optional preservation of OKLab lightness. Input
and output are SDR sRGB. Profiles contain statistics, not image paths or pixels.
Normalized source/reference crops, sample basenames and SHA-256 fingerprints of
normalized sampled RGBA pixels are retained for explicit relinking. Files and
absolute paths are never embedded. Artists
explicitly capture a look from a representative source and reference;
the same mapping is used throughout a shot. This avoids frame-dependent pumping
and tile-dependent statistics. It is color matching, not semantic recoloring,
texture synthesis, or color-managed HDR grading.

The FX follows 3D LUT Bake's nonlinear RGB and premultiplication conventions.
Its profile is a non-animatable parameter included in normal scene persistence
and render identity. Changing the profile is undoable. Imported profiles are
copied into the scene, so they need no external-file relinking at render time.

## Neural and temporal extension boundary

Neural texture styling is not implemented by this native color transform. A
future optional local worker must have a versioned request/result protocol,
verified model manifest (publisher, digest, license, provenance, input contract),
explicit installation, bounded memory and concurrency, cancellation and atomic
result publication. Keep runtime/model versions in cache identity. Do not load
executable Python checkpoints or automatically fetch models during rendering.

Temporal stylization additionally needs shot boundaries, ordered dependencies,
held-drawing reuse, motion/occlusion handling and deterministic invalidation.
It must pass motion, cut, occlusion and random-seek tests before being advertised
as animation-consistent. None of these capabilities is implied by the native
color-matching feature. Weight downloads, runtime installation, training and external artwork transfer
are outside the current implementation authorization.

## Artist workflow

In the FX Schematic choose **Image Adjust > Reference Color Match**. Connect
Source, open **Create / Edit Color Profile**, load a representative source image
and a reference, set normalized crops and optional sampling masks, then Preview.
Use Profile embeds the result in the scene and participates in ordinary FX undo.
Use the standard FX intensity control for animated blending. Clear restores
pass-through. A missing profile is pass-through; a malformed profile is an error.

The source and reference samples can be replaced explicitly in the editor.
Original images and their paths are not retained in a profile. To revise a
captured look later, load the samples again. Matching fingerprints verify the
relink without changing the captured look; differing replacements require a new
Preview. Recorded sampling masks must be relinked or explicitly cleared before
recomputing. Missing source images never prevent using the stored statistics.
Fingerprints describe decoded/downsampled pixels, not original file bytes;
decoder differences can require explicit recapture. Strength and lightness can be adjusted on an imported profile.
Export Profile saves a portable `.gzcolor` file. Import copies its contents into
the scene, so subsequent edits to that file cannot change an existing render.

Connect Mask to restrict the rendered effect and ProtectInk to protect explicit
ink coverage. Both use premultiplied luminance: opaque white is full coverage;
black or transparent is none. An ink image with black strokes must first be
converted to a white coverage mask. Sampling masks in the editor influence
statistics only; they are not the FX application mask. Protected pixels and
alpha remain unchanged. Float inputs are treated as SDR; out-of-range transformed
RGB is clipped. This is not HDR or automatic ICC/OCIO color management.

Color Model's context menu offers **Match Palette to Reference Colors** for
raster references. Reference Panel offers **Match Colors** for its current
image/PSD layer. View rotation and opacity do not affect sampling. Review the
before/after swatches and explicitly check styles to apply. All rows start
unchecked, protecting ink by default. Transparent style zero, linked styles,
non-solid styles and animated/locked palettes are excluded or rejected. The
whole batch is one undo step; IDs, names, alpha and drawing geometry are retained.
Shared palette edits affect all drawings using that palette.

A 33-point creative `.cube` can be exported for the existing 3D LUT Bake FX.
Interpolation approximates the analytic color transform. Avoid applying the
profile and its exported LUT twice. A creative look is not monitor calibration.

## Bounds, scheduling and cache

Image loading refuses dimensions above 16 megapixels before decode and samples
at most 1024 by 1024. Two source images and two sampling masks are retained by
the editor; only one analysis job runs at a time. A changed input cancels the
current job and invalidates its result. Analysis and preview check cancellation
on every row; decoder calls themselves are not interruptible. Closing the dialog
cancels work and joins it before releasing its state. Decode formats supported
by Qt may allocate a full source image before downsampling.

The native FX holds no persistent image/model cache. Profiles are limited to
4096 characters and parsed per tile; existing renderer cache identity includes
the serialized profile and connected mask inputs. Render tiles above 16
megapixels fail explicitly. Two optional mask rasters and an atomic output copy
bound this node's additional working storage to at most 768 MiB for float RGBA
at that limit, excluding upstream rendering. Ordinary 1080p tiles use much less.
The native transform has no previous-frame dependency and no random seed.

## Verification gates

`referencecolormatch_test` is registered with `BUILD_LUT3D_TESTS` (also enabled
by `BUILD_TESTS`). Platform CI compiles the application and the test, and runs the
new test alongside existing integration tests. Coverage includes numerical RGB
results at three pixel depths, fractional masks and ink protection, shuffled
frames, partitioned tiles, profile cache identity, reopened FX rendering, sample
fingerprints and LUT grid values. Tests are authored, not executed
locally, in accordance with repository policy. CI must verify the exact final
revision before this draft can become ready.

Manual gates: compare source/reference crops; verify mask alignment and partial
alpha; review palette names/IDs/undo across scene switches; inspect existing and
new scenes after reopen; move a scene to another machine without its reference
images; cancel and edit during preview; inspect dark fills separately from ink;
compare FX and exported LUT within interpolation tolerance; test Qt 5/6 and
Windows/macOS/Linux, including Apple Silicon; obtain artist approval on a cel,
a textured background, a pan and held drawings. No artist validation has occurred.

## Remaining feature-family work

The native baseline is an implementation candidate, not completion of the
neural-style feature family. Optional neural models, runtime packaging,
model-provenance clearance, model download management, motion estimation,
occlusion handling and temporally coherent texture synthesis remain unimplemented.
No neural or temporal UI advertises these capabilities as available.
