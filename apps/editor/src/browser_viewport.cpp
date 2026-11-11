#include "internal/browser_viewport.h"
#include "internal/viewport_object_interaction.h"

#include <ludus/foundation/base/core.h>
#include <ludus/foundation/containers/static_array.hpp>
#include <ludus/foundation/math/scalar.hpp>
#include <ludus/foundation/profiling/profiling.hpp>

#include <QApplication>
#include <QPoint>
#include <QRect>
#include <QScreen>
#include <QString>

#include <emscripten.h>

namespace ludus::editor
{
using namespace foundation;
namespace
{
constexpr int32 SERVICE_INTERVAL_MILLISECONDS = 16;
/// Longest page query read for the viewport-backend override, including the terminator.
constexpr usize MAX_PAGE_QUERY_BYTES = 2048;
uint32 NextCanvasIdentity = 1;
// Thanks to Qt Group, "Qt for WebAssembly", OpenGL and Windowing:
// https://doc.qt.io/qt-6.10/wasm.html
// Qt's widget canvas cannot share WebGPU resources. A separate DOM canvas follows
// the widget's screen rectangle; pointer-events:none preserves Qt's input routing.
// clang-format off
EM_JS(int32, CreateViewportCanvas, (const char* selector), {
    const screen = document.getElementById('screen');
    if (screen == null) return 0;
    const canvas = document.createElement('canvas');
    canvas.id = UTF8ToString(selector).slice(1);
    canvas.className = 'ludus-gpu-viewport';
    screen.dataset.viewportError = "";
    canvas.setAttribute('aria-hidden', 'true');
    const VIEWPORT_LAYER = 2147483647;
    canvas.style.cssText = 'position:fixed;pointer-events:none;display:none';
    canvas.style.zIndex = String(VIEWPORT_LAYER);
    screen.appendChild(canvas);
    // The selection overlay: a transparent 2D canvas on the same layer, later in the page so it draws on top.
    const overlay = document.createElement('canvas');
    overlay.id = canvas.id + '-overlay';
    overlay.className = 'ludus-gpu-viewport-overlay';
    overlay.setAttribute('aria-hidden', 'true');
    overlay.style.cssText = 'position:fixed;pointer-events:none;display:none';
    overlay.style.zIndex = String(VIEWPORT_LAYER);
    screen.appendChild(overlay);
    return 1;
});
EM_JS(void, SyncViewportCanvas, (const char* selector, int32 x, int32 y, int32 width, int32 height,
                                int32 screenWidth, int32 screenHeight, int32 isVisible, float64 presentedFrames,
                                int32 drawableWidth, int32 drawableHeight, int32 isOverlayShown,
                                int32 overlayWidth, int32 overlayHeight), {
    const canvas = document.querySelector(UTF8ToString(selector));
    const screen = document.getElementById('screen');
    if (canvas == null || screen == null) return;
    const bounds = screen.getBoundingClientRect();
    const scaleX = bounds.width / Math.max(1, screenWidth);
    const scaleY = bounds.height / Math.max(1, screenHeight);
    const overlay = document.querySelector(UTF8ToString(selector) + '-overlay');
    for (const layer of overlay == null ? [canvas] : [canvas, overlay]) {
        layer.style.left = (bounds.left + x * scaleX) + 'px';
        layer.style.top = (bounds.top + y * scaleY) + 'px';
        layer.style.width = (width * scaleX) + 'px';
        layer.style.height = (height * scaleY) + 'px';
    }
    canvas.style.display = isVisible ? 'block' : 'none';
    if (overlay != null) {
        // Resizing clears the overlay; its owner repaints it after a size change.
        if (overlay.width !== overlayWidth || overlay.height !== overlayHeight) {
            overlay.width = overlayWidth;
            overlay.height = overlayHeight;
        }
        overlay.style.display = isVisible && isOverlayShown ? 'block' : 'none';
    }
    // WebGL draws into the canvas's own width/height (WebGPU's surface configuration sets them).
    // Platform does not own this canvas, so its owner matches them to the requested drawable.
    if (canvas.dataset.api === 'webgl2' && (canvas.width !== drawableWidth || canvas.height !== drawableHeight)) {
        canvas.width = drawableWidth;
        canvas.height = drawableHeight;
    }
    const frameCount = String(presentedFrames);
    if (presentedFrames > 0 && canvas.dataset.frames != frameCount) {
        delete canvas.dataset.error;
        screen.dataset.viewportError = "";
    }
    canvas.dataset.frames = frameCount;
});
EM_JS(void, ReportViewportFailure, (const char* selector, const char* message), {
    const canvas = document.querySelector(UTF8ToString(selector));
    const text = UTF8ToString(message);
    if (canvas != null) canvas.dataset.error = text;
    const screen = document.getElementById('screen');
    if (screen != null) screen.dataset.viewportError = text;
});
EM_JS(void, RemoveViewportCanvas, (const char* selector), {
    document.querySelector(UTF8ToString(selector))?.remove();
    document.querySelector(UTF8ToString(selector) + '-overlay')?.remove();
});
// Clear the region painted last time, then copy straight-alpha RGBA8 pixels into the overlay canvas.
EM_JS(void, DrawViewportOverlay, (const char* selector, int32 clearX, int32 clearY, int32 clearWidth,
                                  int32 clearHeight, const uint8* pixels, int32 x, int32 y, int32 width,
                                  int32 height), {
    const overlay = document.querySelector(UTF8ToString(selector) + '-overlay');
    const context = overlay?.getContext('2d');
    if (context == null) return;
    if (clearWidth > 0 && clearHeight > 0) context.clearRect(clearX, clearY, clearWidth, clearHeight);
    if (width > 0 && height > 0) {
        const RGBA_BYTES = 4;
        const bytes = new Uint8ClampedArray(HEAPU8.buffer.slice(pixels, pixels + width * height * RGBA_BYTES));
        context.putImageData(new ImageData(bytes, width, height), x, y);
    }
});
EM_JS(void, MarkViewportApi, (const char* selector, int32 isWebGl2), {
    const canvas = document.querySelector(UTF8ToString(selector));
    if (canvas != null) canvas.dataset.api = isWebGl2 ? 'webgl2' : 'webgpu';
});
EM_JS(void, PublishViewportPrompt, (const char* message), {
    const screen = document.getElementById('screen');
    if (screen != null) screen.dataset.viewportPrompt = UTF8ToString(message);
});
EM_JS(int32, ReadPageQuery, (char* buffer, int32 capacity), {
    return stringToUTF8(location.search, buffer, capacity);
});
// clang-format on
} // namespace
BrowserViewport::BrowserViewport(RenderPreview& inoutPreview, QWidget* parentOrNull)
    : QWidget(parentOrNull), mPreview(&inoutPreview)
{
    setObjectName(QStringLiteral("LudusWebGpuViewport"));
    setAttribute(Qt::WA_TransparentForMouseEvents);
    mSelector = QByteArray("#ludus-gpu-viewport-") + QByteArray::number(NextCanvasIdentity++);
    mService.setInterval(SERVICE_INTERVAL_MILLISECONDS);
    connect(&mService, &QTimer::timeout, this, [this]() { service(); });
    connect(&inoutPreview, &RenderPreview::ResettingFrames, this, [this]() { releaseFrame(); });
    connect(&inoutPreview, &RenderPreview::Stopped, this, [this]() { ClearFrame(); });
    connect(&inoutPreview,
            &RenderPreview::BrowserApiFailed,
            this,
            [this](BrowserViewportApi api, const QString& reason) {
                RemoveViewportCanvas(mSelector.constData());
                if (recordStartupFailure(api, reason) == true && mPreview.isNull() == false)
                {
                    // RenderPreview stopped the failed device but keeps the start pending; continue it here
                    // once Tick has returned. Start reports its own failure, so false needs no handling.
                    mIsRetrying = true;
                    QTimer::singleShot(0, this, [this]() {
                        if (mPreview.isNull() == true || mPreview->Running() == false)
                        {
                            // The preview was stopped before the retry ran; nothing is waiting for it.
                            mIsRetrying = false;
                            return;
                        }
                        if (Start() == false)
                        {
                            hide();
                        }
                    });
                }
            });
    connect(&inoutPreview, &RenderPreview::Failed, this, [this](const QString& error) {
        const QByteArray message = error.toUtf8();
        ReportViewportFailure(mSelector.constData(), message.constData());
    });
}
BrowserViewport::~BrowserViewport()
{
    mService.stop();
    releaseFrame();
    if (mPreview.isNull() == false)
    {
        mPreview->Stop();
    }
    RemoveViewportCanvas(mSelector.constData());
}
bool BrowserViewport::Start()
{
    if (mPreview.isNull() == true)
    {
        return false;
    }
    if (mIsRetrying == false)
    {
        core::StaticArray<char, MAX_PAGE_QUERY_BYTES> query{};
        const int32 written = ReadPageQuery(query.GetData(), static_cast<int32>(query.GetSize()));
        mRequested = ParseBrowserViewportBackend(QString::fromUtf8(query.GetData(), math::Max(0, written)));
        mNextApi = GetFirstBrowserViewportApi(mRequested);
        mWebGpuFailure.clear();
        mWebGl2Failure.clear();
    }
    mIsRetrying = false;
    for (;;)
    {
        // Every attempt gets a canvas no graphics API has claimed: WebGPU's commits it.
        RemoveViewportCanvas(mSelector.constData());
        // The new overlay canvas is blank and unsized; the next service sizes and repaints it.
        mOverlaySizePhysical = QSize();
        const BrowserViewportApi api = mNextApi;
        if (CreateViewportCanvas(mSelector.constData()) == 0)
        {
            // Report it like any other startup failure, so a waiting scene import names the cause.
            if (recordStartupFailure(api, QStringLiteral("the viewport canvas could not be created")) == true)
            {
                continue;
            }
            return false;
        }
        mHasPresentedImage = false;
        // Mark before the first sync: a WebGL 2 canvas is sized there, and resizing clears it.
        MarkViewportApi(mSelector.constData(), api == BrowserViewportApi::WebGl2 ? 1 : 0);
        service();
        RenderPreview::BrowserDeviceRequest request;
        request.CanvasSelectorOrNull = mSelector.constData();
        request.Api = api;
        request.ApiNote = DescribeBrowserViewportApi(api, mWebGpuFailure);
        if (mPreview->AdmitBrowserDevice(request) == true)
        {
            mService.start();
            return true;
        }
        if (recordStartupFailure(api, mPreview->GetBrowserStartupFailure()) == false)
        {
            RemoveViewportCanvas(mSelector.constData());
            return false;
        }
    }
}
bool BrowserViewport::recordStartupFailure(BrowserViewportApi api, const QString& reason)
{
    (api == BrowserViewportApi::WebGl2 ? mWebGl2Failure : mWebGpuFailure) = reason;
    if (ShouldFallBackToWebGl2(mRequested, api, false) == true)
    {
        mNextApi = BrowserViewportApi::WebGl2;
        return true;
    }
    if (mPreview.isNull() == false)
    {
        mPreview->ReportBrowserUnavailable(
            DescribeBrowserViewportUnavailable(mRequested, mWebGpuFailure, mWebGl2Failure));
    }
    return false;
}
void BrowserViewport::ShowFrame()
{
    mCanPresent = true;
    service();
}
void BrowserViewport::ClearFrame()
{
    mCanPresent = false;
    mHasPresentedImage = false;
    releaseFrame();
    service();
}
void BrowserViewport::PublishPrompt(const QString& text)
{
    const QByteArray message = text.toUtf8();
    PublishViewportPrompt(message.constData());
}
void BrowserViewport::SetFixedDrawableSize(const QSize& sizeOrEmpty)
{
    mFixedDrawableSize = sizeOrEmpty;
    service();
}
uint64 BrowserViewport::GetPresentedFrames() const noexcept
{
    return mPresentedFrames;
}
void BrowserViewport::SetObjectInteraction(ViewportObjectInteraction& inoutInteraction)
{
    if (mInteraction.isNull() == false)
    {
        disconnect(mInteraction, nullptr, this, nullptr);
    }
    mInteraction = &inoutInteraction;
    mIsOverlayDirty = true;
    connect(&inoutInteraction, &ViewportObjectInteraction::Changed, this, [this]() { mIsOverlayDirty = true; });
}
void BrowserViewport::updateOverlay(bool isShown)
{
    if (isShown == false)
    {
        // The hidden canvas keeps its pixels; repaint them when it shows again.
        mIsOverlayDirty = true;
        return;
    }
    if (mIsOverlayDirty == false)
    {
        return;
    }
    mIsOverlayDirty = false;
    // The same logical frame the proxy's pointer input uses, so handles sit where they are picked.
    mInteraction->BuildOverlay(QRectF(QPointF(), QSizeF(size())), mOverlay);
    {
        LUDUS_PROFILE_SCOPE(EditorObjectOverlayPaint);
        if (mOverlayLayer.Update(mOverlay, mOverlaySizePhysical, devicePixelRatioF()) == false)
        {
            return;
        }
    }
    LUDUS_PROFILE_SCOPE(EditorObjectOverlayUpload);
    const QRect cleared = mOverlayLayer.GetClearedRegion();
    const QRect painted = mOverlayLayer.GetPaintedRegion();
    DrawViewportOverlay(mSelector.constData(),
                        cleared.x(),
                        cleared.y(),
                        cleared.width(),
                        cleared.height(),
                        mOverlayLayer.GetPixels(),
                        painted.x(),
                        painted.y(),
                        painted.width(),
                        painted.height());
}
void BrowserViewport::releaseFrame() noexcept
{
    if (mHasFrame == true && mPreview.isNull() == false)
    {
        mPreview->ReleaseFrame(mFrame);
    }
    mHasFrame = false;
}
void BrowserViewport::service()
{
    if (mPreview.isNull() == true)
    {
        mService.stop();
        RemoveViewportCanvas(mSelector.constData());
        return;
    }
    const bool isVisible = isVisibleTo(window()) == true && window()->isVisible() == true &&
                           QApplication::activePopupWidget() == nullptr && QApplication::activeModalWidget() == nullptr;
    mPreview->SetPresentationVisible(isVisible);
    const QScreen* screen = this->screen();
    LUDUS_REQUIRE(screen != nullptr);
    const QRect rectangleScreenLogical = screen->geometry();
    const QPoint positionScreenLogical = mapToGlobal(QPoint{}) - rectangleScreenLogical.topLeft();
    const float64 pixelRatio = devicePixelRatioF();
    const int32 widthPhysical = mFixedDrawableSize.isEmpty() == false
                                    ? mFixedDrawableSize.width()
                                    : math::Max(1, static_cast<int32>(static_cast<float64>(width()) * pixelRatio));
    const int32 heightPhysical = mFixedDrawableSize.isEmpty() == false
                                     ? mFixedDrawableSize.height()
                                     : math::Max(1, static_cast<int32>(static_cast<float64>(height()) * pixelRatio));
    if (widthPhysical != mRequestedWidth || heightPhysical != mRequestedHeight)
    {
        mRequestedWidth = widthPhysical;
        mRequestedHeight = heightPhysical;
        mPreview->Resize(widthPhysical, heightPhysical);
    }
    if (mCanPresent == true && mPreview->HasPendingFrame() == true)
    {
        graphics::renderer::ViewportFrame frame;
        graphics::renderer::ViewportImage image;
        if (mPreview->TakeFrame(frame, image) == true)
        {
            releaseFrame();
            mFrame = frame;
            mHasFrame = true;
            mHasPresentedImage = true;
            ++mPresentedFrames;
            // The camera may have moved with the frame; the overlay follows it.
            mIsOverlayDirty = true;
        }
    }
    const QSize overlaySizePhysical(math::Max(1, static_cast<int32>(static_cast<float64>(width()) * pixelRatio)),
                                    math::Max(1, static_cast<int32>(static_cast<float64>(height()) * pixelRatio)));
    if (overlaySizePhysical != mOverlaySizePhysical)
    {
        mOverlaySizePhysical = overlaySizePhysical;
        mOverlayLayer.Invalidate();
        mIsOverlayDirty = true;
    }
    const bool isFrameShown = isVisible == true && mCanPresent == true && mHasPresentedImage == true;
    const bool isOverlayShown =
        isFrameShown == true && mInteraction.isNull() == false && mInteraction->IsActive() == true;
    SyncViewportCanvas(mSelector.constData(),
                       positionScreenLogical.x(),
                       positionScreenLogical.y(),
                       width(),
                       height(),
                       rectangleScreenLogical.width(),
                       rectangleScreenLogical.height(),
                       isFrameShown == true ? 1 : 0,
                       static_cast<float64>(mPresentedFrames),
                       mRequestedWidth,
                       mRequestedHeight,
                       isOverlayShown == true ? 1 : 0,
                       mOverlaySizePhysical.width(),
                       mOverlaySizePhysical.height());
    updateOverlay(isOverlayShown);
}
} // namespace ludus::editor
