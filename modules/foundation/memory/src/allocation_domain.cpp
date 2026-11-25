#include <ludus/foundation/memory/allocation_domain.hpp>

#include "internal/accounting.hpp"
#include "internal/observation.hpp"

namespace ludus::foundation
{
namespace
{
[[nodiscard]] LUDUS_INLINE bool IsValidAlignment(usize alignment) noexcept
{
    return alignment != 0 && (alignment & (alignment - 1)) == 0;
}
} // namespace

void* AllocationDomain::TryAllocate(usize bytes, usize alignment) const noexcept
{
    return TryAllocateAtSite(bytes, alignment, 0);
}

void* AllocationDomain::TryAllocateAtSite(usize bytes, usize alignment, AllocationSiteId site) const noexcept
{
    if (IsValidAlignment(alignment) == false || mAllocate == nullptr || mFree == nullptr)
    {
        memory_internal::RecordRejection(AllocationAccounting::GetCounters(*this));
        return nullptr;
    }
    if (bytes == 0)
    {
        // An empty request succeeds without a block or a lifetime.
        return nullptr;
    }
    memory_internal::Observation* observationOrNull = mObservationOrNull;
    if (observationOrNull == nullptr)
    {
        return allocateAndCount(bytes, alignment);
    }
    const memory_internal::ObservationScope scope(*observationOrNull);
    void* pointerOrNull = allocateAndCount(bytes, alignment);
    if (scope.IsOutermost() == true)
    {
        const memory_internal::ActivityKind kind =
            pointerOrNull == nullptr ? memory_internal::ActivityKind::Failure : memory_internal::ActivityKind::Allocate;
        observationOrNull->Record(kind, mObservationDomain, pointerOrNull, bytes, alignment, site);
    }
    return pointerOrNull;
}

void* AllocationDomain::allocateAndCount(usize bytes, usize alignment) const noexcept
{
    AllocationAccounting::Counters& counters = AllocationAccounting::GetCounters(*this);
    void* pointerOrNull = mAllocate(mContext, bytes, alignment);
    if (LUDUS_UNLIKELY(pointerOrNull == nullptr))
    {
        memory_internal::RecordFailure(counters);
        return nullptr;
    }
    memory_internal::RecordAllocation(counters, bytes);
    return pointerOrNull;
}

void AllocationDomain::Free(void* pointerOrNull, usize bytes, usize alignment) const noexcept
{
    if (pointerOrNull == nullptr)
    {
        return;
    }
    memory_internal::Observation* observationOrNull = mObservationOrNull;
    if (observationOrNull == nullptr)
    {
        freeAndCount(pointerOrNull, bytes, alignment);
        return;
    }
    const memory_internal::ObservationScope scope(*observationOrNull);
    if (scope.IsOutermost() == true)
    {
        observationOrNull->Record(memory_internal::ActivityKind::Free,
                                  mObservationDomain,
                                  pointerOrNull,
                                  bytes,
                                  alignment);
    }
    freeAndCount(pointerOrNull, bytes, alignment);
}

void AllocationDomain::freeAndCount(void* pointer, usize bytes, usize alignment) const noexcept
{
    AllocationAccounting::Counters& counters = AllocationAccounting::GetCounters(*this);
    if (LUDUS_UNLIKELY(bytes == 0 || IsValidAlignment(alignment) == false))
    {
        // No successful allocation has this layout. Keep the previous release
        // behavior, but stop presenting the counters as exact.
        memory_internal::MarkAccountingError(counters);
    }
    else
    {
        memory_internal::RecordFree(counters, bytes);
    }
    mFree(mContext, pointer, bytes, alignment);
}
} // namespace ludus::foundation
