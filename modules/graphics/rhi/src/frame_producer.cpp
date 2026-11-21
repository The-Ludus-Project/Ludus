#include <ludus/foundation/base/assert.hpp>
#include <ludus/graphics/rhi/frame_producer.h>

namespace ludus::graphics::rhi
{
using namespace foundation;
namespace
{
/// Largest slot generation; a slot at it cannot be leased again, because Release could not advance it.
constexpr uint64 LAST_GENERATION = ~uint64{0};
} // namespace

RasterStatus ToRasterStatus(DeviceStatus status) noexcept
{
    switch (status)
    {
        case DeviceStatus::Ready:
            return RasterStatus::Ready;
        case DeviceStatus::Pending:
        case DeviceStatus::Skipped:
        case DeviceStatus::NotReady:
            return RasterStatus::NotReady;
        case DeviceStatus::InvalidHandle:
            return RasterStatus::InvalidHandle;
        case DeviceStatus::InvalidState:
            return RasterStatus::InvalidState;
        case DeviceStatus::InvalidDescription:
            return RasterStatus::InvalidDescription;
        case DeviceStatus::Unsupported:
            return RasterStatus::Unsupported;
        case DeviceStatus::Failed:
            return RasterStatus::Failed;
        case DeviceStatus::DeviceLost:
            return RasterStatus::DeviceLost;
        case DeviceStatus::IdentityExhausted:
            return RasterStatus::IdentityExhausted;
        case DeviceStatus::SurfaceLost:
            return RasterStatus::Failed;
        case DeviceStatus::CapacityExceeded:
            return RasterStatus::CapacityExceeded;
    }
    return RasterStatus::InvalidDescription;
}

RasterStatus FrameRecording::Begin(SurfaceHandle surface) noexcept
{
    if (mIsOpen == true)
    {
        return RasterStatus::InvalidState;
    }
    const DeviceStatus started = BeginFrame(mDevice, surface);
    if (started != DeviceStatus::Ready)
    {
        return ToRasterStatus(started);
    }
    mSurface = surface;
    mRecordingStatus = RasterStatus::Ready;
    mIsOpen = true;
    return RasterStatus::Ready;
}
void FrameRecording::Record(RasterStatus status) noexcept
{
    LUDUS_ASSERT(mIsOpen == true, "FrameRecording::Record needs an open frame");
    if (mRecordingStatus == RasterStatus::Ready)
    {
        mRecordingStatus = status;
    }
}
RasterStatus FrameRecording::End(SubmissionToken& outCompletion) noexcept
{
    mHasAcceptedSubmission = false;
    if (mIsOpen == false || outCompletion.IsNull() == false)
    {
        return RasterStatus::InvalidState;
    }
    const RasterStatus submitted = EndFrame(mDevice, outCompletion);
    // EndFrame can publish accepted work alongside a failure. Ask the device, as the producer's consumers will.
    mHasAcceptedSubmission = GetStatus(mDevice, outCompletion) != RasterStatus::InvalidHandle;
    // A reservation failure leaves the frame open on its surface; only the device knows which happened.
    FrameInfo frame;
    mIsOpen = GetFrameInfo(mDevice, mSurface, frame) == DeviceStatus::Ready;
    if (mIsOpen == true)
    {
        return submitted;
    }
    return mRecordingStatus != RasterStatus::Ready ? mRecordingStatus : submitted;
}

RasterStatus FrameProducer::PollProducers() const noexcept
{
    for (usize slot = 0; slot < SLOT_COUNT; ++slot)
    {
        if (mHasProducer[slot] == false)
        {
            continue;
        }
        const RasterStatus producer = GetStatus(mDevice, mProducers[slot]);
        if (producer != RasterStatus::Ready)
        {
            return producer;
        }
    }
    return RasterStatus::Ready;
}
RasterStatus FrameProducer::BeginFrame(SurfaceHandle surface, uint32& outSlot) noexcept
{
    if (mRecording.IsOpen() == true)
    {
        return RasterStatus::InvalidState;
    }
    uint32 slot = 1 - mLatest;
    if (mIsLeased[slot] == true || mGenerations[slot] == LAST_GENERATION)
    {
        slot = 1 - slot;
    }
    if (mIsLeased[slot] == true)
    {
        return RasterStatus::NotReady;
    }
    if (mGenerations[slot] == LAST_GENERATION)
    {
        return RasterStatus::IdentityExhausted;
    }
    const RasterStatus started = mRecording.Begin(surface);
    if (started != RasterStatus::Ready)
    {
        return started;
    }
    mIsPublished[slot] = false;
    mProducing = slot;
    outSlot = slot;
    return RasterStatus::Ready;
}
void FrameProducer::Record(RasterStatus status) noexcept
{
    mRecording.Record(status);
}
RasterStatus FrameProducer::EndFrame(SubmissionToken& outCompletion) noexcept
{
    const RasterStatus ended = mRecording.End(outCompletion);
    if (mRecording.HasAcceptedSubmission() == true)
    {
        mProducers[mProducing] = outCompletion;
        mHasProducer[mProducing] = true;
    }
    if (mRecording.IsOpen() == false && ended == RasterStatus::Ready)
    {
        mLatest = mProducing;
        mIsPublished[mProducing] = true;
    }
    return ended;
}
RasterStatus FrameProducer::Acquire(FrameLease& outLease, SubmissionToken& outProducer) noexcept
{
    const uint32 slot = mLatest;
    if (mIsPublished[slot] == false || mIsLeased[slot] == true)
    {
        return RasterStatus::NotReady;
    }
    const RasterStatus completed = GetStatus(mDevice, mProducers[slot]);
    if (completed != RasterStatus::Ready)
    {
        return completed;
    }
    if (mGenerations[slot] == LAST_GENERATION)
    {
        return RasterStatus::IdentityExhausted;
    }
    mIsLeased[slot] = true;
    outLease = { .Slot = slot, .Generation = mGenerations[slot] };
    outProducer = mProducers[slot];
    return RasterStatus::Ready;
}
RasterStatus FrameProducer::Release(FrameLease lease) noexcept
{
    if (lease.Slot >= SLOT_COUNT || mIsLeased[lease.Slot] == false || lease.Generation != mGenerations[lease.Slot])
    {
        return RasterStatus::InvalidHandle;
    }
    mIsLeased[lease.Slot] = false;
    ++mGenerations[lease.Slot];
    return RasterStatus::Ready;
}
} // namespace ludus::graphics::rhi
