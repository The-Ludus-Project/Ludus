// Memory baseline workloads (memory profiling phase 0; memory-management
// step 0E). Each invocation runs one named workload: one warmup, then a fixed
// number of timed samples written to stdout as integer CSV rows. The
// scripts/python/memory_baseline.py runner starts every workload in fresh
// processes, records each process's peak RSS and computes rates offline. This
// tool measures the current system-backed paths only; it changes no allocation
// policy and is not a CI timing gate. Memory profiling phase 1 added the raw
// and contended churn workloads to measure domain accounting overhead
// (docs/development/memory-phase-1.md).
//
// See docs/development/memory-phase-0.md for the method and recorded results.
#include <ludus/foundation/base/core.h>
#include <ludus/foundation/containers/array.hpp>
#include <ludus/foundation/containers/static_array.hpp>
#include <ludus/foundation/memory/allocation_domain.hpp>
#include <ludus/foundation/profiling/profiling.hpp>
#include <ludus/foundation/profiling/trace_system.hpp>
#include <ludus/foundation/time/time.hpp>

#include <atomic>
#include <cstdio>
#include <new>
#include <string_view>
#include <thread> // std::thread drives the remote-free and multi-producer workloads

namespace
{
using ludus::foundation::AllocationDomain;
using ludus::foundation::Array;
using ludus::foundation::GetSystemAllocationDomain;
using ludus::foundation::uint32;
using ludus::foundation::uint64;
using ludus::foundation::uint8;
using ludus::foundation::usize;
using ludus::foundation::core::StaticArray;
using ludus::foundation::time::NowTicks;

constexpr usize SAMPLE_COUNT = 7;
constexpr uint64 RANDOM_SEED = 0x9e3779b97f4a7c15ULL;
constexpr usize PAGE_BYTES = 4096;
constexpr usize DECIMAL_BUFFER_BYTES = 24;

// One timed sample. `Bytes` is the workload's payload volume (documented per
// workload); `Checksum` keeps the optimizer from discarding the work.
struct Sample
{
    uint64 Operations = 0;
    uint64 Nanoseconds = 0;
    uint64 Bytes = 0;
    uint64 Checksum = 0;
};

using WorkloadFunction = Sample (*)();

struct Workload
{
    std::string_view Name;
    WorkloadFunction Run;
};

[[nodiscard]] uint64 NextRandom(uint64& inoutState) noexcept
{
    // xorshift64*: deterministic, independent of any engine RNG.
    inoutState ^= inoutState >> 12U;
    inoutState ^= inoutState << 25U;
    inoutState ^= inoutState >> 27U;
    return inoutState * 0x2545f4914f6cdd1dULL;
}

void WriteText(std::string_view text) noexcept
{
    (void)std::fwrite(text.data(), 1, text.size(), stdout);
}

void WriteUnsigned(uint64 value) noexcept
{
    StaticArray<char, DECIMAL_BUFFER_BYTES> digits{};
    usize cursor = DECIMAL_BUFFER_BYTES;
    do
    {
        --cursor;
        digits[cursor] = static_cast<char>('0' + value % 10);
        value /= 10;
    } while (value != 0);
    WriteText(std::string_view(digits.GetData() + cursor, DECIMAL_BUFFER_BYTES - cursor));
}

void WriteRow(std::string_view workload, usize sample, const Sample& result) noexcept
{
    WriteText(workload);
    WriteText(",");
    WriteUnsigned(sample);
    WriteText(",");
    WriteUnsigned(result.Operations);
    WriteText(",");
    WriteUnsigned(result.Nanoseconds);
    WriteText(",");
    WriteUnsigned(result.Bytes);
    WriteText(",");
    WriteUnsigned(result.Checksum);
    WriteText("\n");
}

// ---- System domain ----------------------------------------------------------

constexpr usize CHURN_SLOT_COUNT = 4096;
constexpr usize CHURN_SIZE_CLASS_COUNT = 9;
constexpr usize CHURN_BLOCK_ALIGNMENT = 16;
constexpr StaticArray<usize, CHURN_SIZE_CLASS_COUNT> CHURN_SIZE_CLASSES = {16, 24, 32, 48, 64, 96, 128, 192, 256};

// Allocates through the system AllocationDomain: the accounted engine path.
struct DomainBackend
{
    const AllocationDomain& Domain;

    [[nodiscard]] void* Allocate(usize bytes) const noexcept
    {
        return Domain.TryAllocate(bytes, CHURN_BLOCK_ALIGNMENT);
    }
    void Free(void* block, usize bytes) const noexcept
    {
        Domain.Free(block, bytes, CHURN_BLOCK_ALIGNMENT);
    }
};

// Calls the system domain's backend family directly, with no domain dispatch,
// validation or accounting: the unaccounted reference for the same workload.
struct RawBackend
{
    [[nodiscard]] void* Allocate(usize bytes) const noexcept
    {
        return ::operator new(bytes, std::align_val_t{CHURN_BLOCK_ALIGNMENT}, std::nothrow);
    }
    void Free(void* block, usize) const noexcept
    {
        ::operator delete(block, std::align_val_t{CHURN_BLOCK_ALIGNMENT});
    }
};

// Mixed small-block churn over a retained set: each operation picks a random
// slot and frees it if occupied, otherwise allocates a random size class and
// touches the payload. Bytes = requested bytes allocated during the sample.
// Untimed: the final release of the retained set.
template <typename BackendType>
Sample RunSmallChurn(const BackendType& backend, uint64 operationCount, uint64 seed)
{
    StaticArray<uint8*, CHURN_SLOT_COUNT> blocks{};
    StaticArray<usize, CHURN_SLOT_COUNT> blockSizes{};
    uint64 state = seed;
    Sample sample;
    sample.Operations = operationCount;

    const uint64 start = NowTicks();
    for (uint64 operation = 0; operation < operationCount; ++operation)
    {
        const uint64 random = NextRandom(state);
        const usize slot = static_cast<usize>(random % CHURN_SLOT_COUNT);
        if (blocks[slot] != nullptr)
        {
            sample.Checksum += blocks[slot][0];
            backend.Free(blocks[slot], blockSizes[slot]);
            blocks[slot] = nullptr;
            continue;
        }
        const usize bytes = CHURN_SIZE_CLASSES[static_cast<usize>((random >> 32U) % CHURN_SIZE_CLASS_COUNT)];
        uint8* block = static_cast<uint8*>(backend.Allocate(bytes));
        if (block == nullptr)
        {
            continue;
        }
        block[0] = static_cast<uint8>(operation);
        block[bytes - 1] = static_cast<uint8>(slot);
        blocks[slot] = block;
        blockSizes[slot] = bytes;
        sample.Bytes += bytes;
    }
    sample.Nanoseconds = NowTicks() - start;

    for (usize slot = 0; slot < CHURN_SLOT_COUNT; ++slot)
    {
        if (blocks[slot] != nullptr)
        {
            backend.Free(blocks[slot], blockSizes[slot]);
        }
    }
    return sample;
}

constexpr uint64 CHURN_OPERATION_COUNT = 1U << 20;

Sample RunSystemSmallChurn()
{
    return RunSmallChurn(DomainBackend{GetSystemAllocationDomain()}, CHURN_OPERATION_COUNT, RANDOM_SEED);
}

Sample RunRawSmallChurn()
{
    return RunSmallChurn(RawBackend{}, CHURN_OPERATION_COUNT, RANDOM_SEED);
}

// Four threads churn concurrently, released together; wall time from release
// to the last thread's completion. With the domain backend every thread updates
// the same domain's counters. Operations = total operations.
template <typename BackendType>
Sample RunContendedChurn(const BackendType& backend)
{
    constexpr uint32 THREAD_COUNT = 4;
    constexpr uint64 OPERATIONS_PER_THREAD = 1U << 18;
    std::atomic<bool> shouldStart{false};
    std::atomic<uint64> lastFinish{0};
    StaticArray<Sample, THREAD_COUNT> results{};
    Array<std::thread> threads;
    threads.EnsureCapacity(THREAD_COUNT);
    for (uint32 t = 0; t < THREAD_COUNT; ++t)
    {
        threads.AddInPlace([&backend, &shouldStart, &lastFinish, &results, t] {
            while (shouldStart.load(std::memory_order_acquire) == false)
            {
                std::this_thread::yield();
            }
            results[t] = RunSmallChurn(backend, OPERATIONS_PER_THREAD, RANDOM_SEED + t);
            const uint64 now = NowTicks();
            uint64 previous = lastFinish.load(std::memory_order_relaxed);
            while (previous < now &&
                   lastFinish.compare_exchange_weak(previous, now, std::memory_order_relaxed) == false)
            {
            }
        });
    }
    const uint64 start = NowTicks();
    shouldStart.store(true, std::memory_order_release);
    for (std::thread& thread : threads)
    {
        thread.join();
    }
    Sample sample;
    sample.Operations = THREAD_COUNT * OPERATIONS_PER_THREAD;
    sample.Nanoseconds = lastFinish.load(std::memory_order_relaxed) - start;
    for (const Sample& result : results)
    {
        sample.Bytes += result.Bytes;
        sample.Checksum += result.Checksum;
    }
    return sample;
}

Sample RunSystemContendedChurn()
{
    return RunContendedChurn(DomainBackend{GetSystemAllocationDomain()});
}

Sample RunRawContendedChurn()
{
    return RunContendedChurn(RawBackend{});
}

// Page-aligned 4 MiB blocks: allocate, touch one byte per page, free.
// Operations = blocks; Bytes = requested bytes.
Sample RunSystemLargeBlocks()
{
    constexpr usize BLOCK_BYTES = usize{4} << 20U;
    constexpr uint64 BLOCK_COUNT = 32;
    const AllocationDomain& domain = GetSystemAllocationDomain();
    Sample sample;
    sample.Operations = BLOCK_COUNT;

    const uint64 start = NowTicks();
    for (uint64 i = 0; i < BLOCK_COUNT; ++i)
    {
        uint8* block = static_cast<uint8*>(domain.TryAllocate(BLOCK_BYTES, PAGE_BYTES));
        if (block == nullptr)
        {
            continue;
        }
        for (usize offset = 0; offset < BLOCK_BYTES; offset += PAGE_BYTES)
        {
            block[offset] = static_cast<uint8>(offset >> 12U);
        }
        sample.Checksum += block[BLOCK_BYTES - PAGE_BYTES];
        domain.Free(block, BLOCK_BYTES, PAGE_BYTES);
        sample.Bytes += BLOCK_BYTES;
    }
    sample.Nanoseconds = NowTicks() - start;
    return sample;
}

// Blocks allocated on this thread and freed by another. Timing covers only the
// remote thread's frees (thread start and join excluded). Operations = frees.
Sample RunSystemRemoteFree()
{
    constexpr usize BLOCK_COUNT = 1U << 16;
    constexpr usize BLOCK_BYTES = 64;
    constexpr usize BLOCK_ALIGNMENT = 16;
    const AllocationDomain& domain = GetSystemAllocationDomain();
    Array<void*> blocks;
    blocks.EnsureCapacity(BLOCK_COUNT);
    for (usize i = 0; i < BLOCK_COUNT; ++i)
    {
        void* block = domain.TryAllocate(BLOCK_BYTES, BLOCK_ALIGNMENT);
        if (block != nullptr)
        {
            static_cast<uint8*>(block)[0] = static_cast<uint8>(i);
            blocks.Add(block);
        }
    }
    Sample sample;
    sample.Operations = blocks.GetSize();
    sample.Bytes = blocks.GetSize() * BLOCK_BYTES;
    std::atomic<uint64> elapsed{0};
    std::thread remote([&] {
        const uint64 start = NowTicks();
        for (usize i = 0; i < blocks.GetSize(); ++i)
        {
            domain.Free(blocks[i], BLOCK_BYTES, BLOCK_ALIGNMENT);
        }
        elapsed.store(NowTicks() - start, std::memory_order_relaxed);
    });
    remote.join();
    sample.Nanoseconds = elapsed.load(std::memory_order_relaxed);
    sample.Checksum = blocks.GetSize();
    return sample;
}

// ---- Containers -------------------------------------------------------------

// Appends 2^20 uint32 values to an empty Array (capacity growth included) and
// destroys it. Operations = elements; Bytes = final capacity bytes.
Sample RunArrayAppendUnreserved()
{
    constexpr uint32 ELEMENT_COUNT = 1U << 20;
    Sample sample;
    sample.Operations = ELEMENT_COUNT;
    const uint64 start = NowTicks();
    {
        Array<uint32> values;
        for (uint32 i = 0; i < ELEMENT_COUNT; ++i)
        {
            values.Add(i);
        }
        sample.Checksum = values[ELEMENT_COUNT / 2];
        sample.Bytes = values.GetCapacity() * sizeof(uint32);
    }
    sample.Nanoseconds = NowTicks() - start;
    return sample;
}

// The same appends with capacity reserved first.
Sample RunArrayAppendReserved()
{
    constexpr uint32 ELEMENT_COUNT = 1U << 20;
    Sample sample;
    sample.Operations = ELEMENT_COUNT;
    const uint64 start = NowTicks();
    {
        Array<uint32> values;
        values.EnsureCapacity(ELEMENT_COUNT);
        for (uint32 i = 0; i < ELEMENT_COUNT; ++i)
        {
            values.Add(i);
        }
        sample.Checksum = values[ELEMENT_COUNT / 2];
        sample.Bytes = values.GetCapacity() * sizeof(uint32);
    }
    sample.Nanoseconds = NowTicks() - start;
    return sample;
}

constexpr usize CACHE_LINE_PAYLOAD_WORDS = 7;

struct alignas(64) CacheLineElement
{
    uint64 Key = 0;
    StaticArray<uint64, CACHE_LINE_PAYLOAD_WORDS> Payload{};
};

// Appends 2^16 over-aligned 64-byte elements without reservation.
Sample RunArrayAppendAligned64()
{
    constexpr uint64 ELEMENT_COUNT = 1U << 16;
    Sample sample;
    sample.Operations = ELEMENT_COUNT;
    const uint64 start = NowTicks();
    {
        Array<CacheLineElement> values;
        for (uint64 i = 0; i < ELEMENT_COUNT; ++i)
        {
            CacheLineElement element;
            element.Key = i;
            values.Add(element);
        }
        sample.Checksum = values[ELEMENT_COUNT / 2].Key;
        sample.Bytes = values.GetCapacity() * sizeof(CacheLineElement);
    }
    sample.Nanoseconds = NowTicks() - start;
    return sample;
}

// ---- Trace recorder ---------------------------------------------------------

#if LUDUS_PROFILING_ENABLED
constexpr usize TRACE_CHUNK_COUNT = 64;
// Untimed drain after each capture. Exporting returns every chunk to the pool,
// so successive samples start from the same state on recorders that, like the
// pre-phase-0 one, reclaim chunks only through export. POSIX null device.
constexpr std::string_view TRACE_DRAIN_PATH = "/dev/null";

void EmitMarks(uint64 count) noexcept
{
    for (uint64 i = 0; i < count; ++i)
    {
        LUDUS_PROFILE_MARK(MemoryBenchMark);
    }
}

// One producer emitting instant events into an armed 64-chunk capture (Begin,
// End and the drain excluded). Operations = events; Checksum = recorded events;
// Bytes = dropped events.
Sample RunTraceEmitOneThread()
{
    constexpr uint64 EVENT_COUNT = 200000;
    namespace profiling = ludus::foundation::profiling;
    Sample sample;
    sample.Operations = EVENT_COUNT;
    if (profiling::BeginCapture(TRACE_CHUNK_COUNT) == false)
    {
        return sample;
    }
    const uint64 start = NowTicks();
    EmitMarks(EVENT_COUNT);
    sample.Nanoseconds = NowTicks() - start;
    profiling::EndCapture();
    const profiling::TraceHealth health = profiling::GetTraceHealth();
    (void)profiling::ExportPerfettoTrace(TRACE_DRAIN_PATH);
    sample.Checksum = health.EventsRecorded;
    sample.Bytes = health.EventsDropped;
    return sample;
}

// Four producers released together; wall time from release to the last
// producer's completion. Operations = events. Bytes = dropped events.
Sample RunTraceEmitFourThreads()
{
    constexpr uint32 THREAD_COUNT = 4;
    constexpr uint64 EVENTS_PER_THREAD = 50000;
    namespace profiling = ludus::foundation::profiling;
    Sample sample;
    sample.Operations = THREAD_COUNT * EVENTS_PER_THREAD;
    if (profiling::BeginCapture(TRACE_CHUNK_COUNT) == false)
    {
        return sample;
    }
    std::atomic<bool> shouldStart{false};
    std::atomic<uint32> finished{0};
    std::atomic<uint64> lastFinish{0};
    Array<std::thread> threads;
    threads.EnsureCapacity(THREAD_COUNT);
    for (uint32 t = 0; t < THREAD_COUNT; ++t)
    {
        threads.AddInPlace([&] {
            while (shouldStart.load(std::memory_order_acquire) == false)
            {
                std::this_thread::yield();
            }
            EmitMarks(EVENTS_PER_THREAD);
            const uint64 now = NowTicks();
            uint64 previous = lastFinish.load(std::memory_order_relaxed);
            while (previous < now &&
                   lastFinish.compare_exchange_weak(previous, now, std::memory_order_relaxed) == false)
            {
            }
            finished.fetch_add(1, std::memory_order_release);
        });
    }
    const uint64 start = NowTicks();
    shouldStart.store(true, std::memory_order_release);
    for (std::thread& thread : threads)
    {
        thread.join();
    }
    sample.Nanoseconds = lastFinish.load(std::memory_order_relaxed) - start;
    profiling::EndCapture();
    const profiling::TraceHealth health = profiling::GetTraceHealth();
    (void)profiling::ExportPerfettoTrace(TRACE_DRAIN_PATH);
    sample.Checksum = health.EventsRecorded + finished.load(std::memory_order_acquire);
    sample.Bytes = health.EventsDropped;
    return sample;
}

// Begin + End with no events, alternating 64 and 65 chunks so every session
// releases and reserves (and zero-initializes) a new slab. Operations = sessions.
Sample RunTraceSessionSetup()
{
    constexpr uint64 SESSION_COUNT = 16;
    namespace profiling = ludus::foundation::profiling;
    Sample sample;
    sample.Operations = SESSION_COUNT;
    const uint64 start = NowTicks();
    for (uint64 i = 0; i < SESSION_COUNT; ++i)
    {
        if (profiling::BeginCapture(TRACE_CHUNK_COUNT + (i & 1U)) == true)
        {
            ++sample.Checksum;
        }
        profiling::EndCapture();
    }
    sample.Nanoseconds = NowTicks() - start;
    return sample;
}
#endif

// Does nothing: the runner's peak RSS for this workload is the process baseline.
Sample RunIdle()
{
    Sample sample;
    sample.Operations = 1;
    return sample;
}

constexpr usize WORKLOAD_COUNT =
#if LUDUS_PROFILING_ENABLED
    13;
#else
    10;
#endif

const StaticArray<Workload, WORKLOAD_COUNT> WORKLOADS = {{
    { .Name = "idle", .Run = RunIdle },
    { .Name = "system-small-churn", .Run = RunSystemSmallChurn },
    { .Name = "raw-small-churn", .Run = RunRawSmallChurn },
    { .Name = "system-contended-churn-4-threads", .Run = RunSystemContendedChurn },
    { .Name = "raw-contended-churn-4-threads", .Run = RunRawContendedChurn },
    { .Name = "system-large-blocks", .Run = RunSystemLargeBlocks },
    { .Name = "system-remote-free", .Run = RunSystemRemoteFree },
    { .Name = "array-append-unreserved", .Run = RunArrayAppendUnreserved },
    { .Name = "array-append-reserved", .Run = RunArrayAppendReserved },
    { .Name = "array-append-aligned64", .Run = RunArrayAppendAligned64 },
#if LUDUS_PROFILING_ENABLED
    { .Name = "trace-emit-1-thread", .Run = RunTraceEmitOneThread },
    { .Name = "trace-emit-4-threads", .Run = RunTraceEmitFourThreads },
    { .Name = "trace-session-setup", .Run = RunTraceSessionSetup },
#endif
}};

void WriteUsage() noexcept
{
    WriteText("usage: ludus_memory_bench <workload>|--list\nworkloads:\n");
    for (const Workload& workload : WORKLOADS)
    {
        WriteText("  ");
        WriteText(workload.Name);
        WriteText("\n");
    }
}
} // namespace

int main(int argumentCount, char** arguments)
{
    if (argumentCount != 2)
    {
        WriteUsage();
        return 2;
    }
    const std::string_view requested(arguments[1]);
    if (requested == "--list")
    {
        for (const Workload& workload : WORKLOADS)
        {
            WriteText(workload.Name);
            WriteText("\n");
        }
        return 0;
    }
    for (const Workload& workload : WORKLOADS)
    {
        if (workload.Name != requested)
        {
            continue;
        }
        (void)workload.Run(); // warmup
        WriteText("workload,sample,operations,nanoseconds,bytes,checksum\n");
        for (usize sample = 0; sample < SAMPLE_COUNT; ++sample)
        {
            const Sample result = workload.Run();
            WriteRow(workload.Name, sample, result);
        }
        return 0;
    }
    WriteUsage();
    return 2;
}
