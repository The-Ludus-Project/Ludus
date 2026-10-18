#pragma once

// Failure-injecting allocation domain shared by the FoundationMemory factory
// tests (owned_object_tests.cpp, shared_ptr_tests.cpp). Every successful
// allocation is recorded so each free can be matched against the exact
// pointer, size and alignment it came from.
#include <ludus/foundation/base/core.h>

#include <ludus/foundation/containers/static_array.hpp>
#include <ludus/foundation/memory/allocation_domain.hpp>

namespace ludus_memory_tests
{
using ludus::foundation::AllocationDomain;
using ludus::foundation::GetSystemAllocationDomain;
using ludus::foundation::StaticArray;
using ludus::foundation::usize;

inline constexpr usize MAX_LIVE_ALLOCATIONS = 8;
inline constexpr usize NEVER_FAIL = 0;

struct LiveAllocation
{
    void* StorageOrNull = nullptr;
    usize Bytes = 0;
    usize Alignment = 0;
};

// Failure-injecting domain state. Every successful allocation is recorded so a
// free can be matched against the exact pointer, size and alignment it came
// from; a free that matches nothing is counted as a mismatch.
struct DomainRecord
{
    StaticArray<LiveAllocation, MAX_LIVE_ALLOCATIONS> Live{};
    usize AttemptCount = 0;
    usize AllocationCount = 0;
    usize FreeCount = 0;
    usize LiveCount = 0;
    usize MismatchedFreeCount = 0;
    usize FailAtAttempt = NEVER_FAIL;
    bool ShouldFailEveryAttempt = false;
    LiveAllocation LastAllocation{};
    LiveAllocation LastFree{};
};

inline void* AllocateRecorded(void* context, usize bytes, usize alignment) noexcept
{
    DomainRecord& record = *static_cast<DomainRecord*>(context);
    ++record.AttemptCount;
    if (record.ShouldFailEveryAttempt == true || record.AttemptCount == record.FailAtAttempt)
    {
        return nullptr;
    }
    for (usize index = 0; index < MAX_LIVE_ALLOCATIONS; ++index)
    {
        LiveAllocation& slot = record.Live[index];
        if (slot.StorageOrNull == nullptr)
        {
            void* storageOrNull = GetSystemAllocationDomain().TryAllocate(bytes, alignment);
            if (storageOrNull == nullptr)
            {
                return nullptr;
            }
            slot = LiveAllocation{ .StorageOrNull = storageOrNull, .Bytes = bytes, .Alignment = alignment };
            record.LastAllocation = slot;
            ++record.AllocationCount;
            ++record.LiveCount;
            return storageOrNull;
        }
    }
    return nullptr;
}

// The parameter order is fixed by AllocationDomain::FreeFunction.
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
inline void FreeRecorded(void* context, void* pointer, usize bytes, usize alignment) noexcept
{
    DomainRecord& record = *static_cast<DomainRecord*>(context);
    ++record.FreeCount;
    record.LastFree = LiveAllocation{ .StorageOrNull = pointer, .Bytes = bytes, .Alignment = alignment };
    for (usize index = 0; index < MAX_LIVE_ALLOCATIONS; ++index)
    {
        LiveAllocation& slot = record.Live[index];
        if (slot.StorageOrNull == pointer)
        {
            if (slot.Bytes != bytes || slot.Alignment != alignment)
            {
                ++record.MismatchedFreeCount;
            }
            GetSystemAllocationDomain().Free(pointer, slot.Bytes, slot.Alignment);
            slot = LiveAllocation{};
            --record.LiveCount;
            return;
        }
    }
    ++record.MismatchedFreeCount;
}

// Owns the record and the immutable domain bound to it. Declare it before any
// owner it allocates for, so the domain outlives those owners.
struct RecordingDomain
{
    DomainRecord Record{};
    AllocationDomain Domain{&Record, AllocateRecorded, FreeRecorded};
};
} // namespace ludus_memory_tests
