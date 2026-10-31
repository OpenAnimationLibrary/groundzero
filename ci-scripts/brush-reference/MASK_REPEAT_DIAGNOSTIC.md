# Mask repeat diagnostic

Manual `mask-repeat` focus runs four distinct acquisition profiles: reused or
fresh paint node, each with the default thread configuration or a configured
maximum of one worker. Each profile uses unchanged original maskwash presets
043, 044, 070 and 071, with eight fixed independent strokes per preset. The
ordinary maskwash matrix and its gates remain unchanged. These profiles are
not source-parity qualifications and cannot substitute for the full matrix.

Each stroke has exact reset validation and fixed readback checkpoints. Existing
visible-RGBA repeat equality is mandatory; the first discrepancy fails the
profile and retains both images. There is no retry-until-stable or favorable
capture selection. Different profiles are separate jobs and cache identities.

The one-worker profile sets the verified root `maxNumberOfThreads=1` option
before source startup and verifies `Krita.readSetting("", "maxNumberOfThreads", ...)`
before document creation. The default profile verifies the option is absent.
This records a configured limit, not a measurement of active OS threads.

Pinned primary references (configuration/API plumbing only):

- [KisImageConfig root group and maxNumberOfThreads](https://github.com/KDE/krita/blob/bd6e8d0/libs/image/kis_image_config.cpp)
- [Public Krita.readSetting](https://github.com/KDE/krita/blob/bd6e8d0/libs/libkis/Krita.cpp)
- [Public Node addition, removal and identity](https://github.com/KDE/krita/blob/bd6e8d0/libs/libkis/Node.cpp)

Interpret outcomes cautiously. Failure in default/reused but not fresh-node
suggests retained-node state; failure across both node policies only with the
default thread limit suggests concurrency sensitivity. Neither a small passing
sample nor one-worker stability establishes general source fidelity. If all
profiles fail or all pass, retain that evidence and design the next controlled
experiment instead of changing baseline acceptance thresholds.
