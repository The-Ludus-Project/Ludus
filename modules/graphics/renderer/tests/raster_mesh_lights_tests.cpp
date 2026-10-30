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
constexpr usize HEADER_WORDS = renderer::MESH_LIGHT_POLYGON_HEADER_WORDS;
constexpr usize TRIANGLE_RECORD_WORDS = HEADER_WORDS + CORNER_COUNT;
const StaticArray<math::Vector3, CORNER_COUNT> POSITIONS{{{-1, -1, 1}, {0, 1, 1}, {1, -1, 1}}};
const StaticArray<uint32, CORNER_COUNT> INDICES{{0, 1, 2}};
renderer::MeshDescription MakeMesh() noexcept
{
    return {POSITIONS.GetData(), POSITIONS.GetSize(), INDICES.GetData(), INDICES.GetSize()};
}
usize CountRecords(const Array<math::Vector4>& words)
{
    usize count = 0;
    for (usize offset = 0; offset < words.GetSize(); ++count)
    {
        const usize recordWords = internal::GetRasterMeshLightRecordWords({words.GetData(), words.GetSize()}, offset);
        REQUIRE(recordWords != 0);
        offset += recordWords;
    }
    return count;
}
// The production entry point, with the record count of the words already present.
rhi::RasterStatus AppendLights(const renderer::MeshDescription& mesh,
                               const math::Affine3& meshToWorld,
                               const renderer::SceneShaderMaterial& material,
                               Array<math::Vector4>& inoutWords)
{
    uint32 recordCount = static_cast<uint32>(CountRecords(inoutWords));
    const uint32 before = recordCount;
    const rhi::RasterStatus status =
        internal::AppendRasterMeshLights(mesh, meshToWorld, material, inoutWords, recordCount);
    CHECK(static_cast<usize>(recordCount) == CountRecords(inoutWords));
    CHECK((status == rhi::RasterStatus::Ready || recordCount == before) == true);
    return status;
}
} // namespace
TEST_CASE("raster mesh emitters retain area geometry and independent placed instances", "[mesh-area]")
{
    Array<math::Vector4> words;
    renderer::SceneShaderMaterial material;
    material.Emission = {1, 2, 0};
    REQUIRE(AppendLights(MakeMesh(), {}, material, words) == rhi::RasterStatus::Ready);
    REQUIRE(words.GetSize() == TRIANGLE_RECORD_WORDS);
    CHECK(words[HEADER_WORDS].X == -1);
    CHECK(words[0].Z == -1);
    CHECK(words[1].W == static_cast<float32>(CORNER_COUNT));
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
    REQUIRE(AppendLights(MakeMesh(), placement, material, words) == rhi::RasterStatus::Ready);
    REQUIRE(words.GetSize() == 2 * TRIANGLE_RECORD_WORDS);
    CHECK(words[TRIANGLE_RECORD_WORDS + HEADER_WORDS].X == 3);
    // Reflection preserves the authored outward normal via inverse transpose, not winding.
    CHECK(words[TRIANGLE_RECORD_WORDS].Z == -1);
}
TEST_CASE("masked raster emitters apply texture and vertex coverage to patch radiance", "[mesh-area][opacity]")
{
    constexpr usize TEXEL_BYTES = 4;
    StaticArray<uint8, TEXEL_BYTES> texel{{255, 255, 255, 0}};
    renderer::SceneShaderMaterial material;
    material.Emission = {1, 2, 0};
    material.IsAlphaMasked = true;
    material.AlphaCutoff = .5F;
    material.ColorTexture = {texel.GetData(), texel.GetSize(), texel.GetSize(), 1, 1, rhi::RasterFormat::Rgba8Srgb};
    Array<math::Vector4> words;
    REQUIRE(AppendLights(MakeMesh(), {}, material, words) == rhi::RasterStatus::Ready);
    CHECK(words.IsEmpty() == true);
    texel[3] = 255;
    REQUIRE(AppendLights(MakeMesh(), {}, material, words) == rhi::RasterStatus::Ready);
    REQUIRE(CountRecords(words) == 4);
    const math::Vector3 receiverNormalWorld{0, 0, 1};
    const math::Vector3 baseColor{.5F, .5F, .5F};
    math::Vector3 actual{};
    for (usize offset = 0; offset < words.GetSize(); offset += TRIANGLE_RECORD_WORDS)
    {
        actual = actual + internal::EvaluateRasterMeshLight({words.GetData() + offset, TRIANGLE_RECORD_WORDS},
                                                            {},
                                                            receiverNormalWorld,
                                                            receiverNormalWorld,
                                                            baseColor,
                                                            0,
                                                            1);
    }
    material.IsAlphaMasked = false;
    Array<math::Vector4> whole;
    REQUIRE(AppendLights(MakeMesh(), {}, material, whole) == rhi::RasterStatus::Ready);
    const math::Vector3 expected = internal::EvaluateRasterMeshLight({whole.GetData(), whole.GetSize()},
                                                                     {},
                                                                     receiverNormalWorld,
                                                                     receiverNormalWorld,
                                                                     baseColor,
                                                                     0,
                                                                     1);
    CHECK(math::Length(actual - expected) < 1e-5F);
    words.Clear();
    material.IsAlphaMasked = true;
    renderer::MeshDescription mesh = MakeMesh();
    const StaticArray<math::Vector4, CORNER_COUNT> colors{{{1, 1, 1, .25F}, {1, 1, 1, .25F}, {1, 1, 1, .25F}}};
    mesh.Colors = {colors.GetData(), colors.GetSize()};
    REQUIRE(AppendLights(mesh, {}, material, words) == rhi::RasterStatus::Ready);
    CHECK(words.IsEmpty() == true);
}

TEST_CASE("raster emitter sidedness receiver horizon and degeneracy are explicit", "[mesh-area]")
{
    Array<math::Vector4> words;
    renderer::SceneShaderMaterial material;
    material.Emission = {1, 1, 1};
    REQUIRE(AppendLights(MakeMesh(), {}, material, words) == rhi::RasterStatus::Ready);
    CHECK(internal::EvaluateRasterMeshLight({words.GetData(), words.GetSize()},
                                            {0, 0, 2},
                                            {0, 0, -1},
                                            {0, 0, -1},
                                            {.5F, .5F, .5F},
                                            0,
                                            1) == math::Vector3{});
    words[0].W = 1;
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
    REQUIRE(AppendLights(mesh, {}, material, words) == rhi::RasterStatus::Ready);
    CHECK(words.GetSize() == TRIANGLE_RECORD_WORDS);
    const StaticArray<uint32, CORNER_COUNT> invalid{{0, 1, 100}};
    mesh.Indices = invalid.GetData();
    CHECK(AppendLights(mesh, {}, material, words) == rhi::RasterStatus::InvalidDescription);
    CHECK(words.GetSize() == TRIANGLE_RECORD_WORDS);
}
TEST_CASE("textured raster emitters use decoded fixed-mip patch radiance and bounded admission", "[mesh-area]")
{
    constexpr usize TEXEL_BYTES = 4;
    const StaticArray<uint8, TEXEL_BYTES> texel{{128, 0, 255, 255}};
    renderer::SceneShaderMaterial material;
    material.Emission = {2, 2, 2};
    material.EmissionTexture = {texel.GetData(), texel.GetSize(), texel.GetSize(), 1, 1, rhi::RasterFormat::Rgba8Srgb};
    Array<math::Vector4> words;
    REQUIRE(AppendLights(MakeMesh(), {}, material, words) == rhi::RasterStatus::Ready);
    REQUIRE(words.GetSize() == TEXEL_BYTES * TRIANGLE_RECORD_WORDS);
    CHECK(math::Abs(words[1].X - 2 * math::DecodeSrgb(128.0F / 255)) < 1e-6F);
    CHECK(words[1].Y == 0);
    CHECK(words[1].Z == 2);
    math::Vector3 patchSum{};
    for (usize patch = 0; patch < words.GetSize(); patch += TRIANGLE_RECORD_WORDS)
    {
        patchSum = patchSum + internal::EvaluateRasterMeshLight({words.GetData() + patch, TRIANGLE_RECORD_WORDS},
                                                                {},
                                                                {0, 0, 1},
                                                                {0, 0, 1},
                                                                {.5F, .5F, .5F},
                                                                0,
                                                                1);
    }
    renderer::SceneShaderMaterial constantMaterial = material;
    constantMaterial.EmissionTexture = {};
    constantMaterial.Emission = {words[1].X, words[1].Y, words[1].Z};
    Array<math::Vector4> whole;
    REQUIRE(AppendLights(MakeMesh(), {}, constantMaterial, whole) == rhi::RasterStatus::Ready);
    const math::Vector3 wholeResult = internal::EvaluateRasterMeshLight({whole.GetData(), whole.GetSize()},
                                                                        {},
                                                                        {0, 0, 1},
                                                                        {0, 0, 1},
                                                                        {.5F, .5F, .5F},
                                                                        0,
                                                                        1);
    CHECK(math::Length(patchSum - wholeResult) < 1e-5F);
    // Admission counts records, not words; a full set is rejected with both outputs preserved.
    uint32 recordCount = renderer::MAX_RASTER_MESH_LIGHT_POLYGONS;
    const usize wordCount = words.GetSize();
    CHECK(internal::AppendRasterMeshLights(MakeMesh(), {}, material, words, recordCount) ==
          rhi::RasterStatus::CapacityExceeded);
    CHECK(words.GetSize() == wordCount);
    CHECK(recordCount == renderer::MAX_RASTER_MESH_LIGHT_POLYGONS);
}
namespace
{
// A Staircase2 small square light: a 15 cm square tessellated as a 17 x 17 vertex grid, facing -z at z = 1.
constexpr usize GRID_VERTICES_PER_SIDE = 17;
constexpr float32 GRID_HALF_SIDE_METERS = .075F;
// The pbrt disk fixture of scene_cook: a center vertex and 64 rim vertices, facing -z at z = 1.
constexpr usize DISK_SEGMENTS = 64;
constexpr float32 DISK_RADIUS_METERS = .1F;
// Float32 sums of hundreds of per-triangle integrals against one polygon integral.
constexpr float32 MERGE_RELATIVE_TOLERANCE = 1e-4F;

struct TestMesh final
{
    Array<math::Vector3> Positions;
    Array<uint32> Indices;
    [[nodiscard]] renderer::MeshDescription GetDescription() const noexcept
    {
        return {Positions.GetData(), Positions.GetSize(), Indices.GetData(), Indices.GetSize()};
    }
};
struct Receiver final
{
    math::Vector3 PositionWorld{};
    math::Vector3 NormalWorld{};
    float32 Metallic = 0;
    float32 Roughness = 1;
};
void AddTriangle(TestMesh& inoutMesh, uint32 a, uint32 b, uint32 c)
{
    REQUIRE(inoutMesh.Indices.TryAdd(a) == true);
    REQUIRE(inoutMesh.Indices.TryAdd(b) == true);
    REQUIRE(inoutMesh.Indices.TryAdd(c) == true);
}
TestMesh MakeGrid()
{
    TestMesh mesh;
    const float32 step = 2 * GRID_HALF_SIDE_METERS / static_cast<float32>(GRID_VERTICES_PER_SIDE - 1);
    for (usize j = 0; j < GRID_VERTICES_PER_SIDE; ++j)
    {
        for (usize i = 0; i < GRID_VERTICES_PER_SIDE; ++i)
        {
            REQUIRE(mesh.Positions.TryAdd({-GRID_HALF_SIDE_METERS + static_cast<float32>(i) * step,
                                           -GRID_HALF_SIDE_METERS + static_cast<float32>(j) * step,
                                           1}) == true);
        }
    }
    for (usize j = 0; j + 1 < GRID_VERTICES_PER_SIDE; ++j)
    {
        for (usize i = 0; i + 1 < GRID_VERTICES_PER_SIDE; ++i)
        {
            const uint32 v00 = static_cast<uint32>(j * GRID_VERTICES_PER_SIDE + i);
            const uint32 v10 = v00 + 1;
            const uint32 v01 = v00 + static_cast<uint32>(GRID_VERTICES_PER_SIDE);
            const uint32 v11 = v01 + 1;
            AddTriangle(mesh, v00, v11, v10);
            AddTriangle(mesh, v00, v01, v11);
        }
    }
    return mesh;
}
TestMesh MakeDisk(usize segmentCount)
{
    TestMesh mesh;
    REQUIRE(mesh.Positions.TryAdd({0, 0, 1}) == true);
    for (usize i = 0; i < segmentCount; ++i)
    {
        const float32 angle = static_cast<float32>(i) * 2 * math::kPiF / static_cast<float32>(segmentCount);
        REQUIRE(mesh.Positions.TryAdd(
                    {DISK_RADIUS_METERS * math::Cos(angle), DISK_RADIUS_METERS * math::Sin(angle), 1}) == true);
    }
    for (usize i = 0; i < segmentCount; ++i)
    {
        AddTriangle(mesh, 0, static_cast<uint32>(1 + (i + 1) % segmentCount), static_cast<uint32>(1 + i));
    }
    return mesh;
}
// Unit-square cells (column, row) facing -z at z = 1, two triangles each.
TestMesh MakeCells(BorrowedView<const math::Vector2> cells)
{
    TestMesh mesh;
    for (usize cell = 0; cell < cells.GetSize(); ++cell)
    {
        const math::Vector2 corner = cells[cell];
        const uint32 first = static_cast<uint32>(mesh.Positions.GetSize());
        REQUIRE(mesh.Positions.TryAdd({corner.X, corner.Y, 1}) == true);
        REQUIRE(mesh.Positions.TryAdd({corner.X + 1, corner.Y, 1}) == true);
        REQUIRE(mesh.Positions.TryAdd({corner.X, corner.Y + 1, 1}) == true);
        REQUIRE(mesh.Positions.TryAdd({corner.X + 1, corner.Y + 1, 1}) == true);
        AddTriangle(mesh, first, first + 3, first + 1);
        AddTriangle(mesh, first, first + 2, first + 3);
    }
    return mesh;
}
math::Vector3 EvaluateRecords(const Array<math::Vector4>& words, const Receiver& receiver) noexcept
{
    math::Vector3 result{};
    for (usize record = 0; record < words.GetSize();)
    {
        const usize recordWords = internal::GetRasterMeshLightRecordWords({words.GetData(), words.GetSize()}, record);
        REQUIRE(recordWords != 0);
        const usize offset = record;
        record += recordWords;
        result = result + internal::EvaluateRasterMeshLight({words.GetData() + offset, recordWords},
                                                            receiver.PositionWorld,
                                                            receiver.NormalWorld,
                                                            receiver.NormalWorld,
                                                            {.5F, .5F, .5F},
                                                            receiver.Metallic,
                                                            receiver.Roughness);
    }
    return result;
}
// Each triangle appended on its own stays a three-corner record: the unmerged reference.
Array<math::Vector4> AppendTriangleRecords(const TestMesh& mesh,
                                           const math::Affine3& meshToWorld,
                                           const renderer::SceneShaderMaterial& material)
{
    Array<math::Vector4> words;
    for (usize triangle = 0; triangle < mesh.Indices.GetSize(); triangle += CORNER_COUNT)
    {
        renderer::MeshDescription description = mesh.GetDescription();
        description.Indices = mesh.Indices.GetData() + triangle;
        description.IndexCount = CORNER_COUNT;
        REQUIRE(AppendLights(description, meshToWorld, material, words) == rhi::RasterStatus::Ready);
    }
    return words;
}
void CheckMergedIntegral(const TestMesh& mesh, const math::Affine3& meshToWorld)
{
    renderer::SceneShaderMaterial material;
    material.Emission = {1, 2, 3};
    Array<math::Vector4> merged;
    REQUIRE(AppendLights(mesh.GetDescription(), meshToWorld, material, merged) == rhi::RasterStatus::Ready);
    const Array<math::Vector4> triangles = AppendTriangleRecords(mesh, meshToWorld, material);
    REQUIRE(CountRecords(triangles) == mesh.Indices.GetSize() / CORNER_COUNT);
    math::Vector3 tiltedNormal;
    REQUIRE(math::TryNormalize({1, .3F, .2F}, tiltedNormal) == math::MathStatus::Success);
    // Below the light, then a tilted receiver whose horizon cuts through it, rough dielectric and glossy metal.
    const StaticArray<Receiver, 4> receivers{{
        { .PositionWorld = {.02F, -.01F, .4F}, .NormalWorld = {0, 0, 1}, .Metallic = 0, .Roughness = 1 },
        { .PositionWorld = {.02F, -.01F, .4F}, .NormalWorld = {0, 0, 1}, .Metallic = 1, .Roughness = .2F },
        { .PositionWorld = {0, 0, .9F}, .NormalWorld = tiltedNormal, .Metallic = 0, .Roughness = .7F },
        { .PositionWorld = {0, 0, .9F}, .NormalWorld = tiltedNormal, .Metallic = 1, .Roughness = .3F },
    }};
    for (usize receiver = 0; receiver < receivers.GetSize(); ++receiver)
    {
        Receiver placed = receivers[receiver];
        placed.PositionWorld = math::TransformPoint(meshToWorld, placed.PositionWorld);
        math::Matrix3 normalMatrix;
        REQUIRE(math::TryNormalMatrix(meshToWorld, {}, normalMatrix) == math::MathStatus::Success);
        REQUIRE(math::TryNormalize(normalMatrix * placed.NormalWorld, placed.NormalWorld) == math::MathStatus::Success);
        const math::Vector3 expected = EvaluateRecords(triangles, placed);
        const math::Vector3 actual = EvaluateRecords(merged, placed);
        CAPTURE(receiver, expected.X, actual.X, expected.Z, actual.Z);
        REQUIRE(expected.X > 0);
        CHECK(math::Abs(actual.X - expected.X) <= MERGE_RELATIVE_TOLERANCE * expected.X);
        CHECK(math::Abs(actual.Y - expected.Y) <= MERGE_RELATIVE_TOLERANCE * expected.Y);
        CHECK(math::Abs(actual.Z - expected.Z) <= MERGE_RELATIVE_TOLERANCE * expected.Z);
    }
}
} // namespace
TEST_CASE("coplanar constant-emission grids merge into one quad with the same receiver integral", "[mesh-area]")
{
    const TestMesh grid = MakeGrid();
    renderer::SceneShaderMaterial material;
    material.Emission = {1, 1, 1};
    Array<math::Vector4> words;
    REQUIRE(AppendLights(grid.GetDescription(), {}, material, words) == rhi::RasterStatus::Ready);
    REQUIRE(CountRecords(words) == 1);
    CHECK(words[1].W == 4.0F);
    CHECK(words[0].Z == -1);
    for (usize corner = 0; corner < 4; ++corner)
    {
        CHECK(math::Abs(words[HEADER_WORDS + corner].X) == GRID_HALF_SIDE_METERS);
        CHECK(math::Abs(words[HEADER_WORDS + corner].Y) == GRID_HALF_SIDE_METERS);
    }
    CheckMergedIntegral(grid, {});
    // A reflected, scaled and translated placement merges in mesh space and keeps the authored normal.
    math::Affine3 placement;
    placement.Columns[0].X = -2;
    placement.Columns[1].Y = .5F;
    placement.Translation = {1, 3, -2};
    words.Clear();
    REQUIRE(AppendLights(grid.GetDescription(), placement, material, words) == rhi::RasterStatus::Ready);
    REQUIRE(CountRecords(words) == 1);
    CHECK(words[0].Z == -1);
    CheckMergedIntegral(grid, placement);
}
TEST_CASE("fanned disks become one polygon record up to the corner limit", "[mesh-area]")
{
    renderer::SceneShaderMaterial material;
    material.Emission = {1, 1, 1};
    // A Staircase2 disk is one 64-corner record; the center vertex is interior and gone.
    const TestMesh disk = MakeDisk(DISK_SEGMENTS);
    Array<math::Vector4> words;
    REQUIRE(AppendLights(disk.GetDescription(), {}, material, words) == rhi::RasterStatus::Ready);
    REQUIRE(CountRecords(words) == 1);
    CHECK(words[1].W == static_cast<float32>(DISK_SEGMENTS));
    CheckMergedIntegral(disk, {});
    // A finer outline fans into records of at most MAX_MESH_LIGHT_POLYGON_CORNERS corners sharing chords.
    constexpr usize FINE_SEGMENTS = usize{2} * renderer::MAX_MESH_LIGHT_POLYGON_CORNERS;
    constexpr usize RIM_CORNERS_PER_RECORD = renderer::MAX_MESH_LIGHT_POLYGON_CORNERS - 2;
    const TestMesh fine = MakeDisk(FINE_SEGMENTS);
    words.Clear();
    REQUIRE(AppendLights(fine.GetDescription(), {}, material, words) == rhi::RasterStatus::Ready);
    const usize recordCount = CountRecords(words);
    CHECK(recordCount == (FINE_SEGMENTS - 2 + RIM_CORNERS_PER_RECORD - 1) / RIM_CORNERS_PER_RECORD);
    usize cornerSum = 0;
    for (usize record = 0; record < words.GetSize(); record += HEADER_WORDS + static_cast<usize>(words[record + 1].W))
    {
        const float32 cornerCount = words[record + 1].W;
        CHECK(cornerCount >= static_cast<float32>(CORNER_COUNT));
        CHECK(cornerCount <= static_cast<float32>(renderer::MAX_MESH_LIGHT_POLYGON_CORNERS));
        cornerSum += static_cast<usize>(cornerCount);
    }
    CHECK(cornerSum == FINE_SEGMENTS + 2 * (recordCount - 1));
    CheckMergedIntegral(fine, {});
}
TEST_CASE("non-convex, folded, coincident and textured emitters stay triangles", "[mesh-area]")
{
    renderer::SceneShaderMaterial material;
    material.Emission = {1, 1, 1};
    // Two cells side by side merge into one rectangle; an L of three cells has a reflex corner.
    const StaticArray<math::Vector2, 2> pairCells{{{0, 0}, {1, 0}}};
    const StaticArray<math::Vector2, 3> lCells{{{0, 0}, {1, 0}, {0, 1}}};
    for (const bool isLShape : {false, true})
    {
        const TestMesh cells = isLShape == true ? MakeCells({lCells.GetData(), lCells.GetSize()})
                                                : MakeCells({pairCells.GetData(), pairCells.GetSize()});
        Array<math::Vector4> words;
        REQUIRE(AppendLights(cells.GetDescription(), {}, material, words) == rhi::RasterStatus::Ready);
        CHECK(CountRecords(words) == (isLShape == true ? cells.Indices.GetSize() / CORNER_COUNT : 1));
    }
    // A 90-degree fold across a shared edge is not coplanar.
    TestMesh folded;
    for (const math::Vector3 position :
         {math::Vector3{0, 0, 1}, math::Vector3{1, 0, 1}, math::Vector3{0, 1, 1}, math::Vector3{0, 0, 2}})
    {
        REQUIRE(folded.Positions.TryAdd(position) == true);
    }
    AddTriangle(folded, 0, 2, 1);
    AddTriangle(folded, 0, 1, 3);
    Array<math::Vector4> words;
    REQUIRE(AppendLights(folded.GetDescription(), {}, material, words) == rhi::RasterStatus::Ready);
    CHECK(CountRecords(words) == 2);
    // Coincident copies double the emitted radiance and must not cancel into a merged outline.
    TestMesh coincident = MakeCells({pairCells.GetData(), 1});
    const StaticArray<uint32, 2 * CORNER_COUNT> copy{{coincident.Indices[0],
                                                      coincident.Indices[1],
                                                      coincident.Indices[2],
                                                      coincident.Indices[3],
                                                      coincident.Indices[4],
                                                      coincident.Indices[5]}};
    REQUIRE(coincident.Indices.TryAddRange({copy.GetData(), copy.GetSize()}) == true);
    words.Clear();
    REQUIRE(AppendLights(coincident.GetDescription(), {}, material, words) == rhi::RasterStatus::Ready);
    CHECK(CountRecords(words) == 4);
    // A back-to-back copy faces the other way: each side merges on its own and keeps its sidedness.
    TestMesh doubleSided = MakeCells({pairCells.GetData(), 1});
    for (usize triangle = 0; triangle < 2; ++triangle)
    {
        AddTriangle(doubleSided,
                    doubleSided.Indices[triangle * CORNER_COUNT],
                    doubleSided.Indices[triangle * CORNER_COUNT + 2],
                    doubleSided.Indices[triangle * CORNER_COUNT + 1]);
    }
    words.Clear();
    REQUIRE(AppendLights(doubleSided.GetDescription(), {}, material, words) == rhi::RasterStatus::Ready);
    REQUIRE(CountRecords(words) == 2);
    CHECK(words[0].Z == -1);
    CHECK(words[HEADER_WORDS + 4].Z == 1);
    // Emission maps keep per-triangle patches.
    constexpr usize TEXEL_BYTES = 4;
    const StaticArray<uint8, TEXEL_BYTES> texel{{255, 255, 255, 255}};
    material.EmissionTexture = {texel.GetData(), texel.GetSize(), texel.GetSize(), 1, 1, rhi::RasterFormat::Rgba8Srgb};
    const TestMesh pair = MakeCells({pairCells.GetData(), pairCells.GetSize()});
    words.Clear();
    REQUIRE(AppendLights(pair.GetDescription(), {}, material, words) == rhi::RasterStatus::Ready);
    CHECK(CountRecords(words) == pair.Indices.GetSize() / CORNER_COUNT * TEXEL_BYTES);
}
