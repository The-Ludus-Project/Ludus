#include "../src/internal/raster_mesh_light_grid.h"
#include "../src/internal/raster_mesh_light_shadows.h"
#include "../src/internal/raster_mesh_lights.h"

#include <ludus/foundation/containers/static_array.hpp>
#include <ludus/foundation/math/geometry.hpp>
#include <ludus/foundation/math/matrix.hpp>
#include <ludus/foundation/math/scalar.hpp>
#include <ludus/graphics/renderer/deferred_layout.h>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

namespace
{
using namespace ludus::foundation;
namespace math = ludus::foundation::math;
namespace renderer = ludus::graphics::renderer;
namespace rhi = ludus::graphics::rhi;
namespace internal = renderer::internal;
constexpr usize CORNER_COUNT = 3;
constexpr uint32 CASTER_CORNER = renderer::MESH_LIGHT_POLYGON_HEADER_WORDS + renderer::MESH_LIGHT_SHADOW_CASTER_CORNER;
/// Distance between emitters placed far enough apart that they never share a caster, in metres.
constexpr float32 SEPARATION_METERS = 4;
/// Empty receiver bounds, which leave each caster's range at its members' culling reach.
constexpr math::Aabb3 NO_RECEIVER_BOUNDS = math::EmptyAabb3();
/// The axis of each cube face: +X, -X, +Y, -Y, +Z, -Z.
constexpr StaticArray<math::Vector3, renderer::MESH_LIGHT_SHADOW_FACE_COUNT> FACE_FORWARD_WORLD{
    {{1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1}}};
/// Deterministic uniform values in [0, 1).
struct Sequence final
{
    uint32 State = 0x12345678U;
    float32 Next() noexcept
    {
        State = State * 1664525U + 1013904223U;
        return static_cast<float32>(State >> 8U) / static_cast<float32>(1U << 24U);
    }
};
/// One triangle record facing normalWorld, centred near centerWorld, with its bound word (deferred_layout.h).
void AddTriangle(Array<math::Vector4>& inoutWords,
                 math::Vector3 centerWorld,
                 float32 size,
                 math::Vector3 normalWorld,
                 math::Vector3 emission,
                 bool isTwoSided)
{
    // Two in-plane axes perpendicular to the normal.
    const math::Vector3 helperWorld = math::Abs(normalWorld.Z) < .9F ? math::Vector3{0, 0, 1} : math::Vector3{1, 0, 0};
    math::Vector3 tangentWorld;
    REQUIRE(math::TryNormalize(math::Cross(helperWorld, normalWorld), tangentWorld) == math::MathStatus::Success);
    const math::Vector3 bitangentWorld = math::Cross(normalWorld, tangentWorld);
    StaticArray<math::Vector3, CORNER_COUNT> cornersWorld{{centerWorld - tangentWorld * size - bitangentWorld * size,
                                                           centerWorld + tangentWorld * size - bitangentWorld * size,
                                                           centerWorld + bitangentWorld * size}};
    // Counter-clockwise about the normal.
    if (math::Dot(math::Cross(cornersWorld[1] - cornersWorld[0], cornersWorld[2] - cornersWorld[0]), normalWorld) < 0)
    {
        const math::Vector3 first = cornersWorld[1];
        cornersWorld[1] = cornersWorld[2];
        cornersWorld[2] = first;
    }
    const math::Vector3 centroidWorld = (cornersWorld[0] + cornersWorld[1] + cornersWorld[2]) * (1.0F / CORNER_COUNT);
    float32 radius = 0;
    for (const math::Vector3& cornerWorld : cornersWorld)
    {
        radius = math::Max(radius, math::Length(cornerWorld - centroidWorld));
    }
    const float32 area =
        math::Length(math::Cross(cornersWorld[1] - cornersWorld[0], cornersWorld[2] - cornersWorld[0])) * 0.5F;
    REQUIRE(inoutWords.TryAdd({normalWorld.X, normalWorld.Y, normalWorld.Z, isTwoSided == true ? 1.0F : 0.0F}) == true);
    REQUIRE(inoutWords.TryAdd({emission.X, emission.Y, emission.Z, static_cast<float32>(CORNER_COUNT)}) == true);
    REQUIRE(inoutWords.TryAdd({centroidWorld.X, centroidWorld.Y, centroidWorld.Z, radius}) == true);
    for (usize corner = 0; corner < CORNER_COUNT; ++corner)
    {
        REQUIRE(
            inoutWords.TryAdd(
                {cornersWorld[corner].X, cornersWorld[corner].Y, cornersWorld[corner].Z, corner == 0 ? area : 0.0F}) ==
            true);
    }
}
/// The caster key (caster plus one) of the record-th triangle in a list of triangle records.
uint32 GetCasterKey(const Array<math::Vector4>& words, usize record)
{
    return static_cast<uint32>(
        words[record * (renderer::MESH_LIGHT_POLYGON_HEADER_WORDS + CORNER_COUNT) + CASTER_CORNER].W);
}
} // namespace

TEST_CASE("mesh-light shadow casters merge nearby emitters and separate distant ones",
          "[mesh-area][mesh-light-shadows]")
{
    Array<math::Vector4> words;
    // A small panel of three adjacent triangles facing down, and a bulb far away.
    for (usize piece = 0; piece < 3; ++piece)
    {
        AddTriangle(words, {.1F * static_cast<float32>(piece), 3, 0}, .05F, {0, -1, 0}, {5, 5, 5}, false);
    }
    AddTriangle(words, {SEPARATION_METERS, 3, 0}, .05F, {0, -1, 0}, {1, 1, 1}, false);
    Array<internal::MeshLightShadowCaster> casters;
    internal::MeshLightShadowReport report;
    REQUIRE(internal::BuildRasterMeshLightShadowCasters(words, 4, NO_RECEIVER_BOUNDS, casters, report) ==
            rhi::RasterStatus::Ready);
    REQUIRE(casters.GetSize() == 2);
    CHECK(report.Casters == 2);
    CHECK(report.ShadowedRecords == 4);
    CHECK(report.UnshadowedRecords == 0);
    // Brightest first: the panel is caster 0.
    CHECK(GetCasterKey(words, 0) == 1);
    CHECK(GetCasterKey(words, 1) == 1);
    CHECK(GetCasterKey(words, 2) == 1);
    CHECK(GetCasterKey(words, 3) == 2);
    const internal::MeshLightShadowCaster& panel = casters[0];
    // One-sided: rendered from below the downward-facing panel, with its +Y face (behind it) dropped.
    CHECK(panel.PositionWorld.Y < 3);
    CHECK((panel.FaceMask & (1U << 2)) == 0);
    CHECK((panel.FaceMask & (1U << 3)) != 0);
    CHECK(report.Faces == 10);
    // One-sided: its emitters lie behind it, so the near distance is half its offset in front of them, whatever the
    // range: faces store linear depth, whose precision does not depend on the near distance.
    CHECK(panel.NearMeters == Catch::Approx(.5F * (3 - panel.PositionWorld.Y)).margin(1e-6F));
    CHECK(panel.RangeMeters > panel.NearMeters);
    // The range covers every receiver the grid would list for a member.
    math::Vector3 reachCenterWorld;
    float32 reachRadius = 0;
    REQUIRE(internal::GetRasterMeshLightReach({words.GetData(), words.GetSize()}, 0, reachCenterWorld, reachRadius) ==
            true);
    CHECK(panel.RangeMeters >= math::Length(reachCenterWorld - panel.PositionWorld) + reachRadius - 1e-4F);
}

TEST_CASE("mesh-light shadow casters carry the light disk whose penumbra the resolve estimates",
          "[mesh-area][mesh-light-shadows]")
{
    constexpr float32 PANEL_SIZE_METERS = .2F;
    constexpr float32 PAIR_SEPARATION_METERS = .4F;
    constexpr float32 SMALL_SIZE_METERS = .02F;
    Array<math::Vector4> words;
    // A downward panel, a pair of small downward triangles within one caster's radius, and a two-sided emitter.
    AddTriangle(words, {0, 3, 0}, PANEL_SIZE_METERS, {0, -1, 0}, {4, 4, 4}, false);
    AddTriangle(words, {SEPARATION_METERS, 3, 0}, SMALL_SIZE_METERS, {0, -1, 0}, {2, 2, 2}, false);
    AddTriangle(words,
                {SEPARATION_METERS + PAIR_SEPARATION_METERS, 3, 0},
                SMALL_SIZE_METERS,
                {0, -1, 0},
                {2, 2, 2},
                false);
    AddTriangle(words, {2 * SEPARATION_METERS, 3, 0}, SMALL_SIZE_METERS, {0, 0, 1}, {1, 1, 1}, true);
    Array<internal::MeshLightShadowCaster> casters;
    internal::MeshLightShadowReport report;
    REQUIRE(internal::BuildRasterMeshLightShadowCasters(words, 4, NO_RECEIVER_BOUNDS, casters, report) ==
            rhi::RasterStatus::Ready);
    REQUIRE(casters.GetSize() == 3);
    const auto getArea = [&words](usize record) {
        return words[record * (renderer::MESH_LIGHT_POLYGON_HEADER_WORDS + CORNER_COUNT) +
                     renderer::MESH_LIGHT_POLYGON_HEADER_WORDS]
            .W;
    };
    // One polygon: its equal-area disk, across its normal, well inside the sphere through its corners.
    const internal::MeshLightShadowCaster& panel = casters[GetCasterKey(words, 0) - 1];
    CHECK(panel.LightNormalWorld == math::Vector3{0, -1, 0});
    CHECK(panel.DiskRadiusMeters ==
          Catch::Approx(math::Sqrt(getArea(0) / static_cast<float32>(math::kPi))).epsilon(1e-5F));
    CHECK(panel.DiskRadiusMeters < panel.LightRadiusMeters);
    // Two small emitters apart: the disk spans their spread, which the uniform disk of the same second moment of
    // area sets, rather than their tiny areas.
    REQUIRE(GetCasterKey(words, 1) == GetCasterKey(words, 2));
    const internal::MeshLightShadowCaster& pair = casters[GetCasterKey(words, 1) - 1];
    CHECK(pair.DiskRadiusMeters > PAIR_SEPARATION_METERS * .5F);
    CHECK(pair.DiskRadiusMeters <= pair.LightRadiusMeters);
    // An enclosing caster's disk faces each receiver, so it carries no normal.
    const internal::MeshLightShadowCaster& enclosing = casters[GetCasterKey(words, 3) - 1];
    CHECK(enclosing.LightNormalWorld == math::Vector3{});
    CHECK(enclosing.DiskRadiusMeters > 0);
    CHECK(enclosing.DiskRadiusMeters <= enclosing.LightRadiusMeters);

    // The table flags the disk words in its header and appends them after every caster's words, in caster order.
    Array<math::Vector4> table;
    REQUIRE(table.TryAdd({}) == true);
    uint32 tableWord = 0;
    REQUIRE(internal::AppendRasterMeshLightShadowTable(table,
                                                       {casters.GetData(), casters.GetSize()},
                                                       renderer::MESH_LIGHT_SHADOW_ATLAS_TEXELS,
                                                       tableWord) == rhi::RasterStatus::Ready);
    CHECK(tableWord == 1);
    REQUIRE(table.GetSize() == tableWord + renderer::MESH_LIGHT_SHADOW_HEADER_WORDS +
                                   casters.GetSize() * (renderer::MESH_LIGHT_SHADOW_CASTER_WORDS +
                                                        renderer::MESH_LIGHT_SHADOW_DISK_WORDS));
    CHECK(table[tableWord].X == 3);
    CHECK(table[tableWord].W == 1);
    const usize diskWord = tableWord + renderer::MESH_LIGHT_SHADOW_HEADER_WORDS +
                           casters.GetSize() * renderer::MESH_LIGHT_SHADOW_CASTER_WORDS;
    for (usize caster = 0; caster < casters.GetSize(); ++caster)
    {
        const math::Vector4 disk = table[diskWord + caster * renderer::MESH_LIGHT_SHADOW_DISK_WORDS];
        CHECK(math::Vector3{disk.X, disk.Y, disk.Z} == casters[caster].LightNormalWorld);
        CHECK(disk.W == casters[caster].DiskRadiusMeters);
        // The caster's own words still open with its position and range, then its sphere radius.
        const usize casterWord =
            tableWord + renderer::MESH_LIGHT_SHADOW_HEADER_WORDS + caster * renderer::MESH_LIGHT_SHADOW_CASTER_WORDS;
        CHECK(table[casterWord + 1].X == casters[caster].LightRadiusMeters);
    }
}

TEST_CASE("mesh-light shadow ranges stop at the receiver bounds", "[mesh-area][mesh-light-shadows]")
{
    // A bright downward panel whose culling reach is far beyond a 4 m room.
    Array<math::Vector4> words;
    AddTriangle(words, {0, 3, 0}, .2F, {0, -1, 0}, {1000, 1000, 1000}, false);
    Array<math::Vector4> unboundedWords;
    REQUIRE(unboundedWords.TryAddRange({words.GetData(), words.GetSize()}) == true);
    const math::Aabb3 roomWorld{{-2, 0, -2}, {2, 3, 2}};
    Array<internal::MeshLightShadowCaster> casters;
    Array<internal::MeshLightShadowCaster> unbounded;
    internal::MeshLightShadowReport report;
    REQUIRE(internal::BuildRasterMeshLightShadowCasters(words, 1, roomWorld, casters, report) ==
            rhi::RasterStatus::Ready);
    REQUIRE(internal::BuildRasterMeshLightShadowCasters(unboundedWords, 1, NO_RECEIVER_BOUNDS, unbounded, report) ==
            rhi::RasterStatus::Ready);
    REQUIRE(casters.GetSize() == 1);
    REQUIRE(unbounded.GetSize() == 1);
    const internal::MeshLightShadowCaster& caster = casters[0];
    // The farthest room corner from the caster, which sits just below the panel.
    const math::Vector3 positionWorld = caster.PositionWorld;
    const math::Vector3 farthestWorld{math::Max(positionWorld.X - roomWorld.Min.X, roomWorld.Max.X - positionWorld.X),
                                      math::Max(positionWorld.Y - roomWorld.Min.Y, roomWorld.Max.Y - positionWorld.Y),
                                      math::Max(positionWorld.Z - roomWorld.Min.Z, roomWorld.Max.Z - positionWorld.Z)};
    const float32 farthest = math::Length(farthestWorld);
    CHECK(unbounded[0].RangeMeters > 10 * farthest);
    // Every receiver, moved by the resolve's normal offset, stays within the range; no more than an eighth beyond.
    CHECK(caster.RangeMeters >= farthest);
    CHECK(caster.RangeMeters <= 1.125F * farthest + 1e-4F);
    CHECK(caster.NearMeters == unbounded[0].NearMeters);
    // So each 16-bit linear depth code spans well under a tenth of a millimetre.
    CHECK((caster.RangeMeters - caster.NearMeters) / 65535 < 1e-4F);
}

TEST_CASE("two-sided emitters shadow every cube face and opposing normals stay apart",
          "[mesh-area][mesh-light-shadows]")
{
    Array<math::Vector4> words;
    AddTriangle(words, {0, 0, 0}, .05F, {0, 0, 1}, {1, 1, 1}, true);
    // Back to back with the first but one-sided and facing away: different sidedness and cone.
    AddTriangle(words, {0, 0, -.01F}, .05F, {0, 0, -1}, {1, 1, 1}, false);
    Array<internal::MeshLightShadowCaster> casters;
    internal::MeshLightShadowReport report;
    REQUIRE(internal::BuildRasterMeshLightShadowCasters(words, 2, NO_RECEIVER_BOUNDS, casters, report) ==
            rhi::RasterStatus::Ready);
    REQUIRE(casters.GetSize() == 2);
    CHECK(casters[0].FaceMask == (1U << renderer::MESH_LIGHT_SHADOW_FACE_COUNT) - 1);
    // Two-sided: the caster sits inside its emitter, which must not occlude what it lights.
    CHECK(casters[0].NearMeters >= casters[0].LightRadiusMeters);
    // Two-sided casters render from their members' sphere centre, here the first triangle's bound centre.
    CHECK(casters[0].PositionWorld == math::Vector3{words[2].X, words[2].Y, words[2].Z});
    CHECK((casters[1].FaceMask & (1U << 4)) == 0);
    CHECK(GetCasterKey(words, 0) != GetCasterKey(words, 1));
}

TEST_CASE("bulbs of opposing one-sided facets merge into enclosing casters when finer casters exceed the limit",
          "[mesh-area][mesh-light-shadows]")
{
    // Each bulb is an upper facet facing up and a lower one facing down. Clusterings with a normal cone need two
    // casters per bulb, more than the limit, so the bulbs are clustered across normals: one caster each.
    constexpr uint32 BULB_COUNT = renderer::MAX_MESH_LIGHT_SHADOW_CASTERS / 2 + 1;
    constexpr float32 FACET_OFFSET_METERS = .02F;
    Array<math::Vector4> words;
    for (uint32 bulb = 0; bulb < BULB_COUNT; ++bulb)
    {
        const math::Vector3 centerWorld{SEPARATION_METERS * static_cast<float32>(bulb), 3, 0};
        AddTriangle(words, centerWorld + math::Vector3{0, FACET_OFFSET_METERS, 0}, .03F, {0, 1, 0}, {2, 2, 2}, false);
        AddTriangle(words, centerWorld - math::Vector3{0, FACET_OFFSET_METERS, 0}, .03F, {0, -1, 0}, {2, 2, 2}, false);
    }
    Array<internal::MeshLightShadowCaster> casters;
    internal::MeshLightShadowReport report;
    REQUIRE(internal::BuildRasterMeshLightShadowCasters(words, 2 * BULB_COUNT, NO_RECEIVER_BOUNDS, casters, report) ==
            rhi::RasterStatus::Ready);
    REQUIRE(casters.GetSize() == BULB_COUNT);
    CHECK(report.UnshadowedRecords == 0);
    CHECK(report.ShadowedRecords == 2 * BULB_COUNT);
    CHECK(report.Faces == BULB_COUNT * renderer::MESH_LIGHT_SHADOW_FACE_COUNT);
    for (usize bulb = 0; bulb < BULB_COUNT; ++bulb)
    {
        const uint32 key = GetCasterKey(words, 2 * bulb);
        REQUIRE(key != 0);
        CHECK(GetCasterKey(words, 2 * bulb + 1) == key);
        // The caster encloses both facets: it renders from between them, every face, past their sphere.
        const internal::MeshLightShadowCaster& caster = casters[key - 1];
        // Equal facets: the sphere enclosing both is centred between their bound spheres (deferred_layout.h).
        const usize upperBound = 2 * bulb * (renderer::MESH_LIGHT_POLYGON_HEADER_WORDS + CORNER_COUNT) +
                                 renderer::MESH_LIGHT_POLYGON_BOUND_WORD;
        const usize lowerBound = upperBound + renderer::MESH_LIGHT_POLYGON_HEADER_WORDS + CORNER_COUNT;
        const math::Vector3 centerWorld =
            (math::Vector3{words[upperBound].X, words[upperBound].Y, words[upperBound].Z} +
             math::Vector3{words[lowerBound].X, words[lowerBound].Y, words[lowerBound].Z}) *
            .5F;
        CHECK(math::Length(caster.PositionWorld - centerWorld) < 1e-4F);
        CHECK(caster.FaceMask == (1U << renderer::MESH_LIGHT_SHADOW_FACE_COUNT) - 1);
        CHECK(caster.NearMeters >= caster.LightRadiusMeters);
        CHECK(caster.LightRadiusMeters > FACET_OFFSET_METERS);
    }
}

TEST_CASE("one-sided facets spread beyond the cone about their mean normal share an enclosing caster",
          "[mesh-area][mesh-light-shadows]")
{
    // Facets of a downward lamp tilted 29 degrees about z from the brightest one's normal: all lie in the cone
    // about that first member, so they form one caster.
    constexpr float32 TILT_SINE = .48480962F;
    constexpr float32 TILT_COSINE = .87461971F;
    // The brightest facet faces straight down, tiltedCount facets lean one way and one the other way.
    const auto addLamp = [](Array<math::Vector4>& outWords, usize tiltedCount) {
        AddTriangle(outWords, {0, 3, 0}, .05F, {0, -1, 0}, {5, 5, 5}, false);
        for (usize facet = 0; facet < tiltedCount; ++facet)
        {
            AddTriangle(outWords,
                        {.05F * static_cast<float32>(facet + 1), 3, 0},
                        .05F,
                        {TILT_SINE, -TILT_COSINE, 0},
                        {4, 4, 4},
                        false);
        }
        AddTriangle(outWords, {-.05F, 3, 0}, .05F, {-TILT_SINE, -TILT_COSINE, 0}, {4, 4, 4}, false);
    };
    // One facet each way: their mean points down, 29 degrees from each, so the caster renders from in front of
    // them, without the face behind them.
    Array<math::Vector4> balanced;
    addLamp(balanced, 1);
    Array<internal::MeshLightShadowCaster> casters;
    internal::MeshLightShadowReport report;
    REQUIRE(internal::BuildRasterMeshLightShadowCasters(balanced, 3, NO_RECEIVER_BOUNDS, casters, report) ==
            rhi::RasterStatus::Ready);
    REQUIRE(casters.GetSize() == 1);
    CHECK(casters[0].PositionWorld.Y < 3);
    CHECK((casters[0].FaceMask & (1U << 2)) == 0);
    // Five facets one way tilt the mean 17 degrees towards them, 46 degrees from the opposite facet: no point in
    // front of every facet sees past the others, so the caster encloses them.
    constexpr usize LOPSIDED_COUNT = 5;
    Array<math::Vector4> lopsided;
    addLamp(lopsided, LOPSIDED_COUNT);
    REQUIRE(internal::BuildRasterMeshLightShadowCasters(lopsided,
                                                        LOPSIDED_COUNT + 2,
                                                        NO_RECEIVER_BOUNDS,
                                                        casters,
                                                        report) == rhi::RasterStatus::Ready);
    REQUIRE(casters.GetSize() == 1);
    CHECK(casters[0].FaceMask == (1U << renderer::MESH_LIGHT_SHADOW_FACE_COUNT) - 1);
    CHECK(casters[0].NearMeters >= casters[0].LightRadiusMeters);
}

TEST_CASE("mesh-light shadow casters stop at the limit and leave dimmer records unshadowed",
          "[mesh-area][mesh-light-shadows]")
{
    constexpr uint32 EXTRA = 6;
    constexpr uint32 COUNT = renderer::MAX_MESH_LIGHT_SHADOW_CASTERS + EXTRA;
    Array<math::Vector4> words;
    for (uint32 record = 0; record < COUNT; ++record)
    {
        // Brightness decreases with the index; the last EXTRA records are the dimmest.
        const float32 emission = static_cast<float32>(COUNT - record);
        AddTriangle(words,
                    {SEPARATION_METERS * static_cast<float32>(record), 0, 0},
                    .05F,
                    {0, 0, 1},
                    {emission, emission, emission},
                    true);
    }
    Array<internal::MeshLightShadowCaster> casters;
    internal::MeshLightShadowReport report;
    REQUIRE(internal::BuildRasterMeshLightShadowCasters(words, COUNT, NO_RECEIVER_BOUNDS, casters, report) ==
            rhi::RasterStatus::Ready);
    CHECK(casters.GetSize() == renderer::MAX_MESH_LIGHT_SHADOW_CASTERS);
    CHECK(report.UnshadowedRecords == EXTRA);
    for (uint32 record = 0; record < COUNT; ++record)
    {
        CHECK((GetCasterKey(words, record) == 0) == (record >= renderer::MAX_MESH_LIGHT_SHADOW_CASTERS));
    }
}

TEST_CASE("mesh-light shadow faces pack without overlap and shrink to fit", "[mesh-area][mesh-light-shadows]")
{
    Array<math::Vector4> words;
    constexpr uint32 COUNT = 12;
    for (uint32 record = 0; record < COUNT; ++record)
    {
        const float32 emission = static_cast<float32>(record + 1);
        AddTriangle(words,
                    {SEPARATION_METERS * static_cast<float32>(record), 0, 0},
                    .05F,
                    {0, 0, 1},
                    {emission, emission, emission},
                    true);
    }
    Array<internal::MeshLightShadowCaster> casters;
    internal::MeshLightShadowReport report;
    REQUIRE(internal::BuildRasterMeshLightShadowCasters(words, COUNT, NO_RECEIVER_BOUNDS, casters, report) ==
            rhi::RasterStatus::Ready);
    struct AllocationCase final
    {
        uint32 AtlasTexels = 0;
        uint32 DirectionalCount = 0;
    };
    constexpr usize ALLOCATION_CASE_COUNT = 3;
    constexpr uint32 SMALL_ATLAS_TEXELS = 512;
    for (const AllocationCase& allocation : StaticArray<AllocationCase, ALLOCATION_CASE_COUNT>{{
             {renderer::MESH_LIGHT_SHADOW_ATLAS_TEXELS, 1},
             {renderer::MESH_LIGHT_SHADOW_ATLAS_TEXELS, internal::MAX_DIRECTIONAL_SHADOW_REGIONS},
             {SMALL_ATLAS_TEXELS, 0},
         }})
    {
        const uint32 atlasTexels = allocation.AtlasTexels;
        const uint32 directionalCount = allocation.DirectionalCount;
        Array<internal::MeshLightShadowCaster> allocated;
        REQUIRE(allocated.TryAddRange({casters.GetData(), casters.GetSize()}) == true);
        REQUIRE(internal::AllocateRasterMeshLightShadowFaces(allocated, atlasTexels, directionalCount) == true);
        Array<internal::MeshLightShadowCaster> again;
        REQUIRE(again.TryAddRange({casters.GetData(), casters.GetSize()}) == true);
        REQUIRE(internal::AllocateRasterMeshLightShadowFaces(again, atlasTexels, directionalCount) == true);
        uint64 area = 0;
        for (usize first = 0; first < allocated.GetSize(); ++first)
        {
            const internal::MeshLightShadowCaster& caster = allocated[first];
            CHECK(caster.FaceTexels >= renderer::MIN_MESH_LIGHT_SHADOW_FACE_TEXELS);
            CHECK(caster.FaceTexels <= renderer::MAX_MESH_LIGHT_SHADOW_FACE_TEXELS);
            CHECK(caster.FaceTexels == again[first].FaceTexels);
            for (uint32 face = 0; face < renderer::MESH_LIGHT_SHADOW_FACE_COUNT; ++face)
            {
                const internal::MeshLightShadowFaceOrigin origin = caster.FaceOrigins[face];
                CHECK(origin.X == again[first].FaceOrigins[face].X);
                CHECK(origin.Y == again[first].FaceOrigins[face].Y);
                CHECK(origin.X % caster.FaceTexels == 0);
                CHECK(origin.Y % caster.FaceTexels == 0);
                CHECK(origin.X + caster.FaceTexels <= atlasTexels);
                CHECK(origin.Y + caster.FaceTexels <= atlasTexels);
                // Cube faces stay clear of every reserved directional square, gutters included.
                for (uint32 directional = 0; directional < directionalCount; ++directional)
                {
                    const rhi::RasterRectangle interior = internal::GetDirectionalShadowInterior(directional);
                    const uint32 squareX = interior.X - 1;
                    const uint32 squareY = interior.Y - 1;
                    CHECK(squareX + internal::DIRECTIONAL_SHADOW_TEXELS <= atlasTexels);
                    CHECK(squareY + internal::DIRECTIONAL_SHADOW_TEXELS <= atlasTexels);
                    CHECK((origin.X + caster.FaceTexels <= squareX ||
                           squareX + internal::DIRECTIONAL_SHADOW_TEXELS <= origin.X ||
                           origin.Y + caster.FaceTexels <= squareY ||
                           squareY + internal::DIRECTIONAL_SHADOW_TEXELS <= origin.Y));
                }
                area += uint64{caster.FaceTexels} * caster.FaceTexels;
                // No other face slot overlaps this one.
                for (usize second = first; second < allocated.GetSize(); ++second)
                {
                    const internal::MeshLightShadowCaster& other = allocated[second];
                    for (uint32 otherFace = second == first ? face + 1 : 0;
                         otherFace < renderer::MESH_LIGHT_SHADOW_FACE_COUNT;
                         ++otherFace)
                    {
                        const internal::MeshLightShadowFaceOrigin otherOrigin = other.FaceOrigins[otherFace];
                        const bool isSeparate = origin.X + caster.FaceTexels <= otherOrigin.X ||
                                                otherOrigin.X + other.FaceTexels <= origin.X ||
                                                origin.Y + caster.FaceTexels <= otherOrigin.Y ||
                                                otherOrigin.Y + other.FaceTexels <= origin.Y;
                        CHECK(isSeparate == true);
                    }
                }
            }
        }
        CHECK(area <= uint64{atlasTexels} * atlasTexels);
    }
    // Casters are ordered brightest first; the brightest receives the largest slot and the dimmest a smaller one.
    Array<internal::MeshLightShadowCaster> allocated;
    REQUIRE(allocated.TryAddRange({casters.GetData(), casters.GetSize()}) == true);
    REQUIRE(internal::AllocateRasterMeshLightShadowFaces(allocated, renderer::MESH_LIGHT_SHADOW_ATLAS_TEXELS) == true);
    for (const internal::MeshLightShadowCaster& caster : allocated)
    {
        CHECK(caster.FaceTexels <= allocated[0].FaceTexels);
    }
    CHECK(allocated[allocated.GetSize() - 1].FaceTexels < allocated[0].FaceTexels);
    // An atlas smaller than every minimal slot together is rejected and the casters are kept.
    const uint32 brightestTexels = allocated[0].FaceTexels;
    CHECK(internal::AllocateRasterMeshLightShadowFaces(allocated, renderer::MIN_MESH_LIGHT_SHADOW_FACE_TEXELS) ==
          false);
    CHECK(allocated[0].FaceTexels == brightestTexels);
}

TEST_CASE("mesh-light shadow lookup matches the face projections", "[mesh-area][mesh-light-shadows]")
{
    internal::MeshLightShadowCaster caster;
    caster.PositionWorld = {1, 2, 3};
    caster.NearMeters = .1F;
    caster.RangeMeters = 20;
    caster.FaceTexels = 256;
    caster.FaceMask = (1U << renderer::MESH_LIGHT_SHADOW_FACE_COUNT) - 1;
    for (uint32 face = 0; face < renderer::MESH_LIGHT_SHADOW_FACE_COUNT; ++face)
    {
        caster.FaceOrigins[face] = {256 * face, 512};
    }
    Sequence sequence;
    for (usize sample = 0; sample < 500; ++sample)
    {
        math::Vector3 directionWorld;
        REQUIRE(math::TryNormalize({sequence.Next() - .5F, sequence.Next() - .5F, sequence.Next() - .5F + 1e-3F},
                                   directionWorld) == math::MathStatus::Success);
        const math::Vector3 positionWorld = caster.PositionWorld + directionWorld * (1 + 10 * sequence.Next());
        uint32 face = 0;
        math::Vector2 texel;
        float32 depth = 0;
        REQUIRE(internal::LocateRasterMeshLightShadowTexel(caster, positionWorld, face, texel, depth) == true);
        math::Matrix4 worldToClip;
        REQUIRE(internal::MakeRasterMeshLightShadowFaceToClip(caster, face, worldToClip) == rhi::RasterStatus::Ready);
        const math::Vector4 clip = worldToClip * math::Vector4{positionWorld.X, positionWorld.Y, positionWorld.Z, 1};
        REQUIRE(clip.W > 0);
        const float32 ndcX = clip.X / clip.W;
        const float32 ndcY = clip.Y / clip.W;
        // The receiver lies in its face's 90-degree interior, inside the gutter-widened slot.
        const float32 interior = 1.0F / internal::GetRasterMeshLightShadowTangent(caster.FaceTexels);
        CHECK(math::Abs(ndcX) <= interior + 1e-4F);
        CHECK(math::Abs(ndcY) <= interior + 1e-4F);
        // Rasterized depth stays conventional; clip z itself is the linear depth the face stores.
        CHECK(clip.Z / clip.W >= 0);
        CHECK(clip.Z / clip.W <= 1);
        CHECK(math::Abs(depth - clip.Z) < 1e-5F);
        const float32 forward = math::Dot(positionWorld - caster.PositionWorld, FACE_FORWARD_WORLD[face]);
        CHECK(math::Abs(depth - (forward - caster.NearMeters) / (caster.RangeMeters - caster.NearMeters)) < 1e-5F);
        const float32 texels = static_cast<float32>(caster.FaceTexels);
        CHECK(math::Abs(texel.X - (static_cast<float32>(caster.FaceOrigins[face].X) + (ndcX * .5F + .5F) * texels)) <
              1e-2F);
        CHECK(math::Abs(texel.Y - (static_cast<float32>(caster.FaceOrigins[face].Y) + (.5F - ndcY * .5F) * texels)) <
              1e-2F);
    }
}

TEST_CASE("mesh-light shadow depths round-trip through sixteen bits", "[mesh-area][mesh-light-shadows]")
{
    constexpr float32 HALF_STEP = .5F / 65535;
    for (const float32 depth : StaticArray<float32, 5>{{0, .25F, .5F, .999F, 1}})
    {
        CHECK(math::Abs(internal::DecodeRasterMeshLightShadowDepth(internal::EncodeRasterMeshLightShadowDepth(depth)) -
                        depth) <= HALF_STEP + 1e-7F);
    }
    CHECK(internal::EncodeRasterMeshLightShadowDepth(-1) == 0);
    CHECK(internal::EncodeRasterMeshLightShadowDepth(2) == 65535);
}

TEST_CASE("mesh-light grid cells list each caster's records together", "[mesh-area][mesh-light-shadows]")
{
    Array<math::Vector4> words;
    Sequence sequence;
    constexpr uint32 COUNT = 40;
    for (uint32 record = 0; record < COUNT; ++record)
    {
        // Interleave two lamp clusters in record order so grouping must reorder them.
        const float32 x = record % 2 == 0 ? 0.0F : SEPARATION_METERS;
        // Bright enough that both clusters reach the query point midway between them.
        const float32 emission = 100 + sequence.Next();
        AddTriangle(words, {x + .01F * sequence.Next(), 1, 0}, .01F, {0, 0, 1}, {emission, emission, emission}, true);
    }
    Array<internal::MeshLightShadowCaster> casters;
    internal::MeshLightShadowReport report;
    REQUIRE(internal::BuildRasterMeshLightShadowCasters(words, COUNT, NO_RECEIVER_BOUNDS, casters, report) ==
            rhi::RasterStatus::Ready);
    REQUIRE(casters.GetSize() == 2);
    uint32 gridWord = 0;
    REQUIRE(internal::AppendRasterMeshLightGrid(words, COUNT, {{-1, 0, -1}, {SEPARATION_METERS + 1, 2, 1}}, gridWord) ==
            rhi::RasterStatus::Ready);
    REQUIRE(gridWord != 0);
    Array<uint32> listed;
    REQUIRE(internal::GetRasterMeshLightCell({words.GetData(), words.GetSize()}, gridWord, {2, 1, 0}, listed) == true);
    REQUIRE(listed.GetSize() == COUNT);
    for (usize entry = 1; entry < listed.GetSize(); ++entry)
    {
        CHECK(words[listed[entry - 1] + CASTER_CORNER].W <= words[listed[entry] + CASTER_CORNER].W);
    }
}
