#include <catch2/catch_test_macros.hpp>

#if defined(LUDUS_EDITOR_NATIVE_PREVIEW)
#    include "internal/render_preview.h"
#    include "internal/viewport_object_interaction.h"
#    include "internal/viewport_object_overlay.h"
#    include "internal/viewport_object_overlay_item.h"
#    include "internal/world_document.h"

#    include <ludus/foundation/base/types.h>
#    include <ludus/graphics/renderer/renderer.hpp>

#    include <QColor>
#    include <QGuiApplication>
#    include <QImage>
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
#endif
