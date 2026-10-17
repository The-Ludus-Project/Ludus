// Isolated allocation replacement; exclude this executable under sanitizers.
#include <ludus/foundation/profiling/trace_system.hpp>
#include <ludus/foundation/threading/job_profiling.hpp>
#include <ludus/foundation/threading/jobs.hpp>

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <cstdlib>
#include <new>

using namespace ludus::foundation;
using namespace ludus::foundation::threading;

namespace
{
std::atomic<uint32> gAllocations{0};
std::atomic<uint32> gFailAt{0};
std::atomic<bool> gIsTracking{false};
void* Allocate(usize bytes) noexcept
{
    if (gIsTracking.load() == true)
    {
        const uint32 count = ++gAllocations;
        if (count == gFailAt.load())
        {
            return nullptr;
        }
    }
    return std::malloc(bytes == 0 ? 1 : bytes);
}
JobOutcome Increment(void* context) noexcept
{
    ++*static_cast<uint32*>(context);
    return JobOutcome::Succeeded;
}

// A minimal non-profiler trace sink: counts reports without storing them.
void CountTrace(void* context, const JobTraceEvent&) noexcept
{
    ++*static_cast<std::atomic<uint32>*>(context);
}

enum class TracingMode : uint8
{
    Off,             // no hooks
    CountingSink,    // a plain trace hook that traces every submission
    ProfilerIdle,    // profiler hooks, no active capture: every submission declined
    ProfilerCapture, // profiler hooks with an active capture: every event recorded
};

// Runs repeated Add/DependsOn/Seal/Submit/Wait/Reset cycles under allocation
// tracking and returns the number of allocations observed.
uint32 CountSchedulingAllocations(uint32 workers, TracingMode mode)
{
    std::atomic<uint32> reports{0};
    JobSystemHooks hooks{};
    if (mode == TracingMode::CountingSink)
    {
        hooks = { .TraceOrNull = CountTrace, .ContextOrNull = &reports };
    }
    else if (mode != TracingMode::Off)
    {
        hooks = MakeProfilerJobHooks("Alloc worker");
    }
    uint32 count = 0;
    JobGraph graph;
    JobSystem system;
    REQUIRE(graph.Initialize({2, 1}) == JobStatus::Ok);
    REQUIRE(system.Initialize({ .WorkerCount = workers, .Hooks = hooks }) == JobStatus::Ok);
    // Warm the native worker/runtime path, thread-local profiler state and the
    // labels' one-time site registration before tracking. Assertions stay
    // outside the measured region so Catch2 cannot affect the result.
    const JobLabel first = LUDUS_JOB_LABEL(AllocFirst);
    const JobLabel second = LUDUS_JOB_LABEL(AllocSecond);
    REQUIRE(graph.Seal() == JobStatus::Ok);
    REQUIRE(system.Submit(graph) == JobStatus::Ok);
    JobOutcome outcome{};
    REQUIRE(system.Wait(outcome) == JobStatus::Ok);
    REQUIRE(graph.Reset() == JobStatus::Ok);
    reports = 0;
    gAllocations = 0;
    gIsTracking = true;
    bool isValid = true;
    for (uint32 iteration = 0; iteration < 100; ++iteration)
    {
        JobHandle firstHandle{};
        JobHandle secondHandle{};
        isValid &= graph.Add(Increment, &count, firstHandle, first) == JobStatus::Ok;
        isValid &= graph.Add(Increment, &count, secondHandle, second) == JobStatus::Ok;
        isValid &= graph.DependsOn(secondHandle, firstHandle) == JobStatus::Ok;
        isValid &= graph.Seal() == JobStatus::Ok;
        isValid &= system.Submit(graph) == JobStatus::Ok;
        isValid &= system.Wait(outcome) == JobStatus::Ok;
        isValid &= outcome == JobOutcome::Succeeded;
        isValid &= graph.Reset() == JobStatus::Ok;
    }
    gIsTracking = false;
    const uint32 allocations = gAllocations.load();
    REQUIRE(isValid == true);
    REQUIRE(count == 200);
    // Submitted, 2 x (Started, Finished), 1 Signalled, WaitEntered, WaitReturned.
    REQUIRE(reports == (mode == TracingMode::CountingSink ? 800u : 0u));
    REQUIRE(system.Shutdown() == JobStatus::Ok);
    return allocations;
}
} // namespace

// libc++ names the replaceable allocation functions' parameters with reserved
// identifiers (`__sz`), so these replacements cannot match its declarations.
// NOLINTBEGIN(readability-inconsistent-declaration-parameter-name)
void* operator new(usize bytes)
{
    void* allocationOrNull = Allocate(bytes);
    if (allocationOrNull != nullptr)
    {
        return allocationOrNull;
    }
    throw std::bad_alloc{};
}
void* operator new[](usize bytes)
{
    return ::operator new(bytes);
}
void* operator new(usize bytes, const std::nothrow_t&) noexcept
{
    return Allocate(bytes);
}
void* operator new[](usize bytes, const std::nothrow_t&) noexcept
{
    return Allocate(bytes);
}
void operator delete(void* allocation) noexcept
{
    std::free(allocation);
}
void operator delete[](void* allocation) noexcept
{
    std::free(allocation);
}
void operator delete(void* allocation, usize) noexcept
{
    std::free(allocation);
}
void operator delete[](void* allocation, usize) noexcept
{
    std::free(allocation);
}
void operator delete(void* allocation, const std::nothrow_t&) noexcept
{
    std::free(allocation);
}
void operator delete[](void* allocation, const std::nothrow_t&) noexcept
{
    std::free(allocation);
}
// NOLINTEND(readability-inconsistent-declaration-parameter-name)

TEST_CASE("Every graph allocation failure cleans up and permits retry", "[threading][alloc]")
{
    for (const uint32 failAt : {1u, 2u, 3u, 4u})
    {
        JobGraph graph;
        gAllocations = 0;
        gFailAt = failAt;
        gIsTracking = true;
        const JobStatus status = graph.Initialize({8, 8});
        gIsTracking = false;
        gFailAt = 0;
        REQUIRE(status == JobStatus::OutOfMemory);
        REQUIRE(graph.Initialize({8, 8}) == JobStatus::Ok);
    }
    JobSystem system;
    gAllocations = 0;
    gFailAt = 1;
    gIsTracking = true;
    const JobStatus status = system.Initialize();
    gIsTracking = false;
    gFailAt = 0;
    REQUIRE(status == JobStatus::OutOfMemory);
    REQUIRE(system.Initialize() == JobStatus::Ok);
}

TEST_CASE("Graph build seal submit wait reset have zero scheduler allocations", "[threading][alloc]")
{
    for (const uint32 workers : {0u, 3u})
    {
        REQUIRE(CountSchedulingAllocations(workers, TracingMode::Off) == 0);
    }
}

TEST_CASE("Tracing hooks keep scheduling allocation-free with tracing on and off", "[threading][alloc][trace]")
{
    ludus::foundation::profiling::RegisterThreadForTrace("Alloc owner");
    for (const uint32 workers : {0u, 3u})
    {
        REQUIRE(CountSchedulingAllocations(workers, TracingMode::CountingSink) == 0);
        REQUIRE(CountSchedulingAllocations(workers, TracingMode::ProfilerIdle) == 0);
        // The capture reserves its whole chunk pool before tracking starts.
        REQUIRE(ludus::foundation::profiling::BeginCapture(64) == true);
        const uint32 allocations = CountSchedulingAllocations(workers, TracingMode::ProfilerCapture);
        ludus::foundation::profiling::EndCapture();
        REQUIRE(allocations == 0);
#if LUDUS_PROFILING_ENABLED
        REQUIRE(ludus::foundation::profiling::GetTraceHealth().EventsRecorded > 0);
#endif
        REQUIRE(ludus::foundation::profiling::GetTraceHealth().EventsDropped == 0);
    }
}
