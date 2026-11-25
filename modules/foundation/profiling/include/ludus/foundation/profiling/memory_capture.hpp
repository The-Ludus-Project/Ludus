#pragma once

#include <ludus/foundation/base/types.h>

#include <ludus/foundation/containers/borrowed_view.hpp>
#include <ludus/foundation/memory/allocation_statistics.hpp>

namespace ludus::foundation::profiling
{
/// Outcome of a cold Activity capture control or site registration.
enum class MemoryCaptureStatus : uint8
{
    Ok,               ///< The requested operation succeeded.
    InvalidInput,     ///< Repair the definition, limits, domain selection, or sink.
    Busy,             ///< End the active capture or release the retained capture before retrying.
    UnknownCapture,   ///< The ID does not name the retained capture.
    WrongState,       ///< The operation requires an active or sealed capture as documented.
    StorageExhausted, ///< Recorder allocation, site capacity, or capture ID space was exhausted.
    Incomplete,       ///< End succeeded and sealed an artifact with reduced history or invalid counters.
    WriteFailed,      ///< The sink rejected bytes. The sealed capture can be exported again.
};

/// Opaque process-local capture identity. Zero is never issued.
using MemoryCaptureId = uint64;

/// Maximum distinct process-lifetime site definitions.
inline constexpr usize MAX_MEMORY_ACTIVITY_SITES = 256;
/// Maximum UTF-8 bytes in each site text field.
inline constexpr usize MAX_MEMORY_ACTIVITY_SITE_TEXT_BYTES = 191;

/// Borrowed site definition copied at registration. All text is counted UTF-8 with no NUL bytes.
struct MemorySiteDefinition final
{
    BorrowedView<const char> SourcePath; ///< Build-relative source path, using '/' separators.
    BorrowedView<const char> Function;   ///< Source function name.
    BorrowedView<const char> Module;     ///< Build/module identity supplied by the consumer.
    uint32 Line = 0;                     ///< One-based source line.
};

/// @brief Registers a site outside allocation hooks.
/// @param definition Text remains alive until this call returns. Equal definitions reuse an ID.
/// @param outSite Receives a process-local ID on success; unchanged otherwise.
/// @return Ok, InvalidInput, Busy while a capture is active, or StorageExhausted.
/// @pre Serialize all site registrations and capture controls in the owning coordinator.
[[nodiscard]] MemoryCaptureStatus RegisterMemorySite(const MemorySiteDefinition& definition,
                                                     AllocationSiteId& outSite) noexcept;

/// Fixed coverage and logical recorder limits selected before producers are resumed.
struct MemoryActivityConfig final
{
    BorrowedView<const AllocationDomainId> Domains; ///< Nonempty set of distinct registered domain IDs.
    usize LiveSlots = 0; ///< Table slots; zero selects the full SDK capacity. Otherwise at least two.
    usize Events = 0;    ///< Journal records; zero selects the full SDK capacity.
};

/// @brief Appends bytes from a sealed artifact to a consumer-owned sink.
/// Bytes are valid only during the call. Return false to stop the export. Do not reenter capture controls.
using MemoryArtifactWriter = bool (*)(void* contextOrNull, BorrowedView<const char> bytes) noexcept;

/// @brief Starts one Activity capture and samples its starting counters.
/// @param config Coverage and limits; the domain view is copied during the call.
/// @param outCapture Receives a fresh ID on success; unchanged on failure.
/// @return Ok, InvalidInput, Busy, or StorageExhausted.
/// @pre The owner has stopped admission and joined every whole operation on the selected domains,
/// including backend calls. Keep them quiescent during the call, then resume via synchronization.
/// All capture/site controls are serialized by the owner. Activity begins with an unproven ownership baseline.
[[nodiscard]] MemoryCaptureStatus BeginMemoryCapture(const MemoryActivityConfig& config,
                                                     MemoryCaptureId& outCapture) noexcept;

/// @brief Detaches the active recorder, samples final counters and seals the capture.
/// @param capture The active ID returned by BeginMemoryCapture.
/// @return Ok or Incomplete on a successful seal, or UnknownCapture or WrongState.
/// @pre Reestablish the same whole-operation safe point as for BeginMemoryCapture.
/// Serialize with other controls. Resume producers via synchronization after returning.
[[nodiscard]] MemoryCaptureStatus EndMemoryCapture(MemoryCaptureId capture) noexcept;

/// @brief Streams the versioned JSON artifact from sealed storage, without reattaching the recorder.
/// @param capture Sealed ID.
/// @param writer Non-null sink; called with bounded chunks. The caller handles open, close and commit errors.
/// @param contextOrNull Borrowed sink state, passed through unchanged.
/// @return Ok, UnknownCapture, WrongState, InvalidInput, or WriteFailed.
/// On failure the sink can contain a partial artifact and the sealed session is retained for retry.
/// @pre Serialize with other capture/site controls.
[[nodiscard]] MemoryCaptureStatus
ExportMemoryCapture(MemoryCaptureId capture, MemoryArtifactWriter writer, void* contextOrNull) noexcept;

/// @brief Discards a sealed capture and frees its storage.
/// @param capture Sealed ID. ID values are never reused within the process.
/// @return Ok, UnknownCapture, or WrongState when still active.
/// @pre Serialize with other capture/site controls. End and release before module or process teardown.
[[nodiscard]] MemoryCaptureStatus ReleaseMemoryCapture(MemoryCaptureId capture) noexcept;
} // namespace ludus::foundation::profiling
