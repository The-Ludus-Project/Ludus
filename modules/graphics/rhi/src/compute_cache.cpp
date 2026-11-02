#include <ludus/graphics/rhi/compute_cache.hpp>

namespace ludus::graphics::rhi
{
RasterStatus
ComputeCache::Stage(DeviceHandle device, ComputeCacheKey key, const ComputeWorkloadDescription& description) noexcept
{
    if (key.Scene == 0 || key.Model == 0 || (mDevice != DeviceHandle{} && mDevice != device))
    {
        return RasterStatus::InvalidDescription;
    }
    if (key == mActiveKey)
    {
        CancelCandidate();
        return RasterStatus::Ready;
    }
    if (key == mCandidateKey)
    {
        return Poll();
    }
    if (mNextCandidate == 0)
    {
        return RasterStatus::IdentityExhausted;
    }
    if (description.Buffers.IsValid() == false)
    {
        return RasterStatus::InvalidDescription;
    }
    for (foundation::usize i = 0; i < description.Buffers.GetSize(); ++i)
    {
        const ComputeWorkloadBuffer& buffer = description.Buffers[i];
        // Candidate writes must target fresh owned storage, never a live generation.
        if (buffer.External != BufferHandle{} && buffer.IsReadOnly == false)
        {
            return RasterStatus::InvalidDescription;
        }
    }
    ComputeWorkload candidate;
    const RasterStatus status = candidate.Initialize(device, description);
    if (status != RasterStatus::Ready && status != RasterStatus::Pending)
    {
        return status;
    }
    mCandidate = foundation::Move(candidate);
    mCandidateKey = key;
    mCandidateGeneration = mNextCandidate;
    mNextCandidate = mNextCandidate == ~foundation::uint64{0} ? 0 : mNextCandidate + 1;
    mDevice = device;
    mCompletion = {};
    mFailure = RasterStatus::NotReady;
    mPhase = status == RasterStatus::Pending ? ComputeCachePhase::Preparing : ComputeCachePhase::Prepared;
    return RasterStatus::Pending;
}
RasterStatus ComputeCache::Poll() noexcept
{
    if (mPhase == ComputeCachePhase::Empty)
    {
        return HasActive() == true ? RasterStatus::Ready : RasterStatus::NotReady;
    }
    if (mPhase == ComputeCachePhase::Failed)
    {
        return mFailure;
    }
    if (mPhase == ComputeCachePhase::Preparing)
    {
        const RasterStatus status = mCandidate.Poll();
        if (status == RasterStatus::Ready)
        {
            mPhase = ComputeCachePhase::Prepared;
        }
        else if (status != RasterStatus::Pending)
        {
            mPhase = ComputeCachePhase::Failed;
            mFailure = status;
            return status;
        }
    }
    if (mPhase == ComputeCachePhase::Executing)
    {
        const RasterStatus status = GetStatus(mDevice, mCompletion);
        if (status == RasterStatus::Ready)
        {
            mActive = foundation::Move(mCandidate);
            mActiveKey = mCandidateKey;
            mCandidateKey = {};
            mCompletion = {};
            mPhase = ComputeCachePhase::Empty;
            return RasterStatus::Ready;
        }
        if (status != RasterStatus::Pending)
        {
            mPhase = ComputeCachePhase::Failed;
            mFailure = status;
            return status;
        }
    }
    return RasterStatus::Pending;
}
RasterStatus ComputeCache::BuildCandidateGraph(OrderedGraph& outGraph) noexcept
{
    if (mPhase != ComputeCachePhase::Prepared)
    {
        return RasterStatus::NotReady;
    }
    const RasterStatus status = mCandidate.BuildGraph(outGraph);
    if (status == RasterStatus::Ready)
    {
        mPhase = ComputeCachePhase::AwaitingSubmission;
    }
    return status;
}
ComputeCacheTicket ComputeCache::GetCandidateTicket() const noexcept
{
    ComputeCacheTicket ticket;
    if (mPhase != ComputeCachePhase::Empty)
    {
        ticket.OwnerOrNull = this;
        ticket.Generation = mCandidateGeneration;
    }
    return ticket;
}
RasterStatus ComputeCache::SetCandidateCompletion(ComputeCacheTicket ticket, SubmissionToken completion) noexcept
{
    if (mPhase != ComputeCachePhase::AwaitingSubmission || ticket.OwnerOrNull != this ||
        ticket.Generation != mCandidateGeneration)
    {
        return RasterStatus::InvalidDescription;
    }
    const RasterStatus status = GetStatus(mDevice, completion);
    if (status != RasterStatus::Ready && status != RasterStatus::Pending)
    {
        return status;
    }
    mCompletion = completion;
    mPhase = ComputeCachePhase::Executing;
    return RasterStatus::Pending;
}
void ComputeCache::CancelCandidate() noexcept
{
    mCandidate.Reset();
    mCandidateKey = {};
    mCandidateGeneration = 0;
    mCompletion = {};
    mPhase = ComputeCachePhase::Empty;
    mFailure = RasterStatus::NotReady;
}
bool ComputeCache::HasActive() const noexcept
{
    return mActiveKey.Scene != 0;
}
ComputeCacheKey ComputeCache::GetActiveKey() const noexcept
{
    return mActiveKey;
}
ComputeCachePhase ComputeCache::GetPhase() const noexcept
{
    return mPhase;
}
RasterStatus ComputeCache::GetActiveBuffer(foundation::usize index, BufferHandle& outBuffer) const noexcept
{
    return mActive.GetBuffer(index, outBuffer);
}
void ComputeCache::Reset() noexcept
{
    CancelCandidate();
    mActive.Reset();
    mActiveKey = {};
    mDevice = {};
}
} // namespace ludus::graphics::rhi
