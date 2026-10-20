#pragma once

// -----------------------------------------------------------------------------
// Trace recorder: per-thread recording + capture lifecycle (final design §6,
// §8, §21, §23). Internal — never installed.
//
// Ownership model:
//   * Each producing thread owns a TLS `ThreadRecorder` holding its current
//     chunk. Writing an event into that chunk needs no lock.
//   * A process-wide, never-destroyed `TraceRecorder` owns the aligned chunk
//     slab, the free and full chunk lists, the list of producers that hold or
//     have held a chunk, the registration tables and the capture-enabled flag.
//     One mutex guards the lists, slab and registration writers; it is taken
//     once per chunk (4096 events), never per event.
//
// Capture lifecycle (memory profiling phase 0 lifetime repair):
//   * BeginCapture() reserves the slab, rebuilds the free list from every chunk
//     and arms capture.
//   * Emit() announces itself in its thread's `IsEmitting` flag before testing
//     the capture flag. EndCapture() clears the capture flag first and then
//     waits until no linked producer is emitting. That store/load pairing is
//     sequentially consistent, so a producer either observes the disarm and
//     touches no chunk, or EndCapture observes its write in flight and waits.
//   * After that wait EndCapture() publishes every producer's partial chunk, so
//     a capture ends as a join: all events recorded before it returns are
//     drainable, whether or not their threads are still alive.
//   * A producer only holds a chunk between acquiring it under the mutex while
//     capture is armed and the next EndCapture() or its own thread exit, so the
//     slab is never released or rebuilt while a producer can write into it.
//
// Capture control calls (Begin/End/Reset/export) are serialized by their caller;
// the recorder does not support concurrent controllers. Nothing allocates on the
// hot path; over-capacity events are dropped and counted (Trace overflow
// policy, §21).
// -----------------------------------------------------------------------------

#include "internal/trace_chunk.hpp"
#include "internal/trace_event.hpp"

#include <ludus/foundation/base/types.h>

#include <atomic>
#include <mutex>
#include <string_view>

namespace ludus::foundation::profiling::internal
{

// Global fast-path flag. Checked first on every emit so that, when no capture is
// active, an enabled-build instrumentation site costs a single relaxed atomic
// load + predicted-not-taken branch and nothing else (§19, gate G2). Defined in
// recorder.cpp.
// Clang-tidy 18 misclassifies extern atomics with the SDK libc++ headers.
// The matching constinit definition enforces constant initialization.
// NOLINTNEXTLINE(bugprone-dynamic-static-initializers)
extern constinit std::atomic<bool> gCaptureActive;

[[nodiscard]] inline bool IsCaptureActive() noexcept
{
    return gCaptureActive.load(std::memory_order_relaxed);
}

// Runtime statistics / health (mirrors the logging LogHealth hooks; §21).
struct TraceHealth
{
    uint64 EventsRecorded = 0;
    uint64 EventsDropped = 0; // over-capacity drops (chunk pool exhausted)
    uint64 ChunksInFlight = 0;
    uint64 ThreadsRegistered = 0;
};

// Thread-local recorder state, one per producing thread. Only its own thread
// writes `CurrentOrNull` while capture is armed; EndCapture() and thread exit
// change it under the recorder mutex once the thread is not emitting.
struct ThreadRecorder
{
    TraceChunk* CurrentOrNull = nullptr;
    // Intrusive membership in the recorder's producer list (mutex-guarded).
    ThreadRecorder* NextOrNull = nullptr;
    ThreadRecorder* PreviousOrNull = nullptr;
    // True while Emit() may read or write CurrentOrNull; see the file header.
    std::atomic<bool> IsEmitting{false};
    uint32 ThreadId = 0;
    bool IsRegistered = false;
    bool IsLinked = false;

    ThreadRecorder() noexcept = default;
    ThreadRecorder(const ThreadRecorder&) = delete;
    ThreadRecorder& operator=(const ThreadRecorder&) = delete;
    ThreadRecorder(ThreadRecorder&&) = delete;
    ThreadRecorder& operator=(ThreadRecorder&&) = delete;
    ~ThreadRecorder() noexcept;
};

[[nodiscard]] ThreadRecorder& CurrentThreadRecorder() noexcept;

// Process-wide recorder. Single instance accessed via Instance(); it is never
// destroyed, so thread-exit hooks running during process teardown can still
// detach. The collector drain is synchronous (EndCapture/export run on the
// calling thread); see recorder.cpp.
class TraceRecorder
{
public:
    [[nodiscard]] static TraceRecorder& Instance() noexcept;

    // Reserve the chunk pool and arm capture. `maxChunks` bounds total trace
    // memory (maxChunks * sizeof(TraceChunk)); zero is treated as one. Returns
    // false, leaving capture disarmed, when a capture is already active or the
    // slab cannot be reserved. Any events left from a previous capture that were
    // not drained are discarded, and previously drained chunk pointers become
    // invalid.
    bool BeginCapture(usize maxChunks) noexcept;

    // Disarm capture, wait for producers that are writing an event, and publish
    // every producer's partial chunk to the full list. Safe to call when no
    // capture is active.
    void EndCapture() noexcept;

    // Register/unregister the calling thread. Registration assigns a stable
    // profiling-local thread id and stores the name for export. Safe to call
    // before BeginCapture. Unregister publishes the thread's partial chunk.
    uint32 RegisterThread(std::string_view name) noexcept;
    void UnregisterThread() noexcept;

    // Thread name for a registered id (for export). Empty view if unknown.
    [[nodiscard]] std::string_view ThreadName(uint32 threadId) const noexcept;
    [[nodiscard]] usize ThreadCount() const noexcept;
    // Thread id stored in name-table slot `index` (< ThreadCount()), for export.
    [[nodiscard]] uint32 ThreadIdAt(usize index) const noexcept;

    // Site registry (§13/§14). RegisterSite records id -> {name,file,line} once
    // per site (the instrumentation macro guards it with a function-local
    // static). Names are borrowed string_views into process-lifetime string
    // literals. SiteName returns empty for an unknown id.
    void RegisterSite(uint32 siteId, std::string_view name, std::string_view file, uint32 line) noexcept;
    [[nodiscard]] std::string_view SiteName(uint32 siteId) const noexcept;

    // Emit one event from the current thread. Returns false if capture is not
    // armed or the event was dropped (pool exhausted). Hot path: called via the
    // inline detail::Emit* wrappers.
    bool Emit(const TraceEvent& event) noexcept;

    // Publish the calling thread's partial chunk and leave the producer list.
    // Called from ThreadRecorder's destructor and UnregisterThread().
    void DetachThread(ThreadRecorder& inoutRecorder) noexcept;

    // Collector side ---------------------------------------------------------
    // Take ownership of all full chunks (LIFO). Caller walks PoolNextOrNull and, when
    // done, returns each chunk via RecycleChunk so the pool can be reused.
    // Callable during capture as well as after EndCapture().
    [[nodiscard]] TraceChunk* DrainFullChunks() noexcept;
    [[nodiscard]] bool HasPublishedChunks() const noexcept;
    void RecycleChunk(TraceChunk* chunk) noexcept;

    [[nodiscard]] TraceHealth Health() const noexcept;

    // Release the chunk slab. Returns false, keeping the slab, while capture is
    // active. Drained chunk pointers become invalid.
    bool Reset() noexcept;

    // Number of chunks in the reserved slab (0 when none is reserved).
    [[nodiscard]] usize PoolCapacity() const noexcept;

private:
    friend union ImmortalTraceRecorder;

    constexpr TraceRecorder() noexcept = default;

    [[nodiscard]] TraceChunk* replaceChunk(ThreadRecorder& inoutRecorder) noexcept;
    [[nodiscard]] bool isAnyProducerEmitting() const noexcept;
    void harvestProducerChunks() noexcept;
    void returnHeldChunk(ThreadRecorder& inoutRecorder) noexcept;
    void linkProducer(ThreadRecorder& inoutRecorder) noexcept;
    void unlinkProducer(ThreadRecorder& inoutRecorder) noexcept;
    void pushFree(TraceChunk* chunk) noexcept;
    void pushFull(TraceChunk* chunk) noexcept;
    [[nodiscard]] TraceChunk* popFree() noexcept;
    [[nodiscard]] bool reservePool(usize chunkCount) noexcept;
    void releasePool() noexcept;

    // Bounded thread-name table (§14/§21: fixed capacity, drop-with-none on
    // overflow — a rare event since threads register once). Writers append
    // under the mutex and publish the count with release ordering; readers load
    // the count with acquire ordering and read only published, immutable entries.
    static constexpr usize MAX_THREADS = 256;
    static constexpr usize THREAD_NAME_CAPACITY = 32;
    struct ThreadNameEntry
    {
        uint32 Id = 0;
        uint16 Length = 0;
        char Name[THREAD_NAME_CAPACITY] = {};
    };

    // Site registry entry: borrowed process-lifetime views (string literals /
    // __func__ static arrays). Published like the thread-name table.
    static constexpr usize MAX_SITES = 4096;
    struct SiteEntry
    {
        uint32 SiteId = 0;
        uint32 Line = 0;
        std::string_view Name;
        std::string_view File;
    };

    mutable std::mutex mMutex;
    TraceChunk* mFreeHeadOrNull = nullptr;      // guarded by mMutex
    TraceChunk* mFullHeadOrNull = nullptr;      // guarded by mMutex
    ThreadRecorder* mProducersOrNull = nullptr; // guarded by mMutex
    TraceChunk* mPoolStorageOrNull = nullptr;   // guarded by mMutex; aligned slab
    usize mPoolCapacity = 0;                    // guarded by mMutex
    usize mPoolBytes = 0;                       // guarded by mMutex
    // Lock-free hint that lets exhausted producers drop without the mutex. The
    // mutex-guarded free list remains authoritative.
    std::atomic<bool> mHasFreeChunks{false};
    std::atomic<uint64> mEventsRecorded{0};
    std::atomic<uint64> mEventsDropped{0};
    std::atomic<uint32> mNextThreadId{0};
    ThreadNameEntry mThreadNames[MAX_THREADS];
    std::atomic<uint32> mThreadNameCount{0};
    SiteEntry mSites[MAX_SITES];
    std::atomic<uint32> mSiteCount{0};
};

} // namespace ludus::foundation::profiling::internal
