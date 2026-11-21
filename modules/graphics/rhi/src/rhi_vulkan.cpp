#include "internal/adapter.h"
#include "internal/backend.h"
#include "internal/gpu_scope_backend.h"
#include "internal/hosted_adapter.h"
#include "internal/hosted_vulkan.h"
#include "internal/lifecycle.h"
#include "internal/measurement.h"
#include "internal/native_interop.h"
#include "internal/native_preparation.h"
#include "internal/owner.h"
#include "internal/raster.h"
#include "internal/ray_backend.h"
#include "internal/resources.h"
#include "internal/surface.h"
#include "internal/vma.h"
#include <ludus/foundation/containers/borrowed_view.hpp>

#include <ludus/foundation/containers/static_array.hpp>
#include <ludus/foundation/logging/log_format.hpp>
#include <ludus/foundation/math/scalar.hpp>
#include <ludus/graphics/rhi/device.h>
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
void ClosePassLabels() noexcept;
void CloseScopeLabels() noexcept;
/// Best-effort VK_EXT_debug_utils object name; ignored without the extension or a device.
void SetVulkanObjectName(VkObjectType type, foundation::uint64 object, const char* name) noexcept;
/// Extension name when the adapter supports it with the device and NowTicks host time domains.
const char* AdmitClockCalibrationExtensionOrNull(VkPhysicalDevice physical,
                                                 NativeCalibratedTimestamps extension) noexcept;
/// Resolves the calibration entry point of an enabled extension; null disables calibration.
void ConnectClockCalibration(const char* extensionOrNull) noexcept;
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
/// Instance extensions one loader may report; a larger count is rejected, never truncated.
constexpr uint32 MAX_AVAILABLE_INSTANCE_EXTENSIONS = 128;
/// Queue families one adapter may report; later families are not considered.
constexpr uint32 MAX_QUEUE_FAMILIES = 32;
/// Surface formats one swapchain query may report.
constexpr uint32 MAX_SURFACE_FORMATS = 64;
/// Longest wait for a frame or present fence: five seconds.
constexpr uint64 FENCE_TIMEOUT_NANOSECONDS = 5000000000ULL;
void RasterVkDependencies(bool surface, VkSubpassDependency (&dependencies)[2]) noexcept;
struct Frame final
{
    VkCommandBuffer Command = VK_NULL_HANDLE;
    VkFence Fence = VK_NULL_HANDLE;
    VkSemaphore Acquired = VK_NULL_HANDLE;
    bool Submitted = false;
    /// Surface slot of the last frame submitted with this record (R14b).
    usize Surface = 0;
    /// The submitted frame's render completion is not yet counted for its surface.
    bool IsRenderPending = false;
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
/// @brief Presentation record of one surface (R14b): its borrowed window, swapchain or
/// hosted producer, offscreen and depth images, framebuffers, frame target and counters.
///
/// Slot 0 is the primary surface. Every surface of a session uses the primary's window
/// system, color format and surface render pass, so pipelines and cached surface passes
/// serve all of them.
struct VulkanSurface final
{
    WindowInfo Window;
    VkSurfaceKHR Surface = VK_NULL_HANDLE;
    VkSwapchainKHR Swapchain = VK_NULL_HANDLE;
    foundation::StaticArray<VkImage, IMAGES> Images{};
    foundation::StaticArray<VkImageView, IMAGES> Views{};
    foundation::StaticArray<VkFramebuffer, IMAGES> Targets{};
    foundation::StaticArray<VkSemaphore, IMAGES> Finished{};
    foundation::StaticArray<VkFence, IMAGES> PresentFences{};
    foundation::StaticArray<bool, IMAGES> Presented{};
    VkDeviceMemory HeadlessMemory = VK_NULL_HANDLE;
    VkImage DepthImage = VK_NULL_HANDLE;
    VkImageView DepthView = VK_NULL_HANDLE;
    VkDeviceMemory DepthMemory = VK_NULL_HANDLE;
    uint32 ImageCount = 0;
    uint32 Image = 0;
    VkExtent2D Extent{};
    VkExtent2D RequestedExtent{};
    FrameTarget Target;
    /// Renders a WindowSystem::Hosted window's frames into exported images. Hosted
    /// targets follow the headless layouts (VulkanState::IsHeadless is true).
    HostedProducerVulkan HostedProducer;
    /// Framebuffer of the acquired hosted image during a hosted frame.
    VkFramebuffer HostedFramebuffer = VK_NULL_HANDLE;
    /// Incremented whenever CreateTargets() recreates this surface's depth attachment.
    uint64 DepthEpoch = 0;
    bool ShouldResize = true;
    bool IsConnected = false;
    uint64 RenderedFrames = 0;
    uint64 PresentedFrames = 0;
};
/// @brief Vulkan instance, device, queue, frames, surfaces and fullscreen resources of one
/// device owner (R14c). Every owner creates its own instance and device; see AcquireLoader.
struct VulkanState final
{
    VkInstance Instance = VK_NULL_HANDLE;
    VkPhysicalDevice Physical = VK_NULL_HANDLE;
    VkDevice Device = VK_NULL_HANDLE;
    VkQueue Queue = VK_NULL_HANDLE;
    uint32 Family = 0;
    bool HasQueueCompute = false;
    bool IsRayEnabled = false;
    bool IsRayApiAvailable = false;
    bool HasUniformTextureIndexing = false;
    bool HasBindlessTextureIndexing = false;
    bool HasRuntimeTextureArrays = false;
    bool HasBindingApi = false;
    VkPhysicalDeviceAccelerationStructurePropertiesKHR RayProperties{};
    VkCommandPool Commands = VK_NULL_HANDLE;
    VkRenderPass Pass = VK_NULL_HANDLE;
    bool ShouldWaitAcquired = false;
    bool HasOwnedImage = false;
    /// Surface slot whose swapchain image HasOwnedImage refers to.
    usize OwnedImageSurface = 0;
    bool HasInstanceMaintenance = false;
    VkFormat DepthFormat = VK_FORMAT_D32_SFLOAT;
    usize FrameIndex = 0;
    foundation::StaticArray<Frame, FRAMES> Frames{};
    foundation::StaticArray<VulkanSurface, MAX_DEVICE_SURFACES> Surfaces{};
    /// Surface routed SetTarget/Begin/End; see backend::SelectSurface.
    usize ActiveSurface = 0;
    foundation::StaticArray<Shader, internal::RESOURCE_CAPACITY> Shaders{};
    foundation::StaticArray<Uniform, internal::RESOURCE_CAPACITY> Uniforms{};
    foundation::StaticArray<Pipeline, internal::RESOURCE_CAPACITY> Pipelines{};
    VkFormat Format = VK_FORMAT_UNDEFINED;
    uint32 Session = 0;
    uint32 MaxDimension = 0;
    uint32 MaxUniformSize = 0;
    bool IsHeadless = false;
    bool IsBorrowedDevice = false;
    bool HasDebugLabels = false;
    uint32 NativeInstanceVersion = VK_API_VERSION_1_1;
    bool IsEncoding = false;
    VkResult LastResult = VK_SUCCESS;
};
/// One record per device owner slot (R14c); see internal/owner.h.
foundation::StaticArray<VulkanState, MAX_DEVICE_OWNERS> gVulkanStates{};
/// VulkanState of the bound owner slot.
VulkanState& GetVulkan() noexcept
{
    return gVulkanStates[internal::GetBoundOwnerSlot()];
}
VulkanSurface& GetActiveSurface() noexcept
{
    return GetVulkan().Surfaces[GetVulkan().ActiveSurface];
}
/// @brief The VkInstance that every owner creating its own device shares (R14c).
///
/// Thanks to Khronos, Vulkan Specification, "Command Function Pointers": a command pointer
/// returned by vkGetInstanceProcAddr must only be called with that instance or one of its
/// children. volk keeps one process-wide table, so owners share one instance and dispatch
/// their devices through its loader trampolines; volk never loads one owner's device table.
/// https://docs.vulkan.org/spec/latest/chapters/initialization.html#initialization-functionpointers
/// A borrowed (native interop) device brings its own instance, so it is the only owner.
struct SharedVulkanInstance final
{
    VkInstance Object = VK_NULL_HANDLE;
    uint32 ApiVersion = VK_API_VERSION_1_1;
    bool HasInstanceMaintenance = false;
    bool HasDebugLabels = false;
    /// Owners holding Object; the last one destroys it and unloads volk.
    usize Users = 0;
};
SharedVulkanInstance gSharedInstance;
/// Instance of any live owner, borrowed or shared, or VK_NULL_HANDLE when none holds one.
VkInstance FindLiveInstance() noexcept
{
    for (const VulkanState& state : gVulkanStates)
    {
        if (state.Instance != VK_NULL_HANDLE)
        {
            return state.Instance;
        }
    }
    return VK_NULL_HANDLE;
}
/// Count a frame's render completion for its surface once its fence is known signaled.
void CountRenderedFrame(Frame& inoutFrame) noexcept
{
    if (inoutFrame.IsRenderPending == true)
    {
        ++GetVulkan().Surfaces[inoutFrame.Surface].RenderedFrames;
        inoutFrame.IsRenderPending = false;
    }
}

bool Check(VkResult result) noexcept
{
    GetVulkan().LastResult = result;
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
    return GetActiveSurface().Window.System == platform::WindowSystem::Hosted;
}
/// Framebuffer of the open frame's surface image.
VkFramebuffer SurfaceFramebuffer() noexcept
{
    return GetActiveSurface().HostedProducer.IsConnected() == true
               ? GetActiveSurface().HostedFramebuffer
               : GetActiveSurface().Targets[GetActiveSurface().Image];
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
    if (GetVulkan().Device != VK_NULL_HANDLE)
    {
        if (GetVulkan().IsBorrowedDevice || internal::NativeInteropPinned())
        {
            // A foreign queue can wait for a later host submission. Drain only
            // accepted RHI work, never the source device's unrelated submissions.
            for (Frame& frame : GetVulkan().Frames)
            {
                if (frame.Submitted && Check(vkWaitForFences(GetVulkan().Device, 1, &frame.Fence, VK_TRUE, UINT64_MAX)))
                {
                    frame.Submitted = false;
                }
            }
            WaitRasterSubmissions();
            WaitLifetimeSubmissions();
        }
        else
        {
            (void)Check(vkDeviceWaitIdle(GetVulkan().Device));
        }
    }
}
bool Allocate(const VkMemoryRequirements& requirements, VkMemoryPropertyFlags flags, VkDeviceMemory& memory) noexcept
{
    VkPhysicalDeviceMemoryProperties properties{};
    vkGetPhysicalDeviceMemoryProperties(GetVulkan().Physical, &properties);
    for (usize i = 0; i < properties.memoryTypeCount; ++i)
    {
        if ((requirements.memoryTypeBits & (1U << i)) != 0 &&
            (properties.memoryTypes[i].propertyFlags & flags) == flags)
        {
            VkMemoryAllocateInfo info{};
            info.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
            info.allocationSize = requirements.size;
            info.memoryTypeIndex = static_cast<uint32>(i);
            return Check(vkAllocateMemory(GetVulkan().Device, &info, nullptr, &memory));
        }
    }
    LUDUS_LOG_ERROR(LOG_RHI, "No compatible Vulkan memory type");
    return false;
}
void WaitPresents() noexcept
{
    if (GetVulkan().Device == VK_NULL_HANDLE)
    {
        return;
    }
    for (usize i = 0; i < IMAGES; ++i)
    {
        if (GetActiveSurface().Presented[i])
        {
            (void)Check(
                vkWaitForFences(GetVulkan().Device, 1, &GetActiveSurface().PresentFences[i], VK_TRUE, UINT64_MAX));
            GetActiveSurface().Presented[i] = false;
        }
    }
}
void RetireAcquiredImage() noexcept
{
    if (GetVulkan().HasOwnedImage == false || GetVulkan().Device == VK_NULL_HANDLE)
    {
        return;
    }
    if (GetVulkan().ShouldWaitAcquired)
    {
        // Abort may occur after acquisition but before submission. Consume the
        // asynchronous acquisition signal before releasing its semaphore.
        const VkPipelineStageFlags stage = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
        VkSubmitInfo submit{};
        submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        submit.waitSemaphoreCount = 1;
        submit.pWaitSemaphores = &GetVulkan().Frames[GetVulkan().FrameIndex].Acquired;
        submit.pWaitDstStageMask = &stage;
        Frame& frame = GetVulkan().Frames[GetVulkan().FrameIndex];
        if (Check(vkResetFences(GetVulkan().Device, 1, &frame.Fence)))
        {
            frame.Submitted = Check(vkQueueSubmit(GetVulkan().Queue, 1, &submit, frame.Fence));
        }
        GetVulkan().ShouldWaitAcquired = false;
    }
    WaitIdle();
    VkReleaseSwapchainImagesInfoEXT release{};
    release.sType = VK_STRUCTURE_TYPE_RELEASE_SWAPCHAIN_IMAGES_INFO_EXT;
    release.swapchain = GetActiveSurface().Swapchain;
    release.imageIndexCount = 1;
    release.pImageIndices = &GetActiveSurface().Image;
    (void)Check(vkReleaseSwapchainImagesEXT(GetVulkan().Device, &release));
    GetVulkan().HasOwnedImage = false;
}
void ReleaseTargets() noexcept
{
    if (GetVulkan().Device == VK_NULL_HANDLE)
    {
        return;
    }
    WaitPresents();
    for (usize i = 0; i < IMAGES; ++i)
    {
        vkDestroyFence(GetVulkan().Device, GetActiveSurface().PresentFences[i], nullptr);
        GetActiveSurface().PresentFences[i] = VK_NULL_HANDLE;
        vkDestroyFramebuffer(GetVulkan().Device, GetActiveSurface().Targets[i], nullptr);
        GetActiveSurface().Targets[i] = VK_NULL_HANDLE;
        vkDestroyImageView(GetVulkan().Device, GetActiveSurface().Views[i], nullptr);
        GetActiveSurface().Views[i] = VK_NULL_HANDLE;
        vkDestroySemaphore(GetVulkan().Device, GetActiveSurface().Finished[i], nullptr);
        GetActiveSurface().Finished[i] = VK_NULL_HANDLE;
    }
    vkDestroyImageView(GetVulkan().Device, GetActiveSurface().DepthView, nullptr);
    vkDestroyImage(GetVulkan().Device, GetActiveSurface().DepthImage, nullptr);
    vkFreeMemory(GetVulkan().Device, GetActiveSurface().DepthMemory, nullptr);
    GetActiveSurface().DepthView = VK_NULL_HANDLE;
    GetActiveSurface().DepthImage = VK_NULL_HANDLE;
    GetActiveSurface().DepthMemory = VK_NULL_HANDLE;
    if (GetVulkan().IsHeadless)
    {
        vkDestroyImage(GetVulkan().Device, GetActiveSurface().Images[0], nullptr);
        vkFreeMemory(GetVulkan().Device, GetActiveSurface().HeadlessMemory, nullptr);
        GetActiveSurface().HeadlessMemory = VK_NULL_HANDLE;
    }
    if (GetActiveSurface().Swapchain != VK_NULL_HANDLE)
    {
        vkDestroySwapchainKHR(GetVulkan().Device, GetActiveSurface().Swapchain, nullptr);
    }
    GetActiveSurface().Swapchain = VK_NULL_HANDLE;
    for (VkImage& image : GetActiveSurface().Images)
    {
        image = VK_NULL_HANDLE;
    }
    GetActiveSurface().ImageCount = 0;
}
/// @brief Retire the active surface's presentation resources (R14b).
///
/// Waits for the queue, releases an acquired image of this surface, its swapchain, images,
/// framebuffers, depth attachment and hosted producer, and destroys its VkSurfaceKHR.
/// Completion counters are kept, so a lost surface still reports what it rendered. Safe
/// after the session's device was released.
void ReleaseActiveSurface() noexcept
{
    VulkanSurface& surface = GetActiveSurface();
    if (GetVulkan().HasOwnedImage == true && GetVulkan().OwnedImageSurface == GetVulkan().ActiveSurface)
    {
        RetireAcquiredImage();
    }
    WaitIdle();
    for (Frame& frame : GetVulkan().Frames)
    {
        // WaitIdle drained every submission, so each pending frame of this surface rendered.
        if (frame.Surface == GetVulkan().ActiveSurface && GetVulkan().Device != VK_NULL_HANDLE)
        {
            CountRenderedFrame(frame);
        }
    }
    ReleaseTargets();
    if (surface.HostedProducer.IsConnected() == true)
    {
        surface.HostedProducer.Disconnect();
    }
    if (surface.Surface != VK_NULL_HANDLE && GetVulkan().Instance != VK_NULL_HANDLE)
    {
        vkDestroySurfaceKHR(GetVulkan().Instance, surface.Surface, nullptr);
    }
    surface.Surface = VK_NULL_HANDLE;
    surface.HostedFramebuffer = VK_NULL_HANDLE;
    surface.Window = {};
    surface.Target = {};
    surface.Extent = {};
    surface.RequestedExtent = {};
    surface.Image = 0;
    surface.ShouldResize = true;
    surface.IsConnected = false;
}
/// Retire every created surface (slots above the primary) and route frames to the primary.
void ReleaseCreatedSurfaces() noexcept
{
    for (usize slot = 1; slot < GetVulkan().Surfaces.GetSize(); ++slot)
    {
        if (GetVulkan().Surfaces[slot].IsConnected == true)
        {
            GetVulkan().ActiveSurface = slot;
            ReleaseActiveSurface();
        }
    }
    GetVulkan().ActiveSurface = 0;
}
/// @brief A created surface stopped accepting frames: retire only its presentation resources
/// and report it lost. The device and other surfaces keep working; no frame opens.
FrameStatus LoseActiveSurface() noexcept
{
    const usize slot = GetVulkan().ActiveSurface;
    ReleaseActiveSurface();
    internal::LoseSurface(slot);
    return FrameStatus::Skipped;
}
bool CreatePass() noexcept
{
    if (GetVulkan().Pass != VK_NULL_HANDLE)
    {
        return true;
    }
    VkFormatProperties depthProperties{};
    vkGetPhysicalDeviceFormatProperties(GetVulkan().Physical, VK_FORMAT_D32_SFLOAT, &depthProperties);
    GetVulkan().DepthFormat =
        (depthProperties.optimalTilingFeatures & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT) != 0
            ? VK_FORMAT_D32_SFLOAT
            : VK_FORMAT_D16_UNORM;
    VkAttachmentDescription attachments[2]{
        { .samples = VK_SAMPLE_COUNT_1_BIT },
        { .samples = VK_SAMPLE_COUNT_1_BIT },
    };
    auto& color = attachments[0];
    color.format = GetVulkan().Format;
    color.samples = VK_SAMPLE_COUNT_1_BIT;
    color.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    color.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    color.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    color.finalLayout = GetVulkan().IsHeadless ? VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL : VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    auto& depth = attachments[1];
    depth.format = GetVulkan().DepthFormat;
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
    return Check(vkCreateRenderPass(GetVulkan().Device, &info, nullptr, &GetVulkan().Pass));
}
bool CreateTargets() noexcept
{
    WaitIdle();
    ReleaseTargets();
    GetActiveSurface().Extent =
    {
        .width = GetActiveSurface().Target.Width,
        .height = GetActiveSurface().Target.Height,
    };
    if (IsHostedWindow() == true)
    {
        // The hosted producer owns the color images and their framebuffers; only
        // the shared depth attachment and the surface pass are created here.
        GetVulkan().Format = HOSTED_IMAGE_FORMAT;
        GetActiveSurface().ImageCount = 0;
    }
    else if (GetVulkan().IsHeadless)
    {
        GetVulkan().Format = VK_FORMAT_R8G8B8A8_UNORM;
        GetActiveSurface().ImageCount = 1;
        VkImageCreateInfo info{ .samples = VK_SAMPLE_COUNT_1_BIT };
        info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        info.imageType = VK_IMAGE_TYPE_2D;
        info.format = GetVulkan().Format;
        info.extent =
        {
            .width = GetActiveSurface().Extent.width,
            .height = GetActiveSurface().Extent.height,
            .depth = 1,
        };
        info.mipLevels = 1;
        info.arrayLayers = 1;
        info.samples = VK_SAMPLE_COUNT_1_BIT;
        info.tiling = VK_IMAGE_TILING_OPTIMAL;
        info.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
        if (Check(vkCreateImage(GetVulkan().Device, &info, nullptr, &GetActiveSurface().Images[0])) == false)
        {
            return false;
        }
        VkMemoryRequirements requirements{};
        vkGetImageMemoryRequirements(GetVulkan().Device, GetActiveSurface().Images[0], &requirements);
        if (Allocate(requirements, 0, GetActiveSurface().HeadlessMemory) == false ||
            Check(vkBindImageMemory(GetVulkan().Device,
                                    GetActiveSurface().Images[0],
                                    GetActiveSurface().HeadlessMemory,
                                    0)) == false)
        {
            return false;
        }
    }
    else
    {
        VkSurfaceCapabilitiesKHR caps{ .currentTransform = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR };
        if (Check(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(GetVulkan().Physical, GetActiveSurface().Surface, &caps)) ==
            false)
        {
            return false;
        }
        if (caps.currentExtent.width != UINT32_MAX)
        {
            GetActiveSurface().Extent = caps.currentExtent;
        }
        else
        {
            GetActiveSurface().Extent.width = foundation::math::Clamp(GetActiveSurface().Extent.width,
                                                                      caps.minImageExtent.width,
                                                                      caps.maxImageExtent.width);
            GetActiveSurface().Extent.height = foundation::math::Clamp(GetActiveSurface().Extent.height,
                                                                       caps.minImageExtent.height,
                                                                       caps.maxImageExtent.height);
        }
        uint32 count = 0;
        if (Check(vkGetPhysicalDeviceSurfaceFormatsKHR(GetVulkan().Physical,
                                                       GetActiveSurface().Surface,
                                                       &count,
                                                       nullptr)) == false ||
            count == 0 || count > MAX_SURFACE_FORMATS)
        {
            return false;
        }
        foundation::StaticArray<VkSurfaceFormatKHR, MAX_SURFACE_FORMATS> formats{};
        if (Check(vkGetPhysicalDeviceSurfaceFormatsKHR(GetVulkan().Physical,
                                                       GetActiveSurface().Surface,
                                                       &count,
                                                       formats.GetData())) == false)
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
        if (GetVulkan().Format != VK_FORMAT_UNDEFINED && GetVulkan().Format != selected.format)
        {
            return false;
        }
        GetVulkan().Format = selected.format;
        // A minimized surface can negotiate zero pixels. Its format still
        // defines a valid pipeline; defer framebuffer/swapchain creation.
        if (GetActiveSurface().Extent.width == 0 || GetActiveSurface().Extent.height == 0)
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
        info.surface = GetActiveSurface().Surface;
        info.minImageCount = desired;
        info.imageFormat = GetVulkan().Format;
        info.imageColorSpace = selected.colorSpace;
        info.imageExtent = GetActiveSurface().Extent;
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
        if (Check(vkCreateSwapchainKHR(GetVulkan().Device, &info, nullptr, &GetActiveSurface().Swapchain)) == false ||
            Check(vkGetSwapchainImagesKHR(GetVulkan().Device,
                                          GetActiveSurface().Swapchain,
                                          &GetActiveSurface().ImageCount,
                                          nullptr)) == false ||
            GetActiveSurface().ImageCount > IMAGES ||
            Check(vkGetSwapchainImagesKHR(GetVulkan().Device,
                                          GetActiveSurface().Swapchain,
                                          &GetActiveSurface().ImageCount,
                                          GetActiveSurface().Images.GetData())) == false)
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
    depthImage.format = GetVulkan().DepthFormat;
    depthImage.extent = {GetActiveSurface().Extent.width, GetActiveSurface().Extent.height, 1};
    depthImage.mipLevels = 1;
    depthImage.arrayLayers = 1;
    depthImage.samples = VK_SAMPLE_COUNT_1_BIT;
    depthImage.tiling = VK_IMAGE_TILING_OPTIMAL;
    depthImage.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
    ++GetActiveSurface().DepthEpoch;
    if (Check(vkCreateImage(GetVulkan().Device, &depthImage, nullptr, &GetActiveSurface().DepthImage)) == false)
    {
        return false;
    }
    VkMemoryRequirements depthMemory{};
    vkGetImageMemoryRequirements(GetVulkan().Device, GetActiveSurface().DepthImage, &depthMemory);
    if (Allocate(depthMemory, 0, GetActiveSurface().DepthMemory) == false ||
        Check(
            vkBindImageMemory(GetVulkan().Device, GetActiveSurface().DepthImage, GetActiveSurface().DepthMemory, 0)) ==
            false)
    {
        return false;
    }
    VkImageViewCreateInfo depthView{};
    depthView.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    depthView.image = GetActiveSurface().DepthImage;
    depthView.viewType = VK_IMAGE_VIEW_TYPE_2D;
    depthView.format = GetVulkan().DepthFormat;
    depthView.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
    depthView.subresourceRange.levelCount = 1;
    depthView.subresourceRange.layerCount = 1;
    if (Check(vkCreateImageView(GetVulkan().Device, &depthView, nullptr, &GetActiveSurface().DepthView)) == false)
    {
        return false;
    }
    for (usize i = 0; i < GetActiveSurface().ImageCount; ++i)
    {
        VkImageViewCreateInfo view{};
        view.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        view.image = GetActiveSurface().Images[i];
        view.viewType = VK_IMAGE_VIEW_TYPE_2D;
        view.format = GetVulkan().Format;
        view.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        view.subresourceRange.levelCount = 1;
        view.subresourceRange.layerCount = 1;
        if (Check(vkCreateImageView(GetVulkan().Device, &view, nullptr, &GetActiveSurface().Views[i])) == false)
        {
            return false;
        }
        VkFramebufferCreateInfo target{};
        target.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
        target.renderPass = GetVulkan().Pass;
        const VkImageView attachments[2]{GetActiveSurface().Views[i], GetActiveSurface().DepthView};
        target.attachmentCount = 2;
        target.pAttachments = attachments;
        target.width = GetActiveSurface().Extent.width;
        target.height = GetActiveSurface().Extent.height;
        target.layers = 1;
        if (Check(vkCreateFramebuffer(GetVulkan().Device, &target, nullptr, &GetActiveSurface().Targets[i])) == false)
        {
            return false;
        }
        if (GetVulkan().IsHeadless == false)
        {
            VkSemaphoreCreateInfo semaphore{};
            semaphore.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
            VkFenceCreateInfo fence{};
            fence.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
            if (Check(vkCreateFence(GetVulkan().Device, &fence, nullptr, &GetActiveSurface().PresentFences[i])) ==
                    false ||
                Check(vkCreateSemaphore(GetVulkan().Device, &semaphore, nullptr, &GetActiveSurface().Finished[i])) ==
                    false)
            {
                return false;
            }
        }
    }
    GetActiveSurface().ShouldResize = false;
    return true;
}
FrameStatus Failure() noexcept
{
    internal::Fail(GetVulkan().Session,
                   GetVulkan().LastResult == VK_ERROR_DEVICE_LOST ? StartupError::DeviceLost
                                                                  : StartupError::RenderingUnavailable);
    return FrameStatus::Failed;
}
ResourceStatus ResourceFailure() noexcept
{
    if (GetVulkan().LastResult == VK_ERROR_DEVICE_LOST)
    {
        internal::Fail(GetVulkan().Session, StartupError::DeviceLost);
    }
    return ResourceStatus::Failed;
}
// R14a adapter catalog. Thanks to Khronos Group, Vulkan Specification,
// "VkPhysicalDeviceIDProperties" and "VkPhysicalDeviceProperties", for the stable
// device UUID and the raw adapter properties reported here:
// https://docs.vulkan.org/refpages/latest/refpages/source/VkPhysicalDeviceIDProperties.html
// https://docs.vulkan.org/refpages/latest/refpages/source/VkPhysicalDeviceProperties.html
static_assert(VK_UUID_SIZE == ADAPTER_IDENTITY_BYTES);
AdapterIdentity GetVulkanIdentity(VkPhysicalDevice device) noexcept
{
    AdapterIdentity identity{ .Api = Backend::Vulkan };
    VkPhysicalDeviceProperties properties{};
    vkGetPhysicalDeviceProperties(device, &properties);
    // Vulkan 1.0 adapters keep a zero identity: they cannot be selected explicitly.
    if (properties.apiVersion >= VK_API_VERSION_1_1 && vkGetPhysicalDeviceProperties2 != nullptr)
    {
        platform::HostedAdapterIdentity uuids;
        ReadHostedAdapterIdentity(device, uuids);
        std::memcpy(identity.Bytes.GetData(), uuids.DeviceUuid.GetData(), ADAPTER_IDENTITY_BYTES);
    }
    return identity;
}
AdapterKind GetVulkanKind(VkPhysicalDeviceType type) noexcept
{
    switch (type)
    {
        case VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU:
            return AdapterKind::Integrated;
        case VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU:
            return AdapterKind::Discrete;
        case VK_PHYSICAL_DEVICE_TYPE_VIRTUAL_GPU:
            return AdapterKind::Virtual;
        case VK_PHYSICAL_DEVICE_TYPE_CPU:
            return AdapterKind::Software;
        default:
            return AdapterKind::Unknown;
    }
}
bool HasGraphicsQueue(VkPhysicalDevice device) noexcept
{
    uint32 count = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(device, &count, nullptr);
    count = count < MAX_QUEUE_FAMILIES ? count : MAX_QUEUE_FAMILIES;
    StaticArray<VkQueueFamilyProperties, MAX_QUEUE_FAMILIES> families{};
    vkGetPhysicalDeviceQueueFamilyProperties(device, &count, families.GetData());
    for (uint32 i = 0; i < count; ++i)
    {
        if (families[i].queueCount != 0 && (families[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) != 0)
        {
            return true;
        }
    }
    return false;
}
void DescribeVulkanAdapter(VkPhysicalDevice device, AdapterInfo& outAdapter) noexcept
{
    VkPhysicalDeviceProperties properties{};
    vkGetPhysicalDeviceProperties(device, &properties);
    outAdapter = {};
    outAdapter.Identity = GetVulkanIdentity(device);
    internal::AssignAdapterName(
        {properties.deviceName, strnlen(properties.deviceName, VK_MAX_PHYSICAL_DEVICE_NAME_SIZE)},
        outAdapter.Name);
    outAdapter.Kind = GetVulkanKind(properties.deviceType);
    outAdapter.VendorId = properties.vendorID;
    outAdapter.DeviceId = properties.deviceID;
    outAdapter.MaxTextureDimension2D = properties.limits.maxImageDimension2D;
    VkPhysicalDeviceMemoryProperties memory{};
    vkGetPhysicalDeviceMemoryProperties(device, &memory);
    for (uint32 i = 0; i < memory.memoryHeapCount && i < VK_MAX_MEMORY_HEAPS; ++i)
    {
        if ((memory.memoryHeaps[i].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) != 0)
        {
            outAdapter.LocalMemoryBytes += memory.memoryHeaps[i].size;
        }
    }
    outAdapter.HasBackendBaseline = outAdapter.Identity.IsAutomatic() == false && HasGraphicsQueue(device) == true;
}
AdapterStatus ListAdapters(VkInstance instance, AdapterCatalog& outCatalog) noexcept
{
    uint32 count = 0;
    if (vkEnumeratePhysicalDevices(instance, &count, nullptr) != VK_SUCCESS)
    {
        return AdapterStatus::Failed;
    }
    StaticArray<VkPhysicalDevice, MAX_PHYSICAL_DEVICES> devices{};
    uint32 listed = count < MAX_PHYSICAL_DEVICES ? count : MAX_PHYSICAL_DEVICES;
    const VkResult result = vkEnumeratePhysicalDevices(instance, &listed, devices.GetData());
    if (result != VK_SUCCESS && result != VK_INCOMPLETE)
    {
        return AdapterStatus::Failed;
    }
    outCatalog = { .Api = Backend::Vulkan };
    for (uint32 i = 0; i < listed; ++i)
    {
        AdapterInfo* entryOrNull = internal::ReserveCatalogEntryOrNull(outCatalog);
        if (entryOrNull != nullptr)
        {
            DescribeVulkanAdapter(devices[i], *entryOrNull);
        }
    }
    outCatalog.ReportedCount = count > outCatalog.ReportedCount ? count : outCatalog.ReportedCount;
    return AdapterStatus::Ready;
}
/// @brief Create the shared owned instance and load volk from it (R14c); see
/// SharedVulkanInstance. The first owner's application name and version describe it.
bool CreateSharedInstance(const ApplicationInfo& app) noexcept
{
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
        availableCount > MAX_AVAILABLE_INSTANCE_EXTENSIONS)
    {
        volkFinalize();
        return false;
    }
    VkExtensionProperties available[MAX_AVAILABLE_INSTANCE_EXTENSIONS]{};
    if (Check(vkEnumerateInstanceExtensionProperties(nullptr, &availableCount, available)) == false)
    {
        volkFinalize();
        return false;
    }
    bool hasSurfaceCapabilities2 = false;
    bool hasSurfaceMaintenance = false;
    for (usize i = 0; i < availableCount; ++i)
    {
        hasSurfaceCapabilities2 =
            hasSurfaceCapabilities2 == true ||
            std::strcmp(available[i].extensionName, VK_KHR_GET_SURFACE_CAPABILITIES_2_EXTENSION_NAME) == 0;
        hasSurfaceMaintenance =
            hasSurfaceMaintenance == true ||
            std::strcmp(available[i].extensionName, VK_EXT_SURFACE_MAINTENANCE_1_EXTENSION_NAME) == 0;
    }
    gSharedInstance.HasInstanceMaintenance = hasSurfaceCapabilities2 == true && hasSurfaceMaintenance == true;
    if (gSharedInstance.HasInstanceMaintenance)
    {
        extensions[extensionCount++] = VK_KHR_GET_SURFACE_CAPABILITIES_2_EXTENSION_NAME;
        extensions[extensionCount++] = VK_EXT_SURFACE_MAINTENANCE_1_EXTENSION_NAME;
    }
#endif
    uint32 propertyCount = 0;
    gSharedInstance.HasDebugLabels = false;
    if (vkEnumerateInstanceExtensionProperties(nullptr, &propertyCount, nullptr) == VK_SUCCESS &&
        propertyCount <= MAX_AVAILABLE_INSTANCE_EXTENSIONS)
    {
        StaticArray<VkExtensionProperties, MAX_AVAILABLE_INSTANCE_EXTENSIONS> properties;
        if (vkEnumerateInstanceExtensionProperties(nullptr, &propertyCount, properties.GetData()) == VK_SUCCESS)
        {
            for (usize index = 0; index < propertyCount; ++index)
            {
                if (std::strcmp(properties[index].extensionName, VK_EXT_DEBUG_UTILS_EXTENSION_NAME) == 0)
                {
                    extensions[extensionCount++] = VK_EXT_DEBUG_UTILS_EXTENSION_NAME;
                    gSharedInstance.HasDebugLabels = true;
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
    // Owners created later share this instance, so it carries the version any owner's ray or
    // binding-model profile needs; each device still enables only what its owner requested.
    application.apiVersion = loaderVersion >= VK_API_VERSION_1_2 ? VK_API_VERSION_1_2 : VK_API_VERSION_1_1;
    gSharedInstance.ApiVersion = application.apiVersion;
    VkInstanceCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    info.pApplicationInfo = &application;
    info.enabledExtensionCount = extensionCount;
    info.ppEnabledExtensionNames = extensions.GetData();
    if (Check(vkCreateInstance(&info, nullptr, &gSharedInstance.Object)) == false)
    {
        gSharedInstance = {};
        volkFinalize();
        return false;
    }
    volkLoadInstance(gSharedInstance.Object);
    return true;
}
} // namespace
Backend Kind() noexcept
{
    return Backend::Vulkan;
}
AdapterStatus EnumerateAdapters(AdapterCatalog& outCatalog) noexcept
{
    // Any live owner's instance enumerates the same adapters; reloading volk for a temporary
    // instance would replace the function table that owner is dispatching through.
    const VkInstance live = FindLiveInstance();
    if (live != VK_NULL_HANDLE)
    {
        return ListAdapters(live, outCatalog);
    }
    // Without a session, list through a temporary instance and unload it again, so the
    // next Initialize loads the loader and its instance functions from a clean state.
    if (volkInitialize() != VK_SUCCESS)
    {
        return AdapterStatus::Failed;
    }
    uint32 loaderVersion = VK_API_VERSION_1_0;
    if (vkEnumerateInstanceVersion != nullptr)
    {
        static_cast<void>(vkEnumerateInstanceVersion(&loaderVersion));
    }
    const VkApplicationInfo application
    {
        .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
        .apiVersion = loaderVersion >= VK_API_VERSION_1_1 ? VK_API_VERSION_1_1 : VK_API_VERSION_1_0,
    };
    const VkInstanceCreateInfo info
    {
        .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
        .pApplicationInfo = &application,
    };
    VkInstance instance = VK_NULL_HANDLE;
    if (vkCreateInstance(&info, nullptr, &instance) != VK_SUCCESS)
    {
        volkFinalize();
        return AdapterStatus::Failed;
    }
    volkLoadInstance(instance);
    const AdapterStatus status = ListAdapters(instance, outCatalog);
    vkDestroyInstance(instance, nullptr);
    volkFinalize();
    return status;
}
bool GetSelectedAdapter(AdapterInfo& outAdapter) noexcept
{
    if (GetVulkan().Physical == VK_NULL_HANDLE)
    {
        return false;
    }
    DescribeVulkanAdapter(GetVulkan().Physical, outAdapter);
    return true;
}
bool Supports(BackendSelection selection) noexcept
{
    // Native builds only ever run Vulkan; a forced browser backend fails
    // explicitly rather than silently falling back to Vulkan.
    return selection == BackendSelection::Auto;
}
static bool InitializeInstance(const ApplicationInfo& app) noexcept
{
    if (const auto* imported = internal::NativeDeviceRequest())
    {
        if (volkInitialize() != VK_SUCCESS)
        {
            return false;
        }
        GetVulkan().IsBorrowedDevice = true;
        GetVulkan().Instance = static_cast<VkInstance>(imported->Identity.Instance.GetObject());
        volkLoadInstance(GetVulkan().Instance);
        GetVulkan().Device = static_cast<VkDevice>(imported->Identity.Device.GetObject());
        GetVulkan().Physical = static_cast<VkPhysicalDevice>(imported->Identity.PhysicalDevice.GetObject());
        GetVulkan().Queue = static_cast<VkQueue>(imported->Identity.CommandQueue.GetObject());
        GetVulkan().Family = imported->Identity.QueueFamily;
        GetVulkan().NativeInstanceVersion = imported->InstanceApiVersion;
        volkLoadDevice(GetVulkan().Device);
        // Only an extension the source declares enabled may be called; otherwise duration-only.
        ConnectClockCalibration(
            AdmitClockCalibrationExtensionOrNull(GetVulkan().Physical, imported->CalibratedTimestamps));
        uint32 count = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(GetVulkan().Physical, &count, nullptr);
        if (count > MAX_QUEUE_FAMILIES || GetVulkan().Family >= count)
        {
            return false;
        }
        StaticArray<VkQueueFamilyProperties, MAX_QUEUE_FAMILIES> families{};
        vkGetPhysicalDeviceQueueFamilyProperties(GetVulkan().Physical, &count, families.GetData());
        if ((families[GetVulkan().Family].queueFlags & VK_QUEUE_GRAPHICS_BIT) == 0 ||
            imported->Identity.QueueIndex >= families[GetVulkan().Family].queueCount)
        {
            return false;
        }
        // The source contract guarantees that this queue was created on this device.
        VkQueue actual = VK_NULL_HANDLE;
        vkGetDeviceQueue(GetVulkan().Device, GetVulkan().Family, imported->Identity.QueueIndex, &actual);
        if (actual != GetVulkan().Queue)
        {
            return false;
        }
        VkPhysicalDeviceProperties properties{};
        vkGetPhysicalDeviceProperties(GetVulkan().Physical, &properties);
        if (properties.apiVersion < VK_API_VERSION_1_1)
        {
            return false;
        }
        GetVulkan().MaxDimension = properties.limits.maxImageDimension2D;
        GetVulkan().MaxUniformSize = properties.limits.maxUniformBufferRange;
        GetVulkan().HasQueueCompute = (families[GetVulkan().Family].queueFlags & VK_QUEUE_COMPUTE_BIT) != 0;
        GetVulkan().IsRayEnabled = GetVulkan().IsRayApiAvailable = false;
        GetVulkan().HasUniformTextureIndexing = false;
        GetVulkan().HasBindlessTextureIndexing = false;
        GetVulkan().HasRuntimeTextureArrays = false;
        GetVulkan().HasBindingApi = false;
        GetVulkan().IsHeadless = true;
        return true;
    }
    if (GetVulkan().Instance != VK_NULL_HANDLE)
    {
        return true;
    }
    if (gSharedInstance.Users == 0 && CreateSharedInstance(app) == false)
    {
        return false;
    }
    ++gSharedInstance.Users;
    GetVulkan().Instance = gSharedInstance.Object;
    GetVulkan().HasInstanceMaintenance = gSharedInstance.HasInstanceMaintenance;
    GetVulkan().HasDebugLabels = gSharedInstance.HasDebugLabels;
    GetVulkan().NativeInstanceVersion = gSharedInstance.ApiVersion;
    const bool hasVersion12 = gSharedInstance.ApiVersion >= VK_API_VERSION_1_2;
    GetVulkan().IsRayApiAvailable = internal::RequestsRayProfile() == true && hasVersion12 == true;
    GetVulkan().HasBindingApi =
        (internal::IsBindlessRequested() == true || internal::IsRuntimeTextureArrayRequested() == true) &&
        hasVersion12 == true;
    return true;
}
static bool ConnectPrimarySurface(const WindowInfo& window) noexcept
{
    if (GetVulkan().Instance == VK_NULL_HANDLE)
    {
        return false;
    }
    // Startup connects only the primary surface.
    GetVulkan().ActiveSurface = 0;
    if (GetVulkan().IsBorrowedDevice && GetVulkan().Pass == VK_NULL_HANDLE)
    {
        if (window.System == platform::WindowSystem::Hosted)
        {
            // A borrowed device has no exportable-memory extension negotiated by RHI.
            return false;
        }
        GetActiveSurface().Window = window;
        GetVulkan().IsHeadless = true;
        GetActiveSurface().Target.Width = window.Width;
        GetActiveSurface().Target.Height = window.Height;
        if (window.Width == 0 || window.Height == 0 || window.Width > GetVulkan().MaxDimension ||
            window.Height > GetVulkan().MaxDimension)
        {
            return false;
        }
        GetActiveSurface().RequestedExtent = { .width = window.Width, .height = window.Height };
        return CreateTargets();
    }
    if (GetVulkan().Device != VK_NULL_HANDLE)
    {
        return GetActiveSurface().Window.System == window.System &&
               GetActiveSurface().Window.Display == window.Display &&
               GetActiveSurface().Window.Surface == window.Surface;
    }
    GetActiveSurface().Window = window;
    if (IsHostedWindow() == true && window.HostedSurfaceOrNull == nullptr)
    {
        return false;
    }
    GetVulkan().IsHeadless = window.System == platform::WindowSystem::Headless || IsHostedWindow() == true;
    if (GetVulkan().IsHeadless == false)
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
        if (Check(vkCreateWaylandSurfaceKHR(GetVulkan().Instance, &info, nullptr, &GetActiveSurface().Surface)) ==
            false)
        {
            return false;
        }
#else
        return false;
#endif
    }
    uint32 count = 0;
    if (Check(vkEnumeratePhysicalDevices(GetVulkan().Instance, &count, nullptr)) == false || count == 0 ||
        count > MAX_PHYSICAL_DEVICES)
    {
        return false;
    }
    foundation::StaticArray<VkPhysicalDevice, MAX_PHYSICAL_DEVICES> devices{};
    if (Check(vkEnumeratePhysicalDevices(GetVulkan().Instance, &count, devices.GetData())) == false)
    {
        return false;
    }
    const auto connectAdapter = [&window]() noexcept {
        VkPhysicalDeviceProperties properties{};
        vkGetPhysicalDeviceProperties(GetVulkan().Physical, &properties);
        if (properties.apiVersion < VK_API_VERSION_1_1)
        {
            LUDUS_LOG_ERROR(LOG_RHI, "Vulkan 1.1 is required for the generated SPIR-V profile");
            return false;
        }
        if (IsHostedWindow() == true && HasDeviceExtension(GetVulkan().Physical, HOSTED_MEMORY_EXTENSION_NAME) == false)
        {
            // Filter before committing to the adapter, so a later capable one is still considered.
            LUDUS_LOG_ERROR(LOG_RHI, "A hosted window needs VK_KHR_external_memory_fd on the adapter");
            return false;
        }
        GetVulkan().MaxDimension = properties.limits.maxImageDimension2D;
        const uint32 dimensionLimits[] = {properties.limits.maxFramebufferWidth,
                                          properties.limits.maxFramebufferHeight,
                                          properties.limits.maxViewportDimensions[0],
                                          properties.limits.maxViewportDimensions[1]};
        for (const auto limit : dimensionLimits)
        {
            if (limit < GetVulkan().MaxDimension)
            {
                GetVulkan().MaxDimension = limit;
            }
        }
        GetVulkan().MaxUniformSize = properties.limits.maxUniformBufferRange;
        const float32 priority = 1;
        VkDeviceQueueCreateInfo queue{};
        queue.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
        queue.queueFamilyIndex = GetVulkan().Family;
        queue.queueCount = 1;
        queue.pQueuePriorities = &priority;
        // Swapchain (two), ray queries (three), the hosted memory extension (one) or
        // calibrated timestamps (one).
        constexpr usize MAX_ENABLED_DEVICE_EXTENSIONS = 7;
        const char* extensions[MAX_ENABLED_DEVICE_EXTENSIONS] = {VK_KHR_SWAPCHAIN_EXTENSION_NAME,
                                                                 VK_EXT_SWAPCHAIN_MAINTENANCE_1_EXTENSION_NAME};
        uint32 enabledExtensions = GetVulkan().IsHeadless ? 0U : 2U;
        if (IsHostedWindow() == true)
        {
            extensions[enabledExtensions++] = HOSTED_MEMORY_EXTENSION_NAME;
        }
        // Optional: places timed passes on the CPU trace clock; absence keeps them duration-only.
        const char* clockCalibrationExtensionOrNull =
            AdmitClockCalibrationExtensionOrNull(GetVulkan().Physical, NativeCalibratedTimestamps::Khr);
        if (clockCalibrationExtensionOrNull == nullptr)
        {
            clockCalibrationExtensionOrNull =
                AdmitClockCalibrationExtensionOrNull(GetVulkan().Physical, NativeCalibratedTimestamps::Ext);
        }
        if (clockCalibrationExtensionOrNull != nullptr)
        {
            extensions[enabledExtensions++] = clockCalibrationExtensionOrNull;
        }
        VkPhysicalDeviceSwapchainMaintenance1FeaturesEXT maintenance{};
        maintenance.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SWAPCHAIN_MAINTENANCE_1_FEATURES_EXT;
        if (GetVulkan().IsHeadless == false)
        {
            uint32 countExtensions = 0;
            if (GetVulkan().HasInstanceMaintenance == false ||
                Check(vkEnumerateDeviceExtensionProperties(GetVulkan().Physical, nullptr, &countExtensions, nullptr)) ==
                    false ||
                countExtensions > 512)
            {
                LUDUS_LOG_ERROR(LOG_RHI,
                                "Wayland rendering requires surface/swapchain maintenance1 for safe retirement");
                return false;
            }
            VkExtensionProperties propertiesExtensions[512]{};
            if (Check(vkEnumerateDeviceExtensionProperties(GetVulkan().Physical,
                                                           nullptr,
                                                           &countExtensions,
                                                           propertiesExtensions)) == false)
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
            vkGetPhysicalDeviceFeatures2(GetVulkan().Physical, &features);
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
        GetVulkan().IsRayEnabled = false;
        if (GetVulkan().IsRayApiAvailable && properties.apiVersion >= VK_API_VERSION_1_2 && GetVulkan().HasQueueCompute)
        {
            uint32 extensionCount = 0;
            if (vkEnumerateDeviceExtensionProperties(GetVulkan().Physical, nullptr, &extensionCount, nullptr) ==
                    VK_SUCCESS &&
                extensionCount <= 512)
            {
                VkExtensionProperties available[512]{};
                if (vkEnumerateDeviceExtensionProperties(GetVulkan().Physical, nullptr, &extensionCount, available) ==
                    VK_SUCCESS)
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
                        vkGetPhysicalDeviceFeatures2(GetVulkan().Physical, &features);
                        GetVulkan().IsRayEnabled =
                            address.bufferDeviceAddress && structures.accelerationStructure && queries.rayQuery;
                        if (GetVulkan().IsRayEnabled)
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
                            queries.pNext = GetVulkan().IsHeadless ? nullptr : &maintenance;
                            GetVulkan().RayProperties =
                            {
                                .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_PROPERTIES_KHR,
                            };
                            VkPhysicalDeviceProperties2 rayProperties
                            {
                                .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2,
                                .pNext = &GetVulkan().RayProperties,
                            };
                            vkGetPhysicalDeviceProperties2(GetVulkan().Physical, &rayProperties);
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
        GetVulkan().HasBindlessTextureIndexing = false;
        GetVulkan().HasRuntimeTextureArrays = false;
        if (GetVulkan().HasBindingApi == true && properties.apiVersion >= VK_API_VERSION_1_2)
        {
            VkPhysicalDeviceFeatures2 features
            {
                .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2,
                .pNext = &indexing,
            };
            vkGetPhysicalDeviceFeatures2(GetVulkan().Physical, &features);
            GetVulkan().HasBindlessTextureIndexing = internal::IsBindlessRequested() == true &&
                                                     indexing.shaderSampledImageArrayNonUniformIndexing == VK_TRUE;
            GetVulkan().HasRuntimeTextureArrays = internal::IsRuntimeTextureArrayRequested() == true &&
                                                  indexing.runtimeDescriptorArray == VK_TRUE &&
                                                  indexing.descriptorBindingVariableDescriptorCount == VK_TRUE;
        }
        if ((internal::IsBindlessRequired() == true && GetVulkan().HasBindlessTextureIndexing == false) ||
            (internal::IsRuntimeTextureArrayRequired() == true && GetVulkan().HasRuntimeTextureArrays == false))
        {
            // Required profiles filter candidates before committing native device
            // ownership, so a later supported adapter is still considered.
            return false;
        }
        indexing =
        {
            .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_INDEXING_FEATURES,
            .shaderSampledImageArrayNonUniformIndexing =
                GetVulkan().HasBindlessTextureIndexing == true ? VK_TRUE : VK_FALSE,
            .descriptorBindingVariableDescriptorCount =
                GetVulkan().HasRuntimeTextureArrays == true ? VK_TRUE : VK_FALSE,
            .runtimeDescriptorArray = GetVulkan().HasRuntimeTextureArrays == true ? VK_TRUE : VK_FALSE,
        };
        void* featureChain = GetVulkan().IsRayEnabled == true ? static_cast<void*>(&address)
                             : GetVulkan().IsHeadless == true ? nullptr
                                                              : static_cast<void*>(&maintenance);
        if (GetVulkan().HasBindlessTextureIndexing == true || GetVulkan().HasRuntimeTextureArrays == true)
        {
            indexing.pNext = featureChain;
            featureChain = &indexing;
        }
        VkPhysicalDeviceFeatures supported{};
        vkGetPhysicalDeviceFeatures(GetVulkan().Physical, &supported);
        const VkPhysicalDeviceFeatures enabled
        {
            // R12a GPU-written nonzero firstInstance; reported only when enabled here.
            .drawIndirectFirstInstance = supported.drawIndirectFirstInstance,
            .fillModeNonSolid = supported.fillModeNonSolid,
            .textureCompressionBC = supported.textureCompressionBC,
            // R12b fragment storage writes; reported only when enabled here.
            .fragmentStoresAndAtomics = supported.fragmentStoresAndAtomics,
            .shaderSampledImageArrayDynamicIndexing = supported.shaderSampledImageArrayDynamicIndexing,
        };
        GetVulkan().HasUniformTextureIndexing = enabled.shaderSampledImageArrayDynamicIndexing == VK_TRUE;
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
        if (Check(vkCreateDevice(GetVulkan().Physical, &device, nullptr, &GetVulkan().Device)) == false)
        {
            return false;
        }
        // No volkLoadDevice: other owners' devices dispatch through the same table
        // (SharedVulkanInstance), so device commands keep the instance's loader trampolines.
        ConnectClockCalibration(clockCalibrationExtensionOrNull);
        vkGetDeviceQueue(GetVulkan().Device, GetVulkan().Family, 0, &GetVulkan().Queue);
        SetVulkanObjectName(VK_OBJECT_TYPE_QUEUE, reinterpret_cast<uint64>(GetVulkan().Queue), "Ludus/Queue");
        if (IsHostedWindow() == true)
        {
            GetActiveSurface().HostedProducer.Connect(
                {
                    .PhysicalDevice = GetVulkan().Physical,
                    .Device = GetVulkan().Device,
                    .QueueFamily = GetVulkan().Family,
                },
                *window.HostedSurfaceOrNull);
        }
        GetActiveSurface().Target.Width = window.Width;
        GetActiveSurface().Target.Height = window.Height;
        if (GetActiveSurface().Target.Width == 0 || GetActiveSurface().Target.Height == 0 ||
            GetActiveSurface().Target.Width > GetVulkan().MaxDimension ||
            GetActiveSurface().Target.Height > GetVulkan().MaxDimension)
        {
            return false;
        }
        GetActiveSurface().RequestedExtent =
        {
            .width = GetActiveSurface().Target.Width,
            .height = GetActiveSurface().Target.Height,
        };
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
    // R14a: an explicit catalog identity restricts selection to that adapter.
    const AdapterIdentity* requestedOrNull = internal::GetRequestedAdapterOrNull();
    for (usize position = 0; position < count; ++position)
    {
        const usize i = order[position];
        if (requestedOrNull != nullptr && GetVulkanIdentity(devices[i]) != *requestedOrNull)
        {
            continue;
        }
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
            if (GetVulkan().IsHeadless == false &&
                Check(vkGetPhysicalDeviceSurfaceSupportKHR(devices[i],
                                                           static_cast<uint32>(j),
                                                           GetActiveSurface().Surface,
                                                           &present)) == false)
            {
                continue;
            }
            if (properties[j].queueCount != 0 && (properties[j].queueFlags & VK_QUEUE_GRAPHICS_BIT) != 0 &&
                present == VK_TRUE)
            {
                GetVulkan().Physical = devices[i];
                GetVulkan().Family = static_cast<uint32>(j);
                GetVulkan().HasQueueCompute = (properties[j].queueFlags & VK_QUEUE_COMPUTE_BIT) != 0;
                // Required workloads filter candidates before committing to a
                // queue/adapter, so a later usable candidate remains reachable.
                if (internal::RequiresComputeProfile() && ComputeLimits().MaxStorageRange == 0)
                {
                    GetVulkan().Physical = VK_NULL_HANDLE;
                    continue;
                }
                break;
            }
        }
        if (GetVulkan().Physical != VK_NULL_HANDLE)
        {
            if (connectAdapter())
            {
                return true;
            }
            ReleaseTargets();
            GetActiveSurface().HostedProducer.Disconnect();
            if (GetVulkan().Device != VK_NULL_HANDLE)
            {
                vkDestroyRenderPass(GetVulkan().Device, GetVulkan().Pass, nullptr);
                vkDestroyDevice(GetVulkan().Device, nullptr);
            }
            GetVulkan().Device = VK_NULL_HANDLE;
            GetVulkan().Physical = VK_NULL_HANDLE;
            GetVulkan().Queue = VK_NULL_HANDLE;
            GetVulkan().Pass = VK_NULL_HANDLE;
            GetVulkan().Format = VK_FORMAT_UNDEFINED;
            GetVulkan().IsEncoding = false;
        }
    }
    return false;
}

// Thanks to Khronos, Vulkan Specification, vkSetDebugUtilsObjectNameEXT:
// https://docs.vulkan.org/refpages/latest/refpages/source/vkSetDebugUtilsObjectNameEXT.html
// An empty name removes the object's name. Absent VK_EXT_debug_utils keeps objects unnamed.
void SetVulkanObjectName(VkObjectType type, uint64 object, const char* name) noexcept
{
    if (object == 0 || GetVulkan().Device == VK_NULL_HANDLE || GetVulkan().HasDebugLabels == false ||
        vkSetDebugUtilsObjectNameEXT == nullptr)
    {
        return;
    }
    const VkDebugUtilsObjectNameInfoEXT information
    {
        .sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_OBJECT_NAME_INFO_EXT,
        .objectType = type,
        .objectHandle = object,
        .pObjectName = name,
    };
    static_cast<void>(vkSetDebugUtilsObjectNameEXT(GetVulkan().Device, &information));
}
static bool InitializeFrameResources() noexcept
{
    if (GetVulkan().Device == VK_NULL_HANDLE)
    {
        return false;
    }
    if (GetVulkan().Commands != VK_NULL_HANDLE)
    {
        return true;
    }
    VkCommandPoolCreateInfo pool{};
    pool.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    pool.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    pool.queueFamilyIndex = GetVulkan().Family;
    if (Check(vkCreateCommandPool(GetVulkan().Device, &pool, nullptr, &GetVulkan().Commands)) == false)
    {
        return false;
    }
    for (Frame& frame : GetVulkan().Frames)
    {
        VkCommandBufferAllocateInfo allocate{};
        allocate.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        allocate.commandPool = GetVulkan().Commands;
        allocate.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        allocate.commandBufferCount = 1;
        if (Check(vkAllocateCommandBuffers(GetVulkan().Device, &allocate, &frame.Command)) == false)
        {
            return false;
        }
        SetVulkanObjectName(VK_OBJECT_TYPE_COMMAND_BUFFER, reinterpret_cast<uint64>(frame.Command), "Ludus/Frame");
        VkFenceCreateInfo fence{};
        fence.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
        fence.flags = VK_FENCE_CREATE_SIGNALED_BIT;
        VkSemaphoreCreateInfo semaphore{};
        semaphore.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
        if (Check(vkCreateFence(GetVulkan().Device, &fence, nullptr, &frame.Fence)) == false ||
            Check(vkCreateSemaphore(GetVulkan().Device, &semaphore, nullptr, &frame.Acquired)) == false)
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
    GetVulkan().Session = token;
    if (InitializeInstance(app) == false)
    {
        return StartupError::InstanceUnavailable;
    }
    if (ConnectPrimarySurface(window) == false)
    {
        return StartupError::SurfaceUnavailable;
    }
    if (InitializeFrameResources() == false)
    {
        return StartupError::RenderingUnavailable;
    }
    internal::Complete(
        token,
        StartupError::None,
        { .MaxFrameDimension2D = GetVulkan().MaxDimension, .MaxUniformBufferSize = GetVulkan().MaxUniformSize });
    return StartupError::None;
}
static void ReleaseFrameResources() noexcept
{
    if (GetVulkan().Device == VK_NULL_HANDLE)
    {
        return;
    }
    const usize selected = GetVulkan().ActiveSurface;
    // An acquired image belongs to the surface whose frame acquired it.
    GetVulkan().ActiveSurface = GetVulkan().HasOwnedImage == true ? GetVulkan().OwnedImageSurface : selected;
    RetireAcquiredImage();
    WaitIdle();
    for (usize slot = 0; slot < GetVulkan().Surfaces.GetSize(); ++slot)
    {
        GetVulkan().ActiveSurface = slot;
        WaitPresents();
        GetVulkan().Surfaces[slot].HostedProducer.Cancel();
        GetVulkan().Surfaces[slot].HostedFramebuffer = VK_NULL_HANDLE;
    }
    GetVulkan().ActiveSurface = selected;
    for (Frame& frame : GetVulkan().Frames)
    {
        CountRenderedFrame(frame);
        vkDestroyFence(GetVulkan().Device, frame.Fence, nullptr);
        vkDestroySemaphore(GetVulkan().Device, frame.Acquired, nullptr);
        frame = {};
    }
    vkDestroyCommandPool(GetVulkan().Device, GetVulkan().Commands, nullptr);
    GetVulkan().Commands = VK_NULL_HANDLE;
    GetVulkan().IsEncoding = false;
}
void Shutdown() noexcept
{
    ReleaseCreatedSurfaces();
    ReleaseFrameResources();
    ReleaseTargets();
    // Frame resource teardown waited for every submission that wrote a hosted image.
    GetActiveSurface().HostedProducer.Disconnect();
    if (GetVulkan().Device != VK_NULL_HANDLE)
    {
        vkDestroyRenderPass(GetVulkan().Device, GetVulkan().Pass, nullptr);
        GetVulkan().Pass = VK_NULL_HANDLE;
        if (GetVulkan().IsBorrowedDevice == false)
        {
            vkDestroyDevice(GetVulkan().Device, nullptr);
        }
        GetVulkan().Device = VK_NULL_HANDLE;
    }
    if (GetVulkan().Instance != VK_NULL_HANDLE)
    {
        if (GetActiveSurface().Surface != VK_NULL_HANDLE)
        {
            vkDestroySurfaceKHR(GetVulkan().Instance, GetActiveSurface().Surface, nullptr);
        }
        GetActiveSurface().Surface = VK_NULL_HANDLE;
        GetVulkan().Instance = VK_NULL_HANDLE;
        if (GetVulkan().IsBorrowedDevice == true)
        {
            // The source owns the instance; this owner loaded volk alone.
            volkFinalize();
        }
        else if (--gSharedInstance.Users == 0)
        {
            vkDestroyInstance(gSharedInstance.Object, nullptr);
            gSharedInstance = {};
            volkFinalize();
        }
    }
    GetVulkan().IsBorrowedDevice = false;
    GetVulkan().HasDebugLabels = false;
    ConnectClockCalibration(nullptr);
    GetVulkan().NativeInstanceVersion = VK_API_VERSION_1_1;
    GetVulkan().Physical = VK_NULL_HANDLE;
    GetVulkan().IsRayEnabled = GetVulkan().IsRayApiAvailable = false;
    GetVulkan().HasUniformTextureIndexing = false;
    GetVulkan().HasBindlessTextureIndexing = false;
    GetVulkan().HasRuntimeTextureArrays = false;
    GetVulkan().HasBindingApi = false;
    GetVulkan().RayProperties = {};
    GetVulkan().Queue = VK_NULL_HANDLE;
    GetVulkan().FrameIndex = 0;
    GetVulkan().Session = 0;
    GetVulkan().MaxDimension = 0;
    GetVulkan().MaxUniformSize = 0;
    GetVulkan().ShouldWaitAcquired = false;
    GetVulkan().HasOwnedImage = false;
    GetVulkan().HasInstanceMaintenance = false;
    GetVulkan().OwnedImageSurface = 0;
    GetVulkan().ActiveSurface = 0;
    GetVulkan().Surfaces[0].RenderedFrames = 0;
    GetVulkan().Surfaces[0].PresentedFrames = 0;
    GetActiveSurface().ShouldResize = true;
    GetVulkan().Format = VK_FORMAT_UNDEFINED;
    GetActiveSurface().Target = {};
    GetActiveSurface().Extent = {};
    GetActiveSurface().RequestedExtent = {};
}
// R14c: owners that create their device share one instance (SharedVulkanInstance) and keep
// their own device, queue and VulkanState. A borrowed device is dispatched through volk's
// single device table from its source's instance, so it is hosted only as the sole owner.
usize GetDeviceOwnerLimit(bool isNativeDevice) noexcept
{
    if (isNativeDevice == true)
    {
        return 1;
    }
    for (const VulkanState& state : gVulkanStates)
    {
        if (state.IsBorrowedDevice == true)
        {
            return 1;
        }
    }
    return MAX_DEVICE_OWNERS;
}
usize AdditionalSurfaceLimit() noexcept
{
    // A borrowed device renders only its imported headless target.
    return GetVulkan().Commands != VK_NULL_HANDLE && GetVulkan().IsBorrowedDevice == false ? MAX_DEVICE_SURFACES - 1
                                                                                           : 0;
}
StartupError CreateSurface(usize slot, const WindowInfo& window) noexcept
{
    if (GetVulkan().Commands == VK_NULL_HANDLE || GetVulkan().IsBorrowedDevice == true ||
        GetVulkan().IsEncoding == true || slot == 0 || slot >= GetVulkan().Surfaces.GetSize() ||
        GetVulkan().Surfaces[slot].IsConnected == true || window.Width > GetVulkan().MaxDimension ||
        window.Height > GetVulkan().MaxDimension)
    {
        return StartupError::InvalidWindow;
    }
    // Surface passes, pipelines and device extensions follow the primary window system.
    if (window.System != GetVulkan().Surfaces[0].Window.System)
    {
        return StartupError::BackendUnavailable;
    }
    if (window.System == platform::WindowSystem::Hosted && window.HostedSurfaceOrNull == nullptr)
    {
        return StartupError::InvalidWindow;
    }
    VulkanSurface& surface = GetVulkan().Surfaces[slot];
    if (window.System == platform::WindowSystem::Wayland)
    {
#if defined(VK_USE_PLATFORM_WAYLAND_KHR)
        if (window.Display == nullptr || window.Surface == nullptr)
        {
            return StartupError::InvalidWindow;
        }
        VkWaylandSurfaceCreateInfoKHR info{};
        info.sType = VK_STRUCTURE_TYPE_WAYLAND_SURFACE_CREATE_INFO_KHR;
        info.display = window.Display;
        info.surface = window.Surface;
        if (Check(vkCreateWaylandSurfaceKHR(GetVulkan().Instance, &info, nullptr, &surface.Surface)) == false)
        {
            return StartupError::SurfaceUnavailable;
        }
        VkBool32 canPresent = VK_FALSE;
        if (Check(vkGetPhysicalDeviceSurfaceSupportKHR(GetVulkan().Physical,
                                                       GetVulkan().Family,
                                                       surface.Surface,
                                                       &canPresent)) == false ||
            canPresent == VK_FALSE)
        {
            vkDestroySurfaceKHR(GetVulkan().Instance, surface.Surface, nullptr);
            surface.Surface = VK_NULL_HANDLE;
            return StartupError::SurfaceUnavailable;
        }
#else
        return StartupError::InvalidWindow;
#endif
    }
    surface.Window = window;
    surface.Target = { .Width = window.Width, .Height = window.Height };
    surface.RequestedExtent = { .width = window.Width, .height = window.Height };
    surface.ShouldResize = true;
    surface.RenderedFrames = 0;
    surface.PresentedFrames = 0;
    surface.IsConnected = true;
    const usize selected = GetVulkan().ActiveSurface;
    GetVulkan().ActiveSurface = slot;
    if (window.System == platform::WindowSystem::Hosted)
    {
        surface.HostedProducer.Connect(
            { .PhysicalDevice = GetVulkan().Physical, .Device = GetVulkan().Device, .QueueFamily = GetVulkan().Family },
            *window.HostedSurfaceOrNull);
    }
    // Build the swapchain now so a format or extent the primary's pass cannot serve fails here.
    const bool isCreated = window.Width == 0 || window.Height == 0 || CreateTargets() == true;
    if (isCreated == false)
    {
        ReleaseActiveSurface();
    }
    GetVulkan().ActiveSurface = selected;
    if (isCreated == false && GetVulkan().LastResult == VK_ERROR_DEVICE_LOST)
    {
        internal::Fail(GetVulkan().Session, StartupError::DeviceLost);
    }
    return isCreated == true ? StartupError::None : StartupError::SurfaceUnavailable;
}
void DestroySurface(usize slot) noexcept
{
    if (slot == 0 || slot >= GetVulkan().Surfaces.GetSize() || GetVulkan().Surfaces[slot].IsConnected == false)
    {
        return;
    }
    const usize selected = GetVulkan().ActiveSurface;
    GetVulkan().ActiveSurface = slot;
    ReleaseActiveSurface();
    GetVulkan().ActiveSurface = selected == slot ? 0 : selected;
}
void SelectSurface(usize slot) noexcept
{
    GetVulkan().ActiveSurface = slot < GetVulkan().Surfaces.GetSize() ? slot : 0;
}
SurfaceCompletion GetSurfaceCompletion(usize slot) noexcept
{
    if (slot >= GetVulkan().Surfaces.GetSize())
    {
        return {};
    }
    for (Frame& frame : GetVulkan().Frames)
    {
        if (frame.IsRenderPending == true && frame.Surface == slot && frame.Fence != VK_NULL_HANDLE &&
            GetVulkan().Device != VK_NULL_HANDLE && vkGetFenceStatus(GetVulkan().Device, frame.Fence) == VK_SUCCESS)
        {
            CountRenderedFrame(frame);
        }
    }
    return
    {
        .RenderedFrames = GetVulkan().Surfaces[slot].RenderedFrames,
        .PresentedFrames = GetVulkan().Surfaces[slot].PresentedFrames,
    };
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
    if (target.Width > GetVulkan().MaxDimension || target.Height > GetVulkan().MaxDimension ||
        ValidColor(target.Red) == false || ValidColor(target.Green) == false || ValidColor(target.Blue) == false ||
        ValidColor(target.Alpha) == false)
    {
        return FrameStatus::InvalidState;
    }
    if (target.Width != 0 && target.Height != 0)
    {
        // Compare requests, not the clamped acquired extent. A minimized tick
        // or a persistent surface clamp must not recreate targets every frame.
        GetActiveSurface().ShouldResize = GetActiveSurface().ShouldResize ||
                                          target.Width != GetActiveSurface().RequestedExtent.width ||
                                          target.Height != GetActiveSurface().RequestedExtent.height;
        GetActiveSurface().RequestedExtent = { .width = target.Width, .height = target.Height };
    }
    GetActiveSurface().Target = target;
    return FrameStatus::Ready;
}
/// Announce completed hosted frames of every surface; a failed created surface is lost.
/// @return false when the primary surface's hosted consumer failed.
bool PollHostedSurfaces() noexcept
{
    const usize selected = GetVulkan().ActiveSurface;
    bool isPrimaryHealthy = true;
    for (usize slot = 0; slot < GetVulkan().Surfaces.GetSize(); ++slot)
    {
        HostedProducerVulkan& producer = GetVulkan().Surfaces[slot].HostedProducer;
        if (producer.IsConnected() == false || producer.Poll() == true)
        {
            continue;
        }
        if (slot == 0)
        {
            isPrimaryHealthy = false;
            continue;
        }
        GetVulkan().ActiveSurface = slot;
        static_cast<void>(LoseActiveSurface());
    }
    GetVulkan().ActiveSurface = selected;
    return isPrimaryHealthy;
}
FrameStatus Begin() noexcept
{
    if (PollHostedSurfaces() == false)
    {
        GetVulkan().LastResult = VK_ERROR_DEVICE_LOST;
        return Failure();
    }
    if (GetVulkan().ActiveSurface != 0 && GetActiveSurface().IsConnected == false)
    {
        // Lost earlier, for example while polling its hosted consumer above.
        return FrameStatus::Skipped;
    }
    if (GetActiveSurface().Target.Width == 0 || GetActiveSurface().Target.Height == 0)
    {
        return FrameStatus::Skipped;
    }
    if (GetActiveSurface().ShouldResize && CreateTargets() == false)
    {
        // A created surface whose swapchain cannot be rebuilt is lost; the device is not.
        return GetVulkan().ActiveSurface != 0 && GetVulkan().LastResult != VK_ERROR_DEVICE_LOST ? LoseActiveSurface()
                                                                                                : Failure();
    }
    if (GetActiveSurface().Extent.width == 0 || GetActiveSurface().Extent.height == 0)
    {
        return FrameStatus::Skipped;
    }
    Frame& frame = GetVulkan().Frames[GetVulkan().FrameIndex];
    if (Check(vkWaitForFences(GetVulkan().Device, 1, &frame.Fence, VK_TRUE, FENCE_TIMEOUT_NANOSECONDS)) == false)
    {
        return Failure();
    }
    frame.Submitted = false;
    CountRenderedFrame(frame);
    // Hosted frames guarded by this fence have completed; it may be reset from now on.
    for (VulkanSurface& hosted : GetVulkan().Surfaces)
    {
        if (hosted.HostedProducer.IsConnected() == true)
        {
            hosted.HostedProducer.MarkSignaled(frame.Fence);
        }
    }
    // Harvest the captured query before its frame fence can be reset for later work.
    (void)TimingPoll();
    static_cast<void>(PollPassTimings());
    if (GetVulkan().Device == VK_NULL_HANDLE)
    {
        return FrameStatus::Failed;
    }
    (void)RasterCompleted();
    if (GetVulkan().Device == VK_NULL_HANDLE)
    {
        return FrameStatus::Failed;
    }
    GetActiveSurface().Image = 0;
    if (GetActiveSurface().HostedProducer.IsConnected() == true)
    {
        const HostedVulkanAcquireStatus acquired =
            GetActiveSurface().HostedProducer.Acquire(GetActiveSurface().Extent.width,
                                                      GetActiveSurface().Extent.height,
                                                      GetVulkan().Pass,
                                                      GetActiveSurface().DepthView,
                                                      GetActiveSurface().DepthEpoch,
                                                      GetActiveSurface().HostedFramebuffer);
        if (acquired == HostedVulkanAcquireStatus::Skipped)
        {
            return FrameStatus::Skipped;
        }
        if (acquired == HostedVulkanAcquireStatus::Failed)
        {
            return GetVulkan().ActiveSurface != 0 ? LoseActiveSurface() : Failure();
        }
    }
    if (GetVulkan().IsHeadless == false)
    {
        const VkResult result = vkAcquireNextImageKHR(GetVulkan().Device,
                                                      GetActiveSurface().Swapchain,
                                                      5000000000ULL,
                                                      frame.Acquired,
                                                      VK_NULL_HANDLE,
                                                      &GetActiveSurface().Image);
        if (result == VK_ERROR_OUT_OF_DATE_KHR)
        {
            GetActiveSurface().ShouldResize = true;
            return FrameStatus::Skipped;
        }
        if (result == VK_TIMEOUT || result == VK_NOT_READY)
        {
            return FrameStatus::Skipped;
        }
        if (result == VK_ERROR_SURFACE_LOST_KHR && GetVulkan().ActiveSurface != 0)
        {
            return LoseActiveSurface();
        }
        if (result == VK_SUBOPTIMAL_KHR)
        {
            GetActiveSurface().ShouldResize = true;
        }
        else if (Check(result) == false)
        {
            return Failure();
        }
    }
    if (GetVulkan().IsHeadless == false)
    {
        GetVulkan().ShouldWaitAcquired = true;
        GetVulkan().HasOwnedImage = true;
        GetVulkan().OwnedImageSurface = GetVulkan().ActiveSurface;
        if (GetActiveSurface().Presented[GetActiveSurface().Image])
        {
            if (Check(vkWaitForFences(GetVulkan().Device,
                                      1,
                                      &GetActiveSurface().PresentFences[GetActiveSurface().Image],
                                      VK_TRUE,
                                      FENCE_TIMEOUT_NANOSECONDS)) == false ||
                Check(vkResetFences(GetVulkan().Device,
                                    1,
                                    &GetActiveSurface().PresentFences[GetActiveSurface().Image])) == false)
            {
                return Failure();
            }
            GetActiveSurface().Presented[GetActiveSurface().Image] = false;
        }
    }
    if (Check(vkResetCommandBuffer(frame.Command, 0)) == false)
    {
        GetActiveSurface().HostedProducer.Cancel();
        return Failure();
    }
    VkCommandBufferBeginInfo begin{};
    begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    if (Check(vkBeginCommandBuffer(frame.Command, &begin)) == false)
    {
        GetActiveSurface().HostedProducer.Cancel();
        return Failure();
    }
    TimingBegin();
    BeginPassTimingFrame();
    VkClearValue clears[2]{};
    auto& clear = clears[0];
    clears[1].depthStencil = {1, 0};
    const FrameTarget& target = GetActiveSurface().Target;
    clear.color = VkClearColorValue
    {
        .float32 =
            {
                static_cast<float32>(target.Red),
                static_cast<float32>(target.Green),
                static_cast<float32>(target.Blue),
                static_cast<float32>(target.Alpha),
            },
    };
    VkRenderPassBeginInfo pass{};
    pass.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    pass.renderPass = GetVulkan().Pass;
    pass.framebuffer = SurfaceFramebuffer();
    pass.renderArea.extent = GetActiveSurface().Extent;
    pass.clearValueCount = 2;
    pass.pClearValues = clears;
    vkCmdBeginRenderPass(frame.Command, &pass, VK_SUBPASS_CONTENTS_INLINE);
    GetVulkan().IsEncoding = true;
    RasterFrameExtent();
    return FrameStatus::Ready;
}
FrameStatus End() noexcept
{
    Frame& frame = GetVulkan().Frames[GetVulkan().FrameIndex];
    if (GetVulkan().IsEncoding)
    {
        ClosePassLabels();
        vkCmdEndRenderPass(frame.Command);
    }
    GetVulkan().IsEncoding = false;
    // Scopes never span submission: end every label region an early return left open.
    CloseScopeLabels();
    TimingEnd();
    if (GetActiveSurface().HostedProducer.IsConnected() == true)
    {
        // Hand the image to the editor: VK_QUEUE_FAMILY_EXTERNAL, SHADER_READ_ONLY_OPTIMAL.
        GetActiveSurface().HostedProducer.RecordRelease(frame.Command);
    }
    if (Check(vkEndCommandBuffer(frame.Command)) == false ||
        Check(vkResetFences(GetVulkan().Device, 1, &frame.Fence)) == false)
    {
        GetActiveSurface().HostedProducer.Cancel();
        return Failure();
    }
    const VkPipelineStageFlags waitStage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    VkSubmitInfo submit{};
    submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &frame.Command;
    if (GetVulkan().IsHeadless == false)
    {
        submit.waitSemaphoreCount = 1;
        submit.pWaitSemaphores = &frame.Acquired;
        submit.pWaitDstStageMask = &waitStage;
        submit.signalSemaphoreCount = 1;
        submit.pSignalSemaphores = &GetActiveSurface().Finished[GetActiveSurface().Image];
    }
    if (Check(vkQueueSubmit(GetVulkan().Queue, 1, &submit, frame.Fence)) == false)
    {
        GetActiveSurface().HostedProducer.Cancel();
        return Failure();
    }
    frame.Submitted = true;
    frame.Surface = GetVulkan().ActiveSurface;
    frame.IsRenderPending = true;
    if (GetActiveSurface().HostedProducer.IsConnected() == true)
    {
        // Announced with Present once this fence signals (CPU fence).
        GetActiveSurface().HostedProducer.Submit(frame.Fence);
        GetActiveSurface().HostedFramebuffer = VK_NULL_HANDLE;
        static_cast<void>(GetActiveSurface().HostedProducer.Poll());
        ++GetActiveSurface().PresentedFrames;
    }
    TimingSubmitted();
    SubmitPassTimings();
    GetVulkan().ShouldWaitAcquired = false;
    GetVulkan().FrameIndex = (GetVulkan().FrameIndex + 1) % FRAMES;
    if (GetVulkan().IsHeadless == false)
    {
        VkSwapchainPresentFenceInfoEXT fence{};
        fence.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_PRESENT_FENCE_INFO_EXT;
        fence.swapchainCount = 1;
        fence.pFences = &GetActiveSurface().PresentFences[GetActiveSurface().Image];
        VkPresentInfoKHR present{};
        present.pNext = &fence;
        present.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
        present.waitSemaphoreCount = 1;
        present.pWaitSemaphores = &GetActiveSurface().Finished[GetActiveSurface().Image];
        present.swapchainCount = 1;
        present.pSwapchains = &GetActiveSurface().Swapchain;
        present.pImageIndices = &GetActiveSurface().Image;
        const VkResult result = vkQueuePresentKHR(GetVulkan().Queue, &present);
        // Outdated/lost surfaces still enqueue the wait and presentation fence.
        if (result == VK_SUCCESS || result == VK_SUBOPTIMAL_KHR || result == VK_ERROR_OUT_OF_DATE_KHR ||
            result == VK_ERROR_SURFACE_LOST_KHR)
        {
            GetActiveSurface().Presented[GetActiveSurface().Image] = true;
            GetVulkan().HasOwnedImage = false;
        }
        if (result == VK_SUCCESS || result == VK_SUBOPTIMAL_KHR)
        {
            ++GetActiveSurface().PresentedFrames;
        }
        if (result == VK_ERROR_SURFACE_LOST_KHR && GetVulkan().ActiveSurface != 0)
        {
            // The frame was submitted; only its created surface stops presenting.
            static_cast<void>(LoseActiveSurface());
            return FrameStatus::Ready;
        }
        if (result == VK_ERROR_OUT_OF_DATE_KHR || result == VK_SUBOPTIMAL_KHR)
        {
            GetActiveSurface().ShouldResize = true;
            return result == VK_ERROR_OUT_OF_DATE_KHR ? FrameStatus::Skipped : FrameStatus::Ready;
        }
        if (Check(result) == false)
        {
            return Failure();
        }
    }
    return FrameStatus::Ready;
}
ResourceStatus CreateShader(usize slot, const ShaderDescription& description, uint32) noexcept
{
    // Check the declared SPIR-V entry/stage before passing it to the driver.
    bool found = false;
    for (usize offset = internal::SPIRV_HEADER_WORDS; offset < description.Spirv.GetSize();)
    {
        const uint32 word = description.Spirv[offset];
        const usize count = word >> 16;
        if (count == 0 || count > description.Spirv.GetSize() - offset)
        {
            return ResourceFailure();
        }
        if ((word & 65535U) == 15 && count >= 4)
        {
            const char* name = reinterpret_cast<const char*>(description.Spirv.GetDataOrNull() + offset +
                                                             internal::SPIRV_ENTRY_POINT_NAME_WORD_OFFSET);
            const usize capacity = (count - internal::SPIRV_ENTRY_POINT_NAME_WORD_OFFSET) * sizeof(uint32);
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
    Shader& shader = GetVulkan().Shaders[slot];
    shader.Stage = description.Stage;
    std::memcpy(shader.Entry, description.SpirvEntry.data(), description.SpirvEntry.size());
    shader.Entry[description.SpirvEntry.size()] = '\0';
    VkShaderModuleCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    info.codeSize = (description.Spirv.GetSize() * sizeof(uint32));
    info.pCode = description.Spirv.GetDataOrNull();
    return Check(vkCreateShaderModule(GetVulkan().Device, &info, nullptr, &shader.Module)) ? ResourceStatus::Ready
                                                                                           : ResourceFailure();
}
void DestroyShader(usize slot) noexcept
{
    if (GetVulkan().Device != VK_NULL_HANDLE)
    {
        vkDestroyShaderModule(GetVulkan().Device, GetVulkan().Shaders[slot].Module, nullptr);
    }
    GetVulkan().Shaders[slot] = {};
}
ResourceStatus CreateUniform(usize slot, const UniformDescription& description, uint32) noexcept
{
    const usize size = description.Size;
    Uniform& uniform = GetVulkan().Uniforms[slot];
    uniform.Size = size;
    for (usize i = 0; i < FRAMES; ++i)
    {
        VkBufferCreateInfo info{};
        info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        info.size = size;
        info.usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;
        if (Check(vkCreateBuffer(GetVulkan().Device, &info, nullptr, &uniform.Buffers[i])) == false)
        {
            return ResourceFailure();
        }
        VkMemoryRequirements requirements{};
        vkGetBufferMemoryRequirements(GetVulkan().Device, uniform.Buffers[i], &requirements);
        if (Allocate(requirements,
                     VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                     uniform.Memory[i]) == false ||
            Check(vkBindBufferMemory(GetVulkan().Device, uniform.Buffers[i], uniform.Memory[i], 0)) == false ||
            Check(vkMapMemory(GetVulkan().Device, uniform.Memory[i], 0, size, 0, &uniform.Mapped[i])) == false)
        {
            return ResourceFailure();
        }
    }
    return ResourceStatus::Ready;
}
void UpdateUniform(usize slot, ludus::foundation::BorrowedView<const uint8> bytes) noexcept
{
    std::memcpy(GetVulkan().Uniforms[slot].Bytes, bytes.GetDataOrNull(), bytes.GetSize());
}
void DestroyUniform(usize slot) noexcept
{
    Uniform& uniform = GetVulkan().Uniforms[slot];
    if (GetVulkan().Device != VK_NULL_HANDLE)
    {
        if (uniform.Size != 0)
        {
            WaitIdle();
        }
        for (usize i = 0; i < FRAMES; ++i)
        {
            if (uniform.Mapped[i] != nullptr)
            {
                vkUnmapMemory(GetVulkan().Device, uniform.Memory[i]);
            }
            vkDestroyBuffer(GetVulkan().Device, uniform.Buffers[i], nullptr);
            vkFreeMemory(GetVulkan().Device, uniform.Memory[i], nullptr);
        }
    }
    uniform = {};
}
ResourceStatus CreatePipeline(usize slot, const PipelineResources& resources, uint32) noexcept
{
    if (GetVulkan().Pass == VK_NULL_HANDLE)
    {
        return ResourceFailure();
    }
    const usize vertex = resources.Vertex, fragment = resources.Fragment, uniform = resources.Uniform;
    Pipeline& pipeline = GetVulkan().Pipelines[slot];
    pipeline.UniformSlot = uniform;
    VkDescriptorSetLayoutBinding binding{};
    binding.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    binding.descriptorCount = 1;
    binding.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    VkDescriptorSetLayoutCreateInfo bindings{};
    bindings.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    bindings.bindingCount = 1;
    bindings.pBindings = &binding;
    if (Check(vkCreateDescriptorSetLayout(GetVulkan().Device, &bindings, nullptr, &pipeline.Bindings)) == false)
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
    if (Check(vkCreateDescriptorPool(GetVulkan().Device, &pool, nullptr, &pipeline.Pool)) == false)
    {
        return ResourceFailure();
    }
    VkDescriptorSetLayout layouts[FRAMES] = {pipeline.Bindings, pipeline.Bindings};
    VkDescriptorSetAllocateInfo allocate{};
    allocate.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    allocate.descriptorPool = pipeline.Pool;
    allocate.descriptorSetCount = FRAMES;
    allocate.pSetLayouts = layouts;
    if (Check(vkAllocateDescriptorSets(GetVulkan().Device, &allocate, pipeline.Sets)) == false)
    {
        return ResourceFailure();
    }
    for (usize i = 0; i < FRAMES; ++i)
    {
        VkDescriptorBufferInfo buffer{};
        buffer.buffer = GetVulkan().Uniforms[uniform].Buffers[i];
        buffer.range = GetVulkan().Uniforms[uniform].Size;
        VkWriteDescriptorSet write{};
        write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        write.dstSet = pipeline.Sets[i];
        write.descriptorCount = 1;
        write.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        write.pBufferInfo = &buffer;
        vkUpdateDescriptorSets(GetVulkan().Device, 1, &write, 0, nullptr);
    }
    VkPipelineLayoutCreateInfo layout{};
    layout.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    layout.setLayoutCount = 1;
    layout.pSetLayouts = &pipeline.Bindings;
    if (Check(vkCreatePipelineLayout(GetVulkan().Device, &layout, nullptr, &pipeline.Layout)) == false)
    {
        return ResourceFailure();
    }
    VkPipelineShaderStageCreateInfo stages[2]{
        { .stage = VK_SHADER_STAGE_VERTEX_BIT },
        { .stage = VK_SHADER_STAGE_FRAGMENT_BIT },
    };
    stages[0].sType = stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = GetVulkan().Shaders[vertex].Module;
    stages[0].pName = GetVulkan().Shaders[vertex].Entry;
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = GetVulkan().Shaders[fragment].Module;
    stages[1].pName = GetVulkan().Shaders[fragment].Entry;
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
    info.renderPass = GetVulkan().Pass;
    return Check(vkCreateGraphicsPipelines(GetVulkan().Device, VK_NULL_HANDLE, 1, &info, nullptr, &pipeline.Object))
               ? ResourceStatus::Ready
               : ResourceFailure();
}
void DestroyPipeline(usize slot) noexcept
{
    Pipeline& pipeline = GetVulkan().Pipelines[slot];
    if (GetVulkan().Device != VK_NULL_HANDLE)
    {
        if (pipeline.Object != VK_NULL_HANDLE)
        {
            WaitIdle();
        }
        vkDestroyPipeline(GetVulkan().Device, pipeline.Object, nullptr);
        vkDestroyPipelineLayout(GetVulkan().Device, pipeline.Layout, nullptr);
        vkDestroyDescriptorPool(GetVulkan().Device, pipeline.Pool, nullptr);
        vkDestroyDescriptorSetLayout(GetVulkan().Device, pipeline.Bindings, nullptr);
    }
    pipeline = {};
}
FrameInfo GetFrameInfo() noexcept
{
    return
    {
        .Width = GetActiveSurface().Extent.width,
        .Height = GetActiveSurface().Extent.height,
        .Encoding = GetVulkan().Format == VK_FORMAT_B8G8R8A8_SRGB || GetVulkan().Format == VK_FORMAT_R8G8B8A8_SRGB
                        ? SurfaceEncoding::Srgb
                        : SurfaceEncoding::Unorm,
    };
}
ResourceStatus Draw(usize slot) noexcept
{
    Pipeline& pipeline = GetVulkan().Pipelines[slot];
    Uniform& uniform = GetVulkan().Uniforms[pipeline.UniformSlot];
    std::memcpy(uniform.Mapped[GetVulkan().FrameIndex], uniform.Bytes, uniform.Size);
    const VkCommandBuffer command = GetVulkan().Frames[GetVulkan().FrameIndex].Command;
    VkViewport viewport{};
    viewport.width = static_cast<float32>(GetActiveSurface().Extent.width);
    viewport.height = static_cast<float32>(GetActiveSurface().Extent.height);
    viewport.maxDepth = 1;
    VkRect2D scissor{};
    scissor.extent = GetActiveSurface().Extent;
    vkCmdSetViewport(command, 0, 1, &viewport);
    vkCmdSetScissor(command, 0, 1, &scissor);
    vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline.Object);
    vkCmdBindDescriptorSets(command,
                            VK_PIPELINE_BIND_POINT_GRAPHICS,
                            pipeline.Layout,
                            0,
                            1,
                            &pipeline.Sets[GetVulkan().FrameIndex],
                            0,
                            nullptr);
    vkCmdDraw(command, 3, 1, 0, 0);
    return ResourceStatus::Ready;
}

#if defined(LUDUS_RHI_TEST_READBACK)
bool ReadHeadlessPixels(std::span<uint8> pixels) noexcept
{
    /// Bytes in one RGBA8 headless readback pixel.
    constexpr usize READBACK_PIXEL_BYTES = 4;
    if (GetVulkan().IsHeadless == false || GetVulkan().IsEncoding ||
        pixels.size() != static_cast<usize>(GetActiveSurface().Extent.width) * GetActiveSurface().Extent.height *
                             READBACK_PIXEL_BYTES)
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
                vkFreeCommandBuffers(GetVulkan().Device, GetVulkan().Commands, 1, &Command);
            }
            vkDestroyBuffer(GetVulkan().Device, Buffer, nullptr);
            vkFreeMemory(GetVulkan().Device, Memory, nullptr);
        }
    } copy;
    WaitIdle();
    VkBufferCreateInfo buffer{};
    buffer.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    buffer.size = pixels.size();
    buffer.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    if (Check(vkCreateBuffer(GetVulkan().Device, &buffer, nullptr, &copy.Buffer)) == false)
    {
        return false;
    }
    VkMemoryRequirements requirements{};
    vkGetBufferMemoryRequirements(GetVulkan().Device, copy.Buffer, &requirements);
    if (Allocate(requirements,
                 VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                 copy.Memory) == false ||
        Check(vkBindBufferMemory(GetVulkan().Device, copy.Buffer, copy.Memory, 0)) == false)
    {
        return false;
    }
    VkCommandBufferAllocateInfo allocate{};
    allocate.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    allocate.commandPool = GetVulkan().Commands;
    allocate.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    allocate.commandBufferCount = 1;
    if (Check(vkAllocateCommandBuffers(GetVulkan().Device, &allocate, &copy.Command)) == false)
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
    region.imageExtent =
    {
        .width = GetActiveSurface().Extent.width,
        .height = GetActiveSurface().Extent.height,
        .depth = 1,
    };
    vkCmdCopyImageToBuffer(copy.Command,
                           GetActiveSurface().Images[0],
                           VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                           copy.Buffer,
                           1,
                           &region);
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
    if (Check(vkQueueSubmit(GetVulkan().Queue, 1, &submit, VK_NULL_HANDLE)) == false ||
        Check(vkQueueWaitIdle(GetVulkan().Queue)) == false)
    {
        return false;
    }
    void* mapped = nullptr;
    if (Check(vkMapMemory(GetVulkan().Device, copy.Memory, 0, pixels.size(), 0, &mapped)) == false)
    {
        return false;
    }
    std::memcpy(pixels.data(), mapped, pixels.size());
    vkUnmapMemory(GetVulkan().Device, copy.Memory);
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
