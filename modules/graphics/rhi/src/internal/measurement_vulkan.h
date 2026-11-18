#pragma once
#include <ludus/foundation/time/time.hpp>
// Thanks to Khronos, Vulkan Specification, Queries, timestamp queries and availability:
// https://docs.vulkan.org/spec/latest/chapters/queries.html
// TOP/BOTTOM delimit one graphics queue command buffer; no CPU clock correlation is claimed.
namespace ludus::graphics::rhi::backend
{
namespace
{
/// Frame timestamp queries of one device owner (R14c).
struct MeasurementVulkanState final
{
    GpuTiming Timing;
    VkQueryPool Pool = VK_NULL_HANDLE;
    uint64 CpuStart = 0;
    usize Frame = 0;
};
/// One record per device owner slot (R14c); see internal/owner.h.
foundation::StaticArray<MeasurementVulkanState, MAX_DEVICE_OWNERS> gMeasurementVulkanStates{};
/// MeasurementVulkanState of the bound owner slot.
MeasurementVulkanState& GetMeasurementVulkan() noexcept
{
    return gMeasurementVulkanStates[internal::GetBoundOwnerSlot()];
}
} // namespace
// Thanks to AMD GPUOpen, Vulkan Memory Allocator 3.3.0, Statistics:
// https://gpuopen-librariesandsdks.github.io/VulkanMemoryAllocator/html/statistics.html
// blockBytes describes API device-memory blocks; allocationBytes describes binding spans.
bool AllocatorBlockBytes(uint64& bytes) noexcept
{
    bytes = 0;
    if (GetRasterVulkan().Allocator != nullptr)
    {
        VmaTotalStatistics statistics{};
        vmaCalculateStatistics(GetRasterVulkan().Allocator, &statistics);
        bytes = statistics.total.statistics.blockBytes;
    }
    return true;
}
MeasurementAdapter TimingAdapter() noexcept
{
    VkPhysicalDeviceProperties properties{};
    vkGetPhysicalDeviceProperties(GetVulkan().Physical, &properties);
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
    if (GetMeasurementVulkan().Pool != VK_NULL_HANDLE && GetVulkan().Device != VK_NULL_HANDLE)
    {
        // Device teardown already drains rendering before releasing raster records.
        vkDestroyQueryPool(GetVulkan().Device, GetMeasurementVulkan().Pool, nullptr);
    }
    GetMeasurementVulkan().Pool = VK_NULL_HANDLE;
    GetMeasurementVulkan().Timing = {};
}
GpuTiming TimingPoll() noexcept
{
    if (GetMeasurementVulkan().Timing.State == GpuTimingState::Pending)
    {
        // Query reset executes on the GPU. Old values can remain available until then;
        // the captured submission fence must complete before reading the reused pool.
        const VkResult fence =
            vkGetFenceStatus(GetVulkan().Device, GetVulkan().Frames[GetMeasurementVulkan().Frame].Fence);
        if (fence == VK_NOT_READY)
        {
            return GetMeasurementVulkan().Timing;
        }
        if (fence != VK_SUCCESS)
        {
            GetMeasurementVulkan().Timing.State = GpuTimingState::Failed;
            if (fence == VK_ERROR_DEVICE_LOST)
            {
                internal::Fail(GetVulkan().Session, StartupError::DeviceLost);
            }
            return GetMeasurementVulkan().Timing;
        }
        uint64 values[4]{};
        const VkResult result = vkGetQueryPoolResults(GetVulkan().Device,
                                                      GetMeasurementVulkan().Pool,
                                                      0,
                                                      2,
                                                      sizeof(values),
                                                      values,
                                                      sizeof(uint64) * 2,
                                                      VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WITH_AVAILABILITY_BIT);
        if (result == VK_SUCCESS && values[1] != 0 && values[3] != 0)
        {
            const auto now = time::NowTicks();
            GetMeasurementVulkan().Timing.State =
                now >= GetMeasurementVulkan().CpuStart &&
                        internal::TimestampDuration(
                            values[0],
                            values[2],
                            {GetMeasurementVulkan().Timing.ValidBits, GetMeasurementVulkan().Timing.TickNanoseconds},
                            now - GetMeasurementVulkan().CpuStart,
                            GetMeasurementVulkan().Timing.DurationNanoseconds)
                    ? GpuTimingState::Available
                    : GpuTimingState::Disjoint;
        }
        else if (result != VK_NOT_READY && result != VK_SUCCESS)
        {
            GetMeasurementVulkan().Timing.State = GpuTimingState::Failed;
            if (result == VK_ERROR_DEVICE_LOST)
            {
                internal::Fail(GetVulkan().Session, StartupError::DeviceLost);
            }
        }
    }
    return GetMeasurementVulkan().Timing;
}
RasterStatus TimingArm() noexcept
{
    const auto state = TimingPoll().State;
    if (GetVulkan().Device == VK_NULL_HANDLE)
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
    vkGetPhysicalDeviceQueueFamilyProperties(GetVulkan().Physical, &count, families);
    VkPhysicalDeviceProperties properties{};
    vkGetPhysicalDeviceProperties(GetVulkan().Physical, &properties);
    if (GetVulkan().Family >= count || families[GetVulkan().Family].timestampValidBits == 0 ||
        properties.limits.timestampPeriod <= 0)
    {
        GetMeasurementVulkan().Timing = { .State = GpuTimingState::Unavailable };
        return RasterStatus::Unsupported;
    }
    if (GetMeasurementVulkan().Pool == VK_NULL_HANDLE)
    {
        VkQueryPoolCreateInfo info{};
        info.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO;
        info.queryType = VK_QUERY_TYPE_TIMESTAMP;
        info.queryCount = 2;
        const VkResult result = vkCreateQueryPool(GetVulkan().Device, &info, nullptr, &GetMeasurementVulkan().Pool);
        if (result != VK_SUCCESS)
        {
            GetMeasurementVulkan().Pool = VK_NULL_HANDLE;
            if (result == VK_ERROR_DEVICE_LOST)
            {
                internal::Fail(GetVulkan().Session, StartupError::DeviceLost);
                return RasterStatus::DeviceLost;
            }
            return result == VK_ERROR_OUT_OF_HOST_MEMORY || result == VK_ERROR_OUT_OF_DEVICE_MEMORY
                       ? RasterStatus::OutOfMemory
                       : RasterStatus::Failed;
        }
    }
    GetMeasurementVulkan().Timing =
    {
        .State = GpuTimingState::Armed,
        .TickNanoseconds = static_cast<float64>(properties.limits.timestampPeriod),
        .ValidBits = families[GetVulkan().Family].timestampValidBits,
    };
    return RasterStatus::Ready;
}
void TimingBegin() noexcept
{
    if (GetMeasurementVulkan().Timing.State == GpuTimingState::Armed)
    {
        GetMeasurementVulkan().CpuStart = time::NowTicks();
        vkCmdResetQueryPool(GetVulkan().Frames[GetVulkan().FrameIndex].Command, GetMeasurementVulkan().Pool, 0, 2);
        vkCmdWriteTimestamp(GetVulkan().Frames[GetVulkan().FrameIndex].Command,
                            VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                            GetMeasurementVulkan().Pool,
                            0);
    }
}
void TimingEnd() noexcept
{
    if (GetMeasurementVulkan().Timing.State == GpuTimingState::Armed)
    {
        vkCmdWriteTimestamp(GetVulkan().Frames[GetVulkan().FrameIndex].Command,
                            VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
                            GetMeasurementVulkan().Pool,
                            1);
        GetMeasurementVulkan().Frame = GetVulkan().FrameIndex;
    }
}
void TimingSubmitted() noexcept
{
    if (GetMeasurementVulkan().Timing.State == GpuTimingState::Armed)
    {
        GetMeasurementVulkan().Timing.State = GpuTimingState::Pending;
    }
}
bool RasterAllocationBytes(internal::RasterKind kind, usize slot, uint64& bytes) noexcept
{
    bytes = 0;
    const auto add = [&bytes](VmaAllocation allocation) noexcept {
        if (allocation != nullptr)
        {
            VmaAllocationInfo info{};
            vmaGetAllocationInfo(GetRasterVulkan().Allocator, allocation, &info);
            bytes += info.size;
        }
    };
    if (kind == internal::RasterKind::Buffer && slot < GetRasterVulkan().Buffers.GetSize())
    {
        add(GetRasterVulkan().Buffers[slot].Allocation);
    }
    else if (kind == internal::RasterKind::Texture && slot < GetRasterVulkan().Textures.GetSize())
    {
        const RasterVulkanTexture& texture = GetRasterVulkan().Textures[slot];
        add(texture.Allocation);
        add(texture.DepthAllocation);
        add(texture.Upload.Allocation);
    }
    return true;
}
} // namespace ludus::graphics::rhi::backend
