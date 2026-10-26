#pragma once
#include "gpu_pass_timing_buffer.h"
#include "measurement.h"
#include <ludus/foundation/time/time.hpp>

// Thanks to Khronos, Vulkan Specification, "Queries", timestamp availability and
// valid-bit wrapping: https://docs.vulkan.org/spec/latest/chapters/queries.html
// Timestamp pairs outside render passes measure elapsed queue intervals, including
// stalls. The captured submission fence must complete before any reused query is read.
// Thanks also to Khronos, Vulkan Specification, "VK_KHR_calibrated_timestamps" (promoted
// from VK_EXT_calibrated_timestamps) and vkGetCalibratedTimestampsKHR:
// https://docs.vulkan.org/refpages/latest/refpages/source/VK_KHR_calibrated_timestamps.html
// https://docs.vulkan.org/refpages/latest/refpages/source/vkGetCalibratedTimestampsKHR.html
// Adopted: sample the device time domain together with the host clock that NowTicks reads,
// and bound the pairing error by maxDeviation. Ludus samples once before the armed frame is
// recorded and submitted, and once when its fence is observed complete, then interpolates
// pass timestamps between the two, as the Metal backend does (gpu_scope_metal.h). Each batch
// recalibrates; a large deviation or a device/host clock discontinuity keeps it duration-only.
namespace ludus::graphics::rhi::backend
{
namespace
{
constexpr uint32 PASS_QUERY_COUNT = static_cast<uint32>(MAX_GPU_TIMED_PASSES * 2);
constexpr usize MAX_TIMESTAMP_QUEUE_FAMILIES = 128;
/// Device/host elapsed-time disagreement tolerated beyond both deviations, in parts per million of
/// the calibration window: timestampPeriod is nominal. Larger disagreement means a counter reset,
/// suspend or other discontinuity between the two samples.
constexpr float64 MAX_CLOCK_RATE_ERROR_PARTS_PER_MILLION = 1'000;
constexpr float64 PARTS_PER_MILLION = 1'000'000;
/// One device-domain timestamp paired with its host-domain NowTicks value.
struct ClockSample final
{
    internal::ClockCalibration Calibration{};
    uint64 MaxDeviationNanoseconds = 0;
};
internal::GpuPassTimingBuffer gPassTimings;
VkQueryPool gPassQueryPool = VK_NULL_HANDLE;
VkFence gPassFence = VK_NULL_HANDLE;
uint64 gPassCpuStart = 0;
internal::TimestampCounter gPassClock{};
usize gActiveTimedPass = MAX_GPU_TIMED_PASSES;
/// Admitted at owned-device creation or from a borrowed device's declaration; null stays duration-only.
PFN_vkGetCalibratedTimestampsKHR gGetCalibratedTimestampsOrNull = nullptr;
ClockSample gPassBeginClock;
bool hasPassBeginClock = false;
#if defined(LUDUS_PLATFORM_LINUX) || defined(LUDUS_PLATFORM_ANDROID)
/// steady_clock, and therefore NowTicks, reads CLOCK_MONOTONIC nanoseconds on Linux.
constexpr VkTimeDomainKHR HOST_TIME_DOMAIN = VK_TIME_DOMAIN_CLOCK_MONOTONIC_KHR;
/// Calibrateable domains one device may report; more are rejected, never truncated.
constexpr uint32 MAX_TIME_DOMAINS = 16;
/// Each calibration pairs the device domain with the host domain, in this order.
constexpr uint32 CALIBRATED_DOMAIN_COUNT = 2;
constexpr usize DEVICE_DOMAIN_INDEX = 0;
constexpr usize HOST_DOMAIN_INDEX = 1;
constexpr VkStructureType CALIBRATED_TIMESTAMP_INFO = VK_STRUCTURE_TYPE_CALIBRATED_TIMESTAMP_INFO_KHR;
bool HasClockDomains(PFN_vkGetPhysicalDeviceCalibrateableTimeDomainsKHR getDomainsOrNull,
                     VkPhysicalDevice physical) noexcept
{
    uint32 count = 0;
    if (getDomainsOrNull == nullptr || getDomainsOrNull(physical, &count, nullptr) != VK_SUCCESS ||
        count > MAX_TIME_DOMAINS)
    {
        return false;
    }
    StaticArray<VkTimeDomainKHR, MAX_TIME_DOMAINS> domains;
    if (getDomainsOrNull(physical, &count, domains.GetData()) != VK_SUCCESS)
    {
        return false;
    }
    bool hasDevice = false;
    bool hasHost = false;
    for (uint32 i = 0; i < count; ++i)
    {
        hasDevice = hasDevice == true || domains[i] == VK_TIME_DOMAIN_DEVICE_KHR;
        hasHost = hasHost == true || domains[i] == HOST_TIME_DOMAIN;
    }
    return hasDevice == true && hasHost == true;
}
/// Pairs the device and host domains. The host value must lie between the NowTicks reads
/// around the call, which proves that the domain is the NowTicks clock.
bool SampleClocks(ClockSample& outSample) noexcept
{
    if (gGetCalibratedTimestampsOrNull == nullptr)
    {
        return false;
    }
    StaticArray<VkCalibratedTimestampInfoKHR, CALIBRATED_DOMAIN_COUNT> domains;
    domains[DEVICE_DOMAIN_INDEX] = { .sType = CALIBRATED_TIMESTAMP_INFO, .timeDomain = VK_TIME_DOMAIN_DEVICE_KHR };
    domains[HOST_DOMAIN_INDEX] = { .sType = CALIBRATED_TIMESTAMP_INFO, .timeDomain = HOST_TIME_DOMAIN };
    StaticArray<uint64, CALIBRATED_DOMAIN_COUNT> timestamps;
    uint64 deviation = 0;
    const uint64 before = time::NowTicks();
    const VkResult result = gGetCalibratedTimestampsOrNull(gDevice,
                                                           CALIBRATED_DOMAIN_COUNT,
                                                           domains.GetData(),
                                                           timestamps.GetData(),
                                                           &deviation);
    const uint64 after = time::NowTicks();
    const uint64 host = timestamps[HOST_DOMAIN_INDEX];
    if (result != VK_SUCCESS || host < before || host > after)
    {
        return false;
    }
    outSample =
    {
        .Calibration = { .Ticks = host, .DeviceTimestamp = timestamps[DEVICE_DOMAIN_INDEX] },
        .MaxDeviationNanoseconds = deviation,
    };
    return true;
}
#else
bool SampleClocks(ClockSample&) noexcept
{
    return false;
}
#endif
/// Validates the begin/completion pair. Both clocks must advance by the same elapsed time,
/// within both deviations and the nominal-rate tolerance, inside one device counter period.
bool IsCalibrationValid(const ClockSample& first, const ClockSample& last, uint64& outUncertainty) noexcept
{
    const internal::ClockCalibration& begin = first.Calibration;
    const internal::ClockCalibration& end = last.Calibration;
    if (first.MaxDeviationNanoseconds > internal::MAX_CLOCK_UNCERTAINTY_NANOSECONDS ||
        last.MaxDeviationNanoseconds > internal::MAX_CLOCK_UNCERTAINTY_NANOSECONDS || end.Ticks <= begin.Ticks)
    {
        return false;
    }
    const uint64 elapsedHost = end.Ticks - begin.Ticks;
    const float64 tolerance =
        static_cast<float64>(first.MaxDeviationNanoseconds + last.MaxDeviationNanoseconds) +
        static_cast<float64>(elapsedHost) * MAX_CLOCK_RATE_ERROR_PARTS_PER_MILLION / PARTS_PER_MILLION;
    float64 elapsedDevice = 0;
    // Rejects windows of one counter period or more and devices running ahead of the host.
    if (internal::TimestampDuration(begin.DeviceTimestamp,
                                    end.DeviceTimestamp,
                                    gPassClock,
                                    elapsedHost + static_cast<uint64>(tolerance),
                                    elapsedDevice) == false ||
        elapsedDevice + tolerance < static_cast<float64>(elapsedHost))
    {
        return false;
    }
    outUncertainty = first.MaxDeviationNanoseconds > last.MaxDeviationNanoseconds ? first.MaxDeviationNanoseconds
                                                                                  : last.MaxDeviationNanoseconds;
    return true;
}
} // namespace
const char* AdmitClockCalibrationExtensionOrNull(VkPhysicalDevice physical,
                                                 NativeCalibratedTimestamps extension) noexcept
{
#if defined(LUDUS_PLATFORM_LINUX) || defined(LUDUS_PLATFORM_ANDROID)
    if (extension == NativeCalibratedTimestamps::Khr &&
        HasDeviceExtension(physical, VK_KHR_CALIBRATED_TIMESTAMPS_EXTENSION_NAME) == true &&
        HasClockDomains(vkGetPhysicalDeviceCalibrateableTimeDomainsKHR, physical) == true)
    {
        return VK_KHR_CALIBRATED_TIMESTAMPS_EXTENSION_NAME;
    }
    if (extension == NativeCalibratedTimestamps::Ext &&
        HasDeviceExtension(physical, VK_EXT_CALIBRATED_TIMESTAMPS_EXTENSION_NAME) == true &&
        HasClockDomains(vkGetPhysicalDeviceCalibrateableTimeDomainsEXT, physical) == true)
    {
        return VK_EXT_CALIBRATED_TIMESTAMPS_EXTENSION_NAME;
    }
#else
    static_cast<void>(physical);
    static_cast<void>(extension);
#endif
    return nullptr;
}
void ConnectClockCalibration(const char* extensionOrNull) noexcept
{
    gGetCalibratedTimestampsOrNull = nullptr;
    hasPassBeginClock = false;
    if (extensionOrNull == nullptr)
    {
        return;
    }
    gGetCalibratedTimestampsOrNull = std::strcmp(extensionOrNull, VK_KHR_CALIBRATED_TIMESTAMPS_EXTENSION_NAME) == 0
                                         ? vkGetCalibratedTimestampsKHR
                                         : vkGetCalibratedTimestampsEXT;
}
bool HasPassClockCalibration() noexcept
{
    return gGetCalibratedTimestampsOrNull != nullptr;
}
void ResetPassTimings() noexcept
{
    if (gPassQueryPool != VK_NULL_HANDLE && gDevice != VK_NULL_HANDLE)
    {
        vkDestroyQueryPool(gDevice, gPassQueryPool, nullptr);
    }
    gPassQueryPool = VK_NULL_HANDLE;
    gPassFence = VK_NULL_HANDLE;
    gActiveTimedPass = MAX_GPU_TIMED_PASSES;
    hasPassBeginClock = false;
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
        hasPassBeginClock = false;
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
    ClockSample completionClock;
    uint64 uncertainty = 0;
    const bool isCalibrated = result == VK_SUCCESS && hasPassBeginClock == true &&
                              SampleClocks(completionClock) == true &&
                              IsCalibrationValid(gPassBeginClock, completionClock, uncertainty) == true;
    constexpr usize WORDS_PER_PASS = 4;
    for (usize index = 0; index < report.Count; ++index)
    {
        const usize first = index * WORDS_PER_PASS;
        GpuPassTiming& pass = report.Passes[index];
        GpuTiming& timing = pass.Timing;
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
        internal::TimestampInterval ticks;
        if (isCalibrated == true && timing.State == GpuTimingState::Available &&
            internal::MapCalibratedInterval(gPassBeginClock.Calibration,
                                            completionClock.Calibration,
                                            gPassClock.Bits,
                                            { .Start = values[first], .End = values[first + 2] },
                                            ticks) == true)
        {
            pass.HasCpuInterval = true;
            pass.StartTicks = ticks.Start;
            pass.EndTicks = ticks.End;
            report.ClockUncertaintyNanoseconds = uncertainty;
        }
    }
    hasPassBeginClock = false;
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
        // Sampled before recording, so no timed operation of this frame can precede it.
        hasPassBeginClock = SampleClocks(gPassBeginClock);
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
