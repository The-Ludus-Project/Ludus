#pragma once

#include <ludus/foundation/base/compiler.h>
#include <ludus/foundation/base/types.h>

#include "internal/viewport_reference_capture.h"

#include <QElapsedTimer>
#include <QImage>
#include <QPointer>
#include <QString>
#include <QTimer>
#include <QWidget>

class QComboBox;
class QLabel;
class QProgressBar;
class QPushButton;
class QSpinBox;

namespace ludus::editor
{
class RenderPreview;

/// Wall time one service tick may spend preparing or tracing before yielding to the event loop.
inline constexpr foundation::uint64 REFERENCE_SERVICE_BUDGET_NANOSECONDS = 8'000'000;
/// Shortest interval between progressive display refreshes while tracing.
inline constexpr foundation::int32 REFERENCE_DISPLAY_REFRESH_MILLISECONDS = 250;

/// Non-modal window that renders an RT1 reference image of one viewport's current camera.
///
/// Capture freezes the viewport's scene and camera at the click; later navigation or edits do
/// not change the running capture. Work runs on the GUI thread in short service ticks. The
/// capture is released before the viewport's device stops. GUI thread only.
class ReferenceCaptureWindow final : public QWidget
{
    Q_OBJECT
public:
    /// preview is borrowed; the window releases its capture when the preview stops its device.
    explicit ReferenceCaptureWindow(RenderPreview* preview, QWidget* parentOrNull = nullptr);
    ~ReferenceCaptureWindow() override;

    /// Start a capture with the settings shown in the window. False reports why in the status.
    bool StartCapture();
    /// Cancel the running capture, keeping nothing.
    void StopCapture();
    /// Write the current capture to hdrPath and a display PNG beside it.
    [[nodiscard]] bool SaveCapture(const QString& hdrPath, QString& outError) const;
    [[nodiscard]] LUDUS_INLINE const ViewportReferenceCapture& GetCapture() const noexcept
    {
        return mCapture;
    }
    /// Most recent progressive display image; null before the first samples.
    [[nodiscard]] LUDUS_INLINE const QImage& GetImage() const noexcept
    {
        return mImage;
    }
    [[nodiscard]] QString GetStatusText() const;

Q_SIGNALS:
    /// The capture completed, failed or was stopped.
    void CaptureFinished();

protected:
    void resizeEvent(QResizeEvent* event) override;

private:
    void service();
    void refreshImage();
    void updateControls();
    void updateStatus();
    void updateTraversalAvailability();
    void saveWithDialog();
    void showScaledImage();
    QPointer<RenderPreview> mPreview;
    ViewportReferenceCapture mCapture;
    QTimer mServiceTimer;
    QElapsedTimer mRefreshTimer;
    QElapsedTimer mCaptureTimer;
    QImage mImage;
    QString mMessage;
    // Child widgets, created in the constructor and owned by this window.
    QSpinBox* const mLongSide;
    QSpinBox* const mSamples;
    QSpinBox* const mMaxDepth;
    QComboBox* const mTraversal;
    QPushButton* const mCaptureButton;
    QPushButton* const mStopButton;
    QPushButton* const mSaveButton;
    QProgressBar* const mProgress;
    QLabel* const mStatus;
    QLabel* const mImageLabel;
};
} // namespace ludus::editor
