#include "../src/internal/raster_mesh_light_grid.h"
#include "../src/internal/raster_mesh_lights.h"

#include <ludus/foundation/containers/static_array.hpp>
#include <ludus/foundation/math/scalar.hpp>
#include <ludus/graphics/renderer/deferred_layout.h>

#include <limits>

#include <catch2/catch_test_macros.hpp>

namespace
{
using namespace ludus::foundation;
namespace math = ludus::foundation::math;
namespace renderer = ludus::graphics::renderer;
namespace rhi = ludus::graphics::rhi;
namespace internal = renderer::internal;
constexpr usize CORNER_COUNT = 3;
constexpr uint32 EMITTER_COUNT = 400;
constexpr uint32 RECEIVER_COUNT = 300;
constexpr float32 SCENE_METERS = 20;
/// The grid bounds the Lambert term; a dielectric's F0 = 0.04 specular lobe at roughness one rides slightly above it.
constexpr float32 SPECULAR_ALLOWANCE = 1.25F;
/// Deterministic uniform values in [0, 1).
struct Sequence final
{
    uint32 State = 0x9E3779B9U;
    float32 Next() noexcept
    {
        State = State * 1664525U + 1013904223U;
        return static_cast<float32>(State >> 8U) / static_cast<float32>(1U << 24U);
    }
    math::Vector3 NextPoint() noexcept
    {
        const float32 x = Next();
        const float32 y = Next();
        const float32 z = Next();
        return math::Vector3{x, y, z} * SCENE_METERS;
    }
};
// One triangle record (deferred_layout.h): normal and sidedness, emission and corner count, bounding sphere, corners.
void AddTriangle(Array<math::Vector4>& inoutWords,
                 math::Vector3 centerWorld,
                 float32 size,
                 math::Vector3 emission,
                 bool isTwoSided)
{
    REQUIRE(inoutWords.TryAdd({0, 0, 1, isTwoSided == true ? 1.0F : 0.0F}) == true);
    REQUIRE(inoutWords.TryAdd({emission.X, emission.Y, emission.Z, static_cast<float32>(CORNER_COUNT)}) == true);
    // The corners' centroid lies size / 3 below centerWorld; the apex, 4 * size / 3 above it, is the farthest corner.
    const math::Vector3 centroidWorld = centerWorld - math::Vector3{0, size / 3, 0};
    REQUIRE(inoutWords.TryAdd({centroidWorld.X, centroidWorld.Y, centroidWorld.Z, 4 * size / 3}) == true);
    // The first corner's w holds the area: base 2 * size, height 2 * size.
    REQUIRE(inoutWords.TryAdd({centerWorld.X - size, centerWorld.Y - size, centerWorld.Z, 2 * size * size}) == true);
    REQUIRE(inoutWords.TryAdd({centerWorld.X + size, centerWorld.Y - size, centerWorld.Z, 0}) == true);
    REQUIRE(inoutWords.TryAdd({centerWorld.X, centerWorld.Y + size, centerWorld.Z, 0}) == true);
}
} // namespace

TEST_CASE("mesh-light grid cells omit only records below the cull radiance", "[mesh-area][mesh-light-grid]")
{
    Sequence sequence;
    Array<math::Vector4> words;
    for (uint32 emitter = 0; emitter < EMITTER_COUNT; ++emitter)
    {
        // Mostly dim centimetre bulbs, with a few bright half-metre panels that reach the whole scene.
        const bool isPanel = emitter % 50 == 0;
        AddTriangle(words,
                    sequence.NextPoint(),
                    isPanel == true ? .5F : .01F,
                    isPanel == true ? math::Vector3{20, 10, 5} : math::Vector3{1, 1, 1},
                    emitter % 3 == 0);
    }
    const usize recordWords = words.GetSize();
    const math::Aabb3 receiversWorld{{0, 0, 0}, {SCENE_METERS, SCENE_METERS, SCENE_METERS}};
    uint32 gridWord = 0;
    REQUIRE(internal::AppendRasterMeshLightGrid(words, EMITTER_COUNT, receiversWorld, gridWord) ==
            rhi::RasterStatus::Ready);
    REQUIRE(gridWord == recordWords);
    const BorrowedView<const math::Vector4> view{words.GetData(), words.GetSize()};
    usize listedTotal = 0;
    for (uint32 receiver = 0; receiver < RECEIVER_COUNT; ++receiver)
    {
        const math::Vector3 positionWorld = sequence.NextPoint();
        math::Vector3 normalWorld;
        REQUIRE(math::TryNormalize(sequence.NextPoint() - math::Vector3{10, 10, 10}, normalWorld) ==
                math::MathStatus::Success);
        Array<uint32> listed;
        REQUIRE(internal::GetRasterMeshLightCell(view, gridWord, positionWorld, listed) == true);
        listedTotal += listed.GetSize();
        for (usize offset = 0; offset < recordWords; offset += internal::GetRasterMeshLightRecordWords(view, offset))
        {
            bool isListed = false;
            for (const uint32 word : listed)
            {
                isListed = isListed == true || word == offset;
            }
            if (isListed == true)
            {
                continue;
            }
            const math::Vector3 omitted = internal::EvaluateRasterMeshLight(
                {words.GetData() + offset, internal::GetRasterMeshLightRecordWords(view, offset)},
                positionWorld,
                normalWorld,
                normalWorld,
                {1, 1, 1},
                0,
                1);
            CHECK(math::Max(omitted.X, math::Max(omitted.Y, omitted.Z)) <=
                  internal::MESH_LIGHT_CULL_RADIANCE * SPECULAR_ALLOWANCE);
        }
    }
    // The dim bulbs reach only their own neighbourhood, so a cell lists far fewer than every record.
    CHECK(listedTotal < usize{RECEIVER_COUNT} * EMITTER_COUNT / 4);
}

TEST_CASE("mesh-light grid positions outside the bounds read the nearest boundary cell", "[mesh-area][mesh-light-grid]")
{
    // A reconstructed floor position just below the receiver bounds must not fall back to every record. Each emitter
    // reaches about 5 m, so opposite corners of the 10 m grid list only their own.
    Array<math::Vector4> words;
    AddTriangle(words, {1, 1, 1}, .01F, {100, 100, 100}, true);
    AddTriangle(words, {9, 9, 9}, .01F, {100, 100, 100}, true);
    const uint32 secondRecordWord = static_cast<uint32>(words.GetSize() / 2);
    uint32 gridWord = 0;
    REQUIRE(internal::AppendRasterMeshLightGrid(words, 2, {{0, 0, 0}, {10, 10, 10}}, gridWord) ==
            rhi::RasterStatus::Ready);
    const BorrowedView<const math::Vector4> view{words.GetData(), words.GetSize()};
    Array<uint32> boundary;
    REQUIRE(internal::GetRasterMeshLightCell(view, gridWord, {1, 1, .05F}, boundary) == true);
    REQUIRE(boundary.GetSize() == 1);
    CHECK(boundary[0] == 0);
    for (const math::Vector3 positionWorld : StaticArray<math::Vector3, 3>{{{1, 1, -.05F}, {1, 1, -5}, {-3, 1, 1}}})
    {
        Array<uint32> outside;
        REQUIRE(internal::GetRasterMeshLightCell(view, gridWord, positionWorld, outside) == true);
        REQUIRE(outside.GetSize() == 1);
        CHECK(outside[0] == 0);
    }
    Array<uint32> beyond;
    REQUIRE(internal::GetRasterMeshLightCell(view, gridWord, {12, 9, 9}, beyond) == true);
    REQUIRE(beyond.GetSize() == 1);
    CHECK(beyond[0] == secondRecordWord);
    Array<uint32> nonfinite;
    CHECK(
        internal::GetRasterMeshLightCell(view, gridWord, {std::numeric_limits<float32>::infinity(), 1, 1}, nonfinite) ==
        false);
}

TEST_CASE("mesh-light grid lists skip cells behind one-sided emitters", "[mesh-area][mesh-light-grid]")
{
    Array<math::Vector4> words;
    AddTriangle(words, {5, 5, 5}, .5F, {100, 100, 100}, false);
    uint32 gridWord = 0;
    REQUIRE(internal::AppendRasterMeshLightGrid(words, 1, {{0, 0, 0}, {10, 10, 10}}, gridWord) ==
            rhi::RasterStatus::Ready);
    const BorrowedView<const math::Vector4> view{words.GetData(), words.GetSize()};
    Array<uint32> front;
    REQUIRE(internal::GetRasterMeshLightCell(view, gridWord, {5, 5, 8}, front) == true);
    CHECK(front.GetSize() == 1);
    Array<uint32> behind;
    REQUIRE(internal::GetRasterMeshLightCell(view, gridWord, {5, 5, 1}, behind) == true);
    CHECK(behind.IsEmpty() == true);
}

TEST_CASE("mesh-light grid is absent without records or receivers", "[mesh-area][mesh-light-grid]")
{
    Array<math::Vector4> words;
    uint32 gridWord = 7;
    REQUIRE(internal::AppendRasterMeshLightGrid(words, 0, {{0, 0, 0}, {1, 1, 1}}, gridWord) ==
            rhi::RasterStatus::Ready);
    CHECK(gridWord == 0);
    CHECK(words.IsEmpty() == true);
    AddTriangle(words, {0, 0, 0}, 1, {1, 1, 1}, true);
    gridWord = 7;
    REQUIRE(internal::AppendRasterMeshLightGrid(words, 1, math::EmptyAabb3(), gridWord) == rhi::RasterStatus::Ready);
    CHECK(gridWord == 0);
    CHECK(words.GetSize() == renderer::MESH_LIGHT_POLYGON_HEADER_WORDS + CORNER_COUNT);
}
