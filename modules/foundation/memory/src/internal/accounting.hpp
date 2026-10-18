#pragma once

// Checked per-domain accounting (memory profiling phase 1).
//
// Contracts: docs/architecture/memory-profiling.md, "Questions and metric
// contracts"; docs/architecture/memory-management.md §11.2-§11.3. Every update is
// a relaxed atomic read-modify-write on the domain's own counters: no global
// atomic, lock, table or per-pointer metadata is touched on allocation or free.
// Counters are therefore samples, not a transaction across fields or domains.
//
// Thanks to Ricky Lung, "Design and Implementation of an In-Game Memory
// Profiler", Game Programming Gems 8, section 4.6, pp. 402-408, for charging a
// free to the allocation's origin and keeping live totals separate from
// accumulated traffic; to Jason Gregory, "In-Game Memory Stats and Leak
// Detection", Game Engine Architecture, 3rd edition, section 10.9, pp. 615-618,
// for per-subsystem totals that state what lies outside allocator coverage; and
// to the Perfetto authors, "Native heap profiler"
// (https://perfetto.dev/docs/data-sources/native-heap-profiler), for reporting
// unreleased size/count beside total allocated size/count. These ideas reached
// this code through docs/architecture/memory-profiling-gems-review.md, which
// records the page ranges and departures. The implementation is Ludus-owned:
// sticky invalidity replaces plausible wrapped totals, peaks stay per domain,
// and there is no allocation prefix or origin tree.

#include <ludus/foundation/base/core.h>

#include <ludus/foundation/memory/allocation_domain.hpp>
#include <ludus/foundation/memory/allocation_statistics.hpp>

#include <atomic>

namespace ludus::foundation
{
/// Grants Memory's implementation access to a domain's private counters.
struct AllocationAccounting final
{
    using Counters = AllocationDomain::Counters;

    [[nodiscard]] static LUDUS_INLINE Counters& GetCounters(const AllocationDomain& domain) noexcept
    {
        return domain.mCounters;
    }
};

namespace memory_internal
{
using AllocationCounters = AllocationAccounting::Counters;

inline constexpr uint32 SATURATED_FLAG = 1U << 0U;
inline constexpr uint32 ACCOUNTING_ERROR_FLAG = 1U << 1U;
inline constexpr usize MAX_USIZE = ~usize{0};
inline constexpr uint64 MAX_UINT64 = ~uint64{0};

static_assert(std::atomic_ref<usize>::is_always_lock_free, "Domain counters must not need a lock");
static_assert(std::atomic_ref<uint64>::is_always_lock_free, "Domain counters must not need a lock");
static_assert(std::atomic_ref<uint32>::is_always_lock_free, "Domain counters must not need a lock");
static_assert(std::atomic_ref<usize>::required_alignment <= alignof(usize), "Counters need no extra alignment");
static_assert(std::atomic_ref<uint64>::required_alignment <= alignof(uint64), "Counters need no extra alignment");
static_assert(std::atomic_ref<uint32>::required_alignment <= alignof(uint32), "Counters need no extra alignment");

template <typename ValueType>
[[nodiscard]] LUDUS_INLINE std::atomic_ref<ValueType> AsAtomic(ValueType& inoutValue) noexcept
{
    return std::atomic_ref<ValueType>(inoutValue);
}

// Adds and reports whether the addition wrapped. The stored value then no
// longer means anything; the caller makes that visible through a sticky flag.
template <typename ValueType>
[[nodiscard]] LUDUS_INLINE bool AddWrapped(ValueType& inoutCounter, ValueType amount) noexcept
{
    const ValueType previous = AsAtomic(inoutCounter).fetch_add(amount, std::memory_order_relaxed);
    return previous > static_cast<ValueType>(~ValueType{0} - amount);
}

// Subtracts and reports whether the subtraction went below zero.
template <typename ValueType>
[[nodiscard]] LUDUS_INLINE bool SubtractWrapped(ValueType& inoutCounter, ValueType amount) noexcept
{
    const ValueType previous = AsAtomic(inoutCounter).fetch_sub(amount, std::memory_order_relaxed);
    return previous < amount;
}

LUDUS_COLD inline void MarkSaturated(AllocationCounters& inoutCounters) noexcept
{
    (void)AsAtomic(inoutCounters.ValidityFlags).fetch_or(SATURATED_FLAG, std::memory_order_relaxed);
}

// An underflow after a saturation is a consequence of the wrapped value, not
// evidence of a bad free, so it does not escalate the reported state.
LUDUS_COLD inline void MarkUnderflow(AllocationCounters& inoutCounters) noexcept
{
    const uint32 flags = AsAtomic(inoutCounters.ValidityFlags).load(std::memory_order_relaxed);
    if ((flags & SATURATED_FLAG) == 0U)
    {
        (void)AsAtomic(inoutCounters.ValidityFlags).fetch_or(ACCOUNTING_ERROR_FLAG, std::memory_order_relaxed);
    }
}

LUDUS_COLD inline void MarkAccountingError(AllocationCounters& inoutCounters) noexcept
{
    (void)AsAtomic(inoutCounters.ValidityFlags).fetch_or(ACCOUNTING_ERROR_FLAG, std::memory_order_relaxed);
}

// Raises the high-water mark to `liveBytes`, the exact counter value this
// allocation produced. Only a new maximum pays a compare-exchange.
LUDUS_INLINE void RaisePeak(AllocationCounters& inoutCounters, usize liveBytes) noexcept
{
    std::atomic_ref<usize> peak = AsAtomic(inoutCounters.PeakRequestedBytes);
    usize observed = peak.load(std::memory_order_relaxed);
    while (liveBytes > observed &&
           peak.compare_exchange_weak(observed, liveBytes, std::memory_order_relaxed, std::memory_order_relaxed) ==
               false)
    {
    }
}

/// Records one successful nonzero allocation of `bytes` requested bytes.
LUDUS_INLINE void RecordAllocation(AllocationCounters& inoutCounters, usize bytes) noexcept
{
    const usize previousLive = AsAtomic(inoutCounters.LiveRequestedBytes).fetch_add(bytes, std::memory_order_relaxed);
    bool hasWrapped = previousLive > MAX_USIZE - bytes;
    hasWrapped = AddWrapped(inoutCounters.LiveBlocks, usize{1}) || hasWrapped;
    hasWrapped = AddWrapped(inoutCounters.AllocatedBytes, static_cast<uint64>(bytes)) || hasWrapped;
    hasWrapped = AddWrapped(inoutCounters.AllocationCount, uint64{1}) || hasWrapped;
    if (LUDUS_UNLIKELY(hasWrapped == true))
    {
        MarkSaturated(inoutCounters);
        return;
    }
    RaisePeak(inoutCounters, previousLive + bytes);
}

/// Records one non-null free of `bytes` requested bytes, before the backend releases it.
LUDUS_INLINE void RecordFree(AllocationCounters& inoutCounters, usize bytes) noexcept
{
    bool hasUnderflowed = SubtractWrapped(inoutCounters.LiveRequestedBytes, bytes);
    hasUnderflowed = SubtractWrapped(inoutCounters.LiveBlocks, usize{1}) || hasUnderflowed;
    bool hasWrapped = AddWrapped(inoutCounters.FreedBytes, static_cast<uint64>(bytes));
    hasWrapped = AddWrapped(inoutCounters.FreeCount, uint64{1}) || hasWrapped;
    if (LUDUS_UNLIKELY(hasWrapped == true))
    {
        MarkSaturated(inoutCounters);
    }
    if (LUDUS_UNLIKELY(hasUnderflowed == true))
    {
        MarkUnderflow(inoutCounters);
    }
}

/// Records a valid request that the backend could not satisfy.
LUDUS_COLD inline void RecordFailure(AllocationCounters& inoutCounters) noexcept
{
    if (AddWrapped(inoutCounters.FailureCount, uint64{1}) == true)
    {
        MarkSaturated(inoutCounters);
    }
}

/// Records a request rejected before reaching the backend.
LUDUS_COLD inline void RecordRejection(AllocationCounters& inoutCounters) noexcept
{
    if (AddWrapped(inoutCounters.RejectedCount, uint64{1}) == true)
    {
        MarkSaturated(inoutCounters);
    }
}

/// Loads every field with relaxed ordering; validity is read last. Nothing is
/// modified; the reference is non-const only because C++23 atomic_ref needs it.
[[nodiscard]] inline AllocationStatistics ReadStatistics(AllocationCounters& inoutCounters) noexcept
{
    AllocationStatistics statistics;
    statistics.LiveRequestedBytes = AsAtomic(inoutCounters.LiveRequestedBytes).load(std::memory_order_relaxed);
    statistics.LiveBlocks = AsAtomic(inoutCounters.LiveBlocks).load(std::memory_order_relaxed);
    statistics.PeakRequestedBytes = AsAtomic(inoutCounters.PeakRequestedBytes).load(std::memory_order_relaxed);
    statistics.AllocatedBytes = AsAtomic(inoutCounters.AllocatedBytes).load(std::memory_order_relaxed);
    statistics.FreedBytes = AsAtomic(inoutCounters.FreedBytes).load(std::memory_order_relaxed);
    statistics.AllocationCount = AsAtomic(inoutCounters.AllocationCount).load(std::memory_order_relaxed);
    statistics.FreeCount = AsAtomic(inoutCounters.FreeCount).load(std::memory_order_relaxed);
    statistics.FailureCount = AsAtomic(inoutCounters.FailureCount).load(std::memory_order_relaxed);
    statistics.RejectedCount = AsAtomic(inoutCounters.RejectedCount).load(std::memory_order_relaxed);
    const uint32 flags = AsAtomic(inoutCounters.ValidityFlags).load(std::memory_order_relaxed);
    if ((flags & ACCOUNTING_ERROR_FLAG) != 0U)
    {
        statistics.Validity = AccountingValidity::AccountingError;
    }
    else if ((flags & SATURATED_FLAG) != 0U)
    {
        statistics.Validity = AccountingValidity::Saturated;
    }
    return statistics;
}
} // namespace memory_internal
} // namespace ludus::foundation
