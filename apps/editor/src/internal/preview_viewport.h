#pragma once

#include "internal/render_preview.h"

#include <ludus/foundation/containers/static_array.hpp>

#include <QElapsedTimer>
#include <QFocusEvent>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QResizeEvent>
#include <QTimer>
#include <QWheelEvent>
#include <QWidget>

namespace ludus::editor
{
/// How the viewport shows engine frames in this process.
enum class ViewportPresentation : foundation::uint8
{
    /// Qt Quick and the engine share one GPU device: Qt's Metal device, or on Linux a Vulkan
    /// device the editor creates on Qt's instance and Qt Quick adopts (any Qt 6 release).
    SharedDevice,
    /// Engine WebGPU canvas positioned over the Qt layout/input proxy, without pixel readback.
    BrowserCanvas,
    /// The engine owns a headless Vulkan device and the viewport paints double-buffered CPU copies.
    /// Used when forced with LUDUS_EDITOR_VIEWPORT=offscreen or when no shareable device presents.
    OffscreenReadback,
    /// Neither path exists on this platform; the viewport only explains why.
    Unavailable,
};
// One embedded presentation surface for both World bindings and cooked scenes.
class PreviewViewport final : public QWidget
{
public:
    explicit PreviewViewport(RenderPreview* preview, QWidget* parentOrNull = nullptr, bool isAutomatic = true);
    ~PreviewViewport() override;
    void ShowFrame();
    void ClearFrame(const QString& text);
    /// Text of the GPU/CPU statistics overlay as currently shown; empty while Stats is off.
    [[nodiscard]] QString GetStatisticsText() const;
    [[nodiscard]] foundation::uint64 PresentedFrames() const noexcept;
    /// True when either the shared-device or the offscreen readback path can show frames.
    [[nodiscard]] static bool CanPresent() noexcept;
    /// Select the presentation path from the Qt version and platform plugin. Setting the
    /// environment variable LUDUS_EDITOR_VIEWPORT=offscreen forces the readback path on Linux.
    [[nodiscard]] static ViewportPresentation GetPresentation() noexcept;

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void wheelEvent(QWheelEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;
    void keyReleaseEvent(QKeyEvent* event) override;
    void focusOutEvent(QFocusEvent* event) override;
    void hideEvent(QHideEvent* event) override;

private:
    struct State;
    foundation::UniquePtr<State> mState;
    void resizeSurface();
    /// Reformat the statistics overlay from RenderPreview and push it to the active presentation.
    void updateStatistics();
    RenderPreview* Preview_;
    static constexpr foundation::usize MOVEMENT_KEY_COUNT = 7;
    foundation::StaticArray<bool, MOVEMENT_KEY_COUNT> mHeldKeys{};
    QPointF mLastPositionGlobal;
    QPoint mCaptureStartGlobal;
    bool mCanWarpPointer = false;
    QTimer mNavigationTimer;
    QElapsedTimer mNavigationClock;
    foundation::float32 mSpeedMultiplier = 1;
    bool mIsFlying = false;
    bool mHasMouseCapture = false;
    void stopNavigation();
    void advanceNavigation();
    void recenterPointer();
    [[nodiscard]] foundation::float32 getDrawableHeight() const noexcept;
};
} // namespace ludus::editor
