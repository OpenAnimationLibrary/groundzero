# Paired performance measurements

The opt-in **Paired Benchmarks** workflow compares two validated builds on the
same runner, with Windows first and macOS second. It produces measurements, not
a speed threshold for ordinary pull requests. No performance improvement is
claimed until a compatible pair has actually run and its results are reviewed.

## Workload and compatibility

`ci-scripts/benchmarks/scene_io_v2.json` defines the versioned workload. The small
case has 20 columns, 100 rows, 20 levels, five pegbars and 512 trailing empty
columns in a separate sparse fixture. The standard case has 200 columns, 500
rows, 200 levels, 20 pegbars and 4,096 trailing empty columns. Each process uses
`scenebench_test --repeat 1`; its correctness checks remain enabled.

These fixtures measure synthetic scene metadata serialization. They do not
measure drawing files, full document load, startup, rendering, stroke latency or
scrubbing. See [the scene I/O profile](scene_io_profile.md) and the broader
[performance roadmap](performance_roadmap_2026_10_29.md) for those gaps.

Both input bundles must have identical suite contents, parameters, required
phase names/units, benchmark fixture source hash, architecture, compiler version,
Qt version, build configuration and normalized compiler/linker flag fingerprints. Source revisions and executable
hashes may differ. Missing metrics, unknown fixture arguments, an incomplete run,
a nonzero exit or a correctness failure invalidate the measurement.

**Older binaries without the sparse fixture are incompatible.** Supplying a new
suite description does not upgrade an old executable. To measure an optimization
against an older implementation, first prepare a baseline with the same benchmark
fixture changes but without the production optimization, then build both inputs.
Record those exact commits. Do not compare different workloads or label results
from unrelated hosted machines as a paired experiment.

## Preparing and running an experiment

Builds and benchmark execution run in CI, as required by `CLAUDE.md`.
GitHub may require the workflow to be integrated into the repository's default
branch before manual dispatch is available. The relocated benchmark bundles
still require an opt-in CI run for runtime validation; static review and harness
contract checks do not establish that validation or a performance result.

1. Manually dispatch **Windows Build** for each chosen revision with
   `benchmark_bundle` enabled. For the macOS experiment, use **MacOS Build**.
   The switch defaults to false and does not run a benchmark. Existing builds
   and correctness checks run normally; a successful run publishes the optional
   `benchmark-input-windows` or `benchmark-input-macos` artifact.
2. Record each successful build's run ID and full commit SHA. Both artifacts
   must still be available. Bundles reuse compiled executables and deployed
   dependencies. macOS uses a separate copy of the app's Contents directory and
   deploys the benchmark's dependencies into that copy.
3. Manually dispatch **Paired Benchmarks** with the platform and both run ID/SHA
   pairs. It checks the source workflow, successful conclusion and exact SHA,
   resolves immutable artifact IDs and digests, then downloads those specific
   artifacts. It never dispatches or retries a build automatically.
4. Review `comparison.json` and the per-invocation logs in the resulting artifact.
   A failed measurement still uploads its partial report and diagnostics. Fix
   its recorded cause before collecting another sample.

The underlying CI entry points are `benchmark_bundle.py` for packaging and
`paired_benchmark.py` for measurement. The latter accepts `--baseline` and
`--candidate` archive paths, `--provenance` JSON and a fresh `--output` directory.
Optional `--samples`, `--warmups` and `--timeout` control the experiment; workflow
defaults are nine measured pairs, one warmup pair and 300 seconds per process.

## Reading the report

Each case runs baseline/candidate, then candidate/baseline, alternating throughout
warmup and measurement. Warmups must pass but are excluded from summaries. Both
sides use fresh processes, working directories and the fixture's temporary
preferences. OS caches remain warm; this is not a controlled cold-disk experiment.
Standard output and errors go directly to files, avoiding pipe-buffer deadlocks.

The JSON includes raw samples, median, nearest-rank p95, minimum/maximum, paired
absolute differences and the percentage change between medians. Positive change
means the candidate used more time or memory. Zero baseline timings have no
percentage ratio. The p95 describes repeated benchmark processes, not interactive
event latency; with nine samples it is the maximum. Use more repetitions and
inspect variation before interpreting small differences.

Phase timings come from the benchmark's internal timer, excluding the setup and
validation documented for that phase. Process wall time additionally includes
startup, fixture construction, checks and shutdown. Native memory accounting is
Windows `GetProcessMemoryInfo().PeakWorkingSetSize` or macOS `wait4().ru_maxrss`
(bytes). Windows measures the benchmark process itself. POSIX accounting for the
reaped child can include high-water contributions from descendants it waited for;
the current metadata fixture launches no external workers. Neither counter is
simultaneous **total process-tree memory**, live allocation size or a leak
measurement. An unavailable native counter fails measurement rather than being
reported as zero.

Metadata records platform/release/architecture, CPU model and logical CPU count,
runner image, Python/compiler/Qt versions, build configuration, suite and executable
hashes, exact source revisions, run IDs, artifact IDs/digests and the downloaded
archive hashes. Compiler flags are fingerprinted after normalizing the checkout
prefix. Host names, account identities, full environment dumps and private paths
are omitted; known path prefixes in diagnostic logs are normalized.

Hosted runners can vary in scheduling, frequency, thermal state and storage.
Alternation reduces ordering bias but does not remove those sources of noise.
Treat the report as evidence to investigate. Accept optimizations only when the
correctness gates remain green and repeated, matched experiments support the
claimed gain. Ordinary builds run lightweight harness contract checks on Windows
and macOS; those check failure handling, workload compatibility, ordering, units
and native memory accounting without imposing performance thresholds.
