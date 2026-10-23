# Threading kernel validation and performance evidence

## Scope and method

Measured on 2026-10-04 with Clang 18, the Development build (optimized, assertions
enabled), on an Intel Core i5-8265U: four physical cores, eight logical CPUs.
No affinity or frequency pinning. Static analysis and sanitizer tests shared
the machine; the recorded one-minute system load was 7.24-7.27. These are
contended-machine observations, not an isolated latency guarantee.

The optional `ludus_threading_benchmark` builds 256 independent jobs before timing.
Each sweep uses one warm-up and 11 complete Submit-to-Wait samples per worker/grain
combination. Three sweeps were run; the table is the median of their medians in
milliseconds. Worker counts exclude the helping caller. Every job output is compared
with its serial oracle; all comparisons passed. This is a synthetic arithmetic
batch, not an engine-frame or competitive scheduler benchmark.

| Iterations per job | 0 workers | 1 worker | 2 workers | 4 workers | 8 workers |
| --- | ---: | ---: | ---: | ---: | ---: |
| 0 | 0.007905 | 0.040011 | 0.053121 | 0.090280 | 0.121377 |
| 10000 | 5.838764 | 2.907198 | 2.022913 | 1.277750 | 1.144395 |
| 100000 | 58.761773 | 29.747872 | 20.214239 | 13.258273 | 12.040987 |

Checksums across all worker counts: `256`, `18367026793921049022`, and
`7928766144346410088`, respectively. Checksums supplement the per-job equality
checks; equality is not inferred from checksum agreement alone.

Tiny jobs show a clear dispatch penalty. At 10,000 and 100,000 iterations, four
workers plus a helping caller completed these batches about 4.6x and 4.4x faster
than the serial executor. Eight workers do not provide a reliable improvement
over four across all sweeps on this SMT machine. Keep worker budget explicit and
batch small tasks; neither the results nor hardware thread count justify consuming
the entire CPU budget of a game. The shared gate remains a known scaling limit.

Reproduce with:

```sh
./init.sh --cli --locked --with-tests --preset-only linux-clang-development
out/host-tools/venv/bin/cmake --preset linux-clang-development \
  -DLUDUS_BUILD_THREADING_BENCHMARK=ON -DLUDUS_WARNINGS_AS_ERRORS=ON
out/host-tools/venv/bin/cmake --build --preset linux-clang-development \
  --target ludus_threading_benchmark
out/build/linux-clang-development/modules/foundation/threading/ludus_threading_benchmark
```

## Correctness coverage

The threading suite exercises graph bounds, cycles, duplicate/stale/foreign
handles, ordinary non-atomic output publication through fan-in dependencies, wide
fan-out/fan-in and repeated reuse, serial/worker execution, invalid callback
outcomes, failure/cancellation propagation, recursive-wait rejection, graph/system
destruction and draining. Startup-failure tests inject synchronization
initialization and partial thread startup failures and verify joins and retry:
Linux through GNU `--wrap` on the unmodified archive, macOS and Windows through a
private compile-time seam in an unexported test backend (see
[platform completion](#platform-completion-t3)). Allocation injection
checks initialization cleanup; a separate allocation-counting executable checks
zero scheduler allocation during repeated Add/Seal/Submit/Wait/Reset cycles.

TSan runs the execution and native startup tests independently of ASan/UBSan. The
browser serial smoke checks dependency execution, reset invalidation and failure
blocking under the pinned Emscripten toolchain. An installed SDK consumer links
and executes the public API without private engine headers. CI also exercises the
normal Debug/Development, sanitizers, PCH, public-header and build-time-budget gates.

The Windows backend is compiled and executed by a dedicated CI lane, recorded
[below](#platform-completion-t3). The first real engine workload, the static-lighting bake,
is recorded [below](#static-lighting-bake-t1). See [the architecture](../architecture/threading.md) for the staged measurement
and consumer-integration plan.

## macOS ARM64 validation (T0)

Measured on 2026-10-16 on an Apple M5 (4 performance and 6 efficiency cores,
10 logical CPUs, 16 GiB) running macOS 26.6. The pinned Clang 18.1.8 built the
Development configuration with warnings as errors. Other builds shared the machine;
the one-minute load average was 24.8-25.2.

- `ludus_foundation_threading_tests` (16 test cases, including the T0 contract
  tests) passed in 200 consecutive runs. `ludus_threading_alloc_tests` passed in 20
  repeated CTest runs, with zero scheduler allocations during steady reuse.
- `ludus_threading_benchmark` produced the same checksums as the Linux run above
  (`256`, `18367026793921049022`, `7928766144346410088`). Every per-job output
  matched the serial oracle for 0, 1, 2, 4 and 8 workers. Because of the load,
  its timings are not reported as performance evidence.
- ASan/UBSan and TSan were not completed on this host. With the pinned LLVM 18
  runtime on macOS 26.6, ASan deadlocks during runtime initialization before
  `main` (an existing ASan test binary from another checkout behaves the same), and
  an empty TSan program crashes on start. These are toolchain failures before test
  entry, not threading results. CI's macOS 14 ASan/UBSan job now runs
  `ludus_foundation_threading_tests`; the development job runs every test labelled
  `threading`.

Milestone T3 later added macOS startup-failure injection, a completed macOS TSan
run and Windows execution; see [platform completion](#platform-completion-t3).

## Static-lighting bake (T1)

Measured on 2026-10-16 on the same Apple M5 (4 performance and 6 efficiency cores,
16 GiB, macOS 26.6), with the pinned Clang 18.1.8 Development build (optimized,
assertions enabled), no affinity or frequency pinning. Other sessions share this
machine, so the benchmark started only once the one-minute load average fell below 4.
The reported sweep ran at a load of 3.8-4.2.

`ludus_static_lighting_benchmark` bakes a 2,062-triangle scene: a point-lit
32×32-cell receiver grid under 13 area emitters and a partial blocker, at the
editor's quality of 64 samples per triangle and depth 4. Scheduled bakes use the
default 64 triangle ranges plus one preparation job. Each configuration runs one
warm-up and 21 measured bakes, and every run (warm-up included) compared every chart
byte for byte with the serial `Step` oracle. All runs matched. Worker counts exclude the
caller, which helps in `Finish`, so *N* workers execute on *N*+1 threads. Times are
milliseconds unless marked. Percentiles are nearest-rank over the 21 runs.

| Configuration | Bake p50 | p95 | p99 | Critical path p50 | Callback work p50 | Dispatch to first range p50 | Overhead p50 | Speedup p50 |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| Former editor path: `Initialize` + `Step(256)` on one thread | 135.18 | 136.46 | 141.14 | — | — | — | — | — |
| Serial executor (0 workers) | 121.16 | 126.90 | 130.95 | 3.79 | 121.15 | 0.2 µs | 0.02 | 1.00 |
| 1 worker | 76.35 | 84.89 | 85.39 | 4.69 | 149.80 | 2.2 µs | 1.45 | 1.59 |
| 2 workers | 56.81 | 57.82 | 58.46 | 5.17 | 164.96 | 4.5 µs | 1.82 | 2.13 |
| 4 workers | 40.00 | 41.29 | 42.06 | 6.51 | 189.95 | 9.8 µs | 2.01 | 3.03 |
| 8 workers | 28.80 | 39.39 | 42.64 | 7.38 | 230.53 | 28.2 µs | 3.19 | 4.21 |

Definitions, from `StaticLightingBakeProfile`:

- *Bake* is the owner's wall time from `Submit` until `Finish` returns, including
  allocation, graph construction and the owner merge.
- *Critical path* is preparation + dispatch to the first range + the longest range
  + the owner merge. That is the span of this two-level graph. Serially it is 3.8 ms:
  preparation (BVH and surface terms) plus one range of about 1.9 ms (121 ms / 64).
- *Callback work* sums every job callback's wall time plus the merge. The owner merge
  (status merge and chart encoding) took 0.05-0.09 ms at p50.
- *Overhead* is bake time minus the lower bound max(critical path, callback work /
  executing threads). On the serial executor that is pure scheduling cost: 0.02 ms
  for 65 jobs, about 0.3 µs per job. With workers it also includes load imbalance,
  wake latency and the 64-range tail. That totals 1.4-3.2 ms, or 2-11% of the bake.

Interpretation:

- The serial executor costs nothing measurable compared with the former single-thread
  loop. The two differ by about 10% between sweeps in both directions (a second sweep at
  load 4.2-5.5 measured 119.90 and 122.51 ms). This is the oracle configuration.
- Callback work grows from 121 ms serial to 231 ms with 8 workers. Once
  more than four threads run, the efficiency cores and shared caches make each path
  slower, so throughput scales sublinearly: 4.2× on nine threads. The critical path
  (7.4 ms) is not the limit at this size. Dispatch to the first range grows with the
  wake herd (28 µs at eight workers), but it stays well under 1% of the bake.
- The editor runs three workers and polls without helping, which also means three
  executing threads, so it is comparable to the 2-worker row. That moves the former
  135 ms bake to about 57 ms of background latency while keeping cores free for Qt and
  the RHI. The second sweep's 2-worker row measured 75.31 ms. Run-to-run variance on
  this shared machine is therefore around ±30% at a given budget, and these figures
  are not latency guarantees.
- A third sweep was discarded: another session's build raised the load to 21 during
  it, and its serial executor median rose to 328 ms.

Correctness evidence for the consumer, in the `[static-lighting][threading]` renderer
tests:

- Charts equal the serial `Step` oracle bit for bit for 0/1/2/4 workers × 1/3/64/1024
  ranges, including more ranges than triangles, through both `Poll` and `Finish`.
- Admission and preparation failures (uninitialized or busy system, invalid range
  count, preparation-time validation failure) leave outputs and profiles preserved.
- Cancelling mid-evaluation stops ranges, reports `NotReady` and releases the system.
- `Cancel` followed by `Shutdown` reports cancellation, `Shutdown` without `Cancel`
  drains the bake to its oracle result, and destroying an in-flight bake cancels and
  drains it before the next bake reuses the system.

These cases passed 200 consecutive runs. The full renderer suite passed (96 cases),
and so did the editor suite (185 passed, 6 skipped), including the `StaticLightingJob`
interruption and destruction test. ASan/UBSan and TSan for this consumer must come
from CI for the reason given in the T0 section.

Reproduce with:

```sh
out/host-tools/venv/bin/cmake --preset macos-clang-development -B out/build/t1-benchmark \
  -DLUDUS_BUILD_THREADING_BENCHMARK=ON -DLUDUS_BUILD_EDITOR=OFF
out/host-tools/venv/bin/cmake --build out/build/t1-benchmark --target ludus_static_lighting_benchmark
out/build/t1-benchmark/modules/graphics/renderer/ludus_static_lighting_benchmark
```

## Execution traces (T2)

### Contended sweep (2026-10-17)

Measured on the same Apple M5 (4 performance and 6 efficiency cores, 16 GiB, macOS
26.6) with the pinned Clang 18.1.8 Development build (optimized, assertions
enabled), with no affinity or frequency pinning. Other sessions were building on
the machine throughout: the one-minute load average, sampled every 5 s, ranged from
**24.5 to 99.5** between 01:15 and 01:37. These figures are **relative overhead
measured under heavy contention, not isolated timings**. No absolute latency from
this sweep is reported as a result.

Method: each benchmark keeps two systems with the same worker budget, one without
hooks and one with `MakeProfilerJobHooks`. It alternates which system runs first
in each round, so drift affects both alike. Each round gives one *pair*, and the
overhead of a pair is traced/untraced − 1. The tables show the median overhead and
the p25–p75 interval across pairs (nearest rank). Every traced and untraced output
matched the serial oracle (threading) or the `Step` oracle charts (bake), and no
trace event was dropped. Run `ludus_threading_benchmark --tracing` and
`ludus_static_lighting_benchmark --tracing [trace.json]` to reproduce.

**Tracing off: hooks installed, no capture.** `ludus_threading_benchmark` ran 31
pairs for each of 15 configurations (0, 10,000 and 100,000 iterations × 0, 1, 2, 4
and 8 workers), in three sweeps. The 45 median overheads range from −35% to +8%, and
every p25–p75 interval contains zero (one has zero as its upper bound). **No overhead
is measurable at this noise level**, as the design predicts: a declined submission
adds one branch per job.

**Tracing on: capture active.** Empty jobs (0 iterations) isolate the recording
cost: every job records two slices, a flow end, a counter sample and any flow
starts. That cost is clearly separable:

| Workers | Sweep 1 median (p25–p75) | Sweep 2 | Sweep 3 |
| ---: | ---: | ---: | ---: |
| 0 | +766% (+736 to +1186) | +758% (+693 to +1015) | +779% (+759 to +999) |
| 1 | +516% (+237 to +926) | +362% (+240 to +527) | +919% (+451 to +1243) |
| 2 | +569% (+181 to +1019) | +262% (+148 to +448) | +519% (+286 to +937) |
| 4 | +153% (+59 to +790) | +189% (+130 to +323) | +265% (+198 to +405) |
| 8 | +159% (+95 to +284) | +136% (+98 to +208) | +221% (+112 to +339) |

On the serial executor, recording an empty job's trace costs about eight times
more than scheduling it, and the factor is stable across sweeps. With workers, the
recording runs in parallel on each executing thread and the base cost includes wake
latency, so the relative figure is lower and less stable. With work in the jobs
(10,000 or 100,000 iterations), 29 of the 30 per-sweep configurations have a
p25–p75 interval containing zero (medians −8% to +29%). The exception (100,000
iterations, 4 workers, sweep 2: +11%, +0.7 to +17.9%) did not repeat in the other
sweeps. At these grain sizes the recording cost cannot be separated from noise.

**Static-lighting bake** (21 pairs per row; 65 jobs per bake; 0 workers and the
editor's 3):

| Configuration | Sweep 1 median (p25–p75) | Sweep 2 |
| --- | ---: | ---: |
| No capture, 0 workers | −9% (−14 to +16) | +34% (−9 to +76) |
| No capture, 3 workers | −12% (−44 to +49) | −1% (−16 to +10) |
| Capture, 0 workers | −8% (−23 to +17) | −1% (−22 to +11) |
| Capture, 3 workers | +6% (−32 to +87) | −3% (−11 to +21) |

Every interval contains zero. Tracing a real bake, idle or recording, shows **no
overhead measurable at this noise level**. With about 65 × 6 events per bake, the
recording cost is small next to the bake's path tracing. The exported traces show
the `StaticLightingPrepare` slice, 64 `StaticLightingRange` slices per bake on the
named `Bake worker` tracks and the helping owner, the dispatch arrows and the
ready-depth counter. The capture of sweep 1 holds 66 bakes: 66 preparation
slices, 4,224 range slices and 4,290 `JobDispatch` flow ends, one for every job.

Correctness evidence, in the threading suites:

- `[threading][trace]` cases (0, 1 and 4 workers, helping and non-helping `Wait`, 20
  reuses each): every executed job reports `Started`, its `Signalled` edges and
  `Finished` on one thread, in that order. Ready sequences form a permutation with
  roots first in Add order. A dependent's `Started` names the edge whose earlier
  `Signalled` released it. Every report has returned when `Wait` returns. The serial
  executor reports one fixed order. Failed jobs signal no edges, Blocked jobs are not
  reported, a declining filter leaves a submission completely untraced, and worker
  hooks run once per worker on the worker before `Initialize` returns and before
  `Shutdown` joins. Lifecycle calls from hooks return `InsideJob`.
- `ludus_threading_profiling_tests` exports a capture of a labelled fan-out graph and
  checks named worker tracks, one slice per job under its label, balanced slices,
  five `JobDispatch` flow starts with five matching ends, and six ready-depth samples.
  A profiling-disabled build checks that the adapter returns empty hooks.
- `ludus_threading_alloc_tests` counts zero allocations over 100
  Add/DependsOn/Seal/Submit/Wait/Reset cycles, with 0 and 3 workers, in four modes: no
  hooks, a plain trace hook, profiler hooks without a capture and profiler hooks
  recording into an active capture.

The trace cases passed 300 consecutive runs and the profiler/allocation executables
50 runs each on this machine. The renderer suite (96 cases) and the full CTest run
passed except `ludus_platform_cocoa_tests`, which skips every case without
`LUDUS_TEST_COCOA=1` and is unaffected by this change. As in T0 and T1, ASan/UBSan and
TSan results for these paths come from CI.

## Platform completion (T3)

Recorded on 2026-10-17 from the `Threading platforms` workflow
(`.github/workflows/threading-platforms.yml`, run 39044027072 on PR #322). Every
batch runs the same lanes, and promotion requires them. See
[Continuous integration](continuous-integration.md#threading-platform-lanes).

### macOS startup-failure injection

Apple's linker has no GNU `--wrap`. On macOS and Windows,
`ludus_threading_startup_tests` links an unexported test backend that compiles
`jobs.cpp` and the platform's native source with `LUDUS_THREADING_NATIVE_FAULT_TESTING`.
In that build only, `src/internal/native_test_hooks.hpp` hooks run before
thread creation, thread join and gate mutex/condition initialization. The test
executable defines them. Production archives and the installed SDK contain no hooks
or fault state, and the public API is unchanged. The test cases are shared with
the Linux `--wrap` variant. A start failing at worker 1, 2 or 4 of 4 must report
`ThreadCreationFailed`, join exactly the workers already started, and leave a
system that initializes and shuts down again. Mutex or condition initialization
failure must leave a reusable system.

On the Apple M5 (macOS 26.6, Clang 18.1.8 Development build, warnings as errors),
the startup tests (2 cases, 18 assertions) passed in 200 consecutive runs. Local
ASan/TSan remain unavailable on that host, as recorded for
[T0](#macos-arm64-validation-t0); the sanitizer result comes from CI below.

### macOS ThreadSanitizer

The hosted `macos-15-arm64` image (macOS 15.7.9, build 24G830) ran Homebrew
Clang 18.1.8, the pinned major, with `-DLUDUS_ENABLE_TSAN=ON` and warnings as
errors. The pinned runtime starts there, unlike on macOS 26 hosts.

- `ludus_tsan_control` detected its deliberate race in 16 of 16 runs before
  any threading result was accepted.
- `ludus_foundation_threading_tests` (21 cases, 14,303 assertions),
  `ludus_threading_startup_tests` (2 cases, 18 assertions, through the seam) and
  `ludus_threading_profiling_tests` (2 cases, 51 assertions) passed under TSan.
- The scheduler and startup suites then passed 10 further repeated CTest runs each.

This is the completed macOS sanitizer result required by the
[baseline gate](../architecture/threading.md#baseline-completion-gate). The
allocation-counting suite is excluded from sanitized builds by design.

### Windows backend

The hosted `windows-2022` image (20261012.333.1, x86_64) ran Chocolatey's
LLVM 18.1.8 `clang++` targeting `x86_64-pc-windows-msvc`, with Development
flags and warnings as errors. Catch2 came from the locked local recipe with
`config/conan/profiles/windows-clang-x86_64`.

Current Microsoft STL releases reject Clang 18. VS 2022 17.13 and newer require
Clang 19, and the image's VS 2026 STL (14.51) requires Clang 20 (`STL1000`). The
lane installs the MSVC 14.40 toolset (VS 2022 17.10 LTSC) and selects it through
`VCToolsInstallDir`. A Windows build with the pinned LLVM therefore needs that
toolset, or an LLVM pin update, which would be a separate toolchain decision.

On Windows, CMake configures only the FoundationThreading closure (Base, Memory,
Containers, Hash, Strings, Time, Profiling and Threading). FoundationBase uses a report-only
assertion backend: emergency bytes go to the standard error handle, and the
POSIX report socket and helper control endpoint are unavailable. The Microsoft
CRT's deprecation of portable ISO C calls is disabled with `_CRT_SECURE_NO_WARNINGS`.

- `ludus_foundation_threading_tests` (21 cases, 14,550 assertions; the count
  varies with timing-dependent loops), `ludus_threading_startup_tests` (2 cases,
  18 assertions, through the seam on `_beginthreadex`/`WaitForSingleObject` and
  gate initialization), `ludus_threading_profiling_tests` (2 cases, 51 assertions)
  and `ludus_threading_alloc_tests` (3 cases, 104 assertions, zero steady
  scheduler allocations) passed.
- The scheduler and startup suites then passed 20 further repeated CTest runs each.

This establishes that the Windows backend compiles, executes and passes the
threading contracts. It does not provide a Windows SDK, other engine modules on
Windows, Windows ASan/TSan or Windows performance measurements.
