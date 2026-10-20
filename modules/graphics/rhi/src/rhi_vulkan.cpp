#include "internal/backend.h"
#include "internal/gpu_scope_backend.h"
#include "internal/hosted_adapter.h"
#include "internal/hosted_vulkan.h"
#include "internal/lifecycle.h"
#include "internal/measurement.h"
#include "internal/native_interop.h"
#include "internal/native_preparation.h"
#include "internal/raster.h"
#include "internal/ray_backend.h"
#include "internal/resources.h"
#include "internal/vma.h"

#include <ludus/foundation/containers/static_array.hpp>
#include <ludus/foundation/logging/log_format.hpp>
#include <ludus/foundation/math/scalar.hpp>
#include <ludus/graphics/rhi/render.h>
#include <ludus/graphics/rhi/rhi.h>
#include <ludus/platform/hosted_surface.h>

#include <cstring>
#include <initializer_list>
#include <span>

#include <volk.h>

namespace ludus::graphics::rhi::backend
{
void RasterFrameExtent() noexcept;
void WaitRasterSubmissions() noexcept;
void WaitLifetimeSubmissions() noexcept;
void TimingBegin() noexcept;
void TimingEnd() noexcept;
void TimingSubmitted() noexcept;
void BeginPassTimingFrame() noexcept;
void SubmitPassTimings() noexcept;
void BeginPhysicalPassTiming(GpuPassKind kind) noexcept;
void EndPhysicalPassTiming() noexcept;
namespace
{
using namespace foundation;
constexpr logging::LogCategory LOG_RHI{"RHI"};
constexpr usize FRAMES = 2;
constexpr usize IMAGES = 8;
/// Adapters one instance may enumerate; a larger count is rejected, never truncated.
constexpr uint32 MAX_PHYSICAL_DEVICES = 32;
void RasterVkDependencies(bool surface, VkSubpassDependency (&dependencies)[2]) noexcept;
struct Frame final
{
    VkCommandBuffer Command = VK_NULL_HANDLE;
    VkFence Fence = VK_NULL_HANDLE;
    VkSemaphore Acquired = VK_NULL_HANDLE;
    bool Submitted = false;
};
struct Shader final
{
    VkShaderModule Module = VK_NULL_HANDLE;
    char Entry[64]{};
    ShaderStage Stage = ShaderStage::Vertex;
};
struct Uniform final
{
    VkBuffer Buffers[FRAMES]{};
    VkDeviceMemory Memory[FRAMES]{};
    void* Mapped[FRAMES]{};
    uint8 Bytes[internal::UNIFORM_CAPACITY]{};
    usize Size = 0;
};
struct Pipeline final
{
    VkDescriptorSetLayout Bindings = VK_NULL_HANDLE;
    VkDescriptorPool Pool = VK_NULL_HANDLE;
    VkDescriptorSet Sets[FRAMES]{};
    VkPipelineLayout Layout = VK_NULL_HANDLE;
    VkPipeline Object = VK_NULL_HANDLE;
    usize UniformSlot = 0;
};
VkInstance gInstance = VK_NULL_HANDLE;
VkPhysicalDevice gPhysical = VK_NULL_HANDLE;
VkDevice gDevice = VK_NULL_HANDLE;
VkQueue gQueue = VK_NULL_HANDLE;
uint32 gFamily = 0;
bool gQueueCompute = false;
bool gRayEnabled = false, gRayApiAvailable = false;
bool gHasUniformTextureIndexing = false;
bool gHasBindlessTextureIndexing = false;
bool gHasRuntimeTextureArrays = false;
bool gHasBindingApi = false;
VkPhysicalDeviceAccelerationStructurePropertiesKHR gRayProperties{};
VkSurfaceKHR gSurface = VK_NULL_HANDLE;
VkSwapchainKHR gSwapchain = VK_NULL_HANDLE;
VkCommandPool gCommands = VK_NULL_HANDLE;
VkRenderPass gPass = VK_NULL_HANDLE;
VkImage gImages[IMAGES]{};
VkImageView gViews[IMAGES]{};
VkFramebuffer gTargets[IMAGES]{};
VkSemaphore gFinished[IMAGES]{};
VkFence gPresentFences[IMAGES]{};
bool gPresented[IMAGES]{};
bool gAcquiredWait = false;
bool gOwnedImage = false;
bool gInstanceMaintenance = false;
VkDeviceMemory gHeadlessMemory = VK_NULL_HANDLE;
VkImage gDepthImage = VK_NULL_HANDLE;
VkImageView gDepthView = VK_NULL_HANDLE;
VkDeviceMemory gDepthMemory = VK_NULL_HANDLE;
VkFormat gDepthFormat = VK_FORMAT_D32_SFLOAT;
uint32 gImageCount = 0;
uint32 gImage = 0;
usize gFrame = 0;
Frame gFrames[FRAMES];
Shader gShaders[internal::RESOURCE_CAPACITY];
Uniform gUniforms[internal::RESOURCE_CAPACITY];
Pipeline gPipelines[internal::RESOURCE_CAPACITY];
VkFormat gFormat = VK_FORMAT_UNDEFINED;
VkExtent2D gExtent{};
VkExtent2D gRequestedExtent{};
FrameTarget gTarget;
uint32 gSession = 0;
uint32 gMaxDimension = 0;
uint32 gMaxUniformSize = 0;
WindowInfo gWindow;
bool gHeadless = false;
/// Renders a WindowSystem::Hosted window's frames into exported images. Hosted
/// targets follow the headless layouts (gHeadless is true).
HostedProducerVulkan gHostedProducer;
/// Framebuffer of the acquired hosted image during a hosted frame.
VkFramebuffer gHostedFramebuffer = VK_NULL_HANDLE;
/// Incremented whenever CreateTargets() recreates the depth attachment.
uint64 gDepthEpoch = 0;
bool gBorrowedDevice = false;
bool hasDebugLabels = false;
uint32 gNativeInstanceVersion = VK_API_VERSION_1_1;
bool gEncoding = false;
bool gResize = true;
VkResult gLastResult = VK_SUCCESS;

bool Check(VkResult result) noexcept
{
    gLastResult = result;
    if (result != VK_SUCCESS)
    {
        LUDUS_LOG_ERROR(LOG_RHI, "Vulkan operation failed: {}", static_cast<int32>(result));
        return false;
    }
    return true;
}
/// True when the connected window is WindowSystem::Hosted.
bool IsHostedWindow() noexcept
{
    return gWindow.System == platform::WindowSystem::Hosted;
}
/// Framebuffer of the open frame's surface image.
VkFramebuffer SurfaceFramebuffer() noexcept
{
    return gHostedProducer.IsConnected() == true ? gHostedFramebuffer : gTargets[gImage];
}
bool HasDeviceExtension(VkPhysicalDevice physical, const char* name) noexcept
{
    constexpr uint32 MAX_DEVICE_EXTENSIONS = 512;
    uint32 count = 0;
    if (vkEnumerateDeviceExtensionProperties(physical, nullptr, &count, nullptr) != VK_SUCCESS ||
        count > MAX_DEVICE_EXTENSIONS)
    {
        return false;
    }
    foundation::StaticArray<VkExtensionProperties, MAX_DEVICE_EXTENSIONS> available{};
    if (vkEnumerateDeviceExtensionProperties(physical, nullptr, &count, available.GetData()) != VK_SUCCESS)
    {
        return false;
    }
    for (uint32 i = 0; i < count; ++i)
    {
        if (std::strcmp(available[i].extensionName, name) == 0)
        {
            return true;
        }
    }
    return false;
}
void WaitIdle() noexcept
{
    if (gDevice != VK_NULL_HANDLE)
    {
        if (gBorrowedDevice || internal::NativeInteropPinned())
        {
            // A foreign queue can wait for a later host submission. Drain only
            // accepted RHI work, never the source device's unrelated submissions.
            for (auto& frame : gFrames)
            {
                if (frame.Submitted && Check(vkWaitForFences(gDevice, 1, &frame.Fence, VK_TRUE, UINT64_MAX)))
                {
                    frame.Submitted = false;
                }
            }
            WaitRasterSubmissions();
            WaitLifetimeSubmissions();
        }
        else
        {
            (void)Check(vkDeviceWaitIdle(gDevice));
        }
    }
}
bool Allocate(const VkMemoryRequirements& requirements, VkMemoryPropertyFlags flags, VkDeviceMemory& memory) noexcept
{
    VkPhysicalDeviceMemoryProperties properties{};
    vkGetPhysicalDeviceMemoryProperties(gPhysical, &properties);
    for (usize i = 0; i < properties.memoryTypeCount; ++i)
    {
        if ((requirements.memoryTypeBits & (1U << i)) != 0 &&
            (properties.memoryTypes[i].propertyFlags & flags) == flags)
        {
            VkMemoryAllocateInfo info{};
            info.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
            info.allocationSize = requirements.size;
            info.memoryTypeIndex = static_cast<uint32>(i);
            return Check(vkAllocateMemory(gDevice, &info, nullptr, &memory));
        }
    }
    LUDUS_LOG_ERROR(LOG_RHI, "No compatible Vulkan memory type");
    return false;
}
void WaitPresents() noexcept
{
    if (gDevice == VK_NULL_HANDLE)
    {
        return;
    }
    for (usize i = 0; i < IMAGES; ++i)
    {
        if (gPresented[i])
        {
            (void)Check(vkWaitForFences(gDevice, 1, &gPresentFences[i], VK_TRUE, UINT64_MAX));
            gPresented[i] = false;
        }
    }
}
void RetireAcquiredImage() noexcept
{
    if (gOwnedImage == false || gDevice == VK_NULL_HANDLE)
    {
        return;
    }
    if (gAcquiredWait)
    {
        // Abort may occur after acquisition but before submission. Consume the
        // asynchronous acquisition signal before releasing its semaphore.
        const VkPipelineStageFlags stage = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
        VkSubmitInfo submit{};
        submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        submit.waitSemaphoreCount = 1;
        submit.pWaitSemaphores = &gFrames[gFrame].Acquired;
        submit.pWaitDstStageMask = &stage;
        auto& frame = gFrames[gFrame];
        if (Check(vkResetFences(gDevice, 1, &frame.Fence)))
        {
            frame.Submitted = Check(vkQueueSubmit(gQueue, 1, &submit, frame.Fence));
        }
        gAcquiredWait = false;
    }
    WaitIdle();
    VkReleaseSwapchainImagesInfoEXT release{};
    release.sType = VK_STRUCTURE_TYPE_RELEASE_SWAPCHAIN_IMAGES_INFO_EXT;
    release.swapchain = gSwapchain;
    release.imageIndexCount = 1;
    release.pImageIndices = &gImage;
    (void)Check(vkReleaseSwapchainImagesEXT(gDevice, &release));
    gOwnedImage = false;
}
void ReleaseTargets() noexcept
{
    if (gDevice == VK_NULL_HANDLE)
    {
        return;
    }
    WaitPresents();
    for (usize i = 0; i < IMAGES; ++i)
    {
        vkDestroyFence(gDevice, gPresentFences[i], nullptr);
        gPresentFences[i] = VK_NULL_HANDLE;
        vkDestroyFramebuffer(gDevice, gTargets[i], nullptr);
        gTargets[i] = VK_NULL_HANDLE;
        vkDestroyImageView(gDevice, gViews[i], nullptr);
        gViews[i] = VK_NULL_HANDLE;
        vkDestroySemaphore(gDevice, gFinished[i], nullptr);
        gFinished[i] = VK_NULL_HANDLE;
    }
    vkDestroyImageView(gDevice, gDepthView, nullptr);
    vkDestroyImage(gDevice, gDepthImage, nullptr);
    vkFreeMemory(gDevice, gDepthMemory, nullptr);
    gDepthView = VK_NULL_HANDLE;
    gDepthImage = VK_NULL_HANDLE;
    gDepthMemory = VK_NULL_HANDLE;
    if (gHeadless)
    {
        vkDestroyImage(gDevice, gImages[0], nullptr);
        vkFreeMemory(gDevice, gHeadlessMemory, nullptr);
        gHeadlessMemory = VK_NULL_HANDLE;
    }
    if (gSwapchain != VK_NULL_HANDLE)
    {
        vkDestroySwapchainKHR(gDevice, gSwapchain, nullptr);
    }
    gSwapchain = VK_NULL_HANDLE;
    for (auto& image : gImages)
    {
        image = VK_NULL_HANDLE;
    }
    gImageCount = 0;
}
bool CreatePass() noexcept
{
    if (gPass != VK_NULL_HANDLE)
    {
        return true;
    }
    VkFormatProperties depthProperties{};
    vkGetPhysicalDeviceFormatProperties(gPhysical, VK_FORMAT_D32_SFLOAT, &depthProperties);
    gDepthFormat = (depthProperties.optimalTilingFeatures & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT) != 0
                       ? VK_FORMAT_D32_SFLOAT
                       : VK_FORMAT_D16_UNORM;
    VkAttachmentDescription attachments[2]{
        { .samples = VK_SAMPLE_COUNT_1_BIT },
        { .samples = VK_SAMPLE_COUNT_1_BIT },
    };
    auto& color = attachments[0];
    color.format = gFormat;
    color.samples = VK_SAMPLE_COUNT_1_BIT;
    color.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    color.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    color.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    color.finalLayout = gHeadless ? VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL : VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    auto& depth = attachments[1];
    depth.format = gDepthFormat;
    depth.samples = VK_SAMPLE_COUNT_1_BIT;
    depth.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    depth.storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    depth.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    depth.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    depth.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    depth.finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    VkAttachmentReference depthReference{1, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL};
    VkAttachmentReference reference{};
    reference.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    VkSubpassDescription subpass{};
    subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount = 1;
    subpass.pColorAttachments = &reference;
    subpass.pDepthStencilAttachment = &depthReference;
    VkSubpassDependency dependencies[2]{};
    RasterVkDependencies(true, dependencies);
    VkRenderPassCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    info.attachmentCount = 2;
    info.pAttachments = attachments;
    info.subpassCount = 1;
    info.pSubpasses = &subpass;
    info.dependencyCount = 2;
    info.pDependencies = dependencies;
    return Check(vkCreateRenderPass(gDevice, &info, nullptr, &gPass));
}
bool CreateTargets() noexcept
{
    WaitIdle();
    ReleaseTargets();
    gExtent = { .width = gTarget.Width, .height = gTarget.Height };
    if (IsHostedWindow() == true)
    {
        // The hosted producer owns the color images and their framebuffers; only
        // the shared depth attachment and the surface pass are created here.
        gFormat = HOSTED_IMAGE_FORMAT;
        gImageCount = 0;
    }
    else if (gHeadless)
    {
        gFormat = VK_FORMAT_R8G8B8A8_UNORM;
        gImageCount = 1;
        VkImageCreateInfo info{ .samples = VK_SAMPLE_COUNT_1_BIT };
        info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        info.imageType = VK_IMAGE_TYPE_2D;
        info.format = gFormat;
        info.extent = { .width = gExtent.width, .height = gExtent.height, .depth = 1 };
        info.mipLevels = 1;
        info.arrayLayers = 1;
        info.samples = VK_SAMPLE_COUNT_1_BIT;
        info.tiling = VK_IMAGE_TILING_OPTIMAL;
        info.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
        if (Check(vkCreateImage(gDevice, &info, nullptr, &gImages[0])) == false)
        {
            return false;
        }
        VkMemoryRequirements requirements{};
        vkGetImageMemoryRequirements(gDevice, gImages[0], &requirements);
        if (Allocate(requirements, 0, gHeadlessMemory) == false ||
            Check(vkBindImageMemory(gDevice, gImages[0], gHeadlessMemory, 0)) == false)
        {
            return false;
        }
    }
    else
    {
        VkSurfaceCapabilitiesKHR caps{ .currentTransform = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR };
        if (Check(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(gPhysical, gSurface, &caps)) == false)
        {
            return false;
        }
        if (caps.currentExtent.width != UINT32_MAX)
        {
            gExtent = caps.currentExtent;
        }
        else
        {
            gExtent.width =
                foundation::math::Clamp(gExtent.width, caps.minImageExtent.width, caps.maxImageExtent.width);
            gExtent.height =
                foundation::math::Clamp(gExtent.height, caps.minImageExtent.height, caps.maxImageExtent.height);
        }
        uint32 count = 0;
        if (Check(vkGetPhysicalDeviceSurfaceFormatsKHR(gPhysical, gSurface, &count, nullptr)) == false || count == 0 ||
            count > 64)
        {
            return false;
        }
        VkSurfaceFormatKHR formats[64]{};
        if (Check(vkGetPhysicalDeviceSurfaceFormatsKHR(gPhysical, gSurface, &count, formats)) == false)
        {
            return false;
        }
        VkSurfaceFormatKHR selected{};
        for (usize i = 0; i < count; ++i)
        {
            const auto format = formats[i].format;
            if (formats[i].colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR &&
                (format == VK_FORMAT_UNDEFINED || format == VK_FORMAT_B8G8R8A8_UNORM ||
                 format == VK_FORMAT_R8G8B8A8_UNORM || format == VK_FORMAT_B8G8R8A8_SRGB ||
                 format == VK_FORMAT_R8G8B8A8_SRGB))
            {
                selected = formats[i];
                if (format == VK_FORMAT_UNDEFINED || format == VK_FORMAT_B8G8R8A8_UNORM)
                {
                    selected.format = VK_FORMAT_B8G8R8A8_UNORM;
                    break;
                }
            }
        }
        if (selected.format == VK_FORMAT_UNDEFINED)
        {
            LUDUS_LOG_ERROR(LOG_RHI, "Surface has no supported RGBA8/BGRA8 UNORM or sRGB format");
            return false;
        }
        if (gFormat != VK_FORMAT_UNDEFINED && gFormat != selected.format)
        {
            return false;
        }
        gFormat = selected.format;
        // A minimized surface can negotiate zero pixels. Its format still
        // defines a valid pipeline; defer framebuffer/swapchain creation.
        if (gExtent.width == 0 || gExtent.height == 0)
        {
            return CreatePass();
        }
        uint32 desired = foundation::math::Max(caps.minImageCount, 2U);
        if (caps.maxImageCount != 0)
        {
            desired = foundation::math::Min(desired, caps.maxImageCount);
        }
        if (desired > IMAGES || (caps.supportedUsageFlags & VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT) == 0)
        {
            return false;
        }
        VkSwapchainCreateInfoKHR info
        {
            .preTransform = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR,
            .compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR,
        };
        info.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
        info.surface = gSurface;
        info.minImageCount = desired;
        info.imageFormat = gFormat;
        info.imageColorSpace = selected.colorSpace;
        info.imageExtent = gExtent;
        info.imageArrayLayers = 1;
        info.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
        info.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
        info.preTransform = caps.currentTransform;
        for (auto alpha : {VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR,
                           VK_COMPOSITE_ALPHA_PRE_MULTIPLIED_BIT_KHR,
                           VK_COMPOSITE_ALPHA_POST_MULTIPLIED_BIT_KHR,
                           VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR})
        {
            if ((caps.supportedCompositeAlpha & alpha) != 0)
            {
                info.compositeAlpha = alpha;
                break;
            }
        }
        info.presentMode = VK_PRESENT_MODE_FIFO_KHR;
        info.clipped = VK_TRUE;
        if (Check(vkCreateSwapchainKHR(gDevice, &info, nullptr, &gSwapchain)) == false ||
            Check(vkGetSwapchainImagesKHR(gDevice, gSwapchain, &gImageCount, nullptr)) == false ||
            gImageCount > IMAGES || Check(vkGetSwapchainImagesKHR(gDevice, gSwapchain, &gImageCount, gImages)) == false)
        {
            return false;
        }
    }
    if (CreatePass() == false)
    {
        return false;
    }
    VkImageCreateInfo depthImage{ .samples = VK_SAMPLE_COUNT_1_BIT };
    depthImage.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    depthImage.imageType = VK_IMAGE_TYPE_2D;
    depthImage.format = gDepthFormat;
    depthImage.extent = {gExtent.width, gExtent.height, 1};
    depthImage.mipLevels = 1;
    depthImage.arrayLayers = 1;
    depthImage.samples = VK_SAMPLE_COUNT_1_BIT;
    depthImage.tiling = VK_IMAGE_TILING_OPTIMAL;
    depthImage.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
    ++gDepthEpoch;
    if (Check(vkCreateImage(gDevice, &depthImage, nullptr, &gDepthImage)) == false)
    {
        return false;
    }
    VkMemoryRequirements depthMemory{};
    vkGetImageMemoryRequirements(gDevice, gDepthImage, &depthMemory);
    if (Allocate(depthMemory, 0, gDepthMemory) == false ||
        Check(vkBindImageMemory(gDevice, gDepthImage, gDepthMemory, 0)) == false)
    {
        return false;
    }
    VkImageViewCreateInfo depthView{};
    depthView.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    depthView.image = gDepthImage;
    depthView.viewType = VK_IMAGE_VIEW_TYPE_2D;
    depthView.format = gDepthFormat;
    depthView.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
    depthView.subresourceRange.levelCount = 1;
    depthView.subresourceRange.layerCount = 1;
    if (Check(vkCreateImageView(gDevice, &depthView, nullptr, &gDepthView)) == false)
    {
        return false;
    }
    for (usize i = 0; i < gImageCount; ++i)
    {
        VkImageViewCreateInfo view{};
        view.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        view.image = gImages[i];
        view.viewType = VK_IMAGE_VIEW_TYPE_2D;
        view.format = gFormat;
        view.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        view.subresourceRange.levelCount = 1;
        view.subresourceRange.layerCount = 1;
        if (Check(vkCreateImageView(gDevice, &view, nullptr, &gViews[i])) == false)
        {
            return false;
        }
        VkFramebufferCreateInfo target{};
        target.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
        target.renderPass = gPass;
        const VkImageView attachments[2]{gViews[i], gDepthView};
        target.attachmentCount = 2;
        target.pAttachments = attachments;
        target.width = gExtent.width;
        target.height = gExtent.height;
        target.layers = 1;
        if (Check(vkCreateFramebuffer(gDevice, &target, nullptr, &gTargets[i])) == false)
        {
            return false;
        }
        if (gHeadless == false)
        {
            VkSemaphoreCreateInfo semaphore{};
            semaphore.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
            VkFenceCreateInfo fence{};
            fence.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
            if (Check(vkCreateFence(gDevice, &fence, nullptr, &gPresentFences[i])) == false ||
                Check(vkCreateSemaphore(gDevice, &semaphore, nullptr, &gFinished[i])) == false)
            {
                return false;
            }
        }
    }
    gResize = false;
    return true;
}
FrameStatus Failure() noexcept
{
    internal::Fail(gSession,
                   gLastResult == VK_ERROR_DEVICE_LOST ? StartupError::DeviceLost : StartupError::RenderingUnavailable);
    return FrameStatus::Failed;
}
ResourceStatus ResourceFailure() noexcept
{
    if (gLastResult == VK_ERROR_DEVICE_LOST)
    {
        internal::Fail(gSession, StartupError::DeviceLost);
    }
    return ResourceStatus::Failed;
}
} // namespace
Backend Kind() noexcept
{
    return Backend::Vulkan;
}
bool Supports(BackendSelection selection) noexcept
{
    // Native builds only ever run Vulkan; a forced browser backend fails
    // explicitly rather than silently falling back to Vulkan.
    return selection == BackendSelection::Auto;
}
bool Initialize(const ApplicationInfo& app) noexcept
{
    if (const auto* imported = internal::NativeDeviceRequest())
    {
        if (volkInitialize() != VK_SUCCESS)
        {
            return false;
        }
        gBorrowedDevice = true;
        gInstance = static_cast<VkInstance>(imported->Identity.Instance.GetObject());
        volkLoadInstance(gInstance);
        gDevice = static_cast<VkDevice>(imported->Identity.Device.GetObject());
        gPhysical = static_cast<VkPhysicalDevice>(imported->Identity.PhysicalDevice.GetObject());
        gQueue = static_cast<VkQueue>(imported->Identity.CommandQueue.GetObject());
        gFamily = imported->Identity.QueueFamily;
        gNativeInstanceVersion = imported->InstanceApiVersion;
        volkLoadDevice(gDevice);
        uint32 count = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(gPhysical, &count, nullptr);
        if (count > 32 || gFamily >= count)
        {
            return false;
        }
        VkQueueFamilyProperties families[32]{};
        vkGetPhysicalDeviceQueueFamilyProperties(gPhysical, &count, families);
        if ((families[gFamily].queueFlags & VK_QUEUE_GRAPHICS_BIT) == 0 ||
            imported->Identity.QueueIndex >= families[gFamily].queueCount)
        {
            return false;
        }
        // The source contract guarantees that this queue was created on this device.
        VkQueue actual = VK_NULL_HANDLE;
        vkGetDeviceQueue(gDevice, gFamily, imported->Identity.QueueIndex, &actual);
        if (actual != gQueue)
        {
            return false;
        }
        VkPhysicalDeviceProperties properties{};
        vkGetPhysicalDeviceProperties(gPhysical, &properties);
        if (properties.apiVersion < VK_API_VERSION_1_1)
        {
            return false;
        }
        gMaxDimension = properties.limits.maxImageDimension2D;
        gMaxUniformSize = properties.limits.maxUniformBufferRange;
        gQueueCompute = (families[gFamily].queueFlags & VK_QUEUE_COMPUTE_BIT) != 0;
        gRayEnabled = gRayApiAvailable = false;
        gHasUniformTextureIndexing = false;
        gHasBindlessTextureIndexing = false;
        gHasRuntimeTextureArrays = false;
        gHasBindingApi = false;
        gHeadless = true;
        return true;
    }
    if (gInstance != VK_NULL_HANDLE)
    {
        return true;
    }
    if (Check(volkInitialize()) == false)
    {
        return false;
    }
    constexpr usize MAX_INSTANCE_EXTENSIONS = 5;
    StaticArray<const char*, MAX_INSTANCE_EXTENSIONS> extensions;
    extensions[0] = VK_KHR_SURFACE_EXTENSION_NAME;
    extensions[1] = "VK_KHR_wayland_surface";
    uint32 extensionCount = 0;
#if defined(VK_USE_PLATFORM_WAYLAND_KHR)
    extensionCount = 2;
    uint32 availableCount = 0;
    if (Check(vkEnumerateInstanceExtensionProperties(nullptr, &availableCount, nullptr)) == false ||
        availableCount > 128)
    {
        volkFinalize();
        return false;
    }
    VkExtensionProperties available[128]{};
    if (Check(vkEnumerateInstanceExtensionProperties(nullptr, &availableCount, available)) == false)
    {
        volkFinalize();
        return false;
    }
    bool caps = false, maintenance = false;
    for (usize i = 0; i < availableCount; ++i)
    {
        caps = caps || std::strcmp(available[i].extensionName, VK_KHR_GET_SURFACE_CAPABILITIES_2_EXTENSION_NAME) == 0;
        maintenance =
            maintenance || std::strcmp(available[i].extensionName, VK_EXT_SURFACE_MAINTENANCE_1_EXTENSION_NAME) == 0;
    }
    gInstanceMaintenance = caps && maintenance;
    if (gInstanceMaintenance)
    {
        extensions[extensionCount++] = VK_KHR_GET_SURFACE_CAPABILITIES_2_EXTENSION_NAME;
        extensions[extensionCount++] = VK_EXT_SURFACE_MAINTENANCE_1_EXTENSION_NAME;
    }
#endif
    constexpr usize MAX_INSTANCE_EXTENSION_PROPERTIES = 128;
    uint32 propertyCount = 0;
    hasDebugLabels = false;
    if (vkEnumerateInstanceExtensionProperties(nullptr, &propertyCount, nullptr) == VK_SUCCESS &&
        propertyCount <= MAX_INSTANCE_EXTENSION_PROPERTIES)
    {
        StaticArray<VkExtensionProperties, MAX_INSTANCE_EXTENSION_PROPERTIES> properties;
        if (vkEnumerateInstanceExtensionProperties(nullptr, &propertyCount, properties.GetData()) == VK_SUCCESS)
        {
            for (usize index = 0; index < propertyCount; ++index)
            {
                if (std::strcmp(properties[index].extensionName, VK_EXT_DEBUG_UTILS_EXTENSION_NAME) == 0)
                {
                    extensions[extensionCount++] = VK_EXT_DEBUG_UTILS_EXTENSION_NAME;
                    hasDebugLabels = true;
                    break;
                }
            }
        }
    }
    VkApplicationInfo application{};
    application.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    application.pApplicationName = app.Name.c_str();
    application.applicationVersion = app.Version;
    uint32 loaderVersion = VK_API_VERSION_1_0;
    if (vkEnumerateInstanceVersion != nullptr)
    {
        static_cast<void>(vkEnumerateInstanceVersion(&loaderVersion));
    }
    gRayApiAvailable = internal::RequestsRayProfile() && loaderVersion >= VK_API_VERSION_1_2;
    gHasBindingApi = (internal::IsBindlessRequested() == true || internal::IsRuntimeTextureArrayRequested() == true) &&
                     loaderVersion >= VK_API_VERSION_1_2;
    application.apiVersion =
        gRayApiAvailable == true || gHasBindingApi == true ? VK_API_VERSION_1_2 : VK_API_VERSION_1_1;
    gNativeInstanceVersion = application.apiVersion;
    VkInstanceCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    info.pApplicationInfo = &application;
    info.enabledExtensionCount = extensionCount;
    info.ppEnabledExtensionNames = extensions.GetData();
    if (Check(vkCreateInstance(&info, nullptr, &gInstance)) == false)
    {
        volkFinalize();
        return false;
    }
    volkLoadInstance(gInstance);
    return true;
}
bool ConnectWindow(const WindowInfo& window) noexcept
{
    if (gInstance == VK_NULL_HANDLE)
    {
        return false;
    }
    if (gBorrowedDevice && gPass == VK_NULL_HANDLE)
    {
        if (window.System == platform::WindowSystem::Hosted)
        {
            // A borrowed device has no exportable-memory extension negotiated by RHI.
            return false;
        }
        gWindow = window;
        gHeadless = true;
        gTarget.Width = window.Width;
        gTarget.Height = window.Height;
        if (window.Width == 0 || window.Height == 0 || window.Width > gMaxDimension || window.Height > gMaxDimension)
        {
            return false;
        }
        gRequestedExtent = { .width = window.Width, .height = window.Height };
        return CreateTargets();
    }
    if (gDevice != VK_NULL_HANDLE)
    {
        return gWindow.System == window.System && gWindow.Display == window.Display &&
               gWindow.Surface == window.Surface;
    }
    gWindow = window;
    if (IsHostedWindow() == true && window.HostedSurfaceOrNull == nullptr)
    {
        return false;
    }
    gHeadless = window.System == platform::WindowSystem::Headless || IsHostedWindow() == true;
    if (gHeadless == false)
    {
#if defined(VK_USE_PLATFORM_WAYLAND_KHR)
        if (window.System != platform::WindowSystem::Wayland || window.Display == nullptr || window.Surface == nullptr)
        {
            return false;
        }
        VkWaylandSurfaceCreateInfoKHR info{};
        info.sType = VK_STRUCTURE_TYPE_WAYLAND_SURFACE_CREATE_INFO_KHR;
        info.display = window.Display;
        info.surface = window.Surface;
        if (Check(vkCreateWaylandSurfaceKHR(gInstance, &info, nullptr, &gSurface)) == false)
        {
            return false;
        }
#else
        return false;
#endif
    }
    uint32 count = 0;
    if (Check(vkEnumeratePhysicalDevices(gInstance, &count, nullptr)) == false || count == 0 ||
        count > MAX_PHYSICAL_DEVICES)
    {
        return false;
    }
    foundation::StaticArray<VkPhysicalDevice, MAX_PHYSICAL_DEVICES> devices{};
    if (Check(vkEnumeratePhysicalDevices(gInstance, &count, devices.GetData())) == false)
    {
        return false;
    }
    const auto connectAdapter = [&window]() noexcept {
        VkPhysicalDeviceProperties properties{};
        vkGetPhysicalDeviceProperties(gPhysical, &properties);
        if (properties.apiVersion < VK_API_VERSION_1_1)
        {
            LUDUS_LOG_ERROR(LOG_RHI, "Vulkan 1.1 is required for the generated SPIR-V profile");
            return false;
        }
        if (IsHostedWindow() == true && HasDeviceExtension(gPhysical, HOSTED_MEMORY_EXTENSION_NAME) == false)
        {
            // Filter before committing to the adapter, so a later capable one is still considered.
            LUDUS_LOG_ERROR(LOG_RHI, "A hosted window needs VK_KHR_external_memory_fd on the adapter");
            return false;
        }
        gMaxDimension = properties.limits.maxImageDimension2D;
        const uint32 dimensionLimits[] = {properties.limits.maxFramebufferWidth,
                                          properties.limits.maxFramebufferHeight,
                                          properties.limits.maxViewportDimensions[0],
                                          properties.limits.maxViewportDimensions[1]};
        for (const auto limit : dimensionLimits)
        {
            if (limit < gMaxDimension)
            {
                gMaxDimension = limit;
            }
        }
        gMaxUniformSize = properties.limits.maxUniformBufferRange;
        const float32 priority = 1;
        VkDeviceQueueCreateInfo queue{};
        queue.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
        queue.queueFamilyIndex = gFamily;
        queue.queueCount = 1;
        queue.pQueuePriorities = &priority;
        // Swapchain (two), ray queries (three) or the hosted memory extension (one).
        constexpr usize MAX_ENABLED_DEVICE_EXTENSIONS = 6;
        const char* extensions[MAX_ENABLED_DEVICE_EXTENSIONS] = {VK_KHR_SWAPCHAIN_EXTENSION_NAME,
                                                                 VK_EXT_SWAPCHAIN_MAINTENANCE_1_EXTENSION_NAME};
        uint32 enabledExtensions = gHeadless ? 0U : 2U;
        if (IsHostedWindow() == true)
        {
            extensions[enabledExtensions++] = HOSTED_MEMORY_EXTENSION_NAME;
        }
        VkPhysicalDeviceSwapchainMaintenance1FeaturesEXT maintenance{};
        maintenance.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SWAPCHAIN_MAINTENANCE_1_FEATURES_EXT;
        if (gHeadless == false)
        {
            uint32 countExtensions = 0;
            if (gInstanceMaintenance == false ||
                Check(vkEnumerateDeviceExtensionProperties(gPhysical, nullptr, &countExtensions, nullptr)) == false ||
                countExtensions > 512)
            {
                LUDUS_LOG_ERROR(LOG_RHI,
                                "Wayland rendering requires surface/swapchain maintenance1 for safe retirement");
                return false;
            }
            VkExtensionProperties propertiesExtensions[512]{};
            if (Check(
                    vkEnumerateDeviceExtensionProperties(gPhysical, nullptr, &countExtensions, propertiesExtensions)) ==
                false)
            {
                return false;
            }
            bool supported = false;
            for (usize i = 0; i < countExtensions; ++i)
            {
                supported = supported || std::strcmp(propertiesExtensions[i].extensionName, extensions[1]) == 0;
            }
            if (supported == false)
            {
                LUDUS_LOG_ERROR(LOG_RHI, "VK_EXT_swapchain_maintenance1 is required for safe presentation retirement");
                return false;
            }
            VkPhysicalDeviceFeatures2 features{};
            features.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
            features.pNext = &maintenance;
            vkGetPhysicalDeviceFeatures2(gPhysical, &features);
            if (maintenance.swapchainMaintenance1 != VK_TRUE)
            {
                return false;
            }
        }
        VkPhysicalDeviceBufferDeviceAddressFeatures address{};
        address.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_BUFFER_DEVICE_ADDRESS_FEATURES;
        VkPhysicalDeviceAccelerationStructureFeaturesKHR structures{};
        structures.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_FEATURES_KHR;
        VkPhysicalDeviceRayQueryFeaturesKHR queries{};
        queries.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_QUERY_FEATURES_KHR;
        gRayEnabled = false;
        if (gRayApiAvailable && properties.apiVersion >= VK_API_VERSION_1_2 && gQueueCompute)
        {
            uint32 extensionCount = 0;
            if (vkEnumerateDeviceExtensionProperties(gPhysical, nullptr, &extensionCount, nullptr) == VK_SUCCESS &&
                extensionCount <= 512)
            {
                VkExtensionProperties available[512]{};
                if (vkEnumerateDeviceExtensionProperties(gPhysical, nullptr, &extensionCount, available) == VK_SUCCESS)
                {
                    const char* required[]{VK_KHR_ACCELERATION_STRUCTURE_EXTENSION_NAME,
                                           VK_KHR_RAY_QUERY_EXTENSION_NAME,
                                           VK_KHR_DEFERRED_HOST_OPERATIONS_EXTENSION_NAME};
                    bool complete = true;
                    for (const char* name : required)
                    {
                        bool found = false;
                        for (usize i = 0; i < extensionCount; ++i)
                        {
                            found = found || std::strcmp(name, available[i].extensionName) == 0;
                        }
                        complete = complete && found;
                    }
                    if (complete)
                    {
                        address.pNext = &structures;
                        structures.pNext = &queries;
                        VkPhysicalDeviceFeatures2 features{};
                        features.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
                        features.pNext = &address;
                        vkGetPhysicalDeviceFeatures2(gPhysical, &features);
                        gRayEnabled =
                            address.bufferDeviceAddress && structures.accelerationStructure && queries.rayQuery;
                        if (gRayEnabled)
                        {
                            // Enable only the implemented static/query subset, not unrelated advertised bits.
                            address =
                            {
                                .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_BUFFER_DEVICE_ADDRESS_FEATURES,
                                .pNext = &structures,
                                .bufferDeviceAddress = VK_TRUE,
                            };
                            structures =
                            {
                                .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_FEATURES_KHR,
                                .pNext = &queries,
                                .accelerationStructure = VK_TRUE,
                            };
                            queries =
                            {
                                .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_QUERY_FEATURES_KHR,
                                .rayQuery = VK_TRUE,
                            };
                            for (const char* name : required)
                            {
                                extensions[enabledExtensions++] = name;
                            }
                            queries.pNext = gHeadless ? nullptr : &maintenance;
                            gRayProperties =
                            {
                                .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_PROPERTIES_KHR,
                            };
                            VkPhysicalDeviceProperties2 rayProperties
                            {
                                .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2,
                                .pNext = &gRayProperties,
                            };
                            vkGetPhysicalDeviceProperties2(gPhysical, &rayProperties);
                        }
                    }
                }
            }
        }
        // Thanks to Khronos Group, VkPhysicalDeviceDescriptorIndexingFeatures,
        // https://docs.vulkan.org/refpages/latest/refpages/source/VkPhysicalDeviceDescriptorIndexingFeatures.html
        // for independent non-uniform indexing, runtime arrays and variable-count
        // allocation admission; no native mutable descriptor features are enabled.
        VkPhysicalDeviceDescriptorIndexingFeatures indexing
        {
            .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_INDEXING_FEATURES,
        };
        gHasBindlessTextureIndexing = false;
        gHasRuntimeTextureArrays = false;
        if (gHasBindingApi == true && properties.apiVersion >= VK_API_VERSION_1_2)
        {
            VkPhysicalDeviceFeatures2 features
            {
                .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2,
                .pNext = &indexing,
            };
            vkGetPhysicalDeviceFeatures2(gPhysical, &features);
            gHasBindlessTextureIndexing = internal::IsBindlessRequested() == true &&
                                          indexing.shaderSampledImageArrayNonUniformIndexing == VK_TRUE;
            gHasRuntimeTextureArrays = internal::IsRuntimeTextureArrayRequested() == true &&
                                       indexing.runtimeDescriptorArray == VK_TRUE &&
                                       indexing.descriptorBindingVariableDescriptorCount == VK_TRUE;
        }
        if ((internal::IsBindlessRequired() == true && gHasBindlessTextureIndexing == false) ||
            (internal::IsRuntimeTextureArrayRequired() == true && gHasRuntimeTextureArrays == false))
        {
            // Required profiles filter candidates before committing native device
            // ownership, so a later supported adapter is still considered.
            return false;
        }
        indexing =
        {
            .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_INDEXING_FEATURES,
            .shaderSampledImageArrayNonUniformIndexing = gHasBindlessTextureIndexing == true ? VK_TRUE : VK_FALSE,
            .descriptorBindingVariableDescriptorCount = gHasRuntimeTextureArrays == true ? VK_TRUE : VK_FALSE,
            .runtimeDescriptorArray = gHasRuntimeTextureArrays == true ? VK_TRUE : VK_FALSE,
        };
        void* featureChain = gRayEnabled == true ? static_cast<void*>(&address)
                             : gHeadless == true ? nullptr
                                                 : static_cast<void*>(&maintenance);
        if (gHasBindlessTextureIndexing == true || gHasRuntimeTextureArrays == true)
        {
            indexing.pNext = featureChain;
            featureChain = &indexing;
        }
        VkPhysicalDeviceFeatures supported{};
        vkGetPhysicalDeviceFeatures(gPhysical, &supported);
        const VkPhysicalDeviceFeatures enabled
        {
            .textureCompressionBC = supported.textureCompressionBC,
            .shaderSampledImageArrayDynamicIndexing = supported.shaderSampledImageArrayDynamicIndexing,
        };
        gHasUniformTextureIndexing = enabled.shaderSampledImageArrayDynamicIndexing == VK_TRUE;
        const VkDeviceCreateInfo device
        {
            .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
            .pNext = featureChain,
            .queueCreateInfoCount = 1,
            .pQueueCreateInfos = &queue,
            .enabledExtensionCount = enabledExtensions,
            .ppEnabledExtensionNames = extensions,
            .pEnabledFeatures = &enabled,
        };
        if (Check(vkCreateDevice(gPhysical, &device, nullptr, &gDevice)) == false)
        {
            return false;
        }
        volkLoadDevice(gDevice);
        vkGetDeviceQueue(gDevice, gFamily, 0, &gQueue);
        if (IsHostedWindow() == true)
        {
            gHostedProducer.Connect({ .PhysicalDevice = gPhysical, .Device = gDevice, .QueueFamily = gFamily },
                                    *window.HostedSurfaceOrNull);
        }
        gTarget.Width = window.Width;
        gTarget.Height = window.Height;
        if (gTarget.Width == 0 || gTarget.Height == 0 || gTarget.Width > gMaxDimension ||
            gTarget.Height > gMaxDimension)
        {
            return false;
        }
        gRequestedExtent = { .width = gTarget.Width, .height = gTarget.Height };
        if (CreateTargets() == false)
        {
            return false;
        }
        LUDUS_LOG_INFO(LOG_RHI, "Vulkan adapter: {}", properties.deviceName);
        return true;
    };
    // A hosted window tries the adapter its consumer imports onto first; see internal/hosted_adapter.h.
    platform::HostedAdapterIdentity consumerAdapter;
    const bool hasConsumerAdapter =
        IsHostedWindow() == true && window.HostedSurfaceOrNull->TryGetConsumerAdapter(consumerAdapter) == true;
    foundation::StaticArray<platform::HostedAdapterIdentity, MAX_PHYSICAL_DEVICES> adapters{};
    for (usize i = 0; i < count && hasConsumerAdapter == true; ++i)
    {
        VkPhysicalDeviceProperties properties{};
        vkGetPhysicalDeviceProperties(devices[i], &properties);
        // Older adapters keep a zero identity: they never match, and connectAdapter rejects them.
        if (properties.apiVersion >= VK_API_VERSION_1_1)
        {
            ReadHostedAdapterIdentity(devices[i], adapters[i]);
        }
    }
    foundation::StaticArray<usize, MAX_PHYSICAL_DEVICES> order{};
    const usize matchCount =
        internal::OrderAdaptersByPreference({adapters.GetData(), count},
                                            hasConsumerAdapter == true ? &consumerAdapter : nullptr,
                                            {order.GetData(), count});
    if (hasConsumerAdapter == true && matchCount == 0)
    {
        LUDUS_LOG_WARN(LOG_RHI, "No Vulkan adapter matches the hosting editor's; it will refuse this game's images");
    }
    for (usize position = 0; position < count; ++position)
    {
        const usize i = order[position];
        uint32 families = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(devices[i], &families, nullptr);
        if (families > 32)
        {
            continue;
        }
        VkQueueFamilyProperties properties[32]{};
        vkGetPhysicalDeviceQueueFamilyProperties(devices[i], &families, properties);
        for (usize j = 0; j < families; ++j)
        {
            VkBool32 present = VK_TRUE;
            if (gHeadless == false &&
                Check(vkGetPhysicalDeviceSurfaceSupportKHR(devices[i], static_cast<uint32>(j), gSurface, &present)) ==
                    false)
            {
                continue;
            }
            if (properties[j].queueCount != 0 && (properties[j].queueFlags & VK_QUEUE_GRAPHICS_BIT) != 0 &&
                present == VK_TRUE)
            {
                gPhysical = devices[i];
                gFamily = static_cast<uint32>(j);
                gQueueCompute = (properties[j].queueFlags & VK_QUEUE_COMPUTE_BIT) != 0;
                // Required workloads filter candidates before committing to a
                // queue/adapter, so a later usable candidate remains reachable.
                if (internal::RequiresComputeProfile() && ComputeLimits().MaxStorageRange == 0)
                {
                    gPhysical = VK_NULL_HANDLE;
                    continue;
                }
                break;
            }
        }
        if (gPhysical != VK_NULL_HANDLE)
        {
            if (connectAdapter())
            {
                return true;
            }
            ReleaseTargets();
            gHostedProducer.Disconnect();
            if (gDevice != VK_NULL_HANDLE)
            {
                vkDestroyRenderPass(gDevice, gPass, nullptr);
                vkDestroyDevice(gDevice, nullptr);
            }
            gDevice = VK_NULL_HANDLE;
            gPhysical = VK_NULL_HANDLE;
            gQueue = VK_NULL_HANDLE;
            gPass = VK_NULL_HANDLE;
            gFormat = VK_FORMAT_UNDEFINED;
            gEncoding = false;
        }
    }
    return false;
}

bool InitializeRendering() noexcept
{
    if (gDevice == VK_NULL_HANDLE)
    {
        return false;
    }
    if (gCommands != VK_NULL_HANDLE)
    {
        return true;
    }
    VkCommandPoolCreateInfo pool{};
    pool.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    pool.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    pool.queueFamilyIndex = gFamily;
    if (Check(vkCreateCommandPool(gDevice, &pool, nullptr, &gCommands)) == false)
    {
        return false;
    }
    for (auto& frame : gFrames)
    {
        VkCommandBufferAllocateInfo allocate{};
        allocate.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        allocate.commandPool = gCommands;
        allocate.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        allocate.commandBufferCount = 1;
        if (Check(vkAllocateCommandBuffers(gDevice, &allocate, &frame.Command)) == false)
        {
            return false;
        }
        VkFenceCreateInfo fence{};
        fence.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
        fence.flags = VK_FENCE_CREATE_SIGNALED_BIT;
        VkSemaphoreCreateInfo semaphore{};
        semaphore.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
        if (Check(vkCreateFence(gDevice, &fence, nullptr, &frame.Fence)) == false ||
            Check(vkCreateSemaphore(gDevice, &semaphore, nullptr, &frame.Acquired)) == false)
        {
            return false;
        }
    }
    return true;
}
StartupError Start(const ApplicationInfo& app, const WindowInfo& window, uint32 token, BackendSelection) noexcept
{
    // Native ignores the browser selection (the facade already rejected a forced
    // browser backend via Supports); Vulkan is the only native backend.
    const bool isValidHosted = window.System == platform::WindowSystem::Hosted && window.HostedSurfaceOrNull != nullptr;
    if (window.System != platform::WindowSystem::Headless && isValidHosted == false &&
        (window.System != platform::WindowSystem::Wayland || window.Display == nullptr || window.Surface == nullptr))
    {
        return StartupError::InvalidWindow;
    }
    gSession = token;
    if (backend::Initialize(app) == false)
    {
        return StartupError::InstanceUnavailable;
    }
    if (ConnectWindow(window) == false)
    {
        return StartupError::SurfaceUnavailable;
    }
    if (InitializeRendering() == false)
    {
        return StartupError::RenderingUnavailable;
    }
    internal::Complete(token,
                       StartupError::None,
                       { .MaxFrameDimension2D = gMaxDimension, .MaxUniformBufferSize = gMaxUniformSize });
    return StartupError::None;
}
void ShutdownRendering() noexcept
{
    if (gDevice == VK_NULL_HANDLE)
    {
        return;
    }
    RetireAcquiredImage();
    WaitIdle();
    WaitPresents();
    gHostedProducer.Cancel();
    gHostedFramebuffer = VK_NULL_HANDLE;
    for (auto& frame : gFrames)
    {
        vkDestroyFence(gDevice, frame.Fence, nullptr);
        vkDestroySemaphore(gDevice, frame.Acquired, nullptr);
        frame = {};
    }
    vkDestroyCommandPool(gDevice, gCommands, nullptr);
    gCommands = VK_NULL_HANDLE;
    gEncoding = false;
}
void Shutdown() noexcept
{
    ShutdownRendering();
    ReleaseTargets();
    // ShutdownRendering waited for every submission that wrote a hosted image.
    gHostedProducer.Disconnect();
    if (gDevice != VK_NULL_HANDLE)
    {
        vkDestroyRenderPass(gDevice, gPass, nullptr);
        gPass = VK_NULL_HANDLE;
        if (gBorrowedDevice == false)
        {
            vkDestroyDevice(gDevice, nullptr);
        }
        gDevice = VK_NULL_HANDLE;
    }
    if (gInstance != VK_NULL_HANDLE)
    {
        if (gSurface != VK_NULL_HANDLE)
        {
            vkDestroySurfaceKHR(gInstance, gSurface, nullptr);
        }
        gSurface = VK_NULL_HANDLE;
        if (gBorrowedDevice == false)
        {
            vkDestroyInstance(gInstance, nullptr);
        }
        gInstance = VK_NULL_HANDLE;
        volkFinalize();
    }
    gBorrowedDevice = false;
    hasDebugLabels = false;
    gNativeInstanceVersion = VK_API_VERSION_1_1;
    gPhysical = VK_NULL_HANDLE;
    gRayEnabled = gRayApiAvailable = false;
    gHasUniformTextureIndexing = false;
    gHasBindlessTextureIndexing = false;
    gHasRuntimeTextureArrays = false;
    gHasBindingApi = false;
    gRayProperties = {};
    gQueue = VK_NULL_HANDLE;
    gFrame = 0;
    gSession = 0;
    gMaxDimension = 0;
    gMaxUniformSize = 0;
    gAcquiredWait = false;
    gOwnedImage = false;
    gInstanceMaintenance = false;
    gResize = true;
    gFormat = VK_FORMAT_UNDEFINED;
    gTarget = {};
    gExtent = {};
    gRequestedExtent = {};
}
namespace
{
bool ValidColor(float64 value) noexcept
{
    return value >= 0 && value <= 1;
}
} // namespace
FrameStatus SetTarget(const FrameTarget& target) noexcept
{
    if (target.ShouldPresentToSurface == false)
    {
        return FrameStatus::Unsupported;
    }
    if (target.Width > gMaxDimension || target.Height > gMaxDimension || ValidColor(target.Red) == false ||
        ValidColor(target.Green) == false || ValidColor(target.Blue) == false || ValidColor(target.Alpha) == false)
    {
        return FrameStatus::InvalidState;
    }
    if (target.Width != 0 && target.Height != 0)
    {
        // Compare requests, not the clamped acquired extent. A minimized tick
        // or a persistent surface clamp must not recreate targets every frame.
        gResize = gResize || target.Width != gRequestedExtent.width || target.Height != gRequestedExtent.height;
        gRequestedExtent = { .width = target.Width, .height = target.Height };
    }
    gTarget = target;
    return FrameStatus::Ready;
}
FrameStatus Begin() noexcept
{
    if (gHostedProducer.IsConnected() == true && gHostedProducer.Poll() == false)
    {
        gLastResult = VK_ERROR_DEVICE_LOST;
        return Failure();
    }
    if (gTarget.Width == 0 || gTarget.Height == 0)
    {
        return FrameStatus::Skipped;
    }
    if (gResize && CreateTargets() == false)
    {
        return Failure();
    }
    if (gExtent.width == 0 || gExtent.height == 0)
    {
        return FrameStatus::Skipped;
    }
    auto& frame = gFrames[gFrame];
    if (Check(vkWaitForFences(gDevice, 1, &frame.Fence, VK_TRUE, 5000000000ULL)) == false)
    {
        return Failure();
    }
    frame.Submitted = false;
    // Hosted frames guarded by this fence have completed; it may be reset from now on.
    gHostedProducer.MarkSignaled(frame.Fence);
    // Harvest the captured query before its frame fence can be reset for later work.
    (void)TimingPoll();
    static_cast<void>(PollPassTimings());
    if (gDevice == VK_NULL_HANDLE)
    {
        return FrameStatus::Failed;
    }
    (void)RasterCompleted();
    if (gDevice == VK_NULL_HANDLE)
    {
        return FrameStatus::Failed;
    }
    gImage = 0;
    if (gHostedProducer.IsConnected() == true)
    {
        const HostedVulkanAcquireStatus acquired =
            gHostedProducer.Acquire(gExtent.width, gExtent.height, gPass, gDepthView, gDepthEpoch, gHostedFramebuffer);
        if (acquired == HostedVulkanAcquireStatus::Skipped)
        {
            return FrameStatus::Skipped;
        }
        if (acquired == HostedVulkanAcquireStatus::Failed)
        {
            return Failure();
        }
    }
    if (gHeadless == false)
    {
        const auto result =
            vkAcquireNextImageKHR(gDevice, gSwapchain, 5000000000ULL, frame.Acquired, VK_NULL_HANDLE, &gImage);
        if (result == VK_ERROR_OUT_OF_DATE_KHR)
        {
            gResize = true;
            return FrameStatus::Skipped;
        }
        if (result == VK_TIMEOUT || result == VK_NOT_READY)
        {
            return FrameStatus::Skipped;
        }
        if (result == VK_SUBOPTIMAL_KHR)
        {
            gResize = true;
        }
        else if (Check(result) == false)
        {
            return Failure();
        }
    }
    if (gHeadless == false)
    {
        gAcquiredWait = true;
        gOwnedImage = true;
        if (gPresented[gImage])
        {
            if (Check(vkWaitForFences(gDevice, 1, &gPresentFences[gImage], VK_TRUE, 5000000000ULL)) == false ||
                Check(vkResetFences(gDevice, 1, &gPresentFences[gImage])) == false)
            {
                return Failure();
            }
            gPresented[gImage] = false;
        }
    }
    if (Check(vkResetCommandBuffer(frame.Command, 0)) == false)
    {
        gHostedProducer.Cancel();
        return Failure();
    }
    VkCommandBufferBeginInfo begin{};
    begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    if (Check(vkBeginCommandBuffer(frame.Command, &begin)) == false)
    {
        gHostedProducer.Cancel();
        return Failure();
    }
    TimingBegin();
    BeginPassTimingFrame();
    VkClearValue clears[2]{};
    auto& clear = clears[0];
    clears[1].depthStencil = {1, 0};
    clear.color.float32[0] = static_cast<float32>(gTarget.Red);
    clear.color.float32[1] = static_cast<float32>(gTarget.Green);
    clear.color.float32[2] = static_cast<float32>(gTarget.Blue);
    clear.color.float32[3] = static_cast<float32>(gTarget.Alpha);
    VkRenderPassBeginInfo pass{};
    pass.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    pass.renderPass = gPass;
    pass.framebuffer = SurfaceFramebuffer();
    pass.renderArea.extent = gExtent;
    pass.clearValueCount = 2;
    pass.pClearValues = clears;
    vkCmdBeginRenderPass(frame.Command, &pass, VK_SUBPASS_CONTENTS_INLINE);
    gEncoding = true;
    RasterFrameExtent();
    return FrameStatus::Ready;
}
FrameStatus End() noexcept
{
    auto& frame = gFrames[gFrame];
    if (gEncoding)
    {
        vkCmdEndRenderPass(frame.Command);
    }
    gEncoding = false;
    TimingEnd();
    if (gHostedProducer.IsConnected() == true)
    {
        // Hand the image to the editor: VK_QUEUE_FAMILY_EXTERNAL, SHADER_READ_ONLY_OPTIMAL.
        gHostedProducer.RecordRelease(frame.Command);
    }
    if (Check(vkEndCommandBuffer(frame.Command)) == false || Check(vkResetFences(gDevice, 1, &frame.Fence)) == false)
    {
        gHostedProducer.Cancel();
        return Failure();
    }
    const VkPipelineStageFlags waitStage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    VkSubmitInfo submit{};
    submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &frame.Command;
    if (gHeadless == false)
    {
        submit.waitSemaphoreCount = 1;
        submit.pWaitSemaphores = &frame.Acquired;
        submit.pWaitDstStageMask = &waitStage;
        submit.signalSemaphoreCount = 1;
        submit.pSignalSemaphores = &gFinished[gImage];
    }
    if (Check(vkQueueSubmit(gQueue, 1, &submit, frame.Fence)) == false)
    {
        gHostedProducer.Cancel();
        return Failure();
    }
    frame.Submitted = true;
    if (gHostedProducer.IsConnected() == true)
    {
        // Announced with Present once this fence signals (CPU fence).
        gHostedProducer.Submit(frame.Fence);
        gHostedFramebuffer = VK_NULL_HANDLE;
        static_cast<void>(gHostedProducer.Poll());
    }
    TimingSubmitted();
    SubmitPassTimings();
    gAcquiredWait = false;
    gFrame = (gFrame + 1) % FRAMES;
    if (gHeadless == false)
    {
        VkSwapchainPresentFenceInfoEXT fence{};
        fence.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_PRESENT_FENCE_INFO_EXT;
        fence.swapchainCount = 1;
        fence.pFences = &gPresentFences[gImage];
        VkPresentInfoKHR present{};
        present.pNext = &fence;
        present.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
        present.waitSemaphoreCount = 1;
        present.pWaitSemaphores = &gFinished[gImage];
        present.swapchainCount = 1;
        present.pSwapchains = &gSwapchain;
        present.pImageIndices = &gImage;
        const auto result = vkQueuePresentKHR(gQueue, &present);
        // Outdated/lost surfaces still enqueue the wait and presentation fence.
        if (result == VK_SUCCESS || result == VK_SUBOPTIMAL_KHR || result == VK_ERROR_OUT_OF_DATE_KHR ||
            result == VK_ERROR_SURFACE_LOST_KHR)
        {
            gPresented[gImage] = true;
            gOwnedImage = false;
        }
        if (result == VK_ERROR_OUT_OF_DATE_KHR || result == VK_SUBOPTIMAL_KHR)
        {
            gResize = true;
            return result == VK_ERROR_OUT_OF_DATE_KHR ? FrameStatus::Skipped : FrameStatus::Ready;
        }
        if (Check(result) == false)
        {
            return Failure();
        }
    }
    return FrameStatus::Ready;
}
bool BeginFrame() noexcept
{
    return Begin() == FrameStatus::Ready;
}
bool EndFrame() noexcept
{
    return End() == FrameStatus::Ready;
}
ResourceStatus CreateShader(usize slot, const ShaderDescription& description, uint32) noexcept
{
    // Check the declared SPIR-V entry/stage before passing it to the driver.
    bool found = false;
    for (usize offset = 5; offset < description.Spirv.size();)
    {
        const uint32 word = description.Spirv[offset];
        const usize count = word >> 16;
        if (count == 0 || count > description.Spirv.size() - offset)
        {
            return ResourceFailure();
        }
        if ((word & 65535U) == 15 && count >= 4)
        {
            const auto* name = reinterpret_cast<const char*>(description.Spirv.data() + offset + 3);
            const usize capacity = (count - 3) * sizeof(uint32);
            const usize length = description.SpirvEntry.size();
            const uint32 stage = description.Stage == ShaderStage::Vertex ? 0U : 4U;
            found = found || (description.Spirv[offset + 1] == stage && length < capacity && name[length] == '\0' &&
                              std::memcmp(name, description.SpirvEntry.data(), length) == 0);
        }
        offset += count;
    }
    if (found == false)
    {
        return ResourceFailure();
    }
    auto& shader = gShaders[slot];
    shader.Stage = description.Stage;
    std::memcpy(shader.Entry, description.SpirvEntry.data(), description.SpirvEntry.size());
    shader.Entry[description.SpirvEntry.size()] = '\0';
    VkShaderModuleCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    info.codeSize = description.Spirv.size_bytes();
    info.pCode = description.Spirv.data();
    return Check(vkCreateShaderModule(gDevice, &info, nullptr, &shader.Module)) ? ResourceStatus::Ready
                                                                                : ResourceFailure();
}
void DestroyShader(usize slot) noexcept
{
    if (gDevice != VK_NULL_HANDLE)
    {
        vkDestroyShaderModule(gDevice, gShaders[slot].Module, nullptr);
    }
    gShaders[slot] = {};
}
ResourceStatus CreateUniform(usize slot, const UniformDescription& description, uint32) noexcept
{
    const usize size = description.Size;
    auto& uniform = gUniforms[slot];
    uniform.Size = size;
    for (usize i = 0; i < FRAMES; ++i)
    {
        VkBufferCreateInfo info{};
        info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        info.size = size;
        info.usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;
        if (Check(vkCreateBuffer(gDevice, &info, nullptr, &uniform.Buffers[i])) == false)
        {
            return ResourceFailure();
        }
        VkMemoryRequirements requirements{};
        vkGetBufferMemoryRequirements(gDevice, uniform.Buffers[i], &requirements);
        if (Allocate(requirements,
                     VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                     uniform.Memory[i]) == false ||
            Check(vkBindBufferMemory(gDevice, uniform.Buffers[i], uniform.Memory[i], 0)) == false ||
            Check(vkMapMemory(gDevice, uniform.Memory[i], 0, size, 0, &uniform.Mapped[i])) == false)
        {
            return ResourceFailure();
        }
    }
    return ResourceStatus::Ready;
}
void UpdateUniform(usize slot, std::span<const uint8> bytes) noexcept
{
    std::memcpy(gUniforms[slot].Bytes, bytes.data(), bytes.size());
}
void DestroyUniform(usize slot) noexcept
{
    auto& uniform = gUniforms[slot];
    if (gDevice != VK_NULL_HANDLE)
    {
        if (uniform.Size != 0)
        {
            WaitIdle();
        }
        for (usize i = 0; i < FRAMES; ++i)
        {
            if (uniform.Mapped[i] != nullptr)
            {
                vkUnmapMemory(gDevice, uniform.Memory[i]);
            }
            vkDestroyBuffer(gDevice, uniform.Buffers[i], nullptr);
            vkFreeMemory(gDevice, uniform.Memory[i], nullptr);
        }
    }
    uniform = {};
}
ResourceStatus CreatePipeline(usize slot, const PipelineResources& resources, uint32) noexcept
{
    if (gPass == VK_NULL_HANDLE)
    {
        return ResourceFailure();
    }
    const usize vertex = resources.Vertex, fragment = resources.Fragment, uniform = resources.Uniform;
    auto& pipeline = gPipelines[slot];
    pipeline.UniformSlot = uniform;
    VkDescriptorSetLayoutBinding binding{};
    binding.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    binding.descriptorCount = 1;
    binding.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    VkDescriptorSetLayoutCreateInfo bindings{};
    bindings.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    bindings.bindingCount = 1;
    bindings.pBindings = &binding;
    if (Check(vkCreateDescriptorSetLayout(gDevice, &bindings, nullptr, &pipeline.Bindings)) == false)
    {
        return ResourceFailure();
    }
    VkDescriptorPoolSize poolSize{};
    poolSize.type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    poolSize.descriptorCount = FRAMES;
    VkDescriptorPoolCreateInfo pool{};
    pool.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    pool.maxSets = FRAMES;
    pool.poolSizeCount = 1;
    pool.pPoolSizes = &poolSize;
    if (Check(vkCreateDescriptorPool(gDevice, &pool, nullptr, &pipeline.Pool)) == false)
    {
        return ResourceFailure();
    }
    VkDescriptorSetLayout layouts[FRAMES] = {pipeline.Bindings, pipeline.Bindings};
    VkDescriptorSetAllocateInfo allocate{};
    allocate.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    allocate.descriptorPool = pipeline.Pool;
    allocate.descriptorSetCount = FRAMES;
    allocate.pSetLayouts = layouts;
    if (Check(vkAllocateDescriptorSets(gDevice, &allocate, pipeline.Sets)) == false)
    {
        return ResourceFailure();
    }
    for (usize i = 0; i < FRAMES; ++i)
    {
        VkDescriptorBufferInfo buffer{};
        buffer.buffer = gUniforms[uniform].Buffers[i];
        buffer.range = gUniforms[uniform].Size;
        VkWriteDescriptorSet write{};
        write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        write.dstSet = pipeline.Sets[i];
        write.descriptorCount = 1;
        write.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        write.pBufferInfo = &buffer;
        vkUpdateDescriptorSets(gDevice, 1, &write, 0, nullptr);
    }
    VkPipelineLayoutCreateInfo layout{};
    layout.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    layout.setLayoutCount = 1;
    layout.pSetLayouts = &pipeline.Bindings;
    if (Check(vkCreatePipelineLayout(gDevice, &layout, nullptr, &pipeline.Layout)) == false)
    {
        return ResourceFailure();
    }
    VkPipelineShaderStageCreateInfo stages[2]{
        { .stage = VK_SHADER_STAGE_VERTEX_BIT },
        { .stage = VK_SHADER_STAGE_FRAGMENT_BIT },
    };
    stages[0].sType = stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = gShaders[vertex].Module;
    stages[0].pName = gShaders[vertex].Entry;
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = gShaders[fragment].Module;
    stages[1].pName = gShaders[fragment].Entry;
    VkPipelineVertexInputStateCreateInfo input{};
    input.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    VkPipelineInputAssemblyStateCreateInfo assembly{};
    assembly.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkPipelineViewportStateCreateInfo viewport{};
    viewport.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    viewport.viewportCount = 1;
    viewport.scissorCount = 1;
    VkPipelineRasterizationStateCreateInfo raster{};
    raster.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    raster.polygonMode = VK_POLYGON_MODE_FILL;
    raster.lineWidth = 1;
    VkPipelineMultisampleStateCreateInfo samples{ .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT };
    samples.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    samples.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkPipelineColorBlendAttachmentState color{};
    color.colorWriteMask = 15;
    VkPipelineColorBlendStateCreateInfo blend{};
    blend.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    blend.attachmentCount = 1;
    blend.pAttachments = &color;
    VkDynamicState states[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dynamic{};
    dynamic.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dynamic.dynamicStateCount = 2;
    dynamic.pDynamicStates = states;
    VkGraphicsPipelineCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    info.stageCount = 2;
    info.pStages = stages;
    info.pVertexInputState = &input;
    info.pInputAssemblyState = &assembly;
    info.pViewportState = &viewport;
    info.pRasterizationState = &raster;
    info.pMultisampleState = &samples;
    VkPipelineDepthStencilStateCreateInfo depth{};
    depth.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    info.pDepthStencilState = &depth;
    info.pColorBlendState = &blend;
    info.pDynamicState = &dynamic;
    info.layout = pipeline.Layout;
    info.renderPass = gPass;
    return Check(vkCreateGraphicsPipelines(gDevice, VK_NULL_HANDLE, 1, &info, nullptr, &pipeline.Object))
               ? ResourceStatus::Ready
               : ResourceFailure();
}
void DestroyPipeline(usize slot) noexcept
{
    auto& pipeline = gPipelines[slot];
    if (gDevice != VK_NULL_HANDLE)
    {
        if (pipeline.Object != VK_NULL_HANDLE)
        {
            WaitIdle();
        }
        vkDestroyPipeline(gDevice, pipeline.Object, nullptr);
        vkDestroyPipelineLayout(gDevice, pipeline.Layout, nullptr);
        vkDestroyDescriptorPool(gDevice, pipeline.Pool, nullptr);
        vkDestroyDescriptorSetLayout(gDevice, pipeline.Bindings, nullptr);
    }
    pipeline = {};
}
FrameInfo GetFrameInfo() noexcept
{
    return
    {
        .Width = gExtent.width,
        .Height = gExtent.height,
        .Encoding = gFormat == VK_FORMAT_B8G8R8A8_SRGB || gFormat == VK_FORMAT_R8G8B8A8_SRGB ? SurfaceEncoding::Srgb
                                                                                             : SurfaceEncoding::Unorm,
    };
}
ResourceStatus Draw(usize slot) noexcept
{
    auto& pipeline = gPipelines[slot];
    auto& uniform = gUniforms[pipeline.UniformSlot];
    std::memcpy(uniform.Mapped[gFrame], uniform.Bytes, uniform.Size);
    const auto command = gFrames[gFrame].Command;
    VkViewport viewport{};
    viewport.width = static_cast<float32>(gExtent.width);
    viewport.height = static_cast<float32>(gExtent.height);
    viewport.maxDepth = 1;
    VkRect2D scissor{};
    scissor.extent = gExtent;
    vkCmdSetViewport(command, 0, 1, &viewport);
    vkCmdSetScissor(command, 0, 1, &scissor);
    vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline.Object);
    vkCmdBindDescriptorSets(command,
                            VK_PIPELINE_BIND_POINT_GRAPHICS,
                            pipeline.Layout,
                            0,
                            1,
                            &pipeline.Sets[gFrame],
                            0,
                            nullptr);
    vkCmdDraw(command, 3, 1, 0, 0);
    return ResourceStatus::Ready;
}

#if defined(LUDUS_RHI_TEST_READBACK)
bool ReadHeadlessPixels(std::span<uint8> pixels) noexcept
{
    if (gHeadless == false || gEncoding || pixels.size() != static_cast<usize>(gExtent.width) * gExtent.height * 4)
    {
        return false;
    }
    struct Readback final
    {
        VkBuffer Buffer = VK_NULL_HANDLE;
        VkDeviceMemory Memory = VK_NULL_HANDLE;
        VkCommandBuffer Command = VK_NULL_HANDLE;
        ~Readback() noexcept
        {
            WaitIdle();
            if (Command != VK_NULL_HANDLE)
            {
                vkFreeCommandBuffers(gDevice, gCommands, 1, &Command);
            }
            vkDestroyBuffer(gDevice, Buffer, nullptr);
            vkFreeMemory(gDevice, Memory, nullptr);
        }
    } copy;
    WaitIdle();
    VkBufferCreateInfo buffer{};
    buffer.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    buffer.size = pixels.size();
    buffer.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    if (Check(vkCreateBuffer(gDevice, &buffer, nullptr, &copy.Buffer)) == false)
    {
        return false;
    }
    VkMemoryRequirements requirements{};
    vkGetBufferMemoryRequirements(gDevice, copy.Buffer, &requirements);
    if (Allocate(requirements,
                 VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                 copy.Memory) == false ||
        Check(vkBindBufferMemory(gDevice, copy.Buffer, copy.Memory, 0)) == false)
    {
        return false;
    }
    VkCommandBufferAllocateInfo allocate{};
    allocate.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    allocate.commandPool = gCommands;
    allocate.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    allocate.commandBufferCount = 1;
    if (Check(vkAllocateCommandBuffers(gDevice, &allocate, &copy.Command)) == false)
    {
        return false;
    }
    VkCommandBufferBeginInfo begin{};
    begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    if (Check(vkBeginCommandBuffer(copy.Command, &begin)) == false)
    {
        return false;
    }
    VkBufferImageCopy region{};
    region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    region.imageSubresource.layerCount = 1;
    region.imageExtent = { .width = gExtent.width, .height = gExtent.height, .depth = 1 };
    vkCmdCopyImageToBuffer(copy.Command, gImages[0], VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, copy.Buffer, 1, &region);
    VkMemoryBarrier barrier{};
    barrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
    barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
    vkCmdPipelineBarrier(copy.Command,
                         VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_HOST_BIT,
                         0,
                         1,
                         &barrier,
                         0,
                         nullptr,
                         0,
                         nullptr);
    if (Check(vkEndCommandBuffer(copy.Command)) == false)
    {
        return false;
    }
    VkSubmitInfo submit{};
    submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &copy.Command;
    if (Check(vkQueueSubmit(gQueue, 1, &submit, VK_NULL_HANDLE)) == false || Check(vkQueueWaitIdle(gQueue)) == false)
    {
        return false;
    }
    void* mapped = nullptr;
    if (Check(vkMapMemory(gDevice, copy.Memory, 0, pixels.size(), 0, &mapped)) == false)
    {
        return false;
    }
    std::memcpy(pixels.data(), mapped, pixels.size());
    vkUnmapMemory(gDevice, copy.Memory);
    return true;
}
#endif
} // namespace ludus::graphics::rhi::backend

#include "internal/raster_vulkan.h"

#include "internal/lifetime_vulkan.h"

#include "internal/ray_vulkan.h"

#include "internal/gpu_scope_vulkan.h"
#include "internal/measurement_vulkan.h"

#include "internal/native_interop_vulkan.h"
