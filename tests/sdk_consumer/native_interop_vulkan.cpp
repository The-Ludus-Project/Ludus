#include "native_interop_fixture.h"
#include "native_sample.h"

#include <ludus/foundation/base/core.h>

#include <cstring>
#include <new>

#define VK_NO_PROTOTYPES
#include <dlfcn.h>
#include <vulkan/vulkan.h>

namespace native_fixture
{
namespace
{
using namespace ludus::foundation;
#define NATIVE_DEVICE_API(X)                                                                                           \
    X(vkDestroyDevice)                                                                                                 \
    X(vkGetDeviceQueue)                                                                                                \
    X(vkDeviceWaitIdle)                                                                                                \
    X(vkCreateImage)                                                                                                   \
    X(vkDestroyImage)                                                                                                  \
    X(vkGetImageMemoryRequirements)                                                                                    \
    X(vkAllocateMemory)                                                                                                \
    X(vkFreeMemory)                                                                                                    \
    X(vkBindImageMemory)                                                                                               \
    X(vkCreateImageView)                                                                                               \
    X(vkDestroyImageView)                                                                                              \
    X(vkCreateBuffer)                                                                                                  \
    X(vkDestroyBuffer)                                                                                                 \
    X(vkGetBufferMemoryRequirements)                                                                                   \
    X(vkBindBufferMemory)                                                                                              \
    X(vkMapMemory)                                                                                                     \
    X(vkUnmapMemory)                                                                                                   \
    X(vkCreateShaderModule)                                                                                            \
    X(vkDestroyShaderModule)                                                                                           \
    X(vkCreateSampler)                                                                                                 \
    X(vkDestroySampler)                                                                                                \
    X(vkCreateDescriptorSetLayout)                                                                                     \
    X(vkDestroyDescriptorSetLayout)                                                                                    \
    X(vkCreateDescriptorPool)                                                                                          \
    X(vkDestroyDescriptorPool)                                                                                         \
    X(vkAllocateDescriptorSets)                                                                                        \
    X(vkUpdateDescriptorSets)                                                                                          \
    X(vkCreateRenderPass)                                                                                              \
    X(vkDestroyRenderPass)                                                                                             \
    X(vkCreateFramebuffer)                                                                                             \
    X(vkDestroyFramebuffer)                                                                                            \
    X(vkCreatePipelineLayout)                                                                                          \
    X(vkDestroyPipelineLayout)                                                                                         \
    X(vkCreateGraphicsPipelines)                                                                                       \
    X(vkDestroyPipeline)                                                                                               \
    X(vkCreateCommandPool)                                                                                             \
    X(vkDestroyCommandPool)                                                                                            \
    X(vkAllocateCommandBuffers)                                                                                        \
    X(vkBeginCommandBuffer)                                                                                            \
    X(vkEndCommandBuffer)                                                                                              \
    X(vkCmdBeginRenderPass)                                                                                            \
    X(vkCmdEndRenderPass)                                                                                              \
    X(vkCmdBindVertexBuffers)                                                                                          \
    X(vkCmdBindPipeline)                                                                                               \
    X(vkCmdBindDescriptorSets)                                                                                         \
    X(vkCmdDraw)                                                                                                       \
    X(vkCmdPipelineBarrier)                                                                                            \
    X(vkCmdCopyImageToBuffer)                                                                                          \
    X(vkCreateFence)                                                                                                   \
    X(vkDestroyFence)                                                                                                  \
    X(vkQueueSubmit)                                                                                                   \
    X(vkCreateSemaphore)                                                                                               \
    X(vkDestroySemaphore)                                                                                              \
    X(vkSignalSemaphore)                                                                                               \
    X(vkWaitForFences)
struct DeviceApi final
{
#define DECLARE_NATIVE_API(name) PFN_##name name = nullptr;
    NATIVE_DEVICE_API(DECLARE_NATIVE_API)
#undef DECLARE_NATIVE_API
};
struct Source final
{
    void* Loader = nullptr;
    PFN_vkGetInstanceProcAddr GetInstance = nullptr;
    PFN_vkDestroyInstance DestroyInstance = nullptr;
    VkInstance Instance = VK_NULL_HANDLE;
    VkPhysicalDevice Physical = VK_NULL_HANDLE;
    VkDevice Device = VK_NULL_HANDLE;
    VkQueue Queue = VK_NULL_HANDLE;
    uint32 Family = 0;
    DeviceApi Api;
    VkPhysicalDeviceMemoryProperties Memory{};
    usize Retained = 0;
    usize Released = 0;
    VkImage Target = VK_NULL_HANDLE;
    VkDeviceMemory TargetMemory = VK_NULL_HANDLE;
    VkDeviceMemory OutputMemory = VK_NULL_HANDLE;
    VkImageView SourceView = VK_NULL_HANDLE;
    VkImageView TargetView = VK_NULL_HANDLE;
    VkBuffer Output = VK_NULL_HANDLE;
    VkBuffer Vertices = VK_NULL_HANDLE;
    VkDeviceMemory VertexMemory = VK_NULL_HANDLE;
    VkSampler Sampler = VK_NULL_HANDLE;
    VkShaderModule Shaders[2]{};
    VkDescriptorSetLayout SetLayout = VK_NULL_HANDLE;
    VkDescriptorPool Descriptors = VK_NULL_HANDLE;
    VkDescriptorSet Set = VK_NULL_HANDLE;
    VkRenderPass Pass = VK_NULL_HANDLE;
    VkFramebuffer Frame = VK_NULL_HANDLE;
    VkPipelineLayout PipelineLayout = VK_NULL_HANDLE;
    VkPipeline Pipeline = VK_NULL_HANDLE;
    VkCommandPool Commands = VK_NULL_HANDLE;
    VkCommandBuffer Command = VK_NULL_HANDLE;
    VkFence Fence = VK_NULL_HANDLE;
    VkSemaphore Gate = VK_NULL_HANDLE;
    uint64 GateValue = 0;
    static bool Retain(void* context) noexcept
    {
        ++static_cast<Source*>(context)->Retained;
        return true;
    }
    static void Release(void* context) noexcept
    {
        ++static_cast<Source*>(context)->Released;
    }
};
#define NATIVE_VK(expression)                                                                                          \
    do                                                                                                                 \
    {                                                                                                                  \
        if ((expression) != VK_SUCCESS)                                                                                \
            return false;                                                                                              \
    } while (false)
bool Allocate(Source& source,
              const VkMemoryRequirements& requirements,
              VkMemoryPropertyFlags flags,
              VkDeviceMemory& output) noexcept
{
    for (uint32 i = 0; i < source.Memory.memoryTypeCount; ++i)
    {
        if ((requirements.memoryTypeBits & (1U << i)) != 0 &&
            (source.Memory.memoryTypes[i].propertyFlags & flags) == flags)
        {
            VkMemoryAllocateInfo allocate
            {
                .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
                .allocationSize = requirements.size,
                .memoryTypeIndex = i,
            };
            return source.Api.vkAllocateMemory(source.Device, &allocate, nullptr, &output) == VK_SUCCESS;
        }
    }
    return false;
}
bool SignalGate(Source& source, uint64 value) noexcept
{
    if (source.Gate == VK_NULL_HANDLE || source.GateValue >= value)
    {
        return true;
    }
    VkSemaphoreSignalInfo signal
    {
        .sType = VK_STRUCTURE_TYPE_SEMAPHORE_SIGNAL_INFO,
        .semaphore = source.Gate,
        .value = value,
    };
    const auto result = source.Api.vkSignalSemaphore(source.Device, &signal);
    if (result != VK_SUCCESS && result != VK_ERROR_DEVICE_LOST)
    {
        return false;
    }
    source.GateValue = value;
    return true;
}
void RetireSample(Source& source) noexcept
{
    if (source.Device == VK_NULL_HANDLE)
    {
        return;
    }
    auto& api = source.Api;
    api.vkDestroyCommandPool(source.Device, source.Commands, nullptr);
    api.vkDestroyFence(source.Device, source.Fence, nullptr);
    api.vkDestroyPipeline(source.Device, source.Pipeline, nullptr);
    api.vkDestroyPipelineLayout(source.Device, source.PipelineLayout, nullptr);
    api.vkDestroyFramebuffer(source.Device, source.Frame, nullptr);
    api.vkDestroyRenderPass(source.Device, source.Pass, nullptr);
    api.vkDestroyDescriptorPool(source.Device, source.Descriptors, nullptr);
    api.vkDestroyDescriptorSetLayout(source.Device, source.SetLayout, nullptr);
    for (auto shader : source.Shaders)
    {
        api.vkDestroyShaderModule(source.Device, shader, nullptr);
    }
    api.vkDestroySampler(source.Device, source.Sampler, nullptr);
    api.vkDestroyImageView(source.Device, source.SourceView, nullptr);
    api.vkDestroyImageView(source.Device, source.TargetView, nullptr);
    api.vkDestroyImage(source.Device, source.Target, nullptr);
    api.vkFreeMemory(source.Device, source.TargetMemory, nullptr);
    api.vkDestroyBuffer(source.Device, source.Output, nullptr);
    api.vkFreeMemory(source.Device, source.OutputMemory, nullptr);
    api.vkDestroyBuffer(source.Device, source.Vertices, nullptr);
    api.vkFreeMemory(source.Device, source.VertexMemory, nullptr);
    source.Commands = VK_NULL_HANDLE;
    source.Fence = VK_NULL_HANDLE;
    source.Pipeline = VK_NULL_HANDLE;
    source.PipelineLayout = VK_NULL_HANDLE;
    source.Frame = VK_NULL_HANDLE;
    source.Pass = VK_NULL_HANDLE;
    source.Descriptors = VK_NULL_HANDLE;
    source.SetLayout = VK_NULL_HANDLE;
    for (auto& shader : source.Shaders)
    {
        shader = VK_NULL_HANDLE;
    }
    source.Sampler = VK_NULL_HANDLE;
    source.SourceView = source.TargetView = VK_NULL_HANDLE;
    source.Target = VK_NULL_HANDLE;
    source.TargetMemory = VK_NULL_HANDLE;
    source.Output = VK_NULL_HANDLE;
    source.OutputMemory = VK_NULL_HANDLE;
    source.Vertices = VK_NULL_HANDLE;
    source.VertexMemory = VK_NULL_HANDLE;
}
} // namespace
bool CreateForeign(Foreign& fixture) noexcept
{
    auto* source = new (std::nothrow) Source;
    if (source == nullptr)
    {
        return false;
    }
    fixture.Context = source;
    // An independent loader reference and function table survive the RHI's own
    // loader teardown. This consumer uses only public native and installed SDK APIs.
    source->Loader = dlopen("libvulkan.so.1", RTLD_NOW | RTLD_LOCAL);
    if (source->Loader == nullptr)
    {
        return false;
    }
    source->GetInstance = reinterpret_cast<PFN_vkGetInstanceProcAddr>(dlsym(source->Loader, "vkGetInstanceProcAddr"));
    if (source->GetInstance == nullptr)
    {
        return false;
    }
    const auto createInstance =
        reinterpret_cast<PFN_vkCreateInstance>(source->GetInstance(nullptr, "vkCreateInstance"));
    VkApplicationInfo application
    {
        .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
        .apiVersion = VK_API_VERSION_1_2,
    };
    VkInstanceCreateInfo instance
    {
        .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
        .pApplicationInfo = &application,
    };
    NATIVE_VK(createInstance(&instance, nullptr, &source->Instance));
    source->DestroyInstance =
        reinterpret_cast<PFN_vkDestroyInstance>(source->GetInstance(source->Instance, "vkDestroyInstance"));
    const auto enumerate = reinterpret_cast<PFN_vkEnumeratePhysicalDevices>(
        source->GetInstance(source->Instance, "vkEnumeratePhysicalDevices"));
    const auto families = reinterpret_cast<PFN_vkGetPhysicalDeviceQueueFamilyProperties>(
        source->GetInstance(source->Instance, "vkGetPhysicalDeviceQueueFamilyProperties"));
    const auto memory = reinterpret_cast<PFN_vkGetPhysicalDeviceMemoryProperties>(
        source->GetInstance(source->Instance, "vkGetPhysicalDeviceMemoryProperties"));
    const auto createDevice =
        reinterpret_cast<PFN_vkCreateDevice>(source->GetInstance(source->Instance, "vkCreateDevice"));
    const auto getDevice =
        reinterpret_cast<PFN_vkGetDeviceProcAddr>(source->GetInstance(source->Instance, "vkGetDeviceProcAddr"));
    uint32 count = 0;
    NATIVE_VK(enumerate(source->Instance, &count, nullptr));
    if (count == 0 || count > 16)
    {
        return false;
    }
    VkPhysicalDevice devices[16]{};
    NATIVE_VK(enumerate(source->Instance, &count, devices));
    for (uint32 d = 0; d < count && source->Physical == VK_NULL_HANDLE; ++d)
    {
        uint32 n = 0;
        families(devices[d], &n, nullptr);
        if (n > 32)
        {
            continue;
        }
        VkQueueFamilyProperties queues[32]{};
        families(devices[d], &n, queues);
        for (uint32 q = 0; q < n; ++q)
        {
            if ((queues[q].queueFlags & VK_QUEUE_GRAPHICS_BIT) != 0 && queues[q].queueCount != 0)
            {
                source->Physical = devices[d];
                source->Family = q;
                break;
            }
        }
    }
    if (source->Physical == VK_NULL_HANDLE)
    {
        return false;
    }
    float32 priority = 1;
    VkDeviceQueueCreateInfo queue
    {
        .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
        .queueFamilyIndex = source->Family,
        .queueCount = 1,
        .pQueuePriorities = &priority,
    };
    VkDeviceCreateInfo device
    {
        .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
        .queueCreateInfoCount = 1,
        .pQueueCreateInfos = &queue,
    };
    const auto getFeatures = reinterpret_cast<PFN_vkGetPhysicalDeviceFeatures2>(
        source->GetInstance(source->Instance, "vkGetPhysicalDeviceFeatures2"));
    const auto getProperties = reinterpret_cast<PFN_vkGetPhysicalDeviceProperties>(
        source->GetInstance(source->Instance, "vkGetPhysicalDeviceProperties"));
    VkPhysicalDeviceProperties properties{};
    getProperties(source->Physical, &properties);
    if (properties.apiVersion < VK_API_VERSION_1_2)
    {
        return false;
    }
    VkPhysicalDeviceTimelineSemaphoreFeatures timeline
    {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_FEATURES,
    };
    VkPhysicalDeviceFeatures2 features
    {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2,
        .pNext = &timeline,
    };
    getFeatures(source->Physical, &features);
    if (timeline.timelineSemaphore == 0)
    {
        return false;
    }
    device.pNext = &timeline;
    NATIVE_VK(createDevice(source->Physical, &device, nullptr, &source->Device));
#define LOAD_NATIVE_API(name) source->Api.name = reinterpret_cast<PFN_##name>(getDevice(source->Device, #name));
    NATIVE_DEVICE_API(LOAD_NATIVE_API)
#undef LOAD_NATIVE_API
    source->Api.vkGetDeviceQueue(source->Device, source->Family, 0, &source->Queue);
    memory(source->Physical, &source->Memory);
    VkSemaphoreTypeCreateInfo type
    {
        .sType = VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO,
        .semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE,
    };
    VkSemaphoreCreateInfo semaphore
    {
        .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO,
        .pNext = &type,
    };
    NATIVE_VK(source->Api.vkCreateSemaphore(source->Device, &semaphore, nullptr, &source->Gate));
    // Thanks to Khronos, "Timeline semaphore", Vulkan Samples, host signalling
    // and avoiding vkDeviceWaitIdle deadlocks. A legal wait-before-signal gate
    // proves that RHI admission returns without draining foreign queue work.
    // https://docs.vulkan.org/samples/latest/samples/extensions/timeline_semaphore/README.html
    const uint64 value = 1;
    const VkPipelineStageFlags stage = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
    VkTimelineSemaphoreSubmitInfo gate
    {
        .sType = VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO,
        .waitSemaphoreValueCount = 1,
        .pWaitSemaphoreValues = &value,
    };
    VkSubmitInfo wait
    {
        .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
        .pNext = &gate,
        .waitSemaphoreCount = 1,
        .pWaitSemaphores = &source->Gate,
        .pWaitDstStageMask = &stage,
    };
    NATIVE_VK(source->Api.vkQueueSubmit(source->Queue, 1, &wait, VK_NULL_HANDLE));
    fixture.Description =
    {
        .Identity =
        {
            .Api = ludus::graphics::rhi::Backend::Vulkan,
            .Instance = ludus::graphics::rhi::NativeInstance{source->Instance},
            .PhysicalDevice = ludus::graphics::rhi::NativePhysicalDevice{source->Physical},
            .Device = ludus::graphics::rhi::NativeLogicalDevice{source->Device},
            .CommandQueue = ludus::graphics::rhi::NativeCommandQueue{source->Queue},
            .QueueFamily = source->Family,
        },
        .Lifetime = { .Context = source, .Retain = Source::Retain, .Release = Source::Release },
        .InstanceApiVersion = VK_API_VERSION_1_2,
        .SerializedOwnerQueue = true,
    };
    return true;
}
bool AllowProducer(Foreign& fixture) noexcept
{
    return SignalGate(*static_cast<Source*>(fixture.Context), 1);
}
bool RetireConsumers(Foreign& fixture) noexcept
{
    auto* source = static_cast<Source*>(fixture.Context);
    if (source != nullptr && source->Device != VK_NULL_HANDLE)
    {
        if (SignalGate(*source, 2) == false)
        {
            return false;
        }
        const auto result = source->Api.vkDeviceWaitIdle(source->Device);
        if (result != VK_SUCCESS && result != VK_ERROR_DEVICE_LOST)
        {
            return false;
        }
        RetireSample(*source);
    }
    return true;
}
void DestroyForeign(Foreign& fixture) noexcept
{
    auto* source = static_cast<Source*>(fixture.Context);
    if (source == nullptr)
    {
        return;
    }
    if (source->Device != VK_NULL_HANDLE)
    {
        (void)source->Api.vkDeviceWaitIdle(source->Device);
        RetireSample(*source);
        source->Api.vkDestroySemaphore(source->Device, source->Gate, nullptr);
        source->Api.vkDestroyDevice(source->Device, nullptr);
    }
    if (source->Instance != VK_NULL_HANDLE)
    {
        source->DestroyInstance(source->Instance, nullptr);
    }
    if (source->Loader != nullptr)
    {
        dlclose(source->Loader);
    }
    delete source;
    fixture = {};
}
bool SubmitSample(Foreign& fixture, const ludus::graphics::rhi::NativeTextureInfo& texture) noexcept
{
    auto& source = *static_cast<Source*>(fixture.Context);
    auto& api = source.Api;
    const auto imageBits = texture.VulkanImage.GetBits();
#if VK_USE_64_BIT_PTR_DEFINES
    VkImage shared = VK_NULL_HANDLE;
    std::memcpy(reinterpret_cast<void*>(&shared), &imageBits, sizeof(imageBits));
#else
    const auto shared = static_cast<VkImage>(imageBits);
#endif
    VkImageViewCreateInfo view
    {
        .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
        .image = shared,
        .viewType = VK_IMAGE_VIEW_TYPE_2D,
        .format = VK_FORMAT_R8G8B8A8_UNORM,
        .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
    };
    NATIVE_VK(api.vkCreateImageView(source.Device, &view, nullptr, &source.SourceView));
    VkImageCreateInfo image
    {
        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .imageType = VK_IMAGE_TYPE_2D,
        .format = VK_FORMAT_R8G8B8A8_UNORM,
        .extent = {4, 4, 1},
        .mipLevels = 1,
        .arrayLayers = 1,
        .samples = VK_SAMPLE_COUNT_1_BIT,
        .tiling = VK_IMAGE_TILING_OPTIMAL,
        .usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
    };
    NATIVE_VK(api.vkCreateImage(source.Device, &image, nullptr, &source.Target));
    VkMemoryRequirements requirements{};
    api.vkGetImageMemoryRequirements(source.Device, source.Target, &requirements);
    if (Allocate(source, requirements, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, source.TargetMemory) == false)
    {
        return false;
    }
    NATIVE_VK(api.vkBindImageMemory(source.Device, source.Target, source.TargetMemory, 0));
    view.image = source.Target;
    NATIVE_VK(api.vkCreateImageView(source.Device, &view, nullptr, &source.TargetView));
    VkBufferCreateInfo buffer
    {
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size = 64,
        .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT,
    };
    NATIVE_VK(api.vkCreateBuffer(source.Device, &buffer, nullptr, &source.Output));
    api.vkGetBufferMemoryRequirements(source.Device, source.Output, &requirements);
    if (Allocate(source,
                 requirements,
                 VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                 source.OutputMemory) == false)
    {
        return false;
    }
    NATIVE_VK(api.vkBindBufferMemory(source.Device, source.Output, source.OutputMemory, 0));
    buffer.size = sizeof(TRIANGLE_POSITIONS);
    buffer.usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;
    NATIVE_VK(api.vkCreateBuffer(source.Device, &buffer, nullptr, &source.Vertices));
    api.vkGetBufferMemoryRequirements(source.Device, source.Vertices, &requirements);
    if (Allocate(source,
                 requirements,
                 VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                 source.VertexMemory) == false)
    {
        return false;
    }
    NATIVE_VK(api.vkBindBufferMemory(source.Device, source.Vertices, source.VertexMemory, 0));
    void* mappedVertices = nullptr;
    NATIVE_VK(api.vkMapMemory(source.Device, source.VertexMemory, 0, sizeof(TRIANGLE_POSITIONS), 0, &mappedVertices));
    std::memcpy(mappedVertices, TRIANGLE_POSITIONS, sizeof(TRIANGLE_POSITIONS));
    api.vkUnmapMemory(source.Device, source.VertexMemory);
    VkSamplerCreateInfo sampler
    {
        .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
        .magFilter = VK_FILTER_NEAREST,
        .minFilter = VK_FILTER_NEAREST,
        .maxLod = 0,
    };
    NATIVE_VK(api.vkCreateSampler(source.Device, &sampler, nullptr, &source.Sampler));
    const VkDescriptorSetLayoutBinding bindings[2]{
        {0, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
        {1, VK_DESCRIPTOR_TYPE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr}};
    VkDescriptorSetLayoutCreateInfo layout
    {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
        .bindingCount = 2,
        .pBindings = bindings,
    };
    NATIVE_VK(api.vkCreateDescriptorSetLayout(source.Device, &layout, nullptr, &source.SetLayout));
    const VkDescriptorPoolSize sizes[2]{{VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1}, {VK_DESCRIPTOR_TYPE_SAMPLER, 1}};
    VkDescriptorPoolCreateInfo pool
    {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
        .maxSets = 1,
        .poolSizeCount = 2,
        .pPoolSizes = sizes,
    };
    NATIVE_VK(api.vkCreateDescriptorPool(source.Device, &pool, nullptr, &source.Descriptors));
    VkDescriptorSetAllocateInfo set
    {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
        .descriptorPool = source.Descriptors,
        .descriptorSetCount = 1,
        .pSetLayouts = &source.SetLayout,
    };
    NATIVE_VK(api.vkAllocateDescriptorSets(source.Device, &set, &source.Set));
    const VkDescriptorImageInfo descriptors[2]{
        {VK_NULL_HANDLE, source.SourceView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
        {source.Sampler, VK_NULL_HANDLE, VK_IMAGE_LAYOUT_UNDEFINED}};
    VkWriteDescriptorSet writes[2]{};
    for (uint32 i = 0; i < 2; ++i)
    {
        writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[i].dstSet = source.Set;
        writes[i].dstBinding = i;
        writes[i].descriptorCount = 1;
        writes[i].descriptorType = bindings[i].descriptorType;
        writes[i].pImageInfo = &descriptors[i];
    }
    api.vkUpdateDescriptorSets(source.Device, 2, writes, 0, nullptr);
    VkAttachmentDescription attachment
    {
        .format = VK_FORMAT_R8G8B8A8_UNORM,
        .samples = VK_SAMPLE_COUNT_1_BIT,
        .loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
        .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
        .finalLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
    };
    const VkAttachmentReference color{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkSubpassDescription subpass
    {
        .pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS,
        .colorAttachmentCount = 1,
        .pColorAttachments = &color,
    };
    const VkSubpassDependency dependency{0,
                                         VK_SUBPASS_EXTERNAL,
                                         VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                                         VK_PIPELINE_STAGE_TRANSFER_BIT,
                                         VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
                                         VK_ACCESS_TRANSFER_READ_BIT,
                                         0};
    VkRenderPassCreateInfo pass
    {
        .sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,
        .attachmentCount = 1,
        .pAttachments = &attachment,
        .subpassCount = 1,
        .pSubpasses = &subpass,
        .dependencyCount = 1,
        .pDependencies = &dependency,
    };
    NATIVE_VK(api.vkCreateRenderPass(source.Device, &pass, nullptr, &source.Pass));
    VkFramebufferCreateInfo frame
    {
        .sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
        .renderPass = source.Pass,
        .attachmentCount = 1,
        .pAttachments = &source.TargetView,
    };
    frame.width = frame.height = 4;
    frame.layers = 1;
    NATIVE_VK(api.vkCreateFramebuffer(source.Device, &frame, nullptr, &source.Frame));
    const auto vertex = ludus::shaders::native_sample::Vertex().Artifact;
    const auto fragment = ludus::shaders::native_sample::Fragment().Artifact;
    const ludus::graphics::rhi::ShaderDescription shaders[2]{vertex, fragment};
    VkPipelineShaderStageCreateInfo stages[2]{};
    for (usize i = 0; i < 2; ++i)
    {
        VkShaderModuleCreateInfo module
        {
            .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
            .codeSize = shaders[i].Spirv.size_bytes(),
            .pCode = shaders[i].Spirv.data(),
        };
        NATIVE_VK(api.vkCreateShaderModule(source.Device, &module, nullptr, &source.Shaders[i]));
        stages[i] =
        {
            .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
            .stage = i == 0 ? VK_SHADER_STAGE_VERTEX_BIT : VK_SHADER_STAGE_FRAGMENT_BIT,
            .module = source.Shaders[i],
            .pName = shaders[i].SpirvEntry.data(),
        };
    }
    VkPipelineLayoutCreateInfo pipelineLayout
    {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
        .setLayoutCount = 1,
        .pSetLayouts = &source.SetLayout,
    };
    NATIVE_VK(api.vkCreatePipelineLayout(source.Device, &pipelineLayout, nullptr, &source.PipelineLayout));
    VkPipelineVertexInputStateCreateInfo inputs
    {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
    };
    const VkVertexInputBindingDescription vertexBinding{0, VERTEX_STRIDE, VK_VERTEX_INPUT_RATE_VERTEX};
    const VkVertexInputAttributeDescription attribute{0, 0, VK_FORMAT_R32G32B32_SFLOAT, 0};
    inputs.vertexBindingDescriptionCount = 1;
    inputs.pVertexBindingDescriptions = &vertexBinding;
    inputs.vertexAttributeDescriptionCount = 1;
    inputs.pVertexAttributeDescriptions = &attribute;
    VkPipelineInputAssemblyStateCreateInfo assembly
    {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
        .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,
    };
    const VkViewport viewport{0, 0, 4, 4, 0, 1};
    const VkRect2D scissor{{0, 0}, {4, 4}};
    VkPipelineViewportStateCreateInfo viewports
    {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
        .viewportCount = 1,
        .pViewports = &viewport,
        .scissorCount = 1,
        .pScissors = &scissor,
    };
    VkPipelineRasterizationStateCreateInfo raster
    {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
        .polygonMode = VK_POLYGON_MODE_FILL,
        .lineWidth = 1,
    };
    VkPipelineMultisampleStateCreateInfo samples
    {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
        .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT,
    };
    VkPipelineColorBlendAttachmentState blend
    {
        .colorWriteMask = 15,
    };
    VkPipelineColorBlendStateCreateInfo blends
    {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
        .attachmentCount = 1,
        .pAttachments = &blend,
    };
    VkGraphicsPipelineCreateInfo graphics
    {
        .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
        .stageCount = 2,
        .pStages = stages,
        .pVertexInputState = &inputs,
        .pInputAssemblyState = &assembly,
        .pViewportState = &viewports,
        .pRasterizationState = &raster,
        .pMultisampleState = &samples,
        .pColorBlendState = &blends,
        .layout = source.PipelineLayout,
        .renderPass = source.Pass,
    };
    NATIVE_VK(api.vkCreateGraphicsPipelines(source.Device, VK_NULL_HANDLE, 1, &graphics, nullptr, &source.Pipeline));
    VkCommandPoolCreateInfo commands
    {
        .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
        .queueFamilyIndex = source.Family,
    };
    NATIVE_VK(api.vkCreateCommandPool(source.Device, &commands, nullptr, &source.Commands));
    VkCommandBufferAllocateInfo allocate
    {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .commandPool = source.Commands,
        .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
        .commandBufferCount = 1,
    };
    NATIVE_VK(api.vkAllocateCommandBuffers(source.Device, &allocate, &source.Command));
    VkFenceCreateInfo fence
    {
        .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO,
    };
    NATIVE_VK(api.vkCreateFence(source.Device, &fence, nullptr, &source.Fence));
    VkCommandBufferBeginInfo begin
    {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
    };
    NATIVE_VK(api.vkBeginCommandBuffer(source.Command, &begin));
    VkRenderPassBeginInfo render
    {
        .sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
        .renderPass = source.Pass,
        .framebuffer = source.Frame,
        .renderArea = {{0, 0}, {4, 4}},
    };
    api.vkCmdBeginRenderPass(source.Command, &render, VK_SUBPASS_CONTENTS_INLINE);
    api.vkCmdBindPipeline(source.Command, VK_PIPELINE_BIND_POINT_GRAPHICS, source.Pipeline);
    api.vkCmdBindDescriptorSets(source.Command,
                                VK_PIPELINE_BIND_POINT_GRAPHICS,
                                source.PipelineLayout,
                                0,
                                1,
                                &source.Set,
                                0,
                                nullptr);
    const VkDeviceSize vertexOffset = 0;
    api.vkCmdBindVertexBuffers(source.Command, 0, 1, &source.Vertices, &vertexOffset);
    api.vkCmdDraw(source.Command, 3, 1, 0, 0);
    api.vkCmdEndRenderPass(source.Command);
    const VkBufferImageCopy copy{0, 0, 0, {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, {0, 0, 0}, {4, 4, 1}};
    api.vkCmdCopyImageToBuffer(source.Command,
                               source.Target,
                               VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                               source.Output,
                               1,
                               &copy);
    VkBufferMemoryBarrier host
    {
        .sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
        .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
        .dstAccessMask = VK_ACCESS_HOST_READ_BIT,
    };
    host.srcQueueFamilyIndex = host.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    host.buffer = source.Output;
    host.size = VK_WHOLE_SIZE;
    api.vkCmdPipelineBarrier(source.Command,
                             VK_PIPELINE_STAGE_TRANSFER_BIT,
                             VK_PIPELINE_STAGE_HOST_BIT,
                             0,
                             0,
                             nullptr,
                             1,
                             &host,
                             0,
                             nullptr);
    NATIVE_VK(api.vkEndCommandBuffer(source.Command));
    VkSubmitInfo submit
    {
        .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
        .commandBufferCount = 1,
        .pCommandBuffers = &source.Command,
    };
    const uint64 value = 2;
    const VkPipelineStageFlags stage = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
    VkTimelineSemaphoreSubmitInfo gate
    {
        .sType = VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO,
        .waitSemaphoreValueCount = 1,
        .pWaitSemaphoreValues = &value,
    };
    submit.pNext = &gate;
    submit.waitSemaphoreCount = 1;
    submit.pWaitSemaphores = &source.Gate;
    submit.pWaitDstStageMask = &stage;
    NATIVE_VK(api.vkQueueSubmit(source.Queue, 1, &submit, source.Fence));
    return true;
}
bool ReadSample(Foreign& fixture, ludus::foundation::uint8 (&pixels)[64]) noexcept
{
    auto& source = *static_cast<Source*>(fixture.Context);
    if (SignalGate(source, 2) == false)
    {
        return false;
    }
    NATIVE_VK(source.Api.vkWaitForFences(source.Device, 1, &source.Fence, VK_TRUE, 5000000000ULL));
    void* mapped = nullptr;
    NATIVE_VK(source.Api.vkMapMemory(source.Device, source.OutputMemory, 0, 64, 0, &mapped));
    std::memcpy(pixels, mapped, sizeof(pixels));
    source.Api.vkUnmapMemory(source.Device, source.OutputMemory);
    // GPU completion precedes destruction of the foreign image view and descriptor.
    RetireSample(source);
    return true;
}
ludus::foundation::usize Retains(const Foreign& fixture) noexcept
{
    return static_cast<const Source*>(fixture.Context)->Retained;
}
ludus::foundation::usize Releases(const Foreign& fixture) noexcept
{
    return static_cast<const Source*>(fixture.Context)->Released;
}
} // namespace native_fixture
