// Allocation-domain gauges on the trace timeline and their Perfetto export
// (memory profiling phase 1; docs/development/memory-phase-1.md).
#include <ludus/foundation/memory/allocation_domain.hpp>
#include <ludus/foundation/memory/allocation_statistics.hpp>
#include <ludus/foundation/profiling/memory_gauges.hpp>
#include <ludus/foundation/profiling/trace_system.hpp>

#include "internal/memory_gauge.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cstdio>
#include <new>
#include <string>

namespace
{
using ludus::foundation::AccountingValidity;
using ludus::foundation::AllocationDomain;
using ludus::foundation::AllocationDomainId;
using ludus::foundation::AllocationDomainRegistrationStatus;
using ludus::foundation::GetAllocationStatistics;
using ludus::foundation::TryRegisterAllocationDomain;
using ludus::foundation::uint32;
using ludus::foundation::uint64;
using ludus::foundation::usize;
namespace profiling = ludus::foundation::profiling;
namespace internal = ludus::foundation::profiling::internal;

void* SystemAllocate(void*, usize bytes, usize alignment) noexcept
{
    return ::operator new(bytes, std::align_val_t{alignment}, std::nothrow);
}

void SystemFree(void*, void* pointer, usize, usize alignment) noexcept
{
    ::operator delete(pointer, std::align_val_t{alignment});
}

void IgnoreFree(void*, void*, usize, usize) noexcept {}

constinit AllocationDomain gGaugeDomain{nullptr, SystemAllocate, SystemFree};
constinit AllocationDomain gInvalidDomain{nullptr, SystemAllocate, IgnoreFree};

std::string ReadFile(const std::string& path)
{
    std::string contents;
    std::FILE* file = std::fopen(path.c_str(), "rb");
    if (file == nullptr)
    {
        return contents;
    }
    char buffer[4096];
    usize read = 0;
    while ((read = std::fread(buffer, 1, sizeof(buffer), file)) > 0)
    {
        contents.append(buffer, read);
    }
    std::fclose(file);
    return contents;
}

usize CountOccurrences(const std::string& text, const std::string& pattern)
{
    usize count = 0;
    for (usize position = text.find(pattern); position != std::string::npos;
         position = text.find(pattern, position + pattern.size()))
    {
        ++count;
    }
    return count;
}

std::string ExportCapture(const char* name)
{
    const std::string path = std::string("ludus_profiling_memory_") + name + ".json";
    REQUIRE(profiling::ExportPerfettoTrace(path) == true);
    std::string json = ReadFile(path);
    std::remove(path.c_str());
    return json;
}
} // namespace

TEST_CASE("Memory gauge site encoding round-trips domain and metric", "[profiling][memory]")
{
    const uint32 site = internal::EncodeMemoryGaugeSite(31, internal::MemoryGaugeMetric::RejectedCount);
    REQUIRE(internal::DecodeMemoryGaugeDomain(site) == 31);
    REQUIRE(internal::DecodeMemoryGaugeMetric(site) == static_cast<uint32>(internal::MemoryGaugeMetric::RejectedCount));
    REQUIRE(internal::GetMemoryGaugeMetricName(static_cast<uint32>(internal::MemoryGaugeMetric::LiveBlocks)) ==
            "LiveBlocks");
}

TEST_CASE("Memory gauges are not recorded without an active capture", "[profiling][memory]")
{
    REQUIRE(profiling::RecordMemoryGauges() == false);
}

TEST_CASE("Quiescent memory gauges export as per-domain counters with a coverage manifest", "[profiling][memory]")
{
    AllocationDomainId domainId = 0;
    REQUIRE(TryRegisterAllocationDomain(gGaugeDomain, "ProfilingTest.Gauges", domainId) ==
            AllocationDomainRegistrationStatus::Ok);

    void* first = gGaugeDomain.TryAllocate(100, 16);
    void* second = gGaugeDomain.TryAllocate(28, 8);
    REQUIRE(first != nullptr);
    REQUIRE(second != nullptr);

    profiling::RegisterThreadForTrace("Main");
    REQUIRE(profiling::BeginCapture(4) == true);
    REQUIRE(profiling::RecordMemoryGauges() == true);
    gGaugeDomain.Free(second, 28, 8);
    REQUIRE(profiling::RecordMemoryGauges() == true);
    profiling::EndCapture();
    gGaugeDomain.Free(first, 100, 16);

    const std::string json = ExportCapture("quiescent");
    // Two samples of the test domain, each one counter event with every metric.
    REQUIRE(CountOccurrences(json, "\"ph\":\"C\",\"name\":\"Memory/ProfilingTest.Gauges\"") == 2);
    CHECK(json.find("\"args\":{\"Validity\":0,\"LiveRequestedBytes\":128,\"LiveBlocks\":2,\"PeakRequestedBytes\":128,"
                    "\"AllocatedBytes\":128,\"FreedBytes\":0,\"AllocationCount\":2,\"FreeCount\":0,"
                    "\"FailureCount\":0,\"RejectedCount\":0}") != std::string::npos);
    CHECK(json.find("\"args\":{\"Validity\":0,\"LiveRequestedBytes\":100,\"LiveBlocks\":1,\"PeakRequestedBytes\":128,"
                    "\"AllocatedBytes\":128,\"FreedBytes\":28,\"AllocationCount\":2,\"FreeCount\":1,") !=
          std::string::npos);
    CHECK(json.find("\"name\":\"Memory/System\"") != std::string::npos);
    CHECK(json.find("\"name\":\"Memory/Containers\"") != std::string::npos);

    // The coverage manifest names the sampled domains and the excluded paths.
    CHECK(json.find("\"metadata\":{\"ludus-memory-gauges\":{\"schema\":1") != std::string::npos);
    CHECK(json.find("{\"id\":1,\"name\":\"System\"}") != std::string::npos);
    CHECK(json.find("{\"id\":2,\"name\":\"Containers\"}") != std::string::npos);
    CHECK(json.find("\"name\":\"ProfilingTest.Gauges\"}") != std::string::npos);
    CHECK(json.find("\"excludedPaths\":[{\"name\":\"unregistered-domains\"") != std::string::npos);
    CHECK(json.find("\"name\":\"direct-engine-allocation\"") != std::string::npos);
    CHECK(json.find("never sum them into a process peak") != std::string::npos);
    CHECK(json.back() == '\n');
}

TEST_CASE("An invalid domain exports only its validity, never plausible values", "[profiling][memory]")
{
    AllocationDomainId domainId = 0;
    REQUIRE(TryRegisterAllocationDomain(gInvalidDomain, "ProfilingTest.Invalid", domainId) ==
            AllocationDomainRegistrationStatus::Ok);
    // Freeing a block the domain never allocated underflows its live counters.
    uint64 notAllocated = 0;
    gInvalidDomain.Free(&notAllocated, sizeof(notAllocated), alignof(uint64));
    REQUIRE(GetAllocationStatistics(gInvalidDomain).Validity == AccountingValidity::AccountingError);

    profiling::RegisterThreadForTrace("Main");
    REQUIRE(profiling::BeginCapture(4) == true);
    REQUIRE(profiling::RecordMemoryGauges() == true);
    profiling::EndCapture();

    const std::string json = ExportCapture("invalid");
    CHECK(json.find("\"name\":\"Memory/ProfilingTest.Invalid\",\"args\":{\"Validity\":2}}") != std::string::npos);
}
