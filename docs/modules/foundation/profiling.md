# FoundationProfiling

CPU trace instrumentation for the Ludus engine. Implements **Phase 0 (shared
clock)** and **Phase 1 (CPU Trace MVP + Perfetto export)** of the finalized
[profiling architecture](../../architecture/profiling-final.md).

## What this module provides now

- A shared FoundationTime monotonic clock (`NowTicks()`, integer nanoseconds).
- Ubiquitous, cheap-to-include instrumentation macros:
  - `LUDUS_PROFILE_SCOPE(Name)` / `LUDUS_PROFILE_SCOPE(Name, Category)` — RAII CPU scope.
  - `LUDUS_PROFILE_FUNCTION()` — scope named from `__func__`.
  - `LUDUS_PROFILE_FRAME()` — frame boundary marker (advances the frame epoch).
  - `LUDUS_PROFILE_MARK(Name)` — zero-duration instant marker.
  - `LUDUS_PROFILE_FLOW_OUT(Name)` / `LUDUS_PROFILE_FLOW_IN(id)` — cross-context links.
- Per-thread recording into fixed, pre-reserved chunk buffers. Writing an event
  takes no lock; a full chunk is handed over under one recorder mutex, once per
  4,096 events.
- Capture lifecycle (`BeginCapture` / `EndCapture`) and Chrome/Perfetto JSON
  export (`ExportPerfettoTrace`) — open the file in <https://ui.perfetto.dev>.
  The export names each registered thread's track.
- An [integration emission API](#integration-emission-api) for runtime systems,
  such as the job scheduler, that identify their work at run time.

## Capture lifecycle contract

- `BeginCapture(maxChunks)` reserves one slab of `maxChunks` 96 KiB chunks from
  the system allocation domain at the chunks' 64-byte alignment, using checked
  size arithmetic. It returns `false` and leaves capture disarmed when a capture
  is active, the size overflows or the reservation fails. It discards any events
  from a previous capture that were not exported.
- `EndCapture()` joins the capture. It disarms recording, waits for producer
  threads that are writing an event, and publishes every thread's partial chunk.
  All events accepted before it returns are exported, including those of threads
  that are still running or that exited during the capture.
- Thread exit (or `UnregisterThreadForTrace`) publishes the thread's partial chunk.
- `BeginCapture`, `EndCapture` and `ExportPerfettoTrace` are control calls. Call
  them from one coordinating thread, or serialize them yourself. Producers may
  keep emitting across any of them.
- Site and thread registration publish each table entry completely before it
  becomes visible to the exporter. A site that several threads reach for the
  first time is registered once.

## Usage

```cpp
#include <ludus/foundation/profiling/profiling.hpp>      // macros (cheap)
#include <ludus/foundation/profiling/trace_system.hpp>   // control (app layer only)

using namespace ludus::foundation::profiling;

RegisterThreadForTrace("Main");
BeginCapture(/*maxChunks=*/64);

void RenderScene() {
    LUDUS_PROFILE_FUNCTION();
    LUDUS_PROFILE_SCOPE(ShadowMaps);
    // ... work ...
}
// per frame: LUDUS_PROFILE_FRAME();

EndCapture();
ExportPerfettoTrace("trace.json");
```

For scheduled captures of engine applications and benchmark harnesses, see
[Runtime profiling](../../development/runtime-profiling.md).

## Integration emission API

`<ludus/foundation/profiling/integration.hpp>` exposes the per-thread recorder
through plain functions for code that receives site IDs as data instead of
declaring a static site per lexical scope. `RegisterSite` records a descriptor's
name once; `BeginSite`/`EndSite` open and close a slice by site ID;
`StartFlow`/`FinishFlow` link slices with a caller-owned flow ID, which must set
`CALLER_FLOW_ID_BIT` so it cannot collide with `LUDUS_PROFILE_FLOW_OUT` IDs; and
`SampleCounter` records a timestamped counter value, exported as a Chrome counter
event (`"ph":"C"`). Counter samples are trace-timeline events, not the planned
Phase 2 per-frame counters and gauges. Every function is a relaxed load and branch
while no capture is active and never allocates.

The [job scheduler's profiler adapter](threading.md#execution-traces) is the first
user: it records Submit/Wait slices, one slice per job, dispatch flows and a
ready-queue counter.

## Memory gauges

`<ludus/foundation/profiling/memory_gauges.hpp>` declares `RecordMemoryGauges()`.
During a capture it samples every registered FoundationMemory allocation domain
on the calling thread's timeline. `ExportPerfettoTrace` writes each sample as one
`Memory/<domain>` counter event and adds a coverage manifest under the trace's
`metadata` object. Call it at a boundary where the application has stopped
allocation in every registered domain if the values must reconcile exactly.

```cpp
#include <ludus/foundation/profiling/memory_gauges.hpp>

// After the frame's workers have finished:
(void)ludus::foundation::profiling::RecordMemoryGauges();
```

Domain accounting belongs to FoundationMemory and stays on when
`LUDUS_PROFILING_ENABLED` is off. The metric, validity and coverage contracts
are in [memory profiling phase 1](../../development/memory-phase-1.md).

## Build flavors

`LUDUS_PROFILING_ENABLED` is SDK-owned (generated `profiling_config.hpp`) and
derived from the build flavor: **on** for Debug/Development/Profile, **off** for
Release. In a disabled build every macro expands to `((void)0)` and evaluates no
argument — verified by `tests/disabled_tests.cpp` and by inspecting generated
code (gate G1).

## Design boundaries (unchanged from the architecture)

- The event stream is the ground truth; the hierarchical tree, inclusive/
  exclusive time and calls-per-frame are **derived off-line** (by Perfetto or a
  future analysis pass), never stored on the hot path.
- Visualization is external (Perfetto now, Tracy later). No in-engine timeline.
- Counters/gauges (Phase 2), hitch capture (Phase 3), flow correlation logic
  (Phase 4), GPU trace (Phase 5, needs an RHI) and allocation recording beyond
  the domain gauges above are **not** in this module yet, by design.

The proposed [memory profiling architecture](../../architecture/memory-profiling.md)
defines a distinct bounded memory recorder, domain-baseline and loss contracts,
and staged integration. Its [reference review](../../architecture/memory-profiling-gems-review.md)
documents the supporting reading. Only its phase 1 domain gauges are
implemented; the CPU stream does not record allocation lifetimes.

## Implementation notes / deliberate MVP adjustments

These are the smallest defensible deviations from the architecture document,
recorded per the implementation brief:

1. **Synchronous drain instead of a dedicated collector thread.** For Phase 1 —
   explicit capture *sessions*, not the continuous rolling ring of Phase 3 — the
   collector drains on the calling thread at export time. A background collector
   thread buys nothing here and adds join/lifetime surface; it lands with Phase 3,
   where continuous capture needs it.
2. **Mutex-guarded chunk handoff and an explicit producer join.** Memory
   profiling [phase 0](../../development/memory-phase-0.md) replaced the
   lock-free chunk stacks, which were ABA-prone with several concurrent free-list
   poppers. They also gave `EndCapture` no way to know that producers had stopped
   writing, so it could not safely publish their partial chunks. While capture is
   armed, each event still costs a thread-local chunk write, but it now also
   sets and clears that thread's in-flight flag with a sequentially consistent
   store. The design's "no atomics on the common push" no longer holds; the
   phase 0 page records the measured per-event cost.
3. **No dependency on FoundationLogging.** To keep the FoundationBase-only
   dependency direction (AGENTS.md), profiling keeps its own thread-identity
   table rather than reusing the logging TLS identity.
4. **`TraceEvent` is a single 24-byte record** with an `Aux` field carrying the
   optional flow id or category id. The design's separate side-channel stream is
   deferred until the event-size measurement (gates G4/G5) says it is worth it.
5. **`LUDUS_PROFILE_FUNCTION` builds its descriptor at runtime** (`constexpr`,
   not `consteval`) because `__func__` is not a constant expression; the cost is
   two `string_view` assignments, and site registration still happens once.

## Validation status

The `clock`, `scope`, `export` and `disabled` tests cover the §11 correctness
matrix (single/nested/recursive/early-return/multi-thread/frame-boundary/
incomplete) and gates G1 (disabled = no evaluation), G3 (clock monotonicity),
and G13 (incomplete scopes exported as `"incomplete":true`). The `recorder`
tests cover slab alignment and overflow, joins with live and exiting threads,
stop/restart while producers keep emitting, concurrent drain/recycle and
concurrent registration; run them under ASan/UBSan and TSan. An export test
checks integration slices, matched caller flows, counter samples and thread-name
records. The pinned toolchain
(Clang 23 + Conan + Catch2) runs the authoritative warnings-as-errors, ASan/UBSan
and build-budget checks in CI.
