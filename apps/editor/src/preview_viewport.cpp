#include "internal/preview_viewport.h"
#include "internal/empty_world_view.h"
#include "internal/preview_shading.h"
#include "internal/viewport_statistics.h"

#include <ludus/foundation/base/core.h>

#include <new>

#include <QApplication>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QFontDatabase>
#include <QFontMetricsF>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QLabel>
#include <QPointer>
#include <QPushButton>
#include <QSignalBlocker>
#include <QToolButton>
#include <QVBoxLayout>

#if defined(LUDUS_EDITOR_BROWSER_PREVIEW)
#    include "internal/browser_viewport.h"
#endif
#if defined(LUDUS_EDITOR_NATIVE_PREVIEW)
#    include "internal/offscreen_frame_reader.h"
#    include "internal/preview_shader_reload.h"
#    include "internal/qt_viewport_device.h"
#    include "internal/render_graph_preview.h"
#    include "internal/renderer_workspace.h"
#    include "internal/world_graph_view.h"
#    include <QLibraryInfo>
#    include <QPainter>
#    include <QQuickItem>
#    include <QQuickPaintedItem>
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
#if defined(LUDUS_EDITOR_NATIVE_PREVIEW)
namespace
{
/// Inset of the statistics panel from the viewport's top-left corner, in logical pixels.
constexpr qreal STATISTICS_MARGIN = 8;
/// Padding between the statistics panel edge and its text, in logical pixels.
constexpr qreal STATISTICS_PADDING = 6;
/// Corner radius of the statistics panel, in logical pixels.
constexpr qreal STATISTICS_CORNER_RADIUS = 4;
/// Translucent dark panel behind the statistics text, kept readable over any debug view.
constexpr QRgb STATISTICS_BACKGROUND = qRgba(9, 12, 15, 190);
/// Statistics text colour.
constexpr QRgb STATISTICS_TEXT = qRgb(232, 236, 240);
/// Translucent red panel behind live shader errors and restart notices.
constexpr QRgb SHADER_ERROR_BACKGROUND = qRgba(110, 20, 24, 215);
/// Live shaders toggle tooltip; while an error is shown the tooltip carries its full text instead.
QString GetLiveShadersToolTip()
{
    return QStringLiteral(
        "Watch this engine checkout's renderer shaders (modules/graphics/renderer/shaders and their "
        "includes). Saved edits compile in the background with ludus-shader-worker and replace the "
        "viewport's shaders in every shading mode without restarting. Compile errors keep the last good "
        "shaders and are shown over the viewport.");
}
/// Stacking order above the frame item, so clearFrames recreating that item never covers it.
constexpr qreal STATISTICS_Z = 1;
QFont GetStatisticsFont()
{
    return QFontDatabase::systemFont(QFontDatabase::FixedFont);
}
/// Size of the padded statistics panel for text, in logical pixels; empty text has no panel.
QSizeF MeasureStatistics(const QString& text)
{
    if (text.isEmpty() == true)
    {
        return {};
    }
    const QRectF bounds = QFontMetricsF(GetStatisticsFont()).boundingRect(QRectF(), Qt::AlignLeft, text);
    return {bounds.width() + 2 * STATISTICS_PADDING, bounds.height() + 2 * STATISTICS_PADDING};
}
/// Paint an overlay panel (statistics or live shader status) with its top-left corner at origin.
void PaintStatistics(QPainter& inoutPainter,
                     const QString& text,
                     QPointF origin,
                     QRgb background = STATISTICS_BACKGROUND)
{
    if (text.isEmpty() == true)
    {
        return;
    }
    const QRectF panel(origin, MeasureStatistics(text));
    inoutPainter.save();
    inoutPainter.setRenderHint(QPainter::Antialiasing);
    inoutPainter.setPen(Qt::NoPen);
    inoutPainter.setBrush(QColor::fromRgba(background));
    inoutPainter.drawRoundedRect(panel, STATISTICS_CORNER_RADIUS, STATISTICS_CORNER_RADIUS);
    inoutPainter.setFont(GetStatisticsFont());
    inoutPainter.setPen(QColor::fromRgb(STATISTICS_TEXT));
    inoutPainter.drawText(
        panel.adjusted(STATISTICS_PADDING, STATISTICS_PADDING, -STATISTICS_PADDING, -STATISTICS_PADDING),
        Qt::AlignLeft | Qt::AlignTop,
        text);
    inoutPainter.restore();
}
/// Shorten each line of an overlay to fit maxWidth (the viewport minus margins), keeping both
/// ends so a long path still shows its file name and line. A non-positive maxWidth keeps text.
QString FitOverlayText(const QString& text, qreal maxWidth)
{
    const qreal available = maxWidth - 2 * (STATISTICS_MARGIN + STATISTICS_PADDING);
    if (text.isEmpty() == true || maxWidth <= 0 || available <= 0)
    {
        return text;
    }
    const QFontMetricsF metrics(GetStatisticsFont());
    QStringList lines = text.split(QLatin1Char('\n'));
    for (QString& line : lines)
    {
        line = metrics.elidedText(line, Qt::ElideMiddle, available);
    }
    return lines.join(QLatin1Char('\n'));
}
/// Top-left of the live shader status panel: below the statistics panel when it is shown.
QPointF GetShaderStatusOrigin(const QString& statisticsText)
{
    const qreal statisticsHeight = MeasureStatistics(statisticsText).height();
    return {STATISTICS_MARGIN, STATISTICS_MARGIN + (statisticsHeight > 0 ? statisticsHeight + STATISTICS_MARGIN : 0)};
}
/// Overlay panel inside the viewport's QQuickWindow. A sibling QWidget would sit under the
/// native window that createWindowContainer embeds on macOS and Linux; a scene-graph item is
/// composited by Qt Quick in the same Metal/Vulkan pass as the frame, so it is always on top.
class StatisticsItem final : public QQuickPaintedItem
{
public:
    StatisticsItem(QQuickItem* parent, const QString& name) : QQuickPaintedItem(parent)
    {
        setObjectName(name);
        setZ(STATISTICS_Z);
        setPosition({STATISTICS_MARGIN, STATISTICS_MARGIN});
        setAntialiasing(true);
        setVisible(false);
    }
    void SetText(const QString& text, QRgb background = STATISTICS_BACKGROUND)
    {
        if (text == mText && background == mBackground)
        {
            return;
        }
        mText = text;
        mBackground = background;
        setSize(MeasureStatistics(text));
        setVisible(text.isEmpty() == false);
        update();
    }
    void paint(QPainter* painter) override
    {
        PaintStatistics(*painter, mText, {0, 0}, mBackground);
    }

private:
    QString mText;
    QRgb mBackground = STATISTICS_BACKGROUND;
};
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
    /// Overlay text painted over the frame; empty hides the statistics panel.
    void SetStatisticsText(const QString& text)
    {
        if (text != mStatisticsText)
        {
            mStatisticsText = text;
            update();
        }
    }
    /// Live shader status painted below the statistics; empty hides the panel.
    void SetShaderStatusText(const QString& text, bool isError)
    {
        if (text != mShaderStatusText || isError != mHasShaderError)
        {
            mShaderStatusText = text;
            mHasShaderError = isError;
            update();
        }
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
        PaintStatistics(painter, mStatisticsText, {STATISTICS_MARGIN, STATISTICS_MARGIN});
        PaintStatistics(painter,
                        FitOverlayText(mShaderStatusText, width()),
                        GetShaderStatusOrigin(mStatisticsText),
                        mHasShaderError == true ? SHADER_ERROR_BACKGROUND : STATISTICS_BACKGROUND);
    }

private:
    QImage mImage;
    QString mStatisticsText;
    QString mShaderStatusText;
    bool mHasShaderError = false;
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
} // namespace
#endif
struct PreviewViewport::State final
{
    /// Admits the device and reports its status; the frame source's images live on that device.
    QPointer<RenderPreview> Preview;
    /// Produces the presented frames and owns their leases.
    QPointer<ViewportFrameSource> Source;
    PreviewViewport* Widget = nullptr;
    QVBoxLayout* Layout = nullptr;
    QHBoxLayout* ControlsOrNull = nullptr;
    QLabel* Message = nullptr;
    uint64 Presented = 0;
    /// Statistics overlay text currently shown; empty while the overlay is off.
    QString StatisticsText;
    /// Browser fallback: the engine's DOM canvas covers Qt widgets, so text goes in the toolbar.
    QLabel* StatisticsLabelOrNull = nullptr;
    /// Live shader status shown below the statistics; empty while there is nothing to report.
    QString ShaderStatusText;
    bool HasShaderError = false;
    bool IsAutomatic = true;
    bool CanShowFrames = true;
#if defined(LUDUS_EDITOR_BROWSER_PREVIEW)
    BrowserViewport* BrowserOrNull = nullptr;
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
        /// The source's read of this slot's image; the source decides how it is opened and closed.
        ViewportFrameRead Read{};
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
                next.Wrapper = mState.Device->WrapTexture(*window(), next.Read.Image);
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
    StatisticsItem* StatisticsOrNull = nullptr;
    StatisticsItem* ShaderStatusOrNull = nullptr;
    /// Live shader rebuilds of this viewport's preview; null where unsupported.
    PreviewShaderReload* ShaderReloadOrNull = nullptr;
    QWidget* Container = nullptr;
    FrameCanvas* CanvasOrNull = nullptr;
    WorldGraphView* GraphViewOrNull = nullptr;
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
    /// Retry only while a submitted Qt sampling read is awaiting its GPU completion marker.
    QTimer RetirementRetry;
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
    void applyStatistics()
    {
        if (StatisticsOrNull != nullptr)
        {
            StatisticsOrNull->SetText(StatisticsText);
        }
        if (CanvasOrNull != nullptr)
        {
            CanvasOrNull->SetStatisticsText(StatisticsText);
        }
        applyShaderStatus();
    }
    void applyShaderStatus()
    {
        if (ShaderStatusOrNull != nullptr)
        {
            ShaderStatusOrNull->SetText(FitOverlayText(ShaderStatusText, Window->width()),
                                        HasShaderError == true ? SHADER_ERROR_BACKGROUND : STATISTICS_BACKGROUND);
            ShaderStatusOrNull->setPosition(GetShaderStatusOrigin(StatisticsText));
        }
        if (CanvasOrNull != nullptr)
        {
            CanvasOrNull->SetShaderStatusText(ShaderStatusText, HasShaderError);
        }
    }
    void createItem()
    {
        Item = new ImageItem(*this, Window->contentItem());
        Item->setSize(QSizeF(Window->size()));
    }
    void createCanvas()
    {
        CanvasOrNull = new FrameCanvas(Widget, QStringLiteral("LudusOffscreenViewport"));
        CanvasOrNull->SetStatisticsText(StatisticsText);
        CanvasOrNull->SetShaderStatusText(ShaderStatusText, HasShaderError);
        Layout->addWidget(CanvasOrNull, 1);
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
            Source->ReleaseFrame(ReadLease);
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
        if (CanShowFrames == false || Source->HasPendingFrame() == false)
        {
            return;
        }
        renderer::ViewportImage image;
        if (Source->TakeFrame(ReadLease, image) == false)
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
        if (GraphViewOrNull != nullptr)
        {
            if (Preview->HasDevice() == false && Preview->AdmitOffscreenDevice() == false)
            {
                Message->setText(Preview->Status());
                Message->show();
            }
            else
            {
                Message->hide();
                GraphViewOrNull->show();
            }
            return;
        }
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
        StatisticsOrNull = new StatisticsItem(Window->contentItem(), QStringLiteral("viewportStatisticsOverlay"));
        StatisticsOrNull->SetText(StatisticsText);
        ShaderStatusOrNull = new StatisticsItem(Window->contentItem(), QStringLiteral("viewportShaderStatusOverlay"));
        applyShaderStatus();
        Container = QWidget::createWindowContainer(Window, Widget);
        Container->setFocusPolicy(Qt::NoFocus);
        Container->setAttribute(Qt::WA_TransparentForMouseEvents);
        ShouldRecreateHiddenSurface = IsHiddenSubsurfaceBroken();
        if (ShouldRecreateHiddenSurface == true)
        {
            // The filter sees Hide before the container hides the embedded window.
            Container->installEventFilter(Widget);
        }
        Container->setObjectName(QStringLiteral("viewportRenderSurface"));
        Container->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
        Layout->addWidget(Container, 1);
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
        if (IsRecording == true)
        {
            // A nested event loop must not seal reads ahead of Qt's current submission.
            // afterFrameEnd retries them once that submission is actually queued.
            return;
        }
        bool hasPendingReads = false;
        for (Frame& frame : Frames)
        {
            if (frame.HasLease == false || frame.IsRetiring == false)
            {
                continue;
            }
            const rhi::RasterStatus retired = Source->EndFrameRead(frame.Read);
            if (retired == rhi::RasterStatus::Ready)
            {
                frame = {};
            }
            else if (retired == rhi::RasterStatus::Pending)
            {
                hasPendingReads = true;
            }
        }
        if (hasPendingReads == true)
        {
            if (RetirementRetry.isActive() == false)
            {
                RetirementRetry.start();
            }
        }
        else
        {
            RetirementRetry.stop();
        }
    }
    void clearFrames()
    {
        RetirementRetry.stop();
        CanShowFrames = IsAutomatic;
        if (GraphViewOrNull != nullptr)
        {
            GraphViewOrNull->SetCanPresent(IsAutomatic);
        }
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
                Source->DrainFrameRead(frame.Read);
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
        RetirementRetry.stop();
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
        StatisticsOrNull = nullptr;
        ShaderStatusOrNull = nullptr;
        Window = nullptr;
        Device.Reset();
        IsDestroyingWindow = false;
        Message->show();
    }
    void showFrame()
    {
        if (CanvasOrNull != nullptr && Preview.isNull() == false && Source.isNull() == false)
        {
            serviceCanvas();
            return;
        }
        if (Preview.isNull() == true || Source.isNull() == true || Window == nullptr || Pending >= 0 ||
            Source->HasPendingFrame() == false)
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
            const rhi::RasterStatus acquired = Source->TakeFrameRead(DeviceLease, DeviceInfo.Identity, frame.Read);
            if (acquired == rhi::RasterStatus::NotReady)
            {
                return;
            }
            if (acquired != rhi::RasterStatus::Ready)
            {
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
    : PreviewViewport(preview, preview, parentOrNull, isAutomatic)
{
}
PreviewViewport::PreviewViewport(RenderPreview* preview,
                                 ViewportFrameSource* frameSource,
                                 QWidget* parentOrNull,
                                 bool isAutomatic)
    : QWidget(parentOrNull), Preview_(preview), mFrameSource(frameSource)
#if defined(Q_OS_MACOS)
      ,
      mRelativeMouse(*this)
#endif
{
    LUDUS_REQUIRE(preview != nullptr);
    LUDUS_REQUIRE(frameSource != nullptr);
    mState.Reset(new State);
    setMinimumSize(160, 160);
    setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Ignored);
    setFocusPolicy(Qt::StrongFocus);
    mNavigationTimer.setInterval(16);
    connect(&mNavigationTimer, &QTimer::timeout, this, [this]() { advanceNavigation(); });
    connect(preview, &RenderPreview::Stopped, this, [this]() { stopNavigation(); });
#if defined(Q_OS_MACOS)
    connect(qGuiApp, &QGuiApplication::applicationStateChanged, this, [this](Qt::ApplicationState state) {
        if (state != Qt::ApplicationActive)
        {
            stopNavigation();
        }
    });
#endif
    setToolTip(
        QStringLiteral("Right-drag to look · hold right mouse + WASD/QE to fly · Alt/Option-left-drag to orbit · "
                       "Shift-Alt/Option-left-drag or middle-drag to pan · "
                       "wheel to dolly (right mouse: speed) · F frames selection · Home frames all"));
    mState->Preview = preview;
    mState->Source = frameSource;
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
    // Combo indices equal PreviewShading values; new modes are appended so saved indices stay valid.
    for (foundation::usize i = 0; i < PREVIEW_SHADING_COUNT; ++i)
    {
        shading->addItem(GetPreviewShadingLabel(static_cast<PreviewShading>(i)));
    }
    shading->setToolTip(
        QStringLiteral("Solid uses an editor-only neutral material and preview light; Albedo shows "
                       "base colours without illumination. Lit shows authored lights, emitting surfaces "
                       "as shadowed area lights. The viewport uses direct lighting without baking lightmaps.\n"
                       "Debug views (Normal, Depth, Roughness, Metallic, UV, Light count) replace shading with "
                       "one material or lighting quantity per pixel so it can be inspected directly."));
    shading->setCurrentIndex(static_cast<foundation::int32>(preview->GetShading()));
    QHBoxLayout* controls = new QHBoxLayout;
    mState->ControlsOrNull = controls;
    controls->addWidget(shading, 1);
    QDoubleSpinBox* lookSensitivity = new QDoubleSpinBox(this);
    lookSensitivity->setObjectName(QStringLiteral("viewportLookSensitivity"));
    lookSensitivity->setAccessibleName(QStringLiteral("Mouse look sensitivity"));
    lookSensitivity->setPrefix(QStringLiteral("Look "));
    lookSensitivity->setSuffix(QStringLiteral("×"));
    lookSensitivity->setRange(static_cast<float64>(RenderPreview::MINIMUM_LOOK_SENSITIVITY),
                              static_cast<float64>(RenderPreview::MAXIMUM_LOOK_SENSITIVITY));
    lookSensitivity->setSingleStep(static_cast<float64>(RenderPreview::MINIMUM_LOOK_SENSITIVITY));
    lookSensitivity->setDecimals(1);
    lookSensitivity->setValue(static_cast<float64>(preview->GetLookSensitivity()));
    lookSensitivity->setToolTip(QStringLiteral("Mouse sensitivity for right-drag look and Alt/Option orbit. "
                                               "Lower values rotate more slowly; 1× is the default."));
    controls->addWidget(lookSensitivity);
    connect(lookSensitivity, &QDoubleSpinBox::valueChanged, preview, [preview](float64 sensitivity) {
        preview->SetLookSensitivity(static_cast<float32>(sensitivity));
    });
    connect(preview, &RenderPreview::NavigationChanged, lookSensitivity, [preview, lookSensitivity]() {
        const QSignalBlocker blocker(lookSensitivity);
        lookSensitivity->setValue(static_cast<float64>(preview->GetLookSensitivity()));
    });
    QToolButton* frameAll = new QToolButton(this);
    frameAll->setObjectName(QStringLiteral("viewportFrameAll"));
    frameAll->setText(QStringLiteral("Frame all"));
    frameAll->setAccessibleName(QStringLiteral("Frame all visible objects"));
    frameAll->setToolTip(QStringLiteral("Fit all visible geometry in the viewport (Home)."));
    controls->addWidget(frameAll);
    connect(frameAll, &QToolButton::clicked, preview, &RenderPreview::FrameAll);
    QToolButton* forceRaster = new QToolButton(this);
    forceRaster->setObjectName(QStringLiteral("viewportForceRaster"));
    forceRaster->setText(QStringLiteral("Force raster"));
    forceRaster->setCheckable(true);
    forceRaster->setChecked(preview->ShouldForceRaster());
    forceRaster->setAccessibleName(QStringLiteral("Force raster renderer"));
    forceRaster->setToolTip(QStringLiteral("Use raster even when hardware raytracing is available. "
                                           "Unchecked selects automatically from the current GPU and API."));
    controls->addWidget(forceRaster);
    connect(forceRaster, &QToolButton::toggled, preview, &RenderPreview::SetForceRaster);
    connect(preview, &RenderPreview::RendererPreferenceChanged, forceRaster, [forceRaster](bool shouldForceRaster) {
        const QSignalBlocker blocker(forceRaster);
        forceRaster->setChecked(shouldForceRaster);
    });
    QToolButton* statistics = new QToolButton(this);
    statistics->setObjectName(QStringLiteral("viewportStatistics"));
    statistics->setText(QStringLiteral("Stats"));
    statistics->setCheckable(true);
    statistics->setChecked(preview->IsStatisticsVisible());
    statistics->setAccessibleName(QStringLiteral("Show viewport statistics"));
    statistics->setToolTip(QStringLiteral("Overlay GPU frame time, CPU frame time, resolution and draw count, "
                                          "averaged over recent frames. GPU shows n/a when the device has no "
                                          "timestamp queries."));
    controls->addWidget(statistics);
    connect(statistics, &QToolButton::toggled, preview, &RenderPreview::SetStatisticsVisible);
#if defined(LUDUS_EDITOR_NATIVE_PREVIEW)
    if (PreviewShaderReload::IsSupported() == true)
    {
        mState->ShaderReloadOrNull = new PreviewShaderReload(*preview, this);
        QToolButton* liveShaders = new QToolButton(this);
        liveShaders->setObjectName(QStringLiteral("viewportLiveShaders"));
        liveShaders->setText(QStringLiteral("Live shaders"));
        liveShaders->setCheckable(true);
        liveShaders->setAccessibleName(QStringLiteral("Reload renderer shaders when their sources change"));
        liveShaders->setToolTip(GetLiveShadersToolTip());
        controls->addWidget(liveShaders);
        PreviewShaderReload* reload = mState->ShaderReloadOrNull;
        connect(liveShaders, &QToolButton::toggled, reload, &PreviewShaderReload::SetEnabled);
        connect(reload, &PreviewShaderReload::StatusChanged, this, [this]() { updateShaderStatus(); });
    }
#endif
    mState->Layout->addLayout(controls);
#if defined(LUDUS_EDITOR_BROWSER_PREVIEW)
    mState->StatisticsLabelOrNull = new QLabel(this);
    mState->StatisticsLabelOrNull->setObjectName(QStringLiteral("viewportStatisticsOverlay"));
    mState->StatisticsLabelOrNull->setTextFormat(Qt::PlainText);
    mState->StatisticsLabelOrNull->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
    mState->StatisticsLabelOrNull->hide();
    mState->Layout->addWidget(mState->StatisticsLabelOrNull, 0, Qt::AlignTop);
#endif
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
    mState->Layout->addWidget(navigationHint, 0, Qt::AlignTop);
    connect(shading, &QComboBox::currentIndexChanged, this, [preview](foundation::int32 index) {
        preview->SetShading(static_cast<PreviewShading>(index));
    });
    connect(preview, &RenderPreview::NavigationChanged, shading, [preview, shading]() {
        const QSignalBlocker blocker(shading);
        shading->setCurrentIndex(static_cast<foundation::int32>(preview->GetShading()));
    });
    connect(preview, &RenderPreview::NavigationChanged, statistics, [preview, statistics]() {
        const QSignalBlocker blocker(statistics);
        statistics->setChecked(preview->IsStatisticsVisible());
    });
    connect(preview, &RenderPreview::StatisticsChanged, this, [this]() { updateStatistics(); });
    mState->Message = new EmptyWorldView(*preview, this);
    mState->Message->setText(QStringLiteral("Open or create a World to start authoring."));
    mState->Message->setWordWrap(true);
    mState->Layout->addWidget(mState->Message, 1);
#if defined(LUDUS_EDITOR_BROWSER_PREVIEW)
    mState->BrowserOrNull = new BrowserViewport(*preview, this);
    mState->BrowserOrNull->hide();
    mState->Layout->addWidget(mState->BrowserOrNull, 1);
    connect(preview, &RenderPreview::DeviceRequested, this, [this]() {
        mState->Message->hide();
        mState->BrowserOrNull->show();
        mState->Layout->activate();
        if (mState->BrowserOrNull->Start() == false)
        {
            mState->BrowserOrNull->hide();
            mState->Message->setText(mState->Preview->Status());
            mState->Message->show();
        }
    });
    connect(preview, &RenderPreview::Failed, this, [this](const QString& error) {
        mState->Message->setText(error);
        mState->Message->show();
    });
#endif
#if defined(LUDUS_EDITOR_NATIVE_PREVIEW)
    connect(preview, &RenderPreview::DeviceRequested, this, [this]() { mState->createWindow(); });
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
        if (mState->GraphViewOrNull != nullptr)
        {
            return;
        }
        mState->recreatePendingSurface();
        mState->releaseRetired();
        // Always finish an in-flight offscreen copy; otherwise only present when admitted.
        if (mState->CanShowFrames == true || mState->Reader.IsBusy() == true)
        {
            mState->showFrame();
        }
    });
    mState->RetirementRetry.setInterval(ViewportFramePump::COMPLETION_POLL_MILLISECONDS);
    mState->RetirementRetry.setTimerType(Qt::PreciseTimer);
    mState->RetirementRetry.setSingleShot(true);
    connect(&mState->RetirementRetry, &QTimer::timeout, this, [this]() { mState->releaseRetired(); });
#endif
    mIsAutomatic = isAutomatic;
    connectFrameSource();
}
void PreviewViewport::connectFrameSource()
{
#if defined(LUDUS_EDITOR_NATIVE_PREVIEW)
    mResettingFramesConnection =
        connect(mFrameSource, &ViewportFrameSource::ResettingFrames, this, [this]() { mState->clearFrames(); });
#endif
    if (mIsAutomatic == true)
    {
        mFrameReadyConnection =
            connect(mFrameSource, &ViewportFrameSource::FrameReady, this, &PreviewViewport::ShowFrame);
    }
}
void PreviewViewport::SetFrameSource(ViewportFrameSource* frameSourceOrNull)
{
    ViewportFrameSource* next = frameSourceOrNull == nullptr ? Preview_ : frameSourceOrNull;
    if (next == mFrameSource)
    {
        return;
    }
#if defined(LUDUS_EDITOR_NATIVE_PREVIEW)
    // The current source's reads end through that source before it is replaced.
    mState->clearFrames();
#endif
    // Only the source's own connections: a RenderPreview keeps its device and navigation signals.
    disconnect(mResettingFramesConnection);
    disconnect(mFrameReadyConnection);
    mFrameSource = next;
    mState->Source = next;
    connectFrameSource();
    resizeSurface();
#if defined(LUDUS_EDITOR_NATIVE_PREVIEW)
    // Present the next source's pending frame at once; a gated viewport stays gated.
    if (mState->CanShowFrames == true)
    {
        mState->showFrame();
    }
#endif
}
ViewportFrameSource& PreviewViewport::GetFrameSource() const noexcept
{
    return *mFrameSource;
}
bool PreviewViewport::CanImportHostedImages() const noexcept
{
#if defined(LUDUS_EDITOR_NATIVE_PREVIEW)
    return mState->Window != nullptr && mState->Device.Get() != nullptr && mState->Device->CanImportHostedImages();
#else
    return false;
#endif
}
PreviewViewport::~PreviewViewport()
{
    stopNavigation();
#if defined(LUDUS_EDITOR_BROWSER_PREVIEW)
    delete mState->BrowserOrNull;
    mState->BrowserOrNull = nullptr;
#endif
#if defined(LUDUS_EDITOR_NATIVE_PREVIEW)
    if (mState->Preview.isNull() == false)
    {
        mState->Preview->Stop();
    }
    mState->destroyWindow();
#endif
}
void PreviewViewport::UseProjectGraph(RendererWorkspace& inoutWorkspace)
{
#if defined(LUDUS_EDITOR_NATIVE_PREVIEW)
    if (mState->GraphViewOrNull != nullptr || inoutWorkspace.GetPreviewOrNull() == nullptr)
    {
        return;
    }
    const bool wasRunning = Preview_->Running();
    if (wasRunning == true)
    {
        Preview_->Stop();
    }
    Preview_->UseProjectGraph();
    const auto synchronizeProblem = [this, &inoutWorkspace]() {
        Preview_->SetProjectGraphProblem(inoutWorkspace.GetScenePreparationProblem());
    };
    // A failure can originate inside activation polling. Rollback may detach that activation,
    // so deliver the current diagnostic only after the producer has left its stack.
    connect(&inoutWorkspace, &RendererWorkspace::RenderProblemChanged, this, synchronizeProblem, Qt::QueuedConnection);
    connect(&inoutWorkspace, &RendererWorkspace::DocumentChanged, this, synchronizeProblem);
    connect(inoutWorkspace.GetPreviewOrNull(),
            &RenderGraphPreview::Changed,
            this,
            synchronizeProblem,
            Qt::QueuedConnection);
    synchronizeProblem();
    mState->GraphViewOrNull = new WorldGraphView(inoutWorkspace, this);
    mState->GraphViewOrNull->setVisible(Preview_->Scene().Identity.isEmpty() == false);
    mState->GraphViewOrNull->SetCanPresent(mState->IsAutomatic);
    mState->Layout->addWidget(mState->GraphViewOrNull, 1);
    mState->ControlsOrNull->insertWidget(0, mState->GraphViewOrNull->TakeControls(this), 1);
    findChild<QComboBox*>(QStringLiteral("viewportShading"))->hide();
    for (const QString& name : {QStringLiteral("viewportLiveShaders")})
    {
        QToolButton* buttonOrNull = findChild<QToolButton*>(name);
        if (buttonOrNull != nullptr)
        {
            buttonOrNull->hide();
        }
    }
    QToolButton* statistics = findChild<QToolButton*>(QStringLiteral("viewportStatistics"));
    LUDUS_REQUIRE(statistics != nullptr);
    statistics->setToolTip(QStringLiteral("Show delivered-image FPS, active CPU work, GPU device time and elapsed "
                                          "readback/conversion latency. Times average the last 30 samples; "
                                          "FPS falls to zero when idle. GPU shows n/a without timestamps."));
    updateStatistics();
    if (wasRunning == true)
    {
        static_cast<void>(Preview_->Start());
    }
#else
    static_cast<void>(inoutWorkspace);
#endif
}
void PreviewViewport::HoldFrame()
{
    mState->CanShowFrames = false;
#if defined(LUDUS_EDITOR_NATIVE_PREVIEW)
    if (mState->GraphViewOrNull != nullptr)
    {
        mState->GraphViewOrNull->SetCanPresent(false);
    }
#endif
}
void PreviewViewport::ShowFrame()
{
#if defined(LUDUS_EDITOR_BROWSER_PREVIEW)
    mState->Message->hide();
    mState->BrowserOrNull->ShowFrame();
#endif
#if defined(LUDUS_EDITOR_NATIVE_PREVIEW)
    // A manually gated import may present only after its catalog is committed.
    // Once admitted, also retry frames that arrived while both slots were leased.
    mState->CanShowFrames = true;
    if (mState->GraphViewOrNull == nullptr)
    {
        mState->showFrame();
    }
    else
    {
        mState->GraphViewOrNull->SetCanPresent(true);
    }
#endif
}
bool PreviewViewport::HasRetainedGraphFrame() const noexcept
{
#if defined(LUDUS_EDITOR_NATIVE_PREVIEW)
    return mState->GraphViewOrNull != nullptr && mState->GraphViewOrNull->GetImage().isNull() == false;
#else
    return false;
#endif
}
void PreviewViewport::ClearFrame(const QString& text)
{
#if defined(LUDUS_EDITOR_BROWSER_PREVIEW)
    mState->BrowserOrNull->ClearFrame();
    mState->BrowserOrNull->hide();
    mState->Message->show();
#endif
#if defined(LUDUS_EDITOR_NATIVE_PREVIEW)
    mState->clearFrames();
    if (mState->GraphViewOrNull != nullptr)
    {
        mState->GraphViewOrNull->ClearFrame();
        if (Preview_->Scene().Identity.isEmpty() == true)
        {
            mState->GraphViewOrNull->hide();
        }
    }
#endif
    mState->Message->setText(text);
    if (Preview_->Scene().Identity.isEmpty() == true)
    {
        mState->Message->show();
        mState->Message->update();
    }
}
QString PreviewViewport::GetStatisticsText() const
{
#if defined(LUDUS_EDITOR_NATIVE_PREVIEW)
    if (mState->GraphViewOrNull != nullptr)
    {
        return mState->GraphViewOrNull->GetStatisticsText();
    }
#endif
    return mState->StatisticsText;
}
QString PreviewViewport::GetShaderStatusText() const
{
    return mState->ShaderStatusText;
}
PreviewShaderReload* PreviewViewport::GetShaderReloadOrNull() const noexcept
{
#if defined(LUDUS_EDITOR_NATIVE_PREVIEW)
    return mState->ShaderReloadOrNull;
#else
    return nullptr;
#endif
}
void PreviewViewport::updateShaderStatus()
{
#if defined(LUDUS_EDITOR_NATIVE_PREVIEW)
    const PreviewShaderReload* reload = mState->ShaderReloadOrNull;
    mState->ShaderStatusText = reload->GetStatusText();
    mState->HasShaderError = reload->HasError();
    mState->applyShaderStatus();
    // The overlay shortens long lines to the viewport; the toggle's tooltip keeps the whole error.
    QToolButton* liveShaders = findChild<QToolButton*>(QStringLiteral("viewportLiveShaders"));
    if (liveShaders != nullptr)
    {
        liveShaders->setToolTip(mState->HasShaderError == true ? mState->ShaderStatusText : GetLiveShadersToolTip());
    }
#endif
}
void PreviewViewport::updateStatistics()
{
#if defined(LUDUS_EDITOR_NATIVE_PREVIEW)
    if (mState->GraphViewOrNull != nullptr)
    {
        mState->GraphViewOrNull->SetStatisticsVisible(Preview_->IsStatisticsVisible());
        return;
    }
#endif
    const QString text =
        Preview_->IsStatisticsVisible() == true ? FormatViewportStatistics(Preview_->GetStatistics()) : QString();
    if (text == mState->StatisticsText)
    {
        return;
    }
    mState->StatisticsText = text;
#if defined(LUDUS_EDITOR_BROWSER_PREVIEW)
    mState->StatisticsLabelOrNull->setText(text);
    mState->StatisticsLabelOrNull->setVisible(text.isEmpty() == false);
#endif
#if defined(LUDUS_EDITOR_NATIVE_PREVIEW)
    mState->applyStatistics();
#endif
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
#if defined(LUDUS_EDITOR_BROWSER_PREVIEW)
    return static_cast<foundation::float32>(mState->BrowserOrNull->height());
#endif
#if defined(LUDUS_EDITOR_NATIVE_PREVIEW)
    if (mState->GraphViewOrNull != nullptr)
    {
        return static_cast<foundation::float32>(mState->GraphViewOrNull->height());
    }
    if (mState->Window != nullptr)
    {
        return static_cast<foundation::float32>(mState->Window->height());
    }
    if (mState->CanvasOrNull != nullptr)
    {
        return static_cast<foundation::float32>(mState->CanvasOrNull->height());
    }
#endif
    return static_cast<foundation::float32>(height());
}
void PreviewViewport::resizeSurface()
{
#if defined(LUDUS_EDITOR_NATIVE_PREVIEW)
    if (mState->CanvasOrNull != nullptr)
    {
        // RenderPreview recreates the offscreen targets when the requested size changes.
        const float64 ratio = mState->CanvasOrNull->devicePixelRatioF();
        mFrameSource->Resize(qRound(static_cast<float64>(mState->CanvasOrNull->width()) * ratio),
                             qRound(static_cast<float64>(mState->CanvasOrNull->height()) * ratio));
        return;
    }
    if (mState->Window != nullptr && mState->Item != nullptr)
    {
        const foundation::float64 ratio = mState->Window->devicePixelRatio();
        mFrameSource->Resize(qRound(static_cast<foundation::float64>(mState->Window->width()) * ratio),
                             qRound(static_cast<foundation::float64>(mState->Window->height()) * ratio));
        mState->Item->setSize(QSizeF(mState->Window->size()));
        mState->Item->update();
        mState->applyShaderStatus();
    }
#endif
}
bool PreviewViewport::CanPresent() noexcept
{
    return GetPresentation() != ViewportPresentation::Unavailable;
}
ViewportPresentation PreviewViewport::GetPresentation() noexcept
{
#if defined(LUDUS_EDITOR_BROWSER_PREVIEW)
    return ViewportPresentation::BrowserCanvas;
#endif
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
#if defined(LUDUS_EDITOR_BROWSER_PREVIEW)
    return mState->BrowserOrNull->GetPresentedFrames();
#elif defined(LUDUS_EDITOR_NATIVE_PREVIEW)
    return mState->GraphViewOrNull != nullptr ? mState->GraphViewOrNull->GetPresentedFrames() : mState->Presented;
#else
    return mState->Presented;
#endif
}
} // namespace ludus::editor
