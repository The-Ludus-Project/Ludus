#include "internal/preview_viewport.h"
#include "internal/preview_bake.h"
#include "internal/preview_shading.h"

#include <ludus/foundation/base/core.h>

#include <new>

#include <QApplication>
#include <QComboBox>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QLabel>
#include <QPointer>
#include <QPushButton>
#include <QSignalBlocker>
#include <QToolButton>
#include <QVBoxLayout>

#if defined(LUDUS_EDITOR_SOFTWARE_VIEWPORT)
#    include "internal/software_viewport.h"
#    include <QPainter>
#    include <QTimer>
#    include <ludus/foundation/math/scalar.hpp>
#endif
#if defined(LUDUS_EDITOR_NATIVE_PREVIEW)
#    include "internal/offscreen_frame_reader.h"
#    include "internal/qt_viewport_device.h"
#    include <QLibraryInfo>
#    include <QPainter>
#    include <QQuickItem>
#    include <QQuickWindow>
#    include <QSGSimpleTextureNode>
#    include <QSGTexture>
#    include <QThread>
#    include <QVersionNumber>
#endif

namespace ludus::editor
{
using namespace foundation;
namespace math = foundation::math;
namespace renderer = graphics::renderer;
namespace rhi = graphics::rhi;
#if defined(LUDUS_EDITOR_SOFTWARE_VIEWPORT) || defined(LUDUS_EDITOR_NATIVE_PREVIEW)
namespace
{
/// Plain widget that paints the last complete CPU or offscreen frame, scaled to its rectangle.
class FrameCanvas final : public QWidget
{
public:
    FrameCanvas(QWidget* parent, const QString& name) : QWidget(parent)
    {
        setObjectName(name);
        setAttribute(Qt::WA_OpaquePaintEvent);
        // The owning PreviewViewport keeps handling orbit, pan, zoom and framing.
        setAttribute(Qt::WA_TransparentForMouseEvents);
    }
    void SetImage(const QImage& image)
    {
        mImage = image;
        update();
    }
    [[nodiscard]] const QImage& GetImage() const noexcept
    {
        return mImage;
    }

protected:
    void paintEvent(QPaintEvent*) override
    {
        QPainter painter(this);
        painter.fillRect(rect(), QColor(9, 12, 15));
        if (mImage.isNull() == false)
        {
            painter.setRenderHint(QPainter::SmoothPixmapTransform);
            painter.drawImage(rect(), mImage);
        }
    }

private:
    QImage mImage;
};
#    if defined(LUDUS_EDITOR_NATIVE_PREVIEW)
/// Service period for frame polling, about one 60 Hz display refresh.
constexpr int32 SERVICE_INTERVAL_MILLISECONDS = 16;
/// Reports whether this QtWayland release breaks a hidden then re-shown Vulkan subsurface.
///
/// Thanks to The Qt Company and the QtWayland contributors. Before 6.5.1:
/// - "Client: Cleanup vulkan surfaces on hide" (qtwayland d78e8c0284) is missing, so
///   QWaylandWindow::reset keeps the VkSurfaceKHR of the destroyed wl_surface. The
///   swapchain then attaches to a recycled object id and the compositor disconnects us.
/// - "QWaylandWindow: Init parent wl_surface when creating subsurface" (qtwayland
///   d06d66fad7) is missing, so a child shown before its parent gets no wl_subsurface
///   and is never exposed. QWidget shows children before their own window.
/// https://github.com/qt/qtwayland/commit/d78e8c02846a3634445a808d9845be8613bb1bfe
/// https://github.com/qt/qtwayland/commit/d06d66fad70d7c61559211b6f489b94d1c3a8a05
bool IsHiddenSubsurfaceBroken() noexcept
{
    return QGuiApplication::platformName().startsWith(QStringLiteral("wayland")) == true &&
           QLibraryInfo::version() < QVersionNumber(6, 5, 1);
}
#    endif
#    if defined(LUDUS_EDITOR_SOFTWARE_VIEWPORT)
/// Longest rendered side in pixels; the CPU preview is scaled up to the widget.
constexpr float32 SOFTWARE_MAXIMUM_SIDE = 640;
#    endif
} // namespace
#endif
struct PreviewViewport::State final
{
    QPointer<RenderPreview> Preview;
    PreviewViewport* Widget = nullptr;
    QVBoxLayout* Layout = nullptr;
    QLabel* Message = nullptr;
    uint64 Presented = 0;
    bool IsAutomatic = true;
    bool CanShowFrames = true;
#if defined(LUDUS_EDITOR_SOFTWARE_VIEWPORT)
    FrameCanvas* SoftwareCanvasOrNull = nullptr;
    bool IsRenderQueued = false;
    void queueSoftwareRender()
    {
        if (IsRenderQueued == true)
        {
            return;
        }
        IsRenderQueued = true;
        // Coalesce bursts of navigation events into one render on the next event-loop turn.
        QTimer::singleShot(0, Widget, [this]() { renderSoftware(); });
    }
    void renderSoftware()
    {
        IsRenderQueued = false;
        if (Preview.isNull() == true || CanShowFrames == false || Preview->Scene().Objects.isEmpty() == true)
        {
            return;
        }
        // Size the render from the canvas that displays it, so the aspect matches after layout.
        Message->hide();
        if (SoftwareCanvasOrNull->isHidden() == true)
        {
            SoftwareCanvasOrNull->show();
            Layout->activate();
        }
        const float64 ratio = SoftwareCanvasOrNull->devicePixelRatioF();
        const float32 width = static_cast<float32>(math::Max(1.0, SoftwareCanvasOrNull->width() * ratio));
        const float32 height = static_cast<float32>(math::Max(1.0, SoftwareCanvasOrNull->height() * ratio));
        const float32 scale = math::Min(1.0F, SOFTWARE_MAXIMUM_SIDE / math::Max(width, height));
        const uint32 renderWidth = math::Max(uint32{1}, static_cast<uint32>(width * scale));
        const uint32 renderHeight = math::Max(uint32{1}, static_cast<uint32>(height * scale));
        graphics::renderer::ViewDescription view;
        QString error;
        if (Preview->DescribeView(renderWidth, renderHeight, view, error) == false)
        {
            SoftwareCanvasOrNull->hide();
            Message->setText(error);
            Message->show();
            return;
        }
        SoftwareCanvasOrNull->SetImage(
            RenderSoftwareView(Preview->Scene(), view, renderWidth, renderHeight, Preview->GetShading()));
        ++Presented;
    }
#endif
#if defined(LUDUS_EDITOR_NATIVE_PREVIEW)
    // Thanks to The Qt Company: QQuickWindow frame stages and Native Interfaces
    // define wrapping and submission order. We use its basic owner-thread loop,
    // and seal foreign reads afterFrameEnd, never afterRendering (still recording).
    // https://doc.qt.io/qt-6/qquickwindow.html#afterFrameEnd
    // https://doc.qt.io/qt-6/qnativeinterface-qsgmetaltexture.html
    // https://doc.qt.io/qt-6/qnativeinterface-qsgvulkantexture.html
    struct Frame final
    {
        renderer::ViewportFrame Lease{};
        rhi::NativeTextureRead Read{};
        rhi::NativeTextureInfo Image{};
        QSGTexture* Wrapper = nullptr;
        bool HasLease = false;
        bool IsRetiring = false;
    };
    class ImageItem final : public QQuickItem
    {
    public:
        explicit ImageItem(State& inoutState, QQuickItem* parent) : QQuickItem(parent), mState(inoutState)
        {
            setFlag(ItemHasContents);
        }

    protected:
        QSGNode* updatePaintNode(QSGNode* oldNodeOrNull, UpdatePaintNodeData*) override
        {
            LUDUS_REQUIRE(QThread::currentThread() == mState.Widget->thread());
            QSGSimpleTextureNode* node = static_cast<QSGSimpleTextureNode*>(oldNodeOrNull);
            if (mState.Pending >= 0)
            {
                Frame& next = mState.Frames[mState.Pending];
                next.Wrapper = mState.Device->WrapTexture(*window(), next.Image);
                if (next.Wrapper == nullptr)
                {
                    mState.Message->setText(
                        QStringLiteral("Qt cannot import this GPU texture. Editing remains available."));
                    next.IsRetiring = true;
                    mState.Pending = -1;
                    return node;
                }
                next.Wrapper->setFiltering(QSGTexture::Linear);
                if (node == nullptr)
                {
                    node = new QSGSimpleTextureNode;
                    node->setOwnsTexture(false);
                }
                if (mState.Active >= 0)
                {
                    Frame& previous = mState.Frames[mState.Active];
                    previous.IsRetiring = true;
                    delete previous.Wrapper;
                    previous.Wrapper = nullptr;
                }
                mState.Active = mState.Pending;
                mState.Pending = -1;
                node->setTexture(next.Wrapper);
                ++mState.Presented;
            }
            if (node != nullptr)
            {
                node->setRect(boundingRect());
            }
            return node;
        }

    private:
        State& mState;
    };
    QQuickWindow* Window = nullptr;
    QWidget* Container = nullptr;
    FrameCanvas* CanvasOrNull = nullptr;
    OffscreenFrameReader Reader;
    renderer::ViewportFrame ReadLease{};
    bool HasReadLease = false;
    ImageItem* Item = nullptr;
    UniquePtr<QtViewportDevice> Device;
    rhi::NativeDeviceLease DeviceLease{};
    rhi::NativeDeviceInfo DeviceInfo{};
    Frame Frames[2]{};
    int32 Active = -1;
    int32 Pending = -1;
    uint32 SourcePins = 0;
    bool HasDeviceLease = false;
    bool IsDestroyingWindow = false;
    bool IsRecording = false;
    /// Destroy the native window on hide and recreate it under a mapped parent (QtWayland < 6.5.1).
    bool ShouldRecreateHiddenSurface = false;
    /// The window was shown before its parent had a surface; recreate it once the parent is visible.
    bool IsSurfaceRecreationPending = false;
    QTimer Service;
    static bool Retain(void* context) noexcept
    {
        State& state = *static_cast<State*>(context);
        if (state.Window == nullptr || state.IsDestroyingWindow == true)
        {
            return false;
        }
        ++state.SourcePins;
        return true;
    }
    static void Release(void* context) noexcept
    {
        State& state = *static_cast<State*>(context);
        LUDUS_REQUIRE(state.SourcePins > 0);
        --state.SourcePins;
    }
    friend class PreviewViewport;

private:
    void createItem()
    {
        Item = new ImageItem(*this, Window->contentItem());
        Item->setSize(QSizeF(Window->size()));
    }
    void createCanvas()
    {
        CanvasOrNull = new FrameCanvas(Widget, QStringLiteral("LudusOffscreenViewport"));
        Layout->addWidget(CanvasOrNull);
        Message->hide();
        CanvasOrNull->show();
        if (Preview->AdmitOffscreenDevice() == false)
        {
            const QString status = Preview->Status();
            Preview->Stop();
            delete CanvasOrNull;
            CanvasOrNull = nullptr;
            Message->setText(status);
            Message->show();
            return;
        }
        static_cast<PreviewViewport*>(Widget)->resizeSurface();
        Service.start(SERVICE_INTERVAL_MILLISECONDS);
    }
    void releaseReadLease() noexcept
    {
        Reader.Cancel();
        if (HasReadLease == true)
        {
            Preview->ReleaseFrame(ReadLease);
            ReadLease = {};
            HasReadLease = false;
        }
    }
    // Two display images alternate: the renderer draws into one while this reads the other back.
    void serviceCanvas()
    {
        if (Reader.IsBusy() == true)
        {
            const OffscreenReadStatus status = Reader.Poll();
            if (status == OffscreenReadStatus::Complete)
            {
                CanvasOrNull->SetImage(Reader.TakeImage());
                releaseReadLease();
                ++Presented;
            }
            else if (status == OffscreenReadStatus::Failed)
            {
                releaseReadLease();
                Message->setText(QStringLiteral("Offscreen viewport copy failed. Editing remains available."));
                Message->show();
            }
            return;
        }
        if (CanShowFrames == false || Preview->HasPendingFrame() == false)
        {
            return;
        }
        renderer::ViewportImage image;
        if (Preview->TakeFrame(ReadLease, image) == false)
        {
            return;
        }
        HasReadLease = true;
        if (Reader.Begin(Preview->GetDevice(), image.Texture, image.Width, image.Height) == false)
        {
            releaseReadLease();
            Message->setText(QStringLiteral("Offscreen viewport copy is unavailable. Editing remains available."));
            Message->show();
            return;
        }
        static_cast<void>(Reader.Poll());
    }
    void createWindow()
    {
        if (Window != nullptr || CanvasOrNull != nullptr || Preview.isNull() == true)
        {
            return;
        }
        const ViewportPresentation presentation = PreviewViewport::GetPresentation();
        if (presentation == ViewportPresentation::OffscreenReadback)
        {
            createCanvas();
            return;
        }
        if (presentation != ViewportPresentation::SharedDevice)
        {
            Preview->Stop();
            Message->setText(QStringLiteral("This display cannot show the GPU viewport (needs macOS Metal, or "
                                            "X11/Wayland with Vulkan)."));
            return;
        }
        // Basic rendering keeps all RHI calls and native queue submissions on
        // the GUI/device owner. The editor has no other Qt Quick render loop.
        qputenv("QSG_RENDER_LOOP", QByteArrayLiteral("basic"));
#    if defined(Q_OS_MACOS)
        QQuickWindow::setGraphicsApi(QSGRendererInterface::Metal);
#    else
        QQuickWindow::setGraphicsApi(QSGRendererInterface::Vulkan);
#    endif
        Window = new QQuickWindow;
        Window->setObjectName(QStringLiteral("LudusGpuViewport"));
        Window->installEventFilter(Widget);
        Window->setPersistentGraphics(true);
        Window->setPersistentSceneGraph(true);
        Window->setColor(QColor(9, 12, 15));
        Device.Reset(new (std::nothrow) QtViewportDevice);
        if (Device.Get() == nullptr || Device->PrepareWindow(*Window) == false)
        {
            destroyWindow();
#    if defined(Q_OS_MACOS)
            Preview->Stop();
            Message->setText(QStringLiteral("Qt GPU initialization failed. Editing remains available."));
#    else
            // No adapter can present to this window with a shareable device; keep an
            // interactive view through the engine-owned offscreen device instead.
            createCanvas();
#    endif
            return;
        }
        QObject::connect(
            Window,
            &QQuickWindow::sceneGraphInitialized,
            Widget,
            [this]() {
                LUDUS_REQUIRE(QThread::currentThread() == Widget->thread());
                rhi::NativeDeviceDescription description;
                if (Device->GetDescription(*Window, description) == false)
                {
                    Message->setText(QStringLiteral("Qt Quick did not adopt the shared GPU device. Editing remains "
                                                    "available."));
                    QTimer::singleShot(0, Widget, [this]() { Preview->Stop(); });
                    return;
                }
                description.Lifetime = { .Context = this, .Retain = Retain, .Release = Release };
                if (Preview->AdmitDevice(description) == false)
                {
                    QTimer::singleShot(0, Widget, [this]() { Preview->Stop(); });
                    return;
                }
                HasDeviceLease = Preview->PinDevice(DeviceLease) == rhi::RasterStatus::Ready;
                if (HasDeviceLease == false ||
                    rhi::GetNativeDeviceInfo(DeviceLease, DeviceInfo) != rhi::RasterStatus::Ready ||
                    Device->IsCompatible(*Window, DeviceInfo.Identity) == false)
                {
                    QTimer::singleShot(0, Widget, [this]() { Preview->Stop(); });
                }
            },
            Qt::DirectConnection);
        QObject::connect(
            Window,
            &QQuickWindow::beforeFrameBegin,
            Widget,
            [this]() { IsRecording = true; },
            Qt::DirectConnection);
        QObject::connect(
            Window,
            &QQuickWindow::afterFrameEnd,
            Widget,
            [this]() {
                IsRecording = false;
                releaseRetired();
            },
            Qt::DirectConnection);
        QObject::connect(
            Window,
            &QQuickWindow::sceneGraphInvalidated,
            Widget,
            [this]() {
                if (IsDestroyingWindow == false && Preview.isNull() == false)
                {
                    // The Qt source is still alive here; retire RHI before Qt destroys it.
                    IsDestroyingWindow = true;
                    Preview->Stop();
                }
            },
            Qt::DirectConnection);
        QObject::connect(Window,
                         &QQuickWindow::sceneGraphError,
                         Widget,
                         [this](QQuickWindow::SceneGraphError, const QString& error) {
                             Message->setText(error);
                             QTimer::singleShot(0, Widget, [this]() { Preview->Stop(); });
                         });
        QObject::connect(Window, &QQuickWindow::widthChanged, Widget, [this]() {
            static_cast<PreviewViewport*>(Widget)->resizeSurface();
        });
        QObject::connect(Window, &QQuickWindow::heightChanged, Widget, [this]() {
            static_cast<PreviewViewport*>(Widget)->resizeSurface();
        });
        createItem();
        Container = QWidget::createWindowContainer(Window, Widget);
        Container->setFocusPolicy(Qt::NoFocus);
        Container->setAttribute(Qt::WA_TransparentForMouseEvents);
        ShouldRecreateHiddenSurface = IsHiddenSubsurfaceBroken();
        if (ShouldRecreateHiddenSurface == true)
        {
            // The filter sees Hide before the container hides the embedded window.
            Container->installEventFilter(Widget);
        }
        Layout->addWidget(Container);
        Message->hide();
        Container->show();
        Service.start(SERVICE_INTERVAL_MILLISECONDS);
    }
    // Destroying the native window releases Qt Quick's swapchain and the VkSurfaceKHR
    // of the wl_surface QtWayland just unmapped, so the next show builds both afresh.
    // The scene graph, RHI and frame leases persist; nothing samples while hidden.
    void releaseHiddenSurface()
    {
        if (ShouldRecreateHiddenSurface == false || IsDestroyingWindow == true || Window == nullptr)
        {
            return;
        }
        LUDUS_REQUIRE(IsRecording == false);
        IsSurfaceRecreationPending = false;
        Window->destroy();
    }
    void noteWindowShown()
    {
        // A parent that is still hidden has no wl_surface to attach a subsurface to.
        const QWindow* parentOrNull = Window->parent();
        IsSurfaceRecreationPending =
            ShouldRecreateHiddenSurface == true && (parentOrNull == nullptr || parentOrNull->isVisible() == false);
    }
    void recreatePendingSurface()
    {
        if (IsSurfaceRecreationPending == false || Window == nullptr || IsDestroyingWindow == true)
        {
            return;
        }
        if (Window->isVisible() == false)
        {
            // Hidden again; its next show decides afresh.
            IsSurfaceRecreationPending = false;
            return;
        }
        const QWindow* parentOrNull = Window->parent();
        if (parentOrNull == nullptr || parentOrNull->isVisible() == false || IsRecording == true)
        {
            return;
        }
        releaseHiddenSurface();
        Window->show();
    }
    void releaseRetired()
    {
        LUDUS_REQUIRE(QThread::currentThread() == Widget->thread());
        for (Frame& frame : Frames)
        {
            if (frame.HasLease == false || frame.IsRetiring == false)
            {
                continue;
            }
            const rhi::RasterStatus released = rhi::ReleaseNativeTextureRead(frame.Read);
            if (released == rhi::RasterStatus::Ready)
            {
                Preview->ReleaseFrame(frame.Lease);
                frame = {};
            }
        }
    }
    void clearFrames()
    {
        CanShowFrames = IsAutomatic;
        if (CanvasOrNull != nullptr)
        {
            releaseReadLease();
            return;
        }
        bool hasFrames = false;
        for (const Frame& frame : Frames)
        {
            hasFrames = hasFrames || frame.HasLease;
        }
        if (hasFrames == false)
        {
            return;
        }
        LUDUS_REQUIRE(IsRecording == false);
        // Detach the complete item from the scene graph before freeing wrappers.
        // Its obsolete node owns no texture and cannot record further sampling.
        delete Item;
        Item = nullptr;
        Active = Pending = -1;
        for (Frame& frame : Frames)
        {
            delete frame.Wrapper;
            frame.Wrapper = nullptr;
            if (frame.HasLease == true)
            {
                const rhi::RasterStatus released = rhi::DrainNativeTextureRead(frame.Read);
                LUDUS_REQUIRE(released == rhi::RasterStatus::Ready);
                Preview->ReleaseFrame(frame.Lease);
                frame = {};
            }
        }
        if (Window != nullptr && IsDestroyingWindow == false)
        {
            createItem();
        }
    }
    void destroyWindow()
    {
        Service.stop();
        IsDestroyingWindow = true;
        // Source pins must already have been released by RHI shutdown. The
        // device lease keeps the Qt source handshake until the final release.
        if (HasDeviceLease == true)
        {
            static_cast<void>(rhi::ReleaseNativeDevice(DeviceLease));
            HasDeviceLease = false;
        }
        LUDUS_REQUIRE(SourcePins == 0);
        if (CanvasOrNull != nullptr)
        {
            LUDUS_REQUIRE(HasReadLease == false);
            delete CanvasOrNull;
            CanvasOrNull = nullptr;
        }
        if (Container != nullptr)
        {
            delete Container;
            Container = nullptr;
        }
        else
        {
            delete Window;
        }
        if (Window != nullptr && Item != nullptr)
        {
            // The container owns and destroys Window and its content items.
            Item = nullptr;
        }
        Window = nullptr;
        Device.Reset();
        IsDestroyingWindow = false;
        Message->show();
    }
    void showFrame()
    {
        if (CanvasOrNull != nullptr && Preview.isNull() == false)
        {
            serviceCanvas();
            return;
        }
        if (Preview.isNull() == true || Window == nullptr || Pending >= 0 || Preview->HasPendingFrame() == false)
        {
            return;
        }
        for (int32 i = 0; i < 2; ++i)
        {
            Frame& frame = Frames[i];
            if (frame.HasLease == true)
            {
                continue;
            }
            renderer::ViewportImage image;
            if (Preview->TakeFrame(frame.Lease, image) == false)
            {
                return;
            }
            const rhi::RasterStatus acquired = rhi::AcquireNativeTextureRead(DeviceLease,
                                                                             DeviceInfo.Identity,
                                                                             image.Texture,
                                                                             image.Producer,
                                                                             frame.Read,
                                                                             frame.Image);
            if (acquired != rhi::RasterStatus::Ready)
            {
                Preview->ReleaseFrame(frame.Lease);
                Message->setText(QStringLiteral("GPU texture sharing failed. Editing remains available."));
                return;
            }
            frame.HasLease = true;
            Pending = i;
            Item->update();
            Window->update();
            return;
        }
    }
#endif
};
PreviewViewport::PreviewViewport(RenderPreview* preview, QWidget* parentOrNull, bool isAutomatic)
    : QWidget(parentOrNull), Preview_(preview)
{
    LUDUS_REQUIRE(preview != nullptr);
    mState.Reset(new State);
    setMinimumSize(160, 160);
    setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Ignored);
    setFocusPolicy(Qt::StrongFocus);
    mNavigationTimer.setInterval(16);
    connect(&mNavigationTimer, &QTimer::timeout, this, [this]() { advanceNavigation(); });
    connect(preview, &RenderPreview::Stopped, this, [this]() { stopNavigation(); });
    setToolTip(
        QStringLiteral("Right-drag to look · hold right mouse + WASD/QE to fly · Alt/Option-left-drag to orbit · "
                       "Shift-Alt/Option-left-drag or middle-drag to pan · "
                       "wheel to dolly (right mouse: speed) · F frames selection · Home frames all"));
    mState->Preview = preview;
    mState->IsAutomatic = isAutomatic;
    mState->CanShowFrames = isAutomatic;
    mState->Widget = this;
    mState->Layout = new QVBoxLayout(this);
    mState->Layout->setContentsMargins(0, 0, 0, 0);
    QComboBox* shading = new QComboBox(this);
    shading->setObjectName(QStringLiteral("viewportShading"));
    shading->setAccessibleName(QStringLiteral("Viewport shading"));
    shading->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
    shading->setMinimumContentsLength(4);
    shading->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
    shading->addItem(QStringLiteral("Solid — neutral preview shading"));
    shading->addItem(QStringLiteral("Albedo — unlit surface colors"));
    shading->addItem(QStringLiteral("Lit — scene lighting"));
    shading->setToolTip(QStringLiteral("Solid uses an editor-only neutral material and preview light; Albedo shows "
                                       "base colors without illumination. Lit shows authored lights, emitting surfaces "
                                       "as shadowed area lights and, once baked, static diffuse lighting."));
    shading->setCurrentIndex(static_cast<foundation::int32>(preview->GetShading()));
    QHBoxLayout* controls = new QHBoxLayout;
    controls->addWidget(shading, 1);
    QToolButton* frameAll = new QToolButton(this);
    frameAll->setObjectName(QStringLiteral("viewportFrameAll"));
    frameAll->setText(QStringLiteral("Frame all"));
    frameAll->setAccessibleName(QStringLiteral("Frame all visible objects"));
    frameAll->setToolTip(QStringLiteral("Fit all visible geometry in the viewport (Home)."));
    controls->addWidget(frameAll);
    connect(frameAll, &QToolButton::clicked, preview, &RenderPreview::FrameAll);
    mState->Layout->addLayout(controls);
    QLabel* navigationHint = new QLabel(this);
    navigationHint->setObjectName(QStringLiteral("viewportNavigationHint"));
    navigationHint->setTextFormat(Qt::PlainText);
#if defined(Q_OS_MACOS)
    navigationHint->setText(QStringLiteral("Option-drag: orbit · Shift-Option-drag: pan · Scroll: zoom"));
#else
    navigationHint->setText(QStringLiteral("Alt-drag: orbit · Shift-Alt-drag: pan · Scroll: zoom"));
#endif
    navigationHint->setWordWrap(true);
    navigationHint->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    navigationHint->setToolTip(toolTip());
    mState->Layout->addWidget(navigationHint);
    // Keep bake diagnostics from squeezing the shading and framing controls.
    QHBoxLayout* bakeControls = new QHBoxLayout;
    bakeControls->setContentsMargins(0, 0, 0, 0);
    QLabel* bakeNotice = new QLabel(this);
    bakeNotice->setObjectName(QStringLiteral("viewportBakeNotice"));
    bakeNotice->setWordWrap(true);
    bakeNotice->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    bakeNotice->hide();
    bakeControls->addWidget(bakeNotice, 1);
    QPushButton* bakeAction = new QPushButton(this);
    bakeAction->setObjectName(QStringLiteral("viewportBakeAction"));
    bakeAction->hide();
    bakeControls->addWidget(bakeAction);
    mState->Layout->addLayout(bakeControls);
    const auto updateBake = [preview, bakeNotice, bakeAction]() {
        const PreviewBakeState state = preview->GetBakeState();
        PreviewBakeNotice notice;
        const bool hasNotice = preview->GetBakeNotice(notice) == true && state != PreviewBakeState::Idle;
        bakeNotice->setVisible(hasNotice);
        if (hasNotice == true)
        {
            bakeNotice->setText(QStringLiteral("⚠ %1 collapsed triangles skipped").arg(notice.SkippedTriangleCount));
            bakeNotice->setToolTip(
                QStringLiteral("The static bake skipped %1 zero-area triangles in %2: %3. They cover no surface, so "
                               "the baked lighting is unaffected; fix the meshes to remove this warning. Cancel the "
                               "bake to keep direct lighting only.")
                    .arg(notice.SkippedTriangleCount)
                    .arg(notice.AffectedObjectCount == 1 ? QStringLiteral("1 object")
                                                         : QStringLiteral("%1 objects").arg(notice.AffectedObjectCount))
                    .arg(notice.AffectedObjects));
        }
        bakeAction->setVisible(state == PreviewBakeState::Running || state == PreviewBakeState::Cancelled);
        bakeAction->setText(state == PreviewBakeState::Cancelled ? QStringLiteral("Bake again")
                                                                 : QStringLiteral("Cancel bake"));
        bakeAction->setToolTip(state == PreviewBakeState::Cancelled
                                   ? QStringLiteral("Restart the static lighting bake for this scene.")
                                   : QStringLiteral("Stop the static lighting bake; direct lighting stays visible."));
    };
    connect(preview, &RenderPreview::BakeChanged, this, updateBake);
    connect(bakeAction, &QPushButton::clicked, this, [preview]() {
        if (preview->GetBakeState() == PreviewBakeState::Cancelled)
        {
            static_cast<void>(preview->RestartBake());
        }
        else
        {
            preview->CancelBake();
        }
    });
    connect(shading, &QComboBox::currentIndexChanged, this, [preview, updateBake](foundation::int32 index) {
        preview->SetShading(static_cast<PreviewShading>(index));
        updateBake();
    });
    connect(preview, &RenderPreview::NavigationChanged, shading, [preview, shading]() {
        const QSignalBlocker blocker(shading);
        shading->setCurrentIndex(static_cast<foundation::int32>(preview->GetShading()));
    });
    mState->Message = new QLabel(QStringLiteral("Open a World to render it here."), this);
    mState->Message->setAlignment(Qt::AlignCenter);
    mState->Message->setWordWrap(true);
    mState->Layout->addWidget(mState->Message);
#if defined(LUDUS_EDITOR_SOFTWARE_VIEWPORT)
    mState->SoftwareCanvasOrNull = new FrameCanvas(this, QStringLiteral("LudusSoftwareViewport"));
    mState->SoftwareCanvasOrNull->hide();
    mState->Layout->addWidget(mState->SoftwareCanvasOrNull, 1);
    connect(preview, &RenderPreview::NavigationChanged, this, [this]() { mState->queueSoftwareRender(); });
#endif
#if defined(LUDUS_EDITOR_NATIVE_PREVIEW)
    connect(preview, &RenderPreview::DeviceRequested, this, [this]() { mState->createWindow(); });
    connect(preview, &RenderPreview::ResettingFrames, this, [this]() { mState->clearFrames(); });
    // A stopped preview uncovers the message label; replace the workspace's
    // placeholder with the failure so the viewport says why it is empty.
    connect(preview, &RenderPreview::Failed, this, [this](const QString& error) { mState->Message->setText(error); });
    connect(preview, &RenderPreview::Stopped, this, [this]() {
        if (mState->IsDestroyingWindow == true)
        {
            if (mState->HasDeviceLease == true)
            {
                static_cast<void>(rhi::ReleaseNativeDevice(mState->DeviceLease));
                mState->HasDeviceLease = false;
            }
            QTimer::singleShot(0, this, [this]() { mState->destroyWindow(); });
        }
        else
        {
            mState->destroyWindow();
        }
    });
    connect(&mState->Service, &QTimer::timeout, this, [this]() {
        mState->recreatePendingSurface();
        mState->releaseRetired();
        // Always finish an in-flight offscreen copy; otherwise only present when admitted.
        if (mState->CanShowFrames == true || mState->Reader.IsBusy() == true)
        {
            mState->showFrame();
        }
    });
#endif
    if (isAutomatic == true)
    {
        connect(preview, &RenderPreview::FrameReady, this, &PreviewViewport::ShowFrame);
    }
}
PreviewViewport::~PreviewViewport()
{
    stopNavigation();
#if defined(LUDUS_EDITOR_NATIVE_PREVIEW)
    if (mState->Preview.isNull() == false)
    {
        mState->Preview->Stop();
    }
    mState->destroyWindow();
#endif
}
void PreviewViewport::ShowFrame()
{
#if defined(LUDUS_EDITOR_SOFTWARE_VIEWPORT)
    mState->CanShowFrames = true;
    mState->queueSoftwareRender();
#endif
#if defined(LUDUS_EDITOR_NATIVE_PREVIEW)
    // A manually gated import may present only after its catalog is committed.
    // Once admitted, also retry frames that arrived while both slots were leased.
    mState->CanShowFrames = true;
    mState->showFrame();
#endif
}
void PreviewViewport::ClearFrame(const QString& text)
{
#if defined(LUDUS_EDITOR_SOFTWARE_VIEWPORT)
    mState->CanShowFrames = mState->IsAutomatic;
    mState->SoftwareCanvasOrNull->SetImage({});
    mState->SoftwareCanvasOrNull->hide();
    mState->Message->show();
#endif
#if defined(LUDUS_EDITOR_NATIVE_PREVIEW)
    mState->clearFrames();
#endif
    mState->Message->setText(text);
}
bool PreviewViewport::eventFilter(QObject* watched, QEvent* event)
{
#if defined(LUDUS_EDITOR_NATIVE_PREVIEW)
    if (watched == mState->Window)
    {
        switch (event->type())
        {
            case QEvent::MouseButtonPress:
                mousePressEvent(static_cast<QMouseEvent*>(event));
                return true;
            case QEvent::MouseButtonRelease:
                mouseReleaseEvent(static_cast<QMouseEvent*>(event));
                return true;
            case QEvent::KeyPress:
                keyPressEvent(static_cast<QKeyEvent*>(event));
                return event->isAccepted();
            case QEvent::KeyRelease:
                keyReleaseEvent(static_cast<QKeyEvent*>(event));
                return event->isAccepted();
            case QEvent::FocusOut:
                if (QApplication::focusWidget() != this)
                {
                    stopNavigation();
                }
                break;
            case QEvent::WindowDeactivate:
                stopNavigation();
                break;
            case QEvent::MouseMove:
                mouseMoveEvent(static_cast<QMouseEvent*>(event));
                return true;
            case QEvent::MouseButtonDblClick:
                event->ignore();
                return false;
            case QEvent::Wheel:
                wheelEvent(static_cast<QWheelEvent*>(event));
                return true;
            case QEvent::Show:
                mState->noteWindowShown();
                break;
            default:
                break;
        }
    }
    if (watched == mState->Container && event->type() == QEvent::Hide)
    {
        mState->releaseHiddenSurface();
    }
#endif
    if (event->type() == QEvent::WindowDeactivate)
    {
        stopNavigation();
    }
    return QWidget::eventFilter(watched, event);
}
foundation::float32 PreviewViewport::getDrawableHeight() const noexcept
{
#if defined(LUDUS_EDITOR_NATIVE_PREVIEW)
    if (mState->Window != nullptr)
    {
        return static_cast<foundation::float32>(mState->Window->height());
    }
    if (mState->CanvasOrNull != nullptr)
    {
        return static_cast<foundation::float32>(mState->CanvasOrNull->height());
    }
#endif
#if defined(LUDUS_EDITOR_SOFTWARE_VIEWPORT)
    if (mState->SoftwareCanvasOrNull != nullptr)
    {
        return static_cast<foundation::float32>(mState->SoftwareCanvasOrNull->height());
    }
#endif
    return static_cast<foundation::float32>(height());
}
void PreviewViewport::resizeSurface()
{
#if defined(LUDUS_EDITOR_SOFTWARE_VIEWPORT)
    if (mState->SoftwareCanvasOrNull != nullptr && mState->SoftwareCanvasOrNull->isHidden() == false)
    {
        mState->queueSoftwareRender();
    }
#endif
#if defined(LUDUS_EDITOR_NATIVE_PREVIEW)
    if (mState->CanvasOrNull != nullptr)
    {
        // RenderPreview recreates the offscreen targets when the requested size changes.
        const float64 ratio = mState->CanvasOrNull->devicePixelRatioF();
        Preview_->Resize(qRound(static_cast<float64>(mState->CanvasOrNull->width()) * ratio),
                         qRound(static_cast<float64>(mState->CanvasOrNull->height()) * ratio));
        return;
    }
    if (mState->Window != nullptr && mState->Item != nullptr)
    {
        const foundation::float64 ratio = mState->Window->devicePixelRatio();
        Preview_->Resize(qRound(static_cast<foundation::float64>(mState->Window->width()) * ratio),
                         qRound(static_cast<foundation::float64>(mState->Window->height()) * ratio));
        mState->Item->setSize(QSizeF(mState->Window->size()));
        mState->Item->update();
    }
#endif
}
bool PreviewViewport::CanPresent() noexcept
{
    return GetPresentation() != ViewportPresentation::Unavailable;
}
ViewportPresentation PreviewViewport::GetPresentation() noexcept
{
#if defined(LUDUS_EDITOR_NATIVE_PREVIEW)
    const QString platform = QGuiApplication::platformName();
#    if defined(Q_OS_MACOS)
    return platform == QStringLiteral("cocoa") ? ViewportPresentation::SharedDevice : ViewportPresentation::Unavailable;
#    else
    if (qEnvironmentVariable("LUDUS_EDITOR_VIEWPORT") == QStringLiteral("offscreen"))
    {
        return ViewportPresentation::OffscreenReadback;
    }
    const bool isDesktopDisplay = platform == QStringLiteral("xcb") || platform.startsWith(QStringLiteral("wayland"));
    if (isDesktopDisplay == false)
    {
        return ViewportPresentation::Unavailable;
    }
    // The editor creates the Vulkan device and Qt Quick adopts it, so every Qt 6
    // release shares GPU textures; readback remains the fallback when that fails.
    return ViewportPresentation::SharedDevice;
#    endif
#else
    return ViewportPresentation::Unavailable;
#endif
}
uint64 PreviewViewport::PresentedFrames() const noexcept
{
    return mState->Presented;
}
} // namespace ludus::editor
