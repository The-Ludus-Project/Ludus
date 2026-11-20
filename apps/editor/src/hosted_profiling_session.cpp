#include "internal/hosted_profiling_session.h"

#include <ludus/foundation/time/time.hpp>

#include <QJsonArray>
#include <QJsonValue>

#include <cstring>

// Hosted recording admission and export; the contract is owned by
// docs/architecture/editor-gui-systems.md, "Hosted recording".
namespace ludus::editor
{
using namespace foundation;
namespace
{
/// Editor recording identities, never reused within this process. A delayed sample of an
/// earlier recording, of this or an earlier game process, therefore never matches.
uint64 gLastRecording = 0;
/// Request numbers increase across every session the process starts.
uint64 gLastRequest = 0;
/// GPU process identity in the exporter's trace (export_perfetto.cpp, pid 2).
constexpr int32 GPU_TRACE_PROCESS = 2;

[[nodiscard]] QString GetName(const platform::HostedProducerName& name)
{
    usize length = 0;
    while (length < name.GetSize() && name[length] != '\0')
    {
        ++length;
    }
    return QString::fromUtf8(name.GetData(), static_cast<qsizetype>(length));
}

[[nodiscard]] QString DescribeEndpointStatus(platform::HostedEndpointStatus status)
{
    switch (status)
    {
        case platform::HostedEndpointStatus::NotConnected:
            return QStringLiteral("The game is not connected to the viewport.");
        case platform::HostedEndpointStatus::Overflow:
            return QStringLiteral("The game is not reading its window channel. Try again once it renders.");
        case platform::HostedEndpointStatus::InvalidState:
            return QStringLiteral("The game is closing or has unanswered recording requests. Wait, then try again.");
        default:
            return QStringLiteral("The recording request could not be sent to the game.");
    }
}

[[nodiscard]] QJsonValue ToJson(uint64 value)
{
    // Qt's JSON numbers are binary64; identities and ticks keep every digit as text.
    return QString::number(value);
}
} // namespace

HostedProfilingSession::~HostedProfilingSession() noexcept
{
    Reset();
}

// A sample capacity and a clock reading, in that order, as documented on the declaration.
// NOLINTBEGIN(bugprone-easily-swappable-parameters)
bool HostedProfilingSession::Start(HostedRecordingTarget& inoutTarget,
                                   usize maximumSamples,
                                   uint64 nowTicks,
                                   QString& outReason)
// NOLINTEND(bugprone-easily-swappable-parameters)
{
    Reset();
    if (inoutTarget.IsRecordingConnected() == false)
    {
        outReason = QStringLiteral("The game is not connected to the viewport.");
        return false;
    }
    if (mSamples.TryEnsureCapacity(maximumSamples) == false)
    {
        outReason = QStringLiteral("Cannot reserve GPU recording storage.");
        return false;
    }
    platform::HostedRecordingRequest request;
    request.Kind = platform::HostedRecordingRequestKind::Start;
    request.Recording = gLastRecording + 1;
    request.Request = gLastRequest + 1;
    const platform::HostedEndpointStatus status = inoutTarget.SendRecordingRequest(request);
    if (status != platform::HostedEndpointStatus::Ok)
    {
        outReason = DescribeEndpointStatus(status);
        return false;
    }
    gLastRecording = request.Recording;
    gLastRequest = request.Request;
    mTargetOrNull = &inoutTarget;
    mMaximumSamples = maximumSamples;
    mStartRequest = request.Request;
    mPhase = HostedRecordingPhase::Starting;
    mDeadlineTicks = nowTicks + ACKNOWLEDGEMENT_NANOSECONDS;
    mProducer.Recording = request.Recording;
    mProducer.ProcessId = inoutTarget.GetProducerProcessId();
    mProducer.StartRequestTicks = nowTicks;
    mConsumedAtStart = inoutTarget.GetConsumedFrames();
    mDroppedEventsAtStart = inoutTarget.GetDroppedRecordingEvents();
    inoutTarget.SetRecordingSink(this);
    return true;
}

void HostedProfilingSession::Stop(uint64 nowTicks) noexcept
{
    if (mPhase != HostedRecordingPhase::Recording || mTargetOrNull == nullptr)
    {
        return;
    }
    platform::HostedRecordingRequest request;
    request.Kind = platform::HostedRecordingRequestKind::Stop;
    request.Recording = mProducer.Recording;
    request.Request = gLastRequest + 1;
    mProducer.StopRequestTicks = nowTicks;
    mDeadlineTicks = nowTicks + ACKNOWLEDGEMENT_NANOSECONDS;
    mPhase = HostedRecordingPhase::Stopping;
    if (mTargetOrNull->SendRecordingRequest(request) != platform::HostedEndpointStatus::Ok)
    {
        // Without a Stop the game cannot report its coverage: the recording is incomplete.
        finish();
        return;
    }
    gLastRequest = request.Request;
    mStopRequest = request.Request;
}

void HostedProfilingSession::Poll(uint64 nowTicks) noexcept
{
    if ((mPhase == HostedRecordingPhase::Starting || mPhase == HostedRecordingPhase::Stopping) &&
        nowTicks >= mDeadlineTicks)
    {
        mProducer.HasTimedOut = true;
        if (mPhase == HostedRecordingPhase::Starting)
        {
            mFailure = QStringLiteral("The game did not answer the recording request within two seconds. It may not "
                                      "attach a hosted GPU recorder; check that it is still rendering.");
        }
        finish();
    }
}

void HostedProfilingSession::Reset() noexcept
{
    if (mTargetOrNull != nullptr)
    {
        mTargetOrNull->SetRecordingSink(nullptr);
    }
    mTargetOrNull = nullptr;
    mPhase = HostedRecordingPhase::Idle;
    mStartRequest = 0;
    mStopRequest = 0;
    mDeadlineTicks = 0;
    mConsumedAtStart = 0;
    mDroppedEventsAtStart = 0;
    mEditorDroppedSamples = 0;
    mEndpointDroppedEvents = 0;
    mMaximumSamples = 0;
    mSamples.Clear();
    mProducer = {};
    mFailure.clear();
}

void HostedProfilingSession::finish() noexcept
{
    if (mTargetOrNull != nullptr)
    {
        mProducer.ConsumedFrames = mTargetOrNull->GetConsumedFrames() - mConsumedAtStart;
        mEndpointDroppedEvents = mTargetOrNull->GetDroppedRecordingEvents() - mDroppedEventsAtStart;
        mTargetOrNull->SetRecordingSink(nullptr);
    }
    mTargetOrNull = nullptr;
    mPhase = HostedRecordingPhase::Finished;
}

void HostedProfilingSession::EndRecordingChannel() noexcept
{
    if (mPhase == HostedRecordingPhase::Idle || mPhase == HostedRecordingPhase::Finished)
    {
        return;
    }
    mProducer.IsDisconnected = true;
    if (mPhase == HostedRecordingPhase::Starting)
    {
        mFailure = QStringLiteral("The game disconnected before it answered the recording request.");
    }
    // The target has already forgotten this sink; read its counters before forgetting it.
    finish();
}

void HostedProfilingSession::AcceptRecordingEvent(const platform::HostedRecordingEvent& event) noexcept
{
    if (event.Kind == platform::HostedRecordingEventKind::Acknowledgement)
    {
        acceptAcknowledgement(event.Acknowledgement);
        return;
    }
    acceptSample(event.Sample);
}

void HostedProfilingSession::acceptAcknowledgement(
    const platform::HostedRecordingAcknowledgement& acknowledgement) noexcept
{
    if (acknowledgement.Recording != mProducer.Recording)
    {
        return;
    }
    const uint64 now = time::NowTicks();
    if (mPhase == HostedRecordingPhase::Starting && acknowledgement.Request == mStartRequest)
    {
        mProducer.ProducerName = GetName(acknowledgement.ProducerName);
        if (acknowledgement.Result != platform::HostedRecordingResult::Ok || acknowledgement.Frame == 0)
        {
            mFailure = acknowledgement.Result == platform::HostedRecordingResult::Busy
                           ? QStringLiteral("The game is already recording for another request.")
                           : QStringLiteral("The game cannot record GPU timing: it has no hosted GPU recorder, or its "
                                            "device was created without GPU timing.");
            finish();
            return;
        }
        mProducer.StartFrame = acknowledgement.Frame;
        mProducer.IsPassTimingSupported = acknowledgement.IsPassTimingSupported;
        mProducer.StartAcknowledgedTicks = acknowledgement.Ticks;
        mProducer.StartReceivedTicks = now;
        mPhase = HostedRecordingPhase::Recording;
        return;
    }
    if (mPhase == HostedRecordingPhase::Stopping && acknowledgement.Request == mStopRequest)
    {
        mProducer.StopAcknowledgedTicks = acknowledgement.Ticks;
        mProducer.StopReceivedTicks = now;
        if (acknowledgement.Result == platform::HostedRecordingResult::Ok)
        {
            mProducer.IsStopAcknowledged = true;
            mProducer.StopFrame = acknowledgement.Frame;
            mProducer.Coverage = acknowledgement.Coverage;
        }
        finish();
    }
}

void HostedProfilingSession::acceptSample(const platform::HostedRecordingSample& sample) noexcept
{
    // Only this recording's samples, from its acknowledged start frame on. Frame numbers
    // restart with every game process, so the recording identity is what separates them.
    // A device without per-pass counters sends whole frames, and only then.
    if ((mPhase != HostedRecordingPhase::Recording && mPhase != HostedRecordingPhase::Stopping) ||
        sample.Recording != mProducer.Recording || sample.Frame < mProducer.StartFrame ||
        sample.IsFrame == mProducer.IsPassTimingSupported)
    {
        ++mProducer.RejectedSamples;
        return;
    }
    RecordedGpuSample recorded;
    static_assert(platform::HOSTED_RECORDING_PASS_NAME_BYTES < graphics::rhi::GPU_SCOPE_NAME_BYTES);
    recorded.Scope.Identity = sample.ScopeIdentity;
    recorded.Scope.LogicalPassCount = sample.LogicalPassCount;
    std::memcpy(recorded.Scope.Name.GetData(), sample.Name.GetData(), sample.Name.GetSize());
    recorded.Sample =
    {
        .SubmissionTicks = sample.SubmissionTicks,
        .CollectionTicks = sample.CollectionTicks,
        .DurationNanoseconds = sample.DurationNanoseconds,
        .FrameId = sample.Frame,
        .DeviceSession = sample.Recording,
        .PassName = {},
        .ScopeIdentity = sample.ScopeIdentity,
        .ScopePassIndex = sample.ScopePassIndex,
        .LogicalPassCount = sample.LogicalPassCount,
        .IsCompute = sample.Kind != platform::HostedPassKind::Raster,
        .IsRayTracing = sample.Kind == platform::HostedPassKind::RayTracing,
        .HasExecutionInterval = sample.HasCpuInterval,
        .ExecutionStartTicks = sample.StartTicks,
        .ExecutionEndTicks = sample.EndTicks,
        .ClockUncertaintyNanoseconds = sample.ClockUncertaintyNanoseconds,
    };
    if (mSamples.GetSize() >= mMaximumSamples || mSamples.TryAdd(recorded) == false)
    {
        ++mEditorDroppedSamples;
        return;
    }
    if (mProducer.FirstSampleFrame == 0 || sample.Frame < mProducer.FirstSampleFrame)
    {
        mProducer.FirstSampleFrame = sample.Frame;
    }
    if (sample.Frame > mProducer.LastSampleFrame)
    {
        mProducer.LastSampleFrame = sample.Frame;
    }
}

profiling::GpuTraceCoverage HostedProfilingSession::GetCoverage() const noexcept
{
    const platform::HostedRecordingCoverage& producer = mProducer.Coverage;
    return
    {
        .IsGpuRequested = true,
        .IsPassTimingSupported = HasStarted() == true && mProducer.IsPassTimingSupported == true,
        .EligibleFrames = producer.SubmittedFrames,
        .InvalidSamples = producer.InvalidSamples,
        .DroppedSamples = producer.DroppedSamples + mEditorDroppedSamples + mEndpointDroppedEvents,
        .UnresolvedSamples = 0,
        .QueryDroppedPasses = producer.QueryDroppedPasses,
        .UnresolvedPassBatches = producer.UnresolvedBatches,
    };
}

Array<RecordedGpuSample> HostedProfilingSession::TakeSamples() noexcept
{
    return Move(mSamples);
}

void AppendHostedProducerTrace(QJsonObject& inoutTrace, const HostedProducerRecord& producer)
{
    const QString processName =
        QStringLiteral("GPU - hosted game %1 (pid %2)")
            .arg(producer.ProducerName.isEmpty() == true ? QStringLiteral("(unnamed)") : producer.ProducerName)
            .arg(producer.ProcessId);
    QJsonArray events = inoutTrace.value(QStringLiteral("traceEvents")).toArray();
    for (qsizetype index = 0; index < events.size(); ++index)
    {
        QJsonObject event = events[index].toObject();
        if (event.value(QStringLiteral("pid")).toInt() == GPU_TRACE_PROCESS &&
            event.value(QStringLiteral("ph")).toString() == QStringLiteral("M") &&
            event.value(QStringLiteral("name")).toString() == QStringLiteral("process_name"))
        {
            event.insert(QStringLiteral("args"), QJsonObject{{QStringLiteral("name"), processName}});
            events[index] = event;
        }
    }
    inoutTrace.insert(QStringLiteral("traceEvents"), events);
    const platform::HostedRecordingCoverage& coverage = producer.Coverage;
    const bool isComplete = producer.IsStopAcknowledged == true && producer.IsDisconnected == false;
    const QJsonObject record{
        {QStringLiteral("producerName"), producer.ProducerName},
        {QStringLiteral("processId"), ToJson(producer.ProcessId)},
        {QStringLiteral("processIdSource"), QStringLiteral("operating-system peer credentials at admission")},
        {QStringLiteral("recording"), ToJson(producer.Recording)},
        {QStringLiteral("frameNumbering"), QStringLiteral("game recorder's submitted frames, from one per process")},
        {QStringLiteral("startFrame"), ToJson(producer.StartFrame)},
        {QStringLiteral("passTimingSupported"), producer.IsPassTimingSupported},
        {QStringLiteral("sampleKind"),
         producer.IsPassTimingSupported == true
             ? QStringLiteral("named physical passes")
             : QStringLiteral("whole frames: the game's device has no per-pass counters")},
        {QStringLiteral("stopFrame"), ToJson(producer.StopFrame)},
        {QStringLiteral("firstSampleFrame"), ToJson(producer.FirstSampleFrame)},
        {QStringLiteral("lastSampleFrame"), ToJson(producer.LastSampleFrame)},
        {QStringLiteral("complete"), isComplete},
        {QStringLiteral("stopAcknowledged"), producer.IsStopAcknowledged},
        {QStringLiteral("disconnected"), producer.IsDisconnected},
        {QStringLiteral("acknowledgementTimedOut"), producer.HasTimedOut},
        {QStringLiteral("clockRelationship"),
         QStringLiteral("every ts and sample tick is on the game's monotonic clock; editor ticks bracket each "
                        "acknowledgement but establish no offset between the processes")},
        {QStringLiteral("startRequestEditorTicks"), ToJson(producer.StartRequestTicks)},
        {QStringLiteral("startAcknowledgedGameTicks"), ToJson(producer.StartAcknowledgedTicks)},
        {QStringLiteral("startReceivedEditorTicks"), ToJson(producer.StartReceivedTicks)},
        {QStringLiteral("stopRequestEditorTicks"), ToJson(producer.StopRequestTicks)},
        {QStringLiteral("stopAcknowledgedGameTicks"), ToJson(producer.StopAcknowledgedTicks)},
        {QStringLiteral("stopReceivedEditorTicks"), ToJson(producer.StopReceivedTicks)},
        {QStringLiteral("submittedFrames"), ToJson(coverage.SubmittedFrames)},
        {QStringLiteral("measuredFrames"), ToJson(coverage.MeasuredFrames)},
        {QStringLiteral("skippedFrames"), ToJson(coverage.SkippedFrames)},
        {QStringLiteral("deliveredSamples"), ToJson(coverage.DeliveredSamples)},
        {QStringLiteral("gameDroppedSamples"), ToJson(coverage.DroppedSamples)},
        {QStringLiteral("rejectedSamples"), ToJson(producer.RejectedSamples)},
        {QStringLiteral("imageLeases"),
         QStringLiteral("the editor samples presented images and returns each one; recording holds none")},
        {QStringLiteral("consumedFrames"), ToJson(producer.ConsumedFrames)},
    };
    inoutTrace.insert(QStringLiteral("ludusHostedProducer"), record);
}
} // namespace ludus::editor
