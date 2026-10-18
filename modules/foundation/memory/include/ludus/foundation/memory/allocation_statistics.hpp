#pragma once

/// @file
/// @brief Opt-in queries for allocation-domain counters, the domain registry and coverage.
///
/// Counters are relaxed atomic samples while allocations continue: a query is
/// not a transaction across fields or domains. Exact reconciliation needs a
/// caller-established quiescent boundary on every domain read. Per-domain peaks
/// are histories of one domain's requested bytes; never sum them into a process
/// peak. Contracts: docs/architecture/memory-profiling.md ("Questions and metric
/// contracts") and docs/development/memory-phase-1.md.

#include <ludus/foundation/base/types.h>

#include <ludus/foundation/memory/allocation_domain.hpp>

namespace ludus::foundation
{
/// @brief Whether a domain's counters still describe its requested-byte history exactly.
///
/// Both invalid states are sticky: later arithmetic cannot restore accuracy.
enum class AccountingValidity : uint8
{
    Valid,           ///< Every update stayed in range.
    Saturated,       ///< A counter overflowed its width; values are no longer exact.
    AccountingError, ///< A free exceeded live bytes/blocks or had an invalid layout.
};

/// @brief One relaxed sample of an allocation domain's counters.
///
/// Requested bytes are the sizes passed to `TryAllocate`, not backend usable,
/// committed or resident bytes. Zero-byte requests create no block.
struct AllocationStatistics final
{
    usize LiveRequestedBytes = 0; ///< Requested bytes allocated and not yet freed.
    usize LiveBlocks = 0;         ///< Nonzero blocks allocated and not yet freed.
    usize PeakRequestedBytes = 0; ///< High-water of LiveRequestedBytes for this domain only.
    uint64 AllocatedBytes = 0;    ///< Cumulative requested bytes successfully allocated.
    uint64 FreedBytes = 0;        ///< Cumulative requested bytes freed.
    uint64 AllocationCount = 0;   ///< Cumulative successful nonzero allocations.
    uint64 FreeCount = 0;         ///< Cumulative non-null frees.
    uint64 FailureCount = 0;      ///< Valid requests that the backend could not satisfy.
    uint64 RejectedCount = 0;     ///< Requests rejected before the backend (invalid alignment or missing callback).
    AccountingValidity Validity = AccountingValidity::Valid; ///< Sticky validity of the values above.
};

/// @brief Samples a domain's counters.
///
/// @param domain Any domain, registered or not.
/// @return Relaxed per-field loads. While other threads allocate or free in
/// `domain`, fields may come from different moments; at a quiescent boundary
/// `LiveRequestedBytes == AllocatedBytes - FreedBytes` and
/// `LiveBlocks == AllocationCount - FreeCount` while Validity is Valid.
/// @note Thread safe and allocation free.
[[nodiscard]] AllocationStatistics GetAllocationStatistics(const AllocationDomain& domain) noexcept;

/// Numeric identity of a registered domain, stable for the process lifetime. Zero is never assigned.
using AllocationDomainId = uint32;

/// Identity of the built-in `System` domain returned by `GetSystemAllocationDomain`.
inline constexpr AllocationDomainId SYSTEM_ALLOCATION_DOMAIN_ID = 1;
/// Identity of the built-in `Containers` domain returned by `GetContainerAllocationDomain`.
inline constexpr AllocationDomainId CONTAINERS_ALLOCATION_DOMAIN_ID = 2;
/// Maximum registered domains, built-in domains included.
inline constexpr usize MAX_ALLOCATION_DOMAINS = 32;
/// Maximum bytes of a registered domain name, excluding its terminator.
inline constexpr usize MAX_ALLOCATION_DOMAIN_NAME_BYTES = 47;

/// @brief Outcome of `TryRegisterAllocationDomain`.
enum class AllocationDomainRegistrationStatus : uint8
{
    Ok,                      ///< Registered, or the identical definition was already registered.
    InvalidName,             ///< Empty, too long, or not made of ASCII letters, digits, '.', '_' and '-'.
    NameCollision,           ///< Another domain already uses the name.
    DomainAlreadyRegistered, ///< The domain is already registered under another name.
    RegistryFull,            ///< MAX_ALLOCATION_DOMAINS definitions already exist.
};

/// @brief Registers a process-lifetime domain under a stable name for enumeration and export.
///
/// Registration is a cold, serialized operation; do not call it from an
/// allocation callback. The name is copied into bounded registry storage.
/// Registering the identical domain and name again returns the existing ID.
/// Registration never affects allocation: unregistered domains still count,
/// and a failed registration only reduces attribution.
/// @param domain Domain with static storage duration. Registered domains are
/// never removed, so a shorter-lived domain must not be registered.
/// @param name Null-terminated name of 1 to MAX_ALLOCATION_DOMAIN_NAME_BYTES
/// ASCII letters, digits, '.', '_' or '-'.
/// @param outId Receives the domain's ID on `Ok`; unchanged otherwise.
/// @return Registration outcome.
/// @note Thread safe. Allocation free.
[[nodiscard]] AllocationDomainRegistrationStatus
TryRegisterAllocationDomain(const AllocationDomain& domain, const char* name, AllocationDomainId& outId) noexcept;

/// @brief One published registry entry.
struct RegisteredAllocationDomain final
{
    AllocationDomainId Id = 0;                      ///< Stable identity; also index + 1.
    const AllocationDomain* DomainOrNull = nullptr; ///< Registered domain; null only in a default value.
    const char* NameOrNull = nullptr;               ///< Null-terminated name in process-lifetime registry storage.
};

/// @brief Returns how many domains are registered, built-in domains included.
/// @return Published entry count; entries below it never change.
/// @note Thread safe; a concurrent registration may publish more entries afterwards.
[[nodiscard]] usize GetRegisteredAllocationDomainCount() noexcept;

/// @brief Reads one published registry entry.
/// @param index Entry index, below `GetRegisteredAllocationDomainCount()`.
/// @param outEntry Receives the entry on success; unchanged otherwise.
/// @return False when `index` is not published.
/// @note Thread safe and allocation free.
[[nodiscard]] bool TryGetRegisteredAllocationDomain(usize index, RegisteredAllocationDomain& outEntry) noexcept;

/// @brief One allocation path that domain accounting does not cover.
struct AllocationCoverageExclusion final
{
    const char* Name;   ///< Stable kebab-case identifier.
    const char* Reason; ///< Short explanation of what bypasses domain accounting.
};

/// @brief Returns how many coverage exclusions are listed.
/// @return Number of entries in the static exclusion manifest.
[[nodiscard]] usize GetAllocationCoverageExclusionCount() noexcept;

/// @brief Reads one entry of the static coverage exclusion manifest.
///
/// Domain totals cover only allocations made through registered domains. This
/// manifest names the known paths outside them, so a report never presents
/// covered totals as whole-process memory. It is not a measured denominator.
/// @param index Entry index, below `GetAllocationCoverageExclusionCount()`.
/// @return Process-lifetime entry.
/// @pre `index < GetAllocationCoverageExclusionCount()`; violations terminate
/// through `LUDUS_REQUIRE`.
[[nodiscard]] const AllocationCoverageExclusion& GetAllocationCoverageExclusion(usize index) noexcept;
} // namespace ludus::foundation
