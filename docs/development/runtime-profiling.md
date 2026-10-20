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
| `--capability NAME` | Declare a capability the runner cannot detect, such as `presentation` for a desktop session or a GPU class `gpu-<class>` (repeatable). |
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
`process/wall_ns`. Trace scenarios report each scope's `count`, `median_ns` and
`p95_ns`, plus `frames/count` and frame-interval percentiles from
`LUDUS_PROFILE_FRAME()` markers. Percentiles use the nearest-rank convention
shared with `scripts/benchmark-rhi`.

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
| `ubuntu` | `ubuntu-24.04` | `linux-clang-profile` | none (headless, no GPU) |
| `macos` | `macos-15` | `macos-clang-profile` | `presentation` (hosted macOS already presents Metal frames in CI) and `gpu-apple-paravirtual`, with a `--device` identity naming the virtual GPU |

The hosted macOS lane runs the GPU scenarios every night on GitHub's paravirtual
Metal GPU. That catches benchmark breakage (an image mismatch, a crash, an
incomplete sample set or missing GPU timings) within a day, at no cost. Its
timings show only large trends: the GPU is virtual, shared and changes with the
runner image, so they are not evidence about any real adapter.

Each lane:

1. Prepares the locked Profile preset without the optional Editor, using the
   shared native or macOS setup action and the pinned shader tools. The macOS
   presentation scenario uses the native World demo.
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
| `capabilities` | Space-separated extra capabilities; `presentation` only when the runner has a desktop session. |

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

The world demo scenarios use the demo's `--trace-output <path>` option. With
`--headless`, `--frames N` selects the number of simulation ticks (see the
[world demo guide](../examples/world-demo.md#run-the-application)).

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

The proposed [editor recording workflow](../architecture/graphics-debugging.md#perfetto-recording-in-the-editor)
adds interactive CPU/GPU/Both capture, separately from this scheduled benchmark
runner. The [GPU Perfetto contract](../architecture/profiling-final.md#basic-gpu-timing-for-perfetto)
starts with duration counters and explicit sampling coverage; benchmark output
does not yet establish GPU execution lanes or an implemented editor recorder.

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
| `trace` | `true` appends `--trace-output <path>`; the executable must write a Perfetto trace there. |
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
