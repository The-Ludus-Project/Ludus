#pragma once

#include <ludus/foundation/base/core.h>

#include <ludus/foundation/containers/array.hpp>
#include <ludus/foundation/profiling/perfetto_export.hpp>
#include <ludus/graphics/rhi/gpu_scope.h>
#include <ludus/platform/hosted_recording.h>
#include <ludus/platform/hosted_window.h>

#include <QJsonObject>
#include <QString>

namespace ludus::editor
{
/// Own names until export completes; borrowed sample views are rebuilt after storage moves.
struct RecordedGpuSample final
{
    foundation::profiling::GpuTraceSample Sample{};
    graphics::rhi::GpuScopeDescription Scope{};
};

class HostedRecordingSink;

/// @brief Editor end of a hosted game's recording channel.
///
/// PlayViewportSource implements it over its hosted-window endpoint; tests substitute a
/// scripted channel. GUI thread only.
class HostedRecordingTarget
{
public:
    /// True while a game is connected and can answer requests.
    [[nodiscard]] virtual bool IsRecordingConnected() const noexcept = 0;
    /// Send one request to the game's recorder; see HostedWindowEndpoint::SendRecordingRequest.
    [[nodiscard]] virtual platform::HostedEndpointStatus
    SendRecordingRequest(const platform::HostedRecordingRequest& request) noexcept = 0;
    /// Operating-system process identifier of the connected game, or zero.
    [[nodiscard]] virtual foundation::uint64 GetProducerProcessId() const noexcept = 0;
    /// Game frames a viewport has taken and returned since the session started.
    [[nodiscard]] virtual foundation::uint64 GetConsumedFrames() const noexcept = 0;
    /// Recording events dropped at the endpoint's full queue since the session started.
    [[nodiscard]] virtual foundation::uint64 GetDroppedRecordingEvents() const noexcept = 0;
    /// @brief Deliver recording events, then the channel's end, to `sinkOrNull`.
    ///
    /// Events are delivered as they are received, before the target reports a disconnection,
    /// so a recording keeps everything the game sent. Null stops delivery.
    virtual void SetRecordingSink(HostedRecordingSink* sinkOrNull) noexcept = 0;

protected:
    HostedRecordingTarget() noexcept = default;
    ~HostedRecordingTarget() = default;
    HostedRecordingTarget(const HostedRecordingTarget&) = default;
    HostedRecordingTarget& operator=(const HostedRecordingTarget&) = default;
    HostedRecordingTarget(HostedRecordingTarget&&) = default;
    HostedRecordingTarget& operator=(HostedRecordingTarget&&) = default;
};

/// Receives a HostedRecordingTarget's events on the GUI thread.
class HostedRecordingSink
{
public:
    /// One acknowledgement or sample, in arrival order.
    virtual void AcceptRecordingEvent(const platform::HostedRecordingEvent& event) noexcept = 0;
    /// @brief The game disconnected or the session stopped; no further events arrive.
    ///
    /// The target forgets the sink before calling this.
    virtual void EndRecordingChannel() noexcept = 0;

protected:
    HostedRecordingSink() noexcept = default;
    ~HostedRecordingSink() = default;
    HostedRecordingSink(const HostedRecordingSink&) = default;
    HostedRecordingSink& operator=(const HostedRecordingSink&) = default;
    HostedRecordingSink(HostedRecordingSink&&) = default;
    HostedRecordingSink& operator=(HostedRecordingSink&&) = default;
};

/// Lifecycle of one hosted recording.
enum class HostedRecordingPhase : foundation::uint8
{
    /// Not started.
    Idle,
    /// Start sent; waiting for the game's acknowledgement.
    Starting,
    /// The game acknowledged Start and streams samples.
    Recording,
    /// Stop sent; waiting for the game's acknowledgement.
    Stopping,
    /// Ended: Stop acknowledged, the game refused or disconnected, or a deadline passed.
    Finished,
};

/// What the export records about the game that produced a hosted trace.
struct HostedProducerRecord final
{
    QString ProducerName;
    foundation::uint64 ProcessId = 0;
    foundation::uint64 Recording = 0;
    /// First frame the game acknowledged; zero when Start was never acknowledged.
    foundation::uint64 StartFrame = 0;
    /// The game's Start acknowledgement reported per-pass counters on its device. False
    /// before the acknowledgement and when the game records whole-frame timing only.
    bool IsPassTimingSupported = false;
    /// Last frame the game reported at Stop; zero when Stop was not acknowledged.
    foundation::uint64 StopFrame = 0;
    /// Lowest and highest frame of an admitted sample; zero without samples.
    foundation::uint64 FirstSampleFrame = 0;
    foundation::uint64 LastSampleFrame = 0;
    bool IsStopAcknowledged = false;
    bool IsDisconnected = false;
    bool HasTimedOut = false;
    /// Editor clock when Start was sent and its acknowledgement received; game clock in between.
    foundation::uint64 StartRequestTicks = 0;
    foundation::uint64 StartAcknowledgedTicks = 0;
    foundation::uint64 StartReceivedTicks = 0;
    /// Editor clock when Stop was sent and its acknowledgement received; game clock in between.
    foundation::uint64 StopRequestTicks = 0;
    foundation::uint64 StopAcknowledgedTicks = 0;
    foundation::uint64 StopReceivedTicks = 0;
    /// The game's coverage from its Stop acknowledgement.
    platform::HostedRecordingCoverage Coverage{};
    /// Samples refused because they named another recording, preceded the start frame or
    /// were not the kind the Start acknowledgement announced.
    foundation::uint64 RejectedSamples = 0;
    /// Game frames the viewport took and returned while recording.
    foundation::uint64 ConsumedFrames = 0;
};

/// @brief One editor recording of a hosted game's GPU passes.
///
/// Owns the request/acknowledgement exchange and sample admission for one recording;
/// ProfilingController owns the lifecycle, storage limit and export. Admission accepts a
/// sample only for this session's nonzero recording identity, which no other recording of
/// this editor process uses, and only from the acknowledged start frame on, so delayed
/// samples of an earlier recording or game process are refused even when their frame
/// numbers repeat. A recording admits only the sample kind its Start acknowledgement
/// announced: named passes, or whole frames from a device without per-pass counters.
/// Samples received before a disconnection are kept. GUI thread only.
class HostedProfilingSession final : public HostedRecordingSink
{
public:
    /// Time the game has to acknowledge Start or Stop.
    static constexpr foundation::uint64 ACKNOWLEDGEMENT_NANOSECONDS = 2'000'000'000;

    HostedProfilingSession() noexcept = default;
    /// Stops receiving events from the target.
    ~HostedProfilingSession() noexcept;
    HostedProfilingSession(const HostedProfilingSession&) = delete;
    HostedProfilingSession& operator=(const HostedProfilingSession&) = delete;
    HostedProfilingSession(HostedProfilingSession&&) = delete;
    HostedProfilingSession& operator=(HostedProfilingSession&&) = delete;

    /// @brief Ask the connected game to start recording.
    /// @param inoutTarget Connected game; borrowed until the session finishes or Reset().
    /// @param maximumSamples Admitted samples kept; later samples are counted as dropped.
    /// @param nowTicks Editor clock.
    /// @param outReason Why recording could not start; set only on failure.
    /// @return true when Start was sent; the session is then Starting.
    [[nodiscard]] bool Start(HostedRecordingTarget& inoutTarget,
                             foundation::usize maximumSamples,
                             foundation::uint64 nowTicks,
                             QString& outReason);
    /// Send Stop once the game is recording; a session still Starting finishes when the
    /// acknowledgement deadline passes.
    void Stop(foundation::uint64 nowTicks) noexcept;
    /// Apply the acknowledgement deadline.
    void Poll(foundation::uint64 nowTicks) noexcept;
    /// Stop receiving events and forget the target and every result.
    void Reset() noexcept;

    void AcceptRecordingEvent(const platform::HostedRecordingEvent& event) noexcept override;
    void EndRecordingChannel() noexcept override;

    [[nodiscard]] HostedRecordingPhase GetPhase() const noexcept
    {
        return mPhase;
    }
    /// True when the game acknowledged Start; false for a refusal or a missing acknowledgement.
    [[nodiscard]] bool HasStarted() const noexcept
    {
        return mProducer.StartFrame != 0;
    }
    /// Why Start failed, once Finished without HasStarted().
    [[nodiscard]] QString GetFailure() const
    {
        return mFailure;
    }
    [[nodiscard]] foundation::usize GetSampleCount() const noexcept
    {
        return mSamples.GetSize();
    }
    [[nodiscard]] const HostedProducerRecord& GetProducer() const noexcept
    {
        return mProducer;
    }
    /// Coverage in the exporter's vocabulary; producer counts appear once acknowledged.
    [[nodiscard]] foundation::profiling::GpuTraceCoverage GetCoverage() const noexcept;
    /// Move the admitted samples out, leaving none.
    [[nodiscard]] foundation::Array<RecordedGpuSample> TakeSamples() noexcept;

private:
    void acceptAcknowledgement(const platform::HostedRecordingAcknowledgement& acknowledgement) noexcept;
    void acceptSample(const platform::HostedRecordingSample& sample) noexcept;
    void finish() noexcept;

    HostedRecordingTarget* mTargetOrNull = nullptr;
    HostedRecordingPhase mPhase = HostedRecordingPhase::Idle;
    foundation::uint64 mStartRequest = 0;
    foundation::uint64 mStopRequest = 0;
    foundation::uint64 mDeadlineTicks = 0;
    foundation::uint64 mConsumedAtStart = 0;
    foundation::uint64 mDroppedEventsAtStart = 0;
    foundation::uint64 mEditorDroppedSamples = 0;
    foundation::uint64 mEndpointDroppedEvents = 0;
    foundation::usize mMaximumSamples = 0;
    foundation::Array<RecordedGpuSample> mSamples;
    HostedProducerRecord mProducer;
    QString mFailure;
};

/// @brief Add the hosted producer's identity, frames, clocks and coverage to an exported trace.
///
/// Names the GPU process after the game and records under "ludusHostedProducer" that every
/// timestamp is on the game's clock: the editor's request/receive ticks bracket the game's
/// acknowledgement ticks but establish no offset between the two clocks.
void AppendHostedProducerTrace(QJsonObject& inoutTrace, const HostedProducerRecord& producer);
} // namespace ludus::editor
