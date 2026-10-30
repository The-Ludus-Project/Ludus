#pragma once

#include <ludus/foundation/base/compiler.h>
#include <ludus/foundation/base/types.h>

#include "internal/reference_comparison.h"
#include "internal/viewport_reference_capture.h"
#if defined(LUDUS_EDITOR_NATIVE_PREVIEW)
#    include "internal/viewport_graph_capture.h"
#    include "internal/viewport_raster_capture.h"
#endif

#include <QElapsedTimer>
#include <QImage>
#include <QPointer>
#include <QString>
#include <QTimer>
#include <QWidget>

class QCheckBox;
class QComboBox;
class QLabel;
class QProgressBar;
class QPushButton;
class QSpinBox;
class QTabWidget;

namespace ludus::editor
{
class RenderGraphPreview;
class RenderPreview;
class RendererWorkspace;

/// Wall time one service tick may spend preparing or tracing before yielding to the event loop.
inline constexpr foundation::uint64 REFERENCE_SERVICE_BUDGET_NANOSECONDS = 8'000'000;
/// Shortest interval between progressive display refreshes while tracing.
inline constexpr foundation::int32 REFERENCE_DISPLAY_REFRESH_MILLISECONDS = 250;

/// Source index of the RT1 progressive reference in the window's Source choice.
inline constexpr foundation::int32 CAPTURE_SOURCE_REFERENCE_INDEX = 0;
/// Source index of the viewport's render graph in the window's Source choice.
inline constexpr foundation::int32 CAPTURE_SOURCE_GRAPH_INDEX = 1;

/// Non-modal window that captures one viewport's current camera, either as an RT1 reference image or as the
/// viewport's render graph run to a target sample count. A reference can be compared with the viewport's
/// project-graph output at the same camera and extent: a converged graph capture when the output accumulates
/// ray samples, otherwise one raster frame. See docs/architecture/render-graph-frame-capture.md.
///
/// Capture freezes the viewport's scene and camera at the click; later navigation or edits do
/// not change the running capture. Work runs on the GUI thread in short service ticks. The
/// captures are released before the viewport's device stops. GUI thread only.
class ReferenceCaptureWindow final : public QWidget
{
    Q_OBJECT
public:
    /// preview is borrowed; the window releases its captures when the preview stops its device.
    explicit ReferenceCaptureWindow(RenderPreview* preview, QWidget* parentOrNull = nullptr);
    ~ReferenceCaptureWindow() override;

    /// Borrow the viewport's project renderer, whose live graph producer supplies the package, output and
    /// renderer choice that the Render graph source and the comparison render. The window names that graph,
    /// configuration and output, and follows changes to them. Null (and browser builds) disables both.
    void SetProjectGraph(RendererWorkspace* workspaceOrNull);
    /// Start a capture with the settings shown in the window. False reports why in the status.
    bool StartCapture();
    /// Cancel the running captures, keeping nothing.
    void StopCapture();
    /// Write the current capture to hdrPath and a display PNG beside it: a LUHDR01 reference, or a `.lurgcap`
    /// graph capture for the Render graph source. With a comparison, also write `<base>.raster.png`,
    /// `<base>.difference.png` and `<base>.comparison.json`, and for a graph comparison `<base>.graph.lurgcap`.
    [[nodiscard]] bool SaveCapture(const QString& hdrPath, QString& outError) const;
    [[nodiscard]] LUDUS_INLINE const ViewportReferenceCapture& GetCapture() const noexcept
    {
        return mCapture;
    }
#if defined(LUDUS_EDITOR_NATIVE_PREVIEW)
    /// The render graph capture: the Render graph source, or the converged side of a comparison.
    [[nodiscard]] LUDUS_INLINE const ViewportGraphCapture& GetGraphCapture() const noexcept
    {
        return mGraphCapture;
    }
    /// True when the running or last comparison compares a converged graph capture, not one raster frame.
    [[nodiscard]] LUDUS_INLINE bool IsGraphComparison() const noexcept
    {
        return mIsGraphComparison;
    }
#endif
    /// Most recent progressive display image; null before the first samples.
    [[nodiscard]] LUDUS_INLINE const QImage& GetImage() const noexcept
    {
        return mImage;
    }
    /// True once the raster output and some reference samples have been compared.
    [[nodiscard]] LUDUS_INLINE bool HasComparison() const noexcept
    {
        return mHasComparison;
    }
    /// Latest comparison; meaningful while HasComparison.
    [[nodiscard]] LUDUS_INLINE const ReferenceComparison& GetComparison() const noexcept
    {
        return mComparison;
    }
    /// Captured project-graph output as displayed; null without a completed raster capture.
    [[nodiscard]] LUDUS_INLINE const QImage& GetRasterImage() const noexcept
    {
        return mRasterImage;
    }
    /// Signed difference image of the latest comparison.
    [[nodiscard]] LUDUS_INLINE const QImage& GetDifferenceImage() const noexcept
    {
        return mDifferenceImage;
    }
    [[nodiscard]] QString GetStatusText() const;
    /// The graph, configuration and output that the Render graph source and the comparison would use now.
    [[nodiscard]] QString GetGraphText() const;
    /// Path Save writes beside hdrPath for one comparison artefact, such as "raster.png".
    [[nodiscard]] static QString GetComparisonPath(const QString& hdrPath, const QString& suffix);

Q_SIGNALS:
    /// The capture completed, failed or was stopped.
    void CaptureFinished();

protected:
    void resizeEvent(QResizeEvent* event) override;

private:
    void service();
    void refreshImage();
    void updateComparison();
    void updateControls();
    void updateStatus();
    void updateTraversalAvailability();
    void updateComparisonAvailability();
    void updateGraphDescription();
    void releaseCaptures() noexcept;
    void saveWithDialog();
    void showScaledImages();
    [[nodiscard]] bool isRasterRendering() const noexcept;
    [[nodiscard]] bool isGraphSource() const noexcept;
    [[nodiscard]] bool startRasterCapture(const QSize& requestedSize, QSize& outExtent, QString& outError);
    [[nodiscard]] bool startGraphCapture(const QSize& requestedSize, QSize& outExtent, QString& outError);
    [[nodiscard]] bool startGraphSource();
    [[nodiscard]] QString describeRaster() const;
    [[nodiscard]] bool saveComparison(const QString& hdrPath, QString& outError) const;
    QPointer<RenderPreview> mPreview;
    ViewportReferenceCapture mCapture;
#if defined(LUDUS_EDITOR_NATIVE_PREVIEW)
    QPointer<RendererWorkspace> mProjectWorkspace;
    QPointer<RenderGraphPreview> mProjectGraph;
    ViewportRasterCapture mRasterCapture;
    ViewportGraphCapture mGraphCapture;
    /// Package the current raster capture loaded, recorded with a saved comparison.
    ViewportRasterSource mRasterSource;
    /// The comparison side is a converged graph capture rather than one raster frame.
    bool mIsGraphComparison = false;
    /// Why a comparison fell back to one raster frame; empty for a graph comparison.
    QString mGraphFallbackReason;
#endif
    /// The running or last capture used the Render graph source.
    bool mIsCapturingGraph = false;
    QTimer mServiceTimer;
    QElapsedTimer mRefreshTimer;
    QElapsedTimer mCaptureTimer;
    QImage mImage;
    QImage mRasterImage;
    QImage mDifferenceImage;
    ReferenceComparison mComparison;
    QString mMessage;
    QString mComparisonError;
    bool mHasComparison = false;
    bool mIsComparing = false;
    bool mIsComparisonAvailable = false;
    // Child widgets, created in the constructor and owned by this window.
    QSpinBox* const mLongSide;
    QSpinBox* const mSamples;
    QSpinBox* const mMaxDepth;
    QComboBox* const mTraversal;
    QCheckBox* const mCompare;
    QPushButton* const mCaptureButton;
    QPushButton* const mStopButton;
    QPushButton* const mSaveButton;
    QProgressBar* const mProgress;
    QLabel* const mStatus;
    QLabel* const mComparisonText;
    QTabWidget* const mImages;
    QLabel* const mImageLabel;
    QLabel* const mRasterLabel;
    QLabel* const mDifferenceLabel;
    QComboBox* const mSource;
    QLabel* const mGraph;
};
} // namespace ludus::editor
