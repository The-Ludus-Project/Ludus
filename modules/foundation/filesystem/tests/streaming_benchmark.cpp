#include "streaming_benchmark.hpp"

#include <ludus/foundation/containers/array.hpp>
#include <ludus/foundation/containers/static_array.hpp>
#include <ludus/foundation/filesystem/async.hpp>
#include <ludus/foundation/filesystem/pack.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <fstream>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#if defined(LUDUS_PLATFORM_LINUX) || defined(LUDUS_PLATFORM_MACOS)
#    include <sys/resource.h>
#endif

// Benchmark-only STL/CRT use follows ADR 0003. Production reads, CRC/decode,
// scheduler, revision retention and budgets are measured through the real SDK.
// Thanks to Didona et al., "Understanding Modern Storage APIs: A systematic study
// of libaio, SPDK, and io-uring", SYSTOR 2022 (abstract), and Seo et al., "DPAS:
// A Prompt, Accurate and Safe I/O Completion Method for SSDs", FAST 2026, sec. 4-5,
// for CPU/latency comparison questions. No algorithm or speedup is imported.
// https://research.ibm.com/publications/understanding-modern-storage-apis-a-systematic-study-of-libaio-spdk-and-io-uring
// https://www.usenix.org/conference/fast26/presentation/seo
// See docs/development/filesystem-benchmark.md for measurement boundaries.
using namespace ludus::foundation;
using namespace ludus::foundation::filesystem;
namespace
{
constexpr usize FILE_COUNT = 72;
constexpr usize FILE_NAME_CAPACITY = 32;
constexpr usize SMALL_FILE_COUNT = 64;
constexpr usize SMALL_BYTES = 4096;
constexpr usize LARGE_BYTES = usize{2} * 1024 * 1024;
constexpr usize MAX_READ_BYTES = usize{1024} * 1024;
constexpr usize WINDOW_COUNT = 16;
constexpr usize MAX_REQUEST_COUNT = 4096;
constexpr uint64 RUN_TIMEOUT_NS = uint64{60} * 1000 * 1000 * 1000;
struct Request final
{
    uint64 Release = 0;
    usize FileIndex = 0;
    uint64 Offset = 0;
    usize Length = 0;
    int32 Priority = 0;
    bool ShouldCancel = false;
};
struct Sample final
{
    uint64 Queue = 0;
    uint64 Service = 0;
    uint64 Delivery = 0;
    uint64 Completion = 0;
    uint64 ReleaseLateness = 0;
    uint64 Cancellation = 0;
    bool IsCollected = false;
};
struct Slot final
{
    bool IsOccupied = false;
    usize RequestIndex = 0;
};
class CpuContentionLoad final
{
public:
    ~CpuContentionLoad()
    {
        mShouldStop.store(true);
        for (std::thread& thread : mThreads)
        {
            thread.join();
        }
    }
    void Start(usize count)
    {
        mThreads.EnsureCapacity(count);
        for (usize index = 0; index < count; ++index)
        {
            mThreads.AddInPlace([this, index] {
                uint64 value = index + 1;
                mReady.fetch_add(1);
                while (mShouldStop.load(std::memory_order_relaxed) == false)
                {
                    for (usize step = 0; step < 4096; ++step)
                    {
                        value ^= value << 13;
                        value ^= value >> 7;
                        value ^= value << 17;
                    }
                    mChecksum.fetch_xor(value, std::memory_order_relaxed);
                }
            });
        }
        while (mReady.load() != count)
        {
            std::this_thread::yield();
        }
    }

private:
    std::atomic<usize> mReady{0};
    std::atomic<bool> mShouldStop{false};
    std::atomic<uint64> mChecksum{0};
    Array<std::thread> mThreads;
};
bool ParseUnsigned(std::string_view input, uint64 maximum, uint64& outValue)
{
    uint64 value = 0;
    if (input.empty() == true)
    {
        return false;
    }
    for (const char digit : input)
    {
        if (digit < '0' || digit > '9')
        {
            return false;
        }
        const uint64 next = static_cast<uint64>(digit - '0');
        if (next > maximum || value > (maximum - next) / 10)
        {
            return false;
        }
        value = value * 10 + next;
    }
    outValue = value;
    return true;
}
bool ReadNumber(std::ifstream& inoutStream, uint64 maximum, uint64& outValue)
{
    std::string token;
    return static_cast<bool>(inoutStream >> token) == true && ParseUnsigned(token, maximum, outValue) == true;
}
bool ReadTraceFile(const char* path, Array<Request>& outRequests)
{
    std::ifstream stream(path);
    std::string magic;
    uint64 version = 0;
    uint64 count = 0;
    if (static_cast<bool>(stream >> magic) == false || magic != "fs-r2" || ReadNumber(stream, 1, version) == false ||
        version != 1 || ReadNumber(stream, MAX_REQUEST_COUNT, count) == false || count == 0)
    {
        return false;
    }
    outRequests.Resize(static_cast<usize>(count));
    uint64 previous = 0;
    for (Request& request : outRequests)
    {
        uint64 fileIndex = 0;
        uint64 length = 0;
        uint64 priority = 0;
        uint64 cancel = 0;
        if (ReadNumber(stream, RUN_TIMEOUT_NS / 2, request.Release) == false ||
            ReadNumber(stream, FILE_COUNT - 1, fileIndex) == false ||
            ReadNumber(stream, LARGE_BYTES, request.Offset) == false ||
            ReadNumber(stream, MAX_READ_BYTES, length) == false || length == 0 ||
            ReadNumber(stream, 0x7fffffff, priority) == false || ReadNumber(stream, 1, cancel) == false)
        {
            return false;
        }
        const uint64 fileBytes = fileIndex < SMALL_FILE_COUNT ? SMALL_BYTES : LARGE_BYTES;
        if (request.Release < previous || request.Offset > fileBytes || length > fileBytes - request.Offset)
        {
            return false;
        }
        request.FileIndex = static_cast<usize>(fileIndex);
        request.Length = static_cast<usize>(length);
        request.Priority = static_cast<int32>(priority);
        request.ShouldCancel = cancel == 1;
        previous = request.Release;
    }
    std::string trailing;
    return static_cast<bool>(stream >> trailing) == false && stream.eof() == true;
}
uint8 ComputeByte(usize fileIndex, uint64 position)
{
    const uint32 blockOffset = static_cast<uint32>(position % SMALL_BYTES);
    if (fileIndex % 3 == 0 || (fileIndex % 3 == 2 && blockOffset < SMALL_BYTES / 2))
    {
        return static_cast<uint8>((position + fileIndex) % 256);
    }
    uint32 word = static_cast<uint32>(position / 4) ^ (static_cast<uint32>(fileIndex) * 0x9e3779b9U);
    word ^= word >> 16;
    word *= 0x7feb352dU;
    word ^= word >> 15;
    word *= 0x846ca68bU;
    word ^= word >> 16;
    return static_cast<uint8>(word >> ((position % 4) * 8));
}
bool Validate(const Request& request, std::span<const uint8> bytes)
{
    for (usize index = 0; index < request.Length; ++index)
    {
        if (bytes[index] != ComputeByte(request.FileIndex, request.Offset + index))
        {
            return false;
        }
    }
    return true;
}
uint64 Percentile(Array<uint64>& inoutValues, usize percent)
{
    if (inoutValues.IsEmpty() == true)
    {
        return 0;
    }
    std::sort(inoutValues.begin(), inoutValues.end());
    const usize rank = (inoutValues.GetSize() * percent + 99) / 100;
    return inoutValues[rank == 0 ? 0 : rank - 1];
}
void PrintDistribution(const char* name, Array<uint64> values)
{
    std::printf(",\"%s\":{\"count\":%zu,\"p50_ns\":%llu,\"p95_ns\":%llu,\"p99_ns\":%llu}",
                name,
                values.GetSize(),
                static_cast<unsigned long long>(Percentile(values, 50)),
                static_cast<unsigned long long>(Percentile(values, 95)),
                static_cast<unsigned long long>(Percentile(values, 99)));
}
} // namespace
int RunStreamingBenchmark(int argumentCount, char** arguments)
{
    // --stream ROOT LAYOUT WORKERS TRACE POLL_US CONTENTION OPEN_MODE TRACE_CAPACITY CACHE
    if (argumentCount != 12 || std::string_view(arguments[1]) != "--stream")
    {
        return 20;
    }
    uint64 workers = 0;
    uint64 pollMicroseconds = 0;
    uint64 contentionCount = 0;
    uint64 traceCapacity = 0;
    if (ParseUnsigned(arguments[4], 64, workers) == false ||
        ParseUnsigned(arguments[6], 1'000'000, pollMicroseconds) == false || pollMicroseconds == 0 ||
        ParseUnsigned(arguments[7], 8, contentionCount) == false ||
        ParseUnsigned(arguments[9], MAX_REQUEST_COUNT, traceCapacity) == false)
    {
        return 21;
    }
    // The last argument records the driver-controlled pass identity. A warm pass
    // explicitly reads its trace before timing; uncontrolled makes no cold claim.
    // argument 10 is a required reserved schema token for future trace formats.
    if (std::string_view(arguments[10]) != "v1")
    {
        return 21;
    }
    const std::string_view openMode(arguments[8]);
    const std::string_view cache(arguments[11]);
    if ((openMode != "shared" && openMode != "independent") ||
        (cache != "warm" && cache != "uncontrolled" && cache != "cold-start"))
    {
        return 21;
    }
    Array<Request> requests;
    if (ReadTraceFile(arguments[5], requests) == false)
    {
        return 22;
    }
    ProviderHandle storage;
    ProviderHandle provider;
    if (CreateDirectoryProvider(arguments[2], storage).Succeeded() == false)
    {
        return 23;
    }
    const std::string_view layout(arguments[3]);
    if (layout == "loose")
    {
        provider = storage;
    }
    else if (layout == "raw.pack" || layout == "lz4.pack")
    {
        if (CreatePackProvider(storage, layout, {}, provider).Succeeded() == false)
        {
            return 23;
        }
    }
    else
    {
        return 23;
    }
    const Mount mount{Root::Assets, {}, 0, 1, provider};
    MountSnapshot snapshot;
    if (MountSnapshot::Create({&mount, 1}, 1, snapshot).Succeeded() == false)
    {
        return 23;
    }
    // A shared run has one opened ProviderFile per logical asset. Independent
    // runs open each used asset separately for each destination slot, before timing.
    Array<VirtualFile> files(FILE_COUNT * WINDOW_COUNT);
    Array<bool> isUsed(FILE_COUNT, false);
    for (const Request& request : requests)
    {
        isUsed[request.FileIndex] = true;
    }
    const uint64 openStart = AsyncNow();
    for (usize fileIndex = 0; fileIndex < FILE_COUNT; ++fileIndex)
    {
        if (isUsed[fileIndex] == false)
        {
            continue;
        }
        core::StaticArray<char, FILE_NAME_CAPACITY> name{};
        std::snprintf(name.GetData(),
                      name.GetSize(),
                      "%s/%02zu",
                      fileIndex < SMALL_FILE_COUNT ? "small" : "large",
                      fileIndex < SMALL_FILE_COUNT ? fileIndex : fileIndex - SMALL_FILE_COUNT);
        VirtualPath path;
        if (path.Set(Root::Assets, name.GetData()).Succeeded() == false)
        {
            return 24;
        }
        for (usize slot = 0; slot < WINDOW_COUNT; ++slot)
        {
            VirtualFile& file = files[slot * FILE_COUNT + fileIndex];
            const Result result = slot != 0 && openMode == "shared" ? files[fileIndex].Clone(file)
                                                                    : snapshot.OpenRead(path, file).Outcome;
            if (result.Succeeded() == false)
            {
                return 24;
            }
        }
    }
    const uint64 openNanoseconds = AsyncNow() - openStart;
    Array<uint8> buffers(WINDOW_COUNT * MAX_READ_BYTES);
    Array<Sample> samples(requests.GetSize());
    Array<Slot> slots(WINDOW_COUNT);
    if (cache == "warm")
    {
        for (const Request& request : requests)
        {
            const ReadResult result = files[request.FileIndex].ReadAt(request.Offset, {buffers.data(), request.Length});
            if (result.Outcome.Succeeded() == false || result.BytesRead != request.Length ||
                Validate(request, {buffers.data(), request.Length}) == false)
            {
                return 25;
            }
        }
    }
    AsyncReader reader;
    if (workers != 0 && reader.Initialize({WINDOW_COUNT,
                                           static_cast<uint32>(workers),
                                           WINDOW_COUNT * MAX_READ_BYTES,
                                           5'000'000,
                                           static_cast<uint32>(traceCapacity)}) != AsyncStatus::Ok)
    {
        return 26;
    }
    CpuContentionLoad contention;
    contention.Start(contentionCount);
    usize next = 0;
    usize collected = 0;
    uint64 bytes = 0;
    uint64 cancelled = 0;
    uint64 cancelIntents = 0;
    uint64 validationNanoseconds = 0;
    uint32 peakCompleting = 0;
    uint32 peakQueued = 0;
    uint64 pollTicks = 0;
    const uint64 start = AsyncNow();
    uint64 nextPoll = start;
    const std::clock_t cpuStart = std::clock();
    SetBenchmarkAllocationTracking(true);
    while (collected < requests.size())
    {
        const uint64 now = AsyncNow();
        if (now - start > RUN_TIMEOUT_NS)
        {
            SetBenchmarkAllocationTracking(false);
            return 27;
        }
        if (workers == 0)
        {
            const Request& request = requests[next];
            if (now - start < request.Release)
            {
                std::this_thread::sleep_for(std::chrono::nanoseconds(request.Release - (now - start)));
                continue;
            }
            Sample& sample = samples[next];
            sample.ReleaseLateness = now - start - request.Release;
            if (request.ShouldCancel == true)
            {
                ++cancelIntents;
                ++cancelled;
            }
            else
            {
                const uint64 begin = AsyncNow();
                const ReadResult result =
                    files[request.FileIndex].ReadAt(request.Offset, {buffers.data(), request.Length});
                sample.Service = AsyncNow() - begin;
                sample.Completion = sample.Service;
                const uint64 validationStart = AsyncNow();
                if (result.Outcome.Succeeded() == false || result.BytesRead != request.Length ||
                    Validate(request, {buffers.data(), request.Length}) == false)
                {
                    SetBenchmarkAllocationTracking(false);
                    return 28;
                }
                validationNanoseconds += AsyncNow() - validationStart;
                bytes += result.BytesRead;
            }
            sample.IsCollected = true;
            ++next;
            ++collected;
            continue;
        }
        if (now < nextPoll)
        {
            std::this_thread::sleep_for(std::chrono::nanoseconds(nextPoll - now));
            continue;
        }
        ++pollTicks;
        nextPoll = now + pollMicroseconds * 1000;
        AsyncMetrics tickMetrics;
        if (reader.Metrics(tickMetrics) != AsyncStatus::Ok)
        {
            SetBenchmarkAllocationTracking(false);
            return 29;
        }
        peakCompleting = std::max(peakCompleting, tickMetrics.Completing);
        peakQueued = std::max(peakQueued, tickMetrics.Queued);
        ReadCompletion completion;
        while (reader.Poll(completion) == true)
        {
            const usize slot = static_cast<usize>(completion.Tag % WINDOW_COUNT);
            const usize index = static_cast<usize>(completion.Tag / WINDOW_COUNT);
            if (index >= requests.size() || slots[slot].IsOccupied == false || slots[slot].RequestIndex != index ||
                samples[index].IsCollected == true)
            {
                SetBenchmarkAllocationTracking(false);
                return 30;
            }
            const Request& request = requests[index];
            Sample& sample = samples[index];
            sample.Queue = completion.QueueNanoseconds;
            sample.Service = completion.ServiceNanoseconds;
            sample.Completion = completion.CompletionNanoseconds;
            if (sample.Queue > sample.Completion || sample.Service > sample.Completion - sample.Queue)
            {
                SetBenchmarkAllocationTracking(false);
                return 30;
            }
            sample.Delivery = sample.Completion - sample.Queue - sample.Service;
            sample.Cancellation = completion.CancellationNanoseconds;
            if (completion.Disposition == ReadDisposition::Cancelled)
            {
                if (request.ShouldCancel == false || completion.Read.BytesRead != 0)
                {
                    SetBenchmarkAllocationTracking(false);
                    return 31;
                }
                ++cancelled;
            }
            else
            {
                const uint64 validationStart = AsyncNow();
                if (completion.Disposition != ReadDisposition::Read || completion.Read.Outcome.Succeeded() == false ||
                    completion.Read.BytesRead != request.Length ||
                    Validate(request, {buffers.data() + slot * MAX_READ_BYTES, request.Length}) == false)
                {
                    SetBenchmarkAllocationTracking(false);
                    return 32;
                }
                validationNanoseconds += AsyncNow() - validationStart;
                bytes += completion.Read.BytesRead;
            }
            sample.IsCollected = true;
            slots[slot].IsOccupied = false;
            ++collected;
        }
        for (usize slot = 0; slot < WINDOW_COUNT && next < requests.size(); ++slot)
        {
            if (slots[slot].IsOccupied == true)
            {
                continue;
            }
            const Request& request = requests[next];
            const uint64 admitted = AsyncNow();
            if (admitted - start < request.Release)
            {
                break;
            }
            RequestHandle handle;
            if (reader.Submit(files[slot * FILE_COUNT + request.FileIndex],
                              request.Offset,
                              {buffers.data() + slot * MAX_READ_BYTES, request.Length},
                              handle,
                              {request.Priority, 0, next * WINDOW_COUNT + slot, nullptr}) != AsyncStatus::Ok)
            {
                SetBenchmarkAllocationTracking(false);
                return 33;
            }
            samples[next].ReleaseLateness = admitted - start - request.Release;
            slots[slot] = {true, next};
            if (request.ShouldCancel == true)
            {
                ++cancelIntents;
                if (reader.Cancel(handle) != AsyncStatus::Ok)
                {
                    SetBenchmarkAllocationTracking(false);
                    return 34;
                }
            }
            ++next;
        }
    }
    const uint64 elapsed = AsyncNow() - start;
    const float64 cpuMilliseconds = static_cast<float64>(std::clock() - cpuStart) * 1000.0 / CLOCKS_PER_SEC;
    SetBenchmarkAllocationTracking(false);
    const uint64 allocations = GetBenchmarkAllocationCount();
    AsyncMetrics metrics;
    uint64 traceCount = 0;
    if (workers != 0)
    {
        if (reader.Metrics(metrics) != AsyncStatus::Ok || metrics.InFlight != 0 || metrics.BytesInFlight != 0 ||
            metrics.Accepted != requests.size() || metrics.Reads + metrics.Cancelled != requests.size() ||
            metrics.Cancelled != cancelled || metrics.BytesRead != bytes || metrics.PeakRequests > WINDOW_COUNT ||
            metrics.PeakBytes > WINDOW_COUNT * MAX_READ_BYTES)
        {
            return 35;
        }
        ReadTrace trace;
        while (reader.PollTrace(trace) == true)
        {
            ++traceCount;
        }
        if (traceCapacity != 0 && traceCount + metrics.DroppedTraces != requests.size())
        {
            return 35;
        }
        if (reader.Shutdown() != AsyncStatus::Ok)
        {
            return 35;
        }
    }
#if defined(LUDUS_PLATFORM_LINUX) || defined(LUDUS_PLATFORM_MACOS)
    struct rusage usage{};
    if (::getrusage(RUSAGE_SELF, &usage) != 0)
    {
        return 36;
    }
    // macOS reports bytes; Linux reports KiB. This is whole-process peak RSS,
    // including warmed provider scratch and harness storage, not a live gauge.
#    if defined(LUDUS_PLATFORM_MACOS)
    const uint64 peakResidentBytes = static_cast<uint64>(usage.ru_maxrss);
#    else
    const uint64 peakResidentBytes = static_cast<uint64>(usage.ru_maxrss) * 1024;
#    endif
#else
    const uint64 peakResidentBytes = 0;
#endif
    std::printf("{\"schema\":1,\"workers\":%llu,\"poll_us\":%llu,\"contention_threads\":%llu,\"samples\":%zu,"
                "\"bytes\":%llu,\"elapsed_ns\":%llu,\"cpu_ms\":%.3f,\"mib_per_sec\":%.3f,"
                "\"open_ns\":%llu,\"validation_ns\":%llu,\"cpp_hot_allocations\":%llu,\"peak_rss_bytes\":%llu,"
                "\"destination_capacity_bytes\":%zu,\"peak_retained_bytes\":%llu,\"peak_requests\":%u,"
                "\"peak_completing\":%u,\"peak_queued\":%u,\"cancel_intents\":%llu,\"cancelled\":%llu,"
                "\"trace_capacity\":%llu,\"trace_records\":%llu,\"dropped_traces\":%llu,\"poll_ticks\":%llu",
                static_cast<unsigned long long>(workers),
                static_cast<unsigned long long>(pollMicroseconds),
                static_cast<unsigned long long>(contentionCount),
                requests.size(),
                static_cast<unsigned long long>(bytes),
                static_cast<unsigned long long>(elapsed),
                cpuMilliseconds,
                static_cast<float64>(bytes) * 1e9 / 1048576.0 / static_cast<float64>(elapsed),
                static_cast<unsigned long long>(openNanoseconds),
                static_cast<unsigned long long>(validationNanoseconds),
                static_cast<unsigned long long>(allocations),
                static_cast<unsigned long long>(peakResidentBytes),
                buffers.size(),
                static_cast<unsigned long long>(metrics.PeakBytes),
                metrics.PeakRequests,
                peakCompleting,
                peakQueued,
                static_cast<unsigned long long>(cancelIntents),
                static_cast<unsigned long long>(cancelled),
                static_cast<unsigned long long>(traceCapacity),
                static_cast<unsigned long long>(traceCount),
                static_cast<unsigned long long>(metrics.DroppedTraces),
                static_cast<unsigned long long>(pollTicks));
    Array<uint64> distribution;
    distribution.EnsureCapacity(samples.GetSize());
    for (const char* name : {"queue", "service", "delivery", "completion", "release_lateness", "cancellation"})
    {
        distribution.Clear();
        const std::string_view metric(name);
        for (const Sample& sample : samples)
        {
            if (sample.IsCollected == false)
            {
                return 37;
            }
            if (metric == "cancellation" && sample.Cancellation == 0)
            {
                continue;
            }
            const uint64 value = metric == "queue"              ? sample.Queue
                                 : metric == "service"          ? sample.Service
                                 : metric == "delivery"         ? sample.Delivery
                                 : metric == "completion"       ? sample.Completion
                                 : metric == "release_lateness" ? sample.ReleaseLateness
                                                                : sample.Cancellation;
            distribution.Add(value);
        }
        PrintDistribution(name, distribution);
    }
    std::puts("}");
    return 0;
}
