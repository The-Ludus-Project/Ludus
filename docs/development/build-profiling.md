# Build Profiling

Ludus treats build time as a measured quantity, not a feeling. This page
describes how to profile a build and records the current baseline so future
changes can be judged against numbers.

## How to profile

```bash
./scripts/profile-build linux-clang-profile
```

This performs a **clean** build of the preset (the build tree is wiped first so
runs are comparable) and:

- times CMake **configure** and **build** separately;
- parses Ninja's `.ninja_log` for per-target timings, total wall-time, and
  achieved parallelism (summed compile time / wall time);
- compiles with Clang `-ftime-trace` (via `LUDUS_ENABLE_TIME_TRACE=ON`) and
  aggregates the per-translation-unit traces with
  [ClangBuildAnalyzer](https://github.com/aras-p/ClangBuildAnalyzer) into a
  ranked report of the most expensive headers and template instantiations.

Outputs land under `out/profile/`:

| File | Contents |
| --- | --- |
| `<preset>-profile.json` | machine-readable summary (timings, Ninja stats) — diff this between runs |
| `<preset>-analysis.txt` | ClangBuildAnalyzer ranked report (headers, templates, frontend/backend split) |
| `<preset>-cba.bin` | raw aggregated trace (input to `--analyze`) |

Flags:

- `--no-time-trace` — skip `-ftime-trace`/ClangBuildAnalyzer (Ninja timings only; faster).
- `--ccache` — enable ccache for the profiled build (measure warm-cache rebuilds).

ClangBuildAnalyzer is fetched and built on demand into `out/host-tools/`; it is
**not** part of `./init.sh`, so normal onboarding is unaffected.
The pinned 1.6.0 release updates its bundled simdjson for newer compilers and
recognizes Clang 19 and later header trace events. Thanks to
[Aras Pranckevičius and the release contributors](https://github.com/aras-p/ClangBuildAnalyzer/releases/tag/v1.6.0).
Each analyzer version has its own source/build directory, so an older cached
binary cannot silently satisfy a newer pin. Existing profiling budgets remain
unchanged.
Bootstrap also adds the explicit `<cstdlib>`, `<type_traits>` and `<exception>`
includes required by its bundled enkiTS and LLVM demangler with libc++ 23.
These marked include edits preserve upstream bodies and licenses, accept an
already applied profile, and reject changed include anchors.

## How to improve, quantitatively

1. Run `profile-build` to get a baseline.
2. Change **one** variable (prune an include, add a PCH, enable a lever).
3. Re-run `profile-build` and diff `<preset>-profile.json` and the analysis
   report. Keep the change only if the numbers improved.

Do not adopt an optimization without a before/after measurement from this
harness.

## CI tracking

CI builds with ccache enabled (`LUDUS_ENABLE_CCACHE=1`) and a persistent cache.
Each run records total build wall-time and `ccache --show-stats` (hit rate) in
the job summary, so cache effectiveness and build-time trends are visible per
commit.

The build-budget job uses `CMAKE_BUILD_PARALLEL_LEVEL=2` to keep CPU contention
stable when summing elapsed frontend timings. Reproduce that job locally with:

```bash
CMAKE_BUILD_PARALLEL_LEVEL=2 ./scripts/check-build-budget --profile
```

The previous unrestricted CI profile in [run 36767757513](https://github.com/The-Ludus-Project/Ludus/actions/runs/36767757513)
reported 129.2 s frontend time, 32.98 s build wall time, and 5.71 effective
parallelism. All reported project headers passed their existing limits. The
aggregate limit remains 120 s; compare future profiles at the same concurrency
before treating timing changes as include regressions.

## Baseline

### LLVM 23 reference-toolchain calibration

The compiler migration changes the reference measurement. These uncached
Ubuntu 24.04 CI runs both used two build jobs and the complete Development
graph, without PCH, source exclusions or reduced tests:

| Measurement | LLVM 18, [run 39067596315](https://github.com/The-Ludus-Project/Ludus/actions/runs/39067596315) | LLVM 23.1.3, [run 39070823313](https://github.com/The-Ludus-Project/Ludus/actions/runs/39070823313) |
| --- | ---: | ---: |
| Ninja edges | 640 | 643 |
| Build wall time | 263.56 s | 277.37 s |
| Summed frontend parsing | 307.8 s | 367.2 s |
| Summed backend/codegen | 175.6 s | 146.3 s |

The build wall time differs by about 5.2%; frontend parsing differs by 19.3%.
The compiler and analyzer versions differ, so these runs do not isolate a
source include regression or establish a controlled optimization result.
ClangBuildAnalyzer 1.6.0 also corrects duplicate frontend-file reporting and
Clang 19+ header events; its compilation-event count is not directly comparable
with the old report. Ninja edges identify the nearly unchanged build graph.

Every reported project header passes its existing limit. The LLVM 23 report's
largest include costs are Catch2's assertion header and standard string/span
headers; no new heavy public include was introduced by the promotion repairs.
The old 320-second aggregate had only about 4% headroom over the earlier
307.8-second run. Recalibrate only that aggregate to 440 seconds, about 20%
above the measured LLVM 23 baseline, following the existing calibration policy.
Keep per-header limits, overrides, assertions, tests and profiling inputs
unchanged. Verify the next complete LLVM 23 run and tighten the aggregate when
comparable repeated profiles justify it; continue to fix avoidable includes.

### Original small-graph baseline

Captured with the pinned toolchain (Clang 18, libstdc++ 14, Ninja, lld) on the
`linux-clang-profile` preset (RelWithDebInfo, `-ftime-trace` on, ccache off),
clean build. Absolute seconds are host-dependent — treat the **ratios and the
ranked lists** as the durable signal.

- Configure: ~0.7 s
- Build (Ninja wall): ~5.0 s across 64 edges, ~7.2x achieved parallelism
- Frontend (parsing) vs backend (codegen): ~23 s vs ~10 s summed across TUs

### Top compile-time costs (baseline)

Most expensive headers, by aggregate parse time across all TUs:

| Header | Aggregate | Note |
| --- | --- | --- |
| `foundation/logging/log.hpp` | ~8.3 s (7×, avg ~1.2 s) | pulls in `<format>` → `<chrono>` + Unicode/format machinery; taxes every logging consumer |
| `catch2/catch_test_macros.hpp` | ~2.7 s (4×) | test-only |
| `<chrono>` | ~2.3 s (6×) | dragged in transitively by `<format>` and Catch2 |
| `foundation/logging/src/sinks/file_sink.hpp` | ~1.7 s (2×) | pulls `<filesystem>` |

### Applied optimizations

- **Type-erase `std::format` at the logging boundary** (ADR 0004). `Log(...)`
  now erases to `std::format_args` and calls a single non-template `VLog` in
  `logger.cpp`, so `std::vformat` is instantiated once instead of per TU.
  Measured on `linux-clang-development`: frontend 52.5 s → 22.5 s (−57%),
  backend 21.9 s → 9.0 s (−59%), `log.hpp` aggregate ~19 s → ~9 s.

### Improvement backlog (measure each before/after)

- **`log.hpp` residual cost** is now `<format>` being *parsed* (still included
  for `std::format_string`/`std::make_format_args`) plus transitive `<chrono>`.
  Next lever: a precompiled header for the stable heavy STL headers, or trimming
  the header further.
- **Precompiled header** for the foundational vocabulary is now available
  (`-DLUDUS_ENABLE_PCH=ON`; `cmake/EnginePch.cmake`, ADR 0007). It precompiles
  `core.h` only — a build accelerator, not a place to park heavy STL. Extending
  a PCH to cover `<format>`/`<chrono>`/`<string>` for the modules that consume
  them would be a *separate*, measured decision, and must not turn the PCH into
  the dependency model.
- **`file_sink.hpp`** still pulls `<filesystem>`/`<format>` into a header — move
  those into the `.cpp`.
- **`version.hpp`** is unexpectedly heavy for a version header — IWYU candidate.
- **ccache** warm-cache rebuild time (enabled in CI; watch hit rate).
- **include-what-you-use** pass to prune transitive includes at the root.

These are candidates, not commitments — each must show a win in
`profile-build` output before it lands.

## Assertion test graph budget recalibration

The supplied assertion-branch CI log shows successful compilation followed by
an aggregate-budget failure: **73.8 s summed frontend time against 65 s**. Build wall
time was **18.51 s**. These are different metrics: the frontend number adds
time across concurrent compiler invocations, including tests and dependency
scanning; it is not the developer's elapsed build time.

All project headers listed in that CI report passed their existing limits:
`log.hpp` averaged 2443 ms (limit 3200), `file_sink.hpp` 1748 ms (2400),
`version.hpp` 547 ms (2000), `debugger_sink.hpp` 1250 ms (2000), and
`formatter.hpp` 623 ms (2000). The aggregate limit had been calibrated to
approximately 45 s before adding the assertion runtime, unit tests, native and
fake backend death tests, allocation probes, logger-lifetime tests, and build
contract tests. The new assertion headers do not include the heavy standard
headers seen in the report. In particular, `<barrier>` is test-only; its two
expensive inclusions came from compiling the same death-test driver twice.

### Optimization and measurement

Compile `assert_child.cpp` once in the private `ludus_assert_test_driver` object
target and use that object in both child executables. Their compiler flags were
identical before this change and remain identical afterward. The driver does
not link FoundationBase: the native child still links the production archive,
while the fake child still compiles its isolated runtime and fake platform
backend. Both executables retain their own state, native termination, and
complete test coverage. No runtime, public API, or installed target changes.

Clean before/after runs used the pinned Clang 18 toolchain, time traces on,
ccache off, and a fixed six-job limit on the same development host:

```bash
CMAKE_BUILD_PARALLEL_LEVEL=6 ./scripts/profile-build linux-clang-development
./scripts/check-build-budget
```

| Metric | Before | After |
| --- | ---: | ---: |
| Summed frontend | 52.1 s | 49.4 s |
| Summed backend | 21.4 s | 22.1 s |
| Build wall (script) | 14.67 s | 14.06 s |
| Compile database entries | 30 | 29 |
| ClangBuildAnalyzer compilation events, including scans | 56 | 54 |

This is one controlled pair, not a statistically established speedup. Removing
the duplicate compiler invocation is deterministic; the timing deltas include
host noise. An initial run without the six-job limit measured 133.0 s frontend
and is not comparable to this pair. Neither local timing is used to calibrate
the CI budget. Raw local evidence is retained under the ignored
`out/assert-budget-review/` directory.

### Budget decision

Raise only `total_frontend_seconds` from **65 to 100**, approximately 35%
headroom over the supplied **73.8 s CI measurement**. This accepts the expanded
test graph while retaining the original policy of headroom for runner noise.
It conservatively uses the observed CI run before the driver optimization;
post-optimization CI timing is not yet measured. All per-header limits and all
tests remain enabled. Local `check-build-budget` passed with 49.4 s frontend.

Existing Logging header costs still warrant their separately reviewed
optimization work; this adjustment does not declare them cheap or raise their
limits. Recalibrate again after integrating the Logging redesign on `main`,
using the actual combined CI graph rather than adding speculative allowances.

### Follow-up validation

`./scripts/test` passed 12/12 for each of `linux-clang-debug`,
`linux-clang-development`, and `linux-clang-profile`, and 11/11 for
`linux-clang-asan-ubsan`. The existing dedicated build trees were validated with
the following commands (the ordinary Release preset disables tests):

```bash
out/host-tools/venv/bin/cmake --build out/build/linux-clang-release-assert-tests -j2
out/host-tools/venv/bin/ctest --test-dir out/build/linux-clang-release-assert-tests --output-on-failure -j2
out/host-tools/venv/bin/cmake --build out/build/linux-clang-assert-tsan -j2
out/host-tools/venv/bin/ctest --test-dir out/build/linux-clang-assert-tsan --output-on-failure -j2
./scripts/check linux-clang-development --all
./scripts/install-sdk linux-clang-development
git diff --check
```

Release passed 12/12, TSan 11/11, format/tidy passed, and the installed SDK
consumer built and ran. Socket/death and sanitizer tests required execution
outside the sandbox's socket/process-inspection restrictions. Replaying the
supplied CI report through the budget checker reproduced the 65 s failure and
passed at 100 s; probes at 100.1 s total and 3201 ms for `log.hpp` still failed.

## Audio content CI calibration

PR #62 adds native streaming/device sources, Content/AudioContent, a pinned C
parser, the packager/sample and three native Catch2 translation units. Its full
uncached CI profile uses the same Ubuntu 24.04 runner class, pinned Clang 18,
Development preset and two-job concurrency as main:

| Measurement | Main `41dd763` | Audio `2da869a` |
| --- | ---: | ---: |
| ClangBuildAnalyzer compilation events | 304 | 336 |
| Summed frontend parsing | 139.6 s | 174.3 s |
| `audio_types.h` average/include | 172 ms | 167 ms |
| `audio_system.h` average/include | 134 ms | 150 ms |
| `core.h` average/include | 29 ms | 42 ms |

Sources: [main CI profile](https://github.com/The-Ludus-Project/Ludus/actions/runs/37128917880/job/111221053785)
and [audio CI profile](https://github.com/The-Ludus-Project/Ludus/actions/runs/37129607360/job/111222059306).
These run on separate hosts; their difference is not an exact attribution of
feature cost. The audio run fails only the aggregate 170 s limit. All ranked
project headers pass, including `audio_source.h` at 123 ms. New content public
headers contain no `<format>`, `<filesystem>` or `<chrono>` includes.

Recalibrate `total_frontend_seconds` to **210 s**, about 20% headroom over the
measured expanded graph, consistent with the preceding main headroom (170 s over
139.6 s). Per-header budgets, overrides, profile inputs and tests are unchanged.
No header is ignored. This replaces the earlier heavily contended local 792.6 s
measurement as the CI calibration input; it does not claim that local measurement
passes. The next CI profile must verify the updated limit; tighten it when repeated
measurements justify doing so.

## Network core CI calibration

The full PR #82 graph at `79ab51b` measured **214.6 s summed frontend parsing**
over **419 compilation events**, with **59.4 s backend time** and **162.21 s build
wall time** on ubuntu-24.04, pinned Clang 18, and two uncached jobs. The evidence
is [CI run 37256537947, Build-time budget job](https://github.com/The-Ludus-Project/Ludus/actions/runs/37256537947/job/111594875472);
its `native-build-profile` artifact retains the raw traces and report. Only the
aggregate 210 s limit failed. The preceding audio-content calibration measured
174.3 s over 336 events. These are separate runners and different complete build
graphs: the difference is not an isolated measurement of NetworkCore overhead.

The reported project headers pass unchanged 2000 ms limits: `audio_types.h`
averaged 165 ms, `core.h` 43 ms, and `audio_system.h` 148 ms. NetworkCore adds four
implementation translation units and one Catch2 test translation unit; its
public headers include fixed-width types and `<span>` only. No heavy standard
header was added to them. Other modules merged on main since the previous
calibration also contribute to the measured aggregate.

Recalibrate only `total_frontend_seconds` from **210 to 260 s**, approximately
21% headroom over this measured complete graph, consistent with the previous
~20% calibration. All per-header limits, overrides, profile inputs and tests
remain unchanged. This accepts the measured graph growth; it makes no runtime
optimization or isolated networking compile-time claim. Verify the new limit
in CI and tighten it when repeated comparable profiles support doing so.

## Native Content CI calibration

PR #114's full native profile at `cb697f2` measured **267.2 s summed frontend
parsing over 502 compilation events**, with **76.7 s backend time**, on the
ubuntu-24.04 runner, pinned Clang 18, Development preset, two uncached jobs.
The [CI profile job](https://github.com/The-Ludus-Project/Ludus/actions/runs/37555651368/job/112581449768)
failed only the aggregate 260 s limit; its `native-build-profile` artifact retains
the complete report. The preceding
[F3 profile](https://github.com/The-Ludus-Project/Ludus/actions/runs/37548495788/job/112558762024)
measured **251.4 s over 493 events**, leaving just **3.4% headroom** at 260 s.
These are separate hosts and different complete graphs; the difference does not
isolate Content overhead or establish an optimization measurement.

The 260 s calibration originated with NetworkCore's 214.6 s over 419 events.
Subsequent modules, pack tests and native persistence coverage increased the
graph. Content adds a private failure-injection backend and two native test
translation units; its public header changes are documentation, with no new
heavy includes or templates. The reported project headers pass their unchanged
limits: `core.h` averages 43 ms and `audio_types.h` 171 ms, both below 2000 ms.
The ranked costs remain existing Catch2/filesystem and standard formatting
consumers. No production header overage was reported.

Recalibrate only `total_frontend_seconds` from **260 to 320 s**, approximately
20% headroom over the observed 267.2 s graph, consistent with the preceding
aggregate policy. All per-header limits, overrides, exclusions, profile inputs
and tests remain unchanged. No performance improvement is claimed. Verify the
next full CI profile and tighten the aggregate when repeated comparable runs
support doing so.

## PR 204 standard-format dependency cleanup

The [PR 204 build-budget job](https://github.com/The-Ludus-Project/Ludus/actions/runs/38178357133/job/114585651898)
measured 343.9 seconds of summed frontend parsing against the unchanged 320-second
budget, with 1376 compile edges and two uncached jobs. Ranked project headers
passed; the aggregate failed. Catch2's unconditional chrono stringification
spread `<chrono>` to 135 compilations (55.148 seconds of inclusive header time).
The report also measured 137 instantiations each of narrow and wide `vformat_to`,
20.181 and 15.344 seconds respectively. These timings overlap; do not add them
or treat them as predicted savings.

Repository-owned `<format>` includes and standard-format calls are banned,
including test fixtures and the independent WebGPU probe. File-sink filenames
and logging fixture names use the existing bounded Ludus formatter. Production
sorting uses Ludus heap/merge sorting; min/max/clamp use scalar helpers or
local comparisons, and fixed buffer copy/fill uses loops.

The pinned local Catch2 3.4.0 recipe rebuilds the test runtime with its adapted
headers, preserving licenses and the assertion model. Its stream insertions
compile behind a test-only boundary and chrono printers are available through
explicit `<catch2/catch_chrono_stringmaker.hpp>`. Benchmark forward declarations
retain their exact `duration<double, nano>` ABI through the qualified vendors'
small duration headers, avoiding calendar/locale/format definitions. Unknown
standard libraries retain the upstream chrono include until separately qualified.
No declarations are invented inside namespace `std`. No binary built against the old
inline definitions is mixed with the adapted test headers. The dependency's
private runtime includes remain in its normal dependency build; every engine
and test translation unit remains in the existing profile, without a PCH or
a budget increase. `./init.sh --with-tests` prepares this recipe; stale upstream
packages fail configuration with a repair instruction. Value-printing and
intentional-failure regressions verify diagnostics and failing exit codes.

`python tools/check_build_time_includes.py` runs before CI compilation and rejects
standard formatting in owned sources and heavy headers in public module headers.
Concepts, traits and explicitly approved low-level operations remain permitted.

## Native C++ module scanning

Ludus uses no C++20 named modules or `import std`. With Ninja and C++23, CMake
3.28+ (policy CMP0155) still runs `clang-scan-deps` on every C++ source and
collates each target's results before that target compiles. The root
`CMakeLists.txt` disabled scanning only for Emscripten; it now sets
`CMAKE_CXX_SCAN_FOR_MODULES OFF` for every engine target.

Measured with `./scripts/profile-build macos-clang-development` (RelWithDebInfo,
tests enabled, ccache off) on a 10-core Apple M5 host. Four before/after pairs
ran interleaved, three with `--no-time-trace` and one with time traces. After
each clean build, `core.h` was touched and the tree rebuilt to measure the
inner loop.

| Measurement | Scanning on | Scanning off |
| --- | ---: | ---: |
| Clean-build `.ninja_log` entries | 1559 | 543 |
| Compiler invocations (`.o`) | 382 | 382 |
| Scan and collation CPU, clean build | 72–91 s | 0 |
| `.ninja_log` entries after touching `core.h` | 1046 | 349 |
| Scan and collation CPU after touching `core.h` | 51–76 s | 0 |
| Configure, median of five interleaved runs | 5.40 s | 5.15 s |

The removed entries are 375 `.ddi` scans, 375 `.modmap` files, and 133 each of
the per-target `CXX.dd` and `CXXModules.json` collation outputs, written by 375
scan and 133 collation commands. Their CPU time comes from the scanning-on runs'
own `.ninja_log`, counting each command once: one collation writes its target's
`CXX.dd`, `CXXModules.json` and all of its `.modmap` files. Scanning and
collation add 8–10% to compile CPU for a clean build and 6–10% after touching
`core.h`.

No wall-time improvement is claimed. The host was shared with other builds
(load average 9–28), and the same 382 compilations summed between 687 s and
1250 s across runs. Clean-build wall time was 72–108 s with scanning and
60–108 s without, and the paired differences changed sign. At full local
parallelism the removed work mostly overlaps compilation; it matters most with
few jobs, such as CI's `CMAKE_BUILD_PARALLEL_LEVEL=2`. Compare the CI job
summaries' build wall time before and after this change.

The ClangBuildAnalyzer aggregate gated by the build-time budget is essentially
unaffected: the time-trace pair recorded 718 and 715 compilation events. Their
frontend totals (423.5 s and 386.1 s) moved with their backend totals (276.5 s
and 249.0 s), which scanning does not touch, so the difference is host load.
No budget changes.

`compile_commands.json` entries no longer reference generated `.modmap`
response files. The installed SDK helpers turn scanning off for the game-project
targets they configure; see
[SDK game project module scanning](#sdk-game-project-module-scanning).
Re-enable scanning for any target that adopts named modules.

## SDK game project module scanning

The installed SDK helpers apply the same setting to the game-project targets
they configure. `CXX_SCAN_FOR_MODULES` is `OFF` on `ludus_add_game`'s module,
host and shipping targets, and on every target passed to
`ludus_apply_app_policy`, including `ludus_generate_reflection` libraries. The
helpers do not set `CMAKE_CXX_SCAN_FOR_MODULES`, so a project's other targets
keep CMake's default. A target that adopts named modules can set the property
back `ON` after the helper call.

Measured on a 10-core Apple M5 host with CMake 3.29.6 and the pinned Clang 18.
`./scripts/install-sdk macos-clang-development` supplied a RelWithDebInfo SDK.
A copy of that prefix, differing only in `LudusConfig.cmake` and
`LudusGame.cmake`, provided the scanning-on side. Each consumer was configured
and built clean against each prefix, alternating order: 18 interleaved pairs
at Ninja's default parallelism (load average 7–15).

| Consumer | Targets the helpers configure | Ninja steps (on → off) | `.ninja_log` entries (on → off) | Scan and collation CPU removed | Compile CPU |
| --- | --- | ---: | ---: | ---: | ---: |
| `examples/cornell-box` | 2 of 2 | 13 → 7 | 30 → 18 | 0.46 s (0.40–1.43 s) | 0.9 s |
| `examples/live-edit-game` | 4 of 4 | 21 → 11 | 40 → 20 | 1.0 s (0.8–7.5 s) | 2.5 s |
| `tests/sdk_consumer` | 2 of 3 | 57 → 52 | 94 → 84 | 0.89 s (0.58–1.09 s) | 17–19 s |

CPU values are medians with their ranges; the `tests/sdk_consumer` split by
target comes from the last 12 pairs. Removed CPU sums the `.ddi`, `.modmap`,
`CXX.dd` and `CXXModules.json` entries of the scanning-on runs, counting each
command once: one collation writes `CXX.dd`, `CXXModules.json` and all of its
target's `.modmap` files. For the two examples, scanning added a median 49% and
38% to compile CPU. The same compilations ran either way, and configure time
was unchanged. `tests/sdk_consumer` keeps scanning its own `ludus_sdk_consumer`
target (22 sources, 5.8 s median), as intended. Its compile commands still
reference `.modmap` files; those of the helper-configured targets no longer do.

At default parallelism, Ninja's build span (first start to last end in
`.ninja_log`) was shorter without scanning in 16 of 18 `cornell-box` pairs
(median 1.24 → 1.02 s, median paired difference 0.15 s) and 17 of 18
`live-edit-game` pairs (1.10 → 0.85 s, 0.29 s). In these small builds, each
target's scan and collation sit on the critical path before its first
compilation. `tests/sdk_consumer` showed no consistent difference (10 of 18
pairs, differences from −1.7 to +0.9 s). Six pairs at `--parallel 2` pointed
the same way for the two examples but are too few to quantify.

## Fresh worktree setup cost

A new agent worktree runs
`./init.sh --cli macos-clang-development --preset-only --locked --with-tests`
and then a clean `./scripts/build macos-clang-development`. These runs measure
that cost and the opt-in caches in
[Sharing caches between checkouts](building.md#sharing-caches-between-checkouts).
Every run used commit `8f570f74` on a 10-core Apple M5 with 16 GB. Other
agents' builds shared the host, with load averages from 24 to 109. Wall time
moved with that load, so the tables report ranges and CPU time (user plus
system, from `/usr/bin/time`), which moved much less. Runs during which the
host slept are excluded from wall-time figures.

| Fresh checkout | Checkout-local caches (two runs) | Shared caches, first checkout | Shared caches, later checkouts |
| --- | ---: | ---: | ---: |
| `./init.sh` wall time | 219–814 s | 288 s | 11–16 s |
| `./init.sh` CPU time | 409–491 s | 437 s | 6–14 s |
| Clean build wall time | 342–512 s | 122–1531 s | 79–195 s |
| Clean build CPU time | 517–557 s | 472–964 s | 97–110 s |
| ccache hits | not used | 1 of 392 | 380 of 392 |
| `out/conan/` size | 667–672 MB | 144 KB | 144 KB |
| Shared Conan home | none | 673 MB | 673 MB, shared |
| Shared ccache | none | 52 MB | 61 MB after four builds, shared |

`out/host-tools/` stayed 155 MB in every checkout. The later-checkout init
times ran at load 24–31; the first-checkout init ran at load 37–58.

### Where a checkout-local init spends its time

A checkout-local init builds five Conan packages from source: Catch2,
FreeType, HarfBuzz, and Conan's own `ninja` and `pkgconf` build tools. In the
slower baseline run, at load 60–76, the Conan install took 728 of 814 s.
HarfBuzz alone took 417 s, including 55 s to unpack its source archive. The
virtual environment and pinned Python packages took 51 s at that load and
4 s at load 24. The macOS SDK compile/link probe runs twice per init, at
0.8–7 s each. Downloads were at most about 15 s. A checkout-local cache also
keeps 517 MB of build folders in its 672 MB.

### Conan sharing options

| Option | Saved per later checkout | Per-checkout cost | Notes |
| --- | --- | --- | --- |
| `core.download:download_cache` | Downloads only, about 15 s | Still builds all five packages, 672 MB | Conan locks it for concurrent use |
| `conan cache save` / `restore` | The package builds | 60 s wall and 279 MB to restore (4 s CPU) | Saving took 300 s wall at load 70 and must be redone whenever recipes or `conan.lock` change |
| Shared `LUDUS_CONAN_HOME` | The package builds and downloads | 144 KB | One 673 MB home; `./init.sh` serializes its Conan steps |

The shared home costs nothing extra for the first checkout: its 437 s of
init CPU falls within the checkout-local range. A second init started while
the first held the lock printed the waiting message after 29 s. It waited
226 s, then installed every package from the cache in 9 s, with 12 s of CPU
for the whole init.

### Compiler cache across checkouts

A clean development build compiles 392 cacheable C and C++ translation
units. The four Objective-C++ files have no ccache launcher. Three later
checkouts at different paths reused 380 of them:
`/private/tmp/ludus-wtcost-d`, `/private/tmp/ludus-wtcost/nested/e` (two
directories deeper), and a worktree under `/Users/.../.claude/worktrees/` (no
symlink in its path). Their clean builds used 97–110 s of CPU, about a fifth of
a checkout-local build, and took 79–195 s at load 70–109. The 12 misses are
test and sample translation units whose definitions name an absolute checkout
path: scene goldens, text and game-host fixtures, and the smoke app's font.
Six more such units hit in ccache's preprocessed mode, because they do not use
the definition.

Filling an empty cache added no measurable CPU at moderate load: 472 s at load
24–40. A second fill at a sustained load near 100 used 964 s, 255 s of it
system time. At that load even CPU time inflates, so that run says more about
the host than about ccache.

Before the launcher handled symlinked checkout paths, each checkout under
`/private/tmp` hit only 1 of 392. CMake spelled its paths `/tmp/...`, and
ccache relativized them from the resolved working directory, through `/` and
the checkout's own directory name. Listing both spellings in `base_dir` and
setting `PWD` to the build directory fixed this. The `/Users` checkout above
then reused objects compiled under `/private/tmp`.

Sharing leaves incremental builds unchanged. After a warm build, touching
`core.h` dirtied 381 Ninja steps, as in a checkout-local tree, even though the
depfiles now list relative paths. A reused object records its source as
`../../../modules/...` and names the build directory of the checkout that
compiled it first as `DW_AT_comp_dir`. A checkout-local object records the
absolute source path and its own build directory.
