# Compact scene files

Master plan C3, parity plan P1. An optional way to save the scene file
(`.tnz`) compressed. Nothing changes unless the preference is turned on.

## Use

**Preferences > Saving > Compress Scene Files** (off by default). When it is
on, every scene save (Save Scene, Save All, autosave, Save Scene As, Save a
Copy, sub-xsheet export) writes the scene compressed. Turning it off writes
plain XML again from the next save. OpenToonz opens both kinds, so a project
can mix them. The preference only affects the scene file; levels, palettes and
the `.bak` backup copy are saved as before (the backup is a plain copy of
the previous file, whatever its kind).

## File format

No new format is invented. The compression already existed in `TOStream`
(`common/tstream/tstream.cpp`, used for compressed `.tpl` palettes) and is
LZ4, which tnzbase already links; the master plan said zstd, but a second
library for the same job was not worth it.

```
offset  size  content
0       4     "TABc"
4       4     0x0A0B0C0D, written in the byte order of the writer
8       4     decompressed size of the XML
12      4     compressed size
16      n     one LZ4 frame holding the usual `.tnz` XML
```

`TIStream` recognizes the file by the first byte ("T" instead of "<") and
decompresses it into memory, so every reader of scenes
(`ToonzScene::loadTnzFile`, `loadFrameCount`, the file browser, the farm)
reads a compressed scene with no change. Files written on a machine of the
other endianness are read too.

## Safety

- The save is as before: the scene is written to `<name>.tnz.tmp` and then
  replaces the scene. `TOStream` writes a compressed file only when it is
  destroyed and cannot report a failure then, so after the write
  `ToonzScene` checks that the temporary file starts with `TABc` and
  otherwise deletes it and throws, leaving the previous scene untouched.
- The reader was hardened for this use: a truncated header or payload, a
  declared size that LZ4 could not reach, and a declared size smaller than the
  data are rejected as corrupted files (the last one used to loop forever).

## Lazy level loading

Nothing to do here. `TXshSimpleLevel::load` only reads the frame list of a
level (`TLevelReader::loadInfo`) and registers empty frames; images are read
on demand through the image cache when a frame is first drawn or rendered.
Loading a scene is therefore dominated by parsing the XML and by the level
frame lists, which is where the compression helps when the file is large or
on slow storage.

## Compatibility

A compressed scene cannot be opened by a build without this option, and
neither by tools that read the XML directly. Plain scenes are unchanged,
byte for byte. The preference is stored in the user profile only.

## Test

`toonzlib/tests/compressed_stream_test.cpp` (CMake option
`BUILD_STREAM_TESTS`, on with `BUILD_TESTS`; the header comment has a build
line): round-trips a scene-shaped document through `TOStream` (compressed and
plain) and `TIStream`, and checks that damaged files are rejected.
