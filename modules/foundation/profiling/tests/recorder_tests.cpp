// Trace recorder slab and lifetime regressions (memory profiling phase 0).
//
// These drive the internal recorder directly so every emitted event has a
// deterministic accepted/dropped outcome. Run them under ASan/UBSan and TSan:
// the stop/restart and concurrent-recycle cases reallocate or reuse chunk
// storage while producer threads keep emitting, so a lifetime or publication
// defect shows up as a sanitizer report or as an event-count mismatch.
#include <ludus/foundation/profiling/profiling.hpp>
#include <ludus/foundation/profiling/trace_system.hpp>

#include "internal/recorder.hpp"
#include "internal/trace_chunk.hpp"
#include "internal/trace_event.hpp"

#include <ludus/foundation/containers/static_array.hpp>
#include <ludus/foundation/threading/thread.hpp>

#include <catch2/catch_test_macros.hpp>

#include <atomic>

namespace threading = ludus::foundation::threading;

#if !defined(__EMSCRIPTEN__) || LUDUS_PROFILING_ENABLED

namespace
{
using ludus::foundation::uint32;
using ludus::foundation::uint64;
using ludus::foundation::usize;
using ludus::foundation::core::StaticArray;
using ludus::foundation::profiling::internal::kEventsPerChunk;
using ludus::foundation::profiling::internal::TraceChunk;
using ludus::foundation::profiling::internal::TraceEvent;
using ludus::foundation::profiling::internal::TraceEventKind;
using ludus::foundation::profiling::internal::TraceRecorder;

struct DrainSummary
{
    uint64 Events = 0;
    uint64 Chunks = 0;
    uint64 MisalignedChunks = 0;
    uint64 OverfullChunks = 0;
};

[[nodiscard]] TraceEvent MakeInstant(uint64 ticks) noexcept
{
    TraceEvent event;
    event.Ticks = ticks;
    event.Kind = static_cast<ludus::foundation::uint16>(TraceEventKind::Instant);
    return event;
}

// Drains every full chunk, validates it and returns it to the pool.
void DrainInto(DrainSummary& inoutSummary)
{
    TraceRecorder& recorder = TraceRecorder::Instance();
    TraceChunk* chunk = recorder.DrainFullChunks();
    while (chunk != nullptr)
    {
        TraceChunk* next = chunk->PoolNextOrNull;
        if (reinterpret_cast<usize>(chunk) % alignof(TraceChunk) != 0)
        {
            ++inoutSummary.MisalignedChunks;
        }
        if (chunk->Count > kEventsPerChunk)
        {
            ++inoutSummary.OverfullChunks;
        }
        inoutSummary.Events += chunk->Count;
        ++inoutSummary.Chunks;
        recorder.RecycleChunk(chunk);
        chunk = next;
    }
}

[[nodiscard]] DrainSummary Drain()
{
    DrainSummary summary;
    DrainInto(summary);
    return summary;
}

[[nodiscard]] uint64 EmitMany(uint64 count) noexcept
{
    uint64 accepted = 0;
    for (uint64 i = 0; i < count; ++i)
    {
        if (TraceRecorder::Instance().Emit(MakeInstant(i)) == true)
        {
            ++accepted;
        }
    }
    return accepted;
}

void WaitUntil(const std::atomic<bool>& flag)
{
    while (flag.load(std::memory_order_acquire) == false)
    {
        threading::YieldThread();
    }
}

void WaitForCount(const std::atomic<uint32>& counter, uint32 expected)
{
    while (counter.load(std::memory_order_acquire) < expected)
    {
        threading::YieldThread();
    }
}
} // namespace

TEST_CASE("trace slab chunks honor TraceChunk alignment across reallocation", "[profiling][recorder][memory]")
{
    TraceRecorder& recorder = TraceRecorder::Instance();
    static_assert(alignof(TraceChunk) == 64);
    constexpr usize CAPACITY_COUNT = 4;
    const StaticArray<usize, CAPACITY_COUNT> capacities = {1, 3, 2, 7};
    for (const usize capacity : capacities)
    {
        REQUIRE(recorder.BeginCapture(capacity) == true);
        CHECK(recorder.PoolCapacity() == capacity);
        // Fill every chunk so each one is handed out and published.
        const uint64 accepted = EmitMany(static_cast<uint64>(capacity) * kEventsPerChunk + 1);
        recorder.EndCapture();
        const DrainSummary summary = Drain();
        CHECK(accepted == static_cast<uint64>(capacity) * kEventsPerChunk);
        CHECK(summary.Chunks == capacity);
        CHECK(summary.Events == accepted);
        CHECK(summary.MisalignedChunks == 0);
        CHECK(recorder.Health().EventsDropped == 1);
    }
    CHECK(recorder.Reset() == true);
    CHECK(recorder.PoolCapacity() == 0);
}

TEST_CASE("trace slab rejects an overflowing chunk count and stays disarmed", "[profiling][recorder][memory]")
{
    TraceRecorder& recorder = TraceRecorder::Instance();
    const usize overflowing = static_cast<usize>(-1) / sizeof(TraceChunk) + 1;
    CHECK(recorder.BeginCapture(overflowing) == false);
    CHECK(ludus::foundation::profiling::internal::IsCaptureActive() == false);
    CHECK(recorder.Emit(MakeInstant(1)) == false);

    // The recorder remains usable afterwards.
    REQUIRE(recorder.BeginCapture(1) == true);
    CHECK(recorder.Emit(MakeInstant(2)) == true);
    recorder.EndCapture();
    CHECK(Drain().Events == 1);
}

TEST_CASE("Reset is refused while a capture is active", "[profiling][recorder]")
{
    TraceRecorder& recorder = TraceRecorder::Instance();
    REQUIRE(recorder.BeginCapture(2) == true);
    CHECK(recorder.Reset() == false);
    CHECK(recorder.PoolCapacity() == 2);
    CHECK(recorder.BeginCapture(2) == false);
    recorder.EndCapture();
    (void)Drain();
    CHECK(recorder.Reset() == true);
}

TEST_CASE("EndCapture publishes partial chunks held by live threads", "[profiling][recorder][threading]")
{
    TraceRecorder& recorder = TraceRecorder::Instance();
    constexpr uint32 THREAD_COUNT = 4;
    constexpr uint64 EVENTS_PER_THREAD = 1000; // well below one chunk
    REQUIRE(recorder.BeginCapture(8) == true);

    std::atomic<uint32> finishedEmitting{0};
    std::atomic<bool> shouldExit{false};
    std::atomic<uint64> accepted{0};
    StaticArray<threading::Thread, THREAD_COUNT> threads;
    for (uint32 t = 0; t < THREAD_COUNT; ++t)
    {
        REQUIRE(threads[t].Start([&] {
            accepted.fetch_add(EmitMany(EVENTS_PER_THREAD), std::memory_order_relaxed);
            finishedEmitting.fetch_add(1, std::memory_order_release);
            // Stay alive, holding a partial chunk, until the capture has ended.
            WaitUntil(shouldExit);
        }) == threading::ThreadStatus::Ok);
    }
    WaitForCount(finishedEmitting, THREAD_COUNT);
    recorder.EndCapture();
    const DrainSummary summary = Drain();
    shouldExit.store(true, std::memory_order_release);
    for (threading::Thread& thread : threads)
    {
        REQUIRE(thread.Join() == threading::ThreadStatus::Ok);
    }

    CHECK(accepted.load() == THREAD_COUNT * EVENTS_PER_THREAD);
    CHECK(summary.Events == THREAD_COUNT * EVENTS_PER_THREAD);
    CHECK(summary.Chunks == THREAD_COUNT);
    // Exiting after the capture ended publishes nothing further.
    CHECK(Drain().Events == 0);
}

TEST_CASE("threads exiting during a capture publish their partial chunks", "[profiling][recorder][threading]")
{
    TraceRecorder& recorder = TraceRecorder::Instance();
    constexpr uint32 THREAD_COUNT = 6;
    constexpr uint64 EVENTS_PER_THREAD = kEventsPerChunk + 17;
    REQUIRE(recorder.BeginCapture(static_cast<usize>(THREAD_COUNT) * 2) == true);

    std::atomic<uint64> accepted{0};
    StaticArray<threading::Thread, THREAD_COUNT> threads;
    for (uint32 t = 0; t < THREAD_COUNT; ++t)
    {
        // No UnregisterThreadForTrace(): thread exit itself must publish.
        REQUIRE(threads[t].Start([&] { accepted.fetch_add(EmitMany(EVENTS_PER_THREAD), std::memory_order_relaxed); }) ==
                threading::ThreadStatus::Ok);
    }
    for (threading::Thread& thread : threads)
    {
        REQUIRE(thread.Join() == threading::ThreadStatus::Ok);
    }
    recorder.EndCapture();
    const DrainSummary summary = Drain();
    CHECK(accepted.load() == THREAD_COUNT * EVENTS_PER_THREAD);
    CHECK(summary.Events == accepted.load());
    CHECK(recorder.Health().EventsDropped == 0);
}

TEST_CASE("stop and restart with delayed producers never loses or invents events", "[profiling][recorder][threading]")
{
    TraceRecorder& recorder = TraceRecorder::Instance();
    constexpr uint32 THREAD_COUNT = 4;
    constexpr uint32 SESSION_COUNT = 40;

    std::atomic<bool> shouldStop{false};
    StaticArray<threading::Thread, THREAD_COUNT> threads;
    for (uint32 t = 0; t < THREAD_COUNT; ++t)
    {
        REQUIRE(threads[t].Start([&shouldStop, t] {
            uint64 tick = 0;
            while (shouldStop.load(std::memory_order_relaxed) == false)
            {
                (void)TraceRecorder::Instance().Emit(MakeInstant(tick));
                ++tick;
                if ((tick & 0x3ffu) == t)
                {
                    threading::YieldThread(); // vary producer timing around Begin/End
                }
            }
        }) == threading::ThreadStatus::Ok);
    }

    uint64 sessionsWithEvents = 0;
    for (uint32 session = 0; session < SESSION_COUNT; ++session)
    {
        // Varying the size forces the slab to be released and reallocated while
        // producers keep emitting between sessions.
        const usize chunkCount = 1 + session % 4;
        REQUIRE(recorder.BeginCapture(chunkCount) == true);
        for (uint32 spin = 0; spin < 64; ++spin)
        {
            threading::YieldThread();
        }
        recorder.EndCapture();
        const uint64 recorded = recorder.Health().EventsRecorded;
        const DrainSummary summary = Drain();
        CHECK(summary.Events == recorded);
        CHECK(summary.MisalignedChunks == 0);
        CHECK(summary.OverfullChunks == 0);
        CHECK(summary.Chunks <= chunkCount);
        if (summary.Events > 0)
        {
            ++sessionsWithEvents;
        }
        if (session % 7 == 0)
        {
            CHECK(recorder.Reset() == true);
        }
    }
    shouldStop.store(true, std::memory_order_relaxed);
    for (threading::Thread& thread : threads)
    {
        REQUIRE(thread.Join() == threading::ThreadStatus::Ok);
    }
    CHECK(sessionsWithEvents > 0);
    CHECK(recorder.Reset() == true);
}

TEST_CASE("concurrent chunk acquisition and recycling keeps every event accounted", "[profiling][recorder][threading]")
{
    TraceRecorder& recorder = TraceRecorder::Instance();
    constexpr uint32 THREAD_COUNT = 8;
    constexpr uint64 EVENTS_PER_THREAD = 3 * kEventsPerChunk + 5;
    // Far fewer chunks than producers: many threads pop the free list while the
    // collector recycles drained chunks back into it.
    REQUIRE(recorder.BeginCapture(3) == true);

    std::atomic<uint32> finishedProducers{0};
    std::atomic<uint64> accepted{0};
    DrainSummary drained;
    threading::Thread collector;
    REQUIRE(collector.Start([&] {
        while (finishedProducers.load(std::memory_order_acquire) < THREAD_COUNT)
        {
            DrainInto(drained);
            threading::YieldThread();
        }
    }) == threading::ThreadStatus::Ok);
    StaticArray<threading::Thread, THREAD_COUNT> threads;
    for (uint32 t = 0; t < THREAD_COUNT; ++t)
    {
        REQUIRE(threads[t].Start([&] {
            accepted.fetch_add(EmitMany(EVENTS_PER_THREAD), std::memory_order_relaxed);
            finishedProducers.fetch_add(1, std::memory_order_release);
        }) == threading::ThreadStatus::Ok);
    }
    for (threading::Thread& thread : threads)
    {
        REQUIRE(thread.Join() == threading::ThreadStatus::Ok);
    }
    REQUIRE(collector.Join() == threading::ThreadStatus::Ok);
    recorder.EndCapture();
    DrainInto(drained);

    const ludus::foundation::profiling::internal::TraceHealth health = recorder.Health();
    CHECK(drained.Events == accepted.load());
    CHECK(health.EventsRecorded == accepted.load());
    CHECK(health.EventsRecorded + health.EventsDropped == THREAD_COUNT * EVENTS_PER_THREAD);
    CHECK(drained.MisalignedChunks == 0);
    CHECK(drained.OverfullChunks == 0);
}

TEST_CASE("concurrent first use of a site and thread registration publish complete entries",
          "[profiling][recorder][threading]")
{
    constexpr uint32 THREAD_COUNT = 8;
    std::atomic<bool> shouldStart{false};
    StaticArray<std::atomic<uint32>, THREAD_COUNT> threadIds{};
    StaticArray<threading::Thread, THREAD_COUNT> threads;
    for (uint32 t = 0; t < THREAD_COUNT; ++t)
    {
        const threading::ThreadStatus started = threads[t].Start([&shouldStart, &threadIds, t] {
            WaitUntil(shouldStart);
            const uint32 id = TraceRecorder::Instance().RegisterThread("RegistrationWorker");
            threadIds[t].store(id, std::memory_order_relaxed);
            // Several threads reach this site's one-time registration together.
            LUDUS_PROFILE_MARK(ConcurrentFirstUse);
        });
        REQUIRE(started == threading::ThreadStatus::Ok);
    }
    shouldStart.store(true, std::memory_order_release);
    for (threading::Thread& thread : threads)
    {
        REQUIRE(thread.Join() == threading::ThreadStatus::Ok);
    }
    for (uint32 t = 0; t < THREAD_COUNT; ++t)
    {
        // Every published name entry is complete once its count is visible.
        CHECK(TraceRecorder::Instance().ThreadName(threadIds[t].load()) == "RegistrationWorker");
    }
}

#endif
