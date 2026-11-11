#include <catch2/catch_test_macros.hpp>

#if defined(LUDUS_EDITOR_NATIVE_PREVIEW)
#    include "internal/render_preview.h"
#    include "internal/selection_outline_request.h"
#    include "internal/viewport_gizmo_readout.h"
#    include "internal/viewport_object_interaction.h"
#    include "internal/viewport_object_overlay.h"
#    include "internal/viewport_object_overlay_item.h"
#    include "internal/viewport_overlay_layer.h"
#    include "internal/world_document.h"

#    include <ludus/foundation/base/types.h>
#    include <ludus/graphics/renderer/renderer.hpp>

#    include <QColor>
#    include <QGuiApplication>
#    include <QImage>
#    include <QPainter>
#    include <QQuickItem>
#    include <QQuickPaintedItem>
#    include <QQuickWindow>
#    include <QSGClipNode>
#    include <QSGGeometryNode>
#    include <QSignalSpy>
#    include <QTest>

using namespace ludus::editor;
using namespace ludus::foundation;

TEST_CASE("Native object overlays batch bounded strokes into reusable clipped triangle geometry",
          "[editor][object-interaction][native-overlay]")
{
    ViewportObjectOverlay overlay;
    CHECK(UpdateViewportObjectOverlayNode(overlay, nullptr) == nullptr);
    overlay.FramePixel = {10, 20, 300, 200};
    REQUIRE(overlay.OutlineLinesPixel.TryAdd(QLineF(10, 20, 110, 20)) == true);
    REQUIRE(overlay.HandleStrokes.TryAdd({QLineF(20, 30, 20, 110), qRgb(255, 0, 0), 4}) == true);
    QSGNode* rootOrNull = UpdateViewportObjectOverlayNode(overlay, nullptr);
    REQUIRE(rootOrNull != nullptr);
    QSGClipNode* clip = static_cast<QSGClipNode*>(rootOrNull);
    CHECK(clip->isRectangular() == true);
    CHECK(clip->clipRect() == overlay.FramePixel);
    QSGGeometryNode* node = static_cast<QSGGeometryNode*>(clip->firstChild());
    REQUIRE(node != nullptr);
    CHECK(node->geometry()->drawingMode() == QSGGeometry::DrawTriangles);
    // The outline segment is stroked twice, a casing under the amber, followed by the handle stroke.
    CHECK(node->geometry()->vertexCount() == 18);
    const QSGGeometry::ColoredPoint2D* verticesPixel = node->geometry()->vertexDataAsColoredPoint2D();
    // The casing is premultiplied, wider than the outline, and drawn first.
    constexpr float32 CASING_HALF_WIDTH = ViewportObjectOverlay::CASING_WIDTH_PIXELS * 0.5F;
    constexpr float32 CASING_CAP = ViewportObjectOverlay::CASING_WIDTH_PIXELS * 0.5F;
    CHECK(verticesPixel[0].x == 10 - CASING_CAP);
    CHECK(verticesPixel[0].y == 20 + CASING_HALF_WIDTH);
    CHECK(verticesPixel[0].a == ViewportObjectOverlay::CASING_ALPHA);
    CHECK(verticesPixel[0].r <= qRed(ViewportObjectOverlay::CASING_COLOR));
    // The amber outline follows with the token colour, fully opaque.
    constexpr float32 OUTLINE_HALF_WIDTH = ViewportObjectOverlay::OUTLINE_WIDTH_PIXELS * 0.5F;
    CHECK(verticesPixel[6].y == 20 + OUTLINE_HALF_WIDTH);
    CHECK(verticesPixel[6].r == qRed(ViewportObjectOverlay::SELECTION_COLOR));
    CHECK(verticesPixel[6].g == qGreen(ViewportObjectOverlay::SELECTION_COLOR));
    CHECK(verticesPixel[6].b == qBlue(ViewportObjectOverlay::SELECTION_COLOR));
    CHECK(verticesPixel[6].a == 255);
    // The handle stroke keeps its own colour and has no cap.
    CHECK(verticesPixel[12].x == 18);
    CHECK(verticesPixel[12].r == 255);
    CHECK(verticesPixel[12].g == 0);
    overlay.OutlineLinesPixel[0].translate(5, 0);
    CHECK(UpdateViewportObjectOverlayNode(overlay, rootOrNull) == rootOrNull);
    CHECK(node->geometry()->vertexDataAsColoredPoint2D() == verticesPixel);
    CHECK(verticesPixel[6].x == 15 - OUTLINE_HALF_WIDTH);
    overlay.OutlineLinesPixel[0] = QLineF(1, 1, 1, 1);
    REQUIRE(UpdateViewportObjectOverlayNode(overlay, rootOrNull) == rootOrNull);
    CHECK(verticesPixel[6].x == 1);
    REQUIRE(overlay.OutlineLinesPixel.TryResize(ViewportObjectOverlay::MAX_OUTLINE_LINES + 1) == true);
    rootOrNull = UpdateViewportObjectOverlayNode(overlay, rootOrNull);
    CHECK(rootOrNull == nullptr);
    overlay.OutlineLinesPixel.Clear();
    overlay.HandleStrokes.Clear();
    REQUIRE(overlay.OutlineLinesPixel.TryAdd(QLineF(0, 0, 1, 1)) == true);
    rootOrNull = UpdateViewportObjectOverlayNode(overlay, rootOrNull);
    REQUIRE(rootOrNull != nullptr);
    overlay.FramePixel = {};
    CHECK(UpdateViewportObjectOverlayNode(overlay, rootOrNull) == nullptr);
}

TEST_CASE("Native object overlays upload solid handle fills after the strokes",
          "[editor][object-interaction][native-overlay]")
{
    ViewportObjectOverlay overlay;
    overlay.FramePixel = {0, 0, 300, 200};
    REQUIRE(overlay.HandleStrokes.TryAdd({QLineF(20, 30, 20, 110), qRgb(255, 0, 0), 4}) == true);
    ViewportHandleFill arrow;
    arrow.CornerCount = 3;
    arrow.Color = qRgb(0, 255, 0);
    arrow.CornersPixel = {QPointF(20, 120), QPointF(14, 105), QPointF(26, 105), QPointF(20, 120)};
    ViewportHandleFill cube;
    cube.CornerCount = 4;
    cube.Color = qRgb(0, 0, 255);
    cube.CornersPixel = {QPointF(50, 50), QPointF(60, 50), QPointF(60, 60), QPointF(50, 60)};
    REQUIRE(overlay.HandleFills.TryAdd(arrow) == true);
    REQUIRE(overlay.HandleFills.TryAdd(cube) == true);
    QSGNode* rootOrNull = UpdateViewportObjectOverlayNode(overlay, nullptr);
    REQUIRE(rootOrNull != nullptr);
    QSGGeometryNode* node = static_cast<QSGGeometryNode*>(rootOrNull->firstChild());
    // One stroke and two fills, six vertices each.
    REQUIRE(node->geometry()->vertexCount() == 18);
    const QSGGeometry::ColoredPoint2D* verticesPixel = node->geometry()->vertexDataAsColoredPoint2D();
    // A triangle's fourth vertex repeats its third, so the sixth vertex adds no area.
    CHECK(verticesPixel[6].x == 20);
    CHECK(verticesPixel[6].y == 120);
    CHECK(verticesPixel[6].g == 255);
    CHECK(verticesPixel[9].x == 20);
    CHECK(verticesPixel[10].x == 26);
    CHECK(verticesPixel[11].x == 26);
    CHECK(verticesPixel[11].y == 105);
    // A quad is two triangles sharing a diagonal.
    CHECK(verticesPixel[12].x == 50);
    CHECK(verticesPixel[13].x == 60);
    CHECK(verticesPixel[14].y == 60);
    CHECK(verticesPixel[16].y == 60);
    CHECK(verticesPixel[17].x == 50);
    CHECK(verticesPixel[17].b == 255);
    REQUIRE(overlay.HandleFills.TryResize(ViewportObjectOverlay::MAX_HANDLE_FILLS + 1) == true);
    CHECK(UpdateViewportObjectOverlayNode(overlay, rootOrNull) == nullptr);
}

TEST_CASE("Gizmo readouts print signed values at a precision that suits each tool",
          "[editor][object-interaction][gizmo]")
{
    const QString degree = QString(QChar(u'\u00B0'));
    const QString times = QString(QChar(u'\u00D7'));
    CHECK(FormatMoveReadout({0, 0.25}, {}) == QStringLiteral("X +0.250 m"));
    CHECK(FormatMoveReadout({0, 0.25}, {2, -0.12}) == QStringLiteral("X +0.250  Z -0.120 m"));
    CHECK(FormatMoveReadout({1, 12.3456}, {}) == QStringLiteral("Y +12.346 m"));
    // A value that rounds to nothing carries no sign.
    CHECK(FormatMoveReadout({1, -0.0004}, {}) == QStringLiteral("Y 0.000 m"));
    CHECK(FormatMoveReadout({1, 0.0004}, {}) == QStringLiteral("Y 0.000 m"));
    CHECK(FormatRotateReadout(2, 15) == QStringLiteral("Z +15.0") + degree);
    CHECK(FormatRotateReadout(0, -7.5) == QStringLiteral("X -7.5") + degree);
    CHECK(FormatRotateReadout(1, -0.01) == QStringLiteral("Y 0.0") + degree);
    CHECK(FormatScaleReadout(GIZMO_READOUT_NO_AXIS, 1.2549) == times + QStringLiteral("1.25"));
    CHECK(FormatScaleReadout(1, 0.5) == QStringLiteral("Y ") + times + QStringLiteral("0.50"));
}

TEST_CASE("Gizmo readout labels stay beside the pointer and inside the frame", "[editor][object-interaction][gizmo]")
{
    CHECK(MeasureGizmoReadout(QString()).isEmpty() == true);
    const QSizeF size = MeasureGizmoReadout(QStringLiteral("X +0.250 m"));
    REQUIRE(size.width() > 0);
    REQUIRE(size.height() > 0);
    CHECK(MeasureGizmoReadout(QStringLiteral("X +0.250  Z -0.120 m")).width() > size.width());
    const QRectF frame{10, 20, 800, 600};
    // Below and to the right of the pointer, so the pointer does not hide the number.
    const QPointF beside = PlaceGizmoReadout({100, 100}, size, frame);
    CHECK(beside.x() > 100);
    CHECK(beside.y() > 100);
    // Against the right or bottom edge it moves to the other side of the pointer.
    const QPointF atRight = PlaceGizmoReadout({frame.right() - 2, 100}, size, frame);
    CHECK(atRight.x() + size.width() <= frame.right() - 2);
    CHECK(atRight.y() == beside.y());
    const QPointF atBottom = PlaceGizmoReadout({100, frame.bottom() - 2}, size, frame);
    CHECK(atBottom.y() + size.height() <= frame.bottom() - 2);
    // A corner gets both flips, and a frame smaller than the label keeps the label's top-left on it.
    const QPointF atCorner = PlaceGizmoReadout(frame.bottomRight(), size, frame);
    CHECK(frame.contains(QRectF(atCorner, size)) == true);
    CHECK(PlaceGizmoReadout({20, 30}, size, QRectF(10, 20, 10, 10)).x() >= 10);
}

TEST_CASE("Gizmo readout labels paint a dark plate with light text and keep out of the stroke batch",
          "[editor][object-interaction][gizmo]")
{
    const QString text = QStringLiteral("X +0.250 m");
    const QSizeF size = MeasureGizmoReadout(text);
    QImage image(static_cast<int32>(size.width()) + 8,
                 static_cast<int32>(size.height()) + 8,
                 QImage::Format_ARGB32_Premultiplied);
    image.fill(Qt::white);
    {
        QPainter painter(&image);
        PaintGizmoReadout(painter, text, {4, 4});
    }
    // The corner of the plate is dark over the white image; the text brightens some pixel inside it again.
    const QColor plate = image.pixelColor(static_cast<int32>(size.width() * 0.5), 5);
    CHECK(plate.lightness() < 100);
    bool hasLightText = false;
    for (int32 y = 6; y < static_cast<int32>(size.height()) + 2; ++y)
    {
        for (int32 x = 6; x < static_cast<int32>(size.width()) + 2; ++x)
        {
            hasLightText = hasLightText == true || image.pixelColor(x, y).lightness() > 200;
        }
    }
    CHECK(hasLightText == true);

    // The label is painted text, not triangles: it adds nothing to the uploaded geometry.
    ViewportObjectOverlay overlay;
    overlay.FramePixel = {0, 0, 300, 200};
    REQUIRE(overlay.HandleStrokes.TryAdd({QLineF(20, 30, 20, 110), qRgb(255, 0, 0), 4}) == true);
    QSGNode* rootOrNull = UpdateViewportObjectOverlayNode(overlay, nullptr);
    REQUIRE(rootOrNull != nullptr);
    const int32 vertexCount = static_cast<QSGGeometryNode*>(rootOrNull->firstChild())->geometry()->vertexCount();
    overlay.ReadoutText = text;
    overlay.ReadoutAnchorPixel = {50, 50};
    rootOrNull = UpdateViewportObjectOverlayNode(overlay, rootOrNull);
    REQUIRE(rootOrNull != nullptr);
    CHECK(static_cast<QSGGeometryNode*>(rootOrNull->firstChild())->geometry()->vertexCount() == vertexCount);
    delete rootOrNull;
}

TEST_CASE("Selection outlines and picking follow the presented image camera, not the live one",
          "[editor][object-interaction]")
{
    RenderScene scene;
    scene.Identity = QByteArrayLiteral("presented-view-object-overlay-fixture");
    PreviewMesh mesh;
    mesh.Positions = {{-1, -1, 0}, {1, -1, 0}, {0, 1, 0}};
    mesh.Indices = {0, 1, 2};
    scene.Meshes.append(mesh);
    PreviewObject object;
    object.Id = QStringLiteral("selected");
    object.Transform.Translation = {0.5F, 0, 0};
    scene.Objects.append(object);
    RenderPreview preview;
    REQUIRE(preview.SetScene(scene) == true);
    WorldDocument document;
    ViewportObjectInteraction interaction(preview, nullptr);
    interaction.SetWorld(document, object.Id, false);
    const QRectF framePixel{0, 0, 800, 600};
    ViewportObjectOverlay shown;
    interaction.BuildOverlay(framePixel, shown);
    REQUIRE(shown.OutlineLinesPixel.GetSize() == 3);
    // The three open edges of the triangle share each corner twice.
    constexpr float64 ENDPOINTS_PER_CORNER = 2;
    const QPointF centroidPixel =
        (shown.OutlineLinesPixel[0].p1() + shown.OutlineLinesPixel[0].p2() + shown.OutlineLinesPixel[1].p1() +
         shown.OutlineLinesPixel[1].p2() + shown.OutlineLinesPixel[2].p1() + shown.OutlineLinesPixel[2].p2()) /
        (3.0 * ENDPOINTS_PER_CORNER);
    REQUIRE(interaction.Pick(centroidPixel, framePixel) == object.Id);

    // An asynchronously rendered image still shows the camera it was rendered with.
    ludus::graphics::renderer::ViewDescription view;
    QString error;
    REQUIRE(preview.DescribeConventionalView(static_cast<uint32>(framePixel.width()),
                                             static_cast<uint32>(framePixel.height()),
                                             view,
                                             error) == true);
    QSignalSpy changes(&interaction, &ViewportObjectInteraction::Changed);
    interaction.SetPresentedView(view.WorldToClip);
    CHECK(changes.count() == 1);
    interaction.SetPresentedView(view.WorldToClip);
    CHECK(changes.count() == 1);
    preview.Orbit(40, 15);
    ViewportObjectOverlay presented;
    interaction.BuildOverlay(framePixel, presented);
    REQUIRE(presented.OutlineLinesPixel.GetSize() == shown.OutlineLinesPixel.GetSize());
    constexpr float64 TOLERANCE_PIXELS = 0.01;
    for (usize line = 0; line < shown.OutlineLinesPixel.GetSize(); ++line)
    {
        const QPointF differencePixel = presented.OutlineLinesPixel[line].p1() - shown.OutlineLinesPixel[line].p1();
        CHECK(differencePixel.manhattanLength() < TOLERANCE_PIXELS);
    }
    CHECK(interaction.Pick(centroidPixel, framePixel) == object.Id);

    // Without a presented image the outline follows navigation again.
    interaction.ClearPresentedView();
    ViewportObjectOverlay live;
    interaction.BuildOverlay(framePixel, live);
    REQUIRE(live.OutlineLinesPixel.GetSize() == shown.OutlineLinesPixel.GetSize());
    const QPointF movedPixel = live.OutlineLinesPixel[0].p1() - shown.OutlineLinesPixel[0].p1();
    CHECK(movedPixel.manhattanLength() > 1);
}

TEST_CASE("The GPU outline replaces the CPU silhouette and reports the selected mesh, drag placement and stroke scale",
          "[editor][object-interaction][gpu-outline]")
{
    RenderScene scene;
    scene.Identity = QByteArrayLiteral("gpu-outline-fixture");
    PreviewMesh mesh;
    mesh.Positions = {{-1, -1, 0}, {1, -1, 0}, {0, 1, 0}};
    mesh.Indices = {0, 1, 2};
    scene.Meshes.append(mesh);
    PreviewObject object;
    object.Id = QStringLiteral("selected");
    object.Transform.Translation = {0.5F, 0, 0};
    scene.Objects.append(object);
    RenderPreview preview;
    REQUIRE(preview.SetScene(scene) == true);
    WorldDocument document;
    ViewportObjectInteraction interaction(preview, nullptr);
    interaction.SetWorld(document, object.Id, false);
    interaction.SetTool(ViewportObjectInteraction::Tool::Select);
    const QRectF framePixel{0, 0, 800, 600};
    ViewportObjectOverlay cpu;
    interaction.BuildOverlay(framePixel, cpu);
    REQUIRE(cpu.OutlineLinesPixel.GetSize() == 3);
    SelectionOutlineRequest request;
    CHECK(interaction.TryGetGpuOutlineRequest(request) == false);

    // The image is rendered at twice the logical size of the viewport.
    interaction.SetImageSize({1600, 1200});
    QSignalSpy reports(&interaction, &ViewportObjectInteraction::GpuOutlineChanged);
    interaction.SetGpuOutlineEnabled(true);
    ViewportObjectOverlay gpu;
    interaction.BuildOverlay(framePixel, gpu);
    CHECK(gpu.OutlineLinesPixel.IsEmpty() == true);
    REQUIRE(reports.count() == 1);
    REQUIRE(interaction.TryGetGpuOutlineRequest(request) == true);
    CHECK(request.Mesh == 0);
    CHECK(request.SceneIdentity == scene.Identity);
    CHECK(request.PositionsMesh.GetSize() == 3);
    CHECK(request.Indices.GetSize() == 3);
    CHECK(request.MeshToWorld == object.Transform);
    CHECK(request.StrokeScale == 2.0F);
    CHECK(request.OutlineWidth == ViewportObjectOverlay::OUTLINE_WIDTH_PIXELS);
    CHECK(request.CasingWidth == ViewportObjectOverlay::CASING_WIDTH_PIXELS);
    // An unchanged request is reported once, even when the camera moves: the GPU projects it.
    interaction.BuildOverlay(framePixel, gpu);
    preview.Orbit(40, 15);
    interaction.BuildOverlay(framePixel, gpu);
    CHECK(reports.count() == 1);

    // Back on the CPU, the silhouette returns and nothing more is reported.
    interaction.SetGpuOutlineEnabled(false);
    interaction.BuildOverlay(framePixel, gpu);
    CHECK(gpu.OutlineLinesPixel.GetSize() == 3);
    CHECK(reports.count() == 1);
    CHECK(interaction.TryGetGpuOutlineRequest(request) == false);
}

TEST_CASE("Native selection highlighting renders without a full-viewport painted texture",
          "[editor][object-interaction][native-overlay][native-pixels]")
{
    if (QGuiApplication::platformName() != QStringLiteral("cocoa"))
    {
        SKIP("Requires the Cocoa scene graph for native GPU pixel validation");
    }
    RenderScene scene;
    scene.Identity = QByteArrayLiteral("native-object-overlay-fixture");
    PreviewMesh mesh;
    mesh.Positions = {{-1, -1, 0}, {1, -1, 0}, {0, 1, 0}};
    mesh.Indices = {0, 1, 2};
    scene.Meshes.append(mesh);
    PreviewObject object;
    object.Id = QStringLiteral("selected");
    scene.Objects.append(object);
    RenderPreview preview;
    REQUIRE(preview.SetScene(scene) == true);
    WorldDocument document;
    ViewportObjectInteraction interaction(preview, nullptr);
    interaction.SetWorld(document, object.Id, false);
    QQuickWindow window;
    window.setColor(Qt::black);
    window.resize(800, 600);
    QQuickItem* item = CreateViewportObjectOverlayItem(interaction, *window.contentItem());
    CHECK(qobject_cast<QQuickPaintedItem*>(item) == nullptr);
    item->setSize(window.size());
    QSignalSpy frames(&window, &QQuickWindow::frameSwapped);
    window.show();
    REQUIRE(QTest::qWaitForWindowExposed(&window) == true);
    QTRY_VERIFY(frames.count() > 0);
    const QImage image = window.grabWindow();
    REQUIRE(image.isNull() == false);
    bool hasHighlight = false;
    for (int32 y = 0; y < image.height(); ++y)
    {
        for (int32 x = 0; x < image.width(); ++x)
        {
            const QColor color = image.pixelColor(x, y);
            if (color.red() > 200 && color.green() > 120 && color.blue() < 120)
            {
                hasHighlight = true;
            }
        }
    }
    CHECK(hasHighlight == true);
    interaction.SetActive(false);
    const int32 before = static_cast<int32>(frames.count());
    QTRY_VERIFY(frames.count() > before);
    const QImage cleared = window.grabWindow();
    REQUIRE(cleared.isNull() == false);
    bool hasRemainingHighlight = false;
    for (int32 y = 0; y < cleared.height(); ++y)
    {
        for (int32 x = 0; x < cleared.width(); ++x)
        {
            const QColor color = cleared.pixelColor(x, y);
            if (color.red() > 200 && color.green() > 120 && color.blue() < 120)
            {
                hasRemainingHighlight = true;
            }
        }
    }
    CHECK(hasRemainingHighlight == false);
}
namespace
{
/// Straight-alpha RGBA8 pixel of a layer's packed pixels at a physical position; transparent outside its painted
/// region.
QColor ReadLayerPixel(const ViewportOverlayLayer& layer, QPoint positionPhysical)
{
    const QRect region = layer.GetPaintedRegion();
    constexpr usize RGBA_BYTES = 4;
    const usize offset = (static_cast<usize>(positionPhysical.y() - region.y()) * static_cast<usize>(region.width()) +
                          static_cast<usize>(positionPhysical.x() - region.x())) *
                         RGBA_BYTES;
    if (region.contains(positionPhysical) == false || offset + RGBA_BYTES > layer.GetPixelBytes())
    {
        return QColor(0, 0, 0, 0);
    }
    const uint8* pixel = layer.GetPixels() + offset;
    return QColor(pixel[0], pixel[1], pixel[2], pixel[3]);
}
} // namespace

TEST_CASE("Object overlay bounds cover every stroke, fill and readout and stay inside the frame",
          "[editor][object-interaction][browser-overlay]")
{
    ViewportObjectOverlay overlay;
    overlay.FramePixel = {0, 0, 400, 300};
    CHECK(ComputeViewportObjectOverlayBounds(overlay).isEmpty() == true);
    REQUIRE(overlay.OutlineLinesPixel.TryAdd(QLineF(100, 100, 200, 100)) == true);
    const QRectF outline = ComputeViewportObjectOverlayBounds(overlay);
    // The casing is the widest stroke; its half width and caps lie inside the bounds.
    constexpr float64 CASING_HALF_WIDTH = static_cast<float64>(ViewportObjectOverlay::CASING_WIDTH_PIXELS) * 0.5;
    CHECK(outline.contains(QRectF(QPointF(100 - CASING_HALF_WIDTH, 100 - CASING_HALF_WIDTH),
                                  QPointF(200 + CASING_HALF_WIDTH, 100 + CASING_HALF_WIDTH))) == true);
    CHECK(outline.width() < 120);
    ViewportHandleFill fill;
    fill.CornersPixel = {QPointF(300, 200), QPointF(320, 200), QPointF(310, 220), QPointF(0, 0)};
    fill.CornerCount = ViewportHandleFill::TRIANGLE_CORNERS;
    fill.Color = qRgb(0, 255, 0);
    REQUIRE(overlay.HandleFills.TryAdd(fill) == true);
    const QRectF withFill = ComputeViewportObjectOverlayBounds(overlay);
    CHECK(withFill.contains(QRectF(300, 200, 20, 20)) == true);
    // The unused fourth corner at the origin is not part of the triangle.
    CHECK(withFill.left() > 50);
    overlay.ReadoutText = QStringLiteral("X +0.250 m");
    overlay.ReadoutAnchorPixel = {150, 150};
    const QSizeF labelSize = MeasureGizmoReadout(overlay.ReadoutText);
    const QRectF label(PlaceGizmoReadout(overlay.ReadoutAnchorPixel, labelSize, overlay.FramePixel), labelSize);
    CHECK(ComputeViewportObjectOverlayBounds(overlay).contains(label) == true);
    // Strokes beyond the frame are clipped by the painter, so the bounds stop at the frame.
    REQUIRE(overlay.HandleStrokes.TryAdd({QLineF(-50, 10, 600, 10), qRgb(255, 0, 0), 4}) == true);
    CHECK(overlay.FramePixel.contains(ComputeViewportObjectOverlayBounds(overlay)) == true);
}

TEST_CASE("The browser overlay layer repaints only the covered region and clears the previous one",
          "[editor][object-interaction][browser-overlay]")
{
    ViewportOverlayLayer layer;
    ViewportObjectOverlay overlay;
    overlay.FramePixel = {0, 0, 400, 300};
    const QSize layerSize(400, 300);
    // Nothing drawn before or now leaves the canvas alone.
    CHECK(layer.Update(overlay, layerSize, 1) == false);
    CHECK(layer.GetPaintedRegion().isEmpty() == true);

    REQUIRE(overlay.HandleStrokes.TryAdd({QLineF(100, 50, 100, 150), qRgb(255, 0, 0), 4}) == true);
    REQUIRE(layer.Update(overlay, layerSize, 1) == true);
    const QRect first = layer.GetPaintedRegion();
    CHECK(layer.GetClearedRegion().isEmpty() == true);
    CHECK(first.contains(QRect(98, 50, 4, 100)) == true);
    // Only the stroke's neighbourhood is rasterized and copied, not the viewport.
    CHECK(first.width() < 20);
    CHECK(layer.GetPixelBytes() == static_cast<usize>(first.width() * first.height()) * 4);
    // Straight alpha: the stroke is opaque red; away from it the layer is transparent.
    const QColor stroke = ReadLayerPixel(layer, {100, 100});
    CHECK(stroke.red() == 255);
    CHECK(stroke.green() == 0);
    CHECK(stroke.alpha() == 255);
    CHECK(ReadLayerPixel(layer, first.topLeft()).alpha() == 0);

    // Moving the stroke clears where it was and paints where it is.
    overlay.HandleStrokes[0].LinePixel.translate(200, 0);
    REQUIRE(layer.Update(overlay, layerSize, 1) == true);
    CHECK(layer.GetClearedRegion() == first);
    CHECK(layer.GetPaintedRegion().intersects(first) == false);

    // Removing it only clears.
    const QRect second = layer.GetPaintedRegion();
    overlay.HandleStrokes.Clear();
    REQUIRE(layer.Update(overlay, layerSize, 1) == true);
    CHECK(layer.GetClearedRegion() == second);
    CHECK(layer.GetPaintedRegion().isEmpty() == true);
    CHECK(layer.Update(overlay, layerSize, 1) == false);

    // A resized canvas starts blank, so nothing is left to clear.
    REQUIRE(overlay.HandleStrokes.TryAdd({QLineF(100, 50, 100, 150), qRgb(255, 0, 0), 4}) == true);
    REQUIRE(layer.Update(overlay, layerSize, 1) == true);
    layer.Invalidate();
    REQUIRE(layer.Update(overlay, layerSize, 1) == true);
    CHECK(layer.GetClearedRegion().isEmpty() == true);
}

TEST_CASE("The browser overlay layer paints at the screen's pixel ratio inside the layer",
          "[editor][object-interaction][browser-overlay]")
{
    ViewportOverlayLayer layer;
    ViewportObjectOverlay overlay;
    overlay.FramePixel = {0, 0, 400, 300};
    REQUIRE(overlay.HandleStrokes.TryAdd({QLineF(100, 50, 100, 150), qRgb(255, 0, 0), 4}) == true);
    REQUIRE(layer.Update(overlay, {400, 300}, 1) == true);
    const QRect logical = layer.GetPaintedRegion();
    constexpr float64 HIGH_DENSITY = 2;
    REQUIRE(layer.Update(overlay, {800, 600}, HIGH_DENSITY) == true);
    const QRect physical = layer.GetPaintedRegion();
    CHECK(physical.width() >= logical.width() * 2 - 1);
    CHECK(physical.contains(QRect(196, 100, 8, 200)) == true);
    // The stroke is four logical pixels, eight physical ones, wide.
    CHECK(ReadLayerPixel(layer, {197, 200}).alpha() == 255);
    CHECK(ReadLayerPixel(layer, {202, 200}).alpha() == 255);
    // A region past the layer's edge is cut to the layer.
    REQUIRE(layer.Update(overlay, {220, 120}, HIGH_DENSITY) == true);
    CHECK(layer.GetPaintedRegion().isEmpty() == false);
    CHECK(QRect(0, 0, 220, 120).contains(layer.GetPaintedRegion()) == true);
}

TEST_CASE("The browser overlay layer shows the pixels the readback canvas paints",
          "[editor][object-interaction][browser-overlay]")
{
    ViewportObjectOverlay overlay;
    overlay.FramePixel = {0, 0, 320, 240};
    REQUIRE(overlay.OutlineLinesPixel.TryAdd(QLineF(40, 40, 200, 60)) == true);
    REQUIRE(overlay.OutlineLinesPixel.TryAdd(QLineF(200, 60, 120, 180)) == true);
    REQUIRE(overlay.HandleStrokes.TryAdd({QLineF(150, 120, 250, 120), qRgb(230, 70, 60), 3}) == true);
    ViewportHandleFill fill;
    fill.CornersPixel = {QPointF(250, 112), QPointF(266, 120), QPointF(250, 128), QPointF(0, 0)};
    fill.Color = qRgb(230, 70, 60);
    REQUIRE(overlay.HandleFills.TryAdd(fill) == true);
    overlay.ReadoutText = QStringLiteral("X +0.250 m");
    overlay.ReadoutAnchorPixel = {150, 120};

    // The readback canvas paints the whole frame transparent, then the overlay.
    QImage reference(320, 240, QImage::Format_ARGB32_Premultiplied);
    reference.fill(Qt::transparent);
    {
        QPainter painter(&reference);
        PaintViewportObjectOverlay(painter, overlay);
    }
    ViewportOverlayLayer layer;
    REQUIRE(layer.Update(overlay, reference.size(), 1) == true);
    const QRect region = layer.GetPaintedRegion();
    // Every pixel the full-frame painting changed lies in the layer's region and matches it, within the rounding of
    // converting premultiplied to straight alpha.
    constexpr int32 ROUNDING = 2;
    int32 paintedPixels = 0;
    for (int32 y = 0; y < reference.height(); ++y)
    {
        for (int32 x = 0; x < reference.width(); ++x)
        {
            const QRgb expected = qUnpremultiply(reference.pixel(x, y));
            if (qAlpha(expected) == 0)
            {
                continue;
            }
            ++paintedPixels;
            REQUIRE(region.contains(x, y) == true);
            const QColor actual = ReadLayerPixel(layer, {x, y});
            CHECK(qAbs(actual.alpha() - qAlpha(expected)) <= ROUNDING);
            if (qAlpha(expected) > ROUNDING * ROUNDING * ROUNDING)
            {
                CHECK(qAbs(actual.red() - qRed(expected)) <= ROUNDING);
                CHECK(qAbs(actual.green() - qGreen(expected)) <= ROUNDING);
                CHECK(qAbs(actual.blue() - qBlue(expected)) <= ROUNDING);
            }
        }
    }
    CHECK(paintedPixels > 0);
}

TEST_CASE("The browser overlay layer draws a selected object's outline from its interaction",
          "[editor][object-interaction][browser-overlay]")
{
    RenderScene scene;
    scene.Identity = QByteArrayLiteral("browser-overlay-layer-fixture");
    PreviewMesh mesh;
    mesh.Positions = {{-1, -1, 0}, {1, -1, 0}, {0, 1, 0}};
    mesh.Indices = {0, 1, 2};
    scene.Meshes.append(mesh);
    PreviewObject object;
    object.Id = QStringLiteral("selected");
    scene.Objects.append(object);
    RenderPreview preview;
    REQUIRE(preview.SetScene(scene) == true);
    WorldDocument document;
    ViewportObjectInteraction interaction(preview, nullptr);
    interaction.SetWorld(document, object.Id, false);
    // The browser builds the overlay in the proxy widget's logical frame, as its pointer input does.
    const QRectF framePixel{0, 0, 640, 480};
    ViewportObjectOverlay overlay;
    interaction.BuildOverlay(framePixel, overlay);
    REQUIRE(overlay.OutlineLinesPixel.GetSize() == 3);
    ViewportOverlayLayer layer;
    REQUIRE(layer.Update(overlay, {640, 480}, 1) == true);
    for (const QLineF& linePixel : overlay.OutlineLinesPixel)
    {
        // The pixel under the middle of each edge is covered by the outline or its casing.
        const QPointF middle = linePixel.center();
        const QPoint pixel(static_cast<int32>(middle.x()), static_cast<int32>(middle.y()));
        CHECK(ReadLayerPixel(layer, pixel).alpha() > 0);
    }
    // The amber stroke itself is opaque somewhere along the outline.
    const QRect region = layer.GetPaintedRegion();
    bool hasAmber = false;
    for (int32 y = region.top(); y <= region.bottom() && hasAmber == false; ++y)
    {
        for (int32 x = region.left(); x <= region.right() && hasAmber == false; ++x)
        {
            hasAmber =
                ReadLayerPixel(layer, {x, y}).rgba() == QColor::fromRgb(ViewportObjectOverlay::SELECTION_COLOR).rgba();
        }
    }
    CHECK(hasAmber == true);
}
#endif
