# Scene save and load profile

Master plan E6, the profiling step. Nothing is optimised yet: this page lists
the benchmark and the hot spots that reading the code suggests, so that the
next step can act on measured numbers instead of guesses. Line numbers are
those of the `tier12` branch when this was written.

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
The test fails (exit code 1) only when a loaded scene has different cells,
frame ids or levels than the saved one; different keyframe or FX node counts
are reported as a notice. The first lines give the scene size and the build
time, the last one a summary.

| Phase | What it measures |
|---|---|
| `save plain` | `ToonzScene::saveCopy`: the whole scene file written by `writeSceneFile`, as every scene save does, without the icon, the backups and the levels |
| `save compressed` | The same with the Compress Scene Files preference on (`doc/compact_scene.md`) |
| `xsheet saveData plain stream` | `TXsheet::saveData` alone through a plain `TOStream` (to the file, with a flush per line) |
| `xsheet saveData compressed stream` | The same through a compressed `TOStream`: written to memory, then LZ4 and one write. The difference with the plain one is the cost of the per line flush minus the LZ4 cost |
| `write plain bytes` | One `QFile` write of the plain scene's bytes: the floor of the file write |
| `load plain`, `load compressed` | `ToonzScene::loadTnzFile` into a new scene: the parse and the object building (not the level frame lists) |
| `free loaded scene` | Destroying a loaded scene |
| `read plain bytes` | One `QFile` read of the plain file: the floor of the file read |
| `open compressed` | The `TIStream` constructor on the compressed file: read, LZ4 decompression and the copies into the `istringstream` |
| `parse plain`, `parse compressed` | A `TIStream` walk of every tag, attribute and value without building objects: `load` minus `parse` is the object building |
| `scan plain` | `TIStream::skipCurrentTag` over the root tag: the character by character reading alone |
| `cell frame id regex` | The regular expression that `TXshLevelColumn::loadData` compiles for each `<cell>` entry, run once per entry |
| `cell frame id expand` | `TFrameId::expand` once per entry, as `TXshLevelColumn::saveData` does |
| `cell level lookups` | What `fixBiancoProblem` does for each cell after every load: a cell copy and two lookups by level name |

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
| `toonzlib/txsheet.cpp:1272` | `getFirstFreeColumnIndex()` is called for every column; it walks back over the trailing empty columns, so it is quadratic only when many trailing columns are empty (not in the benchmark) | not measured |
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
