#pragma once

#include <ludus/foundation/profiling/perfetto_export.hpp>

#include <QObject>
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
/// GPU data comes from the viewport's existing sampler, never a second query owner.
class ProfilingController final : public QObject
{
    Q_OBJECT
public:
    explicit ProfilingController(RenderPreview* preview, QObject* parentOrNull = nullptr);
    ~ProfilingController() override;
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
    void poll();
    void beginExport();
    void finishExport();
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
    foundation::Array<foundation::profiling::GpuTraceSample> mSamples;
    foundation::Array<foundation::uint8> mSnapshot;
    foundation::profiling::GpuTraceCoverage mCoverage;
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
