#pragma once

// -----------------------------------------------------------------------------
// Trace chunks and the chunk handoff (final design §6, §8, §23).
//
// Recording is per-thread and single-writer: pushing an event is a plain bump of
// a chunk index (the buffer is owned by exactly one producing thread). Only when
// a chunk fills, at capture end or on thread exit is the whole chunk handed to
// the collector under the recorder's mutex — so that synchronisation is
// amortised over thousands of events rather than paid per event.
//
// The handoff lists were previously lock-free CAS stacks. Several producers
// popping the free list concurrently made that stack ABA-prone (a popper could
// install a `PoolNextOrNull` read from a chunk another thread had already popped and
// reused). Memory profiling phase 0 replaced them with intrusive lists guarded
// by one mutex; see docs/architecture/profiling.md.
//
// Overflow policy is Trace's: DROP the event and count it (never block, never
// allocate, never grow). Memory profiling's different (lossless) contract lives
// elsewhere (§7, C4) and must never reuse this policy.
// -----------------------------------------------------------------------------

#include "internal/trace_event.hpp"

#include <ludus/foundation/base/compiler.h>
#include <ludus/foundation/base/types.h>

namespace ludus::foundation::profiling::internal
{

// Number of events per chunk. 4096 * 24 B = 96 KiB. Tunable; the final value is
// a measurement decision (gate G4/G5) — this is a defensible MVP default.
inline constexpr usize kEventsPerChunk = 4096;

// A fixed-capacity block of events owned by one producer at a time, then handed
// to the collector. Cache-line aligned so producer writes never false-share the
// bookkeeping the collector reads.
#if defined(LUDUS_COMPILER_MSVC)
#    pragma warning(push)
// The tail padding keeps adjacent chunks cache-line aligned in the trace slab.
#    pragma warning(disable : 4324)
#endif
struct alignas(64) TraceChunk
{
    // Producer identity stamped when the chunk is acquired, so the collector can
    // attribute events to a thread without a per-event thread id (§10).
    uint32 ThreadId = 0;
    uint32 Count = 0;                     // number of valid events (producer-written)
    TraceChunk* PoolNextOrNull = nullptr; // free/full list linkage, guarded by the recorder mutex
    TraceEvent Events[kEventsPerChunk];
};
#if defined(LUDUS_COMPILER_MSVC)
#    pragma warning(pop)
#endif

} // namespace ludus::foundation::profiling::internal
