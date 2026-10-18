// Built-in allocation domains, the stable domain registry, statistics queries
// and the coverage exclusion manifest (memory profiling phase 1).
//
// Every object here is constant-initialized and trivially destructible, so the
// built-in domains and registry work before dynamic initialization and through
// static and thread-local teardown (docs/architecture/memory-management.md §6.4).
// FoundationMemory cannot depend on FoundationContainers, so the registry uses
// plain fixed arrays instead of StaticArray.

#include <ludus/foundation/base/core.h>

#include <ludus/foundation/memory/allocation_domain.hpp>
#include <ludus/foundation/memory/allocation_statistics.hpp>

#include "internal/accounting.hpp"

#include <atomic>
#include <new>

namespace ludus::foundation
{
namespace
{
void* SystemAllocate(void*, usize bytes, usize alignment) noexcept
{
    return ::operator new(bytes, std::align_val_t{alignment}, std::nothrow);
}

void SystemFree(void*, void* pointer, usize, usize alignment) noexcept
{
    ::operator delete(pointer, std::align_val_t{alignment});
}

constinit AllocationDomain gSystemDomain{nullptr, SystemAllocate, SystemFree};
constinit AllocationDomain gContainerDomain{nullptr, SystemAllocate, SystemFree};

constexpr usize NAME_STORAGE_BYTES = MAX_ALLOCATION_DOMAIN_NAME_BYTES + 1;
constexpr usize BUILT_IN_DOMAIN_COUNT = 2;

struct RegistryEntry
{
    const AllocationDomain* DomainOrNull = nullptr;
    char Name[NAME_STORAGE_BYTES] = {};
};

// Entries below gPublishedCount are immutable. Writers append under gRegistryLock
// and publish the count with release ordering; readers acquire it.
constinit RegistryEntry gRegistry[MAX_ALLOCATION_DOMAINS] = {
    { .DomainOrNull = &gSystemDomain, .Name = "System" },
    { .DomainOrNull = &gContainerDomain, .Name = "Containers" },
};
constinit std::atomic<usize> gPublishedCount{BUILT_IN_DOMAIN_COUNT};
// Registration is cold and short; a trivially destructible spin lock stays
// usable during static teardown, unlike a destructed mutex.
constinit std::atomic<bool> gRegistryLock{false};

static_assert(SYSTEM_ALLOCATION_DOMAIN_ID == 1, "System is the first built-in registry entry");
static_assert(CONTAINERS_ALLOCATION_DOMAIN_ID == 2, "Containers is the second built-in registry entry");

class RegistryLockScope final
{
public:
    RegistryLockScope() noexcept
    {
        while (gRegistryLock.exchange(true, std::memory_order_acquire) == true)
        {
        }
    }
    ~RegistryLockScope()
    {
        gRegistryLock.store(false, std::memory_order_release);
    }
    RegistryLockScope(const RegistryLockScope&) = delete;
    RegistryLockScope& operator=(const RegistryLockScope&) = delete;
    RegistryLockScope(RegistryLockScope&&) = delete;
    RegistryLockScope& operator=(RegistryLockScope&&) = delete;
};

[[nodiscard]] bool IsNameCharacter(char character) noexcept
{
    return (character >= 'a' && character <= 'z') || (character >= 'A' && character <= 'Z') ||
           (character >= '0' && character <= '9') || character == '.' || character == '_' || character == '-';
}

// Returns the name length, or zero when the name is empty, too long or has a
// character outside the allowed set. Never reads past the allowed length + 1.
[[nodiscard]] usize MeasureValidName(const char* name) noexcept
{
    usize length = 0;
    while (name[length] != '\0')
    {
        if (length == MAX_ALLOCATION_DOMAIN_NAME_BYTES || IsNameCharacter(name[length]) == false)
        {
            return 0;
        }
        ++length;
    }
    return length;
}

[[nodiscard]] bool AreNamesEqual(const char* storedName, const char* name, usize nameLength) noexcept
{
    for (usize i = 0; i < nameLength; ++i)
    {
        if (storedName[i] != name[i])
        {
            return false;
        }
    }
    return storedName[nameLength] == '\0';
}

[[nodiscard]] AllocationDomainId ToDomainId(usize index) noexcept
{
    return static_cast<AllocationDomainId>(index + 1);
}

constexpr usize EXCLUSION_COUNT = 7;

// Known allocation paths outside domain accounting. Keep in step with the
// coverage table in docs/development/memory-phase-1.md.
constexpr AllocationCoverageExclusion EXCLUSIONS[EXCLUSION_COUNT] = {
    {
        .Name = "unregistered-domains",
        .Reason = "AllocationDomain objects that were never registered count their own traffic but are not enumerated",
    },
    {
        .Name = "direct-engine-allocation",
        .Reason = "Engine new/delete, malloc and UniquePtr default deleters that do not call an AllocationDomain",
    },
    {
        .Name = "standard-library-storage",
        .Reason = "std::string, std::function, std::thread and other standard-library internal allocations",
    },
    {
        .Name = "third-party-libraries",
        .Reason = "FreeType, HarfBuzz, decoders and other native library heaps without adapted callbacks",
    },
    {
        .Name = "game-host-module-backing",
        .Reason = "GameHost module allocations, reported separately as host backing bytes",
    },
    {
        .Name = "graphics-driver-and-gpu",
        .Reason = "Driver CPU heaps, GPU device memory and browser graphics implementations",
    },
    {
        .Name = "process-runtime",
        .Reason = "Static storage, thread stacks, thread-local storage, loader and C runtime memory",
    },
};
} // namespace

const AllocationDomain& GetSystemAllocationDomain() noexcept
{
    return gSystemDomain;
}

const AllocationDomain& GetContainerAllocationDomain() noexcept
{
    return gContainerDomain;
}

AllocationStatistics GetAllocationStatistics(const AllocationDomain& domain) noexcept
{
    return memory_internal::ReadStatistics(AllocationAccounting::GetCounters(domain));
}

AllocationDomainRegistrationStatus
TryRegisterAllocationDomain(const AllocationDomain& domain, const char* name, AllocationDomainId& outId) noexcept
{
    if (name == nullptr)
    {
        return AllocationDomainRegistrationStatus::InvalidName;
    }
    const usize nameLength = MeasureValidName(name);
    if (nameLength == 0)
    {
        return AllocationDomainRegistrationStatus::InvalidName;
    }

    const RegistryLockScope lock;
    const usize count = gPublishedCount.load(std::memory_order_relaxed);
    for (usize index = 0; index < count; ++index)
    {
        const RegistryEntry& entry = gRegistry[index];
        const bool isSameDomain = entry.DomainOrNull == &domain;
        const bool isSameName = AreNamesEqual(entry.Name, name, nameLength);
        if (isSameDomain == true && isSameName == true)
        {
            outId = ToDomainId(index);
            return AllocationDomainRegistrationStatus::Ok;
        }
        if (isSameName == true)
        {
            return AllocationDomainRegistrationStatus::NameCollision;
        }
        if (isSameDomain == true)
        {
            return AllocationDomainRegistrationStatus::DomainAlreadyRegistered;
        }
    }
    if (count == MAX_ALLOCATION_DOMAINS)
    {
        return AllocationDomainRegistrationStatus::RegistryFull;
    }

    RegistryEntry& entry = gRegistry[count];
    entry.DomainOrNull = &domain;
    for (usize i = 0; i < nameLength; ++i)
    {
        entry.Name[i] = name[i];
    }
    entry.Name[nameLength] = '\0';
    gPublishedCount.store(count + 1, std::memory_order_release);
    outId = ToDomainId(count);
    return AllocationDomainRegistrationStatus::Ok;
}

usize GetRegisteredAllocationDomainCount() noexcept
{
    return gPublishedCount.load(std::memory_order_acquire);
}

bool TryGetRegisteredAllocationDomain(usize index, RegisteredAllocationDomain& outEntry) noexcept
{
    if (index >= gPublishedCount.load(std::memory_order_acquire))
    {
        return false;
    }
    const RegistryEntry& entry = gRegistry[index];
    outEntry.Id = ToDomainId(index);
    outEntry.DomainOrNull = entry.DomainOrNull;
    outEntry.NameOrNull = entry.Name;
    return true;
}

usize GetAllocationCoverageExclusionCount() noexcept
{
    return EXCLUSION_COUNT;
}

const AllocationCoverageExclusion& GetAllocationCoverageExclusion(usize index) noexcept
{
    LUDUS_REQUIRE(index < EXCLUSION_COUNT, "Allocation coverage exclusion index out of range");
    return EXCLUSIONS[index];
}
} // namespace ludus::foundation
