#pragma once

// Producer side of hosted recording: a game rendering into a hosted window measures
// its own physical GPU passes when the editor asks, and returns them over the window's
// recording channel. The editor owns admission, coverage and export. The contract is
// owned by docs/architecture/editor-gui-systems.md, "Hosted recording".

#include <ludus/foundation/base/core.h>

#include <ludus/foundation/containers/borrowed_view.hpp>
#include <ludus/foundation/containers/static_array.hpp>
#include <ludus/graphics/rhi/device.h>
#include <ludus/graphics/rhi/gpu_scope.h>
#include <ludus/graphics/rhi/measurement.h>
#include <ludus/platform/hosted_recording.h>
#include <ludus/platform/native_window.h>

namespace ludus::graphics::rhi
{
static_assert(GPU_SCOPE_NAME_BYTES - 1 <= platform::HOSTED_RECORDING_PASS_NAME_BYTES,
              "every GPU scope name fits a hosted recording sample");

/// Frames a stopping recording keeps polling its last measured batch before it reports
/// that batch as unresolved.
inline constexpr foundation::uint32 HOSTED_RECORDING_STOP_FRAMES = 8;

/// @brief Measures a hosted game's physical GPU passes for an editor recording.
///
/// The recorder answers the editor's Start and Stop requests and owns the device's
/// per-pass timing batch (ArmGpuPassTimings) while a recording runs, so the game must
/// not arm pass timing itself during one. It never waits for the GPU: one batch is in
/// flight at a time, and a submitted frame that cannot be armed because the previous
/// batch has not resolved is counted as skipped rather than measured. Each resolved
/// pass inside a named GpuScope becomes one sample carrying the scope's name and the
/// recorder's frame number; frames count the game's submitted frames from one.
///
/// A device without per-pass timestamp counters (ArmGpuPassTimings returns Unsupported)
/// still records: Start is acknowledged with IsPassTimingSupported false, and the
/// recorder then owns the device's frame probe (ArmGpuTiming) instead, sending one
/// whole-frame sample (HostedRecordingSample::IsFrame) per measured frame. Neither
/// probe is ever substituted silently; the acknowledgement names which one the
/// recording uses.
///
/// Recording messages travel without taking the window's surface send room, so a
/// recording never delays a Present or holds a presented image; a sample that does not
/// fit is dropped and counted. Ticks are the game's foundation::time::NowTicks clock
/// and stay process-local.
///
/// Call order per frame, on the thread that pumps the window and owns the device:
/// pump the window, BeginFrame(device), the game's rhi::BeginFrame ... rhi::EndFrame,
/// then EndFrame(isSubmitted). Not synchronized. The device needs
/// DeviceDescription::EnableGpuTiming for Start to succeed; otherwise, or when the
/// device can measure neither passes nor frames, every Start is answered Unsupported
/// and rendering is unaffected.
class HostedGpuRecorder final
{
public:
    /// Construct a detached recorder. Does not allocate.
    HostedGpuRecorder() noexcept = default;
    /// Detach from the window.
    ~HostedGpuRecorder() noexcept;
    /// Not copyable: one recorder answers one window's requests.
    HostedGpuRecorder(const HostedGpuRecorder&) = delete;
    /// Not copyable: one recorder answers one window's requests.
    HostedGpuRecorder& operator=(const HostedGpuRecorder&) = delete;
    /// Not movable: the window's channel is borrowed for the recorder's lifetime.
    HostedGpuRecorder(HostedGpuRecorder&&) = delete;
    /// Not movable: the window's channel is borrowed for the recorder's lifetime.
    HostedGpuRecorder& operator=(HostedGpuRecorder&&) = delete;

    /// @brief Answer recording requests for a hosted window.
    /// @param window The window's native information; its HostedRecordingOrNull is borrowed
    /// until Detach() or destruction, so the window must outlive the recorder's attachment.
    /// @param producerName UTF-8 name reported with every acknowledgement, at most
    /// platform::HOSTED_RECORDING_PRODUCER_NAME_BYTES bytes without zero bytes; may be empty.
    /// @return false, leaving the recorder detached, for a window that is not hosted, an
    /// invalid name or a recorder that is already attached.
    [[nodiscard]] bool Attach(const platform::NativeWindowInfo& window,
                              foundation::BorrowedView<const char> producerName) noexcept;

    /// @brief Stop answering requests; the window answers later Starts with Unsupported.
    ///
    /// A recording in progress ends without a Stop acknowledgement, which the editor reports
    /// as incomplete. Safe when detached.
    void Detach() noexcept;

    /// @brief Answer requests, deliver a resolved batch and arm the next frame.
    ///
    /// Call between frames, after pumping the window and before the game's BeginFrame.
    /// Nonblocking; failures to measure or send are counted in the recording's coverage and
    /// never affect rendering. Does nothing while detached.
    /// @param device The device that renders into the attached window.
    void BeginFrame(DeviceHandle device) noexcept;

    /// @brief Report the outcome of the frame begun after BeginFrame().
    /// @param isSubmitted True when the game's EndFrame submitted the frame; false when the
    /// frame was skipped, which keeps an armed batch for the next submitted frame.
    void EndFrame(bool isSubmitted) noexcept;

    /// True while a recording runs or is finishing its Stop.
    [[nodiscard]] bool IsRecording() const noexcept;

    /// Frames submitted since Attach(), the numbering samples and acknowledgements use.
    [[nodiscard]] LUDUS_INLINE foundation::uint64 GetFrame() const noexcept
    {
        return mFrame;
    }

private:
    enum class Phase : foundation::uint8
    {
        Idle,
        Recording,
        Stopping,
    };
    /// An acknowledgement waiting for room in the window's recording send queue.
    struct PendingAcknowledgement final
    {
        /// True while the entry holds an unsent acknowledgement.
        bool IsValid = false;
        /// The answer to send, in request order.
        platform::HostedRecordingAcknowledgement Acknowledgement{};
    };
    void answer(const platform::HostedRecordingRequest& request,
                platform::HostedRecordingResult result,
                foundation::uint64 frame) noexcept;
    void flushAcknowledgements() noexcept;
    void startRecording(DeviceHandle device, const platform::HostedRecordingRequest& request) noexcept;
    void collectBatch(DeviceHandle device) noexcept;
    void deliverBatch(const GpuPassTimings& batch) noexcept;
    void deliverFrame(const GpuTiming& timing) noexcept;
    void sendSample(const platform::HostedRecordingSample& sample) noexcept;
    [[nodiscard]] RasterStatus armBatch(DeviceHandle device) const noexcept;
    void finishStop() noexcept;
    [[nodiscard]] bool hasBatch() const noexcept;

    platform::HostedRecordingChannel* mChannelOrNull = nullptr;
    platform::HostedProducerName mProducerName{};
    Phase mPhase = Phase::Idle;
    foundation::uint64 mFrame = 0;
    foundation::uint64 mRecording = 0;
    foundation::uint64 mStopRequest = 0;
    foundation::uint64 mStopFrame = 0;
    foundation::uint32 mStopFramesLeft = 0;
    platform::HostedRecordingCoverage mCoverage{};
    /// Armed and waiting for a submitted frame.
    bool mIsBatchArmed = false;
    /// Batches measure whole frames (ArmGpuTiming) because the device has no per-pass
    /// counters; set when a Start arms a fresh batch and kept while one is in flight.
    bool mIsFrameTiming = false;
    /// Submitted frame the in-flight batch measures; zero without one.
    foundation::uint64 mBatchFrame = 0;
    /// Recording the batch was armed for; zero for a batch armed before a Stop that no
    /// recording claims.
    foundation::uint64 mBatchRecording = 0;
    foundation::uint64 mBatchTicks = 0;
    foundation::StaticArray<PendingAcknowledgement, platform::MAX_HOSTED_RECORDING_REQUESTS> mPending{};
};
} // namespace ludus::graphics::rhi
