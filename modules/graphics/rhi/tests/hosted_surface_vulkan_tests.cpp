// Two-process hosted surface tests on Vulkan (Linux; P5 of the play-in-viewport
// design, docs/architecture/editor-gui-systems.md, "Delivery stages"). The test
// process plays the editor: it owns the hosted-window endpoint and imports the
// game's exported memory onto its own VkDevice. The game is
// ludus_rhi_hosted_producer, an ordinary RHI application in a hosted window.
//
// The first case is the device-UUID probe the design required before the
// backend: every adapter reports VkPhysicalDeviceIDProperties, and the hosted
// image (RGBA8, optimal tiling, the hosted usage) is exportable and importable
// as an opaque file descriptor.
//
// The tests skip when the host has no Vulkan adapter with VK_KHR_external_memory_fd.

#include <ludus/foundation/base/core.h>

#include <ludus/foundation/containers/static_array.hpp>
#include <ludus/graphics/rhi/hosted_surface.h>
#include <ludus/graphics/rhi/native_interop.h>
#include <ludus/platform/hosted_surface.h>

#include "hosted_test_harness.h"
#include "hosted_test_protocol.h"
#include "internal/hosted_vulkan.h"

#include <cstring>

#include <catch2/catch_test_macros.hpp>
#include <volk.h>

using namespace ludus::foundation;
namespace rhi = ludus::graphics::rhi;
namespace backend = ludus::graphics::rhi::backend;
namespace hosted_test = ludus::graphics::rhi::hosted_test;

namespace
{
constexpr usize BYTES_PER_PIXEL = 4;
constexpr usize RESIZE_ROUNDS = 5;
constexpr uint32 MAX_PHYSICAL_DEVICES = 16;
constexpr uint32 MAX_QUEUE_FAMILIES = 32;
constexpr uint64 FENCE_TIMEOUT_NANOSECONDS = 5'000'000'000ULL;

/// The editor's own Vulkan device with VK_KHR_external_memory_fd, as Qt would own it.
class ConsumerDevice final
{
public:
    ConsumerDevice()
    {
        if (volkInitialize() != VK_SUCCESS)
        {
            return;
        }
        VkApplicationInfo application{};
        application.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
        application.apiVersion = VK_API_VERSION_1_1;
        VkInstanceCreateInfo instance{};
        instance.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
        instance.pApplicationInfo = &application;
        if (vkCreateInstance(&instance, nullptr, &mInstance) != VK_SUCCESS)
        {
            mInstance = VK_NULL_HANDLE;
            return;
        }
        volkLoadInstanceOnly(mInstance);
        // Choose the adapter as the producer's RHI does: the first with a graphics queue
        // and the hosted memory extension.
        uint32 count = 0;
        if (vkEnumeratePhysicalDevices(mInstance, &count, nullptr) != VK_SUCCESS || count == 0 ||
            count > MAX_PHYSICAL_DEVICES)
        {
            return;
        }
        StaticArray<VkPhysicalDevice, MAX_PHYSICAL_DEVICES> devices{};
        if (vkEnumeratePhysicalDevices(mInstance, &count, devices.GetData()) != VK_SUCCESS)
        {
            return;
        }
        for (uint32 i = 0; i < count && mPhysical == VK_NULL_HANDLE; ++i)
        {
            uint32 families = 0;
            vkGetPhysicalDeviceQueueFamilyProperties(devices[i], &families, nullptr);
            if (families > MAX_QUEUE_FAMILIES || HasExtension(devices[i]) == false)
            {
                continue;
            }
            StaticArray<VkQueueFamilyProperties, MAX_QUEUE_FAMILIES> properties{};
            vkGetPhysicalDeviceQueueFamilyProperties(devices[i], &families, properties.GetData());
            for (uint32 j = 0; j < families; ++j)
            {
                if (properties[j].queueCount != 0 && (properties[j].queueFlags & VK_QUEUE_GRAPHICS_BIT) != 0)
                {
                    mPhysical = devices[i];
                    mFamily = j;
                    break;
                }
            }
        }
        if (mPhysical == VK_NULL_HANDLE)
        {
            return;
        }
        const float32 priority = 1.0F;
        VkDeviceQueueCreateInfo queue{};
        queue.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
        queue.queueFamilyIndex = mFamily;
        queue.queueCount = 1;
        queue.pQueuePriorities = &priority;
        const char* extension = backend::HOSTED_MEMORY_EXTENSION_NAME;
        VkDeviceCreateInfo device{};
        device.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
        device.queueCreateInfoCount = 1;
        device.pQueueCreateInfos = &queue;
        device.enabledExtensionCount = 1;
        device.ppEnabledExtensionNames = &extension;
        if (vkCreateDevice(mPhysical, &device, nullptr, &mDevice) != VK_SUCCESS)
        {
            mDevice = VK_NULL_HANDLE;
            return;
        }
        volkLoadDevice(mDevice);
        vkGetDeviceQueue(mDevice, mFamily, 0, &mQueue);
        VkCommandPoolCreateInfo pool{};
        pool.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
        pool.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        pool.queueFamilyIndex = mFamily;
        if (vkCreateCommandPool(mDevice, &pool, nullptr, &mCommands) != VK_SUCCESS)
        {
            mCommands = VK_NULL_HANDLE;
        }
    }
    ~ConsumerDevice()
    {
        if (mDevice != VK_NULL_HANDLE)
        {
            static_cast<void>(vkDeviceWaitIdle(mDevice));
            vkDestroyCommandPool(mDevice, mCommands, nullptr);
            vkDestroyDevice(mDevice, nullptr);
        }
        if (mInstance != VK_NULL_HANDLE)
        {
            vkDestroyInstance(mInstance, nullptr);
        }
    }
    ConsumerDevice(const ConsumerDevice&) = delete;
    ConsumerDevice& operator=(const ConsumerDevice&) = delete;
    ConsumerDevice(ConsumerDevice&&) = delete;
    ConsumerDevice& operator=(ConsumerDevice&&) = delete;

    [[nodiscard]] bool IsReady() const noexcept
    {
        return mCommands != VK_NULL_HANDLE;
    }
    [[nodiscard]] VkPhysicalDevice GetPhysicalDevice() const noexcept
    {
        return mPhysical;
    }

    [[nodiscard]] rhi::NativeDeviceIdentity GetIdentity() const
    {
        rhi::NativeDeviceIdentity identity;
        identity.Api = rhi::Backend::Vulkan;
        identity.Instance = rhi::NativeInstance(static_cast<void*>(mInstance));
        identity.PhysicalDevice = rhi::NativePhysicalDevice(static_cast<void*>(mPhysical));
        identity.Device = rhi::NativeLogicalDevice(static_cast<void*>(mDevice));
        identity.CommandQueue = rhi::NativeCommandQueue(static_cast<void*>(mQueue));
        identity.QueueFamily = mFamily;
        return identity;
    }

    /// Sample a frame: acquire it from VK_QUEUE_FAMILY_EXTERNAL, copy its centre pixel, wait.
    [[nodiscard]] StaticArray<uint8, BYTES_PER_PIXEL> ReadCenterPixel(const rhi::HostedFrame& frame) const
    {
        REQUIRE(frame.VulkanLayout == rhi::HOSTED_VULKAN_SHADER_READ_LAYOUT);
        REQUIRE(frame.VulkanSourceQueueFamily == rhi::HOSTED_VULKAN_QUEUE_FAMILY_EXTERNAL);
        const VkImage image = reinterpret_cast<VkImage>(frame.VulkanImage.GetBits());
        VkBufferCreateInfo bufferInfo{};
        bufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        bufferInfo.size = BYTES_PER_PIXEL;
        bufferInfo.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        VkBuffer buffer = VK_NULL_HANDLE;
        REQUIRE(vkCreateBuffer(mDevice, &bufferInfo, nullptr, &buffer) == VK_SUCCESS);
        VkMemoryRequirements requirements{};
        vkGetBufferMemoryRequirements(mDevice, buffer, &requirements);
        VkDeviceMemory memory = AllocateHostVisible(requirements);
        REQUIRE(vkBindBufferMemory(mDevice, buffer, memory, 0) == VK_SUCCESS);

        VkCommandBufferAllocateInfo allocate{};
        allocate.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        allocate.commandPool = mCommands;
        allocate.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        allocate.commandBufferCount = 1;
        VkCommandBuffer command = VK_NULL_HANDLE;
        REQUIRE(vkAllocateCommandBuffers(mDevice, &allocate, &command) == VK_SUCCESS);
        VkCommandBufferBeginInfo begin{};
        begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        REQUIRE(vkBeginCommandBuffer(command, &begin) == VK_SUCCESS);
        // Acquire half of the producer's release: same layouts, EXTERNAL to this family.
        VkImageMemoryBarrier acquire{};
        acquire.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        acquire.srcAccessMask = 0;
        acquire.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        acquire.oldLayout = static_cast<VkImageLayout>(frame.VulkanLayout);
        acquire.newLayout = static_cast<VkImageLayout>(frame.VulkanLayout);
        acquire.srcQueueFamilyIndex = frame.VulkanSourceQueueFamily;
        acquire.dstQueueFamilyIndex = mFamily;
        acquire.image = image;
        acquire.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        acquire.subresourceRange.levelCount = 1;
        acquire.subresourceRange.layerCount = 1;
        VkImageMemoryBarrier toTransfer = acquire;
        toTransfer.srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
        toTransfer.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        toTransfer.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        toTransfer.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        toTransfer.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        vkCmdPipelineBarrier(command,
                             VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                             VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                             0,
                             0,
                             nullptr,
                             0,
                             nullptr,
                             1,
                             &acquire);
        vkCmdPipelineBarrier(command,
                             VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                             VK_PIPELINE_STAGE_TRANSFER_BIT,
                             0,
                             0,
                             nullptr,
                             0,
                             nullptr,
                             1,
                             &toTransfer);
        VkBufferImageCopy region{};
        region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        region.imageSubresource.layerCount = 1;
        region.imageOffset =
        {
            .x = static_cast<int32>(frame.Width / 2),
            .y = static_cast<int32>(frame.Height / 2),
            .z = 0,
        };
        region.imageExtent = { .width = 1, .height = 1, .depth = 1 };
        vkCmdCopyImageToBuffer(command, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buffer, 1, &region);
        REQUIRE(vkEndCommandBuffer(command) == VK_SUCCESS);
        VkFenceCreateInfo fenceInfo{};
        fenceInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
        VkFence fence = VK_NULL_HANDLE;
        REQUIRE(vkCreateFence(mDevice, &fenceInfo, nullptr, &fence) == VK_SUCCESS);
        VkSubmitInfo submit{};
        submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        submit.commandBufferCount = 1;
        submit.pCommandBuffers = &command;
        REQUIRE(vkQueueSubmit(mQueue, 1, &submit, fence) == VK_SUCCESS);
        // The frame may be released only after this sampling has completed.
        REQUIRE(vkWaitForFences(mDevice, 1, &fence, VK_TRUE, FENCE_TIMEOUT_NANOSECONDS) == VK_SUCCESS);
        void* mappedOrNull = nullptr;
        REQUIRE(vkMapMemory(mDevice, memory, 0, BYTES_PER_PIXEL, 0, &mappedOrNull) == VK_SUCCESS);
        StaticArray<uint8, BYTES_PER_PIXEL> pixel{};
        std::memcpy(pixel.GetData(), mappedOrNull, BYTES_PER_PIXEL);
        vkUnmapMemory(mDevice, memory);
        vkDestroyFence(mDevice, fence, nullptr);
        vkFreeCommandBuffers(mDevice, mCommands, 1, &command);
        vkDestroyBuffer(mDevice, buffer, nullptr);
        vkFreeMemory(mDevice, memory, nullptr);
        return pixel;
    }

private:
    [[nodiscard]] static bool HasExtension(VkPhysicalDevice physical)
    {
        constexpr uint32 MAX_EXTENSIONS = 512;
        uint32 count = 0;
        if (vkEnumerateDeviceExtensionProperties(physical, nullptr, &count, nullptr) != VK_SUCCESS ||
            count > MAX_EXTENSIONS)
        {
            return false;
        }
        StaticArray<VkExtensionProperties, MAX_EXTENSIONS> available{};
        if (vkEnumerateDeviceExtensionProperties(physical, nullptr, &count, available.GetData()) != VK_SUCCESS)
        {
            return false;
        }
        for (uint32 i = 0; i < count; ++i)
        {
            if (std::strcmp(available[i].extensionName, backend::HOSTED_MEMORY_EXTENSION_NAME) == 0)
            {
                return true;
            }
        }
        return false;
    }

    [[nodiscard]] VkDeviceMemory AllocateHostVisible(const VkMemoryRequirements& requirements) const
    {
        VkPhysicalDeviceMemoryProperties properties{};
        vkGetPhysicalDeviceMemoryProperties(mPhysical, &properties);
        const VkMemoryPropertyFlags flags = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
        for (uint32 i = 0; i < properties.memoryTypeCount; ++i)
        {
            if ((requirements.memoryTypeBits & (1U << i)) != 0 &&
                (properties.memoryTypes[i].propertyFlags & flags) == flags)
            {
                VkMemoryAllocateInfo allocate{};
                allocate.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
                allocate.allocationSize = requirements.size;
                allocate.memoryTypeIndex = i;
                VkDeviceMemory memory = VK_NULL_HANDLE;
                REQUIRE(vkAllocateMemory(mDevice, &allocate, nullptr, &memory) == VK_SUCCESS);
                return memory;
            }
        }
        FAIL("No host-visible coherent memory type");
        return VK_NULL_HANDLE;
    }

    VkInstance mInstance = VK_NULL_HANDLE;
    VkPhysicalDevice mPhysical = VK_NULL_HANDLE;
    VkDevice mDevice = VK_NULL_HANDLE;
    VkQueue mQueue = VK_NULL_HANDLE;
    VkCommandPool mCommands = VK_NULL_HANDLE;
    uint32 mFamily = 0;
};

void RequireConsumerDevice(const ConsumerDevice& device)
{
    if (device.IsReady() == false)
    {
        SKIP("No Vulkan adapter with VK_KHR_external_memory_fd on this host");
    }
}

void CheckTestColor(const StaticArray<uint8, BYTES_PER_PIXEL>& pixel)
{
    for (usize i = 0; i < BYTES_PER_PIXEL; ++i)
    {
        const int32 difference = static_cast<int32>(pixel[i]) - static_cast<int32>(hosted_test::HOSTED_TEST_BYTES[i]);
        CHECK(difference <= hosted_test::COLOR_TOLERANCE);
        CHECK(-difference <= hosted_test::COLOR_TOLERANCE);
    }
}
} // namespace

TEST_CASE("Probe: the adapter identifies itself and exports hosted images as opaque fds", "[rhi][hosted][linux][probe]")
{
    const ConsumerDevice device;
    RequireConsumerDevice(device);
    VkPhysicalDeviceIDProperties identity{};
    identity.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES;
    VkPhysicalDeviceProperties2 properties{};
    properties.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
    properties.pNext = &identity;
    vkGetPhysicalDeviceProperties2(device.GetPhysicalDevice(), &properties);
    // An all-zero UUID would make every adapter look alike to the mismatch check.
    uint8 deviceBits = 0;
    uint8 driverBits = 0;
    for (usize i = 0; i < VK_UUID_SIZE; ++i)
    {
        deviceBits = static_cast<uint8>(deviceBits | identity.deviceUUID[i]);
        driverBits = static_cast<uint8>(driverBits | identity.driverUUID[i]);
    }
    CHECK(deviceBits != 0);
    CHECK(driverBits != 0);

    VkPhysicalDeviceExternalImageFormatInfo external{};
    external.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_IMAGE_FORMAT_INFO;
    external.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT;
    VkPhysicalDeviceImageFormatInfo2 format{};
    format.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_FORMAT_INFO_2;
    format.pNext = &external;
    format.format = backend::HOSTED_IMAGE_FORMAT;
    format.type = VK_IMAGE_TYPE_2D;
    format.tiling = VK_IMAGE_TILING_OPTIMAL;
    format.usage = backend::HOSTED_IMAGE_USAGE;
    VkExternalImageFormatProperties externalProperties{};
    externalProperties.sType = VK_STRUCTURE_TYPE_EXTERNAL_IMAGE_FORMAT_PROPERTIES;
    VkImageFormatProperties2 formatProperties{};
    formatProperties.sType = VK_STRUCTURE_TYPE_IMAGE_FORMAT_PROPERTIES_2;
    formatProperties.pNext = &externalProperties;
    REQUIRE(vkGetPhysicalDeviceImageFormatProperties2(device.GetPhysicalDevice(), &format, &formatProperties) ==
            VK_SUCCESS);
    const VkExternalMemoryFeatureFlags features = externalProperties.externalMemoryProperties.externalMemoryFeatures;
    CHECK((features & VK_EXTERNAL_MEMORY_FEATURE_EXPORTABLE_BIT) != 0);
    CHECK((features & VK_EXTERNAL_MEMORY_FEATURE_IMPORTABLE_BIT) != 0);
    CHECK(formatProperties.imageFormatProperties.maxExtent.width >= hosted_test::INITIAL_WIDTH);
}

TEST_CASE("A hosted Vulkan game's frame arrives in the editor with its known colour", "[rhi][hosted][linux][gpu]")
{
    const ConsumerDevice device;
    RequireConsumerDevice(device);
    hosted_test::HostedSession session;
    session.Start(device.GetIdentity(), "render");
    rhi::HostedFrame frame;
    REQUIRE(session.TryAcquireNext(frame));
    CHECK(frame.Width == hosted_test::INITIAL_WIDTH);
    CHECK(frame.Height == hosted_test::INITIAL_HEIGHT);
    CHECK(frame.VulkanImage.IsNull() == false);
    CheckTestColor(device.ReadCenterPixel(frame));
    CHECK(session.GetConsumer().ReleaseFrame(frame) == rhi::HostedConsumerStatus::Ok);
    rhi::HostedFrame next;
    REQUIRE(session.TryAcquireNext(next));
    CHECK(next.Sequence > frame.Sequence);
    CHECK(session.GetConsumer().ReleaseFrame(next) == rhi::HostedConsumerStatus::Ok);
    hosted_test::ProducerStatistics statistics;
    session.Close(statistics);
    CHECK(statistics.ReadyFrames >= 2);
}

TEST_CASE("A hosted Vulkan game skips frames while the editor holds every image", "[rhi][hosted][linux][gpu]")
{
    const ConsumerDevice device;
    RequireConsumerDevice(device);
    hosted_test::HostedSession session;
    session.Start(device.GetIdentity(), "render");
    StaticArray<rhi::HostedFrame, ludus::platform::HOSTED_SURFACE_IMAGE_COUNT> held{};
    StaticArray<bool, ludus::platform::HOSTED_SURFACE_IMAGE_COUNT> isImageHeld{};
    for (rhi::HostedFrame& frame : held)
    {
        REQUIRE(session.TryAcquireNext(frame));
        REQUIRE(frame.Image < isImageHeld.GetSize());
        CHECK_FALSE(isImageHeld[frame.Image]);
        isImageHeld[frame.Image] = true;
    }
    CHECK_FALSE(
        session.PumpUntil([&session]() { return session.GetConsumer().HasFrame(); }, hosted_test::QUIET_NANOSECONDS));
    REQUIRE(session.GetConsumer().ReleaseFrame(held[1]) == rhi::HostedConsumerStatus::Ok);
    rhi::HostedFrame resumed;
    REQUIRE(session.TryAcquireNext(resumed));
    CHECK(resumed.Image == held[1].Image);
    CheckTestColor(device.ReadCenterPixel(resumed));
    CHECK(session.GetConsumer().ReleaseFrame(resumed) == rhi::HostedConsumerStatus::Ok);
    CHECK(session.GetConsumer().ReleaseFrame(held[0]) == rhi::HostedConsumerStatus::Ok);
    CHECK(session.GetConsumer().ReleaseFrame(held[2]) == rhi::HostedConsumerStatus::Ok);
    hosted_test::ProducerStatistics statistics;
    session.Close(statistics);
    CHECK(statistics.SkippedFrames > 0);
    CHECK(statistics.MaxSkippedNanoseconds < hosted_test::MAX_SKIPPED_NANOSECONDS);
}

TEST_CASE("Resizing a hosted Vulkan game retires the old generations", "[rhi][hosted][linux][gpu]")
{
    const ConsumerDevice device;
    RequireConsumerDevice(device);
    hosted_test::HostedSession session;
    session.Start(device.GetIdentity(), "render");
    rhi::HostedFrame shown;
    REQUIRE(session.TryAcquireNext(shown));
    for (usize round = 0; round < RESIZE_ROUNDS; ++round)
    {
        const uint32 width = 32 + static_cast<uint32>(round * 8);
        const uint32 height = 16 + static_cast<uint32>(round * 4);
        REQUIRE(session.GetEndpoint().SendResize(width, height, 1.0F) == ludus::platform::HostedEndpointStatus::Ok);
        rhi::HostedFrame next;
        REQUIRE(session.TryAcquireNext(next));
        while (next.Width != width)
        {
            REQUIRE(session.GetConsumer().ReleaseFrame(next) == rhi::HostedConsumerStatus::Ok);
            REQUIRE(session.TryAcquireNext(next));
        }
        CHECK(next.Height == height);
        CheckTestColor(device.ReadCenterPixel(next));
        REQUIRE(session.GetConsumer().ReleaseFrame(shown) == rhi::HostedConsumerStatus::Ok);
        CHECK(session.GetConsumer().GetImportedGenerationCount() == 1);
        shown = next;
    }
    REQUIRE(session.GetConsumer().ReleaseFrame(shown) == rhi::HostedConsumerStatus::Ok);
    hosted_test::ProducerStatistics statistics;
    session.Close(statistics);
}

TEST_CASE("The editor refuses hosted images from another adapter", "[rhi][hosted][linux][gpu]")
{
    const ConsumerDevice device;
    RequireConsumerDevice(device);
    hosted_test::HostedSession session;
    // The forged producer announces UUIDs no adapter reports.
    session.Start(device.GetIdentity(), "forged");
    REQUIRE(session.PumpUntil(
        [&session]() { return session.GetConsumer().GetRefusal() != rhi::HostedConsumerRefusal::None; }));
    CHECK(session.GetConsumer().GetRefusal() == rhi::HostedConsumerRefusal::AdapterMismatch);
    CHECK(session.GetLastStatus() == rhi::HostedConsumerStatus::Refused);
    hosted_test::ProducerStatistics statistics;
    session.Close(statistics);
}
