#pragma once
#include "internal/measurement.h"
#include "internal/raster.h"
#include <ludus/foundation/containers/static_array.hpp>
#include <ludus/graphics/rhi/native_interop.h>
namespace ludus::graphics::rhi::reference
{
inline bool NativeAvailable = true;
inline bool NativePending = false;
inline bool NativeLost = false;
inline RasterStatus NativePublishResult = RasterStatus::Ready;
inline foundation::usize NativePublications = 0;
inline foundation::usize NativeReleases = 0;
inline foundation::usize NativeRetirements = 0;
inline internal::StorageTable<bool> NativeReadReleased;
inline foundation::uint32 NativeObjects[4]{};
inline GpuTiming Timing;
inline foundation::uint32 TimingLossToken = 0;
inline RasterStatus TimingAdmission = RasterStatus::Ready;
inline bool AllocationAvailable = true;
inline foundation::uint64 AllocationBytes = 128;
inline bool ProfileAvailable = true;
inline bool HdrAvailable = true;
inline bool ComputeAvailable = true;
inline bool IsSpatialUpscaleAvailable = false;
inline foundation::usize SpatialUpscaleEncodes = 0;
inline foundation::StaticArray<bool, RASTER_FORMAT_COUNT> FormatUnavailable{};
inline bool FormatBlendUnavailable = false;
inline bool OverrideRasterLimits = false;
inline bool HasBindingLimitOverride = false;
inline BindingCapabilities BindingLimitOverride;
inline RasterCapabilities RasterLimitOverride;
inline foundation::usize Dispatches = 0;
inline RasterStatus ComputeResult = RasterStatus::Ready;
inline RasterStatus Next = RasterStatus::Ready;
inline RasterStatus Submission = RasterStatus::Ready;
inline foundation::uint32 LastRequest = 0;
inline internal::StorageArray<foundation::uint32> PendingRequests;
inline foundation::usize PendingCount = 0;
inline foundation::uint32 LossOnCompletion = 0;
inline foundation::uint64 Submitted = 0;
inline foundation::uint64 Completed = 0;
inline foundation::usize Creates[static_cast<foundation::usize>(internal::RasterKind::Count)]{};
inline foundation::usize Destroys[static_cast<foundation::usize>(internal::RasterKind::Count)]{};
inline foundation::usize Draws = 0;
inline foundation::usize Passes = 0;
inline foundation::usize Barriers = 0;
inline RasterStatus PassStart = RasterStatus::Ready;
inline RasterStatus PassPrepare = RasterStatus::Ready;
inline internal::RasterPacket Packet;
inline internal::ComputePacket DispatchPacket;
inline internal::RasterPassInfo Pass;
inline internal::StorageTable<internal::RasterPipelineInfo> Pipelines;
inline internal::StorageTable<foundation::Array<foundation::uint8>> BufferBytes;
inline internal::StorageTable<foundation::usize> BufferSizes;
inline internal::StorageTable<foundation::Array<foundation::uint8>> TextureBytes;
inline internal::StorageTable<TextureDescription> TextureDescriptions;
inline foundation::uint8 ReadbackBytes[internal::LIFETIME_TRANSFERS][internal::LIFETIME_TRANSFER_BYTES]{};
inline RasterStatus Transfers[2][internal::LIFETIME_TRANSFERS];
inline bool TransferOccupied[2][internal::LIFETIME_TRANSFERS]{};
inline bool RetainTransfersOnReset = false;
inline RasterStatus TransferStart = RasterStatus::Ready;
inline RasterStatus ReadbackCopy = RasterStatus::Ready;
inline foundation::usize Discards = 0;
inline foundation::usize Copies = 0;
void Reset() noexcept;
} // namespace ludus::graphics::rhi::reference
