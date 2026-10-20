#pragma once

#include "internal/render_preview.h"
#include "internal/viewport_frame_source.h"
#if defined(Q_OS_MACOS)
#    include "internal/viewport_relative_mouse.h"
#endif

#include <ludus/foundation/containers/static_array.hpp>
#include <ludus/input/key.h>

#include <QElapsedTimer>
#include <QFocusEvent>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QResizeEvent>
#include <QTimer>
#include <QWheelEvent>
#include <QWidget>

namespace ludus::platform
{
class HostedWindowEndpoint;
}
namespace ludus::editor
{
class PreviewShaderReload;
class RendererWorkspace;
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
// Frames come from a ViewportFrameSource; device admission and navigation stay with the RenderPreview.
class PreviewViewport final : public QWidget
{
public:
    /// Present preview's own frames.
    explicit PreviewViewport(RenderPreview* preview, QWidget* parentOrNull = nullptr, bool isAutomatic = true);
    /// Present frames leased from frameSource, whose images live on preview's admitted device.
    /// preview still admits the device and receives navigation. frameSource must emit
    /// ResettingFrames before it is destroyed and before preview's device stops, so that no
    /// lease this viewport holds outlives its image. The browser canvas presents preview directly.
    PreviewViewport(RenderPreview* preview,
                    ViewportFrameSource* frameSource,
                    QWidget* parentOrNull = nullptr,
                    bool isAutomatic = true);
    ~PreviewViewport() override;
    /// Present frames from another source from now on. Every read of the current source is
    /// ended first, so its last image disappears when the next source's first frame is shown;
    /// switch once that source has a pending frame. Null returns to preview's own frames.
    /// The new source has the same contract as the constructor's. Native builds only; the
    /// browser canvas always presents preview.
    void SetFrameSource(ViewportFrameSource* frameSourceOrNull);
    /// The source this viewport presents.
    [[nodiscard]] ViewportFrameSource& GetFrameSource() const noexcept;
    /// @brief Whether a hosted game's images can be shown here.
    ///
    /// True while the viewport presents through its shared Qt device and that device can import
    /// them (Metal, or Vulkan with `VK_KHR_external_memory_fd`); false for the readback canvas.
    [[nodiscard]] bool CanImportHostedImages() const noexcept;
    /// @brief Make this viewport the window of a game playing in it, or return it to navigation.
    ///
    /// While set, the viewport acts as the game's window: keyboard focus becomes FocusEntered and
    /// FocusLost, keys go to the game by physical key (Platform's native key maps), editor
    /// shortcuts do not take them, and showing or hiding reports Visible or Occluded. Two chords
    /// stay with the editor: Shift+F6 (Stop) and Shift+F1, which gives the keyboard back. Mouse
    /// input does not navigate. Platform has no pointer input yet, so the game receives none.
    /// @param endpointOrNull The game's hosted window, borrowed until replaced; null ends forwarding.
    void SetPlayInput(platform::HostedWindowEndpoint* endpointOrNull);
    /// Present the project's graph output using this viewport's scene, camera and device.
    /// Call on the GUI thread; workspace must outlive the binding. Native builds only.
    void UseProjectGraph(RendererWorkspace& inoutWorkspace);
    /// Retain the last shown frame while an import candidate awaits publication.
    void HoldFrame();
    void ShowFrame();
    void ClearFrame(const QString& text);
    /// Whether a delegated graph image remains displayed, including after its device stops.
    /// GUI thread only. Native graph viewports only; false before the first image or after ClearFrame.
    [[nodiscard]] bool HasRetainedGraphFrame() const noexcept;
    /// Text of the GPU/CPU statistics overlay as currently shown; empty while Stats is off.
    [[nodiscard]] QString GetStatisticsText() const;
    /// Live shader status shown below the statistics (reload notices and errors); empty when none.
    [[nodiscard]] QString GetShaderStatusText() const;
    /// Live shader controller behind the toolbar's "Live shaders" toggle; null in the browser
    /// editor, which has no compiler worker or shader source tree.
    [[nodiscard]] PreviewShaderReload* GetShaderReloadOrNull() const noexcept;
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
    void focusInEvent(QFocusEvent* event) override;
    void hideEvent(QHideEvent* event) override;
    void showEvent(QShowEvent* event) override;
    bool event(QEvent* event) override;

private:
    struct State;
    foundation::UniquePtr<State> mState;
    void resizeSurface();
    /// Reformat the statistics overlay from RenderPreview and push it to the active presentation.
    void updateStatistics();
    /// Copy the live shader status from its controller and push it to the active presentation.
    void updateShaderStatus();
    RenderPreview* Preview_;
    ViewportFrameSource* mFrameSource;
    /// The current source's connections, replaced by SetFrameSource.
    QMetaObject::Connection mResettingFramesConnection;
    QMetaObject::Connection mFrameReadyConnection;
    bool mIsAutomatic = true;
    void connectFrameSource();
    platform::HostedWindowEndpoint* mPlayEndpointOrNull = nullptr;
    /// Keys the game has seen pressed and not released, by input::KeyIndex.
    foundation::StaticArray<bool, input::KEY_COUNT> mPlayHeldKeys{};
    [[nodiscard]] bool handlePlayKey(QKeyEvent& inoutEvent, bool isPress);
    void sendPlayFocus(bool isFocused);
#if defined(Q_OS_MACOS)
    friend class ViewportRelativeMouse;
    ViewportRelativeMouse mRelativeMouse;
#endif
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
    [[nodiscard]] bool navigateMouse(foundation::float32 deltaX,
                                     foundation::float32 deltaY,
                                     Qt::MouseButtons buttons,
                                     Qt::KeyboardModifiers modifiers);
    [[nodiscard]] foundation::float32 getDrawableHeight() const noexcept;
};
} // namespace ludus::editor
