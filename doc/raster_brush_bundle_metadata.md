# Krita bundle tag metadata

The bundle adapter preserves resource tag labels from the namespaced
`manifest:file-entry/manifest:tags/manifest:tag` structure written by Krita's
[manifest writer at f39b11e4fb5cfb0d4b642eae6126a496d319e805](https://github.com/KDE/krita/blob/f39b11e4fb5cfb0d4b642eae6126a496d319e805/libs/resources/KoResourceBundleManifest.cpp#L120).
Krita's [resource management documentation](https://docs.krita.org/en/reference_manual/resource_management.html)
describes per-resource tags and distinguishes them from resource names.
The implementation is independent; it reads the documented emitted structure.

Native and portable presets retain `provenance.bundleMetadata` with this schema:

```json
{
  "version": 1,
  "manifestVersion": "1.2",
  "presetTags": ["Ink"],
  "resourceTags": [
    {"member": "paintoppresets/original.kpp", "mediaType": "paintoppresets", "tags": ["Ink"]}
  ]
}
```

`presetTags` belongs only to the selected preset. `resourceTags` preserves all
tagged entries from its source library, including brush tips, patterns and other
resources. It is source metadata, not a declaration that every listed resource
is used or supported by the preset. Resource lookup still resolves the preset's
explicit reference by checksum or unambiguous filename; tags never affect lookup.
Preset names continue to come from the KPP preset name.

Labels retain their Unicode text, whitespace, order and duplicates. The adapter
does not translate, merge, trim or reinterpret labels as rendering settings,
rights grants or provenance verification. Empty tag collections remain empty.
All native save/reopen, worker transport and portable packaging modes preserve
this metadata. Archive member identifiers must be relative, traversal-free and
scoped beneath their declared resource type; absolute filesystem paths cannot
be inserted through this field.

The supported form is manifest version 1.2 with text-only, namespaced tags.
Nested markup, misplaced tags, multiple tags containers on one entry, unexpected
namespaces/attributes and unknown metadata schema versions fail with an explicit
error. The adapter does not silently discard these cases. Limits are 64 tags per
resource, 256 UTF-16 code units per label, 512 tagged manifest entries, and 12 KiB
compact bundle metadata; the complete preset provenance also retains its 16 KiB
limit. Oversized metadata fails import/export rather than truncating labels.
SQLite resource-cache tags, standalone `.tag` files and locale-specific tag
translations are outside this bundle-manifest adapter and are not inferred.

Original fixtures cover compressed and stored bundles, Unicode/entities,
duplicates/whitespace, tip tags distinct from preset tags, native and portable
roundtrips, worker transport, disk save/reopen, malformed tag structure and
forged metadata paths/versions. These checks run in CI, not locally.
