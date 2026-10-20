#pragma once

// Native import of hosted surface images for HostedSurfaceConsumer. The
// generation and frame bookkeeping is portable (hosted_consumer.cpp); each
// backend provides these hooks: Metal wraps IOSurfaces received as Mach send
// rights, Vulkan imports opaque memory file descriptors. Builds without a hosted
// surface provide hooks that refuse every import.

#include <ludus/foundation/base/core.h>

#include <ludus/foundation/containers/static_array.hpp>
#include <ludus/graphics/rhi/hosted_surface.h>
#include <ludus/graphics/rhi/native_interop.h>
#include <ludus/platform/hosted_surface.h>

namespace ludus::graphics::rhi::internal
{
/// Native objects imported for one generation; only the consumer API's fields are set.
struct HostedImport final
{
    /// Metal: retained `id<MTLTexture>` objects (CFBridgingRetain), released by DestroyHostedImport().
    foundation::StaticArray<void*, platform::HOSTED_SURFACE_IMAGE_COUNT> MetalTextures{};
    /// Vulkan: `VkImage` bits.
    foundation::StaticArray<foundation::uint64, platform::HOSTED_SURFACE_IMAGE_COUNT> VulkanImages{};
    /// Vulkan: `VkDeviceMemory` bits that own the imported payloads.
    foundation::StaticArray<foundation::uint64, platform::HOSTED_SURFACE_IMAGE_COUNT> VulkanMemories{};
};

/// True when this build can import hosted images onto a device of `api`.
[[nodiscard]] bool IsHostedImportAvailable(Backend api) noexcept;

/// @brief Import one generation onto the consumer's device.
///
/// Takes the handles out of `inoutImages` whatever the outcome; a failed import
/// releases everything it created.
/// @return None on success, or why the images cannot be used.
[[nodiscard]] HostedConsumerRefusal ImportHostedGeneration(const NativeDeviceIdentity& device,
                                                           platform::HostedSurfaceImages& inoutImages,
                                                           HostedImport& outImport) noexcept;

/// Destroy the native objects of one generation; leaves `inoutImport` empty.
void DestroyHostedImport(const NativeDeviceIdentity& device, HostedImport& inoutImport) noexcept;

/// A completion marker committed on the consumer's command queue after work already submitted there.
struct HostedQueueMarker final
{
    /// Metal: retained `id<MTLCommandBuffer>` (CFBridgingRetain), released by DestroyHostedQueueMarker().
    void* MetalCommandBufferOrNull = nullptr;
    /// Vulkan: `VkFence` bits, submitted alone to the queue and destroyed by DestroyHostedQueueMarker().
    foundation::uint64 VulkanFence = 0;
};
/// @brief Commit a marker behind everything already submitted to `device.CommandQueue`.
///
/// Call on the thread that submits to that queue.
/// @return False when the marker could not be committed (the caller cannot release after the queue).
[[nodiscard]] bool EnqueueHostedQueueMarker(const NativeDeviceIdentity& device, HostedQueueMarker& outMarker) noexcept;
/// True once the marker, and with it all earlier work on the queue, has finished (or the device was lost).
[[nodiscard]] bool IsHostedQueueMarkerComplete(const NativeDeviceIdentity& device,
                                               const HostedQueueMarker& marker) noexcept;
/// Block until the marker completes; teardown only, never while presenting.
void WaitHostedQueueMarker(const NativeDeviceIdentity& device, const HostedQueueMarker& marker) noexcept;
/// Release the marker's native object; leaves `inoutMarker` empty.
void DestroyHostedQueueMarker(const NativeDeviceIdentity& device, HostedQueueMarker& inoutMarker) noexcept;

/// Direction of a hosted image's queue family ownership transfer on the consumer's queue.
enum class HostedOwnershipTransfer : foundation::uint8
{
    /// From the external queue family (the game) to the consumer's, before sampling.
    Acquire,
    /// From the consumer's queue family back to the external one, after sampling.
    Release,
};
/// @brief Record one ownership transfer of `image` into `commandBuffer`.
/// @return Ok; InvalidArgument for a null command buffer where one is needed; Unavailable when
/// the backend cannot record it. Metal has no transfer and returns Ok.
[[nodiscard]] HostedConsumerStatus RecordHostedOwnershipTransfer(const NativeDeviceIdentity& device,
                                                                 foundation::uint64 image,
                                                                 NativeVulkanCommandBuffer commandBuffer,
                                                                 HostedOwnershipTransfer transfer) noexcept;
} // namespace ludus::graphics::rhi::internal
