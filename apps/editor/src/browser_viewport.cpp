#include "internal/browser_viewport.h"

#include <ludus/foundation/base/core.h>
#include <ludus/foundation/containers/static_array.hpp>
#include <ludus/foundation/math/scalar.hpp>

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
    return 1;
});
EM_JS(void, SyncViewportCanvas, (const char* selector, int32 x, int32 y, int32 width, int32 height,
                                int32 screenWidth, int32 screenHeight, int32 isVisible, float64 presentedFrames,
                                int32 drawableWidth, int32 drawableHeight), {
    const canvas = document.querySelector(UTF8ToString(selector));
    const screen = document.getElementById('screen');
    if (canvas == null || screen == null) return;
    const bounds = screen.getBoundingClientRect();
    const scaleX = bounds.width / Math.max(1, screenWidth);
    const scaleY = bounds.height / Math.max(1, screenHeight);
    canvas.style.left = (bounds.left + x * scaleX) + 'px';
    canvas.style.top = (bounds.top + y * scaleY) + 'px';
    canvas.style.width = (width * scaleX) + 'px';
    canvas.style.height = (height * scaleY) + 'px';
    canvas.style.display = isVisible ? 'block' : 'none';
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
});
EM_JS(void, MarkViewportApi, (const char* selector, int32 isWebGl2), {
    const canvas = document.querySelector(UTF8ToString(selector));
    if (canvas != null) canvas.dataset.api = isWebGl2 ? 'webgl2' : 'webgpu';
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
                    // Continue through the ordinary start path; RenderPreview stopped the failed device.
                    mIsRetrying = true;
                    QTimer::singleShot(0, this, [this]() {
                        if (mPreview.isNull() == false && mPreview->Start() == false)
                        {
                            // The retry could not even request a device; surface why instead of waiting.
                            mIsRetrying = false;
                            mPreview->ReportBrowserUnavailable(mPreview->Status());
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
        if (CreateViewportCanvas(mSelector.constData()) == 0)
        {
            return false;
        }
        mHasPresentedImage = false;
        const BrowserViewportApi api = mNextApi;
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
void BrowserViewport::SetFixedDrawableSize(const QSize& sizeOrEmpty)
{
    mFixedDrawableSize = sizeOrEmpty;
    service();
}
uint64 BrowserViewport::GetPresentedFrames() const noexcept
{
    return mPresentedFrames;
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
        }
    }
    SyncViewportCanvas(mSelector.constData(),
                       positionScreenLogical.x(),
                       positionScreenLogical.y(),
                       width(),
                       height(),
                       rectangleScreenLogical.width(),
                       rectangleScreenLogical.height(),
                       isVisible == true && mCanPresent == true && mHasPresentedImage == true ? 1 : 0,
                       static_cast<float64>(mPresentedFrames),
                       mRequestedWidth,
                       mRequestedHeight);
}
} // namespace ludus::editor
