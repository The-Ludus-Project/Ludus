#include <ludus/foundation/base/types.h>

#include <ludus/foundation/containers/static_array.hpp>
#include <ludus/graphics/renderer/overlays.hpp>
#include <ludus/graphics/rhi/graph.h>
#include <ludus/graphics/ui_render/ui_render.hpp>
#include <ludus/ui/context.h>

#include <catch2/catch_test_macros.hpp>

#include <limits>

namespace
{
using namespace ludus::foundation;
namespace ui = ludus::ui;
namespace renderer = ludus::graphics::renderer;
namespace rhi = ludus::graphics::rhi;
namespace ui_render = ludus::graphics::ui_render;
using Status = rhi::RasterStatus;

constexpr float64 PIXEL_CENTER = 0.5;
/// Clip-space vertices are float32 NDC; their pixel reconstruction is exact to well below a pixel.
constexpr float32 PIXEL_TOLERANCE = 1e-3F;
constexpr usize QUAD_VERTEX_COUNT = 6;
constexpr ui::Color RED{1, 0, 0, 1};
constexpr ui::Color GREEN{0, 1, 0, 1};
constexpr ui::Color BLUE{0, 0, 1, 1};
constexpr ui::Color GRAY{0.25F, 0.25F, 0.25F, 1};

[[nodiscard]] bool IsNear(float32 value, float32 expected) noexcept
{
    return value > expected - PIXEL_TOLERANCE && value < expected + PIXEL_TOLERANCE;
}

[[nodiscard]] bool IsInside(float32 value, float32 start, float32 extent) noexcept
{
    const float32 end = start + extent;
    return value >= start && value < end;
}

/// Check the snapping contract on the X axis against the routing predicate at every pixel centre.
[[nodiscard]] bool DoesAxisMatchRouting(const ui::Rect& rectangle, const ui_render::SurfaceMapping& mapping) noexcept
{
    const float32 start = rectangle.X;
    const float32 extent = rectangle.Width;
    const uint32 pixels = mapping.Width;
    rhi::RasterRectangle snapped;
    if (ui_render::TryComputePhysicalRectangle(rectangle, mapping, snapped) == false)
    {
        return false;
    }
    for (uint32 pixel = 0; pixel < pixels; ++pixel)
    {
        const float64 centre = static_cast<float64>(pixel) + PIXEL_CENTER;
        const ui_render::LogicalPoint point = ui_render::MapPhysicalToLogical(mapping, centre, centre);
        const bool isRouted = IsInside(point.X, start, extent);
        const bool isPainted = pixel >= snapped.X && pixel < snapped.X + snapped.Width;
        if (isRouted != isPainted)
        {
            return false;
        }
    }
    return true;
}

// The issue #1219 clip/hit document: button A overflows its clipped panel, sibling button B is
// painted after A and overlaps it, and button C is anchored to the bottom-right so a resize moves it.
constexpr ui::WidgetId PANEL = 1;
constexpr ui::WidgetId OVERFLOW_BUTTON = 2;
constexpr ui::WidgetId SIBLING_BUTTON = 3;
constexpr ui::WidgetId CORNER_BUTTON = 4;
constexpr usize ELEMENT_COUNT = 4;
using Document = core::StaticArray<ui::Element, ELEMENT_COUNT>;
constexpr float32 PANEL_OFFSET = 4;
constexpr float32 PANEL_WIDTH = 30.25F; // Right edge on a pixel centre at scale 2: half-open edge probe.
constexpr float32 PANEL_HEIGHT = 20;
constexpr float32 OVERFLOW_OFFSET_X = 16;
constexpr float32 OVERFLOW_OFFSET_Y = 4;
constexpr float32 OVERFLOW_WIDTH = 20;
constexpr float32 OVERFLOW_HEIGHT = 10;
constexpr float32 SIBLING_OFFSET_X = 24;
constexpr float32 SIBLING_OFFSET_Y = 12;
constexpr float32 SIBLING_WIDTH = 8;
constexpr float32 SIBLING_HEIGHT = 16;
constexpr float32 CORNER_EXTENT = 6;

[[nodiscard]] Document MakeDocument() noexcept
{
    Document document;
    ui::Element& panel = document[0];
    panel.Id = PANEL;
    panel.Placement.Width = {PANEL_WIDTH, ui::Unit::Pixels};
    panel.Placement.Height = {PANEL_HEIGHT, ui::Unit::Pixels};
    panel.Placement.OffsetX = PANEL_OFFSET;
    panel.Placement.OffsetY = PANEL_OFFSET;
    panel.Fill = GRAY;
    ui::Element& overflow = document[1];
    overflow.Id = OVERFLOW_BUTTON;
    overflow.Parent = 0;
    overflow.SemanticRole = ui::Role::Button;
    overflow.Placement.Width = {OVERFLOW_WIDTH, ui::Unit::Pixels};
    overflow.Placement.Height = {OVERFLOW_HEIGHT, ui::Unit::Pixels};
    overflow.Placement.OffsetX = OVERFLOW_OFFSET_X;
    overflow.Placement.OffsetY = OVERFLOW_OFFSET_Y;
    overflow.Fill = RED;
    ui::Element& sibling = document[2];
    sibling.Id = SIBLING_BUTTON;
    sibling.SemanticRole = ui::Role::Button;
    sibling.Placement.Width = {SIBLING_WIDTH, ui::Unit::Pixels};
    sibling.Placement.Height = {SIBLING_HEIGHT, ui::Unit::Pixels};
    sibling.Placement.OffsetX = SIBLING_OFFSET_X;
    sibling.Placement.OffsetY = SIBLING_OFFSET_Y;
    sibling.Fill = BLUE;
    ui::Element& corner = document[3];
    corner.Id = CORNER_BUTTON;
    corner.SemanticRole = ui::Role::Button;
    corner.Placement.Width = {CORNER_EXTENT, ui::Unit::Pixels};
    corner.Placement.Height = {CORNER_EXTENT, ui::Unit::Pixels};
    corner.Placement.Horizontal = ui::Align::End;
    corner.Placement.Vertical = ui::Align::End;
    corner.Fill = GREEN;
    return document;
}

[[nodiscard]] bool IsButton(ui::WidgetId id) noexcept
{
    return id == OVERFLOW_BUTTON || id == SIBLING_BUTTON || id == CORNER_BUTTON;
}

/// Route a complete press at one physical pixel centre; return the activated widget.
[[nodiscard]] ui::WidgetId
Press(ui::Context& inoutContext, const ui_render::SurfaceMapping& mapping, uint32 x, uint32 y) noexcept
{
    const ui_render::LogicalPoint point = ui_render::MapPhysicalToLogical(mapping,
                                                                          static_cast<float64>(x) + PIXEL_CENTER,
                                                                          static_cast<float64>(y) + PIXEL_CENTER);
    static_cast<void>(inoutContext.Route({ui::EventKind::PointerDown, point.X, point.Y, 0}));
    return inoutContext.Route({ui::EventKind::PointerUp, point.X, point.Y, 0}).Activated;
}

/// Every pixel's topmost snapped button must be the button Ui activates there.
void CheckRoutingMatchesPaint(float32 scale, uint32 width, uint32 height)
{
    const ui_render::SurfaceMapping mapping{scale, 0, 0, width, height};
    ui::Context context;
    REQUIRE(context.TryInitialize(ELEMENT_COUNT) == ui::Status::Ok);
    const Document document = MakeDocument();
    const ui::Viewport viewport{{0, 0, static_cast<float32>(width) / scale, static_cast<float32>(height) / scale}};
    REQUIRE(context.TrySetDocument({document.GetData(), document.GetSize()}, viewport) == ui::Status::Ok);
    const std::span<const ui::PaintCommand> commands = context.GetPaintCommands();
    REQUIRE(commands.size() == ELEMENT_COUNT);
    core::StaticArray<rhi::RasterRectangle, ELEMENT_COUNT> snapped;
    for (usize index = 0; index < commands.size(); ++index)
    {
        REQUIRE(ui_render::TryComputePhysicalRectangle(commands[index].Bounds, mapping, snapped[index]) == true);
    }
    usize mismatches = 0;
    for (uint32 y = 0; y < height; ++y)
    {
        for (uint32 x = 0; x < width; ++x)
        {
            ui::WidgetId expected = 0;
            for (usize index = 0; index < commands.size(); ++index)
            {
                const rhi::RasterRectangle& rectangle = snapped[index];
                const bool isCovered = x >= rectangle.X && x < rectangle.X + rectangle.Width && y >= rectangle.Y &&
                                       y < rectangle.Y + rectangle.Height;
                if (isCovered == true && IsButton(commands[index].Id) == true)
                {
                    expected = commands[index].Id;
                }
            }
            if (Press(context, mapping, x, y) != expected)
            {
                ++mismatches;
            }
        }
    }
    CHECK(mismatches == 0);
}

[[nodiscard]] ui::PaintCommand MakeCommand(ui::Rect bounds, ui::Rect clip) noexcept
{
    return {1, bounds, RED, clip};
}
} // namespace

TEST_CASE("UiRender mapping validation", "[ui_render]")
{
    CHECK(ui_render::IsValid({1, 0, 0, 1, 1}) == true);
    CHECK(ui_render::IsValid({0, 0, 0, 1, 1}) == false);
    CHECK(ui_render::IsValid({-1, 0, 0, 1, 1}) == false);
    CHECK(ui_render::IsValid({std::numeric_limits<float32>::infinity(), 0, 0, 1, 1}) == false);
    CHECK(ui_render::IsValid({1, std::numeric_limits<float32>::quiet_NaN(), 0, 1, 1}) == false);
    CHECK(ui_render::IsValid({1, 0, 0, 0, 1}) == false);
    CHECK(ui_render::IsValid({1, 0, 0, 1, 0}) == false);
}

TEST_CASE("UiRender snaps logical rectangles to covered pixel centres", "[ui_render]")
{
    rhi::RasterRectangle rectangle;
    REQUIRE(ui_render::TryComputePhysicalRectangle({2, 3, 4, 5}, {1, 0, 0, 64, 64}, rectangle) == true);
    CHECK((rectangle.X == 2 && rectangle.Y == 3 && rectangle.Width == 4 && rectangle.Height == 5));
    REQUIRE(ui_render::TryComputePhysicalRectangle({2, 3, 4, 5}, {2, 0, 0, 64, 64}, rectangle) == true);
    CHECK((rectangle.X == 4 && rectangle.Y == 6 && rectangle.Width == 8 && rectangle.Height == 10));

    // At scale 2, logical 10.25 lands exactly on the centre of physical pixel 20: the left edge
    // includes it and a right edge at the same value excludes it, as half-open hit testing does.
    REQUIRE(ui_render::TryComputePhysicalRectangle({10.25F, 0, 20, 1}, {2, 0, 0, 64, 64}, rectangle) == true);
    CHECK((rectangle.X == 20 && rectangle.Width == 40));

    // Origin shifts the logical frame; clamping keeps the rectangle inside the framebuffer.
    REQUIRE(ui_render::TryComputePhysicalRectangle({-8, -8, 12, 100}, {1, 2, 3, 16, 16}, rectangle) == true);
    CHECK((rectangle.X == 0 && rectangle.Y == 0 && rectangle.Width == 6 && rectangle.Height == 16));
    REQUIRE(ui_render::TryComputePhysicalRectangle({100, 100, 4, 4}, {1, 0, 0, 16, 16}, rectangle) == true);
    CHECK((rectangle.Width == 0 && rectangle.Height == 0));

    // A rectangle between two pixel centres covers nothing.
    REQUIRE(ui_render::TryComputePhysicalRectangle({0.6F, 0.6F, 0.8F, 0.8F}, {1, 0, 0, 16, 16}, rectangle) == true);
    CHECK((rectangle.Width == 0 && rectangle.Height == 0));

    const rhi::RasterRectangle unchanged = rectangle;
    CHECK(ui_render::TryComputePhysicalRectangle({0, 0, -1, 1}, {1, 0, 0, 16, 16}, rectangle) == false);
    CHECK(ui_render::TryComputePhysicalRectangle({std::numeric_limits<float32>::quiet_NaN(), 0, 1, 1},
                                                 {1, 0, 0, 16, 16},
                                                 rectangle) == false);
    CHECK(ui_render::TryComputePhysicalRectangle({0, 0, 1, 1}, {0, 0, 0, 16, 16}, rectangle) == false);
    CHECK((rectangle.X == unchanged.X && rectangle.Width == unchanged.Width));
}

TEST_CASE("UiRender pixel coverage equals the routed region at integer and fractional scales", "[ui_render]")
{
    constexpr uint32 PIXELS = 96;
    constexpr core::StaticArray<float32, 7> SCALES{{1, 2, 3, 1.25F, 1.5F, 0.75F, 1.0F / 3.0F}};
    constexpr core::StaticArray<float32, 3> ORIGINS{{0, 0.5F, 3}};
    // Edges on, near and between pixel centres, including values without an exact binary form.
    constexpr core::StaticArray<float32, 10> STARTS{{0, 1, 2.5F, 3.25F, 4.75F, 0.1F, 7.3F, 10.25F, 1.0F / 3.0F, -2.5F}};
    constexpr core::StaticArray<float32, 6> EXTENTS{{0, 0.5F, 1, 7.7F, 20.25F, 30.1F}};
    usize mismatches = 0;
    for (const float32 scale : SCALES)
    {
        for (const float32 origin : ORIGINS)
        {
            for (const float32 start : STARTS)
            {
                for (const float32 extent : EXTENTS)
                {
                    const ui_render::SurfaceMapping mapping{scale, origin, origin, PIXELS, PIXELS};
                    if (DoesAxisMatchRouting({start, start, extent, extent}, mapping) == false)
                    {
                        ++mismatches;
                    }
                }
            }
        }
    }
    CHECK(mismatches == 0);
}

TEST_CASE("UiRender painted buttons are exactly the pixels Ui activates", "[ui_render]")
{
    // Two integer scales of the same 48 x 32 logical document, then a resize, as in the SDK fixture,
    // plus a fractional scale whose clip edge falls between pixel centres.
    CheckRoutingMatchesPaint(1, 48, 32);
    CheckRoutingMatchesPaint(2, 96, 64);
    CheckRoutingMatchesPaint(2, 80, 60);
    CheckRoutingMatchesPaint(1.5F, 72, 48);
}

TEST_CASE("UiRender appends clipped solid quads in paint order and merges equal clips", "[ui_render]")
{
    renderer::OverlayList list;
    REQUIRE(list.Initialize() == Status::Ready);
    ui::Context context;
    REQUIRE(context.TryInitialize(ELEMENT_COUNT) == ui::Status::Ok);
    const Document document = MakeDocument();
    REQUIRE(context.TrySetDocument({document.GetData(), document.GetSize()}, {{0, 0, 48, 32}}) == ui::Status::Ok);
    const ui_render::SurfaceMapping mapping{2, 0, 0, 96, 64};
    ui_render::PaintReport report;
    REQUIRE(ui_render::AppendPaint(list, context.GetPaintCommands(), mapping, report) == Status::Ready);
    CHECK(report.Commands == ELEMENT_COUNT);
    CHECK(report.Quads == ELEMENT_COUNT);
    CHECK(report.Culled == 0);
    // Panel (viewport clip), overflow button (panel clip), then sibling and corner share the viewport clip.
    CHECK(report.Runs == 3);
    REQUIRE(list.CommandCount() == 3);
    CHECK(list.VertexCount() == ELEMENT_COUNT * QUAD_VERTEX_COUNT);
    const renderer::OverlayCommand& clipped = list.Commands()[1];
    CHECK(clipped.Style.Clip == true);
    // Panel spans logical [4, 34.25) x [4, 24): physical [8, 68.5) x [8, 48) at scale 2; the right
    // edge's pixel-centre tie at 68.5 excludes pixel 68.
    CHECK((clipped.Style.Scissor.X == 8 && clipped.Style.Scissor.Y == 8 && clipped.Style.Scissor.Width == 60 &&
           clipped.Style.Scissor.Height == 40));
    CHECK(list.Commands()[2].Count == 2 * QUAD_VERTEX_COUNT);
    // The overflow button's quad is clipped geometry, not only scissored: [40, 68) x [16, 36).
    const renderer::OverlayVertex& topLeft = list.Vertices()[clipped.First];
    const renderer::OverlayVertex& bottomRight = list.Vertices()[clipped.First + 2];
    CHECK(IsNear((topLeft.Clip.X + 1) * 48, 40) == true);
    CHECK(IsNear((1 - topLeft.Clip.Y) * 32, 16) == true);
    CHECK(IsNear((bottomRight.Clip.X + 1) * 48, 68) == true);
    CHECK(IsNear((1 - bottomRight.Clip.Y) * 32, 36) == true);
    CHECK(topLeft.Color.X == 1);
    CHECK(topLeft.Coverage.Z == 0);

    // A second append under the last clip merges into the existing run.
    const ui::PaintCommand again{CORNER_BUTTON, {40, 26, 6, 6}, GREEN, {0, 0, 48, 32}};
    REQUIRE(ui_render::AppendPaint(list, {&again, 1}, mapping, report) == Status::Ready);
    CHECK(report.Runs == 0);
    CHECK(list.CommandCount() == 3);
}

TEST_CASE("UiRender culls commands that cover no pixel centre", "[ui_render]")
{
    renderer::OverlayList list;
    REQUIRE(list.Initialize() == Status::Ready);
    const core::StaticArray<ui::PaintCommand, 2> commands{{
        MakeCommand({0.6F, 0.6F, 0.8F, 0.8F}, {0, 0, 16, 16}),
        MakeCommand({2, 2, 4, 4}, {0, 0, 16, 16}),
    }};
    ui_render::PaintReport report;
    REQUIRE(ui_render::AppendPaint(list, {commands.GetData(), commands.GetSize()}, {1, 0, 0, 16, 16}, report) ==
            Status::Ready);
    CHECK(report.Culled == 1);
    CHECK(report.Quads == 1);
    CHECK(report.Runs == 1);
    CHECK(list.VertexCount() == QUAD_VERTEX_COUNT);
}

TEST_CASE("UiRender failures preserve the overlay list and report", "[ui_render]")
{
    renderer::OverlayList uninitialized;
    ui_render::PaintReport report;
    report.Commands = 7;
    const ui::PaintCommand valid = MakeCommand({0, 0, 4, 4}, {0, 0, 16, 16});
    CHECK(ui_render::AppendPaint(uninitialized, {&valid, 1}, {1, 0, 0, 16, 16}, report) == Status::NotReady);

    renderer::OverlayList list;
    REQUIRE(list.Initialize() == Status::Ready);
    CHECK(ui_render::AppendPaint(list, {&valid, 1}, {0, 0, 0, 16, 16}, report) == Status::InvalidDescription);

    // An invalid command after a valid one appends neither.
    core::StaticArray<ui::PaintCommand, 2> commands{{valid, valid}};
    commands[1].Fill.G = 2;
    CHECK(ui_render::AppendPaint(list, {commands.GetData(), commands.GetSize()}, {1, 0, 0, 16, 16}, report) ==
          Status::InvalidDescription);
    commands[1] = valid;
    commands[1].Clip.Width = std::numeric_limits<float32>::infinity();
    CHECK(ui_render::AppendPaint(list, {commands.GetData(), commands.GetSize()}, {1, 0, 0, 16, 16}, report) ==
          Status::InvalidDescription);
    CHECK(list.VertexCount() == 0);
    CHECK(list.CommandCount() == 0);
    CHECK(report.Commands == 7);
}

TEST_CASE("UiRender rejects commands beyond the overlay capacity without partial geometry", "[ui_render]")
{
    constexpr usize QUAD_CAPACITY = renderer::OverlayList::MAX_VERTICES / QUAD_VERTEX_COUNT;
    constexpr uint32 EXTENT = 64;
    const ui_render::SurfaceMapping mapping{1, 0, 0, EXTENT, EXTENT};
    renderer::OverlayList list;
    REQUIRE(list.Initialize() == Status::Ready);
    ui_render::PaintReport report;

    // Vertex capacity: fill all but one quad under one clip, then offer two.
    const ui::PaintCommand quad = MakeCommand({0, 0, 1, 1}, {0, 0, EXTENT, EXTENT});
    for (usize index = 0; index + 1 < QUAD_CAPACITY; ++index)
    {
        REQUIRE(ui_render::AppendPaint(list, {&quad, 1}, mapping, report) == Status::Ready);
    }
    const core::StaticArray<ui::PaintCommand, 2> pair{{quad, quad}};
    const usize vertices = list.VertexCount();
    CHECK(ui_render::AppendPaint(list, {pair.GetData(), pair.GetSize()}, mapping, report) == Status::CapacityExceeded);
    CHECK(list.VertexCount() == vertices);
    CHECK(list.DroppedCommands() == 0);
    REQUIRE(ui_render::AppendPaint(list, {pair.GetData(), 1}, mapping, report) == Status::Ready);
    CHECK(list.VertexCount() == renderer::OverlayList::MAX_VERTICES);

    // Run capacity: alternate clips so every quad needs its own draw.
    list.Clear();
    const ui::PaintCommand left = MakeCommand({0, 0, 1, 1}, {0, 0, 2, 2});
    const ui::PaintCommand right = MakeCommand({0, 0, 1, 1}, {0, 0, 3, 3});
    for (usize index = 0; index + 1 < renderer::OverlayList::MAX_COMMANDS; ++index)
    {
        REQUIRE(ui_render::AppendPaint(list, {index % 2 == 0 ? &left : &right, 1}, mapping, report) == Status::Ready);
    }
    const usize runs = list.CommandCount();
    // The last run uses the left clip, so right then left needs two new runs where one remains.
    const core::StaticArray<ui::PaintCommand, 2> alternating{{right, left}};
    CHECK(ui_render::AppendPaint(list, {alternating.GetData(), alternating.GetSize()}, mapping, report) ==
          Status::CapacityExceeded);
    CHECK(list.CommandCount() == runs);
    CHECK(list.DroppedCommands() == 0);
}
