#pragma once

#include "gpu_pass_timing_buffer.h"
#include "measurement.h"
#include <ludus/foundation/time/time.hpp>

// Thanks to Apple, "Sampling GPU data into counter sample buffers", Metal:
// https://developer.apple.com/documentation/metal/sampling-gpu-data-into-counter-sample-buffers
// Stage-boundary attachments measure physical encoder stages. Overlapping render
// stages form one elapsed envelope, never a sum.
// Thanks also to Apple, "Converting GPU timestamps into CPU time", Metal:
// https://developer.apple.com/documentation/metal/converting-gpu-timestamps-into-cpu-time
// Adopted: interpolate counter timestamps between two sampleTimestamps:gpuTimestamp:
// pairs. Ludus departs by mapping onto the NowTicks clock (CLOCK_MONOTONIC_RAW-based),
// whose epoch differs from Metal's CPU timestamps, and rejects calibrations whose
// bracketing reads are too slow or span a sleep.
namespace ludus::graphics::rhi::backend
{
namespace
{
constexpr usize PASS_SAMPLE_STRIDE = 4;
constexpr usize FRAGMENT_END_SAMPLE = PASS_SAMPLE_STRIDE - 1;
constexpr usize PASS_SAMPLE_COUNT = MAX_GPU_TIMED_PASSES * PASS_SAMPLE_STRIDE;
/// One device/CPU timestamp pair bracketed by NowTicks reads.
struct ClockSample final
{
    uint64 Ticks = 0;
    uint64 ReadGapNanoseconds = 0;
    MTLTimestamp CpuTimestamp = 0;
    MTLTimestamp DeviceTimestamp = 0;
};
internal::GpuPassTimingBuffer gPassTimings;
id<MTLCounterSampleBuffer> gPassSamplesOrNull = nil;
id<MTLCommandBuffer> gPassCommandOrNull = nil;
NSString* gPassLabelOrNull = nil;
ClockSample gPassSubmitClock;
bool hasPassSubmitClock = false;
bool SampleClocks(ClockSample& outSample) noexcept
{
    const uint64 before = foundation::time::NowTicks();
    MTLTimestamp cpu = 0;
    MTLTimestamp device = 0;
    [gDevice sampleTimestamps:&cpu gpuTimestamp:&device];
    const uint64 after = foundation::time::NowTicks();
    if (after < before || device == 0)
    {
        return false;
    }
    outSample =
    {
        .Ticks = before + (after - before) / 2,
        .ReadGapNanoseconds = after - before,
        .CpuTimestamp = cpu,
        .DeviceTimestamp = device,
    };
    return true;
}
/// Validates the submit/completion pair. Metal CPU timestamps exclude sleep while NowTicks
/// includes it, so differing elapsed times identify a discontinuity between the samples.
bool IsCalibrationValid(const ClockSample& first, const ClockSample& last, uint64& outUncertainty) noexcept
{
    if (last.Ticks <= first.Ticks || last.DeviceTimestamp <= first.DeviceTimestamp ||
        last.CpuTimestamp < first.CpuTimestamp)
    {
        return false;
    }
    const uint64 elapsedTicks = last.Ticks - first.Ticks;
    const uint64 elapsedCpu = last.CpuTimestamp - first.CpuTimestamp;
    const uint64 difference = elapsedTicks > elapsedCpu ? elapsedTicks - elapsedCpu : elapsedCpu - elapsedTicks;
    const uint64 largerGap =
        first.ReadGapNanoseconds > last.ReadGapNanoseconds ? first.ReadGapNanoseconds : last.ReadGapNanoseconds;
    if (difference > first.ReadGapNanoseconds + last.ReadGapNanoseconds ||
        largerGap > internal::MAX_CLOCK_UNCERTAINTY_NANOSECONDS)
    {
        return false;
    }
    outUncertainty = (largerGap + 1) / 2;
    return true;
}
/// The shared interpolation's view of a sample. Metal timestamps are full 64-bit nanoseconds.
internal::ClockCalibration ToCalibration(const ClockSample& sample) noexcept
{
    return { .Ticks = sample.Ticks, .DeviceTimestamp = sample.DeviceTimestamp };
}
} // namespace
void ResetPassTimings() noexcept
{
    gPassSamplesOrNull = nil;
    gPassCommandOrNull = nil;
    gPassLabelOrNull = nil;
    hasPassSubmitClock = false;
    gPassTimings.Reset();
}
GpuPassTimings PollPassTimings() noexcept
{
    GpuPassTimings& report = gPassTimings.GetReport();
    if (report.State != GpuTimingState::Pending)
    {
        return report;
    }
    if (gPassCommandOrNull.status == MTLCommandBufferStatusError)
    {
        report.State = GpuTimingState::Failed;
        for (usize index = 0; index < report.Count; ++index)
        {
            report.Passes[index].Timing.State = GpuTimingState::Failed;
        }
        hasPassSubmitClock = false;
        static_cast<void>(CheckCommand(gPassCommandOrNull));
    }
    else if (gPassCommandOrNull.status == MTLCommandBufferStatusCompleted)
    {
        NSData* dataOrNull = [gPassSamplesOrNull resolveCounterRange:NSMakeRange(0, report.Count * PASS_SAMPLE_STRIDE)];
        ClockSample completionClock;
        uint64 uncertainty = 0;
        const bool isCalibrated = hasPassSubmitClock == true && SampleClocks(completionClock) == true &&
                                  IsCalibrationValid(gPassSubmitClock, completionClock, uncertainty) == true;
        for (usize index = 0; index < report.Count; ++index)
        {
            GpuTiming& timing = report.Passes[index].Timing;
            timing.State = GpuTimingState::Disjoint;
            timing.TickNanoseconds = 1;
            timing.ValidBits = sizeof(uint64) * CHAR_BIT;
            if (dataOrNull == nil ||
                dataOrNull.length != report.Count * PASS_SAMPLE_STRIDE * sizeof(MTLCounterResultTimestamp))
            {
                continue;
            }
            StaticArray<MTLCounterResultTimestamp, PASS_SAMPLE_STRIDE> values;
            const uint8* bytes = static_cast<const uint8*>(dataOrNull.bytes);
            std::memcpy(values.GetData(), bytes + index * sizeof(values), sizeof(values));
            uint64 start = values[0].timestamp;
            uint64 end = values[1].timestamp;
            bool isValid = start != MTLCounterErrorValue && end != MTLCounterErrorValue;
            if (report.Passes[index].Kind == GpuPassKind::Raster)
            {
                isValid = isValid == true && values[2].timestamp != MTLCounterErrorValue &&
                          values[FRAGMENT_END_SAMPLE].timestamp != MTLCounterErrorValue;
                start = start < values[2].timestamp ? start : values[2].timestamp;
                end = end > values[FRAGMENT_END_SAMPLE].timestamp ? end : values[FRAGMENT_END_SAMPLE].timestamp;
            }
            if (isValid == true && end >= start)
            {
                timing.DurationNanoseconds = static_cast<float64>(end - start);
                timing.State = GpuTimingState::Available;
                GpuPassTiming& pass = report.Passes[index];
                internal::TimestampInterval ticks;
                if (isCalibrated == true && internal::MapCalibratedInterval(ToCalibration(gPassSubmitClock),
                                                                            ToCalibration(completionClock),
                                                                            sizeof(MTLTimestamp) * CHAR_BIT,
                                                                            { .Start = start, .End = end },
                                                                            ticks) == true)
                {
                    pass.HasCpuInterval = true;
                    pass.StartTicks = ticks.Start;
                    pass.EndTicks = ticks.End;
                    report.ClockUncertaintyNanoseconds = uncertainty;
                }
            }
        }
        report.State = GpuTimingState::Available;
        gPassCommandOrNull = nil;
        hasPassSubmitClock = false;
    }
    return report;
}
RasterStatus ArmPassTimings() noexcept
{
    static_cast<void>(PollPassTimings());
    if (gDevice == nil)
    {
        return GetStartup().State == StartupState::DeviceLost ? RasterStatus::DeviceLost : RasterStatus::Failed;
    }
    if (gPassTimings.CanArm() == false)
    {
        return RasterStatus::InvalidState;
    }
    if ([gDevice supportsCounterSampling:MTLCounterSamplingPointAtStageBoundary] == NO)
    {
        gPassTimings.GetReport() = { .State = GpuTimingState::Unavailable };
        return RasterStatus::Unsupported;
    }
    if (gPassSamplesOrNull == nil)
    {
        id<MTLCounterSet> timestampSetOrNull = nil;
        for (id<MTLCounterSet> counterSet in gDevice.counterSets)
        {
            if ([counterSet.name isEqualToString:MTLCommonCounterSetTimestamp] == YES)
            {
                timestampSetOrNull = counterSet;
                break;
            }
        }
        if (timestampSetOrNull == nil)
        {
            gPassTimings.GetReport() = { .State = GpuTimingState::Unavailable };
            return RasterStatus::Unsupported;
        }
        MTLCounterSampleBufferDescriptor* descriptionOrNull = [MTLCounterSampleBufferDescriptor new];
        if (descriptionOrNull == nil)
        {
            return RasterStatus::OutOfMemory;
        }
        descriptionOrNull.counterSet = timestampSetOrNull;
        descriptionOrNull.sampleCount = PASS_SAMPLE_COUNT;
        descriptionOrNull.storageMode = MTLStorageModeShared;
        descriptionOrNull.label = @"Ludus per-pass timestamps";
        NSError* errorOrNull = nil;
        gPassSamplesOrNull = [gDevice newCounterSampleBufferWithDescriptor:descriptionOrNull error:&errorOrNull];
        if (gPassSamplesOrNull == nil)
        {
            return RasterStatus::OutOfMemory;
        }
    }
    gPassTimings.Arm();
    return RasterStatus::Ready;
}
// Thanks to Apple, "pushDebugGroup(_:)" and "label", Metal command-buffer/encoder
// documentation: https://developer.apple.com/documentation/metal/mtlcommandbuffer/pushdebuggroup(_:)
// The command group and encoder labels share the copied timing identity's name.
RasterStatus BeginScope(const GpuScopeDescription& scope) noexcept
{
    if (gPassTimings.BeginScope(scope) == false)
    {
        return RasterStatus::InvalidState;
    }
    gPassLabelOrNull = [NSString stringWithUTF8String:scope.Name.GetData()];
    if (gPassLabelOrNull == nil)
    {
        static_cast<void>(gPassTimings.EndScope());
        return RasterStatus::OutOfMemory;
    }
    [gCommand pushDebugGroup:gPassLabelOrNull];
    return RasterStatus::Ready;
}
RasterStatus EndScope() noexcept
{
    if (gPassTimings.EndScope() == false)
    {
        return RasterStatus::InvalidState;
    }
    [gCommand popDebugGroup];
    gPassLabelOrNull = nil;
    return RasterStatus::Ready;
}
void BeginPassTimingFrame() noexcept
{
    gPassTimings.BeginFrame();
}
void SubmitPassTimings() noexcept
{
    gPassTimings.Submit();
    if (gPassTimings.GetReport().State == GpuTimingState::Pending && gPassCommandOrNull == nil)
    {
        gPassCommandOrNull = gCommand;
        // Sampled before commit, so no operation of this command buffer can precede it.
        hasPassSubmitClock = SampleClocks(gPassSubmitClock);
    }
}
void ConfigurePassTiming(MTLRenderPassDescriptor* description) noexcept
{
    const usize index = gPassTimings.AddPass(GpuPassKind::Raster);
    if (index < MAX_GPU_TIMED_PASSES)
    {
        MTLRenderPassSampleBufferAttachmentDescriptor* attachment = description.sampleBufferAttachments[0];
        attachment.sampleBuffer = gPassSamplesOrNull;
        const usize first = index * PASS_SAMPLE_STRIDE;
        attachment.startOfVertexSampleIndex = first;
        attachment.endOfVertexSampleIndex = first + 1;
        attachment.startOfFragmentSampleIndex = first + 2;
        attachment.endOfFragmentSampleIndex = first + PASS_SAMPLE_STRIDE - 1;
    }
}
void ConfigurePassTiming(MTLComputePassDescriptor* description) noexcept
{
    const usize index = gPassTimings.AddPass(GpuPassKind::Compute);
    if (index < MAX_GPU_TIMED_PASSES)
    {
        MTLComputePassSampleBufferAttachmentDescriptor* attachment = description.sampleBufferAttachments[0];
        attachment.sampleBuffer = gPassSamplesOrNull;
        attachment.startOfEncoderSampleIndex = index * PASS_SAMPLE_STRIDE;
        attachment.endOfEncoderSampleIndex = index * PASS_SAMPLE_STRIDE + 1;
    }
}
} // namespace ludus::graphics::rhi::backend
