// CPU-scope correctness matrix (final design §11): single, nested, recursive,
// early return, multi-thread, frame-boundary crossing, and incomplete scope.
//
// These cases assert real recording, so they are only meaningful when the SDK
// build has profiling compiled in (Debug/Development/Profile). In a Release
// build (LUDUS_PROFILING_ENABLED == 0) the macros are no-ops and this TU
// intentionally contributes no test cases — the compile-out contract itself is
// covered by disabled_tests.cpp. We inspect the internal recorder to count
// events, since the event stream is the ground truth (C1).
#include <ludus/foundation/profiling/profiling.hpp>
#include <ludus/foundation/profiling/trace_system.hpp>

#include "internal/recorder.hpp"
#include "internal/trace_chunk.hpp"
#include "internal/trace_event.hpp"

#include <ludus/foundation/containers/array.hpp>

#include <catch2/catch_test_macros.hpp>

#include <thread> // std::thread element type for the multithread worker pool

#if LUDUS_PROFILING_ENABLED

using namespace ludus::foundation::profiling;
using ludus::foundation::uint32;
using ludus::foundation::uint64;

namespace
{

// Drain the recorder into a flat, timestamp-ordered vector per thread. Mirrors
// what the exporter does; used here to assert structural correctness.
struct Drained
{
    ludus::foundation::Array<internal::TraceEvent> events;
    uint64 beginCount = 0;
    uint64 endCount = 0;
    uint64 instantCount = 0;
    uint64 frameCount = 0;
    uint64 flowOut = 0;
    uint64 flowIn = 0;
    uint64 counterCount = 0;
    uint64 memoryGaugeCount = 0;
};

Drained drainAll()
{
    Drained result;
    internal::TraceRecorder& recorder = internal::TraceRecorder::Instance();
    internal::TraceChunk* chunk = recorder.DrainFullChunks();
    ludus::foundation::Array<internal::TraceChunk*> chunks;
    while (chunk != nullptr)
    {
        internal::TraceChunk* next = chunk->PoolNextOrNull;
        for (uint32 i = 0; i < chunk->Count; ++i)
        {
            const internal::TraceEvent& event = chunk->Events[i];
            result.events.Add(event);
            switch (static_cast<internal::TraceEventKind>(event.Kind))
            {
                case internal::TraceEventKind::Begin:
                    ++result.beginCount;
                    break;
                case internal::TraceEventKind::End:
                    ++result.endCount;
                    break;
                case internal::TraceEventKind::Instant:
                    ++result.instantCount;
                    break;
                case internal::TraceEventKind::FrameMark:
                    ++result.frameCount;
                    break;
                case internal::TraceEventKind::FlowOut:
                    ++result.flowOut;
                    break;
                case internal::TraceEventKind::FlowIn:
                    ++result.flowIn;
                    break;
                case internal::TraceEventKind::Counter:
                    ++result.counterCount;
                    break;
                case internal::TraceEventKind::MemoryGauge:
                    ++result.memoryGaugeCount;
                    break;
            }
        }
        chunks.Add(chunk);
        chunk = next;
    }
    for (internal::TraceChunk* c : chunks)
    {
        recorder.RecycleChunk(c);
    }
    return result;
}

LUDUS_PROFILE_CATEGORY(CatTest, "Test");

void recurse(int n)
{
    LUDUS_PROFILE_SCOPE(Recurse);
    if (n > 0)
    {
        recurse(n - 1);
    }
}

int earlyReturn(bool takeEarly)
{
    LUDUS_PROFILE_SCOPE(EarlyReturn);
    if (takeEarly)
    {
        return 1; // RAII dtor must still emit End
    }
    return 0;
}

} // namespace

TEST_CASE("single scope emits one begin and one end", "[profiling][scope]")
{
    RegisterThreadForTrace("Main");
    REQUIRE(BeginCapture(8));
    {
        LUDUS_PROFILE_SCOPE(Single);
    }
    EndCapture();

    const Drained drained = drainAll();
    CHECK(drained.beginCount == 1);
    CHECK(drained.endCount == 1);
}

TEST_CASE("nested scopes preserve begin/end pairing order", "[profiling][scope]")
{
    RegisterThreadForTrace("Main");
    REQUIRE(BeginCapture(8));
    {
        LUDUS_PROFILE_SCOPE(Outer);
        {
            LUDUS_PROFILE_SCOPE(Inner, CatTest);
        }
    }
    EndCapture();

    const Drained drained = drainAll();
    REQUIRE(drained.events.size() == 4);
    // Order within a thread is emission order: B(Outer) B(Inner) E(Inner) E(Outer).
    CHECK(drained.events[0].Kind == static_cast<uint16_t>(internal::TraceEventKind::Begin));
    CHECK(drained.events[1].Kind == static_cast<uint16_t>(internal::TraceEventKind::Begin));
    CHECK(drained.events[2].Kind == static_cast<uint16_t>(internal::TraceEventKind::End));
    CHECK(drained.events[3].Kind == static_cast<uint16_t>(internal::TraceEventKind::End));
    // Inner carried a category.
    CHECK((drained.events[1].Flags & internal::TRACE_FLAG_HAS_CATEGORY) != 0);
}

TEST_CASE("recursion produces one begin/end pair per invocation", "[profiling][scope]")
{
    RegisterThreadForTrace("Main");
    REQUIRE(BeginCapture(8));
    recurse(3); // 4 invocations
    EndCapture();

    const Drained drained = drainAll();
    CHECK(drained.beginCount == 4);
    CHECK(drained.endCount == 4);
}

TEST_CASE("early return still closes the scope", "[profiling][scope]")
{
    RegisterThreadForTrace("Main");
    REQUIRE(BeginCapture(8));
    (void)earlyReturn(true);
    EndCapture();

    const Drained drained = drainAll();
    CHECK(drained.beginCount == 1);
    CHECK(drained.endCount == 1);
}

TEST_CASE("frame marker and instant marker are recorded", "[profiling][scope][frame]")
{
    RegisterThreadForTrace("Main");
    REQUIRE(BeginCapture(8));
    {
        LUDUS_PROFILE_SCOPE(Frameful);
        LUDUS_PROFILE_MARK(Marker);
        LUDUS_PROFILE_FRAME();
    }
    EndCapture();

    const Drained drained = drainAll();
    CHECK(drained.instantCount == 1);
    CHECK(drained.frameCount == 1);
}

TEST_CASE("a scope spanning a frame boundary is attributed by emission order (D7)", "[profiling][scope][frame]")
{
    RegisterThreadForTrace("Main");
    REQUIRE(BeginCapture(8));
    {
        LUDUS_PROFILE_SCOPE(CrossFrame);
        LUDUS_PROFILE_FRAME(); // boundary does not close the open scope
    }
    EndCapture();

    const Drained drained = drainAll();
    // B(CrossFrame), FrameMark, E(CrossFrame) — the frame mark sits inside the
    // open scope; the scope is not split.
    REQUIRE(drained.events.size() == 3);
    CHECK(drained.events[0].Kind == static_cast<uint16_t>(internal::TraceEventKind::Begin));
    CHECK(drained.events[1].Kind == static_cast<uint16_t>(internal::TraceEventKind::FrameMark));
    CHECK(drained.events[2].Kind == static_cast<uint16_t>(internal::TraceEventKind::End));
}

TEST_CASE("flow out/in are recorded with a shared id", "[profiling][flow]")
{
    RegisterThreadForTrace("Main");
    REQUIRE(BeginCapture(8));
    const uint64 flow = LUDUS_PROFILE_FLOW_OUT(Job);
    LUDUS_PROFILE_FLOW_IN(flow);
    EndCapture();

    const Drained drained = drainAll();
    CHECK(drained.flowOut == 1);
    CHECK(drained.flowIn == 1);
    // Both events carry the same flow id in Aux.
    REQUIRE(drained.events.size() == 2);
    CHECK((drained.events[0].Flags & internal::TRACE_FLAG_HAS_FLOW) != 0);
    CHECK(drained.events[0].Aux == flow);
    CHECK(drained.events[1].Aux == flow);
}

TEST_CASE("multi-threaded scope generation is lossless within capacity", "[profiling][scope][threading]")
{
    RegisterThreadForTrace("Main");
    REQUIRE(BeginCapture(256));

    constexpr int kThreads = 8;
    constexpr int kScopesPerThread = 20000;
    ludus::foundation::Array<std::thread> threads;
    threads.EnsureCapacity(kThreads);
    for (int t = 0; t < kThreads; ++t)
    {
        threads.AddInPlace([] {
            RegisterThreadForTrace("Worker");
            for (int i = 0; i < kScopesPerThread; ++i)
            {
                LUDUS_PROFILE_SCOPE(Work);
            }
            UnregisterThreadForTrace(); // flush partial chunk before join
        });
    }
    for (auto& thread : threads)
    {
        thread.join();
    }
    EndCapture();

    const TraceHealth health = GetTraceHealth();
    CHECK(health.EventsDropped == 0);
    // 8 threads * 20000 scopes * 2 events, plus none dropped.
    CHECK(health.EventsRecorded == static_cast<uint64>(kThreads) * kScopesPerThread * 2);
}

TEST_CASE("buffer saturation drops with a count and never crashes", "[profiling][scope][overflow]")
{
    RegisterThreadForTrace("Main");
    REQUIRE(BeginCapture(1)); // 1 chunk = 4096 events
    for (int i = 0; i < 20000; ++i)
    {
        LUDUS_PROFILE_MARK(Tick);
    }
    const TraceHealth health = GetTraceHealth();
    EndCapture();
    (void)drainAll();

    CHECK(health.EventsRecorded > 0);
    CHECK(health.EventsDropped > 0);
    // No corruption: recorded is bounded by the single chunk capacity.
    CHECK(health.EventsRecorded <= 4096);
}

#endif // LUDUS_PROFILING_ENABLED
