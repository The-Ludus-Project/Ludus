#include "internal/preview_viewport.h"
#include "internal/empty_world_view.h"
#include "internal/orientation_widget.h"
#include "internal/preview_shading.h"
#include "internal/reference_capture_window.h"
#include "internal/viewport_size.h"
#include "internal/viewport_statistics.h"

#include <ludus/foundation/base/core.h>
#include <ludus/foundation/math/scalar.hpp>
#include <ludus/foundation/time/time.hpp>

#include <algorithm>
#include <new>

#include <QAction>
#include <QApplication>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QFontDatabase>
#include <QFontMetricsF>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QLabel>
#include <QLayout>
#include <QLinearGradient>
#include <QList>
#include <QMenu>
#include <QPainter>
#include <QPointer>
#include <QPushButton>
#include <QSignalBlocker>
#include <QToolBar>
#include <QToolButton>
#include <QToolTip>
#include <QVBoxLayout>
#include <QWidgetAction>

#if defined(LUDUS_EDITOR_BROWSER_PREVIEW)
#    include "internal/browser_viewport.h"
#endif
#if defined(LUDUS_EDITOR_NATIVE_PREVIEW)
#    include "internal/offscreen_frame_reader.h"
#    include "internal/preview_shader_reload.h"
#    include "internal/qt_viewport_device.h"
#    include "internal/ray_path_inspection.h"
#    include "internal/render_graph_preview.h"
#    include "internal/renderer_workspace.h"
#    include "internal/viewport_object_overlay_item.h"
#    include "internal/world_graph_view.h"
#    include <QLibraryInfo>
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
namespace
{
/// Colour of the viewport background, also painted in the bars around a letterboxed frame.
constexpr QRgb VIEWPORT_BACKGROUND = qRgb(9, 12, 15);
/// Arrow-key and spin-button step of the field of view control, in degrees.
constexpr float64 FIELD_OF_VIEW_STEP_DEGREES = 5;
/// Outline drawn around a letterboxed frame so its edge stays visible over a dark scene.
constexpr QRgb FRAME_OUTLINE = qRgb(58, 66, 76);
/// Width of the shading menu in characters; longer labels are cut, and the full text is in the tooltip.
constexpr int SHADING_COMBO_CHARACTERS = 12;
#if defined(LUDUS_EDITOR_NATIVE_PREVIEW)
/// Shows or hides a widget that was added to the controls toolbar. A toolbar owns the visibility of its
/// widgets through their actions, so hiding the widget itself would be undone on the next layout.
void SetControlVisible(QToolBar& inoutControls, QWidget* controlOrNull, bool isVisible)
{
    if (controlOrNull == nullptr)
    {
        return;
    }
    for (QAction* action : inoutControls.actions())
    {
        const QWidgetAction* widgetAction = qobject_cast<QWidgetAction*>(action);
        if (widgetAction != nullptr && widgetAction->defaultWidget() == controlOrNull)
        {
            action->setVisible(isVisible);
            return;
        }
    }
}
#endif
/// Places every presentation widget (message and grid, render surface, readback canvas, graph
/// view or browser proxy) on the frame of the current size preset, centred in the layout's area.
/// Only one of them is visible at a time, so they share the frame instead of stacking.
class ViewportFrameLayout final : public QLayout
{
public:
    ViewportFrameLayout() = default;
    ~ViewportFrameLayout() override
    {
        for (QLayoutItem* item : mItems)
        {
            delete item;
        }
    }
    /// Select the preset and place the widgets on its frame at once.
    void SetSize(ViewportSize size)
    {
        mSize = size;
        setGeometry(geometry());
    }
    [[nodiscard]] QRect GetFrameRectangle() const noexcept
    {
        return ComputeViewportFrameRectangle(mSize, geometry());
    }
    void addItem(QLayoutItem* item) override
    {
        mItems.append(item);
        invalidate();
    }
    int count() const override
    {
        return static_cast<int>(mItems.size());
    }
    QLayoutItem* itemAt(int index) const override
    {
        return index >= 0 && index < mItems.size() ? mItems.at(index) : nullptr;
    }
    QLayoutItem* takeAt(int index) override
    {
        if (index < 0 || index >= mItems.size())
        {
            return nullptr;
        }
        QLayoutItem* item = mItems.takeAt(index);
        invalidate();
        return item;
    }
    Qt::Orientations expandingDirections() const override
    {
        return Qt::Horizontal | Qt::Vertical;
    }
    QSize sizeHint() const override
    {
        QSize hint;
        for (const QLayoutItem* item : mItems)
        {
            if (item->isEmpty() == false)
            {
                hint = hint.expandedTo(item->sizeHint());
            }
        }
        return hint;
    }
    QSize minimumSize() const override
    {
        QSize minimum;
        for (const QLayoutItem* item : mItems)
        {
            if (item->isEmpty() == false)
            {
                minimum = minimum.expandedTo(item->minimumSize());
            }
        }
        return minimum;
    }
    void setGeometry(const QRect& rectangle) override
    {
        QLayout::setGeometry(rectangle);
        const QRect frame = GetFrameRectangle();
        for (QLayoutItem* item : mItems)
        {
            item->setGeometry(frame);
        }
    }

private:
    QList<QLayoutItem*> mItems;
    ViewportSize mSize;
};
} // namespace
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
/// Width of the legend's colour bar, in logical pixels.
constexpr qreal LEGEND_BAR_WIDTH = 168;
/// Height of the legend's colour bar, in logical pixels.
constexpr qreal LEGEND_BAR_HEIGHT = 10;
/// Gap between the legend's title, bar and end labels, in logical pixels.
constexpr qreal LEGEND_GAP = 4;
/// Colour stops sampled along the legend bar; the ramp is smooth enough that this matches the shader.
constexpr int32 LEGEND_STOP_COUNT = 16;
/// Outline around the legend bar so its dark end stays visible on the translucent panel.
constexpr QRgb LEGEND_BAR_OUTLINE = qRgb(120, 130, 140);
/// Space between the end labels of a legend, in logical pixels.
constexpr qreal LEGEND_LABEL_GAP = 12;
QLinearGradient MakeLegendGradient(PreviewLegendRamp ramp)
{
    QLinearGradient gradient(0, 0, 1, 0);
    gradient.setCoordinateMode(QGradient::ObjectBoundingMode);
    for (int32 stop = 0; stop < LEGEND_STOP_COUNT; ++stop)
    {
        const float32 position = static_cast<float32>(stop) / static_cast<float32>(LEGEND_STOP_COUNT - 1);
        const math::Vector3 colour = GetPreviewRampColour(ramp, position);
        gradient.setColorAt(static_cast<qreal>(position), QColor::fromRgbF(colour.X, colour.Y, colour.Z));
    }
    return gradient;
}
/// Size of the padded legend panel, in logical pixels.
QSizeF MeasureLegend(const PreviewLegend& legend)
{
    const QFontMetricsF metrics(GetStatisticsFont());
    const qreal labelsWidth =
        metrics.horizontalAdvance(legend.LowLabel) + LEGEND_LABEL_GAP + metrics.horizontalAdvance(legend.HighLabel);
    const qreal contentWidth = std::max({LEGEND_BAR_WIDTH, metrics.horizontalAdvance(legend.Title), labelsWidth});
    const qreal contentHeight = 2 * metrics.height() + LEGEND_BAR_HEIGHT + 2 * LEGEND_GAP;
    return {contentWidth + 2 * STATISTICS_PADDING, contentHeight + 2 * STATISTICS_PADDING};
}
/// Paint a legend panel with its top-left corner at origin: title, colour bar, then the two end labels.
void PaintLegend(QPainter& inoutPainter, const PreviewLegend& legend, QPointF origin)
{
    const QSizeF size = MeasureLegend(legend);
    const QFontMetricsF metrics(GetStatisticsFont());
    inoutPainter.save();
    inoutPainter.setRenderHint(QPainter::Antialiasing);
    inoutPainter.setPen(Qt::NoPen);
    inoutPainter.setBrush(QColor::fromRgba(STATISTICS_BACKGROUND));
    inoutPainter.drawRoundedRect(QRectF(origin, size), STATISTICS_CORNER_RADIUS, STATISTICS_CORNER_RADIUS);
    inoutPainter.setFont(GetStatisticsFont());
    inoutPainter.setPen(QColor::fromRgb(STATISTICS_TEXT));
    const qreal left = origin.x() + STATISTICS_PADDING;
    const qreal contentWidth = size.width() - 2 * STATISTICS_PADDING;
    qreal top = origin.y() + STATISTICS_PADDING;
    inoutPainter.drawText(QRectF(left, top, contentWidth, metrics.height()),
                          Qt::AlignLeft | Qt::AlignTop,
                          legend.Title);
    top += metrics.height() + LEGEND_GAP;
    const QRectF bar(left, top, contentWidth, LEGEND_BAR_HEIGHT);
    inoutPainter.setBrush(MakeLegendGradient(legend.Ramp));
    inoutPainter.setPen(QColor::fromRgb(LEGEND_BAR_OUTLINE));
    inoutPainter.drawRect(bar);
    inoutPainter.setPen(QColor::fromRgb(STATISTICS_TEXT));
    top += LEGEND_BAR_HEIGHT + LEGEND_GAP;
    const QRectF labels(left, top, contentWidth, metrics.height());
    inoutPainter.drawText(labels, Qt::AlignLeft | Qt::AlignTop, legend.LowLabel);
    inoutPainter.drawText(labels, Qt::AlignRight | Qt::AlignTop, legend.HighLabel);
    inoutPainter.restore();
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
/// Legend key of the current debug view, anchored to the bottom-left corner of the viewport window.
class LegendItem final : public QQuickPaintedItem
{
public:
    explicit LegendItem(QQuickItem* parent) : QQuickPaintedItem(parent)
    {
        setObjectName(QStringLiteral("viewportLegendOverlay"));
        setZ(STATISTICS_Z);
        setAntialiasing(true);
        setVisible(false);
        connect(parent, &QQuickItem::heightChanged, this, [this]() { place(); });
    }
    /// Show the legend, or hide the panel when legendOrNull is null.
    void SetLegend(const PreviewLegend* legendOrNull)
    {
        const bool wasLegendShown = mHasLegend;
        mHasLegend = legendOrNull != nullptr;
        if (mHasLegend == true)
        {
            if (wasLegendShown == true && mLegend.Title == legendOrNull->Title && mLegend.Ramp == legendOrNull->Ramp &&
                mLegend.LowLabel == legendOrNull->LowLabel && mLegend.HighLabel == legendOrNull->HighLabel)
            {
                return;
            }
            mLegend = *legendOrNull;
            setSize(MeasureLegend(mLegend));
            place();
        }
        setVisible(mHasLegend);
        update();
    }
    void paint(QPainter* painter) override
    {
        if (mHasLegend == true)
        {
            PaintLegend(*painter, mLegend, {0, 0});
        }
    }

private:
    void place()
    {
        setPosition({STATISTICS_MARGIN, parentItem()->height() - height() - STATISTICS_MARGIN});
    }
    PreviewLegend mLegend;
    bool mHasLegend = false;
};
/// Plain widget that paints the last complete CPU or offscreen frame, scaled to its rectangle.
/// Axis orientation widget anchored to the top-right corner of the viewport window.
class OrientationItem final : public QQuickPaintedItem
{
public:
    explicit OrientationItem(QQuickItem* parent) : QQuickPaintedItem(parent)
    {
        setObjectName(QStringLiteral("viewportOrientationOverlay"));
        setZ(STATISTICS_Z);
        setAntialiasing(true);
        setVisible(false);
        setSize({ORIENTATION_WIDGET_SIZE_PIXEL, ORIENTATION_WIDGET_SIZE_PIXEL});
        connect(parent, &QQuickItem::widthChanged, this, [this]() { place(); });
    }
    /// Show the widget for this camera, or hide it when isShown is false.
    void SetCamera(const gameplay::camera::CameraSample& sample, bool isShown)
    {
        mSample = sample;
        setVisible(isShown);
        place();
        update();
    }
    void paint(QPainter* painter) override
    {
        PaintOrientationWidget(*painter, BuildOrientationLayout(mSample, QRectF(QPointF(0, 0), size())), mSample);
    }

private:
    void place()
    {
        setPosition(GetOrientationWidgetRect(parentItem()->width()).topLeft());
    }
    gameplay::camera::CameraSample mSample;
};
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
    void SetInteraction(ViewportObjectInteraction& inoutInteraction)
    {
        mInteractionOrNull = &inoutInteraction;
        connect(&inoutInteraction, &ViewportObjectInteraction::Changed, this, [this]() { update(); });
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
    /// Orientation widget painted at the top-right; hidden when isShown is false.
    void SetOrientation(const gameplay::camera::CameraSample& sample, bool isShown)
    {
        mOrientationSample = sample;
        mIsOrientationShown = isShown;
        update();
    }
    /// Legend key painted at the bottom-left; null hides it.
    void SetLegend(const PreviewLegend* legendOrNull)
    {
        const bool wasLegendShown = mHasLegend;
        mHasLegend = legendOrNull != nullptr;
        if (mHasLegend == true)
        {
            mLegend = *legendOrNull;
        }
        if (wasLegendShown == true || mHasLegend == true)
        {
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
        painter.fillRect(rect(), QColor::fromRgb(VIEWPORT_BACKGROUND));
        if (mImage.isNull() == false)
        {
            painter.setRenderHint(QPainter::SmoothPixmapTransform);
            painter.drawImage(rect(), mImage);
        }
        if (mInteractionOrNull != nullptr)
        {
            mInteractionOrNull->Paint(painter, rect());
        }
        PaintStatistics(painter, mStatisticsText, {STATISTICS_MARGIN, STATISTICS_MARGIN});
        PaintStatistics(painter,
                        FitOverlayText(mShaderStatusText, width()),
                        GetShaderStatusOrigin(mStatisticsText),
                        mHasShaderError == true ? SHADER_ERROR_BACKGROUND : STATISTICS_BACKGROUND);
        if (mIsOrientationShown == true)
        {
            PaintOrientationWidget(painter,
                                   BuildOrientationLayout(mOrientationSample, GetOrientationWidgetRect(width())),
                                   mOrientationSample);
        }
        if (mHasLegend == true)
        {
            PaintLegend(painter,
                        mLegend,
                        {STATISTICS_MARGIN, height() - MeasureLegend(mLegend).height() - STATISTICS_MARGIN});
        }
    }

private:
    ViewportObjectInteraction* mInteractionOrNull = nullptr;
    PreviewLegend mLegend;
    bool mHasLegend = false;
    gameplay::camera::CameraSample mOrientationSample;
    bool mIsOrientationShown = false;
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
    /// Shared frame of the presentation widgets; letterboxed by the size preset.
    ViewportFrameLayout* FrameLayoutOrNull = nullptr;
    QToolBar* ControlsOrNull = nullptr;
    EmptyWorldView* MessageOrNull = nullptr;
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
#if defined(LUDUS_EDITOR_GPU_PREVIEW)
    /// Reference capture window of this viewport, created on first use and owned by the widget.
    QPointer<ReferenceCaptureWindow> ReferenceWindow;
#endif
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
        ViewportTraceIdentity TraceIdentity;
        uint64 FrameBeginTicks = 0;
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
            if (mState.ProjectGraph.isNull() == false && mState.Source == &mState.ProjectGraph->GetDisplayFrames() &&
                mState.ProjectGraph->HasGpuFrame() == false && mState.ProjectGraph->GetImage().isNull() == false)
            {
                QSGTexture* wrapperOrNull = window()->createTextureFromImage(mState.ProjectGraph->GetImage());
                if (wrapperOrNull == nullptr)
                {
                    return node;
                }
                if (node == nullptr)
                {
                    node = new QSGSimpleTextureNode;
                    node->setOwnsTexture(false);
                }
                if (mState.Pending >= 0)
                {
                    mState.Frames[mState.Pending].IsRetiring = true;
                    mState.Pending = -1;
                }
                if (mState.Active >= 0)
                {
                    Frame& previous = mState.Frames[mState.Active];
                    previous.IsRetiring = true;
                    delete previous.Wrapper;
                    previous.Wrapper = nullptr;
                    mState.Active = -1;
                }
                delete mState.CpuWrapperOrNull;
                mState.CpuWrapperOrNull = wrapperOrNull;
                node->setTexture(wrapperOrNull);
                const QSizeF size =
                    QSizeF(mState.ProjectGraph->GetImage().size()).scaled(boundingRect().size(), Qt::KeepAspectRatio);
                node->setRect(QRectF(QPointF((width() - size.width()) / 2, (height() - size.height()) / 2), size));
                mState.PresentedCpuImageKey = mState.ProjectGraph->GetImage().cacheKey();
                ++mState.Presented;
                return node;
            }
            if (mState.Pending >= 0)
            {
                Frame& next = mState.Frames[mState.Pending];
                next.Wrapper = mState.Device->WrapTexture(*window(), next.Read.Image);
                if (next.Wrapper == nullptr)
                {
                    mState.MessageOrNull->ShowProblem(
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
                delete mState.CpuWrapperOrNull;
                mState.CpuWrapperOrNull = nullptr;
                node->setTexture(next.Wrapper);
                if (mState.ProjectGraph.isNull() == false && mState.Source == &mState.ProjectGraph->GetDisplayFrames())
                {
                    mState.PaintTrace.Queue(next.TraceIdentity, next.FrameBeginTicks);
                    mState.PaintTrace.BeginPaint();
                }
                ++mState.Presented;
            }
            if (node != nullptr)
            {
                QRectF target = boundingRect();
                if (mState.ProjectGraph.isNull() == false &&
                    mState.Source == &mState.ProjectGraph->GetDisplayFrames() && mState.Active >= 0)
                {
                    const rhi::NativeTextureInfo& image = mState.Frames[mState.Active].Read.Image;
                    const QSizeF size = QSizeF(image.Width, image.Height).scaled(target.size(), Qt::KeepAspectRatio);
                    target = QRectF(QPointF((width() - size.width()) / 2, (height() - size.height()) / 2), size);
                }
                node->setRect(target);
            }
            return node;
        }

    private:
        State& mState;
    };
    QQuickItem* ObjectOverlayOrNull = nullptr;
    QQuickWindow* Window = nullptr;
    StatisticsItem* StatisticsOrNull = nullptr;
    StatisticsItem* ShaderStatusOrNull = nullptr;
    LegendItem* LegendOrNull = nullptr;
    OrientationItem* OrientationOrNull = nullptr;
    /// Live shader rebuilds of this viewport's preview; null where unsupported.
    PreviewShaderReload* ShaderReloadOrNull = nullptr;
    QWidget* Container = nullptr;
    FrameCanvas* CanvasOrNull = nullptr;
    WorldGraphView* GraphViewOrNull = nullptr;
    QPointer<RenderGraphPreview> ProjectGraph;
    ViewportPaintTrace PaintTrace;
    QSGTexture* CpuWrapperOrNull = nullptr;
    int64 PresentedCpuImageKey = 0;
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
    /// Show the key of a debug view's ramp over the frame, or hide it for the other modes.
    void applyLegend(PreviewShading shading)
    {
        PreviewLegend legend;
        const bool hasLegend = TryGetPreviewLegend(shading, legend);
        if (LegendOrNull != nullptr)
        {
            LegendOrNull->SetLegend(hasLegend == true ? &legend : nullptr);
        }
        if (CanvasOrNull != nullptr)
        {
            CanvasOrNull->SetLegend(hasLegend == true ? &legend : nullptr);
        }
    }
    /// Show the axis widget for the current camera while a frame is on screen and the game is not playing.
    void applyOrientation()
    {
        if (Widget == nullptr || Preview == nullptr)
        {
            return;
        }
        const bool isShown = Widget->IsOrientationWidgetShown();
        const gameplay::camera::CameraSample& sample = Preview->GetVisibleCameraSample();
        if (OrientationOrNull != nullptr)
        {
            OrientationOrNull->SetCamera(sample, isShown);
        }
        if (CanvasOrNull != nullptr)
        {
            CanvasOrNull->SetOrientation(sample, isShown);
        }
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
        if (GraphViewOrNull != nullptr)
        {
            ProjectGraph->SetDirectPresentation(false);
            Widget->SetFrameSource(Preview.data());
            if (Preview->AdmitOffscreenDevice() == true)
            {
                GraphViewOrNull->show();
                MessageOrNull->hide();
            }
            else
            {
                MessageOrNull->ShowProblem(Preview->Status());
                MessageOrNull->show();
            }
            return;
        }
        CanvasOrNull = new FrameCanvas(Widget, QStringLiteral("LudusOffscreenViewport"));
        CanvasOrNull->SetInteraction(Widget->GetObjectInteraction());
        CanvasOrNull->SetStatisticsText(StatisticsText);
        CanvasOrNull->SetShaderStatusText(ShaderStatusText, HasShaderError);
        applyLegend(Preview->GetShading());
        FrameLayoutOrNull->addWidget(CanvasOrNull);
        MessageOrNull->hide();
        applyOrientation();
        CanvasOrNull->show();
        if (Preview->AdmitOffscreenDevice() == false)
        {
            const QString status = Preview->Status();
            Preview->Stop();
            delete CanvasOrNull;
            CanvasOrNull = nullptr;
            MessageOrNull->ShowProblem(status);
            MessageOrNull->show();
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
                MessageOrNull->ShowProblem(
                    QStringLiteral("Offscreen viewport copy failed. Editing remains available."));
                MessageOrNull->show();
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
            MessageOrNull->ShowProblem(
                QStringLiteral("Offscreen viewport copy is unavailable. Editing remains available."));
            MessageOrNull->show();
            return;
        }
        static_cast<void>(Reader.Poll());
    }
    void createWindow()
    {
        if (GraphViewOrNull != nullptr && PreviewViewport::GetPresentation() != ViewportPresentation::SharedDevice)
        {
            if (Preview->HasDevice() == false && Preview->AdmitOffscreenDevice() == false)
            {
                MessageOrNull->ShowProblem(Preview->Status());
                MessageOrNull->show();
            }
            else
            {
                MessageOrNull->hide();
                GraphViewOrNull->show();
            }
            return;
        }
        if (Window != nullptr || CanvasOrNull != nullptr || Preview.isNull() == true)
        {
            return;
        }
        const ViewportPresentation presentation = PreviewViewport::GetPresentation();
        if (ProjectGraph.isNull() == false)
        {
            ProjectGraph->SetDirectPresentation(presentation == ViewportPresentation::SharedDevice);
            GraphViewOrNull->hide();
        }
        if (presentation == ViewportPresentation::OffscreenReadback)
        {
            createCanvas();
            return;
        }
        if (presentation != ViewportPresentation::SharedDevice)
        {
            Preview->Stop();
            MessageOrNull->ShowProblem(
                QStringLiteral("This display cannot show the GPU viewport (needs macOS Metal, or "
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
        Window->setColor(QColor::fromRgb(VIEWPORT_BACKGROUND));
        Device.Reset(new (std::nothrow) QtViewportDevice);
        if (Device.Get() == nullptr || Device->PrepareWindow(*Window) == false)
        {
            destroyWindow();
#    if defined(Q_OS_MACOS)
            Preview->Stop();
            MessageOrNull->ShowProblem(QStringLiteral("Qt GPU initialization failed. Editing remains available."));
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
                    MessageOrNull->ShowProblem(
                        QStringLiteral("Qt Quick did not adopt the shared GPU device. Editing remains "
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
                PaintTrace.EndPaint();
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
                             MessageOrNull->ShowProblem(error);
                             QTimer::singleShot(0, Widget, [this]() { Preview->Stop(); });
                         });
        QObject::connect(Window, &QQuickWindow::widthChanged, Widget, [this]() {
            static_cast<PreviewViewport*>(Widget)->resizeSurface();
        });
        QObject::connect(Window, &QQuickWindow::heightChanged, Widget, [this]() {
            static_cast<PreviewViewport*>(Widget)->resizeSurface();
        });
        createItem();
        ObjectOverlayOrNull = CreateViewportObjectOverlayItem(Widget->GetObjectInteraction(), *Window->contentItem());
        ObjectOverlayOrNull->setSize(Window->size());
        StatisticsOrNull = new StatisticsItem(Window->contentItem(), QStringLiteral("viewportStatisticsOverlay"));
        StatisticsOrNull->SetText(StatisticsText);
        ShaderStatusOrNull = new StatisticsItem(Window->contentItem(), QStringLiteral("viewportShaderStatusOverlay"));
        applyShaderStatus();
        LegendOrNull = new LegendItem(Window->contentItem());
        applyLegend(Preview->GetShading());
        OrientationOrNull = new OrientationItem(Window->contentItem());
        applyOrientation();
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
        FrameLayoutOrNull->addWidget(Container);
        MessageOrNull->hide();
        Container->show();
        Window->update();
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
        bool hasFrames = CpuWrapperOrNull != nullptr;
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
        delete CpuWrapperOrNull;
        CpuWrapperOrNull = nullptr;
        PresentedCpuImageKey = 0;
        PaintTrace.Cancel();
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
            ObjectOverlayOrNull = nullptr;
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
        LegendOrNull = nullptr;
        OrientationOrNull = nullptr;
        Window = nullptr;
        Device.Reset();
        IsDestroyingWindow = false;
        MessageOrNull->show();
    }
    void showFrame()
    {
        if (CanvasOrNull != nullptr && Preview.isNull() == false && Source.isNull() == false)
        {
            serviceCanvas();
            return;
        }
        if (ProjectGraph.isNull() == false && Source == &ProjectGraph->GetDisplayFrames() &&
            ProjectGraph->HasGpuFrame() == false && Window != nullptr && Item != nullptr &&
            ProjectGraph->GetImage().isNull() == false && ProjectGraph->GetImage().cacheKey() != PresentedCpuImageKey &&
            Widget->isVisible() == true)
        {
            Item->update();
            Window->update();
            return;
        }
        if (Preview.isNull() == true || Source.isNull() == true || Window == nullptr || Pending >= 0 ||
            Source->HasPendingFrame() == false)
        {
            return;
        }
        if (ProjectGraph.isNull() == false && Source == &ProjectGraph->GetDisplayFrames() &&
            (Widget->isVisible() == false ||
             (Active >= 0 && Frames[Active].Read.Token == ProjectGraph->GetDisplayFrames().GetLatestSerial())))
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
                MessageOrNull->ShowProblem(QStringLiteral("GPU texture sharing failed. Editing remains available."));
                return;
            }
            frame.HasLease = true;
            if (ProjectGraph.isNull() == false && Source == &ProjectGraph->GetDisplayFrames())
            {
                const ViewportFrameTrace& trace = ProjectGraph->GetFrameTrace();
                frame.TraceIdentity = trace.GetPublishedIdentity();
                frame.FrameBeginTicks = trace.GetPublishedBeginTicks();
            }
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
    mObjectInteraction = new ViewportObjectInteraction(*preview, this);
    mState.Reset(new State);
    updateObjectInteractionAvailability();
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
    shading->setMinimumContentsLength(SHADING_COMBO_CHARACTERS);
    shading->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
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
    // A toolbar rather than a plain row: controls that do not fit move behind its overflow chevron instead of
    // being squeezed until their text is cut off.
    QToolBar* controls = new QToolBar(this);
    controls->setObjectName(QStringLiteral("viewportControls"));
    controls->setAccessibleName(QStringLiteral("Viewport controls"));
    controls->setMovable(false);
    controls->setFloatable(false);
    controls->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
    mState->ControlsOrNull = controls;
    controls->addWidget(shading);
    QComboBox* viewportSize = new QComboBox(this);
    viewportSize->setObjectName(QStringLiteral("viewportSize"));
    viewportSize->setAccessibleName(QStringLiteral("Viewport size"));
    viewportSize->setSizeAdjustPolicy(QComboBox::AdjustToContents);
    for (foundation::usize i = 0; i < VIEWPORT_SIZE_PRESET_COUNT; ++i)
    {
        if (IsViewportSizeGroupStart(i) == true)
        {
            viewportSize->insertSeparator(viewportSize->count());
        }
        viewportSize->addItem(GetViewportSizeLabel(i), QVariant::fromValue(i));
        viewportSize->setItemData(viewportSize->count() - 1, GetViewportSizeDescription(i), Qt::ToolTipRole);
    }
    viewportSize->setCurrentIndex(viewportSize->findData(QVariant::fromValue(preview->GetViewportSize())));
    viewportSize->setToolTip(
        QStringLiteral("Shape and resolution of the rendered image. Fill uses the whole viewport. An aspect ratio "
                       "letterboxes the image to that shape at the viewport's own pixel density. A fixed "
                       "resolution renders exactly that many pixels and scales the image to fit, so a project "
                       "graph's Viewport pixels output and Play in the viewport receive that size."));
    controls->addWidget(viewportSize);
    connect(viewportSize, &QComboBox::currentIndexChanged, preview, [preview, viewportSize]() {
        bool isIndex = false;
        const foundation::usize index = viewportSize->currentData().toULongLong(&isIndex);
        if (isIndex == true)
        {
            preview->SetViewportSize(index);
        }
    });
    connect(preview, &RenderPreview::NavigationChanged, this, [this, preview, viewportSize]() {
        const QSignalBlocker blocker(viewportSize);
        viewportSize->setCurrentIndex(viewportSize->findData(QVariant::fromValue(preview->GetViewportSize())));
        applyViewportSize();
    });
    QPushButton* view = new QPushButton(this);
    view->setObjectName(QStringLiteral("viewportView"));
    view->setAccessibleName(QStringLiteral("Viewport projection and view direction"));
    view->setToolTip(QStringLiteral("Perspective, or an orthographic view along a world axis for aligning and "
                                    "measuring. Axis views keep the pivot and the visible size there, and look from "
                                    "outside the scene. With the viewport focused: numpad 7, 1 and 3 for Top, Front "
                                    "and Right (add Ctrl, or Command on macOS, for the opposite side), numpad 5 to "
                                    "switch projection. The menu also sets the perspective field of view and the "
                                    "mouse look sensitivity."));
    QMenu* viewMenu = new QMenu(view);
    QAction* perspective = viewMenu->addAction(QStringLiteral("Perspective"));
    perspective->setObjectName(QStringLiteral("viewportViewPerspective"));
    connect(perspective, &QAction::triggered, preview, [preview]() { preview->SetOrthographic(false); });
    QAction* orthographic = viewMenu->addAction(QStringLiteral("Orthographic"));
    orthographic->setObjectName(QStringLiteral("viewportViewOrthographic"));
    connect(orthographic, &QAction::triggered, preview, [preview]() { preview->SetOrthographic(true); });
    viewMenu->addSeparator();
    for (foundation::usize i = 0; i < EDITOR_AXIS_VIEW_COUNT; ++i)
    {
        const EditorAxisView axis = static_cast<EditorAxisView>(i);
        QAction* action = viewMenu->addAction(QString::fromUtf8(GetEditorAxisViewLabel(axis)));
        action->setObjectName(QStringLiteral("viewportView%1").arg(QString::fromUtf8(GetEditorAxisViewLabel(axis))));
        connect(action, &QAction::triggered, preview, [preview, axis]() { preview->SetAxisView(axis); });
    }
    // The field of view lives in the View menu beside the projection it applies to, which keeps
    // the toolbar row narrow enough for its buttons to show their full text.
    viewMenu->addSeparator();
    QDoubleSpinBox* fieldOfView = new QDoubleSpinBox(viewMenu);
    fieldOfView->setObjectName(QStringLiteral("viewportFieldOfView"));
    fieldOfView->setAccessibleName(QStringLiteral("Vertical field of view in degrees"));
    fieldOfView->setPrefix(QStringLiteral("FOV "));
    fieldOfView->setSuffix(QStringLiteral("°"));
    fieldOfView->setDecimals(0);
    fieldOfView->setRange(static_cast<float64>(MINIMUM_EDITOR_VERTICAL_FOV_DEGREES),
                          static_cast<float64>(MAXIMUM_EDITOR_VERTICAL_FOV_DEGREES));
    fieldOfView->setSingleStep(FIELD_OF_VIEW_STEP_DEGREES);
    fieldOfView->setToolTip(QStringLiteral("Vertical field of view of the editor camera. Wider angles show more "
                                           "of the scene with more perspective distortion. Orthographic views "
                                           "have no field of view."));
    QWidgetAction* fieldOfViewAction = new QWidgetAction(viewMenu);
    fieldOfViewAction->setDefaultWidget(fieldOfView);
    viewMenu->addAction(fieldOfViewAction);
    // Mouse look sensitivity sits beside the field of view for the same reason: with it in the
    // toolbar row, Linux's default fonts left the View button a pixel short at a 1280-pixel window.
    QDoubleSpinBox* lookSensitivity = new QDoubleSpinBox(viewMenu);
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
    QWidgetAction* lookSensitivityAction = new QWidgetAction(viewMenu);
    lookSensitivityAction->setDefaultWidget(lookSensitivity);
    viewMenu->addAction(lookSensitivityAction);
    view->setMenu(viewMenu);
    controls->addWidget(view);
    connect(fieldOfView, &QDoubleSpinBox::valueChanged, preview, [preview](float64 degrees) {
        preview->SetVerticalFovDegrees(static_cast<float32>(degrees));
    });
    const auto updateView = [preview, view, perspective, orthographic, fieldOfView]() {
        const gameplay::camera::CameraSample& sample = preview->GetVisibleCameraSample();
        const bool isOrthographic = sample.Lens.Projection == gameplay::camera::CameraProjection::OrthographicFinite;
        EditorAxisView axis = EditorAxisView::Top;
        if (isOrthographic == false)
        {
            view->setText(QStringLiteral("Perspective"));
        }
        else if (TryGetEditorAxisView(sample, axis) == true)
        {
            view->setText(QString::fromUtf8(GetEditorAxisViewLabel(axis)));
        }
        else
        {
            view->setText(QStringLiteral("Orthographic"));
        }
        perspective->setEnabled(isOrthographic == true);
        orthographic->setEnabled(isOrthographic == false);
        fieldOfView->setEnabled(isOrthographic == false);
        const QSignalBlocker blocker(fieldOfView);
        fieldOfView->setValue(static_cast<float64>(math::RadiansToDegrees(sample.Lens.VerticalFov)));
    };
    updateView();
    connect(preview, &RenderPreview::NavigationChanged, view, updateView);
    connect(lookSensitivity, &QDoubleSpinBox::valueChanged, preview, [preview](float64 sensitivity) {
        preview->SetLookSensitivity(static_cast<float32>(sensitivity));
    });
    connect(preview, &RenderPreview::NavigationChanged, lookSensitivity, [preview, lookSensitivity]() {
        const QSignalBlocker blocker(lookSensitivity);
        lookSensitivity->setValue(static_cast<float64>(preview->GetLookSensitivity()));
    });
    QComboBox* objectTool = new QComboBox(this);
    objectTool->setObjectName(QStringLiteral("viewportObjectTool"));
    objectTool->setAccessibleName(QStringLiteral("Object interaction tool"));
    objectTool->addItems({QStringLiteral("Select (Q)"),
                          QStringLiteral("Move (W)"),
                          QStringLiteral("Rotate (E)"),
                          QStringLiteral("Scale (R)")});
    objectTool->setCurrentIndex(static_cast<int32>(ViewportObjectInteraction::Tool::Move));
    objectTool->setToolTip(QStringLiteral("Click an object to select it. Drag an axis handle to transform it. "
                                          "Shift snaps; Escape cancels. Alt-drag or right-drag navigates."));
    objectTool->hide();
    controls->addWidget(objectTool);
    updateObjectInteractionAvailability();
    if (mObjectInteraction->IsActive() == false)
    {
        objectTool->setToolTip(
            QStringLiteral("Object interaction is unavailable in this viewport. "
                           "Select objects in the hierarchy and edit their transforms in Properties."));
    }
    connect(objectTool, &QComboBox::currentIndexChanged, this, [this](int32 index) {
        mObjectInteraction->SetTool(static_cast<ViewportObjectInteraction::Tool>(index));
    });
    connect(mObjectInteraction, &ViewportObjectInteraction::Changed, objectTool, [this, objectTool]() {
        const QSignalBlocker blocker(objectTool);
        objectTool->setCurrentIndex(static_cast<int32>(mObjectInteraction->GetTool()));
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
#if defined(LUDUS_EDITOR_GPU_PREVIEW)
    QToolButton* reference = new QToolButton(this);
    reference->setObjectName(QStringLiteral("viewportReference"));
    reference->setText(QStringLiteral("Reference…"));
    reference->setAccessibleName(QStringLiteral("Open reference capture"));
    reference->setToolTip(QStringLiteral("Render a path-traced reference image of this viewport's current camera "
                                         "with the renderer's RT1 progressive path tracer."));
    controls->addWidget(reference);
    connect(reference, &QToolButton::clicked, this, [this, preview]() {
        if (mState->ReferenceWindow.isNull() == true)
        {
            mState->ReferenceWindow = new ReferenceCaptureWindow(preview, this);
        }
#    if defined(LUDUS_EDITOR_NATIVE_PREVIEW)
        // The project graph may have been adopted after the window was created.
        mState->ReferenceWindow->SetProjectGraph(mState->ProjectGraph.data());
#    endif
        mState->ReferenceWindow->show();
        mState->ReferenceWindow->raise();
        mState->ReferenceWindow->activateWindow();
    });
#endif
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
    // Navigation help lives in a tooltip on the controls row instead of a line of its own,
    // so the rendered image keeps the height; clicking shows it without waiting for hover.
    QToolButton* navigationHint = new QToolButton(this);
    navigationHint->setObjectName(QStringLiteral("viewportNavigationHint"));
    navigationHint->setText(QStringLiteral("?"));
    navigationHint->setAccessibleName(QStringLiteral("Viewport navigation help"));
#if defined(Q_OS_MACOS)
    navigationHint->setToolTip(QStringLiteral("Option-drag: orbit · Shift-Option-drag: pan · Scroll: zoom\n") +
                               toolTip());
#else
    navigationHint->setToolTip(QStringLiteral("Alt-drag: orbit · Shift-Alt-drag: pan · Scroll: zoom\n") + toolTip());
#endif
    controls->addWidget(navigationHint);
    connect(navigationHint, &QToolButton::clicked, navigationHint, [navigationHint]() {
        QToolTip::showText(navigationHint->mapToGlobal(navigationHint->rect().bottomLeft()),
                           navigationHint->toolTip(),
                           navigationHint);
    });
    mState->Layout->addWidget(controls);
#if defined(LUDUS_EDITOR_BROWSER_PREVIEW)
    mState->StatisticsLabelOrNull = new QLabel(this);
    mState->StatisticsLabelOrNull->setObjectName(QStringLiteral("viewportStatisticsOverlay"));
    mState->StatisticsLabelOrNull->setTextFormat(Qt::PlainText);
    mState->StatisticsLabelOrNull->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
    mState->StatisticsLabelOrNull->hide();
    mState->Layout->addWidget(mState->StatisticsLabelOrNull, 0, Qt::AlignTop);
#endif
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
#if defined(LUDUS_EDITOR_NATIVE_PREVIEW)
    connect(preview, &RenderPreview::NavigationChanged, this, [this, preview]() {
        mState->applyLegend(preview->GetShading());
        mState->applyOrientation();
    });
#endif
    mState->MessageOrNull = new EmptyWorldView(*preview, this);
    mState->MessageOrNull->SetPrompt(QStringLiteral("Open or create a World to start authoring."));
    mState->MessageOrNull->setWordWrap(true);
    // The orientation widget follows the message: it shows only while a frame is on screen.
    mState->MessageOrNull->installEventFilter(this);
    mState->FrameLayoutOrNull = new ViewportFrameLayout;
    mState->Layout->addLayout(mState->FrameLayoutOrNull, 1);
    mState->FrameLayoutOrNull->addWidget(mState->MessageOrNull);
    connect(preview, &RenderPreview::Failed, this, [this](const QString& error) {
        mState->MessageOrNull->ShowProblem(error);
#if defined(LUDUS_EDITOR_BROWSER_PREVIEW)
        mState->MessageOrNull->show();
#endif
    });
#if defined(LUDUS_EDITOR_BROWSER_PREVIEW)
    mState->BrowserOrNull = new BrowserViewport(*preview, this);
    mState->BrowserOrNull->hide();
    mState->FrameLayoutOrNull->addWidget(mState->BrowserOrNull);
    connect(preview, &RenderPreview::DeviceRequested, this, [this]() {
        mState->MessageOrNull->hide();
        mState->BrowserOrNull->show();
        mState->Layout->activate();
        if (mState->BrowserOrNull->Start() == false)
        {
            mState->BrowserOrNull->hide();
            mState->MessageOrNull->ShowProblem(mState->Preview->Status());
            mState->MessageOrNull->show();
        }
    });
#endif
#if defined(LUDUS_EDITOR_NATIVE_PREVIEW)
    connect(preview, &RenderPreview::DeviceRequested, this, [this]() { mState->createWindow(); });
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
        if (mState->GraphViewOrNull != nullptr && mState->Window == nullptr)
        {
            return;
        }
        mState->recreatePendingSurface();
        if (mState->Window != nullptr && mState->Preview->HasDevice() == false)
        {
            // The first frame admits Qt's device even before a producer image exists.
            mState->Window->update();
        }
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
    applyViewportSize();
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
#if defined(LUDUS_EDITOR_NATIVE_PREVIEW)
    if (frameSourceOrNull == nullptr && mState->ProjectGraph.isNull() == false && mState->Window != nullptr)
    {
        next = &mState->ProjectGraph->GetDisplayFrames();
    }
#endif
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
    mState->ProjectGraph = inoutWorkspace.GetPreviewOrNull();
    mState->GraphViewOrNull = new WorldGraphView(inoutWorkspace, this);
    mState->GraphViewOrNull->SetObjectInteraction(*mObjectInteraction);
    mState->GraphViewOrNull->setVisible(Preview_->Scene().Identity.isEmpty() == false);
    mState->GraphViewOrNull->SetCanPresent(mState->IsAutomatic);
    mState->FrameLayoutOrNull->addWidget(mState->GraphViewOrNull);
    QWidget* graphControls = mState->GraphViewOrNull->TakeControls(this);
    mState->ControlsOrNull->insertWidget(mState->ControlsOrNull->actions().value(0), graphControls);
    SetControlVisible(*mState->ControlsOrNull, findChild<QComboBox*>(QStringLiteral("viewportShading")), false);
    SetControlVisible(*mState->ControlsOrNull, findChild<QToolButton*>(QStringLiteral("viewportLiveShaders")), false);
    QToolButton* statistics = findChild<QToolButton*>(QStringLiteral("viewportStatistics"));
    LUDUS_REQUIRE(statistics != nullptr);
    statistics->setToolTip(QStringLiteral("Show delivered-image FPS, active CPU work, GPU device time and elapsed "
                                          "readback/conversion latency. Times average the last 30 samples; "
                                          "FPS falls to zero when idle. GPU shows n/a without timestamps."));
    if (GetPresentation() == ViewportPresentation::SharedDevice)
    {
        SetFrameSource(&mState->ProjectGraph->GetDisplayFrames());
        mState->GraphViewOrNull->hide();
        connect(mState->ProjectGraph, &RenderGraphPreview::FramePresented, this, &PreviewViewport::updateStatistics);
        connect(mState->GraphViewOrNull, &WorldGraphView::StatusChanged, this, [this](const QString& text) {
            if (mFrameSource == &mState->ProjectGraph->GetDisplayFrames())
            {
                mState->ShaderStatusText = text;
                mState->HasShaderError = false;
                mState->applyShaderStatus();
            }
        });
        connect(mState->GraphViewOrNull, &WorldGraphView::StatisticsChanged, this, [this]() {
            mState->StatisticsText = mState->GraphViewOrNull->GetStatisticsText();
            mState->applyStatistics();
        });
    }
    connect(mState->ProjectGraph, &RenderGraphPreview::FramePresented, this, [this]() {
        if (mIsInspectingRayPaths == false)
        {
            mObjectInteraction->SetImageSize(mState->ProjectGraph->GetTargetSize());
        }
    });
    updateStatistics();
    mRayPaths = inoutWorkspace.GetRayPathInspectionOrNull();
    mRayPathWorkspaceOrNull = &inoutWorkspace;
    if (mRayPaths.isNull() == false)
    {
        connect(mRayPaths, &RayPathInspection::Changed, this, &PreviewViewport::updateRayPathInspection);
        connect(mRayPaths, &RayPathInspection::OverlayChanged, this, &PreviewViewport::updateRayPathOverlay);
    }
    if (wasRunning == true)
    {
        static_cast<void>(Preview_->Start());
    }
#else
    static_cast<void>(inoutWorkspace);
#endif
}
void PreviewViewport::updateRayPathInspection()
{
#if defined(LUDUS_EDITOR_NATIVE_PREVIEW)
    if (mRayPaths.isNull() == true || mRayPathWorkspaceOrNull == nullptr)
    {
        return;
    }
    // Only the viewport whose preview feeds the graph presents its inspection.
    const bool isSource = mRayPathWorkspaceOrNull->GetPreviewSourceOrNull() == Preview_;
    const RayPathInspectionStage stage = mRayPaths->GetStage();
    const bool shouldInspect = isSource == true && stage == RayPathInspectionStage::Inspecting;
    if (isSource == true && stage == RayPathInspectionStage::PickingPixel)
    {
        setCursor(Qt::CrossCursor);
    }
    else
    {
        unsetCursor();
    }
    const QString status = isSource == true ? mRayPaths->GetStatus() : QString{};
    if (status.isEmpty() == false || mIsShowingRayPathStatus == true)
    {
        mState->ShaderStatusText = status;
        mState->HasShaderError = false;
        mState->applyShaderStatus();
        mIsShowingRayPathStatus = status.isEmpty() == false;
    }
    if (shouldInspect == mIsInspectingRayPaths)
    {
        return;
    }
    mObjectInteraction->SetImageSize(shouldInspect == true ? QSize{} : mState->ProjectGraph->GetTargetSize());
    mIsInspectingRayPaths = shouldInspect;
    updateObjectInteractionAvailability();
    // The inspection view is the built-in raster preview from the free camera; never the frozen graph image.
    Preview_->SetRayPathInspection(shouldInspect);
    if (mState->Window != nullptr)
    {
        SetFrameSource(shouldInspect == true ? Preview_ : nullptr);
    }
    else if (mState->GraphViewOrNull != nullptr)
    {
        mState->GraphViewOrNull->setVisible(shouldInspect == false && Preview_->Scene().Identity.isEmpty() == false);
    }
    updateRayPathOverlay();
#endif
}
void PreviewViewport::updateRayPathOverlay()
{
#if defined(LUDUS_EDITOR_NATIVE_PREVIEW)
    if (mRayPaths.isNull() == true || mIsInspectingRayPaths == false)
    {
        return;
    }
    math::Aabb3 boundsWorld;
    const bool hasBounds = Preview_->Scene().TryGetBounds({}, boundsWorld);
    Preview_->SetRayPathOverlay(mRayPaths->BuildOverlay(hasBounds == true ? &boundsWorld : nullptr));
#endif
}
void PreviewViewport::updateObjectInteractionAvailability()
{
    const ViewportPresentation presentation = GetPresentation();
    const bool canShowOverlay =
        presentation == ViewportPresentation::SharedDevice || presentation == ViewportPresentation::OffscreenReadback;
    const bool canInteract = canShowOverlay == true && mPlayEndpointOrNull == nullptr && mIsInspectingRayPaths == false;
    mObjectInteraction->SetActive(canInteract);
    QComboBox* toolOrNull = findChild<QComboBox*>(QStringLiteral("viewportObjectTool"));
    if (toolOrNull != nullptr)
    {
        toolOrNull->setEnabled(canInteract);
    }
#if defined(LUDUS_EDITOR_NATIVE_PREVIEW)
    mState->applyOrientation();
#endif
}
bool PreviewViewport::IsOrientationWidgetShown() const noexcept
{
    return mPlayEndpointOrNull == nullptr && mIsInspectingRayPaths == false && mState->MessageOrNull != nullptr &&
           mState->MessageOrNull->isHidden() == true;
}
bool PreviewViewport::tryPickOrientationWidget(QPointF globalPosition)
{
    if (IsOrientationWidgetShown() == false || mState->FrameLayoutOrNull == nullptr)
    {
        return false;
    }
    const QRect frame = mState->FrameLayoutOrNull->GetFrameRectangle();
    const QPointF local = getInteractionPosition(globalPosition) - QPointF(frame.topLeft());
    EditorAxisView view = EditorAxisView::Front;
    if (TryHitOrientationHandle(BuildOrientationLayout(Preview_->GetVisibleCameraSample(),
                                                       GetOrientationWidgetRect(static_cast<qreal>(frame.width()))),
                                local,
                                view) == false)
    {
        return false;
    }
    Preview_->SetAxisView(view);
    return true;
}
QRectF PreviewViewport::getInteractionFrame() const
{
    return mState->FrameLayoutOrNull != nullptr ? mState->FrameLayoutOrNull->GetFrameRectangle() : QRectF{};
}
QPointF PreviewViewport::getInteractionPosition(QPointF globalPosition) const
{
    return mapFromGlobal(globalPosition);
}
bool PreviewViewport::pickRayPathPixel(QPointF globalPosition)
{
#if defined(LUDUS_EDITOR_NATIVE_PREVIEW)
    if (mRayPaths.isNull() == true || mRayPathWorkspaceOrNull == nullptr ||
        mRayPaths->GetStage() != RayPathInspectionStage::PickingPixel ||
        mRayPathWorkspaceOrNull->GetPreviewSourceOrNull() != Preview_)
    {
        return false;
    }
    // The graph output fills the letterboxed frame, as both presentation paths show it.
    const QRect frame = mState->FrameLayoutOrNull->GetFrameRectangle();
    const QPointF local = QPointF(mapFromGlobal(globalPosition.toPoint())) - QPointF(frame.topLeft());
    if (frame.isEmpty() == true || local.x() < 0 || local.y() < 0 || local.x() >= frame.width() ||
        local.y() >= frame.height())
    {
        return true;
    }
    static_cast<void>(mRayPaths->PickPixel(
        {static_cast<float32>(local.x() / frame.width()), static_cast<float32>(local.y() / frame.height())}));
    return true;
#else
    static_cast<void>(globalPosition);
    return false;
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
    mState->MessageOrNull->hide();
    mState->BrowserOrNull->ShowFrame();
#endif
#if defined(LUDUS_EDITOR_NATIVE_PREVIEW)
    // A manually gated import may present only after its catalog is committed.
    // Once admitted, also retry frames that arrived while both slots were leased.
    mState->CanShowFrames = true;
    if (mState->GraphViewOrNull == nullptr || mState->Window != nullptr)
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
    return mState->GraphViewOrNull != nullptr &&
           ((mState->Window != nullptr && (mState->Active >= 0 || mState->CpuWrapperOrNull != nullptr)) ||
            mState->GraphViewOrNull->GetImage().isNull() == false);
#else
    return false;
#endif
}
uint64 PreviewViewport::GetPresentedGraphFrameSerial() const noexcept
{
#if defined(LUDUS_EDITOR_NATIVE_PREVIEW)
    if (mState->ProjectGraph.isNull() == false && mState->Source == &mState->ProjectGraph->GetDisplayFrames() &&
        mState->Active >= 0)
    {
        return mState->Frames[mState->Active].Read.Token;
    }
#endif
    return 0;
}
void PreviewViewport::ClearFrame(const QString& text)
{
#if defined(LUDUS_EDITOR_BROWSER_PREVIEW)
    mState->BrowserOrNull->ClearFrame();
    mState->BrowserOrNull->hide();
    mState->MessageOrNull->show();
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
    mState->MessageOrNull->SetPrompt(text);
    if (Preview_->Scene().Identity.isEmpty() == true)
    {
        mState->MessageOrNull->show();
        mState->MessageOrNull->update();
    }
}
QString PreviewViewport::GetStatisticsText() const
{
#if defined(LUDUS_EDITOR_NATIVE_PREVIEW)
    if (mState->GraphViewOrNull != nullptr && mState->Window == nullptr)
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
        if (GetPresentation() == ViewportPresentation::SharedDevice)
        {
            mState->StatisticsText = Preview_->IsStatisticsVisible() == true
                                         ? FormatGraphViewportStatistics(mState->ProjectGraph->GetStatistics(),
                                                                         Preview_->GetStatistics(),
                                                                         foundation::time::Now().Nanoseconds)
                                         : QString{};
            mState->applyStatistics();
        }
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
    if (watched == mState->MessageOrNull && (event->type() == QEvent::Show || event->type() == QEvent::Hide))
    {
        mState->applyOrientation();
    }
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
    if (mState->GraphViewOrNull != nullptr && mState->Window == nullptr)
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
    const ViewportSize size = GetViewportSizePreset(Preview_->GetViewportSize());
    if (mState->CanvasOrNull != nullptr)
    {
        // RenderPreview recreates the offscreen targets when the requested size changes.
        const QSize drawable =
            ComputeViewportDrawableSize(size, mState->CanvasOrNull->size(), mState->CanvasOrNull->devicePixelRatioF());
        mFrameSource->Resize(drawable.width(), drawable.height());
        return;
    }
    if (mState->Window != nullptr && mState->Item != nullptr)
    {
        const QSize drawable =
            ComputeViewportDrawableSize(size, mState->Window->size(), mState->Window->devicePixelRatio());
        if (mState->ProjectGraph.isNull() == false && mFrameSource == &mState->ProjectGraph->GetDisplayFrames())
        {
            Preview_->Resize(drawable.width(), drawable.height());
        }
        mFrameSource->Resize(drawable.width(), drawable.height());
        mState->Item->setSize(QSizeF(mState->Window->size()));
        if (mState->ObjectOverlayOrNull != nullptr)
        {
            mState->ObjectOverlayOrNull->setSize(mState->Window->size());
            mState->ObjectOverlayOrNull->update();
        }
        mState->Item->update();
        mState->applyShaderStatus();
    }
#endif
}
void PreviewViewport::applyViewportSize()
{
    const foundation::usize index = Preview_->GetViewportSize();
    if (index == mAppliedViewportSize || mState->FrameLayoutOrNull == nullptr)
    {
        return;
    }
    mAppliedViewportSize = index;
    const ViewportSize size = GetViewportSizePreset(index);
#if defined(LUDUS_EDITOR_BROWSER_PREVIEW)
    mState->BrowserOrNull->SetFixedDrawableSize(
        size.Kind == ViewportSizeKind::Fixed ? ComputeViewportDrawableSize(size, {}, 1) : QSize());
#endif
    mState->FrameLayoutOrNull->SetSize(size);
    // A fixed size may keep the frame's shape, so its drawable changes without a resize.
    resizeSurface();
    update();
}
void PreviewViewport::paintEvent(QPaintEvent*)
{
    if (GetViewportSizePreset(mAppliedViewportSize).Kind == ViewportSizeKind::Fill ||
        mState->FrameLayoutOrNull == nullptr)
    {
        return;
    }
    // The bars beside a letterboxed frame belong to the viewport, not to the surrounding panel.
    QPainter painter(this);
    painter.fillRect(mState->FrameLayoutOrNull->geometry(), QColor::fromRgb(VIEWPORT_BACKGROUND));
    painter.setPen(QColor::fromRgb(FRAME_OUTLINE));
    painter.drawRect(mState->FrameLayoutOrNull->GetFrameRectangle().adjusted(-1, -1, 0, 0));
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
    return mState->GraphViewOrNull != nullptr && mState->Window == nullptr
               ? mState->GraphViewOrNull->GetPresentedFrames()
               : mState->Presented;
#else
    return mState->Presented;
#endif
}
} // namespace ludus::editor
