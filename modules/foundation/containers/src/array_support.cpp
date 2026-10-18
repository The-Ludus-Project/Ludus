// Out-of-line, cold, non-template support for Array<T>.
//
// These are deliberately NOT in the header: keeping the allocation seam, growth
// math, and OOM/overflow sinks out-of-line keeps vector.hpp cheap to parse and
// keeps the per-element-type inline code small (no duplicated diagnostic
// strings, no allocation machinery inlined into every call site).

#include <ludus/foundation/base/core.h>

#include <ludus/foundation/memory/allocation_domain.hpp>

namespace ludus::foundation::core::detail
{
// The Array storage seam allocates through the process-lifetime Containers
// allocation domain (memory profiling phase 1; docs/development/memory-phase-1.md).
// Its system backend is the same aligned nothrow operator new/delete family the
// seam called directly before, so blocks, failure behavior and Array's layout
// are unchanged. The domain adds checked requested-byte accounting: Array
// charges capacity bytes, including the old and new buffers during growth.
// A null result still reports failure, so the container decides fatal versus
// fallible handling; a zero-byte request returns null without a block.
[[nodiscard]] void* AllocateBytes(usize bytes, usize alignment) noexcept
{
    return GetContainerAllocationDomain().TryAllocate(bytes, alignment);
}

void FreeBytes(void* ptr,
               usize bytes, // NOLINT(bugprone-easily-swappable-parameters): byte-size / alignment seam signature
               usize alignment) noexcept
{
    // A null pointer is ignored; otherwise the domain subtracts the requested
    // bytes before releasing the block with the original size and alignment.
    GetContainerAllocationDomain().Free(ptr, bytes, alignment);
}

// Geometric 2x growth: newCap = max(requested, currentCapacity * 2), with a
// small minimum first allocation and an overflow/ceiling check. Returns 0 to
// signal overflow (caller treats as fatal or fallible failure).
//
// Growth-factor decision (resolves containers.md section 15 / open question #1):
// the design proposed 1.5x as a hypothesis to be settled by benchmark. Measured
// on the pinned Clang 18 (see docs/architecture/containers.md "Migration results"
// and the growth simulation): for append-heavy workloads 2x performs ~40% fewer
// reallocations and copies ~30% fewer total bytes than 1.5x, with negligible
// peak-memory difference, and matches libstdc++/libc++. The 1.5x "allocator can
// reuse freed blocks" argument depends on an allocator behaviour Ludus does not
// yet have; the extra reallocation cost of 1.5x is real today. So the shipped
// default is 2x. This is one named constant (kGrowthNum/kGrowthDen) and is
// trivial to revisit once the engine allocator exists.
[[nodiscard]] usize
ComputeGrowthCapacity(usize currentCapacity, // NOLINT(bugprone-easily-swappable-parameters): growth-policy inputs
                      usize requested,
                      usize elementSize) noexcept
{
    constexpr usize kGrowthNum = 2; // 2x growth (numerator/denominator form keeps
    constexpr usize kGrowthDen = 1; // the policy explicit and easy to retune).
    // Element-count ceiling for this element size (PTRDIFF_MAX/elementSize-ish).
    constexpr usize kPtrdiffMax = static_cast<usize>(-1) >> 1;
    const usize maxCount = elementSize == 0 ? static_cast<usize>(-1) : kPtrdiffMax / elementSize;
    if (requested > maxCount)
    {
        return 0; // overflow / exceeds addressable element count
    }

    // Minimum first allocation: at least 1 element, and enough small elements to
    // fill roughly a 64-byte line so tiny types don't reallocate immediately.
    usize minCapacity = 1;
    if (elementSize != 0)
    {
        const usize perLine = 64 / elementSize;
        if (perLine > minCapacity)
        {
            minCapacity = perLine;
        }
    }

    // grown = currentCapacity * (kGrowthNum/kGrowthDen), guarding overflow.
    const usize extra = currentCapacity / kGrowthDen * (kGrowthNum - kGrowthDen);
    usize grown = currentCapacity;
    if (extra <= maxCount - currentCapacity)
    {
        grown = currentCapacity + extra;
    }
    else
    {
        grown = maxCount; // saturate rather than overflow
    }

    usize result = requested;
    if (grown > result)
    {
        result = grown;
    }
    if (result < minCapacity)
    {
        result = minCapacity;
    }
    if (result > maxCount)
    {
        result = maxCount;
    }
    // If even the ceiling cannot satisfy the request, signal overflow.
    if (result < requested)
    {
        return 0;
    }
    return result;
}

[[noreturn]] LUDUS_COLD void OnAllocationFailure() noexcept
{
    LUDUS_FATAL("Ludus::Array allocation failed (out of memory)");
}

[[noreturn]] LUDUS_COLD void OnCapacityOverflow() noexcept
{
    LUDUS_FATAL("Ludus::Array capacity overflow (requested size exceeds MaxSize)");
}
} // namespace ludus::foundation::core::detail
