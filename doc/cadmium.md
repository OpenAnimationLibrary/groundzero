Cadmium assisted paint
=====================

This feature is under validation. Model predictions require artist review.
Windows x64 is the initial runtime target.

Cadmium suggests flat fills from already painted reference drawings. It runs
locally using pretrained models. No account, hosted inference service, or model
training is needed. Once the runtime and models are installed, painting works
offline.

Setup and startup
-----------------

The startup window and Preferences both expose Cadmium startup mode:

- Off: keep the local worker stopped.
- Load when needed: start when paint generation is requested; this is the default.
- Preload at startup: initialize models in the background after the main window
  appears. This reduces first-use loading delay and uses memory earlier.

The choice is remembered automatically. No configuration-file editing is needed.
Use Unload Models to release the worker's memory. An inactivity timeout can unload
it automatically; the next request then needs another model load. Closing the
application stops its worker.

Select the optional runtime component's executable in Cadmium settings. Keep its
runtime libraries and manifest beside it. Choose a model folder, then explicitly
download the universal or Windows acceleration model set. Downloads verify file
sizes and checksums before installation. Existing verified model files can also
be installed offline by selecting their folder.

The optional runtime component is delivered separately with its dependency
notices. Extract the component before selecting its executable.

Painting
--------

1. Open a supported level in the Level Strip.
2. Mark already painted reference drawings in the reference gutter or context
   menu. Reference marks are independent of selection and onion skin.
3. Select target drawings and open Cadmium Assisted Paint.
4. Check the reference assigned to each target. Each target uses one reference.
5. For full-color artwork, choose transparent line art, approved paper extraction,
   or a linked clean-line level. Inspect extraction before approving it. Painted
   flattened references generally need clean-line input or approved extraction.
6. Generate proposals. Generation does not modify drawings.
7. Inspect each result, resolve palette ambiguities or skip regions, and mark the
   result reviewed before applying a reviewed batch.
8. Apply accepted proposals or discard them. Correct paint using existing tools.

Repeated exposures of a drawing are processed once. Editing a target, reference,
palette, or vector geometry invalidates affected proposals. Changing scenes must
never redirect a proposal to another document.

Existing paint is protected by default. Explicit repaint mode permits replacing
paint in accepted regions. Full-color white paper and white paint cannot be
distinguished perfectly from pixels alone: inspect the chosen source mode and
protection behavior before applying.

Toonz Raster results retain native palette paint, inks, and antialiasing tone.
Vector results update native region fills while retaining strokes and geometry.
Full-color results create a new level in the scene cast and preserve the source;
insert the result into the scene where required. Supported sample precision and
image geometry are retained. Undo and redo replay stored accepted results without
running the model again.

Input limits
------------

The workflow targets solid flat cel painting. Animated and procedural palettes
need a separate policy and are rejected where native style identity cannot be
preserved. Equal-colored native styles can require explicit region decisions.

Vector analysis uses the model's analysis scale. Tiny or hidden regions omitted
at that scale are reported and left unchanged. An open contour may have no native
fillable region; repair it with the existing vector tools and regenerate.
Complex paths or regions lost during model preparation can fail safely with an
error. These cases require real-art validation before production use.

Computer guidance
-----------------

Practical planning targets are Windows 11 x64, a modern four-core or better
processor, 16 GB system memory, and an SSD with about 10 GB free. Large scenes
benefit from 32 GB memory. These are recommendations pending integration
benchmarks, not established minimum requirements.

CPU processing is supported. Compatible DirectX 12 hardware can accelerate
inference through DirectML. A particular GPU brand or CUDA installation is not
required. An 8 GB graphics-memory target is provisional; no minimum graphics
memory or frame latency has been established for this integration.

The universal models occupy approximately 1.89 GB. Adding Windows accelerator
models increases this to approximately 3.52 GB. Downloads, updates, source
drawings, and temporary working data require additional space. Preloading avoids
some model-loading delay; it does not remove inference work.

Validation
----------

Release validation must include native preservation, exact undo and redo,
save/reopen, source/reference invalidation, offline use, cancellation, worker
failure, model corruption, memory limits, CPU execution, and DirectML execution
on actual compatible hardware. Passing a build or a worker health check alone
does not establish coloring quality. Artist evaluation on representative shots
is required before calling the workflow production-ready.
