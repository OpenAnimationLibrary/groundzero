# Scene save and load profile

Master plan E6, the profiling step. This page describes the benchmark and
source-level costs to measure. The hotspot tables retain line references from
the original `tier12` review; some work has since been implemented, including
buffered plain-stream writes, cached frame-ID expressions and a single occupied
column boundary scan per xsheet save. Source changes alone do not establish a
measured speedup; compare the same fixture and settings on the same machine.

## The benchmark

`toonz/sources/toonzlib/tests/scenebench_test.cpp`, registered as the ctest
test `scenebench` (CMake option `BUILD_SCENEBENCH_TESTS`, on with
`BUILD_TESTS`). The Linux Tests workflow runs it with the other tests, then
again with `ctest -V -R '^(scenebench)$'`, and shows every line starting with
`NOTICE:` as a notice of the run.

It runs headless (`QCoreApplication`, an empty temporary stuff folder as in
`scriptengine_test.cpp`, the sandbox project) and builds in memory:

- 200 level columns of 500 cells (`--columns`, `--rows`), each with its own
  vector level (`--levels`, without drawings: only the scene file is
  measured). A third of the columns have random frame ids, which save as one
  `<cell>` entry per cell; a third are on twos (one entry per two cells); a
  third are on ones (one entry per run of 24 cells);
- 20 pegbars (`--pegbars`) with x, y and rotation keys every 4 frames, every
  column parented to one and keyed on x every 10 frames;
- a `blendFx` node between each column and the xsheet node, with its value
  keyed every 8 frames.

Each phase runs 3 times (`--repeat N`) and prints the median as
`NOTICE: <phase> <ms> ms (<count> <unit>)`, after the single runs. A phase
that throws prints `NOTICE: <phase> failed: <what>` and the others still run.
Any failed phase or round-trip comparison makes the test fail (exit code 1).
Comparisons include exact drawing placements and frame IDs, column and level
counts, keyframe counts, and internal and terminal FX counts. The output includes
the dense scene size, build time and summary, followed by the sparse-scene phases.

A separate sparse fixture keeps the dense benchmark workload unchanged. It has
two occupied columns separated by an empty column with a keyframe, followed by
512 empty columns (`--trailing-columns N`). The `save sparse plain` and
`save sparse compressed` phases time scene serialization only; setup, reload and
validation run outside the timer. They report the number of trailing columns.
Checks compare plain xsheet bytes before and after padding, round-trip the
interior column state, expand and shrink the occupied range between saves, and
exercise an entirely empty padded sheet. These are metadata fixtures; they do
not measure drawing-file serialization or end-to-end resource loading.

| Phase | What it measures |
|---|---|
| `save plain` | `ToonzScene::saveCopy`: the whole scene file written by `writeSceneFile`, as every scene save does, without the icon, the backups and the levels |
| `save compressed` | The same with the Compress Scene Files preference on (`doc/compact_scene.md`) |
| `xsheet saveData plain stream` | `TXsheet::saveData` alone through a plain `TOStream`, using buffered writes and a final checked flush |
| `xsheet saveData compressed stream` | The same through a compressed `TOStream`: written to memory, then LZ4 and one write |
| `write plain bytes` | One `QFile` write of the plain scene's bytes: the floor of the file write |
| `load plain`, `load compressed` | `ToonzScene::loadTnzFile` into a new scene: the parse and the object building (not the level frame lists) |
| `free loaded scene` | Destroying a loaded scene |
| `read plain bytes` | One `QFile` read of the plain file: the floor of the file read |
| `open compressed` | The `TIStream` constructor on the compressed file: read, LZ4 decompression and the copies into the `istringstream` |
| `parse plain`, `parse compressed` | A `TIStream` walk of every tag, attribute and value without building objects: `load` minus `parse` is the object building |
| `scan plain` | `TIStream::skipCurrentTag` over the root tag: the character by character reading alone |
| `cell frame id regex` | Historical per-cell expression-compilation cost, run once per entry; the production loader now caches the expression |
| `cell frame id expand` | `TFrameId::expand` once per entry, as `TXshLevelColumn::saveData` does |
| `cell level lookups` | What `fixBiancoProblem` does for each cell after every load: a cell copy and two lookups by level name |
| `save sparse plain`, `save sparse compressed` | Scene saves with a configurable empty column tail, isolating the serialization-boundary scan from dense cell work |

How to read the numbers: `save plain` against `write plain bytes` gives the
serialisation cost; `load plain` against `parse plain` against `scan plain`
against `read plain bytes` splits the load into object building, tag parsing,
character reading and I/O. The three `cell` phases tell how much of the load
and save a single per cell step explains.

## Likely hot spots

Found by reading the code; the benchmark phases that will confirm or reject
each one are named on the right.

### Writing

| Where | What | Measured by |
|---|---|---|
| `common/tstream/tstream.cpp:475` (`TOStream::cr`) | Every line ends with `std::endl`, which flushes the stream: on a plain scene one `write` call per line (hundreds of thousands for a large scene). The compressed stream writes to an `ostringstream`, where the flush costs little | `xsheet saveData plain stream` vs `compressed stream`, `save plain` vs `write plain bytes` |
| `tstream.cpp:486`, `:249`, `:270` (`TOStream::child`) | `os.child("tag") << value` builds a temporary `TOStream` (a `shared_ptr` copy), pushes the tag name string, and closes the tag in the destructor; used for every cell, keyframe `prev`, stage object field | `save plain` |
| `tstream.cpp:352`, `:406`, `:22` | `operator<<(std::string)` takes the string by value and tests each character with `iswalnum`; the `wstring` overload converts to a new string first; `escape` copies the string again | `save plain` |
| `tstream.cpp:566` | `operator<<(TPersist *)` looks up a `std::map` and calls the virtual `getStreamTag()` (a new string) up to three times, for every level reference of every `<cell>` | `save plain` |
| `tstream.cpp:286` | A compressed save copies the whole document out of the `ostringstream` (`str()`), then allocates the LZ4 bound: the document is in memory three times | `save compressed` vs `save plain` |
| `toonzlib/txshlevelcolumn.cpp:240` to `:283` | The cell loop copies each `TXshCell` (a reference count change) and writes `fid.expand()`, which builds an `ostringstream` per cell (`common/tsystem/tfilepath.cpp:126`) | `cell frame id expand`, `save plain` |
| `common/tparam/tdoublekeyframe.cpp:24` to `:88` | Per keyframe: a `std::map` of attributes, `operator[]` on a static map of type codes, and a `QString` conversion of the degree sign (`:59`) to compare the unit name | `save plain` (the keyframe share grows with `--rows`) |
| `toonzlib/txsheet.cpp:1272` | Previously, `getFirstFreeColumnIndex()` rescanned the trailing empty columns for every column. The implementation now computes this boundary once per save and reuses it for columns, stage objects and FX; there is no persistent boundary cache | `save sparse plain`, `save sparse compressed`; vary `--trailing-columns` |
| `toonzlib/tcolumnfxset.cpp:37`, `toonzlib/levelset.cpp:227` | FX nodes and the levels of a cast folder are saved in pointer order (`std::set<TFx *>`, `std::map<TXshLevel *, ...>`): not a speed issue, but two saves of the same scene can differ, which hurts diffs and compression | not measured |

### Reading

| Where | What | Measured by |
|---|---|---|
| `toonzlib/txshlevelcolumn.cpp:26` to `:45`, called at `:185` | `qstringToFrameId` builds and compiles a `QRegularExpression` for every `<cell>` entry | `cell frame id regex` against `load plain` |
| `common/tstream/tstream.cpp:656`, `:666`, `:684`, `:961` | The parser reads one character at a time through `std::istream::get` and `peek` (a sentry and a virtual call each) and appends to strings one character at a time | `scan plain` and `parse plain` against `read plain bytes` |
| `tstream.cpp:725` to `:787`, `:1136` | Each tag builds a `StreamTag` with a `std::map` of attributes; `operator>>(TPersist *&)` copies the whole tag, map included, for every level reference | `parse plain`, `load plain` |
| `tstream.cpp:1229` | `getTagParam(name, int &)` builds an `istringstream` for each integer attribute read (keyframe `step`, frame counts) | `load plain` |
| `tstream.cpp:899`, `:915` | A compressed file is decompressed into `m_strbuffer`, then copied into a temporary string and again into the `istringstream`; the buffer is kept until the stream is destroyed | `open compressed` against `read plain bytes` |
| `toonzlib/toonzscene.cpp:239` to `:281` (`fixBiancoProblem`) | After every load, for each cell: a `TXshCell` copy, `insertLevel` and `getLevel` by name, each copying the level's name (`TXshLevel::getName` returns a `std::wstring` by value) for a `std::map` lookup | `cell level lookups` |
| `common/tparam/tdoubleparam.cpp:1152` to `:1185` | Each keyframe is read into a `TDoubleKeyframe`, then converted to a `TActualDoubleKeyframe` with its own `TExpression` and grammar; each parameter notifies its observers at the end | `load plain` (the keyframe share grows with `--rows`) |
| `toonzlib/txsheet.cpp:1220` to `:1245` | After the FX nodes, every parameter of every FX is visited with up to four `dynamic_cast`s to set the expression grammar | `load plain` |
| `toonzlib/fxdag.cpp:264`, `:298` | The loaded FX sets are indexed with `TFxSet::getFx(i)`, which walks the `std::set` from the start (`tcolumnfxset.cpp:66`): quadratic in the number of FX nodes | `load plain` (small with 200 nodes; grows with `--columns`) |
| `toonzlib/txshcolumn.cpp:159` | `setCell` on a column appends one cell at a time (fine); on the overwrite path it calls `checkColumn()` (`:223`) even in release builds, which scans the column twice | not on the load path (cells are appended) |

## Results

Scene of 200 columns x 500 rows (100000 cells, 200 levels, 20 pegbars, 17500
keyframes, 200 FX nodes), medians of 3 runs on the GitHub-hosted Ubuntu 22.04
runner. The two runs ran on machines of different speed (building the scene
in memory took 50.6 ms and 103.2 ms), so compare within a run as well as
across runs.

| Phase | Before (run 2) | After (run 3) |
|---|---|---|
| build scene (machine speed) | 50.6 ms | 103.2 ms |
| save plain (7.25 MB) | 146.7 ms | 111.6 ms |
| save compressed (0.57 MB) | 58.1 ms | 105.9 ms |
| load plain | 483.6 ms | 210.8 ms |
| load compressed | 485.9 ms | 210.8 ms |

Before, a plain save took 2.5 times as long as a compressed one, which writes
to memory: the time went into the flush per line. After `TOStream::cr` stopped
flushing, the two take the same time. Loading took 2.3 times less time on a
machine about twice as slow: the frame id regular expression compiled per
`<cell>` (the `cell frame id regex` phase, which still replays the old
behaviour, takes 616 ms for 34814 cells on the slower machine) was most of the
load. After both changes, parsing the XML (`parse plain`, 163 ms) is most of
what is left of a load, and building the xsheet in memory most of a save.

## Not measured

- The scene icon: `ToonzScene::save` renders frame 0 at every explicit save
  (`toonz/sources/toonzlib/toonzscene.cpp:753`, `makeSceneIcon` at `:143`),
  with the backups (`:746`) and the project lookup (`:726`). It needs real
  drawings; time it in the application if the save still feels slow after the
  scene file is fast.
- The levels: `IoCmd::saveScene` (`toonz/iocommand.cpp:1710`) saves the dirty
  levels, and `ToonzScene::load` reads the frame list of every level
  (`loadResources`). The benchmark levels have no files.
- Sub-xsheets, sound and palette columns, Plastic deformations.

## Next step

Take the medians of one CI run and, for each hot spot above, compare its phase
with the totals. Only the ones that explain a large share of `save plain` or
`load plain` are worth changing. The changes must leave the saved file byte
for byte as it is today (for example: a newline without a flush, a regular
expression built once, the level name compared without a copy), so that
scenes stay readable by older builds and the compatibility rules of
`doc/development_checklist.md` hold; then run the benchmark again to show the
gain.
