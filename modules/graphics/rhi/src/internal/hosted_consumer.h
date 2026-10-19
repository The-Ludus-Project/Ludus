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
} // namespace ludus::graphics::rhi::internal
