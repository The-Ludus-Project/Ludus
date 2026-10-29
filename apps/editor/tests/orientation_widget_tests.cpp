#include <catch2/catch_test_macros.hpp>

#include "design_tokens.h"
#include "internal/editor_camera_controller.h"
#include "internal/orientation_widget.h"

#include <ludus/foundation/base/types.h>

#include <QImage>
#include <QPainter>

using namespace ludus::editor;
using namespace ludus::foundation;

namespace
{
constexpr qreal WIDGET_TEST_VIEWPORT_WIDTH = 640;
/// A point well away from every handle inside the widget square.
constexpr qreal EMPTY_CORNER_OFFSET = 3;
constexpr int PAINT_IMAGE_SIZE = 100;
/// Top and Bottom keep a fraction of a degree of pitch, so their depth axis lands a hair off centre.
constexpr qreal CENTRE_TOLERANCE_PIXEL = 1;
} // namespace

TEST_CASE("The orientation widget sits in the top-right corner of the viewport", "[editor][orientation-widget]")
{
    const QRectF rect = GetOrientationWidgetRect(WIDGET_TEST_VIEWPORT_WIDTH);
    CHECK(rect.right() == WIDGET_TEST_VIEWPORT_WIDTH - ORIENTATION_WIDGET_MARGIN_PIXEL);
    CHECK(rect.top() == ORIENTATION_WIDGET_MARGIN_PIXEL);
    CHECK(rect.width() == ORIENTATION_WIDGET_SIZE_PIXEL);
}

TEST_CASE("Clicking the handle facing the camera in each axis view selects that same view",
          "[editor][orientation-widget]")
{
    EditorCameraController controller;
    const QRectF rect = GetOrientationWidgetRect(WIDGET_TEST_VIEWPORT_WIDTH);
    for (const EditorAxisView view : {EditorAxisView::Top,
                                      EditorAxisView::Bottom,
                                      EditorAxisView::Front,
                                      EditorAxisView::Back,
                                      EditorAxisView::Right,
                                      EditorAxisView::Left})
    {
        INFO(GetEditorAxisViewLabel(view));
        REQUIRE(controller.TrySetAxisView(view, nullptr) == true);
        const OrientationLayout layout = BuildOrientationLayout(controller.GetSample(), rect);
        // The last handle is nearest, and an axis view looks straight down it, onto the widget's centre.
        const OrientationHandle& nearest = layout.Handles[ORIENTATION_HANDLE_COUNT - 1];
        CHECK(nearest.View == view);
        CHECK(QLineF(nearest.CentrePixel, rect.center()).length() < CENTRE_TOLERANCE_PIXEL);
        EditorAxisView hit = EditorAxisView::Top;
        REQUIRE(TryHitOrientationHandle(layout, rect.center(), hit) == true);
        CHECK(hit == view);
    }
}

TEST_CASE("Each handle selects the view that looks at the scene from that end of its axis",
          "[editor][orientation-widget]")
{
    EditorCameraController controller;
    REQUIRE(controller.TrySetAxisView(EditorAxisView::Front, nullptr) == true);
    const QRectF rect = GetOrientationWidgetRect(WIDGET_TEST_VIEWPORT_WIDTH);
    const OrientationLayout layout = BuildOrientationLayout(controller.GetSample(), rect);
    struct Expectation final
    {
        EditorAxisView Source;
        EditorAxisView Target;
        bool IsPositive;
        char Letter;
        qreal DirectionRight;
        qreal DirectionDown;
    };
    // Front view: +X to the right, +Y up. The depth axis Z collapses onto the centre.
    for (const Expectation& expectation : {Expectation{EditorAxisView::Right, EditorAxisView::Right, true, 'X', 1, 0},
                                           Expectation{EditorAxisView::Left, EditorAxisView::Left, false, 0, -1, 0},
                                           Expectation{EditorAxisView::Top, EditorAxisView::Top, true, 'Y', 0, -1},
                                           Expectation{EditorAxisView::Bottom, EditorAxisView::Bottom, false, 0, 0, 1}})
    {
        INFO(GetEditorAxisViewLabel(expectation.Source));
        const OrientationHandle* foundOrNull = nullptr;
        for (const OrientationHandle& handle : layout.Handles)
        {
            if (handle.View == expectation.Source)
            {
                foundOrNull = &handle;
            }
        }
        REQUIRE(foundOrNull != nullptr);
        CHECK(foundOrNull->IsPositive == expectation.IsPositive);
        CHECK(foundOrNull->Letter == expectation.Letter);
        CHECK((foundOrNull->CentrePixel.x() - rect.center().x()) * expectation.DirectionRight > 0.0 - 0.01);
        CHECK((foundOrNull->CentrePixel.y() - rect.center().y()) * expectation.DirectionDown > 0.0 - 0.01);
        EditorAxisView hit = EditorAxisView::Front;
        REQUIRE(TryHitOrientationHandle(layout, foundOrNull->CentrePixel, hit) == true);
        CHECK(hit == expectation.Target);
    }
}

TEST_CASE("Clicks outside every handle fall through to the viewport and leave the output untouched",
          "[editor][orientation-widget]")
{
    EditorCameraController controller;
    REQUIRE(controller.TrySetAxisView(EditorAxisView::Front, nullptr) == true);
    const QRectF rect = GetOrientationWidgetRect(WIDGET_TEST_VIEWPORT_WIDTH);
    const OrientationLayout layout = BuildOrientationLayout(controller.GetSample(), rect);
    EditorAxisView hit = EditorAxisView::Left;
    CHECK(TryHitOrientationHandle(layout, rect.topLeft() + QPointF(EMPTY_CORNER_OFFSET, EMPTY_CORNER_OFFSET), hit) ==
          false);
    CHECK(hit == EditorAxisView::Left);
    CHECK(TryHitOrientationHandle(layout, QPointF(0, 0), hit) == false);
}

TEST_CASE("Handles paint back to front with a letter on every positive end", "[editor][orientation-widget]")
{
    EditorCameraController controller;
    REQUIRE(controller.TrySetOrbit({0, 0, 0}, {35, 25}, 4) == true);
    const QRectF rect(0, 0, ORIENTATION_WIDGET_SIZE_PIXEL, ORIENTATION_WIDGET_SIZE_PIXEL);
    const OrientationLayout layout = BuildOrientationLayout(controller.GetSample(), rect);
    usize letterCount = 0;
    for (usize index = 0; index < ORIENTATION_HANDLE_COUNT; ++index)
    {
        if (index > 0)
        {
            CHECK(layout.Handles[index - 1].Depth >= layout.Handles[index].Depth);
        }
        letterCount += layout.Handles[index].Letter != 0 ? 1 : 0;
        CHECK(rect.contains(layout.Handles[index].CentrePixel) == true);
    }
    CHECK(letterCount == 3);

    QImage image(PAINT_IMAGE_SIZE, PAINT_IMAGE_SIZE, QImage::Format_ARGB32_Premultiplied);
    image.fill(Qt::transparent);
    QPainter painter(&image);
    PaintOrientationWidget(painter, layout, controller.GetSample());
    painter.end();
    const OrientationHandle& nearest = layout.Handles[ORIENTATION_HANDLE_COUNT - 1];
    const QRgb pixel = image.pixel(nearest.CentrePixel.toPoint());
    CHECK(qAlpha(pixel) > 0);
}
