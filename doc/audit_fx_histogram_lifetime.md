# FX histogram lifetime review

This proposal is under review and is not ready for approval or implementation.
Independent review found a startup-reentrancy blocker: renderer startup pumps
events before admitting workers, so a first New Scene or Quit handled there can
deadlock in the proposed synchronous drain waiting for its own paused startup.
The scene-transition guard does not prevent that first entry. A separate reviewed
startup boundary is required; none of the proposed histogram changes is implemented.

Status: source review and proposed repair for R22. No implementation, local build,
runtime reproduction, allocation measurement, or regression result is claimed here.
The inspected source is `08c355522`; this proposal needs review before changing the
histogram retirement protocol. The proposed entry points below do not exist yet.

## Observed ownership and calls

The intended owner is a parameter page, whose lifetime can be longer than a panel's
visible interval. `FxSettings` owns its `ParamViewer` (`toonzqt/fxsettings.cpp:1880`),
which retains page sets by effect-layout key (`:1731`). A page enters a scroll area's
ownership with `QScrollArea::setWidget` (`:1400`); the scroll area enters the page
stack. Keeping these pages while the viewer is alive is intentional caching. Merely
hiding or reopening a cached settings panel does not prove a new allocation or leak.

Actual retirement occurs when the viewer/parent hierarchy is deleted, when a changed
SeExpr layout schedules its previous page set for deletion (`:1757`), or when a newly
created empty page is rejected (`:1438`, `:1524`). The latter paths allocate a histogram
controller even when no histogram widget or render ever exists.

| Resource | Allocation and ownership | Retirement evidence |
| --- | --- | --- |
| Histogram controller | `ParamsPage` allocates `new FxHistogramRender()` at `toonzqt/fxsettings.cpp:174`. The controller calls `QObject()` without a parent at `toonzqt/fxhistogramrender.cpp:48`. The page's raw member is not transferred elsewhere. | `ParamsPage::~ParamsPage` is empty at `fxsettings.cpp:192`. No matching delete, QObject parent assignment, or other owner was found. Each destroyed page leaves its controller and renderer implementation retained. |
| Render port | The controller allocates `FxHistogramRenderPort` at `fxhistogramrender.cpp:54` and registers its raw address with `TRenderer`. | The controller destructor deletes it at `:68`, but that destructor is not reached through the page. It neither removes the registered port nor drains work. Simply starting to call this destructor is unsafe with active callbacks. |
| Last input FX | `FrameInfo::m_fx` is a `TFxP`; `computeHistogram` assigns the input at `fxhistogramrender.cpp:120`. | No completion/reset assignment clears it. With the orphan controller, the input graph stays referenced. This can retain a plugin instance/generation; the exact size is scene-dependent. |
| XML histogram widget | The `<histogram>` branch creates `Histogram` and adds it to the page's layout at `fxsettings.cpp:441`. `Histogram` creates `Histograms(this)` at `toonzqt/histogram.cpp:547`. | Layout/widget parenting gives this hierarchy an owner. The controller nevertheless retains a raw `Histograms*` after page deletion. This widget is not itself an unowned allocation. |
| Tone-curve histogram storage | `ToneCurveField` creates `new Histograms(0, true)` at `toonzqt/tonecurvefield.cpp:850`. It passes the object to the controller and passes its views to curve editors. | The object is never inserted into a layout, parented, stored as an owning member, or deleted. `m_toneCurveStackedWidget->addWidget(c)` at `:856` owns the curve editor, not the separate histogram storage. Its raster member and child views survive editor destruction. |
| Vertical channel bars | Each `ChennelCurveEditor` allocates `new ChannelBar(0, ...)` at `tonecurvefield.cpp:126`. | All uses are size/range updates (`:197`, `:205`) and explicit painter drawing (`:648`). No layout insertion, parent assignment, owning smart pointer, or destructor delete was found. There are six editors in a tone-curve field. |

The borrowed pointer chain is also unsafe after retirement. A worker invokes
`FxHistogramRenderPort::onRenderRasterCompleted`, which clones its raster and emits
`renderCompleted` (`fxhistogramrender.cpp:35`). The GUI-affinity controller receives the
cross-thread signal through the default queued delivery and eventually calls
`m_histograms->setRaster(raster)` at `:207`. For XML histogram pages that pointer can
already be dangling. For tone-curve pages the orphan histogram remains, but its
`HistogramView::m_drawnWidget` still points at a destroyed curve editor: construction
sets that borrow at `tonecurvefield.cpp:123`, and `HistogramView::setValues` calls
`m_drawnWidget->update()` at `histogram.cpp:391`. These are source-established lifetime
violations; this review has not reproduced a crash or attributed an existing dump to them.

## Pending requests and the existing renderer contract

`TRenderer::startRendering` posts a request to `TRendererStartInvoker` and immediately
returns a render ID (`common/tfx/trenderer.cpp:850`; queued connection in
`common/tfx/trendererP.h:41`). The queued invoker holds a renderer reference and the
request owns its FX graph until delivery (`trenderer.cpp:1586`, `:1599`). Accepted
tasks then retain their renderer/FX references until their completion paths retire.

The available cancellation API is useful but is not a complete synchronous drain of
all submitted requests:

- `abortRendering(id)` only marks an entry already present in `m_activeInstances`
  (`trenderer.cpp:983`). It does not remember cancellation of a queued start.
- `stopRendering(true)` cancels current active instances and pumps events only while
  `m_undoneTasks > 0` (`:993`). A queued start before admission can therefore survive
  a call that returns immediately. This is also documented by the existing SVG
  `RenderSession::drain` caller (`toonz/animatedsvgexport.cpp:325`).
- `removePort` changes the registered list under a write lock (`trenderer.cpp:951`).
  Notifications copy raw port pointers under a read lock, release the lock, and then
  invoke them (`:1058`). Removal does not invalidate an already copied callback target.
- `TRenderer::~TRenderer` only releases its implementation reference (`:661`). It is
  not a stop/wait operation. Worker and queued-start references can outlive that wrapper.
- `RenderTask::onFinished` runs on the GUI thread after the worker's frame callback
  returns (`:1430`). Terminal notification follows retirement of that request's accepted
  workers (`:1465`); startup failure/cancellation also has a terminal path (`:1594`).
  The histogram port currently declares `onRenderFinished()` without the base method's
  `bool` argument, so it does not override `TRenderPort::onRenderFinished(bool)`.

Parenting the controller, deleting it in the page destructor, or combining port removal
with a nonblocking stop does not establish safe callback retirement. Adding only
`stopRendering(true)` also misses the queued-start interval and pumps arbitrary events
inside an object destructor for active work. No global renderer redesign is proposed.

## Related state within R22

These are coordinated subcases of the histogram pipeline, not additional counted fixes.

1. `updateRenderer` appends the previous render ID to `m_abortedRendering` at
   `fxhistogramrender.cpp:149`. Its only removal is inside a subsequent successful
   completion carrying that same ID (`:196`); its only clear is construction (`:57`).
   `m_lastFrameInfo.m_renderId` is overwritten by new starts and is not reset at terminal
   completion. After render A has completed, an edit appends A, and no future A callback
   removes it. Repeating settled edits retains one obsolete ID per edit. Cancellation
   failures likewise do not clear IDs because the failure callback is empty.
2. A completion is not compared with `m_lastFrameInfo.m_renderId`. Submitting a different
   input/frame through `computeHistogram` can therefore let an older completion publish
   under the latest input/frame identity. A null-input request clears only the displayed
   raster, not the pending request or saved FX reference. This requires a result-order
   regression; no measured wrong image is claimed yet.
3. The cache existence check uses `alias + ".noext" + frame` (`:107`), while lookup and
   insertion use `fxIdentifier + ".noext" + frame` (`:123`, `:203`). The lookup also
   dereferences its result without checking it. The normal write does not establish the
   key that the next read checks. Repairing lifetime alone must not be presented as fixing
   cache reuse or stale-result identity.

## Scene and exit boundaries

The built graph is not independent of the scene. `AffineFx` stores borrowed `TXsheet*`
and `TStageObject*`, copies them in `clone`, and dereferences them in worker placement
calculations (`toonzlib/scenefx.cpp:186`, `:202`, `:214`). `FxBuilder` inserts this wrapper
for input column movements (`:710`, `:1131`, `:1375`). Plastic deformation also keeps a
borrowed xsheet (`:744`; `toonzlib/plasticdeformerfx.cpp:272`, `:281`). Holding the cloned
FX graph alone therefore does not protect all its dependencies.

A `sceneSwitched` connection alone is too late for normal New Scene and accepted Quit.
`saveSceneIfNeededImpl` calls `scene->clear()` at `toonz/iocommand.cpp:1887`, before later
handle replacement. `ToonzScene::clear` clears its child stack and replaces its properties
(`toonzlib/toonzscene.cpp:363`); `ChildStack::clear` calls `TXsheet::clearAll`
(`toonzlib/childstack.cpp:30`), replacing the stage-object tree (`toonzlib/txsheet.cpp:446`).
The existing `RenderingSuspender` around this clear already drains Previewer,
PreviewFxManager and SwatchViewer (`iocommand.cpp:1733`, `:1884`). This is the specific
pre-clear integration point for histogram work.

The existing scene pin solves a different boundary. `TSceneHandle::retainScene(scene)`
(`toonzlib/tscenehandle.cpp:52`) is a static API available to `toonzqt`; the histogram
implementation already includes that header. It can pin the exact `ToonzScene*` used to
build a request without calling application-layer `TApp`. When low-level `setScene`
replaces a scene, its delayed retirement record and this lease keep the old owner alive
until the request releases its lease (`tscenehandle.cpp:75`). The lease does not prevent
`scene->clear()` or in-place xsheet mutation, so it does not replace the pre-clear drain.

Accepted Quit already has a `beforeClear` callback in `MainWindow::closeEvent`
(`toonz/mainwindow.cpp:2130`). Later it clears `TImageCache`, calls `TThread::shutdown`,
and quits (`:2166`). `aboutToQuit` is too late to be the only histogram boundary. The
post-event-loop fallback in `toonz/main.cpp:1044` handles script/native-runner exits that
bypass `closeEvent`, before undo/preview cleanup. Both existing shutdown sites must drain
histograms while scene, GUI and thread services are still available.

## Proposed ownership and request protocol

Keep ownership inside the histogram component and reuse existing renderer and scene-pin
APIs. The proposed component-local suspension entry point follows the existing
`SwatchViewer::suspendRendering` pattern; it is not a new application ownership manager.

1. Parent the controller to its `ParamsPage` while live. Parent tone-curve `Histograms`
   to the field and each manually drawn `ChannelBar` to its editor, explicitly hiding
   these backing widgets. Use `QPointer<Histograms>` for the controller's UI borrow.
2. Give the controller a weak `TSceneHandle` supplied through the existing
   `FxSettings::setSceneHandle` path, forwarded through `ParamViewer` and page creation.
   Bind every cached page, including pages created after the handle was set. Keep the
   controller's scene-switch connection active while its page is hidden. A scene switch
   clears its prior input/desired request and invalidates result publication; select the
   new scene only through the handle. Reject a result if that handle is gone or its
   current scene differs from the request's scene, including `setScene(nullptr)`, which
   currently emits no `sceneSwitched`. No `TApp` dependency enters `toonzqt`.
3. Allow one submitted renderer request per controller, plus one replaceable latest
   desired request. An edit invalidates the old result, cancels the in-flight render,
   and replaces the desired request. After terminal delivery, build/submit only the latest
   still-valid request. Do not queue every intermediate slider position. This preserves
   the final selected histogram and bounds queued request ownership. It also avoids
   overlapping starts overwriting the renderer's shared `rootFx` (`trenderer.cpp:1709`).
4. Before `startRendering`, mark the submitted slot occupied and acquire
   `TSceneHandle::retainScene(scene)` for that exact scene. Retain the lease through
   terminal delivery, including the queued-start interval. The desired request also
   retains its scene while waiting; clear it on scene change, retirement or shutdown.
   Capture request ID, frame, input identity, build mode, output area, settings and cache
   identity for that submission. Do not interpret a later result using mutable latest
   input/frame state. Keep IDs as `unsigned long`, matching `TRenderer`.
5. Override `FxHistogramRenderPort::onRenderFinished(bool)` exactly. Post its terminal
   signal using `Qt::QueuedConnection`; the receiving slot runs after the port callback
   has returned. With one submitted request, this ID-less renderer terminal callback is
   unambiguous. Clear the submitted slot and scene lease there. Handle synchronous
   submission failure/sentinel return by rolling back the slot/lease, and handle startup,
   worker failure and cancellation through their real terminal path. Never count a raster
   result as terminal. Remove the historical `m_abortedRendering` list.
6. Publish a raster only when its render ID matches the submitted request and its desired
   generation is still selected, the scene handle still matches, and the UI borrow is
   alive. Null input invalidates prior results, clears the saved input/desired request and
   displayed raster, and cancels submitted work without destroying its port or scene lease.
7. `ParamsPage` retirement first detaches its controller from QObject parent ownership,
   rejects further work, invalidates publication, clears UI/desired-input borrows and
   releases its cache entry. Then request cancellation. If no submission exists, the
   page can destroy the detached controller immediately. Otherwise the controller owns
   its finite retirement interval; after terminal delivery it removes the port and uses
   deferred deletion. The port remains registered/alive until its worker callback and
   terminal callback have both returned. No event pumping occurs in a widget destructor.

Maintain a component-private, non-owning list of live controllers from construction to
actual destruction. Retiring controllers remain in the list. Snapshot it with `QPointer`
when draining because queued event service can delete pages or create new ones. Registering
while admission is closed cannot start work. The list is observation/cancellation state;
it does not become a second owner of pages, scenes or controllers.

## Concrete suspension and shutdown order

Add `FxHistogramRender::suspendRendering(bool)` with nested suspension depth, plus terminal,
idempotent `FxHistogramRender::shutdown()`. Do not modify the shared `TRenderer` API.

For a temporary suspension:

1. Increment the component admission depth before serving any events. Controllers reject
   new submissions while depth is nonzero; do not retain queued edit intents from nested
   UI activity during the mutation boundary. Invalidate outstanding result publication,
   discard their desired successor, and cancel every submitted renderer.
2. While any registered controller still has a submitted slot, repeatedly call its
   `stopRendering(false)` and process `QEventLoop::ExcludeUserInputEvents` in short slices.
   Repeating cancellation covers a start that was only queued on the previous iteration.
   Recheck weak pointers after every event-service call. Use terminal slot state, not
   `m_undoneTasks` alone, as the exit condition. Do not hold the histogram mutex while
   serving events. A nested drain joins the same closed admission interval.
3. After all submitted slots clear, remove ports only for controllers actually retiring.
   Drain their deferred deletion from the safe outer drain context, after callback stacks
   return; live cached pages retain idle controllers. Do not release admission from a
   terminal callback or page destructor. Resume decrements depth, reopening only at zero
   and only if terminal shutdown has not been requested. Resume itself starts no render;
   subsequent valid page/input/frame notifications request fresh work.

Wire this into existing `RenderingSuspender`: suspend histograms first, before the other
renderers can pump events, and resume them last in its destructor. Nested guards occur in
New Scene/load paths, so an inner guard must not reopen admission. The existing outer
`SceneTransitionGuard` continues to prevent reentrant scene commands while these drains
serve events; add no second scene-transition framework. Keep displayed completed pixels
on a canceled scene prompt. Live-page input selection can remain available for later edits;
scene-switch invalidation releases it when a replacement actually occurs.

For accepted Quit, call histogram `shutdown()` in the existing `beforeClear` callback,
then the existing icon shutdown, before `saveSceneIfNeededImpl` clears scene contents.
Call it again, harmlessly, beside the subsequent guarded icon drain in `closeEvent`,
before cache clear and `TThread::shutdown`. Repeat the same histogram-then-icon order in
the `main.cpp` post-event-loop fallback. Terminal shutdown closes admission permanently,
discards desired requests, drains submitted requests, releases source/scene/cache state,
clears borrowed UI references, and removes idle ports before returning. Retiring controllers
must be destroyed from the safe drain context instead of depending on a future event-loop
turn. Still-page-owned idle controllers may remain until ordinary QObject teardown; they
hold no render port, scene lease, source FX or cached raster. Repeated calls do nothing.

Normal low-level `TSceneHandle::setScene` replacement invalidates/cancels the old controller
request through its persistent connection; the submitted request's existing scene lease
keeps its borrowed dependencies alive until terminal delivery. This path needs no event
loop inside the handle or controller destructor. It cannot authorize in-place `clear()`
outside the established pre-clear suspension boundary.

A third-party FX that never returns can still prevent a cooperative drain from completing.
No forced worker termination or false timeout-success path is proposed. CI must distinguish
a completed drain from a fixture timeout.

## Cache and pixel compatibility decision

Use a histogram-only cache entry for each controller, allocated with the existing
`TImageCache::getUniqueId()` under a histogram prefix. Keep only the current completed
entry; replace/remove that exact entry when the selected input/frame changes, on explicit
invalidation, scene change, retirement and shutdown. Store its input numeric identifier,
frame, built-FX alias, build mode, render settings and render area as identity metadata.
Use the same owned key for insertion, lookup and removal. Fetch once with `get(key, false)`
and check the resulting `TRasterImageP`/raster; an evicted or wrong-type entry is a cache
miss. Never dereference a prior membership check's assumption.

A hit requires the same request identity and output geometry/settings, not only the input
FX numeric identifier. Compare pre-submission settings using `TRenderSettings::operator==`
and separately compare `m_cameraBox` and `m_getFullSizeBBox`, which that operator omits
(`tnzbase/trasterfx.cpp:1338`), as well as the actual render area/build mode. Do not retain
worker-only cancellation/offscreen-surface pointers in cache identity. An edit explicitly
invalidates the current entry even if a plugin supplies an unchanged alias. Cache metadata
contains no owning input FX reference. No cross-frame cache-index/history is added.

The existing numeric-FX/`.noext`/frame key belongs to Preview FX too
(`toonz/previewfxmanager.cpp:104`). Its producer can transform linear pixels to sRGB,
dither 64-bit output, and merge cropped tiles (`:1084`, `:1095`, `:1110`, `:1125`). Merely
changing the histogram's existence check to that numeric key could turn an existing miss
into reuse of pixels different from a histogram render. Therefore histogram lookup,
publication and invalidation must not read, overwrite or remove those Preview FX entries.
There is no legacy-cache migration or purge; this is derived session data. Unrelated keys
remain untouched.

Keep the existing initial-render versus explicit-refresh graph-building choices, camera
mode, preview settings, active checks, raster clone and histogram channel computation.
This proposal does not change color conversions, image precision or scene rendering to
make cache reuse pass. The native fixture must compare exact channel counts for a fresh
render and a cache hit, including a conflicting Preview FX entry under the old numeric key.

## Required CI scenarios

Use the existing native/UI qualification route; add no PR job. The test should create actual
`ParamsPage`/`ToneCurveField` instances and real renderer work. A small controllable raster FX
can provide entered/release signals at the worker boundary; do not mock renderer callbacks.

| Scenario | Required observations |
| --- | --- |
| No histogram/no render | Repeatedly create and destroy a page, including an empty rejected page. Observe controller and port `QPointer` destruction; no per-page renderer survives. |
| Tone-curve page | Capture the actual histogram storage, six views, and vertical bars; delete the field/page. Every owned QObject disappears, the backing widgets stayed hidden, and another live page remains usable. |
| Completed work | Render a fixed known image and check channel counts. Delete the page after terminal delivery; the retained source FX and output raster release when their last legitimate owner retires. |
| Queued start | Submit, destroy the page before processing the start event, then drain the GUI queue. No stale widget/cache publication, no lost terminal count, no retained source/controller/port afterward. |
| Active worker | Gate a real FX worker, retire the page, release the gate, then observe terminal cleanup. The port stays alive until the worker callback returns; the deleted page receives no update. |
| Result awaiting GUI delivery | Finish worker raster computation, retire the page before its queued result is delivered, then process events. No access to the old histogram or curve editor. |
| Failure/cancellation | Exercise startup failure, worker failure, active cancel, and queued admission followed by cancel. All accepted requests retire once; ownership checks are not satisfied by timing out or skipping work. |
| Repeated settled edits | Render to completion before each of many edits. Check bounded request state and the final histogram, without an accumulating historical ID list. |
| Superseded input/frame | Gate a submitted request and make many edits with different pixels/frames. Only one submission and the latest desired request survive; only the final selected generation is displayed/cached. Null input does not resurrect prior output. |
| Cache hit/eviction | Check exact fresh-render/cache-hit channel counts, explicit invalidation, eviction, wrong-type entries, changed geometry/settings and a conflicting Preview FX key. Verify the Preview FX entry and unrelated keys remain unchanged. |
| Scene clear/replacement | Exercise actual New Scene and load with a worker using column/plastic dependencies; drain before clear replaces their stage tree. Include hidden cached pages, retired controllers, nested suspensions, canceled prompts and direct handle replacement while draining. Prove the old scene lease outlives its submitted worker and releases afterward. |
| Exit | Exercise accepted Quit with a queued-only start and an active worker, plus the direct event-loop-exit fallback. Verify idempotent shutdown finishes before cache/thread teardown, destroys retired controllers without another event-loop turn and rejects nested new submissions. |

Each ownership regression should fail against the corresponding old allocation or missing
retirement behavior, then pass with the repair. A source-only leak finding or `QPointer`
fixture does not establish a process-wide RAM decrease. No fixture in this table has been
implemented or executed by this review.
