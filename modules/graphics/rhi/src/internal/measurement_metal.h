#pragma once
// Thanks to Apple, MTLCommandBuffer GPUStartTime/GPUEndTime (completion-only timing):
// https://developer.apple.com/documentation/metal/mtlcommandbuffer/gpustarttime
// and MTLResource allocatedSize: https://developer.apple.com/documentation/metal/mtlresource/allocatedsize
// Retain a command buffer and poll its status, without installing cross-thread callbacks.
namespace ludus::graphics::rhi::backend
{
namespace
{
/// Frame timing request of one Metal owner (R14c).
struct MeasurementMetalState final
{
    GpuTiming Timing;
    id<MTLCommandBuffer> TimingCommand = nil;
};
StaticArray<MeasurementMetalState, MAX_DEVICE_OWNERS> gMeasurementMetalStates{};
/// The frame timing request of the bound owner slot.
MeasurementMetalState& GetMeasurementMetal() noexcept
{
    return gMeasurementMetalStates[internal::GetBoundOwnerSlot()];
}
} // namespace
bool AllocatorBlockBytes(uint64& bytes) noexcept
{
    bytes = 0;
    return false;
}
MeasurementAdapter TimingAdapter() noexcept
{
    MeasurementAdapter output;
    internal::CopyMeasurementName(GetMetal().Device.name.UTF8String, sizeof(output.Name), output.Name);
    output.SoftwareKnown = true;
    return output;
}
void TimingReset() noexcept
{
    GetMeasurementMetal().TimingCommand = nil;
    GetMeasurementMetal().Timing = {};
}
GpuTiming TimingPoll() noexcept
{
    if (GetMeasurementMetal().Timing.State == GpuTimingState::Pending)
    {
        if (GetMeasurementMetal().TimingCommand.status == MTLCommandBufferStatusError)
        {
            GetMeasurementMetal().Timing.State = GpuTimingState::Failed;
            (void)CheckCommand(GetMeasurementMetal().TimingCommand);
        }
        else if (GetMeasurementMetal().TimingCommand.status == MTLCommandBufferStatusCompleted)
        {
            // GPU start and end times are host seconds.
            const CFTimeInterval start = GetMeasurementMetal().TimingCommand.GPUStartTime;
            const CFTimeInterval end = GetMeasurementMetal().TimingCommand.GPUEndTime;
            const float64 nanosecondsPerSecond = static_cast<float64>(foundation::time::NANOSECONDS_PER_SECOND);
            if (start > 0 && end >= start && end - start < nanosecondsPerSecond)
            {
                GetMeasurementMetal().Timing.DurationNanoseconds = (end - start) * nanosecondsPerSecond;
                GetMeasurementMetal().Timing.State = GpuTimingState::Available;
            }
            else
            {
                GetMeasurementMetal().Timing.State = GpuTimingState::Disjoint;
            }
        }
    }
    return GetMeasurementMetal().Timing;
}
RasterStatus TimingArm() noexcept
{
    const auto state = TimingPoll().State;
    if (GetMetal().Device == nil)
    {
        return internal::GetStartup().State == StartupState::DeviceLost ? RasterStatus::DeviceLost
                                                                        : RasterStatus::Failed;
    }
    if (state == GpuTimingState::Armed || state == GpuTimingState::Pending)
    {
        return RasterStatus::InvalidState;
    }
    TimingReset();
    GetMeasurementMetal().Timing.State = GpuTimingState::Armed;
    return RasterStatus::Ready;
}
void TimingBegin() noexcept {}
void TimingEnd() noexcept
{
    if (GetMeasurementMetal().Timing.State == GpuTimingState::Armed && GetMetal().HasNativeComputeSplit == true)
    {
        GetMeasurementMetal().Timing.State = GpuTimingState::Disjoint;
        return;
    }
    if (GetMeasurementMetal().Timing.State == GpuTimingState::Armed)
    {
        GetMeasurementMetal().TimingCommand = GetMetal().Command;
        GetMeasurementMetal().Timing.State = GpuTimingState::Pending;
    }
}
bool RasterAllocationBytes(internal::RasterKind kind, usize slot, uint64& bytes) noexcept
{
    bytes = 0;
    if (kind == internal::RasterKind::Buffer && slot < GetRasterMetal().Buffers.GetSize())
    {
        bytes = GetRasterMetal().Buffers[slot].allocatedSize;
    }
    else if (kind == internal::RasterKind::Texture && slot < GetRasterMetal().Textures.GetSize())
    {
        bytes = GetRasterMetal().Textures[slot].allocatedSize + GetRasterMetal().Depths[slot].allocatedSize;
    }
    return true;
}
} // namespace ludus::graphics::rhi::backend
