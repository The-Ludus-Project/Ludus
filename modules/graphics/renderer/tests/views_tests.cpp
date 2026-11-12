#include <ludus/graphics/renderer/views.hpp>

#include <ludus/foundation/math/matrix.hpp>
#include <ludus/foundation/math/scalar.hpp>
#include <ludus/foundation/math/vector.hpp>

#include <catch2/catch_test_macros.hpp>
#include <limits>

using namespace ludus::foundation;
namespace rr = ludus::graphics::renderer;
namespace rhi = ludus::graphics::rhi;
TEST_CASE("Virtual mapping shares rounded edges with drawing, bars, picking and explicit DPI", "[renderer][views]")
{
    rr::ViewMapping mapping;
    REQUIRE(rr::ResolveViewMapping({100, 50}, {10, 20, 200, 200}, 220, 240, mapping) == rhi::RasterStatus::Ready);
    CHECK(mapping.Viewport.X == 10);
    CHECK(mapping.Viewport.Y == 70);
    CHECK(mapping.Viewport.Width == 200);
    CHECK(mapping.Viewport.Height == 100);
    math::Vector2 output{777, 888};
    CHECK(rr::MapPhysicalToVirtual(mapping, {110, 40}, output) == rhi::RasterStatus::InvalidDescription);
    CHECK(output.X == 777);
    REQUIRE(rr::MapPhysicalToVirtual(mapping, {110, 120}, output) == rhi::RasterStatus::Ready);
    CHECK(output.X == 50);
    CHECK(output.Y == 25);
    REQUIRE(rr::MapWindowToVirtual(mapping, {55, 60}, {2, 2}, output) == rhi::RasterStatus::Ready);
    CHECK(output.X == 50);
    CHECK(output.Y == 25);
    REQUIRE(rr::MapPhysicalToVirtual(mapping, {110, 40}, output, true) == rhi::RasterStatus::Ready);
    CHECK(output.X == 50);
    CHECK(output.Y == 0);
    CHECK(rr::MapPhysicalToVirtual(mapping, {210, 120}, output) == rhi::RasterStatus::InvalidDescription);
    for (uint32 y = 0; y < 50; y += 7)
    {
        for (uint32 x = 0; x < 100; x += 11)
        {
            math::Vector2 pixel, roundTrip;
            REQUIRE(rr::MapVirtualToPhysical(mapping, {static_cast<float32>(x), static_cast<float32>(y)}, pixel) ==
                    rhi::RasterStatus::Ready);
            REQUIRE(rr::MapPhysicalToVirtual(mapping, pixel, roundTrip) == rhi::RasterStatus::Ready);
            CHECK(roundTrip.X == x);
            CHECK(roundTrip.Y == y);
        }
    }
}
TEST_CASE("Fill crops consistently and integer fit exposes fractional fallback", "[renderer][views]")
{
    rr::ViewMapping mapping;
    REQUIRE(rr::ResolveViewMapping({100, 50, rr::ScreenFit::Fill}, {0, 0, 100, 100}, 100, 100, mapping) ==
            rhi::RasterStatus::Ready);
    CHECK(mapping.Origin.X == -50);
    CHECK(mapping.Scale.X == 2);
    CHECK(mapping.ClipTransform.At(0, 0) == 2);
    math::Vector2 logical;
    REQUIRE(rr::MapPhysicalToVirtual(mapping, {0, 0}, logical) == rhi::RasterStatus::Ready);
    CHECK(logical.X == 25);
    CHECK(logical.Y == 0);
    REQUIRE(rr::ResolveViewMapping({100, 50, rr::ScreenFit::IntegerFit}, {0, 0, 251, 150}, 251, 150, mapping) ==
            rhi::RasterStatus::Ready);
    CHECK(mapping.Viewport.X == 25);
    CHECK(mapping.Viewport.Y == 25);
    CHECK(mapping.Scale.X == 2);
    CHECK_FALSE(mapping.FractionalFallback);
    REQUIRE(rr::ResolveViewMapping({100, 50, rr::ScreenFit::IntegerFit}, {0, 0, 51, 49}, 51, 49, mapping) ==
            rhi::RasterStatus::Ready);
    CHECK(mapping.FractionalFallback);
    CHECK(mapping.Viewport.Height == 26);
    REQUIRE(rr::ResolveViewMapping({100, 50, rr::ScreenFit::Stretch}, {0, 0, 300, 100}, 300, 100, mapping) ==
            rhi::RasterStatus::Ready);
    CHECK(mapping.Scale.X == 3);
    CHECK(mapping.Scale.Y == 2);
}
TEST_CASE("Split views derive shared half-open boundaries including odd dimensions", "[renderer][views]")
{
    for (uint32 columns = 1; columns <= 9; ++columns)
    {
        uint32 edge = 7;
        for (uint32 cell = 0; cell < columns; ++cell)
        {
            rhi::RasterRectangle rectangle;
            REQUIRE(rr::SplitViewRectangle({7, 3, 101, 67}, columns, 1, cell, 0, rectangle) ==
                    rhi::RasterStatus::Ready);
            CHECK(rectangle.X == edge);
            CHECK(rectangle.Y == 3);
            CHECK(rectangle.Height == 67);
            edge += rectangle.Width;
        }
        CHECK(edge == 108);
    }
    rhi::RasterRectangle unchanged{1, 2, 3, 4};
    CHECK(rr::SplitViewRectangle({0, 0, 1, 1}, 2, 1, 0, 0, unchanged) == rhi::RasterStatus::NotReady);
    CHECK(unchanged.X == 1);
    CHECK(rr::SplitViewRectangle({~uint32{0}, 0, 1, 1}, 1, 1, 0, 0, unchanged) ==
          rhi::RasterStatus::InvalidDescription);
    CHECK(rr::SplitViewRectangle({0, 0, 10, 10}, 0, 1, 0, 0, unchanged) == rhi::RasterStatus::InvalidDescription);
}
TEST_CASE("Invalid and hidden mappings preserve caller outputs", "[renderer][views]")
{
    rr::ViewMapping mapping;
    mapping.DrawableWidth = 123;
    CHECK(rr::ResolveViewMapping({100, 50}, {}, 0, 0, mapping) == rhi::RasterStatus::NotReady);
    CHECK(mapping.DrawableWidth == 123);
    CHECK(rr::ResolveViewMapping({100, 50}, {99, 0, 2, 1}, 100, 100, mapping) == rhi::RasterStatus::InvalidDescription);
    CHECK(rr::ResolveViewMapping({std::numeric_limits<float32>::quiet_NaN(), 50}, {0, 0, 10, 10}, 10, 10, mapping) ==
          rhi::RasterStatus::InvalidDescription);
    CHECK(mapping.DrawableWidth == 123);
    math::Vector2 point{2, 3};
    CHECK(rr::MapWindowToVirtual(mapping, {1, 1}, {0, 2}, point) == rhi::RasterStatus::InvalidDescription);
    CHECK(point.X == 2);
}
TEST_CASE("Reverse-Z lenses map near and far correctly and infinite perspective retains distant geometry",
          "[renderer][views]")
{
    rr::ViewMapping mapping;
    REQUIRE(rr::ResolveViewMapping({100, 50}, {0, 0, 200, 100}, 200, 100, mapping) == rhi::RasterStatus::Ready);
    const rr::ProjectionKind kinds[]{rr::ProjectionKind::Orthographic,
                                     rr::ProjectionKind::Perspective,
                                     rr::ProjectionKind::PerspectiveInfinite};
    for (const auto kind : kinds)
    {
        rr::ProjectionDescription lens;
        lens.Kind = kind;
        lens.Near = 1;
        lens.Far = 10;
        rr::ViewDescription view;
        REQUIRE(rr::BuildViewDescription(lens, mapping, view) == rhi::RasterStatus::Ready);
        CHECK(view.Depth == rr::DepthConvention::ReverseZ);
        CHECK(view.InfiniteFar == (kind == rr::ProjectionKind::PerspectiveInfinite));
        const auto near = view.WorldToClip * math::Vector4{0, 0, -1, 1};
        CHECK(math::Abs(near.Z / near.W - 1) < 1e-6F);
        const auto far = view.WorldToClip * math::Vector4{0, 0, -10, 1};
        if (kind == rr::ProjectionKind::PerspectiveInfinite)
        {
            CHECK(math::Abs(far.Z / far.W - .1F) < 1e-6F);
        }
        else
        {
            CHECK(math::Abs(far.Z / far.W) < 1e-6F);
        }
    }
    rr::ProjectionDescription invalid;
    invalid.Near = -1;
    rr::ViewDescription sentinel;
    sentinel.CullMargin = 123;
    CHECK(rr::BuildViewDescription(invalid, mapping, sentinel) == rhi::RasterStatus::InvalidDescription);
    CHECK(sentinel.CullMargin == 123);
}

TEST_CASE("Fill projection distinguishes uncropped source from direct output and applies crop once",
          "[renderer][views]")
{
    rr::ViewMapping mapping;
    REQUIRE(rr::ResolveViewMapping({100, 50, rr::ScreenFit::Fill}, {0, 0, 100, 100}, 100, 100, mapping) ==
            rhi::RasterStatus::Ready);
    rr::ProjectionDescription lens;
    lens.Kind = rr::ProjectionKind::Orthographic;
    lens.OrthographicHeight = 2;
    lens.WorldToView.Columns[3].X = -1;
    rr::ViewDescription source;
    rr::ViewDescription direct;
    REQUIRE(rr::BuildViewDescription(lens, mapping, source) == rhi::RasterStatus::Ready);
    REQUIRE(rr::BuildViewDescription(lens, mapping, direct, rr::ProjectionSpace::OutputRegion) ==
            rhi::RasterStatus::Ready);
    // World x=2 maps to view x=1, then to logical clip x=0.5. Fill doubles logical clip x for direct output.
    const math::Vector4 positionWorld{2, 0, -1, 1};
    CHECK((source.WorldToClip * positionWorld).X == 0.5F);
    CHECK((direct.WorldToClip * positionWorld).X == 1);
    CHECK(source.OverlayToClip.At(0, 0) == 1);
    CHECK(direct.OverlayToClip.At(0, 0) == 2);
    direct.CullMargin = 123;
    CHECK(rr::BuildViewDescription(lens, mapping, direct, static_cast<rr::ProjectionSpace>(255)) ==
          rhi::RasterStatus::InvalidDescription);
    CHECK(direct.CullMargin == 123);
}
