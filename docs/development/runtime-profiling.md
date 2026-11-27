# Runtime profiling

Ludus runs a fixed set of runtime scenarios on a schedule and keeps their
measurements, Perfetto traces and logs as CI artifacts. This guide covers the
local command, the scheduled workflow, how to read a report and how to add a
scenario. Build-time measurement is separate; see [Build Profiling](build-profiling.md).

Timings are **reported, never gated**, on both shared GitHub-hosted runners and
self-hosted [device lanes](#controlled-device-lane). A run fails only when a
scenario is incorrect: a non-zero exit, a timeout, a missing or malformed trace,
an incomplete sample set, output the runner cannot parse, or a software
rasterizer where a GPU was declared. This follows the hardware/performance lane in
the [engineering quality architecture](../architecture/engineering-quality.md#4-validation-lanes-and-scheduling):
timing gates need controlled hardware and a calibrated variability bound. The
device lane provides the hardware; [Toward a timing gate](#toward-a-timing-gate)
describes the calibration that has to come first.

## Run it locally

Prepare the Profile preset once, plus the pinned shader tools for the world demo
scenarios:

```bash
./init.sh --cli linux-clang-profile --preset-only --with-tests
```

```bash
./scripts/shader-probe bootstrap
```

Then build and run every scenario this host supports:

```bash
./scripts/profile-engine
```

The default preset is the host's `-profile` preset (`linux-clang-profile` or
`macos-clang-profile`). The command configures that preset with the CMake options
the selected scenarios declare, builds only their targets, runs each scenario's
warmups and measured repetitions, and writes `out/runtime-profile/<preset>/`.

| Option | Effect |
| --- | --- |
| `--list` | List scenarios, owners and required capabilities. |
| `--scenario ID` | Run only this scenario (repeatable). It must be runnable on this host. |
| `--capability NAME` | Declare a capability the runner cannot detect, such as `editor` for prepared Qt editor dependencies, `presentation` for a desktop session or a GPU class `gpu-<class>` (repeatable). |
| `--device TEXT` | Declared adapter, OS and driver identity, recorded in the report. Required with a GPU class. |
| `--repetitions N` | Override every scenario's measured repetitions (1–50). |
| `--baseline FILE` | Compare medians against an earlier `report.json`. |
| `--output DIR` | Write elsewhere. The runner only replaces a directory it created. |
| `--no-build` | Run already-built executables. |

For example, on a macOS desktop session:

```bash
./scripts/profile-engine --capability presentation --baseline out/previous/report.json
```

To include the GPU scenarios, declare the machine's GPU class and identity:

```bash
./scripts/profile-engine --capability presentation --capability gpu-apple-m5 --device "Apple M5 (8 GPU cores), Metal, macOS 26.6"
```

A GPU class is `gpu-` followed by a short lowercase name for the adapter family
(`gpu-apple-m5`, `gpu-nvidia-rtx4090`). Declaring one also provides the `gpu`
capability that GPU scenarios require; `gpu` cannot be declared on its own, so
every GPU report names its device. `gpu` means a hardware-backed adapter,
physical or virtual; CPU software rasterizers such as llvmpipe are rejected. A
virtual GPU names itself in its class and identity, as the hosted macOS lane's
`gpu-apple-paravirtual` does, because its timings are not hardware evidence.

## Output

| Path | Contents |
| --- | --- |
| `report.json` | Schema 1 report: commit, dirty state, preset, compiler, host identity, CI run and runner identity, capabilities, device identity, and per-scenario metrics (median, min, max, relative spread and every sample). With `--baseline`, also per-metric deltas. |
| `summary.md` | The same results as Markdown tables; the workflow appends it to the job summary. |
| `traces/<scenario>.json` | Chrome/Perfetto trace from the last measured repetition of each trace scenario. Open it at <https://ui.perfetto.dev>. |
| `logs/<scenario>-<run>.log` | Combined stdout/stderr of every warmup and measured run. |

Metric names are paths: `iterations=10000/workers=4/median_ns`,
`case=xxh3_64/ns_per_operation`, `scope=WorldTick/p95_ns`,
`R7/layers=8/merge=1/gpu_duration_median_ns`. Names ending in `_ns`
or `ns_per_operation` are nanoseconds. Every scenario also reports
`process/wall_ns`. Scenarios that use the Perfetto scope parser report each
scope's `count`, `median_ns`, `p95_ns` and `max_ns`, plus `frames/count`
and the median, p95 and maximum interval between consecutive
`LUDUS_PROFILE_FRAME()` markers. The marker placement defines the observation
point. Percentiles use the nearest-rank convention
shared with `scripts/benchmark-rhi`.

Each `max_ns` or `interval_max_ns` sample is the longest observed value in one
measured repetition. The Markdown row's **Median** gives the typical
repetition's maximum and its **Max** gives the longest sample across all
measured repetitions. Use these rows to spot a single stall that leaves p95
steady. Reproduce the suspected workload in a controlled run to locate it in
the trace. The saved trace is from the last measured repetition and may come
from a different run than the stall. Older baselines simply lack these rows.

Reports identify the percentile method as `nearest-rank`. Historical reports
without that marker used a lower rank that understated p95 for some sample
counts. The first comparison with such a baseline keeps median comparisons and
omits p95 deltas; subsequent runs compare matching conventions.

A metric is marked **notable** against its baseline when all of these hold:

- the median moved by at least 10%;
- the measured repetition ranges do not overlap;
- for trace-derived timings, the shift is at least 250 ns. Trace timestamps are
  quantized by the platform clock (41.67 ns per tick on Apple Silicon).

"Notable" is a prompt to look at the traces, not a regression verdict. Compare
like with like: the summary warns when the baseline host or declared device
identity differs.

The report's `device` object records three views of the GPU, so a reader can
check that a lane measured what it claims:

| Field | Source |
| --- | --- |
| `declared` | `--device`, written by whoever registered the lane. |
| `gpu_classes` | The declared `gpu-<class>` capabilities. |
| `system_reported` | The OS report: `system_profiler SPDisplaysDataType` on macOS (model, vendor, cores, Metal family; no display serials), `vulkaninfo --summary` on Linux when installed. Best effort and empty when unavailable. |
| `engine_adapters` | The adapter the RHI actually selected, decoded from the scenarios' `R7ADAPTER` observation: name, vendor and device ids, raw driver version and whether it is a software adapter. |

GPU scenarios fail if the engine selects a known software adapter, or if the
adapter, enabled profile or backend changes between runs.

## Check a GPU budget on target hardware

Choose a minimum supported device, a renderer profile, an internal render
resolution and a presentation target before tuning a representative scene.
At 60 FPS the presentation interval is about 16.67 ms (`1000 / 60`). Choose a
rendering budget below that interval and state the intended headroom. Record
the actual adapter, backend, OS and driver, internal and output pixel extents,
scene and camera route, build profile, and refresh or frame cap. Keep power
mode and measurement conditions comparable. On thermally constrained hardware,
warm up with representative play and check sustained performance as well as
short captures.

1. In the [native World viewport](editor-workspace.md#recording-cpu-and-gpu-profiling),
   record **Both** while exercising the scene. In Perfetto, use completed frame
   samples for the GPU budget and named physical pass samples to investigate
   where time went. Keep the sample count and coverage beside the duration
   distribution. Report unavailable or incomplete timings explicitly. Pass
   intervals can overlap and may omit work outside the measured submissions.
2. Use **Play saved World** in a separate window for the [World demo adapter](editor-workspace.md#save-recovery-and-play)
   if the scene uses that adapter. The game's render path can differ from the
   editor's. The demo's native [`--pacing-report`](../examples/world-demo.md#run-the-application)
   measures frame-loop intervals. An SDK app can opt into the
   [RHI timing probe](../architecture/profiling-final.md#basic-gpu-timing-for-perfetto)
   to measure its own GPU submissions.
3. Repeat the same workload with the [verified Release package](editor-game-releases.md)
   on the physical target. Record the archive hash and the measurement scope.
   Report available GPU durations and observed frame intervals independently.
   When app telemetry is unavailable, use a supported external profiler such
   as [Metal System Trace in Instruments](https://developer.apple.com/metal/tools/)
   for a macOS Metal app.
   When testing a lower render resolution or cheaper rendering profile, change
   one setting at a time and repeat the route to compare cost and image quality.

Thanks to Roberto Lopez Mendez, [*GPU Processing Budget Approach to Game
Development*](https://developer.arm.com/community/arm-community-blogs/b/mobile-graphics-and-gaming-blog/posts/gpu-processing-budget-approach-to-game-development)
(Arm, 2017), for the device, frame-rate and resolution budgeting method. The
article's shader-core frequency calculation gives a theoretical throughput
estimate for the selected GPU. Ludus uses measured elapsed time on the target
for workload evidence. On Mali, offline compiler estimates for the exact
shader variant can help investigate register spills and pipeline cost, with
application measurements from [Arm Performance Studio](https://developer.arm.com/tools-and-software/arm-performance-studio).

### First-room macOS Release budget pilot

Build the pilot from a prepared macOS checkout with a matching installed Release SDK.
The fixture uses the World demo's first-room extraction at tick zero, its quad renderer,
linear scene target, tone mapping and surface composite. The camera holds the authored
view, doubles its orthographic span, then holds a 1.5× span reserved for validation.
Each hold warms up for 300 presented frames and records the following 1,200 frames.
The target is 60 Hz. Each hold must have all 1,200 completed GPU samples and a
nearest-rank p95 at or below **12.5 ms**, leaving about 4.17 ms of frame headroom.
Missing a hold or a timing makes the GPU verdict inconclusive. A completed hold
over budget makes a fully sampled configuration a miss. Record present-return
intervals separately. They include CPU scheduling and exclude display scanout.

Use a 2× Retina display and keep the 480 × 320 point window on that display.
The Metal drawable and intermediate targets scale together, with the compositor
mapping to the same 960 × 640 backing area. Compare 960 × 640, 720 × 480 and
480 × 320. Keep geometry fixed. If reducing the resolution changes authored-view
p95 by at least 0.25 ms and 10%, fit the fixed-geometry intercept, internal pixel
count and nonblack pixel count to the authored and wide views. Check absolute
p95 prediction error and false under-budget decisions on the withheld view.

```sh
python3 tests/scenes/gpu_budget/package.py out/metal-budget
app="$PWD/out/metal-budget/extracted/MetalBudget.app/Contents/MacOS/MetalBudget"
for pair in full-timed half-timed medium-timed full-disabled; do
    size=${pair%-*}
    "$app" "$size" "${pair#*-}" >"out/metal-budget/$pair.bin" || exit 1
    python3 tests/scenes/gpu_budget/analyze.py "out/metal-budget/$pair.bin" "$size" out/metal-budget/evidence || exit 1
done
python3 tests/scenes/gpu_budget/estimate.py out/metal-budget/evidence
```

The archive hash printed by the package command identifies the measured binary.
Capture the matched Development comparisons using the native editor's
`[.metal-budget-preview]` case with `LUDUS_EDITOR_PROFILING_CAPTURE` set to a
new absolute JSON path, and a separate-window `ludus_world_demo --level
apps/world_demo/levels/first-room.world.json --frames 1200 --frame-rate 60 --pacing-report`.
The editor records its preview with Both, and the Development player advances
simulation. The package freezes simulation to control the submitted geometry.

#### Apple M5 first-room result

The 8-core Apple M5 (Mac17,3, 16 GiB) on macOS 26.6 (25G72) passed at all three sizes. The app
selected Metal on Apple M5. Both attached displays ran at 60 Hz with 2× backing.
The package did not record which display contained the window. It ran with
AC power and Low Power Mode off. macOS reported fair thermal state before and
after each capture. Other desktop applications were active. Each run presented
4,500 frames over approximately 82 seconds, including warm-up. Inputs and
geometry stayed fixed across sizes. Each counted frame drew 60 visible quads in
one instanced draw, then two full-screen indexed quads for tone mapping and
presentation, for three draws and 124 triangles in total.

| Internal and Metal drawable | GPU p95 in ms, authored / wide / held-out | Worst hold p95 present-return interval | Completed GPU samples | GPU verdict |
| --- | --- | ---: | ---: | --- |
| 960 × 640 | 1.772 / 1.401 / 1.397 | 17.033 ms | 3,600 / 3,600 | Pass |
| 720 × 480 | 1.576 / 1.265 / 1.592 | 17.600 ms | 3,600 / 3,600 | Pass |
| 480 × 320 | 1.228 / 1.188 / 1.190 | 17.382 ms | 3,600 / 3,600 | Pass |
| 960 × 640, timer disabled | Unavailable in every hold | 17.495 ms | 0 / 3,600 | Inconclusive |

The renderer owner can retain full resolution for this room on this device.
Reducing to half resolution saved 0.544 ms (31%) at authored-view p95 with the
same geometry. The [full-resolution image](gpu-budget-evidence/full.png) and
[half-resolution image](gpu-budget-evidence/half.png) preserve room layout and
colors. Diagonal edges have coarser steps in the smaller render. The full-size
timed and disabled textures were byte-identical for each camera hold. The
frame-loop owner can investigate scheduling and presentation if a p95
present-return interval within 16.67 ms is required. Enabled and disabled
pacing distributions overlap. This single run per setting does not isolate a
causal instrumentation cost. The measurement owner should enable timing or
collect a supported trace for a GPU verdict on a timing-unavailable run.

Triangle count alone gives the same prediction across the three resolutions.
The six training observations gave a fixed-geometry p95 estimate in ms of
`1.105 + 0.262 × internal megapixels + 0.999 × nonblack megapixels`.
The held-out errors at full, medium and half size were 0.108, 0.262 and
0.015 ms. None of the three predictions falsely passed the budget. All three
measured values were well below it, so this pilot leaves prediction accuracy
near the threshold untested.

The [measurement archive](gpu-budget-evidence/apple-m5-first-room.zip) contains
the per-frame CSVs, all twelve display textures, quantiles, withheld estimates,
environment and package manifest. Times are elapsed nanoseconds in the CSVs,
and unavailable durations are blank. The extracted archive SHA-256 was
`3407136f50d732043b19a5f04ec6df939987735c64c67fccb2e8f42403f19b2f`.
The SDK used Clang 23.1.3, arm64 Release at engine base
`9def10d461343b26e56560e0728508eaa1052770` with the bundled Cocoa startup
fix. The archive also records the installed Platform library hash.

The Development comparisons used the same first-room source. The native Cocoa editor
opened the World through its adapter, showed the authored camera in Albedo shading,
and recorded five seconds in Both mode. It completed 153 of 153 eligible frame
timings (p50 1.211 ms, p95 4.371 ms, p99 5.572 ms), plus 306 physical pass samples.
Submission timestamp intervals had p50 34.632 ms and p95 36.350 ms. The trace
contains no compositor or scanout intervals. This comparison used Development
editor objects from SDK manifest revision `62f86866e782009f6d20985bdd932b192078570a`
and the pilot's capture fixture.

The separate-window Development player ran the current World player sources against
installed Clang 23 Development SDK `bba94d8774`, with a 60 Hz cap and the
normal 960 × 640 point window. It presented 1,200 frames. A second 1,200-frame
run confirmed the player window was onscreen after activation. Its 1,198 reported
present-return intervals had p50 99.998 ms, p90 100.624 ms and p99 103.601 ms.
The cadence is consistent with the player's 10 Hz occlusion policy. The report
does not expose the per-frame visibility decision or standalone GPU durations.
The player/platform owner can trace visibility and pacing policy selection on
this desktop. Both play logs and the editor trace are in the measurement archive.

## Scheduled workflow

[`.github/workflows/performance.yml`](../../.github/workflows/performance.yml)
runs nightly at 03:17 UTC on `main` and can be started manually from the
**Actions → performance → Run workflow** button, selecting the lanes (`all`,
`hosted` or `device`), optional scenario ids and a repetition override. Named
scenarios must be runnable on every selected lane; for example, `world-presented`
and the GPU scenarios fail the Ubuntu lane, which has neither `presentation` nor
a GPU. GitHub runs schedules from the default branch only, so a
change to the workflow takes effect after it lands.

| Lane | Runner | Preset | Declared capabilities |
| --- | --- | --- | --- |
| `ubuntu` | `ubuntu-24.04` | `linux-clang-profile` | `editor` (headless, no GPU) |
| `macos` | `macos-15` | `macos-clang-profile` | `editor`, `presentation` (hosted macOS already presents Metal frames in CI) and `gpu-apple-paravirtual`, with a `--device` identity naming the virtual GPU |

The hosted macOS lane runs the GPU scenarios every night on GitHub's paravirtual
Metal GPU. That catches benchmark breakage (an image mismatch, a crash, an
incomplete sample set or missing GPU timings) within a day, at no cost. Its
timings show only large trends: the GPU is virtual, shared and changes with the
runner image, so they are not evidence about any real adapter.

Each lane:

1. Prepares the locked Profile preset with the Editor, using the shared native
   or macOS setup action and the pinned shader tools. Both lanes measure CPU
   texture conversion; macOS also measures the native editor viewport.
2. Downloads the `runtime-profile-<lane>` artifact from the most recent completed
   run on `main` as the baseline, if one exists.
3. Runs `./scripts/profile-engine` and appends `summary.md` to the job summary.
4. Uploads `out/runtime-profile/<lane>/` as `runtime-profile-<lane>` with 90-day
   retention, even when a scenario failed.

The workflow never runs on pull requests and is not a required check. To compare
a branch, run the command locally on the same machine for the base and head
commits, then pass the base `report.json` as `--baseline`. For a change to GPU
performance, use a real GPU and run both sides in one sitting, on mains power
with other applications closed. On the base commit:

```bash
./scripts/profile-engine --capability gpu-apple-m5 --device "Apple M5, Metal, macOS 26.6" --scenario rhi-ordered-graph-gpu-timing --scenario renderer-clustered-lighting --output out/runtime-profile/base
```

Then on the head commit:

```bash
./scripts/profile-engine --capability gpu-apple-m5 --device "Apple M5, Metal, macOS 26.6" --scenario rhi-ordered-graph-gpu-timing --scenario renderer-clustered-lighting --output out/runtime-profile/head --baseline out/runtime-profile/base/report.json
```

A notable change there is a prompt to repeat the pair, not a verdict; fanless
laptops throttle under sustained load.

## Weekly performance review

Reserve one investigation or optimization slot each week. The reviewer, a
developer or an agent, reads the nightly evidence, checks open performance work
in [Ludus Kanban](https://github.com/orgs/The-Ludus-Project/projects/1), and
advances the highest-priority actionable item within the available slot. Carry
forward an active investigation before starting a new one. Pick new work in
[board order](../wiki/contribute/index.md#prioritize-roadmap-work).

Start with these measurements from `report.json`. Each reported median of a
p95 metric is the median across repetitions of their individual p95 values.

| Workflow | Scenario and metric | Use |
| --- | --- | --- |
| Editor camera navigation | `editor-viewport-navigation`, `EDVIEW/workload=<workload>/latency_p95_ns` | Review analytic-light and mesh-light workloads individually. Inspect `cpu_p95_ns`, `gpu_p95_ns`, the log's `EDVIEWPASS` rows and the trace to attribute a change. The latency metric reaches the test's new-presentation and GPU-sample observation, so it moves in display-refresh steps: a frame whose GPU work no longer fits before the next refresh adds a whole interval. |
| Engine frame pacing | `world-presented`, `frames/interval_p95_ns` and `frames/interval_max_ns` | Look for recurring slow frames and isolated hitches. The marker intervals include pacing and scheduling. Use `scope=WorldFrame/p95_ns` and the trace to inspect frame work. |
| Engine simulation | `world-headless`, `scope=WorldTick/p95_ns` | Track the fixed simulation fixture. Inspect tick phase scopes when the total moves. |

Use `renderer-view-preparation`, `editor-texture-display`, and the other module
benchmarks to investigate affected paths. Keep build and iteration costs in
[Build Profiling](build-profiling.md). The nightly runtime suite does not yet
measure launch to usable Editor, project open to first usable viewport, or
import to asset ready. Add coverage in that order unless current user impact
justifies moving another journey ahead. Keep cold and warm runs distinct and
time the point at which the user can act. Record the fixture, timing endpoints,
and capability requirements before setting a budget.

### Turn a nightly signal into work

1. **Gather the last seven completed scheduled runs on `main`.** Open the
   `performance` workflow and use the per-lane job summaries and artifacts.
   From a repository checkout with authenticated GitHub CLI access:

   ```bash
   gh run list --workflow performance.yml --branch main --event schedule --status completed \
       --limit 7 --json databaseId,createdAt,headSha,conclusion,url
   gh run download RUN_ID --pattern 'runtime-profile-*' --dir out/performance-review/RUN_ID
   ```

   Record failed or missing reports and skipped capabilities. A setup or sample
   failure is a measurement gap. A successful job with no physical-device lane
   provides hosted signals only.
2. **Check whether a change persists.** Compare successful repetitions of the
   same scenario and workload, using the same preset, compiler, host CPU, OS,
   runner image, percentile method and, where applicable, adapter and driver.
   Check the fixture and its measured endpoints when commits differ. Use the
   runner's [notable-change rule](#output) to screen changes. Compare two recent
   runs against the same earlier comparable report to see whether a step change
   persisted after the workflow adopted it as the previous-night baseline.
   Compare physical measurements with their declared budgets as well, so a
   consistently slow workload can still enter the queue. If evidence is sparse
   or the host changed, record the gap and specify the rerun needed.
3. **Track the user impact.** Reuse an existing issue when its acceptance
   covers the finding. Otherwise file a focused issue for the owning module
   or Editor workflow. Include the scenario and metric, run links and SHAs,
   absolute values and deltas, sample counts and ranges, trace evidence,
   reproduction command, and the next measurement or improvement to verify.
   Keep a recurring hosted signal provisional pending controlled reproduction.
   Add the issue to Kanban with an owner, priority and rationale following the
   contributor guide. Rank by user impact and frequency, then by the strength
   of the evidence. Track recurring measurement failures as coverage work.
4. **Validate the selected fix.** Reproduce the issue using an isolated base
   and candidate build on the same device. Alternate their repetitions, keep
   the scene, quality, build profile and input route fixed, and report the
   same metric with ranges and sample counts. For a change in one phase,
   measure the end-to-end journey too. Keep the scenario's correctness checks
   and link before and after reports from the PR and issue. Close the item
   after integration and acceptance evidence, and check the next comparable
   nightly report for recurrence.

Record meaningful changes, measurement gaps, and the selected action in the
weekly review. When all monitored metrics lack an actionable signal, carry
forward existing work or advance the next coverage item. Leave timing checks
advisory until the [controlled-device calibration](#toward-a-timing-gate) is
complete.

## Controlled device lane

The `device` job runs the same command on self-hosted machines with a physical
GPU, where the GPU scenarios run and timings vary less than on shared virtual
machines. It is opt-in: without the `LUDUS_PERF_DEVICE_LANES` repository
variable the job is skipped rather than queued for a runner that does not exist.

Each lane is one entry in that variable, a JSON array:

```json
[
  {
    "lane": "macos-m5",
    "os": "macos",
    "gpu_class": "gpu-apple-m5",
    "preset": "macos-clang-profile",
    "device": "Apple M5 (8 GPU cores), Metal, macOS 26.6",
    "capabilities": "presentation"
  }
]
```

| Field | Meaning |
| --- | --- |
| `lane` | Short lowercase id; the artifact is `runtime-profile-device-<lane>`, and its previous run is the baseline. |
| `os` | `macos` or `linux`; matches the runner's default OS label (labels are case-insensitive). |
| `gpu_class` | The runner label and the declared `gpu-<class>` capability. |
| `preset` | `macos-clang-profile` or `linux-clang-profile`, matching `os`. |
| `device` | Adapter, OS and driver identity passed as `--device`. Update it, and expect a new baseline, after a driver or OS update. |
| `capabilities` | Space-separated extra capabilities; `presentation` only when the runner has a desktop session. Add `editor` to prepare and measure editor workloads; Qt must already be installed on a device runner. |

The job runs on runners labelled `[self-hosted, ludus-perf, <os>, <gpu_class>]`.
It validates the entry, prepares the Profile preset with
`./init.sh --cli --preset <preset> --preset-only --locked --with-tests --no-system-install`
and the pinned shader tools, then profiles, publishes and uploads like a hosted
lane. It installs nothing system-wide and uses no secrets.

### Trust boundary

Ludus is a public repository, so a self-hosted runner must never execute code
from a pull request or a fork. The
[engineering quality plan](../architecture/engineering-quality.md#7-reproducible-platform-setup-artifacts-and-caches)
forbids untrusted contributor code on persistent hardware agents, and says trust
admission never comes from a file the pull request supplies. The workflow
condition alone cannot provide that: a pull request controls its own YAML and
could add a job that targets the `ludus-perf` labels. Three layers apply:

1. **Workflow.** The job runs only for `schedule` and `workflow_dispatch` events
   on `refs/heads/main` of `The-Ludus-Project/Ludus`. The workflow has no
   `pull_request` trigger, read-only permissions and no persisted checkout
   credentials.
2. **Runner admission hook (the boundary).** Each device runner runs
   [`tools/ci/perf-runner-job-started.sh`](../../tools/ci/perf-runner-job-started.sh)
   as its `ACTIONS_RUNNER_HOOK_JOB_STARTED` script. It is installed outside the
   workspace and is not writable by the runner account. The runner executes it
   before any step and fails the job unless `GITHUB_WORKFLOW_REF` is
   `performance.yml` on `refs/heads/main` of this repository and the event is
   `schedule` or `workflow_dispatch`. Workflows cannot overwrite these
   `GITHUB_*` variables, so a pull-request job assigned to the runner fails
   before its code runs.
3. **Repository settings.** Set **Settings → Actions → General → Approval for
   running fork pull request workflows** to require approval for all external
   contributors. This narrows exposure but does not replace the hook.

The organization is on GitHub's Free plan. Runner groups restricted to selected
workflows require GitHub Enterprise Cloud; if the organization moves to it,
restrict the runner's group to
`The-Ludus-Project/Ludus/.github/workflows/performance.yml@refs/heads/main` as
an additional layer.

Thanks to GitHub's [secure use reference](https://docs.github.com/en/actions/reference/security/secure-use),
[pre-job script documentation](https://docs.github.com/en/actions/how-tos/manage-runners/self-hosted-runners/run-scripts)
and [variables reference](https://docs.github.com/en/actions/reference/workflows-and-actions/variables)
for these controls.

### Register a device runner

A repository administrator and the device owner do this once per machine:

1. **Prepare the machine.** Use a dedicated machine with a physical GPU and a
   standard user account that has no other credentials. Install the
   prerequisites from [Building Ludus](building.md) (Clang 23, Python 3.10 or
   newer, Git), plus the GitHub CLI `gh` for the baseline download. On Linux,
   install the GPU vendor's Vulkan driver and `vulkan-tools`.
2. **Install the admission hook.** From a reviewed `main` checkout, copy the
   hook to a directory the runner account cannot write, outside the runner and
   workspace directories:

   ```bash
   sudo install -d -o root -m 0755 /usr/local/libexec/ludus-perf
   ```

   ```bash
   sudo install -o root -m 0755 tools/ci/perf-runner-job-started.sh /usr/local/libexec/ludus-perf/job-started
   ```

3. **Register the runner.** In the repository's **Settings → Actions → Runners
   → New self-hosted runner**, follow GitHub's download steps on the machine.
   Registering on the repository rather than the organization keeps the runner
   to this repository. Then configure it with the lane labels; the `self-hosted`
   and OS labels are added automatically:

   ```bash
   ./config.sh --url https://github.com/The-Ludus-Project/Ludus --token <registration token> --labels ludus-perf,gpu-apple-m5 --name ludus-perf-macos-m5 --work _work
   ```

   The registration token is single-use and expires within an hour; do not
   store it. Prefer `--ephemeral` with automated re-registration when the
   device can support it, so every job starts from a fresh runner.
4. **Enable the hook.** In the runner directory, add the hook to `.env` before
   starting the runner:

   ```bash
   echo "ACTIONS_RUNNER_HOOK_JOB_STARTED=/usr/local/libexec/ludus-perf/job-started" >> .env
   ```

5. **Run it as a service.** Use `./svc.sh install` and `./svc.sh start`. On
   macOS this installs a LaunchAgent in the user's session; keep that user
   logged in and the display awake when the lane declares `presentation`.
   Restart the service after any `.env` change.
6. **Declare the lane.** In the repository's **Settings → Secrets and
   variables → Actions → Variables**, add the entry to
   `LUDUS_PERF_DEVICE_LANES`.
7. **Verify.** Start **Actions → performance → Run workflow** on `main` with
   `lanes: device`. The job log starts with the hook's
   `ludus-perf admission: accepted` line. Check the summary's device line and
   the report's `engine_adapters` against the declared identity. Starting the
   workflow from another branch must not reach the runner; if a job from
   another branch does reach it, the hook must refuse it.

When the hook changes on `main`, reinstall it on every device runner.

Keep measurement conditions stable: mains power, a fixed performance mode
(macOS Low Power Mode off; a fixed CPU frequency governor on Linux), no other
runners or scheduled work on the machine during the 03:17 UTC window, and
automatic OS or driver updates outside it. To retire a lane, remove its entry
from the variable first, then remove the runner.

## Scenarios

Scenarios live in
[`config/runtime_profile_scenarios.json`](../../config/runtime_profile_scenarios.json).
The runner rejects unknown keys, duplicate ids and inconsistent settings before
it builds anything.

| Id | Owner | Measures | Requires |
| --- | --- | --- | --- |
| `world-headless` | `apps/world_demo` | 2000 simulation ticks with a trace of the world tick phases | `shader-tools` |
| `world-presented` | `apps/world_demo` | 240 presented frames with a trace of frame, tick and extraction scopes | `shader-tools`, `presentation` |
| `rhi-ordered-graph` | GraphicsRhi | Merged and unmerged ordered raster graph at 1/8/16/31 layers: CPU setup, execute and readback wait, with every image checked against the unmerged oracle | `shader-tools`, `gpu` |
| `rhi-ordered-graph-gpu-timing` | GraphicsRhi | The same workload with GPU timestamp probes armed: device-clock GPU duration of each available sample, plus instrumented CPU timings | `shader-tools`, `gpu` |
| `renderer-clustered-lighting` | Renderer | L5 640×360 clustered, direct-reference and direct-optimized lighting: view preparation, recording and completion/readback CPU latency, with paired image checks | `shader-tools`, `gpu` |
| `threading-jobs` | FoundationThreading | Job graph submit-to-completion by worker count and work size | — |
| `strings-corpus` | FoundationStrings | String construction, sharing, hashing and lookup per operation | — |
| `input-reducer` | Input | Keyboard reducer step and batch latency | — |
| `containers-associative` | FoundationContainers | Sorted/hash/tree lookup and insertion by element count | — |
| `editor-texture-display` | Editor | Production RGBA8, HDR and depth display conversion at 320×180 and 1280×720, with every output pixel checked | `editor` |
| `editor-viewport-navigation` | Editor | Native raster viewport navigation with analytic and emissive mesh lights: active GUI work, GPU frame duration, interaction-to-presentation latency, CPU trace and idle retention | `editor`, `shader-tools`, `presentation`, `gpu` |
| `renderer-view-preparation` | Renderer | Camera-view CPU preparation at 128/512/1024 ordered heterogeneous objects, with visibility and batching checks on the deterministic RHI backend | — |

The world demo scenarios use the demo's `--trace-output <path>` option. With
`--headless`, `--frames N` selects the number of simulation ticks (see the
[world demo guide](../examples/world-demo.md#run-the-application)). The editor
scenario uses the same option of the editor test executables: their shared
Catch2 entry point accepts `--trace-output <path>` in every editor
configuration, records a CPU capture with the viewport wall-clock spans around
the whole session, and fails with exit code 3 instead of writing nothing when
the build has profiling disabled or the file cannot be written. An unknown
option is rejected by Catch2 before any test runs, so a scenario log that
reads `Unrecognised token: --trace-output` means the entry point was built
without the option, not that the viewport failed.

The GPU scenarios run the benchmark sections of `ludus_rhi_render_tests` and
`ludus_renderer_render_tests` with the same environment switches and sample-set
checks as `scripts/benchmark-rhi` and `scripts/benchmark-renderer` (see
[Paired native measurements](fullscreen-rendering.md#paired-native-measurements)
and the [renderer measurement notes](../architecture/renderer-systems.md#correctness-and-measurement)).
Those scripts remain the tools for a one-off qualification record;
the scenarios put the same workloads on a schedule. `rhi-ordered-graph` is the
uninstrumented control for `rhi-ordered-graph-gpu-timing`: compare their CPU
metrics to see the probe cost, without subtracting it. GPU durations use the
device clock and only count samples whose timing state is available; each
group's `samples` metric shows how many were. A run with no available GPU timing
sample fails, because it did not measure GPU duration. Durations are not correlated with
the CPU clock or the display.

The native [editor recording workflow](editor-workspace.md#recording-cpu-and-gpu-profiling)
provides interactive CPU/GPU/Both capture, separately from this scheduled benchmark
runner. The [GPU Perfetto contract](../architecture/profiling-final.md#basic-gpu-timing-for-perfetto)
starts with duration counters and explicit sampling coverage; benchmark output
does not establish calibrated GPU execution lanes or pass-level timing.

For an AMD Vulkan compute workload, see
[Qualify workgroup shapes on AMD Vulkan](../architecture/renderer-systems.md#qualify-workgroup-shapes-on-amd-vulkan)
for the paired renderer benchmark and separate native shader inspection.

### Editor workloads

Prepare an optimized editor before comparing editor workloads:

```bash
./init.sh --cli linux-clang-profile --preset-only --with-tests --with-editor --locked
./scripts/profile-engine --capability editor --scenario editor-texture-display
```

Use `macos-clang-profile` on macOS. `./scripts/editor --preset linux-clang-profile`
also launches the prepared Profile editor for an interactive investigation.

`editor-texture-display` invokes the same `MakeDisplayImage` implementation as
the texture viewer and graph readback display. It measures 20 conversions per
encoding/extent after three warmups, checking pixels outside the timed call.
RGBA8/HDR use the ordinary manual [0, 1] range; depth also exercises automatic
range detection. It reports conversion CPU median/p95, without GPU or window
server requirements. It does not measure ordinary shared-texture presentation,
which bypasses CPU pixel conversion.

`renderer-view-preparation` tracks the renderer's CPU preparation used during
editor navigation. It prepares already ordered heterogeneous opaque objects at
three counts, requiring all objects to remain visible in two instance batches.
The deterministic RHI backend isolates CPU work from driver/GPU variance; it does
not measure native uploads or GPU execution. Its median/p95 by object count helps
identify a return to quadratic packet-ordering work.

On a desktop with a GPU class and identity, add the viewport scenario:

```bash
./scripts/profile-engine --capability editor --capability presentation --capability gpu-intel-uhd620 --device "Intel UHD 620, Vulkan, local driver identity" --scenario editor-viewport-navigation
```

The viewport workload runs the editor's actual Qt presentation, World extraction,
graph service and raster renderer on the analytic-light and emissive-mesh
fixtures. Five warmup navigation steps precede 30 measured steps per workload at
a 640×480 logical widget size. GPU output must remain on the
shared-device path, output readback must remain zero, and an unchanged graph must
retain its view without producing more timed GPU frames. Each row reports active
GUI service CPU work, a completed device-clock GPU frame duration, and wall-clock
latency from issuing the orbit through observing the new presentation, graph
revision, and GPU sample. That latency includes event-loop and presentation
scheduling. GPU timestamps use the device clock. The report records the adapter and
retains a CPU Perfetto trace for diagnosing preparation and scheduling delays.

The scenario log also carries report-only rows that the parser does not summarize.
`EDVIEWFIRST,<workload>,<nanoseconds>` is the wait from starting the preview to its
first presentation, which includes program, shadow-map and sky-map preparation and
is excluded from the orbit samples. A first frame approaching the 60 s deadline
shows up here before the run fails. `EDVIEWPASS` rows give each completed per-pass
GPU sample from the batches the editor's GPU recording uses:
`workload, sample, batch, scope, kind, ordinal, duration_ns, dropped_passes`.
A timed operation belongs to the outermost open scope, so the default raster graph
reports its pass name, such as `Main/raster/draw`, and `ordinal` separates its
native operations in encoding order. The renderer's nested scopes
(`Deferred/Material`, `Deferred/Lighting`, `Deferred/Overlays`) label native
captures only. Metal raster durations are vertex-to-fragment envelopes and can
overlap, so the operations need not sum to the frame duration. A pass batch can
skip a frame while it is pending; `sample` is the orbit in progress when the batch
completed. Compare medians per scope and ordinal across builds of the same
scene, and check that the scope and ordinal sets match before attributing a
change.

These workloads participate in the existing baseline comparison, notable-change
summary and 90-day artifacts. Timing changes remain report-only on shared hosts;
coverage failures (including lost frames, missing GPU timings and malformed
traces) fail the workflow. Device runners opt in with the `editor` capability.

### Add a scenario

Add an entry with every field:

| Field | Meaning |
| --- | --- |
| `id` | Lowercase id, unique in the manifest. |
| `owner` | The module or application directory that owns the workload. |
| `description` | What the workload measures. |
| `target`, `executable` | CMake target and its path relative to the preset build directory. |
| `arguments` | Fixed command-line arguments. |
| `cmake_options` | `LUDUS_*` options (`ON`/`OFF`) the target needs; every runnable scenario must agree. |
| `environment` | `LUDUS_*` variables set for this scenario's runs. A variable any scenario sets is removed from every other scenario's inherited environment. |
| `requires` | Capabilities: `shader-tools` is detected from `out/shader-tools`; `gpu` comes from a declared GPU class; anything else is declared with `--capability`. |
| `trace` | `true` appends `--trace-output <path>`; the executable must write a valid, nonempty Perfetto trace there. Numeric parsers may retain a diagnostic trace while deriving their stable metrics from sample rows. |
| `parser` | How to read metrics (below). |
| `warmup`, `repetitions`, `timeout_seconds` | Unmeasured runs, measured runs and the per-run deadline. |

| Parser `kind` | Fields | Reads |
| --- | --- | --- |
| `perfetto-scopes` | — | The requested trace (only with `trace: true`). |
| `key-value` | `identity`, `metrics` | Lines of `key=value` tokens; identity keys name the row. |
| `csv` | `header`, `key`, `metrics`, `ignore` | The table after the exact `header` line; rows whose key is in `ignore` are skipped. |
| `regex` | `pattern` | Lines matching the pattern; named groups other than `label` are metrics. |
| `tagged-rows` | `tables`, `observe` | CSV sample lines that start with a tag, such as `R5,...`. Each table names its `tag`, positional `columns`, grouping `key`, `metrics`, a `where` filter of exact column values and the exact number of `rows` a run must print. Each group reports the median, p95 and sample count of each metric. `observe` lists identity tags (such as `R7ADAPTER`) that must appear once per run and stay identical across runs. |

Keep workloads deterministic and bounded: fixed seeds and inputs, no network, and
a few seconds per run on a hosted runner. Instrument engine code with
FoundationProfiling scopes (see the [module guide](../modules/foundation/profiling.md))
so trace scenarios break the work into named phases. Run
`PYTHONPATH=scripts/python python3 -m unittest test_runtime_profile -v` after
changing the manifest or the runner.

## Toward a timing gate

The runner writes `timing_policy: report-only` and implements no gate. A timing
metric may become a gate only for one device lane, scenario and metric at a time,
after these steps:

1. **Calibrate on an unchanged commit.** Run the scenario in at least ten
   separate sessions on the lane (separate jobs, not just repetitions within
   one) without changing the commit, OS or driver. The variability bound is a
   high percentile, such as p99, of the relative difference between session
   medians. It belongs to the exact device identity, meaning the declared
   identity plus the engine adapter and driver.
2. **Choose a minimum practical effect.** The owning module states the smallest
   change worth acting on. For trace-derived timings it must exceed clock
   resolution. The gate threshold is the larger of the bound and this effect.
3. **Compare base and head in one job.** Build both commits on the same device
   and interleave their repetitions, alternating which runs first, as
   `scripts/benchmark-rhi` alternates merged and unmerged runs. Flag a
   regression only when the head median exceeds the base by more than the
   threshold, the repetition ranges separate, and a rerun confirms it.
   Comparing against the previous night's artifact stays report-only: OS,
   driver and thermal changes fall between those runs.
4. **Version the calibration.** Store the bound with its device identity,
   sample counts and date beside the scenario. Treat it as invalid when the
   identity changes, and recalibrate after OS or driver updates.
5. **Promote gradually.** Run the gate as advisory first, then as release-candidate
   acceptance for declared devices
   ([section 4](../architecture/engineering-quality.md#4-validation-lanes-and-scheduling)).
   It never runs on pull-request code on device runners.

## Limits

- Hosted runners are shared virtual machines. Their timings vary between runs and
  between image updates, and their GPU timings describe a paravirtual GPU, not
  hardware. The macOS lane presents through a virtual display; its frame
  intervals follow display pacing, not GPU cost.
- Linux hosted runners have no display or GPU, so `world-presented` and the GPU
  scenarios are skipped there.
- Device lanes reduce noise but do not remove it: thermal state, background
  work and driver updates still move timings. GPU durations come from timestamp
  queries on the device clock and cover each workload, not a whole frame.
- The filesystem benchmark needs a generated pack corpus, and the smoke app
  runs until its window closes; neither is a scenario yet.
- Calibrated timing gates (above) and base/head comparison jobs remain future
  work in the engineering quality plan.

## Windows qualification measurements

The [Windows qualification runner](../architecture/windows-direct3d12.md#physical-runtime-evidence)
reuses the World and ordered-graph scenarios on a named physical adapter.
The shared scenario executor resolves Windows executables with their `.exe`
suffix. Tagged tables retain minima and maxima for native samples alongside
median and p95 values, so peak allocation limits can use the observed maximum.
The GPU timing workload also records logical bytes, API accounting availability
and peak API bytes. Direct3D 12 queries the selected adapter's padded committed-resource
requirements for buffers, textures, private depth and retained texture uploads.
These counts exclude surface/facade allocations, descriptor heaps and transfer
rings; they do not measure physical residency. Detached resources remain counted
until their public lifetime dependencies and queue submissions permit retirement.
The Windows qualification limits are scoped to the declared
device and workload.
