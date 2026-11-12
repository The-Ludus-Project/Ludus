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
API/documentation validation. Drafts retain these checks. When the change is not
documentation-only, they also compile: the **Editor, shader worker and unit tests**
job checks that each source compiles once, builds every default target with the
Editor and shader worker and warnings as errors, and analyzes the Editor document
sources, without running its tests or journeys. Both **Clang static analysis**
shards analyze every translation unit, and shard 0 checks changed lines with the
naming and magic-number rules. These jobs restore Conan and compiler caches
without saving their own, so they cannot evict a shared baseline. The Editor job
restores the cache that [develop compiler cache](#develop-compiler-cache) saves
for the latest `develop` commit, and falls back to the batch's cache on `main`.
Measured batch runs took about 25 minutes for the Editor compile and 4 minutes
per analysis shard with warm caches. Without a develop cache, PR compiles start
from the newest batch on `main`, so while batches fail and `main` stays behind
`develop` they take longer; measured PR Editor jobs took 27-38 minutes when
`main` was 60 commits behind. Editing the description or marking a draft ready
reuses the run that already compiled the same commit
([superseded pull-request runs](#superseded-pull-request-runs)).

From 2026-10-19 to 2026-11-08 integration PRs compiled nothing. Between 2026-10-27
and 2026-11-08, 2 of about 38 batches that ran their suites passed: build-graph,
analysis and compile failures merged into `develop`, each failed a 2-hour batch
within 3-13 minutes, and the next change could not be validated until a fix merged.
Source policy also rejects direct nested build commands inside changed
`add_executable`, `add_library` and `target_sources` source lists. This catches
merge damage that leaves a build command inside another command's arguments.
Comments, literals and expansions are ignored; this lexical guard does not
evaluate CMake or establish source existence and target dependency correctness.
The `Ubuntu Clang` gate requires the source/tooling, Editor compile and analysis
jobs for non-documentation changes and verifies that every deferred native job is
skipped. The installed
host-tooling and public API checks remain separate required checks. No label
expands an integration PR into a full run; dispatch a batch on `main` when
earlier platform qualification is needed. A pull request targeting any other
branch runs the full tier. No workflow triggers on `push` or `merge_group`.

The combined batch owns native compilation, unit tests, full analysis,
sanitizers, browser builds, SDK qualification, editor journeys and build budgets.
Its **Editor, shader worker and unit tests** job prepares
`linux-clang-development` with trusted render preview and the native shader
worker (`LUDUS_BUILD_SHADER_WORKER`, used by live shader reload), builds every
default target with warnings as errors, and runs the union of the `unit` and `editor`
CTest labels offscreen, including shader compiler and worker protocol tests, plus
`ludus_target_detection`. Tests carrying both labels run once. It builds with
`LUDUS_EDITOR_PRECOMPILED_HEADERS=OFF`, so an editor source that relies on a Qt
or standard header it does not include fails there; the macOS jobs and the Linux
sanitizer build keep the default (on). It also retains
static analysis and the installed Behavior SDK journey. This replaces the separate native build on every
PR with one authoring build per combined batch. Native dependency and compiler
caches continue to be saved in the batch's `main` scope.

Required native build, test and analysis jobs start after change classification
and any macOS runner admission dependencies. A source-policy or formatting failure therefore no
longer skips every Linux compile in a batch, including **Development and SDK**
and the Editor job: each still reports its own result, so one batch shows every
failure to fix. Promotion is unchanged: the `Ubuntu Clang` gate still requires
the source/tooling job to succeed. Like every coordinator edit, this takes effect
for batches only after it reaches `main`.

PR checks establish that the Linux development configuration with the Editor
compiles and passes analysis, not test or platform readiness. Another platform's
compile failure, a test failure or a runtime regression can still enter `develop`;
the batch must reject it before `main` advances. Inspect failed batch diagnostics and submit a focused fix or
revert through a `develop` PR, as described in [recovery](#recover-a-failed-batch).
Do not bypass full qualification to clear a backlog.

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
When retiring the PR-only integration compile, the native gate accepts its
legacy job entry only when a pre-promotion trusted workflow reports it skipped
outside the integration tier. Missing full-validation jobs still fail. The first
batch promoting this workflow change uses the old Editor job definition; verify
the combined worker-on authoring build on a subsequent candidate.

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
Native, SDK, Linux wiki, macOS documentation tools, browser, threading-platform
and Windows workflows must all succeed. Filesystem block-prefix recovery additionally runs when its workflow, persistence implementation, recovery
probe or recovery drivers changed anywhere in the complete batch diff. Invalid
metadata, failed/cancelled checks and unexpectedly skipped required jobs cannot
promote. No new changes means no full run.

### Stop a failed batch early

Any failed suite job rejects the candidate, so the batch's remaining jobs only add
diagnostics. The **Watch for failed suites** job runs trusted `main` code beside the
suites and polls the run's jobs every minute. When a suite job (`native / ...`,
`windows / ...` and so on) fails, times out or fails to start, the jobs still
running get 30 minutes from that first failure to report (`STOP_GRACE_SECONDS` in
`scripts/python/batch_ci.py`). Then **Cancel failed batch** cancels the run, so the
next tick can validate a candidate containing the fix instead of waiting for the
longest sanitizer and macOS jobs. In measured batches, deterministic failures
appeared within 3-13 minutes and every job except the sanitizers and macOS native
backends ended within about 45, so most failures still appear in one run.

A stopped run shows as cancelled. Its failed jobs, not the jobs cancelled to stop
it, are the batch failures: [develop health](#develop-health) treats it as red and
fix PRs name those jobs. Jobs that had not started, such as installed host tooling
and macOS documentation tools waiting for native validation, report only on a
later candidate. The watcher
fails open: if job listings stay unavailable or it reaches its 330-minute limit,
the batch finishes normally. **Prune superseded caches** still runs after a stop.

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

### Run focused MCMC Vulkan parity

The native `ci` workflow has a manual `mcmc_vulkan_parity_only` input for the
MH/Jump Restore CPU oracle, GPU parity kernel and both cooked template
configurations. Dispatch it on the exact review branch:

```bash
gh workflow run ci.yml --ref <branch> -f mcmc_vulkan_parity_only=true
```

This mode uses Ubuntu 24.04, the pinned native/shader toolchains, Mesa Vulkan and
`VK_LAYER_KHRONOS_validation`. Skipped parity/capture cases and Vulkan validation
errors fail the job. Its `mcmc-vulkan-parity` artifact records the tested commit,
adapter details, emitted SPIR-V/compiler manifest and complete CPU/parity/HDR-capture logs. Mesa software-device
evidence validates Vulkan execution; it does not establish hardware-GPU performance.
The estimator profile and numerical tolerance belong to the
[template contract](render-graph-authoring.md#use-the-rao-blackwellized-mh-template).

The focused dispatch skips the other native jobs and the aggregate gate. Its
skipped aggregate uses a separate check name, so it cannot replace the required
**Ubuntu Clang** check on the same commit. The
input has no effect on pull requests, scheduled runs or batch calls, and does not
promote a commit, publish a site or move the batch marker. It supplements the
required integration/batch checks.

## Activate the process once

The coordinator is disabled until `BATCH_CI_ENABLED=true`. These settings need a
repository/organization administrator; no credential belongs in Git or chat.
Finish them after the migration PR lands, before accepting ordinary PRs into develop.

1. Land the migration PR through the existing `main` merge queue and drain or
   remove the remaining old queue entries before switching processes. Activation
   is complete: the `merge_group` triggers, the reduced tier for PRs targeting
   `main` and its `ci:full` label have since been removed from the workflows.
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

## Develop health

A red batch closes `develop` to everything except proven fixes. Without this,
later changes merge while the batch is already failing, and new breakage hides
behind the existing failure until the batch can no longer be repaired in one step.

The **Develop health** job in `ci.yml` runs on every pull request targeting
`develop`. It reads completed `Batched integration and promotion` runs on `main`,
newest first, and uses the latest *validated* batch: one whose suites ran. Ticks
that captured nothing, failed captures and cancelled runs are skipped, except a
batch [stopped early](#stop-a-failed-batch-early) by **Cancel failed batch**: it is
red, and its failed (not cancelled) suite jobs are the jobs to fix. A batch
whose suites all passed is green even if its promotion then failed. If no
validated batch appears within the scan bound, develop is treated as open.
Because the gate reads run history rather than coordinator state, it applies as
soon as it reaches `develop`.

| Develop | Pull request | Result |
| --- | --- | --- |
| Green | Any | Passes |
| Red | Documentation-only, and no `wiki` suite failed | Passes |
| Red | Declares fixes for failed batch jobs, with evidence on its head commit | Passes, listing the jobs still red |
| Red | Anything else | Fails, naming the failed batch jobs |

To submit a fix while develop is red:

1. Add one line per batch job the PR fixes to its description, using the job
   name exactly as the failed batch reports it:

   ```text
   Batch-Fix: native / ASan and UBSan
   ```

   A fix declares only the jobs it repairs; independent failures can be fixed by
   separate PRs. Prefer reverting the offending change when it is identifiable.
2. Merge current `develop` into the branch. The evidence must contain the last
   attempted batch candidate (`ci/last-batch-attempt`), so it exercises the fix
   on top of the code that failed.
3. Dispatch the workflow that owns each declared job on the branch. The gate
   reports the exact command, for example:

   ```bash
   gh workflow run ci.yml --ref <branch> -f validation_tier=full
   ```

   The prefix selects the workflow: `native` is `ci.yml`, `sdk` is
   `project-sdk.yml`, `wiki` is `wiki.yml`, `browser` is `webgpu-probe.yml`,
   `threading` is `threading-platforms.yml`, `windows` is
   `windows-direct3d12.yml` and `recovery` is `filesystem-recovery.yml`.
   Branch dispatches never deploy the wiki or promote.
4. When each declared job has succeeded on the PR's exact head commit, re-run
   the **Develop health** check. Editing the description starts a new run.

Evidence runs the branch's own workflow definitions. Review workflow edits in a
fix PR: a changed job cannot prove itself. Batches run `main`'s definitions, so a
job renamed on `develop` since the last promotion has no matching evidence job;
the maintainer decides whether to waive that batch. Evidence requires a branch
in this repository, since fork dispatches do not run here.

`develop-health.yml` re-runs the gate on open `develop` pull requests after a
batch completes, when that batch is red or turned develop green. It skips ticks
that validated nothing and green-after-green results. A pull request whose latest
`ci.yml` run is still in progress is listed in that job's summary; re-run its gate
after the run completes. `workflow_run` uses the default branch's definition, so
automatic re-evaluation starts once this workflow reaches `main`. Until then,
dispatch **Develop health re-evaluation** on `develop` to re-check every open PR.

For an infrastructure failure, a maintainer can set the repository variable
`DEVELOP_HEALTH_WAIVER` to the failed batch run ID, then dispatch the
re-evaluation. The waiver opens develop only for that run; the next red batch
closes it again. Dispatch the coordinator with **retry** as described below, and
clear the variable once a batch passes. Agents never set waivers.

Make **Develop health** a required check on `develop` once a batch passes. Until
then it reports the failing batch without blocking acceptance, so the existing
backlog can be cleared with fix and revert PRs first.

## Recover a failed batch

Validation failures or cancellation before promotion keep main and its deployed
documentation unchanged. Cancellation does not undo a promotion already in flight
or completed. Inspect the failing suite and its artifacts. Fix or revert the offending change
through a develop PR; split or bisect a large batch when the source is unclear.
Changes accepted while the run was active remain eligible for the next snapshot.

The scheduler will not repeatedly run heavy CI for an unchanged attempted SHA.
Such a tick, like one with no new changes, captures nothing. Its **Mark run as
not validated** job then cancels the run, so the run list shows it as cancelled
rather than green. It carries a **No promotion this run** notice and job summary
naming the reason; only **Promote validated candidate** succeeding means main advanced.
A run that could not be cancelled fails instead; neither result is a passing batch.
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
legacy main PRs keep their earlier fast tier, and legacy queue/batch runs select
`full`. Standalone scheduled native runs select `nightly`, including advisory
analysis. Manual native runs default to `nightly`; choose `validation_tier=full`
when dispatching `ci.yml` to reproduce the batch's required native qualification
without the separate advisory nightly reports. This choice affects only
standalone manual native runs; PR and batch routing ignore it. For qualification,
leave `mcmc_vulkan_parity_only=false`; that separate diagnostic option runs only
its named parity lane. Scheduled and
manually dispatched batches always run full native validation, including a
documentation-only batch.

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
A same-commit duplicate therefore waits and runs completely rather than leaving a
blocked check. A later push does not cancel such a run for an earlier commit.

An `edited` (other than retargeting) or `ready_for_review` run of `ci` does not
repeat the native compile and analysis. Editing re-runs the workflow so that
**Develop health** re-reads `Batch-Fix:` declarations; the commit and base are
unchanged. **Classify changes** reads this workflow's runs with a read-only
`actions` token. It selects the newest earlier, uncancelled pull-request run for
the same PR, base branch and head commit, which may still be in progress, and
reports it as `validated_by`. Every native job then skips, and the `Ubuntu Clang`
gate waits for that run. It passes only when that run's `Ubuntu Clang` gate passed
and every other job except **Develop health** succeeded or was skipped. **Develop
health** runs again for the new description. A cancelled, unreadable or
unfinished (after 300 minutes) owner fails the gate; re-run the owner, then this
run. If the lookup fails or no earlier run exists, the run compiles as usual.
Retargeting, pushes, reopening and documentation-only changes never reuse a run.
Before this rule, one push plus a description edit cost two 30-minute compiles.

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

The long ARM jobs run in order: sanitizers, native/SDK acceptance, then the nightly
analyzer. This limits simultaneous runner demand after hosted ARM capacity left
parallel jobs unacquired until GitHub cancelled them. Later jobs use an explicit
status condition so a test failure does not suppress independent evidence;
cancelling the workflow still stops the chain. All required results remain part
of the promotion gate. The batch also admits ARM-owning workflows in order:
native, threading, installed host tooling, then macOS documentation tools. This
prevents short ARM jobs from expiring behind native work in a different reusable
workflow. The Linux wiki call starts at candidate capture alongside the browser,
Windows and recovery workflows. The Linux call builds and verifies the public API
and the browser editor, checks sample playback and uploads the Pages artifact.
Promotion waits for both wiki calls, and only the post-promotion job deploys.
The ARM sequence can increase total batch duration. A platform-wide shortage
can still require retrying a job whose annotations show that it failed to acquire
a runner.

Native viewport scheduling acceptance requires presentation and camera input to
advance in each of three consecutive one-second windows while input stays active.
An initial idle window confirms that the native viewport is presenting. A separate
regression verifies that repeated start requests preserve the active service
timer's identity and deadline. The screen's reported refresh rate is approximate,
and shared-runner load and sanitizer overhead affect input and idle work
differently. See Qt's
[screen refresh contract](https://doc.qt.io/qt-6/qscreen.html#refreshRate-prop) and
[timer accuracy contract](https://doc.qt.io/qt-6/qtimer.html#accuracy-and-timer-resolution).
Display throughput follows the separate
[runtime timing policy](runtime-profiling.md#toward-a-timing-gate).

### Threading platform lanes

`.github/workflows/threading-platforms.yml` runs the Windows threading build and the
macOS ThreadSanitizer run of `Ludus::FoundationThreading`
([threading milestone T3](../architecture/threading.md#milestones)). Every batch
calls it, and promotion requires it. Integration PRs run it only when they change
the threading module or the workflow itself. The Windows Conan profile and the
Windows FoundationBase diagnostics backend also build in the
[Direct3D 12 lane](#windows-direct3d-12-lane), so changes to them reach Windows in
the batch.

The Windows job installs the upstream LLVM 23.1.3 installer on `windows-2022`, after
checking it against its pinned SHA-256 digest (Chocolatey does not package LLVM 23
yet), and drives `clang++` against the runner's Microsoft STL and dynamic CRT. It installs the
pinned CMake/Ninja/Conan versions, exports the local Catch2 recipe and installs
only that locked package with `config/conan/profiles/windows-clang-x86_64`. Ludus
configures the FoundationThreading closure on Windows (Base, Memory, Containers,
Hash, Strings, Time, Profiling and Threading) plus the
[Direct3D 12 closure](../architecture/windows-direct3d12.md#build-and-test); this
job builds only the threading targets, and no SDK is installed. The job runs the
threading, startup-seam, profiling-adapter and allocation suites once, then
repeats the scheduler and startup suites 20 times.

The macOS job prepares `macos-clang-development` without the editor or shader
tools, proves TSan detection with the sanitizer control, then runs the threading,
startup-seam and profiling-adapter suites under TSan, repeating the first two.
The pinned LLVM 23 TSan runtime also starts on local macOS 26 hosts, which LLVM
before 22.1.3 could not; see [macOS sanitizer runtimes](building.md#macos-sanitizer-runtimes).

### Windows Direct3D 12 lane

`.github/workflows/windows-direct3d12.yml` builds and runs the
[Windows Direct3D 12 bring-up](../architecture/windows-direct3d12.md): the Win32
Platform backend and the Direct3D 12 RHI backend. Every batch calls it, and
promotion requires it. Like the other native platform suites, it does not run
on integration pull requests, including Windows port changes: its MSVC Ninja,
MSBuild and Runtime SDK lanes take 15–30 minutes per push. The Runtime SDK lane
installs and consumes the Development runtime SDK, and the MSBuild lane also runs
the project IDE journeys. The batch qualifies the combined
`develop` changes before `main` advances. A Windows compile or runtime regression
can therefore enter `develop`; the failed batch rejects it, and a focused fix or
revert follows the [recovery](#recover-a-failed-batch) process. When a Windows
change needs evidence before acceptance, dispatch the workflow manually on its
task branch:

```bash
gh workflow run windows-direct3d12.yml --ref <task-branch>
```

A manual dispatch also runs the Windows editor lane. For targeted validation,
pass `-f editor_only=true` to run the editor, hosted Play and supervisor suites
on WARP by themselves.

A dispatched run checks out the branch head and never cancels an in-progress run
of the same branch. Its result is advisory evidence for review, not promotion.
`scripts/python/test_batch_ci.py` rejects a pull-request, push or merge-group
trigger on this workflow.

The job uses the same pinned LLVM installer, host tools and locked Catch2 package
as the Windows threading job; `scripts/python/test_ci.py` checks that both lanes
share one installer pin and that the Windows module list is closed over its
links. It bootstraps the pinned Slang and DXC with `python scripts/shader-probe
bootstrap`, builds Platform, the RHI and their tests warning-clean (compiling the
fullscreen diagnostic shader to signed DXIL), then runs
`ludus_platform_win32_tests`, `ludus_rhi_d3d12_tests` and
`ludus_rhi_lifecycle_tests`, and fails if the DXIL pixel tests were not
configured. The `windows-2022` image has no GPU, so the
Direct3D 12 tests run on WARP; they establish the backend's behavior, not
hardware-adapter qualification.

The D0 matrix uses MSVC 14.44 and Windows SDK 10.0.26100.0. It runs the
[primitive probe and the existing Win32/RHI source build](../architecture/windows-direct3d12.md#d0-win32-and-rhi-source-build)
with Ninja and Visual Studio/MSBuild. Ninja uses separate Debug, Development,
Profile and Release build trees. MSBuild builds Debug, Development, Profile and Release in one
solution before CTest runs any configuration, then archives a test log for each.
Both generators use compiler-native locked Catch2 and signed DXIL. The D3D12 suite checks the reported software adapter, and the
workflow checks that both the fullscreen and transfer cases were compiled.
The D5a additions run the FoundationBase, memory and containers (including
allocation failure), time, threading, logging, profiling, identity, filesystem,
entropy and input suites in each MSVC configuration. The Windows assertion
probe inspects the terminal diagnostic packet, confirms that report transport
is unavailable and checks that normal exit handlers were bypassed. The filesystem process
probe terminates the writer before and after atomic publication on the runner's
filesystem, then checks the accepted bytes and retries. This exercises process
interruption separately from storage power loss.
The MSVC ASan job selects `windows-msvc-asan`, probes the sanitizer runtime
before building, then exercises Foundation, Win32, RHI and the source runtime
suite, including renderer, content, text, worker, GameHost and Behavior. Its positive
control requires both a clean startup and a specific fatal heap-buffer-overflow
finding. The job archives the probe, control logs and exact CTest results.
Windows quality subprocesses share the editor's suspended-process admission and
kill-on-close Job owner. Their tests verify descendant cleanup, output bounds
and timeout behavior.

The Ninja runtime job checks out the candidate and runs the D7 source suite
on WARP. It checks the exact CTest inventory, fails if a required test is skipped,
and archives the candidate, Windows build, toolchain, inventory and JUnit results
under `out/windows-qualification`. The suite includes the DXIL compiler and
named-pipe [worker protocol](render-graph-authoring.md#windows-worker).
Separate runtime SDK jobs build Debug, Development, Profile and Release with
Ninja, relocate each prefix, then build and run the same external consumer with
both generators. The Development SDK also runs the project create, repair and
launch journey. See the [D7 record](../architecture/windows-direct3d12.md#d7-qualify-the-windows-profiles)
for physical and desktop acceptance. The required Editor and Editor ASan matrix
entries each run
the core and graph-authoring shards and the code/content/layout/geometry suites,
then the native authoring, fractional-DPI and hosted Play tests. A manual
`editor_only` dispatch targets both entries during editor-port iteration.
The `windows-msvc-Ninja` and `windows-msvc-MSBuild` artifacts record the build
configuration and test logs. The historical Clang/WARP job still runs. The MSBuild job also installs all
four SDKs from the solution, relocates their prefixes and configures a separate
consumer against each one. The consumer compiles signed DXIL with the installed
shader helper and presents on WARP. A separate check rejects installing Release
over the Debug prefix.


## Dependencies and caches

CI uses `init.sh --locked` to consume the committed `conan.lock` without
regenerating it or permitting dependencies outside the lock. Update dependencies
through normal initialization and review the resulting lockfile separately.
A missing or incompatible lock fails CI rather than silently resolving new
versions.

Conan cache keys include the preset, the local recipes (`third_party/*/conanfile.py`)
and the support files the Catch2 recipe exports, lockfile, profile, tool versions,
and presets. They omit vendored FetchContent sources under `third_party`, which
are not Conan inputs: hashing them gave every `develop` PR a key the batch on
`main` never saved. Conan's recipe revisions rebuild a package whose exported
files change, so the key only selects a starting cache. Jobs that only restore
fall back to the newest cache of their preset; writers save exact keys. Compiler
cache keys add a commit suffix, which permits refreshed snapshots after restoring
a compatible older cache. Compiler caches are separated by
preset and configuration (ordinary, PCH, editor). CMake honors
`LUDUS_ENABLE_CCACHE=1` from the environment for native builds; browser presets
keep their separate cache policy. The native setup action installs ccache.

The repository cache is limited to 10 GB, and GitHub evicts the least recently
used entries once it is full. Every batch saves a new commit-keyed compiler cache
for each variant, so without cleanup the superseded copies evicted the Conan,
sanitizer and browser caches the next batch needed. The batch's **Prune
superseded caches** job runs after every suite has finished, whatever the outcome,
from the trusted `main` revision with only `actions: write`. For each
repository-owned key family on `main` (`ccache-`, `conan-`, `editor-pages-`,
`editor-tools-`, `web-tools-`, `icu-probe-`, `pip-`), it keeps the newest entry and
deletes the rest. A family is the key without its trailing input and commit
hashes, so entries left behind by a toolchain or lockfile change are removed too.
Caches on other refs and action-managed caches (`setup-python-`, `node-cache-`)
are left alone. Promotion does not depend on this job; a deletion that fails is
reported as a warning, and the next batch retries it.

### Develop compiler cache

`main` moves only when a batch passes, so the batch's caches can trail `develop`
by many changes; pull-request Editor compiles took 27-38 minutes with `main` 60
commits behind. `.github/workflows/develop-cache.yml` runs on every push to
`develop` except documentation-only pushes, and on manual dispatch. Its `editor`
job prepares, configures and builds exactly as the pull-request **Editor, shader
worker and unit tests** job does, with the same preset, cache variant, configure
flags and build command. It then saves the compiler cache and, when no exact
match exists, the `linux-clang-development` Conan cache in `develop`'s scope. A
regression test keeps the two jobs' compile inputs identical.

A pull request may restore caches from its base branch, so pull requests targeting
`develop` start from the newest `develop` commit's objects and recompile little
beyond their own change. The batch, which runs on `main`, cannot read `develop`'s
caches and keeps its own.

Runs are serialized without cancelling: a newer push replaces a pending run, and
a running one always finishes and saves. A compile failure on `develop` is reported
as a warning and still saves the objects that compiled, since the next pull
request, often the fix, reuses them. A failure before the build (for example in
the native setup's initial Editor build) saves nothing for that commit. The
**Prune superseded develop caches** job then keeps the newest entry of each family
on `develop`, with the same rules and `actions: write` scope as the batch's
pruning job, so `develop` holds about one Editor compiler cache (about 830 MB) and
one Conan cache. The workflow validates nothing and reports no required check.

## Analysis and timing

`LUDUS_TIDY_JOBS` sets the maximum number of concurrent native clang-tidy
processes. The local default is one; CI uses four on Linux and three on macOS. Diagnostics are saved per
compile command and printed in source/variant order.
Workflow steps that analyze an explicit source list, such as the Editor
production and regression sources on Linux and macOS, run
`./scripts/check <preset> --tidy-sources <source>...` instead of one serial
`clang-tidy` command line. Each listed source keeps its ordinary
`compile_commands.json` lookup and the repository `.clang-tidy` policy, so the
diagnostics match the direct invocation; the sources only run
`LUDUS_TIDY_JOBS` at a time under the same bounded execution and evidence
directory as the inventory check. A listed source the preset does not compile,
a repeated source or a missing database fails before analysis starts.
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

### GPU correctness test boundaries

Ordinary raster checks keep their short CTest boundary. GPU reference-compute,
render-graph lifecycle/capture and template cases are registered individually
through Catch2 discovery before CTest runs. Their names start with
`ludus_renderer_reference:`, `ludus_render_graph_ray:` and `ludus_render_graph_template:`;
renderer/GPU/ray labels and explicit unsupported-adapter skips remain available.
Each reference-compute or graph lifecycle/capture case has a 300-second deadline
(600 seconds with ASan/UBSan); each template case has a
600-second deadline for its convergence captures. Each raw-AOV camera, lighting
and optical fixture is also an independent case, preserving its samples, seed,
CPU oracle and numerical assertions. Polygon sampling, Metropolis
and physical-optics parity also have separate named tests. Metropolis parity has
a 300-second case deadline (600 seconds with ASan/UBSan), allowing its
material/estimator variants to share the cold shader setup. Its readiness helper yields between polls and allows
180 seconds for cold shader/pipeline preparation and the first submission
(300 seconds with ASan/UBSan); buffer readiness and readback retain their
30-second bounds. Failed readiness
reports the operation, status, elapsed time and deadline, and retires the RHI
even when a test assertion aborts the case. The numerical and
image assertions are unchanged by this partition.

The captured `native-setup` action sets `GALLIVM_PERF=nopt` for the Linux
Development/SDK, sanitizer and Editor job/preset pairs. This placement also
repairs batches using older trusted workflow definitions on `main`: the action
comes from the captured candidate and needs no new caller input. Other jobs and
presets retain their driver environment. The action's real shell regression
checks all admitted pairs and excludes profiling, release and other platforms.
Thanks to the Mesa authors,
[Gallivm's optimization flags and pass manager](https://chromium.googlesource.com/chromiumos/third_party/mesa/+/refs/heads/stabilize-13982.70.B-chromeos-amd/src/gallium/auxiliary/gallivm/lp_bld_init.c)
provide this mode to reduce shader compilation time. The default LLVM
SROA/memory-promotion work can consume the entire test deadline before the first
reference sample. The CI profile retains actual Vulkan shader execution,
validation layers, sample counts and independent CPU/GPU comparisons, while
bounding driver optimization work. Engine shader artifacts and driver defaults
are unchanged; performance qualification uses its own normal driver profile.
The unchanged reference artifact took 111 seconds in a fresh-cache raw-AOV run
with this profile. Its readiness window has 180 seconds of cold allowance
(300 with ASan/UBSan), leaving room for capture within the existing case deadline.

These correctness deadlines include shader preparation on software adapters.
Independent deadlines keep growth in the number of reference captures from
consuming the raster test's deadline. `--durations yes` identifies completed
cases when a deadline fails. GPU math/transport oracle cases use the
`[reference-compute]` tag to receive independent discovery, including wave kernel
parity. Wave template cases are discovered separately with the existing
600-second template deadline; the supplied-scene manual fixture stays hidden.
Their single-sample captures yield while polling and give initialization and
first capture separate cold windows. Each authored scene wave scenario has its
own case; its four-pass graph allows 300 seconds for cold preparation, including
lavapipe LLVM compilation observed beyond 120 seconds. This does not change
sample counts, geometry or pixel assertions. Render-graph capture tests allow 120 seconds for the
first submitted ray sample after initialization or a scene/camera reset, including
first-use software shader work. ASan/UBSan cases allow 300 seconds for that cold
work, including driver allocation interception; subsequent samples retain the
10-second completion bound. A private frame capture allows 180 seconds including
graph preparation and readback. The interactive graph preview allows 120 seconds
for the first sample while keeping its last presented frame.

Raw-reference capture tests yield between advances and distinguish preparation
and first committed samples from the remaining capture. Their cold window uses
the same ordinary/instrumented allowance; after any samples commit, the complete
remaining capture retains its 60-second window. Failures include sample coverage
and elapsed time. Ordinary raster cases retain 60 seconds, with 180 seconds under
ASan/UBSan. No sampling count, estimator, tolerance or unsupported-adapter rule
changes with these scheduling allowances. Performance qualification remains in
its own lane.

The offscreen editor suite runs as four serial Catch2 shards and the canvas
configuration suite as two. Declaration order gives each case exactly one shard;
the existing 600- and 180-second deadlines apply per shard. CTest resource locking
keeps those GPU and shader-worker owners serial even under a parallel CTest run.

The Linux ASan/UBSan job is a four-way matrix, **ASan and UBSan (1/4)** to
**(4/4)**. Serially, its CTest suite took 95-100 minutes of a two-hour job, 83% of
it in render-graph, renderer and wave-graph cases on lavapipe. Each shard builds
the same sanitized tree (one shard writes the Conan and compiler caches) and runs
every fourth test with `ctest --tests-information <shard>,,4`, so new tests join
the rotation without a list to maintain. Within a shard, tests still run one at a
time: each case keeps a whole runner, so the cold software-shader deadlines above
see the same CPU as before. Running cases concurrently on one runner would split
lavapipe's CPU between them instead. With the measured durations, the shards take
about 20, 21, 31 and 28 minutes of tests.

Shard 1 also uploads the sanitizer-control evidence and runs the S0-S7 scripting,
L0 ICU and L2 localization sweeps, and `ludus_game_host_reload_tests` with
`LUDUS_RELOAD_METRICS`. The sharded run excludes that test, so the required reload
evidence always comes from shard 1. All shards run the sanitizer controls before
their tests, and the Ubuntu Clang gate requires every shard. Tests that assert
wall-clock bounds or record timing evidence remain `RUN_SERIAL`, and tests that
share a device or worker owner use `RESOURCE_LOCK`, as the Editor shards do, so a
later parallel CTest run keeps their results stable. The scheduled batch runs the
workflow definitions on `main`, so this takes effect after the first promotion
that includes it.

#### Compare one shared sanitizer build with four independent builds

Dispatch both experiment modes on the same branch commit. Each runs the same four
test shards, sanitizer controls and shard 1 evidence steps. The `shared` run first
builds the preset on one runner, then restores that run's build tree and test
inputs on four fresh runners with the same workspace path. The tar preserves
symlinks, modes and source timestamps for CTest and for shard 1's SDK and probe
steps. The runner installs the system prerequisites before using the bundle.

```bash
gh workflow run ci.yml --ref <branch> -f sanitizer_experiment=rebuild
gh workflow run ci.yml --ref <branch> -f sanitizer_experiment=shared
```

Compare elapsed time from the first job start to the last shard completion and
the sum of job runtimes. Record the archive size, archive/upload and
download/extract times, and whether the producer and baseline jobs hit their
Conan and compiler caches. Repeat with comparable cache states before adopting the
shared build. Both experiments are manual. Pull requests and promotion continue
to use the existing four independent builds.

In the November 11, 2026 warm-cache comparison, the shared build reduced runner
time before the sanitizer controls from 31m 37s to 15m 25s. The full run took
58m 53s and used 132m 29s of runner time, compared with 34m 37s and 124m 57s for
independent builds. The shared job serialized the initial build and shard 1 spent
more time on its subsequent probe builds and CTest cases.
[Independent builds](https://github.com/The-Ludus-Project/Ludus/actions/runs/43127809603)
· [Shared build](https://github.com/The-Ludus-Project/Ludus/actions/runs/43127813584)

The `ludus_editor_tests` build entry point also builds every executable target
ending in `_tests` in the editor's own CMake directory, including optional preview
suites. A real-CMake contract regression checks optional suites enabled and
disabled, and excludes helper objects, benchmarks and other directories. This
keeps older trusted batch workflows complete when a new editor suite is added.

The mandatory Vulkan pixel proof selects `[render-graph]` and rejects skipped
cases. Portable bilinear upscaling stays in that set; optional MetalFX variants
use `[render-graph-metalfx]` and remain in the full GPU and macOS suites.

## Scheduled runtime profiling

The separate `performance` workflow runs runtime profiling scenarios nightly on
hosted Ubuntu and macOS runners, and on manual dispatch. It is not a pull-request
or batch check: scenario correctness failures fail that workflow, and
timings are report-only. Reports, Perfetto traces and logs are kept as artifacts
for 90 days. See [Runtime profiling](runtime-profiling.md). The hosted macOS
lane also runs the RHI, renderer and native editor viewport GPU scenarios on its paravirtual Metal GPU,
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
The `ludus_editor_tests` build target also prepares the split editor test executables
registered alongside it. This keeps the sanitizer job's `editor` label runnable
when a captured candidate introduces a new test executable before its workflow
definition reaches `main`. Keep that dependency closure current when splitting suites.

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
profile. The budget, Linux editor and macOS native jobs also run the
[build graph check](build-profiling.md#build-graph-check), which counts
compilations instead of timing them and fails when several targets compile the
same source. Compare cold and warm runs across several commits when evaluating
changes; parallel jobs can lower completion time while increasing runner minutes.
The browser workflow continues to test the exact packaged Release ZIP and
cancels superseded pull-request runs independently. Renderer browser coverage runs
in separate baseline (L0–L4) and scaling (L5) jobs, each with a ten-minute
budget and its own evidence artifact. All image oracles and backend cases remain
required; the packaged browser lane retains RHI, camera and scripting coverage.
