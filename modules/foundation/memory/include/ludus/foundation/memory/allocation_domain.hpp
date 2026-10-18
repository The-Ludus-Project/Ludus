#pragma once

/// @file
/// @brief Fallible, accounted byte allocation through a stable allocation domain.
///
/// Every domain carries checked requested-byte counters (memory profiling phase
/// 1). Read them through the opt-in `allocation_statistics.hpp`; the metric
/// contracts are in docs/architecture/memory-profiling.md ("Questions and
/// metric contracts") and docs/development/memory-phase-1.md.

#include <ludus/foundation/base/types.h>

namespace ludus::foundation
{
/// @brief Stable, immutable identity for an allocation backend and its context.
///
/// A domain binds the backend that must release a block to the allocation
/// origin that its accounting charges. Routing is immutable; only its counters
/// change. Every successful nonzero allocation and every non-null free updates
/// the domain's checked counters with relaxed atomic operations, so a block
/// freed on another thread, or after its allocating thread exited, still
/// charges the domain that allocated it. Read them through
/// `GetAllocationStatistics` in `allocation_statistics.hpp`.
///
/// @note The domain and context must outlive all allocations made through them.
/// Callbacks must be thread safe and non-reentrant into their caller. No global
/// allocation replacement is implied, and allocations that bypass a domain are
/// not counted.
class AllocationDomain final
{
public:
    /// Allocates bytes with the requested power-of-two alignment; returns nullptr on failure.
    using AllocateFunction = void* (*)(void*, usize, usize) noexcept;
    /// Frees storage using its original context, byte count and alignment.
    using FreeFunction = void (*)(void*, void*, usize, usize) noexcept;

    /// @brief Binds an immutable domain to caller-owned callback state.
    ///
    /// The domain starts with zero counters and valid accounting. Declare a
    /// process-lifetime domain `constinit` so it is usable before dynamic
    /// initialization.
    /// @param context Borrowed backend state; callbacks decide whether nullptr is valid.
    /// @param allocate Thread-safe callback that reports allocation failure with nullptr.
    /// @param free Thread-safe callback that releases allocations from this backend.
    /// @note Both callbacks are required for TryAllocate to accept a request.
    constexpr AllocationDomain(void* context, AllocateFunction allocate, FreeFunction free) noexcept
        : mContext(context), mAllocate(allocate), mFree(free)
    {
    }
    /// Domains cannot be copied; their identity must remain stable.
    AllocationDomain(const AllocationDomain&) = delete;
    /// Domains cannot be assigned; allocations retain the original identity.
    AllocationDomain& operator=(const AllocationDomain&) = delete;
    /// Domains cannot be moved while allocations may refer to their identity.
    AllocationDomain(AllocationDomain&&) = delete;
    /// Domains cannot be move-assigned.
    AllocationDomain& operator=(AllocationDomain&&) = delete;

    /// @brief Attempts an allocation through the bound backend.
    ///
    /// A zero-byte request with a valid alignment is an empty success: it
    /// returns nullptr, calls no backend and changes no counter. An invalid
    /// alignment or missing callback is a rejected request, counted separately
    /// from a backend failure. Successful accounting happens after the backend
    /// returns and before the pointer is returned.
    /// @param bytes Requested storage size in bytes.
    /// @param alignment Nonzero power-of-two storage alignment.
    /// @return Allocated storage, or nullptr for a zero-byte request, invalid
    /// input, missing callbacks, or backend allocation failure.
    /// @note The caller frees successful allocations through this same domain
    /// with the same size and alignment.
    [[nodiscard]] void* TryAllocate(usize bytes, usize alignment) const noexcept;
    /// @brief Releases an allocation through its original backend; nullptr is a no-op.
    ///
    /// The domain subtracts the requested bytes before calling the backend, so
    /// a reused address cannot be charged against an older lifetime. A null
    /// pointer changes no counter.
    /// @param pointer Storage previously allocated through this domain, or nullptr.
    /// @param bytes Original requested size in bytes.
    /// @param alignment Original allocation alignment.
    /// @pre A non-null pointer requires the original live context and free
    /// callback and its original nonzero size and alignment. A violation that
    /// the counters can observe (a zero size, an invalid alignment, or more
    /// bytes or blocks freed than are live) marks the domain's accounting
    /// invalid; the storage is still passed to the backend.
    void Free(void* pointer, usize bytes, usize alignment) const noexcept;

private:
    /// FoundationMemory's private accounting implementation updates and reads the counters.
    friend struct AllocationAccounting;

    /// @brief Per-domain accounting storage; read it through `GetAllocationStatistics`.
    ///
    /// Plain integers keep a domain constant-initializable without a public
    /// `<atomic>` include. FoundationMemory updates and reads every field only
    /// through relaxed atomic operations; no other code may access them.
    struct Counters final
    {
        usize LiveRequestedBytes = 0; ///< Requested bytes allocated and not yet freed.
        usize LiveBlocks = 0;         ///< Nonzero blocks allocated and not yet freed.
        usize PeakRequestedBytes = 0; ///< High-water of LiveRequestedBytes.
        uint64 AllocatedBytes = 0;    ///< Cumulative requested bytes allocated.
        uint64 FreedBytes = 0;        ///< Cumulative requested bytes freed.
        uint64 AllocationCount = 0;   ///< Cumulative successful nonzero allocations.
        uint64 FreeCount = 0;         ///< Cumulative non-null frees.
        uint64 FailureCount = 0;      ///< Valid requests the backend could not satisfy.
        uint64 RejectedCount = 0;     ///< Requests rejected before the backend.
        uint32 ValidityFlags = 0;     ///< Sticky saturation and accounting-error bits.
    };

    void* mContext;
    AllocateFunction mAllocate;
    FreeFunction mFree;
    mutable Counters mCounters{};
};

/// @brief Returns the process-lifetime system allocation domain.
///
/// Registered as `System` (`SYSTEM_ALLOCATION_DOMAIN_ID`); the general domain
/// for engine-owned storage without a more specific domain.
/// @return Stable system backend, available before dynamic initialization and
/// through static and thread-local teardown.
[[nodiscard]] const AllocationDomain& GetSystemAllocationDomain() noexcept;

/// @brief Returns the process-lifetime domain that backs `Array` storage.
///
/// Registered as `Containers` (`CONTAINERS_ALLOCATION_DOMAIN_ID`). It uses the
/// same system backend as `GetSystemAllocationDomain`, so its blocks are
/// interchangeable at the backend level, but it is a separate accounting
/// origin: free its blocks only through this domain.
/// @return Stable domain, available before dynamic initialization and through
/// static and thread-local teardown.
[[nodiscard]] const AllocationDomain& GetContainerAllocationDomain() noexcept;
} // namespace ludus::foundation
