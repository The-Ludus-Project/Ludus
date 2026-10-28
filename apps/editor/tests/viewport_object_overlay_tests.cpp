#include <catch2/catch_test_macros.hpp>

#if defined(LUDUS_EDITOR_NATIVE_PREVIEW)
#    include "internal/render_preview.h"
#    include "internal/viewport_object_interaction.h"
#    include "internal/viewport_object_overlay.h"
#    include "internal/viewport_object_overlay_item.h"
#    include "internal/world_document.h"

#    include <ludus/foundation/base/types.h>

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
    REQUIRE(overlay.WireLinesPixel.TryAdd(QLineF(10, 20, 110, 20)) == true);
    REQUIRE(overlay.HandleStrokes.TryAdd({QLineF(20, 30, 20, 110), qRgb(255, 0, 0), 4}) == true);
    QSGNode* rootOrNull = UpdateViewportObjectOverlayNode(overlay, nullptr);
    REQUIRE(rootOrNull != nullptr);
    QSGClipNode* clip = static_cast<QSGClipNode*>(rootOrNull);
    CHECK(clip->isRectangular() == true);
    CHECK(clip->clipRect() == overlay.FramePixel);
    QSGGeometryNode* node = static_cast<QSGGeometryNode*>(clip->firstChild());
    REQUIRE(node != nullptr);
    CHECK(node->geometry()->drawingMode() == QSGGeometry::DrawTriangles);
    CHECK(node->geometry()->vertexCount() == 12);
    const QSGGeometry::ColoredPoint2D* verticesPixel = node->geometry()->vertexDataAsColoredPoint2D();
    CHECK(verticesPixel[0].x == 10);
    CHECK(verticesPixel[0].y == 20.75F);
    CHECK(verticesPixel[0].r == qRed(ViewportObjectOverlay::SELECTION_COLOR));
    CHECK(verticesPixel[0].g == qGreen(ViewportObjectOverlay::SELECTION_COLOR));
    CHECK(verticesPixel[0].b == qBlue(ViewportObjectOverlay::SELECTION_COLOR));
    CHECK(verticesPixel[0].a == 255);
    CHECK(verticesPixel[6].x == 18);
    CHECK(verticesPixel[6].r == 255);
    CHECK(verticesPixel[6].g == 0);
    overlay.WireLinesPixel[0].translate(5, 0);
    CHECK(UpdateViewportObjectOverlayNode(overlay, rootOrNull) == rootOrNull);
    CHECK(node->geometry()->vertexDataAsColoredPoint2D() == verticesPixel);
    CHECK(verticesPixel[0].x == 15);
    overlay.WireLinesPixel[0] = QLineF(1, 1, 1, 1);
    REQUIRE(UpdateViewportObjectOverlayNode(overlay, rootOrNull) == rootOrNull);
    CHECK(verticesPixel[0].x == 1);
    REQUIRE(overlay.WireLinesPixel.TryResize(ViewportObjectOverlay::MAX_WIRE_LINES + 1) == true);
    rootOrNull = UpdateViewportObjectOverlayNode(overlay, rootOrNull);
    CHECK(rootOrNull == nullptr);
    overlay.WireLinesPixel.Clear();
    overlay.HandleStrokes.Clear();
    REQUIRE(overlay.WireLinesPixel.TryAdd(QLineF(0, 0, 1, 1)) == true);
    rootOrNull = UpdateViewportObjectOverlayNode(overlay, rootOrNull);
    REQUIRE(rootOrNull != nullptr);
    overlay.FramePixel = {};
    CHECK(UpdateViewportObjectOverlayNode(overlay, rootOrNull) == nullptr);
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
