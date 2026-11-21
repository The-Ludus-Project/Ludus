#include "internal/lifecycle.h"
#include "reference_raster.h"

#include <ludus/graphics/rhi/frame_producer.h>
#include <ludus/graphics/rhi/lifetime.h>

#include <catch2/catch_test_macros.hpp>

namespace ludus::graphics::rhi::backend
{
extern foundation::uint32 PendingToken;
extern StartupError ImmediateError;
extern FrameStatus NextFrame;
extern Backend ActiveKind;
} // namespace ludus::graphics::rhi::backend
using namespace ludus::foundation;
using namespace ludus::graphics::rhi;
namespace
{
/// Slot value no producer writes, so a test can see that a refusal left the output untouched.
constexpr uint32 NO_SLOT = static_cast<uint32>(FrameProducer::SLOT_COUNT);
/// Ready reference-backend device with portable raster, as the raster tests use.
struct ProducerSession final
{
    DeviceHandle Device;
    SurfaceHandle Surface;
    ProducerSession()
    {
        Shutdown();
        reference::Reset();
        backend::ImmediateError = StartupError::None;
        backend::NextFrame = FrameStatus::Ready;
        backend::ActiveKind = Backend::WebGPU;
        REQUIRE(CreateDevice({}, {}, {}, Device, Surface) == DeviceStatus::Pending);
        internal::Complete(backend::PendingToken, StartupError::None, {4096, 16384});
    }
    ~ProducerSession() noexcept
    {
        backend::NextFrame = FrameStatus::Ready;
        Shutdown();
    }
    ProducerSession(const ProducerSession&) = delete;
    ProducerSession& operator=(const ProducerSession&) = delete;
    /// Whether the device reports a frame open on this session's surface.
    [[nodiscard]] bool IsDeviceFrameOpen() const noexcept
    {
        FrameInfo frame;
        return GetFrameInfo(Device, Surface, frame) == DeviceStatus::Ready;
    }
};
/// Record one Ready frame on producer, complete it and return the slot it wrote.
uint32 ProduceCompletedFrame(const ProducerSession& session, FrameProducer& inoutProducer)
{
    uint32 slot = NO_SLOT;
    REQUIRE(inoutProducer.BeginFrame(session.Surface, slot) == RasterStatus::Ready);
    REQUIRE(slot < FrameProducer::SLOT_COUNT);
    inoutProducer.Record(RasterStatus::Ready);
    SubmissionToken completion;
    REQUIRE(inoutProducer.EndFrame(completion) == RasterStatus::Ready);
    REQUIRE(completion.IsNull() == false);
    reference::Completed = reference::Submitted;
    return slot;
}
} // namespace

TEST_CASE("ToRasterStatus keeps each device status's meaning", "[rhi][frame-producer]")
{
    CHECK(ToRasterStatus(DeviceStatus::Ready) == RasterStatus::Ready);
    // Startup in flight and a skipped target both mean "retry later", never an output awaiting validation.
    CHECK(ToRasterStatus(DeviceStatus::Pending) == RasterStatus::NotReady);
    CHECK(ToRasterStatus(DeviceStatus::Skipped) == RasterStatus::NotReady);
    CHECK(ToRasterStatus(DeviceStatus::NotReady) == RasterStatus::NotReady);
    CHECK(ToRasterStatus(DeviceStatus::InvalidHandle) == RasterStatus::InvalidHandle);
    CHECK(ToRasterStatus(DeviceStatus::InvalidState) == RasterStatus::InvalidState);
    CHECK(ToRasterStatus(DeviceStatus::InvalidDescription) == RasterStatus::InvalidDescription);
    CHECK(ToRasterStatus(DeviceStatus::Unsupported) == RasterStatus::Unsupported);
    CHECK(ToRasterStatus(DeviceStatus::Failed) == RasterStatus::Failed);
    CHECK(ToRasterStatus(DeviceStatus::DeviceLost) == RasterStatus::DeviceLost);
    CHECK(ToRasterStatus(DeviceStatus::IdentityExhausted) == RasterStatus::IdentityExhausted);
    // The device survives a lost surface; the surface itself must be recreated.
    CHECK(ToRasterStatus(DeviceStatus::SurfaceLost) == RasterStatus::Failed);
    CHECK(ToRasterStatus(DeviceStatus::CapacityExceeded) == RasterStatus::CapacityExceeded);
    constexpr uint8 OUT_OF_RANGE = 0xFF;
    CHECK(ToRasterStatus(static_cast<DeviceStatus>(OUT_OF_RANGE)) == RasterStatus::InvalidDescription);
}

TEST_CASE("Frame recording admits one owned frame and reports refusals without opening one", "[rhi][frame-producer]")
{
    ProducerSession session;
    FrameRecording unbound;
    CHECK(unbound.Begin(session.Surface) == RasterStatus::InvalidHandle);
    CHECK(unbound.IsOpen() == false);

    FrameRecording recording{session.Device};
    SubmissionToken completion;
    CHECK(recording.End(completion) == RasterStatus::InvalidState);
    CHECK(completion.IsNull() == true);
    CHECK(recording.HasAcceptedSubmission() == false);

    backend::NextFrame = FrameStatus::Skipped;
    CHECK(recording.Begin(session.Surface) == RasterStatus::NotReady);
    CHECK(recording.IsOpen() == false);
    CHECK(session.IsDeviceFrameOpen() == false);
    backend::NextFrame = FrameStatus::Ready;

    REQUIRE(recording.Begin(session.Surface) == RasterStatus::Ready);
    CHECK(recording.IsOpen() == true);
    CHECK(recording.Begin(session.Surface) == RasterStatus::InvalidState);
    CHECK(recording.IsOpen() == true);
    // The first failure is kept; a later Ready step cannot hide it.
    recording.Record(RasterStatus::Ready);
    recording.Record(RasterStatus::Unsupported);
    recording.Record(RasterStatus::Ready);
    CHECK(recording.GetRecordingStatus() == RasterStatus::Unsupported);
    REQUIRE(recording.End(completion) == RasterStatus::Unsupported);
    CHECK(recording.IsOpen() == false);
    CHECK(session.IsDeviceFrameOpen() == false);
    // The frame was submitted, so its token names accepted work despite the recording failure.
    CHECK(recording.HasAcceptedSubmission() == true);
    CHECK(GetStatus(session.Device, completion) == RasterStatus::Pending);

    // A refused Begin preserves the last recording failure; a Ready Begin restarts it.
    backend::NextFrame = FrameStatus::Skipped;
    CHECK(recording.Begin(session.Surface) == RasterStatus::NotReady);
    CHECK(recording.GetRecordingStatus() == RasterStatus::Unsupported);
    backend::NextFrame = FrameStatus::Ready;
    REQUIRE(recording.Begin(session.Surface) == RasterStatus::Ready);
    CHECK(recording.GetRecordingStatus() == RasterStatus::Ready);
    SubmissionToken occupied = completion;
    CHECK(recording.End(occupied) == RasterStatus::InvalidState);
    CHECK(recording.IsOpen() == true);
    CHECK(session.IsDeviceFrameOpen() == true);
    SubmissionToken second;
    REQUIRE(recording.End(second) == RasterStatus::Ready);
    CHECK(recording.HasAcceptedSubmission() == true);
}

TEST_CASE("Frame recording retries a frame that EndFrame could not reserve", "[rhi][frame-producer]")
{
    ProducerSession session;
    FrameRecording recording{session.Device};
    REQUIRE(recording.Begin(session.Surface) == RasterStatus::Ready);
    recording.Record(RasterStatus::Ready);
    reference::Submission = RasterStatus::CapacityExceeded;
    SubmissionToken completion;
    CHECK(recording.End(completion) == RasterStatus::CapacityExceeded);
    // Reservation failure leaves the frame open and owned, publishing nothing.
    CHECK(recording.IsOpen() == true);
    CHECK(session.IsDeviceFrameOpen() == true);
    CHECK(completion.IsNull() == true);
    CHECK(recording.HasAcceptedSubmission() == false);
    reference::Submission = RasterStatus::Ready;
    REQUIRE(recording.End(completion) == RasterStatus::Ready);
    CHECK(recording.IsOpen() == false);
    CHECK(session.IsDeviceFrameOpen() == false);
    CHECK(recording.HasAcceptedSubmission() == true);
}

TEST_CASE("Frame producer alternates slots, applies lease backpressure and rejects stale leases",
          "[rhi][frame-producer]")
{
    ProducerSession session;
    FrameProducer producer{session.Device};
    FrameLease lease;
    SubmissionToken image;
    CHECK(producer.Acquire(lease, image) == RasterStatus::NotReady);
    CHECK(producer.PollProducers() == RasterStatus::Ready);

    uint32 first = NO_SLOT;
    REQUIRE(producer.BeginFrame(session.Surface, first) == RasterStatus::Ready);
    CHECK(producer.IsFrameOpen() == true);
    uint32 nested = NO_SLOT;
    CHECK(producer.BeginFrame(session.Surface, nested) == RasterStatus::InvalidState);
    CHECK(nested == NO_SLOT);
    SubmissionToken completion;
    REQUIRE(producer.EndFrame(completion) == RasterStatus::Ready);
    CHECK(producer.IsFrameOpen() == false);
    // Published, but its producer has not completed: Acquire and Poll report the pending work.
    CHECK(producer.PollProducers() == RasterStatus::Pending);
    CHECK(producer.Acquire(lease, image) == RasterStatus::Pending);
    CHECK(lease.Generation == 0);
    reference::Completed = reference::Submitted;
    CHECK(producer.PollProducers() == RasterStatus::Ready);
    REQUIRE(producer.Acquire(lease, image) == RasterStatus::Ready);
    CHECK(lease.Slot == first);
    CHECK(lease.Generation != 0);
    CHECK(GetStatus(session.Device, image) == RasterStatus::Ready);
    FrameLease again;
    CHECK(producer.Acquire(again, image) == RasterStatus::NotReady);
    CHECK(again.Generation == 0);

    // The next frame writes the other slot while the first is leased.
    const uint32 second = ProduceCompletedFrame(session, producer);
    CHECK(second != first);
    FrameLease secondLease;
    REQUIRE(producer.Acquire(secondLease, image) == RasterStatus::Ready);
    CHECK(secondLease.Slot == second);
    // Both slots leased: backpressure opens no frame and preserves the output slot.
    uint32 blocked = NO_SLOT;
    CHECK(producer.BeginFrame(session.Surface, blocked) == RasterStatus::NotReady);
    CHECK(blocked == NO_SLOT);
    CHECK(producer.IsFrameOpen() == false);
    CHECK(session.IsDeviceFrameOpen() == false);

    FrameLease stale = lease;
    ++stale.Generation;
    CHECK(producer.Release(stale) == RasterStatus::InvalidHandle);
    CHECK(producer.Release({ .Slot = NO_SLOT, .Generation = lease.Generation }) == RasterStatus::InvalidHandle);
    REQUIRE(producer.Release(lease) == RasterStatus::Ready);
    CHECK(producer.Release(lease) == RasterStatus::InvalidHandle);

    // Only the first slot is free, so the next frame rewrites it.
    uint32 third = NO_SLOT;
    REQUIRE(producer.BeginFrame(session.Surface, third) == RasterStatus::Ready);
    CHECK(third == first);
    SubmissionToken thirdCompletion;
    REQUIRE(producer.EndFrame(thirdCompletion) == RasterStatus::Ready);
    reference::Completed = reference::Submitted;
    FrameLease thirdLease;
    REQUIRE(producer.Acquire(thirdLease, image) == RasterStatus::Ready);
    CHECK(thirdLease.Slot == first);
    // The rewritten slot carries a new generation, so the released lease stays stale.
    CHECK(thirdLease.Generation != lease.Generation);
    CHECK(producer.Release(lease) == RasterStatus::InvalidHandle);
    REQUIRE(producer.Release(thirdLease) == RasterStatus::Ready);
    REQUIRE(producer.Release(secondLease) == RasterStatus::Ready);
}

TEST_CASE("Frame producer keeps the published image when a frame fails or stays open", "[rhi][frame-producer]")
{
    ProducerSession session;
    FrameProducer producer{session.Device};
    const uint32 published = ProduceCompletedFrame(session, producer);

    // A failed frame writes the other slot, keeps its producer and never republishes.
    uint32 failing = NO_SLOT;
    REQUIRE(producer.BeginFrame(session.Surface, failing) == RasterStatus::Ready);
    CHECK(failing != published);
    producer.Record(RasterStatus::CapacityExceeded);
    reference::Submission = RasterStatus::CapacityExceeded;
    SubmissionToken partial;
    CHECK(producer.EndFrame(partial) == RasterStatus::CapacityExceeded);
    CHECK(producer.IsFrameOpen() == true);
    CHECK(session.IsDeviceFrameOpen() == true);
    CHECK(partial.IsNull() == true);
    reference::Submission = RasterStatus::Ready;
    // The retry closes exactly the owned frame and still reports the recording failure.
    CHECK(producer.EndFrame(partial) == RasterStatus::CapacityExceeded);
    CHECK(producer.IsFrameOpen() == false);
    CHECK(session.IsDeviceFrameOpen() == false);
    CHECK(partial.IsNull() == false);
    CHECK(producer.GetRecordingStatus() == RasterStatus::CapacityExceeded);
    // The accepted partial work is still tracked as that slot's producer.
    CHECK(producer.PollProducers() == RasterStatus::Pending);
    reference::Completed = reference::Submitted;
    CHECK(producer.PollProducers() == RasterStatus::Ready);
    FrameLease lease;
    SubmissionToken image;
    REQUIRE(producer.Acquire(lease, image) == RasterStatus::Ready);
    CHECK(lease.Slot == published);
    REQUIRE(producer.Release(lease) == RasterStatus::Ready);

    // A recovered frame publishes the slot it wrote as the latest image.
    const uint32 recovered = ProduceCompletedFrame(session, producer);
    CHECK(recovered == failing);
    REQUIRE(producer.Acquire(lease, image) == RasterStatus::Ready);
    CHECK(lease.Slot == recovered);
    REQUIRE(producer.Release(lease) == RasterStatus::Ready);
}
