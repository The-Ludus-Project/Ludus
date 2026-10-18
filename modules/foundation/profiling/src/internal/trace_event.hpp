#pragma once

// -----------------------------------------------------------------------------
// Runtime trace event representation (final design §10 / D18).
//
// The event stream is the GROUND TRUTH (C1). The hierarchical tree, inclusive/
// exclusive time, and calls-per-frame are all DERIVED off-line from this stream
// (analysis layer) — never stored on the hot path.
//
// TraceEvent is a fixed-size POD optimised for cheap writes: one append per
// event, touching only the producing thread's own chunk cache lines. Optional
// payloads (flow ids, category ids) ride in the same record here for the MVP;
// the design's side-channel stream is a later optimisation gated on the
// event-size measurement (G4/G5) and is intentionally NOT built yet.
// -----------------------------------------------------------------------------

#include <ludus/foundation/base/types.h>

namespace ludus::foundation::profiling::internal
{

// Event kinds. Begin/End are paired WITHIN a thread (C5 — same-thread pairing is
// a contract; cross-context work uses Flow, not a spanning scope). FrameMark is a
// boundary marker that advances the frame epoch, never a reset (D7). Flow links
// work across threads/jobs/queues (C1/C5) — the primitive is present now; the
// scheduler stamping that uses it is deferred with the job system (Phase 4).
// Enum base is uint8 (the value set fits a byte); the TraceEvent.Kind field is
// deliberately the wider uint16 for record layout and future headroom, so the
// enum value is widened on store. Keeping the enum minimal satisfies
// performance-enum-size without shrinking the record field.
enum class TraceEventKind : uint8
{
    Begin = 0,
    End = 1,
    Instant = 2, // zero-duration marker (LUDUS_PROFILE_MARK)
    FrameMark = 3,
    FlowOut = 4,
    FlowIn = 5,
    Counter = 6,     // timeline counter sample (integration.hpp SampleCounter); Aux = value
    MemoryGauge = 7, // one allocation-domain metric (memory_gauges.hpp); SiteId per memory_gauge.hpp, Aux = value
    // GpuBegin/GpuEnd reserved for Phase 5 (needs an RHI); not emitted yet.
};

// Event flags. `Incomplete` marks a synthesized End produced by the analysis
// layer for a Begin that had no matching End (crash, dropped chunk, migration);
// such a record is labelled, never reported as a measured duration (§6, G13).
enum TraceEventFlags : uint8
{
    TRACE_FLAG_NONE = 0,
    TRACE_FLAG_HAS_CATEGORY = 1u << 0,
    TRACE_FLAG_INCOMPLETE = 1u << 1,
    TRACE_FLAG_HAS_FLOW = 1u << 2,
};

// Fixed 24-byte record (matches the design's 16–24 B target; gate G4/G5 may
// shrink it later by moving the optional u64 to a side channel).
struct TraceEvent
{
    uint64 Ticks = 0;  // monotonic ns from NowTicks(); integer, never float
    uint32 SiteId = 0; // ZoneDescriptor::SiteId (End reuses the Begin's id)
    uint16 Kind = 0;   // TraceEventKind
    uint16 Flags = 0;  // TraceEventFlags
    uint64 Aux = 0;    // FlowId when TRACE_FLAG_HAS_FLOW; else CategoryId when
                       // TRACE_FLAG_HAS_CATEGORY; else 0
};

static_assert(sizeof(TraceEvent) == 24, "TraceEvent must stay compact (§10 target 16-24B)");

} // namespace ludus::foundation::profiling::internal
