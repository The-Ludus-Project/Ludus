#include "internal/profiling_controller.h"
#include "internal/render_preview.h"

#include <ludus/foundation/base/config.h>
#include <ludus/foundation/math/scalar.hpp>
#include <ludus/foundation/profiling/integration.hpp>
#include <ludus/foundation/profiling/profiling.hpp>
#include <ludus/foundation/profiling/trace_system.hpp>
#include <ludus/foundation/time/time.hpp>

#include <QByteArray>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QThread>

namespace ludus::editor
{
using namespace foundation;
namespace profile = foundation::profiling;
namespace
{
constexpr int32 POLL_MILLISECONDS = 16;
constexpr uint64 FINALIZE_NANOSECONDS = 2'000'000'000;
constexpr uint64 MAX_RECORDING_NANOSECONDS = 30'000'000'000;
constexpr usize CPU_CAPTURE_CHUNKS = 64;
constexpr uint64 STATUS_REFRESH_NANOSECONDS = 250'000'000;

/// Appended to a hosted recording's status once the game reports no per-pass counters.
[[nodiscard]] QString DescribeHostedFrameTimingOnly()
{
    return QStringLiteral(" Per-pass counters unavailable on the game's device; frame timing only.");
}

/// True once the game acknowledged Start without per-pass counters on its device.
[[nodiscard]] bool IsHostedFrameTimingOnly(const HostedProducerRecord& producer) noexcept
{
    return producer.StartFrame != 0 && producer.IsPassTimingSupported == false;
}
} // namespace

/// Serializes and saves on a worker; its owned bytes survive an unsuccessful write.
class ProfilingExportJob final : public QThread
{
public:
    Array<RecordedGpuSample> Samples;
    Array<uint8> Bytes;
    profile::GpuTraceCoverage Coverage;
    ViewportTraceSnapshot ViewportSnapshot;
    QJsonArray GraphFrames;
    HostedProducerRecord HostedProducer;
    bool IsHosted = false;
    QString Destination;
    QString Error;
    bool ShouldIncludeCpu = false;
    bool ShouldSerialize = true;
    bool IsSaved = false;

protected:
    void run() override
    {
        Array<profile::GpuTraceSample> traceSamples;
        if (ShouldSerialize == true && traceSamples.TryResize(Samples.GetSize()) == false)
        {
            Error = QStringLiteral("Cannot allocate GPU export views.");
            return;
        }
        for (usize index = 0; index < traceSamples.GetSize(); ++index)
        {
            traceSamples[index] = Samples[index].Sample;
            const graphics::rhi::GpuScopeDescription& scope = Samples[index].Scope;
            if (scope.Identity != 0)
            {
                usize length = 0;
                while (scope.Name[length] != '\0')
                {
                    ++length;
                }
                traceSamples[index].PassName = {scope.Name.GetData(), length};
            }
        }
        if (ShouldSerialize == true && profile::SerializePerfettoTrace(Bytes,
                                                                       {traceSamples.GetData(), traceSamples.GetSize()},
                                                                       Coverage,
                                                                       ShouldIncludeCpu) == false)
        {
            Error = QStringLiteral("No trace data was recorded, or serialization failed.");
            return;
        }
        if (ShouldSerialize == true &&
            (ViewportSnapshot.StartedTicks != 0 || IsHosted == true || GraphFrames.isEmpty() == false))
        {
            const QByteArray original(reinterpret_cast<const char*>(Bytes.GetData()),
                                      static_cast<qsizetype>(Bytes.GetSize()));
            QJsonObject trace = QJsonDocument::fromJson(original).object();
            if (ViewportSnapshot.StartedTicks != 0)
            {
                AppendViewportTrace(trace, ViewportSnapshot);
            }
            if (IsHosted == true)
            {
                AppendHostedProducerTrace(trace, HostedProducer);
            }
            if (GraphFrames.isEmpty() == false)
            {
                trace.insert(QStringLiteral("ludusGpuGraphFrames"), GraphFrames);
            }
            const QByteArray serialized = QJsonDocument(trace).toJson(QJsonDocument::Compact);
            if (Bytes.TryResize(static_cast<usize>(serialized.size())) == false)
            {
                Error = QStringLiteral("Cannot allocate viewport trace export bytes.");
                return;
            }
            for (usize index = 0; index < Bytes.GetSize(); ++index)
            {
                Bytes[index] = static_cast<uint8>(serialized[static_cast<qsizetype>(index)]);
            }
        }
        QFile file(Destination);
        if (QDir().mkpath(QFileInfo(Destination).absolutePath()) == false ||
            file.open(QIODevice::WriteOnly | QIODevice::NewOnly) == false)
        {
            Error = QStringLiteral("Cannot open the trace destination. Choose another path and retry Save.");
            return;
        }
        const int64 size = static_cast<int64>(Bytes.GetSize());
        if (file.write(reinterpret_cast<const char*>(Bytes.GetData()), size) != size || file.flush() == false)
        {
            static_cast<void>(file.remove());
            Error = QStringLiteral("Cannot save the trace. The serialized recording is retained for retry.");
            return;
        }
        IsSaved = true;
    }
};

ProfilingController::ProfilingController(RenderPreview* preview, QObject* parentOrNull)
    : QObject(parentOrNull), mPreviewOrNull(preview)
{
    LUDUS_REQUIRE(preview != nullptr);
    mTimer.setInterval(POLL_MILLISECONDS);
    connect(&mTimer, &QTimer::timeout, this, &ProfilingController::poll);
    connectPreview();
}

bool ProfilingController::SetPreviewSource(RenderPreview* preview)
{
    LUDUS_REQUIRE(preview != nullptr);
    if (preview == mPreviewOrNull)
    {
        return true;
    }
    if (GetState() == ProfilingState::Recording || GetState() == ProfilingState::Finalizing)
    {
        return false;
    }
    if (mPreviewOrNull != nullptr)
    {
        disconnect(mPreviewOrNull, nullptr, this, nullptr);
    }
    mPreviewOrNull = preview;
    connectPreview();
    Q_EMIT Changed();
    return true;
}

bool ProfilingController::SetHostedTarget(HostedRecordingTarget* targetOrNull, QObject* lifetimeOrNull)
{
    LUDUS_REQUIRE(targetOrNull == nullptr || lifetimeOrNull != nullptr);
    if (targetOrNull == mHostedTargetOrNull && mHostedLifetime.data() == lifetimeOrNull)
    {
        return true;
    }
    if (GetState() == ProfilingState::Recording || GetState() == ProfilingState::Finalizing)
    {
        return false;
    }
    mHostedTargetOrNull = targetOrNull;
    mHostedLifetime = lifetimeOrNull;
    Q_EMIT Changed();
    return true;
}

bool ProfilingController::isHostedConnected() const noexcept
{
    return mHostedTargetOrNull != nullptr && mHostedLifetime.isNull() == false &&
           mHostedTargetOrNull->IsRecordingConnected() == true;
}

QString ProfilingController::GetTargetDescription() const
{
    if (isHostedConnected() == true)
    {
        return QStringLiteral("GPU: the game playing in the World viewport (process %1). CPU: editor process.")
            .arg(mHostedTargetOrNull->GetProducerProcessId());
    }
    return QStringLiteral("GPU: the World viewport's scene device. CPU: editor process.");
}

usize ProfilingController::GetRecordedSampleCount() const noexcept
{
    if (mState == ProfilingState::Recording)
    {
        return mIsHosted == true ? mHosted.GetSampleCount() : mSamples.GetSize();
    }
    return mCompletedSampleCount;
}

const HostedProducerRecord& ProfilingController::GetHostedProducer() const noexcept
{
    return mIsHosted == true && mState == ProfilingState::Recording ? mHosted.GetProducer() : mHostedProducer;
}

bool ProfilingController::startHosted(const QString& destination)
{
    QString reason;
    const uint64 now = time::NowTicks();
    if (mHosted.Start(*mHostedTargetOrNull, MAX_GPU_SAMPLES, now, reason) == false)
    {
        mStatus = QStringLiteral("Cannot record the game playing in the viewport: %1").arg(reason);
        Q_EMIT Changed();
        return false;
    }
    mMode = ProfilingMode::Gpu;
    mIsHosted = true;
    mHostedProducer = {};
    mCoverage = { .IsGpuRequested = true };
    mStartedTicks = now;
    mStoppedTicks = 0;
    mLastStatusTicks = now;
    mDeviceSession = 0;
    mDestination = QFileInfo(destination).absoluteFilePath();
    mIsStopping = false;
    mState = ProfilingState::Recording;
    mStatus = QStringLiteral("Recording the game playing in the viewport (process %1). Its recorder measures its own "
                             "passes; press Ctrl+Alt+F9 again to stop.")
                  .arg(mHosted.GetProducer().ProcessId);
    mTimer.start();
    Q_EMIT Changed();
    return true;
}

void ProfilingController::pollHosted()
{
    const uint64 now = time::NowTicks();
    if (mIsStopping == true && mHosted.GetPhase() == HostedRecordingPhase::Recording)
    {
        // Stop arrived while the game had not yet acknowledged Start.
        mHosted.Stop(now);
    }
    mHosted.Poll(now);
    if (mHosted.GetPhase() == HostedRecordingPhase::Finished)
    {
        if (mHosted.HasStarted() == false)
        {
            mTimer.stop();
            mStatus = QStringLiteral("Cannot record the game playing in the viewport: %1").arg(mHosted.GetFailure());
            mHosted.Reset();
            mIsHosted = false;
            mState = ProfilingState::Failed;
            Q_EMIT Changed();
            return;
        }
        // Stop acknowledged, the game disconnected, or its acknowledgement timed out.
        mIsStopping = true;
        mStoppedTicks = mStoppedTicks == 0 ? now : mStoppedTicks;
        beginExport();
        return;
    }
    if (mIsStopping == false && now - mLastStatusTicks >= STATUS_REFRESH_NANOSECONDS)
    {
        mLastStatusTicks = now;
        Q_EMIT Changed();
    }
    if (mIsStopping == false && now - mStartedTicks >= MAX_RECORDING_NANOSECONDS)
    {
        Stop();
    }
}

void ProfilingController::connectPreview()
{
    LUDUS_REQUIRE(mPreviewOrNull != nullptr);
    connect(mPreviewOrNull, &QObject::destroyed, this, [this]() {
        mPreviewOrNull = nullptr;
        Stop();
    });
    connect(mPreviewOrNull, &RenderPreview::GpuProfileSample, this, &ProfilingController::acceptSample);
    connect(mPreviewOrNull,
            &RenderPreview::GpuPassProfileBatch,
            this,
            &ProfilingController::acceptPassBatch,
            Qt::DirectConnection);
    connect(mPreviewOrNull,
            &RenderPreview::GpuProfileSubmission,
            this,
            [this](uint64 frameId, uint64 deviceSession, const QByteArray& runtimeIdentity) {
                if (mState == ProfilingState::Recording && mIsStopping == false && mMode != ProfilingMode::Cpu)
                {
                    ++mCoverage.EligibleFrames;
                    if (runtimeIdentity.isEmpty() == false && deviceSession == mDeviceSession &&
                        mGraphFrames.size() < static_cast<qsizetype>(MAX_GPU_SAMPLES))
                    {
                        mGraphFrames.append(QJsonObject{
                            {QStringLiteral("frameId"), static_cast<qint64>(frameId)},
                            {QStringLiteral("deviceSession"), static_cast<qint64>(deviceSession)},
                            {QStringLiteral("runtime_sha256"), QString::fromLatin1(runtimeIdentity)},
                        });
                    }
                }
            });
    connect(mPreviewOrNull, &RenderPreview::DeviceStopping, this, [this]() {
        if (mState == ProfilingState::Recording && mMode != ProfilingMode::Cpu)
        {
            if (mPreviewOrNull->HasGpuProfilingPending() == true)
            {
                if (mPreviewOrNull->HasGpuPassProfilingPending() == true)
                {
                    ++mCoverage.UnresolvedPassBatches;
                }
                if (mPreviewOrNull->HasGpuFrameProfilingPending() == true)
                {
                    ++mCoverage.UnresolvedSamples;
                }
            }
            Stop();
        }
    });
}

ProfilingController::~ProfilingController()
{
    if (mPreviewOrNull != nullptr)
    {
        static_cast<void>(mPreviewOrNull->SetGpuProfilingEnabled(false));
    }
    if (mCpuStopOrNull != nullptr)
    {
        mCpuStopOrNull->wait();
        delete mCpuStopOrNull;
    }
    if (mJobOrNull != nullptr)
    {
        mJobOrNull->wait();
        delete mJobOrNull;
    }
    else if (mState == ProfilingState::Recording && mMode != ProfilingMode::Gpu)
    {
        static_cast<void>(StopViewportTrace());
        profile::EndCapture();
    }
}

bool ProfilingController::Start(ProfilingMode mode, const QString& destination)
{
    if (mState == ProfilingState::Recording || mState == ProfilingState::Finalizing || HasUnsavedSnapshot() == true ||
        profile::IsCaptureActive() == true)
    {
        mStatus = QStringLiteral("A recording is active or unsaved. Stop, retry Save, or discard it first.");
        Q_EMIT Changed();
        return false;
    }
#if LUDUS_TARGET_OS == LUDUS_OS_WEB
    mStatus = QStringLiteral("Editor Perfetto recording currently requires a native editor build.");
    Q_EMIT Changed();
    return false;
#endif
    if (mode != ProfilingMode::Gpu && profile::HasUnexportedTrace() == true)
    {
        mStatus =
            QStringLiteral("Another CPU recording has unexported data. Export or discard it through its owner first.");
        Q_EMIT Changed();
        return false;
    }
    if constexpr (LUDUS_PROFILING_ENABLED == 0)
    {
        mStatus = QStringLiteral("Profiling is disabled in this build. Use a Debug, Development or Profile editor.");
        Q_EMIT Changed();
        return false;
    }
    if (destination.isEmpty() == true)
    {
        mStatus = QStringLiteral("Choose a new output path for the recording.");
        Q_EMIT Changed();
        return false;
    }
    if (QFileInfo::exists(destination) == true)
    {
        mStatus = QStringLiteral("The output file already exists: %1. Choose a new path or clear Output file "
                                 "to use an automatic destination.")
                      .arg(QFileInfo(destination).absoluteFilePath());
        Q_EMIT Changed();
        return false;
    }
    if (mode != ProfilingMode::Cpu && isHostedConnected() == true)
    {
        if (mode == ProfilingMode::Both)
        {
            // Both would put editor CPU scopes and the game's GPU work in one trace on unrelated clocks.
            mStatus = QStringLiteral("Both is unavailable while a game plays in the viewport: GPU records the game "
                                     "in its own process. Select GPU to record the game, or CPU for the editor.");
            Q_EMIT Changed();
            return false;
        }
        return startHosted(destination);
    }
    if (mode != ProfilingMode::Cpu && CanRecordGpu() == false)
    {
        mStatus = QStringLiteral("GPU timing is unavailable for the selected viewport. Start its preview, "
                                 "or select CPU to record editor scopes.");
        Q_EMIT Changed();
        return false;
    }
    mSamples.Clear();
    if (mode != ProfilingMode::Cpu && mSamples.TryEnsureCapacity(MAX_GPU_SAMPLES) == false)
    {
        mStatus = QStringLiteral("Cannot reserve GPU recording storage.");
        Q_EMIT Changed();
        return false;
    }
    if (mode != ProfilingMode::Cpu && mPreviewOrNull->SetGpuProfilingEnabled(true) == false)
    {
        mStatus = QStringLiteral("The GPU timing facility could not be enabled on this viewport.");
        Q_EMIT Changed();
        return false;
    }
    if (mode != ProfilingMode::Gpu && StartViewportTrace() == false)
    {
        if (mPreviewOrNull != nullptr)
        {
            static_cast<void>(mPreviewOrNull->SetGpuProfilingEnabled(false));
        }
        mStatus = QStringLiteral("Viewport recorder is busy or its bounded storage could not be reserved.");
        Q_EMIT Changed();
        return false;
    }
    if (mode != ProfilingMode::Gpu && profile::BeginCapture(CPU_CAPTURE_CHUNKS) == false)
    {
        if (mPreviewOrNull != nullptr)
        {
            static_cast<void>(mPreviewOrNull->SetGpuProfilingEnabled(false));
        }
        static_cast<void>(StopViewportTrace());
        mStatus = QStringLiteral("CPU recorder is busy or its bounded buffer could not be reserved.");
        Q_EMIT Changed();
        return false;
    }
    mMode = mode;
    mCoverage =
    {
        .IsGpuRequested = mode != ProfilingMode::Cpu,
        .IsPassTimingSupported = mode != ProfilingMode::Cpu && mPreviewOrNull->CanProfileGpuPasses() == true,
    };
    mStartedTicks = time::NowTicks();
    mLastStatusTicks = mStartedTicks;
    mDeviceSession = mPreviewOrNull != nullptr ? mPreviewOrNull->GetDeviceSerial() : 0;
    mDestination = QFileInfo(destination).absoluteFilePath();
    mIsStopping = false;
    mState = ProfilingState::Recording;
    mStatus = QStringLiteral("Recording. Press F9 again to stop; automatic stop after 30 seconds.");
    if (mode != ProfilingMode::Cpu && mCoverage.IsPassTimingSupported == false)
    {
        mStatus += QStringLiteral(" Per-pass counters unavailable; frame timing only.");
    }
    if (mode != ProfilingMode::Gpu)
    {
        profile::RegisterThreadForTrace("Editor GUI");
        LUDUS_PROFILE_MARK(EditorRecordingStarted);
    }
    mTimer.start();
    Q_EMIT Changed();
    return true;
}

void ProfilingController::Stop()
{
    if (mState != ProfilingState::Recording || mIsStopping == true)
    {
        return;
    }
    mStoppedTicks = time::NowTicks();
    if (mIsHosted == true)
    {
        mIsStopping = true;
        mHosted.Stop(mStoppedTicks);
        mStatus = QStringLiteral("Finalizing: waiting up to two seconds for the game to acknowledge Stop.");
        Q_EMIT Changed();
        poll();
        return;
    }
    if (mMode != ProfilingMode::Gpu)
    {
        mViewportSnapshot = StopViewportTrace();
        LUDUS_PROFILE_MARK(EditorRecordingStopped);
        mCpuStopOrNull = QThread::create([]() { profile::EndCapture(); });
        mCpuStopOrNull->start();
    }
    mIsStopping = true;
    if (mPreviewOrNull != nullptr)
    {
        static_cast<void>(mPreviewOrNull->SetGpuProfilingEnabled(false));
    }
    mStatus = QStringLiteral("Finalizing: collecting already submitted GPU timing without waiting.");
    Q_EMIT Changed();
    poll();
}

void ProfilingController::poll()
{
    if (mState != ProfilingState::Recording)
    {
        return;
    }
    if (mIsHosted == true)
    {
        pollHosted();
        return;
    }
    if (mMode != ProfilingMode::Cpu && mPreviewOrNull != nullptr)
    {
        mPreviewOrNull->PollGpuProfiling();
    }
    const uint64 now = time::NowTicks();
    if (mIsStopping == false && now - mLastStatusTicks >= STATUS_REFRESH_NANOSECONDS)
    {
        mLastStatusTicks = now;
        Q_EMIT Changed();
    }
    if (mIsStopping == false && now - mStartedTicks >= MAX_RECORDING_NANOSECONDS)
    {
        Stop();
        return;
    }
    if (mIsStopping == true && (mCpuStopOrNull == nullptr || mCpuStopOrNull->isFinished() == true) &&
        (mMode == ProfilingMode::Cpu || mPreviewOrNull == nullptr ||
         mPreviewOrNull->HasGpuProfilingPending(mStoppedTicks) == false ||
         now - mStoppedTicks >= FINALIZE_NANOSECONDS || mPreviewOrNull->GetDeviceSerial() != mDeviceSession))
    {
        if (mMode != ProfilingMode::Cpu &&
            (mPreviewOrNull == nullptr || mPreviewOrNull->HasGpuProfilingPending(mStoppedTicks) == true))
        {
            if (mPreviewOrNull == nullptr || mPreviewOrNull->HasGpuFrameProfilingPending(mStoppedTicks) == true)
            {
                ++mCoverage.UnresolvedSamples;
            }
            if (mPreviewOrNull != nullptr && mPreviewOrNull->HasGpuPassProfilingPending(mStoppedTicks) == true)
            {
                ++mCoverage.UnresolvedPassBatches;
            }
        }
        beginExport();
    }
}

void ProfilingController::acceptSample(uint64 frameId,
                                       uint64 deviceSession,
                                       uint64 submissionTicks,
                                       uint64 durationNanoseconds,
                                       bool isValid)
{
    if (mState != ProfilingState::Recording || mMode == ProfilingMode::Cpu || deviceSession != mDeviceSession ||
        submissionTicks < mStartedTicks || (mIsStopping == true && submissionTicks > mStoppedTicks))
    {
        return;
    }
    if (isValid == false)
    {
        ++mCoverage.InvalidSamples;
        return;
    }
    if (mSamples.GetSize() >= MAX_GPU_SAMPLES)
    {
        ++mCoverage.DroppedSamples;
        return;
    }
    const profile::GpuTraceSample sample
    {
        .SubmissionTicks = submissionTicks,
        .CollectionTicks = time::NowTicks(),
        .DurationNanoseconds = durationNanoseconds,
        .FrameId = frameId,
        .DeviceSession = deviceSession,
        .PassName = {},
    };
    if (mSamples.TryAdd(RecordedGpuSample{ .Sample = sample }) == false)
    {
        ++mCoverage.DroppedSamples;
    }
}

void ProfilingController::acceptPassBatch(uint64 frameId,
                                          uint64 deviceSession,
                                          uint64 submissionTicks,
                                          const graphics::rhi::GpuPassTimings& batch)
{
    if (mState != ProfilingState::Recording || mMode == ProfilingMode::Cpu || deviceSession != mDeviceSession ||
        submissionTicks < mStartedTicks || (mIsStopping == true && submissionTicks > mStoppedTicks))
    {
        return;
    }
    mCoverage.QueryDroppedPasses += batch.DroppedPasses;
    constexpr float64 MAX_DURATION_NANOSECONDS = 60'000'000'000;
    for (usize index = 0; index < batch.Count; ++index)
    {
        const graphics::rhi::GpuPassTiming& pass = batch.Passes[index];
        const float64 duration = pass.Timing.DurationNanoseconds;
        if (pass.Timing.State != graphics::rhi::GpuTimingState::Available ||
            (duration >= 0 && duration < MAX_DURATION_NANOSECONDS) == false)
        {
            ++mCoverage.InvalidSamples;
            continue;
        }
        RecordedGpuSample recorded;
        recorded.Scope = pass.Scope;
        recorded.Sample =
        {
            .SubmissionTicks = submissionTicks,
            .CollectionTicks = time::NowTicks(),
            // Round to the nearest nanosecond; the duration is nonnegative and below the limit above.
            .DurationNanoseconds = static_cast<uint64>(math::Floor(duration + 0.5)),
            .FrameId = frameId,
            .DeviceSession = deviceSession,
            .PassName = {},
            .ScopeIdentity = pass.Scope.Identity,
            .ScopePassIndex = pass.ScopePassIndex,
            .LogicalPassCount = pass.Scope.LogicalPassCount,
            .IsCompute = pass.Kind != graphics::rhi::GpuPassKind::Raster,
            .IsRayTracing = pass.Kind == graphics::rhi::GpuPassKind::RayTracing,
        };
        if (pass.HasCpuInterval == true)
        {
            recorded.Sample.HasExecutionInterval = true;
            recorded.Sample.ExecutionStartTicks = pass.StartTicks;
            recorded.Sample.ExecutionEndTicks = pass.EndTicks;
            recorded.Sample.ClockUncertaintyNanoseconds = batch.ClockUncertaintyNanoseconds;
        }
        if (mSamples.GetSize() >= MAX_GPU_SAMPLES || mSamples.TryAdd(recorded) == false)
        {
            ++mCoverage.DroppedSamples;
        }
    }
}

void ProfilingController::beginExport()
{
    mTimer.stop();
    if (mCpuStopOrNull != nullptr)
    {
        mCpuStopOrNull->wait();
        delete mCpuStopOrNull;
        mCpuStopOrNull = nullptr;
    }
    mState = ProfilingState::Finalizing;
    mJobOrNull = new ProfilingExportJob;
    if (mIsHosted == true)
    {
        mCoverage = mHosted.GetCoverage();
        mHostedProducer = mHosted.GetProducer();
        mSamples = mHosted.TakeSamples();
        mHosted.Reset();
        mJobOrNull->IsHosted = true;
        mJobOrNull->HostedProducer = mHostedProducer;
    }
    mCompletedSampleCount = mSamples.GetSize();
    mJobOrNull->Samples = Move(mSamples);
    mJobOrNull->Coverage = mCoverage;
    mJobOrNull->ViewportSnapshot = Move(mViewportSnapshot);
    mJobOrNull->GraphFrames.swap(mGraphFrames);
    mJobOrNull->Destination = mDestination;
    mJobOrNull->ShouldIncludeCpu = mMode != ProfilingMode::Gpu;
    connect(mJobOrNull, &QThread::finished, this, &ProfilingController::finishExport);
    mJobOrNull->start();
    Q_EMIT Changed();
}

void ProfilingController::finishExport()
{
    const bool isSaved = mJobOrNull->IsSaved;
    mSnapshot = Move(mJobOrNull->Bytes);
    if (isSaved == true)
    {
        mLastPath = mJobOrNull->Destination;
        mSnapshot.Clear();
        mStatus = QStringLiteral("Saved %1 GPU samples from %2 eligible frames; invalid %3, dropped %4, unresolved %5; "
                                 "query-dropped passes %6, unfinished pass batches %7.")
                      .arg(mCompletedSampleCount)
                      .arg(mCoverage.EligibleFrames)
                      .arg(mCoverage.InvalidSamples)
                      .arg(mCoverage.DroppedSamples)
                      .arg(mCoverage.UnresolvedSamples)
                      .arg(mCoverage.QueryDroppedPasses)
                      .arg(mCoverage.UnresolvedPassBatches);
        if (mMode != ProfilingMode::Cpu && mCoverage.IsPassTimingSupported == false)
        {
            mStatus += QStringLiteral(" Per-pass counters unavailable; frame timing only.");
        }
        if (mMode == ProfilingMode::Cpu)
        {
            mStatus = QStringLiteral("Saved CPU recording.");
        }
        if (mIsHosted == true)
        {
            mStatus = QStringLiteral("Saved %1 GPU samples from the game %2 (process %3), its frames %4 to %5; "
                                     "skipped %6 of %7 submitted frames, dropped %8, rejected %9.")
                          .arg(mCompletedSampleCount)
                          .arg(mHostedProducer.ProducerName)
                          .arg(mHostedProducer.ProcessId)
                          .arg(mHostedProducer.StartFrame)
                          .arg(mHostedProducer.IsStopAcknowledged == true ? mHostedProducer.StopFrame
                                                                          : mHostedProducer.LastSampleFrame)
                          .arg(mHostedProducer.Coverage.SkippedFrames)
                          .arg(mHostedProducer.Coverage.SubmittedFrames)
                          .arg(mCoverage.DroppedSamples)
                          .arg(mHostedProducer.RejectedSamples);
            if (mHostedProducer.IsStopAcknowledged == false)
            {
                mStatus += mHostedProducer.IsDisconnected == true
                               ? QStringLiteral(" Incomplete: the game disconnected before acknowledging Stop, so "
                                                "its frame and skip counts are unknown.")
                               : QStringLiteral(" Incomplete: the game did not acknowledge Stop in time.");
            }
            if (IsHostedFrameTimingOnly(mHostedProducer) == true)
            {
                mStatus += DescribeHostedFrameTimingOnly();
            }
        }
        mStatus += QStringLiteral(" Trace: %1").arg(mLastPath);
    }
    else
    {
        mStatus = mJobOrNull->Error;
        if (mIsHosted == true && mCompletedSampleCount == 0)
        {
            mStatus = mHostedProducer.IsDisconnected == true
                          ? QStringLiteral("The game disconnected before it delivered a GPU sample; nothing was saved.")
                      : IsHostedFrameTimingOnly(mHostedProducer) == true
                          ? QStringLiteral("The game delivered no GPU sample; nothing was saved. Its device has no "
                                           "per-pass counters and resolved no frame timing before Stop.")
                          : QStringLiteral("The game delivered no GPU sample; nothing was saved. Its passes must run "
                                           "inside named GPU scopes.");
        }
    }
    mIsHosted = false;
    mJobOrNull->wait();
    delete mJobOrNull;
    mJobOrNull = nullptr;
    mState = isSaved == true ? ProfilingState::Complete : ProfilingState::Failed;
    Q_EMIT Changed();
}

bool ProfilingController::RetrySave(const QString& destination)
{
    if (mState != ProfilingState::Failed || mSnapshot.IsEmpty() == true || destination.isEmpty() == true ||
        QFileInfo::exists(destination) == true)
    {
        return false;
    }
    mJobOrNull = new ProfilingExportJob;
    mJobOrNull->Bytes = Move(mSnapshot);
    mJobOrNull->Destination = destination;
    mJobOrNull->ShouldSerialize = false;
    mState = ProfilingState::Finalizing;
    connect(mJobOrNull, &QThread::finished, this, &ProfilingController::finishExport);
    mJobOrNull->start();
    Q_EMIT Changed();
    return true;
}

void ProfilingController::Discard()
{
    if (mState == ProfilingState::Failed)
    {
        mSnapshot.Clear();
        mState = ProfilingState::Idle;
        mStatus = QStringLiteral("Recording discarded.");
        Q_EMIT Changed();
    }
}

ProfilingState ProfilingController::GetState() const noexcept
{
    return mState == ProfilingState::Recording && mIsStopping == true ? ProfilingState::Finalizing : mState;
}
QString ProfilingController::GetStatus() const
{
    if (mState == ProfilingState::Recording && mIsStopping == false && mIsHosted == true)
    {
        const QString timing =
            IsHostedFrameTimingOnly(mHosted.GetProducer()) == true ? DescribeHostedFrameTimingOnly() : QString();
        return mStatus + timing +
               QStringLiteral(" Elapsed %1 s; %2 GPU samples from the game.")
                   .arg((time::NowTicks() - mStartedTicks) / time::NANOSECONDS_PER_SECOND)
                   .arg(mHosted.GetSampleCount());
    }
    if (mState == ProfilingState::Recording && mIsStopping == false)
    {
        const profile::TraceHealth health =
            mMode == ProfilingMode::Gpu ? profile::TraceHealth{} : profile::GetTraceHealth();
        return mStatus +
               QStringLiteral(" Elapsed %1 s. CPU events %2, dropped %3; GPU samples %4 across %5 submitted frames.")
                   .arg((time::NowTicks() - mStartedTicks) / time::NANOSECONDS_PER_SECOND)
                   .arg(health.EventsRecorded)
                   .arg(health.EventsDropped)
                   .arg(mSamples.GetSize())
                   .arg(mCoverage.EligibleFrames);
    }
    return mStatus;
}
QString ProfilingController::GetLastPath() const
{
    return mLastPath;
}
bool ProfilingController::HasUnsavedSnapshot() const noexcept
{
    return mSnapshot.IsEmpty() == false;
}
bool ProfilingController::CanRecordGpu() const noexcept
{
    return isHostedConnected() == true || (mPreviewOrNull != nullptr && mPreviewOrNull->CanProfileGpu() == true);
}
} // namespace ludus::editor
