#include <ludus/foundation/containers/static_array.hpp>
#include <ludus/foundation/math/scalar.hpp>
#include <ludus/graphics/renderer/render_graph.hpp>
#include <ludus/graphics/renderer/surface_points.hpp>

#include <catch2/catch_test_macros.hpp>

namespace
{
using namespace ludus::foundation;
namespace renderer = ludus::graphics::renderer;
namespace rhi = ludus::graphics::rhi;
constexpr usize CORNERS = 3;
constexpr usize SAMPLE_COUNT = 128;
constexpr usize BUDGET = usize{4} * 1024 * 1024;
constexpr float32 FEATURE_TOLERANCE = 0.000001F;
struct Fixture final
{
    StaticArray<math::Vector3, CORNERS> Positions{{{0, 0, 0}, {2, 0, 0}, {0, 2, 0}}};
    StaticArray<uint32, CORNERS> Indices{{0, 1, 2}};
    uint32 Binding = 0;
    renderer::SceneShaderMaterial Material;
    renderer::RenderGraphMesh Mesh;
    renderer::RenderGraphItem Item;
    renderer::RenderGraphSceneDescription Scene;
    renderer::SurfacePointSampling Sampling{SAMPLE_COUNT, 0, SAMPLE_COUNT, BUDGET};
    Fixture()
    {
        Mesh.Geometry = {Positions.GetData(), Positions.GetSize(), Indices.GetData(), Indices.GetSize()};
        Mesh.MaterialBindings = {&Binding, 1};
        Item.SourceId = 1;
        Material.BaseColor = {0.5F, 1, 0};
        Material.Emission = {2, 1, 0};
        Scene.Meshes = {&Mesh, 1};
        Scene.Items = {&Item, 1};
        Scene.Inputs.Materials = {&Material, 1};
        Scene.HasMaterialInterface = true;
        Scene.SourceGeneration = 1;
    }
};
} // namespace
TEST_CASE("Surface point preparation preserves transformed source features and deterministic replay",
          "[renderer][surface-points]")
{
    Fixture fixture;
    fixture.Item.MeshToWorld.Translation = {0, 0, 1};
    renderer::SurfacePointCloud points;
    REQUIRE(renderer::PrepareSurfacePointCloud(fixture.Scene, fixture.Sampling, points) == rhi::RasterStatus::Ready);
    CHECK(points.Points.GetSize() == SAMPLE_COUNT);
    CHECK(points.ProposalAreaWorld == 2);
    CHECK(points.SourceGeneration == fixture.Scene.SourceGeneration);
    for (const renderer::SurfacePoint& point : points.Points)
    {
        CHECK(point.PositionWorld.X >= 0);
        CHECK(point.PositionWorld.Y >= 0);
        CHECK(point.PositionWorld.X + point.PositionWorld.Y <= 2);
        CHECK(math::Abs(point.PositionWorld.Z - 1.F) <= FEATURE_TOLERANCE);
        CHECK(point.NormalWorld.Z == 1);
        CHECK(math::Abs(point.Albedo.X - fixture.Material.BaseColor.X) <= FEATURE_TOLERANCE);
        CHECK(math::Abs(point.Albedo.Y - fixture.Material.BaseColor.Y) <= FEATURE_TOLERANCE);
        CHECK(point.Emission.X == fixture.Material.Emission.X);
        CHECK(point.SourceIdLow == fixture.Item.SourceId);
    }
    renderer::SurfacePointCloud replay;
    REQUIRE(renderer::PrepareSurfacePointCloud(fixture.Scene, fixture.Sampling, replay) == rhi::RasterStatus::Ready);
    for (usize i = 0; i < SAMPLE_COUNT; ++i)
    {
        CHECK(replay.Points[i].PositionWorld.X == points.Points[i].PositionWorld.X);
        CHECK(replay.Points[i].PositionWorld.Y == points.Points[i].PositionWorld.Y);
    }
    fixture.Sampling.MaximumBytes = 1;
    CHECK(renderer::PrepareSurfacePointCloud(fixture.Scene, fixture.Sampling, points) ==
          rhi::RasterStatus::CapacityExceeded);
    CHECK(points.Points[0].PositionWorld.X == replay.Points[0].PositionWorld.X);
}
TEST_CASE("Surface point sampling applies texture color and rejects masked holes within its work budget",
          "[renderer][surface-points]")
{
    Fixture fixture;
    constexpr usize TEXEL_BYTES = 4;
    StaticArray<uint8, TEXEL_BYTES> texel{{0, 255, 255, 255}};
    fixture.Material
        .ColorTexture = {texel.GetData(), texel.GetSize(), texel.GetSize(), 1, 1, rhi::RasterFormat::Rgba8Unorm};
    renderer::SurfacePointCloud points;
    REQUIRE(renderer::PrepareSurfacePointCloud(fixture.Scene, fixture.Sampling, points) == rhi::RasterStatus::Ready);
    CHECK(points.Points[0].Albedo.X == 0);
    CHECK(points.Points[0].Albedo.Y == 1);
    const math::Vector3 saved = points.Points[0].PositionWorld;
    fixture.Material.IsAlphaMasked = true;
    texel[3] = 0;
    CHECK(renderer::PrepareSurfacePointCloud(fixture.Scene, fixture.Sampling, points) ==
          rhi::RasterStatus::CapacityExceeded);
    CHECK(points.Points[0].PositionWorld.X == saved.X);
    CHECK(points.Points.GetSize() == SAMPLE_COUNT);
}
