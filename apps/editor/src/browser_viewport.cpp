#include "internal/browser_viewport.h"

#include <ludus/foundation/base/core.h>
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
                                int32 screenWidth, int32 screenHeight, int32 isVisible, float64 presentedFrames), {
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
    RemoveViewportCanvas(mSelector.constData());
    if (CreateViewportCanvas(mSelector.constData()) == 0)
    {
        return false;
    }
    mHasPresentedImage = false;
    service();
    if (mPreview->AdmitBrowserDevice(mSelector.constData()) == false)
    {
        RemoveViewportCanvas(mSelector.constData());
        return false;
    }
    mService.start();
    return true;
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
                       static_cast<float64>(mPresentedFrames));
}
} // namespace ludus::editor
