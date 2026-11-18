#pragma once
#include <ludus/foundation/time/time.hpp>
// Thanks to Khronos, Vulkan Specification, Queries, timestamp queries and availability:
// https://docs.vulkan.org/spec/latest/chapters/queries.html
// TOP/BOTTOM delimit one graphics queue command buffer; no CPU clock correlation is claimed.
namespace ludus::graphics::rhi::backend
{
namespace
{
GpuTiming gTiming;
VkQueryPool gTimingPool = VK_NULL_HANDLE;
uint64 gTimingCpuStart = 0;
usize gTimingFrame = 0;
} // namespace
// Thanks to AMD GPUOpen, Vulkan Memory Allocator 3.3.0, Statistics:
// https://gpuopen-librariesandsdks.github.io/VulkanMemoryAllocator/html/statistics.html
// blockBytes describes API device-memory blocks; allocationBytes describes binding spans.
bool AllocatorBlockBytes(uint64& bytes) noexcept
{
    bytes = 0;
    if (gRasterAllocator != nullptr)
    {
        VmaTotalStatistics statistics{};
        vmaCalculateStatistics(gRasterAllocator, &statistics);
        bytes = statistics.total.statistics.blockBytes;
    }
    return true;
}
MeasurementAdapter TimingAdapter() noexcept
{
    VkPhysicalDeviceProperties properties{};
    vkGetPhysicalDeviceProperties(gPhysical, &properties);
    MeasurementAdapter output;
    internal::CopyMeasurementName(properties.deviceName, sizeof(properties.deviceName), output.Name);
    output.VendorId = properties.vendorID;
    output.DeviceId = properties.deviceID;
    output.DriverVersion = properties.driverVersion;
    output.Software = properties.deviceType == VK_PHYSICAL_DEVICE_TYPE_CPU;
    output.SoftwareKnown = output.Software || properties.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU ||
                           properties.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU;
    return output;
}
void TimingReset() noexcept
{
    if (gTimingPool != VK_NULL_HANDLE && gDevice != VK_NULL_HANDLE)
    {
        // Device teardown already drains rendering before releasing raster records.
        vkDestroyQueryPool(gDevice, gTimingPool, nullptr);
    }
    gTimingPool = VK_NULL_HANDLE;
    gTiming = {};
}
GpuTiming TimingPoll() noexcept
{
    if (gTiming.State == GpuTimingState::Pending)
    {
        // Query reset executes on the GPU. Old values can remain available until then;
        // the captured submission fence must complete before reading the reused pool.
        const auto fence = vkGetFenceStatus(gDevice, gFrames[gTimingFrame].Fence);
        if (fence == VK_NOT_READY)
        {
            return gTiming;
        }
        if (fence != VK_SUCCESS)
        {
            gTiming.State = GpuTimingState::Failed;
            if (fence == VK_ERROR_DEVICE_LOST)
            {
                internal::Fail(gSession, StartupError::DeviceLost);
            }
            return gTiming;
        }
        uint64 values[4]{};
        const auto result = vkGetQueryPoolResults(gDevice,
                                                  gTimingPool,
                                                  0,
                                                  2,
                                                  sizeof(values),
                                                  values,
                                                  sizeof(uint64) * 2,
                                                  VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WITH_AVAILABILITY_BIT);
        if (result == VK_SUCCESS && values[1] != 0 && values[3] != 0)
        {
            const auto now = time::NowTicks();
            gTiming.State =
                now >= gTimingCpuStart && internal::TimestampDuration(values[0],
                                                                      values[2],
                                                                      {gTiming.ValidBits, gTiming.TickNanoseconds},
                                                                      now - gTimingCpuStart,
                                                                      gTiming.DurationNanoseconds)
                    ? GpuTimingState::Available
                    : GpuTimingState::Disjoint;
        }
        else if (result != VK_NOT_READY && result != VK_SUCCESS)
        {
            gTiming.State = GpuTimingState::Failed;
            if (result == VK_ERROR_DEVICE_LOST)
            {
                internal::Fail(gSession, StartupError::DeviceLost);
            }
        }
    }
    return gTiming;
}
RasterStatus TimingArm() noexcept
{
    const auto state = TimingPoll().State;
    if (gDevice == VK_NULL_HANDLE)
    {
        return internal::GetStartup().State == StartupState::DeviceLost ? RasterStatus::DeviceLost
                                                                        : RasterStatus::Failed;
    }
    if (state == GpuTimingState::Armed || state == GpuTimingState::Pending)
    {
        return RasterStatus::InvalidState;
    }
    VkQueueFamilyProperties families[32]{};
    uint32 count = 32;
    vkGetPhysicalDeviceQueueFamilyProperties(gPhysical, &count, families);
    VkPhysicalDeviceProperties properties{};
    vkGetPhysicalDeviceProperties(gPhysical, &properties);
    if (gFamily >= count || families[gFamily].timestampValidBits == 0 || properties.limits.timestampPeriod <= 0)
    {
        gTiming = { .State = GpuTimingState::Unavailable };
        return RasterStatus::Unsupported;
    }
    if (gTimingPool == VK_NULL_HANDLE)
    {
        VkQueryPoolCreateInfo info{};
        info.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO;
        info.queryType = VK_QUERY_TYPE_TIMESTAMP;
        info.queryCount = 2;
        const auto result = vkCreateQueryPool(gDevice, &info, nullptr, &gTimingPool);
        if (result != VK_SUCCESS)
        {
            gTimingPool = VK_NULL_HANDLE;
            if (result == VK_ERROR_DEVICE_LOST)
            {
                internal::Fail(gSession, StartupError::DeviceLost);
                return RasterStatus::DeviceLost;
            }
            return result == VK_ERROR_OUT_OF_HOST_MEMORY || result == VK_ERROR_OUT_OF_DEVICE_MEMORY
                       ? RasterStatus::OutOfMemory
                       : RasterStatus::Failed;
        }
    }
    gTiming =
    {
        .State = GpuTimingState::Armed,
        .TickNanoseconds = static_cast<float64>(properties.limits.timestampPeriod),
        .ValidBits = families[gFamily].timestampValidBits,
    };
    return RasterStatus::Ready;
}
void TimingBegin() noexcept
{
    if (gTiming.State == GpuTimingState::Armed)
    {
        gTimingCpuStart = time::NowTicks();
        vkCmdResetQueryPool(gFrames[gFrame].Command, gTimingPool, 0, 2);
        vkCmdWriteTimestamp(gFrames[gFrame].Command, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, gTimingPool, 0);
    }
}
void TimingEnd() noexcept
{
    if (gTiming.State == GpuTimingState::Armed)
    {
        vkCmdWriteTimestamp(gFrames[gFrame].Command, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, gTimingPool, 1);
        gTimingFrame = gFrame;
    }
}
void TimingSubmitted() noexcept
{
    if (gTiming.State == GpuTimingState::Armed)
    {
        gTiming.State = GpuTimingState::Pending;
    }
}
bool RasterAllocationBytes(internal::RasterKind kind, usize slot, uint64& bytes) noexcept
{
    bytes = 0;
    const auto add = [&bytes](VmaAllocation allocation) noexcept {
        if (allocation != nullptr)
        {
            VmaAllocationInfo info{};
            vmaGetAllocationInfo(gRasterAllocator, allocation, &info);
            bytes += info.size;
        }
    };
    if (kind == internal::RasterKind::Buffer && slot < gRasterBuffers.GetSize())
    {
        add(gRasterBuffers[slot].Allocation);
    }
    else if (kind == internal::RasterKind::Texture && slot < gRasterTextures.GetSize())
    {
        const auto& texture = gRasterTextures[slot];
        add(texture.Allocation);
        add(texture.DepthAllocation);
        add(texture.Upload.Allocation);
    }
    return true;
}
} // namespace ludus::graphics::rhi::backend
