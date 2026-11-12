# Platform and dependency audit

Scope: Groundzero source at `2e52fb78b`, plus the fixes described below. This is a static
inspection of build selection, helper launch, audio import, and package provenance. It is
not a complete security assessment or a claim that the resulting applications have passed CI.
Prepared with Codex assistance.

## Fixed findings

### P2: Empty helper preferences disagree with discovery

`toonz/sources/toonzlib/thirdparty.cpp:195`, `:205`, and `:461` previously called
`QString::at(0)` without checking whether the FFmpeg or Rhubarb directory was empty.
An empty FFmpeg directory is the default in
`toonz/sources/toonzlib/preferences.cpp:539`. Discovery already treats it as the
application directory (`thirdparty.cpp:82`), as do the PDF and LUT helper launchers.

The old launch path could assert in a debug Qt build and did not resolve the application
directory in a release build. The launchers now apply the same empty-directory rule as
discovery. Explicit absolute and dot-relative directories retain their existing behavior.

The existing `ffmpeg_public` fixture now checks FFmpeg, ffprobe, and Rhubarb command
resolution with empty, dot-relative, and absolute directories, including spaces. The
checks inspect the command selected by the public launcher and reap any started process.
They do not require Rhubarb to be installed. Its existing export/decode/abort checks remain.

The integration review also adds HDR export cases through this public writer fixture,
because the direct FFmpeg-class fixture is excluded from MSVC builds. Both pipe and
intermediate-image modes retain floating-point sources, assert their negative/above-one
channels remain unchanged, and independently decode APNG output against expected clamped
pixels. These assertions still await CI on all three platforms.

### P2: Windows header initialization never creates the missing PNG header

`toonz/sources/CMakeLists.txt:172` previously checked the existence of the vendored PNG
directory instead of `pnglibconf.h`. A clean checkout contains that directory and the
template, but not the generated header, so the copy branch never ran.
Windows CI separately copies it at `.github/workflows/workflow_windows.yml:248`, masking
the ordinary configure-path defect.

The condition now checks the header and uses `configure_file(... COPYONLY)`. The adjacent
TIFF header copies use the same operation. Their previous `file(COPY_FILE)` requires
CMake 3.21 while the root advertises 3.14; `COPYONLY` is available in 3.14. This removes
that specific mismatch without claiming that all fetched dependencies support CMake 3.14.
Existing generated headers remain untouched.

Sources: [CMake COPY_FILE](https://cmake.org/cmake/help/latest/command/file.html#copy-file),
[CMake 3.14 configure_file](https://cmake.org/cmake/help/v3.14/command/configure_file.html).

## Dependency selection supported by the source

### Image codecs

- TIFF is active on Windows, Linux, and macOS. Windows selects checked-in
  `thirdparty/tiff-4.0.3/lib/LibTIFF-4.0.3_2015*.lib` archives in
  `toonz/sources/CMakeLists.txt:472`. `toonz/cmake/FindTIFF.cmake:18` forces the
  repository's static `libtiff.a` for the Unix builds. The Linux and macOS workflows
  build that source at `workflow_linux.yml:95` and `workflow_macos.yml:122`.
  `toonz/sources/image/CMakeLists.txt:212` links `${TIFF_LIB}` into image I/O.
- The TIFF dependency is not interchangeable without compatibility work:
  `toonz/sources/image/tzp/tiio_tzp.cpp:10` includes the private `tiffiop.h`, and
  `:229` reads `tif_header`. Toonz-specific TIFF tags are read at `:116` and `:146`.
  Any update needs legacy TZP/TZU/PLT and regular TIFF fixtures before promotion.
- Windows selects checked-in zlib, libpng, and libjpeg-turbo static archives at
  `toonz/sources/CMakeLists.txt:465`. macOS/Linux discover installed PNG, JPEG, and
  zlib libraries at `:500`, `:505`, `:516`, `:574`, `:579`, and `:587`.
  A vulnerable version cannot be inferred from a dormant source folder on those platforms.
- The `LibJPEG/jpeg-9` include entry remains at `toonz/sources/image/CMakeLists.txt:142`,
  but no corresponding tree is tracked and the Windows JPEG archive selection uses
  libjpeg-turbo. Treat this as a stale include-path candidate, not evidence that JPEG 9
  is linked. No dependency was deleted during this pass.
- OpenEXR and Imath use commit-addressed archives with SHA-256 checks in
  `toonz/cmake/OpenEXR.cmake:33` and `:63`, with an explicit system-library option.
  OpenEXR's internal compression backends are enabled at `:59`. Their notices are in
  `stuff/doc/LICENSE/LICENSE_openexr_imath.txt`, including OpenJPH, Zstandard, and
  libdeflate. A package-level comparison against the fetched sources remains unverified.

### Other native dependencies

- Windows explicitly selects bundled GLUT, GLEW, LZ4, SuperLU, OpenBLAS, and libmypaint
  at `toonz/sources/CMakeLists.txt:463` and `:481`. System/bundled LZO and SuperLU
  choices are implemented in `toonz/cmake/FindLZO.cmake` and `FindSuperLU.cmake`;
  one platform's dependency inventory should not be copied to another.
- `thirdparty/superlu/README.txt` records a thread-safety patch; this is evidence of
  local modification, not proof that every checked-in binary contains the patch.
  `thirdparty/openblas/README.txt` records build choices and a removed source archive.
  Neither document provides a complete reproducible binary manifest.
- Partio (`toonz/cmake/Partio.cmake:13`) and SeExpr
  (`toonz/cmake/SeExpr.cmake:15`) use archive digests. The Partio reader patches fail
  when their expected source no longer matches. SeExpr also records a source manifest
  in `toonz/cmake/SeExpr-source.json`. These are useful existing provenance controls.
- spdlog is commit-pinned at `toonz/sources/CMakeLists.txt:995`. The optional Python
  fallback for pybind11 still uses a version tag at `:979`, not a commit or archive
  digest. This is a remaining reproducibility difference, not a demonstrated compromise.
- LiteRT vendors headers with revision/license metadata in `thirdparty/litert/README.md`.
  The separate style worker dynamically loads a runtime; the presence of those headers
  does not mean a runtime or model is embedded in the application. Runtime/model
  acquisition and neural inference qualification are outside this audit lane.

### FFmpeg and audio

- Normal import/export executes an external FFmpeg/ffprobe pair through
  `toonz/sources/toonzlib/thirdparty.cpp:195`. The image and sound CMake targets do not
  directly link libavcodec/libavformat. The configured executable, installed packages,
  and platform bundle determine the codec version in use.
- The Windows recorder is a separate distribution: `.github/ot-dev/recorder/build-ffmpeg.sh`
  pins FFmpeg and x264 revisions, disables network support, restricts enabled formats,
  and includes source archives, notices, recipe, and build configuration. Its decoder
  build is explicitly test-only. This does not replace the general import/export helper.
- Audio import runs FFmpeg to produce 44.1 kHz stereo 16-bit PCM, buffers the output,
  then copies it into a sound track (`toonz/sources/sound/ffmpeg/tsio_ffmpeg.cpp:22`).
  This is a full-buffer import path, not streamed playback. No memory-shortage work
  from pending PR 133 was duplicated.

## Remaining findings and decisions

### P2: Audio import ignores the configured FFmpeg timeout

`toonz/sources/toonzlib/thirdparty.cpp:399` waits a fixed 30 seconds and returns false
on timeout; `preferences.cpp:542` defines a configurable timeout with a 600-second
default. `sound/ffmpeg/tsio_ffmpeg.cpp:28` then returns no sound track. A valid audio
decode taking between 30 seconds and the configured budget therefore fails despite the
preference. Timeout failure also returns before the explicit `process.close()` call.

Recommended next patch: make audio import use the same bounded timeout policy as video,
explicitly stop/reap failures, and cover timeout, start failure, and unsuccessful exit in
an audio fixture. No slow decode was executed in this audit, and no timing behavior was
changed without that fixture.

### P2: Legacy Windows binaries lack a complete source-to-artifact record

The checked-in archives named above are actual link inputs. The inspected build files
do not rebuild them during the application job or establish the compiler flags, complete
patch sets, source digests, and binary digests that produced them. The libmypaint recipe
also checks out the moving `libmypaint-v1` branch and downloads archives without digest
checks (`thirdparty/libmypaint/download-all.sh:11` and `:34`).

Recommended owner action: inventory the selected release/debug binaries, record their
digests and origin, and establish repeatable rebuilds before targeted updates. Compare
the exact source/patch set against upstream advisories. No CVE applicability or security
clearance is claimed from versioned directory names.

### P2: Windows libmypaint notices added; archive verification pending

`.github/workflows/workflow_windows.yml:354` copies the libiconv, libintl, json-c, and
libmypaint DLLs. Their notices are under `thirdparty/libmypaint/license`, whereas the
package copies build-generated `licenses` and `stuff`. The inspected `stuff/doc/LICENSE`
tree does not contain those four corresponding notice sets. No explicit copy from the
libmypaint notice directory was found in that packaging path.

The integrated workflow patch now copies that notice tree into the Windows package and
checks its required notice/author files. CI must still inspect the resulting archive.
An owner still needs to verify
the complete distribution requirements against the actual binaries and source offers.
This is a packaging evidence gap, not a legal compliance conclusion. The workflow security
lane supplied the packaging change; this dependency inventory does not establish provenance.

### P2 roadmap: macOS package availability has dated deadlines

The macOS workflow explicitly installs `qt@5` and `opencv@4` at
`.github/workflows/workflow_macos.yml:83` and `:87`. Homebrew currently lists disable
dates of 2027-05-19 for [qt@5](https://formulae.brew.sh/formula/qt@5) and 2027-07-09
for [opencv@4](https://formulae.brew.sh/formula/opencv@4). This is a scheduled dependency
availability risk, not evidence that today's job fails.

Recommended owner decision: select and qualify a supported acquisition path before those
dates. A Qt/OpenCV migration needs application, tablet, camera, audio, rendering, and
packaging evidence; no speculative major-version change was made in this pass.

## Verification and limits

- `git diff --check` passes for these changes.
- Changed C++ fixture lines pass the repository's clang-format style check.
- No application build, configure execution, automated test, mutation test, or manual
  application run was performed: `CLAUDE.md` requires builds and tests in CI.
- CI still needs to run the existing `ffmpeg_public` target on all three platforms.
  A debug Qt run should demonstrate that reverting the empty-directory checks fails;
  a release run should reject the old incorrect root-level executable path.
- Windows needs a configure run without the workflow's manual header copies, confirming
  missing headers are created and existing headers are preserved. Runtime compatibility
  with CMake 3.14 and all current fetched projects remains unverified.
- Pending PR 141's GL brush changes and PR 133's memory-shortage changes were not altered.
  No third-party source was replaced, no package was published, and no PR was opened.
