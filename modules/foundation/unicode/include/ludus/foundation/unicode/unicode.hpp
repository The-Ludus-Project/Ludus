#pragma once

/// @file
/// @brief Host-owned lifetime of the pinned ICU4C Unicode services.
///
/// `Ludus::Unicode` builds only with `LUDUS_BUILD_UNICODE=ON` after an explicit
/// `scripts/icu-probe bootstrap`. ICU types, headers and ABI never appear here;
/// the facade exposes only the process lifecycle, the linguistic-data identity
/// and allocation diagnostics. See docs/modules/foundation/unicode.md and
/// docs/architecture/localization-l0.md ("ICU backend configuration").

#include <ludus/foundation/base/types.h>

#include <ludus/foundation/memory/allocation_domain.hpp>

#include <string_view>

namespace ludus::foundation::unicode
{
/// Outcome of the Unicode host lifecycle operations.
enum class UnicodeStatus : uint8
{
    Ok,                 ///< The operation completed.
    AlreadyInitialized, ///< InitializeUnicodeHost was called while the host was running; nothing changed.
    NotInitialized,     ///< The host is not running; nothing changed.
    DataMismatch,       ///< The runtime ICU, CLDR or Unicode version differs from the pinned build; ICU was cleaned up.
    BackendFailure,     ///< ICU rejected its memory hooks, embedded data or warm-up; ICU was cleaned up.
};

/// @brief Cumulative ICU allocation diagnostics since the host was last initialized.
///
/// Counts are measurements for attribution and cost reports, not a cap: ICU4C
/// 78.3 cannot recover from allocation failure, so the host never enforces a limit.
struct UnicodeAllocationStatistics final
{
    uint64 Allocations{}; ///< Successful allocation and reallocation requests ICU made.
    uint64 Frees{};       ///< Blocks ICU released.
    usize LiveBlocks{};   ///< Blocks currently owned by ICU, including its process caches.
    usize LiveBytes{};    ///< Bytes currently owned by ICU, excluding Ludus block headers.
    usize PeakBytes{};    ///< Highest LiveBytes since initialization.
};

/// @brief Starts ICU for this process: memory hooks, embedded data and singleton warm-up.
///
/// In order: installs ICU memory functions backed by `domain` before any other
/// ICU call, registers the immutable embedded data package (ICU never opens
/// files), warms the static locale cache, `UnifiedCache` and number-skeleton
/// tables, then compares the runtime ICU/CLDR/Unicode versions with the pinned
/// build. ICU4C 78.3 is unsafe under allocation failure (L0 findings), so from
/// the hook installation onward any allocation failure terminates through
/// FoundationBase's emergency path (`LUDUS_FATAL`) instead of reporting.
///
/// @param domain Backing storage for every ICU allocation. Borrowed; it must
///        outlive ShutdownUnicodeHost and be thread safe.
/// @return Ok; AlreadyInitialized; DataMismatch or BackendFailure after which
///         ICU has been cleaned up and initialization may be retried.
/// @pre Called on the host thread before any ICU user exists, never from a
///      catalog, editor project or live-reloaded module.
[[nodiscard]] UnicodeStatus InitializeUnicodeHost(const AllocationDomain& domain) noexcept;

/// @brief Releases every ICU allocation with `u_cleanup` at host teardown.
///
/// Also clears ICU's memory hooks and data registration; the host may be
/// initialized again afterwards.
/// @return Ok, or NotInitialized when the host is not running.
/// @pre Every ICU user (for example each Localization formatter context) is
///      destroyed. A live user terminates through `LUDUS_FATAL`, because
///      `u_cleanup` would invalidate its objects.
[[nodiscard]] UnicodeStatus ShutdownUnicodeHost() noexcept;

/// Reports whether InitializeUnicodeHost succeeded and ShutdownUnicodeHost has not run since.
[[nodiscard]] bool IsUnicodeHostInitialized() noexcept;

/// @brief Linguistic data identity reported by the running ICU.
///
/// Spelled like a cooked catalog's label, for example
/// `icu4c-78.3/cldr-48/unicode-17.0`. Static storage; empty when the host is
/// not running.
[[nodiscard]] std::string_view GetLinguisticDataLabel() noexcept;

/// @brief Snapshot of ICU allocation diagnostics; zeros before the first initialization.
///
/// Thread safe; counters are read independently, so a snapshot taken while
/// other threads format may be momentarily inconsistent between fields.
[[nodiscard]] UnicodeAllocationStatistics GetUnicodeAllocationStatistics() noexcept;
} // namespace ludus::foundation::unicode
