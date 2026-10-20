#include "internal/profiling_controller.h"
#include "internal/render_preview.h"

#include <ludus/foundation/base/config.h>
#include <ludus/foundation/profiling/integration.hpp>
#include <ludus/foundation/profiling/profiling.hpp>
#include <ludus/foundation/profiling/trace_system.hpp>
#include <ludus/foundation/time/time.hpp>

#include <QDir>
#include <QFile>
#include <QFileInfo>
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
    Array<profile::GpuTraceSample> Samples;
    Array<uint8> Bytes;
    profile::GpuTraceCoverage Coverage;
    QString Destination;
    QString Error;
    bool ShouldIncludeCpu = false;
    bool ShouldSerialize = true;
    bool IsSaved = false;

protected:
    void run() override
    {
        if (ShouldSerialize == true && profile::SerializePerfettoTrace(Bytes,
                                                                       {Samples.GetData(), Samples.GetSize()},
                                                                       Coverage,
                                                                       ShouldIncludeCpu) == false)
        {
            Error = QStringLiteral("No trace data was recorded, or serialization failed.");
            return;
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
    connect(preview, &QObject::destroyed, this, [this]() {
        mPreviewOrNull = nullptr;
        Stop();
    });
    connect(preview, &RenderPreview::GpuProfileSample, this, &ProfilingController::acceptSample);
    connect(preview, &RenderPreview::GpuProfileSubmission, this, [this]() {
        if (mState == ProfilingState::Recording && mIsStopping == false && mMode != ProfilingMode::Cpu)
        {
            ++mCoverage.EligibleFrames;
        }
    });
    connect(preview, &RenderPreview::DeviceStopping, this, [this]() {
        if (mState == ProfilingState::Recording && mMode != ProfilingMode::Cpu)
        {
            if (mPreviewOrNull->HasGpuProfilingPending() == true)
            {
                ++mCoverage.UnresolvedSamples;
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
    if (LUDUS_PROFILING_ENABLED == 0 || destination.isEmpty() == true || QFileInfo::exists(destination) == true ||
        (mode != ProfilingMode::Cpu && CanRecordGpu() == false))
    {
        mStatus = QStringLiteral("Recording unavailable: use a profiling-enabled build, a new output path, and "
                                 "an active GPU viewport for GPU/Both.");
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
    if (mode != ProfilingMode::Gpu && profile::BeginCapture(CPU_CAPTURE_CHUNKS) == false)
    {
        if (mPreviewOrNull != nullptr)
        {
            static_cast<void>(mPreviewOrNull->SetGpuProfilingEnabled(false));
        }
        mStatus = QStringLiteral("CPU recorder is busy or its bounded buffer could not be reserved.");
        Q_EMIT Changed();
        return false;
    }
    mMode = mode;
    mCoverage = { .IsGpuRequested = mode != ProfilingMode::Cpu };
    mStartedTicks = time::NowTicks();
    mLastStatusTicks = mStartedTicks;
    mDeviceSession = mPreviewOrNull != nullptr ? mPreviewOrNull->GetDeviceSerial() : 0;
    mDestination = QFileInfo(destination).absoluteFilePath();
    mIsStopping = false;
    mState = ProfilingState::Recording;
    mStatus = QStringLiteral("Recording (30 second limit). GPU durations use submission attribution.");
    if (mode != ProfilingMode::Gpu)
    {
        profile::RegisterThreadForTrace("Editor GUI");
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
            ++mCoverage.UnresolvedSamples;
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
    if (mSamples.TryAdd(sample) == false)
    {
        ++mCoverage.DroppedSamples;
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
        mStatus = QStringLiteral("Saved %1 GPU samples from %2 eligible frames; invalid %3, dropped %4, unresolved %5.")
                      .arg(mCompletedSampleCount)
                      .arg(mCoverage.EligibleFrames)
                      .arg(mCoverage.InvalidSamples)
                      .arg(mCoverage.DroppedSamples)
                      .arg(mCoverage.UnresolvedSamples);
        if (mMode == ProfilingMode::Cpu)
        {
            mStatus = QStringLiteral("Saved CPU recording.");
        }
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
               QStringLiteral(" Elapsed %1 s. CPU events %2, dropped %3; GPU samples %4 / %5 submitted frames.")
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
