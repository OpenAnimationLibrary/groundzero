# Native style worker

This CPU helper uses the LiteRT 2.3.0 C API directly. It does not require Python.
Its current contract is the qualified Google/Magenta int8 prediction/1 and
transfer/1 pair: float32 RGB reference 256x256, content/output 384x384, and a
100-value style embedding. It is backend implementation; product controls and
temporal processing are not supplied by this helper alone.

The caller must validate the packaged runtime and model hashes against its
trusted manifest before launching the helper. Arguments are the absolute native
library path and the two model paths. The helper checks bounded model file size,
TFLite identifier and exact tensor contracts before acknowledging readiness.
Runtime library objects outlive all models and buffers. Only CPU execution is
requested; no accelerator plugin or model-supplied executable code is loaded.

The binary pipe protocol is little-endian, version 1. Each request/reply header
contains four uint32 values: `0x31535a47` (ASCII GZS1), command, request sequence,
and float32 element count. Payloads immediately follow the header. Pixels are
finite straight SDR RGB in [0,1]. Command 1 captures a 256x256x3 reference and
returns 100 embedding values. Command 2 styles a 384x384x3 content image and
returns that many RGB values. A ready reply has command/sequence/count zero.
Malformed headers, excessive lengths, nonfinite pixels, missing references and
truncated payloads terminate with an explicit diagnostic. The parent must match
sequence IDs and discard incomplete responses.

One process executes requests sequentially and retains both compiled models.
Its single-entry style cache compares complete reference pixels, so reference
replacement cannot reuse stale embeddings. EOF closes the worker. Cancellation
terminates the process; the parent discards its partial response and starts a
fresh worker when needed. Tests check restart determinism, malformed requests,
reference-runtime numerical equivalence and a 512 MiB peak working-set bound.
This does not yet qualify a product-level queue or active-inference interruption.

The isolated CI evaluation builds this helper, copies only the independent C API
library beside it, retains license notices/model provenance and tests with
developer/Python loader paths removed. Deployment still needs dependency-closure
checks and installed-application validation. The official macOS C API library
requires macOS 14. Intel macOS needs a separately qualified source build.

No local build/test commands are provided: repository policy requires CI.
