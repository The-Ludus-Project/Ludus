#include <ludus/foundation/base/types.h>

#include <ludus/foundation/memory/allocation_statistics.hpp>
#include <ludus/foundation/profiling/memory_capture.hpp>
#include <ludus/foundation/threading/thread.hpp>

#include <cstdio>

namespace
{
using namespace ludus::foundation;
using namespace ludus::foundation::profiling;

constexpr usize BLOCK_BYTES = 64;
constexpr usize ALIGNMENT = 16;
constexpr usize BURST_COUNT = 3;
constexpr uint32 BURST_LINE = 71;
constexpr int EXPECTED_ARGUMENT_COUNT = 5;

void* Allocate(void*, usize bytes, usize alignment) noexcept
{
    return GetSystemAllocationDomain().TryAllocate(bytes, alignment);
}

void Free(void*, void* pointer, usize bytes, usize alignment) noexcept
{
    GetSystemAllocationDomain().Free(pointer, bytes, alignment);
}

constinit AllocationDomain gDomain{nullptr, Allocate, Free};

template <usize Size>
constexpr BorrowedView<const char> GetText(const char (&text)[Size]) noexcept
{
    return {text, Size - 1};
}

bool WriteFileChunk(void* contextOrNull, BorrowedView<const char> bytes) noexcept
{
    std::FILE* file = static_cast<std::FILE*>(contextOrNull);
    return std::fwrite(bytes.GetDataOrNull(), 1, bytes.GetSize(), file) == bytes.GetSize();
}

bool RejectChunk(void*, BorrowedView<const char>) noexcept
{
    return false;
}

bool ExportToFile(MemoryCaptureId capture, const char* path) noexcept
{
    std::FILE* fileOrNull = std::fopen(path, "wb");
    if (fileOrNull == nullptr)
    {
        return false;
    }
    const MemoryCaptureStatus status = ExportMemoryCapture(capture, WriteFileChunk, fileOrNull);
    const int closeStatus = std::fclose(fileOrNull);
    return status == MemoryCaptureStatus::Ok && closeStatus == 0;
}

void FreeOnWorker(void* contextOrNull) noexcept
{
    gDomain.Free(contextOrNull, BLOCK_BYTES, ALIGNMENT);
}

bool RunBurst(AllocationSiteId site) noexcept
{
    // Each address is free to be reused on the next iteration.
    for (usize iteration = 0; iteration < BURST_COUNT; ++iteration)
    {
        void* pointerOrNull = gDomain.TryAllocateAtSite(BLOCK_BYTES, ALIGNMENT, site);
        if (pointerOrNull == nullptr)
        {
            return false;
        }
        if (iteration == 0)
        {
            threading::Thread worker;
            if (worker.Start(FreeOnWorker, pointerOrNull) != threading::ThreadStatus::Ok)
            {
                gDomain.Free(pointerOrNull, BLOCK_BYTES, ALIGNMENT);
                return false;
            }
            if (worker.Join() != threading::ThreadStatus::Ok)
            {
                return false;
            }
        }
        else
        {
            gDomain.Free(pointerOrNull, BLOCK_BYTES, ALIGNMENT);
        }
    }
    return true;
}

bool RunCapture(const MemoryActivityConfig& config,
                AllocationSiteId site,
                MemoryCaptureStatus expected,
                const char* path,
                MemoryCaptureId& outCapture) noexcept
{
    MemoryCaptureId capture = 0;
    if (BeginMemoryCapture(config, capture) != MemoryCaptureStatus::Ok ||
        ReleaseMemoryCapture(capture) != MemoryCaptureStatus::WrongState ||
        ExportMemoryCapture(capture, RejectChunk, nullptr) != MemoryCaptureStatus::WrongState ||
        RunBurst(site) == false || EndMemoryCapture(capture) != expected)
    {
        return false;
    }
    MemoryCaptureId untouched = capture;
    if (BeginMemoryCapture(config, untouched) != MemoryCaptureStatus::Busy || untouched != capture ||
        ExportMemoryCapture(capture, RejectChunk, nullptr) != MemoryCaptureStatus::WriteFailed ||
        ExportToFile(capture, path) == false || ReleaseMemoryCapture(capture) != MemoryCaptureStatus::Ok ||
        EndMemoryCapture(capture) != MemoryCaptureStatus::UnknownCapture)
    {
        return false;
    }
    outCapture = capture;
    return true;
}

bool RunTableLoss(const AllocationDomainId domain, AllocationSiteId site, const char* path) noexcept
{
    const MemoryActivityConfig config{ .Domains = {&domain, 1}, .LiveSlots = 2 };
    MemoryCaptureId capture = 0;
    if (BeginMemoryCapture(config, capture) != MemoryCaptureStatus::Ok)
    {
        return false;
    }
    void* firstOrNull = gDomain.TryAllocateAtSite(BLOCK_BYTES, ALIGNMENT, site);
    void* secondOrNull = gDomain.TryAllocateAtSite(BLOCK_BYTES, ALIGNMENT, site);
    gDomain.Free(firstOrNull, BLOCK_BYTES, ALIGNMENT);
    gDomain.Free(secondOrNull, BLOCK_BYTES, ALIGNMENT);
    return firstOrNull != nullptr && secondOrNull != nullptr &&
           EndMemoryCapture(capture) == MemoryCaptureStatus::Incomplete && ExportToFile(capture, path) == true &&
           ReleaseMemoryCapture(capture) == MemoryCaptureStatus::Ok;
}

bool RunUnknownBaseline(const AllocationDomainId domain, AllocationSiteId site, const char* path) noexcept
{
    void* olderOrNull = gDomain.TryAllocateAtSite(BLOCK_BYTES, ALIGNMENT, site);
    if (olderOrNull == nullptr)
    {
        return false;
    }
    const MemoryActivityConfig config{ .Domains = {&domain, 1} };
    MemoryCaptureId capture = 0;
    const MemoryCaptureStatus start = BeginMemoryCapture(config, capture);
    gDomain.Free(olderOrNull, BLOCK_BYTES, ALIGNMENT);
    return start == MemoryCaptureStatus::Ok && EndMemoryCapture(capture) == MemoryCaptureStatus::Ok &&
           ExportToFile(capture, path) == true && ReleaseMemoryCapture(capture) == MemoryCaptureStatus::Ok;
}
} // namespace

int main(int argumentCount, char** arguments)
{
    if (argumentCount != EXPECTED_ARGUMENT_COUNT)
    {
        return 1;
    }
    AllocationDomainId domain = 0;
    AllocationSiteId site = 0;
    const MemorySiteDefinition definition =
    {
        .SourcePath = GetText("tests/sdk_consumer/memory_activity/main.cpp"),
        .Function = GetText("RunBurst"),
        .Module = GetText("installed-sdk-memory-activity"),
        .Line = BURST_LINE,
    };
    if (TryRegisterAllocationDomain(gDomain, "Acceptance.Burst", domain) != AllocationDomainRegistrationStatus::Ok ||
        RegisterMemorySite(definition, site) != MemoryCaptureStatus::Ok)
    {
        return 1;
    }
    const MemoryActivityConfig full{ .Domains = {&domain, 1} };
    const MemoryActivityConfig small{ .Domains = {&domain, 1}, .Events = 1 };
    MemoryCaptureId first = 0;
    MemoryCaptureId second = 0;
    return RunCapture(full, site, MemoryCaptureStatus::Ok, arguments[1], first) == true &&
                   RunCapture(small, site, MemoryCaptureStatus::Incomplete, arguments[2], second) == true &&
                   second > first && RunTableLoss(domain, site, arguments[3]) == true &&
                   RunUnknownBaseline(domain, site, arguments[4]) == true
               ? 0
               : 1;
}
