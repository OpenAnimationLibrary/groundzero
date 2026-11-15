# Plugin parameter layout and descriptor ownership

Status: both stages of this design were approved and are implemented in the draft.
Compilation, runtime regressions and baseline/mutation qualification remain pending
in CI. The source review and authored cases below are not measured RAM improvement
or a leak-free application claim. No Windows text-layout crash attribution is made.

## Repaired ownership paths

Ordinary `create_host` and persistent-factory creation call `notify`, which binds
parameters. Previously, each call also appended raw UIPage/ParamView objects to
shared PluginInformation storage. Host destruction never removed them. Repeating
FX creation therefore accumulated layout objects even without opening settings;
a later settings viewer built all accumulated pages. Cloning used a separate
parameter path and did not itself append these layouts.

`RasterFxPluginHost::build` now constructs the same generic Qt fields directly
from the host's descriptor generation. Page/group order, labels, parameter defaults
and the Version page are preserved. The obsolete UIPage/ParamView classes, their
internal interface files and builder calls are removed. ParamsPage/ParamsPageSet
continue to own actual widgets through Qt. Creating or deleting another host does
not clear an existing viewer's page set.

Descriptor copying previously owned only the top-level page array. Replacements
lost raw nested group/descriptor arrays, retained previous strings, and cleared
enum/spectrum storage while older Param objects could still borrow descriptors.
The new private `PluginParameterStorage` owns every page, group/descriptor array,
string and trait array. A complete validated generation is constructed before
atomic publication to PluginInformation. Allocation failure leaves the previously
published generation intact; the SDK boundary returns its existing out-of-memory
or unknown-error result instead of propagating a C++ exception.

A host captures a shared immutable generation when binding real parameters. The
temporary setup host exists before a descriptor tree is available and does not
prematurely capture an empty generation. A clone retains its source host's exact
generation before rebuilding parameter wrappers. The host's lease is declared
before its Param wrapper collection, so wrappers retire first. Plugin destroy
callbacks run while both wrappers and their descriptor lease remain alive.
Exceptional host/clone construction retains its existing behavior; it is a separate
residual from transactional descriptor copying.

Param stores a const borrowed descriptor pointer. No independent shared Param
owner was found outside the host; SDK handles remain borrowed from that host.
The public plugin SDK structs and callbacks are unchanged. This implementation
does not impose a new setup-only restriction or add plugin unloading.

## Viewer caching and active borrowers

The viewer cache includes the private generation identity alongside its existing
FX type/version key, including plugin generations nested inside macros. Persisted
FX versions are unchanged. Switching generations creates a separate page set;
switching back reuses the old page and field identities.

Cached ParamsPage objects retain their bound current/actual hosts through TFxP.
Consequently a visited generation remains intentionally alive until its cached
pages and other hosts/clones retire. That also keeps the private pointer identity
stable while its cache entry exists. Actual Qt fields copy strings/enum labels and
retain parameter references; they do not borrow the removed UIPage/ParamView data.

An active histogram renderer may retain an input FX independently. Its ownership
is a separate audit boundary. The cases below do not submit histogram rendering
and do not establish retirement of every possible renderer or third-party owner.

## Authored behavior checks

The existing `plugin_probe_single` CTest invokes a helper compiled into toonzqt
only with `BUILD_RELIABILITY_TESTS`. No CI job or standalone test target is added.
The existing probe, optional destroy callback, enum/spectrum and controller/menu
checks remain intact.

- Thirty-two real host creation/destruction cycles precede settings construction.
  Two declared parameter pages plus Version must produce exactly three pages and
  four parameter fields, with enum/spectrum/string/integer values unchanged.
- Descriptor source strings are changed after successful publication and all
  source arrays leave scope before host creation. Copied labels/defaults remain
  readable from new hosts, old hosts, clones and destroy callbacks.
- A test-only allocation checkpoint throws after the first page of a two-page
  replacement has been copied. The SDK must return out-of-memory, preserve the
  original generation, and leave old/newly created hosts and an existing viewer
  using the original values. Nested null arrays and negative/oversized counts
  also reject replacement without changing publication.
- Isolated clone tests replace the published generation, retire original/clone
  in both orders, clone the survivor, then remove all earlier borrowers. A weak
  generation reference must remain live for the final clone and expire after it.
  These checks do not rely on a viewer retaining the original host.
- Two real viewers retain old settings while one switches to the new generation.
  The old field identity and binding remain valid; returning to it reuses the
  cached page. After direct hosts/clones are released, weak generation and widget
  QPointers remain live until the final cached viewer is destroyed, then retire.
- The fixture's destroy callback reads its parameter descriptor, exercising the
  callback-before-wrapper/lease-retirement ordering.

The allocation checkpoint is restricted to test builds and has no production
configuration or counter. These scenarios are authored, not yet executed. The
recursive macro cache key is source-reviewed; a rendered macro scenario has not
been added. Exact baseline/mutation evidence is still required before claiming
that all checks detect their respective old defects.

## Remaining limits

Process-lifetime plugin registration and the required library/declaration owners
remain intentional. Existing plugin user-data aliasing in clone, plain metadata
reference-count concurrency, broader malformed SDK payload validation, duplicate
or failed publication cleanup, and process-exit finalization are separate issues.
Failed host/clone construction can still retain the partially built host and its
generation. Factory and clone ownership are unchanged because deleting a partially
constructed host could invoke an SDK destroy callback before create/user-data
initialization; the setup-only callback contract also remains unchanged. The
synthetic callback tolerates null user data, but third-party create/destroy
exception behavior has not been qualified. No active rendering, process
RSS trend, sanitizer run or platform pass is asserted by this source change.
