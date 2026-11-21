#pragma once

// Frame producer helper: the frame and output bookkeeping that renderer and editor entry points
// otherwise repeat above the RHI when they open their own frames and hand out GPU images. See
// docs/architecture/engine-loop.md, finding 7 ("Frame bookkeeping is repeated above the RHI").

#include <ludus/foundation/base/compiler.h>
#include <ludus/foundation/base/types.h>

#include <ludus/foundation/containers/static_array.hpp>
#include <ludus/graphics/rhi/device.h>
#include <ludus/graphics/rhi/lifetime.h>
#include <ludus/graphics/rhi/raster.h>

namespace ludus::graphics::rhi
{
/// @brief Translate an explicit-device status into the RasterStatus vocabulary of frame and resource work.
///
/// This is the one DeviceStatus to RasterStatus mapping; entry points that report RasterStatus use it
/// instead of their own switch. Every status with a RasterStatus of the same name maps to it. The
/// others map as follows:
/// - Pending (device startup in flight) and Skipped (zero-size target, no frame opened) map to
///   NotReady: retry once the device or target is ready.
/// - SurfaceLost maps to Failed. The device stays usable; destroy and recreate the surface.
/// - A value outside the enumeration maps to InvalidDescription.
///
/// Pure function: no RHI call, no thread affinity.
/// @param status Result of a device, surface or frame operation.
/// @return The translated status. It is never Pending or OutOfMemory, so it never claims that an
/// output awaits validation or that an allocation failed.
[[nodiscard]] RasterStatus ToRasterStatus(DeviceStatus status) noexcept;

/// @brief One producer's ownership of a frame that it opens, records and closes on one device.
///
/// rhi::EndFrame leaves the frame open when it cannot reserve the submission. The RHI allows one
/// open frame per device, so the producer that opened the frame keeps recording ownership. It must
/// retry End before any other frame, setup or reset call on the device. A FrameRecording holds that
/// ownership, the surface the frame was acquired on and the first recording failure. A retry
/// therefore closes exactly the owned frame and still reports a recording failure, never a false
/// success.
///
/// Use it directly for one output, as a graph or shadow producer does. FrameProducer adds two
/// leased output slots on top of it.
///
/// Ownership and lifetime: the device is borrowed and must outlive every frame operation. This type
/// owns no GPU resources. Construction, copy and destruction make no RHI call. Copies duplicate the
/// bookkeeping, so keep one live copy per producer; assign a fresh instance to start over.
///
/// Thread affinity: RHI owner thread only, like the device. Not synchronized.
class FrameRecording final
{
public:
    /// Construct a recording bound to no device. Begin then returns the RHI's InvalidHandle.
    FrameRecording() noexcept = default;
    /// Bind the device whose frames this recording opens. Makes no RHI call.
    /// @param device Borrowed device that must outlive every later frame operation.
    LUDUS_INLINE explicit FrameRecording(DeviceHandle device) noexcept : mDevice(device) {}

    /// Open a frame on surface through rhi::BeginFrame and take recording ownership of it.
    /// @param surface Borrowed surface of the bound device whose frame target is already set.
    /// @return Ready once the frame is open; the recording status restarts at Ready. InvalidState,
    /// with no RHI call, while this recording already owns an open frame. Otherwise the
    /// ToRasterStatus translation of the refusal, for example NotReady for a skipped zero-size
    /// target. A refusal opens no frame and preserves the previous recording status.
    [[nodiscard]] RasterStatus Begin(SurfaceHandle surface) noexcept;
    /// Record the result of one encoding step of the open frame. The first non-Ready result is
    /// kept and later results are ignored, so a later step cannot hide an earlier failure.
    /// @param status Result of the step, for example a DrawView or overlay draw.
    /// @pre IsOpen() is true.
    void Record(RasterStatus status) noexcept;
    /// First failure recorded since the last Ready Begin, or Ready. It persists while the frame
    /// stays open and after End, until the next Ready Begin.
    [[nodiscard]] LUDUS_INLINE RasterStatus GetRecordingStatus() const noexcept
    {
        return mRecordingStatus;
    }
    /// Close the owned frame through rhi::EndFrame, or retry a close that left it open.
    /// @param outCompletion Null token. rhi::EndFrame writes the accepted submission into it;
    /// HasAcceptedSubmission then reports that it names GPU work.
    /// @return While the frame stays open because EndFrame could not reserve the submission, that
    /// EndFrame status. Retry End with a null token before any other frame call. Once the frame is
    /// closed, the recording status if it is a failure, otherwise the EndFrame status. Ready
    /// therefore means that every recorded step succeeded and was submitted. InvalidState, with
    /// no RHI call and outCompletion preserved, when no frame is owned or outCompletion is not null.
    [[nodiscard]] RasterStatus End(SubmissionToken& outCompletion) noexcept;
    /// Whether this recording owns an open frame: from a Ready Begin until End closes it.
    [[nodiscard]] LUDUS_INLINE bool IsOpen() const noexcept
    {
        return mIsOpen;
    }
    /// Whether the last End wrote a token that the device recognises as accepted GPU work. Keep that
    /// token as the producer of what the frame wrote, even when End reports a recording failure.
    /// False before the first End and after an End that published nothing.
    [[nodiscard]] LUDUS_INLINE bool HasAcceptedSubmission() const noexcept
    {
        return mHasAcceptedSubmission;
    }

private:
    DeviceHandle mDevice{};
    SurfaceHandle mSurface{};
    RasterStatus mRecordingStatus = RasterStatus::Ready;
    bool mIsOpen = false;
    bool mHasAcceptedSubmission = false;
};

/// Identity of one leased FrameProducer output slot. An entry point wraps it in its own opaque frame
/// handle, together with its own owner identity, and passes it back to Release.
struct FrameLease final
{
    /// Leased slot, below FrameProducer::SLOT_COUNT.
    foundation::uint32 Slot = 0;
    /// Slot generation when it was leased. Zero names no lease. Release advances the slot's
    /// generation, so a stale copy of a returned lease is rejected.
    foundation::uint64 Generation = 0;
};

/// @brief Two output slots written by frames that this producer records, each leased to a consumer
/// at a time.
///
/// Double buffering lets a consumer sample the latest completed image while the next frame writes the
/// other slot. Each slot has a non-wrapping generation, a lease flag, the producer token of the
/// latest frame that wrote it and whether it holds a published image. The entry point owns the
/// textures or other outputs that slot indices name. A lease records consumer ownership only and
/// retains no resource, so the entry point keeps a leased slot's output alive until Release.
///
/// Publication: a slot's image is published only when a frame that wrote it closes with Ready, under
/// FrameRecording::End's rule that every recorded step succeeded. Opening a frame on a slot withdraws
/// that slot's earlier image first, so a failed frame never republishes stale content. The other
/// slot's image stays published. A leased slot is never written until its lease is released.
///
/// The frame calls follow FrameRecording, including the retry of a frame that EndFrame left open.
/// Ownership, lifetime and thread affinity are as for FrameRecording: the device is borrowed, no
/// GPU resources are owned, no RHI call is made on construction, copy or destruction, and all calls
/// run on the RHI owner thread.
class FrameProducer final
{
public:
    /// Number of output slots: one being written while a consumer holds the other.
    static constexpr foundation::usize SLOT_COUNT = 2;

    /// Construct a producer bound to no device. BeginFrame then returns the RHI's InvalidHandle.
    FrameProducer() noexcept = default;
    /// Bind the device whose frames write the slots. Makes no RHI call.
    /// @param device Borrowed device that must outlive every later frame or completion call.
    LUDUS_INLINE explicit FrameProducer(DeviceHandle device) noexcept : mRecording(device), mDevice(device) {}

    /// Poll the latest producer of every slot without waiting.
    /// @return Ready when every slot written so far has completed its producer, or no slot was
    /// written. Otherwise the first incomplete or failed rhi::GetStatus result, such as Pending.
    [[nodiscard]] RasterStatus PollProducers() const noexcept;
    /// Choose an unleased slot and open a frame on surface to write it. The slot that does not hold
    /// the latest image is preferred, so that image stays available; otherwise the latest slot is
    /// rewritten.
    /// @param surface Borrowed surface of the bound device whose frame target is already set.
    /// @param outSlot Receives the slot to write, only on Ready.
    /// @return Ready once the frame is open; the slot's earlier image is withdrawn. NotReady when
    /// both slots are leased, which applies consumer backpressure. IdentityExhausted when an
    /// unleased slot's generation cannot advance. InvalidState while a frame is already open.
    /// Otherwise FrameRecording::Begin's refusal. Every refusal preserves both slots and outSlot.
    [[nodiscard]] RasterStatus BeginFrame(SurfaceHandle surface, foundation::uint32& outSlot) noexcept;
    /// Record the result of one encoding step; see FrameRecording::Record.
    /// @param status Result of the step.
    /// @pre IsFrameOpen() is true.
    void Record(RasterStatus status) noexcept;
    /// First recording failure of the latest frame, or Ready; see FrameRecording::GetRecordingStatus.
    [[nodiscard]] LUDUS_INLINE RasterStatus GetRecordingStatus() const noexcept
    {
        return mRecording.GetRecordingStatus();
    }
    /// Close the open frame, or retry a close that left it open; see FrameRecording::End.
    /// An accepted submission becomes the producer of the frame's slot even when the frame failed.
    /// A closed Ready frame publishes its slot as the latest image.
    /// @param outCompletion Null token receiving the accepted submission.
    /// @return FrameRecording::End's status. While IsFrameOpen() stays true, retry before any other
    /// frame, setup or reset call on the device.
    [[nodiscard]] RasterStatus EndFrame(SubmissionToken& outCompletion) noexcept;
    /// Whether a frame opened by BeginFrame is still open, including after an EndFrame that could
    /// not reserve its submission.
    [[nodiscard]] LUDUS_INLINE bool IsFrameOpen() const noexcept
    {
        return mRecording.IsOpen();
    }
    /// Lease the latest published image once its producer has completed.
    /// @param outLease Receives the slot and its current generation, only on Ready.
    /// @param outProducer Receives the completed producer token, only on Ready.
    /// @return Ready once leased; the slot is not written again until Release. NotReady when no
    /// image is published or the latest one is already leased. The producer's rhi::GetStatus result
    /// while it is incomplete or failed. IdentityExhausted when the slot's generation cannot
    /// advance. Every refusal preserves both outputs.
    [[nodiscard]] RasterStatus Acquire(FrameLease& outLease, SubmissionToken& outProducer) noexcept;
    /// Return a lease after every consumer of its image, including foreign GPU work, has finished.
    /// @param lease A lease that Acquire returned and that has not been released.
    /// @return Ready once returned; the slot's generation advances. InvalidHandle for an
    /// out-of-range slot, a slot that is not leased or a stale generation, which changes nothing.
    [[nodiscard]] RasterStatus Release(FrameLease lease) noexcept;

private:
    /// Generation of a slot that has never been leased. Zero stays reserved for "no lease".
    static constexpr foundation::uint64 FIRST_GENERATION = 1;
    static_assert(SLOT_COUNT == 2, "Slot selection alternates between exactly two slots.");
    FrameRecording mRecording;
    DeviceHandle mDevice{};
    foundation::StaticArray<SubmissionToken, SLOT_COUNT> mProducers{};
    foundation::StaticArray<foundation::uint64, SLOT_COUNT> mGenerations{FIRST_GENERATION, FIRST_GENERATION};
    foundation::StaticArray<bool, SLOT_COUNT> mIsLeased{};
    foundation::StaticArray<bool, SLOT_COUNT> mIsPublished{};
    foundation::StaticArray<bool, SLOT_COUNT> mHasProducer{};
    foundation::uint32 mLatest = 0;
    foundation::uint32 mProducing = 0;
};
} // namespace ludus::graphics::rhi
