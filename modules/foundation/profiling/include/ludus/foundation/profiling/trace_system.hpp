#pragma once

// -----------------------------------------------------------------------------
// Trace lifecycle & capture control (final design §15, §21, §22).
//
// This is the NON-ubiquitous control surface: the platform/app layer calls it to
// arm a capture, register threads, and export. Ordinary code never includes it;
// it only uses the macros in profiling.hpp. Kept off the hot path, so it may use
// slightly heavier facilities than the instrumentation header (still no
// exceptions, still no iostreams).
// -----------------------------------------------------------------------------

#include <ludus/foundation/base/types.h>

#include <string_view>

namespace ludus::foundation::profiling
{

/// Snapshot of recorder health (§21). All counters are best-effort relaxed reads.
struct TraceHealth
{
    uint64 EventsRecorded = 0;
    uint64 EventsDropped = 0; ///< over-capacity drops during capture
    uint64 ThreadsRegistered = 0;
};

/// @brief Register the calling thread with the tracer so its events are attributed and its partial chunk is flushed
/// on exit.
///
/// Assigns a stable profiling-local thread id and records the human-readable name for export. Safe to call before
/// BeginCapture. Idempotent per thread. (Profiling keeps its own thread identity rather than depending on the
/// logging module, preserving the FoundationBase-only dependency direction — AGENTS.md.)
void RegisterThreadForTrace(std::string_view threadName) noexcept;

/// @brief Unregister the calling thread (publishes its partial chunk).
///
/// Optional — thread destruction does this automatically.
void UnregisterThreadForTrace() noexcept;

/// @brief Arm a capture session.
///
/// Reserves one slab of `maxChunks` 64-byte-aligned chunks (about 96 KiB each; zero is treated as one) so recording
/// never allocates. Events of a previous capture that were not exported are discarded.
/// @param maxChunks Bound on total trace memory, in chunks.
/// @return False when a capture is already active, the slab size overflows `usize`, or the reservation fails; tracing
/// then stays disabled and the engine runs normally (§21).
/// @note Capture control calls (Begin/End/Export) must be serialized by the caller. Producer threads may keep
/// emitting events across them.
bool BeginCapture(usize maxChunks = 64) noexcept;

/// @brief End the active capture session and join its producers.
///
/// Disarms recording, waits for threads that are writing an event, and publishes every thread's partial chunk, so
/// all events accepted before this returns are available to ExportPerfettoTrace(), including those of threads that
/// are still running. Safe to call when no capture is active.
/// @note Waits only for an in-progress event write; a producer thread suspended in the middle of one delays the return.
void EndCapture() noexcept;

/// Whether a stopped CPU recording still has published events awaiting serialization/export.
/// Does not drain or discard data. Serialize this control query with Begin/End/Export;
/// active producers may still hold unpublished events, so this is not an active-capture test.
[[nodiscard]] bool HasUnexportedTrace() noexcept;

/// @brief Serialise the most recent capture to a Chrome/Perfetto trace-event JSON file (final design §16 primary
/// viewer path).
///
/// Off the hot path. Returns false on I/O failure or when nothing was captured. Safe no-op in a disabled build.
/// On web this writes the Emscripten virtual filesystem (normally in-memory
/// MEMFS), not a persistent host file. The application must explicitly download
/// the bytes before page teardown; see tools/web-observability-probe/shell.html.
bool ExportPerfettoTrace(std::string_view path) noexcept;

// Health snapshot.
[[nodiscard]] TraceHealth GetTraceHealth() noexcept;

} // namespace ludus::foundation::profiling
