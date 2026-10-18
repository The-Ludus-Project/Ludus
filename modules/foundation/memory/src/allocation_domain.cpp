#include <ludus/foundation/memory/allocation_domain.hpp>

#include "internal/accounting.hpp"

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
    AllocationAccounting::Counters& counters = AllocationAccounting::GetCounters(*this);
    if (IsValidAlignment(alignment) == false || mAllocate == nullptr || mFree == nullptr)
    {
        memory_internal::RecordRejection(counters);
        return nullptr;
    }
    if (bytes == 0)
    {
        // An empty request succeeds without a block or a lifetime.
        return nullptr;
    }
    void* pointerOrNull = mAllocate(mContext, bytes, alignment);
    if (LUDUS_UNLIKELY(pointerOrNull == nullptr))
    {
        memory_internal::RecordFailure(counters);
        return nullptr;
    }
    memory_internal::RecordAllocation(counters, bytes);
    return pointerOrNull;
}

void AllocationDomain::Free(void* pointer, usize bytes, usize alignment) const noexcept
{
    if (pointer == nullptr)
    {
        return;
    }
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
