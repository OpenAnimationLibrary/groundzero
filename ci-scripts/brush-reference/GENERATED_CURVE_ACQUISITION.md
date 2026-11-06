# Original Soft/Gaussian model discrimination

The completed 32-case `generatedtips` source acquisition at implementation
`703b77b82` provides original K5341 masks for independent model work. This
follow-up preserves those case identities and all existing fidelity gates.
It does not label decoded Soft metadata as implemented behavior.

## Observed facts and unresolved equations

Source Soft circles with a rising identity curve have transparent centers and
increasing interior coverage. At the existing coordinate lattice, the interior
response is consistent with curve evaluation over squared normalized radius,
including integer horizontal and half-pixel vertical samples. Descending cubic
controls are also consistent with that interior model. This observation alone
fails to explain the outer antialias envelope of rising curves: a simple hard
radial cutoff gives large edge errors and is not eligible for source admission.

Source Soft rectangles with a rising identity curve are consistent in their
interiors with a product of squared normalized axis coordinates. Applying the
same direct curve map independently to squared axes does not explain the
nonlinear rectangular captures. A native product approximation would therefore
misrepresent required source behavior. Constant curves and equivalent linear
curves with different knot counts distinguish a separable response, curve
normalization, table/interpolation effects, and potential endpoint shortcuts.

The two asymmetric Soft fade-axis cases are pixel-identical in alpha to their
corresponding symmetric controls. This establishes inactivity for those captured
settings only; it is not evidence that every generator or source version ignores
those fields. Gaussian captures have a different falloff and axis interaction.
Two symmetric fade values plus one asymmetric pair do not determine a general
Gaussian equation. Interpolating a fitted formula between those observations
would be premature.

These conclusions use original captured pixels and serialized controls only.
No foreign renderer implementation or brush pixels were copied into native code.

## New bounded group

`generated_curve_diagnostics.prepare` writes `generatedcurves`, containing 124
original cases:

- 28 Soft integer-origin controls: circle/rectangle, diameter 16/64, constant
  curves at 0/.25/.5/.75/1, identity with three collinear knots, and descending
  linear curves. Existing two-knot identity captures remain in `generatedtips`.
- 24 Gaussian controls: circle/rectangle and diameter 16/64 at fade
  0/.1/.5/.9/1, plus both orientations of asymmetric .1/.9 axes at diameter64.
- 72 Soft origin discriminators: constant-one, two-knot identity and descending
  linear curves, circle/rectangle, diameter16/64, and offsets (-.125,0),(.125,0),
  (0,-.125),(0,.125),(.5,0),(0,.5).

Fractional controls use one public `Node.paintPath` with exact authored floating
vertices, rather than rounding them for the integer `Node.paintLine` binding.
`generated_curve_probe.validate` rejects changed origin, geometry or delivery.
The existing `generated_tip_probe` validates exact effective curve/generator
controls and preserves the full96-square BGRA capture around each tip. Full
source PNGs and deterministic repeats remain required. Constant curves are
exploratory observations; the fixture does not guess which controls should
produce an exact no-op before acquiring it.

## Shared hooks

- `prepare.py`: group `generatedcurves` calls the new prepare function, then the
  usual input finalizer.
- `probe.py` paint_case: before the default integer line path, if
  `generatedCurvePath` is present, import `.generated_curve_probe`, validate,
  construct a QPainterPath from returned vertices and call
  `node.paintPath(path, "ForegroundColor", "None")` once. Return the exact
  vertices, `mode=Node.paintPath`, `singleContinuousStroke=true`, and
  `physicalTabletClaimed=false`.
- Workflow: add `generatedcurves` to the complete matrix and a manual
  `generated-curves` focus; invoke `generated_curve_diagnostics_test.py` in the
  pure oracle contract gate. Preserve existing groups and thresholds.
- Comparator: validate124 fixture identities and actual delivered vertices for
  fractional cases. Native model admission must follow a measured equation;
  source acquisition completion does not itself permit a native approximation
  to stand in for the required Gaussian/Soft renderer.

Independent source-model implementation, full generated geometry/curve/dynamic
combinations and final-head qualification remain open. No local builds/tests ran.
