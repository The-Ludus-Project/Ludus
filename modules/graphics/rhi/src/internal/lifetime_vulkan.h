#pragma once
// Thanks to Khronos, "Synchronization Examples", transfer-to-host and
// transfer-to-vertex/uniform dependencies:
// https://docs.vulkan.org/guide/latest/synchronization_examples.html
// Thanks to AMD GPUOpen, "Vulkan Memory Allocator 3.3.0", Memory mapping,
// https://gpuopen-librariesandsdks.github.io/VulkanMemoryAllocator/html/memory_mapping.html
// for atom-aligned flush/invalidate. Fixed slices retain their command/fence
// and mapping until completion; no vkWaitForFences on the transfer request path.
namespace ludus::graphics::rhi::backend
{
namespace
{
struct LifetimeVulkanTransfer final
{
    RasterVulkanBuffer Staging;
    void* Mapped = nullptr;
    VkCommandBuffer Command = VK_NULL_HANDLE;
    VkFence Fence = VK_NULL_HANDLE;
    bool Busy = false;
};
LifetimeVulkanTransfer gLifetimeVulkan[2][internal::LIFETIME_TRANSFERS];
VkResult LifetimeVulkanBegin(LifetimeVulkanTransfer& transfer, bool readback) noexcept
{
    auto result = RasterAllocator();
    if (result != VK_SUCCESS)
    {
        return result;
    }
    if (transfer.Staging.Object == VK_NULL_HANDLE)
    {
        VkBufferCreateInfo buffer{};
        buffer.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        buffer.size = internal::CurrentStorageLimits().TransferBytes;
        buffer.usage = readback ? VK_BUFFER_USAGE_TRANSFER_DST_BIT : VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
        VmaAllocationCreateInfo allocation{};
        allocation.usage = VMA_MEMORY_USAGE_AUTO;
        allocation.flags =
            VMA_ALLOCATION_CREATE_MAPPED_BIT | (readback ? VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT
                                                         : VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT);
        VmaAllocationInfo mapped{};
        result = vmaCreateBuffer(gRasterAllocator,
                                 &buffer,
                                 &allocation,
                                 &transfer.Staging.Object,
                                 &transfer.Staging.Allocation,
                                 &mapped);
        if (result != VK_SUCCESS)
        {
            return result;
        }
        transfer.Mapped = mapped.pMappedData;
    }
    if (transfer.Mapped == nullptr)
    {
        return VK_ERROR_MEMORY_MAP_FAILED;
    }
    if (transfer.Command == VK_NULL_HANDLE)
    {
        VkCommandBufferAllocateInfo allocate{};
        allocate.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        allocate.commandPool = gCommands;
        allocate.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        allocate.commandBufferCount = 1;
        result = vkAllocateCommandBuffers(gDevice, &allocate, &transfer.Command);
        if (result != VK_SUCCESS)
        {
            return result;
        }
    }
    if (transfer.Fence == VK_NULL_HANDLE)
    {
        VkFenceCreateInfo fence{};
        fence.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
        result = vkCreateFence(gDevice, &fence, nullptr, &transfer.Fence);
        if (result != VK_SUCCESS)
        {
            return result;
        }
    }
    result = vkResetFences(gDevice, 1, &transfer.Fence);
    if (result == VK_SUCCESS)
    {
        result = vkResetCommandBuffer(transfer.Command, 0);
    }
    if (result != VK_SUCCESS)
    {
        return result;
    }
    VkCommandBufferBeginInfo begin{};
    begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    return vkBeginCommandBuffer(transfer.Command, &begin);
}
VkResult LifetimeVulkanSubmit(LifetimeVulkanTransfer& transfer) noexcept
{
    auto result = vkEndCommandBuffer(transfer.Command);
    if (result != VK_SUCCESS)
    {
        return result;
    }
    VkSubmitInfo submit{};
    submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &transfer.Command;
    result = vkQueueSubmit(gQueue, 1, &submit, transfer.Fence);
    transfer.Busy = result == VK_SUCCESS;
    return result;
}
VkBufferMemoryBarrier LifetimeVulkanBarrier(VkBuffer buffer) noexcept
{
    VkBufferMemoryBarrier barrier{};
    barrier.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
    barrier.srcQueueFamilyIndex = barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.buffer = buffer;
    barrier.offset = 0;
    barrier.size = VK_WHOLE_SIZE;
    return barrier;
}
} // namespace
bool LifetimeTransferAvailable(bool readback, usize slot) noexcept
{
    return gLifetimeVulkan[readback ? 1 : 0][slot].Busy == false;
}
RasterStatus LifetimeUpload(usize transferSlot,
                            const BufferDescription& info,
                            const internal::LifetimeUploadTarget& uploadTarget,
                            const uint8* bytes,
                            usize size) noexcept
{
    const auto bufferSlot = uploadTarget.Buffer;

    auto& transfer = gLifetimeVulkan[0][transferSlot];
    if (transfer.Busy)
    {
        return RasterStatus::CapacityExceeded;
    }
    auto result = LifetimeVulkanBegin(transfer, false);
    if (result != VK_SUCCESS)
    {
        return RasterError(result);
    }
    VkBufferCreateInfo buffer{};
    buffer.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    buffer.size = size;
    buffer.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT |
                   (info.Role == BufferRole::Uniform  ? VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT
                    : info.Role == BufferRole::Vertex ? VK_BUFFER_USAGE_VERTEX_BUFFER_BIT
                                                      : VK_BUFFER_USAGE_INDEX_BUFFER_BIT);
    VmaAllocationCreateInfo allocation{};
    allocation.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
    auto& destination = gRasterBuffers[bufferSlot];
    result =
        vmaCreateBuffer(gRasterAllocator, &buffer, &allocation, &destination.Object, &destination.Allocation, nullptr);
    if (result != VK_SUCCESS)
    {
        return RasterError(result);
    }
    std::memcpy(transfer.Mapped, bytes, size);
    result = vmaFlushAllocation(gRasterAllocator, transfer.Staging.Allocation, 0, size);
    if (result != VK_SUCCESS)
    {
        return RasterError(result);
    }
    VkBufferCopy copy{0, 0, size};
    vkCmdCopyBuffer(transfer.Command, transfer.Staging.Object, destination.Object, 1, &copy);
    auto barrier = LifetimeVulkanBarrier(destination.Object);
    barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_VERTEX_ATTRIBUTE_READ_BIT | VK_ACCESS_INDEX_READ_BIT |
                            VK_ACCESS_UNIFORM_READ_BIT | VK_ACCESS_TRANSFER_READ_BIT;
    vkCmdPipelineBarrier(transfer.Command,
                         VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_VERTEX_INPUT_BIT | VK_PIPELINE_STAGE_VERTEX_SHADER_BIT |
                             VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT,
                         0,
                         0,
                         nullptr,
                         1,
                         &barrier,
                         0,
                         nullptr);
    return RasterError(LifetimeVulkanSubmit(transfer));
}
RasterStatus LifetimeReadback(usize transferSlot, BufferRole, const internal::LifetimeCopyRange& range) noexcept
{
    const auto bufferSlot = range.Buffer;
    const auto offset = range.Offset;
    const auto size = range.Size;
    auto& transfer = gLifetimeVulkan[1][transferSlot];
    if (transfer.Busy)
    {
        return RasterStatus::CapacityExceeded;
    }
    const auto result = LifetimeVulkanBegin(transfer, true);
    if (result != VK_SUCCESS)
    {
        return RasterError(result);
    }
    auto source = LifetimeVulkanBarrier(gRasterBuffers[bufferSlot].Object);
    source.srcAccessMask =
        VK_ACCESS_HOST_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT | gRasterBuffers[bufferSlot].WriterAccess;
    source.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    vkCmdPipelineBarrier(transfer.Command,
                         VK_PIPELINE_STAGE_HOST_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT |
                             gRasterBuffers[bufferSlot].WriterStage,
                         VK_PIPELINE_STAGE_TRANSFER_BIT,
                         0,
                         0,
                         nullptr,
                         1,
                         &source,
                         0,
                         nullptr);
    VkBufferCopy copy{offset, 0, size};
    vkCmdCopyBuffer(transfer.Command, source.buffer, transfer.Staging.Object, 1, &copy);
    auto host = LifetimeVulkanBarrier(transfer.Staging.Object);
    host.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    host.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
    vkCmdPipelineBarrier(transfer.Command,
                         VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_HOST_BIT,
                         0,
                         0,
                         nullptr,
                         1,
                         &host,
                         0,
                         nullptr);
    const auto submitted = LifetimeVulkanSubmit(transfer);
    if (submitted == VK_SUCCESS)
    {
        gRasterBuffers[bufferSlot].Stage |= VK_PIPELINE_STAGE_TRANSFER_BIT;
        gRasterBuffers[bufferSlot].Access |= VK_ACCESS_TRANSFER_READ_BIT;
    }
    return RasterError(submitted);
}
// Thanks to Khronos, "Synchronization Examples", transfer/host and image
// dependencies (https://docs.vulkan.org/guide/latest/synchronization_examples.html).
// Transfers keep conservative whole-resource barriers and one ordered queue.
RasterStatus
LifetimeBufferUpdate(usize slot, BufferRole, const internal::LifetimeCopyRange& range, const uint8* bytes) noexcept
{
    auto& transfer = gLifetimeVulkan[0][slot];
    if (transfer.Busy)
    {
        return RasterStatus::CapacityExceeded;
    }
    auto result = LifetimeVulkanBegin(transfer, false);
    if (result != VK_SUCCESS)
    {
        return RasterError(result);
    }
    std::memcpy(transfer.Mapped, bytes, range.Size);
    result = vmaFlushAllocation(gRasterAllocator, transfer.Staging.Allocation, 0, range.Size);
    if (result != VK_SUCCESS)
    {
        return RasterError(result);
    }
    auto& destination = gRasterBuffers[range.Buffer];
    auto barrier = LifetimeVulkanBarrier(destination.Object);
    barrier.srcAccessMask = destination.Access | destination.WriterAccess;
    barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    vkCmdPipelineBarrier(transfer.Command,
                         destination.Stage | destination.WriterStage,
                         VK_PIPELINE_STAGE_TRANSFER_BIT,
                         0,
                         0,
                         nullptr,
                         1,
                         &barrier,
                         0,
                         nullptr);
    const VkBufferCopy copy{0, range.Offset, range.Size};
    vkCmdCopyBuffer(transfer.Command, transfer.Staging.Object, destination.Object, 1, &copy);
    barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
    vkCmdPipelineBarrier(transfer.Command,
                         VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                         0,
                         0,
                         nullptr,
                         1,
                         &barrier,
                         0,
                         nullptr);
    result = LifetimeVulkanSubmit(transfer);
    if (result == VK_SUCCESS)
    {
        destination.Stage = destination.WriterStage = VK_PIPELINE_STAGE_TRANSFER_BIT;
        destination.Access = destination.WriterAccess = VK_ACCESS_TRANSFER_WRITE_BIT;
    }
    return RasterError(result);
}
namespace
{
struct LifetimeVkImageTransition final
{
    VkImageLayout Before;
    VkImageLayout After;
    VkAccessFlags SourceAccess;
    VkAccessFlags DestinationAccess;
};
void LifetimeVkImageBarrier(VkCommandBuffer command,
                            const RasterVulkanTexture& texture,
                            const LifetimeVkImageTransition& transition) noexcept
{
    VkImageMemoryBarrier barrier{};
    barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    barrier.srcAccessMask = transition.Before == VK_IMAGE_LAYOUT_UNDEFINED ? 0U : transition.SourceAccess;
    barrier.dstAccessMask = transition.DestinationAccess;
    barrier.oldLayout = transition.Before;
    barrier.newLayout = transition.After;
    barrier.srcQueueFamilyIndex = barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = texture.Object;
    barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, texture.Info.MipLevels, 0, 1};
    vkCmdPipelineBarrier(command,
                         VK_PIPELINE_STAGE_ALL_COMMANDS_BIT | VK_PIPELINE_STAGE_HOST_BIT,
                         VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                         0,
                         0,
                         nullptr,
                         0,
                         nullptr,
                         1,
                         &barrier);
}
VkImageSubresourceLayers LifetimeVkLayers(const TextureRegion& region) noexcept
{
    return {VK_IMAGE_ASPECT_COLOR_BIT, region.Mip, 0, 1};
}
VkOffset3D LifetimeVkOffset(const TextureRegion& region) noexcept
{
    return {static_cast<int32>(region.X), static_cast<int32>(region.Y), 0};
}
} // namespace
RasterStatus LifetimeTextureTransfer(usize slot, const internal::TextureTransfer& info, const uint8* bytes) noexcept
{
    const bool readback = info.Operation == internal::TextureTransferOperation::Readback;
    const bool update = info.Operation == internal::TextureTransferOperation::Update;
    auto& transfer = gLifetimeVulkan[readback ? 1 : 0][slot];
    if (transfer.Busy)
    {
        return RasterStatus::CapacityExceeded;
    }
    auto result = LifetimeVulkanBegin(transfer, readback);
    if (result != VK_SUCCESS)
    {
        return RasterError(result);
    }
    if (update)
    {
        std::memcpy(transfer.Mapped, bytes, info.Size);
        result = vmaFlushAllocation(gRasterAllocator, transfer.Staging.Allocation, 0, info.Size);
        if (result != VK_SUCCESS)
        {
            return RasterError(result);
        }
    }
    constexpr VkAccessFlags memory = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
    if (update == false)
    {
        const auto& source = gRasterTextures[info.Source];
        LifetimeVkImageBarrier(transfer.Command,
                               source,
                               {
                                   .Before = source.ColorLayout,
                                   .After = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                                   .SourceAccess = source.ColorAccess | source.ColorWriterAccess,
                                   .DestinationAccess = VK_ACCESS_TRANSFER_READ_BIT,
                               });
    }
    if (readback == false)
    {
        const auto& destination = gRasterTextures[info.Destination];
        LifetimeVkImageBarrier(transfer.Command,
                               destination,
                               {
                                   .Before = destination.ColorLayout,
                                   .After = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                   .SourceAccess = destination.ColorAccess | destination.ColorWriterAccess,
                                   .DestinationAccess = VK_ACCESS_TRANSFER_WRITE_BIT,
                               });
    }
    const auto& region = readback ? info.SourceRegion : info.DestinationRegion;
    VkBufferImageCopy buffer{};
    buffer.bufferRowLength = static_cast<uint32>(info.RowPitch / 4);
    buffer.bufferImageHeight = region.Height;
    buffer.imageSubresource = LifetimeVkLayers(region);
    buffer.imageOffset = LifetimeVkOffset(region);
    buffer.imageExtent = {region.Width, region.Height, 1};
    if (update)
    {
        vkCmdCopyBufferToImage(transfer.Command,
                               transfer.Staging.Object,
                               gRasterTextures[info.Destination].Object,
                               VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                               1,
                               &buffer);
    }
    else if (readback)
    {
        vkCmdCopyImageToBuffer(transfer.Command,
                               gRasterTextures[info.Source].Object,
                               VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                               transfer.Staging.Object,
                               1,
                               &buffer);
        auto host = LifetimeVulkanBarrier(transfer.Staging.Object);
        host.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        host.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
        vkCmdPipelineBarrier(transfer.Command,
                             VK_PIPELINE_STAGE_TRANSFER_BIT,
                             VK_PIPELINE_STAGE_HOST_BIT,
                             0,
                             0,
                             nullptr,
                             1,
                             &host,
                             0,
                             nullptr);
    }
    else
    {
        VkImageCopy copy{};
        copy.srcSubresource = LifetimeVkLayers(info.SourceRegion);
        copy.srcOffset = LifetimeVkOffset(info.SourceRegion);
        copy.dstSubresource = LifetimeVkLayers(info.DestinationRegion);
        copy.dstOffset = LifetimeVkOffset(info.DestinationRegion);
        copy.extent = {region.Width, region.Height, 1};
        vkCmdCopyImage(transfer.Command,
                       gRasterTextures[info.Source].Object,
                       VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                       gRasterTextures[info.Destination].Object,
                       VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                       1,
                       &copy);
    }
    if (update == false)
    {
        const auto& source = gRasterTextures[info.Source];
        LifetimeVkImageBarrier(transfer.Command,
                               source,
                               {
                                   .Before = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                                   .After = source.ColorLayout,
                                   .SourceAccess = VK_ACCESS_TRANSFER_READ_BIT,
                                   .DestinationAccess = memory,
                               });
    }
    if (readback == false)
    {
        const auto& destination = gRasterTextures[info.Destination];
        LifetimeVkImageBarrier(transfer.Command,
                               destination,
                               {
                                   .Before = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                   .After = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                                   .SourceAccess = VK_ACCESS_TRANSFER_WRITE_BIT,
                                   .DestinationAccess = VK_ACCESS_SHADER_READ_BIT,
                               });
    }
    result = LifetimeVulkanSubmit(transfer);
    if (result == VK_SUCCESS)
    {
        if (update == false)
        {
            gRasterTextures[info.Source].ColorStage |= VK_PIPELINE_STAGE_TRANSFER_BIT;
            gRasterTextures[info.Source].ColorAccess |= VK_ACCESS_TRANSFER_READ_BIT;
        }
        if (readback == false)
        {
            auto& destination = gRasterTextures[info.Destination];
            destination.ColorLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            destination.ColorStage = VK_PIPELINE_STAGE_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
            destination.ColorAccess = VK_ACCESS_SHADER_READ_BIT;
            destination.ColorWriterStage = VK_PIPELINE_STAGE_TRANSFER_BIT;
            destination.ColorWriterAccess = VK_ACCESS_TRANSFER_WRITE_BIT;
            destination.SampleVisibility = destination.ColorStage;
        }
    }
    return RasterError(result);
}
RasterStatus LifetimePollTransfer(bool readback, usize slot) noexcept
{
    auto& transfer = gLifetimeVulkan[readback ? 1 : 0][slot];
    if (transfer.Busy == false || gDevice == VK_NULL_HANDLE)
    {
        return RasterStatus::Failed;
    }
    const auto result = vkGetFenceStatus(gDevice, transfer.Fence);
    if (result == VK_NOT_READY)
    {
        return RasterStatus::Pending;
    }
    if (result == VK_SUCCESS)
    {
        return RasterStatus::Ready;
    }
    // An error is not proof of safe reuse. Close the session and let teardown
    // establish device-idle/lost ownership before releasing any ring allocation.
    internal::Fail(gSession,
                   result == VK_ERROR_DEVICE_LOST ? StartupError::DeviceLost : StartupError::RenderingUnavailable);
    return result == VK_ERROR_DEVICE_LOST ? RasterStatus::DeviceLost : RasterStatus::Failed;
}
RasterStatus LifetimeCopyReadback(usize slot, uint8* output, usize size) noexcept
{
    const auto ready = LifetimePollTransfer(true, slot);
    if (ready != RasterStatus::Ready)
    {
        return ready;
    }
    auto& transfer = gLifetimeVulkan[1][slot];
    const auto result = vmaInvalidateAllocation(gRasterAllocator, transfer.Staging.Allocation, 0, size);
    if (result != VK_SUCCESS)
    {
        return RasterError(result);
    }
    std::memcpy(output, transfer.Mapped, size);
    return RasterStatus::Ready;
}
void LifetimeReleaseTransfer(bool readback, usize slot) noexcept
{
    gLifetimeVulkan[readback ? 1 : 0][slot].Busy = false;
}
void WaitLifetimeSubmissions() noexcept
{
    for (const auto& direction : gLifetimeVulkan)
    {
        for (const auto& transfer : direction)
        {
            if (transfer.Busy)
            {
                (void)Check(vkWaitForFences(gDevice, 1, &transfer.Fence, VK_TRUE, UINT64_MAX));
            }
        }
    }
}
void LifetimeReset() noexcept
{
    // RasterReset has already drained accepted RHI frame/upload/transfer fences
    // (or an owned device idle/lost), without waiting unrelated foreign work.
    for (auto& direction : gLifetimeVulkan)
    {
        for (auto& transfer : direction)
        {
            if (gDevice != VK_NULL_HANDLE)
            {
                if (transfer.Command != VK_NULL_HANDLE)
                {
                    vkFreeCommandBuffers(gDevice, gCommands, 1, &transfer.Command);
                }
                vkDestroyFence(gDevice, transfer.Fence, nullptr);
                if (transfer.Staging.Object != VK_NULL_HANDLE)
                {
                    vmaDestroyBuffer(gRasterAllocator, transfer.Staging.Object, transfer.Staging.Allocation);
                }
            }
            transfer = {};
        }
    }
}
void RasterDiscardSubmission() noexcept {}
} // namespace ludus::graphics::rhi::backend
