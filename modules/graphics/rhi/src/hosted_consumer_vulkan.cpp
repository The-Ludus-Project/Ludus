#include "internal/hosted_consumer.h"
#include "internal/hosted_vulkan.h"

#include <cstring>
#include <unistd.h>

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
// half of a transfer repeats the release's layouts, and a submission with no batches
// signals its fence once all work submitted to the queue before it has completed.
namespace ludus::graphics::rhi::internal
{
// The public header names these Vulkan values without including Vulkan.
static_assert(HOSTED_VULKAN_QUEUE_FAMILY_EXTERNAL == VK_QUEUE_FAMILY_EXTERNAL);
static_assert(HOSTED_VULKAN_SHADER_READ_LAYOUT == static_cast<foundation::uint32>(backend::HOSTED_PRESENTED_LAYOUT));

namespace
{
using namespace foundation;

/// Device functions of the consumer's device, loaded per import from its own dispatch.
struct DeviceFunctions final
{
    PFN_vkCreateImage CreateImage = nullptr;
    PFN_vkDestroyImage DestroyImage = nullptr;
    PFN_vkGetImageMemoryRequirements GetImageMemoryRequirements = nullptr;
    PFN_vkAllocateMemory AllocateMemory = nullptr;
    PFN_vkFreeMemory FreeMemory = nullptr;
    PFN_vkBindImageMemory BindImageMemory = nullptr;
};

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

template <typename HandleType>
[[nodiscard]] HandleType MakeHandle(uint64 bits) noexcept
{
#if VK_USE_64_BIT_PTR_DEFINES
    return reinterpret_cast<HandleType>(bits);
#else
    return bits;
#endif
}

/// Resolve vkGetInstanceProcAddr; volkInitialize() only loads the loader's entry points.
[[nodiscard]] bool TryInitializeLoader() noexcept
{
    return vkGetInstanceProcAddr != nullptr || volkInitialize() == VK_SUCCESS;
}

template <typename FunctionType>
[[nodiscard]] bool TryLoad(PFN_vkGetDeviceProcAddr getDeviceProcedureAddress,
                           VkDevice device,
                           const char* name,
                           FunctionType& outFunction) noexcept
{
    outFunction = reinterpret_cast<FunctionType>(getDeviceProcedureAddress(device, name));
    return outFunction != nullptr;
}

/// Load one entry point of the consumer's own device, independent of the RHI session's dispatch.
template <typename FunctionType>
[[nodiscard]] bool
TryLoadDeviceFunction(const NativeDeviceIdentity& device, const char* name, FunctionType& outFunction) noexcept
{
    if (TryInitializeLoader() == false)
    {
        return false;
    }
    VkInstance instance = static_cast<VkInstance>(device.Instance.GetObject());
    const PFN_vkGetDeviceProcAddr getDeviceProcedureAddress =
        reinterpret_cast<PFN_vkGetDeviceProcAddr>(vkGetInstanceProcAddr(instance, "vkGetDeviceProcAddr"));
    return getDeviceProcedureAddress != nullptr &&
           TryLoad(getDeviceProcedureAddress, static_cast<VkDevice>(device.Device.GetObject()), name, outFunction) ==
               true;
}

/// Load from the consumer's own instance and device, independent of the RHI session's dispatch.
[[nodiscard]] bool TryLoadDeviceFunctions(VkInstance instance, VkDevice device, DeviceFunctions& outFunctions) noexcept
{
    if (TryInitializeLoader() == false)
    {
        return false;
    }
    const PFN_vkGetDeviceProcAddr getDeviceProcedureAddress =
        reinterpret_cast<PFN_vkGetDeviceProcAddr>(vkGetInstanceProcAddr(instance, "vkGetDeviceProcAddr"));
    DeviceFunctions functions;
    if (getDeviceProcedureAddress == nullptr ||
        TryLoad(getDeviceProcedureAddress, device, "vkCreateImage", functions.CreateImage) == false ||
        TryLoad(getDeviceProcedureAddress, device, "vkDestroyImage", functions.DestroyImage) == false ||
        TryLoad(getDeviceProcedureAddress,
                device,
                "vkGetImageMemoryRequirements",
                functions.GetImageMemoryRequirements) == false ||
        TryLoad(getDeviceProcedureAddress, device, "vkAllocateMemory", functions.AllocateMemory) == false ||
        TryLoad(getDeviceProcedureAddress, device, "vkFreeMemory", functions.FreeMemory) == false ||
        TryLoad(getDeviceProcedureAddress, device, "vkBindImageMemory", functions.BindImageMemory) == false)
    {
        return false;
    }
    outFunctions = functions;
    return true;
}

[[nodiscard]] bool
AreAdaptersEqual(VkInstance instance, VkPhysicalDevice physical, const platform::HostedSurfaceImages& images) noexcept
{
    if (TryInitializeLoader() == false)
    {
        return false;
    }
    const PFN_vkGetPhysicalDeviceProperties2 getProperties = reinterpret_cast<PFN_vkGetPhysicalDeviceProperties2>(
        vkGetInstanceProcAddr(instance, "vkGetPhysicalDeviceProperties2"));
    if (getProperties == nullptr)
    {
        return false;
    }
    VkPhysicalDeviceIDProperties identity{};
    identity.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES;
    VkPhysicalDeviceProperties2 properties{};
    properties.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
    properties.pNext = &identity;
    getProperties(physical, &properties);
    static_assert(VK_UUID_SIZE == platform::HOSTED_ADAPTER_UUID_BYTES);
    return std::memcmp(identity.deviceUUID, images.DeviceUuid.GetData(), VK_UUID_SIZE) == 0 &&
           std::memcmp(identity.driverUUID, images.DriverUuid.GetData(), VK_UUID_SIZE) == 0;
}

void DestroyImported(VkDevice device, const DeviceFunctions& functions, HostedImport& inoutImport) noexcept
{
    for (usize i = 0; i < platform::HOSTED_SURFACE_IMAGE_COUNT; ++i)
    {
        if (inoutImport.VulkanImages[i] != 0)
        {
            functions.DestroyImage(device, MakeHandle<VkImage>(inoutImport.VulkanImages[i]), nullptr);
        }
        if (inoutImport.VulkanMemories[i] != 0)
        {
            functions.FreeMemory(device, MakeHandle<VkDeviceMemory>(inoutImport.VulkanMemories[i]), nullptr);
        }
        inoutImport.VulkanImages[i] = 0;
        inoutImport.VulkanMemories[i] = 0;
    }
}

[[nodiscard]] bool TryImportImage(VkDevice device,
                                  const DeviceFunctions& functions,
                                  const platform::HostedSurfaceImages& images,
                                  usize index,
                                  platform::HostedImageHandle& inoutHandle,
                                  HostedImport& inoutImport) noexcept
{
    VkExternalMemoryImageCreateInfo external{};
    VkImageCreateInfo create{};
    backend::FillHostedImageInfo(images.Width, images.Height, external, create);
    VkImage image = VK_NULL_HANDLE;
    if (functions.CreateImage(device, &create, nullptr, &image) != VK_SUCCESS)
    {
        return false;
    }
    inoutImport.VulkanImages[index] = GetHandleBits(image);
    VkMemoryRequirements requirements{};
    functions.GetImageMemoryRequirements(device, image, &requirements);
    if ((requirements.memoryTypeBits & (1U << images.MemoryTypeIndex)) == 0 ||
        requirements.size > images.AllocationSizes[index])
    {
        return false;
    }
    VkMemoryDedicatedAllocateInfo dedicated{};
    dedicated.sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO;
    dedicated.image = image;
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
    VkDeviceMemory memory = VK_NULL_HANDLE;
    if (functions.AllocateMemory(device, &allocate, nullptr, &memory) != VK_SUCCESS)
    {
        // A failed import leaves the descriptor with the caller, which closes it.
        return false;
    }
    // The implementation owns the descriptor after a successful import.
    static_cast<void>(inoutHandle.ReleaseFileDescriptor());
    inoutImport.VulkanMemories[index] = GetHandleBits(memory);
    return functions.BindImageMemory(device, image, memory, 0) == VK_SUCCESS;
}
} // namespace

bool IsHostedImportAvailable(Backend api) noexcept
{
    return api == Backend::Vulkan;
}

HostedConsumerRefusal ImportHostedGeneration(const NativeDeviceIdentity& device,
                                             platform::HostedSurfaceImages& inoutImages,
                                             HostedImport& outImport) noexcept
{
    VkInstance instance = static_cast<VkInstance>(device.Instance.GetObject());
    VkDevice native = static_cast<VkDevice>(device.Device.GetObject());
    HostedConsumerRefusal refusal = HostedConsumerRefusal::None;
    DeviceFunctions functions;
    HostedImport imported;
    for (const platform::HostedImageHandle& handle : inoutImages.Handles)
    {
        if (handle.GetKind() != platform::HostedHandleKind::OpaqueFileDescriptor)
        {
            refusal = HostedConsumerRefusal::UnsupportedHandle;
        }
    }
    if (refusal == HostedConsumerRefusal::None &&
        AreAdaptersEqual(instance, static_cast<VkPhysicalDevice>(device.PhysicalDevice.GetObject()), inoutImages) ==
            false)
    {
        refusal = HostedConsumerRefusal::AdapterMismatch;
    }
    if (refusal == HostedConsumerRefusal::None && TryLoadDeviceFunctions(instance, native, functions) == false)
    {
        refusal = HostedConsumerRefusal::ImportFailed;
    }
    for (usize i = 0; i < platform::HOSTED_SURFACE_IMAGE_COUNT && refusal == HostedConsumerRefusal::None; ++i)
    {
        if (TryImportImage(native, functions, inoutImages, i, inoutImages.Handles[i], imported) == false)
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
        if (functions.DestroyImage != nullptr)
        {
            DestroyImported(native, functions, imported);
        }
        return refusal;
    }
    outImport = imported;
    return HostedConsumerRefusal::None;
}

void DestroyHostedImport(const NativeDeviceIdentity& device, HostedImport& inoutImport) noexcept
{
    VkInstance instance = static_cast<VkInstance>(device.Instance.GetObject());
    VkDevice native = static_cast<VkDevice>(device.Device.GetObject());
    DeviceFunctions functions;
    if (TryLoadDeviceFunctions(instance, native, functions) == true)
    {
        DestroyImported(native, functions, inoutImport);
    }
}

// The marker is a fence submitted with no batches behind the editor's sampling on the same
// queue; it signals once everything submitted before it has completed.
bool EnqueueHostedQueueMarker(const NativeDeviceIdentity& device, HostedQueueMarker& outMarker) noexcept
{
    if (device.Api != Backend::Vulkan || device.CommandQueue.IsNull() == true)
    {
        return false;
    }
    PFN_vkCreateFence createFence = nullptr;
    PFN_vkDestroyFence destroyFence = nullptr;
    PFN_vkQueueSubmit submit = nullptr;
    if (TryLoadDeviceFunction(device, "vkCreateFence", createFence) == false ||
        TryLoadDeviceFunction(device, "vkDestroyFence", destroyFence) == false ||
        TryLoadDeviceFunction(device, "vkQueueSubmit", submit) == false)
    {
        return false;
    }
    VkDevice native = static_cast<VkDevice>(device.Device.GetObject());
    VkFenceCreateInfo description{};
    description.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    VkFence fence = VK_NULL_HANDLE;
    if (createFence(native, &description, nullptr, &fence) != VK_SUCCESS)
    {
        return false;
    }
    if (submit(static_cast<VkQueue>(device.CommandQueue.GetObject()), 0, nullptr, fence) != VK_SUCCESS)
    {
        destroyFence(native, fence, nullptr);
        return false;
    }
    outMarker.VulkanFence = GetHandleBits(fence);
    return true;
}

bool IsHostedQueueMarkerComplete(const NativeDeviceIdentity& device, const HostedQueueMarker& marker) noexcept
{
    PFN_vkGetFenceStatus getStatus = nullptr;
    if (marker.VulkanFence == 0 || TryLoadDeviceFunction(device, "vkGetFenceStatus", getStatus) == false)
    {
        return true;
    }
    // VK_ERROR_DEVICE_LOST also ends the wait: no further work runs on the queue.
    return getStatus(static_cast<VkDevice>(device.Device.GetObject()), MakeHandle<VkFence>(marker.VulkanFence)) !=
           VK_NOT_READY;
}

void WaitHostedQueueMarker(const NativeDeviceIdentity& device, const HostedQueueMarker& marker) noexcept
{
    PFN_vkWaitForFences waitForFences = nullptr;
    if (marker.VulkanFence == 0 || TryLoadDeviceFunction(device, "vkWaitForFences", waitForFences) == false)
    {
        return;
    }
    constexpr uint64 NO_TIMEOUT = ~uint64{0};
    const VkFence fence = MakeHandle<VkFence>(marker.VulkanFence);
    static_cast<void>(waitForFences(static_cast<VkDevice>(device.Device.GetObject()), 1, &fence, VK_TRUE, NO_TIMEOUT));
}

void DestroyHostedQueueMarker(const NativeDeviceIdentity& device, HostedQueueMarker& inoutMarker) noexcept
{
    PFN_vkDestroyFence destroyFence = nullptr;
    if (inoutMarker.VulkanFence != 0 && TryLoadDeviceFunction(device, "vkDestroyFence", destroyFence) == true)
    {
        destroyFence(static_cast<VkDevice>(device.Device.GetObject()),
                     MakeHandle<VkFence>(inoutMarker.VulkanFence),
                     nullptr);
    }
    inoutMarker = {};
}

HostedConsumerStatus RecordHostedOwnershipTransfer(const NativeDeviceIdentity& device,
                                                   uint64 image,
                                                   NativeVulkanCommandBuffer commandBuffer,
                                                   HostedOwnershipTransfer transfer) noexcept
{
    if (commandBuffer.IsNull() == true)
    {
        return HostedConsumerStatus::InvalidArgument;
    }
    PFN_vkCmdPipelineBarrier recordBarrier = nullptr;
    if (TryLoadDeviceFunction(device, "vkCmdPipelineBarrier", recordBarrier) == false)
    {
        return HostedConsumerStatus::Unavailable;
    }
    const bool isAcquire = transfer == HostedOwnershipTransfer::Acquire;
    VkImageMemoryBarrier barrier{};
    barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    // The acquire repeats the game's release: the same layouts, EXTERNAL to this queue family,
    // with the visibility the release could not supply. The release keeps the layout and
    // only orders the sampling before the image leaves; reads need no availability.
    barrier.srcAccessMask = 0;
    barrier.dstAccessMask = isAcquire == true ? VK_ACCESS_SHADER_READ_BIT : 0;
    barrier.oldLayout = isAcquire == true ? backend::HOSTED_RENDERED_LAYOUT : backend::HOSTED_PRESENTED_LAYOUT;
    barrier.newLayout = backend::HOSTED_PRESENTED_LAYOUT;
    barrier.srcQueueFamilyIndex = isAcquire == true ? VK_QUEUE_FAMILY_EXTERNAL : device.QueueFamily;
    barrier.dstQueueFamilyIndex = isAcquire == true ? device.QueueFamily : VK_QUEUE_FAMILY_EXTERNAL;
    barrier.image = MakeHandle<VkImage>(image);
    barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    barrier.subresourceRange.levelCount = 1;
    barrier.subresourceRange.layerCount = 1;
    const VkPipelineStageFlags sourceStages =
        isAcquire == true ? VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT : VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
    const VkPipelineStageFlags destinationStages =
        isAcquire == true ? VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT : VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT;
    recordBarrier(static_cast<VkCommandBuffer>(commandBuffer.GetObject()),
                  sourceStages,
                  destinationStages,
                  0,
                  0,
                  nullptr,
                  0,
                  nullptr,
                  1,
                  &barrier);
    return HostedConsumerStatus::Ok;
}
} // namespace ludus::graphics::rhi::internal
