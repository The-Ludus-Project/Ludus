# Engineering quality and CI architecture

Status: Q0 implemented; Q1 implemented in advisory rollout; Q2–Q4 proposed, October 11, 2026. Q0 implementation
baseline: `f9cc7c8`. The initial proposal used `01f5ecf` plus its working tree.
Q0 failure controls/inventory and Q1 analysis/editor tooling are implemented; later checks,
presets, reports and services remain an implementation plan. The
[CI guide](../development/continuous-integration.md) owns current behavior.
The initial decisions were recorded before the
[reference review](engineering-quality-gems-review.md). Sections 5, 6 and 8 now
incorporate its scenario coverage, reproducibility, profiling and failure-context
improvements. The [implemented Q0 contracts](../development/continuous-integration.md#sanitizer-failure-controls)
cover fatal UBSan, isolated sanitizer controls and bounded analysis inventory.
No performance improvement is claimed.

## 1. Objectives and boundaries

Produce trustworthy changes with a short feedback loop and a small system that
contributors can understand and reproduce. Optimize developer waiting time,
runner minutes, actionable findings and time to reproduce a failure together.
Tool count, a clean dashboard and a coverage percentage are not quality goals.

Retain C++23, Clang/LLVM 23, no engine exceptions, explicit errors, dependency
direction, public API documentation, lightweight headers and the reference
Ubuntu 24.04 toolchain. Native macOS and pinned Emscripten/browser toolchains
remain distinct capabilities. PowerShell wrappers do not imply Windows support.
No tool dependency, report transport or profiler UI enters FoundationBase or
the installed runtime SDK. Use existing CLI/project operation backends rather
than creating a second editor or CI build system.

The dependency flow is deliberately small:

```text
CMake presets/targets + toolchain pins + Conan lock + quality inventory
                              |
                 existing setup/check/test/profile scripts
                              |
                CLI / VS Code tasks / Editor / CI jobs
                              |
                 bounded local evidence + required gate
```

Scripts consume the recorded configuration; CLI/editor/CI entry points invoke
those scripts. CI schedules commands and does not own a second compiler
configuration. The evidence writer observes results and cannot convert a failed
command to success.

Design constraints:

- One implementation of each check, invoked locally, by CI and by editor tasks.
- Required checks run on the complete applicable source set. A changed-file
  local shortcut cannot substitute for the merge check.
- Failure, skip, unavailable and incomplete evidence are distinct outcomes.
- A contributor can identify the failing command and reproduce its inputs
  without a hosted dashboard or an undocumented machine configuration.
- Keep workflow YAML explicit. Use small Python helpers for tool orchestration
  and reporting; avoid a general workflow DSL, plugin framework or scheduler.
- Add a checker only with an owner, supported version, measurable benefit,
  triage policy, local command, runtime cost and removal condition.

## 2. Baseline and gaps

The current CI already enforces formatting, compiler warnings, no exceptions,
clang-tidy, ASan/UBSan, focused TSan tests, pack fuzzing, SDK relocation, PCH and
non-PCH builds, public-header self-sufficiency and include boundaries, browser
acceptance, documentation checks and ClangBuildAnalyzer budgets. Preserve these
gates during migration. Their existence does not establish coverage of every
backend or lifetime path.

Observed gaps at the initial proposal baseline (the CI guide records Q0 fixes):

| Observation | Proposed correction |
| --- | --- |
| `.clang-tidy` disables defaults without enabling `clang-analyzer-*` | Add selected stable path-sensitive analyzer families, after a baseline run |
| Main tidy scope is `modules/` and `apps/` | Inventory all first-party compiled C/C++/Objective-C++ in modules, apps, tools and tests |
| ASan/UBSan flags do not establish fatal UBSan recovery policy | Make recoverable UB exit unsuccessfully and prove it with a negative control |
| Coverage option uses `--coverage`; no coverage-report job is present | Add a distinct Clang source-coverage profile and HTML/LCOV reporting |
| TSan is focused on threading and filesystem; CI fuzzing on pack decoding | Expand by concurrency and byte-input boundaries, not by module count |
| VS Code clangd uses a fixed Linux compilation-database directory | Bind the language server to the selected, prepared native preset |
| Logs/artifacts are preserved by some jobs | Establish a consistent bounded evidence bundle for every validation lane |
| Build tools are recorded as pins/minimums and native SDK targets differ | Record actual tool/runtime/backend identity and reject unsupported execution |

## 3. Ownership and the source of truth

Platform/tooling maintainers own runner images, setup, caches, workflow routing,
report format and reproducibility. Module maintainers own tests, fuzz harnesses,
concurrency contracts and benchmark scenarios. A check's rollout change names
its accountable owner; do not create a permanent triage queue without one.

Keep `CMakePresets.json` and CMake targets authoritative for configure/build/test
configuration; use the CMake File API and compilation database for target/source
discovery. Existing toolchain files and Conan locks remain authoritative pins.
Extend `config/tool_versions.json` for additional command-line tool pins; record
the resolved executable version and digest in each run. Native reference LLVM
must be major 18; patch upgrades are reviewed and measured. Do not treat a newer
clang-tidy or extension-bundled clangd as equivalent to the reference checker.

Introduce one small `config/quality.json` only for the shared validation
inventory: schema version, lane/check IDs, required job IDs, supported host and
target capabilities, and report limits. It has no executable shell expressions,
machine paths or copied CMake flags. A strict loader rejects unknown IDs and
contradictory requirements. Existing `ci_scope.py` consumes the inventory and a
contract test compares it with workflow jobs, so the required-gate list cannot
silently drift. Do not generate entire workflow files from this manifest.

Reusable setup belongs in the current native-setup action. Reporting is a small
shared Python module with tool-specific parsers. Commands run as argument
vectors, forward output, propagate failure and handle cancellation/child cleanup.
Add functions to existing scripts rather than a new all-purpose CI executor.

## 4. Validation lanes and scheduling

| Lane | Trigger | Required evidence | Blocking policy |
| --- | --- | --- | --- |
| Fast source checks | Every PR, main and manual run | Routing tests, applicable format/include rules, tooling tests, Ruff/actionlint, docs checks | Required for its applicable scope |
| Native correctness | Code/tooling PRs, main and manual | Existing Linux flavors, full tests, SDK consumer/relocation, stable analyzer checks, ASan/UBSan, focused TSan, short seeded fuzzing | Required; preserve present gates |
| Target correctness | Applicable native/browser changes; conservative full fallback | Existing macOS/backend tests and exact packaged browser acceptance | Required for supported capabilities |
| Coverage and extended diagnostics | Short coverage on code PRs; longer runs scheduled/main | Coverage, expanded race scenarios, bounded Valgrind and fuzzing | Reports first; promote stable correctness checks after triage |
| Build cost | Existing code PR/main budget lane | Clean uncached frontend/header budgets and detailed traces | Preserve current required budgets |
| Hardware/performance | Scheduled and explicit runs on controlled devices | Backend/device tests, frame/audio budgets, benchmark repetitions | Report until calibrated; release candidate acceptance for declared devices |
| Release acceptance | Candidate/tag | Locked build, relocated installed SDK, package verification, symbols, smoke journeys and notices | Required before publication |

The hardware/performance lane is implemented as the nightly `performance`
workflow ([Runtime profiling](../development/runtime-profiling.md)). It records
report-only timings and traces on GitHub-hosted runners and on opt-in
self-hosted device lanes. The hosted macOS lane runs the RHI and renderer GPU
scenarios on a declared paravirtual GPU for nightly correctness, not hardware
timing. Device lanes declare a physical GPU class and identity and admit only
scheduled or dispatched `main` runs through a runner-side hook. Base/head repetitions and calibrated timing
gates remain proposed; the guide describes the calibration that comes first.

Documentation-only exemptions remain the existing conservative allowlist. Unknown
paths, malformed comparisons, policy/toolchain changes and mixed changes run full
validation. Preserve deleted/renamed source paths in classification. Do not add
module-level change filtering until the dependency closure, public headers,
generated inputs and SDK tests are demonstrably covered by routing regressions.
Local changed-file checks are an explicit convenience only.

Stable required status names remain available on every event. The final gate
requires successful classification and exactly the expected outcomes; unexpected
skip, cancellation, missing report or empty required suite fails. A legitimate
docs-only skip is recorded with its reason. Scheduled failures create a visible
failure and an owned investigation; they cannot silently turn the PR gate green.
If merge queues are adopted, support and test `merge_group` before enabling them.

Separate configure, build, test, analysis and report steps to retain readable
logs. `fail-fast: false` collects independent findings; cancel superseded PR runs
but let main/release runs finish. Set per-job and per-test timeouts. Retry only
identified transient infrastructure steps, never a correctness failure merely to
obtain a green result. Record first failures and all attempts when investigating
flakiness. Quarantine needs a narrow test ID, owner, issue and expiry; required
acceptance journeys cannot be replaced by permanent quarantine.

## 5. Correctness tools and rollout

### Static analysis

Use the existing pinned clang-tidy and compilation database as the default.
Start with `clang-analyzer-core.*`, `clang-analyzer-cplusplus.*` and
`clang-analyzer-unix.*`, alongside existing rules. Exclude experimental checkers.
Add individual concurrency/CERT checks only after reviewing relevance and noise;
avoid enabling every coding-guideline family against the engine's explicit-error
and ownership conventions. Compiler errors and malformed inputs always fail.

Discover source entries from all first-party roots, including `.c`, `.m` and
`.mm` where compiled. Deduplicate identical compile commands, but analyze distinct
configuration/define variants when their contracts differ. Vendor/generated
sources are excluded by declared ownership, not by broad filename patterns.
Report expected and analyzed translation units; a required nonempty scope with
zero analyzed units or an unexplained omission fails. Build generated inputs
before analysis. Keep deterministic sharding and bounded process concurrency.

Baseline findings are reviewed, fixed or narrowly suppressed with an explanation
and owner. No repository-wide suppression and no auto-baseline regeneration.
Promotion requires a full supported-platform scan and manageable false-positive
rate. CodeChecker is an optional report adapter if HTML paths/differential
triage justify maintenance; it is not a second mandatory analyzer. CodeQL may
provide a later security-focused lane with explicit build extraction and
availability/licensing checks. Cppcheck/commercial tools require evidence of
additional actionable defects before becoming permanent lanes.

### Dynamic analysis

Build the executed code with the selected sanitizer, including testable private
implementation copies and relevant in-tree dependencies. ASan/UBSan remains the
PR default. Enforce `-fno-sanitize-recover=undefined` or an equivalent tested
runtime policy; set symbolizer discovery explicitly. Check negative controls in
isolated expected-failure processes: known bounds/UB/race defects must fail with
the expected diagnostic. An expected-failure child must not become a normal
passing sanitizer finding. Do not count sanitizer startup failure as clean.

TSan stays a separate build tree, incompatible with ASan. Expand workloads to
logging publication/shutdown, trace capture, audio worker ownership and host
reload/retirement as supported. Stress barriers, cancellation, allocation
failure, shutdown and repeated construction, with reproducible seeds. TSan
does not prove deadlock freedom or timing deadlines; add bounded progress tests.

Use Valgrind Memcheck for selected headless unsanitized Linux Debug executables,
initially scheduled. Enable origins and definite/indirect leak failures, set an
error exit code and preserve XML/text reports. Use narrow reviewed suppressions
for external libraries; do not hide all reachable/leaked allocations globally.
Bound workload and runtime; driver/GPU tests need separate validation. Custom
suballocators need appropriate Memcheck/ASan annotations before claiming
per-allocation coverage. MemorySanitizer is deferred until an instrumented
dependency toolchain can be maintained; an uninstrumented dependency is not a
clean uninitialized-read result.

### Fuzzing, properties and coverage

Prioritize byte-input boundaries: parsers, codecs, network packets, content
headers and migration/configuration payloads. Harnesses call production code
with sanitizer and coverage instrumentation; sanitizing only the harness is
insufficient. Make inputs bounded, deterministic and headless. Check semantic
properties such as round-trip behavior, unchanged output on failure and bounded
work; avoid a duplicated implementation used as its own oracle.

Run short seeded campaigns on PRs and longer budgeted campaigns on a schedule.
Keep small reviewed seed corpora in tests; store grown corpora in CI artifacts.
Always replay known regressions in ordinary tests. Preserve input, seed,
executable identity, sanitizer config and minimized reproducer. Minimize against
the same failure signature with a time limit; a failed minimization still saves
the original. Harnesses may have Catch2-style test exceptions; engine code
retains its exception-free build boundary.

Add Clang source coverage with `-fprofile-instr-generate -fcoverage-mapping` in
a distinct profile, never by feeding current gcov `--coverage` output into
`llvm-profdata`. Use unique `%p`/`%m` raw-profile names for concurrent processes;
merge profiles and all participating binaries with matching LLVM tools. Report
source, branch and template coverage by module; exclude only declared vendor,
generated and intentionally unexecuted platform sources. Missing profiles,
failed tests and incompatible binaries are errors, not zero or full coverage.
Publish HTML and LCOV; begin advisory, then use meaningful changed-code/branch
requirements for mature tested modules with a documented exception mechanism.
Coverage measures execution, not correctness. No arbitrary global 90% target.

### Scenario coverage and reproducibility

The [benchmark scene roadmap](../development/benchmark-scenes.md) records
reference scene candidates, suite coverage, initial priorities and the workflow
for adding reproducible Ludus cases. Its candidate status does not enable new CI
gates or establish implementation/performance evidence.

Thanks to Matthew Jack, *Code Coverage for QA*, GPG8 section 4.8, for linking
high-level testing to named expected execution points. Add scenario evidence
beside compiler coverage; see the
[review and departures](engineering-quality-gems-review.md#code-coverage-for-qa).
Reuse existing test/host observations and IDs before introducing runtime markers.
Each owned scenario declares its input/content version, applicable capabilities,
expected checkpoints and observable invariants. The result records expected,
observed, missing and unexpected IDs, independent of test-framework exit status.
An event says a path was reached; the invariant proves its required outcome.

| Scenario | Example required evidence |
| --- | --- |
| Project setup/repair | Presets selectable; correct SDK admitted; stale cache repaired; custom settings preserved; real consumer executes |
| Gameplay reload | Candidate staged; old generation quiesced; new state committed or rejected; retired code/resources released |
| Content persistence | Candidate write completes; atomic publication occurs; failure preserves previous readable revision |
| Audio lifetime | Voice completes/cancels; stream worker retires; callback stays within the supported allocation/progress contract |
| Rendering/package | Declared backend selected; expected pixels/frame produced; executed bytes match the accepted package |

Expectations are reviewed source inputs, never automatically regenerated from
the latest successful observation. Unexpected checkpoints require review; missing
required checkpoints or failed invariants fail the scenario. Merge evidence only
for identical executable/content/scenario identities and report per-run as well
as combined coverage. Partial runs and crashed sessions remain incomplete.
Named checkpoint sets are small and stable across refactoring; no global marker
on every function and no speculative runtime coverage service.

Thanks to Steve Rabin, *The Science of Debugging Games*, GPG4 section 1.1, for
the reproduce/investigate/repair/verify discipline. A seed alone is insufficient:
record initial state, configuration/content digests, input sequence, tick/time
policy and relevant dependency versions. Concurrent/native device work may not
replay identically; record scheduling/device context and failure frequency rather
than promising deterministic replay. Preserve the first failure before retries.
Fixes replay the original case and add adjacent boundary/lifetime cases; bisect
or minimize from a known-good revision before changing unrelated systems.
See the [review](engineering-quality-gems-review.md#the-science-of-debugging-games).

The modern instrumentation mechanics follow LLVM's
[source coverage documentation](https://clang.llvm.org/docs/SourceBasedCodeCoverage.html).
Sanitizer failure semantics follow LLVM's
[UBSan contract](https://clang.llvm.org/docs/UndefinedBehaviorSanitizer.html).

Ruff and actionlint cover host tooling/workflows; introduce selected correctness
rules before formatting churn. ShellCheck can support shell scripts and workflow
steps where applicable. Keep each version pinned and its local invocation simple.

## 6. Headers, build cost and runtime performance

Keep header self-sufficiency probes free of PCH, the foundational include gate,
public/private SDK checks and the current ADR 0005 budgets. Include Cleaner/IWYU
stay advisory; handle umbrella/re-export contracts before removing includes.
An editor suggestion cannot redefine the public include contract.

Build-cost evidence includes toolchain identity, build graph, translation-unit
count, concurrency, cache mode, frontend/backend time, header rankings and
wall time. Compare equivalent clean worktrees at base/head when diagnosing a
regression; never clean a contributor's active build tree for an automatic
editor action. Per-header limits remain blocking; aggregate growth requires
the existing measurement/review process. Track production and test frontend
cost separately as additional diagnostics, without excluding either from the
required complete-graph budget. Reuse `profile-build`/ClangBuildAnalyzer.

Before duplicating full builds across analyzer shards, identify the generated
headers/sources they need. A small CMake analysis-input target can prepare those
dependencies without linking unrelated tests, provided an acceptance test proves
the complete compilation database is analyzable from a clean tree. Keep the
current full-input build until that evidence exists. Add stable CTest capability
and subsystem labels rather than growing handwritten lists of source files and
test executables in YAML. Verify the selected test inventory before execution.

Use FoundationProfiling/Perfetto for frame/scope/flow context; perf/Hotspot on
Linux and Instruments on macOS for sampling and waits, and heaptrack/Instruments
for heap traffic. Keep adapters outside runtime modules. Use optimized Profile
builds with symbols for runtime comparisons; sanitizers and coverage belong in
correctness measurements. RenderDoc covers supported Vulkan/OpenGL targets;
Metal uses Apple tools and backend validation. Software rendering does not
establish GPU timing or real-device correctness.

Module-owned scenarios define representative inputs, warmup, deterministic seed,
iterations, repetitions, hardware/OS/compiler identity and workload size. Report
median and tail distributions plus raw samples. Gate deterministic allocation,
capacity and operation-count contracts on ordinary runners. Timing gates need
controlled hardware, base/head repetitions, a minimum practical effect and a
calibrated variability bound. Never fail on a single noisy hosted-runner sample.
Detect bottlenecks and measure before adding Tracy, continuous capture or a new
allocator/profiler; existing architecture owners retain those contracts.

Thanks to Greg Hjelstrom and Byon Garrabrant, *Real-Time Hierarchical Profiling*,
GPG3 section 1.17, for hierarchical calling context, calls/frame and unaccounted
time. Captures report inclusive/exclusive time, call counts and instrumentation
coverage where meaningful, plus capture window and lost/incomplete events.
Never sum inclusive child totals as exclusive work or sum overlapping worker
durations as frame wall time. Correlate scope events with sampling/wait evidence
to distinguish computation, blocking and uninstrumented work. Use the existing
[profiling architecture](profiling-final.md); the
[review](engineering-quality-gems-review.md#real-time-hierarchical-profiling)
does not propose another producer-side tree.

Thanks to Ricky Lung, *Design and Implementation of an In-Game Memory Profiler*,
GPG8 section 4.6, for allocation-origin attribution and separate live/churn
statistics. Memory scenarios report peak/live requested bytes, allocation/free
counts and retained blocks after an explicitly quiescent lifecycle boundary,
along with measured domain coverage. Cross-thread frees retain allocation-origin
ownership. Process-heap traffic and engine-domain traffic are different measures.
Use existing count tests/external tools first; future accounting must follow
[Memory's architecture](memory-profiling.md). Do not implement runtime function
patching or intrusive allocation prefixes from the historical example. See the
[review](engineering-quality-gems-review.md#design-and-implementation-of-an-in-game-memory-profiler).

## 7. Reproducible platform setup, artifacts and caches

Separate host tools from target SDKs and model capabilities explicitly:
host OS/architecture, native/browser compiler, libc++/libstdc++ identity,
sanitizer availability, display/device requirements and backend. Compile-only,
headless execution, real device and packaged acceptance are different evidence.
Required capabilities missing on a designated runner fail preflight; optional
local probes report unavailable with a remedy. Do not silently switch backend.

Use the existing project setup rules: doctor/loading is read-only; explicit
setup/repair verifies selectable configure/build/test presets, compiler, Ninja,
SDK/dependency prefixes, shader tools and IDE CMake preset mode. Actual
`cmake --list-presets` plus a real configure/build/test establishes setup.
Repair owns ignored local settings only, preserves custom preferences and
invalidates stale caches when compiler/SDK identity changes. Never download from
analysis, completion, project load or editor-open. Keep machine paths in ignored
local files. Share this behavior with the installed CLI and Editor backend.

Cache namespaces include host/target, compiler and standard-library identity,
dependency lock/recipe/toolchain digest, flavor, sanitizer, feature/backend and
PCH configuration. Cache dependencies and compiler outputs, not build trees as
an acceptance substitute. Reports name cache mode/hit rate; clean profiles
disable caches. Preserve one writer per compatible cache namespace; failed
build artifacts are not SDK inputs. Fork/PR jobs cannot publish trusted release
caches or run with signing/upload secrets. Treat restored cache content as an
optimization; validate identities and rebuild when uncertain. Do not execute
untrusted contributor code on persistent privileged hardware agents.

Do not run PR-head build scripts through `pull_request_target` with privileged
credentials. Hardware agents use isolated ephemeral environments and read-only
job tokens; trust admission never comes from a file supplied by the PR. GitHub's
[secure-use guidance](https://docs.github.com/en/actions/reference/security/secure-use)
informs these boundaries. No new hosted service is required by this proposal.

Release candidates promote the exact tested artifact, including symbols, build
identity, notices and verification manifests; do not rebuild different bytes at
publication. Release permissions belong only to publication jobs. Pin external
actions to reviewed commits and downloads to verified versions/checksums.
Perform deliberate periodic tool/dependency upgrades with rollback and the same
validation matrix; no unattended major-version drift.

Release evidence requires a clean checkout at the candidate commit. Local dirty
runs record a digest of their relevant source/input snapshot and are explicitly
local evidence; a dirty flag alone is insufficient to reconstruct their bytes.
Capture only an explicitly selected patch/input bundle when it is needed for
reproduction. Keep SDK source-compatibility consumers and gameplay ABI-version
contracts blocking; ABI-diff tools are optional only for a declared supported
binary interface, not a substitute for installed consumer tests.

## 8. Evidence and error-report contract

Each validation invocation writes under ignored `out/quality/<run>/<lane>/`:

- `run.json`: schema version, revision/dirty state, event and check ID,
  host/target/toolchain identities, UTC start/end, command argv/cwd, declared
  non-secret environment, seed, corpus/workload identity, cache mode and exit
  status. Never dump arbitrary environment variables.
- `results.json`: stable check/test IDs, pass/fail/skip/unavailable/error state,
  applicability and capability reasons, expected/executed counts and durations.
- Original stdout/stderr and tool-native artifacts; JUnit for CTest and
  HTML/LCOV, analyzer paths, symbolized sanitizer reports or fuzz inputs as
  appropriate. SARIF is optional for supported analyzers and GitHub upload;
  local raw reports remain usable without it.
- A short `reproduce.md`: required preset/setup, exact command, seed/input,
  artifact identity and capability prerequisites. The report is data, never
  an executable shell script fetched and run automatically.

Preserve human compiler `path:line:column` diagnostics for editor problem
matchers. Do not parse human prose to decide whether a check passed when a
reliable exit status or structured result exists. A failed check stays failed if
report generation also fails. Missing required evidence fails acceptance even
when the tool process returned zero. Always-upload steps are bounded and do not
hide the primary error; initial limits are 100 MiB per lane and 14-day PR
retention, with explicit larger/longer release-symbol and fuzz-corpus policies.

Crash bundles pair executable and symbols by build ID, with bounded logs,
assertion record, relevant configuration, module/backend versions and replay
input when available. The existing Base emergency path captures only data safe
at failure time; normal logging, allocations, stack symbolization, compression
and upload happen outside it in the helper/postprocessor. No automatic network
reporting or SaaS error SDK in the engine runtime. The macOS Editor's separately
bundled Crashpad handler can submit minidumps to Sentry after user consent;
[diagnostics](../development/diagnostics.md#macos-editor-crash-reporting) owns its
capture, symbols and deployment contract. Dumps may contain proprietary/user
data: capture only needed data and apply access/retention controls before upload.
See the existing assertion, diagnostics, logging and live-reload contracts.

Thanks to J.L. Raza and Peter Iliev Jr., *A More Informative Error Log Generator*,
GPG8 section 4.7, for caller context that helps non-programmers submit useful
reports. Keep the original operation/status/resource IDs alongside stack/module
addresses; a stack is supplementary and cannot replace an explicit error. Helpers
symbolize against the exact build, mark unavailable symbols honestly, and group
recurring failures by check, build and stable signature without deleting first
occurrences. Never block CI on an assertion dialog or perform loader-dependent
stack walking in the emergency path. Existing
[assertion contracts](assertions.md) and
[diagnostics guide](../development/diagnostics.md) remain authoritative; see the
[review](engineering-quality-gems-review.md#a-more-informative-error-log-generator).

## 9. Editor and contributor experience

Retain clangd and CMake Tools. Select a prepared preset, expose its compile
database through a stable ignored local reference, and update that reference
only after valid configure. Bind clangd to matching toolchain headers and a
reviewed LLVM 18 executable; browser/native databases remain separate. Preserve
custom VS Code settings, user shortcuts, F5 and launch preferences during repair.

Expose named process tasks for format verification/fix, static analysis,
tests/sanitizers, advisory includes, coverage and explicit build profiling. Tasks
invoke the same scripts and selected preset as CLI/CI, with problem matchers;
they do not contain their own setup/build logic. Formatting uses `scripts/check`
because stock clang-format does not implement the repository initializer rule.
Document any editor-format shortcut that cannot enforce that extension.

CodeLLDB is optional for supported native debugging; RAD retains its existing
optional Linux role. Coverage Gutters consumes generated LCOV; Ruff supports
Python editing. CMake Tools' test UI can expose registered CTest tests before
adding a second test adapter. Essential workflows must work without extensions.
No automatic debugger launch, profiler build, dependency download or global
settings rewrite on open. A missing database/tool reports a concrete remedy.

Compilation-database and include-diagnostic behavior follows the
[clangd configuration contract](https://clangd.llvm.org/config).

## 10. Migration and acceptance

Keep implementation changes small, individually reviewable and reversible.
Existing current guides gain links to this proposed architecture; new commands
enter those guides only after implementation and successful evidence.

| Phase | Deliverables | Acceptance before promotion |
| --- | --- | --- |
| Q0: Failure semantics (implemented) | Fatal UBSan, negative controls, complete configured-source inventory, bounded analysis/control logs | Known-bad isolated processes fail; omissions/missing artifacts fail; current clean checks remain green |
| Q1: Analysis and editor (advisory rollout) | Stable analyzer profile, pinned Ruff/actionlint, preset-aware clangd/tasks; [current contracts](../development/continuous-integration.md#q1-analysis-and-host-tooling) | Complete configured inventories; real negative controls; repeated selection preserves custom settings; promotion requires supported-platform findings and runtime review |
| Q2: Evidence and coverage | Shared report schema, JUnit/raw artifacts, Clang coverage profile | Parallel profiles merge correctly; fresh/moved checkout works; failed tests/reporters cannot yield success; local reproduction succeeds |
| Q3: Runtime exploration | Expanded TSan/fuzzing, selected Linux Memcheck | Fully instrumented targets, seeded reproducibility, original/minimized crash preservation and bounded runtime |
| Q4: Platform/performance | Controlled device lanes, calibrated scenarios, artifact promotion | Actual capability evidence, repeated base/head measurements, exact package identity and trusted-runner isolation |

Contract regressions cover fresh clone, missing/stale local presets, moved tools
and SDKs, repeated repair, generated sources, multiple compile variants, empty
scopes, broken classification, renamed/deleted paths, cancelled jobs, missing
capabilities, worker timeouts, artifact/report failures and fork events. These
tests verify failure boundaries and observable outcomes, not copied helper logic.

Q2 also adds a small scenario inventory and expected/observed checkpoint reports.
Negative controls remove a required checkpoint, preserve a failure but emit its
checkpoint, mismatch content/build IDs and truncate a run; all must remain failed
or incomplete. Q3 replays original reproducers and verifies post-quiescence
retirement invariants. Q4 validates capture loss/context and CPU/memory metric
interpretation. No reviewed article establishes these gates as already passed.

Measure median/p95 time to first actionable result and total PR completion,
runner minutes, cache hit rate, peak memory, finding usefulness, flaky failure
rate and median time to reproduce. Establish a 20-run baseline before optimizing
job placement. Change one scheduling variable at a time. Reuse same-job builds
where configurations match; do not share compiler output across incompatible
flavors. Analysis cost is balanced against build/memory cost; default CI
concurrency stays bounded until measurements justify changing it.

Removing or moving an existing required check needs a separate measured policy
change, routing regressions and updated contributor rules. A new tool spends
an initial two-week advisory period unless it closes a deterministic failure
semantics defect; promotion needs an owner and triaged baseline. These are
rollout targets, not evidence of validation already completed.

## 11. Design changes after the reference review

| Initial proposal | Final decision | Source contribution |
| --- | --- | --- |
| Compiler source/branch coverage | Add small scenario-specific expected/observed checkpoint sets and invariants | Jack: QA needs meaningful feature-level execution evidence |
| Command, seed and input reproduction | Include initial state, tick policy, content/tool identity, first-failure preservation and neighboring regression cases | Rabin: reproduce, isolate, repair and verify the cause |
| External profiler selection and timing distributions | Specify hierarchical inclusive/exclusive/call metrics, unaccounted work and incomplete capture | Hjelstrom/Garrabrant: nested context makes time actionable |
| Allocation/count budgets | Separate churn/live/retention and preserve allocation origin across threads | Lung: lifetimes do not align with CPU scopes |
| Symbolized crash bundles | Pair stacks with original error context and exact build identity; preserve crash-safe boundary | Raza/Iliev: context assists reports outside developer machines |

The [full reference review](engineering-quality-gems-review.md) documents reading
scope, printed/PDF pagination, useful ideas and rejected historical mechanisms.
Future implementation changes must carry concise attribution near affected code
and link to that review, as required by AGENTS.md. Referenced volumes are a local
research library; no book contents or companion source are redistributed.
