# Port Ludus to a new platform

Use this checklist whenever adding an OS, ABI, device class or execution
environment. It owns the repeatable porting workflow; platform plans link here
instead of maintaining another checklist. Start with
[target facts and compatibility](../wiki/guides/platform-targets.md) and the
[detection contract](../architecture/platform-detection.md).
For the active mobile scope see the [mobile implementation plan](mobile-support-plan.md).

## Start a port record

Create a section in the platform's canonical design/implementation plan, using
the fields below. Search for an existing owner before adding a document. Keep
public contracts/evidence under `docs/`, in navigation/index/search; keep private
signing material and machine paths outside public documentation. A new platform
needs a concrete deployment task, not just another preprocessor flag.

- Target OS/environment, architecture/ABI, endianness and pointer width.
- Host OS and host tools, separately from target SDK/compiler/linker/sysroot.
- Minimum OS/API and runtime graphics/audio requirements; unsupported variants.
- Toolchain/dependency pins and approved policy exceptions, with qualification.
- Physical-device/browser inventory, OS builds and runtime feature queries.
- Named implementation owners, branches/PRs, milestone dependencies and blockers.
- Required subsystems, optional/deferred features and release acceptance gates.
- Last audited base commit/date and last accepted implementation commit/date.

Use separate worktrees and `codex/` task branches for agent work. Preserve other
checkouts; start new work from fetched `origin/develop` and submit PRs to `develop`.

## Maintain subsystem coverage

For every row record **owner, status, required profile, blocker/next action,
implementation PR and evidence**. Status is `unreviewed`, `reuse candidate`,
`needs port`, `in progress`, `verified`, `unsupported` or `not applicable`.
Only mark `verified` for the named profile after its acceptance test. Explain
unsupported/not-applicable decisions and caller-visible behavior. Compilation
alone cannot advance runtime rows to verified.

The table is an audit inventory, not a requirement to implement every optional
feature on every platform. Review it against current module/CMake inventory on
each port, adding newly introduced boundaries here so later ports discover them.

| Boundary / owner | What must be reviewed | Minimum evidence for applicable profiles |
| --- | --- | --- |
| Target identity / FoundationBase | Compiler/SDK detection, build flavor, ABI, SIMD/intrinsics, OS-before-family selection | Real positive/negative compiler-target tests; installed consumer facts |
| Toolchain / build configuration | C++23, compiler extensions, exception/warning flags, ObjC/JNI boundaries, dependencies, licenses | Pinned target compile/link; no host libraries in target closure |
| FoundationBase runtime | Assertions, emergency output, abort/debug break, debugger discovery | Real target diagnostics and controlled failure test; independent of logging |
| Memory / containers | Alignment, page size, allocation failure, atomics and construction assumptions | Allocator/boundary tests, alignment checks, bounded failure behavior |
| Time / threading | Monotonic clocks, suspend delta, threads/TLS, jobs, synchronization, shutdown | Target clock/thread tests and interruption/cancellation journeys |
| Logging / profiling | Emergency versus ordinary sinks, thread safety, trace output destinations | Captured device diagnostics; unavailable sinks fail gracefully |
| Filesystem | Asset/package mounts, writable roots, async reads, persistence, watch semantics, permissions | Pack reads, corrupt inputs, interrupted saves, cancellation and low-storage tests |
| Platform / host | Native host loop, thread affinity, focus/suspension, memory pressure, surface ownership, OS termination | Repeated start/resume/stop/recreation on actual execution environment |
| Input | Touch/mouse/keyboard/controllers as scoped, text/IME, coordinate conversion, cancellation and overflow | Native events plus deterministic reducer tests; no stuck contacts/actions |
| RHI / graphics | Surface creation, lifetime/retirement, capabilities, feature fallback, shader variants, transfers/readback | Real presented pixels, feature queries and transition/failure tests |
| Renderer / graph | Required formats, resource packing, attachment/resolve paths, pass dependencies, bounded budgets | Same scoped scene, graph validation and capture on each backend |
| Text / localization / UI | Fonts/shaping dependencies, atlas limits, scale/insets, orientation, accessibility integration scope | Readable/responsive sample; text entry evidence if supported |
| Content / resources | Host cookers versus target loaders, compression, dependency closure, streaming and lifetime | Same cooked revision loads; missing/corrupt content and memory pressure tests |
| Runtime / game host | Entry point, static/dynamic loading policy, game API, shutdown ordering, app state | Separate SDK consumer and shared game boot/recovery |
| Behavior / scripting | Interpreter/JIT policy, bytecode/cooked metadata, profile admission and sandbox | Real behavior conformance; explicit unsupported execution modes |
| Gameplay / physics | Numeric assumptions, determinism claims, tick policy and device budgets | Scoped gameplay fixtures and resume behavior; evidence for determinism claims |
| Audio | Device/backend selection, session ownership, route/sample-rate changes, interruption and permissions | Playback plus interruption/resume/route-change tests; bounded failures |
| Networking | Sockets/transports, certificates, background suspension, permissions and reconnect | Scoped loopback/device communication and failure/reconnect journeys |
| SDK / projects / editor tooling | Exported dependencies, selectable presets, target selection, read-only setup check, repair and IDE | Installed consumer; real preset listing/configure/build/test; repair regressions |
| Packaging / distribution | Bundle assets, manifest/entitlements, ABI/page alignment, signing, upgrade/data policy and symbols | Clean install/upgrade, Release artifact inspection and symbolication |
| CI / documentation | Cross builds, portable tests, device runners, sanitizers, supported matrix and public contracts | Reproducible checks, explicit unavailable checks, canonical docs/wiki/offline bundle |

## Reusable record format

Use this small table in the platform owner and fill it for every inventory row;
add a row when a new subsystem enters the target scope. Do not copy the review
instructions above into each platform plan.

| Subsystem / required profile | Owner | Status | Blocker / next action | PR | Evidence / commit |
| --- | --- | --- | --- | --- | --- |
| FoundationBase / target ABI | unassigned | unreviewed | Audit target runtime | pending | none |

Evidence records include the exact commit, tool/SDK versions, target profile,
device/OS, commands or reproducible steps, results and remaining limitations.
Keep secrets, serial numbers and machine-specific output private; publish only
sanitized acceptance facts. Link existing canonical evidence when available.

## Admission gates

1. **Recognized:** target detection and negative tests pass. No runtime support
   is implied.
2. **Buildable:** reviewed toolchain and selected module closure compile/link;
   an installed-SDK consumer verifies ABI and dependency export. No device claim
   is implied.
3. **Runnable:** the scoped sample installs/launches and presents/input/storage
   work in the real environment, including lifecycle recovery.
4. **Qualified:** all required subsystem rows are verified; performance and
   failure budgets pass, checks are reproducible, unsupported features explicit.
5. **Distributable:** separately validated signed/release artifact, update/data
   behavior, symbols and current distribution requirements.

Promote profile admission, status docs and package acceptance together. Do not
disable a supported platform, weaken warnings/safety checks or enlarge a build
budget simply to get a new target through. Ludus's exception-free engine,
fixed-width types, default-deny library policy and explicit failure contracts
remain mandatory; document any necessary toolchain/library policy proposal for
review rather than silently treating a new SDK as an exception.

## Checks for every implementation slice

- [ ] Re-audit affected module/source selection and the row's caller contract.
- [ ] Run appropriate target tests and existing native/browser regression gates.
  Run pinned format/tidy and available ASan/UBSan; report unavailable target
  instrumentation precisely and retain the physical acceptance gate.
- [ ] Verify selectable configure/build/test presets with the selected CMake,
  SDK/dependency prefixes, compiler, Ninja, shader tools and IDE preset mode.
  Reuse the [project setup workflow](project-sdk-workflow.md); loading is read-only,
  explicit repair preserves custom settings and proves a real build/test.
- [ ] For setup changes cover fresh clone, absent presets, moved SDK/tools and
  repeat repair; never share writable caches across targets/worktrees.
- [ ] Update public API Doxygen contracts and their canonical guide in the same
  change; shrink resolved legacy baseline entries, never add documentation debt.
- [ ] Update the port row and milestone status with PR/evidence and limitations.
- [ ] Recheck this inventory when adding a subsystem, dependency, host tool or
  platform-sensitive feature. Existing qualified targets need regression
  evidence for changed shared contracts.
- [ ] Run `scripts/check-docs --fix-index`, `scripts/check-docs`, strict MkDocs,
  the documentation/wiki regression suites and `scripts/check-wiki
  --documentation-only`. Public API changes additionally require `scripts/build-api`.
  Package checked sources/HTML with `scripts/package-docs` for offline reading.

## When the port is maintained

Revalidate affected rows after SDK/compiler/dependency upgrades, minimum OS/API
changes, new device classes, new public platform-sensitive APIs or build/profile
changes. A prior successful port is reusable evidence for its recorded scope,
not a permanent guarantee for new environments. Remove stale support claims or
mark the affected profile blocked when a required gate regresses.
