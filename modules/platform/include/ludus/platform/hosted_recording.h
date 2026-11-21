#pragma once

// Hosted recording transport: the editor asks a hosted game to record its own GPU
// work, and the game returns acknowledgements and timing samples over the same
// hosted-window channel as its frames. Platform moves bounded messages; the game's
// RHI recorder measures them and the editor owns admission and export. The contract
// is owned by docs/architecture/editor-gui-systems.md, "Hosted recording".

#include <ludus/foundation/base/core.h>

#include <ludus/foundation/containers/static_array.hpp>
#include <ludus/platform/hosted_surface.h>

namespace ludus::platform
{
/// Largest pass name a sample carries, in UTF-8 bytes without a terminator.
inline constexpr foundation::usize HOSTED_RECORDING_PASS_NAME_BYTES = 127;
/// Largest producer name an acknowledgement carries, in UTF-8 bytes without a terminator.
inline constexpr foundation::usize HOSTED_RECORDING_PRODUCER_NAME_BYTES = 63;
/// Recording requests the endpoint may have sent and not yet seen acknowledged.
inline constexpr foundation::usize MAX_HOSTED_RECORDING_REQUESTS = 4;

/// Terminated pass name storage; bytes after the terminator are zero.
using HostedPassName = foundation::StaticArray<char, HOSTED_RECORDING_PASS_NAME_BYTES + 1>;
/// Terminated producer name storage; bytes after the terminator are zero.
using HostedProducerName = foundation::StaticArray<char, HOSTED_RECORDING_PRODUCER_NAME_BYTES + 1>;

/// @brief What an editor asks of the game's recorder.
enum class HostedRecordingRequestKind : foundation::uint8
{
    /// Begin measuring the frames submitted after the acknowledgement.
    Start = 0,
    /// Stop measuring, deliver what has resolved and report coverage.
    Stop = 1,
};

/// @brief One recording request.
///
/// Recording identifies one editor recording and is never reused by an editor
/// process, so a sample of an earlier recording is recognizable even after the game
/// restarts its frame numbers. Request numbers increase strictly within a session.
struct HostedRecordingRequest final
{
    /// Start or Stop.
    HostedRecordingRequestKind Kind = HostedRecordingRequestKind::Start;
    /// Nonzero editor recording identity.
    foundation::uint64 Recording = 0;
    /// Nonzero request number, strictly increasing within one hosted-window session.
    foundation::uint64 Request = 0;
};

/// @brief How the game's recorder answered a request.
enum class HostedRecordingResult : foundation::uint8
{
    /// Start: recording from Frame. Stop: recording ended after Frame.
    Ok = 0,
    /// The game has no recorder attached to its window, or its device cannot measure GPU time.
    Unsupported = 1,
    /// Start: another recording is active in the game.
    Busy = 2,
    /// Stop: the request names a recording the game is not running.
    UnknownRecording = 3,
};

/// @brief What the game's recorder measured during one recording, reported with its Stop.
///
/// Counts are frames or samples, never durations, and missing work is never reported
/// as a zero-duration sample.
struct HostedRecordingCoverage final
{
    /// Frames the game submitted while recording.
    foundation::uint64 SubmittedFrames = 0;
    /// Submitted frames whose passes were armed for measurement.
    foundation::uint64 MeasuredFrames = 0;
    /// Submitted frames not measured because the previous batch had not resolved.
    foundation::uint64 SkippedFrames = 0;
    /// Samples the channel accepted.
    foundation::uint64 DeliveredSamples = 0;
    /// Resolved samples the channel could not accept without delaying the game's frames.
    foundation::uint64 DroppedSamples = 0;
    /// Resolved passes with a disjoint or failed native interval.
    foundation::uint64 InvalidSamples = 0;
    /// Passes the native query batch had no room for.
    foundation::uint64 QueryDroppedPasses = 0;
    /// Measured frames whose batch had not resolved when the recording stopped.
    foundation::uint64 UnresolvedBatches = 0;
};

/// @brief The game's answer to one HostedRecordingRequest.
struct HostedRecordingAcknowledgement final
{
    /// The answered request's kind.
    HostedRecordingRequestKind Kind = HostedRecordingRequestKind::Start;
    /// Outcome of the request.
    HostedRecordingResult Result = HostedRecordingResult::Ok;
    /// The answered request's recording.
    foundation::uint64 Recording = 0;
    /// The answered request's number.
    foundation::uint64 Request = 0;
    /// Start: the first frame the recording covers. Stop: the last covered frame, or zero.
    /// Frames are the game recorder's count of submitted frames, starting at one.
    foundation::uint64 Frame = 0;
    /// The game's monotonic clock when it answered, in nanoseconds. Process-local:
    /// comparable with the game's sample ticks, not with the editor's clock.
    foundation::uint64 Ticks = 0;
    /// Stop only: what the recording measured. Zero for Start.
    HostedRecordingCoverage Coverage{};
    /// @brief Start with Ok only: the game's device armed per-pass timestamp counters.
    ///
    /// True when the recording's samples measure the game's named passes. False when
    /// the device has no per-pass counters, so every sample is a whole-frame sample
    /// (HostedRecordingSample::IsFrame), and for every other kind and result.
    bool IsPassTimingSupported = false;
    /// UTF-8 name the game gives its recorder, possibly empty.
    HostedProducerName ProducerName{};
};

/// @brief Native operation class of a sampled pass.
enum class HostedPassKind : foundation::uint8
{
    /// Raster pass.
    Raster = 0,
    /// Compute encoder or dispatch.
    Compute = 1,
    /// Native ray-stage pipeline.
    RayTracing = 2,
};

/// @brief One measured physical pass, or one whole submitted frame, of one game frame.
///
/// A pass sample names its scope. A frame sample (IsFrame) measures the device
/// interval of the whole submitted frame, from a recording whose Start acknowledgement
/// reported no per-pass counters; it has a zero ScopeIdentity and ScopePassIndex, one
/// logical operation, the Raster kind, no calibrated interval and an empty Name. Ticks
/// are on the game's monotonic clock and remain process-local.
struct HostedRecordingSample final
{
    /// Recording the pass was measured for.
    foundation::uint64 Recording = 0;
    /// The game recorder's frame number, as in HostedRecordingAcknowledgement::Frame.
    foundation::uint64 Frame = 0;
    /// Game clock when the frame's batch was armed, before the frame was encoded.
    foundation::uint64 SubmissionTicks = 0;
    /// Game clock when the recorder observed the resolved batch; not before SubmissionTicks.
    foundation::uint64 CollectionTicks = 0;
    /// Measured device interval in nanoseconds.
    foundation::uint64 DurationNanoseconds = 0;
    /// Nonzero producer-local identity of the pass's scope; zero for a frame sample.
    foundation::uint64 ScopeIdentity = 0;
    /// Zero-based physical operation within the scope.
    foundation::uint32 ScopePassIndex = 0;
    /// Logical operations the physical pass covers, at least one.
    foundation::uint32 LogicalPassCount = 1;
    /// Native operation class.
    HostedPassKind Kind = HostedPassKind::Raster;
    /// True when the sample measures the whole frame rather than one named pass.
    bool IsFrame = false;
    /// True when StartTicks and EndTicks place the pass on the game's clock; never for a frame.
    bool HasCpuInterval = false;
    /// Calibrated start on the game's clock; meaningful only with HasCpuInterval.
    foundation::uint64 StartTicks = 0;
    /// Calibrated end on the game's clock, not before StartTicks; meaningful only with HasCpuInterval.
    foundation::uint64 EndTicks = 0;
    /// Error bound of the calibrated interval, in nanoseconds.
    foundation::uint64 ClockUncertaintyNanoseconds = 0;
    /// UTF-8 scope name: nonempty for a pass sample, empty for a frame sample.
    HostedPassName Name{};
};

/// @brief Kind of a HostedRecordingEvent.
enum class HostedRecordingEventKind : foundation::uint8
{
    /// HostedRecordingEvent::Acknowledgement is set.
    Acknowledgement,
    /// HostedRecordingEvent::Sample is set.
    Sample,
};

/// @brief One recording message the editor endpoint received from its window, in arrival order.
struct HostedRecordingEvent final
{
    /// Which member is meaningful.
    HostedRecordingEventKind Kind = HostedRecordingEventKind::Sample;
    /// Acknowledgement events.
    HostedRecordingAcknowledgement Acknowledgement{};
    /// Sample events.
    HostedRecordingSample Sample{};
};

/// @brief Producer side of the hosted recording channel, owned by a hosted window.
///
/// A recorder reaches it through NativeWindowInfo::HostedRecordingOrNull. While no
/// recorder is attached, the window answers every Start itself with Unsupported, so an
/// editor learns that the game cannot record instead of waiting. Every call is
/// nonblocking. Use it on the thread that pumps the window; it is not synchronized.
/// Recording messages never take the send room that the window keeps for its surface
/// messages, so recording cannot delay a Present.
class HostedRecordingChannel
{
public:
    /// @brief Announce that a recorder answers requests from now on.
    ///
    /// Requests the window answered itself stay answered.
    virtual void AttachRecorder() noexcept = 0;
    /// @brief Stop answering requests; the window answers later Starts with Unsupported.
    virtual void DetachRecorder() noexcept = 0;
    /// @brief Take the oldest request the window received from the endpoint.
    ///
    /// The window receives requests in HandleEvent(). An attached recorder must answer
    /// each taken request once with Acknowledge().
    /// @param outRequest The request; unchanged unless true is returned.
    /// @return true when a request was taken.
    [[nodiscard]] virtual bool TryTakeRecordingRequest(HostedRecordingRequest& outRequest) noexcept = 0;
    /// @brief Answer one taken request.
    /// @param acknowledgement Kind, Recording and Request of the taken request, and its outcome.
    /// Names longer than HOSTED_RECORDING_PRODUCER_NAME_BYTES or without a terminator are refused.
    /// @return Ok; Busy when the recording send room is full (retry on a later frame);
    /// Disconnected; InvalidArgument for a zero recording or request, an invalid name, or
    /// IsPassTimingSupported on anything but a successful Start.
    [[nodiscard]] virtual HostedSurfaceStatus
    Acknowledge(const HostedRecordingAcknowledgement& acknowledgement) noexcept = 0;
    /// @brief Send one measured pass.
    /// @return Ok; Busy when the recording send room is full, so the caller counts the
    /// sample as dropped; Disconnected; InvalidArgument for zero identities, a zero
    /// logical count, ticks out of order, an unterminated or invalid name, a pass sample
    /// without a name or a frame sample with pass fields (see HostedRecordingSample).
    [[nodiscard]] virtual HostedSurfaceStatus SendSample(const HostedRecordingSample& sample) noexcept = 0;

protected:
    /// Constructed only as part of a hosted window.
    HostedRecordingChannel() noexcept = default;
    /// Not destroyed through this interface.
    ~HostedRecordingChannel() = default;
    /// Not copyable through this interface.
    HostedRecordingChannel(const HostedRecordingChannel&) = default;
    /// Not copyable through this interface.
    HostedRecordingChannel& operator=(const HostedRecordingChannel&) = default;
    /// Not movable through this interface.
    HostedRecordingChannel(HostedRecordingChannel&&) = default;
    /// Not movable through this interface.
    HostedRecordingChannel& operator=(HostedRecordingChannel&&) = default;
};
} // namespace ludus::platform
