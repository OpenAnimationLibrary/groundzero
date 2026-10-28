# Loader robustness: fixes and known issues

This records the result of master-plan item D2. The loaders were checked by
reading the code against the damaged-file harnesses (`toonz/sources/fuzz`:
`psd_fuzz`, `pli_fuzz`, `tstream_fuzz`) and the loader tests
(`toonz/sources/image/tests`). Nothing was built or run when this was written,
so every fix below still needs one run of the tests with `BUILD_TESTS` and
`BUILD_FUZZ_TESTS`.

A fix makes the load fail the way the surrounding code already reports errors
(an exception, or an empty image for the PSD `load()`). A valid file loads as
before.

## Fixed

### PSD reader (`common/psdlib/psd.cpp`, `psdutils.cpp`)

| What | Where |
|---|---|
| An image or region one pixel wide or high reduced the shrink to 0 and then divided by it. The shrink is now at least 1. | `readImageData` |
| Header sizes were not bounded: canvas larger than 30000 pixels (the PSD limit) and bit depth 0 are rejected. Layers larger than 300000 pixels (the PSB limit: a layer can extend beyond the canvas) or with coordinates beyond 2^28 are not loaded; their record, channel lengths included, is still read, so the layers after them load from the right data (checked by `tiio_psd_test`). A raster whose byte size does not fit an int is refused. | `doHeaderInfo`, `readLayerInfo`, `readImageData` |
| `mymalloc` / `mycalloc` returned null on failure and no caller checked it; they now throw `std::bad_alloc` (the reader constructor turns it into a `TImageException`). | `psdutils.cpp` |
| A layer skipped as damaged had no name, no channels and no id, but the layer lists and `doImage` used them (null dereference). It now has an empty name, an id and 0 channels, and loading it fails cleanly. This is only for a channel count above 64 or a record cut by the end of file; a layer with bounds out of range keeps its record and channel lengths but no channel index, and loading it fails cleanly too. | `readLayerInfo`, `doImage` |
| A channel count above 32767 was stored in a `short` and read as negative. | `readLayerInfo` |
| Layer name length read at end of file wrote `name[-1]`; image resource name length at end of file wrote `name[-1]` on the stack. | `readLayerInfo`, `doImageResources` |
| Negative resource, block and mask lengths seeked backwards; a negative extra-data block length looped over the same blocks without end. | `doImageResources`, `skipBlock`, `sigkeyblock`, `readLayerInfo` |
| The channel and row buffers of `readImageData` were sized from the first channel, which can be a layer mask of other size; the channel map and `chindex` were indexed past the channels of the layer (RGB mode with fewer channels); more than 4 image channels (Lab, multichannel) overflowed `inrows[4]`. | `readImageData` |
| Raster not created for depths other than 1, 8, 16 or for bitmaps with alpha, then dereferenced. | `readImageData` |
| Pixel columns were not checked against the row buffer (16 bit rows were checked in bytes, not samples), and the 1 bit path wrote up to 7 pixels past the row when the width is not a multiple of 8. | `readImageData` |
| A row index outside the channel, a channel without row table or ZIP data, and merged ZIP data (not supported) were used through null or stale pointers. They read as a row of zeros. | `readrow`, `readChannel` |
| RLE rows: a packet that continued past the end of the input copied past it. The compressed row length is bounded by the row buffer. | `unpackrow`, `readrow` |
| ZIP channels: the buffer was sized from the declared channel length and the unzip size multiplied in `long`; a ZIP prediction row of width 0, or of a depth that does not divide the buffer, ran past the buffer. Sizes are checked in 64 bits and against the file size, the output is zeroed, and `inflateEnd` is called. | `readChannel`, `psdUnzipWithPrediction`, `psdUnzipWithoutPrediction` |
| Extra-data readers (`lyid`, `lsct`, ...) were called with no layer (null dereference) for top-level extra data; a `Lr16` block inside a layer's extra data recursed without end. | `findbykey` |
| `TPSDParser` group import indexed `m_levels` with an unchecked expression (a file that closes more folders than it opens); `getLevelIndexById(0)` indexed an empty list when no layer is importable. | `doLevels`, `getLevelIndexById` |
| The header struct was uninitialised (resolution, merged alpha, layer info when the file has none). | `TPSDReader::TPSDReader` |
| `rowpos` allocations in `doImage` (never used) took their size from the mask size in the file. They are removed. The per-channel RLE and ZIP buffers of an earlier load are freed instead of leaked. | `doImage`, `readChannel` |

### PLI reader (`image/pli`)

These were the known issues listed by `pli_fuzz` (`pli-short-tag`,
`pli-truncated-tag`, `pli-palette-tag`); the harness no longer skips them.

| What | Where |
|---|---|
| A tag cut short by the end of the file was parsed from uninitialised memory (the read result was not checked). | `MyIfstream::read` |
| Tag readers used fixed parts and counts without checking the tag length (also from a null buffer when no earlier tag had data): chain, color, style (parameters, strings, rasters), geometric transformation, double pair, bitmap, image (suffix, object list), intersection data, precision scale, auto-close tolerance. Each now checks against the tag length first. | `pli_io.cpp` |
| Palette tags whose length is not a multiple of 3 (4 with alpha) wrote one color past the array. | `readPaletteTag`, `readPaletteWithAlphaTag` |
| Raster and bitmap sizes were multiplied in `int` (65535 * 65535 * 4). | `readRasterData`, `readBitmapTag` |
| A tag length larger than the file allocated that many bytes before the read; the frame suffix length too. | `readTagOnce`, `MyIfstream::require` |
| `readTag` recursed once per control tag and without a limit through groups; a long run of control tags or nested groups overflowed the stack. It loops over control tags and stops at 500 levels. | `readTag` |
| Objects an image or transformation refers to were cast without a type check. | `readImageTag`, `readGeometricTransformationTag` |
| A frame tag shorter than its frame number, and tag lengths over 2^31 on a 32 bit `long`, seeked backwards. | `loadInfo`, `MyIfstream::seekg` |
| A precision scale of 0 or less (a divisor of the stroke coordinates) is refused. | `readPrecisionScaleTag` |
| Style parameters were read past the end of the parameter list when a style tag had fewer than the style type reads; a color tag without color and a stroke tag without curves were dereferenced. | `PliInputStream`, `createGroup`, `createStroke`, `doLoad` |

### Found by the first run of the fuzz tests

The first CI run of `pli_fuzz_test` and `tstream_fuzz_test` (tier12) reported
3 hangs in the PLI reader and 2 hangs and 2 crashes in the palette loading.
They come from three causes, found by working out which field of the seed
each mutation damages. They were fixed without a new run.

| Cases | What | Where |
|---|---|---|
| `one-stroke@flip:104:6`, `two-frames@flip:134:5`, `suffixes-v150@set:134:127` | The mutated byte is the high byte of the id of the first style tag in the palette group (16385, 8193 and 32513). The reader adds placeholder styles up to the id, but `TPalette::addStyle` refuses styles from id 4096 on, so the loop never ended. The load now fails. | `readPalette` (`image/pli/tiio_pli.cpp`) |
| `studio-palette@ins:595:3c613e`, `level-palette@del:595:4` | A tag inside `<indices>` of a palette page (`<a>` inserted after the last index, or `</indices>` cut to `<ices>`). `eos()` matched it as a pending begin tag and returned false; `is >> index` then failed on the `<` and consumed nothing, so `while (!is.eos())` never ended. `eos()` now returns true once the stream has failed (the old code already did when no tag was pending), and the load fails on the missing end tag. This covers every `while (!is.eos()) is >> value;` loop of the loaders. | `TIStream::eos` (`common/tstream/tstream.cpp`) |
| `studio-palette@rep:258:1:`, `compressed-palette@set:193:1` | The `pickedpos="3,4"` attribute of a style lost its second coordinate (the `4` deleted, or the `,` set to 0x01 through a literal of the LZ4 block). `stringToPoint` read the second part of the split string without checking there was one. The load now fails with a `TException`. | `stringToPoint` (`common/tvrender/tpalette.cpp`) |

### EXR (`toonzlib/exrlevelsource.cpp`)

The data and display windows were already checked for size. The raster of the
display window is now also checked to fit an int in bytes (a small data
window with a huge display window made `TRasterFP` overflow its size).

### Compressed scenes, and allocations sized by a file header

| What | Where |
|---|---|
| Compressed scenes (LZ4): a size near SIZE_MAX wrapping the buffer size, an unbounded declared size, a size too small making the decompression loop forever, and a header or payload cut short. The reads are checked, the declared size is bounded by the compressed one and by 256 MB of scene text, an allocation failure is reported as a damaged file, and `lz4decompress` stops when it makes no progress. These were the known issues of `tstream_fuzz`; it no longer skips them. | `common/tstream/tstream.cpp` |
| EXR: the buffers were allocated from the header's data window before any pixel was read, so a damaged file of a few hundred bytes could ask for many GB. Images and windows larger than 16384 x 16384 pixels are refused when read. | `image/exr/tiio_exr.cpp`, `toonzlib/exrlevelsource.cpp` |
| PSD: a load that throws (now common on damaged files) left the file open, so each attempt to show the frame leaked a file handle. The file is closed. | `TPSDReader::load` |

## Audited, nothing to fix

* `image/exr/tiio_exr.cpp`, `toonzlib/exrlevelsource.cpp`: pixel counts,
  windows and strides are computed in 64 bits and checked (and bounded, see
  above).
* `glb/glbloader.cpp`: the container, the chunk lengths, accessors, sparse
  accessors, indices and node graph are validated with a size budget, and
  `glbloader_test` has damaged-file cases.
* `image/ffmpeg`: the writers pipe frames to the `ffmpeg` program; no file
  format is parsed in this code base.

## Not fixed

| What | Where | Why |
|---|---|---|
| A `pickedpos` style attribute that is not a number makes `std::stoi` throw `std::invalid_argument`; a palette load that fails inside a style leaks that style | `stringToPoint`, `TPalette::loadData` | Not a crash: the load fails with a `std::exception` instead of a `TException`. |
| A failed raster allocation (null buffer) is not detected before writing pixels | `TRaster` constructor, used by all loaders | A change in the core raster class, outside a loader fix. The PSD and EXR loaders now refuse sizes that overflow an int. |
| The PSD reader destructor frees nothing (`TPSDParser` and the layer tables leak) | `common/psdlib/psd.cpp` | A leak, not a memory-safety problem. A rewrite with smart pointers (see the comment at the top of the file) is the right fix. |
| `TLevelReaderPsd::loadInfo` never deletes its `TPSDParser` | `image/psd/tiio_psd.cpp` | A leak. |
| `TLevelReaderPsd` dereferences `QTextCodec::codecForName(...)` when the layer name encoding preference is not a known codec | `image/psd/tiio_psd.cpp` | Set by preferences, not by the file. |
| 1 bit PSD pixels all get the value of the first bit of the byte; 16 bit gray with alpha loses the alpha | `readImageData` | Wrong output, not unsafe. Not changed to keep the output of valid files. |
| PLI color tags: the style and attribute values are not range checked (asserts only); the style loaders in the palette (`TColorStyle::load`) were not audited for hostile parameter values | `readColorTag`, `readPalette` | Needs the style classes read one by one. `pli_fuzz` reaches them with its palette seeds. |
| PLI: `ThickQuadraticChainTag` coordinates can be inf or NaN through a huge precision scale; the strokes are built from them | `readThickQuadraticChainTag` | Not a crash in the loader; the stroke code should be fuzzed on its own. |
| Channel data positions are summed in `long`, which is 32 bits on Windows: a raw or RLE layer whose data would end past 2 GB (a damaged size, or a file that large) wraps and reads wrong data | `readChannel` | Not unsafe: the buffers are sized from the bounded rows and columns. Files over 2 GB are not readable with this `long`-based code anyway. |
| PSB (version 2) PSD files and CMYK / 32 bit PSD images | `doHeaderInfo`, `TPSDReader::load` | Not supported before; the load returns an empty image or fails as before. |
