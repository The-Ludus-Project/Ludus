# FoundationTime validation

October 4, 2026. Scope: shared monotonic clock, checked arithmetic and owner-local
stopwatch/deadline/frame helpers. Timing samples describe this shared Linux host;
they are not a portable latency or physical-resolution guarantee.

## Research

The initial architecture preceded the local Gems chapter review. The final
[architecture](../architecture/high-resolution-time.md) records selection,
printed/PDF pages, adapted ideas and departures. All three selected chapters were
read; clock figures were visually checked. No historical source was copied.
Source acknowledgments appear at the affected implementation boundaries.

## Performance and dependencies

A Clang 18 C++23 `-O2 -fno-exceptions` probe compared the previous out-of-line
steady-clock implementation against the linked production FoundationTime archive.
Each row is nine trials of one million clock reads with an observable XOR result.
Values include loop/call overhead; builds were running concurrently on the shared
host. Sequential order and load make these unsuitable for a speedup claim.

| Read path | Median ns/read loop | Min | Max |
| --- | ---: | ---: | ---: |
| Previous steady_clock | 120.99 | 86.31 | 141.43 |
| FoundationTime | 105.89 | 81.19 | 112.95 |
| Previous steady_clock, repeat | 110.21 | 88.28 | 128.66 |
| FoundationTime, repeat | 126.71 | 98.63 | 160.75 |

The ordering of medians reverses on repetition. This establishes no reliable
performance improvement or regression. The backend remains the same OS-backed
steady clock; centralization removes duplicate implementations and makes timer
policy independently testable. Direct TSC is not justified by these data.

`nm -C -u libludus_foundation_time.a` shows only the module's own checked-time
functions and `std::chrono::_V2::steady_clock::now()` as unresolved dependencies.
No allocator, logger, mutex or registry dependency appears in the archive.
This is a dependency/code inspection, not a runtime allocation-interception test.

## Validation commands and results

Local integration validation used the primitive-types base `69ede23` plus this
implementation. The published branch incorporates newer upstream work; required
CI checks validate the exact PR commit, including static analysis and the build
budget. Native configuration uses pinned Clang 18, CMake/Ninja and warnings as
errors. Browser configuration uses pinned Emscripten 4.0.23. The selected CMake's
actual configure/build/test preset listings were checked.

| Validation | Local result |
| --- | --- |
| Development full build and `ctest --preset linux-clang-development --no-tests=error` | Passed: 48 registered tests, two display-dependent skips. |
| ASan/UBSan full build and `ctest --preset linux-clang-asan-ubsan --no-tests=error` | Passed: 42 registered tests, two display-dependent skips; no sanitizer findings. |
| `./scripts/install-sdk linux-clang-development` | Passed: installed artifact/dependency audit, standalone consumer build and execution, runtime policy match. |
| `./scripts/check --format` | Passed, including the foundational include boundary. |
| `./scripts/check linux-clang-development --all` | Passed: format/include boundary and all 178 compiled native project sources under Clang 18 static analysis. |
| Browser Development full build | Passed with warnings as errors. |
| Browser Development full CTest suite | All 16 passed, including FoundationTime headers and shared-clock/helper semantics. |

After integrating the platform/editor base `2f602ea`, the warning-clean native
Development build and all 49 registered tests also passed (two display-dependent
skips). The full browser Development format/static-analysis check passed after
correcting the probe formatting and expressing the checked conversion as a
positive range test. Required CI validates the final PR commit across the other
profiles and the build-time budget; no budget was raised for this module.

Native process/socket tests and LeakSanitizer require ordinary OS access.
The restricted sandbox denied socket operations and LeakSanitizer process
inspection; rerunning the full native suites and SDK workflow with that access
passed. Display-dependent Wayland tests remain skipped on this headless host.

## Frame integration

October 16, 2026. Scope: the [frame integration stage](../architecture/high-resolution-time.md#implementation-before-research)
for the world reference and smoke application loops. Each callback reads one
FoundationTime timestamp. The world reference `TickDriver` owns the 250 ms
FrameClock; the smoke application owns a 100 ms FrameClock. Neither driver
applies a second clamp. The float64 tick accumulator is unchanged; the rational
integer driver is the next stage.

Integration tests in `apps/world_demo/tests/simulation_tests.cpp` (`[frame]`)
drive synthetic timestamps at a 2^62 ns epoch without OS sleeps or a mock clock:

| Behavior | Evidence |
| --- | --- |
| Startup | First Advance is a zero-elapsed baseline and runs no tick. Uneven integer partitions of one second accept exactly 1,000,000,000 ns; equal readings are valid zero frames. |
| Accepted/discarded time, no duplicate clamp | A 2 s stall accepts exactly 250 ms and clamps exactly 1.75 s. Catch-up trimming adds less than the accepted 250 ms to the discard total. The next frame measures from the stalled sample. |
| Pause/resume | Paused frames accept nothing; resuming yields a baseline, so paused time is never caught up. Single step still runs exactly one tick. |
| Visibility | Hidden frames and an `Interrupt()` (the input reset a browser queues when a page is hidden while callbacks are suspended) exclude 89.99 s of hidden time with no catch-up tick. |
| Session replacement | Frames during a long load are excluded; activation resets the driver, and the replacement world starts from a baseline at tick zero. |
| Clock regression | A backwards sample adds no time and increments a counter. Recovery measures from the high-water mark. After a lifecycle reset, a regressed sample cannot become a baseline below it. |

For each monotonic sequence, accepted, clamped and lifecycle-excluded totals equal
the observed span exactly. Smoke tests cover its startup baseline, single 100 ms
clamp, hidden reset, return-to-visible baseline and regression.

| Validation (macOS arm64 host, pinned Clang 18, warnings as errors) | Result |
| --- | --- |
| `./scripts/build macos-clang-development` | Passed. |
| `./scripts/test macos-clang-development` | 82 of 85 passed. The Cocoa window suite skips every case without `LUDUS_TEST_COCOA=1` in a WindowServer session, so its runner reports failure. The header self-sufficiency gate exceeded its fixed 120 s timeout under a host load average above 100. Run directly, it passed all 153 public headers in 1 min 42 s. `ludus_target_detection` passed on rerun. |
| World reference with `-DLUDUS_BUILD_WORLD_DEMO=ON` | `ludus_world_demo_tests` (22 cases), allocation, shader layout, smoke simulation and FoundationTime tests passed. `--headless` completed 120 ticks; `--frames 3` presented three native frames through the migrated loop. |
| `python3 scripts/python/source_policy.py --base HEAD` | Passed. |
| `./scripts/check --format` | Passed, including the foundational include boundary and naming scopes. |
| clang-tidy 18 on the changed world reference and smoke sources | No findings. |
| `./scripts/build macos-clang-asan-ubsan` | Passed. |
| ASan/UBSan test execution | Not completed locally. On this macOS 26.6 host the Clang 18 ASan runtime deadlocks in its initializer (`AsanInitFromRtl` spin lock during dyld shared-cache iteration) before `main`, for every sanitized binary. |
| Documentation: `check-docs`, strict MkDocs build | Passed. `check-wiki --documentation-only` reported only the absent generated API reference, which requires the Doxygen bootstrap. |

The Linux reference profile, sanitizer execution, browser presets and build budget
are validated by the pull request's CI tiers.

## Integer simulation driver

October 16, 2026. Scope: the [integer simulation driver stage](../architecture/high-resolution-time.md#implementation-before-research).
The world reference `TickDriver` replaces its `float64` seconds debt (with its
`1e-12` epsilon and `std::fmod` trimming) by a checked `uint64` phase. Each frame
adds accepted nanoseconds times the tick rate; one tick consumes exactly 10^9
units. Whole ticks beyond the catch-up budget are counted as discarded ticks,
separately from the FrameClock clamp, which remains the only clamp. The accepted,
clamped, excluded and regression diagnostics are unchanged. A checked
`TickPolicy` validates the rate, catch-up budget and the
`MaximumFrameDelta * rate + 10^9 - 1` bound when configured.

Tests in `apps/world_demo/tests/simulation_tests.cpp` (`[frame]`) drive
synthetic timestamps at a 2^62 ns epoch:

| Behavior | Evidence |
| --- | --- |
| Partition independence | 1.234567891 s split into 1 ms, rounded 60 Hz and 144 Hz frames, 50 ms frames, alternating 1 ns/49 ms frames and two pseudo-random partitions of 1 ns to 50 ms: every run yields 74 ticks, a 74,073,460-unit remainder and no discarded tick. |
| Exact 60 Hz | One second in seven partitions (including 16,666,666 ns, 16,666,667 ns, 30 Hz and pseudo-random frames) yields exactly 60 ticks with a zero remainder; the previous test's 59-60 tolerance is now exactly 60. 25 ms yields one tick and alpha exactly 0.5. |
| Clamp and catch-up loss | A 2 s stall clamps exactly 1.75 s; its 15 ticks of debt run four and discard 11. 70 ms then carries 0.2 tick; 100 ms more runs four and discards two. Accepted phase equals run plus discarded ticks plus the remainder. A one-tick budget discards five of six ticks. The largest remainder (10^9 - 1 at 1 Hz) interpolates below one. |
| Configuration and overflow | A zero rate, a zero budget, a multiplication overflow and an addition-bound overflow (one rate unit above the limit) return `InvalidConfiguration` and preserve the previous policy. The exact limit is accepted and repeated clamped frames keep the phase exact without overflow. Configure drops the carried phase and frame baseline. |
| Failure and exhaustion | An invalid tick input faults the driver with the world still at tick zero. A world at the last representable tick completes it, the next tick returns `IdentityExhausted`, and the driver stops in Faulted with no further ticks or single step. |

| Validation (macOS arm64 host, pinned Clang 18, warnings as errors) | Result |
| --- | --- |
| Development full build with `-DLUDUS_BUILD_WORLD_DEMO=ON` | Passed, warning-clean. |
| `ctest --preset macos-clang-development --no-tests=error` | 100 of 102 passed. As in the frame integration run, the Cocoa window suite fails without `LUDUS_TEST_COCOA=1` in a WindowServer session, and the header self-sufficiency gate exceeded its fixed 120 s timeout at a host load average above 170; this change adds no public header. |
| World reference | `ludus_world_demo_tests` (27 cases, 11 `[frame]`) and allocation tests passed. `--headless` completed 120 ticks; `--frames 3` presented three native frames. |
| `python3 scripts/python/source_policy.py --base origin/develop` | Passed. |
| `./scripts/check --format` | Passed, including the include boundary, naming scopes and container policy. |
| clang-tidy 18 on the changed world reference sources | No findings. |
| ASan/UBSan execution | Not run locally: the Clang 18 ASan runtime hangs at startup on this macOS 26.6 host (see above). |
| Documentation: `check-docs`, strict MkDocs build, `test_docs.py`, `test_wiki.py` | Passed. `check-wiki --documentation-only` reported only the absent generated API reference, which requires the Doxygen bootstrap. |

The Linux reference profile, sanitizer execution, browser presets and build budget
are validated by the pull request's CI tiers.

## Native frame pacing

October 16, 2026. Scope: the pacing half of the
[pacing and inspection stage](../architecture/high-resolution-time.md#implementation-before-research):
`Ludus::PlatformPacing` (`FramePacer`, `GetSystemPacingClock()`) and the world
reference `LoopPacing`. Runtime timing telemetry and the editor Timing view are
separate later work.

### Policy tests

Unit tests use an injected `PacingClock`. Its wait advances a synthetic counter
at a 2^62 ns epoch, so no test sleeps and results do not depend on host load.

| Behavior | Evidence (`modules/platform/tests/frame_pacing_tests.cpp`) |
| --- | --- |
| Drift-free targets | 600 frames with a 2 ms oversleep on every wake keep every target on `epoch + k * 16,666,667` ns; each wake is exactly 2 ms late and lateness never accumulates. |
| Interruption and early wakes | Two `Interrupted` returns re-wait for the remaining interval (three waits, wake exactly on target). A `Completed` wake 50 µs early waits 50 µs more. Persistent early wakes stop after eight attempts (`WaitLimit`), keep the phase, and the next target, less than two intervals away, is honored. |
| Failed or unsupported waits | One wait, frame starts early (`WaitFailed`), phase kept, no spin. Browser builds always report `Unsupported`. |
| Lateness | A 2.5-interval frame skips one target and keeps the grid. Arriving exactly on a target, or exactly at the four-interval bound, keeps the phase. One nanosecond beyond the bound re-anchors at the frame start, and later frames wait full intervals. A zero bound re-anchors after any lateness. |
| Clock faults and limits | A reading below the previous one, before or during a wait, re-anchors without waiting. An unrepresentable next target drops the schedule. Zero and over-one-second intervals are rejected with the previous policy kept. |
| Lifecycle | After 30 s of suspension, `Reset()` resumes from a baseline. Without it, the bound re-anchors and about 1,800 missed targets never run back to back. A 400 ms session-replacement frame re-anchors; reconfiguring drops the old phase. |

World reference tests (`apps/world_demo/tests/frame_pacing_tests.cpp`) check the
loop policy against the real `TickDriver` and `Session`. Without a cap, presented
frames read no clock and never wait. Loading frames wait at 60 Hz, and presenting
again drops the idle schedule. A 30 Hz cap feeds exactly 30 x 33,333,333 ns to the
driver (59 ticks, none discarded). Pausing the simulation for 30 frames keeps
every frame on its pacing target, and resuming drops only the driver baseline.
Activation after a slow load resets to a baseline. The bounded `--pacing-report`
summary has exact nearest-rank percentiles and counts dropped records.

### Host measurements

These measurements used the original `nanosleep(2)` wait; the
[wake leeway trial](#wake-leeway-trial) below replaced it on macOS.

Method: `ludus_world_demo` Development build (pinned Clang 18, warnings as
errors) at base revision `8f570f74` plus this change, on an Apple M5 MacBook
(10 cores: 4 performance, 6 efficiency) running macOS 26.6. The built-in display's
refresh mode was not queried; uncapped presents returning every 8.14 ms are
consistent with 120 Hz. Each configuration ran three times in sequence, 600
frames each, through the measurement script below. Load averages were recorded
before and after each run. The one-minute load average was 50 to 42 for the
headless runs and 38 to 27 for the windowed runs: five to three times the core
count, from other worktrees' builds. No quieter host condition was available, so
the data shows behavior under heavy load only.

```bash
B=out/build/macos-clang-development/apps/world_demo/ludus_world_demo
for args in "--headless --frame-rate 30" "--headless --frame-rate 60" "--headless --frame-rate 120" \
            "" "--frame-rate 60" "--frame-rate 50"; do
  for rep in 1 2 3; do uptime; $B --frames 600 --pacing-report ${=args} 2>&1 | grep Pacing; done
done
```

The script is written for zsh (`${=args}` splits words). Wake lateness is the
final reading after the wait minus the absolute target, for `Waited` frames. The
frame-start interval is wake to wake. The present-return interval is the time
between readings taken when `EndFrame` returned `Ready`. That reading is CPU-side
and excludes compositor and scanout time; it is not a photon or presentation
timestamp. Values are in milliseconds and pool all three runs per configuration;
p99 and maximum are given as the range across runs.

| Configuration | Wake lateness p50 | p90 | p99 (range) | Frame start p50 / p90 | Present return p50 / p90 | Stalls |
| --- | ---: | ---: | ---: | ---: | ---: | --- |
| Headless, 30 Hz cap | 4.25-4.90 | 5.71-7.07 | 10.3-19.9 | 33.31-33.34 / 36.8-37.0 | n/a | None. 0-2 `Late` per run. |
| Headless, 60 Hz cap | 3.27-3.31 | 4.23-5.93 | 6.3-12.0 | 16.58-16.61 / 19.1-20.0 | n/a | None. 0-3 `Late` per run. |
| Headless, 120 Hz cap | 1.65-1.67 | 1.96-2.05 | 2.4-4.7 | 8.32-8.33 / 9.7-9.8 | n/a | None. |
| Window, uncapped (FIFO) | n/a | n/a | n/a | n/a | 8.14-8.15 / 9.26 | One 236 ms present return in run 1. |
| Window, 60 Hz cap | 0.78-1.01 | 1.01-1.02 | 1.04-1.27 | 16.665-16.667 / 16.9-17.3 | 16.66-16.67 / 16.96-17.20 | Run 2: one 418 ms frame, two `Rescheduled`. |
| Window, 50 Hz cap | 1.01 | 1.02 | 1.05-1.06 | 20.000 / 20.29-20.65 | 20.00 / 20.27-20.64 | Run 3: one 998 ms frame, two `Rescheduled`. |

What the data shows:

- **Absolute targets hold the requested rate.** Mean frame-start intervals were
  33.34, 16.67-16.68, 8.33-8.34, 16.65 and 19.97 ms for the 30, 60, 120, 60
  (windowed) and 50 Hz caps. Apart from the stalled runs, each mean is within
  0.03 ms of its cap over 600 frames: late wakes did not accumulate.
- **A wake is late by an offset that depends on process state, not by ~0.**
  The windowless headless process woke later as the interval grew: 1.65 ms at
  120 Hz, 3.3 ms at 60 Hz and 4.3-4.9 ms at 30 Hz (median lateness).
  The windowed application woke a nearly constant 1.01 ms late (p90 1.02 ms),
  under similar load. The interval-proportional and constant offsets suggest
  operating-system timer leeway (coalescing) rather than CPU queueing alone. The
  [wake leeway trial](#wake-leeway-trial) later confirmed leeway as the main cause.
- **A constant offset shifts phase; it does not add jitter.** The windowed 60 Hz
  cap kept frame-start and present-return p90 within 0.3 to 0.6 ms of 16.667 ms.
- **The lateness policy prevented bursts.** The 418 ms and 998 ms windowed stalls
  produced two `Rescheduled` outcomes each and no back-to-back catch-up frames.
  The 998 ms stall matches the Metal layer's one-second `nextDrawable` timeout,
  but its cause was not traced. Skipped-target `Late` outcomes in the headless
  runs show the in-bound case.
- **On this display the 50 Hz cap was not quantized at the CPU side.** EndFrame
  returns followed the cap at 20.0 ms rather than alternating 16.7 and 33.3 ms.
  This is consistent with a variable-refresh display but does not show scanout.
  A fixed-refresh display is not covered.

What it does not show: wake-up behavior on an idle host, on Linux, on battery or
power-saving modes, or with longer runs; actual presentation (photon) times;
latency from input to display; the cause of either offset. Three sequential runs
per configuration on a shared, loaded host only supports the coarse conclusions
above. HR-02's methodology (randomized order, more executions, confidence
intervals) applies before using these numbers for any comparison.

No spin is implemented. The dominant lateness here is a systematic offset that
shifts phase evenly. Removing about 1 ms per frame by spinning would keep a core
busy for roughly 6 percent of each 60 Hz frame and does not change interval
evenness. That trade is not justified by this data. The
[wake leeway trial](#wake-leeway-trial) removed the offset without a spin.

### Validation

| Validation (macOS arm64 host, pinned Clang 18, warnings as errors) | Result |
| --- | --- |
| Development full build with `-DLUDUS_BUILD_WORLD_DEMO=ON` and `ctest --preset macos-clang-development --no-tests=error` | Build passed, warning-clean. 105 of 106 tests passed, including the header self-sufficiency gate with the new public header. The Cocoa window suite fails without `LUDUS_TEST_COCOA=1` in a WindowServer session, as in earlier stages. |
| `ludus_platform_pacing_tests` | 12 cases (3,778 assertions) passed. |
| `ludus_world_demo_pacing_tests`, `ludus_world_demo_tests` | 6 and 27 cases passed. |
| World reference | `--headless` hash `7529209811830740537` is unchanged with `--frame-rate 60`; windowed `--frames 600` runs above presented every frame. |
| `python3 scripts/python/source_policy.py --base origin/develop` | Passed. |
| `./scripts/check --format` | Passed, including the include boundary, naming scopes (the new Platform files are adopted) and container policy. |
| clang-tidy 18 on the new and changed sources | No findings. |
| Documentation: `check-docs`, strict MkDocs build, `test_docs.py`, `test_wiki.py` | Passed. |
| `scripts/build-api --bootstrap` | 166 headers; no new documentation gaps (the new public header is fully documented). |
| ASan/UBSan execution | Not run locally: the Clang 18 ASan runtime hangs at startup on this macOS 26.6 host (see above). |

The Linux reference profile, sanitizer execution, browser presets, installed SDK
and build budget are validated by the pull request's CI tiers.

### Wake leeway trial

October 17, 2026. Scope: whether thread QoS or an absolute operating-system wait
reduces the macOS wake lateness measured above, and why it occurs. The trial
record and decision are in the
[research backlog](../architecture/high-resolution-time.md#trial-native-pacing-wake-leeway-october-17-2026).

**Clock domain.** A probe linked against the system `libc++.1.dylib` (also used
by the world demo) read `steady_clock`, `clock_gettime_nsec_np`, Mach absolute
time and Mach continuous time back to back. `steady_clock` equaled
`CLOCK_MONOTONIC_RAW` exactly and Mach continuous time within 41-250 ns. Mach
absolute time, the domain of `mach_wait_until`, was 105,294 s behind: it excludes
the host's accumulated system sleep. An absolute `mach_wait_until` target computed
from FoundationTime would therefore be wrong by hours, so it was not tested. macOS
has no `clock_nanosleep`. The domain-matched absolute wait is a `kevent64(2)`
`EVFILT_TIMER` with `NOTE_MACHTIME | NOTE_ABSOLUTE | NOTE_MACH_CONTINUOUS_TIME`.

**Mechanism.** XNU (`xnu-12377.121.6`) delays an ordinary user timer by up to
`min(interval >> shift, max)` to coalesce wake-ups. On arm64 macOS, timeshare
threads get interval / 8 up to 1 ms. Latency-QoS tiers get up to 5, 20 or 75 ms;
the first tier is interval / 4 up to 5 ms. `NOTE_CRITICAL` selects the
user-critical tier, which has zero leeway. The earlier 1.01 ms windowed offset
equals the timeshare cap at 50 and 60 Hz. The headless medians (1.65, 3.3 and
4.3-4.9 ms at 120, 60 and 30 Hz) sit below the first latency tier's 2.1, 4.2 and
5 ms bounds. The process's effective tier was not read, so this attribution is
consistent with the data rather than traced.

**Method.** One Development binary at `16346bce` plus an uncommitted,
experiment-only environment switch selected each arm, so arms shared one build:

| Arm | Wait | Leeway |
| --- | --- | --- |
| `baseline` | `nanosleep(2)` for the remaining interval | Default |
| `qos-ui` | As baseline, after `pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE)` | Default |
| `kevent-abs` | Absolute Mach-continuous kevent timer at the target | Default |
| `kevent-abs-critical` | As `kevent-abs` with `NOTE_CRITICAL` | Zero |
| `kevent-rel` | Relative `NOTE_NSECONDS` kevent timer for the remaining interval | Default |
| `kevent-rel-critical` | As `kevent-rel` with `NOTE_CRITICAL` (the adopted wait) | Zero |

Each run was `ludus_world_demo --frames 600 --frame-rate {60,50} --pacing-report`,
with and without `--headless`. Following HR-02 and HR-06, every
(arm, mode, rate, repetition) execution was shuffled into one random order per
block, and each execution is one sample: frames within a run are correlated. The
one-minute load average was recorded before and after every run, and child CPU
time and context switches in block 2. Intervals are 95% Student-t intervals of
the mean of per-run statistics. Ratios to baseline use 10,000 percentile bootstrap
resamples of runs. Block 1 ran 4 arms x 4 configurations x 10 repetitions (seed
20261017). Block 2 ran 4 arms x 4 configurations x 8 repetitions (seed 20261018).
Windowed runs that presented before reaching 500 `Waited` frames were excluded:
startup frames are presentation-paced. Nine of 160 runs in block 1 and six of 128
in block 2 were excluded this way.

Host: Apple M5 (10 cores), macOS 26.6, shared with other worktrees' builds. The
one-minute load average was 54-128 (median 93) in block 1 and 10-157 (median 112)
in block 2. The main thread already ran at `QOS_CLASS_USER_INTERACTIVE` (0x21) when
launched from the shell, in every arm.

Block 1 (milliseconds; mean of per-run statistics with 95% interval):

| Configuration | Arm | Runs | Wake p50 | Wake p90 | p50 / baseline | Frame start p90 |
| --- | --- | ---: | ---: | ---: | ---: | ---: |
| Headless 60 Hz | baseline | 10 | 4.31 [3.47, 5.15] | 8.71 [6.77, 10.64] | 1.00 | 22.11 |
| | qos-ui | 10 | 4.53 [3.65, 5.41] | 10.02 [8.03, 12.00] | 1.05 [0.84, 1.32] | 23.00 |
| | kevent-abs | 10 | 4.33 [3.26, 5.41] | 9.09 [6.12, 12.06] | 1.01 [0.78, 1.30] | 22.07 |
| | kevent-abs-critical | 10 | 3.07 [2.11, 4.03] | 9.01 [7.63, 10.39] | 0.71 [0.52, 0.95] | 22.36 |
| Headless 50 Hz | baseline | 10 | 4.97 [4.42, 5.51] | 10.19 [9.01, 11.38] | 1.00 | 26.24 |
| | qos-ui | 10 | 5.03 [4.29, 5.77] | 10.26 [7.86, 12.66] | 1.01 [0.87, 1.17] | 26.44 |
| | kevent-abs | 10 | 4.75 [3.85, 5.65] | 9.62 [7.27, 11.97] | 0.96 [0.79, 1.13] | 25.98 |
| | kevent-abs-critical | 10 | 2.08 [1.17, 2.99] | 7.27 [5.37, 9.17] | 0.42 [0.27, 0.58] | 24.44 |
| Window 60 Hz | baseline | 10 | 1.00 [0.97, 1.02] | 1.03 [1.03, 1.03] | 1.00 | 17.30 |
| | qos-ui | 7 | 0.99 [0.95, 1.03] | 1.03 [1.02, 1.04] | 0.99 [0.95, 1.03] | 17.41 |
| | kevent-abs | 5 | 0.96 [0.85, 1.08] | 1.04 [1.04, 1.05] | 0.96 [0.88, 1.03] | 17.33 |
| | kevent-abs-critical | 9 | 0.03 [0.03, 0.03] | 0.05 [0.05, 0.05] | 0.03 [0.03, 0.03] | 16.69 |
| Window 50 Hz | baseline | 10 | 1.20 [0.92, 1.48] | 1.23 [0.93, 1.53] | 1.00 | 20.85 |
| | qos-ui | 10 | 1.22 [0.91, 1.52] | 1.23 [0.93, 1.53] | 1.01 [0.77, 1.30] | 20.77 |
| | kevent-abs | 10 | 1.12 [0.91, 1.34] | 1.24 [0.94, 1.54] | 0.94 [0.74, 1.20] | 20.86 |
| | kevent-abs-critical | 10 | 0.03 [0.03, 0.03] | 0.05 [0.05, 0.05] | 0.03 [0.02, 0.03] | 20.02 |

Block 2 (same units; CPU is the child process's user plus system time per run):

| Configuration | Arm | Runs | Wake p50 | Wake p90 | p50 / baseline | Frame start p90 | CPU s |
| --- | --- | ---: | ---: | ---: | ---: | ---: | ---: |
| Headless 60 Hz | baseline | 8 | 6.42 [5.57, 7.27] | 13.17 [11.14, 15.20] | 1.00 | 24.98 | 0.04 |
| | kevent-rel | 8 | 6.77 [6.14, 7.39] | 14.79 [13.57, 16.01] | 1.05 [0.94, 1.22] | 26.10 | 0.05 |
| | kevent-rel-critical | 8 | 2.61 [0.89, 4.34] | 8.46 [5.17, 11.75] | 0.41 [0.21, 0.63] | 22.09 | 0.04 |
| | kevent-abs-critical | 8 | 3.46 [1.59, 5.32] | 9.43 [5.84, 13.03] | 0.54 [0.30, 0.77] | 22.59 | 0.04 |
| Headless 50 Hz | baseline | 8 | 6.50 [5.59, 7.40] | 12.01 [10.12, 13.90] | 1.00 | 28.14 | 0.04 |
| | kevent-rel | 8 | 6.43 [5.37, 7.49] | 12.76 [10.62, 14.91] | 0.99 [0.83, 1.16] | 27.81 | 0.05 |
| | kevent-rel-critical | 8 | 3.75 [3.03, 4.48] | 10.91 [9.75, 12.07] | 0.58 [0.48, 0.69] | 27.04 | 0.04 |
| | kevent-abs-critical | 8 | 3.43 [1.92, 4.93] | 9.68 [6.42, 12.93] | 0.53 [0.33, 0.71] | 26.39 | 0.04 |
| Window 60 Hz | baseline | 5 | 1.40 [0.75, 2.05] | 1.43 [0.75, 2.10] | 1.00 | 17.76 | 0.85 [0.60, 1.09] |
| | kevent-rel | 8 | 0.72 [0.21, 1.24] | 0.76 [0.22, 1.29] | 0.52 [0.24, 0.91] | 17.06 | 0.97 [0.81, 1.12] |
| | kevent-rel-critical | 7 | 0.03 [0.03, 0.04] | 0.06 [0.05, 0.06] | 0.02 [0.02, 0.03] | 16.69 | 0.89 [0.73, 1.05] |
| | kevent-abs-critical | 7 | 0.03 [0.03, 0.03] | 0.05 [0.04, 0.05] | 0.02 [0.02, 0.03] | 16.69 | 0.97 [0.75, 1.18] |
| Window 50 Hz | baseline | 8 | 1.46 [1.05, 1.87] | 1.53 [1.08, 1.97] | 1.00 | 21.19 | 0.87 [0.70, 1.03] |
| | kevent-rel | 8 | 1.13 [0.77, 1.49] | 1.40 [0.99, 1.81] | 0.77 [0.56, 1.09] | 20.89 | 1.00 [0.80, 1.19] |
| | kevent-rel-critical | 7 | 0.03 [0.03, 0.04] | 0.05 [0.04, 0.05] | 0.02 [0.02, 0.03] | 20.02 | 0.98 [0.80, 1.17] |
| | kevent-abs-critical | 8 | 0.03 [0.03, 0.03] | 0.05 [0.04, 0.05] | 0.02 [0.02, 0.03] | 20.02 | 0.94 [0.71, 1.16] |

What the data shows:

- **Leeway is the windowed offset.** Zero leeway cut windowed median lateness
  from 1.0-1.5 ms to 0.03 ms (ratio 0.02-0.03) in both blocks, with p90 at
  0.05-0.06 ms and run-median p99 at 0.12-0.15 ms. Frame-start p90 fell to within
  0.03 ms of each cap.
- **Neither QoS nor an absolute wait helps by itself.** `qos-ui` changed nothing
  measurable (ratios 0.99-1.05), as expected for a thread that already ran at that
  class. `kevent-abs` and `kevent-rel` with default leeway matched the baseline
  in every headless configuration and at windowed 50 Hz. Absolute and relative
  critical timers were indistinguishable, so the interface can stay relative.
  `kevent-rel` at windowed 60 Hz (0.52 [0.24, 0.91]) is the one interval that
  excludes 1, from five baseline runs with a wide interval; it was not
  investigated.
- **Headless lateness under this load is mostly CPU scheduling.** Zero leeway
  lowered headless medians by 29-59%, but medians stayed 2-4 ms and p90 7-11 ms
  at median load averages of 9 to 11 times the core count. A later confirmation run at
  load 44-58 measured headless medians of 30-36 µs with the adopted wait, with a
  p90 of 0.6-1.8 ms.
- **No measurable CPU cost.** Per-run CPU time and involuntary context switches
  overlapped across arms. Power and energy were not measured
  (`powermetrics` needs administrator rights); zero leeway forgoes batching one
  wake per frame with other timers.

What it does not show: behavior on an idle host, on battery or in Low Power
Mode, for a process launched with a lower QoS class (for example by a launcher
or a non-interactive service), on Linux, or for presentation (photon) times.
Block 2's load differed from block 1's, so compare arms within a block only.

**Decision.** macOS `GetSystemPacingClock()` now waits on a relative one-shot
`kevent64(2)` timer with `NOTE_CRITICAL`, using one lazily opened kqueue per
waiting thread and falling back to `nanosleep(2)` when the queue cannot be opened.
The relative interface, absolute targets, recheck after every wake, no-spin and
browser contracts are unchanged; Linux keeps `nanosleep(2)`. `mach_wait_until` is
rejected because of its clock domain. QoS changes and Linux absolute waits are
deferred until measurements show lateness they remove. The new OS facility is
recorded in [ADR 0003](../decisions/0003-standard-library-usage-policy.md).

Confirmation with the adopted wait (one build, sequential, load 44-58):

| Run | Wake p50 | Wake p90 | Wake p99 | Frame start p50 / p90 |
| --- | ---: | ---: | ---: | ---: |
| Window 60 Hz, run 1 | 0.030 | 0.039 | 0.080 | 16.667 / 16.676 |
| Window 60 Hz, run 2 | 0.028 | 0.040 | 0.079 | 16.667 / 16.678 |
| Headless 60 Hz, run 1 | 0.036 | 0.570 | 3.08 | 16.667 / 17.07 |
| Headless 60 Hz, run 2 | 0.030 | 1.80 | 4.72 | 16.667 / 17.21 |

The headless replay hash is unchanged by the wait (`7399557020790493750` at 600
ticks, the same in every arm).

### Hidden-window power policy

October 17, 2026. Scope: the zero-leeway wait above gave up the kernel's timer
coalescing, so the world reference's minimized window woke 60 times a second for
frames nobody saw. Platform now reports `WindowVisibility`, and `LoopPacing` waits
at 10 Hz while the window is minimized or occluded (see
[native frame pacing](../architecture/high-resolution-time.md#native-frame-pacing)).

**Policy tests** (`apps/world_demo/tests/frame_pacing_tests.cpp`, synthetic clock):

| Behavior | Evidence |
| --- | --- |
| Hidden rate | One simulated second waits 59-60 times while visible and loading, and exactly 10 times while hidden, whether or not the frame presented. Hidden targets stay on `anchor + k * 100 ms`. |
| Lifecycle | Hiding and showing drop the schedule. A window shown 30 ms into a hidden interval starts its next frame at once instead of waiting out the old target, then follows the 60 Hz phase anchored at the reveal. Repeating the current visibility keeps the schedule. |
| Simulation | Hidden frames drop simulation time: five hidden frames add no tick, and the first visible frame is the driver's new baseline. |
| Caps | The hidden rate never raises a lower cap (5 Hz stays 200 ms). A cap chosen while hidden applies when the window is shown; an out-of-range cap leaves the hidden policy unchanged. |

The live Cocoa suite (`LUDUS_TEST_COCOA=1`) checks `Minimized` while miniaturized,
`Occluded` after `orderOut:` and after close, and `Visible` again after
`orderFront:`. Its windows float above normal windows: on the measured host, a
background test process could not activate itself, and a centered normal-level
window opened behind an opaque window and correctly reported `Occluded`.

**Wake measurement.** Method: `ludus_world_demo` Development builds of `origin/develop`
`ccc6bb537` plus this change ("after"), and the same tree with the world
reference's `application.cpp` from `ccc6bb537` ("before"), so only the loop policy
differs. A scratch dynamic library injected with `DYLD_INSERT_LIBRARIES` (not part
of the repository) floated the window, then measured three consecutive phases of
about 15 s each after a 3 s settle: visible, minimized with `miniaturize:`, and
restored with `deminiaturize:`. It read the process's kernel counters with
`proc_pid_rusage(RUSAGE_INFO_V4)`: `ri_interrupt_wkups` (timer and interrupt
wake-ups, all threads) and user plus system time, converted from Mach time units
(125/3 ns on this host). Runs alternated before, after, before, after. Host:
Apple M5 (10 cores), macOS 26.6, one-minute load average 65-79 from other
worktrees' builds.

| Phase | Before: wake-ups/s | After: wake-ups/s | Before: CPU ms/s | After: CPU ms/s |
| --- | ---: | ---: | ---: | ---: |
| Visible | 2.0, 23.5 | 20.8, 2.1 | 39, 46 | 44, 42 |
| Minimized | 61.2, 61.4 | 11.3, 11.8 | 5.4, 6.7 | 1.3, 1.3 |
| Restored | 3.0, 2.1 | 2.1, 2.1 | 40, 40 | 40, 40 |

What the data shows:

- **Minimized wake-ups fell from about 61 to about 11.5 per second**, matching the
  60 Hz idle schedule before and the 10 Hz hidden schedule after, plus about 1.5
  per second from other threads. CPU time while minimized fell from 5-7 ms to
  1.3 ms per second.
- **Visible and restored windows are unchanged.** Both arms are paced by FIFO
  presentation, with 2-3 timer wake-ups per second; one visible phase in each arm
  measured 21-24, so that outlier is not caused by the policy. Restoring returned
  to presentation pacing and kept presenting in every run.

What it does not show: power or energy (`powermetrics` needs administrator
rights), an idle host, battery or Low Power Mode, partial occlusion, Linux
(Wayland reports `Visible` and keeps the 60 Hz idle rate), or the latency from
`deminiaturize:` to the first new frame beyond the 100 ms hidden interval bound.
Two runs per arm on a loaded host support only the coarse rates above.

| Validation (macOS arm64 host, pinned Clang 23.1.3 after merging `develop`, warnings as errors) | Result |
| --- | --- |
| Development full build with `-DLUDUS_BUILD_WORLD_DEMO=ON` | Passed, warning-clean. |
| `ctest --preset macos-clang-development` with `LUDUS_TEST_COCOA=1` | 115 of 116 passed. `ludus_renderer_pixels` fails because Metal rejects the `renderer_output` fragment shader; it passed on the Clang 18 base with this same change, which touches no renderer, RHI or shader code. On the Clang 18 base, `ludus_dependency_diagnostics` once exceeded its 120 s timeout under load and passed when run directly. |
| `ludus_world_demo_pacing_tests`, `ludus_platform_cocoa_tests` | 9 cases (441 assertions) and 7 cases passed; the Cocoa suite also passed three consecutive runs on the Clang 18 base. |
| `source_policy.py`, `./scripts/check --format` | Passed. |
| clang-tidy on the changed native sources | No findings. The browser `window_web.cpp` change is compiled by the browser presets in CI. |
| Documentation: `check-docs`, strict MkDocs build, `test_docs.py`, `test_wiki.py`, `check-wiki --documentation-only` | Passed. |
| `scripts/build-api` | 185 headers; no new documentation gaps. |

## Runtime timing telemetry

October 17, 2026. Scope: the inspection half of the
[pacing and inspection stage](../architecture/high-resolution-time.md#implementation-before-research):
GameHost play-loop pacing, the private `TimingTelemetry` aggregate, the `timing`
record on `Status`, and the editor Timing panel. See the
[contract](../architecture/high-resolution-time.md#runtime-timing-telemetry).

### Tests

| Behavior | Evidence |
| --- | --- |
| Aggregation over synthetic time | `modules/runtime/game_host/tests/timing_tests.cpp` drives the host's `FramePacer` and 250 ms `FrameClock` with an injected clock whose wait oversleeps 1 ms. Over 200 frames: one baseline, 199 `Waited`, a 120-frame window whose intervals are exactly 16,666,667 ns and whose lateness is exactly 1 ms, and accepted time of 199 intervals plus the first oversleep. |
| Stalls, regressions and resets | A one-second stall is one `Rescheduled` frame with 59 abandoned targets, 750 ms discarded beyond the clamp and a one-second window maximum; the next frame waits a full interval. A reading below the previous one counts a regression and adds no interval; a frame-clock reset counts a baseline. |
| Overflow | Totals, window values and the pacing interval saturate at 2^53 - 1 instead of wrapping; the encoder clamps any larger value and the saturated record round-trips. |
| Wire bounds | The record round-trips through the codec inside a one-record `timing` array. The reader rejects values above 2^53 - 1, strings and negative numbers, unknown and missing fields, unknown outcomes, more than 120 or than recorded window frames, and unordered window statistics, leaving its output unchanged. |
| Real host loop | `session_tests.cpp` runs `HostSession::RunLoop` over a socketpair, polls `Status` until five frames are measured, and checks one baseline, the 16,666,667 ns interval, `simulation_ticks == sim_ticks` and ordered statistics. |
| Process boundary | `scripts/python/test_play_host_acceptance.py` reads the record from a `Status` result relayed by the real play supervisor after a committed reload: at least two baselines (startup and reload), ticks equal to `sim_ticks`, a bounded window and no timestamp-like fields. The editor live journey decodes it through `EditorController::RefreshTiming()`. |
| Editor view model | `apps/editor/tests/timing_view_tests.cpp` decodes a record, accepts 2^53 - 1 exactly, rejects 2^53, fractions, strings, unknown outcomes, an unwrapped or two-record array, inconsistent windows and an extra timestamp field, and checks the formatted rows and the panel status line. The layout test checks that Timing is tabbed with the inspector after a layout reset. |

### Records across the process boundary

Method: the `ludus_game_host_supervisor_acceptance` workload (headless
`ludus_game_host`, fixture module A reloaded to B, relayed by
`scripts/python/play_tool.py`), run three times in sequence on the Apple M5 host
described above. Each run printed the `timing` record of the `Status` result
after the committed reload; the scratch copy of the script added only that
print. Each record covers a quarter- to half-second session, so it shows that
real values cross the boundary intact, not a rate distribution.

The first block was measured at revision `16346bce` plus this change, before
the [wake leeway trial](#wake-leeway-trial) replaced the macOS pacing wait
(`nanosleep(2)`, pull request 344 not yet merged). The one-minute load average
was 32 to 23, about three times the core count.

| Run (pre-344 wait) | Frames / measured | Interval min / mean / max (ms) | Lateness mean / max (ms) | Outcomes |
| --- | --- | ---: | ---: | --- |
| 1 | 16 / 14 | 11.58 / 17.06 / 23.55 | 3.84 / 7.77 | 14 `Waited`, 2 baselines |
| 2 | 15 / 13 | 11.33 / 17.74 / 24.43 | 5.23 / 8.64 | 13 `Waited`, 2 baselines |
| 3 | 14 / 12 | 10.17 / 17.73 / 25.00 | 3.57 / 8.33 | 12 `Waited`, 2 baselines |

The second block repeated the workload on `develop` at `a641cd60` (this change
merged after the zero-leeway `kevent64(2)` wait), built with the pinned LLVM
23.1.3. The one-minute load average was 44 to 33, three to four times the core
count.

| Run (zero-leeway wait) | Frames / measured | Interval min / mean / max (ms) | Lateness mean / max (ms) | Outcomes |
| --- | --- | ---: | ---: | --- |
| 1 | 25 / 23 | 13.49 / 16.85 / 18.85 | 1.23 / 3.35 | 23 `Waited`, 2 baselines |
| 2 | 23 / 21 | 9.39 / 17.40 / 24.41 | 2.63 / 10.91 | 20 `Waited`, 1 `Late`, 2 baselines |
| 3 | 16 / 14 | 15.74 / 17.15 / 20.68 | 0.91 / 4.01 | 14 `Waited`, 2 baselines |

Every record in both blocks reports the 16,666,667 ns pacing interval, no
discarded time, no regressions, early wakes or skipped targets, and
`simulation_ticks` equal to `sim_ticks` (2 to 4: the ticks the session ran
before `Pause` plus the one `Step` before the reload). The two baselines are
startup and the committed reload. Intervals shorter than the target follow a
late wake: the next target stays on the absolute schedule, as the pacer contract
requires. The pre-344 mean lateness of 3.6 to 5.2 ms matched the earlier
headless 60 Hz `nanosleep(2)` measurements on this host. With the zero-leeway
wait the mean fell to 0.9 to 2.6 ms at a similar load; the remaining lateness,
including the one 10.9 ms `Late` frame, is CPU scheduling under a load average of
three to four times the core count, as the headless arms of the leeway trial
showed. With 14 to 23 measured frames per record, these are spot checks of the
boundary, not a lateness distribution.

### Validation

| Validation (macOS arm64 host, pinned Clang 18, warnings as errors) | Result |
| --- | --- |
| Development full build (`./scripts/build macos-clang-development`, editor and tests enabled) | Passed, warning-clean. The only linker notes are Homebrew Qt's deployment-target warnings, present before this change. |
| `ctest --test-dir out/build/macos-clang-development --no-tests=error` | 111 of 113 passed, including `ludus_game_host_timing_tests` (5 cases, 116 assertions), the session, reload and supervisor acceptance tests, and `ludus_editor_tests`. The Cocoa window suite fails without `LUDUS_TEST_COCOA=1` in a WindowServer session, as in earlier stages. The header self-sufficiency gate timed out at 120 s at a load average near 100 and passed in 116 s when rerun alone. |
| `python3 scripts/python/source_policy.py --base 16346bce` | Passed. |
| `./scripts/check --format` | Passed, including the include boundary, naming scopes (the new files are adopted) and container policy. |
| clang-tidy 18 on the new and changed sources | No findings in user code (`timing_telemetry.cpp`, `host_session.cpp`, `timing_tests.cpp`, `timing_view.cpp`, `timing_panel.cpp`, `controller_play.cpp`, `main_window_layout.cpp`). |
| Documentation: `check-docs`, strict MkDocs build, `check-wiki --documentation-only`, `test_docs.py`, `test_wiki.py` | Passed; the index needed no change. |
| `scripts/build-api --doxygen` (pinned Doxygen 1.18.0) | 173 headers, no new documentation gaps. No public header changed: the telemetry and editor view model are private. |
| Editor live journey | Not run locally: it needs an installed SDK prefix (`LUDUS_SDK_PREFIX`); CI's editor tier runs it. |
| `ludus_editor_tests` in full on `develop` at `a641cd60` (follow-up) | The binary run directly with `QT_QPA_PLATFORM=offscreen` passed 212 of 221 cases with 9 skipped (five Linux-only RAD session cases, three Linux native preview gates and one installed-SDK gate) and no failure, including the Timing view and layout cases; 5,027 assertions. Its cases summed to 203 s at a load average of 33 to 63, so the same run through `ctest` stopped at the suite's fixed 120 s timeout (reported as `Timeout`, repeated at a load average of 31 to 50). The one failed pre-merge `ctest -R ludus_editor_tests` run matches that timeout, not a Timing dock regression. |
| ASan/UBSan execution | Not run locally: the Clang 18 ASan runtime hangs at startup on this host. |

The Linux reference profile, sanitizer execution, browser presets, installed SDK
and build budget are validated by the pull request's CI tiers.
