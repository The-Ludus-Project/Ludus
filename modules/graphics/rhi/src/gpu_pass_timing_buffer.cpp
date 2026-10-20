#include "internal/gpu_pass_timing_buffer.h"

namespace ludus::graphics::rhi::internal
{
bool GpuPassTimingBuffer::CanArm() const noexcept
{
    return mReport.State != GpuTimingState::Armed && mReport.State != GpuTimingState::Pending;
}
void GpuPassTimingBuffer::Arm() noexcept
{
    mReport = { .State = GpuTimingState::Armed };
    mIsCapturingFrame = false;
}
void GpuPassTimingBuffer::BeginFrame() noexcept
{
    mIsCapturingFrame = mReport.State == GpuTimingState::Armed;
}
void GpuPassTimingBuffer::Submit() noexcept
{
    if (mIsCapturingFrame == true)
    {
        mReport.State = mReport.Count == 0 ? GpuTimingState::Available : GpuTimingState::Pending;
    }
    mIsCapturingFrame = false;
    mHasScope = false;
}
void GpuPassTimingBuffer::Reset() noexcept
{
    mReport = {};
    mHasScope = false;
    mIsCapturingFrame = false;
}
bool GpuPassTimingBuffer::BeginScope(const GpuScopeDescription& scope) noexcept
{
    if (mHasScope == true)
    {
        return false;
    }
    mScope = scope;
    mScopePassIndex = 0;
    mHasScope = true;
    return true;
}
bool GpuPassTimingBuffer::EndScope() noexcept
{
    const bool hasScope = mHasScope;
    mHasScope = false;
    return hasScope;
}
foundation::usize GpuPassTimingBuffer::AddPass(GpuPassKind kind) noexcept
{
    if (mIsCapturingFrame == false || mHasScope == false)
    {
        return MAX_GPU_TIMED_PASSES;
    }
    if (mReport.Count == MAX_GPU_TIMED_PASSES)
    {
        ++mReport.DroppedPasses;
        ++mScopePassIndex;
        return MAX_GPU_TIMED_PASSES;
    }
    const foundation::usize index = mReport.Count++;
    mReport.Passes[index] = { .Scope = mScope, .Kind = kind, .ScopePassIndex = mScopePassIndex++ };
    return index;
}
} // namespace ludus::graphics::rhi::internal
