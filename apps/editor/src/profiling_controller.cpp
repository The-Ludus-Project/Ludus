#include "internal/profiling_controller.h"
#include "internal/render_preview.h"

#include <ludus/foundation/base/config.h>
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
} // namespace

/// Serializes and saves on a worker; its owned bytes survive an unsuccessful write.
class ProfilingExportJob final : public QThread
{
public:
    Array<RecordedGpuSample> Samples;
    Array<uint8> Bytes;
    profile::GpuTraceCoverage Coverage;
    ViewportTraceSnapshot ViewportSnapshot;
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
        if (ShouldSerialize == true && ViewportSnapshot.StartedTicks != 0)
        {
            const QByteArray original(reinterpret_cast<const char*>(Bytes.GetData()),
                                      static_cast<qsizetype>(Bytes.GetSize()));
            QJsonObject trace = QJsonDocument::fromJson(original).object();
            AppendViewportTrace(trace, ViewportSnapshot);
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
    connect(mPreviewOrNull, &RenderPreview::GpuProfileSubmission, this, [this]() {
        if (mState == ProfilingState::Recording && mIsStopping == false && mMode != ProfilingMode::Cpu)
        {
            ++mCoverage.EligibleFrames;
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
    if (LUDUS_PROFILING_ENABLED == 0)
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
            .DurationNanoseconds = static_cast<uint64>(duration + 0.5),
            .FrameId = frameId,
            .DeviceSession = deviceSession,
            .ScopeIdentity = pass.Scope.Identity,
            .ScopePassIndex = pass.ScopePassIndex,
            .LogicalPassCount = pass.Scope.LogicalPassCount,
            .IsCompute = pass.Kind == graphics::rhi::GpuPassKind::Compute,
        };
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
    mCompletedSampleCount = mSamples.GetSize();
    mJobOrNull->Samples = Move(mSamples);
    mJobOrNull->Coverage = mCoverage;
    mJobOrNull->ViewportSnapshot = Move(mViewportSnapshot);
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
        mStatus += QStringLiteral(" Trace: %1").arg(mLastPath);
    }
    else
    {
        mStatus = mJobOrNull->Error;
    }
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
    return mPreviewOrNull != nullptr && mPreviewOrNull->CanProfileGpu() == true;
}
} // namespace ludus::editor
