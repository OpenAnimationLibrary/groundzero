# Plugin storage ownership: proposed R17 repair

Status: source review and proposed design only. No implementation, build, runtime
measurement or baseline/mutation run is supplied by this document. This proposal
is separate from the prepared loader/menu lifetime repairs. It does not attribute
the Windows text-layout crash to plugin storage.

## Confirmed allocation and borrower paths

References below describe the reviewed `pluginhost.cpp` after the loader/menu
lifetime changes, based on the `2b98b5bac` audit revision.

| Storage | Allocation and reader | Retirement | Actual repeat boundary |
| --- | --- | --- | --- |
| `UIPage` and its `Group` objects | `create_host` and `PluginDeclaration::create` call `notify` (`pluginhost.cpp:407`), then `createParamsByDesc` (`:877`). Every descriptor page calls `createUIPage` (`:596`), which appends a raw allocation to shared `PluginInformation::ui_pages_`. `UIPage::begin_group` allocates each group. | Host destruction (`:418`) releases the metadata reference but never removes UI pages. The metadata destructor (`:146`) also does not delete these raw objects. | Creating and deleting ordinary plugin FX instances grows shared layout storage even when no FX Settings panel opens. |
| `ParamView` and components | Each successful parameter in `createParamsByDesc` allocates a `ParamView` (`:637`) and a shared component. `UIPage::Group` borrows that view pointer and copies the parameter name (`plugin_ui_page_interface.h:19,28,75`). | `param_views_` never shrinks or deletes its pointed objects. | The same ordinary FX creation cycle adds one view per parameter. |
| Actual Qt settings widgets | `RasterFxPluginHost::build` traverses all accumulated UI pages (`pluginhost.cpp:645`). `ParamsPage::newParamField` creates a field with the page as parent (`fxsettings.cpp:756`), and `addParamsPage` places pages under owned scroll areas (`:1380,1388`). | Qt page/viewer ownership retires these widgets. `ParamViewer` intentionally caches one page set per FX type/version (`:1767`). | A newly created viewer can build duplicate parameter tabs after many host creations; reusing an existing viewer need not rebuild or increase its widget count. |
| Nested group/descriptor arrays | `setParamStructure` allocates `dst_page.array` and `dst_group.array` with raw `new[]` (`pluginhost.cpp:812,821`). The SDK structs contain raw pointers (`toonz_params.h:231,239`). | Assigning `param_pages_` at `:871` deletes only the old top-level page array. No nested-array deleter was found. | Repeated accepted descriptor replacement, including the existing sixteen-replacement probe fixture, loses old nested arrays. A plugin that sets its structure once has only its current reachable tree. |
| Copied strings | `patch_string` appends owning strings to `param_string_tbl_` (`pluginhost.cpp:788`). Descriptors borrow their `c_str` addresses. | The vector is not cleared on replacement. Its lifetime is the metadata object's lifetime. | Repeated setup/replacement accumulates prior strings; ordinary host creation does not call this path. |
| Enum/spectrum trait arrays | Current `param_resources_` owns typed array allocations. Replacement clears those owners before constructing the new tree (`pluginhost.cpp:801`). | Old trait arrays are released before successful publication of the replacement. | With live descriptor borrowers, replacement or an allocation failure during replacement can invalidate old enum/spectrum data. This is a conditional API-lifetime hazard, not a demonstrated ordinary artist workflow crash. |

`Param` stores a borrowed descriptor pointer (`plugin_param_interface.h:18,24`).
Host cloning reads it to create new parameters (`pluginhost.cpp:460`), and plugin
parameter accessors read its tag/traits (`plugin_param_interface.cpp:281,319,337`).
Therefore deleting all old descriptor/string storage on replacement is unsafe.

The ordinary repeat cycle for the first two rows is adding an enabled plugin FX,
removing it, releasing undo/clipboard owners, and adding another. A panel need not
be opened to reach the layout allocations. The current source has no corresponding
retirement. No byte slope or process RSS result has been measured.

## Refuted or narrower claims

- Process-lifetime plugin registration alone is not per-frame or per-host growth.
  The retained registration is needed by the persistent FX factories.
- `RasterFxPluginHost::clone` creates parameter wrappers and ports, but explicitly
  avoids `createParamsByDesc`; it does not append these UIPage/ParamView objects.
- Existing Qt settings pages are parent-owned. Their deliberate per-type viewer
  cache should not be purged merely because process memory remains above startup.
- Descriptor replacement happens inside the plugin setup callback during ordinary
  discovery. The current host interface does not enforce setup-only use, but no
  real plugin performing late replacement was established in this pass.
- `UIPage::Group` keeps parameter names and ParamView pointers, not live Param or
  host pointers. A built Qt field receives real TParam ownership independently.
- F40's array-deleter correction and F48's metadata reference balance do not fix
  the allocations above. The registry/menu repair does not claim to fix them.

## Recommended first change: remove redundant UI layout allocations

The smallest repair with no new shared ownership state is to build Qt pages
straight from the existing descriptor tree when `RasterFxPluginHost::build` runs.
The current enabled path always adds the same generic parameter field per
parameter. The old UI-page and ParamView interfaces are not exposed by the active
`query_interface` switch; their UUID entries/cases are disabled.

The proposed implementation would:

1. Keep `createParamsByDesc` responsible for real parameter construction/defaults.
   Stop allocating UIPage, Group, ParamView and component objects there.
2. In `build`, walk descriptor pages/groups in their current order, create each
   ParamsPage, add the same generic fields for keys that produced real parameters,
   and preserve page
   labels, group labels, field defaults, and the Version page.
3. Keep Qt widget ownership under ParamsPage/ParamsPageSet. Do not clear an active
   viewer or its cached fields when another FX instance is created or destroyed.
   Existing fields store parameter references and are rebound by
   `ParamsPage::setFx`; they do not require UIPage/ParamView lifetime.
4. Remove the now-unused internal UIPage/ParamView storage and builder path after
   verifying all callers again. Preserve the published plugin SDK structures and
   callbacks. This is not a new UI mode or a plugin-unload mechanism.

Alternatives are to make the layouts explicitly per-host (which also requires
correctly cloning their borrowed view links), or to create one immutable layout
per plugin at startup (which introduces publication/replacement state). Both add
ownership relationships that direct Qt construction avoids. The descriptor tree's
own lifetime remains the separate boundary below; this first change must not
claim to fix late descriptor replacement.

## Separate descriptor repair requiring ownership review

The compatible option for existing borrowers is a private immutable descriptor
generation. It would own the top-level pages, every group/descriptor array, trait
arrays and strings. Construct and validate a whole generation off to the side;
only publish it after construction succeeds. Do not clear the current generation
before building the next one.

`PluginInformation` would retain the current generation. A host would capture its
generation when binding real parameters, not when constructing the temporary
setup host before any structure exists. Clones would copy the source host lease
before reconstructing parameters. Keep that lease alive through destruction of
all Param wrappers; add a separate Param lease only if a borrower can outlive its
host. Each descriptor pointer would remain inside the retained generation.
Existing hosts would keep their original descriptor data; later host creation
would use the new generation. A superseded generation would retire only after its
last host/Param borrower. Qt viewers retaining a host through their FX/parameter
handles must remain valid. This internal data-model change needs review before
implementation, including how a cloned host preserves its exact generation.

An alternative is to restrict replacement to setup before publication. That is
smaller but changes accepted callback behavior and requires an explicit plugin
contract decision. Immediate recursive deletion without borrower retention is
not a compatible repair. Neither alternative requires changing public SDK struct
layouts or adding a user preference.

## Proposed CI scenarios

Use the existing plugin probe fixtures and native UI test route; add no CI job.
These are proposed tests, not authored or passing cases.

- Create/drop at least 32 real hosts, then build a new settings viewer. Require
  exactly the declared parameter page/group/field counts and one Version page,
  with unchanged enum/spectrum values. The old layout accumulation must fail.
- Keep one viewer and host alive while other hosts are created and retired. Verify
  field identity, edit binding, values and current host remain valid. Destroy the
  viewer and observe its widget QPointers retire.
- Clone a live host before and after viewing its settings; verify parameter
  values, layout counts, and destruction in both original/clone orders.
- For a future generation implementation, replace descriptors while an older
  host and its clone remain live. Check old and new values independently, then
  assert old storage retires only after its last borrower. Include a failed
  staged replacement that leaves the previous structure intact.
- Use a narrow old-behavior mutation or baseline binary to prove detection.
  Allocation/lifetime evidence is preferable to a universal RSS threshold.

Remaining untouched paths include malformed SDK payload validation, duplicate or
failed plugin publication cleanup, process-exit finalization, and legacy disabled
UI entry points. These require their own reachable scenarios before fixes.
