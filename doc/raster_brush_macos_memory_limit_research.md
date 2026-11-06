# macOS whole-process memory-cap research

Status: capability experiment only. The strict whole-process 2 GiB
resident/commit acceptance requirement remains open. No production launcher or
existing resource limit is changed by this work.

The source baseline is Apple's `xnu-11215.81.4`, matching the release family
already referenced by [platform isolation](raster_brush_platform_isolation.md).
The experiment requires macOS 15 or later and records the actual OS version,
build, kernel release and executable architecture. A `macos-15` runner label does
not pin its minor release or kernel implementation. Native arm64 and translated
x86_64 runs must qualify separately; source reasoning alone is not runtime
qualification.

## Findings from primary sources

| Mechanism | Relevant source behavior | Consequence |
| --- | --- | --- |
| `task_set_phys_footprint_limit` | The Mach routine exists, but its implementation first calls `proc_check_footprint_priv` and returns `KERN_NO_ACCESS` on denial. That check uses `PRIV_VM_FOOTPRINT_LIMIT`; the default privilege policy grants root or a MAC-policy grant. There is no self-task exemption in this path. | Possessing `mach_task_self()` does not authorize an ordinary helper to set its own limit. The function also preserves the current fatal/nonfatal state rather than selecting a fatal policy. |
| `memorystatus_control(SET_MEMLIMIT_PROPERTIES, ...)` | The syscall checks root or `com.apple.private.memorystatus` before dispatch. Its unprivileged freeze-status exceptions do not cover memory limits. The release-kernel path has no self-limit exception. | A helper signed only with App Sandbox cannot assume this is an available setter. |
| `posix_spawnattr_setjetsam_ext` | Apple's private spawn header declares macOS 10.11 availability. The wrapper fills spawn attributes. `kern_exec.c` installs active/inactive limits through `memorystatus_set_memlimits` under `CONFIG_MEMORYSTATUS`, without that syscall's root/entitlement check. | This is a concrete candidate for an unprivileged parent to install a fatal footprint limit before child user code starts. It is private SPI, not a documented public resource-control contract. |
| Fatal versus nonfatal limit | Fatal footprint violations invoke synchronous process termination with jetsam reason `JETSAM_REASON_MEMORY_PERPROCESSLIMIT`. Nonfatal high-water limits can be exceeded and become pressure candidates. | The experiment requires both active and inactive fatal flags and the exact kernel exit identity. A successful spawn-attribute call or generic `SIGKILL` is insufficient evidence. |

Sources: Apple's [Mach interface](https://github.com/apple-oss-distributions/xnu/blob/xnu-11215.81.4/osfmk/mach/task.defs#L460),
[footprint setter](https://github.com/apple-oss-distributions/xnu/blob/xnu-11215.81.4/osfmk/kern/task.c#L7385),
[privilege policy](https://github.com/apple-oss-distributions/xnu/blob/xnu-11215.81.4/bsd/kern/kern_priv.c#L71),
[memorystatus authorization](https://github.com/apple-oss-distributions/xnu/blob/xnu-11215.81.4/bsd/kern/kern_memorystatus.c#L8250),
[private spawn declaration](https://github.com/apple-oss-distributions/xnu/blob/xnu-11215.81.4/libsyscall/wrappers/spawn/spawn_private.h#L50),
[spawn wrapper](https://github.com/apple-oss-distributions/xnu/blob/xnu-11215.81.4/libsyscall/wrappers/spawn/posix_spawn.c#L2101),
[exec installation](https://github.com/apple-oss-distributions/xnu/blob/xnu-11215.81.4/bsd/kern/kern_exec.c#L4510),
and [fatal-limit callback](https://github.com/apple-oss-distributions/xnu/blob/xnu-11215.81.4/bsd/kern/kern_memorystatus.c#L4473).

Both the [arm64 macOS configuration](https://github.com/apple-oss-distributions/xnu/blob/xnu-11215.81.4/config/MASTER.arm64.MacOSX#L78)
and [x86_64 configuration](https://github.com/apple-oss-distributions/xnu/blob/xnu-11215.81.4/config/MASTER.x86_64#L64)
include `memorystatus`; their omission of the separate `jetsam` configuration
attribute does not remove this footprint callback. The native exec path ignores
the return from `memorystatus_set_memlimits`, so symbol presence, wrapper success
and spawn success cannot establish enforcement. A runtime negative control is
necessary. A `CONFIG_NOMONITORS` build can also return success without installing
the requested explicit footprint limit.

## Why this does not establish the strict bound

`phys_footprint` accounts for billed anonymous/compressed, nonvolatile purgeable,
IOKit and page-table memory. It is not identical to every resident mapped page.
Apple's own regression tests explicitly expect file-backed mappings to leave
footprint unchanged, and expect making anonymous purgeable pages volatile to
remove their footprint charge. These are relevant coverage tests even when an
ordinary malloc stress case is killed correctly. See the [ledger definition](https://github.com/apple-oss-distributions/xnu/blob/xnu-11215.81.4/osfmk/kern/task.c#L1176)
and [mapped-file/purgeable tests](https://github.com/apple-oss-distributions/xnu/blob/xnu-11215.81.4/tests/vm_phys_footprint.c#L313).

Enforcement also happens after accounting crosses the ledger limit. The ledger
sets an AST to invoke its callback before returning from the kernel. This is a
kernel-backed fatal high-water mechanism, not an allocation reservation check
that proves the ledger never exceeded its limit. A lower threshold leaves
headroom, but closing the unchanged 2 GiB requirement would still require a bound
on in-flight charging, other threads and uncharged resident classes. A sampled
peak, including a successful stress run, is not that proof. See Apple's
[ledger crossing and AST handling](https://github.com/apple-oss-distributions/xnu/blob/xnu-11215.81.4/osfmk/kern/ledger.c#L1039).

`RLIMIT_RSS` is a source-compatibility alias for `RLIMIT_AS` on this baseline;
renaming the existing map-size limit cannot create a separate RSS limit. The
public task VM information query observes accounting rather than installing a
bound. Apple's `os_proc_available_memory` declaration is unavailable on macOS
and, on supported systems, is an instantaneous dirty-memory observation. See
[resource identifiers](https://github.com/apple-oss-distributions/xnu/blob/xnu-11215.81.4/bsd/sys/resource.h#L505)
and [available-memory declaration](https://github.com/apple-oss-distributions/xnu/blob/xnu-11215.81.4/libsyscall/os/proc.h#L46).

## CI-only experiment

`ci-scripts/brush-parser/probe_macos_memory_cap.py` compiles the adjacent C source
with the macOS 15 minimum and the requested architecture, embeds a bundle
identifier, and signs a separate child with exactly App Sandbox. It requires an
unprivileged parent and child; the child must observe denied private-sentinel
reads/writes and denied loopback access before allocating.

The driver explicitly requests `codesign --display --entitlements - --xml` and
retains its sanitized output in `display-child-entitlements.log`. Apple's
[TN3125](https://developer.apple.com/documentation/Technotes/tn3125-inside-code-signing-provisioning-profiles)
explains that the default display is a human-readable DER representation, not
XML. This distinction caused the first CI diagnostic attempt to reject the
display before executing any memory case. Extraction now requires one XML plist
whose sole key is App Sandbox with the actual Boolean value true; integer or
extra entitlement values remain rejected. This corrects diagnostic decoding,
without granting additional authority or qualifying the memory bound.
Run in CI, never as a local-build exception:

```sh
python3 ci-scripts/brush-parser/probe_macos_memory_cap.py \
  --arch arm64 --output-dir macos-memory-cap-probe
```

Use `--arch x86_64` for the translated helper qualification. An uncapped signed
ordinary-allocation baseline runs first. The experimental threshold is the
larger of the requested minimum (128 MiB by default) and that observed footprint
plus 96 MiB, rounded up to 64 MiB. A baseline requiring more than the bounded
1024 MiB threshold fails admission. The stress target is three times the admitted
threshold; the capped ordinary control must still succeed. Startup cost cannot
silently turn a Rosetta baseline failure into enforcement evidence. These are
experimental admission settings, not a full-size 2 GiB qualification.

The experiment covers:

- Ordinary small allocation and denied attempts to raise the footprint limit
  through both self-task and memorystatus setters.
- An uncapped malloc control that must complete the same stress target.
- Capped incompressible malloc, Mach allocations and private reservations made
  before touching their pages. Each must terminate through the kernel's exact
  per-process-limit reason, with no parent memory watchdog.
- Private and shared read-only mappings of an original generated file passed by
  an inherited descriptor, plus incremental anonymous purgeable allocations
  made volatile. Each records either the exact kernel limit kill or a completed
  resident-growth escape while footprint remains below the threshold. Missing
  evidence is an experiment failure, not a pass.

The parent leaves the child unreaped until querying exit reason namespace/code
through `proc_pidinfo`; it rejects deadline kills and records `wait4`'s kernel
peak resident counter. Apple's [exit-reason implementation](https://github.com/apple-oss-distributions/xnu/blob/xnu-11215.81.4/bsd/kern/proc_info.c#L3810)
permits the direct parent to query its child, including the zombie path. The
[pinned resource accounting](https://github.com/apple-oss-distributions/xnu/blob/xnu-11215.81.4/bsd/kern/kern_resource.c#L1571)
assigns `resident_size_max` directly to `ru_maxrss` in bytes.

The JSON report separates `native_fatal_footprint_supported` from
`strict_resident_cap_qualified`; the latter remains false. Exit zero means the
requested mechanism/coverage observations were collected, including observed
escapes. It does not qualify a release memory gate. The report and per-case logs
must be retained as CI evidence. No execution results have been fabricated.

The parser workflow runs this independent diagnostic before Qt setup on both
macOS matrix entries: Qt 5.15.2 uses `x86_64`, matching its existing Rosetta build,
and Qt 6 uses native `arm64`. Only this new experimental step permits failure to
continue; existing parser/build/startup qualification retains its previous
failure behavior. The step's actual outcome, last phase and capability flags are
summarized, and its text/JSON artifacts are uploaded under an `always()` condition.
A failed startup baseline is retained with its wait status and kernel exit reason
when available. It does not become a passing limit test.

Only sanitized text/JSON is uploaded. The driver removes checkout, output,
home-directory and private-temporary paths from command diagnostics and retained
logs. It records OS version/build, kernel release and architecture, without
hostnames, account identities, full kernel build strings, environment dumps,
unrelated crash reports, executables or generated resident-map data.

There is no root/private-entitlement requirement in the pinned spawn-attribute
installation path. The parent intentionally remains unprivileged and its child
has only App Sandbox; privileged self-setter calls must be denied. This gives no
source-based authority reason to exclude ordinary GitHub-hosted macOS runners.
Actual symbol availability, sandbox startup, exit-reason observation and fatal
enforcement remain runtime questions that this diagnostic must answer. No sudo,
privileged service, nested virtualization or restricted memorystatus entitlement
is requested by the experiment.
This probe isolates the candidate mechanism: it does not apply the production
map-growth, input, file-size, decoded-resource, child or CPU limits. Its generated
file exceeds the production input bound to distinguish ledger coverage clearly;
an observed escape is not automatically a demonstrated production exploit. Full
adoption would require adversarial tests against the combined production policy,
including allocation concurrency, sandbox container files, inherited mappings,
child/exec attempts, cancellation and the installed signed helper. Apple private
SPI and future OS builds also need an explicit distribution/support decision.

## Paths that preserve the requirement

1. **Pursue the native candidate only with a coverage proof.** Install a fatal
   footprint threshold with adequate headroom, retain the existing limits, and
   account for every reachable resident/commit class and maximum in-flight
   charge. Refuse execution when capability or proof assumptions fail. The
   current private-SPI probe is a prerequisite, not the proof itself.
2. **Constrain foreign decoders inside a bounded execution machine.** A decoder
   runtime with fixed linear memory and no host allocation or VM syscall escape
   could limit the parser-controlled region. The entire native host, stacks,
   shared mappings and runtime still need an independent bound; a linear-memory
   maximum alone does not satisfy the whole-process requirement. This is an
   architectural investigation, not an implemented solution.
3. **Move decoding into a separately bounded guest or service.** A guest can
   enforce a strict process address-space limit in its own kernel. Apple's
   public [virtual-machine memory configuration](https://developer.apple.com/documentation/virtualization/vzvirtualmachineconfiguration/memorysize)
   is an available host-side building block, with a separate
   [virtualization entitlement](https://developer.apple.com/documentation/bundleresources/entitlements/com.apple.security.virtualization).
   Guest RAM alone does not bound the host virtualization process; host overhead,
   packaging, cancellation and protocol trust would need explicit budgets and
   qualification. This changes deployment substantially.
4. **Fail closed on an unqualified platform.** Until an implementation actually
   meets the existing bound, reject foreign parsing on that platform with a
   precise capability diagnostic. This preserves the security requirement while
   leaving the macOS feature-availability acceptance gate unfinished.

A privileged broker can access the setters, but granting privilege does not fix
the footprint-versus-resident distinction or post-charge semantics. Installing a
root helper is therefore not a complete solution and is not part of this probe.
