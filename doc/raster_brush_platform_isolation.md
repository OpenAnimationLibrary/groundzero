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
staging. The original file's ACL is untouched. The unique SID receives read-only
input access, read/write disposable output access, and read/execute access to the
exact trusted executable and already-loaded DLL/codec files. Files already readable
by ordinary AppContainers need no additional grant. No user directory is broadly
granted. Output alone receives a low integrity label. Only the three standard
handles are inherited; environment secrets are not forwarded.

A named global Windows mutex serializes cooperating imports across app processes,
users and logon sessions. It is held from before the first ACL edit until the
worker exits and cleanup finishes; waiters support cancellation and a 120-second
wait limit. The overlapping-process test starts a second importer while the first
worker is deliberately held alive and verifies serialized entry plus cleanup.

RAII cleanup runs after the process terminates on success, failure, cancellation,
or timeout. It revokes only the unique SID's ACEs from the current ACL, then
removes the temporary profile. This does not make Windows DACL read/modify/write
atomic against an unrelated ACL editor: external permission-editing tools must not
run concurrently with import. The mutex coordinates our application instances,
not arbitrary third-party security editors. Unexpected host termination
can leave an orphaned profile/ACE; this is not access granted to other containers,
and import cleanup failures are logged. Installed code whose ACL cannot be read or
narrowly granted fails closed. Ordinary AppContainer OS-public resources and its
private profile remain available: this is not an absolute deny-all-files claim.

The Windows denial probe requires private-file reads/writes and staged-input writes
to fail, and loopback connection to fail specifically with `WSAEACCES` (a missing
server is not evidence). Regular PNG/GBR import and cancellation run through the
same boundary. CI must qualify Windows loader/Qt deployment and profile cleanup.

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

Before reading untrusted bytes, the helper snapshots its trusted Mach virtual map
size and sets both soft and hard `RLIMIT_AS` to that baseline plus 1 GiB. Unexpected
baselines above 64 GiB fail closed. The launcher and packaged qualification disable
the Nano allocator's large speculative reservation with `MallocNanoZone=0`.
Mandatory adversarial reservations through both `mmap` and `mach_vm_allocate` must
fail under this cap; an ordinary small allocation must still succeed. Current
Darwin enforces this in its Mach VM mapping path, rather than the historical
advisory RSS semantics described by older man pages.

This is a hard **virtual-map ceiling**, not a 1 GiB hard resident limit. Darwin's
trusted shared region is approximately 32 GiB on x86_64 and 6 GiB on arm64, and
preexisting mappings can be reused. Separately, the unsandboxed parent samples
`proc_pid_rusage` physical footprint every 50 ms and kills a live helper over
1 GiB or one whose memory cannot be observed. Sampling can overshoot and does not
replace a hard kernel resident cap. The independent 256 MiB decoded-resource and
64 MiB input limits remain in force. A strict cross-platform whole-process 2 GiB
resident/commit guarantee is not established for macOS by these public mechanisms.

Primary implementation evidence: Apple's [Darwin resource limit handling](https://github.com/apple-oss-distributions/xnu/blob/main/bsd/kern/kern_resource.c),
[Mach VM map-limit enforcement](https://github.com/apple-oss-distributions/xnu/blob/main/osfmk/vm/vm_map.c),
[architecture shared-region sizes](https://github.com/apple-oss-distributions/xnu/blob/main/osfmk/mach/shared_region.h),
and [public process usage API](https://github.com/apple-oss-distributions/xnu/blob/main/libsyscall/wrappers/libproc/libproc.h).

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
and [Apple DTS explanation of independent versus inherited helper sandboxes](https://developer.apple.com/forums/thread/679940).

## Qualification status

Implementation and adversarial tests are provided. Platform execution, signed
packaged helper launch, Windows ACL cleanup and final-head application CI must pass
before these boundaries are recorded as qualified. No local builds or tests were
run while preparing this change. Linux Landlock/seccomp restrictions are unchanged.
