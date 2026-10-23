#pragma once
// Thanks to Apple, MTLCommandBuffer GPUStartTime/GPUEndTime (completion-only timing):
// https://developer.apple.com/documentation/metal/mtlcommandbuffer/gpustarttime
// and MTLResource allocatedSize: https://developer.apple.com/documentation/metal/mtlresource/allocatedsize
// Retain a command buffer and poll its status, without installing cross-thread callbacks.
namespace ludus::graphics::rhi::backend
{
namespace
{
GpuTiming gTiming;
id<MTLCommandBuffer> gTimingCommand = nil;
} // namespace
bool AllocatorBlockBytes(uint64& bytes) noexcept
{
    bytes = 0;
    return false;
}
MeasurementAdapter TimingAdapter() noexcept
{
    MeasurementAdapter output;
    internal::CopyMeasurementName(gDevice.name.UTF8String, 255, output.Name);
    output.SoftwareKnown = true;
    return output;
}
void TimingReset() noexcept
{
    gTimingCommand = nil;
    gTiming = {};
}
GpuTiming TimingPoll() noexcept
{
    if (gTiming.State == GpuTimingState::Pending)
    {
        if (gTimingCommand.status == MTLCommandBufferStatusError)
        {
            gTiming.State = GpuTimingState::Failed;
            (void)CheckCommand(gTimingCommand);
        }
        else if (gTimingCommand.status == MTLCommandBufferStatusCompleted)
        {
            const auto start = gTimingCommand.GPUStartTime, end = gTimingCommand.GPUEndTime;
            if (start > 0 && end >= start && end - start < 1e9)
            {
                gTiming.DurationNanoseconds = (end - start) * 1e9;
                gTiming.State = GpuTimingState::Available;
            }
            else
            {
                gTiming.State = GpuTimingState::Disjoint;
            }
        }
    }
    return gTiming;
}
RasterStatus TimingArm() noexcept
{
    const auto state = TimingPoll().State;
    if (gDevice == nil)
    {
        return GetStartup().State == StartupState::DeviceLost ? RasterStatus::DeviceLost : RasterStatus::Failed;
    }
    if (state == GpuTimingState::Armed || state == GpuTimingState::Pending)
    {
        return RasterStatus::InvalidState;
    }
    TimingReset();
    gTiming.State = GpuTimingState::Armed;
    return RasterStatus::Ready;
}
void TimingBegin() noexcept {}
void TimingEnd() noexcept
{
    if (gTiming.State == GpuTimingState::Armed && hasNativeComputeSplit == true)
    {
        gTiming.State = GpuTimingState::Disjoint;
        return;
    }
    if (gTiming.State == GpuTimingState::Armed)
    {
        gTimingCommand = gCommand;
        gTiming.State = GpuTimingState::Pending;
    }
}
bool RasterAllocationBytes(internal::RasterKind kind, usize slot, uint64& bytes) noexcept
{
    bytes = 0;
    if (kind == internal::RasterKind::Buffer && slot < gRasterBuffers.GetSize())
    {
        bytes = gRasterBuffers[slot].allocatedSize;
    }
    else if (kind == internal::RasterKind::Texture && slot < gRasterTextures.GetSize())
    {
        bytes = gRasterTextures[slot].allocatedSize + gRasterDepths[slot].allocatedSize;
    }
    return true;
}
} // namespace ludus::graphics::rhi::backend
