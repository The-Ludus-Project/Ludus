#include <ludus/foundation/base/types.h>
#include <ludus/foundation/logging/log_format.hpp>
#include <ludus/foundation/logging/log_system.hpp>

#include "shaders.h"
#include "uniform_layout.h"

#include <cmath>
#include <cstring>
#include <initializer_list>

#include <volk.h>

namespace
{
using namespace ludus::foundation;
using namespace ludus::foundation::logging;
using namespace ludus::shader_probe;

bool Check(VkResult result) noexcept
{
    if (result != VK_SUCCESS)
    {
        LUDUS_LOG_ERROR(LOG_TEMP, "Shader probe Vulkan error {}", static_cast<int32>(result));
        return false;
    }
    return true;
}

struct Probe final
{
    VkInstance Instance = VK_NULL_HANDLE;
    VkPhysicalDevice Physical = VK_NULL_HANDLE;
    VkDevice Device = VK_NULL_HANDLE;
    VkQueue Queue = VK_NULL_HANDLE;
    uint32 QueueFamily = 0;
    VkBuffer Uniform = VK_NULL_HANDLE;
    VkDeviceMemory UniformMemory = VK_NULL_HANDLE;
    VkBuffer Readback = VK_NULL_HANDLE;
    VkDeviceMemory ReadbackMemory = VK_NULL_HANDLE;
    VkImage Image = VK_NULL_HANDLE;
    VkDeviceMemory ImageMemory = VK_NULL_HANDLE;
    VkImageView View = VK_NULL_HANDLE;
    VkRenderPass Pass = VK_NULL_HANDLE;
    VkFramebuffer Framebuffer = VK_NULL_HANDLE;
    VkDescriptorSetLayout Bindings = VK_NULL_HANDLE;
    VkDescriptorPool Pool = VK_NULL_HANDLE;
    VkDescriptorSet Set = VK_NULL_HANDLE;
    VkPipelineLayout Layout = VK_NULL_HANDLE;
    VkShaderModule Vertex = VK_NULL_HANDLE;
    VkShaderModule Fragment = VK_NULL_HANDLE;
    VkPipeline Pipeline = VK_NULL_HANDLE;
    VkCommandPool Commands = VK_NULL_HANDLE;
    VkCommandBuffer Command = VK_NULL_HANDLE;
    VkFence Fence = VK_NULL_HANDLE;

    Probe() = default;
    Probe(const Probe&) = delete;
    Probe& operator=(const Probe&) = delete;
    ~Probe() noexcept
    {
        if (Device != VK_NULL_HANDLE)
        {
            (void)vkDeviceWaitIdle(Device);
            vkDestroyFence(Device, Fence, nullptr);
            vkDestroyCommandPool(Device, Commands, nullptr);
            vkDestroyPipeline(Device, Pipeline, nullptr);
            vkDestroyShaderModule(Device, Fragment, nullptr);
            vkDestroyShaderModule(Device, Vertex, nullptr);
            vkDestroyPipelineLayout(Device, Layout, nullptr);
            vkDestroyDescriptorPool(Device, Pool, nullptr);
            vkDestroyDescriptorSetLayout(Device, Bindings, nullptr);
            vkDestroyFramebuffer(Device, Framebuffer, nullptr);
            vkDestroyRenderPass(Device, Pass, nullptr);
            vkDestroyImageView(Device, View, nullptr);
            vkDestroyImage(Device, Image, nullptr);
            vkFreeMemory(Device, ImageMemory, nullptr);
            vkDestroyBuffer(Device, Readback, nullptr);
            vkFreeMemory(Device, ReadbackMemory, nullptr);
            vkDestroyBuffer(Device, Uniform, nullptr);
            vkFreeMemory(Device, UniformMemory, nullptr);
            vkDestroyDevice(Device, nullptr);
        }
        if (Instance != VK_NULL_HANDLE)
        {
            vkDestroyInstance(Instance, nullptr);
        }
    }

    bool Allocate(const VkMemoryRequirements& requirements,
                  VkMemoryPropertyFlags flags,
                  VkDeviceMemory& memory) const noexcept
    {
        VkPhysicalDeviceMemoryProperties properties{};
        vkGetPhysicalDeviceMemoryProperties(Physical, &properties);
        for (usize i = 0; i < properties.memoryTypeCount; ++i)
        {
            if ((requirements.memoryTypeBits & (1U << i)) != 0 &&
                (properties.memoryTypes[i].propertyFlags & flags) == flags)
            {
                VkMemoryAllocateInfo info{};
                info.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
                info.allocationSize = requirements.size;
                info.memoryTypeIndex = static_cast<uint32>(i);
                return Check(vkAllocateMemory(Device, &info, nullptr, &memory));
            }
        }
        LUDUS_LOG_ERROR(LOG_TEMP, "No suitable probe memory type");
        return false;
    }

    bool
    CreateBuffer(VkDeviceSize size, VkBuffer& buffer, VkBufferUsageFlags usage, VkDeviceMemory& memory) const noexcept
    {
        VkBufferCreateInfo info{};
        info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        info.size = size;
        info.usage = usage;
        if (Check(vkCreateBuffer(Device, &info, nullptr, &buffer)) == false)
        {
            return false;
        }
        VkMemoryRequirements requirements{};
        vkGetBufferMemoryRequirements(Device, buffer, &requirements);
        return Allocate(requirements,
                        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                        memory) &&
               Check(vkBindBufferMemory(Device, buffer, memory, 0));
    }

    bool Initialize(uint32 width, uint32 height, VkFormat format) noexcept
    {
        if (Check(volkInitialize()) == false)
        {
            return false;
        }
        VkApplicationInfo app{};
        app.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
        app.pApplicationName = "Ludus bounded shader probe";
        app.apiVersion = VK_API_VERSION_1_1;
        VkInstanceCreateInfo instance{};
        instance.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
        instance.pApplicationInfo = &app;
        if (Check(vkCreateInstance(&instance, nullptr, &Instance)) == false)
        {
            return false;
        }
        volkLoadInstance(Instance);
        uint32 count = 0;
        if (Check(vkEnumeratePhysicalDevices(Instance, &count, nullptr)) == false || count == 0 || count > 16)
        {
            return false;
        }
        VkPhysicalDevice physicals[16]{};
        if (Check(vkEnumeratePhysicalDevices(Instance, &count, physicals)) == false)
        {
            return false;
        }
        for (usize i = 0; i < count && Physical == VK_NULL_HANDLE; ++i)
        {
            uint32 queueCount = 0;
            vkGetPhysicalDeviceQueueFamilyProperties(physicals[i], &queueCount, nullptr);
            if (queueCount > 32)
            {
                continue;
            }
            VkQueueFamilyProperties queues[32]{};
            vkGetPhysicalDeviceQueueFamilyProperties(physicals[i], &queueCount, queues);
            for (usize j = 0; j < queueCount; ++j)
            {
                if ((queues[j].queueFlags & VK_QUEUE_GRAPHICS_BIT) != 0)
                {
                    Physical = physicals[i];
                    QueueFamily = static_cast<uint32>(j);
                    break;
                }
            }
        }
        if (Physical == VK_NULL_HANDLE)
        {
            return false;
        }
        VkPhysicalDeviceProperties properties{};
        vkGetPhysicalDeviceProperties(Physical, &properties);
        LUDUS_LOG_INFO(LOG_TEMP,
                       "Vulkan adapter: {} (type {})",
                       properties.deviceName,
                       static_cast<uint32>(properties.deviceType));
        const float32 priority = 1;
        VkDeviceQueueCreateInfo queue{};
        queue.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
        queue.queueFamilyIndex = QueueFamily;
        queue.queueCount = 1;
        queue.pQueuePriorities = &priority;
        VkDeviceCreateInfo device{};
        device.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
        device.queueCreateInfoCount = 1;
        device.pQueueCreateInfos = &queue;
        if (Check(vkCreateDevice(Physical, &device, nullptr, &Device)) == false)
        {
            return false;
        }
        volkLoadDevice(Device);
        vkGetDeviceQueue(Device, QueueFamily, 0, &Queue);
        if (CreateBuffer(sizeof(VulkanUniforms), Uniform, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, UniformMemory) == false ||
            CreateBuffer(static_cast<VkDeviceSize>(width) * height * 4,
                         Readback,
                         VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                         ReadbackMemory) == false)
        {
            return false;
        }
        VkImageCreateInfo image{};
        image.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        image.imageType = VK_IMAGE_TYPE_2D;
        image.format = format;
        image.extent = { .width = width, .height = height, .depth = 1 };
        image.mipLevels = 1;
        image.arrayLayers = 1;
        image.samples = VK_SAMPLE_COUNT_1_BIT;
        image.tiling = VK_IMAGE_TILING_OPTIMAL;
        image.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
        if (Check(vkCreateImage(Device, &image, nullptr, &Image)) == false)
        {
            return false;
        }
        VkMemoryRequirements requirements{};
        vkGetImageMemoryRequirements(Device, Image, &requirements);
        if (Allocate(requirements, 0, ImageMemory) == false ||
            Check(vkBindImageMemory(Device, Image, ImageMemory, 0)) == false)
        {
            return false;
        }
        VkImageViewCreateInfo view{};
        view.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        view.image = Image;
        view.viewType = VK_IMAGE_VIEW_TYPE_2D;
        view.format = format;
        view.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        view.subresourceRange.levelCount = 1;
        view.subresourceRange.layerCount = 1;
        if (Check(vkCreateImageView(Device, &view, nullptr, &View)) == false)
        {
            return false;
        }
        VkAttachmentDescription attachment{};
        attachment.format = format;
        attachment.samples = VK_SAMPLE_COUNT_1_BIT;
        attachment.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        attachment.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        attachment.finalLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        VkAttachmentReference reference{};
        reference.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        VkSubpassDescription subpass{};
        subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
        subpass.colorAttachmentCount = 1;
        subpass.pColorAttachments = &reference;
        VkSubpassDependency dependency{};
        dependency.srcSubpass = 0;
        dependency.dstSubpass = VK_SUBPASS_EXTERNAL;
        dependency.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        dependency.dstStageMask = VK_PIPELINE_STAGE_TRANSFER_BIT;
        dependency.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        dependency.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        VkRenderPassCreateInfo pass{};
        pass.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
        pass.attachmentCount = 1;
        pass.pAttachments = &attachment;
        pass.subpassCount = 1;
        pass.pSubpasses = &subpass;
        pass.dependencyCount = 1;
        pass.pDependencies = &dependency;
        if (Check(vkCreateRenderPass(Device, &pass, nullptr, &Pass)) == false)
        {
            return false;
        }
        VkFramebufferCreateInfo framebuffer{};
        framebuffer.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
        framebuffer.renderPass = Pass;
        framebuffer.attachmentCount = 1;
        framebuffer.pAttachments = &View;
        framebuffer.width = width;
        framebuffer.height = height;
        framebuffer.layers = 1;
        if (Check(vkCreateFramebuffer(Device, &framebuffer, nullptr, &Framebuffer)) == false)
        {
            return false;
        }
        VkDescriptorSetLayoutBinding binding{};
        binding.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        binding.descriptorCount = 1;
        binding.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
        VkDescriptorSetLayoutCreateInfo bindings{};
        bindings.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        bindings.bindingCount = 1;
        bindings.pBindings = &binding;
        if (Check(vkCreateDescriptorSetLayout(Device, &bindings, nullptr, &Bindings)) == false)
        {
            return false;
        }
        VkDescriptorPoolSize poolSize{};
        poolSize.type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        poolSize.descriptorCount = 1;
        VkDescriptorPoolCreateInfo pool{};
        pool.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        pool.maxSets = 1;
        pool.poolSizeCount = 1;
        pool.pPoolSizes = &poolSize;
        if (Check(vkCreateDescriptorPool(Device, &pool, nullptr, &Pool)) == false)
        {
            return false;
        }
        VkDescriptorSetAllocateInfo set{};
        set.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        set.descriptorPool = Pool;
        set.descriptorSetCount = 1;
        set.pSetLayouts = &Bindings;
        if (Check(vkAllocateDescriptorSets(Device, &set, &Set)) == false)
        {
            return false;
        }
        VkDescriptorBufferInfo buffer{};
        buffer.buffer = Uniform;
        buffer.range = sizeof(VulkanUniforms);
        VkWriteDescriptorSet write{};
        write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        write.dstSet = Set;
        write.descriptorCount = 1;
        write.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        write.pBufferInfo = &buffer;
        vkUpdateDescriptorSets(Device, 1, &write, 0, nullptr);
        VkPipelineLayoutCreateInfo layout{};
        layout.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        layout.setLayoutCount = 1;
        layout.pSetLayouts = &Bindings;
        if (Check(vkCreatePipelineLayout(Device, &layout, nullptr, &Layout)) == false)
        {
            return false;
        }
        VkShaderModuleCreateInfo shader{};
        shader.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
        shader.codeSize = sizeof(VERTEX_SPIRV);
        shader.pCode = VERTEX_SPIRV;
        if (Check(vkCreateShaderModule(Device, &shader, nullptr, &Vertex)) == false)
        {
            return false;
        }
        shader.codeSize = sizeof(FRAGMENT_SPIRV);
        shader.pCode = FRAGMENT_SPIRV;
        if (Check(vkCreateShaderModule(Device, &shader, nullptr, &Fragment)) == false)
        {
            return false;
        }
        VkPipelineShaderStageCreateInfo stages[2]{};
        stages[0].sType = stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
        stages[0].module = Vertex;
        stages[0].pName = "main";
        stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
        stages[1].module = Fragment;
        stages[1].pName = "main";
        VkPipelineVertexInputStateCreateInfo vertexInput{};
        vertexInput.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
        VkPipelineInputAssemblyStateCreateInfo assembly{};
        assembly.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
        assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
        VkViewport viewport{};
        viewport.width = static_cast<float32>(width);
        viewport.height = static_cast<float32>(height);
        viewport.maxDepth = 1;
        VkRect2D scissor{};
        scissor.extent = { .width = width, .height = height };
        VkPipelineViewportStateCreateInfo viewportState{};
        viewportState.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
        viewportState.viewportCount = 1;
        viewportState.pViewports = &viewport;
        viewportState.scissorCount = 1;
        viewportState.pScissors = &scissor;
        VkPipelineRasterizationStateCreateInfo raster{};
        raster.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
        raster.polygonMode = VK_POLYGON_MODE_FILL;
        raster.lineWidth = 1;
        VkPipelineMultisampleStateCreateInfo samples{};
        samples.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
        samples.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
        VkPipelineColorBlendAttachmentState blend{};
        blend.colorWriteMask = 15;
        VkPipelineColorBlendStateCreateInfo blending{};
        blending.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
        blending.attachmentCount = 1;
        blending.pAttachments = &blend;
        VkGraphicsPipelineCreateInfo pipeline{};
        pipeline.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
        pipeline.stageCount = 2;
        pipeline.pStages = stages;
        pipeline.pVertexInputState = &vertexInput;
        pipeline.pInputAssemblyState = &assembly;
        pipeline.pViewportState = &viewportState;
        pipeline.pRasterizationState = &raster;
        pipeline.pMultisampleState = &samples;
        pipeline.pColorBlendState = &blending;
        pipeline.layout = Layout;
        pipeline.renderPass = Pass;
        if (Check(vkCreateGraphicsPipelines(Device, VK_NULL_HANDLE, 1, &pipeline, nullptr, &Pipeline)) == false)
        {
            return false;
        }
        VkCommandPoolCreateInfo commands{};
        commands.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
        commands.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        commands.queueFamilyIndex = QueueFamily;
        if (Check(vkCreateCommandPool(Device, &commands, nullptr, &Commands)) == false)
        {
            return false;
        }
        VkCommandBufferAllocateInfo command{};
        command.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        command.commandPool = Commands;
        command.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        command.commandBufferCount = 1;
        if (Check(vkAllocateCommandBuffers(Device, &command, &Command)) == false)
        {
            return false;
        }
        VkFenceCreateInfo fence{};
        fence.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
        return Check(vkCreateFence(Device, &fence, nullptr, &Fence));
    }

    [[nodiscard]] bool Render(uint32 width, uint32 height, bool srgb, float32 elapsed) const noexcept
    {
        const VulkanUniforms uniforms =
        {
            .Resolution = {static_cast<float32>(width), static_cast<float32>(height)},
            .ElapsedTime = elapsed,
            .Padding0 = 0,
            .Direction = {0.2F, 0.4F, 0.6F},
            .Padding1 = 0,
            .Tint = {0.6F, 0.4F, 0.2F, 1.0F},
        };
        void* mapped = nullptr;
        if (Check(vkMapMemory(Device, UniformMemory, 0, sizeof(uniforms), 0, &mapped)) == false)
        {
            return false;
        }
        std::memcpy(mapped, &uniforms, sizeof(uniforms));
        vkUnmapMemory(Device, UniformMemory);
        if (Check(vkResetFences(Device, 1, &Fence)) == false || Check(vkResetCommandBuffer(Command, 0)) == false)
        {
            return false;
        }
        VkCommandBufferBeginInfo begin{};
        begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        if (Check(vkBeginCommandBuffer(Command, &begin)) == false)
        {
            return false;
        }
        VkRenderPassBeginInfo pass{};
        pass.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
        pass.renderPass = Pass;
        pass.framebuffer = Framebuffer;
        pass.renderArea.extent = { .width = width, .height = height };
        vkCmdBeginRenderPass(Command, &pass, VK_SUBPASS_CONTENTS_INLINE);
        vkCmdBindPipeline(Command, VK_PIPELINE_BIND_POINT_GRAPHICS, Pipeline);
        vkCmdBindDescriptorSets(Command, VK_PIPELINE_BIND_POINT_GRAPHICS, Layout, 0, 1, &Set, 0, nullptr);
        vkCmdDraw(Command, 3, 1, 0, 0);
        vkCmdEndRenderPass(Command);
        VkBufferImageCopy copy{};
        copy.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        copy.imageSubresource.layerCount = 1;
        copy.imageExtent = { .width = width, .height = height, .depth = 1 };
        vkCmdCopyImageToBuffer(Command, Image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, Readback, 1, &copy);
        VkMemoryBarrier barrier{};
        barrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
        barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        barrier.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
        vkCmdPipelineBarrier(Command,
                             VK_PIPELINE_STAGE_TRANSFER_BIT,
                             VK_PIPELINE_STAGE_HOST_BIT,
                             0,
                             1,
                             &barrier,
                             0,
                             nullptr,
                             0,
                             nullptr);
        if (Check(vkEndCommandBuffer(Command)) == false)
        {
            return false;
        }
        VkSubmitInfo submit{};
        submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        submit.commandBufferCount = 1;
        submit.pCommandBuffers = &Command;
        if (Check(vkQueueSubmit(Queue, 1, &submit, Fence)) == false ||
            Check(vkWaitForFences(Device, 1, &Fence, VK_TRUE, 5000000000ULL)) == false)
        {
            return false;
        }
        const usize size = static_cast<usize>(width) * height * 4;
        if (Check(vkMapMemory(Device, ReadbackMemory, 0, size, 0, &mapped)) == false)
        {
            return false;
        }
        const auto* pixels = static_cast<const uint8*>(mapped);
        uint32 errors = 0;
        for (usize y = 0; y < height; ++y)
        {
            for (usize x = 0; x < width; ++x)
            {
                const float64 cx = (static_cast<float64>(x) + 0.5 - width * 0.5) / height;
                const float64 cy = (static_cast<float64>(y) + 0.5 - height * 0.5) / height;
                const float64 circle = cx * cx + cy * cy < 0.04 ? 1 : 0;
                const float64 expected[4] = {
                    0.6 * (static_cast<float64>(x) + 0.5) / width + 0.01 * static_cast<float64>(elapsed),
                    0.4 * (static_cast<float64>(y) + 0.5) / height + 0.02 * static_cast<float64>(elapsed),
                    0.2 * circle + 0.03 * static_cast<float64>(elapsed),
                    1,
                };
                for (usize channel = 0; channel < 4; ++channel)
                {
                    float64 value = expected[channel];
                    if (srgb && channel < 3)
                    {
                        value = value <= 0.0031308 ? value * 12.92 : 1.055 * std::pow(value, 1.0 / 2.4) - 0.055;
                    }
                    const int32 byte = static_cast<int32>(std::round(value * 255));
                    const int32 actual = pixels[(static_cast<usize>(y) * width + x) * 4 + channel];
                    if (std::abs(actual - byte) > 2)
                    {
                        ++errors;
                    }
                }
            }
        }
        vkUnmapMemory(Device, ReadbackMemory);
        LUDUS_LOG_INFO(LOG_TEMP, "Pixels {}x{} srgb={} elapsed={} mismatches={}", width, height, srgb, elapsed, errors);
        return errors == 0;
    }
};
} // namespace

int main()
{
    LogConfig config{};
    config.EnableConsole = true;
    config.EnableFile = false;
    config.EnableDebugger = false;
    (void)LogSystem::Initialize(config);
    bool success = true;
    for (const bool srgb : {false, true})
    {
        for (const bool portrait : {false, true})
        {
            const uint32 width = portrait ? 64 : 96;
            const uint32 height = portrait ? 96 : 64;
            Probe probe;
            success = probe.Initialize(width, height, srgb ? VK_FORMAT_R8G8B8A8_SRGB : VK_FORMAT_R8G8B8A8_UNORM) &&
                      probe.Render(width, height, srgb, 0) && probe.Render(width, height, srgb, 2) && success;
        }
    }
    (void)LogSystem::Shutdown();
    return success ? 0 : 1;
}
