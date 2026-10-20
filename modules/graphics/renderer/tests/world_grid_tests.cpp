#include <ludus/foundation/containers/static_array.hpp>
#include <ludus/foundation/math/vector.hpp>
#include <ludus/graphics/renderer/renderer.hpp>
#include <ludus/graphics/renderer/world_grid.hpp>

#include <catch2/catch_test_macros.hpp>

using namespace ludus::foundation;
namespace renderer = ludus::graphics::renderer;
namespace math = ludus::foundation::math;

TEST_CASE("World grid generation preserves outputs on invalid inputs and omits shadow guides", "[renderer][grid]")
{
    StaticArray<renderer::WorldGridLine, renderer::WORLD_GRID_LINE_CAPACITY> lines{};
    renderer::ViewDescription view;
    usize count = 9;
    float32 spacing = 9;
    view.WorldGrid = { .IsVisible = true, .FocusDistance = 5, .Width = 640, .Height = 480 };
    CHECK(renderer::BuildWorldGridLines(view, {lines.GetData(), 1}, count, spacing) == false);
    CHECK(count == 9);
    CHECK(spacing == 9);
    view.WorldGrid.Height = 0;
    CHECK(renderer::BuildWorldGridLines(view, {lines.GetData(), lines.GetSize()}, count, spacing) == false);
    CHECK(count == 9);
    view.WorldGrid.Height = 480;
    view.WorldGrid.FocusDistance = 0;
    CHECK(renderer::BuildWorldGridLines(view, {lines.GetData(), lines.GetSize()}, count, spacing) == false);
    CHECK(count == 9);
    view.WorldGrid.FocusDistance = 5;
    view.WorldGrid.FocusDistance = .001F;
    view.CameraPosition = {1e38F, 0, 0};
    CHECK(renderer::BuildWorldGridLines(view, {lines.GetData(), lines.GetSize()}, count, spacing) == false);
    CHECK(count == 9);
    CHECK(spacing == 9);
    CHECK(lines[0].StartWorld == math::Vector3{});
    view.CameraPosition = {};
    view.WorldGrid.FocusDistance = 5;
    REQUIRE(renderer::BuildWorldGridLines(view, {lines.GetData(), lines.GetSize()}, count, spacing) == true);
    CHECK(count == renderer::WORLD_GRID_LINE_CAPACITY);
    CHECK(spacing > 0);
    const float32 firstSpacing = spacing;
    view.WorldGrid.FocusDistance = 500;
    REQUIRE(renderer::BuildWorldGridLines(view, {lines.GetData(), lines.GetSize()}, count, spacing) == true);
    CHECK(spacing > firstSpacing);
    for (const renderer::WorldGridLine& line : lines)
    {
        CHECK(math::IsFinite(line.StartWorld) == true);
        CHECK(math::IsFinite(line.EndWorld) == true);
        CHECK(math::IsFinite(line.HalfWidthWorld) == true);
        CHECK(line.Color.W == 1);
    }
    view.ShadowCasters = true;
    REQUIRE(renderer::BuildWorldGridLines(view, {lines.GetData(), lines.GetSize()}, count, spacing) == true);
    CHECK(count == 0);
    CHECK(spacing == 0);
}

TEST_CASE("Orthographic world grids choose XY XZ and YZ without changing scene state", "[renderer][grid]")
{
    constexpr usize PLANE_COUNT = 3;
    constexpr StaticArray<math::Vector3, PLANE_COUNT> directions{{{0, 0, -1}, {0, -1, 0}, {-1, 0, 0}}};
    for (usize plane = 0; plane < PLANE_COUNT; ++plane)
    {
        renderer::ViewDescription view;
        view.OrthographicCamera = true;
        view.CameraForward = directions[plane];
        view.WorldGrid = { .IsVisible = true, .FocusDistance = 5, .Width = 640, .Height = 480 };
        StaticArray<renderer::WorldGridLine, renderer::WORLD_GRID_LINE_CAPACITY> lines{};
        usize count = 0;
        float32 spacing = 0;
        REQUIRE(renderer::BuildWorldGridLines(view, {lines.GetData(), lines.GetSize()}, count, spacing) == true);
        REQUIRE(count == renderer::WORLD_GRID_LINE_CAPACITY);
        for (usize i = 0; i + PLANE_COUNT < count; ++i)
        {
            CHECK(math::Dot(lines[i].StartWorld, directions[plane]) == 0);
            CHECK(math::Dot(lines[i].EndWorld, directions[plane]) == 0);
            CHECK(math::Dot(lines[i].HalfWidthWorld, directions[plane]) == 0);
        }
    }
}
