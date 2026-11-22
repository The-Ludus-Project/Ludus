#pragma once

#include "internal/render_scene.h"
#include "internal/viewport_graph_capture.h"
#include "internal/viewport_reference_capture.h"

#include <ludus/foundation/base/pointer.hpp>

#include <QFutureWatcher>
#include <QPointer>
#include <QProcess>
#include <QTimer>
#include <QWidget>

class QLabel;
class QPushButton;
class QSlider;
class QSpinBox;
class QTemporaryDir;

namespace ludus::editor
{
class CapturedImageView;
class RenderGraphPreview;
class RenderPreview;
class WorldEarthTimeField;

/// Capture settings copied from Reference when a time-lapse opens.
struct CelestialTimeLapseSettings final
{
    ViewportReferenceSettings Reference;
    ViewportRasterSource Graph;
    QString GraphProfile;
    QString SourceLabel;
    QSize Extent;
    graphics::renderer::ViewDescription Exposure;
    ViewportDisplayTransform Display;
    foundation::float32 ToneMappingExposure = 0;
    bool ShouldMeterAtmosphere = false;
    bool ShouldCaptureGraph = false;
};

/// Sample both endpoints exactly. Invalid input or a reversed range preserves outUtcDays.
[[nodiscard]] bool TryGetCelestialFrameTime(foundation::float64 startUtcDays,
                                            foundation::float64 endUtcDays,
                                            foundation::int32 frame,
                                            foundation::int32 frameCount,
                                            foundation::float64& outUtcDays) noexcept;

/// Desktop Reference companion. Owns a frozen scene and selected graph, one offscreen frame at a time
/// and the staged MP4 until the encoder completes. All renderer calls belong to the GUI thread.
class CelestialTimeLapseWindow final : public QWidget
{
public:
    CelestialTimeLapseWindow(RenderPreview& inoutPreview,
                             RenderGraphPreview* liveGraphOrNull,
                             const CelestialTimeLapseSettings& settings,
                             QWidget& inoutParent);
    ~CelestialTimeLapseWindow() override;

private:
    enum class Phase : foundation::uint8
    {
        Idle,
        Scrubbing,
        ProbingEncoder,
        Rendering,
        Writing,
        Encoding,
        Cancelling,
    };

    [[nodiscard]] bool pinGraph(QString& outError);
    void requestScrub();
    void startScrub();
    void startExport();
    void startRendering();
    [[nodiscard]] bool startFrame(foundation::float64 utcDays, bool isScrub, QString& outError);
    void startService();
    void service();
    void finishFrame(const QImage& image);
    void writeFinished();
    void startEncoder();
    void encoderFinished(foundation::int32 exitCode, QProcess::ExitStatus exitStatus);
    void releaseFrame() noexcept;
    void cancel(const QString& message);
    void finish(const QString& message);
    void updateControls();
    [[nodiscard]] foundation::int32 frameCount() const;
    [[nodiscard]] bool isExporting() const noexcept;

    QPointer<RenderPreview> mPreview;
    QPointer<RenderGraphPreview> mLiveGraph;
    RenderScene mScene;
    CelestialTimeLapseSettings mSettings;
    ViewportReferenceCapture mReference;
    ViewportGraphCapture mGraph;
    ViewportDisplayTransform mFrameDisplay;
    foundation::core::UniquePtr<QTemporaryDir> mPinnedGraph;
    foundation::core::UniquePtr<QTemporaryDir> mStaging;
    QFutureWatcher<QString> mWriter;
    QProcess mEncoder;
    QTimer mService;
    QTimer mScrubDelay;
    Phase mPhase = Phase::Idle;
    foundation::uint64 mRequestedScrub = 0;
    foundation::uint64 mRunningScrub = 0;
    foundation::float64 mStartUtcDays = 0;
    foundation::float64 mEndUtcDays = 0;
    foundation::int32 mExportFrame = 0;
    foundation::int32 mExportFrameCount = 0;
    QSize mExportExtent;
    QString mDestination;
    QString mEncoderPath;
    QString mSetupError;
    QString mRequestedInstant;
    QString mCancelMessage;
    WorldEarthTimeField* mStartOrNull = nullptr;
    WorldEarthTimeField* mEndOrNull = nullptr;
    QSpinBox* const mDuration;
    QSpinBox* const mFrameRate;
    QSlider* const mTimeline;
    QLabel* const mInstant;
    QLabel* const mStatus;
    QPushButton* const mExport;
    QPushButton* const mStop;
    CapturedImageView* const mImage;
};
} // namespace ludus::editor
