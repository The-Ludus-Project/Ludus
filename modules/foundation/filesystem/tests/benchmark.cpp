#include "streaming_benchmark.hpp"

#include <ludus/foundation/containers/array.hpp>
#include <ludus/foundation/filesystem/async.hpp>
#include <ludus/foundation/filesystem/pack.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <new>
#include <thread>

using namespace ludus::foundation;
using namespace ludus::foundation::filesystem;
namespace
{
std::atomic<uint64> gAllocations{0};
std::atomic<bool> gTracking{false};
void* Allocate(usize size) noexcept
{
    if (gTracking.load())
    {
        ++gAllocations;
    }
    return std::malloc(size == 0 ? 1 : size);
}
constexpr usize SAMPLES = 2048;
constexpr usize WINDOW = 32;
constexpr usize LARGE = usize{1024} * 1024;
struct Sample
{
    uint64 Completion = 0;
    uint64 Queue = 0;
    uint64 Service = 0;
};
usize Descriptors()
{
#if defined(LUDUS_PLATFORM_LINUX)
    std::error_code error;
    usize count = 0;
    for (std::filesystem::directory_iterator it("/proc/self/fd", error), end; error.value() == 0 && it != end;
         it.increment(error))
    {
        ++count;
    }
    return count;
#elif defined(LUDUS_PLATFORM_MACOS)
    // /dev/fd lists live descriptors on Darwin; the iterator itself adds one.
    std::error_code error;
    usize count = 0;
    for (std::filesystem::directory_iterator it("/dev/fd", error), end; error.value() == 0 && it != end;
         it.increment(error))
    {
        ++count;
    }
    return count;
#else
    return 0;
#endif
}
} // namespace
// Standalone benchmark counts C++ new/new[] only, after fixed storage and files open.
void* operator new(usize size)
{
    if (auto* p = Allocate(size))
    {
        return p;
    }
    std::abort();
}
void* operator new[](usize size)
{
    return ::operator new(size);
}
void* operator new(usize size, const std::nothrow_t&) noexcept
{
    return Allocate(size);
}
void* operator new[](usize size, const std::nothrow_t&) noexcept
{
    return Allocate(size);
}
void operator delete(void* p) noexcept
{
    std::free(p);
}
void operator delete[](void* p) noexcept
{
    std::free(p);
}
void operator delete(void* p, usize) noexcept
{
    std::free(p);
}
void operator delete[](void* p, usize) noexcept
{
    std::free(p);
}
void operator delete(void* p, const std::nothrow_t&) noexcept
{
    std::free(p);
}
void operator delete[](void* p, const std::nothrow_t&) noexcept
{
    std::free(p);
}
void SetBenchmarkAllocationTracking(bool shouldTrack) noexcept
{
    if (shouldTrack == true)
    {
        gAllocations = 0;
    }
    gTracking = shouldTrack;
}
uint64 GetBenchmarkAllocationCount() noexcept
{
    return gAllocations.load();
}
int main(int argc, char** argv)
{
    if (argc > 4)
    {
        return RunStreamingBenchmark(argc, argv);
    }
    // Generated corpus directory, loose/raw.pack/lz4.pack, worker count 0/1/4.
    if (argc != 4)
    {
        return 1;
    }
    const uint32 workers = static_cast<uint32>(std::strtoul(argv[3], nullptr, 10));
    ProviderHandle storage, provider;
    if (CreateDirectoryProvider(argv[1], storage).Succeeded() == false)
    {
        return 2;
    }
    const std::string_view layout = argv[2];
    if (layout == "loose")
    {
        provider = storage;
    }
    else if (CreatePackProvider(storage, layout, {}, provider).Succeeded() == false)
    {
        return 3;
    }
    const auto beforeOpen = AsyncNow();
    const Mount mount{Root::Assets, {}, 0, 1, provider};
    MountSnapshot snapshot;
    if (MountSnapshot::Create({&mount, 1}, 1, snapshot).Succeeded() == false)
    {
        return 4;
    }
    VirtualFile files[72];
    for (usize index = 0; index < 72; ++index)
    {
        char name[32]{};
        std::snprintf(name, sizeof(name), "%s/%02zu", index < 64 ? "small" : "large", index < 64 ? index : index - 64);
        VirtualPath path;
        if (path.Set(Root::Assets, name).Succeeded() == false ||
            snapshot.OpenRead(path, files[index]).Outcome.Succeeded() == false)
        {
            return 5;
        }
    }
    const auto openNs = AsyncNow() - beforeOpen;
    const auto descriptors = Descriptors();
    Array<uint8> buffers(WINDOW * LARGE);
    Sample samples[SAMPLES]{};
    Sample ordered[SAMPLES]{};
    for (const char* pass : {"first", "warm"})
    {
        AsyncReader reader;
        if (workers != 0 && reader.Initialize({WINDOW, workers, WINDOW * LARGE, 5'000'000, 0}) != AsyncStatus::Ok)
        {
            return 6;
        }
        uint64 bytes = 0;
        gAllocations = 0;
        gTracking = true;
        const auto cpu = std::clock();
        const auto start = AsyncNow();
        if (workers == 0)
        {
            for (usize index = 0; index < SAMPLES; ++index)
            {
                const bool large = index % 8 == 0;
                const usize length = large ? LARGE : 4096;
                const usize fileIndex = large ? 64 + (index / 8) % 8 : index % 64;
                const uint64 offset = large ? ((index / 64) % 16) * 65536 : 0;
                const auto begin = AsyncNow();
                const ReadResult result = files[fileIndex].ReadAt(offset, {buffers.GetData(), length});
                samples[index] = {AsyncNow() - begin, 0, AsyncNow() - begin};
                if (result.Outcome.Succeeded() == false || result.BytesRead != length)
                {
                    return 7;
                }
                const uint8 expected = static_cast<uint8>(large ? fileIndex - 64 : fileIndex);
                if (buffers[0] != expected || buffers[length - 1] != static_cast<uint8>(expected + 255))
                {
                    return 11;
                }
                bytes += result.BytesRead;
            }
        }
        else
        {
            bool occupied[WINDOW]{};
            usize submitted = 0, completed = 0;
            while (completed < SAMPLES)
            {
                for (usize slot = 0; slot < WINDOW && submitted < SAMPLES; ++slot)
                {
                    if (occupied[slot])
                    {
                        continue;
                    }
                    const bool large = submitted % 8 == 0;
                    const usize length = large ? LARGE : 4096;
                    const usize fileIndex = large ? 64 + (submitted / 8) % 8 : submitted % 64;
                    const uint64 offset = large ? ((submitted / 64) % 16) * 65536 : 0;
                    RequestHandle handle;
                    if (reader.Submit(files[fileIndex],
                                      offset,
                                      {buffers.GetData() + slot * LARGE, length},
                                      handle,
                                      {0, 0, submitted * WINDOW + slot, nullptr}) != AsyncStatus::Ok)
                    {
                        return 8;
                    }
                    occupied[slot] = true;
                    ++submitted;
                }
                ReadCompletion completion;
                if (reader.Poll(completion))
                {
                    if (completion.Read.Outcome.Succeeded() == false || completion.Disposition != ReadDisposition::Read)
                    {
                        return 9;
                    }
                    const usize index = static_cast<usize>(completion.Tag / WINDOW);
                    const usize slot = static_cast<usize>(completion.Tag % WINDOW);
                    const bool large = index % 8 == 0;
                    const usize length = large ? LARGE : 4096;
                    const uint8 expected = static_cast<uint8>(large ? (index / 8) % 8 : index % 64);
                    if (completion.Read.BytesRead != length || buffers[slot * LARGE] != expected ||
                        buffers[slot * LARGE + length - 1] != static_cast<uint8>(expected + 255))
                    {
                        return 11;
                    }
                    occupied[slot] = false;
                    samples[index] = {completion.CompletionNanoseconds,
                                      completion.QueueNanoseconds,
                                      completion.ServiceNanoseconds};
                    bytes += completion.Read.BytesRead;
                    ++completed;
                }
                else
                {
                    std::this_thread::yield();
                }
            }
        }
        const auto elapsed = AsyncNow() - start;
        gTracking = false;
        const float64 cpuMs = static_cast<float64>(std::clock() - cpu) * 1000.0 / CLOCKS_PER_SEC;
        std::copy(samples, samples + SAMPLES, ordered);
        std::sort(ordered, ordered + SAMPLES, [](const Sample& a, const Sample& b) {
            return a.Completion < b.Completion;
        });
        uint64 maxQueue = 0;
        for (const auto& sample : samples)
        {
            maxQueue = std::max(maxQueue, sample.Queue);
        }
        AsyncMetrics metrics;
        if (workers != 0 && reader.Metrics(metrics) != AsyncStatus::Ok)
        {
            return 10;
        }
        // Standalone benchmark output, exempt from engine diagnostics policy.
        std::printf("layout=%s workers=%u pass=%s samples=%zu bytes=%llu p50_ns=%llu p95_ns=%llu p99_ns=%llu "
                    "mib_per_sec=%.2f cpu_ms=%.2f peak_bytes=%llu peak_requests=%u max_queue_ns=%llu descriptors=%zu "
                    "open_ns=%llu cpp_hot_allocations=%llu\n",
                    argv[2],
                    workers,
                    pass,
                    SAMPLES,
                    static_cast<unsigned long long>(bytes),
                    static_cast<unsigned long long>(ordered[SAMPLES / 2].Completion),
                    static_cast<unsigned long long>(ordered[SAMPLES * 95 / 100].Completion),
                    static_cast<unsigned long long>(ordered[SAMPLES * 99 / 100].Completion),
                    static_cast<float64>(bytes) * 1e9 / 1048576.0 / static_cast<float64>(elapsed),
                    cpuMs,
                    static_cast<unsigned long long>(metrics.PeakBytes),
                    metrics.PeakRequests,
                    static_cast<unsigned long long>(maxQueue),
                    descriptors,
                    static_cast<unsigned long long>(openNs),
                    static_cast<unsigned long long>(gAllocations.load()));
    }
    if (workers != 0)
    {
        AsyncReader reader;
        if (reader.Initialize({1, workers, LARGE, 1, 0}) != AsyncStatus::Ok)
        {
            return 12;
        }
        uint64 times[128]{};
        uint32 intents = 0, cancellations = 0;
        for (usize index = 0; index < 128; ++index)
        {
            RequestHandle handle;
            if (reader.Submit(files[64], 0, {buffers.GetData(), LARGE}, handle) != AsyncStatus::Ok ||
                reader.Cancel(handle) != AsyncStatus::Ok)
            {
                return 12;
            }
            ReadCompletion completion;
            while (reader.Poll(completion) == false)
            {
                std::this_thread::yield();
            }
            if (completion.Disposition == ReadDisposition::Cancelled)
            {
                ++cancellations;
            }
            if (completion.CancellationNanoseconds != 0)
            {
                times[intents++] = completion.CancellationNanoseconds;
            }
        }
        std::sort(times, times + intents);
        if (intents == 0)
        {
            return 13;
        }
        std::printf("layout=%s workers=%u cancellation_intents=%u cancelled=%u cancel_p50_ns=%llu cancel_p95_ns=%llu "
                    "cancel_p99_ns=%llu\n",
                    argv[2],
                    workers,
                    intents,
                    cancellations,
                    static_cast<unsigned long long>(times[intents / 2]),
                    static_cast<unsigned long long>(times[intents * 95 / 100]),
                    static_cast<unsigned long long>(times[intents * 99 / 100]));
    }
}
