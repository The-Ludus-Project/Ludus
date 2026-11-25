#pragma once

#include <ludus/foundation/profiling/perfetto_export.hpp>
#include <ludus/graphics/rhi/gpu_scope.h>

#include "internal/hosted_profiling_session.h"
#include "internal/viewport_trace.h"

#include <QJsonArray>
#include <QObject>
#include <QPointer>
#include <QString>
#include <QTimer>

class QThread;

namespace ludus::editor
{
class RenderPreview;
class ProfilingExportJob;
enum class ProfilingMode : foundation::uint8
{
    Cpu,
    Gpu,
    Both,
};
enum class ProfilingState : foundation::uint8
{
    Idle,
    Recording,
    Finalizing,
    Complete,
    Failed,
};
/// GUI-thread recording coordinator. One owner serializes tracer lifecycle and frozen export.
/// GPU data comes from the viewport's existing sampler, never a second query owner. While a
/// game plays in the World viewport, GPU records the game's own passes through its hosted
/// recorder instead (HostedProfilingSession) and exports them as the game's separate trace.
class ProfilingController final : public QObject
{
    Q_OBJECT
public:
    explicit ProfilingController(RenderPreview* preview, QObject* parentOrNull = nullptr);
    ~ProfilingController() override;
    /// Borrows the selected viewport on the GUI thread; destruction stops a recording.
    /// Returns false when changing sources during recording/finalization, preserving pending timing.
    /// @pre preview is nonnull.
    [[nodiscard]] bool SetPreviewSource(RenderPreview* preview);
    /// @brief Choose the hosted game GPU recordings target, or none.
    /// @param targetOrNull The game playing in the World viewport; null when none plays.
    /// @param lifetimeOrNull QObject whose destruction ends the target; required with a target.
    /// @return false when changing targets during recording/finalization; the active recording keeps its game.
    [[nodiscard]] bool SetHostedTarget(HostedRecordingTarget* targetOrNull, QObject* lifetimeOrNull);
    /// What GPU would record now: the editor viewport or the hosted game, with its process.
    [[nodiscard]] QString GetTargetDescription() const;
    /// GPU samples admitted by the active recording, or by the last one once it finalized.
    [[nodiscard]] foundation::usize GetRecordedSampleCount() const noexcept;
    /// The game recorded by the active or last hosted recording; empty before one.
    [[nodiscard]] const HostedProducerRecord& GetHostedProducer() const noexcept;
    [[nodiscard]] bool Start(ProfilingMode mode, const QString& destination);
    void Stop();
    /// Retry writing the retained serialized snapshot after an I/O error.
    [[nodiscard]] bool RetrySave(const QString& destination);
    /// Explicitly discard a failed frozen snapshot; does not delete successful trace files.
    void Discard();
    [[nodiscard]] ProfilingState GetState() const noexcept;
    [[nodiscard]] QString GetStatus() const;
    [[nodiscard]] QString GetLastPath() const;
    [[nodiscard]] bool HasUnsavedSnapshot() const noexcept;
    [[nodiscard]] bool CanRecordGpu() const noexcept;
    static constexpr foundation::usize MAX_GPU_SAMPLES = 4096;

Q_SIGNALS:
    void Changed();

private:
    void connectPreview();
    void poll();
    void beginExport();
    void finishExport();
    [[nodiscard]] bool isHostedConnected() const noexcept;
    [[nodiscard]] bool startHosted(const QString& destination);
    void pollHosted();
    void acceptPassBatch(foundation::uint64 frameId,
                         foundation::uint64 deviceSession,
                         foundation::uint64 submissionTicks,
                         const graphics::rhi::GpuPassTimings& batch);
    void acceptSample(foundation::uint64 frameId,
                      foundation::uint64 deviceSession,
                      foundation::uint64 submissionTicks,
                      foundation::uint64 durationNanoseconds,
                      bool isValid);
    RenderPreview* mPreviewOrNull;
    HostedRecordingTarget* mHostedTargetOrNull = nullptr;
    QPointer<QObject> mHostedLifetime;
    HostedProfilingSession mHosted;
    HostedProducerRecord mHostedProducer;
    bool mIsHosted = false;
    ProfilingExportJob* mJobOrNull = nullptr;
    QThread* mCpuStopOrNull = nullptr;
    QTimer mTimer;
    ProfilingMode mMode = ProfilingMode::Cpu;
    ProfilingState mState = ProfilingState::Idle;
    foundation::Array<RecordedGpuSample> mSamples;
    foundation::Array<foundation::uint8> mSnapshot;
    foundation::profiling::GpuTraceCoverage mCoverage;
    ViewportTraceSnapshot mViewportSnapshot;
    QJsonArray mGraphFrames;
    foundation::usize mCompletedSampleCount = 0;
    foundation::uint64 mStartedTicks = 0;
    foundation::uint64 mStoppedTicks = 0;
    foundation::uint64 mLastStatusTicks = 0;
    foundation::uint64 mDeviceSession = 0;
    bool mIsStopping = false;
    QString mDestination;
    QString mStatus;
    QString mLastPath;
};
} // namespace ludus::editor
