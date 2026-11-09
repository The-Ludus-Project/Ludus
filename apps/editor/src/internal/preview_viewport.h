#pragma once

#include "internal/preview_shading.h"
#include "internal/render_preview.h"
#include "internal/viewport_frame_source.h"
#include "internal/viewport_object_interaction.h"
#include "internal/viewport_size.h"
#if defined(Q_OS_MACOS)
#    include "internal/viewport_relative_mouse.h"
#endif

#include <ludus/foundation/base/compiler.h>
#include <ludus/foundation/containers/static_array.hpp>
#include <ludus/input/key.h>

#include <QElapsedTimer>
#include <QFocusEvent>
#include <QImage>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPointer>
#include <QResizeEvent>
#include <QTimer>
#include <QWheelEvent>
#include <QWidget>

QT_BEGIN_NAMESPACE
class QAction;
class QContextMenuEvent;
QT_END_NAMESPACE
namespace ludus::platform
{
class HostedWindowEndpoint;
}
namespace ludus::editor
{
class PreviewShaderReload;
class RayPathInspection;
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
/// Fit a status overlay's text to a viewport maxWidth logical pixels wide: each line wraps at spaces, and only a
/// single word wider than the panel is shortened in its middle, so a path keeps its file name and line.
/// @param text Overlay text; its own line breaks are kept.
/// @param maxWidth Viewport width in logical pixels; a non-positive width returns text unchanged.
/// @return The text with the added line breaks and shortened words.
[[nodiscard]] QString FitOverlayText(const QString& text, qreal maxWidth);
// One embedded presentation surface for both World bindings and cooked scenes.
// Frames come from a ViewportFrameSource; device admission and navigation stay with the RenderPreview.
class PreviewViewport final : public QWidget
{
    Q_OBJECT

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
    /// switch once that source has a pending frame. Null returns to the project's GPU display
    /// source when bound, otherwise to preview's own frames.
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
    /// Explore the currently presented World from spawn, at speed in metres per second.
    /// Existing right-drag navigation controls the camera; Escape or losing focus restores the editor view.
    [[nodiscard]] bool
    BeginFirstPerson(const gameplay::camera::CameraSample& spawn, foundation::float32 speed, QString& outError);
    void EndFirstPerson();
    /// Present the project's graph output using this viewport's scene, camera and device.
    /// Call on the GUI thread; workspace must outlive the binding. Native builds only.
    /// While this viewport's preview feeds the graph, it also presents the workspace's ray path
    /// inspection: picking a pixel takes the next left click on the graph output, and inspecting
    /// shows the preview's raster frames from the free camera with the frozen paths over them.
    /// Leaving inspection presents the live graph again. Choosing a debug view in the graph's
    /// output menu likewise presents the preview's raster frames in that shading until a graph
    /// output is chosen again.
    void UseProjectGraph(RendererWorkspace& inoutWorkspace);
    /// Whether this viewport presents a ray path inspection (raster frames and frozen paths).
    [[nodiscard]] LUDUS_INLINE bool IsInspectingRayPaths() const noexcept
    {
        return mIsInspectingRayPaths;
    }
    /// Authoring interaction controller; GUI thread only, owned by this viewport.
    [[nodiscard]] LUDUS_INLINE ViewportObjectInteraction& GetObjectInteraction() noexcept
    {
        return *mObjectInteraction;
    }
    /// Distinguish a World context click from right-drag navigation on this surface.
    void EnableWorldContextMenu() noexcept;
    /// View menu action "Set start view from this view". Hidden until an owner that can store a
    /// start view shows it and handles triggered, describing the view with
    /// RenderPreview::TryDescribeStartView. Owned by this viewport; GUI thread only.
    [[nodiscard]] QAction& GetStartViewSaveAction() noexcept;
    /// @brief Save the render result this viewport currently shows as a PNG file (View → Save screenshot…).
    ///
    /// The file holds the presented frame only, at the resolution it was rendered at: editor overlays
    /// (statistics, debug legend, axis widget, object handles) and letterbox bars are left out, and a
    /// translucent frame is composited over the viewport background. A CPU frame (readback canvas or CPU
    /// fallback) is written before this returns. A GPU frame is read back from Qt Quick when the viewport
    /// next draws, which needs a visible viewport; IsTakingScreenshot is true until then. The browser
    /// editor cannot read its canvas back and always fails. GUI thread only.
    /// @param path Destination file; empty asks for one with a save dialog once the image is read, and a
    /// write failure then offers to choose another location. A non-empty path is written without asking.
    /// @param outError Why no screenshot was started: no rendered frame is shown, the viewport cannot be
    /// read, or another screenshot is still being read. Unchanged when this returns true.
    /// @return True when the screenshot was written or its read-back started. A later failure is left in
    /// GetScreenshotError (and, for an empty path, shown in a message box).
    [[nodiscard]] bool SaveScreenshot(const QString& path, QString& outError);
    /// True while a screenshot started by SaveScreenshot waits for the viewport to draw.
    [[nodiscard]] bool IsTakingScreenshot() const noexcept;
    /// File the last screenshot was written to; empty before one succeeded.
    [[nodiscard]] const QString& GetScreenshotPath() const noexcept;
    /// Why the last started screenshot failed; empty while it is pending, after success or a cancelled dialog.
    [[nodiscard]] const QString& GetScreenshotError() const noexcept;
    /// Retain the last shown frame while an import candidate awaits publication.
    void HoldFrame();
    void ShowFrame();
    void ClearFrame(const QString& text);
    /// Whether a delegated graph image remains displayed, including after its device stops.
    /// GUI thread only. Native graph viewports only; false before the first image or after ClearFrame.
    [[nodiscard]] bool HasRetainedGraphFrame() const noexcept;
    /// GUI thread only: serial of the graph snapshot currently sampled by the native scene graph.
    /// Zero before its first GPU image, for another source, or for CPU/browser presentation.
    [[nodiscard]] foundation::uint64 GetPresentedGraphFrameSerial() const noexcept;
    /// True while the axis orientation widget is drawn: a frame is on screen and the game is not playing.
    [[nodiscard]] bool IsOrientationWidgetShown() const noexcept;
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

Q_SIGNALS:
    /// Right click on the edit surface or a keyboard context request. Entity ID is empty on the background.
    void WorldContextMenuRequested(const QPoint& positionGlobal, const QString& entityId);

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;
    void contextMenuEvent(QContextMenuEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void leaveEvent(QEvent* event) override;
    void wheelEvent(QWheelEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;
    void keyReleaseEvent(QKeyEvent* event) override;
    void focusOutEvent(QFocusEvent* event) override;
    void focusInEvent(QFocusEvent* event) override;
    void hideEvent(QHideEvent* event) override;
    void showEvent(QShowEvent* event) override;
    bool event(QEvent* event) override;
    void paintEvent(QPaintEvent* event) override;

private:
    struct State;
    foundation::UniquePtr<State> mState;
    ViewportObjectInteraction* mObjectInteraction;
    void updateObjectInteractionAvailability();
    /// Open hand over a transform handle, closed hand while dragging one; the default cursor otherwise.
    void updateGizmoCursor();
    bool mHasGizmoCursor = false;
    /// Write a read screenshot to the requested path, or ask for one; reports failures as SaveScreenshot says.
    void finishScreenshot(const QImage& image);
    void failScreenshot(const QString& error);
    /// Ask where to save a screenshot; empty when the dialog was cancelled.
    [[nodiscard]] QString askScreenshotPath();
    /// Point object selection at the camera of the shown project graph frame, or the live camera.
    void applyPresentedView();
    /// Draw the selection outline on the GPU into the shown graph frame while that is possible, and on the CPU in the
    /// overlay otherwise; keeps the GPU request in step with the selection and its drag preview.
    void updateGpuSelectionOutline();
    [[nodiscard]] QRectF getInteractionFrame() const;
    [[nodiscard]] QPointF getInteractionPosition(QPointF globalPosition) const;
    /// Snap the camera to the axis view of the orientation handle under globalPosition. False when the
    /// widget is hidden or the point misses every handle.
    [[nodiscard]] bool tryPickOrientationWidget(QPointF globalPosition);
    /// Open the Error List when globalPosition hits the build-failure chip over the frame. False otherwise.
    [[nodiscard]] bool tryActivateProblemChip(QPointF globalPosition);
    void resizeSurface();
    /// Letterbox the frame and resize the drawable for the preview's current size preset.
    void applyViewportSize();
    void setRenderPresentationVisible(bool isVisible) noexcept;
    /// Size preset the frame layout currently uses; differs from the preview's until applied.
    foundation::usize mAppliedViewportSize = VIEWPORT_SIZE_PRESET_COUNT;
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
    foundation::float32 mFirstPersonSpeed = 0;
    bool mShouldJump = false;
    bool mIsFlying = false;
    bool mHasWorldContextMenu = false;
    bool mHasContextPress = false;
    QPointF mContextPressGlobal;
    void beginFlyNavigation(QPointF positionGlobal, Qt::KeyboardModifiers modifiers);
    bool mHasMouseCapture = false;
    void stopNavigation();
    void advanceNavigation();
    void recenterPointer();
    [[nodiscard]] bool navigateMouse(foundation::float32 deltaX,
                                     foundation::float32 deltaY,
                                     Qt::MouseButtons buttons,
                                     Qt::KeyboardModifiers modifiers);
    [[nodiscard]] foundation::float32 getDrawableHeight() const noexcept;
    /// Ray path inspection of the bound project graph; null without one.
    QPointer<RayPathInspection> mRayPaths;
    RendererWorkspace* mRayPathWorkspaceOrNull = nullptr;
    bool mIsInspectingRayPaths = false;
    /// Whether the status overlay shows the inspection's text, which must be cleared when it ends.
    bool mIsShowingRayPathStatus = false;
    /// Follow the inspection stage: switch presentation and the picking cursor.
    void updateRayPathInspection();
    /// The graph output menu selects a built-in debug view, drawn by the preview instead of the graph.
    bool mIsShowingGraphDebugView = false;
    /// Follow the graph output menu's debug view: preview shading and presentation.
    void updateGraphDebugView();
    /// The preview's World is loaded but has no objects, so the graph has nothing to extract; the
    /// preview's built-in view draws the empty World (its grid) instead of the previous World's image.
    bool mIsShowingEmptyWorld = false;
    /// Follow whether the preview's World is empty: preview built-in view and presentation.
    void updateEmptyWorldView();
    /// Whether the preview's raster frames, not the graph's, are presented (inspection, a debug view
    /// or an empty World).
    [[nodiscard]] LUDUS_INLINE bool isShowingBuiltInView() const noexcept
    {
        return mIsInspectingRayPaths == true || mIsShowingGraphDebugView == true || mIsShowingEmptyWorld == true;
    }
    /// Present the preview's or the graph's frames after isShowingBuiltInView changed from wasShowing.
    void applyBuiltInPresentation(bool wasShowing);
    /// Select the edit frame source a newly created GPU window presents: the preview's frames while
    /// isShowingBuiltInView, otherwise the project graph's. A play source keeps the window.
    void applyEditFrameSource();
    /// Hand the inspection's current lines to the preview.
    void updateRayPathOverlay();
    /// Pick the clicked output pixel while the inspection waits for one.
    /// @return True when the click was taken by picking.
    [[nodiscard]] bool pickRayPathPixel(QPointF globalPosition);
};
} // namespace ludus::editor
