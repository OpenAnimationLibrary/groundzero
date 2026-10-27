# Compatibility regression check

The `Compat` job of `.github/workflows/tests_linux.yml` shows that the
unmerged tiers do not change the behaviour of OpenToonz for existing content.
It builds one small tool twice, from `edition` and from the pushed branch, runs
the same workload with both and fails if any output differs.

## The tool

`toonz/sources/toonzlib/tests/compat_regression.cpp`, target
`compat_regression` (CMake option `BUILD_COMPAT_REGRESSION`, on with
`BUILD_TESTS`; it is not a ctest test).

    compat_regression <repository root> <output folder>
    compat_regression --load <scene.tnz> <output folder>

The first form writes:

| Output | Content |
| --- | --- |
| `pli/<key>.txt` | every `.pli` file of the repository (`thirdparty` excluded), as the level reader loads it: frame ids, per stroke its style, group depth, outline options (cap, join, miter) and control points (x, y, thickness), the regions, the palette pages and styles (id, tag, main colour, name) |
| `pli/<key>.pli` | the frames saved again with the level writer |
| `pli/<key>.<frame>.png` | each frame drawn by the vector renderer (128x128) |
| `scene.tnz`, `levels/` | a small scene built in memory: three columns of PLI levels (copied into `levels/`), a pegbar with keys, column keys, a blur FX with keys |
| `scene_dump.txt` | that scene loaded back: levels, columns and cells, stage objects with their keys and values at a few frames, FX nodes with their ports and parameter values, terminal FX nodes |
| `scene.<row>.png` | rows 1, 6 and 12 rendered through `buildSceneFx()` |
| `manifest.txt` | what was done, skipped or failed |

`<key>` is the path of the level in the repository, with `/` replaced by `__`
and spaces by `_`. With `--load`, the tool writes only `scene_dump.txt`, for a
scene saved by the other build.

The same source file is compiled in the `edition` tree (the job copies it and
the CMake block between the `# compat_regression: begin` and `end` markers
there), so it may use only the API that `edition` has.

## The comparison

The job runs each tool on its own checkout (`out_base/`, `out_new/`), then the
new tool on the scene of the base one (`cross_new/`) and the base tool on the
scene of the new one (`cross_base/`). Every file of `out_base/` and `out_new/`
must be identical byte for byte, and each cross dump must be identical to the
dump of the scene it loaded. It reports one notice with the counts, one error
per differing file (the first 10, with the start of the `diff -u`, or the
number of differing pixels and the largest channel difference for a PNG) and
one error with all the differing names.

## What is normalised

- The scene holds no absolute path: the levels are copied next to the scene
  and loaded with the scene folder alias (`$scenefolder/levels/...`), and the
  generator written into the scene is `compat_regression` for both builds.
- The terminal FX nodes of a scene file are written in the order of their
  addresses (`TFxSet` is a `std::set` of pointers), which can differ between
  two builds: the comparison sorts the lines of the `<terminal>` block. The
  scene has a single internal FX for the same reason, and the dumps sort the
  FX nodes and stage objects by name.
- Column FX nodes are dumped by type and column only: tier12 adds time remap
  parameters to them, which are saved only when used.

## Limits

- The renders need an OpenGL context: the job runs the tools under `xvfb-run`
  with the Mesa software renderer. Without a context the tool skips the PNG
  files (`opengl no` in `manifest.txt`) and the job warns that only the dumps
  and the saved files were compared.
- The workload covers the PLI files of the repository and one synthetic scene.
  Raster and toonz raster levels, sub-xsheets and most FX are not exercised.
