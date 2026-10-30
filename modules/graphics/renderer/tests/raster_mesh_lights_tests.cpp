#include "../src/internal/raster_mesh_lights.h"

#include <ludus/foundation/containers/static_array.hpp>
#include <ludus/foundation/math/scalar.hpp>
#include <ludus/graphics/renderer/deferred_layout.h>

#include <catch2/catch_test_macros.hpp>

namespace
{
using namespace ludus::foundation;
namespace math = ludus::foundation::math;
namespace renderer = ludus::graphics::renderer;
namespace rhi = ludus::graphics::rhi;
namespace internal = renderer::internal;
constexpr usize CORNER_COUNT = 3;
const StaticArray<math::Vector3, CORNER_COUNT> POSITIONS{{{-1, -1, 1}, {0, 1, 1}, {1, -1, 1}}};
const StaticArray<uint32, CORNER_COUNT> INDICES{{0, 1, 2}};
renderer::MeshDescription MakeMesh() noexcept
{
    return {POSITIONS.GetData(), POSITIONS.GetSize(), INDICES.GetData(), INDICES.GetSize()};
}
} // namespace
TEST_CASE("raster mesh emitters retain area geometry and independent placed instances", "[mesh-area]")
{
    Array<math::Vector4> words;
    renderer::SceneShaderMaterial material;
    material.Emission = {1, 2, 0};
    REQUIRE(internal::AppendRasterMeshLights(MakeMesh(), {}, material, words) == rhi::RasterStatus::Ready);
    REQUIRE(words.GetSize() == renderer::MESH_LIGHT_TRIANGLE_WORDS);
    CHECK(words[0].X == -1);
    CHECK(words[2].W == -1);
    const math::Vector3 lit = internal::EvaluateRasterMeshLight({words.GetData(), words.GetSize()},
                                                                {},
                                                                {0, 0, 1},
                                                                {0, 0, 1},
                                                                {.5F, .5F, .5F},
                                                                0,
                                                                1);
    CHECK(lit.X > 0);
    CHECK(lit.Y == lit.X * 2);
    CHECK(lit.Z == 0);
    math::Affine3 placement;
    placement.Columns[0].X = -2;
    placement.Columns[1].Y = .5F;
    placement.Translation = {1, 0, 0};
    REQUIRE(internal::AppendRasterMeshLights(MakeMesh(), placement, material, words) == rhi::RasterStatus::Ready);
    REQUIRE(words.GetSize() == usize{2} * renderer::MESH_LIGHT_TRIANGLE_WORDS);
    CHECK(words[renderer::MESH_LIGHT_TRIANGLE_WORDS].X == 3);
    // Reflection preserves the authored outward normal via inverse transpose, not winding.
    CHECK(words[renderer::MESH_LIGHT_TRIANGLE_WORDS + 2].W == -1);
}
TEST_CASE("raster emitter sidedness receiver horizon and degeneracy are explicit", "[mesh-area]")
{
    Array<math::Vector4> words;
    renderer::SceneShaderMaterial material;
    material.Emission = {1, 1, 1};
    REQUIRE(internal::AppendRasterMeshLights(MakeMesh(), {}, material, words) == rhi::RasterStatus::Ready);
    CHECK(internal::EvaluateRasterMeshLight({words.GetData(), words.GetSize()},
                                            {0, 0, 2},
                                            {0, 0, -1},
                                            {0, 0, -1},
                                            {.5F, .5F, .5F},
                                            0,
                                            1) == math::Vector3{});
    words[CORNER_COUNT].W = 1;
    CHECK(internal::EvaluateRasterMeshLight({words.GetData(), words.GetSize()},
                                            {0, 0, 2},
                                            {0, 0, -1},
                                            {0, 0, -1},
                                            {.5F, .5F, .5F},
                                            0,
                                            1)
              .X > 0);
    CHECK(internal::EvaluateRasterMeshLight({words.GetData(), words.GetSize()},
                                            {},
                                            {0, 0, -1},
                                            {0, 0, -1},
                                            {.5F, .5F, .5F},
                                            0,
                                            .1F) == math::Vector3{});
    const StaticArray<math::Vector3, CORNER_COUNT> collapsed{{{0, 0, 1}, {0, 0, 1}, {0, 0, 1}}};
    renderer::MeshDescription mesh = MakeMesh();
    mesh.Positions = collapsed.GetData();
    REQUIRE(internal::AppendRasterMeshLights(mesh, {}, material, words) == rhi::RasterStatus::Ready);
    CHECK(words.GetSize() == renderer::MESH_LIGHT_TRIANGLE_WORDS);
    const StaticArray<uint32, CORNER_COUNT> invalid{{0, 1, 100}};
    mesh.Indices = invalid.GetData();
    CHECK(internal::AppendRasterMeshLights(mesh, {}, material, words) == rhi::RasterStatus::InvalidDescription);
    CHECK(words.GetSize() == renderer::MESH_LIGHT_TRIANGLE_WORDS);
}
TEST_CASE("textured raster emitters use decoded fixed-mip patch radiance and bounded admission", "[mesh-area]")
{
    constexpr usize TEXEL_BYTES = 4;
    const StaticArray<uint8, TEXEL_BYTES> texel{{128, 0, 255, 255}};
    renderer::SceneShaderMaterial material;
    material.Emission = {2, 2, 2};
    material.EmissionTexture = {texel.GetData(), texel.GetSize(), texel.GetSize(), 1, 1, rhi::RasterFormat::Rgba8Srgb};
    Array<math::Vector4> words;
    REQUIRE(internal::AppendRasterMeshLights(MakeMesh(), {}, material, words) == rhi::RasterStatus::Ready);
    REQUIRE(words.GetSize() == TEXEL_BYTES * renderer::MESH_LIGHT_TRIANGLE_WORDS);
    CHECK(math::Abs(words[CORNER_COUNT].X - 2 * math::DecodeSrgb(128.0F / 255)) < 1e-6F);
    CHECK(words[CORNER_COUNT].Y == 0);
    CHECK(words[CORNER_COUNT].Z == 2);
    math::Vector3 patchSum{};
    for (usize patch = 0; patch < words.GetSize(); patch += renderer::MESH_LIGHT_TRIANGLE_WORDS)
    {
        patchSum =
            patchSum + internal::EvaluateRasterMeshLight({words.GetData() + patch, renderer::MESH_LIGHT_TRIANGLE_WORDS},
                                                         {},
                                                         {0, 0, 1},
                                                         {0, 0, 1},
                                                         {.5F, .5F, .5F},
                                                         0,
                                                         1);
    }
    renderer::SceneShaderMaterial constantMaterial = material;
    constantMaterial.EmissionTexture = {};
    constantMaterial.Emission = {words[CORNER_COUNT].X, words[CORNER_COUNT].Y, words[CORNER_COUNT].Z};
    Array<math::Vector4> whole;
    REQUIRE(internal::AppendRasterMeshLights(MakeMesh(), {}, constantMaterial, whole) == rhi::RasterStatus::Ready);
    const math::Vector3 wholeResult = internal::EvaluateRasterMeshLight({whole.GetData(), whole.GetSize()},
                                                                        {},
                                                                        {0, 0, 1},
                                                                        {0, 0, 1},
                                                                        {.5F, .5F, .5F},
                                                                        0,
                                                                        1);
    CHECK(math::Length(patchSum - wholeResult) < 1e-5F);
    REQUIRE(words.TryResize(usize{renderer::MAX_RASTER_MESH_LIGHT_TRIANGLES} * renderer::MESH_LIGHT_TRIANGLE_WORDS) ==
            true);
    CHECK(internal::AppendRasterMeshLights(MakeMesh(), {}, material, words) == rhi::RasterStatus::CapacityExceeded);
    CHECK(words.GetSize() == usize{renderer::MAX_RASTER_MESH_LIGHT_TRIANGLES} * renderer::MESH_LIGHT_TRIANGLE_WORDS);
}
