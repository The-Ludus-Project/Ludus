#pragma once

#include <ludus/foundation/profiling/perfetto_export.hpp>
#include <ludus/graphics/rhi/gpu_scope.h>

#include "internal/viewport_trace.h"

#include <QObject>
#include <QString>
#include <QTimer>

class QThread;

namespace ludus::editor
{
class RenderPreview;
class ProfilingExportJob;
/// Own names until export completes; borrowed sample views are rebuilt after storage moves.
struct RecordedGpuSample final
{
    foundation::profiling::GpuTraceSample Sample{};
    graphics::rhi::GpuScopeDescription Scope{};
};
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
/// GPU data comes from the viewport's existing sampler, never a second query owner.
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
    ProfilingExportJob* mJobOrNull = nullptr;
    QThread* mCpuStopOrNull = nullptr;
    QTimer mTimer;
    ProfilingMode mMode = ProfilingMode::Cpu;
    ProfilingState mState = ProfilingState::Idle;
    foundation::Array<RecordedGpuSample> mSamples;
    foundation::Array<foundation::uint8> mSnapshot;
    foundation::profiling::GpuTraceCoverage mCoverage;
    ViewportTraceSnapshot mViewportSnapshot;
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
