#pragma once

// Vulkan producer of the hosted surface (Linux). Each generation is three RGBA8
// images in dedicated memory exported as opaque file descriptors
// (VK_KHR_external_memory_fd); the descriptors go to the editor with SCM_RIGHTS
// through the hosted window's channel. After rendering, a frame's image is
// released to VK_QUEUE_FAMILY_EXTERNAL in SHADER_READ_ONLY_OPTIMAL, and it is
// announced only after the frame's fence has signalled (CPU fence). See
// docs/architecture/editor-gui-systems.md, "The hosted surface and frame ownership".
//
// Thanks to the Khronos Vulkan Working Group for the VK_KHR_external_memory_fd
// and VkExternalMemoryHandleTypeFlagBits reference pages
// (https://docs.vulkan.org/refpages/latest/refpages/source/VK_KHR_external_memory_fd.html,
// https://docs.vulkan.org/refpages/latest/refpages/source/VkExternalMemoryHandleTypeFlagBits.html),
// which define the export/import calls and that opaque handles require matching
// deviceUUID and driverUUID, and for the queue family ownership transfer rules in
// the specification's "Queue Family Ownership Transfer" section.

#include <ludus/foundation/base/core.h>

#include <ludus/foundation/containers/static_array.hpp>
#include <ludus/platform/hosted_surface.h>

#include "hosted_ring.h"

#include <cstring>

#include <volk.h>

namespace ludus::graphics::rhi::backend
{
/// The device extension hosted surfaces need on Linux, on producer and consumer devices.
inline constexpr const char* HOSTED_MEMORY_EXTENSION_NAME = VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME;
/// Pixel format of hosted images.
inline constexpr VkFormat HOSTED_IMAGE_FORMAT = VK_FORMAT_R8G8B8A8_UNORM;
/// Layout of a hosted image after the game rendered it: its resting layout and the old
/// layout of both halves of its queue family transfer to the editor.
inline constexpr VkImageLayout HOSTED_RENDERED_LAYOUT = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
/// Layout of a hosted image while the editor holds it: the new layout of that transfer.
inline constexpr VkImageLayout HOSTED_PRESENTED_LAYOUT = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
/// Usage of hosted images. Producer and consumer create them identically, as opaque
/// memory import requires.
inline constexpr VkImageUsageFlags HOSTED_IMAGE_USAGE =
    VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;

/// @brief Fill the create info of a hosted image; `outExternal` must outlive the call that uses `outImage`.
inline void FillHostedImageInfo(foundation::uint32 width,
                                foundation::uint32 height,
                                VkExternalMemoryImageCreateInfo& outExternal,
                                VkImageCreateInfo& outImage) noexcept
{
    outExternal = {};
    outExternal.sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO;
    outExternal.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT;
    outImage =
    {
        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .pNext = &outExternal,
        .flags = 0,
        .imageType = VK_IMAGE_TYPE_2D,
        .format = HOSTED_IMAGE_FORMAT,
        .extent = { .width = width, .height = height, .depth = 1 },
        .mipLevels = 1,
        .arrayLayers = 1,
        .samples = VK_SAMPLE_COUNT_1_BIT,
        .tiling = VK_IMAGE_TILING_OPTIMAL,
        .usage = HOSTED_IMAGE_USAGE,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .queueFamilyIndexCount = 0,
        .pQueueFamilyIndices = nullptr,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
    };
}

/// @brief Read an adapter's `deviceUUID` and `driverUUID` through the session's instance dispatch.
/// @pre The instance uses Vulkan 1.1 or later and so does `physicalDevice`.
inline void ReadHostedAdapterIdentity(VkPhysicalDevice physicalDevice,
                                      platform::HostedAdapterIdentity& outAdapter) noexcept
{
    VkPhysicalDeviceIDProperties identity{};
    identity.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES;
    VkPhysicalDeviceProperties2 properties{};
    properties.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
    properties.pNext = &identity;
    vkGetPhysicalDeviceProperties2(physicalDevice, &properties);
    static_assert(VK_UUID_SIZE == platform::HOSTED_ADAPTER_UUID_BYTES);
    std::memcpy(outAdapter.DeviceUuid.GetData(), identity.deviceUUID, VK_UUID_SIZE);
    std::memcpy(outAdapter.DriverUuid.GetData(), identity.driverUUID, VK_UUID_SIZE);
}

/// Outcome of HostedProducerVulkan::Acquire().
enum class HostedVulkanAcquireStatus : foundation::uint8
{
    /// An image is acquired; its framebuffer is ready for the frame's render pass.
    Ready,
    /// No image is free, the new generation is not yet published, or the channel is gone.
    Skipped,
    /// The images could not be created or exported.
    Failed,
};

/// Native objects the producer renders with, borrowed from the Vulkan backend.
struct HostedVulkanDevice final
{
    VkPhysicalDevice PhysicalDevice = VK_NULL_HANDLE;
    VkDevice Device = VK_NULL_HANDLE;
    foundation::uint32 QueueFamily = 0;
};

class HostedProducerVulkan final
{
public:
    HostedProducerVulkan() noexcept = default;
    ~HostedProducerVulkan() noexcept = default;
    HostedProducerVulkan(const HostedProducerVulkan&) = delete;
    HostedProducerVulkan& operator=(const HostedProducerVulkan&) = delete;
    HostedProducerVulkan(HostedProducerVulkan&&) = delete;
    HostedProducerVulkan& operator=(HostedProducerVulkan&&) = delete;

    /// Borrow the device and the hosted window's channel; both outlive Disconnect().
    void Connect(const HostedVulkanDevice& device, platform::HostedSurfaceChannel& inoutChannel) noexcept;
    /// Destroy every image. The caller has waited for the device's submissions.
    void Disconnect() noexcept;
    [[nodiscard]] LUDUS_INLINE bool IsConnected() const noexcept
    {
        return mChannelOrNull != nullptr;
    }

    /// @brief Announce completed frames, apply releases, retire idle generations,
    /// publish a generation for the extent when it changed, then acquire an image.
    /// @param renderPass The surface render pass the framebuffers must be compatible with.
    /// @param depthView The current depth attachment, of the same extent.
    /// @param depthEpoch Changes whenever the backend recreates the depth attachment;
    /// a generation built on another epoch is replaced, so no framebuffer outlives its view.
    /// @param outFramebuffer Framebuffer of the acquired image; set only on Ready.
    [[nodiscard]] HostedVulkanAcquireStatus Acquire(foundation::uint32 width,
                                                    foundation::uint32 height,
                                                    VkRenderPass renderPass,
                                                    VkImageView depthView,
                                                    foundation::uint64 depthEpoch,
                                                    VkFramebuffer& outFramebuffer) noexcept;
    /// @brief Record the release of the acquired image to VK_QUEUE_FAMILY_EXTERNAL.
    ///
    /// Call after the frame's last render pass, before the command buffer ends. The
    /// image leaves the surface's resting layout (TRANSFER_SRC_OPTIMAL) for
    /// SHADER_READ_ONLY_OPTIMAL.
    void RecordRelease(VkCommandBuffer command) const noexcept;
    /// The acquired frame was submitted; `fence` signals when it completes.
    void Submit(VkFence fence) noexcept;
    /// The acquired frame was abandoned before its submission.
    void Cancel() noexcept;
    /// The backend waited for `fence`; submissions it guarded have completed.
    void MarkSignaled(VkFence fence) noexcept;
    /// @brief Announce every completed frame, in submission order, without waiting.
    /// @return false when a fence query reported device loss.
    [[nodiscard]] bool Poll() noexcept;

    [[nodiscard]] LUDUS_INLINE const internal::HostedRing& GetRing() const noexcept
    {
        return mRing;
    }

private:
    [[nodiscard]] bool
    createGeneration(foundation::usize slot, VkRenderPass renderPass, VkImageView depthView) noexcept;
    void destroyGeneration(foundation::usize slot) noexcept;
    [[nodiscard]] bool publishPending() noexcept;
    [[nodiscard]] bool tryFindMemoryType(foundation::uint32 typeBits, foundation::uint32& outIndex) const noexcept;

private:
    struct Image final
    {
        VkImage Object = VK_NULL_HANDLE;
        VkDeviceMemory Memory = VK_NULL_HANDLE;
        VkImageView View = VK_NULL_HANDLE;
        VkFramebuffer Framebuffer = VK_NULL_HANDLE;
        /// Fence of the frame that last wrote it, while unannounced.
        VkFence Fence = VK_NULL_HANDLE;
        bool IsSignaled = false;
    };

    HostedVulkanDevice mDevice;
    platform::HostedSurfaceChannel* mChannelOrNull = nullptr;
    internal::HostedRing mRing;
    foundation::StaticArray<foundation::StaticArray<Image, internal::HOSTED_IMAGE_COUNT>,
                            internal::MAX_HOSTED_GENERATIONS>
        mImages{};
    foundation::StaticArray<foundation::uint8, platform::HOSTED_ADAPTER_UUID_BYTES> mDeviceUuid{};
    foundation::StaticArray<foundation::uint8, platform::HOSTED_ADAPTER_UUID_BYTES> mDriverUuid{};
    /// Depth epoch each generation's framebuffers were built with.
    foundation::StaticArray<foundation::uint64, internal::MAX_HOSTED_GENERATIONS> mDepthEpochs{};
    platform::HostedSurfaceImages mPending;
    bool mHasPending = false;
    bool mIsAcquired = false;
    foundation::uint32 mAcquiredImage = 0;
    bool mIsChannelLost = false;
};
} // namespace ludus::graphics::rhi::backend
