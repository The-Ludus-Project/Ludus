#include "one_more_vertex_oracle.h"

#include <ludus/foundation/containers/static_array.hpp>
#include <ludus/foundation/math/scalar.hpp>

#include <catch2/catch_test_macros.hpp>

using namespace ludus::foundation;
namespace renderer = ludus::graphics::renderer;
namespace
{
constexpr usize SURFACE_COUNT = 3;
constexpr usize TRIANGLE_COUNT = 5;
constexpr usize CORNER_COUNT = 3;
struct SparseReflector final
{
    StaticArray<renderer::internal::ReferenceSurfaceData, SURFACE_COUNT> Surfaces;
    renderer::internal::ReferenceSceneData Scene;
    SparseReflector()
    {
        for (renderer::internal::ReferenceSurfaceData& surface : Surfaces)
        {
            surface.Bsdf.Scattering.Model = ludus::content::scene_format::MaterialModel::Diffuse;
            surface.Bsdf.BaseColor = {0.8F, 0.8F, 0.8F};
            surface.IsEmissionTwoSided = false;
        }
        Surfaces[2].Emission = {8, 8, 8};
        Surfaces[2].Bsdf.BaseColor = {};
        REQUIRE(Scene.Instances.TryResize(SURFACE_COUNT) == true);
        REQUIRE(Scene.Triangles.TryResize(TRIANGLE_COUNT) == true);
        Scene.InstanceCount = SURFACE_COUNT;
        const StaticArray<math::Vector3, TRIANGLE_COUNT * CORNER_COUNT> positions{{
            {-10, -10, 0.5F},
            {-10, 10, 0.5F},
            {10, -10, 0.5F},
            {10, -10, 0.5F},
            {-10, 10, 0.5F},
            {10, 10, 0.5F},
            {0.8F, -0.2F, -0.2F},
            {0.8F, -0.2F, 0.2F},
            {0.8F, 0.2F, -0.2F},
            {0.8F, 0.2F, -0.2F},
            {0.8F, -0.2F, 0.2F},
            {0.8F, 0.2F, 0.2F},
            {-0.6F, -0.2F, -0.5F},
            {-0.4F, -0.2F, -0.5F},
            {-0.5F, 0.2F, -0.5F},
        }};
        for (uint32 index = 0; index < TRIANGLE_COUNT; ++index)
        {
            renderer::internal::ReferenceTriangle& triangle = Scene.Triangles[index];
            triangle.Instance = index / 2;
            triangle.Primitive = index % 2;
            triangle.Error = {0.00001F, 0.00001F, 0.00001F};
            for (usize corner = 0; corner < CORNER_COUNT; ++corner)
            {
                triangle.Positions[corner] = positions[index * CORNER_COUNT + corner];
                triangle.Colors[corner] = {1, 1, 1, 1};
            }
            const math::Vector3 crossWorld = math::Cross(triangle.Positions[1] - triangle.Positions[0],
                                                         triangle.Positions[2] - triangle.Positions[0]);
            triangle.Area = math::Length(crossWorld) * 0.5F;
            triangle.GeometricNormal = crossWorld / math::Length(crossWorld);
            for (math::Vector3& normalWorld : triangle.Normals)
            {
                normalWorld = triangle.GeometricNormal;
            }
        }
        for (usize index = 0; index < SURFACE_COUNT; ++index)
        {
            Scene.Instances[index].Surface = &Surfaces[index];
            Scene.Instances[index].FirstTriangle = index * 2;
            Scene.Instances[index].TriangleCount = index == 2 ? 1 : 2;
        }
        REQUIRE(Scene.Emitters.TryAdd(4) == true);
        REQUIRE(renderer::internal::BuildReferenceBvh(Scene) == ludus::graphics::rhi::RasterStatus::Ready);
    }
};
} // namespace

TEST_CASE("CPU hierarchical geometry sampling retains support and reduces sparse-reflector variance",
          "[renderer][one-more-vertex-nee][cpu]")
{
    SparseReflector fixture;
    renderer::testing::OneMoreVertexOracle oracle;
    REQUIRE(oracle.Build(fixture.Scene) == true);
    for (uint32 receiver = 0; receiver < TRIANGLE_COUNT; ++receiver)
    {
        CHECK(math::Abs(oracle.IntegratePdf(fixture.Scene, receiver, 4) - 1) < 0.00001F);
    }
    constexpr usize ESTIMATOR_COUNT = 3;
    constexpr uint32 SAMPLE_COUNT = 4096;
    const math::RandomKey key{42};
    StaticArray<float64, ESTIMATOR_COUNT> sums{};
    StaticArray<float64, ESTIMATOR_COUNT> squares{};
    ludus::graphics::rhi::RayInput ray;
    ray.Origin[2] = -1;
    ray.Direction[2] = 1;
    ray.MaxDistance = 2;
    for (uint32 sample = 0; sample < SAMPLE_COUNT; ++sample)
    {
        for (uint32 estimator = 0; estimator < ESTIMATOR_COUNT; ++estimator)
        {
            math::Vector3 emission;
            math::Vector3 direct;
            math::Vector3 indirect;
            const math::Vector3 total =
                oracle.Evaluate(fixture.Scene, ray, key, 0, sample, estimator, emission, direct, indirect);
            REQUIRE(math::IsFinite(total) == true);
            REQUIRE(indirect.X >= 0);
            const float64 value = static_cast<float64>(indirect.X);
            sums[estimator] += value;
            squares[estimator] += value * value;
        }
    }
    StaticArray<float64, ESTIMATOR_COUNT> means{};
    StaticArray<float64, ESTIMATOR_COUNT> variances{};
    for (usize estimator = 0; estimator < ESTIMATOR_COUNT; ++estimator)
    {
        means[estimator] = sums[estimator] / SAMPLE_COUNT;
        variances[estimator] = squares[estimator] / SAMPLE_COUNT - means[estimator] * means[estimator];
        INFO("estimator " << estimator << " mean " << means[estimator] << " variance " << variances[estimator]);
        CHECK(means[estimator] > 0);
    }
    INFO("MIS=" << means[0] << ", geometry=" << means[1] << ", BSDF=" << means[2]);
    const float64 uncertainty = 6 * math::Sqrt((variances[1] + variances[2]) / SAMPLE_COUNT);
    CHECK(math::Abs(means[1] - means[2]) < uncertainty);
    CHECK(math::Abs(means[0] - means[1]) < uncertainty);
    CHECK(variances[1] < variances[2] * 0.25);
}

TEST_CASE("CPU geometry samples on an uncovered reflector are null events with fixed-mip material maps",
          "[renderer][one-more-vertex-nee][cpu]")
{
    SparseReflector fixture;
    constexpr usize TEXEL_BYTES = 4;
    const StaticArray<uint8, TEXEL_BYTES> color{{128, 192, 255, 0}};
    const StaticArray<uint8, TEXEL_BYTES> surface{{128, 128, 255, 255}};
    const StaticArray<uint8, TEXEL_BYTES> emission{{255, 128, 64, 255}};
    fixture.Surfaces[1].Alpha = renderer::MaterialAlpha::Masked;
    fixture.Surfaces[1].HasTexture = true;
    fixture.Surfaces[1].HasSurfaceMap = true;
    fixture.Surfaces[2].HasEmissionMap = true;
    fixture.Scene.Instances[1].Texture =
        {color.GetData(), color.GetSize(), TEXEL_BYTES, 1, 1, ludus::graphics::rhi::RasterFormat::Rgba8Unorm};
    fixture.Scene.Instances[1].SurfaceMap =
        {surface.GetData(), surface.GetSize(), TEXEL_BYTES, 1, 1, ludus::graphics::rhi::RasterFormat::Rgba8Unorm};
    fixture.Scene.Instances[2].EmissionMap =
        {emission.GetData(), emission.GetSize(), TEXEL_BYTES, 1, 1, ludus::graphics::rhi::RasterFormat::Rgba8Unorm};
    renderer::testing::OneMoreVertexOracle oracle;
    REQUIRE(oracle.Build(fixture.Scene) == true);
    ludus::graphics::rhi::RayInput ray;
    ray.Origin[2] = -1;
    ray.Direction[2] = 1;
    ray.MaxDistance = 2;
    constexpr uint32 SAMPLE_COUNT = 256;
    constexpr uint32 ESTIMATOR_COUNT = 3;
    for (uint32 sample = 0; sample < SAMPLE_COUNT; ++sample)
    {
        for (uint32 estimator = 0; estimator < ESTIMATOR_COUNT; ++estimator)
        {
            math::Vector3 emissionRadiance;
            math::Vector3 direct;
            math::Vector3 indirect;
            const math::Vector3 total =
                oracle.Evaluate(fixture.Scene, ray, {42}, 0, sample, estimator, emissionRadiance, direct, indirect);
            REQUIRE(math::IsFinite(total) == true);
            CHECK(indirect.X == 0);
            CHECK(indirect.Y == 0);
            CHECK(indirect.Z == 0);
            CHECK(direct.X > direct.Y);
            CHECK(direct.Y > direct.Z);
        }
    }
}
