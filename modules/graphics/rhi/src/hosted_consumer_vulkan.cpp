#include "internal/hosted_consumer.h"
#include "internal/hosted_vulkan.h"

#include <ludus/foundation/containers/static_array.hpp>

#include <cstring>
#include <new>

#include <volk.h>

// Thanks to the Khronos Vulkan Working Group for the VK_KHR_external_memory_fd and
// VkExternalMemoryHandleTypeFlagBits reference pages
// (https://docs.vulkan.org/refpages/latest/refpages/source/VK_KHR_external_memory_fd.html,
// https://docs.vulkan.org/refpages/latest/refpages/source/VkExternalMemoryHandleTypeFlagBits.html):
// an opaque file descriptor is importable only on a device whose deviceUUID and
// driverUUID equal the exporter's, into an allocation of the exported size bound
// to an identically created image. A successful import transfers the descriptor's
// ownership to the implementation. Thanks also for the specification's "Queue Family
// Ownership Transfer" and vkQueueSubmit sections
// (https://docs.vulkan.org/spec/latest/chapters/synchronization.html): the acquire
// half of a transfer repeats the release's layouts, barriers order work across
// submissions to one queue, and a fence signals once all work submitted to the queue
// before it has completed.
namespace ludus::graphics::rhi::internal
{
// The public header names this Vulkan value without including Vulkan.
static_assert(HOSTED_VULKAN_SHADER_READ_LAYOUT == static_cast<foundation::uint32>(backend::HOSTED_PRESENTED_LAYOUT));

namespace
{
using namespace foundation;
/// vkWaitForFences timeout that never expires; teardown only.
constexpr uint64 NO_TIMEOUT = ~uint64{0};

/// Native bits of a non-dispatchable handle, as NativeVulkanImage carries them.
template <typename HandleType>
[[nodiscard]] uint64 GetHandleBits(HandleType handle) noexcept
{
#if VK_USE_64_BIT_PTR_DEFINES
    return reinterpret_cast<uint64>(handle);
#else
    return handle;
#endif
}
} // namespace

/// Vulkan objects of one consumer. The entry points come from volk tables loaded for the
/// consumer's own instance and device, so they stay valid when an RHI session on the same
/// device finalizes volk's global pointers.
struct HostedConsumerBackend final
{
    /// One imported image and the commands that move it between the game and this queue.
    struct Image final
    {
        VkImage Object = VK_NULL_HANDLE;
        VkDeviceMemory Memory = VK_NULL_HANDLE;
        /// Pre-recorded acquire from VK_QUEUE_FAMILY_EXTERNAL to the consumer's queue family.
        VkCommandBuffer Acquire = VK_NULL_HANDLE;
        /// Pre-recorded release back to VK_QUEUE_FAMILY_EXTERNAL.
        VkCommandBuffer Release = VK_NULL_HANDLE;
        /// Signalled by a queued release; unsignalled otherwise.
        VkFence Released = VK_NULL_HANDLE;
        bool IsReleaseQueued = false;
    };

    VkPhysicalDevice PhysicalDevice = VK_NULL_HANDLE;
    VkDevice Device = VK_NULL_HANDLE;
    VkQueue Queue = VK_NULL_HANDLE;
    uint32 QueueFamily = 0;
    VolkInstanceTable Instance{};
    VolkDeviceTable Functions{};
    VkCommandPool Commands = VK_NULL_HANDLE;
    StaticArray<StaticArray<Image, platform::HOSTED_SURFACE_IMAGE_COUNT>, HOSTED_GENERATION_SLOTS> Slots{};
};

namespace
{
using Image = HostedConsumerBackend::Image;

[[nodiscard]] bool AreAdaptersEqual(const HostedConsumerBackend& backend,
                                    const platform::HostedSurfaceImages& images) noexcept
{
    VkPhysicalDeviceIDProperties identity{};
    identity.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES;
    VkPhysicalDeviceProperties2 properties{};
    properties.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
    properties.pNext = &identity;
    backend.Instance.vkGetPhysicalDeviceProperties2(backend.PhysicalDevice, &properties);
    static_assert(VK_UUID_SIZE == platform::HOSTED_ADAPTER_UUID_BYTES);
    return std::memcmp(identity.deviceUUID, images.DeviceUuid.GetData(), VK_UUID_SIZE) == 0 &&
           std::memcmp(identity.driverUUID, images.DriverUuid.GetData(), VK_UUID_SIZE) == 0;
}

void DestroyImage(const HostedConsumerBackend& backend, Image& inoutImage) noexcept
{
    const VolkDeviceTable& functions = backend.Functions;
    if (inoutImage.IsReleaseQueued == true)
    {
        static_cast<void>(functions.vkWaitForFences(backend.Device, 1, &inoutImage.Released, VK_TRUE, NO_TIMEOUT));
    }
    if (inoutImage.Acquire != VK_NULL_HANDLE)
    {
        functions.vkFreeCommandBuffers(backend.Device, backend.Commands, 1, &inoutImage.Acquire);
    }
    if (inoutImage.Release != VK_NULL_HANDLE)
    {
        functions.vkFreeCommandBuffers(backend.Device, backend.Commands, 1, &inoutImage.Release);
    }
    functions.vkDestroyFence(backend.Device, inoutImage.Released, nullptr);
    functions.vkDestroyImage(backend.Device, inoutImage.Object, nullptr);
    functions.vkFreeMemory(backend.Device, inoutImage.Memory, nullptr);
    inoutImage = {};
}

void DestroySlot(HostedConsumerBackend& inoutBackend, usize slot) noexcept
{
    for (Image& image : inoutBackend.Slots[slot])
    {
        DestroyImage(inoutBackend, image);
    }
}

/// Direction of a hosted image's queue family transfer on the consumer's queue.
enum class Transfer : uint8
{
    /// From the external queue family (the game) to the consumer's, before sampling.
    Acquire,
    /// From the consumer's queue family back to the external one, after sampling.
    Release,
};

/// Record one half of the queue family transfer between the game and this queue.
[[nodiscard]] bool
RecordTransfer(const HostedConsumerBackend& backend, VkImage image, Transfer transfer, VkCommandBuffer command) noexcept
{
    const VolkDeviceTable& functions = backend.Functions;
    VkCommandBufferBeginInfo begin{};
    begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    // Each buffer is submitted once per presentation of its image; simultaneous use keeps a
    // resubmission valid however the caller learnt that the earlier one completed.
    begin.flags = VK_COMMAND_BUFFER_USAGE_SIMULTANEOUS_USE_BIT;
    if (functions.vkBeginCommandBuffer(command, &begin) != VK_SUCCESS)
    {
        return false;
    }
    const bool isAcquire = transfer == Transfer::Acquire;
    VkImageMemoryBarrier barrier{};
    barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    // The acquire repeats the game's release: the same layouts, EXTERNAL to this queue family,
    // and makes the contents visible to any later read on the queue. The release keeps the
    // layout and only orders the queue's earlier sampling before the image leaves.
    barrier.srcAccessMask = 0;
    barrier.dstAccessMask = isAcquire == true ? VK_ACCESS_MEMORY_READ_BIT : 0;
    barrier.oldLayout = isAcquire == true ? backend::HOSTED_RENDERED_LAYOUT : backend::HOSTED_PRESENTED_LAYOUT;
    barrier.newLayout = backend::HOSTED_PRESENTED_LAYOUT;
    barrier.srcQueueFamilyIndex = isAcquire == true ? VK_QUEUE_FAMILY_EXTERNAL : backend.QueueFamily;
    barrier.dstQueueFamilyIndex = isAcquire == true ? backend.QueueFamily : VK_QUEUE_FAMILY_EXTERNAL;
    barrier.image = image;
    barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    barrier.subresourceRange.levelCount = 1;
    barrier.subresourceRange.layerCount = 1;
    const VkPipelineStageFlags sourceStages =
        isAcquire == true ? VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT : VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
    const VkPipelineStageFlags destinationStages =
        isAcquire == true ? VK_PIPELINE_STAGE_ALL_COMMANDS_BIT : VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT;
    functions.vkCmdPipelineBarrier(command, sourceStages, destinationStages, 0, 0, nullptr, 0, nullptr, 1, &barrier);
    return functions.vkEndCommandBuffer(command) == VK_SUCCESS;
}

[[nodiscard]] bool TryImportImage(const HostedConsumerBackend& backend,
                                  const platform::HostedSurfaceImages& images,
                                  usize index,
                                  platform::HostedImageHandle& inoutHandle,
                                  Image& inoutImage) noexcept
{
    const VolkDeviceTable& functions = backend.Functions;
    const VkDevice device = backend.Device;
    VkExternalMemoryImageCreateInfo external{};
    VkImageCreateInfo create{};
    backend::FillHostedImageInfo(images.Width, images.Height, external, create);
    if (functions.vkCreateImage(device, &create, nullptr, &inoutImage.Object) != VK_SUCCESS)
    {
        inoutImage.Object = VK_NULL_HANDLE;
        return false;
    }
    VkMemoryRequirements requirements{};
    functions.vkGetImageMemoryRequirements(device, inoutImage.Object, &requirements);
    if ((requirements.memoryTypeBits & (1U << images.MemoryTypeIndex)) == 0 ||
        requirements.size > images.AllocationSizes[index])
    {
        return false;
    }
    VkMemoryDedicatedAllocateInfo dedicated{};
    dedicated.sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO;
    dedicated.image = inoutImage.Object;
    VkImportMemoryFdInfoKHR imported{};
    imported.sType = VK_STRUCTURE_TYPE_IMPORT_MEMORY_FD_INFO_KHR;
    imported.pNext = &dedicated;
    imported.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT;
    imported.fd = inoutHandle.GetFileDescriptor();
    VkMemoryAllocateInfo allocate{};
    allocate.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocate.pNext = &imported;
    allocate.allocationSize = images.AllocationSizes[index];
    allocate.memoryTypeIndex = images.MemoryTypeIndex;
    if (functions.vkAllocateMemory(device, &allocate, nullptr, &inoutImage.Memory) != VK_SUCCESS)
    {
        // A failed import leaves the descriptor with the caller, which closes it.
        inoutImage.Memory = VK_NULL_HANDLE;
        return false;
    }
    // The implementation owns the descriptor after a successful import.
    static_cast<void>(inoutHandle.ReleaseFileDescriptor());
    if (functions.vkBindImageMemory(device, inoutImage.Object, inoutImage.Memory, 0) != VK_SUCCESS)
    {
        return false;
    }
    VkCommandBufferAllocateInfo commands{};
    commands.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    commands.commandPool = backend.Commands;
    commands.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    commands.commandBufferCount = 1;
    if (functions.vkAllocateCommandBuffers(device, &commands, &inoutImage.Acquire) != VK_SUCCESS)
    {
        inoutImage.Acquire = VK_NULL_HANDLE;
        return false;
    }
    if (functions.vkAllocateCommandBuffers(device, &commands, &inoutImage.Release) != VK_SUCCESS)
    {
        inoutImage.Release = VK_NULL_HANDLE;
        return false;
    }
    VkFenceCreateInfo fence{};
    fence.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    if (functions.vkCreateFence(device, &fence, nullptr, &inoutImage.Released) != VK_SUCCESS)
    {
        inoutImage.Released = VK_NULL_HANDLE;
        return false;
    }
    return RecordTransfer(backend, inoutImage.Object, Transfer::Acquire, inoutImage.Acquire) == true &&
           RecordTransfer(backend, inoutImage.Object, Transfer::Release, inoutImage.Release) == true;
}

[[nodiscard]] bool Submit(const HostedConsumerBackend& backend, const VkCommandBuffer& command, VkFence fence) noexcept
{
    VkSubmitInfo submit{};
    submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &command;
    return backend.Functions.vkQueueSubmit(backend.Queue, 1, &submit, fence) == VK_SUCCESS;
}
} // namespace

void HostedConsumerBackendDeleter::operator()(HostedConsumerBackend* backendOrNull) const noexcept
{
    if (backendOrNull == nullptr)
    {
        return;
    }
    if (backendOrNull->Commands != VK_NULL_HANDLE)
    {
        for (usize slot = 0; slot < HOSTED_GENERATION_SLOTS; ++slot)
        {
            DestroySlot(*backendOrNull, slot);
        }
        backendOrNull->Functions.vkDestroyCommandPool(backendOrNull->Device, backendOrNull->Commands, nullptr);
    }
    delete backendOrNull;
}

bool IsHostedImportAvailable(Backend api) noexcept
{
    return api == Backend::Vulkan;
}

HostedConsumerStatus CreateHostedConsumerBackend(const NativeDeviceIdentity& device,
                                                 HostedConsumerBackendOwner& outBackend) noexcept
{
    if (device.Api != Backend::Vulkan || device.Instance.IsNull() == true || device.PhysicalDevice.IsNull() == true ||
        device.Device.IsNull() == true || device.CommandQueue.IsNull() == true)
    {
        return HostedConsumerStatus::InvalidArgument;
    }
    // volkInitialize() resolves only the loader's entry points; the tables below need it once.
    if (vkGetInstanceProcAddr == nullptr && volkInitialize() != VK_SUCCESS)
    {
        return HostedConsumerStatus::Unavailable;
    }
    HostedConsumerBackend* allocationOrNull = new (std::nothrow) HostedConsumerBackend;
    if (allocationOrNull == nullptr)
    {
        return HostedConsumerStatus::Unavailable;
    }
    HostedConsumerBackendOwner backend(allocationOrNull);
    backend->PhysicalDevice = static_cast<VkPhysicalDevice>(device.PhysicalDevice.GetObject());
    backend->Device = static_cast<VkDevice>(device.Device.GetObject());
    backend->Queue = static_cast<VkQueue>(device.CommandQueue.GetObject());
    backend->QueueFamily = device.QueueFamily;
    volkLoadInstanceTable(&backend->Instance, static_cast<VkInstance>(device.Instance.GetObject()));
    volkLoadDeviceTable(&backend->Functions, backend->Device);
    if (backend->Instance.vkGetPhysicalDeviceProperties2 == nullptr ||
        backend->Functions.vkCreateCommandPool == nullptr)
    {
        return HostedConsumerStatus::Unavailable;
    }
    VkCommandPoolCreateInfo pool{};
    pool.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    pool.queueFamilyIndex = device.QueueFamily;
    if (backend->Functions.vkCreateCommandPool(backend->Device, &pool, nullptr, &backend->Commands) != VK_SUCCESS)
    {
        backend->Commands = VK_NULL_HANDLE;
        return HostedConsumerStatus::Unavailable;
    }
    outBackend = foundation::Move(backend);
    return HostedConsumerStatus::Ok;
}

HostedConsumerRefusal ImportHostedGeneration(HostedConsumerBackend& inoutBackend,
                                             usize slot,
                                             platform::HostedSurfaceImages& inoutImages) noexcept
{
    HostedConsumerRefusal refusal = HostedConsumerRefusal::None;
    for (const platform::HostedImageHandle& handle : inoutImages.Handles)
    {
        if (handle.GetKind() != platform::HostedHandleKind::OpaqueFileDescriptor)
        {
            refusal = HostedConsumerRefusal::UnsupportedHandle;
        }
    }
    if (refusal == HostedConsumerRefusal::None && AreAdaptersEqual(inoutBackend, inoutImages) == false)
    {
        refusal = HostedConsumerRefusal::AdapterMismatch;
    }
    for (usize i = 0; i < platform::HOSTED_SURFACE_IMAGE_COUNT && refusal == HostedConsumerRefusal::None; ++i)
    {
        if (TryImportImage(inoutBackend, inoutImages, i, inoutImages.Handles[i], inoutBackend.Slots[slot][i]) == false)
        {
            refusal = HostedConsumerRefusal::ImportFailed;
        }
    }
    for (platform::HostedImageHandle& handle : inoutImages.Handles)
    {
        handle.Reset();
    }
    if (refusal != HostedConsumerRefusal::None)
    {
        DestroySlot(inoutBackend, slot);
    }
    return refusal;
}

void DestroyHostedGeneration(HostedConsumerBackend& inoutBackend, usize slot) noexcept
{
    DestroySlot(inoutBackend, slot);
}

bool AcquireHostedImage(HostedConsumerBackend& inoutBackend, usize slot, uint32 image, HostedFrame& inoutFrame) noexcept
{
    const Image& imported = inoutBackend.Slots[slot][image];
    if (Submit(inoutBackend, imported.Acquire, VK_NULL_HANDLE) == false)
    {
        return false;
    }
    inoutFrame.VulkanImage = NativeVulkanImage(GetHandleBits(imported.Object));
    inoutFrame.VulkanLayout = HOSTED_VULKAN_SHADER_READ_LAYOUT;
    return true;
}

// The release's fence covers everything submitted to the queue before it, so it signals only
// once the editor's sampling of the image has completed too.
bool EnqueueHostedRelease(HostedConsumerBackend& inoutBackend, usize slot, uint32 image) noexcept
{
    Image& imported = inoutBackend.Slots[slot][image];
    if (imported.IsReleaseQueued == true || Submit(inoutBackend, imported.Release, imported.Released) == false)
    {
        return false;
    }
    imported.IsReleaseQueued = true;
    return true;
}

bool TryCompleteHostedRelease(HostedConsumerBackend& inoutBackend, usize slot, uint32 image) noexcept
{
    Image& imported = inoutBackend.Slots[slot][image];
    if (imported.IsReleaseQueued == false)
    {
        return true;
    }
    // VK_ERROR_DEVICE_LOST also ends the wait: no further work runs on the queue.
    if (inoutBackend.Functions.vkGetFenceStatus(inoutBackend.Device, imported.Released) == VK_NOT_READY)
    {
        return false;
    }
    static_cast<void>(inoutBackend.Functions.vkResetFences(inoutBackend.Device, 1, &imported.Released));
    imported.IsReleaseQueued = false;
    return true;
}

void CompleteHostedRelease(HostedConsumerBackend& inoutBackend, usize slot, uint32 image) noexcept
{
    Image& imported = inoutBackend.Slots[slot][image];
    if (imported.IsReleaseQueued == false)
    {
        return;
    }
    static_cast<void>(
        inoutBackend.Functions.vkWaitForFences(inoutBackend.Device, 1, &imported.Released, VK_TRUE, NO_TIMEOUT));
    static_cast<void>(inoutBackend.Functions.vkResetFences(inoutBackend.Device, 1, &imported.Released));
    imported.IsReleaseQueued = false;
}
} // namespace ludus::graphics::rhi::internal
