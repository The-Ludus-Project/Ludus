#pragma once

#include <ludus/foundation/base/types.h>
#include <ludus/graphics/renderer/viewport.hpp>

#include "internal/browser_viewport_backend.h"
#include "internal/render_preview.h"

#include <QByteArray>
#include <QPointer>
#include <QSize>
#include <QTimer>
#include <QWidget>

namespace ludus::editor
{
/// Qt layout/input proxy for a separate engine-owned WebGPU or WebGL 2 canvas.
/// Qt continues to receive pointer/keyboard input through the transparent DOM overlay.
/// The preview/device is stopped before the canvas and its stable selector are removed.
/// Start tries WebGPU and, when it cannot start, WebGL 2 on a fresh canvas; the page's
/// `viewport-backend=webgpu|webgl2` query forces one API (see browser_viewport_backend.h).
class BrowserViewport final : public QWidget
{
public:
    BrowserViewport(RenderPreview& inoutPreview, QWidget* parentOrNull);
    ~BrowserViewport() override;
    /// Create the canvas and start the next API attempt. False only after reporting, through
    /// RenderPreview::ReportBrowserUnavailable, that no API can start (or once the preview is gone).
    [[nodiscard]] bool Start();
    void ShowFrame();
    void ClearFrame();
    /// Mirror the text Qt draws over the empty viewport into `#screen[data-viewport-prompt]`, so the page
    /// and browser tests read the same words, for example a scene import failure and its cause.
    void PublishPrompt(const QString& text);
    /// Request exactly this drawable from the preview instead of the proxy's physical pixels;
    /// an empty size follows the proxy again. The canvas still covers the proxy's rectangle.
    void SetFixedDrawableSize(const QSize& sizeOrEmpty);
    [[nodiscard]] foundation::uint64 GetPresentedFrames() const noexcept;

private:
    void service();
    void releaseFrame() noexcept;
    /// Record why api failed. Returns true when the policy retries on WebGL 2; otherwise reports
    /// the final failure through the preview and returns false.
    [[nodiscard]] bool recordStartupFailure(BrowserViewportApi api, const QString& reason);
    QPointer<RenderPreview> mPreview;
    QByteArray mSelector;
    graphics::renderer::ViewportFrame mFrame;
    QTimer mService;
    foundation::uint64 mPresentedFrames = 0;
    foundation::int32 mRequestedWidth = 0;
    foundation::int32 mRequestedHeight = 0;
    QSize mFixedDrawableSize;
    bool mHasFrame = false;
    bool mHasPresentedImage = false;
    bool mCanPresent = false;
    /// Startup policy of the current Start sequence and the API its next attempt uses.
    BrowserViewportBackend mRequested = BrowserViewportBackend::Automatic;
    BrowserViewportApi mNextApi = BrowserViewportApi::WebGpu;
    QString mWebGpuFailure;
    QString mWebGl2Failure;
    /// True while a WebGL 2 retry continues the current sequence after an asynchronous WebGPU failure.
    bool mIsRetrying = false;
};
} // namespace ludus::editor
