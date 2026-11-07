# Image-cache builder leak qualification

The compressed-memory and compressed-disk cache items own an `ImageBuilder`.
Their destructor repair deletes that builder. The existing `timagecache` fixture
checks pixels, aliasing and cache removal, including 64 compressed RGBA32 cycles
and 64 compressed CM32 cycles. Those functional assertions alone pass with the
missing delete; LeakSanitizer supplies the ownership oracle.

Linux GCC/Clang builds link only `timagecache_test` with `-fsanitize=leak`.
Standalone LSan intercepts allocation functions and scans reachable allocations;
it can detect leaks originating in the otherwise uninstrumented `tnzcore` shared
library. This differs from ASan/UBSan checks that require instrumentation of the
implementation's instructions. See the [GCC instrumentation options](https://gcc.gnu.org/onlinedocs/gcc/Instrumentation-Options.html)
and [Clang LSan usage](https://clang.llvm.org/docs/LeakSanitizer.html).
The application and packaged libraries do not link this test runtime.

The existing Linux job runs `ci-scripts/qualify_image_cache_lsan.py` after the
application build. It performs three checks:

1. The normal fixture must complete its pixel/removal assertions and exit zero
   without a leak report.
2. A disposable source copy removes exactly one `delete m_builder` from
   `CompressedOnMemoryCacheItem`. Its fixture must finish the functional checks,
   exit with LSan code 23 and report a direct allocation stack from the cache
   builder path. A crash, unrelated leak or ordinary failure is insufficient.
3. Restoring that delete in the same disposable tree and rebuilding must make
   the same fixture pass with no leak report.

The copied build reuses the normal build's selected compilers, compiler/linker
flags and feature options. It copies `toonz/sources`, `toonz/cmake` and populated
FetchContent source directories and the SeExpr fixture sources needed for CMake
generation, then configures offline. Linux SDK/install
inputs remain read-only references; application build outputs are never copied
or modified. Only `timagecache_test` and its dependencies are built, using the
existing compiler launcher/cache. Temporary source/build trees are removed;
normal, mutation, repair and configuration logs plus source/fixture hashes remain
under `toonz/build/image-cache-lsan` in the existing reliability artifact.

`ci-scripts/tests/test_image_cache_lsan.py` covers the exact mutation guard,
required LSan exit/stack, fixed-result checks, retained build flags, copy isolation
and timeout process-group cleanup. Both those contracts and C++ qualification
run only in CI. No new PR job or matrix axis is introduced, and no leak
suppression or RSS threshold is used.

This qualification is authored, not runtime-verified. Record the exact CI commit,
run/job and all three result logs before claiming the leak-restored version
fails and the repair passes. The current repeated fixture does not force the
compressed-disk spill/clone path or constructor failures. Those cleanup paths
remain outside this differential result, as do reachable global caches and
application-wide scene, rendering, plugin and undo memory lifetimes.
