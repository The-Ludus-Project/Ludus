# Ludus assertion subsystem design

Status: implementation contract. M0/M1 and the approved M2/M3 hardening and
closed formatting layer are implemented, together with narrow M4 byte-writer
integration. See the [adversarial audit and evidence](assertions-adversarial-review.md).
An amendment adds **resumable enabled `ASSERT`/`ASSERT_F` in eligible non-CI
builds** ([§5.1](#51-interactive-development-resumable-assert),
[§5.2](#52-startup-and-report-delivery-external-diagnostic-helper),
[ADR 0006](../decisions/0006-resumable-development-assertions.md),
the authoritative [interactive-development plan](assertions-interactive-development-plan.md),
and the [interactive-development spec](../../.kiro/specs/assertions-interactive-development/requirements.md)).
That behavior is now **implemented**: `ASSERT`/`ASSERT_F` split from the terminal
fatal path onto a resumable one, a debugger Continue (Debug or Development, non-CI)
or an explicit external-helper Continue-once (non-CI Debug, no debugger) returns
past the assertion, and the SDK assertion-policy version is bumped to 2.
`REQUIRE`/`FATAL` stay terminal in every build (including after a debugger
Continue) and `CHECK` stays boolean.
The [M0/M1 record](assertions-m0-m1.md) is historical. Rich crash collection,
stack capture, asynchronous logging hooks, and other-platform backends remain
prospective; successful tests alone are not production-readiness certification.

Original repository inspection: `64e5bfd36a2e`, September 23, 2026. The observations
below are the design baseline, not a claim that prerequisite defects still exist.
The [reconciliation plan](assertions-implementation-plan.md) identified the actual
test exception flag-order defect; M0 corrected it, along with Profile identity
and SDK assertion configuration. See the implementation evidence for current facts.

## 1. Recommendation

Put a small assertion runtime in **FoundationBase**, with private OS backends.
Expose a template-free `assert.hpp` for conditions and literal messages, and an
opt-in `assert_format.hpp` for a deliberately limited typed diagnostic format.
Use macros only to preserve expression text, capture the caller, suppress
evaluation, and place message evaluation behind the failure branch. Compile
formatting, reporting, debugger detection, and termination once in `.cpp` files.

An enabled `REQUIRE`/`FATAL` invariant **never returns on failure**, including
after a debugger resumes execution. An enabled `ASSERT`/`ASSERT_F` is terminal
by default, but is **resumable through explicit developer action** in a tightly
scoped configuration — locally built, non-CI Debug builds only (see the
[interactive-development policy](#51-interactive-development-resumable-assert)
and [ADR 0006](../decisions/0006-resumable-development-assertions.md)). A
recoverable `CHECK` returns `false`; its caller must implement a valid recovery
branch. None of these forms replaces normal error handling. Report through an
independent emergency path first; normal logging and a future crash reporter are
optional consumers of the already-built report.

This is a handful of headers and implementation files, not a handler framework,
general formatting library, replacement string library, or crash reporter.

The [critical review of Steve Rabin's GPG1 chapter](assertions-rabin-review.md)
records which historical ideas informed this design and which were rejected.
Its useful additions are reflected below: meaningful context, debugger usability,
report handoff without a debugger, and suppression of presentation only.

## 2. Repository findings and constraints

These findings record the initial audit used to design the failure runtime.
Later module implementations can expand those capabilities; the
[current logging guide](../modules/foundation/logging.md) describes its delivered
scope. Assertion reporting continues to use its independent FoundationBase path.


| Area | Observed implementation | Design consequence |
| --- | --- | --- |
| Base | `modules/foundation/base` is a static library, with fixed-width aliases in `types.h`, version APIs, and `UniquePtr` | Assertions must be usable inside Base without linking upward |
| Dependency direction | Logging publicly links Base; Platform publicly links Base and Logging | Base must not include Platform or Logging; OS primitives needed by diagnostics live below the windowing layer |
| Compiler helpers | `defines.h` exposes `LUDUS_INLINE` and a concept via `<type_traits>`; `defines.hpp` duplicates inline/type definitions and adds build detection | Do not include both, or make assertions inherit their transitive dependencies; consolidate only the necessary compiler helpers |
| Header export | Base's public file set exports `defines.h`, not `defines.hpp` | Assertion behavior cannot rely on the latter being installed |
| Logging API | `log.hpp` includes `<format>`, `<source_location>`, `<string_view>`, `<utility>`; `config.hpp` adds `<filesystem>` | Do not include `log.hpp` from any assertion header |
| Logging execution | `logger.cpp` is synchronous; `Asynchronous` is reserved but not implemented | Do not describe existing logs as queued, or pretend an async drain API exists |
| Logging failure path | `VLog` creates a `std::string` through `std::vformat`; dispatch and flush acquire a shared mutex and invoke sinks | Calling `LUDUS_LOG_FATAL` is not a safe implementation of an assertion |
| Fatal logging | Fatal records are flushed and copied to emergency output after normal dispatch; the logging call itself returns | `LUDUS_FATAL` must be distinct from `LUDUS_LOG_FATAL` |
| Emergency logging | Private `EmergencyLog` uses a 512-byte buffer, `snprintf`, `fwrite`, and `fflush`; Windows also uses debugger output | Useful behavior to preserve, but stdio locks/blocking prevent treating it as a hardened crash primitive |
| Thread identity | Logging's private thread ID is an engine counter; thread names use a TLS `std::string` | Assertion handling must not initialize that TLS state; capture a native thread ID independently |
| Platform | Current implementation is Wayland/headless windowing; OS classification is in `platform/config.h` | There is no reusable low-level debugger, crash, or stack-capture service yet |
| Memory and strings | No custom allocator or native general-purpose string/view exists; `UniquePtr` is present | Introduce only a diagnostic pointer/length view, not speculative allocator/string infrastructure |
| Error handling | Window APIs already return `bool`, use out-parameters, and are `noexcept` | Preserve explicit error paths; assertions diagnose programmer defects |
| Builds | Clang 23/LLD, C++23, extensions off, exceptions disabled; tests alone re-enable exceptions | Runtime entry points are `noexcept`; compiler intrinsics are narrowly guarded platform implementation details |
| Profile | `linux-clang-profile` selects `RelWithDebInfo`; current CMake consequently defines `LUDUS_BUILD_DEVELOPMENT`, not `LUDUS_BUILD_PROFILE` | Use an explicit assertion setting; fix build-flavor identity separately |
| SDK | Project options are linked privately with `BUILD_INTERFACE` | Export assertion configuration deliberately; it will not automatically reach installed consumers |

Read these sources before implementation:

- [Contributor rules](../../AGENTS.md) and [steering rules](../../.kiro/steering/coding-standards.md).
- [Standard-library policy](../decisions/0003-standard-library-usage-policy.md).
- [Logging type-erasure decision](../decisions/0004-logging-format-type-erasure.md).
- [Build profiling](../development/build-profiling.md).
- [Logging entry points](../../modules/foundation/logging/include/ludus/foundation/logging/log.hpp),
  [runtime](../../modules/foundation/logging/src/logger.cpp), and
  [emergency output](../../modules/foundation/logging/src/emergency_logger.cpp).
- [Build options](../../cmake/EngineOptions.cmake),
  [target defaults](../../cmake/EngineTargets.cmake), and
  [presets](../../CMakePresets.json).

ADR 0004 records approximately 19 s to 9 s of aggregate logging-header parse
time after moving heavy formatting instantiations out of the header. Those are
historical repository measurements, **not measurements of this proposal**. Its
remaining header cost is exactly the failure mode assertions must avoid.
Some milestone/README descriptions predate the present windowing and logging
code; use source and CMake as the implementation baseline.

## 3. Goals, non-goals, and error classification

### Goals

- One condition evaluation when enabled; none when disabled.
- No runtime work beyond the condition and a conditional branch on success.
- No allocation, locks, TLS access, formatting, or debugger polling on success.
- No C++ exceptions, iostreams, public STL types, or heavyweight public includes.
- Preserve expression, file, line, short function name, failure kind, native
  thread ID, and bounded diagnostic text.
- Work before logger initialization and after shutdown.
- Fail predictably from worker/render threads and within diagnostics itself.
- Keep build cost and emitted code measurable and bounded.

### Non-goals

- Recovering from arbitrary memory corruption or invalid diagnostic pointers.
- A signal-safe general assertion API, stack-overflow recovery, or GPU-side
  shader assertions. Those require separate fault entry points.
- Remote commands, per-site ignore registries, runtime handler chains, or
  automatic debugger attachment. Resumable **`REQUIRE`/`FATAL`** invariants
  remain a non-goal: those forms are terminal in every build. A single,
  non-persistent "Continue once" decision for enabled `ASSERT`/`ASSERT_F` is an
  explicit, tightly scoped exception introduced by
  [ADR 0006](../decisions/0006-resumable-development-assertions.md); it is not a
  general dialog/handler framework or a per-site ignore registry (see
  [§5.1](#51-interactive-development-resumable-assert)).
- A general-purpose formatter, arbitrary object printers, reflection, or
  allocation-heavy symbolication on the failing thread.
- Guaranteed log durability in the presence of a hung kernel/device/process.

### What belongs here

| Situation | Mechanism | Why |
| --- | --- | --- |
| Internal texture pointer must exist by construction | `ASSERT`, or `REQUIRE` if shipping must enforce it | Programmer invariant |
| Internal index could otherwise cause memory corruption in shipping | `REQUIRE` before the dangerous operation | An actual branch, not an optimizer assumption |
| Expensive debug-only graph consistency traversal | `ASSERT` | Deliberately absent from Profile/Release |
| Internal cache inconsistency has a documented safe rebuild path | `if (!CHECK(...)) { rebuild or return failure; }` | Unexpected defect with explicitly valid recovery |
| Impossible enum/state after internal validation | `FATAL` | No valid continuation exists |
| Malformed asset/network packet, missing file, unsupported feature | Status/result/optional plus contextual logging | Expected external failure, even if rare |
| GPU/device loss | Device-loss result and recovery/shutdown policy | A driver/device outcome is not proof of an engine invariant defect |
| Recoverable allocation failure | Fallible allocation API and explicit failure return | Assertions cannot provide memory recovery |
| Compile-time layout/type property | Language `static_assert` | No runtime machinery needed |

Validate external indices with ordinary branches before use. An assertion can
add an internal post-validation invariant, but must never be the only input
validation. `FATAL` is appropriate only after a deliberate policy decision that
the process cannot continue, not merely because an operation returned an error.
Allocation failure inside diagnostic infrastructure drops optional detail; it
must not turn a recoverable `CHECK` into a fatal engine failure.

## 4. Taxonomy, public names, and exact behavior

Use `ludus::foundation::diagnostics` for assertion-specific types and functions.
Use the existing Ludus aliases, PascalCase types/functions/members, `#pragma
once`, and lower-case implementation locals consistent with surrounding code.

| API | Result | Failed condition | Condition evaluated |
| --- | --- | --- | --- |
| `LUDUS_ASSERT(condition [, literal])` | Statement | Report, optional debugger break, terminate — resumable by explicit developer action in local non-CI Debug builds ([§5.1](#51-interactive-development-resumable-assert)) | Only when development assertions are enabled |
| `LUDUS_REQUIRE(condition [, literal])` | Statement | Same, in every build | Exactly once, every build |
| `LUDUS_CHECK(condition [, literal])` | `bool` expression | Best-effort report, optional development break, return `false` | Exactly once, every build |
| `LUDUS_FATAL(literal)` | Nonreturning statement | Unconditional fatal report | Not applicable |
| Corresponding `_F` forms | Same as above | Typed formatting, exclusively on failure | Same rules |

`ASSERT`, `REQUIRE`, and `CHECK` permit no message. `FATAL` requires a reason.
The base forms accept a string literal or a valid NUL-terminated borrowed
message, without brace interpretation. Prefer literals. `_F` requires a
literal-format array and zero or more supported values. Define only the small
family above: no `VERIFY`, `ENSURE`, `ASSERT_ALWAYS`, `UNREACHABLE`, or competing
synonyms until an actual requirement distinguishes their semantics.

`REQUIRE` means “required invariant,” not user-input validation. `CHECK` is
always evaluated but is not a release-safe replacement for an assertion unless
the caller handles `false`. Bare `CHECK` statements should be rejected in review
and flagged by the project's future lint rule; `[[nodiscard]]` on a failure
helper alone cannot reliably diagnose a discarded macro conditional expression.

```cpp
#include <ludus/foundation/base/assert.hpp>

LUDUS_ASSERT(texture != nullptr);
LUDUS_ASSERT(ownerThread == currentThread, "Wrong command-list owner");
LUDUS_REQUIRE(writeOffset <= capacity, "Corrupt internal buffer offset");

if (!LUDUS_CHECK(cacheGeneration == expectedGeneration, "Stale internal cache"))
{
    InvalidateCache();
    return false;
}

// Invalid external input uses normal control flow, in every build.
if (packetIndex >= packetCount)
{
    return DecodeStatus::InvalidIndex;
}

#include <ludus/foundation/base/assert_format.hpp>

LUDUS_ASSERT_F(index < count, "Invalid index: {} of {}", index, count);
LUDUS_REQUIRE_F(bytes <= remaining, "Need {} bytes; have {}", bytes, remaining);
LUDUS_ASSERT_F(texture != nullptr, "Texture address: {}", DiagnosticAddress(texture));
LUDUS_FATAL_F("Invalid internal command state {}", static_cast<uint32>(state));
```

Examples assume the appropriate namespace imports and enclosing functions.
Do not put required work in a condition or message:

```cpp
// Incorrect: the initialization disappears when ASSERT is disabled.
LUDUS_ASSERT(InitializeRenderer());

// Correct: execute it, then apply the application's explicit error policy.
const bool initialized = InitializeRenderer();
if (!initialized)
{
    return false;
}
```

Failure messages must be observational, bounded, and safe to evaluate in the
failure state. They are evaluated at most once, and may be skipped when recursive
or concurrent reporting is suppressed. No application behavior may depend on
them. As with ordinary function arguments, do not depend on argument evaluation
order; the API does not promise left-to-right evaluation.

Pure query functions are permitted in conditions; banning every function call
would prevent useful invariant checks. A query must be valid in the calling
context and have no required side effects. A message should explain the violated
contract and supply distinguishing values (resource ID, expected/actual state,
owner, generation), not merely repeat the condition or manually embed a function
name already captured in metadata. Do not encode messages using
`condition && "message"`, negate a literal for an unconditional failure, or
re-evaluate the condition to obtain diagnostic values. Use the separate message
parameter, explicit `_F` arguments, and `FATAL` respectively.

## 5. Build configuration contract

| Configuration | `ASSERT` / `ASSERT_F` | `REQUIRE`, `CHECK`, `FATAL` | Break for fatal failure when debugger attached | Break for failed `CHECK` | `ASSERT` resumable |
| --- | --- | --- | --- | --- | --- |
| Debug | Enabled | Enabled | Yes | Yes | **Yes (non-CI): debugger Continue, or no-debugger dialog** |
| Development / sanitizer development | Enabled | Enabled | Yes | Yes | Debugger Continue only (non-CI); no dialog |
| Profile | Disabled | Enabled | Yes | No | n/a |
| Shipping / Release / MinSizeRel | Disabled | Enabled | Yes | No | n/a |

Profile excludes optional checks so profiling represents release workloads;
`REQUIRE` still protects critical invariants. Expensive optional checks can be
enabled in a separately named diagnostic profiling build, with that fact in
its build metadata. A debugger break for a **`REQUIRE`/`FATAL`** failure is an
inspection opportunity, never an authorization to resume past the failed
invariant. A debugger break for an enabled **`ASSERT`/`ASSERT_F`** failure is
resumable in Debug **and** Development when not under CI (continuing returns past
the assertion); the **no-debugger Continue-once dialog** is additionally
available only in a non-CI Debug build. Under CI, and for every other kind,
continuing a break still proceeds to termination.

Add numeric `LUDUS_ENABLE_ASSERTS`, `LUDUS_BREAK_ON_CHECK`, and
`LUDUS_ASSERT_DIALOGS_AVAILABLE` values to a tiny generated public
`assert_config.hpp`. `LUDUS_ASSERT_DIALOGS_AVAILABLE` is `1` **only** for a
non-CI Debug build (generated from the explicit flavor and the CI build setting)
and `0` elsewhere; it expresses build-time *eligibility* for the no-debugger
interactive dialog in [§5.1](#51-interactive-development-resumable-assert).
Whether a given run actually resumes is a further runtime decision (CI veto,
attached debugger, or an explicit helper Continue-once). A debugger Continue can
resume `ASSERT` in Development too, so it is deliberately not gated by this
value. Adding this policy value bumps `LUDUS_ASSERT_POLICY_VERSION` (now 2) and
is recorded in the SDK manifest. Generate the header per configured SDK variant;
export the matching include directory and file through Base's public file set.
The source build and installed SDK must use the same values. Do not infer policy
from `NDEBUG`, `DEBUG`, or `defines.hpp`, and do not permit per-TU overrides of
this generated header. Fail configuration on invalid values or conflicting
build flavors. Multi-config SDKs need separate configuration-specific generated
include roots and packages; do not install one flavor over another.

Set the Profile preset's assertion values explicitly now. In the implementation
change, make build flavor a real CMake setting so Profile is not mislabeled as
Development. The current `CONFIG:Profile` expression does not solve this for a
`RelWithDebInfo` preset. Keep assertion policy independent of log filtering.

Mixing assertion settings in public inline/template definitions risks an ODR
violation even if class layouts match. Treat the SDK variant as part of its
build contract, record it in the SDK manifest, and test the installed consumer.
Changing assertion settings intentionally recompiles consuming TUs.

### 5.1 Interactive development: resumable `ASSERT`

This subsection is authoritative for the resumable-`ASSERT` behavior introduced
by [ADR 0006](../decisions/0006-resumable-development-assertions.md). It
supersedes, for `ASSERT`/`ASSERT_F` only, the earlier blanket statement that an
enabled invariant assertion never returns. `REQUIRE`, `REQUIRE_F`, `FATAL`, and
`FATAL_F` are unchanged and terminal in every build; `CHECK`/`CHECK_F` are
unchanged and recoverable.

**What "resumable" means.** A failed enabled `ASSERT`/`ASSERT_F` may, through an
explicit per-hit developer action, return to its caller instead of terminating.
It is a single "Continue once" decision, never a persistent "ignore always,"
never automatic, and never a recovery mechanism the program may depend on. Code
after a resumed `ASSERT` runs with the invariant *known broken*; this is the
price of the workflow and the reason it is confined to Debug and to `ASSERT`.

**Where it is eligible (build-time).** `LUDUS_ASSERT_DIALOGS_AVAILABLE` is `1`
only in a non-CI Debug build; it gates the **no-debugger dialog** path only.
Profile/Release do not compile `ASSERT` at all. Development compiles `ASSERT` and
follows the plan's execution matrix: with a debugger attached (non-CI) an
`ASSERT` break and a debugger Continue **resume** just as in Debug; **without** a
debugger Development is **report-only** and terminates (it never opens a dialog
and needs no display/helper). The dialog is the Debug-only difference, not the
debugger break.

**Whether it actually resumes (runtime).** Even in an eligible Debug binary, a
failed `ASSERT` resumes only when *all* of the following hold at the moment of
failure:

1. **Not CI.** The process must not detect a CI environment. A CI detection
   *vetoes* resume and forces the terminal fatal path, even for a Debug binary
   that was built locally and later executed under CI. This "runtime CI veto"
   prevents a prompt from hanging a runner or a continue from masking a defect.
2. **Explicit action, by exactly one mechanism:**
   - **Debugger attached:** the existing inspection breakpoint is the resume
     point. Continuing past the break returns from the failed `ASSERT` to the
     caller. (For `REQUIRE`/`FATAL` the break still falls through to
     termination — unchanged.)
   - **No debugger attached:** present a **separate decision prompt** offering
     exactly **Continue once** and **Terminate**. "Continue once" returns to the
     caller for that single hit; "Terminate" takes the normal terminal fatal
     path. This prompt exists *only* in a local, non-CI Debug build with usable
     interactive I/O.
3. **Interactive I/O is available.** If the prompt cannot be presented — no
   controlling terminal, headless/detached run, closed or non-interactive
   stdin/stdout, or the prompt is delegated to a helper that is absent or has
   exited — the outcome is **Terminate**. There is no default-continue and no
   silent skip.

**Ordering with the existing failure runtime.** Resume is decided *after* the
owned report has been constructed and published and after the primary emergency
write attempt, i.e. at the point where the current design breaks-then-
terminates. The complete report and the minimal fatal header are committed
before any prompt is shown, so an interrupted or faulting prompt still leaves
the full diagnostic evidence. A resumed `ASSERT` must release the reporting slot
and clear its per-thread entry state exactly like a completed `CHECK`, so a
subsequent failure on any thread can acquire the slot again; it must not leave
the process in the "fatal owner committed" state. Recursion and owner contention
are never resumable: a recursive `ASSERT` on the same thread, or an `ASSERT`
that fails while another failure owns the reporting slot, takes the immediate
secondary/recursive termination path with no prompt. The `REQUIRE`/`FATAL`
primary fatal packet is still never released or reused; a resumable `ASSERT`
that continues must not have consumed that terminal packet.

**Report lifetime across a resume.** Because `ASSERT` can now return, its report
storage must follow the recoverable-`CHECK` lifetime rules, not the terminal
fatal-packet rules: owned bytes valid only for the synchronous report, no
retained borrowed pointers, and no publication into the single-incident terminal
crash packet. A resumed `ASSERT` counts against the same presentation budget
discipline as `CHECK` so a hot resumed `ASSERT` cannot flood diagnostics; the
condition is still evaluated exactly once per encounter regardless of budget.

**Runtime split and success-path surface.** `ASSERT`/`ASSERT_F` use their own
private `detail::BeginAssert` / `FinishAssert` (and `FinishAssertRendered` /
`FinishAssertArgs`) entry points that are **not** declared `[[noreturn]]`;
`REQUIRE`/`FATAL` keep the terminal `BeginFatal` / `FinishFatal*` pair. The
`ASSERT`/`ASSERT_F` macro expansions are otherwise unchanged and the success path
is untouched: no success-path helper call, TLS, atomic, lock, clock, debugger or
CI query, allocation, or per-site state. The resumable decision lives entirely in
`assert.cpp`'s cold `FinishAssert` path via a private `ResolveAssertDecision`,
which reuses the detached OS-primitive boundary (`QueryDebugger` /
`BreakForDebugger`), the shared `IsContinuousIntegration()` CI query, and — only
under `LUDUS_ASSERT_DIALOGS_AVAILABLE` — the versioned control endpoint
(see [§5.2](#52-startup-and-report-delivery-external-diagnostic-helper)). None of
this adds anything to `assert.hpp` or any public header; the failing thread runs
no UI code.

**Report lifetime and CHECK budget interaction.** In the current implementation
a resumed `ASSERT` uses a bounded stack-owned report delivered through the
nonblocking transport (never `gFatalPacket`); it releases the slot and clears TLS
exactly once, so a later failure is independently reportable. On `Terminate` it
publishes that already-built report into the single-incident fatal packet without
re-evaluating anything, then aborts. `CHECK`'s existing report budget is
unchanged, and `CHECK` inspection breaks are additionally suppressed under CI.

### 5.2 Startup and report delivery (external diagnostic helper)

This section records the original M0/M1 startup baseline. Interactive assertion
continuation was added later under [the current policy](#51-interactive-development-resumable-assert);
see [the diagnostic launch guide](../development/diagnostics.md) for current commands.


This subsection is authoritative for the *startup/report-delivery* layer that
supports interactive development. It is implemented independently of, and ahead
of, the resumable-`ASSERT` decision itself: this layer establishes the channels
and delivers reports, and it deliberately **does not change any assertion's
fatal action**. See the
[startup validation record](assertions-startup-validation.md) for exactly what
was tested.

**External diagnostic helper.** Interactive presentation and durable report
capture are the job of a small, separate **external process** — a Python
development helper at `tools/diagnostics/ludus_diagnostic_helper.py` (installed
as `ludus_diagnostic_helper`), never the engine's failure handler, window
manager, render thread, main-thread queue, or normal logger. It uses only the
Python standard library for this milestone; the graphical Continue-once /
Terminate dialog (Zenity) is a later milestone. The helper owns the collector
ends of two channels, launches the engine binary with the engine-side
descriptors, and cleans up when the engine exits:

- **Report channel** — a connected `AF_UNIX`/`SOCK_DGRAM` pair. This reuses the
  existing nonblocking datagram report transport
  ([§11](#11-logging-and-emergency-output)); the engine end is configured with
  `ConfigureEmergencySocket` and written with `TryWriteEmergencyBytes`. The
  helper drains report datagrams to its own output so `ASSERT`/`FATAL`/`CHECK`
  reports and formatted messages are visible/captured **independently of normal
  Logging**, including before logger initialization and after shutdown, and it
  keeps draining while any future dialog is open.
- **Control channel** — a separate connected `AF_UNIX` pair (`SOCK_SEQPACKET`
  on Linux; framed `SOCK_STREAM` on macOS) so
  report traffic can never masquerade as a decision reply. Its wire format is an
  **explicit little-endian byte encoding**, not a compiler-padded C++ struct: a
  fixed 16-byte header (`magic`, `version`, `kind`, `incidentId`, `length`)
  followed by `length` payload bytes, encoded/decoded through
  `EncodeControlHeader`/`DecodeControlHeader`. At healthy startup the engine
  sends `Hello` and the helper replies `HelloAck`; a mismatch or absent ack
  leaves the endpoint `Failed` and the engine runs report-only. The
  `DecisionRequest`/`DecisionReply` kinds (carrying an incident id and bounded
  owned report bytes / a one-byte decision) are defined in the layout for a
  **later** explicit-decision milestone but are not sent by this milestone's
  runtime. This IPC protocol version is independent of the terminal
  fatal-packet version.

**Startup-only integration.** The integration lives in a small target
**above** FoundationBase — `Ludus::DiagnosticsIntegration`
(`tools/diagnostics/`, header `ludus/diagnostics/session.hpp`) — which depends on
Base; **Base does not depend on it**, and it pulls in neither Logging nor
Platform. `InitializeDiagnosticSession()` is called once, early in `main`,
**before engine workers and before logger setup**. It reads the inherited
descriptors from the environment (`LUDUS_DIAGNOSTIC_REPORT_FD`,
`LUDUS_DIAGNOSTIC_CONTROL_FD`), configures both Base transports, resolves the
presentation mode, and returns an explicit status. It **never launches** the
helper or any UI — configuration only — and adds nothing to any engine public
header or to the assertion success path.

**Descriptor ownership.** Both transports duplicate the inherited descriptor
with `F_DUPFD_CLOEXEC` and retain the owned copy for process lifetime; there is
no replacement or teardown, so a published descriptor can never race a failure.
The caller may close its originals. The helper owns and closes the collector
ends on child exit.

**Mode resolution and the CI veto.** Interactive presentation is eligible only
in a local, non-CI, resumable (Debug) build; Development, Profile, and Release
are report-only and need no display or dialog helper. CI is auto-detected
([§12](#12-debugger-and-platform-behavior)) and always forces report-only —
even for a locally built Debug binary executed under CI — and CI workflows also
set the noninteractive policy explicitly. A local headless Debug run can opt into
report-only (`LUDUS_DIAGNOSTIC_INTERACTIVE=0` or the API's `ReportOnly`).

**Validate capability, not availability.** When interactive is eligible and
intended, startup validates a *usable* channel — a controlling terminal, or a
**live Linux display connection** probed through the Wayland/X11 socket
(macOS currently uses a terminal only) — not
merely that a dialog executable exists on `PATH`. If validation fails, startup
degrades to report-only and **reports the failure visibly** (through the ordinary
blocking emergency writer, not the nonblocking assertion transport). Failed
interactive setup never aborts startup and never changes an assertion's action.

## 6. Header and binary architecture

### File ownership

```text
modules/foundation/base/
  include/ludus/foundation/base/
    types.h                    existing aliases; cstddef/cstdint only here
    compiler.hpp               small guarded cold/noinline attribute helpers
    assert.hpp                 Site, detail entry declarations, base macros
    assert_format.hpp          diagnostic value tags/overloads, small pack adapter
    diagnostic_hooks.hpp       rare bootstrap/bridge API; never included by assert.hpp
    diagnostic_output.hpp      narrow emergency byte-output API for Logging
  generated include root/
    ludus/foundation/base/assert_config.hpp
  src/
    assert.cpp                 policy, ownership, report construction, termination flow
    diagnostic_format.cpp      parser and scalar conversion, compiled once
    diagnostic_output.cpp      bounded output routing
    diagnostics_linux.cpp      Linux OS primitives
    diagnostics_windows.cpp    Windows OS primitives when port is supported
    diagnostics_macos.cpp      macOS OS primitives when port is supported
    internal/
      diagnostic_record.hpp    private fixed packet / formatting interfaces
      diagnostic_platform.hpp  private OS backend contract

modules/foundation/logging/src/
  diagnostic_bridge.cpp        optional adapter; no Base dependency upward
```

These files belong to the existing static Base target; no new standalone module
or higher-level Platform dependency is necessary. Low-level diagnostic OS calls
are foundational services, distinct from windows/events. If a broader platform
core emerges later, move their implementation below both Base and Platform;
do not create that library speculatively now.

Public header graph:

```text
assert.hpp ------------> types.h ----------> cstddef, cstdint
       |---------------> compiler.hpp       (no includes)
       `---------------> assert_config.hpp  (only numeric configuration)

assert_format.hpp -----> assert.hpp          (no additional STL includes)

Logging .cpp ---------> diagnostic_hooks.hpp / diagnostic_output.hpp
Logging --------------> FoundationBase
Platform -------------> Logging + FoundationBase
```

`compiler.hpp` supplies only portability helpers needed by these declarations,
such as `LUDUS_NOINLINE` and `LUDUS_COLD`; use empty fallbacks where unsupported.
Use ordinary C++ `[[noreturn]]` and `[[unlikely]]` directly. Do not make a new
compiler-detection framework. Resolve the existing duplicate inline macro in a
small explicit prerequisite change if sharing it, not by including both old
defines headers. Do not change the no-GNU-dialect policy: guarded intrinsics and
attributes are the same narrow portability practice as today's `LUDUS_INLINE`.

### What belongs in the hot header

- Small `AssertionSite` and `DiagnosticText` aggregates, a byte-sized failure-kind enum, and the
  declarations of non-template failure entry points.
- Minimal macros, configuration include, and compiler attributes.
- No handler registry, locks, atomics, buffers, OS headers, logging categories,
  clock APIs, string utilities, format parser, stack unwinder, or initialization.
- No build revision/timestamp include. Only the runtime `.cpp` reads build
  metadata; changing a revision must not invalidate every assertion consumer.

Types crossing this C++ ABI are scalars, tagged unions, pointer/count pairs,
and function pointers. This is a narrow same-toolchain SDK ABI, not a promise
of C ABI compatibility or cross-version binary stability. Version the rare hook
table with size/version fields; do not serialize pointer-containing structs to
disk as a wire format. A future DLL build must have **one** runtime instance;
linking private copies into plugins would split failure ownership and hooks.

## 7. Source location and expression preservation

```cpp
namespace ludus::foundation::diagnostics
{
struct DiagnosticText
{
    const char* Data;
    usize Size;
};

struct AssertionSite
{
    const char* File;
    const char* Function;
    const char* Expression; // nullptr for an unconditional fatal
    uint32 Line;
};

enum class FailureKind : uint8
{
    Assert,
    Require,
    Check,
    Fatal
};
}
```

Construct the site in the macro's failure branch using `__FILE__`, `__LINE__`,
`__func__`, and `#condition`. `__func__` intentionally captures the short
function name, not a large template signature. Restrict runtime assertion use
to function bodies (including constructors/destructors and initialization
lambdas); use `static_assert` at namespace/class scope. A source wrapper macro
must forward the user's location, rather than capturing inside its helper.

| Alternative | Advantages | Costs / reason not selected |
| --- | --- | --- |
| `std::source_location` | Small standard facility; clean default-argument capture, already used by Logging | Adds a public STL type/include and implementation-specific representation; macros are needed anyway; no meaningful simplification here |
| `__FILE__`, `__LINE__`, `__func__` | No include or template cost; portable macro call-site capture; transparent ABI | Function-body restriction; explicit aggregate |
| `__builtin_FILE/LINE/FUNCTION` defaults | Useful for function APIs that need caller capture | Compiler abstraction and fallback work without eliminating expression/elision macros |
| Rich signature / pretty-function builtins | More detail for overloads/templates | Potentially large repeated strings; reserve for debugger symbols |

Compiler location builtins are available for future function-based APIs, but
are not required for this design. Clang documents both their caller-capture
behavior and debugger/trap intrinsics in its
[language extensions reference](https://clang.llvm.org/docs/LanguageExtensions.html).

Stringification preserves `texture != nullptr` without programmer repetition.
It preserves source tokens, with the preprocessor's whitespace normalization;
it does not automatically record operand values. Avoid expression-decomposition
templates: they multiply overloads, change operator behavior, and harm builds.
Pass useful values explicitly to `_F`.

Stringify in the outer user-facing macro before forwarding; otherwise macro
operands may expand before stringification. Parenthesize conditions containing
template/initializer commas: `LUDUS_ASSERT((Matches<A, B>(value)));`. Every
macro reference to the *evaluated* condition appears exactly once; `#condition`
does not evaluate it.

Use build prefix mapping to make file strings repository-relative, verified on
Clang 18 and each port. Keep relative file/line and expression strings for
shipping `REQUIRE`/`CHECK`/`FATAL`; do not introduce a hashed-site database yet.
Disabled assertion macros must reference none of their parameters, so their
expression, file/function metadata, and format literals need not survive in
the object. Measure retained `.rodata`; do not assume linker string merging.
Never place secrets or private user data in diagnostics.

## 8. Macro/function boundary and evaluation order

The following is representative interface/pseudocode. Failure entry points are
visible for macro expansion but remain in `detail`; callers must not invoke
their two-stage protocol directly.

```cpp
namespace ludus::foundation::diagnostics::detail
{
// Copies the site; establishes recursion/ownership before message evaluation.
void BeginFatal(FailureKind kind, const AssertionSite& site) noexcept;
bool BeginCheck(const AssertionSite& site) noexcept;

[[noreturn]] void FinishFatal(const char* message = nullptr) noexcept;
bool FinishCheck(const char* message = nullptr) noexcept; // always returns false
}

// In the real header, all detail symbols are fully qualified.
#define LUDUS_DETAIL_SITE(expressionText) \
    (::ludus::foundation::diagnostics::AssertionSite{ \
        __FILE__, __func__, (expressionText), \
        static_cast<::ludus::foundation::uint32>(__LINE__)})

#if LUDUS_ENABLE_ASSERTS
#define LUDUS_ASSERT(condition, ...) \
    do \
    { \
        if (!static_cast<bool>(condition)) [[unlikely]] \
        { \
            ::ludus::foundation::diagnostics::detail::BeginFatal( \
                ::ludus::foundation::diagnostics::FailureKind::Assert, \
                LUDUS_DETAIL_SITE(#condition)); \
            ::ludus::foundation::diagnostics::detail::FinishFatal(__VA_ARGS__); \
        } \
    } while (false)
#else
#define LUDUS_ASSERT(...) ((void)0)
#endif

#define LUDUS_CHECK(condition, ...) \
    (static_cast<bool>(condition) ? true : \
        (::ludus::foundation::diagnostics::detail::BeginCheck( \
            LUDUS_DETAIL_SITE(#condition)) \
             ? ::ludus::foundation::diagnostics::detail::FinishCheck(__VA_ARGS__) \
             : false))
```

`REQUIRE` uses the enabled assertion expansion with `FailureKind::Require`,
unconditionally. `FATAL` uses the same body without a condition and an expression
pointer of `nullptr`. `_F` macros substitute `FinishFatalFormatted` or
`FinishCheckFormatted`; their Begin call still comes **before** evaluating any
format argument. Plain macros forward an optional message to a defaulted
parameter; C++23 variadic macros permit the empty pack. Use `__VA_OPT__(,)` only
in helpers that actually require an optional separator; no GNU comma swallowing.

Two-stage entry is justified narrowly: it records the site and establishes the
recursion guard before diagnostic argument expressions run. A one-stage
`Fail(site, fmt, ExpensiveDiagnostic())` cannot guard a recursive assertion in
that argument. `BeginFatal` writes a minimal primary header before optional
message evaluation, so a fault during argument evaluation still has context.

The implementation keeps a trivial, constant-initialized TLS entry state:
depth, kind, copied site, and ownership flag. It allocates no TLS string or
dynamic object. Finish requires a matching active Begin on the same thread;
violations use an internal emergency termination path, not another assertion.
`FinishCheck` releases ownership and clears TLS before returning `false`.
There is no exception unwinding or cancellation boundary between Begin and
Finish. Hooks/diagnostic expressions must not use `longjmp`, coroutine suspension,
or thread cancellation through this region.

The boolean macro needs no capturing lambda, per-site lambda type, overloaded
boolean operator, or statement-expression extension. `static_cast<bool>` accepts
explicit boolean conversions and avoids a user-defined `operator!`. A failed
condition is evaluated before Begin, so recursion inside the condition itself
cannot be guarded by the reporting runtime; conditions are normal engine code.

An enabled assertion can appear in a `constexpr` function: the passing branch
can remain constant-evaluable. A failed assertion attempts a non-constexpr
function call and therefore fails constant evaluation; no custom compile-time
error framework is necessary. Use `static_assert` when its diagnostic is better.
Disabled assertions use `((void)0)`, **not** `sizeof(condition)`, `if constexpr
(false)`, an `assume`, or an unreachable builtin. They neither type-check nor
evaluate their arguments; separate enabled-build CI catches stale expressions.

## 9. Strings and formatting

### Selected strategy

Base assertions take literal/borrowed C strings. Formatted assertions require
the explicit `_F` spelling and `assert_format.hpp`. That opt-in is intentional:
the common `ASSERT(ptr)` must not inherit formatting machinery, and changing
formatting support should not rebuild TUs that only use plain assertions.

Use the diagnostic-only `DiagnosticText` view from `assert.hpp` and a closed
tagged value set in `assert_format.hpp`:

```cpp
enum class DiagnosticArgKind : uint8
{
    Signed,
    Unsigned,
    Floating,
    Boolean,
    Text,
    Address
};

struct DiagnosticArg
{
    DiagnosticArgKind Kind;
    union
    {
        int64 Signed;
        uint64 Unsigned;
        float64 Floating;
        bool Boolean;
        DiagnosticText Text;
        const void* Address;
    } Value;
    bool Truncated = false; // bounded C-string/array scan exhausted its extent
};
```

This view is not a general Ludus `StringView`: it exists to pass diagnostic
bytes without transitive STL exposure. It has no allocation, ownership, search,
encoding transformations, or generic iterators. If a native string/view arrives,
an explicit adapter can expose its data and size without changing the runtime
ABI. Existing `std::string_view` users can explicitly pass
`DiagnosticText{view.data(), view.size()}`; do not add a public STL include solely
for automatic conversion.

Provide non-template overloads for all eight fixed-width integer aliases,
`float32`, `float64`, `bool`, and `DiagnosticText`. Normalize narrow integers to
`int64`/`uint64` and floats to `float64` in tiny inline functions. `usize` and
`isize` already alias platform integer types on supported targets; do not add
duplicate overloads. Test this on LP64 and LLP64. Accept char arrays through one
small array overload, with a bounded scan for mutable arrays; literals may use
their known extent. Do not silently convert arbitrary pointers to booleans:
provide a deleted catch-all conversion template so unsupported exact matches
fail to compile. Use explicit `DiagnosticAddress(const void*)` and
`DiagnosticCString(const char*)` wrappers to distinguish addresses from text.
The latter performs a bounded scan only after Begin. Scoped enums require an
explicit underlying-width cast. No ADL formatters or arbitrary user callbacks.

The formatting adapter takes argument values by `const Args&...`, not through
perfect forwarding; it only reads/copies them. Bind rvalues until the call ends,
normalize scalar values immediately, and borrow text only for that synchronous
call. It packs `DiagnosticArg[]` and calls a non-template backend. Assert a
maximum of **eight arguments** with language `static_assert`; callers should
summarize unusually large context. Use `if constexpr (sizeof...(Args) == 0)` to
pass null/zero instead of creating a zero-length C array. One small template is
instantiated per argument-type tuple and literal array extent, not a parser or
formatter per type. Do not use the literal contents as a template parameter.

Representative boundary, in `detail`:

```cpp
[[noreturn]] void FinishFatalArgs(
    DiagnosticText format, const DiagnosticArg* args, usize count) noexcept;
bool FinishCheckArgs(
    DiagnosticText format, const DiagnosticArg* args, usize count) noexcept;

template <usize Extent, typename... Args>
[[noreturn]] void FinishFatalFormatted(
    const char (&format)[Extent], const Args&... args) noexcept
{
    static_assert(sizeof...(Args) <= 8);
    if constexpr (sizeof...(Args) == 0)
    {
        FinishFatalArgs({format, Extent}, nullptr, 0);
    }
    else
    {
        const DiagnosticArg packed[] = {MakeDiagnosticArg(args)...};
        FinishFatalArgs({format, Extent}, packed, sizeof...(Args));
    }
}
```

The internal format view includes the array terminator: the runtime checks it
within the 2,048-byte bound, including nonliteral arrays. The truncation flag
preserves a short unterminated array's real readable extent; a synthetic larger
length must never be used to signal truncation.

The matching Check adapter returns the backend's `false`. Runtime format
pointers are not supported in `_F`; a nonliteral fixed array can still reach the
array overload, so “literal format” is an API rule, not a false claim of compile-
time enforcement. Backend validation is mandatory in all cases.

### Grammar and output contract

The v1 grammar is exactly `{}` in sequence, with `{{` and `}}` for literal braces.
No indexed/named fields, format-specifier mini-language, width, precision,
locale, UTF transcoding, or custom type formatters. Integers print decimal,
booleans `true`/`false`, addresses hexadecimal, floats bounded general notation.
Opaque handles that need hex can use an explicit future scalar wrapper if
evidence warrants it; do not grow a parser for hypothetical convenience.

The parser in `diagnostic_format.cpp` validates braces and argument count at
failure time. Unsupported C++ value types are compile errors; malformed format
text is **not** a compile error in this design. For a malformed format, emit
`[format-error]`, a bounded copy of the original format, and indexed scalar
arguments while space remains. Preserve the original assertion kind/site and
action. Formatting errors never recursively assert, throw, or change `CHECK`
into fatal failure. This explicit tradeoff removes constexpr parsing at every
call site. CI tests every supported pattern; a later offline lint can check
literal field counts without charging all C++ builds.

Use fixed capacities in the runtime, initially:

- 512 bytes for the minimal pre-message header, prioritizing kind, thread,
  file/line, and expression in that order.
- 2,048 bytes for the complete text report, including a terminator and an
  explicit `[truncated]` suffix when needed.
- At most 1,024 inspected bytes per borrowed C string and at most 2,048 inspected
  format bytes; bounded views are also clipped before reads.
- Optional 32 raw stack addresses in the crash packet; zero is a valid result.

These are implementation constants, not installed-header constants. Reserve
space for truncation markers before appending; use subtraction-based bounds
checks to avoid length overflow. Escape embedded NUL/control/newline bytes in
borrowed arguments so one argument cannot forge report records. Source and
literal data get the same output escaping where necessary. Text is treated as
bytes; clipping can split UTF-8, and output consumers must tolerate it.

Use `std::to_chars` **only in the `.cpp`** for numeric conversion, after checking
the pinned library implementation and bounded-buffer behavior. It is a narrow,
nonthrowing conversion primitive, not a formatter: the standard specifies error
codes and no throws for these overloads. This is an explicit addition to ADR
0003's allowed implementation-only facilities, justified by avoiding a custom
floating-point conversion algorithm. See the
[numeric conversion contract](https://eel.is/c++draft/charconv.to.chars).
Do not infer signal safety or an allocation guarantee from that contract;
verify implementation behavior with allocation interception before using it in
the enhanced failure path. The minimal emergency header needs only a tiny
bounded unsigned decimal/hex routine and does not depend on floating conversion.

### Alternatives and costs

| Option | Runtime and binary cost | Build cost | Complexity / disposition |
| --- | --- | --- | --- |
| Literal only everywhere | Smallest runtime and binary; poor value diagnostics | Minimal | Too restrictive for renderer/allocator investigations; retain as default tier |
| C varargs / printf formatting | Bounded output possible, but type mismatches are dangerous; `%n`, widths, C string assumptions need care | Cheap headers; compiler format checking helps but is platform-specific | Conflicts with diagnostics policy as a public API; reject rather than exempt all call sites |
| Existing Logging `std::format` | General type safety, but current backend allocates and can fail fatally in no-exception builds | Public `<format>` cost already measured in this repository | Reject for assertions, including inside failure-only branches |
| `{fmt}` with erased arguments | Mature and capable; careful configuration needed for allocation/error paths | Common compile-time APIs still expose formatter/checker templates | New dependency and capabilities beyond this need; reconsider only with measurements and requirements |
| Small tagged arguments + runtime parser | Fixed stack packing only on failure; one parser/converter implementation | Tiny pack expansion in opt-in header | Selected; closed grammar keeps maintenance bounded |
| `consteval` format validation | Can reject malformed literals early; little runtime difference on success | Parsing/evaluation per format; literal-NTTP designs can duplicate code | Defer; error fallback and tests suffice for diagnostic-only text |
| Deferred closures / async borrowed arguments | Can defer work but risks lifetimes and arbitrary user code after failure | Closure/template specialization per site | Reject; format synchronously, then copy bytes if queued |

This formatter is justified by the combined requirements of no heap, no
exception reporting, a tiny assertion header, and operation during logger
failure. It does **not** justify replacing normal Logging formatting in this
change. A shared formatting facility requires its own measured proposal.

## 10. Failure runtime, concurrency, and reentrancy

Keep all coordination off the success path. In `assert.cpp`, use trivial
constant-initialized TLS state and one process-wide `std::atomic_flag` for the
primary reporting slot. `atomic_flag` provides the required lock-free ownership
primitive; no mutex, dynamic registration, vector, or allocator is involved.
Use acquire on successful test-and-set and release when a Check owner exits.
The packet and hook lifecycle follow that ownership; readers must use an
explicit ready/published protocol, never race with a packet writer.

Begin behavior:

| Case | Action |
| --- | --- |
| First fatal failure | Mark TLS active, acquire primary slot, copy site, publish minimal header; evaluate optional message afterward |
| First Check failure | Mark TLS active, acquire slot; format/report and eventually release it |
| Concurrent Check while slot occupied | Return `false` from Begin without evaluating its message; best-effort increment a lock-free suppression counter |
| Concurrent fatal while slot occupied | Emit its own tiny secondary header if possible, then terminate immediately; do not wait for owner |
| Recursive Check on the same thread | Return `false`, suppress message/hooks, preserve outer entry state |
| Recursive fatal on the same thread | Minimal recursion marker and immediate hard termination; skip formatting, logger, stack capture, and hooks |

Use an atomic counter only if the target supports it lock-free (verified at
build time); otherwise omit suppression counts. A counter is optional telemetry,
never correctness-critical. Every suppressed nonrecursive BeginCheck clears
any TLS entry state it established and releases ownership if it acquired it
before discovering budget exhaustion. A recursive Check leaves the outer TLS
state untouched. This scheme deliberately trades completeness of
simultaneous reports for no application-lock deadlocks. A secondary fatal can
terminate before the primary has finished formatting; the first minimal header
and any committed packet are the available evidence. Do not claim “all failures
are logged” or wait indefinitely for the richest report.

An owner may be a job/render thread holding engine locks. No handoff to the main
thread, renderer, job system, or allocator is needed. Do not stop all threads in
the assertion runtime; a crash collector/debugger owns process-wide capture.

Sequential failed Checks can still flood diagnostics. V1 permits at most 64
detailed Check reports per process, counted under the primary slot; subsequent
checks return `false` with optional suppression counting, no formatting or
breakpoint. This simple process budget may hide a later distinct Check site;
that tradeoff is preferable initially to a per-site registry, timers, or static
atomics at every call site. Fatal failures bypass the budget. Failed Checks are
bugs to fix, not a normal high-frequency reporting channel. Tests can set a
different budget through a private test seam.

This budget suppresses **report presentation only**. Every enabled Check still
evaluates its current condition exactly once: it returns true on success and
false on failure, and the caller's recovery branch still executes on every
failure. Never cache a Check result or skip a condition because its report was
muted. Mark the final permitted detailed Check report with a bounded notice that
later detail will be suppressed; available suppression counts belong in later
collector summaries. This does not solve the acknowledged loss of distinct
later sites. Do not add mutable per-site ignore flags to any assertion macro;
they tax successful calls and introduce shared-state/lifetime concerns.

Conceptual owner flow:

```text
Begin -> copy site, establish TLS/ownership, minimal fatal header
      -> evaluate diagnostic values (failure branch only)
Finish -> bounded formatting into fixed report
       -> commit owned crash/diagnostic packet
       -> emergency byte output
       -> optional bounded mirror / flush attempt
       -> optional raw stack enrichment / crash notification for fatal
       -> if debugger attached and policy permits: breakpoint
       -> fatal: terminate; Check: clear TLS + release slot + return false
```

For a fatal, commit the packet before any optional consumer runs; for a Check,
construct a stack-owned record consumed synchronously or copied by its adapter.
Preserve header/text even
when enrichment fails; set status flags for truncated text, formatting error,
stack unavailable, mirror skipped, and flush incomplete. All internal
validation failures degrade detail using plain branches. Do not use these
assertion macros inside the lowest failure-runtime implementation.

## 11. Logging and emergency output

### Required boundary

Do **not** call `LUDUS_LOG_FATAL`, `LogSystem::ShouldLog`, `LogSystem::Flush`,
`DispatchMessage`, or Logging's thread-name initializer directly from the
failure owner. They can take locks, initialize state, allocate, or reach a sink
that caused the assertion. `noexcept` on a wrapper cannot make them safe.

Extract the final emergency byte writer into Base behind
`diagnostic_output.hpp`. Logging's private `EmergencyLog` keeps responsibility
for its own record construction and calls this shared byte boundary. Assertions
construct their report independently. This extraction is a small, explicit
logging integration change; do not expose Logging's private sink/record headers
in the SDK. Record the shared emergency path in ADR 0003 so ordinary engine
diagnostics still go through Logging and this sanctioned failure path.

The low-level writer must not call logging, assertions, allocation, stdio, or
arbitrary application hooks. Normal output failure is ignored after recording
a status bit; the assertion action is unaffected. Minimal output is usable with
no registered logger or hooks.

### Honesty about bounded output

Replacing `fwrite` with `write` removes stdio's lock but **does not guarantee a
nonblocking write**. Pipes, terminals, files, and debugger output can block.
A timeout checked around a blocking syscall does not bound that syscall.
[The Linux write contract](https://man7.org/linux/man-pages/man2/write.2.html)
also requires handling partial writes and interruptions.

Use an optional preconfigured diagnostic transport with a genuinely nonblocking
try-write contract (for example, a dedicated nonblocking pipe to a collector).
Configuration happens during healthy startup; do not open files, change the
process's shared stderr flags, or allocate a transport during failure. Cap
attempts and handle partial writes/EINTR/EAGAIN without looping indefinitely.
Do not assume one 2 KiB report is atomic on every pipe/OS. Serialize primary
output with the reporting slot; secondary emergency messages may interleave.

For the initial Linux bring-up, direct stderr output is a best-effort fallback
with an explicitly documented blocking limitation. A production shipping port
claiming bounded fatal completion must use the nonblocking transport or skip
unsafe external writes and rely on its committed crash packet/OS crash capture.
When no channel works, termination still occurs. Immediate visibility is
attempted before break/termination; guaranteed visibility in every corrupted
environment is not a satisfiable contract.

### Optional logging bridge

The rare `diagnostic_hooks.hpp` API describes an immutable, versioned hook table
with a context pointer and two operations:

```cpp
enum class DiagnosticDelivery : uint8 { Accepted, Busy, Unavailable };
enum class DiagnosticFlush : uint8 { Complete, TimedOut, Unsupported };

struct DiagnosticRecordView
{
    FailureKind Kind;
    AssertionSite Site;
    uint64 NativeThreadId;
    DiagnosticText Text;
};

// Representative callback signatures, not functions registered per assertion.
using TryPublishDiagnostic = DiagnosticDelivery (*)(
    void* context, const DiagnosticRecordView& record, uint64& ticket) noexcept;
using TryFlushDiagnostic = DiagnosticFlush (*)(
    void* context, uint64 ticket, uint64 monotonicDeadlineNs) noexcept;
```

The view contains kind, copied site fields, native thread ID, and pointer/length
text. A successful publish must synchronously copy all needed bytes into bounded
owned storage; it may not retain argument arrays, caller stack buffers, or site
pointers into an unloadable module. Both operations must have no unbounded
locks, allocation, logger recursion, user callbacks, or blocking I/O on the
failure thread. Deadline units are native monotonic nanoseconds, not wall clock.
“TimedOut” cannot be implemented by timing out a thread still writing borrowed
memory; no detached worker may retain the view.

The current synchronous logger does not meet this contract. **Initially install
no normal-logger hook**, share the hardened emergency writer, and keep the crash
packet. Do not label the current `Flush()` a best-effort bounded flush. This is
a real limitation of current logging, not a hidden assumption in the design.

When async logging lands, implement the bridge through its preallocated bounded
queue and a diagnostic bypass lane if the normal queue is full. A failed
assertion bypasses category/severity filtering. Publish returns a ticket ordering
the report after previously accepted log records; flush waits for that ticket's
sink completion up to **50 ms total** across optional consumers. It must detect
the logging worker and logger/sink reentrancy and return `Unsupported` immediately
there. A full diagnostic lane returns `Busy`; emergency output remains primary.
No assertion thread steals the logger's queue-consumer role or drains sinks
while holding unrelated engine locks. Flush is bounded best effort, never a
prerequisite for termination. A hung sink prevents acknowledgment but not
assertion termination.

Install the table before starting workers, with process-lifetime table/context
storage. No concurrent replacement, unregistration, hot unload, or mutable hook
registry in v1. A logger shutdown sets the adapter's own availability state;
its storage remains valid, and it returns `Unavailable` after shutdown. The
default is a null table. Tests inject through a private seam before spawning
their process/threads. This keeps lifetime management out of every report.

## 12. Debugger and platform behavior

Private backend contract:

```cpp
enum class DebuggerState : uint8 { Attached, Detached, Unknown };

DebuggerState QueryDebugger() noexcept;
void BreakForDebugger() noexcept; // deliberately may return on debugger resume
[[noreturn]] void TerminateForAssertion() noexcept;
[[noreturn]] void TerminateImmediately() noexcept;
```

Query only after failure; never poll on success or cache debugger presence at
engine startup. Attach/detach is possible later. `Unknown` behaves as detached.
Detection cannot atomically synchronize with debugger detachment; a breakpoint
may become a fatal OS exception/signal in that race. No dialog or wait-for-attach
loop is part of the engine API.

| Platform | Detection | Inspection breakpoint | Fatal termination |
| --- | --- | --- | --- |
| Linux / Clang 18 reference | Bounded read/parse of `/proc/self/status` `TracerPid`; inaccessible/truncated information gives Unknown | Guarded `__builtin_debugtrap()` | `abort()` after report/crash notification; immediate `_Exit` path for recursive/compromised reporting |
| Windows port | `IsDebuggerPresent()` in private OS TU | `__debugbreak()` | `RaiseFailFastException`, with immediate process termination fallback if unexpectedly returned |
| macOS port | Private debugger-query adapter using supported SDK facilities; Unknown until validated | Clang debugtrap when supported | POSIX abort/immediate-exit backend, validated with crash collector |
| Future console | Vendor-supported debugger/crash APIs in private adapter | Vendor break | Vendor crash primitive |

Linux exposes tracer identity via
[`TracerPid`](https://man7.org/linux/man-pages/man5/proc_pid_status.5.html);
it indicates tracing, which need not be an interactive debugger. A trace-aware
CI configuration should suppress inspection breaks through startup diagnostics
policy, without weakening fatal termination. Windows provides explicit
[debugger detection](https://learn.microsoft.com/en-us/windows/win32/api/debugapi/nf-debugapi-isdebuggerpresent)
and the [breakpoint intrinsic](https://learn.microsoft.com/en-us/cpp/intrinsics/debugbreak?view=msvc-170).

Do not substitute `__builtin_trap()` for a resumable inspection breakpoint.
Use it only as an abnormal-termination primitive in a validated backend.
Do not assume x86 `int3` encoding or use inline assembly in public headers.
Break inside a noinline cold runtime function; the debugger can inspect the
stored site and walk to the caller. Keeping the breakpoint literally at each
source expansion adds platform machinery/code per site. That cost does not make
debugger usability optional: verify that developers can identify the original
assertion line and select its caller frame, including available locals, in Debug
and optimized Development builds. Test optimized stack quality and disable
sibling-call optimization in the small runtime if needed; debugger skip/step
annotations are preferable to duplicating breakpoint policy in macros. If the
reference debugger cannot provide usable caller context, reconsider breakpoint
placement with measured code/header costs. Source metadata remains authoritative
when optimization removes frames or locals; do not promise full local-variable
recovery in optimized code.

Without a debugger: report and terminate for `REQUIRE`/`FATAL` failures; report
and return false for Checks. There is no unconditional trap for a recoverable
Check. Continuing from an inspection breakpoint in a `REQUIRE`/`FATAL` report
proceeds to termination, never to the invalid operation.

The one exception is an enabled `ASSERT`/`ASSERT_F` under the interactive-
development policy ([§5.1](#51-interactive-development-resumable-assert),
[ADR 0006](../decisions/0006-resumable-development-assertions.md)): in a local,
non-CI Debug build, continuing past the inspection breakpoint of a failed
`ASSERT` returns to the caller instead of terminating, and when no debugger is
attached a separate Continue-once / Terminate prompt offers the same choice.
This requires two additional private, detached backend capabilities beyond the
table below — a **CI-environment query** and an **interactive Continue-once /
Terminate prompt** — both Debug-only, both on the failure path only, and both
degrading to Terminate when unavailable (headless, no terminal, absent helper).
They must not poll on success, must not be a public-header dependency, and must
sit at the same private OS boundary as `QueryDebugger`/`BreakForDebugger`.
`BreakForDebugger` already "deliberately may return on debugger resume"; §5.1
defines what returning means per kind. For `REQUIRE`/`FATAL`, and for `ASSERT`
outside the eligible Debug scope, a returned break still falls through to
termination.

Linux/Clang remains the reference platform. macOS now has the native sysctl
query/debugtrap/termination adapter and helper transport described in the
[diagnostic launch guide](../development/diagnostics.md#macos-native-tooling),
with native startup and explicit-decision regression coverage. The historical
Windows row remains a port contract, not a claim of tested support. Unsupported backend
selection must fail configuration; silent no-op fatal stubs are forbidden.

## 13. Crash packets, stack traces, and termination

Reserve one static primary **fatal** packet with a format version, build ID, failure kind,
native thread ID, bounded owned text/site strings, and optional raw PCs. Copy
the data; do not retain caller-owned message pointers. Publish completeness
through lock-free state with acquire/release ordering. A crash reader may use
only the last committed stage; it must not read a partially written buffer.
Use three separately published, immutable regions: minimal site/header, complete
report, then optional stack/delivery-status enrichment. Each region is written
once and published with a release store; readers check readiness with acquire
loads before touching that region. The fatal owner never releases the slot or
reuses packet storage. Checks use stack records and never publish into this
packet, avoiding reuse races with a concurrent crash reader. A secondary fatal
does not overwrite it. This is a single terminal incident, not a reusable
lock-free history buffer. Core/minidump readers outside the live process must
also validate region readiness/version rather than infer completeness.

Initially, capture no in-process backtrace unless a backend is proven bounded
and allocation-free. Preserve the site plus the native abort/fail-fast crash
context; the debugger/core/minidump captures the failing thread's stack. The
enhancement hook may fill up to 32 PCs, with zero indicating unavailable.
Never call `std::stacktrace`, symbolizers, loader enumeration, or general
`backtrace_symbols` from this path. Native unwinding can allocate or lock even
when its output buffer is supplied; validate each implementation rather than
assuming safety. Do not walk arbitrary frame-pointer chains without bounds.

Build ID, retained symbols/unwind information, and module mappings collected
at healthy startup enable offline symbolication and ASLR relocation. Frame
pointers improve some capture paths but impose whole-program runtime cost;
make their use a measured platform build policy, not a hidden assertion flag.
Symbol files and source mapping must be archived per shipping build.

### Report handoff without a debugger

The complete text report must be copyable as one bounded plain-text block. Include
report-format version, build ID and build flavor, assertion policy, failure kind,
native thread ID, repository-relative file/line, function, expression (or an
explicit unconditional marker), message, and capture/truncation status. Required
identity fields precede optional message detail so a long value cannot displace
the build or site. No address-only or message-only report counts as useful
capture. Preserve the expression even when a descriptive message is provided.

Generate that block once from the report's owned fields using the existing
bounded writer; emergency output and optional consumers reuse its bytes. These
fields do not introduce formatting templates or includes at assertion sites.
Later delivery/stack status belongs to the separately committed enrichment
region; a collector may append it without mutating the original report. Keep
explicit `unavailable` and `truncated` indicators, including when stack capture
was not attempted. Tools can group incidents by the tuple of build ID, relative
file, line, and expression; v1 needs no runtime hashing or persistent site registry.

A launcher, external crash collector, or healthy editor tool may save, display,
and copy an **owned** report to a bug tracker. Clipboard access, dialogs, file
pickers, and uploads do not belong on the failing thread or in Base. UI failure
must not change termination/recovery behavior. The core runtime does not need
to implement that UI to provide a useful artifact.

For a QA deployment, verify end to end that a failure with no debugger leaves a
retrievable report associated with the exact executable/symbols. A stderr line
that disappears when a graphical application closes does not satisfy this
operational acceptance test. The deployment must configure a collector, retained
launcher output, or suitable OS crash artifacts; the core's best-effort transport
alone cannot guarantee persistence. Caller stacks are particularly valuable for
shared low-level helpers: test symbolication through a caller/helper chain when
crash capture is configured. If the deployment has no safe stack capture, state
that limitation visibly instead of attempting unsafe symbolication during failure.

A future crash collector may register one process-lifetime, nonblocking
notification callback in the rare hooks table. It receives the committed packet
and copies/notifies using preallocated storage/IPC. It must not upload, symbolize,
walk all threads, allocate, or block in this call. Call it before final termination;
if Windows fail-fast bypasses the reporter's exception handlers, this explicit
notification is the integration point. External collection/upload runs later.

`abort()` gives a conventional POSIX abnormal termination signal, can produce
a core depending on host policy, and does not run normal C++ destruction or
`atexit` shutdown. Do not depend on it to flush stdio/logging; modern glibc does
not flush streams in `abort`. See [abort behavior](https://man7.org/linux/man-pages/man3/abort.3.html).
A returning SIGABRT handler does not turn the assertion into a recoverable path,
but a stuck application signal handler can still stall normal abort. The
recursive/compromised path uses immediate exit and accepts losing a core. A
shipping port needing a stronger fail-fast guarantee must validate its native
termination/watchdog contract; no C++ declaration can guarantee progress under
an arbitrary hung handler/kernel.

Windows fail-fast is an OS exception mechanism, not a C++ `throw`; it bypasses
frame/vector exception handlers and can invoke WER. That distinction matters
to reporter integration. See
[RaiseFailFastException](https://learn.microsoft.com/en-us/windows/win32/api/errhandlingapi/nf-errhandlingapi-raisefailfastexception).

Do not call `exit`, logger shutdown, destructors, job joins, GPU idle waits, or
allocator cleanup on a fatal path. The state that makes cleanup safe may already
be broken. General assertions are not async-signal-safe: actual SIGSEGV/stack
overflow handlers need a separate minimal backend, possibly an alternate stack,
that only references committed data and uses approved OS operations.

## 14. Runtime and build-time performance contracts

### Passing code

At an optimized plain site, expect the equivalent of:

```text
compare/test condition
conditional jump to cold failure block
continue ordinary code

cold failure block:
    materialize site arguments
    call BeginFatal
    call FinishFatal
```

There is no success-path global/TLS read, atomic operation, static initialization,
clock, debugger query, message evaluation, or function call introduced by the
assertion system. A condition itself can of course be expensive or perform a
call. `[[unlikely]]` is a compiler layout hint, not a hardware predictor guarantee.
Keep failure functions cold/noinline where supported and verify with LTO as well
as ordinary optimized builds. Do not force-inline the formatter/handler.

The compiler can fold known-true enabled conditions. `REQUIRE` retains a real
guard when needed; its nonreturning failure arm may legitimately allow
optimization after the guard. Disabled `ASSERT` must never become `assume` or
unreachable: that would introduce undefined behavior instead of removing a
diagnostic. Both `_F` and base forms evaluate no message arguments on success.
Failure-only argument packing can still influence stack-frame layout at `-O0`
or under some optimizers; do not promise identical stack frames or zero code
size. Inspect optimized output, especially large argument arrays.

### Header and template budget

- `assert.hpp`: target below 200 non-comment lines; no function templates and
  no STL includes beyond `types.h`'s two existing alias headers.
- `assert_format.hpp`: target below 300 non-comment lines; fixed scalar
  overloads, one pack adapter per return category, and only the few array/
  rejection helpers needed for safety. No recursive template metaprogramming.
- Each enabled plain site adds a branch, metadata references, and cold calls.
  It does not instantiate a handler template. Each `_F` site may instantiate
  a small pack adapter; type erasure prevents per-site parser/converter code.
- Every disabled site disappears at preprocessing expansion. A TU that includes
  the format header still pays its small parse cost even with assertions off.
- Format-parser changes rebuild one implementation TU and relink. Handler or
  OS-backend changes do likewise. Changes to hook structures rebuild only rare
  integration consumers. A hot-header edit necessarily invalidates its users.
- No PCH, module, unity build, or linker folding is required for acceptable
  performance. Such tools can improve measurements but cannot excuse a heavy
  public API.

### Measurement gates, before implementation is accepted

Use pinned Clang 18, fixed host/load/options, cache disabled, and record medians
of five runs plus variance. Create synthetic TUs with 0, 100, 1,000, and 10,000
sites: plain, literal-message, repeated formatted type packs, and diverse packs.
Include no-site/header-only baselines and the current logging header as a
comparison. Measure frontend parse/instantiation time with `-ftime-trace`, peak
compiler RSS, backend time, object `.text`/`.rodata`, and linked binary sections.
The comparison is not a claim that logging and assertions have identical scope.

Initial acceptance targets (proposed budgets, not measured results):

1. An empty TU including `assert.hpp` adds at most 10 ms median frontend time
   on the reference host; if run noise exceeds that, use aggregate many-TU
   measurements and report uncertainty instead of inventing precision.
2. At 1,000 plain enabled sites, frontend time stays within 15% of equivalent
   handwritten condition/metadata/cold-call code. At 1,000 formatted sites,
   stay within 25% of equivalent handwritten tagged-array code.
3. No public-header `<format>`, `<filesystem>`, `<chrono>`, `<atomic>`, or OS
   includes; no parser/converter instantiations in consumer trace reports.
4. Disabled sites leave no report call or unique diagnostic string in an
   optimized object. Passing optimized sites contain no runtime coordination.
5. Repeated-type formatter text growth must track only packing/call-site work,
   not copies of the parser; measure diverse type packs separately.
6. Touching only `diagnostic_format.cpp` rebuilds that object and relinks without
   recompiling consumers. Test `assert.hpp` and format-header touches separately.

Use the existing `scripts/profile-build` for an engine-wide before/after result;
it deletes its selected build tree, so use a dedicated profiling preset/tree.
If budgets fail, simplify the adapter/header before introducing PCH or replacing
the formatter. Record actual numbers in a follow-up decision; this document
makes no unmeasured “zero build cost” claim.

## 15. Standard-library dependency decisions

| Facility | Location | Runtime / binary | Parse / portability / API rationale |
| --- | --- | --- | --- |
| `<cstddef>`, `<cstdint>` | Existing `types.h` only | Aliases, no runtime work | Established portable widths; do not duplicate them |
| `<atomic>` | Runtime `.cpp` only | Failure-only flag/counter operations; no allocation | Standard correctness instead of custom compiler atomics; require lock-free properties where needed |
| `<charconv>` / `to_chars` | Formatter `.cpp` only | Bounded conversion/error return; float code can add binary weight, measure it | Compile once; avoids inventing numerical conversion; audit library behavior |
| `<cstdlib>` or platform equivalent | Termination `.cpp` only | One fatal operation | Well-defined normal abort/immediate-exit entry; OS-specific stronger path where needed |
| `<string_view>` | Existing logger/bridge internals, if already needed | Nonowning; no allocation | Acceptable in principle, but no reason to expose it in this new narrow assertion ABI |
| `<source_location>` | Existing Logging only | Usually metadata pointers, implementation-defined layout | Not inherently heavy; rejected here because the required macro already captures a simpler explicit record |
| Containers, strings, smart pointers, `<format>`, `<sstream>`, `<iostream>` | None in assertion subsystem | Avoid allocation/uncontrolled failure/binary machinery | Unnecessary API surface and include cost |

This is selective standard-library use, not a ban on the library. Atomics and
numeric conversion solve concrete correctness problems; strings, generic
formatters, and handler containers do not improve this subsystem enough to
justify their costs. A closed diagnostic tagged union is materially smaller
than inventing an engine-wide type-erasure or variant abstraction.

## 16. Failure modes and edge cases

| Case | Required behavior |
| --- | --- |
| Pre-init, post-shutdown, static destructor | Base runtime requires no logger lifetime; emergency report still attempted |
| Assertion inside logger/sink/allocator | No normal logger call; use independent runtime and emergency transport |
| Message argument asserts recursively | Begin has already guarded the site; recursive fatal exits, recursive Check returns false |
| Resumable `ASSERT` continued (Debug, local, non-CI, explicit action) | Return to caller after committing the report; release slot/clear TLS like a Check; invariant now known broken (§5.1) |
| `ASSERT` failure while headless / no terminal / helper absent | No prompt possible; outcome is Terminate, never default-continue |
| `ASSERT` failure detected under CI (even a local Debug binary) | Runtime CI veto: no prompt, no resume-to-caller; terminal fatal path |
| Recursive/contended `ASSERT` in resumable build | Never resumable; immediate secondary/recursive termination with no prompt |
| Message argument dereferences corrupt pointer | May fault; minimal fatal header already committed; no portable C++ recovery promised |
| Null `DiagnosticCString` / null base message | Explicit null marker for wrapped string; omitted message for base null |
| Invalid non-null text pointer | Caller contract violation; bounded length does not validate memory mapping |
| `DiagnosticText{nullptr, nonzero}` | Render `[invalid-text]` without dereference |
| Huge length/unterminated string | Cap reads/output; safe arithmetic; explicit truncation |
| Unsupported formatting type | Compile error; explicit scalar/view conversion required |
| Format error / output exhaustion | Retain primary site/action, print bounded marker/fallback |
| Output unavailable, partial write, disk full | Stop bounded attempt, set status; never recursively report output errors |
| Full logger queue / hung sink | Drop mirror or timeout; emergency/crash packet precedes it |
| Debugger detach race | May terminate on breakpoint signal; no continuation guarantee |
| Signals or stack exhaustion | Use separate OS crash entry, not normal macros |
| Floating NaN/Inf/negative zero | Stable bounded formatter output; test chosen library representation |
| Fatal in a destructor or lock scope | Terminate without unwinding/cleanup; do not unlock application state |
| Dynamic module unload | Hooks forbidden to unload; queued records own text/site bytes |
| Multiple Base runtimes in DLLs | Unsupported; fix linkage to one runtime before enabling plugins |
| Assertion expression calls expensive code | Caller owns that runtime cost; only `ASSERT` may compile it away |
| Side-effectful diagnostic expression | Unsupported usage even though evaluated at most once; may be skipped |
| Release-only unused variables | Use variables for real work or `[[maybe_unused]]`; do not retain disabled condition evaluation just to silence warnings |
| Null `UniquePtr` destruction | Valid C++ ownership behavior; remove the existing TODO suggesting it should assert/log, do not implement that TODO |

## 17. Validation strategy

Tests must check behavior and failure isolation, not merely duplicate the macro
implementation. Use Catch2 where appropriate, but execute fatal paths in child
processes. Never implement fatal testing by throwing through a production
`noexcept` boundary or by replacing a `[[noreturn]]` function with a returning one.

1. **Evaluation and preprocessing:** Debug/Development/Profile/Release fixtures
   prove zero/one condition evaluations, message evaluation only on failure,
   no evaluation of suppressed reports, explicit-bool conversion, dangling-else
   safety, nested macro stringification, comma expressions, no-message variants,
   `_F` with zero/eight/nine arguments, and constexpr passing/failing cases.
   Disabled fixtures may intentionally reference an undeclared condition/message
   identifier to prove arguments are not semantically compiled.
2. **Type/format safety:** Compile-fail fixtures for unsupported types/pointers
   and over-limit packs. Test every scalar width, signed minimum, maximum unsigned,
   LP64/LLP64 overloads, null strings, embedded control bytes, brace escapes,
   malformed/mismatched formats, long data, and buffer boundary cases. Fuzz the
   non-template parser with valid bounded memory under ASan/UBSan.
3. **Allocation:** Intercept allocation in isolated binaries; first-ever failure,
   formatted failure, pre-init and post-shutdown must not allocate in Base's
   minimal/enhanced approved path. Test float conversion separately. No-exception
   compilation is mandatory for the production library even though tests use Catch2.
4. **Fatal behavior:** Child-process tests verify nonzero abnormal termination,
   expected primary diagnostics, no normal destructor/atexit cleanup, and
   actual native signal/exception behavior. Redirect to a bounded known-good
   test transport; do not accidentally block on a full test pipe.
5. **Debugger policy:** Private backend fakes record query/break/terminate order
   and ensure resuming a breakpoint still terminates. Real LLDB/GDB/Windows
   debugger smoke tests validate stopped stack/site and detached behavior.
   Fake backends do not replace native death tests.
6. **Concurrency:** Barrier-start simultaneous failures, recursive callbacks,
   failures from logger worker/sink and while application locks are held; check
   suppression behavior, packet publication, no owner wait, and bounded exit.
   Use subprocess watchdogs and TSan where supported. Check budget exhaustion
   must not suppress a later fatal. After exhaustion, exercise changing Check
   conditions and prove that every false result still enters recovery and every
   true condition still returns true; verify the suppression notice.
7. **Transport and integration:** Full queues, unavailable hooks, shutdown races
   within the supported fixed-lifetime contract, partial/interrupted writes,
   nonblocking EAGAIN, dropped mirrors, and unsupported/timed-out flushes. Verify
   a retained record owns its bytes after a Check returns.
8. **Binary/build/SDK:** Execute the measurement gates above, inspect assembly
   and strings, run an installed consumer for each assertion policy, ensure
   private platform/sink headers are not exported, and verify no dependency cycle.
9. **QA handoff:** Trigger an invariant in a shared helper from two distinct
   callers, with no debugger attached. Retrieve the bounded report, match its
   build/flavor/site to archived symbols, and inspect a caller stack where the
   deployment supports it. Verify text remains usable after copy/save, missing
   stack/truncation is explicit, and collector/UI failure cannot block termination.

For implementation, run the repository's warning-clean build/unit checks,
format/tidy with pinned version 18, ASan/UBSan, and SDK install/consumer checks.
Add Release-policy test executables even though the current Release preset
normally disables unit tests; configuration-matrix compile/death tests cannot
be skipped merely because production tests are off. Document debugger and
platform manual checks separately from automated results.

## 18. Implementation and migration sequence

1. **Record the policy.** Accept this design or a focused ADR. Explicitly amend
   ADR 0003 for implementation-only numeric conversion and the shared emergency
   boundary. Resolve Profile flavor/config export and the small compiler helper
   split first. Do not refactor unrelated Base utilities.
2. **Deliver the smallest vertical slice.** Add `assert.hpp`, generated policy,
   Linux failure runtime and emergency output; plain `ASSERT`, `REQUIRE`, `CHECK`,
   `FATAL`; death/evaluation tests; install the public headers. It works without
   logging or crash hooks. Keep the two-stage guard from the first version.
3. **Harden failure handling.** TLS recursion guard, atomic ownership, bounded
   packet stages, Check suppression budget, debugger tests, and the private
   nonblocking transport contract. Share the emergency writer with Logging.
   Explicitly record whether a deployment still uses blocking stderr fallback.
4. **Add opt-in formatting.** Closed tagged arguments, tiny pack adapter,
   parser/converters in `.cpp`, compile-fail and fuzz tests, allocation audit.
   Run compile/binary benchmarks before spreading formatted call sites.
5. **Add narrowly scoped call sites.** Start with actual ownership, bounds, and
   state invariants. Keep ordinary window/file/device errors as return values.
   Audit required side effects and recovery branches. Remove the misleading
   null-deletion TODO in `pointer.hpp`; do not assert on a valid null deletion.
6. **Integrate richer diagnostics when available.** Immutable hook table,
   validated crash collector, symbols/build IDs, and optional raw-PC capture.
   Add logger publish/flush only when its implementation meets the stated
   nonblocking lifetime contract; current synchronous `Flush` does not.
7. **Port and measure.** Windows/macOS/console implementations require native
   failure and debugger tests before support is advertised. Maintain compile
   cost/binary-size baselines and one runtime across eventual shared modules.

An implementation is not production-complete merely because the macros compile.
Its target deployment must pass fatal/reentrancy tests, demonstrate allocation
and build-cost properties, and honestly state its transport/crash-capture
guarantees. Rich logging and symbolication are optional; safe fatal control flow
and independent minimal diagnostics are not.

## 19. Decision rationale and complexity budget

| Decision | Problem solved | Realistic alternative | Runtime/build cost | Complexity and fit |
| --- | --- | --- | --- | --- |
| Runtime inside Base | Base/allocator/Logging must assert without upward dependencies | New Diagnostics target below everything | Only link runtime when referenced; no added target graph layer | A few OS TUs fit current small engine; split later only for real layering pressure |
| Fatal invariants, separate Check | Continuing through broken assumptions is unsafe | Ignore/continue dialogs or universal boolean assertions | Fatal cold path; Check branch in all builds | Explicit control flow with no runtime decision framework |
| Four names plus `_F` | Balance usable diagnostics and hot-header cost | One overloaded all-purpose macro | Plain sites template-free; formatting opt-in | Slight naming cost buys a clear include/build boundary |
| Manual site aggregate | Expression macros already required | `std::source_location` | No extra include; metadata strings still cost bytes | Transparent and adequate; not ideological rejection of a small STL facility |
| Closed tagged formatter | Useful values without allocation/heavy headers | printf, std::format, fmt, literals only | Failure-only packs; one runtime parser; float binary cost measured | Small grammar avoids becoming a formatting framework |
| Runtime grammar validation | Avoid per-call compile-time parsing | consteval checks | Tiny failure-path validation; cheaper TU processing | Explicitly weaker literal diagnostics, compensated by deterministic fallback/tests |
| Begin before message evaluation | Recursive/faulting diagnostic expressions | Single reporting function | One extra cold call; small TLS only on failure | Worth the narrow two-stage internal protocol; no generic scopes/closures |
| Single nonwaiting report slot | Concurrent failures and locked worker threads | Global mutex / wait for first handler | Failure-only atomic operation; secondary reports may be lost | Predictable failure rather than synchronization complexity |
| Independent emergency channel | Logging itself may fail or be unavailable | Always log-and-flush | Bounded text copy/output attempt; no logging header | Required safety boundary; shared final writer limits duplication |
| Null initial logging hooks | Existing logger cannot satisfy bounded flush | Wrap current Flush with a timer | No unsafe call; fewer persistent log records on fatal | Honest integration gap, resolved with future async backend rather than fake safety |
| Optional committed crash packet | Preserve evidence without safe in-process unwinding | Symbolicate on failing thread | Fixed static storage; failure-only copies | Separates evidence collection from expensive crash processing |
| Explicit generated policy | Profile mismatch, NDEBUG ambiguity, installed SDK drift | Header fallbacks / per-TU defines | Policy changes rebuild consumers, intentionally | One authoritative variant contract prevents silent ODR/configuration mistakes |

The recommendation keeps the always-included surface almost entirely language
syntax and declarations. Its custom pieces have specific engine requirements:
bounded diagnostic text, independent failure handling, and a controlled ABI.
It reuses standard atomics and numerical conversion privately where doing so is
safer and cheaper than inventing equivalents. The implementation remains small
because it refuses resumable fatal invariants, arbitrary formatters, handler
registries, and promises that the current logger cannot fulfill.

## 20. Validation performed for this design

The representative site/base-macro declarations were extracted into a temporary
standalone probe and compiled with Clang 18, C++23, `-fno-exceptions`, and
warnings-as-errors. Enabled and disabled probes passed condition/message
evaluation, suppressed Check messages, explicit-bool conversion, constexpr
success, dangling-else, and disabled undeclared-argument checks. Document-local
links, code fences, and whitespace were checked. These probes validate the
proposed macro boundary only; the runtime, formatter, crash transports, platform
ports, and performance budgets remain implementation work. No full engine build
or sanitizer run was performed for this documentation-only change.

The subsequent GPG1 review visually inspected the complete chapter (printed
pages 109-114; PDF pages 107-112). Its findings and a small modern-C++ probe are
recorded in [the review](assertions-rabin-review.md). No historical sample was
adopted as engine implementation.
