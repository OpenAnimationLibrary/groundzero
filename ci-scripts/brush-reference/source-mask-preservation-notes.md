# Preserved source-mask declaration and resource lifecycle

The existing `Preset::sourceMasked` renderer remains the measured, fixed
generated secondary field. Its distinct retained accumulation, flow, opacity,
spacing and composition equations are unchanged. Native `Secondary` is not a
replacement for that source model.

`Record::sourceMaskDefinitionV1` is optional metadata, never an evaluator input.
It retains explicit enabled/master-size/composition declarations; independent
brush geometry and static control values; and a typed generated definition or
one decoded resource. Absent declarations remain null. Masked sensor lists,
individual/shared curves, and sensor flags live in `sourceSensorsV1` with scope
`masked`, separate from the primary controls. Unsupported or unprojectable
fields retain bounded technical markers and a source-field SHA256, not raw XML
or resolver paths. Bitmap-mask resources do not activate a new source profile.

A resource is decoded through the existing bounded adapter. Grayscale GBR masks
can remain alpha U8. Color resources retain canonical decoded RGBA8, and tagged
PNGs may be normalized to encoded sRGB by the existing image decoder. The source
resource byte SHA256 is retained separately from the canonical pixel identity.
The `resourceEncoding`, application declaration, and explicit
`unverified-resource-color-contract` marker distinguish this preservation from
an established source alpha-from-RGB interpretation. Original encoded bytes and
ICC profiles cannot be reconstructed from the retained pixels. No additional
coverage baking or source color equation is introduced.

The mask image belongs to the same content-hash graph as primary tips, native
secondary tips and textures. Worker RGBA8 sidecars, aggregate decoded-memory
budgets, resource inventory, redistribution declarations, manifest-only export,
private/permitted copies, repair retries and pixel-hash relinking include it.
A missing declared resource prevents a portable package from becoming complete;
relinking verifies canonical pixel identity, geometry and color. Repair restores
canonical decoded pixels, not original source encoding/profile bytes. A source
bundle dependency that cannot be resolved stays explicitly unresolved/inactive.
Malformed resource decoders fail import rather than publishing a partial graph.
Multiple-frame GIH masks and unmeasured geometry/control composition remain
unsupported. No unmeasured mask declaration is silently applied to a native or
source brush.

Authored final-validation cases cover measured fixed-generated admission,
separate masked controls, independent bitmap-mask geometry, canonical resource
identity and color-contract markers, native/worker round trips, decoded-budget
failure, resource rights, portable omission/repair, malformed bindings, and
aggregate bundle accounting. They are written but have not been executed.
No builds, tests, checks or CI/reference artifact access occurred for this work.
