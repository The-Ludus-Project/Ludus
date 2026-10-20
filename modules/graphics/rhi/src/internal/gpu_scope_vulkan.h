#pragma once
#include "gpu_pass_timing_buffer.h"
#include <ludus/foundation/time/time.hpp>

// Thanks to Khronos, Vulkan Specification, "Queries", timestamp availability and
// valid-bit wrapping: https://docs.vulkan.org/spec/latest/chapters/queries.html
// Timestamp pairs outside render passes measure elapsed queue intervals, including
// stalls. The captured submission fence must complete before any reused query is read.
namespace ludus::graphics::rhi::backend
{
namespace
{
constexpr uint32 PASS_QUERY_COUNT = static_cast<uint32>(MAX_GPU_TIMED_PASSES * 2);
constexpr usize MAX_TIMESTAMP_QUEUE_FAMILIES = 128;
internal::GpuPassTimingBuffer gPassTimings;
VkQueryPool gPassQueryPool = VK_NULL_HANDLE;
VkFence gPassFence = VK_NULL_HANDLE;
uint64 gPassCpuStart = 0;
internal::TimestampCounter gPassClock{};
usize gActiveTimedPass = MAX_GPU_TIMED_PASSES;
} // namespace
void ResetPassTimings() noexcept
{
    if (gPassQueryPool != VK_NULL_HANDLE && gDevice != VK_NULL_HANDLE)
    {
        vkDestroyQueryPool(gDevice, gPassQueryPool, nullptr);
    }
    gPassQueryPool = VK_NULL_HANDLE;
    gPassFence = VK_NULL_HANDLE;
    gActiveTimedPass = MAX_GPU_TIMED_PASSES;
    gPassTimings.Reset();
}
GpuPassTimings PollPassTimings() noexcept
{
    GpuPassTimings& report = gPassTimings.GetReport();
    if (report.State != GpuTimingState::Pending)
    {
        return report;
    }
    const VkResult fence = vkGetFenceStatus(gDevice, gPassFence);
    if (fence == VK_NOT_READY)
    {
        return report;
    }
    if (fence != VK_SUCCESS)
    {
        report.State = GpuTimingState::Failed;
        for (usize index = 0; index < report.Count; ++index)
        {
            report.Passes[index].Timing.State = GpuTimingState::Failed;
        }
        if (fence == VK_ERROR_DEVICE_LOST)
        {
            internal::Fail(gSession, StartupError::DeviceLost);
        }
        return report;
    }
    constexpr usize QUERY_WORDS = MAX_GPU_TIMED_PASSES * 4;
    StaticArray<uint64, QUERY_WORDS> values;
    const uint32 count = static_cast<uint32>(report.Count * 2);
    const VkResult result = vkGetQueryPoolResults(gDevice,
                                                  gPassQueryPool,
                                                  0,
                                                  count,
                                                  count * sizeof(uint64) * 2,
                                                  values.GetData(),
                                                  sizeof(uint64) * 2,
                                                  VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WITH_AVAILABILITY_BIT);
    const uint64 now = time::NowTicks();
    constexpr usize WORDS_PER_PASS = 4;
    for (usize index = 0; index < report.Count; ++index)
    {
        const usize first = index * WORDS_PER_PASS;
        GpuTiming& timing = report.Passes[index].Timing;
        timing.TickNanoseconds = gPassClock.Tick;
        timing.ValidBits = gPassClock.Bits;
        timing.State = result == VK_SUCCESS && values[first + 1] != 0 && values[first + WORDS_PER_PASS - 1] != 0 &&
                               now >= gPassCpuStart &&
                               internal::TimestampDuration(values[first],
                                                           values[first + 2],
                                                           gPassClock,
                                                           now - gPassCpuStart,
                                                           timing.DurationNanoseconds) == true
                           ? GpuTimingState::Available
                           : GpuTimingState::Disjoint;
    }
    report.State = result == VK_SUCCESS ? GpuTimingState::Available : GpuTimingState::Failed;
    if (result == VK_ERROR_DEVICE_LOST)
    {
        internal::Fail(gSession, StartupError::DeviceLost);
    }
    return report;
}
RasterStatus ArmPassTimings() noexcept
{
    static_cast<void>(PollPassTimings());
    if (gDevice == VK_NULL_HANDLE)
    {
        return GetStartup().State == StartupState::DeviceLost ? RasterStatus::DeviceLost : RasterStatus::Failed;
    }
    if (gPassTimings.CanArm() == false)
    {
        return RasterStatus::InvalidState;
    }
    uint32 count = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(gPhysical, &count, nullptr);
    if (count > MAX_TIMESTAMP_QUEUE_FAMILIES)
    {
        gPassTimings.GetReport() = { .State = GpuTimingState::Unavailable };
        return RasterStatus::Unsupported;
    }
    StaticArray<VkQueueFamilyProperties, MAX_TIMESTAMP_QUEUE_FAMILIES> families;
    vkGetPhysicalDeviceQueueFamilyProperties(gPhysical, &count, families.GetData());
    VkPhysicalDeviceProperties properties{};
    vkGetPhysicalDeviceProperties(gPhysical, &properties);
    if (gFamily >= count || families[gFamily].timestampValidBits == 0 || properties.limits.timestampPeriod <= 0)
    {
        gPassTimings.GetReport() = { .State = GpuTimingState::Unavailable };
        return RasterStatus::Unsupported;
    }
    if (gPassQueryPool == VK_NULL_HANDLE)
    {
        const VkQueryPoolCreateInfo description
        {
            .sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO,
            .queryType = VK_QUERY_TYPE_TIMESTAMP,
            .queryCount = PASS_QUERY_COUNT,
        };
        const VkResult result = vkCreateQueryPool(gDevice, &description, nullptr, &gPassQueryPool);
        if (result != VK_SUCCESS)
        {
            gPassQueryPool = VK_NULL_HANDLE;
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
    gPassClock = {families[gFamily].timestampValidBits, static_cast<float64>(properties.limits.timestampPeriod)};
    gPassTimings.Arm();
    return RasterStatus::Ready;
}
// Thanks to Khronos, Vulkan Guide, "VK_EXT_debug_utils", command-buffer labels:
// https://docs.vulkan.org/guide/latest/extensions/VK_EXT_debug_utils.html
// Optional labels share scope text; extension absence never changes rendering.
RasterStatus BeginScope(const GpuScopeDescription& scope) noexcept
{
    if (gPassTimings.BeginScope(scope) == false)
    {
        return RasterStatus::InvalidState;
    }
    if (hasDebugLabels == true && vkCmdBeginDebugUtilsLabelEXT != nullptr)
    {
        const VkDebugUtilsLabelEXT label
        {
            .sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_LABEL_EXT,
            .pLabelName = scope.Name.GetData(),
        };
        vkCmdBeginDebugUtilsLabelEXT(gFrames[gFrame].Command, &label);
    }
    return RasterStatus::Ready;
}
RasterStatus EndScope() noexcept
{
    if (gPassTimings.EndScope() == false)
    {
        return RasterStatus::InvalidState;
    }
    if (hasDebugLabels == true && vkCmdEndDebugUtilsLabelEXT != nullptr)
    {
        vkCmdEndDebugUtilsLabelEXT(gFrames[gFrame].Command);
    }
    return RasterStatus::Ready;
}
void BeginPassTimingFrame() noexcept
{
    gPassTimings.BeginFrame();
    gActiveTimedPass = MAX_GPU_TIMED_PASSES;
    if (gPassTimings.GetReport().State == GpuTimingState::Armed)
    {
        gPassCpuStart = time::NowTicks();
        vkCmdResetQueryPool(gFrames[gFrame].Command, gPassQueryPool, 0, PASS_QUERY_COUNT);
    }
}
void SubmitPassTimings() noexcept
{
    if (gPassTimings.GetReport().State == GpuTimingState::Armed)
    {
        gPassFence = gFrames[gFrame].Fence;
    }
    gPassTimings.Submit();
}
void BeginPhysicalPassTiming(GpuPassKind kind) noexcept
{
    gActiveTimedPass = gPassTimings.AddPass(kind);
    if (gActiveTimedPass < MAX_GPU_TIMED_PASSES)
    {
        vkCmdWriteTimestamp(gFrames[gFrame].Command,
                            VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                            gPassQueryPool,
                            static_cast<uint32>(gActiveTimedPass * 2));
    }
}
void EndPhysicalPassTiming() noexcept
{
    if (gActiveTimedPass < MAX_GPU_TIMED_PASSES)
    {
        vkCmdWriteTimestamp(gFrames[gFrame].Command,
                            VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
                            gPassQueryPool,
                            static_cast<uint32>(gActiveTimedPass * 2 + 1));
        gActiveTimedPass = MAX_GPU_TIMED_PASSES;
    }
}
} // namespace ludus::graphics::rhi::backend
