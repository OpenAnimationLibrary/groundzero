# Brush importer platform boundaries

The importer treats foreign bytes as untrusted. Resource caps and process crash
containment are separate from privilege isolation. The parent never interprets
foreign images or brush descriptors while transferring worker results.

## Windows

The parent creates a unique temporary AppContainer profile with **no capabilities**
and supplies its SID using `PROC_THREAD_ATTRIBUTE_SECURITY_CAPABILITIES` at
process creation. The decoder requires an AppContainer token before reading
foreign input. A Job Object separately limits committed memory, CPU and children.

The selected input is copied with the bounded regular-file reader into private
staging. The original file's ACL is untouched. Production uses a dedicated minimal
`brush-import-runtime/rasterbrushimport-helper.exe` with its Qt/codec/zlib
runtime, rather than the drawing executable and its broad dependency set.

At build/package time, `windeployqt` deploys Qt and plugins; CMake resolves remaining
non-system dependencies and writes a SHA-256 manifest. At import time, the parent
copies only manifest-listed regular files into a fresh private directory, validates
their hashes, and grants the unique AppContainer SID read/execute access to those
**private copies**. Installed application files are only read. Their ACLs are never
inspected or modified, so ordinary Program Files installations need neither
administrator privileges nor a permission prompt. The existing application's
Microsoft Visual C++ Redistributable requirement remains; system DLLs are not
copied. Debug CI builds explicitly deploy the matching Visual Studio DebugCRT
using `windeployqt --debug --compiler-runtime` and the selected Windows SDK's
`ucrtbased.dll`, then verify their dependency closure. These development-only
files stay on the licensed build/test machine; release bundles reject Debug CRT
DLLs and continue to require the official installed release redistributable.
See [Microsoft runtime deployment guidance](https://learn.microsoft.com/en-us/cpp/windows/determining-which-dlls-to-redistribute)
and [Qt's DebugCRT deployment implementation](https://github.com/qt/qtbase/blob/6.8/src/tools/windeployqt/main.cpp).
Package signing must precede regeneration of `runtime.files` because a signature
changes executable bytes.

The copied runtime is capped at 256 files, 128 MiB per file and 512 MiB total; the
build reports its measured file count and byte size and fails if it exceeds these
bounds. Copying adds disk I/O per import and temporarily consumes the measured
runtime size. Files are hashed and copied in 64 KiB chunks with cancellation and the import's
shared 120-second deadline checked between chunks; staging is included in that
deadline. No full DLL-sized scratch allocation is required. There is deliberately no persistent executable cache or cache
invalidation trust boundary. Runtime/compiler upgrades require rebuilding and
qualifying the self-contained bundle. No download or deployment tool runs on the
user's machine during import.

The SID receives read-only input, read/write disposable output, and read/execute
private code access. Output alone receives a low integrity label. Only standard
handles are inherited, and the environment names only the private runtime and
Windows system library paths. No broad user directory is granted.

All ACL edits now target per-import private files, eliminating shared application
DACL races and the global serialization requirement. The overlapping-process test
requires both independent imports to complete and clean up. RAII cleanup runs
after process termination on success, failure, cancellation or timeout and removes
the ephemeral grants/profile. Unexpected host termination can leave an orphaned
private directory/profile; cleanup failures are logged. Ordinary AppContainer
OS-public resources and its private profile remain available: this is not an
absolute deny-all-files claim.

The Windows denial probe requires private-file reads/writes and staged-input writes
to fail, and loopback connection to a parent-held live listener to fail with
`WSAEACCES` or `WSAECONNREFUSED`. Ordinary parent connections must succeed before
and after the worker probe; refusal from an unused port is not evidence. The
parent listener is not inherited by the worker, and positive-control waits are
bounded. A pending connection is a distinct failure outcome, never sufficient
by itself. Test-only read-only WFP enumeration can qualify it only with a recent
kernel classify/capability drop matching the staged helper application ID, its
unique AppContainer SID, and the live IPv4/TCP loopback endpoint. Enumeration is
capped at 4,096 events and two seconds. Missing event access, disabled event
collection or no matching drop keeps the test failed; the observer never changes
firewall policy or event-collection settings. Matcher negative fixtures reject
wrong applications, SIDs, ports, stale timestamps, allow events and nonloopback
events. This observer is absent from production builds.
Regular PNG/GBR import and cancellation run through the
same boundary. The packaged-runtime smoke imports an original fixture, compares
installed runtime ACLs before/after, and rejects a changed manifest-listed file.
CI must qualify Windows loader/Qt deployment and profile cleanup.

Primary mechanism: [Microsoft AppContainer launch guidance](https://learn.microsoft.com/en-us/windows/win32/secauthz/implementing-an-appcontainer).

## macOS

A separate minimal `rasterbrushimport-helper` executable is signed with only
`com.apple.security.app-sandbox`; it does **not** inherit the unrestricted drawing
application's privileges. No network, user-selected-file or temporary-exception
entitlement is granted. This uses App Sandbox, not deprecated `sandbox_init` or
private SBPL profiles. The signed boundary applies at exec, before decoder code.

The parent stages bounded bytes and passes preopened stdin/stdout files. The helper
receives a basename and a private sentinel path, never authority to arbitrary
user-selected paths. Before parsing it verifies the sandbox entitlement and requires
private-sentinel access and loopback networking to be denied. It uses Darwin's own
sandbox temporary directory, CPU/core/file-size/child limits, and private worker
records. App Sandbox's system resources and helper container remain available.

Before reading untrusted bytes, the helper enumerates the actual top-level Mach
virtual-map entries with the public `mach_vm_region` / `VM_REGION_BASIC_INFO_64`
API. It includes submap reservations once without recursively double-counting
mapped children. Enumeration is stack-only, capped at 65,536 regions and two
seconds, checks forward progress and all address/size sums for overflow, and
fails closed on unexpected kernel results. Both soft and hard `RLIMIT_AS` are set
to the observed raw baseline plus the unchanged 1 GiB growth budget. Mapping
changes during enumeration that prevent establishing the cap fail closed.

`MACH_TASK_BASIC_INFO` and `TASK_VM_INFO` report an adjusted size, whereas Darwin
checks `RLIMIT_AS` against raw `map->size`. Rosetta reservations can make the former
smaller than the latter, causing `EINVAL` for an apparently sufficient limit.
Native Qt/runtime reservations can also exceed the previous assumed 64 GiB
baseline even with `MallocNanoZone=0`. There is no replacement arbitrary total-size
guard: the admission bound measures map enumeration work, checked representable
size, and fixed additional mapping budget. Mandatory adversarial reservations via
both `mmap` and `mach_vm_allocate` must be denied, and an ordinary small allocation
must still succeed. A process able to raise the hard ceiling is rejected.

This is a hard **virtual-map growth ceiling relative to a trusted startup
baseline**, not an absolute resident/commit bound. Existing large reservations can
be committed without growing that map. Separately, the unsandboxed parent samples
`proc_pid_rusage` physical footprint every 50 ms and kills a live helper over
1 GiB or one whose memory cannot be observed. Sampling can overshoot and does not
replace a hard kernel resident cap. The independent 256 MiB decoded-resource and
64 MiB input limits remain in force. A strict cross-platform whole-process 2 GiB
resident/commit guarantee remains unresolved for this native macOS architecture;
these mechanisms do not close that requirement.

Release-specific primary evidence: Apple's [Darwin resource limit handling](https://github.com/apple-oss-distributions/xnu/blob/xnu-11215.81.4/bsd/kern/kern_resource.c),
[raw map-limit enforcement and public region-query implementation](https://github.com/apple-oss-distributions/xnu/blob/xnu-11215.81.4/osfmk/vm/vm_map.c),
[adjusted task metrics](https://github.com/apple-oss-distributions/xnu/blob/xnu-11215.81.4/osfmk/kern/task.c),
and [public process usage API](https://github.com/apple-oss-distributions/xnu/blob/xnu-11215.81.4/libsyscall/wrappers/libproc/libproc.h).

The bounded `GZBW0001` envelope contains only strict private metadata/raw-RGBA names.
The parent holds the response descriptor open throughout execution and streams
bounded chunks into newly created regular files. It rejects traversal, duplicate
names, truncation, excess counts/lengths and trailing bytes, then applies the same
anchored response validation as other platforms. The helper also refuses special
files when packaging its output. No image decoder runs in the parent.

Standalone CI signs a separate uninstrumented test helper. The application bundles
the minimal helper and deploys its Qt dependencies explicitly. Deployment modifies
Mach-O paths, so CI signs and verifies the helper again **after** deployment. Release
signing must preserve the helper's independent entitlement and sign nested code
before the enclosing app; an unsigned or incorrectly entitled helper fails closed.
Ad-hoc CI signatures qualify local execution, not Developer ID distribution or
notarization. These release signatures require the project's release-signing
pipeline, not a new runtime user permission prompt.

Primary contracts: [Apple App Sandbox](https://developer.apple.com/documentation/security/app-sandbox),
[Apple entitlement inspection API](https://developer.apple.com/documentation/security/sectask),
and [Apple DTS explanation of independent versus inherited helper sandboxes](https://developer.apple.com/forums/thread/706390).

## Qualification status

Implementation and adversarial tests are provided. Platform execution, signed
packaged helper launch, Windows ACL cleanup and final-head application CI must pass
before these boundaries are recorded as qualified. No local builds or tests were
run while preparing this change. Linux Landlock/seccomp restrictions are unchanged.
