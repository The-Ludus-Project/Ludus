# Continuous integration

This guide records the implemented workflows. The proposed
[engineering quality architecture](../architecture/engineering-quality.md) defines
additional tooling and evidence improvements; its
[reference review](../architecture/engineering-quality-gems-review.md) records
the Game Programming Gems ideas incorporated after the initial design.
Q0 failure semantics and Q1 analysis remain described below.

## Integrate changes and promote a validated batch

`develop` is the integration branch. `main` is the last completely validated commit.
Keep `main` as the default branch: the scheduler and promotion code run from its
trusted workflow revision. Task branches and pull requests target `develop` after
activation. Review changes and resolve review threads before accepting a PR.
Acceptance into `develop` does not establish full platform qualification.

Pull requests targeting `develop` run source formatting, source-policy and workflow
syntax checks, tooling regressions, the installed host-tooling tests, and public
API/documentation validation. Drafts retain these inexpensive checks. When the
change is not documentation-only, they also run **Integration compile (Linux
Clang, Editor)**: one `ubuntu-24.04` job that prepares `linux-clang-development`
with the Editor, its trusted render preview and the native shader worker
(`LUDUS_BUILD_SHADER_WORKER`, which live shader reload uses), builds every default
target with warnings as errors, and runs the `unit` CTest label (Editor tests
offscreen, plus the shader compiler and worker protocol tests) and
`ludus_target_detection`. It
restores the Conan and `editor` compiler caches that the batch's Optional editor
job saves on `main` and never writes its own, so it takes about 10 minutes with a
warm cache and about 30 minutes cold. The `Ubuntu Clang` gate requires it on
integration PRs and expects it skipped elsewhere, where Development and Optional
editor supersede it. A macOS compile stays batch-only: macOS setup alone takes
about 11 minutes and hosted macOS minutes cost ten times as much as Linux minutes.
Other native build variants, full analysis, sanitizers, browser builds, SDK
qualification, editor journeys and build budgets run on the combined batch. The
`ci:full` label does not expand an integration PR into a full run.

`.github/workflows/batch-ci.yml` wakes at minutes 13 and 43 of each hour. When
`BATCH_CI_ENABLED` is `true`, it captures the current `main` and `develop` commit SHAs.
If they differ and `develop` descends from `main`, it records the candidate in
`ci/last-batch-attempt` and invokes the existing validation workflows with that
exact SHA. The original workflow definitions come from `main`; source, local
build actions and tests come from the captured candidate. Changed workflows are
syntax-checked before admission and take effect as coordinator definitions after
promotion. Periodic workflows can start late when GitHub is busy; 30 minutes is
the intended cadence, not a completion deadline.

A batch that promotes coordinator edits still executes its original workflow
revision through the last job. Verify changed coordinator behavior with a later
`develop` candidate after the repair reaches `main`. Dispatching without a new
candidate skips validation and publication, so it does not exercise the change.

Before changing the compiler pin, prepare the trusted validation workflows to
read the captured candidate's `config/tool_versions.json`. Install its LLVM major
and use its configured compiler/formatter paths, including installed-SDK and
generated-project checks. When the candidate supplies `llvm_apt_source.py`, run
it before apt so newer pins can use their committed package-source key. Older
candidates without that helper keep their existing Ubuntu package source.
Land this compatible workflow preparation before the new pin: changing both
together in `develop` cannot update the workflow definitions already on `main`.
If recovery requires landing that preparation directly on `main`, bring its
history into `develop` through an integration PR using a merge commit. Squashing
or rebasing that PR discards the ancestry link; promotion rejects a candidate
that diverges from `main`, even when the files contain the same repair.

Only one batch is active. New `develop` commits do not cancel it or change its input.
Concurrent scheduler ticks coalesce into at most one pending run. A tick captured
before `main` advanced skips, letting the next tick use the new stable revision.
Native, SDK, wiki, browser and threading-platform workflows must all succeed. Filesystem block-prefix
recovery additionally runs when its workflow, persistence implementation, recovery
probe or recovery drivers changed anywhere in the complete batch diff. Invalid
metadata, failed/cancelled checks and unexpectedly skipped required jobs cannot
promote. No new changes means no full run.

After validation, a separate job checks out the original trusted `main` revision
and mints a fresh repository-scoped GitHub App credential. The trusted snapshot
job uses a separate short-lived App token only to read refs and record the attempt.
Test jobs receive no App credential or inherited secrets. Promotion rechecks that `main` is unchanged,
that the candidate descends from it, and that the candidate remains in `develop`'s
history. Git verifies ancestry again and pushes exactly that commit with an
explicit expected-old-SHA lease. A concurrent `main` update rejects the push;
there is no rebase, squash, new merge commit or non-fast-forward promotion.

The validated wiki/embedded editor artifact is published after promotion, without
rebuilding it. A publication failure leaves the promoted source intact and the
previous website available; rerun the publication job. Promotion does not trigger
another native, SDK or wiki build on `main`. Batches run in the `main` cache scope
and save compiler entries under the candidate SHA, warming caches visible to PRs.
The separate performance and release workflows retain their existing schedules
and explicit publication policies. Standalone nightly native/browser runs retain
Q1 advisory analyzer coverage. Batches skip the advisory macOS analyzer job and
advisory analyzer steps; required inventories and clang-tidy remain mandatory.
Linux builds/tidy use four workers and native macOS jobs use three. The uncached
build-time budget retains two workers for comparable measurements.

## Activate the process once

The coordinator is disabled until `BATCH_CI_ENABLED=true`. These settings need a
repository/organization administrator; no credential belongs in Git or chat.
Finish them after the migration PR lands, before accepting ordinary PRs into develop.

1. Land the migration PR through the existing `main` merge queue. Legacy PRs
   targeting `main` and `merge_group` checks remain supported for this transition.
   Drain or remove the remaining old queue entries before switching processes.
2. Use the existing `develop` branch. After landing, fast-forward it to main if
   it is behind; if it contains unique commits, merge main through a reviewed PR
   so those commits are preserved. Keep `main` the default branch.
   Protect `develop` against deletion and non-fast-forward updates; require PRs,
   existing review requirements, and these GitHub Actions checks: `Ubuntu Clang`,
   `Host tooling (ludus_tools) unit tests`, and `Public API documentation`.
   Do not require the skipped browser/wiki packaging checks on develop. Apply the
   Copilot review rule to develop as well. An integration merge queue is unnecessary;
   do not require every develop PR to rebase whenever develop advances.
3. Create a dedicated GitHub App and install it on this repository only. Grant
   **Contents: read/write**, **Workflows: read/write** and the automatic Metadata
   read permission. Workflow permission lets promotion include reviewed commits
   that change workflow files; no Administration, Actions or secret permission
   is needed. Disable webhooks if the App does not use them. Add its client ID
   as repository variable `BATCH_PROMOTION_APP_CLIENT_ID`.
4. Create the `batch-promotion` Actions environment, allow deployment from
   **main only**, and store the App private key there as environment secret
   `BATCH_PROMOTION_APP_PRIVATE_KEY`. Use no required reviewer for unattended
   promotion. The environment restriction is part of the credential boundary;
   do not put this key in an unrestricted repository secret. Keep the
   `github-pages` environment restricted to main too.
5. Replace the main merge-queue rule with promotion-only protection. Keep
   deletion and non-fast-forward rules in an invariants ruleset without an App
   bypass. Use a separate main ruleset with
   **Restrict updates**, allowing only the promotion App to bypass it, and main
   PR/status requirements with that same App as an **Always** bypass actor.
   Remove **Require merge queue**. Ordinary users and Actions' default token
   must not update main. Keep protections on develop and the marker independent so
   a main-only App bypass does not grant access to them.
6. Give `ci/last-batch-attempt` its own ruleset restricting creation, updates and
   deletion to the same App, with non-fast-forward protection. This ref records
   attempts, including failures, not successful qualification. The trusted snapshot
   job uses the main-only environment to mint its own short-lived App token; the
   default `GITHUB_TOKEN` needs only read permission throughout the coordinator.
7. Set repository variable `BATCH_CI_ENABLED=true`. Retarget remaining feature
   PRs to develop. The next real develop change starts full validation at the next tick;
   use **Actions → Batched integration and promotion → Run workflow**, selecting
   main, to start sooner. Confirm the first candidate, all required suites, exact
   promoted SHA and Pages deployment in that run before declaring rollout proven.

GitHub's [scheduled workflow contract](https://docs.github.com/en/actions/reference/workflows-and-actions/events-that-trigger-workflows#schedule),
[reusable workflow contract](https://docs.github.com/en/actions/how-tos/reuse-automations/reuse-workflows),
[App-token action](https://github.com/actions/create-github-app-token), and
[Git push lease contract](https://git-scm.com/docs/git-push) inform this workflow.
The administrator must verify ruleset interactions before enabling promotion;
a missing permission or denied push fails rather than weakening the protections.

## Recover a failed batch

Validation failures or cancellation before promotion keep main and its deployed
documentation unchanged. Cancellation does not undo a promotion already in flight
or completed. Inspect the failing suite and its artifacts. Fix or revert the offending change
through a develop PR; split or bisect a large batch when the source is unclear.
Changes accepted while the run was active remain eligible for the next snapshot.

The scheduler will not repeatedly run heavy CI for an unchanged attempted SHA.
After an infrastructure failure, dispatch the coordinator on main with **retry**
enabled. This reruns every required suite before another promotion attempt;
never push main manually or mark failed checks successful. If main moved during
validation, retry against its new base. If develop diverged, reconcile it through
reviewed merges; do not reset either branch or rewrite other contributors' work.
Set `BATCH_CI_ENABLED=false` to stop new snapshots. An already active batch is
unaffected; cancel that run explicitly if it must not promote.

If source promotion succeeded but documentation publication failed or was skipped,
check **Publish promoted documentation** separately from **Promote validated
candidate**. An intentionally skipped optional recovery job must not suppress
publication after successful promotion. Failed, skipped or cancelled promotion
must never publish a candidate's artifact. Confirm both jobs succeed, then open
the deployed site and check the changed content before declaring publication proven.

Rerun a failed publication job while its validated artifact is available. GitHub
[reruns](https://docs.github.com/en/actions/how-tos/manage-workflow-runs/re-run-workflows-and-jobs)
use the original workflow revision; a coordinator repair therefore needs promotion
and a later batch to verify its new behavior. To rebuild and publish the current
stable documentation explicitly, select **Actions → Ludus Wiki → Run workflow**
and choose **main**. That workflow revalidates the wiki and browser payload before
deployment; it does not promote a new source commit.

## Change routing

The shared change-scope action tests routing before classifying the complete
change. PRs compare the merge base with their head; legacy queue runs compare the
queue base and candidate. Integration PRs select the cheap `integration` tier;
legacy main PRs keep their earlier fast tier, and legacy queue/manual/batch runs
select `full`. Standalone scheduled/manual native runs select `nightly`, including
the advisory analysis. Scheduled and manually dispatched batches always run full native
validation, including a documentation-only batch.

The documentation exemption for individual PRs is an explicit allowlist in
`scripts/python/ci_scope.py`: docs, the README, MkDocs and pinned Doxygen inputs,
and owning API/wiki tools. Unknown paths, mixed changes, malformed metadata and
missing comparisons require native routing. Rename detection is disabled so a
move into docs does not hide an engine change. The `Ubuntu Clang` gate checks the
complete job inventory and requires exactly the successes/skips selected by the
tier. Missing outputs or failed/cancelled dependencies fail the gate; the only
exception is a superseded pull-request run, described below.

Required checks use job conditions instead of workflow-level path filtering, so
branch protection receives a result even when expensive jobs are intentionally
skipped. The full batch retains native Debug/Development/Profile/Release, SDK
relocation, ASan/UBSan and TSan controls, static analysis, editor journeys, PCH,
uncached build budgets, browser backends and documentation tooling on both macOS
architectures. The independent public API check still enforces the shrink-only
legacy baseline against the captured main SHA.

### Superseded pull-request runs

GitHub evaluates every check run with a required name on the PR head commit, one
per workflow run, so a cancelled duplicate blocks merging even when another run
of the same commit passed. Several PR activities can fire for one commit at
once; for example, pushing and marking a draft ready produce `synchronize`,
`ready_for_review` and often `edited` in the same second. `ci`, `Ludus Wiki` and
`project-sdk` therefore cancel in-progress runs only for `synchronize`, which
always carries a new head. Its runs share one group per PR and supersede runs for
earlier commits. Every other activity revalidates the head in a group named for
that commit that never cancels an in-progress run. GitHub keeps at most one run
pending per group and cancels an older pending run, which has created no checks.
A same-commit duplicate therefore waits and runs completely; it costs a duplicate
run, not a blocked check. A later push does not cancel such a run for an earlier commit.

The `Ubuntu Clang` gate still runs after cancellation. If a dependency was
cancelled, it reads this workflow's runs with the job's read-only `actions`
token. It passes only when a newer, uncancelled pull-request run of the same
workflow exists for the same head commit. That run publishes its own required
gate, which owns the result. Otherwise, or if the lookup fails, the gate fails
and asks for a rerun. Classification failures, failed jobs and unexpected skips
never consult other runs. Merge-group, scheduled, manual and batch runs never
treat cancellation as success. Other workflows' required checks are ordinary
jobs; the per-commit group prevents them from being cancelled by duplicates.

To recover a PR blocked by a cancelled or failed check left before this rule,
rerun that cancelled workflow run; its new attempt replaces the check in the
same check suite. Do not manually cancel a PR's newest run. The gate of the run
it replaced may already have passed while the newest run was pending, so the
commit has no validation until the newest run is rerun. Rerunning an older
commit's push run joins the push group and cancels the head's in-progress push
run; rerun the head commit's runs instead.

`.github/actions/native-setup` prepares only the job's selected preset. `CI=true`
enables warnings as errors without invoking the full `init.sh --ci` validation.
The profiling job disables compiler caching and retains its existing measurement
concurrency for comparable uncached profiles.

Ubuntu 24.04 runners do not package the pinned LLVM 23. Before installing
`clang-23` and its tools, Linux jobs run `scripts/python/llvm_apt_source.py`.
It registers only the `llvm-toolchain-noble-23` suite from apt.llvm.org, signed
by the committed and fingerprint-checked `config/apt/apt.llvm.org.asc`
([ADR 0025](../decisions/0025-llvm-toolchain-pin.md)). That suite serves only
its newest 23.1.x build. The macOS setup action installs `llvm@23` and `lld@23`
from Homebrew; init rejects either if `brew upgrade` moved it to another major.

### macOS runner images

Native jobs use `macos-15` arm64. Host and documentation tooling separately check
`macos-15-intel`. The SDK deployment target remains `14.0`; a macOS 15 runner does
not establish macOS 14 runtime qualification. See GitHub's
[macOS 14 deprecation notice](https://github.com/actions/runner-images/issues/13518).

### Threading platform lanes

`.github/workflows/threading-platforms.yml` runs the only Windows build and the
macOS ThreadSanitizer run of `Ludus::FoundationThreading`
([threading milestone T3](../architecture/threading.md#milestones)). Every batch
calls it, and promotion requires it. Integration PRs run it only when they change
the threading module, the Windows FoundationBase diagnostics backend, the Windows
Conan profile or the workflow itself.

The Windows job installs the upstream LLVM 23.1.3 installer on `windows-2022`, after
checking it against its pinned SHA-256 digest (Chocolatey does not package LLVM 23
yet), and drives `clang++` against the runner's Microsoft STL and dynamic CRT. It installs the
pinned CMake/Ninja/Conan versions, exports the local Catch2 recipe and installs
only that locked package with `config/conan/profiles/windows-clang-x86_64`. Ludus
configures only the FoundationThreading closure on Windows (Base, Memory,
Containers, Time, Profiling and Threading); other modules have no Windows backend
and no SDK is installed. The job runs the threading, startup-seam, profiling-adapter
and allocation suites once, then repeats the scheduler and startup suites 20 times.

The macOS job prepares `macos-clang-development` without the editor or shader
tools, proves TSan detection with the sanitizer control, then runs the threading,
startup-seam and profiling-adapter suites under TSan, repeating the first two.
The pinned LLVM 23 TSan runtime also starts on local macOS 26 hosts, which LLVM
before 22.1.3 could not; see [macOS sanitizer runtimes](building.md#macos-sanitizer-runtimes).

## Dependencies and caches

CI uses `init.sh --locked` to consume the committed `conan.lock` without
regenerating it or permitting dependencies outside the lock. Update dependencies
through normal initialization and review the resulting lockfile separately.
A missing or incompatible lock fails CI rather than silently resolving new
versions.

Conan cache keys include the preset, local recipes and sources, lockfile,
profile, tool versions, and presets. A commit suffix permits refreshed snapshots
after restoring a compatible older cache. Compiler caches are separated by
preset and configuration (ordinary, PCH, editor). CMake honors
`LUDUS_ENABLE_CCACHE=1` from the environment for native builds; browser presets
keep their separate cache policy. The native setup action installs ccache.

## Analysis and timing

`LUDUS_TIDY_JOBS` sets the maximum number of concurrent native clang-tidy
processes. The local default is one; CI uses four on Linux and three on macOS. Diagnostics are saved per
compile command and printed in source/variant order.
The selected preset's CMake File API codemodel is reconciled with its compilation
database. All compiled first-party C, C++, Objective-C and Objective-C++ sources
under `modules/`, `apps/`, `tools/`, `tests/` and `examples/` are inventoried;
platform-disabled sources are outside that configured scope. CMake-declared
generated sources and prepared/external dependencies are reported as exclusions.
This does not claim coverage of optional targets or separately configured SDK
consumers absent from that preset; their existing dedicated checks remain.

Missing files, malformed/stale databases, missing source or target variants, and
empty required scopes fail. Exact duplicate commands are deduplicated; distinct
commands for the same source are analyzed using separate single-entry databases.
`LUDUS_TIDY_SHARD_COUNT` and `LUDUS_TIDY_SHARD_INDEX` select deterministic shards
only after the full inventory is validated. Empty shards fail.

Ignored `out/quality/analysis/run-*/` contains `inventory.json`, the exact
single-entry compile databases, original combined stdout/stderr byte prefixes
and `results.json`. Each process has a 300-second deadline and a 1 MiB diagnostic
limit; the invocation has a shared 100 MiB raw-log budget. A timeout, truncation,
missing log, launch failure or omitted result fails even if the process returns
zero. Independent commands still report findings. CI always uploads each shard's
available evidence with 14-day retention; a missing artifact fails the upload.
These are Q0 inventory/control records, not the proposed Q2 full run schema.

## Scheduled runtime profiling

The separate `performance` workflow runs runtime profiling scenarios nightly on
hosted Ubuntu and macOS runners, and on manual dispatch. It is not a pull-request
or batch check: scenario correctness failures fail that workflow, and
timings are report-only. Reports, Perfetto traces and logs are kept as artifacts
for 90 days. See [Runtime profiling](runtime-profiling.md). The hosted macOS
lane also runs the RHI and renderer GPU scenarios on its paravirtual Metal GPU,
for correctness and rough trends rather than hardware timing.

Its opt-in `device` job adds self-hosted machines with a declared physical GPU,
where the same GPU scenarios produce hardware timings. Lanes are listed in the
`LUDUS_PERF_DEVICE_LANES` repository variable; without it the job is skipped.
The job runs only scheduled or manually dispatched `main` code, never pull
requests or forks. Each device runner's job-started hook
(`tools/ci/perf-runner-job-started.sh`, installed outside the workspace) refuses
any other job before it runs, so admission does not depend on pull-request YAML.
Registration steps and the trust boundary are in
[Controlled device lane](runtime-profiling.md#controlled-device-lane).

## Sanitizer failure controls

`LUDUS_ENABLE_UBSAN=ON` propagates `-fno-sanitize-recover=undefined` through the
same project options as sanitizer instrumentation. Recoverable UB therefore
fails the invoking test. Thanks to LLVM's
[Clang 23 UBSan usage contract](https://releases.llvm.org/23.1.0/tools/clang/docs/UndefinedBehaviorSanitizer.html#usage)
for the fatal recovery policy and signed-overflow control.

Enabled ASan, UBSan and TSan builds register `sanitizer-control` CTest tests.
Their deliberately defective children live only in `tests/quality/`; they are
never installed or called by production. Each Python driver requires clean
startup to exit zero, then requires a separate defect process to exit nonzero
and emit the specific bounds/overflow/race diagnostic. A recovery exit, unrelated
crash, runtime startup failure, truncated output or timeout is a failed control.
`WILL_FAIL` alone is insufficient and is not used.

Run the controls after building the selected sanitizer preset:

```bash
./scripts/build linux-clang-asan-ubsan
out/host-tools/venv/bin/ctest --preset linux-clang-asan-ubsan --label-regex sanitizer-control --output-on-failure --no-tests=error
```

On macOS use `macos-clang-asan-ubsan`. TSan remains a separate build tree;
its existing CI job now builds and runs `ludus_tsan_control_child` before the
concurrency acceptance suite. It follows LLVM's [Clang 23.1.0 ThreadSanitizer
usage example](https://releases.llvm.org/23.1.0/tools/clang/docs/ThreadSanitizer.html#usage):
parent and worker write one shared global volatile slot before joining, using
direct `pthread_create`/`pthread_join` calls with explicit error checks. The
worker polls a separate relaxed atomic gate set after the parent's write. This
forces the conflicting access to follow the live parent's write in wall-clock
time without a release/acquire happens-before relationship. Using release/acquire
for this gate would remove the intentional defect. Volatile stores remain
visible in optimized builds. CI requires 16 consecutive successful clean/fault
control pairs with `--repeat until-fail:16`; any failure stops acceptance rather
than retrying to obtain a pass. Native Clang controls explicitly locate the
compiler's LLVM symbolizer; missing symbolizers fail configuration. Control
children receive explicit sanitizer options with no inherited suppression/log
redirection; UBSan requests runtime recovery to prove the compiler policy still
terminates. This environment applies only to the test children.

Each child has a 15-second deadline and 1 MiB log ceiling; each two-process
control has a 2 MiB raw-log ceiling. Its unique run directory under
`out/build/<preset>/tests/quality/<kind>-evidence/` retains clean/fault logs and
`results.json`. Sanitizer and TSan CI jobs preserve that evidence even on failure.
The macOS sanitizer preset runs and preserves controls before native runtime and
Behavior acceptance, so a later application failure cannot prevent their capture.
Its separate Debug diagnostic configure retains its startup/decision tests.

## Q1 analysis and host tooling

Platform/tooling owns the new baseline checks and finding triage. They remain
advisory for at least two weeks; promotion requires reviewed Linux/macOS/browser
inventories and findings, an accountable maintainer and measured runtime cost.
Existing format, tidy, compiler, sanitizer and source-inventory gates stay
required. Advisory commands still exit unsuccessfully for findings, missing
tools, omissions, timeouts or incomplete output; CI explicitly tolerates their
step failure during rollout and preserves their evidence. Artifact failures are
not advisory. Complete macOS source admission has a separate required
`--inventory` step; Linux/browser inventories run inside their required tidy
checks. Analyzer findings cannot excuse source omissions.

```bash
./scripts/setup-quality-tools   # explicit network install into ignored out/
./scripts/check --tooling       # no installation, configure or engine build
./scripts/check linux-clang-development --analyzer
```

`config/tool_versions.json` pins Ruff and actionlint, including SHA-256 checksums
for actionlint's Linux/macOS x86_64/arm64 archives. Checks verify exact executable
versions and record executable digests. `ruff.toml` enables selected syntax,
undefined-name, duplicate-definition/key and invalid-control-flow rules. It
does not introduce formatting or import-order churn. The command inventories
tracked Python files and Python entry points without extensions; vendor code is
excluded explicitly. Stage new files before a local check to include them.
actionlint checks every `.github/workflows/` YAML workflow's syntax and expression
contracts. Embedded Python/ShellCheck adapters are disabled so optional unpinned
tools cannot silently change the check. Composite action YAML is not claimed as
actionlint coverage. ShellCheck remains a separate future rollout.

`config/clang-analyzer.yml` selects stable LLVM 23 `core`, `cplusplus` and `unix`
families. Experimental/opt-in checkers are not enabled.
The released Move check uses `WarnOn=KnownsOnly`: it models known standard types
without treating Ludus' documented empty/reusable moved-from states as errors.
`--analyzer` uses the same
complete per-configuration native inventory and bounded execution as required
tidy. Browser checks now reconcile their database with the independent CMake
File API too, retaining explicit Emscripten flags. Linux shards, the complete
macOS Development build and both browser profiles produce advisory baselines;
no baseline suppression file is generated. Fix a finding or justify a narrow
source suppression before promotion.

The initial local macOS Development scan examined 267 sources / 291 compile
commands, including fault-test variants, with no inventory omissions. After
reviewing the Move model, four commands report findings in three sources:
`diagnostic_format.cpp` (null argument/count relationship), `namespace.cpp`
(empty span versus entry allocation, two variants), and `jobs.cpp` (graph
destruction/initialization). These remain unsuppressed advisory findings for
maintainer path review; this scan does not establish that they are defects or
that other supported-platform baselines are clean. The same build passes its
native headless tests; device availability is reported separately.

Unique `out/quality/tooling/run-*/` directories retain Ruff/actionlint logs,
commands, versions, digests, source counts and status. Analyzer evidence remains
under `out/quality/analysis/run-*/`. CI uploads both for 14 days, including failed
checks. Regression controls prove actual Ruff/actionlint/analyzer diagnostics
reject known-bad inputs and cover checksum, empty/missing scope and editor setup
boundaries. [VS Code setup](building.md#vs-code) uses the same prepared preset
for clangd and Ludus tasks without rewriting custom editor files.

## Measuring feedback

GitHub's separate setup/build/test/analysis steps expose phase durations.
Compiler jobs publish cache statistics, and the budget job uploads its detailed
profile. Compare cold and warm runs across several commits when evaluating
changes; parallel jobs can lower completion time while increasing runner minutes.
The browser workflow continues to test the exact packaged Release ZIP and
cancels superseded pull-request runs independently. Renderer browser coverage runs
in separate baseline (L0–L4) and scaling (L5) jobs, each with a ten-minute
budget and its own evidence artifact. All image oracles and backend cases remain
required; the packaged browser lane retains RHI, camera and scripting coverage.
