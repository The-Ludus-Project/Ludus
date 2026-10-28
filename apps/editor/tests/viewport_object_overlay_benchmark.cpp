// Benchmark-only fixture: compare the previous QPicture/painted-texture path with
// the production batched scene-graph geometry at identical size, strokes and frame cadence.
#include "internal/viewport_object_overlay.h"
#include "internal/viewport_object_overlay_item.h"

#include <ludus/foundation/base/compiler.h>
#include <ludus/foundation/base/types.h>
#include <ludus/foundation/containers/array.hpp>
#include <ludus/foundation/containers/sort.hpp>

#include <QApplication>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QImage>
#include <QJsonDocument>
#include <QJsonObject>
#include <QObject>
#include <QPainter>
#include <QPen>
#include <QPicture>
#include <QQuickPaintedItem>
#include <QQuickWindow>
#include <QSGRendererInterface>
#include <QTest>
#include <QTextStream>
#include <QTimer>

#include <cstdio>

using namespace ludus::editor;
using namespace ludus::foundation;
namespace
{
constexpr int32 WIDTH_PIXELS = 1280;
constexpr int32 HEIGHT_PIXELS = 720;
constexpr int32 GRID_COLUMNS = 100;
constexpr int32 GRID_ROWS = static_cast<int32>(ViewportObjectOverlay::MAX_WIRE_TRIANGLES) / GRID_COLUMNS;
constexpr int32 WARMUP_FRAMES = 5;
constexpr int32 MEASURED_FRAMES = 30;
constexpr int32 FRAME_TIMEOUT_MILLISECONDS = 10000;
constexpr float64 NANOSECONDS_PER_MILLISECOND = 1000000;

class PaintedOverlay final : public QQuickPaintedItem
{
public:
    explicit PaintedOverlay(QQuickItem& inoutParent) : QQuickPaintedItem(&inoutParent)
    {
        setAntialiasing(true);
    }
    void Prepare(const ViewportObjectOverlay& overlay)
    {
        mHasUpdated = false;
        mPicture = QPicture{};
        QPainter painter(&mPicture);
        painter.setClipRect(overlay.FramePixel);
        painter.setRenderHint(QPainter::Antialiasing);
        painter.setPen(QPen(QColor::fromRgb(ViewportObjectOverlay::SELECTION_COLOR),
                            static_cast<float64>(ViewportObjectOverlay::WIRE_WIDTH_PIXELS)));
        for (const QLineF& linePixel : overlay.WireLinesPixel)
        {
            painter.drawLine(linePixel);
        }
        update();
    }
    void paint(QPainter* painter) override
    {
        QElapsedTimer timer;
        timer.start();
        painter->drawPicture(0, 0, mPicture);
        mCpuSamples.Add(static_cast<float64>(timer.nsecsElapsed()) / NANOSECONDS_PER_MILLISECOND);
        mHasUpdated = true;
    }
    [[nodiscard]] LUDUS_INLINE bool HasUpdated() const noexcept
    {
        return mHasUpdated;
    }
    [[nodiscard]] LUDUS_INLINE const Array<float64>& GetCpuSamples() const noexcept
    {
        return mCpuSamples;
    }

private:
    QPicture mPicture;
    Array<float64> mCpuSamples;
    bool mHasUpdated = false;
};
class GeometryOverlay final : public QQuickItem
{
public:
    GeometryOverlay(const ViewportObjectOverlay& overlay, QQuickItem& inoutParent)
        : QQuickItem(&inoutParent), mOverlay(overlay)
    {
        setFlag(ItemHasContents);
    }
    void Prepare(const ViewportObjectOverlay&)
    {
        mHasUpdated = false;
        update();
    }
    [[nodiscard]] LUDUS_INLINE bool HasUpdated() const noexcept
    {
        return mHasUpdated;
    }
    [[nodiscard]] LUDUS_INLINE const Array<float64>& GetCpuSamples() const noexcept
    {
        return mCpuSamples;
    }

protected:
    QSGNode* updatePaintNode(QSGNode* oldNodeOrNull, UpdatePaintNodeData*) override
    {
        QElapsedTimer timer;
        timer.start();
        QSGNode* nodeOrNull = UpdateViewportObjectOverlayNode(mOverlay, oldNodeOrNull);
        mCpuSamples.Add(static_cast<float64>(timer.nsecsElapsed()) / NANOSECONDS_PER_MILLISECOND);
        mHasUpdated = true;
        return nodeOrNull;
    }

private:
    const ViewportObjectOverlay& mOverlay;
    Array<float64> mCpuSamples;
    bool mHasUpdated = false;
};

template <typename Item>
QJsonObject MeasureOverlay(const ViewportObjectOverlay& overlay, Item& inoutItem, QQuickWindow& inoutWindow)
{
    Array<float64> durationsMilliseconds;
    if (durationsMilliseconds.TryEnsureCapacity(MEASURED_FRAMES) == false)
    {
        return {{QStringLiteral("error"), QStringLiteral("duration allocation failed")}};
    }
    inoutItem.setSize(inoutWindow.size());
    inoutWindow.show();
    if (QTest::qWaitForWindowExposed(&inoutWindow) == false)
    {
        return {{QStringLiteral("error"), QStringLiteral("window was not exposed")}};
    }
    for (int32 frame = 0; frame < WARMUP_FRAMES + MEASURED_FRAMES; ++frame)
    {
        QEventLoop loop;
        QTimer timeout;
        timeout.setSingleShot(true);
        bool hasPresented = false;
        QObject::connect(&inoutWindow, &QQuickWindow::frameSwapped, &loop, [&loop, &hasPresented, &inoutItem]() {
            if (inoutItem.HasUpdated() == true)
            {
                hasPresented = true;
                loop.quit();
            }
        });
        QObject::connect(&timeout, &QTimer::timeout, &loop, &QEventLoop::quit);
        QElapsedTimer timer;
        timer.start();
        inoutItem.Prepare(overlay);
        inoutWindow.requestUpdate();
        timeout.start(FRAME_TIMEOUT_MILLISECONDS);
        loop.exec();
        if (hasPresented == false)
        {
            return {{QStringLiteral("error"), QStringLiteral("frame timed out")}, {QStringLiteral("frame"), frame}};
        }
        if (frame >= WARMUP_FRAMES)
        {
            durationsMilliseconds.Add(static_cast<float64>(timer.nsecsElapsed()) / NANOSECONDS_PER_MILLISECOND);
        }
    }
    const QImage image = inoutWindow.grabWindow();
    if (image.isNull() == true)
    {
        return {{QStringLiteral("error"), QStringLiteral("window capture failed")}};
    }
    usize highlightedPixels = 0;
    for (int32 y = 0; y < image.height(); ++y)
    {
        for (int32 x = 0; x < image.width(); ++x)
        {
            const QColor color = image.pixelColor(x, y);
            if (color.red() > 200 && color.green() > 120 && color.blue() < 120)
            {
                ++highlightedPixels;
            }
        }
    }
    Sort(durationsMilliseconds.GetData(), durationsMilliseconds.GetSize(), [](float64 left, float64 right) noexcept {
        return left < right;
    });
    QJsonObject result;
    result.insert(QStringLiteral("medianFrameMilliseconds"), durationsMilliseconds[MEASURED_FRAMES / 2]);
    result.insert(QStringLiteral("maximumFrameMilliseconds"), durationsMilliseconds.GetLast());
    result.insert(QStringLiteral("highlightedPixels"), static_cast<qint64>(highlightedPixels));
    result.insert(QStringLiteral("devicePixelRatio"), inoutWindow.devicePixelRatio());
    const Array<float64>& samples = inoutItem.GetCpuSamples();
    if (samples.GetSize() < MEASURED_FRAMES)
    {
        result.insert(QStringLiteral("error"), QStringLiteral("too few CPU callbacks"));
        result.insert(QStringLiteral("cpuCallbacks"), static_cast<qint64>(samples.GetSize()));
        return result;
    }
    Array<float64> cpuMilliseconds;
    if (cpuMilliseconds.TryEnsureCapacity(MEASURED_FRAMES) == false)
    {
        return {};
    }
    for (usize index = samples.GetSize() - MEASURED_FRAMES; index < samples.GetSize(); ++index)
    {
        cpuMilliseconds.Add(samples[index]);
    }
    Sort(cpuMilliseconds.GetData(), cpuMilliseconds.GetSize(), [](float64 left, float64 right) noexcept {
        return left < right;
    });
    result.insert(QStringLiteral("medianOverlayCpuMilliseconds"), cpuMilliseconds[MEASURED_FRAMES / 2]);
    inoutWindow.hide();
    return result;
}
} // namespace

int main(int argumentCount, char** arguments)
{
    qputenv("QSG_RENDER_LOOP", QByteArrayLiteral("basic"));
    QApplication application(argumentCount, arguments);
#if defined(Q_OS_MACOS)
    QQuickWindow::setGraphicsApi(QSGRendererInterface::Metal);
#endif
    ViewportObjectOverlay overlay;
    overlay.FramePixel = {0, 0, WIDTH_PIXELS, HEIGHT_PIXELS};
    if (overlay.WireLinesPixel.TryEnsureCapacity(ViewportObjectOverlay::MAX_WIRE_LINES) == false)
    {
        return 1;
    }
    for (int32 row = 0; row < GRID_ROWS; ++row)
    {
        for (int32 column = 0; column < GRID_COLUMNS; ++column)
        {
            const QPointF firstPixel(static_cast<float64>(column) * WIDTH_PIXELS / GRID_COLUMNS,
                                     static_cast<float64>(row) * HEIGHT_PIXELS / GRID_ROWS);
            const QPointF secondPixel = firstPixel + QPointF(static_cast<float64>(WIDTH_PIXELS) / GRID_COLUMNS, 0);
            const QPointF thirdPixel = firstPixel + QPointF(0, static_cast<float64>(HEIGHT_PIXELS) / GRID_ROWS);
            overlay.WireLinesPixel.Add(QLineF(firstPixel, secondPixel));
            overlay.WireLinesPixel.Add(QLineF(secondPixel, thirdPixel));
            overlay.WireLinesPixel.Add(QLineF(thirdPixel, firstPixel));
        }
    }
    QJsonObject painted;
    {
        QQuickWindow window;
        window.setColor(Qt::black);
        window.resize(WIDTH_PIXELS, HEIGHT_PIXELS);
        PaintedOverlay* item = new PaintedOverlay(*window.contentItem());
        painted = MeasureOverlay(overlay, *item, window);
    }
    QJsonObject geometry;
    {
        QQuickWindow window;
        window.setColor(Qt::black);
        window.resize(WIDTH_PIXELS, HEIGHT_PIXELS);
        GeometryOverlay* item = new GeometryOverlay(overlay, *window.contentItem());
        geometry = MeasureOverlay(overlay, *item, window);
    }
    QJsonObject report;
    report.insert(QStringLiteral("strokes"), static_cast<qint64>(overlay.WireLinesPixel.GetSize()));
    report.insert(QStringLiteral("logicalWidth"), WIDTH_PIXELS);
    report.insert(QStringLiteral("logicalHeight"), HEIGHT_PIXELS);
    report.insert(QStringLiteral("framesPerPath"), MEASURED_FRAMES);
    report.insert(QStringLiteral("renderLoop"), QStringLiteral("basic"));
    report.insert(QStringLiteral("paintedTexture"), painted);
    report.insert(QStringLiteral("batchedGeometry"), geometry);
    QTextStream output(stdout);
    output << QJsonDocument(report).toJson();
    return painted.isEmpty() == true || geometry.isEmpty() == true ||
                   painted.contains(QStringLiteral("error")) == true ||
                   geometry.contains(QStringLiteral("error")) == true ||
                   painted.value(QStringLiteral("highlightedPixels")).toInteger() == 0 ||
                   geometry.value(QStringLiteral("highlightedPixels")).toInteger() == 0
               ? 1
               : 0;
}
