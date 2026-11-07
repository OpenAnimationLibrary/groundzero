# Derived brush support diagnostics

`toonz/rasterbrushsupport.h` derives explanations from the current `Record` and
`Preset`. It does not persist a support verdict, change active source admission,
interpret a foreign descriptor enum, or grant reference verification.

Each of the existing eleven capability categories records whether current
settings feed a native path, a constrained experimental source path, no active
component, or a rejected current combination. The existing `validate(preset)`
guard remains authoritative for numeric, resource and combination restrictions.
A recognized profile name alone never makes its broader capability supported.
Configured mapping counts describe current settings; they do not prove that a
preserved source declaration was mapped, matched or evaluated.

Sensor and mask declarations are reported separately as metadata. Primary and
masked control scopes, fixed target/sensor labels, retained raw pressure and
use-curve switches, curve-knot counts, and Time/Distance/Fade intervals/repeat
retain their original declared meaning. Missing switches stay unspecified.
A false raw pressure switch does not imply a whole control is disabled: an
independently configured curve may still be active. Time intervals remain
source milliseconds, distance remains source length units, and Fade remains
source dab count. No interpolation, sensor product, normalization or native
interval conversion is inferred. Raw XML, source field names and arbitrary
private values never enter these labels.

A preserved secondary mask reports declared enable/composition state and its
separately retained resource/geometry. Unresolved resources, unprojected fields
and unverified color-to-coverage rules have fixed actionable explanations.
Even when an independently admitted fixed mask is active, that does not prove
the preserved definition or its sensor declarations drive that evaluator.
Native edits and explicit native conversion recompute the current-path report;
retained source metadata cannot restore cleared source equations.

ABR static conversion records omitted categories under `sourceOmissionsV1`.
The vocabulary is fixed: shape, scatter, transfer, color, texture, secondary
mask, brush pose, tool options, tip geometry, stroke effects and unclassified
settings. These are historical omission facts, not enum mappings or active
capability permissions. Disabled source groups do not create active omission
facts. Unknown/malformed omission input can add only a generic warning.

Portable export keeps only that bounded category list along with existing
validated typed metadata and safe conversion classification. Free-form source
reports/descriptors remain excluded. Reopening recomputes the report from the
current settings and retained declarations; imported support/reference flags
cannot grant a positive result. The API always leaves source reference proof
unestablished. Existing rights and resource-packaging choices still apply.

The import acceptance details, editor details, preview tooltip and explicit
conversion explanation use this same derived report. The editor summary and
preview explanation refresh with current settings. User-facing text names
plain brush components and required next actions, not implementation enums.

The related bitmap-mask conversion identifies the original and derived primary
image separately from the complete retained resource graph. A distinct source
mask, or a mask aliasing the original primary, survives with its own declaration
and rights. It cannot prevent conversion merely by adding another resource.

`tests/rasterbrushsupport_test.h` authors current-guard, stale-state,
metadata-versus-mapping, raw-pressure-switch, scope/interval, unknown-label,
ABR omission, fixed-tag privacy and portable lifecycle cases. The GUI conversion
fixture also covers retained mask images with distinct and aliased identities.
These regression sources have not been executed; builds, tests and CI remain
deferred until the full implementation plan is complete.
