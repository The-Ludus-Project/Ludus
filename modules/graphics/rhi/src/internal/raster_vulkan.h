#pragma once

#include <ludus/foundation/containers/static_array.hpp>

#include "profile.h"
// Private implementation included only by rhi_vulkan.cpp. Thanks to AMD GPUOpen,
// "Vulkan Memory Allocator 3.3.0", Quick start and Memory mapping (flush alignment),
// https://gpuopen-librariesandsdks.github.io/VulkanMemoryAllocator/html/memory_mapping.html
// and Khronos, "Synchronization Examples" (transfer write -> shader sample),
// https://docs.vulkan.org/guide/latest/synchronization_examples.html
// Initial uploads use a bounded synchronous setup copy, not an upload ring.
// Submitted raster resource release uses the existing ordered queue's fences.
namespace ludus::graphics::rhi::backend
{
namespace
{
struct RasterVulkanBuffer final
{
    VkBuffer Object = VK_NULL_HANDLE;
    VmaAllocation Allocation = nullptr;
    VkPipelineStageFlags Stage = VK_PIPELINE_STAGE_HOST_BIT;
    VkAccessFlags Access = VK_ACCESS_HOST_WRITE_BIT;
    VkPipelineStageFlags WriterStage = VK_PIPELINE_STAGE_HOST_BIT;
    VkAccessFlags WriterAccess = VK_ACCESS_HOST_WRITE_BIT;
};
struct RasterVulkanTexture final
{
    VkImage Object = VK_NULL_HANDLE;
    VmaAllocation Allocation = nullptr;
    TextureDescription Info;
    VkImage Depth = VK_NULL_HANDLE;
    VmaAllocation DepthAllocation = nullptr;
    VkImageView ColorView = VK_NULL_HANDLE;
    VkImageView DepthView = VK_NULL_HANDLE;
    VkFramebuffer Framebuffer = VK_NULL_HANDLE;
    VkImageLayout ColorLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    VkImageLayout DepthLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    VkPipelineStageFlags ColorStage = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
    VkAccessFlags ColorAccess = 0;
    VkPipelineStageFlags ColorWriterStage = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
    VkAccessFlags ColorWriterAccess = 0;
    VkPipelineStageFlags SampleVisibility = 0;
    RasterVulkanBuffer Upload;
    VkCommandBuffer Command = VK_NULL_HANDLE;
    VkFence Fence = VK_NULL_HANDLE;
    uint32 Request = 0;
};
struct RasterVulkanShader final
{
    VkShaderModule Object = VK_NULL_HANDLE;
    char Entry[64]{};
};
inline constexpr usize DESCRIPTOR_KINDS = 6;
inline constexpr uint32 DESCRIPTOR_SETS_PER_ARENA = 16;
struct RasterDescriptorArena final
{
    VkDescriptorPool Pool = VK_NULL_HANDLE;
    foundation::StaticArray<uint32, DESCRIPTOR_KINDS> Counts{};
    uint32 Capacity = 0;
    uint32 LiveSets = 0;
};
struct RasterVulkanSet final
{
    usize Arena = internal::RASTER_INVALID;
    VkDescriptorSet Object = VK_NULL_HANDLE;
};
struct RasterVulkanPipeline final
{
    VkPipeline Object = VK_NULL_HANDLE;
    VkPipelineLayout Layout = VK_NULL_HANDLE;
    internal::RasterPipelineInfo Info;
};
VmaAllocator gRasterAllocator = nullptr;
internal::StorageTable<RasterVulkanBuffer> gRasterBuffers;
struct ComputeVulkanPipeline final
{
    VkPipeline Object = VK_NULL_HANDLE;
    VkPipelineLayout Layout = VK_NULL_HANDLE;
};
internal::StorageTable<ComputeVulkanPipeline> gComputePipelines;
internal::StorageTable<RasterVulkanTexture> gRasterTextures;
internal::StorageTable<VkImageView> gRasterViews;
internal::StorageTable<VkImageView> gRasterViewDepths;
internal::StorageTable<VkSampler> gRasterSamplers;
internal::StorageTable<RasterVulkanShader> gRasterShaders;
internal::StorageTable<VkDescriptorSetLayout> gRasterLayouts;
internal::StorageTable<RasterVulkanSet> gRasterSets;
internal::StorageTable<RasterDescriptorArena> gRasterDescriptorArenas;
internal::StorageTable<RasterVulkanPipeline> gRasterPipelines;
uint64 gRasterOrdinals[FRAMES]{};
uint64 gRasterCompleted = 0;
VkExtent2D gRasterExtent{};
RasterPassDescription gRasterRegion{};
usize gRasterActiveTexture = internal::RASTER_INVALID;
struct RasterVkPass final
{
    VkRenderPass Object = VK_NULL_HANDLE;
    VkFormat Format = VK_FORMAT_UNDEFINED;
    RasterPassDescription State;
    bool Surface = false;
    internal::RasterPassInfo Advanced;
};
internal::StorageTable<RasterVkPass> gRasterPasses;
struct RasterVkFramebuffer final
{
    VkFramebuffer Object = VK_NULL_HANDLE;
    VkRenderPass Pass = VK_NULL_HANDLE;
    VkImageView Views[9]{};
    usize Count = 0;
    uint32 Width = 0, Height = 0;
};
internal::StorageTable<RasterVkFramebuffer> gRasterAdvancedFrames;
internal::RasterPassInfo gRasterNativePass;
VkFormat RasterPixelFormat(RasterFormat) noexcept;
RasterStatus RasterError(VkResult result) noexcept
{
    if (result == VK_SUCCESS)
    {
        return RasterStatus::Ready;
    }
    (void)Check(result);
    if (result == VK_ERROR_DEVICE_LOST)
    {
        internal::Fail(gSession, StartupError::DeviceLost);
        return RasterStatus::DeviceLost;
    }
    return result == VK_ERROR_OUT_OF_HOST_MEMORY || result == VK_ERROR_OUT_OF_DEVICE_MEMORY ? RasterStatus::OutOfMemory
                                                                                            : RasterStatus::Failed;
}
// Thanks to Khronos, Vulkan Guide, "Synchronization Examples", Graphics to
// Graphics Dependencies (color attachment -> sampled image) and WAR/WAW cases:
// https://docs.vulkan.org/guide/latest/synchronization_examples.html
// Adapted to the retained Vulkan 1.1 barriers/render-pass path, not sync2.
// Thanks to Khronos, Vulkan Specification, "Render Pass Compatibility":
// https://docs.vulkan.org/spec/latest/chapters/renderpass.html#renderpass-compatibility
// Dependencies must match across compatible pass objects, including read-only
// depth variants and the original surface pipeline/framebuffer render pass.
void RasterVkDependencies(bool surface, VkSubpassDependency (&dependencies)[2]) noexcept
{
    auto& incoming = dependencies[0];
    incoming.srcSubpass = VK_SUBPASS_EXTERNAL;
    incoming.dstSubpass = 0;
    incoming.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT |
                            VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
    incoming.dstStageMask = incoming.srcStageMask;
    incoming.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    incoming.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
                             VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    auto& outgoing = dependencies[1];
    outgoing.srcSubpass = 0;
    outgoing.dstSubpass = VK_SUBPASS_EXTERNAL;
    outgoing.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    outgoing.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    outgoing.dstStageMask =
        surface && gHeadless ? VK_PIPELINE_STAGE_TRANSFER_BIT : VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    outgoing.dstAccessMask = surface && gHeadless ? VK_ACCESS_TRANSFER_READ_BIT : 0;
}
VkResult
RasterVkRenderPass(VkFormat format, bool surface, const RasterPassDescription& state, VkRenderPass& output) noexcept
{
    const auto limit = internal::CurrentStorageLimits().RasterPassStates;
    if (gRasterPasses.EnsureSlots(limit) == false)
    {
        return internal::StorageFailure() == RasterStatus::OutOfMemory ? VK_ERROR_OUT_OF_HOST_MEMORY
                                                                       : VK_ERROR_TOO_MANY_OBJECTS;
    }
    RasterVkPass* available = nullptr;
    for (usize i = 0; i < limit; ++i)
    {
        auto& pass = gRasterPasses[i];
        const auto& cached = pass.State;
        if (pass.Object == VK_NULL_HANDLE)
        {
            if (available == nullptr)
            {
                available = &pass;
            }
            continue;
        }
        if (pass.Advanced.ColorCount == 0 && pass.Format == format && pass.Surface == surface &&
            cached.ColorLoad == state.ColorLoad && cached.ColorStore == state.ColorStore &&
            cached.DepthLoad == state.DepthLoad && cached.DepthStore == state.DepthStore &&
            cached.DepthReadOnly == state.DepthReadOnly)
        {
            output = pass.Object;
            return VK_SUCCESS;
        }
    }
    if (available == nullptr)
    {
        return VK_ERROR_TOO_MANY_OBJECTS;
    }
    VkAttachmentDescription attachments[2]{
        { .samples = VK_SAMPLE_COUNT_1_BIT },
        { .samples = VK_SAMPLE_COUNT_1_BIT },
    };
    auto& color = attachments[0];
    color.format = format;
    color.samples = VK_SAMPLE_COUNT_1_BIT;
    color.loadOp = state.ColorLoad == RasterLoad::Clear  ? VK_ATTACHMENT_LOAD_OP_CLEAR
                   : state.ColorLoad == RasterLoad::Load ? VK_ATTACHMENT_LOAD_OP_LOAD
                                                         : VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    color.storeOp =
        state.ColorStore == RasterStore::Store ? VK_ATTACHMENT_STORE_OP_STORE : VK_ATTACHMENT_STORE_OP_DONT_CARE;
    color.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    color.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    color.initialLayout = surface ? (gHeadless ? VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL : VK_IMAGE_LAYOUT_PRESENT_SRC_KHR)
                                  : VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    color.finalLayout = color.initialLayout;
    auto& depth = attachments[1];
    depth.format = gDepthFormat;
    depth.samples = VK_SAMPLE_COUNT_1_BIT;
    depth.loadOp = state.DepthLoad == RasterLoad::Clear  ? VK_ATTACHMENT_LOAD_OP_CLEAR
                   : state.DepthLoad == RasterLoad::Load ? VK_ATTACHMENT_LOAD_OP_LOAD
                                                         : VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    depth.storeOp =
        state.DepthStore == RasterStore::Store ? VK_ATTACHMENT_STORE_OP_STORE : VK_ATTACHMENT_STORE_OP_DONT_CARE;
    depth.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    depth.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    depth.initialLayout = depth.finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    VkAttachmentReference colorReference{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkAttachmentReference depthReference{1,
                                         state.DepthReadOnly ? VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL
                                                             : VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL};
    VkSubpassDescription subpass{};
    subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount = 1;
    subpass.pColorAttachments = &colorReference;
    subpass.pDepthStencilAttachment = &depthReference;
    VkSubpassDependency dependencies[2]{};
    RasterVkDependencies(surface, dependencies);
    VkRenderPassCreateInfo create{};
    create.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    create.attachmentCount = 2;
    create.pAttachments = attachments;
    create.subpassCount = 1;
    create.pSubpasses = &subpass;
    create.dependencyCount = 2;
    create.pDependencies = dependencies;
    const auto result = vkCreateRenderPass(gDevice, &create, nullptr, &available->Object);
    if (result == VK_SUCCESS)
    {
        available->Format = format;
        available->Surface = surface;
        available->State = state;
        output = available->Object;
    }
    return result;
}
// Thanks to Khronos, Vulkan Specification, "Render Pass", color resolves and
// compatibility: https://docs.vulkan.org/spec/latest/chapters/renderpass.html
// Keep explicit load/store/resolve operations at pass boundaries; R13 retains whole-image barriers.
VkAttachmentLoadOp RasterVkLoad(RasterLoad load) noexcept
{
    return load == RasterLoad::Clear  ? VK_ATTACHMENT_LOAD_OP_CLEAR
           : load == RasterLoad::Load ? VK_ATTACHMENT_LOAD_OP_LOAD
                                      : VK_ATTACHMENT_LOAD_OP_DONT_CARE;
}
VkAttachmentStoreOp RasterVkStore(RasterStore store) noexcept
{
    return store == RasterStore::Store ? VK_ATTACHMENT_STORE_OP_STORE : VK_ATTACHMENT_STORE_OP_DONT_CARE;
}
VkResult RasterVkAdvancedPass(const internal::RasterPassInfo& info, VkRenderPass& output) noexcept
{
    const auto limit = internal::CurrentStorageLimits().RasterPassStates;
    if (gRasterPasses.EnsureSlots(limit) == false)
    {
        return VK_ERROR_OUT_OF_HOST_MEMORY;
    }
    RasterVkPass* available = nullptr;
    for (usize i = 0; i < limit; ++i)
    {
        auto& cached = gRasterPasses[i];
        if (cached.Object == VK_NULL_HANDLE)
        {
            if (available == nullptr)
            {
                available = &cached;
            }
            continue;
        }
        const auto& other = cached.Advanced;
        bool same = other.ColorCount == info.ColorCount && other.SampleCount == info.SampleCount &&
                    other.Stencil == info.Stencil && other.Description.DepthLoad == info.Description.DepthLoad &&
                    other.Description.DepthStore == info.Description.DepthStore &&
                    other.Description.DepthReadOnly == info.Description.DepthReadOnly &&
                    other.Description.StencilLoad == info.Description.StencilLoad &&
                    other.Description.StencilStore == info.Description.StencilStore &&
                    other.Description.StencilReadOnly == info.Description.StencilReadOnly;
        for (usize c = 0; c < info.ColorCount && same; ++c)
        {
            same = other.Colors[c].Format == info.Colors[c].Format && other.Colors[c].Load == info.Colors[c].Load &&
                   other.Colors[c].Store == info.Colors[c].Store &&
                   (other.Colors[c].ResolveView != internal::RASTER_INVALID) ==
                       (info.Colors[c].ResolveView != internal::RASTER_INVALID);
        }
        if (same)
        {
            output = cached.Object;
            return VK_SUCCESS;
        }
    }
    if (available == nullptr)
    {
        return VK_ERROR_TOO_MANY_OBJECTS;
    }
    // Initialize only the attachment prefix passed to Vulkan; resolve entries
    // copy complete initialized color descriptions below.
    VkAttachmentDescription attachments[9];
    VkAttachmentReference colors[4]{}, resolves[4]{};
    const auto count = static_cast<uint32>(info.ColorCount);
    for (uint32 i = 0; i < count; ++i)
    {
        VkAttachmentDescription& a = attachments[i];
        a = { .samples = static_cast<VkSampleCountFlagBits>(info.SampleCount) };
        const auto& c = info.Colors[i];
        a.format = RasterPixelFormat(c.Format);
        a.samples = static_cast<VkSampleCountFlagBits>(info.SampleCount);
        a.loadOp = RasterVkLoad(c.Load);
        a.storeOp = RasterVkStore(c.Store);
        a.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        a.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        a.initialLayout = a.finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        colors[i] = {i, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
        resolves[i] = {VK_ATTACHMENT_UNUSED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    }
    VkAttachmentDescription& depth = attachments[count];
    depth = { .samples = static_cast<VkSampleCountFlagBits>(info.SampleCount) };
    depth.format = info.Stencil ? VK_FORMAT_D32_SFLOAT_S8_UINT : gDepthFormat;
    depth.samples = static_cast<VkSampleCountFlagBits>(info.SampleCount);
    depth.loadOp = RasterVkLoad(info.Description.DepthLoad);
    depth.storeOp = RasterVkStore(info.Description.DepthStore);
    depth.stencilLoadOp = info.Stencil ? RasterVkLoad(info.Description.StencilLoad) : VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    depth.stencilStoreOp =
        info.Stencil ? RasterVkStore(info.Description.StencilStore) : VK_ATTACHMENT_STORE_OP_DONT_CARE;
    depth.initialLayout = depth.finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    VkAttachmentReference depthRef{count, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL};
    uint32 total = count + 1;
    bool hasResolve = false;
    for (uint32 i = 0; i < count; ++i)
    {
        if (info.Colors[i].ResolveView != internal::RASTER_INVALID)
        {
            auto& a = attachments[total];
            a = attachments[i];
            a.samples = VK_SAMPLE_COUNT_1_BIT;
            a.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
            a.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
            resolves[i].attachment = total++;
            hasResolve = true;
        }
    }
    VkSubpassDescription subpass{};
    subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount = count;
    subpass.pColorAttachments = colors;
    subpass.pDepthStencilAttachment = &depthRef;
    subpass.pResolveAttachments = hasResolve ? resolves : nullptr;
    VkRenderPassCreateInfo create{};
    create.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    create.attachmentCount = total;
    create.pAttachments = attachments;
    create.subpassCount = 1;
    create.pSubpasses = &subpass;
    const auto result = vkCreateRenderPass(gDevice, &create, nullptr, &available->Object);
    if (result == VK_SUCCESS)
    {
        available->Advanced = info;
        output = available->Object;
    }
    return result;
}
VkResult
RasterVkAdvancedFramebuffer(const internal::RasterPassInfo& info, VkRenderPass pass, VkFramebuffer& output) noexcept
{
    VkImageView views[9]{};
    usize count = 0;
    for (usize i = 0; i < info.ColorCount; ++i)
    {
        views[count++] = gRasterViews[info.Colors[i].View];
    }
    views[count++] = gRasterViewDepths[info.Colors[0].View];
    for (usize i = 0; i < info.ColorCount; ++i)
    {
        if (info.Colors[i].ResolveView != internal::RASTER_INVALID)
        {
            views[count++] = gRasterViews[info.Colors[i].ResolveView];
        }
    }
    const auto limit = internal::CurrentStorageLimits().RasterPassStates;
    if (gRasterAdvancedFrames.EnsureSlots(limit) == false)
    {
        return VK_ERROR_OUT_OF_HOST_MEMORY;
    }
    RasterVkFramebuffer* available = nullptr;
    for (usize i = 0; i < limit; ++i)
    {
        auto& frame = gRasterAdvancedFrames[i];
        if (frame.Object == VK_NULL_HANDLE)
        {
            if (available == nullptr)
            {
                available = &frame;
            }
            continue;
        }
        bool same =
            frame.Pass == pass && frame.Count == count && frame.Width == info.Width && frame.Height == info.Height;
        for (usize v = 0; v < count && same; ++v)
        {
            same = frame.Views[v] == views[v];
        }
        if (same)
        {
            output = frame.Object;
            return VK_SUCCESS;
        }
    }
    if (available == nullptr)
    {
        return VK_ERROR_TOO_MANY_OBJECTS;
    }
    VkFramebufferCreateInfo create{};
    create.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
    create.renderPass = pass;
    create.attachmentCount = static_cast<uint32>(count);
    create.pAttachments = views;
    create.width = info.Width;
    create.height = info.Height;
    create.layers = 1;
    const auto result = vkCreateFramebuffer(gDevice, &create, nullptr, &available->Object);
    if (result == VK_SUCCESS)
    {
        available->Pass = pass;
        available->Count = count;
        available->Width = info.Width;
        available->Height = info.Height;
        for (usize i = 0; i < count; ++i)
        {
            available->Views[i] = views[i];
        }
        output = available->Object;
    }
    return result;
}
VkResult RasterAllocator() noexcept
{
    if (gRasterAllocator != nullptr)
    {
        return VK_SUCCESS;
    }
    VmaVulkanFunctions functions{};
    functions.vkGetInstanceProcAddr = vkGetInstanceProcAddr;
    functions.vkGetDeviceProcAddr = vkGetDeviceProcAddr;
    VmaAllocatorCreateInfo info{};
    info.physicalDevice = gPhysical;
    info.device = gDevice;
    info.instance = gInstance;
    info.vulkanApiVersion = gRayEnabled ? VK_API_VERSION_1_2 : VK_API_VERSION_1_1;
    info.flags = gRayEnabled ? VMA_ALLOCATOR_CREATE_BUFFER_DEVICE_ADDRESS_BIT : 0U;
    info.pVulkanFunctions = &functions;
    return vmaCreateAllocator(&info, &gRasterAllocator);
}
VkResult RasterUploadBuffer(VkBufferUsageFlags usage, std::span<const uint8> bytes, RasterVulkanBuffer& buffer) noexcept
{
    auto result = RasterAllocator();
    if (result != VK_SUCCESS)
    {
        return result;
    }
    VkBufferCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    info.size = bytes.size();
    info.usage = usage;
    VmaAllocationCreateInfo allocation{};
    allocation.usage = VMA_MEMORY_USAGE_AUTO;
    allocation.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
    VmaAllocationInfo mapped{};
    result = vmaCreateBuffer(gRasterAllocator, &info, &allocation, &buffer.Object, &buffer.Allocation, &mapped);
    if (result != VK_SUCCESS)
    {
        return result;
    }
    if (mapped.pMappedData == nullptr)
    {
        return VK_ERROR_MEMORY_MAP_FAILED;
    }
    std::memcpy(mapped.pMappedData, bytes.data(), bytes.size());
    return vmaFlushAllocation(gRasterAllocator, buffer.Allocation, 0, bytes.size());
}
VkDescriptorType RasterDescriptor(RasterBindingKind kind, bool hasDynamicOffset = false) noexcept
{
    return kind == RasterBindingKind::UniformBuffer
               ? (hasDynamicOffset == true ? VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC
                                           : VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER)
           : kind == RasterBindingKind::StorageRead || kind == RasterBindingKind::StorageReadWrite
               ? (hasDynamicOffset == true ? VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC
                                           : VK_DESCRIPTOR_TYPE_STORAGE_BUFFER)
           : internal::TextureBinding(kind) ? VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE
                                            : VK_DESCRIPTOR_TYPE_SAMPLER;
}
VkFormat RasterPixelFormat(RasterFormat format) noexcept
{
    return format == RasterFormat::Bc1RgbaUnorm   ? VK_FORMAT_BC1_RGBA_UNORM_BLOCK
           : format == RasterFormat::Bc3RgbaUnorm ? VK_FORMAT_BC3_UNORM_BLOCK
           : format == RasterFormat::Bc5RgUnorm   ? VK_FORMAT_BC5_UNORM_BLOCK
           : format == RasterFormat::Rgba16Float  ? VK_FORMAT_R16G16B16A16_SFLOAT
           : format == RasterFormat::Rg8Unorm     ? VK_FORMAT_R8G8_UNORM
           : format == RasterFormat::R8Unorm      ? VK_FORMAT_R8_UNORM
           : format == RasterFormat::Rgba8Unorm   ? VK_FORMAT_R8G8B8A8_UNORM
                                                  : VK_FORMAT_R8G8B8A8_SRGB;
}
} // namespace
// Thanks to Khronos Group, "Vulkan Specification", Formats and
// vkGetPhysicalDeviceImageFormatProperties: query the actual combined image usage,
// not only individual feature bits. Keep Vulkan 1.1 and engine profile bounds.
// https://docs.vulkan.org/refpages/latest/refpages/source/vkGetPhysicalDeviceImageFormatProperties.html
FormatCapabilities FormatLimits(RasterFormat format) noexcept
{
    if (gPhysical == VK_NULL_HANDLE || static_cast<uint8>(format) > static_cast<uint8>(RasterFormat::Bc5RgUnorm))
    {
        return {};
    }
    VkPhysicalDeviceFeatures enabled{};
    vkGetPhysicalDeviceFeatures(gPhysical, &enabled);
    if (internal::CompressedTexture(format) &&
        (internal::NativeDeviceRequest() != nullptr || enabled.textureCompressionBC != VK_TRUE))
    {
        return {};
    }
    const auto pixel = RasterPixelFormat(format);
    VkFormatProperties properties{};
    vkGetPhysicalDeviceFormatProperties(gPhysical, pixel, &properties);
    const VkFormatFeatureFlags required =
        VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT |
        VK_FORMAT_FEATURE_TRANSFER_DST_BIT |
        (format == RasterFormat::Rgba8Unorm ? VK_FORMAT_FEATURE_TRANSFER_SRC_BIT : 0U);
    if ((properties.optimalTilingFeatures & required) != required)
    {
        return {};
    }
    const VkImageUsageFlags usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                                    (format == RasterFormat::Rgba8Unorm ? VK_IMAGE_USAGE_TRANSFER_SRC_BIT : 0U);
    VkImageFormatProperties sampled{};
    if (vkGetPhysicalDeviceImageFormatProperties(gPhysical,
                                                 pixel,
                                                 VK_IMAGE_TYPE_2D,
                                                 VK_IMAGE_TILING_OPTIMAL,
                                                 usage,
                                                 0,
                                                 &sampled) != VK_SUCCESS ||
        (sampled.sampleCounts & VK_SAMPLE_COUNT_1_BIT) == 0)
    {
        return {};
    }
    FormatCapabilities result = internal::PortableFormatLimits(format, RasterLimits());
    result.HasUnalignedBlockExtents = internal::CompressedTexture(format);
    if (sampled.maxExtent.width < result.MaxSampledDimension2D)
    {
        result.MaxSampledDimension2D = sampled.maxExtent.width;
    }
    if (sampled.maxExtent.height < result.MaxSampledDimension2D)
    {
        result.MaxSampledDimension2D = sampled.maxExtent.height;
    }
    uint32 extentMips = 1;
    for (auto extent = result.MaxSampledDimension2D; extent > 1; extent >>= 1U)
    {
        ++extentMips;
    }
    if (extentMips < result.MaxMipLevels)
    {
        result.MaxMipLevels = extentMips;
    }
    if (sampled.maxMipLevels < result.MaxMipLevels)
    {
        result.MaxMipLevels = sampled.maxMipLevels;
    }
    result.MaxArrayLayers = sampled.maxArrayLayers;
    result.PartialViews = true;
    VkImageFormatProperties cube{}, volume{};
    if (vkGetPhysicalDeviceImageFormatProperties(gPhysical,
                                                 pixel,
                                                 VK_IMAGE_TYPE_2D,
                                                 VK_IMAGE_TILING_OPTIMAL,
                                                 usage,
                                                 VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT,
                                                 &cube) == VK_SUCCESS &&
        cube.maxArrayLayers >= 6)
    {
        result.MaxCubeDimension = result.MaxSampledDimension2D;
        if (cube.maxExtent.width < result.MaxCubeDimension)
        {
            result.MaxCubeDimension = cube.maxExtent.width;
        }
        if (cube.maxExtent.height < result.MaxCubeDimension)
        {
            result.MaxCubeDimension = cube.maxExtent.height;
        }
    }
    // Compressed volumes are a separate feature/profile; the R9 BC profile is 2D/array/cube.
    if (internal::CompressedTexture(format) == false &&
        vkGetPhysicalDeviceImageFormatProperties(gPhysical,
                                                 pixel,
                                                 VK_IMAGE_TYPE_3D,
                                                 VK_IMAGE_TILING_OPTIMAL,
                                                 usage,
                                                 0,
                                                 &volume) == VK_SUCCESS)
    {
        result.MaxDimension3D = volume.maxExtent.width;
        const uint32 bounds[]{volume.maxExtent.width, volume.maxExtent.height, volume.maxExtent.depth};
        for (const auto bound : bounds)
        {
            if (bound < result.MaxDimension3D)
            {
                result.MaxDimension3D = bound;
            }
        }
    }
    VkImageFormatProperties color{}, depth{};
    const bool attachment = result.Uses.ColorAttachment &&
                            (properties.optimalTilingFeatures & VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT) != 0 &&
                            vkGetPhysicalDeviceImageFormatProperties(gPhysical,
                                                                     pixel,
                                                                     VK_IMAGE_TYPE_2D,
                                                                     VK_IMAGE_TILING_OPTIMAL,
                                                                     usage | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT,
                                                                     0,
                                                                     &color) == VK_SUCCESS &&
                            vkGetPhysicalDeviceImageFormatProperties(gPhysical,
                                                                     gDepthFormat,
                                                                     VK_IMAGE_TYPE_2D,
                                                                     VK_IMAGE_TILING_OPTIMAL,
                                                                     VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT |
                                                                         VK_IMAGE_USAGE_SAMPLED_BIT,
                                                                     0,
                                                                     &depth) == VK_SUCCESS &&
                            (color.sampleCounts & depth.sampleCounts & VK_SAMPLE_COUNT_1_BIT) != 0;
    result.Uses.ColorAttachment = attachment;
    result.Uses.Blendable =
        attachment && (properties.optimalTilingFeatures & VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BLEND_BIT) != 0;
    result.SampleCounts = attachment ? static_cast<uint32>(color.sampleCounts & depth.sampleCounts) & 5U : 0U;
    uint32 stencilDimension = 0;
    if (attachment)
    {
        VkImageFormatProperties stencil{};
        result.StencilAttachment = vkGetPhysicalDeviceImageFormatProperties(
                                       gPhysical,
                                       VK_FORMAT_D32_SFLOAT_S8_UINT,
                                       VK_IMAGE_TYPE_2D,
                                       VK_IMAGE_TILING_OPTIMAL,
                                       VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                                       0,
                                       &stencil) == VK_SUCCESS;
        result.DepthSampled = true;
        result.StencilSampled = result.StencilAttachment;
        result.AttachmentViews = true;
        result.MaxAttachmentMipLevels =
            color.maxMipLevels < depth.maxMipLevels ? color.maxMipLevels : depth.maxMipLevels;
        if (result.MaxAttachmentMipLevels > result.MaxMipLevels)
        {
            result.MaxAttachmentMipLevels = result.MaxMipLevels;
        }
        result.MaxAttachmentLayers =
            color.maxArrayLayers < depth.maxArrayLayers ? color.maxArrayLayers : depth.maxArrayLayers;
        if (result.StencilAttachment)
        {
            stencilDimension =
                stencil.maxExtent.width < stencil.maxExtent.height ? stencil.maxExtent.width : stencil.maxExtent.height;
            result.SampleCounts &= static_cast<uint32>(stencil.sampleCounts);
            if (stencil.maxMipLevels < result.MaxAttachmentMipLevels)
            {
                result.MaxAttachmentMipLevels = stencil.maxMipLevels;
            }
            if (stencil.maxArrayLayers < result.MaxAttachmentLayers)
            {
                result.MaxAttachmentLayers = stencil.maxArrayLayers;
            }
        }
    }
    result.MaxAttachmentDimension2D = attachment ? result.MaxSampledDimension2D : 0;
    VkPhysicalDeviceProperties device{};
    vkGetPhysicalDeviceProperties(gPhysical, &device);
    constexpr usize ATTACHMENT_DIMENSION_BOUND_COUNT = 8;
    const StaticArray<uint32, ATTACHMENT_DIMENSION_BOUND_COUNT> bounds{{color.maxExtent.width,
                                                                        color.maxExtent.height,
                                                                        depth.maxExtent.width,
                                                                        depth.maxExtent.height,
                                                                        device.limits.maxFramebufferWidth,
                                                                        device.limits.maxFramebufferHeight,
                                                                        device.limits.maxViewportDimensions[0],
                                                                        device.limits.maxViewportDimensions[1]}};
    for (const uint32 bound : bounds)
    {
        if (bound < result.MaxAttachmentDimension2D)
        {
            result.MaxAttachmentDimension2D = bound;
        }
    }
    if (result.StencilAttachment == true && stencilDimension < result.MaxAttachmentDimension2D)
    {
        result.MaxAttachmentDimension2D = stencilDimension;
    }
    if (attachment)
    {
        VkImageFormatProperties colorCube{}, depthCube{}, stencilCube{};
        const auto depthUsage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
        if (vkGetPhysicalDeviceImageFormatProperties(gPhysical,
                                                     pixel,
                                                     VK_IMAGE_TYPE_2D,
                                                     VK_IMAGE_TILING_OPTIMAL,
                                                     usage | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT,
                                                     VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT,
                                                     &colorCube) == VK_SUCCESS &&
            vkGetPhysicalDeviceImageFormatProperties(gPhysical,
                                                     gDepthFormat,
                                                     VK_IMAGE_TYPE_2D,
                                                     VK_IMAGE_TILING_OPTIMAL,
                                                     depthUsage,
                                                     VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT,
                                                     &depthCube) == VK_SUCCESS &&
            (result.StencilAttachment == false ||
             vkGetPhysicalDeviceImageFormatProperties(gPhysical,
                                                      VK_FORMAT_D32_SFLOAT_S8_UINT,
                                                      VK_IMAGE_TYPE_2D,
                                                      VK_IMAGE_TILING_OPTIMAL,
                                                      depthUsage,
                                                      VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT,
                                                      &stencilCube) == VK_SUCCESS))
        {
            result.MaxAttachmentCubeDimension = result.MaxAttachmentDimension2D;
            const uint32 cubeBounds[]{colorCube.maxExtent.width,
                                      colorCube.maxExtent.height,
                                      depthCube.maxExtent.width,
                                      depthCube.maxExtent.height};
            for (auto bound : cubeBounds)
            {
                if (bound < result.MaxAttachmentCubeDimension)
                {
                    result.MaxAttachmentCubeDimension = bound;
                }
            }
            if (result.StencilAttachment)
            {
                if (stencilCube.maxExtent.width < result.MaxAttachmentCubeDimension)
                {
                    result.MaxAttachmentCubeDimension = stencilCube.maxExtent.width;
                }
                if (stencilCube.maxExtent.height < result.MaxAttachmentCubeDimension)
                {
                    result.MaxAttachmentCubeDimension = stencilCube.maxExtent.height;
                }
            }
        }
        if (result.StencilAttachment)
        {
            VkImageFormatProperties stencil{};
            if (vkGetPhysicalDeviceImageFormatProperties(gPhysical,
                                                         VK_FORMAT_D32_SFLOAT_S8_UINT,
                                                         VK_IMAGE_TYPE_2D,
                                                         VK_IMAGE_TILING_OPTIMAL,
                                                         depthUsage,
                                                         0,
                                                         &stencil) == VK_SUCCESS)
            {
                if (stencil.maxExtent.width < result.MaxAttachmentDimension2D)
                {
                    result.MaxAttachmentDimension2D = stencil.maxExtent.width;
                }
                if (stencil.maxExtent.height < result.MaxAttachmentDimension2D)
                {
                    result.MaxAttachmentDimension2D = stencil.maxExtent.height;
                }
            }
        }
    }
    return result;
}
uint32 RasterColorAttachmentLimit() noexcept
{
    VkPhysicalDeviceProperties properties{};
    vkGetPhysicalDeviceProperties(gPhysical, &properties);
    return properties.limits.maxColorAttachments < 4 ? properties.limits.maxColorAttachments : 4;
}
uint32 RasterVertexStrideLimit() noexcept
{
    return 256;
}
bool gRasterHdrProbed = false, gRasterHdr = false;
bool GetDescriptorArenaUsage(internal::DescriptorArenaUsage& outUsage) noexcept
{
    outUsage = {};
    for (const RasterDescriptorArena& arena : gRasterDescriptorArenas)
    {
        if (arena.Pool == VK_NULL_HANDLE)
        {
            continue;
        }
        ++outUsage.Pools;
        uint32 descriptorCount = 0;
        for (uint32 count : arena.Counts)
        {
            descriptorCount += count;
        }
        // Empty layouts reserve one otherwise-unused uniform slot per native set.
        outUsage.ReservedDescriptors += uint64{descriptorCount == 0 ? 1U : descriptorCount} * arena.Capacity;
    }
    return true;
}
BindingCapabilities BindingLimits() noexcept
{
    if (gPhysical == VK_NULL_HANDLE || gDevice == VK_NULL_HANDLE)
    {
        return {};
    }
    VkPhysicalDeviceProperties properties{};
    vkGetPhysicalDeviceProperties(gPhysical, &properties);
    const VkPhysicalDeviceLimits& limits = properties.limits;
    uint32 textures = internal::CurrentStorageLimits().DescriptorsPerSet;
    if (limits.maxPerStageDescriptorSampledImages < textures)
    {
        textures = limits.maxPerStageDescriptorSampledImages;
    }
    if (limits.maxDescriptorSetSampledImages < textures)
    {
        textures = limits.maxDescriptorSetSampledImages;
    }
    uint32 uniforms = limits.maxPerStageDescriptorUniformBuffers < limits.maxDescriptorSetUniformBuffers
                          ? limits.maxPerStageDescriptorUniformBuffers
                          : limits.maxDescriptorSetUniformBuffers;
    uint32 samplers = limits.maxPerStageDescriptorSamplers < limits.maxDescriptorSetSamplers
                          ? limits.maxPerStageDescriptorSamplers
                          : limits.maxDescriptorSetSamplers;
    uniforms = uniforms < internal::RASTER_BINDINGS ? uniforms : static_cast<uint32>(internal::RASTER_BINDINGS);
    samplers = samplers < internal::RASTER_BINDINGS ? samplers : static_cast<uint32>(internal::RASTER_BINDINGS);
    return
    {
        .MaxDynamicUniformBuffers = limits.maxDescriptorSetUniformBuffersDynamic < internal::RASTER_BINDINGS
                                        ? limits.maxDescriptorSetUniformBuffersDynamic
                                        : static_cast<uint32>(internal::RASTER_BINDINGS),
        .MaxDynamicStorageBuffers = limits.maxDescriptorSetStorageBuffersDynamic < internal::RASTER_BINDINGS
                                        ? limits.maxDescriptorSetStorageBuffersDynamic
                                        : static_cast<uint32>(internal::RASTER_BINDINGS),
        .MaxTextureArrayElements = textures,
        .MaxDescriptorsPerSet = limits.maxPerStageResources < internal::CurrentStorageLimits().DescriptorsPerSet
                                    ? limits.maxPerStageResources
                                    : internal::CurrentStorageLimits().DescriptorsPerSet,
        .HasUniformTextureIndexing = gHasUniformTextureIndexing,
        .HasBindlessTextureIndexing = gHasBindlessTextureIndexing,
        .MaxBindings = static_cast<uint32>(internal::RASTER_BINDINGS),
        .MaxUniformBuffersPerStage = uniforms,
        .MaxSampledTexturesPerStage = textures,
        .MaxSamplersPerStage = samplers,
        .MaxUniformBuffersPerSet = limits.maxDescriptorSetUniformBuffers < internal::RASTER_BINDINGS
                                       ? limits.maxDescriptorSetUniformBuffers
                                       : static_cast<uint32>(internal::RASTER_BINDINGS),
        .MaxSamplersPerSet = limits.maxDescriptorSetSamplers < internal::RASTER_BINDINGS
                                 ? limits.maxDescriptorSetSamplers
                                 : static_cast<uint32>(internal::RASTER_BINDINGS),
        .HasBufferDescriptorArrays = true,
        .HasSamplerDescriptorArrays = true,
        .HasRuntimeTextureArrays = gHasRuntimeTextureArrays,
        .HasPartiallyBoundArrays = true,
    };
}
RasterCapabilities RasterLimits() noexcept
{
    VkPhysicalDeviceProperties properties{};
    vkGetPhysicalDeviceProperties(gPhysical, &properties);
    if (gRasterHdrProbed == false)
    {
        VkFormatProperties hdr{};
        vkGetPhysicalDeviceFormatProperties(gPhysical, VK_FORMAT_R16G16B16A16_SFLOAT, &hdr);
        constexpr auto required = VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT |
                                  VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BLEND_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT |
                                  VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT;
        gRasterHdr = (hdr.optimalTilingFeatures & required) == required;
        gRasterHdrProbed = true;
    }
    const uint32 max = properties.limits.maxImageDimension2D;
    const usize alignment = properties.limits.minUniformBufferOffsetAlignment;
    // Thanks to Khronos Group, Vulkan Specification, maintenance3 allocation limits:
    // https://docs.vulkan.org/refpages/latest/refpages/source/VkPhysicalDeviceMaintenance3Properties.html
    // Vulkan 1.1 is required; staging and buffer allocations share the enabled allocation bound.
    VkPhysicalDeviceMaintenance3Properties allocation
    {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAINTENANCE_3_PROPERTIES,
    };
    VkPhysicalDeviceProperties2 extended
    {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2,
        .pNext = &allocation,
    };
    vkGetPhysicalDeviceProperties2(gPhysical, &extended);
    const usize bufferBytes = internal::GetResourceSizeLimit(allocation.maxMemoryAllocationSize);
    return {bufferBytes,
            max,
            alignment > 256 ? alignment : 256,
            properties.limits.maxUniformBufferRange < 16384 ? properties.limits.maxUniformBufferRange : 16384,
            internal::CurrentStorageLimits().ResourcesPerKind,
            internal::CurrentStorageLimits().DrawsPerFrame,
            properties.limits.maxVertexInputAttributes < 16 ? properties.limits.maxVertexInputAttributes : 16,
            gRasterHdr,
            bufferBytes};
}
ComputeCapabilities ComputeLimits() noexcept
{
    if (gPhysical == VK_NULL_HANDLE || gQueueCompute == false)
    {
        return {};
    }
    VkPhysicalDeviceProperties properties{};
    vkGetPhysicalDeviceProperties(gPhysical, &properties);
    const auto& limits = properties.limits;
    if (limits.maxPerStageDescriptorStorageBuffers < 4 || limits.maxComputeWorkGroupInvocations < 256)
    {
        return {};
    }
    const usize range = RasterLimits().MaxBufferSize;
    ComputeCapabilities result;
    result.MaxStorageRange = limits.maxStorageBufferRange < range ? limits.maxStorageBufferRange : range;
    result.StorageOffsetAlignment =
        limits.minStorageBufferOffsetAlignment > 256 ? limits.minStorageBufferOffsetAlignment : 256;
    const uint32 storageBindings = limits.maxPerStageDescriptorStorageBuffers < limits.maxDescriptorSetStorageBuffers
                                       ? limits.maxPerStageDescriptorStorageBuffers
                                       : limits.maxDescriptorSetStorageBuffers;
    result.MaxStorageBindings =
        storageBindings < internal::RASTER_BINDINGS ? storageBindings : static_cast<uint32>(internal::RASTER_BINDINGS);
    result.MaxWorkgroupInvocations = 256;
    const uint32 size[3]{256, 256, 64};
    for (usize i = 0; i < 3; ++i)
    {
        result.MaxWorkgroupSize[i] =
            limits.maxComputeWorkGroupSize[i] < size[i] ? limits.maxComputeWorkGroupSize[i] : size[i];
        result.MaxDispatch[i] = limits.maxComputeWorkGroupCount[i] < 65535 ? limits.maxComputeWorkGroupCount[i] : 65535;
    }
    return result;
}
// Thanks to Khronos Group, Vulkan Specification, "Pipelines" and
// vkCreateGraphicsPipelines host-synchronization rules:
// https://docs.vulkan.org/spec/latest/chapters/pipelines.html.
// A dedicated lane builds pipelines without a shared VkPipelineCache. Captured
// shader/layout/render-pass handles stay retained by the resource ledger until
// owner-thread completion (including detached candidates and device shutdown).
bool hasPreparationDeviceLoss = false;
RasterStatus GetPreparationStatus(VkResult result) noexcept
{
    if (result == VK_SUCCESS)
    {
        return RasterStatus::Ready;
    }
    static_cast<void>(Check(result)); // Owner-thread diagnostics; failure publication stays outside the worker.
    if (result == VK_ERROR_DEVICE_LOST)
    {
        if (isPreparationDraining == false)
        {
            hasPreparationDeviceLoss = true;
        }
        return RasterStatus::DeviceLost;
    }
    return result == VK_ERROR_OUT_OF_HOST_MEMORY || result == VK_ERROR_OUT_OF_DEVICE_MEMORY ? RasterStatus::OutOfMemory
                                                                                            : RasterStatus::Failed;
}
struct VulkanComputePreparation final : NativePreparationTask
{
    VkDevice Device = VK_NULL_HANDLE;
    VkDescriptorSetLayout Bindings = VK_NULL_HANDLE;
    RasterVulkanShader Shader;
    ComputeVulkanPipeline Pipeline;
    ~VulkanComputePreparation() noexcept override
    {
        if (Pipeline.Object != VK_NULL_HANDLE)
        {
            vkDestroyPipeline(Device, Pipeline.Object, nullptr);
        }
        if (Pipeline.Layout != VK_NULL_HANDLE)
        {
            vkDestroyPipelineLayout(Device, Pipeline.Layout, nullptr);
        }
    }
    VkResult Outcome = VK_NOT_READY;
    void Execute() noexcept override
    {
        const VkPipelineLayoutCreateInfo layout
        {
            .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
            .setLayoutCount = 1,
            .pSetLayouts = &Bindings,
        };
        Outcome = vkCreatePipelineLayout(Device, &layout, nullptr, &Pipeline.Layout);
        if (Outcome != VK_SUCCESS)
        {
            return;
        }
        const VkComputePipelineCreateInfo descriptor
        {
            .sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
            .stage =
            {
                .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                .stage = VK_SHADER_STAGE_COMPUTE_BIT,
                .module = Shader.Object,
                .pName = Shader.Entry,
            },
            .layout = Pipeline.Layout,
        };
        Outcome = vkCreateComputePipelines(Device, VK_NULL_HANDLE, 1, &descriptor, nullptr, &Pipeline.Object);
    }
    RasterStatus Complete(bool) noexcept override
    {
        gComputePipelines[ResourceSlot] = Pipeline;
        Pipeline = {};
        const RasterStatus status = GetPreparationStatus(Outcome);
        internal::RasterComplete(Request, status);
        return status;
    }
};
struct VulkanRasterPreparation final : NativePreparationTask
{
    VkDevice Device = VK_NULL_HANDLE;
    VkDescriptorSetLayout Bindings = VK_NULL_HANDLE;
    VkRenderPass Pass = VK_NULL_HANDLE;
    RasterVulkanShader Vertex;
    RasterVulkanShader Fragment;
    internal::RasterPipelineInfo Info;
    RasterVulkanPipeline Pipeline;
    ~VulkanRasterPreparation() noexcept override
    {
        if (Pipeline.Object != VK_NULL_HANDLE)
        {
            vkDestroyPipeline(Device, Pipeline.Object, nullptr);
        }
        if (Pipeline.Layout != VK_NULL_HANDLE)
        {
            vkDestroyPipelineLayout(Device, Pipeline.Layout, nullptr);
        }
    }
    VkResult Outcome = VK_NOT_READY;
    VkResult Build() noexcept;
    void Execute() noexcept override
    {
        Outcome = Build();
    }
    RasterStatus Complete(bool) noexcept override
    {
        gRasterPipelines[ResourceSlot] = Pipeline;
        Pipeline = {};
        const RasterStatus status = GetPreparationStatus(Outcome);
        internal::RasterComplete(Request, status);
        return status;
    }
};
struct VulkanShaderPreparation final : NativePreparationTask
{
    VkDevice Device = VK_NULL_HANDLE;
    RasterVulkanShader Shader;
    internal::StorageArray<uint32> Code;
    ~VulkanShaderPreparation() noexcept override
    {
        if (Shader.Object != VK_NULL_HANDLE)
        {
            vkDestroyShaderModule(Device, Shader.Object, nullptr);
        }
    }
    VkResult Outcome = VK_NOT_READY;
    void Execute() noexcept override
    {
        const VkShaderModuleCreateInfo create
        {
            .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
            .codeSize = Code.GetSize() * sizeof(uint32),
            .pCode = Code.GetData(),
        };
        Outcome = vkCreateShaderModule(Device, &create, nullptr, &Shader.Object);
    }
    RasterStatus Complete(bool) noexcept override
    {
        gRasterShaders[ResourceSlot] = Shader;
        Shader.Object = VK_NULL_HANDLE;
        const RasterStatus status = GetPreparationStatus(Outcome);
        internal::RasterComplete(Request, status);
        return status;
    }
};
RasterStatus ComputeCreatePipeline(usize slot, const internal::ComputePipelineInfo& info, uint32 request) noexcept
{
    UniquePtr<VulkanComputePreparation> task = MakePreparationTask<VulkanComputePreparation>();
    if (task.Get() == nullptr)
    {
        return internal::StorageFailure();
    }
    task->IsPipelinePreparation = true;
    task->Kind = internal::RasterKind::ComputePipeline;
    task->Device = gDevice;
    task->Bindings = gRasterLayouts[info.Layout];
    task->Shader = gRasterShaders[info.Shader];
    task->ResourceSlot = slot;
    task->Request = request;
    return SubmitNativePreparation(foundation::Move(task), info.ShouldPrepareAsynchronously);
}
RasterStatus ComputeEncode(const internal::ComputePacket& packet) noexcept
{
    const auto command = gFrames[gFrame].Command;
    const auto& pipeline = gComputePipelines[packet.Pipeline];
    vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline.Object);
    vkCmdBindDescriptorSets(command,
                            VK_PIPELINE_BIND_POINT_COMPUTE,
                            pipeline.Layout,
                            0,
                            1,
                            &gRasterSets[packet.Set].Object,
                            packet.DynamicOffsetCount,
                            packet.DynamicOffsets);
    vkCmdDispatch(command, packet.Groups[0], packet.Groups[1], packet.Groups[2]);
    return RasterStatus::Ready;
}
// Thanks to Khronos, "Synchronization Examples", compute-to-compute and
// compute-to-graphics dependencies. This adapts the narrow stage/access pairs to
// Vulkan 1.1 barriers and retains the original writer for later independent reads.
// https://docs.vulkan.org/guide/latest/synchronization_examples.html
void ComputeBufferBarrier(usize slot, GraphAccessMode use) noexcept
{
    auto& buffer = gRasterBuffers[slot];
    VkPipelineStageFlags stage = 0;
    VkAccessFlags access = 0;
    switch (use)
    {
        case GraphAccessMode::Vertex:
            stage = VK_PIPELINE_STAGE_VERTEX_INPUT_BIT;
            access = VK_ACCESS_VERTEX_ATTRIBUTE_READ_BIT;
            break;
        case GraphAccessMode::Index:
            stage = VK_PIPELINE_STAGE_VERTEX_INPUT_BIT;
            access = VK_ACCESS_INDEX_READ_BIT;
            break;
        case GraphAccessMode::Uniform:
            stage = VK_PIPELINE_STAGE_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT |
                    (gQueueCompute ? VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT : 0U);
            access = VK_ACCESS_UNIFORM_READ_BIT;
            break;
        case GraphAccessMode::Indirect:
            stage = VK_PIPELINE_STAGE_DRAW_INDIRECT_BIT;
            access = VK_ACCESS_INDIRECT_COMMAND_READ_BIT;
            break;
        case GraphAccessMode::StorageRead:
            stage = VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
            access = VK_ACCESS_SHADER_READ_BIT;
            break;
        case GraphAccessMode::StorageReadWrite:
            stage = VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
            access = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
            break;
        default:
            return;
    }
    VkBufferMemoryBarrier barrier{};
    barrier.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
    barrier.buffer = buffer.Object;
    barrier.size = VK_WHOLE_SIZE;
    barrier.srcQueueFamilyIndex = barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.srcAccessMask = buffer.Access | buffer.WriterAccess;
    barrier.dstAccessMask = access;
    vkCmdPipelineBarrier(gFrames[gFrame].Command,
                         buffer.Stage | buffer.WriterStage,
                         stage,
                         0,
                         0,
                         nullptr,
                         1,
                         &barrier,
                         0,
                         nullptr);
    if (use == GraphAccessMode::StorageReadWrite)
    {
        buffer.Stage = buffer.WriterStage = stage;
        buffer.Access = access;
        buffer.WriterAccess = VK_ACCESS_SHADER_WRITE_BIT;
    }
    else
    {
        buffer.Stage |= stage;
        buffer.Access |= access;
    }
}
RasterStatus
RasterCreateBuffer(usize slot, const BufferDescription& info, std::span<const uint8> bytes, uint32) noexcept
{
    const auto usage = info.Role == BufferRole::Uniform   ? VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT
                       : info.Role == BufferRole::Vertex  ? VK_BUFFER_USAGE_VERTEX_BUFFER_BIT
                       : info.Role == BufferRole::Storage ? VK_BUFFER_USAGE_STORAGE_BUFFER_BIT
                       : info.Role == BufferRole::StorageVertex
                           ? VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_VERTEX_BUFFER_BIT
                       : info.Role == BufferRole::StorageIndirect
                           ? VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT
                           : VK_BUFFER_USAGE_INDEX_BUFFER_BIT;
    return RasterError(RasterUploadBuffer(usage | VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                          bytes,
                                          gRasterBuffers[slot]));
}
RasterStatus
RasterCreateTexture(usize slot, const TextureDescription& info, const TextureUpload& upload, uint32 request) noexcept
{
    VkFormatProperties properties{};
    const auto format = RasterPixelFormat(info.Format);
    vkGetPhysicalDeviceFormatProperties(gPhysical, format, &properties);
    const VkFormatFeatureFlags required =
        VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT |
        VK_FORMAT_FEATURE_TRANSFER_DST_BIT |
        (info.Format == RasterFormat::Rgba8Unorm ? VK_FORMAT_FEATURE_TRANSFER_SRC_BIT : 0U);
    const auto features = required | (info.Attachment ? VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT : 0U);
    if ((properties.optimalTilingFeatures & features) != features)
    {
        return RasterStatus::Unsupported;
    }
    auto result = RasterAllocator();
    if (result != VK_SUCCESS)
    {
        return RasterError(result);
    }
    auto& texture = gRasterTextures[slot];
    texture.Info = info;
    VkImageCreateInfo image{ .samples = VK_SAMPLE_COUNT_1_BIT };
    image.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    image.imageType = info.Dimension == TextureDimension::D3 ? VK_IMAGE_TYPE_3D : VK_IMAGE_TYPE_2D;
    image.flags = info.Dimension == TextureDimension::Cube ? VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT : 0U;
    image.format = format;
    image.extent = {info.Width, info.Height, info.Dimension == TextureDimension::D3 ? info.DepthOrLayers : 1U};
    image.mipLevels = info.MipLevels;
    image.arrayLayers = info.Dimension == TextureDimension::D3 ? 1U : info.DepthOrLayers;
    image.samples = static_cast<VkSampleCountFlagBits>(info.SampleCount);
    image.tiling = VK_IMAGE_TILING_OPTIMAL;
    image.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                  (info.Format == RasterFormat::Rgba8Unorm ? VK_IMAGE_USAGE_TRANSFER_SRC_BIT : 0U) |
                  (info.Attachment ? VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT : 0U);
    VmaAllocationCreateInfo allocation{};
    allocation.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
    result = vmaCreateImage(gRasterAllocator, &image, &allocation, &texture.Object, &texture.Allocation, nullptr);
    if (result != VK_SUCCESS)
    {
        return RasterError(result);
    }
    if (info.Attachment)
    {
        image.format = info.Stencil ? VK_FORMAT_D32_SFLOAT_S8_UINT : gDepthFormat;
        image.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
        result =
            vmaCreateImage(gRasterAllocator, &image, &allocation, &texture.Depth, &texture.DepthAllocation, nullptr);
        VkImageViewCreateInfo view{};
        view.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        view.viewType = VK_IMAGE_VIEW_TYPE_2D;
        view.image = texture.Object;
        view.format = format;
        view.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        if (result == VK_SUCCESS)
        {
            result = vkCreateImageView(gDevice, &view, nullptr, &texture.ColorView);
        }
        view.image = texture.Depth;
        view.format = image.format;
        view.subresourceRange.aspectMask =
            VK_IMAGE_ASPECT_DEPTH_BIT | (info.Stencil ? VK_IMAGE_ASPECT_STENCIL_BIT : 0U);
        if (result == VK_SUCCESS)
        {
            result = vkCreateImageView(gDevice, &view, nullptr, &texture.DepthView);
        }
        if (info.SampleCount != 1 || info.Stencil || info.MipLevels != 1 || info.Dimension != TextureDimension::D2)
        {
            return RasterError(result);
        }
        VkRenderPass pass = VK_NULL_HANDLE;
        if (result == VK_SUCCESS)
        {
            result = RasterVkRenderPass(format, false, {}, pass);
        }
        VkFramebufferCreateInfo framebuffer{};
        framebuffer.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
        const VkImageView views[2]{texture.ColorView, texture.DepthView};
        framebuffer.renderPass = pass;
        framebuffer.attachmentCount = 2;
        framebuffer.pAttachments = views;
        framebuffer.width = info.Width;
        framebuffer.height = info.Height;
        framebuffer.layers = 1;
        if (result == VK_SUCCESS)
        {
            result = vkCreateFramebuffer(gDevice, &framebuffer, nullptr, &texture.Framebuffer);
        }
        return RasterError(result);
    }
    if (info.DeferredInitialization)
    {
        return RasterStatus::Ready;
    }
    texture.ColorLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    texture.ColorStage = VK_PIPELINE_STAGE_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
    texture.ColorAccess = VK_ACCESS_SHADER_READ_BIT;
    texture.ColorWriterStage = VK_PIPELINE_STAGE_TRANSFER_BIT;
    texture.ColorWriterAccess = VK_ACCESS_TRANSFER_WRITE_BIT;
    texture.SampleVisibility = VK_PIPELINE_STAGE_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
    RasterVulkanBuffer staging;
    result = RasterUploadBuffer(VK_BUFFER_USAGE_TRANSFER_SRC_BIT, upload.Bytes, staging);
    VkCommandBuffer command = VK_NULL_HANDLE;
    VkFence fence = VK_NULL_HANDLE;
    if (result == VK_SUCCESS)
    {
        VkCommandBufferAllocateInfo allocate{};
        allocate.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        allocate.commandPool = gCommands;
        allocate.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        allocate.commandBufferCount = 1;
        result = vkAllocateCommandBuffers(gDevice, &allocate, &command);
    }
    if (result == VK_SUCCESS)
    {
        VkCommandBufferBeginInfo begin{};
        begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        result = vkBeginCommandBuffer(command, &begin);
    }
    if (result == VK_SUCCESS)
    {
        VkImageMemoryBarrier barrier{};
        barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        barrier.srcQueueFamilyIndex = barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        barrier.image = texture.Object;
        barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, info.MipLevels, 0, image.arrayLayers};
        vkCmdPipelineBarrier(command,
                             VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                             VK_PIPELINE_STAGE_TRANSFER_BIT,
                             0,
                             0,
                             nullptr,
                             0,
                             nullptr,
                             1,
                             &barrier);
        VkBufferImageCopy copies[16]{};
        uint32 width = info.Width, height = info.Height;
        for (uint32 level = 0; level < info.MipLevels; ++level)
        {
            const auto mip = upload.Mips.empty() ? TextureMipUpload{0, upload.RowPitch} : upload.Mips[level];
            const auto channels = internal::TextureBlockBytes(info.Format);
            const auto block = internal::TextureBlockWidth(info.Format);
            copies[level].bufferOffset = mip.Offset;
            copies[level].bufferRowLength = static_cast<uint32>(mip.RowPitch / channels) * block;
            copies[level].bufferImageHeight = internal::TextureBlockRows(info.Format, height) * block;
            copies[level].imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, level, 0, image.arrayLayers};
            copies[level].imageExtent = {width,
                                         height,
                                         info.Dimension == TextureDimension::D3 ? internal::TextureSlices(info, level)
                                                                                : 1U};
            width = width > 1 ? width / 2 : 1;
            height = height > 1 ? height / 2 : 1;
        }
        vkCmdCopyBufferToImage(command,
                               staging.Object,
                               texture.Object,
                               VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                               info.MipLevels,
                               copies);
        barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        vkCmdPipelineBarrier(command,
                             VK_PIPELINE_STAGE_TRANSFER_BIT,
                             VK_PIPELINE_STAGE_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                             0,
                             0,
                             nullptr,
                             0,
                             nullptr,
                             1,
                             &barrier);
        result = vkEndCommandBuffer(command);
    }
    if (result == VK_SUCCESS)
    {
        VkFenceCreateInfo create{};
        create.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
        result = vkCreateFence(gDevice, &create, nullptr, &fence);
    }
    bool submitted = false;
    if (result == VK_SUCCESS)
    {
        VkSubmitInfo submit{};
        submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        submit.commandBufferCount = 1;
        submit.pCommandBuffers = &command;
        result = vkQueueSubmit(gQueue, 1, &submit, fence);
        submitted = result == VK_SUCCESS;
    }
    // Immutable content/atlas replacement is a streaming operation: retain owned staging and
    // poll its fence through RasterCompleted instead of waiting on the GPU.
    if (submitted && (info.AsyncUpload || info.MipLevels > 1 || info.Format == RasterFormat::R8Unorm ||
                      info.Format == RasterFormat::Rg8Unorm))
    {
        texture.Upload = staging;
        texture.Command = command;
        texture.Fence = fence;
        texture.Request = request;
        internal::RasterExpect(request, internal::RasterCallbacks::One);
        return RasterStatus::Pending;
    }
    if (submitted)
    {
        result = vkWaitForFences(gDevice, 1, &fence, VK_TRUE, 5000000000ULL);
    }
    // Timeout or host-memory pressure does not establish GPU completion. Keep
    // the staging allocation and command until the fence signals; device loss
    // permits immediate teardown. Never infer completion from an unchecked wait.
    if (submitted && result != VK_SUCCESS && result != VK_ERROR_DEVICE_LOST)
    {
        texture.Upload = staging;
        texture.Command = command;
        texture.Fence = fence;
        texture.Request = request;
        internal::RasterExpect(request, internal::RasterCallbacks::One);
        return RasterStatus::Pending;
    }
    if (command != VK_NULL_HANDLE)
    {
        vkFreeCommandBuffers(gDevice, gCommands, 1, &command);
    }
    vkDestroyFence(gDevice, fence, nullptr);
    if (staging.Object != VK_NULL_HANDLE)
    {
        vmaDestroyBuffer(gRasterAllocator, staging.Object, staging.Allocation);
    }
    return RasterError(result);
}
RasterStatus RasterCreateView(usize slot, const internal::RasterViewSource& source, uint32) noexcept
{
    auto& texture = gRasterTextures[source.Texture];
    const auto& range = source.View;
    VkImageViewCreateInfo view{};
    view.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    view.viewType = range.Dimension == TextureDimension::D2Array ? VK_IMAGE_VIEW_TYPE_2D_ARRAY
                    : range.Dimension == TextureDimension::Cube  ? VK_IMAGE_VIEW_TYPE_CUBE
                    : range.Dimension == TextureDimension::D3    ? VK_IMAGE_VIEW_TYPE_3D
                                                                 : VK_IMAGE_VIEW_TYPE_2D;
    view.image = range.Aspect == TextureAspect::Color ? texture.Object : texture.Depth;
    view.format = range.Aspect == TextureAspect::Color ? RasterPixelFormat(texture.Info.Format)
                  : texture.Info.Stencil               ? VK_FORMAT_D32_SFLOAT_S8_UINT
                                                       : gDepthFormat;
    view.subresourceRange = {range.Aspect == TextureAspect::Color   ? VK_IMAGE_ASPECT_COLOR_BIT
                             : range.Aspect == TextureAspect::Depth ? VK_IMAGE_ASPECT_DEPTH_BIT
                                                                    : VK_IMAGE_ASPECT_STENCIL_BIT,
                             range.BaseMip,
                             range.MipCount,
                             range.BaseLayer,
                             range.LayerCount};
    auto result = vkCreateImageView(gDevice, &view, nullptr, &gRasterViews[slot]);
    if (result == VK_SUCCESS && range.Attachment)
    {
        view.image = texture.Depth;
        view.format = texture.Info.Stencil ? VK_FORMAT_D32_SFLOAT_S8_UINT : gDepthFormat;
        view.subresourceRange.aspectMask =
            VK_IMAGE_ASPECT_DEPTH_BIT | (texture.Info.Stencil ? VK_IMAGE_ASPECT_STENCIL_BIT : 0U);
        result = vkCreateImageView(gDevice, &view, nullptr, &gRasterViewDepths[slot]);
    }
    return RasterError(result);
}
RasterStatus RasterCreateSampler(usize slot, const SamplerDescription& info, uint32) noexcept
{
    VkSamplerCreateInfo create{};
    create.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    create.minFilter = create.magFilter = info.Filter == RasterFilter::Nearest ? VK_FILTER_NEAREST : VK_FILTER_LINEAR;
    create.mipmapMode =
        info.MipFilter == RasterMipFilter::Linear ? VK_SAMPLER_MIPMAP_MODE_LINEAR : VK_SAMPLER_MIPMAP_MODE_NEAREST;
    create.minLod = info.MinLod;
    create.maxLod = info.MipFilter == RasterMipFilter::None ? 0 : info.MaxLod;
    create.addressModeU =
        info.U == RasterAddress::Clamp ? VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE : VK_SAMPLER_ADDRESS_MODE_REPEAT;
    create.addressModeV =
        info.V == RasterAddress::Clamp ? VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE : VK_SAMPLER_ADDRESS_MODE_REPEAT;
    create.addressModeW =
        info.W == RasterAddress::Clamp ? VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE : VK_SAMPLER_ADDRESS_MODE_REPEAT;
    return RasterError(vkCreateSampler(gDevice, &create, nullptr, &gRasterSamplers[slot]));
}
RasterStatus RasterCreateShader(usize slot,
                                const ShaderDescription& info,
                                const internal::RasterShaderInfo& reflected,
                                uint32 request) noexcept
{
    bool found = false;
    uint32 entryId = 0;
    bool localSize = info.Stage != ShaderStage::Compute;
    for (usize offset = 5; offset < info.Spirv.size();)
    {
        const auto word = info.Spirv[offset];
        const usize count = word >> 16;
        if (count == 0 || count > info.Spirv.size() - offset)
        {
            return RasterStatus::InvalidDescription;
        }
        if ((word & 65535U) == 15 && count >= 4)
        {
            const auto* name = reinterpret_cast<const char*>(info.Spirv.data() + offset + 3);
            const auto length = info.SpirvEntry.size();
            found = found || (info.Spirv[offset + 1] == (info.Stage == ShaderStage::Vertex     ? 0U
                                                         : info.Stage == ShaderStage::Fragment ? 4U
                                                                                               : 5U) &&
                              length < (count - 3) * sizeof(uint32) && name[length] == '\0' &&
                              std::memcmp(name, info.SpirvEntry.data(), length) == 0);
        }
        if ((word & 65535U) == 15 && count >= 4 && found && entryId == 0)
        {
            entryId = info.Spirv[offset + 2];
        }
        if ((word & 65535U) == 16 && count == 6 && info.Spirv[offset + 1] == entryId && info.Spirv[offset + 2] == 17)
        {
            localSize = info.Spirv[offset + 3] == reflected.WorkgroupSize[0] &&
                        info.Spirv[offset + 4] == reflected.WorkgroupSize[1] &&
                        info.Spirv[offset + 5] == reflected.WorkgroupSize[2];
        }
        offset += count;
    }
    if (found == false || localSize == false)
    {
        return RasterStatus::InvalidDescription;
    }
    UniquePtr<VulkanShaderPreparation> task = MakePreparationTask<VulkanShaderPreparation>();
    if (task.Get() == nullptr)
    {
        return internal::StorageFailure();
    }
    if (task->Code.TryResize(info.Spirv.size()) == false)
    {
        return internal::StorageFailure();
    }
    std::memcpy(task->Code.GetData(), info.Spirv.data(), info.Spirv.size_bytes());
    std::memcpy(task->Shader.Entry, info.SpirvEntry.data(), info.SpirvEntry.size());
    task->Device = gDevice;
    task->ResourceSlot = slot;
    task->Request = request;
    return SubmitNativePreparation(foundation::Move(task), reflected.ShouldPrepareAsynchronously);
}
RasterStatus RasterCreateLayout(usize slot, const internal::RasterLayout& info, uint32) noexcept
{
    foundation::StaticArray<VkDescriptorSetLayoutBinding, internal::RASTER_BINDINGS> entries{};
    foundation::StaticArray<VkDescriptorBindingFlags, internal::RASTER_BINDINGS> flags{};
    uint32 runtimeCapacity = 0;
    for (usize index = 0; index < info.Count; ++index)
    {
        const RasterBinding& entry = info.Entries[index];
        VkShaderStageFlags visibility = 0;
        if ((static_cast<uint8>(entry.Visibility) & 1) != 0)
        {
            visibility |= VK_SHADER_STAGE_VERTEX_BIT;
        }
        if ((static_cast<uint8>(entry.Visibility) & 4) != 0)
        {
            visibility |= VK_SHADER_STAGE_COMPUTE_BIT;
        }
        if ((static_cast<uint8>(entry.Visibility) & 2) != 0)
        {
            visibility |= VK_SHADER_STAGE_FRAGMENT_BIT;
        }
        if (entry.IsRuntimeSized == true)
        {
            flags[index] = VK_DESCRIPTOR_BINDING_VARIABLE_DESCRIPTOR_COUNT_BIT;
            runtimeCapacity = entry.Count;
        }
        entries[index] =
        {
            .binding = entry.Binding,
            .descriptorType = RasterDescriptor(entry.Kind, entry.HasDynamicOffset),
            .descriptorCount = entry.Count,
            .stageFlags = visibility,
        };
    }
    // Thanks to Khronos Group, Vulkan Guide, "Descriptor Arrays", and
    // VkDescriptorBindingFlagBits, for the independent runtime array/allocation
    // features and highest-binding rule. Sparse logical slots use valid fallback
    // descriptors; immutable replacement never enables native UPDATE_AFTER_BIND.
    // https://docs.vulkan.org/guide/latest/descriptor_arrays.html
    const VkDescriptorSetLayoutBindingFlagsCreateInfo bindingFlags
    {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO,
        .bindingCount = static_cast<uint32>(info.Count),
        .pBindingFlags = flags.GetData(),
    };
    const VkDescriptorSetLayoutCreateInfo create
    {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
        .pNext = runtimeCapacity == 0 ? nullptr : &bindingFlags,
        .bindingCount = static_cast<uint32>(info.Count),
        .pBindings = entries.GetData(),
    };
    if (runtimeCapacity != 0)
    {
        VkDescriptorSetVariableDescriptorCountLayoutSupport variable
        {
            .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_VARIABLE_DESCRIPTOR_COUNT_LAYOUT_SUPPORT,
        };
        VkDescriptorSetLayoutSupport support
        {
            .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_SUPPORT,
            .pNext = &variable,
        };
        vkGetDescriptorSetLayoutSupport(gDevice, &create, &support);
        if (support.supported == VK_FALSE || runtimeCapacity > variable.maxVariableDescriptorCount)
        {
            return RasterStatus::Unsupported;
        }
    }
    return RasterError(vkCreateDescriptorSetLayout(gDevice, &create, nullptr, &gRasterLayouts[slot]));
}
RasterStatus
RasterCreateSet(usize slot, const internal::RasterLayout& layout, const internal::RasterSet& info, uint32) noexcept
{
    // Thanks to Khronos Group, Vulkan Samples, "Descriptor and buffer management",
    // https://docs.vulkan.org/samples/latest/samples/performance/descriptor_management/README.html, for pool
    // amortization. Immutable snapshots retire through the common completion
    // registry; individual sets are freed only after their final GPU use.
    RasterVulkanSet& set = gRasterSets[slot];
    constexpr foundation::StaticArray<VkDescriptorType, DESCRIPTOR_KINDS> types = {
        {
            VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
            VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,
            VK_DESCRIPTOR_TYPE_SAMPLER,
            VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
            VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC,
            VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC,
        },
    };
    foundation::StaticArray<uint32, DESCRIPTOR_KINDS> counts{};
    for (usize entry = 0; entry < layout.Count; ++entry)
    {
        const RasterBinding& binding = layout.Entries[entry];
        for (usize kind = 0; kind < DESCRIPTOR_KINDS; ++kind)
        {
            if (types[kind] == RasterDescriptor(binding.Kind, binding.HasDynamicOffset))
            {
                counts[kind] += binding.Count;
            }
        }
    }
    usize arenaSlot = internal::RASTER_INVALID;
    usize emptySlot = internal::RASTER_INVALID;
    for (usize index = 0; index < gRasterDescriptorArenas.GetSize(); ++index)
    {
        const RasterDescriptorArena& arena = gRasterDescriptorArenas[index];
        if (arena.Pool == VK_NULL_HANDLE)
        {
            emptySlot = index;
            continue;
        }
        bool hasMatchingCounts = arena.LiveSets < arena.Capacity;
        for (usize kind = 0; kind < DESCRIPTOR_KINDS; ++kind)
        {
            hasMatchingCounts = hasMatchingCounts == true && arena.Counts[kind] == counts[kind];
        }
        if (hasMatchingCounts == true)
        {
            arenaSlot = index;
            break;
        }
    }
    if (arenaSlot == internal::RASTER_INVALID)
    {
        if (emptySlot == internal::RASTER_INVALID)
        {
            emptySlot = gRasterDescriptorArenas.GetSize();
            if (gRasterDescriptorArenas.EnsureSlots(emptySlot + 1) == false)
            {
                return internal::StorageFailure();
            }
        }
        RasterDescriptorArena& arena = gRasterDescriptorArenas[emptySlot];
        const uint32 recordLimit = internal::CurrentStorageLimits().ResourcesPerKind;
        arena.Capacity = recordLimit < DESCRIPTOR_SETS_PER_ARENA ? recordLimit : DESCRIPTOR_SETS_PER_ARENA;
        for (uint32 count : counts)
        {
            if (count != 0 && arena.Capacity > ~uint32{0} / count)
            {
                arena.Capacity = ~uint32{0} / count;
            }
        }
        foundation::StaticArray<VkDescriptorPoolSize, DESCRIPTOR_KINDS> sizes{};
        uint32 sizeCount = 0;
        for (usize kind = 0; kind < DESCRIPTOR_KINDS; ++kind)
        {
            arena.Counts[kind] = counts[kind];
            if (counts[kind] != 0)
            {
                sizes[sizeCount++] = { .type = types[kind], .descriptorCount = counts[kind] * arena.Capacity };
            }
        }
        if (sizeCount == 0)
        {
            sizes[sizeCount++] = { .type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, .descriptorCount = arena.Capacity };
        }
        const VkDescriptorPoolCreateInfo create
        {
            .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
            .flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT,
            .maxSets = arena.Capacity,
            .poolSizeCount = sizeCount,
            .pPoolSizes = sizes.GetData(),
        };
        const VkResult result = vkCreateDescriptorPool(gDevice, &create, nullptr, &arena.Pool);
        if (result != VK_SUCCESS)
        {
            arena = {};
            return RasterError(result);
        }
        arenaSlot = emptySlot;
    }
    RasterDescriptorArena& arena = gRasterDescriptorArenas[arenaSlot];
    uint32 runtimeCount = 0;
    for (usize entry = 0; entry < layout.Count; ++entry)
    {
        if (layout.Entries[entry].IsRuntimeSized == true)
        {
            runtimeCount = layout.Entries[entry].Count;
        }
    }
    const VkDescriptorSetVariableDescriptorCountAllocateInfo variable
    {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_VARIABLE_DESCRIPTOR_COUNT_ALLOCATE_INFO,
        .descriptorSetCount = 1,
        .pDescriptorCounts = &runtimeCount,
    };
    const VkDescriptorSetAllocateInfo allocate
    {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
        .pNext = runtimeCount == 0 ? nullptr : &variable,
        .descriptorPool = arena.Pool,
        .descriptorSetCount = 1,
        .pSetLayouts = &gRasterLayouts[info.Layout],
    };
    const VkResult result = vkAllocateDescriptorSets(gDevice, &allocate, &set.Object);
    if (result != VK_SUCCESS)
    {
        if (arena.LiveSets == 0)
        {
            vkDestroyDescriptorPool(gDevice, arena.Pool, nullptr);
            arena = {};
        }
        set.Object = VK_NULL_HANDLE;
        return RasterError(result);
    }
    ++arena.LiveSets;
    set.Arena = arenaSlot;
    internal::StorageArray<VkWriteDescriptorSet> writes;
    internal::StorageArray<VkDescriptorBufferInfo> buffers;
    internal::StorageArray<VkDescriptorImageInfo> images;
    if (writes.TryResize(layout.DescriptorCount) == false || buffers.TryResize(layout.DescriptorCount) == false ||
        images.TryResize(layout.DescriptorCount) == false)
    {
        return internal::StorageFailure();
    }
    for (usize index = 0; index < layout.DescriptorCount; ++index)
    {
        const RasterBinding& entry = layout.GetDescriptorBinding(index);
        const bool isBuffer = entry.Kind == RasterBindingKind::UniformBuffer ||
                              entry.Kind == RasterBindingKind::StorageRead ||
                              entry.Kind == RasterBindingKind::StorageReadWrite;
        if (isBuffer == true)
        {
            buffers[index] =
            {
                .buffer = gRasterBuffers[info.Slots[index]].Object,
                .offset = info.Offsets[index],
                .range = info.Sizes[index],
            };
        }
        else
        {
            const bool isTexture = internal::TextureBinding(entry.Kind);
            images[index] =
            {
                .sampler = isTexture == false ? gRasterSamplers[info.Slots[index]] : VK_NULL_HANDLE,
                .imageView = isTexture == true ? gRasterViews[info.Slots[index]] : VK_NULL_HANDLE,
                .imageLayout = isTexture == true ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL : VK_IMAGE_LAYOUT_UNDEFINED,
            };
        }
        writes[index] =
        {
            .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
            .dstSet = set.Object,
            .dstBinding = entry.Binding,
            .dstArrayElement = layout.GetArrayElement(index),
            .descriptorCount = 1,
            .descriptorType = RasterDescriptor(entry.Kind, entry.HasDynamicOffset),
            .pImageInfo = isBuffer == false ? &images[index] : nullptr,
            .pBufferInfo = isBuffer == true ? &buffers[index] : nullptr,
        };
    }
    vkUpdateDescriptorSets(gDevice, static_cast<uint32>(layout.DescriptorCount), writes.GetData(), 0, nullptr);
    return RasterStatus::Ready;
}
VkResult VulkanRasterPreparation::Build() noexcept
{
    RasterVulkanPipeline& pipeline = Pipeline;
    const internal::RasterPipelineInfo& info = Info;
    pipeline.Info = info;
    VkPipelineLayoutCreateInfo layout{};
    layout.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    layout.setLayoutCount = 1;
    layout.pSetLayouts = &Bindings;
    VkResult result = vkCreatePipelineLayout(Device, &layout, nullptr, &pipeline.Layout);
    if (result != VK_SUCCESS)
    {
        return result;
    }
    VkPipelineShaderStageCreateInfo stages[2]{
        { .stage = VK_SHADER_STAGE_VERTEX_BIT },
        { .stage = VK_SHADER_STAGE_FRAGMENT_BIT },
    };
    stages[0].sType = stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[0].module = Vertex.Object;
    stages[1].module = Fragment.Object;
    stages[0].pName = Vertex.Entry;
    stages[1].pName = Fragment.Entry;
    VkVertexInputBindingDescription bindings[2]{};
    VkVertexInputAttributeDescription attributes[16]{};
    for (usize i = 0; i < info.StreamCount; ++i)
    {
        bindings[i] = {static_cast<uint32>(i),
                       info.Streams[i].Stride,
                       info.Streams[i].PerInstance ? VK_VERTEX_INPUT_RATE_INSTANCE : VK_VERTEX_INPUT_RATE_VERTEX};
    }
    for (usize i = 0; i < info.AttributeCount; ++i)
    {
        const auto& entry = info.Attributes[i];
        attributes[i] = {entry.Location,
                         entry.Stream,
                         entry.Format == RasterVertexFormat::Float2   ? VK_FORMAT_R32G32_SFLOAT
                         : entry.Format == RasterVertexFormat::Float3 ? VK_FORMAT_R32G32B32_SFLOAT
                                                                      : VK_FORMAT_R32G32B32A32_SFLOAT,
                         entry.Offset};
    }
    VkPipelineVertexInputStateCreateInfo input{};
    input.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    input.vertexBindingDescriptionCount = static_cast<uint32>(info.StreamCount);
    input.pVertexBindingDescriptions = bindings;
    input.vertexAttributeDescriptionCount = static_cast<uint32>(info.AttributeCount);
    input.pVertexAttributeDescriptions = attributes;
    VkPipelineInputAssemblyStateCreateInfo assembly{};
    assembly.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkPipelineViewportStateCreateInfo viewport{};
    viewport.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    viewport.viewportCount = viewport.scissorCount = 1;
    VkPipelineRasterizationStateCreateInfo raster{};
    raster.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    raster.polygonMode = VK_POLYGON_MODE_FILL;
    raster.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE; // Canonical clip-space CCW with the negative-height viewport.
    raster.lineWidth = 1;
    VkPipelineMultisampleStateCreateInfo samples{ .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT };
    samples.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    samples.rasterizationSamples = static_cast<VkSampleCountFlagBits>(info.SampleCount);
    VkPipelineDepthStencilStateCreateInfo depth{};
    depth.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    depth.depthTestEnable = info.Depth;
    depth.depthWriteEnable = info.Depth && info.DepthWrite;
    const VkCompareOp comparisons[]{VK_COMPARE_OP_LESS,
                                    VK_COMPARE_OP_LESS_OR_EQUAL,
                                    VK_COMPARE_OP_GREATER,
                                    VK_COMPARE_OP_GREATER_OR_EQUAL,
                                    VK_COMPARE_OP_ALWAYS};
    depth.depthCompareOp = comparisons[static_cast<usize>(info.DepthCompare)];
    depth.stencilTestEnable = info.Stencil;
    const VkCompareOp stencilCompare[]{VK_COMPARE_OP_NEVER,
                                       VK_COMPARE_OP_LESS,
                                       VK_COMPARE_OP_EQUAL,
                                       VK_COMPARE_OP_LESS_OR_EQUAL,
                                       VK_COMPARE_OP_GREATER,
                                       VK_COMPARE_OP_NOT_EQUAL,
                                       VK_COMPARE_OP_GREATER_OR_EQUAL,
                                       VK_COMPARE_OP_ALWAYS};
    const VkStencilOp ops[]{VK_STENCIL_OP_KEEP,
                            VK_STENCIL_OP_ZERO,
                            VK_STENCIL_OP_REPLACE,
                            VK_STENCIL_OP_INCREMENT_AND_CLAMP,
                            VK_STENCIL_OP_DECREMENT_AND_CLAMP,
                            VK_STENCIL_OP_INVERT,
                            VK_STENCIL_OP_INCREMENT_AND_WRAP,
                            VK_STENCIL_OP_DECREMENT_AND_WRAP};
    const RasterStencilFace faces[]{info.StencilFront, info.StencilBack};
    VkStencilOpState* output[]{&depth.front, &depth.back};
    for (usize i = 0; i < 2; ++i)
    {
        output[i]->compareOp = stencilCompare[static_cast<usize>(faces[i].Compare)];
        output[i]->failOp = ops[static_cast<usize>(faces[i].Fail)];
        output[i]->depthFailOp = ops[static_cast<usize>(faces[i].DepthFail)];
        output[i]->passOp = ops[static_cast<usize>(faces[i].Pass)];
        output[i]->compareMask = info.StencilReadMask;
        output[i]->writeMask = info.StencilWriteMask;
        output[i]->reference = info.StencilReference;
    }
    VkPipelineColorBlendAttachmentState color{};
    color.colorWriteMask = 15;
    color.blendEnable = info.Blend;
    color.srcColorBlendFactor = color.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
    color.dstColorBlendFactor = color.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    VkPipelineColorBlendStateCreateInfo blend{};
    blend.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    VkPipelineColorBlendAttachmentState colors[4]{};
    for (auto& entry : colors)
    {
        entry = color;
    }
    blend.attachmentCount = info.ColorCount == 0 ? 1 : static_cast<uint32>(info.ColorCount);
    blend.pAttachments = colors;
    const VkDynamicState states[2]{VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dynamic{};
    dynamic.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dynamic.dynamicStateCount = 2;
    dynamic.pDynamicStates = states;
    VkGraphicsPipelineCreateInfo create{};
    create.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    create.stageCount = 2;
    create.pStages = stages;
    create.pVertexInputState = &input;
    create.pInputAssemblyState = &assembly;
    create.pViewportState = &viewport;
    create.pRasterizationState = &raster;
    create.pMultisampleState = &samples;
    create.pDepthStencilState = &depth;
    create.pColorBlendState = &blend;
    create.pDynamicState = &dynamic;
    create.layout = pipeline.Layout;
    create.renderPass = Pass;
    return vkCreateGraphicsPipelines(Device, VK_NULL_HANDLE, 1, &create, nullptr, &pipeline.Object);
}
RasterStatus RasterCreatePipeline(usize slot, const internal::RasterPipelineInfo& info, uint32 request) noexcept
{
    UniquePtr<VulkanRasterPreparation> task = MakePreparationTask<VulkanRasterPreparation>();
    if (task.Get() == nullptr)
    {
        return internal::StorageFailure();
    }
    VkResult result = VK_SUCCESS;
    if (info.ColorCount == 0)
    {
        const VkFormat format = info.Target == RasterTarget::Surface       ? gFormat
                                : info.Target == RasterTarget::Rgba16Float ? VK_FORMAT_R16G16B16A16_SFLOAT
                                : info.Target == RasterTarget::Rgba8Unorm  ? VK_FORMAT_R8G8B8A8_UNORM
                                                                           : VK_FORMAT_R8G8B8A8_SRGB;
        result = RasterVkRenderPass(format, info.Target == RasterTarget::Surface, {}, task->Pass);
    }
    else
    {
        internal::RasterPassInfo pass;
        pass.ColorCount = info.ColorCount;
        pass.SampleCount = info.SampleCount;
        pass.Stencil = info.Stencil;
        for (usize i = 0; i < info.ColorCount; ++i)
        {
            pass.Colors[i].Format = info.ColorFormats[i];
        }
        result = RasterVkAdvancedPass(pass, task->Pass);
    }
    if (result != VK_SUCCESS)
    {
        return RasterError(result);
    }
    task->IsPipelinePreparation = true;
    task->Kind = internal::RasterKind::Pipeline;
    task->Device = gDevice;
    task->Bindings = gRasterLayouts[info.Layout];
    task->Vertex = gRasterShaders[info.Vertex];
    task->Fragment = gRasterShaders[info.Fragment];
    task->Info = info;
    task->ResourceSlot = slot;
    task->Request = request;
    return SubmitNativePreparation(foundation::Move(task), info.ShouldPrepareAsynchronously);
}
void RasterDestroy(internal::RasterKind kind, usize slot) noexcept
{
    if (gDevice == VK_NULL_HANDLE)
    {
        return;
    }
    switch (kind)
    {
        case internal::RasterKind::Buffer:
            if (gRasterBuffers[slot].Object != VK_NULL_HANDLE)
            {
                vmaDestroyBuffer(gRasterAllocator, gRasterBuffers[slot].Object, gRasterBuffers[slot].Allocation);
            }
            gRasterBuffers[slot] = {};
            break;
        case internal::RasterKind::Texture:
            vkDestroyFramebuffer(gDevice, gRasterTextures[slot].Framebuffer, nullptr);
            vkDestroyImageView(gDevice, gRasterTextures[slot].ColorView, nullptr);
            vkDestroyImageView(gDevice, gRasterTextures[slot].DepthView, nullptr);
            if (gRasterTextures[slot].Depth != VK_NULL_HANDLE)
            {
                vmaDestroyImage(gRasterAllocator, gRasterTextures[slot].Depth, gRasterTextures[slot].DepthAllocation);
            }
            if (gRasterTextures[slot].Command != VK_NULL_HANDLE)
            {
                vkFreeCommandBuffers(gDevice, gCommands, 1, &gRasterTextures[slot].Command);
                vkDestroyFence(gDevice, gRasterTextures[slot].Fence, nullptr);
                const auto& upload = gRasterTextures[slot].Upload;
                vmaDestroyBuffer(gRasterAllocator, upload.Object, upload.Allocation);
            }
            if (gRasterTextures[slot].Object != VK_NULL_HANDLE)
            {
                vmaDestroyImage(gRasterAllocator, gRasterTextures[slot].Object, gRasterTextures[slot].Allocation);
            }
            gRasterTextures[slot] = {};
            break;
        case internal::RasterKind::View:
            for (auto& frame : gRasterAdvancedFrames)
            {
                bool uses = false;
                for (usize i = 0; i < frame.Count; ++i)
                {
                    uses |= frame.Views[i] == gRasterViews[slot] || frame.Views[i] == gRasterViewDepths[slot];
                }
                if (uses && frame.Object != VK_NULL_HANDLE)
                {
                    vkDestroyFramebuffer(gDevice, frame.Object, nullptr);
                    frame = {};
                }
            }
            vkDestroyImageView(gDevice, gRasterViewDepths[slot], nullptr);
            gRasterViewDepths[slot] = VK_NULL_HANDLE;
            vkDestroyImageView(gDevice, gRasterViews[slot], nullptr);
            gRasterViews[slot] = VK_NULL_HANDLE;
            break;
        case internal::RasterKind::Sampler:
            vkDestroySampler(gDevice, gRasterSamplers[slot], nullptr);
            gRasterSamplers[slot] = VK_NULL_HANDLE;
            break;
        case internal::RasterKind::Shader:
            vkDestroyShaderModule(gDevice, gRasterShaders[slot].Object, nullptr);
            gRasterShaders[slot] = {};
            break;
        case internal::RasterKind::Layout:
            vkDestroyDescriptorSetLayout(gDevice, gRasterLayouts[slot], nullptr);
            gRasterLayouts[slot] = VK_NULL_HANDLE;
            break;
        case internal::RasterKind::Set:
            if (gRasterSets[slot].Arena != internal::RASTER_INVALID)
            {
                RasterDescriptorArena& arena = gRasterDescriptorArenas[gRasterSets[slot].Arena];
                (void)vkFreeDescriptorSets(gDevice, arena.Pool, 1, &gRasterSets[slot].Object);
                --arena.LiveSets;
                if (arena.LiveSets == 0)
                {
                    vkDestroyDescriptorPool(gDevice, arena.Pool, nullptr);
                    arena = {};
                }
            }
            gRasterSets[slot] = {};
            break;
        case internal::RasterKind::Pipeline:
            vkDestroyPipeline(gDevice, gRasterPipelines[slot].Object, nullptr);
            vkDestroyPipelineLayout(gDevice, gRasterPipelines[slot].Layout, nullptr);
            gRasterPipelines[slot] = {};
            break;
        case internal::RasterKind::ComputePipeline:
            vkDestroyPipeline(gDevice, gComputePipelines[slot].Object, nullptr);
            vkDestroyPipelineLayout(gDevice, gComputePipelines[slot].Layout, nullptr);
            gComputePipelines[slot] = {};
            break;
        case internal::RasterKind::RayGeometry:
        case internal::RasterKind::RayScene:
        case internal::RasterKind::RayProgram:
        case internal::RasterKind::RayBatch:
            RayDestroy(kind, slot);
            break;
        case internal::RasterKind::Count:
            break;
    }
}
RasterStatus RasterDraw(const internal::RasterPacket& packet) noexcept
{
    const auto& pipeline = gRasterPipelines[packet.Pipeline];
    const auto command = gFrames[gFrame].Command;
    const auto extent = gRasterExtent.width == 0 ? gExtent : gRasterExtent;
    internal::RasterArea area;
    if (internal::RasterResolveArea(gRasterRegion, extent.width, extent.height, area) == false)
    {
        return RasterStatus::InvalidDescription;
    }
    if (area.Empty)
    {
        return RasterStatus::Ready;
    }
    const auto& v = area.Viewport;
    const auto& c = area.Scissor;
    // Thanks to Khronos, "VkViewport", Vulkan specification:
    // https://docs.vulkan.org/refpages/latest/refpages/source/VkViewport.html
    // Negative height and a lower-edge Y preserve canonical Y-up/top-left pixels.
    const VkViewport viewport{static_cast<float32>(v.X),
                              static_cast<float32>(v.Y + v.Height),
                              static_cast<float32>(v.Width),
                              -static_cast<float32>(v.Height),
                              0,
                              1};
    const VkRect2D scissor{{static_cast<int32>(c.X), static_cast<int32>(c.Y)}, {c.Width, c.Height}};
    vkCmdSetViewport(command, 0, 1, &viewport);
    vkCmdSetScissor(command, 0, 1, &scissor);
    vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline.Object);
    vkCmdBindDescriptorSets(command,
                            VK_PIPELINE_BIND_POINT_GRAPHICS,
                            pipeline.Layout,
                            0,
                            1,
                            &gRasterSets[packet.Set].Object,
                            packet.DynamicOffsetCount,
                            packet.DynamicOffsets);
    for (usize i = 0; i < pipeline.Info.StreamCount; ++i)
    {
        const VkDeviceSize offset = packet.Offsets[i];
        vkCmdBindVertexBuffers(command, static_cast<uint32>(i), 1, &gRasterBuffers[packet.Vertices[i]].Object, &offset);
    }
    vkCmdBindIndexBuffer(command,
                         gRasterBuffers[packet.Indices].Object,
                         packet.IndexOffset,
                         packet.Index32 ? VK_INDEX_TYPE_UINT32 : VK_INDEX_TYPE_UINT16);
    if (packet.Indirect != internal::RASTER_INVALID)
    {
        vkCmdDrawIndexedIndirect(command,
                                 gRasterBuffers[packet.Indirect].Object,
                                 packet.IndirectOffset,
                                 1,
                                 sizeof(IndexedIndirectArguments));
    }
    else
    {
        vkCmdDrawIndexed(command, packet.IndexCount, packet.InstanceCount, 0, 0, 0);
    }
    return RasterStatus::Ready;
}
void RasterFrameExtent() noexcept
{
    gRasterExtent = {};
    gRasterRegion = {};
    gRasterActiveTexture = internal::RASTER_INVALID;
}
void RasterEndPass() noexcept
{
    if (gEncoding == false)
    {
        return;
    }
    vkCmdEndRenderPass(gFrames[gFrame].Command);
    gEncoding = false;
    if (gRasterNativePass.ColorCount != 0)
    {
        for (usize i = 0; i < gRasterNativePass.ColorCount; ++i)
        {
            for (const auto slot : {gRasterNativePass.Colors[i].Texture, gRasterNativePass.Colors[i].ResolveTexture})
            {
                if (slot != internal::RASTER_INVALID)
                {
                    auto& texture = gRasterTextures[slot];
                    texture.ColorLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
                    texture.ColorStage = texture.ColorWriterStage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
                    texture.ColorAccess = texture.ColorWriterAccess = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
                    texture.SampleVisibility = 0;
                }
            }
        }
    }
    if (gRasterActiveTexture != internal::RASTER_INVALID)
    {
        auto& texture = gRasterTextures[gRasterActiveTexture];
        texture.ColorLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        texture.ColorStage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        texture.ColorAccess = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        texture.ColorWriterStage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        texture.ColorWriterAccess = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        texture.SampleVisibility = 0;
        texture.DepthLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    }
}
void RasterTextureBarrier(usize slot, RasterTextureUse use) noexcept
{
    auto& texture = gRasterTextures[slot];
    const bool color = use == RasterTextureUse::ColorAttachment;
    if (texture.Depth != VK_NULL_HANDLE)
    {
        const auto depthLayout =
            color ? VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        const VkPipelineStageFlags tests =
            VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
        const VkPipelineStageFlags shaders =
            VK_PIPELINE_STAGE_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
        VkImageMemoryBarrier depth{};
        depth.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        depth.image = texture.Depth;
        depth.oldLayout = texture.DepthLayout;
        depth.newLayout = depthLayout;
        depth.srcQueueFamilyIndex = depth.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        depth.srcAccessMask =
            texture.DepthLayout == VK_IMAGE_LAYOUT_UNDEFINED ? 0U
            : texture.DepthLayout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL
                ? VK_ACCESS_SHADER_READ_BIT
                : VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
        depth.dstAccessMask =
            color ? VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT
                  : VK_ACCESS_SHADER_READ_BIT;
        depth.subresourceRange = {VK_IMAGE_ASPECT_DEPTH_BIT | (texture.Info.Stencil ? VK_IMAGE_ASPECT_STENCIL_BIT : 0U),
                                  0,
                                  texture.Info.MipLevels,
                                  0,
                                  texture.Info.DepthOrLayers};
        vkCmdPipelineBarrier(gFrames[gFrame].Command,
                             texture.DepthLayout == VK_IMAGE_LAYOUT_UNDEFINED ? VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT
                                                                              : tests | shaders,
                             color ? tests : shaders,
                             0,
                             0,
                             nullptr,
                             0,
                             nullptr,
                             1,
                             &depth);
        texture.DepthLayout = depthLayout;
    }
    const VkPipelineStageFlags stage =
        color                                    ? VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT
        : use == RasterTextureUse::SampledVertex ? VK_PIPELINE_STAGE_VERTEX_SHADER_BIT
        : use == RasterTextureUse::SampledFragment
            ? VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT
            : VK_PIPELINE_STAGE_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
    const auto access =
        color ? VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT : VK_ACCESS_SHADER_READ_BIT;
    const auto layout = color ? VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    if (texture.ColorLayout == layout && (texture.SampleVisibility & stage) == stage && color == false)
    {
        texture.ColorStage |= stage;
        return;
    }
    VkImageMemoryBarrier barrier{};
    barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    barrier.oldLayout = texture.ColorLayout;
    barrier.newLayout = layout;
    barrier.srcQueueFamilyIndex = barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.srcAccessMask = texture.ColorWriterAccess | texture.ColorAccess;
    barrier.dstAccessMask = access;
    barrier.image = texture.Object;
    barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT,
                                0,
                                texture.Info.MipLevels,
                                0,
                                texture.Info.Dimension == TextureDimension::D3 ? 1U : texture.Info.DepthOrLayers};
    vkCmdPipelineBarrier(gFrames[gFrame].Command,
                         texture.ColorStage | texture.ColorWriterStage,
                         stage,
                         0,
                         0,
                         nullptr,
                         0,
                         nullptr,
                         1,
                         &barrier);
    texture.ColorLayout = layout;
    texture.ColorStage = color ? stage : texture.ColorStage | stage;
    texture.ColorAccess = access;
    if (color == false)
    {
        texture.SampleVisibility |= stage;
    }
}
RasterStatus RasterPreparePass(const internal::RasterPassInfo& info) noexcept
{
    VkRenderPass pass = VK_NULL_HANDLE;
    const bool surface = info.Texture == internal::RASTER_INVALID;
    const auto format = surface ? gFormat : RasterPixelFormat(gRasterTextures[info.Texture].Info.Format);
    auto result = info.ColorCount == 0 ? RasterVkRenderPass(format, surface, info.Description, pass)
                                       : RasterVkAdvancedPass(info, pass);
    if (result == VK_SUCCESS && info.ColorCount != 0)
    {
        VkFramebuffer frame = VK_NULL_HANDLE;
        result = RasterVkAdvancedFramebuffer(info, pass, frame);
    }
    return result == VK_ERROR_TOO_MANY_OBJECTS ? RasterStatus::CapacityExceeded : RasterError(result);
}
RasterStatus RasterBeginPass(const internal::RasterPassInfo& info) noexcept
{
    const bool surface = info.Texture == internal::RASTER_INVALID;
    VkRenderPass pass = VK_NULL_HANDLE;
    const auto format = surface ? gFormat : RasterPixelFormat(gRasterTextures[info.Texture].Info.Format);
    const auto prepared = info.ColorCount == 0 ? RasterVkRenderPass(format, surface, info.Description, pass)
                                               : RasterVkAdvancedPass(info, pass);
    if (prepared != VK_SUCCESS)
    {
        return RasterError(prepared);
    }
    VkFramebuffer framebuffer = surface ? gTargets[gImage] : gRasterTextures[info.Texture].Framebuffer;
    gRasterExtent =
        surface ? gExtent
                : VkExtent2D{gRasterTextures[info.Texture].Info.Width, gRasterTextures[info.Texture].Info.Height};
    gRasterNativePass = info;
    if (info.ColorCount != 0)
    {
        const auto result = RasterVkAdvancedFramebuffer(info, pass, framebuffer);
        if (result != VK_SUCCESS)
        {
            return RasterError(result);
        }
        gRasterExtent = {info.Width, info.Height};
        for (usize i = 0; i < info.ColorCount; ++i)
        {
            RasterTextureBarrier(info.Colors[i].Texture, RasterTextureUse::ColorAttachment);
            if (info.Colors[i].ResolveTexture != internal::RASTER_INVALID)
            {
                RasterTextureBarrier(info.Colors[i].ResolveTexture, RasterTextureUse::ColorAttachment);
            }
        }
    }
    gRasterRegion = info.Description;
    gRasterActiveTexture = info.Texture;
    if (surface == false)
    {
        RasterTextureBarrier(info.Texture, RasterTextureUse::ColorAttachment);
        auto& texture = gRasterTextures[info.Texture];
        VkImageMemoryBarrier depth{};
        depth.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        depth.oldLayout = texture.DepthLayout;
        depth.newLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
        depth.srcQueueFamilyIndex = depth.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        depth.srcAccessMask =
            texture.DepthLayout == VK_IMAGE_LAYOUT_UNDEFINED ? 0U : VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
        depth.dstAccessMask =
            VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
        depth.image = texture.Depth;
        depth.subresourceRange = {VK_IMAGE_ASPECT_DEPTH_BIT | (texture.Info.Stencil ? VK_IMAGE_ASPECT_STENCIL_BIT : 0U),
                                  0,
                                  texture.Info.MipLevels,
                                  0,
                                  texture.Info.DepthOrLayers};
        const auto stages = VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
        vkCmdPipelineBarrier(gFrames[gFrame].Command,
                             texture.DepthLayout == VK_IMAGE_LAYOUT_UNDEFINED ? VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT
                                                                              : stages,
                             stages,
                             0,
                             0,
                             nullptr,
                             0,
                             nullptr,
                             1,
                             &depth);
    }
    VkClearValue clear[2]{};
    for (usize i = 0; i < 4; ++i)
    {
        clear[0].color.float32[i] = info.Description.Clear[i];
    }
    clear[1].depthStencil = {info.Description.ClearDepth, 0};
    VkClearValue advancedClear[9]{};
    for (usize i = 0; i < info.ColorCount; ++i)
    {
        for (usize c = 0; c < 4; ++c)
        {
            advancedClear[i].color.float32[c] = info.Colors[i].Clear[c];
        }
    }
    advancedClear[info.ColorCount].depthStencil = {info.Description.ClearDepth, info.Description.ClearStencil};
    VkRenderPassBeginInfo begin{};
    begin.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    begin.renderPass = pass;
    begin.framebuffer = framebuffer;
    begin.renderArea.extent = gRasterExtent;
    begin.clearValueCount = info.ColorCount == 0 ? 2 : static_cast<uint32>(info.ColorCount + 1);
    begin.pClearValues = info.ColorCount == 0 ? clear : advancedClear;
    vkCmdBeginRenderPass(gFrames[gFrame].Command, &begin, VK_SUBPASS_CONTENTS_INLINE);
    gEncoding = true;
    return RasterStatus::Ready;
}
RasterStatus RasterReserveSubmission() noexcept
{
    return RasterStatus::Ready;
}
void RasterSubmit(uint64 ordinal) noexcept
{
    gRasterOrdinals[(gFrame + FRAMES - 1) % FRAMES] = ordinal;
}
uint64 RasterCompleted() noexcept
{
    PollNativePreparation();
    if (hasPreparationDeviceLoss == true)
    {
        hasPreparationDeviceLoss = false;
        internal::Fail(gSession, StartupError::DeviceLost);
    }
    if (gDevice == VK_NULL_HANDLE)
    {
        return gRasterCompleted;
    }
    for (auto& texture : gRasterTextures)
    {
        if (texture.Fence == VK_NULL_HANDLE)
        {
            continue;
        }
        const auto result = vkGetFenceStatus(gDevice, texture.Fence);
        if (result != VK_SUCCESS && result != VK_NOT_READY)
        {
            internal::Fail(gSession,
                           result == VK_ERROR_DEVICE_LOST ? StartupError::DeviceLost
                                                          : StartupError::RenderingUnavailable);
            return gRasterCompleted;
        }
        if (result == VK_SUCCESS)
        {
            vkFreeCommandBuffers(gDevice, gCommands, 1, &texture.Command);
            vkDestroyFence(gDevice, texture.Fence, nullptr);
            vmaDestroyBuffer(gRasterAllocator, texture.Upload.Object, texture.Upload.Allocation);
            texture.Upload = {};
            texture.Command = VK_NULL_HANDLE;
            texture.Fence = VK_NULL_HANDLE;
            internal::RasterComplete(texture.Request, RasterStatus::Ready);
        }
    }
    for (usize i = 0; i < FRAMES; ++i)
    {
        if (gFrames[i].Fence == VK_NULL_HANDLE)
        {
            continue;
        }
        const auto result = vkGetFenceStatus(gDevice, gFrames[i].Fence);
        if (result != VK_SUCCESS && result != VK_NOT_READY)
        {
            internal::Fail(gSession,
                           result == VK_ERROR_DEVICE_LOST ? StartupError::DeviceLost
                                                          : StartupError::RenderingUnavailable);
            return gRasterCompleted;
        }
        if (result == VK_SUCCESS && gRasterOrdinals[i] > gRasterCompleted)
        {
            gRasterCompleted = gRasterOrdinals[i];
        }
    }
    return gRasterCompleted;
}
void WaitRasterSubmissions() noexcept
{
    for (const auto& texture : gRasterTextures)
    {
        // This fence is retained only after an accepted asynchronous upload.
        if (texture.Fence != VK_NULL_HANDLE)
        {
            (void)Check(vkWaitForFences(gDevice, 1, &texture.Fence, VK_TRUE, UINT64_MAX));
        }
    }
}
void RasterReset() noexcept
{
    DrainNativePreparation();
    hasPreparationDeviceLoss = false;
    gRasterHdrProbed = gRasterHdr = false;
    WaitIdle();
    for (auto& frame : gRasterAdvancedFrames)
    {
        if (gDevice != VK_NULL_HANDLE)
        {
            vkDestroyFramebuffer(gDevice, frame.Object, nullptr);
        }
        frame = {};
    }
    for (auto& ordinal : gRasterOrdinals)
    {
        ordinal = 0;
    }
    gRasterCompleted = 0;
}
void RasterShutdown() noexcept
{
    for (auto& pass : gRasterPasses)
    {
        if (gDevice != VK_NULL_HANDLE)
        {
            vkDestroyRenderPass(gDevice, pass.Object, nullptr);
        }
        pass = {};
    }
    if (gRasterAllocator != nullptr)
    {
        vmaDestroyAllocator(gRasterAllocator);
        gRasterAllocator = nullptr;
    }
}

RasterStatus RasterGrow(internal::RasterKind kind, usize count) noexcept
{
    switch (kind)
    {
        case internal::RasterKind::Buffer:
            return gRasterBuffers.EnsureSlots(count) ? RasterStatus::Ready : internal::StorageFailure();
        case internal::RasterKind::Texture:
            return gRasterTextures.EnsureSlots(count) ? RasterStatus::Ready : internal::StorageFailure();
        case internal::RasterKind::View:
            return gRasterViews.EnsureSlots(count) && gRasterViewDepths.EnsureSlots(count) ? RasterStatus::Ready
                                                                                           : internal::StorageFailure();
        case internal::RasterKind::Sampler:
            return gRasterSamplers.EnsureSlots(count) ? RasterStatus::Ready : internal::StorageFailure();
        case internal::RasterKind::Shader:
            return gRasterShaders.EnsureSlots(count) ? RasterStatus::Ready : internal::StorageFailure();
        case internal::RasterKind::Layout:
            return gRasterLayouts.EnsureSlots(count) ? RasterStatus::Ready : internal::StorageFailure();
        case internal::RasterKind::Set:
            return gRasterSets.EnsureSlots(count) ? RasterStatus::Ready : internal::StorageFailure();
        case internal::RasterKind::Pipeline:
            return gRasterPipelines.EnsureSlots(count) ? RasterStatus::Ready : internal::StorageFailure();
        case internal::RasterKind::ComputePipeline:
            return gComputePipelines.EnsureSlots(count) ? RasterStatus::Ready : internal::StorageFailure();
        default:
            return RasterStatus::InvalidDescription;
    }
}
RasterStatus RasterPrepareStorage() noexcept
{
    return (gRasterPasses.EnsureSlots(internal::CurrentStorageLimits().RasterPassStates) &&
            gRasterAdvancedFrames.EnsureSlots(internal::CurrentStorageLimits().RasterPassStates))
               ? RasterStatus::Ready
               : internal::StorageFailure();
}
} // namespace ludus::graphics::rhi::backend
