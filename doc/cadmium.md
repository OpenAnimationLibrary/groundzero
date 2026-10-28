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
4. Check the painted references assigned to each target. Select one or several
   marked drawings and choose which reference to display during review.
5. For full-color artwork, choose transparent line art, approved paper extraction,
   or a linked clean-line level. Inspect extraction before approving it. Painted
   flattened references generally need clean-line input or approved extraction.
6. Generate proposals. Generation does not modify drawings.
7. Inspect each result, select regions directly in the comparison view, resolve
   conflicting fills, and protect or skip regions. Mark the result reviewed
   before applying a reviewed batch.
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
image geometry are retained. The model uses display-color proxies; accepted
full-color fills use exact native reference samples, including high-bit-depth
and HDR values. Distinct native fills sharing one model color require an explicit
reference-fill choice. References without a dominant flat native fill need
another reference or an explicit skip.
Undo and redo replay stored accepted results without
running the model again.

Review and corrections
----------------------

The comparison view provides synchronized before/after inspection, a draggable
wipe, and a reference overlay with adjustable opacity. Zoom, pan, fit, and
full-size inspection use the same drawing coordinates in each comparison.
Click a paint region to select its decision in the region table. The context
menu offers fill choices, skip, protection, and unlocking for regeneration.
Selected and protected regions are highlighted in the comparison view.

Choosing a fill or explicitly skipping a region locks that decision. Protecting
a region keeps its existing artwork and its decision while other regions are
generated. Unlock a decision when it should receive another prediction. Native
palette drawings also support choosing a supported existing palette style.
Full-color choices retain the exact reference samples, including values that
look identical in the model's display-color proxy.

Using several references
------------------------

Each checked reference is processed separately by the existing pretrained model.
Fills supported by every contributing reference can be selected automatically.
Disagreement, missing support, or multiple exact native fills remain unresolved
until the artist chooses a fill or skips the region. The workflow does not
silently average native colors or replace different palette styles with one
display color. Manual locked choices and protected regions remain under artist
control while references are combined.

Adding references increases inference time and may expose additional conflicts.
The reference set is limited to 32 drawings per target. Use a smaller reference
set or batch when the proposal memory budget is reached.

Selective regeneration
----------------------

Generation can target the drawing being inspected, unresolved regions, or the
selected regions. Accepted locked decisions and protected regions are retained
when their region geometry is unchanged. Unlock a region before requesting a
new prediction for it. Regeneration clears the drawing's reviewed status when
its decisions change; inspect it again before applying a reviewed batch.

Region numbers alone do not establish identity after artwork edits. If a
selected region changed or disappeared, generate a new drawing proposal and
select the current region. Vector geometry changes can require whole-drawing
regeneration. Changing reference assignments or native palette information can
also invalidate decisions even when the drawing still looks similar.

Saving review sessions
----------------------

Save the scene before retaining a painting session across application restarts.
Session controls save, restore, and clear reference assignments, proposals,
locked and protected decisions, and review progress. Completed decisions and
generation progress are also autosaved. Recovery uses the last completed
atomic save; an interrupted write does not replace the previous saved session.
Unfinished generation can continue after reopening the saved scene.

Review sessions are stored in local application data and do not embed source
artwork or pretrained models. Scene reference marks remain part of the scene.
A local session does not automatically travel with a scene copied to another
computer. Restoring a session checks its target drawings, references, native
regions, palette, and input settings against the current scene before enabling
application of the proposals. Changed or unavailable inputs require fresh
generation rather than applying stale decisions.

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
