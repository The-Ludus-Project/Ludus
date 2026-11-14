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
    static_cast<void>(CloseScopes());
}
void GpuPassTimingBuffer::Reset() noexcept
{
    mReport = {};
    mIsCapturingFrame = false;
    static_cast<void>(CloseScopes());
}
bool GpuPassTimingBuffer::BeginScope(const GpuScopeDescription& scope) noexcept
{
    if (mDepth == MAX_GPU_SCOPE_DEPTH)
    {
        return false;
    }
    if (mDepth == 0)
    {
        mScopePassIndex = 0;
    }
    mScopes[mDepth++] = scope;
    return true;
}
bool GpuPassTimingBuffer::EndScope() noexcept
{
    if (mDepth == 0)
    {
        return false;
    }
    --mDepth;
    return true;
}
foundation::uint32 GpuPassTimingBuffer::CloseScopes() noexcept
{
    const foundation::uint32 closed = mDepth;
    mDepth = 0;
    // Zero is never a live epoch, so wrapping skips it.
    mEpoch = mEpoch == ~foundation::uint64{0} ? 1 : mEpoch + 1;
    return closed;
}
foundation::usize GpuPassTimingBuffer::AddPass(GpuPassKind kind) noexcept
{
    if (mIsCapturingFrame == false || mDepth == 0)
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
    mReport.Passes[index] = { .Scope = mScopes[0], .Kind = kind, .ScopePassIndex = mScopePassIndex++ };
    return index;
}
} // namespace ludus::graphics::rhi::internal
