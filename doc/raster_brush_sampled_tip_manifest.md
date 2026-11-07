# Sampled ABR tip collection identity

The legacy **Extract Sampled Tips from ABR** action publishes `sampled-tips.json`
inside each collection, in the same atomic directory publication as its PNGs.
The selector continues to list PNGs only. A cancelled or failed unpublished
collection is removed with its private staging directory.

Version 1 uses format `groundzero-abr-sampled-tip-collection` and extraction kind
`sampled-images-only`. Its source object retains the ABR SHA-256, container
version and subversion. It contains no source file or machine path, inferred
rights declaration, descriptor preset name association or dynamics claim.

Each sample has a zero-based ordinal in parsed sampled-image order, its exact
parsed name and byte-preserving Latin-1 sample ID (empty when absent), source
8/16-bit depth, dimensions, saved PNG filename, and SHA-256 of the emitted PNG
bytes. Names remain exact even when portable filenames require sanitization,
truncation, numbering or Unicode collision handling. Modern `tip_001` names are
parser sample names; this manifest does not substitute descriptor preset names.
Source 16-bit depth remains recorded even though extraction emits the existing
8-bit grayscale coverage image.

The strict reader rejects unknown/missing fields, wrong types, unsupported
versions, invalid ordinals, duplicate portable filename identities, path
traversal, unlisted directory members, and changed output digests or PNG header
dimensions. Published filename identity uses canonical Unicode normalization and
case folding; duplicate actual member keys reject, while parsed sample names
remain exact. The reader binds each manifest filename to the actual enumerated
member before its anchored digest read. Files use anchored, single-link, no-follow regular-file reads; PNG
content is hashed in 64 KiB chunks without invoking a decoder. The collection
has at most 256 samples, 2 Mi pixels per image and 8 Mi pixels total; its JSON
is bounded to 8 MiB and its aggregate PNG output to 64 MiB. Parsed names are
bounded to 4,096 UTF-16 units and sample IDs to 36 bytes. Cancellation is checked
between records and chunks and immediately before publication.

These hashes establish consistency with the stored manifest. They do not prove
authenticity against someone rewriting both the PNG and its manifest, source
brush compatibility, or permission to redistribute the embedded images.

Authored regression sources cover normalization-distinct original names, output
filename aliases, source IDs and depths, staging/publication, cancellation,
strict schema rejection, changed PNG bytes/digests/dimensions and unexpected
members. Their execution remains a separate validation step.
