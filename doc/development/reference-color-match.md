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
- [ ] Native neural component and application transport on Linux, Windows,
      Apple Silicon and Intel Mac, including installed-package validation.
- [ ] Scene-owned texture references, crop/mask capture, editor previews,
      cancellation, replacement/relinking and portable `.gzstyle` profiles.
- [ ] Non-destructive texture FX with exact current alpha, ink protection,
      fixed camera sampling and canonical shot dependencies.
- [ ] Motion-compensated history, occlusion/cut rejection, held exposures,
      random seeking, cache eviction and nested FX qualification.
- [ ] Artist review of texture detail, line edges and representative animation.

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

Neural texture styling is a separate FX from the native color transform. Its
optional local worker uses a versioned request/result protocol,
verified model manifest (publisher, digest, license, provenance, input contract),
explicit installation, bounded memory and concurrency, cancellation and atomic
result publication. Keep runtime/model versions in cache identity. Do not load
executable Python checkpoints or automatically fetch models during rendering.

Temporal stylization implements shot boundaries, ordered dependencies,
held-drawing reuse, motion/occlusion handling and deterministic invalidation.
It must pass motion, cut, occlusion and random-seek tests before being advertised
as animation-consistent. None of these capabilities is implied by the native
color-matching feature. The isolated neural qualification workflow evaluates verified publisher weights
and pinned registry runtimes with synthetic inputs. Training and external
artwork transfer are not part of this workflow.

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
product model installation, motion estimation,
occlusion handling and temporally coherent texture synthesis remain unimplemented.
No neural or temporal UI advertises these capabilities as available.


## Neural qualification and product integration contract

The official Google publisher pages explicitly identify **Version 1** of both
[256-int8-prediction](https://www.kaggle.com/models/google/arbitrary-image-stylization-v1/tfLite/256-int8-prediction)
and
[256-int8-transfer](https://www.kaggle.com/models/google/arbitrary-image-stylization-v1/tfLite/256-int8-transfer)
as **Apache-2.0**. Their source links name the corresponding Google/Magenta TFHub
artifacts. Preserve these links, publisher identity, exact downloaded sizes,
SHA-256 hashes and license notices in any distributed model manifest. The
runtime's license alone is not the model license evidence.

`Neural style evaluation` uses LiteRT 2.3.0 with hash-pinned CPython 3.12 registry
wheels on Linux x64, Windows x64 and macOS ARM64. It limits downloads to 100 MiB
and its working directory to 500 MiB, installs in an isolated environment,
retains notices, validates the TFLite identifier, and exercises the two official
models on synthetic arrays. Numerical checks require finite outputs, repeatable
inference and separate content/reference influence. This is operator evaluation,
not artist-quality or temporal qualification. Reported environment size excludes
the CI-provided Python runtime. Native worker checks also cover reference changes,
cancellation/restart, malformed requests and a measured 512 MiB peak memory bound.
macOS native qualification uses the publisher's universal C API framework and
checks its ARM/Intel slices and deployment targets. Intel uses saved same-revision
Linux numerical fixtures, avoiding a dependency on an unavailable Python wheel.

Product integration uses a separate native helper, following existing
helper packaging in `toonz/sources/toonz/CMakeLists.txt` and QProcess lifecycle
patterns in `toonz/sources/toonz/purerefboard.cpp`. The optional Python scripting
and SeExpr SDK do not provide a redistributable Python interpreter. A successful
Python evaluation does not establish that native helper's packaging.

The application transport and FX have these validation gates:

- Handshake: protocol version, worker/runtime versions, supported tensor formats,
  model manifest hashes, supported dimensions and resource limits. Reject
  unsupported combinations before submitting artwork.
- Request: job identifier, content/reference/mask digests, cropped reference,
  straight RGBA buffers, frame dimensions, shot identifier and immutable settings.
  Use bounded local pipes or a private job directory; no network inference.
- Result: matching job/settings digests, complete image dimensions, pixels,
  timing and status. Publish only complete, current results. Restore original
  alpha and explicitly protected ink; never interpret model-provided executable
  code or Python pickle data.
- Scheduling: one inference worker per bounded queue, cancellation acknowledged
  or worker terminated, temporary output discarded, no partial render committed.
  Cache style embeddings by reference/model/preprocessing digest. Cache images by
  source, mask, settings, runtime/model and temporal dependency digests.
- Temporal mode: establish shot boundaries and canonical frame dependencies;
  reuse held drawings; estimate motion and occlusion; reject unreliable warped
  history; invalidate dependent results after edits. Random seeking must reproduce
  ordered rendering. A fixed reference or blending the previous image alone is
  not temporal consistency.

Before product integration is complete, qualify native packaging on every target
architecture, bounded peak memory, worker crash/cancellation recovery, alpha and
ink edges, large-frame strategy, color-space conversion, held drawings, motion,
cuts, occlusions and random seeking. Compare temporal stability against both
independent-frame styling and the unchanged native color baseline. Keep artist
approval separate from numerical or synthetic test results.

## Neural texture artist workflow and limits

Choose **Image Adjust > Neural Texture Style**, connect Source and optionally
Mask / ProtectInk, then **Create / Edit Texture Reference**. Load a representative
source sample, reference and optional reference sampling mask. Set the reference
crop and preview. White mask pixels include the reference; excluded/transparent
areas are composited onto white before model input. This is image preprocessing,
not a neural model trained to understand masks. The captured 256-square reference
is embedded in the scene, so missing original files never change its look.
Relinking originals is necessary only to recapture a different crop or mask.
Changing the reference clears its old mask unless the original fingerprint
matches. Import/export uses `.gzstyle`; Color Model and Reference Panel can create
these profiles from their existing images. FX profile changes use normal undo.

Set **Shot First Frame** and **Shot Last Frame** explicitly, using displayed
one-based frame numbers. Outside that range the FX passes through. A range is
limited to 120 frames; split longer sequences at intentional shot boundaries.
**Temporal History %** is shot-locked and limited to 85%. Zero requests independent
frame styling. **Style Strength** supports animation and zero is exact passthrough.
Fractional shutter samples use independent inference rather than inventing a
history between integer drawings. Current full-resolution alpha and explicit
ink/mask inputs are restored after texture generation. The untouched contribution
of float HDR pixels is retained at partial strength, but model inference is SDR.

Every frame is sampled on the fixed camera domain into a letterboxed 384-square
canvas. Texture resolution therefore remains 384 pixels: upsampling preserves
placement and original alpha, not fine synthesized texture. This is unsuitable
for claiming full-resolution detail restoration. The camera must be finite.
Sampling is independent of requested output tiles and occupied artwork bounds.

The temporal pass uses OpenCV Farneback forward/backward flow between source
frames. It rejects history outside bounds, on alpha disagreement, inconsistent
motion or excessive color residual; explicit shot boundaries and conservative
cut/overlap guards reset history. Exact held inputs reuse the same stylized image.
Fast/deforming motion, flat regions and occlusions remain quality risks requiring
artist review. Results depend on canonical history from shot start, not the last
frame viewed. Editing an earlier input changes downstream identities. A 32 MiB
checkpoint cache may evict results without changing output. Cold random seeks and
nested temporal FX can be expensive because all prior dependencies are sampled.

One process is leased per bounded inference batch, with at most 120 frames. It
retains compiled models and one embedding within that batch, then exits; idle
sessions hold no worker. Cancellation discards partial responses and unpublished
results. Individual optical-flow calls finish before cancellation is observed.
Active protocol requests have 30-second deadlines. Queue waiting is cancellable
and uses FIFO admission with at most eight admitted waiters; it does not time out
merely because a valid long batch is active. Additional callers receive
cancellable backpressure. The renderer reserves 800 MiB plus its output
tiles for the worker/history/cache/flow peak; output tiles are capped at 16 MP.
The worker's measured CI peak must remain below 512 MiB. These are bounded design
and measured checks, not an OS-enforced memory sandbox.

The component directory contains only the helper, native LiteRT library, two
verified models, hashes and notices. It is supplied by trusted application
packaging, never by a scene or a rendering-time download. Manifest checksums
detect corruption; authenticity depends on the trusted package distribution.
Source builds without that optional component retain color matching and show an
explicit component-unavailable error for neural styling.

The editor, FX, temporal pass and their integration tests are authored but require
final-revision application CI and installed-package execution. Operator checks
already qualify the selected native C API on all four architectures; that alone
does not qualify this newly added product integration or artistic quality.

Application CI emits `ReferenceStyle-*` artifacts with original synthetic cel and
painted-background comparisons. Columns are source, independent neural and
temporal; rows cover a pan, a held exposure and deformation/foreground occlusion.
They include current-alpha/ink checks and known-motion difference measurements,
plus an editor screenshot. Those metrics are not an artist approval. The tests
also save and relocate an actual `.tnz` FX graph after removing its reference
original, exercise end-to-end masked 8/16-bit and float rendering, and change or
cancel a reference after real model inference starts. A separate installed-package
gate verifies the shipped component hashes/notices and executes inference without
development loader paths or a Python inference library.
