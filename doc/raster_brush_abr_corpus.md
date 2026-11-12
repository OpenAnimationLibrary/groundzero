# Additional original ABR 6.2 corpus evidence

This is serialized-format evidence from two author-published brush collections.
It does not qualify Photoshop rendering behavior or expand source-profile
eligibility. No brush assets or pixel payloads are redistributed here.

## Pinned sources and license scope

* [Spray paint collection, pinned author README](https://github.com/mhulse/spray-paint-brushes/blob/bdb2cb33bb2c86309e8340bf99325ebc36038a26/README.md):
  explicitly licenses the work under Apache License 2.0. The inspected asset is
  [`spray-paint-brushes.abr`](https://github.com/mhulse/spray-paint-brushes/blob/bdb2cb33bb2c86309e8340bf99325ebc36038a26/spray-paint-brushes.abr),
  18,269,762 bytes, SHA-256
  `3ecd58fb143775ae206b13e8a835dc6f94e1de2b779942337b07c17984c29c99`.
* [Eugene collection, pinned author README](https://github.com/mhulse/eugene-brushes/blob/c27843ee052805c785cf3c8d4e88d31adfb4dc98/README.md):
  describes the author's original photographs converted into Photoshop brushes.
  This brush-asset repository supplies an
  [Apache License 2.0 LICENSE](https://github.com/mhulse/eugene-brushes/blob/c27843ee052805c785cf3c8d4e88d31adfb4dc98/LICENSE).
  The inspected asset is
  [`around-eugene.abr`](https://github.com/mhulse/eugene-brushes/blob/c27843ee052805c785cf3c8d4e88d31adfb4dc98/around-eugene.abr),
  47,222,408 bytes, SHA-256
  `a73d4c13debfe6f8a1d74c235812a758a888409ccdad78457427c76938a31a29`.

These are the author repositories' asset publications and license statements;
no license is inferred from an unrelated parser's source-code license. Any future
redistribution must preserve applicable license and attribution requirements.

## Complete section and descriptor census

Bounded independent binary inspection traversed all sections and typed descriptor
records, rather than inspecting only version headers. Descriptor trees were
fully consumed. Image payloads were not decoded and no source application was run.

| Observation | Spray paint | Eugene |
| --- | --- | --- |
| Header major/subversion | 6/2 | 6/2 |
| `samp` section bytes | 18,252,348 | 47,192,992 |
| Sample records | 24 | 40 |
| `patt` section bytes | 0 | 0 |
| `desc` section bytes | 17,374 | 29,376 |
| Descriptor version | 16 | 16 |
| Primary `sampledBrush` presets | 24 | 40 |
| Unique, resolved `sampledData` references | 24 | 40 |
| Sample depth / compression tag | 8 / 1 | 8 / 1 |

Both descriptor roots have class `null`, a `Brsh` list of `brushPreset` objects,
and primary `Brsh` objects of class `sampledBrush`. All 64 `sampledData` TEXT
references resolve uniquely to their sample IDs. The sample compression tag is
an observation in the `samp` context, not evidence for pattern VM compression.

All 64 presets contain typed diameter `Dmtr/#Pxl`, angle `Angl/#Ang`, roundness
`Rndn/#Prc`, and spacing `Spcn/#Prc`. In this corpus angle is 0, roundness is 100,
spacing is 25, `Intr` is true, and `flipX`/`flipY` are false. Diameter varies.
All presets explicitly disable `useTipDynamics`, `useScatter`, `useTexture`,
`usePaintDynamics`, `useColorDynamics`, `useBrushSize`, `Wtdg`, `Nose`, and `Rpt `.
The nested `dualBrush/useDualBrush` and `brushGroup/useBrushGroup` flags are false.

## Evidence boundary

This adds 64 original static sampled presets to the observed 6.2 binding corpus.
It contains no modern primary computed brush, nonempty pattern resource, active
dynamic control, or other ABR version/subversion. It supplies no controlled
before/after UI settings, exact producer build, or rendered stroke reference.
Consequently it does not establish dynamic control IDs/units, pattern-to-preset
association, source spacing or interpolation equations, or behavioral parity.

The [resource adapter evidence](raster_brush_abr_resources.md) and
[RCP1 compatibility contract](raster_brush_compatibility.md) retain those gaps.
Decoded fields, native conversion, source candidates and reference-verified
behavior remain distinct statuses.
