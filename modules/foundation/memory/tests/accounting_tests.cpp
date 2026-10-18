// Per-domain accounting, registry and coverage manifest (memory profiling
// phase 1). Contracts: docs/architecture/memory-profiling.md ("Questions and
// metric contracts") and docs/development/memory-phase-1.md.
#include <ludus/foundation/base/core.h>

#include <ludus/foundation/containers/array.hpp>
#include <ludus/foundation/containers/static_array.hpp>
#include <ludus/foundation/memory/allocation_domain.hpp>
#include <ludus/foundation/memory/allocation_statistics.hpp>

#include "internal/accounting.hpp"

#include <atomic>
#include <new>
#include <string_view>
#include <thread>

#include <catch2/catch_test_macros.hpp>

namespace
{
using ludus::foundation::AccountingValidity;
using ludus::foundation::AllocationCoverageExclusion;
using ludus::foundation::AllocationDomain;
using ludus::foundation::AllocationDomainId;
using ludus::foundation::AllocationDomainRegistrationStatus;
using ludus::foundation::AllocationStatistics;
using ludus::foundation::Array;
using ludus::foundation::CONTAINERS_ALLOCATION_DOMAIN_ID;
using ludus::foundation::GetAllocationCoverageExclusion;
using ludus::foundation::GetAllocationCoverageExclusionCount;
using ludus::foundation::GetAllocationStatistics;
using ludus::foundation::GetContainerAllocationDomain;
using ludus::foundation::GetRegisteredAllocationDomainCount;
using ludus::foundation::GetSystemAllocationDomain;
using ludus::foundation::MAX_ALLOCATION_DOMAIN_NAME_BYTES;
using ludus::foundation::MAX_ALLOCATION_DOMAINS;
using ludus::foundation::RegisteredAllocationDomain;
using ludus::foundation::StaticArray;
using ludus::foundation::SYSTEM_ALLOCATION_DOMAIN_ID;
using ludus::foundation::TryGetRegisteredAllocationDomain;
using ludus::foundation::TryRegisterAllocationDomain;
using ludus::foundation::uint32;
using ludus::foundation::uint64;
using ludus::foundation::uint8;
using ludus::foundation::usize;
using ludus::foundation::memory_internal::AllocationCounters;
namespace memory_internal = ludus::foundation::memory_internal;

constexpr usize MAX_USIZE = ~usize{0};
constexpr uint64 MAX_UINT64 = ~uint64{0};

// Backend that forwards to aligned global new/delete and counts backend calls,
// so tests can tell a rejected request (no backend call) from a failure.
struct BackendRecord
{
    std::atomic<usize> AllocateCalls{0};
    std::atomic<usize> FreeCalls{0};
    std::atomic<bool> ShouldFail{false};
};

void* RecordAllocate(void* context, usize bytes, usize alignment) noexcept
{
    BackendRecord& record = *static_cast<BackendRecord*>(context);
    record.AllocateCalls.fetch_add(1, std::memory_order_relaxed);
    if (record.ShouldFail.load(std::memory_order_relaxed) == true)
    {
        return nullptr;
    }
    return ::operator new(bytes, std::align_val_t{alignment}, std::nothrow);
}

// NOLINTNEXTLINE(bugprone-easily-swappable-parameters): AllocationDomain::FreeFunction signature
void RecordFree(void* context, void* pointer, usize, usize alignment) noexcept
{
    BackendRecord& record = *static_cast<BackendRecord*>(context);
    record.FreeCalls.fetch_add(1, std::memory_order_relaxed);
    ::operator delete(pointer, std::align_val_t{alignment});
}

// A domain bound to its own backend record; each test constructs a fresh one.
struct RecordedDomain
{
    BackendRecord Record;
    AllocationDomain Domain{&Record, RecordAllocate, RecordFree};
};

void RequireReconciled(const AllocationStatistics& statistics)
{
    REQUIRE(statistics.Validity == AccountingValidity::Valid);
    REQUIRE(statistics.LiveRequestedBytes == statistics.AllocatedBytes - statistics.FreedBytes);
    REQUIRE(statistics.LiveBlocks == statistics.AllocationCount - statistics.FreeCount);
}
} // namespace

TEST_CASE("Domain counters follow allocations, frees and the high-water mark", "[memory][accounting]")
{
    RecordedDomain recorded;
    const AllocationDomain& domain = recorded.Domain;
    AllocationStatistics statistics = GetAllocationStatistics(domain);
    REQUIRE(statistics.LiveRequestedBytes == 0);
    REQUIRE(statistics.PeakRequestedBytes == 0);
    REQUIRE(statistics.Validity == AccountingValidity::Valid);

    void* first = domain.TryAllocate(100, 8);
    void* second = domain.TryAllocate(50, 64);
    REQUIRE(first != nullptr);
    REQUIRE(second != nullptr);
    statistics = GetAllocationStatistics(domain);
    REQUIRE(statistics.LiveRequestedBytes == 150);
    REQUIRE(statistics.LiveBlocks == 2);
    REQUIRE(statistics.PeakRequestedBytes == 150);
    REQUIRE(statistics.AllocatedBytes == 150);
    REQUIRE(statistics.AllocationCount == 2);

    domain.Free(first, 100, 8);
    void* third = domain.TryAllocate(20, 16);
    REQUIRE(third != nullptr);
    statistics = GetAllocationStatistics(domain);
    REQUIRE(statistics.LiveRequestedBytes == 70);
    REQUIRE(statistics.LiveBlocks == 2);
    REQUIRE(statistics.PeakRequestedBytes == 150);
    REQUIRE(statistics.AllocatedBytes == 170);
    REQUIRE(statistics.FreedBytes == 100);
    REQUIRE(statistics.FreeCount == 1);

    domain.Free(second, 50, 64);
    domain.Free(third, 20, 16);
    statistics = GetAllocationStatistics(domain);
    RequireReconciled(statistics);
    REQUIRE(statistics.LiveRequestedBytes == 0);
    REQUIRE(statistics.LiveBlocks == 0);
    REQUIRE(statistics.PeakRequestedBytes == 150);
    REQUIRE(statistics.FailureCount == 0);
    REQUIRE(statistics.RejectedCount == 0);
}

TEST_CASE("Zero-size requests and null frees create no block and change no counter", "[memory][accounting]")
{
    RecordedDomain recorded;
    const AllocationDomain& domain = recorded.Domain;
    REQUIRE(domain.TryAllocate(0, 16) == nullptr);
    domain.Free(nullptr, 0, 16);
    domain.Free(nullptr, 64, 16);

    const AllocationStatistics statistics = GetAllocationStatistics(domain);
    REQUIRE(statistics.AllocationCount == 0);
    REQUIRE(statistics.FreeCount == 0);
    REQUIRE(statistics.FailureCount == 0);
    REQUIRE(statistics.RejectedCount == 0);
    REQUIRE(statistics.LiveBlocks == 0);
    REQUIRE(statistics.Validity == AccountingValidity::Valid);
    REQUIRE(recorded.Record.AllocateCalls.load() == 0);
    REQUIRE(recorded.Record.FreeCalls.load() == 0);
}

TEST_CASE("Rejected requests never reach the backend and are distinct from failures", "[memory][accounting]")
{
    RecordedDomain recorded;
    const AllocationDomain& domain = recorded.Domain;
    REQUIRE(domain.TryAllocate(64, 0) == nullptr);
    REQUIRE(domain.TryAllocate(64, 24) == nullptr);
    REQUIRE(domain.TryAllocate(0, 3) == nullptr); // An invalid empty layout is still rejected.

    const AllocationDomain missingFree{&recorded.Record, RecordAllocate, nullptr};
    REQUIRE(missingFree.TryAllocate(64, 16) == nullptr);

    AllocationStatistics statistics = GetAllocationStatistics(domain);
    REQUIRE(statistics.RejectedCount == 3);
    REQUIRE(statistics.FailureCount == 0);
    REQUIRE(GetAllocationStatistics(missingFree).RejectedCount == 1);
    REQUIRE(recorded.Record.AllocateCalls.load() == 0);

    recorded.Record.ShouldFail.store(true);
    REQUIRE(domain.TryAllocate(64, 16) == nullptr);
    REQUIRE(domain.TryAllocate(128, 16) == nullptr);
    statistics = GetAllocationStatistics(domain);
    REQUIRE(statistics.FailureCount == 2);
    REQUIRE(statistics.RejectedCount == 3);
    REQUIRE(statistics.AllocationCount == 0);
    REQUIRE(statistics.LiveRequestedBytes == 0);
    REQUIRE(statistics.Validity == AccountingValidity::Valid);
    REQUIRE(recorded.Record.AllocateCalls.load() == 2);
}

TEST_CASE("A free on another thread charges the domain after the allocating thread exits", "[memory][accounting]")
{
    constexpr usize BLOCK_COUNT = 64;
    constexpr usize BLOCK_BYTES = 48;
    constexpr usize BLOCK_ALIGNMENT = 16;
    RecordedDomain recorded;
    const AllocationDomain& domain = recorded.Domain;
    StaticArray<void*, BLOCK_COUNT> blocks{};

    std::thread allocator([&] {
        for (void*& block : blocks)
        {
            block = domain.TryAllocate(BLOCK_BYTES, BLOCK_ALIGNMENT);
        }
    });
    allocator.join();

    AllocationStatistics statistics = GetAllocationStatistics(domain);
    REQUIRE(statistics.LiveBlocks == BLOCK_COUNT);
    REQUIRE(statistics.LiveRequestedBytes == BLOCK_COUNT * BLOCK_BYTES);

    for (void* block : blocks)
    {
        REQUIRE(block != nullptr);
        domain.Free(block, BLOCK_BYTES, BLOCK_ALIGNMENT);
    }
    statistics = GetAllocationStatistics(domain);
    RequireReconciled(statistics);
    REQUIRE(statistics.LiveBlocks == 0);
    REQUIRE(statistics.FreeCount == BLOCK_COUNT);
    REQUIRE(statistics.PeakRequestedBytes == BLOCK_COUNT * BLOCK_BYTES);
}

TEST_CASE("Concurrent allocation and remote frees reconcile at quiescence", "[memory][accounting]")
{
    constexpr uint32 THREAD_COUNT = 4;
    constexpr usize OPERATIONS_PER_THREAD = 4096;
    constexpr usize SLOT_COUNT = 32;
    constexpr usize BLOCK_ALIGNMENT = 16;
    RecordedDomain recorded;
    const AllocationDomain& domain = recorded.Domain;

    // Each producer leaves its last blocks live; the main thread frees them
    // after every producer has exited.
    StaticArray<StaticArray<void*, SLOT_COUNT>, THREAD_COUNT> survivors{};
    StaticArray<StaticArray<usize, SLOT_COUNT>, THREAD_COUNT> survivorBytes{};
    Array<std::thread> threads;
    for (uint32 t = 0; t < THREAD_COUNT; ++t)
    {
        threads.AddInPlace([&domain, &survivors, &survivorBytes, t] {
            StaticArray<void*, SLOT_COUNT>& slots = survivors[t];
            StaticArray<usize, SLOT_COUNT>& sizes = survivorBytes[t];
            uint64 state = 0x9e3779b97f4a7c15ULL + t;
            for (usize operation = 0; operation < OPERATIONS_PER_THREAD; ++operation)
            {
                state = state * 6364136223846793005ULL + 1442695040888963407ULL;
                const usize slot = static_cast<usize>(state >> 59U);
                if (slots[slot] != nullptr)
                {
                    domain.Free(slots[slot], sizes[slot], BLOCK_ALIGNMENT);
                    slots[slot] = nullptr;
                    continue;
                }
                const usize bytes = 1 + static_cast<usize>((state >> 32U) % 512U);
                slots[slot] = domain.TryAllocate(bytes, BLOCK_ALIGNMENT);
                sizes[slot] = bytes;
            }
        });
    }
    for (std::thread& thread : threads)
    {
        thread.join();
    }

    AllocationStatistics statistics = GetAllocationStatistics(domain);
    RequireReconciled(statistics);
    usize expectedLiveBytes = 0;
    usize expectedLiveBlocks = 0;
    for (uint32 t = 0; t < THREAD_COUNT; ++t)
    {
        for (usize slot = 0; slot < SLOT_COUNT; ++slot)
        {
            if (survivors[t][slot] != nullptr)
            {
                expectedLiveBytes += survivorBytes[t][slot];
                ++expectedLiveBlocks;
            }
        }
    }
    REQUIRE(statistics.LiveRequestedBytes == expectedLiveBytes);
    REQUIRE(statistics.LiveBlocks == expectedLiveBlocks);

    for (uint32 t = 0; t < THREAD_COUNT; ++t)
    {
        for (usize slot = 0; slot < SLOT_COUNT; ++slot)
        {
            domain.Free(survivors[t][slot], survivorBytes[t][slot], BLOCK_ALIGNMENT);
        }
    }
    statistics = GetAllocationStatistics(domain);
    RequireReconciled(statistics);
    REQUIRE(statistics.LiveRequestedBytes == 0);
    REQUIRE(statistics.LiveBlocks == 0);
    REQUIRE(statistics.AllocationCount == recorded.Record.AllocateCalls.load());
    REQUIRE(statistics.FreeCount == recorded.Record.FreeCalls.load());
}

TEST_CASE("Counter overflow saturates stickily without becoming an accounting error", "[memory][accounting]")
{
    SECTION("Live requested bytes")
    {
        AllocationCounters counters;
        counters.LiveRequestedBytes = MAX_USIZE - 8;
        counters.PeakRequestedBytes = MAX_USIZE - 8;
        memory_internal::RecordAllocation(counters, 16);
        AllocationStatistics statistics = memory_internal::ReadStatistics(counters);
        REQUIRE(statistics.Validity == AccountingValidity::Saturated);
        REQUIRE(statistics.PeakRequestedBytes == MAX_USIZE - 8); // A wrapped value never becomes a peak.

        // Releasing from the wrapped value would look like an underflow; it is
        // a consequence of the saturation, not a bad free.
        memory_internal::RecordFree(counters, 64);
        statistics = memory_internal::ReadStatistics(counters);
        REQUIRE(statistics.Validity == AccountingValidity::Saturated);
    }
    SECTION("Cumulative allocated bytes")
    {
        AllocationCounters counters;
        counters.AllocatedBytes = MAX_UINT64 - 4;
        memory_internal::RecordAllocation(counters, 4);
        REQUIRE(memory_internal::ReadStatistics(counters).Validity == AccountingValidity::Valid);
        memory_internal::RecordAllocation(counters, 1);
        REQUIRE(memory_internal::ReadStatistics(counters).Validity == AccountingValidity::Saturated);
    }
    SECTION("Allocation and free counts")
    {
        AllocationCounters counters;
        counters.AllocationCount = MAX_UINT64;
        memory_internal::RecordAllocation(counters, 8);
        REQUIRE(memory_internal::ReadStatistics(counters).Validity == AccountingValidity::Saturated);

        AllocationCounters freeCounters;
        freeCounters.LiveRequestedBytes = 8;
        freeCounters.LiveBlocks = 1;
        freeCounters.FreeCount = MAX_UINT64;
        memory_internal::RecordFree(freeCounters, 8);
        REQUIRE(memory_internal::ReadStatistics(freeCounters).Validity == AccountingValidity::Saturated);
    }
    SECTION("Failure and rejection counts")
    {
        AllocationCounters counters;
        counters.FailureCount = MAX_UINT64;
        memory_internal::RecordFailure(counters);
        REQUIRE(memory_internal::ReadStatistics(counters).Validity == AccountingValidity::Saturated);

        AllocationCounters rejectedCounters;
        rejectedCounters.RejectedCount = MAX_UINT64;
        memory_internal::RecordRejection(rejectedCounters);
        REQUIRE(memory_internal::ReadStatistics(rejectedCounters).Validity == AccountingValidity::Saturated);
    }
    SECTION("Saturation survives a later return to plausible values")
    {
        AllocationCounters counters;
        counters.LiveBlocks = MAX_USIZE;
        memory_internal::RecordAllocation(counters, 8);
        memory_internal::RecordFree(counters, 8);
        memory_internal::RecordAllocation(counters, 8);
        REQUIRE(memory_internal::ReadStatistics(counters).Validity == AccountingValidity::Saturated);
    }
}

TEST_CASE("Freeing more than is live is a sticky accounting error", "[memory][accounting]")
{
    SECTION("Injected underflow")
    {
        AllocationCounters counters;
        memory_internal::RecordFree(counters, 32);
        REQUIRE(memory_internal::ReadStatistics(counters).Validity == AccountingValidity::AccountingError);
        memory_internal::RecordAllocation(counters, 32);
        REQUIRE(memory_internal::ReadStatistics(counters).Validity == AccountingValidity::AccountingError);
    }
    SECTION("Free with a layout no allocation can have")
    {
        RecordedDomain recorded;
        const AllocationDomain& domain = recorded.Domain;
        void* block = domain.TryAllocate(32, 16);
        REQUIRE(block != nullptr);
        domain.Free(block, 0, 16);
        // The block is still released, preserving the backend contract.
        REQUIRE(recorded.Record.FreeCalls.load() == 1);
        REQUIRE(GetAllocationStatistics(domain).Validity == AccountingValidity::AccountingError);
    }
    SECTION("A later saturation does not hide an earlier error")
    {
        AllocationCounters counters;
        memory_internal::RecordFree(counters, 1);
        counters.AllocationCount = MAX_UINT64;
        memory_internal::RecordAllocation(counters, 1);
        REQUIRE(memory_internal::ReadStatistics(counters).Validity == AccountingValidity::AccountingError);
    }
}

TEST_CASE("Built-in domains are registered and count independently", "[memory][registry]")
{
    REQUIRE(GetRegisteredAllocationDomainCount() >= 2);
    RegisteredAllocationDomain entry;
    REQUIRE(TryGetRegisteredAllocationDomain(0, entry) == true);
    REQUIRE(entry.Id == SYSTEM_ALLOCATION_DOMAIN_ID);
    REQUIRE(entry.DomainOrNull == &GetSystemAllocationDomain());
    REQUIRE(std::string_view(entry.NameOrNull) == "System");
    REQUIRE(TryGetRegisteredAllocationDomain(1, entry) == true);
    REQUIRE(entry.Id == CONTAINERS_ALLOCATION_DOMAIN_ID);
    REQUIRE(entry.DomainOrNull == &GetContainerAllocationDomain());
    REQUIRE(std::string_view(entry.NameOrNull) == "Containers");

    const AllocationDomain& system = GetSystemAllocationDomain();
    const AllocationStatistics systemBefore = GetAllocationStatistics(system);
    const AllocationStatistics containersBefore = GetAllocationStatistics(GetContainerAllocationDomain());
    void* block = system.TryAllocate(256, 32);
    REQUIRE(block != nullptr);
    const AllocationStatistics systemDuring = GetAllocationStatistics(system);
    REQUIRE(systemDuring.LiveRequestedBytes == systemBefore.LiveRequestedBytes + 256);
    REQUIRE(systemDuring.AllocationCount == systemBefore.AllocationCount + 1);
    system.Free(block, 256, 32);
    const AllocationStatistics systemAfter = GetAllocationStatistics(system);
    REQUIRE(systemAfter.LiveRequestedBytes == systemBefore.LiveRequestedBytes);
    REQUIRE(systemAfter.FreeCount == systemBefore.FreeCount + 1);
    REQUIRE(GetAllocationStatistics(GetContainerAllocationDomain()).AllocationCount ==
            containersBefore.AllocationCount);
}

namespace
{
void* NeverAllocate(void*, usize, usize) noexcept
{
    return nullptr;
}

void NeverFree(void*, void*, usize, usize) noexcept {}

struct FillerDomain
{
    AllocationDomain Domain{nullptr, NeverAllocate, NeverFree};
};

constinit AllocationDomain gAudioTestDomain{nullptr, NeverAllocate, NeverFree};
constinit AllocationDomain gOtherTestDomain{nullptr, NeverAllocate, NeverFree};
StaticArray<FillerDomain, MAX_ALLOCATION_DOMAINS> gFillerDomains{};
} // namespace

// The registry is process-wide and never shrinks, so every registration in
// this binary happens in this one case, ending with exhaustion.
TEST_CASE("Registry validates names, keeps identities stable and reports exhaustion", "[memory][registry]")
{
    const usize initialCount = GetRegisteredAllocationDomainCount();
    AllocationDomainId audioId = 0;
    REQUIRE(TryRegisterAllocationDomain(gAudioTestDomain, "Audio.Test-PCM_1", audioId) ==
            AllocationDomainRegistrationStatus::Ok);
    REQUIRE(audioId == initialCount + 1);
    REQUIRE(GetRegisteredAllocationDomainCount() == initialCount + 1);

    AllocationDomainId repeatedId = 0;
    REQUIRE(TryRegisterAllocationDomain(gAudioTestDomain, "Audio.Test-PCM_1", repeatedId) ==
            AllocationDomainRegistrationStatus::Ok);
    REQUIRE(repeatedId == audioId);

    AllocationDomainId unchangedId = 77;
    REQUIRE(TryRegisterAllocationDomain(gOtherTestDomain, "Audio.Test-PCM_1", unchangedId) ==
            AllocationDomainRegistrationStatus::NameCollision);
    REQUIRE(TryRegisterAllocationDomain(gOtherTestDomain, "System", unchangedId) ==
            AllocationDomainRegistrationStatus::NameCollision);
    REQUIRE(TryRegisterAllocationDomain(gAudioTestDomain, "Audio.Other", unchangedId) ==
            AllocationDomainRegistrationStatus::DomainAlreadyRegistered);
    REQUIRE(TryRegisterAllocationDomain(gOtherTestDomain, "", unchangedId) ==
            AllocationDomainRegistrationStatus::InvalidName);
    REQUIRE(TryRegisterAllocationDomain(gOtherTestDomain, nullptr, unchangedId) ==
            AllocationDomainRegistrationStatus::InvalidName);
    REQUIRE(TryRegisterAllocationDomain(gOtherTestDomain, "has space", unchangedId) ==
            AllocationDomainRegistrationStatus::InvalidName);
    REQUIRE(TryRegisterAllocationDomain(gOtherTestDomain, "quote\"", unchangedId) ==
            AllocationDomainRegistrationStatus::InvalidName);

    StaticArray<char, MAX_ALLOCATION_DOMAIN_NAME_BYTES + 2> longName{};
    for (usize i = 0; i < MAX_ALLOCATION_DOMAIN_NAME_BYTES + 1; ++i)
    {
        longName[i] = 'n';
    }
    REQUIRE(TryRegisterAllocationDomain(gOtherTestDomain, longName.GetData(), unchangedId) ==
            AllocationDomainRegistrationStatus::InvalidName);
    longName[MAX_ALLOCATION_DOMAIN_NAME_BYTES] = '\0';
    AllocationDomainId longId = 0;
    REQUIRE(TryRegisterAllocationDomain(gOtherTestDomain, longName.GetData(), longId) ==
            AllocationDomainRegistrationStatus::Ok);
    REQUIRE(unchangedId == 77);

    RegisteredAllocationDomain entry;
    REQUIRE(TryGetRegisteredAllocationDomain(audioId - 1, entry) == true);
    REQUIRE(entry.DomainOrNull == &gAudioTestDomain);
    REQUIRE(std::string_view(entry.NameOrNull) == "Audio.Test-PCM_1");

    usize filler = 0;
    AllocationDomainRegistrationStatus status = AllocationDomainRegistrationStatus::Ok;
    while (status == AllocationDomainRegistrationStatus::Ok)
    {
        StaticArray<char, 16> name{};
        name[0] = 'f';
        name[1] = static_cast<char>('a' + filler / 26);
        name[2] = static_cast<char>('a' + filler % 26);
        AllocationDomainId fillerId = 0;
        status = TryRegisterAllocationDomain(gFillerDomains[filler].Domain, name.GetData(), fillerId);
        ++filler;
    }
    REQUIRE(status == AllocationDomainRegistrationStatus::RegistryFull);
    REQUIRE(GetRegisteredAllocationDomainCount() == MAX_ALLOCATION_DOMAINS);
    REQUIRE(TryGetRegisteredAllocationDomain(MAX_ALLOCATION_DOMAINS, entry) == false);
    REQUIRE(entry.DomainOrNull == &gAudioTestDomain); // Failed reads leave the output unchanged.

    // Existing identities still resolve after exhaustion.
    REQUIRE(TryRegisterAllocationDomain(gAudioTestDomain, "Audio.Test-PCM_1", repeatedId) ==
            AllocationDomainRegistrationStatus::Ok);
    REQUIRE(repeatedId == audioId);
}

TEST_CASE("Coverage manifest names the paths outside domain accounting", "[memory][coverage]")
{
    const usize count = GetAllocationCoverageExclusionCount();
    REQUIRE(count > 0);
    for (usize i = 0; i < count; ++i)
    {
        const AllocationCoverageExclusion& exclusion = GetAllocationCoverageExclusion(i);
        REQUIRE(std::string_view(exclusion.Name).empty() == false);
        REQUIRE(std::string_view(exclusion.Reason).empty() == false);
        for (usize j = 0; j < i; ++j)
        {
            REQUIRE(std::string_view(GetAllocationCoverageExclusion(j).Name) != std::string_view(exclusion.Name));
        }
    }
}
