#include "internal/raster_mesh_light_shadows.h"

#include "internal/raster_mesh_light_grid.h"
#include "internal/raster_mesh_lights.h"

#include <ludus/foundation/containers/stable_sort.hpp>
#include <ludus/foundation/math/projection.hpp>
#include <ludus/foundation/math/scalar.hpp>

// Thanks to William T. Reeves, David H. Salesin and Robert L. Cook, "Rendering Antialiased Shadows with Depth Maps",
// SIGGRAPH 1987, pp. 283-291, https://doi.org/10.1145/37401.37435, for depth-map shadows compared at the receiver
// and filtered by the fraction of closer samples (percentage-closer filtering, used by the resolve): each caster
// stores, per cube-face texel, the depth of the nearest occluder, and a receiver deeper than it is shadowed. The maps
// are rendered once per prepared scene, because lights and geometry are static while the camera moves, with the
// renderer's flat shadow pipeline, which stores conventional depth as 16 bits in red and green.
namespace ludus::graphics::renderer::internal
{
using namespace foundation;
namespace
{
using Status = rhi::RasterStatus;
constexpr usize NORMAL_WORD = 0;
constexpr usize EMISSION_WORD = 1;
/// A caster renders from in front of its one-sided members only while every member's normal lies within 30 degrees
/// of their flux-weighted mean; otherwise it encloses them.
constexpr float32 NORMAL_CONE_COSINE = 0.8660254F;
/// A normal-cone cosine every pair of unit normals passes.
constexpr float32 ANY_NORMAL_COSINE = -2;
/// One clustering: members are merged into a caster while their enclosing sphere stays within RadiusMeters (a larger
/// polygon forms a caster of its own) and, when NormalConeCosine is at least -1, while they share the sidedness of the
/// caster's first member and, if one-sided, their normals lie within the cone about its normal.
struct ClusterLevel final
{
    float32 RadiusMeters = 0;
    float32 NormalConeCosine = 0;
};
constexpr usize CLUSTER_LEVEL_COUNT = 5;
/// Ever coarser clusterings. The first that needs at most MAX_MESH_LIGHT_SHADOW_CASTERS casters is used; the last
/// one is used up to the limit otherwise. Light fixtures are often closed shells of one-sided triangles (a bulb, a
/// lantern) whose normals span every direction, so the normal cone is relaxed before the radius grows: such a
/// fixture then forms one enclosing caster instead of one per facet direction.
constexpr StaticArray<ClusterLevel, CLUSTER_LEVEL_COUNT> CLUSTER_LEVELS{{
    {0.5F, NORMAL_CONE_COSINE},
    {0.5F, 0},
    {0.5F, ANY_NORMAL_COSINE},
    {1, ANY_NORMAL_COSINE},
    {2, ANY_NORMAL_COSINE},
}};
/// One-sided casters render from in front of their emitters: at least a centimetre, or this fraction of the radius.
constexpr float32 ONE_SIDED_OFFSET_METERS = 0.01F;
constexpr float32 ONE_SIDED_OFFSET_RELATIVE = 0.05F;
/// A cube face's frustum reaches 54.74 degrees from its axis (at a corner), so it lies wholly behind a plane whose
/// normal makes at least 144.74 degrees with the axis: a cosine at most -sqrt(2/3).
constexpr float32 FACE_BEHIND_COSINE = -0.81649658F;
/// The range always exceeds the near distance by at least this factor, so the projection stays well formed.
constexpr float32 MINIMUM_RANGE_OVER_NEAR = 2;
/// The near distance is at least this fraction of the range: 16-bit conventional depth resolves about
/// distance^2 / (near * 65535), so a centimetre near plane would blur occluders metres away.
constexpr float32 NEAR_RANGE_FRACTION = 0.01F;
/// Smallest near distance in metres, for a degenerate two-sided caster.
constexpr float32 MINIMUM_NEAR_METERS = 0.001F;
/// Largest 16-bit depth code.
constexpr float32 DEPTH_CODE_MAXIMUM = 65535;
constexpr uint32 AXIS_COUNT = 3;
/// Bits of a Morton code per axis that address MESH_LIGHT_SHADOW_ATLAS_TEXELS / MIN_MESH_LIGHT_SHADOW_FACE_TEXELS
/// cells along a side.
constexpr uint32 MORTON_AXIS_BITS = 16;
struct FaceBasis final
{
    math::Vector3 ForwardWorld;
    math::Vector3 RightWorld;
    math::Vector3 UpWorld;
};
// The view bases math::TryLookAt builds for each face with BuildPointShadowMatrix's up hints (+Y, except -Z for +Y
// and +Z for -Y); renderer_mesh_light_shadows.slang repeats this table.
constexpr StaticArray<FaceBasis, MESH_LIGHT_SHADOW_FACE_COUNT> FACE_BASES{{
    {{1, 0, 0}, {0, 0, 1}, {0, 1, 0}},
    {{-1, 0, 0}, {0, 0, -1}, {0, 1, 0}},
    {{0, 1, 0}, {-1, 0, 0}, {0, 0, -1}},
    {{0, -1, 0}, {-1, 0, 0}, {0, 0, 1}},
    {{0, 0, 1}, {-1, 0, 0}, {0, 1, 0}},
    {{0, 0, -1}, {1, 0, 0}, {0, 1, 0}},
}};
constexpr StaticArray<math::Vector3, MESH_LIGHT_SHADOW_FACE_COUNT> FACE_UP_HINTS{
    {{0, 1, 0}, {0, 1, 0}, {0, 0, -1}, {0, 0, 1}, {0, 1, 0}, {0, 1, 0}}};
constexpr uint32 ALL_FACES = (1U << MESH_LIGHT_SHADOW_FACE_COUNT) - 1;
// The smallest slots of every face of every caster fit the atlas, so allocation never fails for want of space.
static_assert(uint64{MAX_MESH_LIGHT_SHADOW_CASTERS} * MESH_LIGHT_SHADOW_FACE_COUNT * MIN_MESH_LIGHT_SHADOW_FACE_TEXELS *
                  MIN_MESH_LIGHT_SHADOW_FACE_TEXELS <=
              uint64{MESH_LIGHT_SHADOW_ATLAS_TEXELS} * MESH_LIGHT_SHADOW_ATLAS_TEXELS);
struct RecordInfo final
{
    uint32 Word = 0;
    math::Vector3 CenterWorld;
    float32 Radius = 0;
    math::Vector3 NormalWorld;
    bool IsTwoSided = false;
    float32 Flux = 0;
    math::Vector3 ReachCenterWorld;
    float32 ReachRadius = 0;
    /// Index into the caster list, or casters' size when unshadowed.
    uint32 Caster = 0;
};
struct BrighterFirst final
{
    bool operator()(const RecordInfo& left, const RecordInfo& right) const noexcept
    {
        return left.Flux > right.Flux;
    }
};
struct CasterBuild final
{
    math::Vector3 CenterWorld;
    float32 Radius = 0;
    math::Vector3 FirstNormalWorld;
    math::Vector3 NormalSumWorld;
    /// The first member's sidedness, which a level with a normal cone requires of every member.
    bool IsTwoSided = false;
    /// Whether the caster renders from its members' sphere centre, seeing past them, rather than from in front.
    bool IsEnclosing = false;
    float32 Flux = 0;
};
math::Vector3 GetPosition(math::Vector4 word) noexcept
{
    return {word.X, word.Y, word.Z};
}
// The smallest sphere enclosing two spheres.
void MergeSpheres(math::Vector3 firstCenterWorld,
                  float32 firstRadius,
                  math::Vector3 secondCenterWorld,
                  float32 secondRadius,
                  math::Vector3& outCenterWorld,
                  float32& outRadius) noexcept
{
    const float32 distance = math::Length(secondCenterWorld - firstCenterWorld);
    if (distance + secondRadius <= firstRadius)
    {
        outCenterWorld = firstCenterWorld;
        outRadius = firstRadius;
        return;
    }
    if (distance + firstRadius <= secondRadius)
    {
        outCenterWorld = secondCenterWorld;
        outRadius = secondRadius;
        return;
    }
    const float32 radius = (distance + firstRadius + secondRadius) * 0.5F;
    outCenterWorld = firstCenterWorld + (secondCenterWorld - firstCenterWorld) * ((radius - firstRadius) / distance);
    outRadius = radius;
}
// Assign the brightest-first records to casters at one level: each joins the caster whose merged sphere stays
// smallest within the level's radius, or starts a caster. Returns false, leaving the assignment incomplete, at the
// first record that would need more than MAX_MESH_LIGHT_SHADOW_CASTERS casters, unless isFinal, which leaves every
// such record unshadowed instead (Caster MAX_MESH_LIGHT_SHADOW_CASTERS).
bool AssignCasters(const ClusterLevel& level,
                   bool isFinal,
                   Array<RecordInfo>& inoutRecords,
                   Array<CasterBuild>& inoutBuilds) noexcept
{
    const bool hasNormalCone = level.NormalConeCosine >= -1;
    inoutBuilds.Clear();
    for (RecordInfo& record : inoutRecords)
    {
        record.Caster = MAX_MESH_LIGHT_SHADOW_CASTERS;
        float32 bestRadius = level.RadiusMeters;
        for (usize caster = 0; caster < inoutBuilds.GetSize(); ++caster)
        {
            const CasterBuild& build = inoutBuilds[caster];
            if (hasNormalCone == true &&
                (build.IsTwoSided != record.IsTwoSided ||
                 (record.IsTwoSided == false &&
                  math::Dot(build.FirstNormalWorld, record.NormalWorld) < level.NormalConeCosine)))
            {
                continue;
            }
            math::Vector3 mergedCenterWorld;
            float32 mergedRadius = 0;
            MergeSpheres(build.CenterWorld,
                         build.Radius,
                         record.CenterWorld,
                         record.Radius,
                         mergedCenterWorld,
                         mergedRadius);
            if (mergedRadius <= bestRadius)
            {
                bestRadius = mergedRadius;
                record.Caster = static_cast<uint32>(caster);
            }
        }
        if (record.Caster != MAX_MESH_LIGHT_SHADOW_CASTERS)
        {
            CasterBuild& build = inoutBuilds[record.Caster];
            MergeSpheres(build.CenterWorld,
                         build.Radius,
                         record.CenterWorld,
                         record.Radius,
                         build.CenterWorld,
                         build.Radius);
        }
        else if (inoutBuilds.GetSize() < MAX_MESH_LIGHT_SHADOW_CASTERS)
        {
            CasterBuild build;
            build.CenterWorld = record.CenterWorld;
            build.Radius = record.Radius;
            build.FirstNormalWorld = record.NormalWorld;
            build.IsTwoSided = record.IsTwoSided;
            // Capacity for every caster is reserved by the caller.
            static_cast<void>(inoutBuilds.TryAdd(build));
            record.Caster = static_cast<uint32>(inoutBuilds.GetSize() - 1);
        }
        else if (isFinal == false)
        {
            return false;
        }
    }
    return true;
}
uint32 GetBitCount(uint32 value) noexcept
{
    uint32 count = 0;
    for (uint32 bits = value; bits != 0; bits &= bits - 1)
    {
        ++count;
    }
    return count;
}
// Every other bit of a 2D Morton code, starting at bit `first`.
uint32 GetMortonAxis(uint32 code, uint32 first) noexcept
{
    uint32 result = 0;
    for (uint32 bit = 0; bit < MORTON_AXIS_BITS; ++bit)
    {
        result |= ((code >> (2 * bit + first)) & 1U) << bit;
    }
    return result;
}
uint32 GetPowerOfTwoAtMost(float32 value) noexcept
{
    uint32 result = 1;
    while (static_cast<float32>(result) * 2 <= value)
    {
        result *= 2;
    }
    return result;
}
struct FaceSlot final
{
    uint32 Texels = 0;
    uint32 Caster = 0;
    uint32 Face = 0;
};
struct LargerSlotFirst final
{
    bool operator()(const FaceSlot& left, const FaceSlot& right) const noexcept
    {
        return left.Texels > right.Texels;
    }
};
} // namespace

Status BuildRasterMeshLightShadowCasters(Array<math::Vector4>& inoutWords,
                                         uint32 recordCount,
                                         Array<MeshLightShadowCaster>& outCasters,
                                         MeshLightShadowReport& outReport) noexcept
{
    const BorrowedView<const math::Vector4> words{inoutWords.GetData(), inoutWords.GetSize()};
    Array<RecordInfo> records;
    if (records.TryEnsureCapacity(recordCount) == false)
    {
        return Status::OutOfMemory;
    }
    usize offset = 0;
    for (uint32 record = 0; record < recordCount; ++record)
    {
        const usize recordWords = GetRasterMeshLightRecordWords(words, offset);
        if (recordWords < MESH_LIGHT_POLYGON_HEADER_WORDS + MESH_LIGHT_SHADOW_CASTER_CORNER + 1)
        {
            return Status::InvalidDescription;
        }
        RecordInfo info;
        info.Word = static_cast<uint32>(offset);
        const math::Vector4 bound = words[offset + MESH_LIGHT_POLYGON_BOUND_WORD];
        info.CenterWorld = GetPosition(bound);
        info.Radius = bound.W;
        info.NormalWorld = GetPosition(words[offset + NORMAL_WORD]);
        info.IsTwoSided = words[offset + NORMAL_WORD].W != 0;
        const math::Vector4 emission = words[offset + EMISSION_WORD];
        info.Flux = math::Max(emission.X, math::Max(emission.Y, emission.Z)) *
                    words[offset + MESH_LIGHT_POLYGON_HEADER_WORDS].W;
        if (GetRasterMeshLightReach(words, offset, info.ReachCenterWorld, info.ReachRadius) == false ||
            math::IsFinite(info.CenterWorld) == false || math::IsFinite(info.Flux) == false || info.Radius < 0)
        {
            return Status::InvalidDescription;
        }
        static_cast<void>(records.TryAdd(info));
        offset += recordWords;
    }
    Array<RecordInfo> scratch;
    if (scratch.TryResize(records.GetSize()) == false ||
        StableSort(records.GetData(), scratch.GetData(), records.GetSize(), BrighterFirst{}) == false)
    {
        return Status::OutOfMemory;
    }
    Array<CasterBuild> builds;
    if (builds.TryEnsureCapacity(MAX_MESH_LIGHT_SHADOW_CASTERS) == false)
    {
        return Status::OutOfMemory;
    }
    for (usize level = 0; level < CLUSTER_LEVEL_COUNT; ++level)
    {
        if (AssignCasters(CLUSTER_LEVELS[level], level + 1 == CLUSTER_LEVEL_COUNT, records, builds) == true)
        {
            break;
        }
    }
    MeshLightShadowReport report;
    for (const RecordInfo& record : records)
    {
        if (record.Caster == MAX_MESH_LIGHT_SHADOW_CASTERS)
        {
            ++report.UnshadowedRecords;
            continue;
        }
        CasterBuild& build = builds[record.Caster];
        build.NormalSumWorld = build.NormalSumWorld + record.NormalWorld * record.Flux;
        build.Flux += record.Flux;
        build.IsEnclosing = build.IsEnclosing == true || record.IsTwoSided == true;
        ++report.ShadowedRecords;
    }
    // One-sided members whose normals spread beyond the cone about their mean, such as the facets of a bulb, would
    // block one another seen from in front of any of them; their caster encloses them instead.
    for (const RecordInfo& record : records)
    {
        if (record.Caster == MAX_MESH_LIGHT_SHADOW_CASTERS)
        {
            continue;
        }
        CasterBuild& build = builds[record.Caster];
        math::Vector3 meanNormalWorld;
        build.IsEnclosing = build.IsEnclosing == true ||
                            math::TryNormalize(build.NormalSumWorld, meanNormalWorld) != math::MathStatus::Success ||
                            math::Dot(meanNormalWorld, record.NormalWorld) < NORMAL_CONE_COSINE;
    }
    Array<MeshLightShadowCaster> casters;
    if (casters.TryResize(builds.GetSize()) == false)
    {
        return Status::OutOfMemory;
    }
    for (usize caster = 0; caster < builds.GetSize(); ++caster)
    {
        const CasterBuild& build = builds[caster];
        MeshLightShadowCaster& result = casters[caster];
        float32 offsetMeters = 0;
        math::Vector3 normalWorld = build.FirstNormalWorld;
        if (build.IsEnclosing == false)
        {
            // Only a caster whose mean normal normalizes stays one-sided (above).
            static_cast<void>(math::TryNormalize(build.NormalSumWorld, normalWorld));
            offsetMeters = math::Max(ONE_SIDED_OFFSET_METERS, ONE_SIDED_OFFSET_RELATIVE * build.Radius);
        }
        result.PositionWorld = build.CenterWorld + normalWorld * offsetMeters;
        result.LightRadiusMeters = build.Radius;
        // A one-sided caster sits in front of its emitters, which lie behind it and never block a receiver they
        // light; an enclosing one sits inside its emitters and must see past them.
        result.NearMeters =
            build.IsEnclosing == true ? math::Max(build.Radius, MINIMUM_NEAR_METERS) : offsetMeters * 0.5F;
        result.Flux = build.Flux;
        result.FaceMask = build.IsEnclosing == true ? ALL_FACES : 0;
    }
    for (const RecordInfo& record : records)
    {
        if (record.Caster == MAX_MESH_LIGHT_SHADOW_CASTERS)
        {
            continue;
        }
        MeshLightShadowCaster& caster = casters[record.Caster];
        caster.RangeMeters =
            math::Max(caster.RangeMeters,
                      math::Length(record.ReachCenterWorld - caster.PositionWorld) + record.ReachRadius);
        if (builds[record.Caster].IsEnclosing == false)
        {
            // A face can receive this member's light unless its whole frustum lies behind the member's plane.
            for (uint32 face = 0; face < MESH_LIGHT_SHADOW_FACE_COUNT; ++face)
            {
                if (math::Dot(FACE_BASES[face].ForwardWorld, record.NormalWorld) > FACE_BEHIND_COSINE)
                {
                    caster.FaceMask |= 1U << face;
                }
            }
        }
    }
    for (MeshLightShadowCaster& caster : casters)
    {
        caster.NearMeters = math::Max(caster.NearMeters, NEAR_RANGE_FRACTION * caster.RangeMeters);
        caster.RangeMeters = math::Max(caster.RangeMeters, caster.NearMeters * MINIMUM_RANGE_OVER_NEAR);
        report.Faces += GetBitCount(caster.FaceMask);
        if (math::IsFinite(caster.PositionWorld) == false || math::IsFinite(caster.RangeMeters) == false)
        {
            return Status::InvalidDescription;
        }
    }
    report.Casters = static_cast<uint32>(casters.GetSize());
    for (const RecordInfo& record : records)
    {
        inoutWords[record.Word + MESH_LIGHT_POLYGON_HEADER_WORDS + MESH_LIGHT_SHADOW_CASTER_CORNER].W =
            record.Caster == MAX_MESH_LIGHT_SHADOW_CASTERS ? 0 : static_cast<float32>(record.Caster + 1);
    }
    outCasters = Move(casters);
    outReport = report;
    return Status::Ready;
}

bool AllocateRasterMeshLightShadowFaces(Array<MeshLightShadowCaster>& inoutCasters, uint32 atlasTexels) noexcept
{
    const uint32 atlasCells = atlasTexels / MIN_MESH_LIGHT_SHADOW_FACE_TEXELS;
    if (atlasTexels % MIN_MESH_LIGHT_SHADOW_FACE_TEXELS != 0 || atlasCells == 0 ||
        (atlasCells & (atlasCells - 1)) != 0 || atlasCells > (1U << MORTON_AXIS_BITS))
    {
        return false;
    }
    if (inoutCasters.IsEmpty() == true)
    {
        return true;
    }
    float32 brightest = 0;
    for (const MeshLightShadowCaster& caster : inoutCasters)
    {
        brightest = math::Max(brightest, caster.Flux);
    }
    Array<uint32> sizes;
    if (sizes.TryResize(inoutCasters.GetSize()) == false)
    {
        return false;
    }
    for (usize caster = 0; caster < inoutCasters.GetSize(); ++caster)
    {
        const float32 share = brightest > 0 ? inoutCasters[caster].Flux / brightest : 1;
        sizes[caster] = math::Clamp(GetPowerOfTwoAtMost(static_cast<float32>(MAX_MESH_LIGHT_SHADOW_FACE_TEXELS) *
                                                        math::Sqrt(math::Max(share, 0.0F))),
                                    MIN_MESH_LIGHT_SHADOW_FACE_TEXELS,
                                    MAX_MESH_LIGHT_SHADOW_FACE_TEXELS);
    }
    const uint64 atlasArea = uint64{atlasTexels} * atlasTexels;
    while (true)
    {
        uint64 area = 0;
        bool canHalve = false;
        for (usize caster = 0; caster < inoutCasters.GetSize(); ++caster)
        {
            area += uint64{GetBitCount(inoutCasters[caster].FaceMask)} * sizes[caster] * sizes[caster];
            canHalve = canHalve == true || sizes[caster] > MIN_MESH_LIGHT_SHADOW_FACE_TEXELS;
        }
        if (area <= atlasArea)
        {
            break;
        }
        if (canHalve == false)
        {
            return false;
        }
        for (uint32& size : sizes)
        {
            size = math::Max(size / 2, MIN_MESH_LIGHT_SHADOW_FACE_TEXELS);
        }
    }
    // Power-of-two squares taken largest first along a Morton curve of the smallest slots each start on a multiple of
    // their own cell count, so each is an aligned square and none overlap.
    Array<FaceSlot> slots;
    for (usize caster = 0; caster < inoutCasters.GetSize(); ++caster)
    {
        for (uint32 face = 0; face < MESH_LIGHT_SHADOW_FACE_COUNT; ++face)
        {
            if ((inoutCasters[caster].FaceMask & (1U << face)) != 0 &&
                slots.TryAdd({sizes[caster], static_cast<uint32>(caster), face}) == false)
            {
                return false;
            }
        }
    }
    Array<FaceSlot> scratch;
    if (scratch.TryResize(slots.GetSize()) == false ||
        StableSort(slots.GetData(), scratch.GetData(), slots.GetSize(), LargerSlotFirst{}) == false)
    {
        return false;
    }
    Array<MeshLightShadowCaster> casters;
    if (casters.TryAddRange({inoutCasters.GetData(), inoutCasters.GetSize()}) == false)
    {
        return false;
    }
    uint32 cursor = 0;
    for (const FaceSlot& slot : slots)
    {
        const uint32 cellsPerSide = slot.Texels / MIN_MESH_LIGHT_SHADOW_FACE_TEXELS;
        casters[slot.Caster].FaceOrigins[slot.Face] = {GetMortonAxis(cursor, 0) * MIN_MESH_LIGHT_SHADOW_FACE_TEXELS,
                                                       GetMortonAxis(cursor, 1) * MIN_MESH_LIGHT_SHADOW_FACE_TEXELS};
        cursor += cellsPerSide * cellsPerSide;
    }
    for (usize caster = 0; caster < casters.GetSize(); ++caster)
    {
        casters[caster].FaceTexels = sizes[caster];
    }
    inoutCasters = Move(casters);
    return true;
}

Status AppendRasterMeshLightShadowTable(Array<math::Vector4>& inoutWords,
                                        BorrowedView<const MeshLightShadowCaster> casters,
                                        uint32 atlasTexels,
                                        uint32& outTableWord) noexcept
{
    const usize tableWord = inoutWords.GetSize();
    if (inoutWords.TryEnsureCapacity(tableWord + MESH_LIGHT_SHADOW_HEADER_WORDS +
                                     casters.GetSize() * MESH_LIGHT_SHADOW_CASTER_WORDS) == false)
    {
        return Status::OutOfMemory;
    }
    // Reserved above, so the appends below cannot fail.
    static_cast<void>(inoutWords.TryAdd({static_cast<float32>(casters.GetSize()),
                                         static_cast<float32>(atlasTexels),
                                         1.0F / static_cast<float32>(atlasTexels),
                                         0}));
    for (usize index = 0; index < casters.GetSize(); ++index)
    {
        const MeshLightShadowCaster& caster = casters[index];
        const math::Vector3 positionWorld = caster.PositionWorld;
        static_cast<void>(inoutWords.TryAdd({positionWorld.X, positionWorld.Y, positionWorld.Z, caster.RangeMeters}));
        static_cast<void>(inoutWords.TryAdd({caster.LightRadiusMeters,
                                             caster.NearMeters / caster.RangeMeters,
                                             static_cast<float32>(caster.FaceTexels),
                                             GetRasterMeshLightShadowTangent(caster.FaceTexels)}));
        // A face that receives no light is never rendered; (-1, -1) marks it for the resolve.
        const auto getOrigin = [&caster](uint32 face) noexcept -> math::Vector2 {
            const bool isRendered = (caster.FaceMask & (1U << face)) != 0;
            return isRendered == true ? math::Vector2{static_cast<float32>(caster.FaceOrigins[face].X),
                                                      static_cast<float32>(caster.FaceOrigins[face].Y)}
                                      : math::Vector2{-1.0F, -1.0F};
        };
        // Two faces' origins per Float4 word.
        for (uint32 face = 0; face < MESH_LIGHT_SHADOW_FACE_COUNT; face += 2)
        {
            const math::Vector2 first = getOrigin(face);
            const math::Vector2 second = getOrigin(face + 1);
            static_cast<void>(inoutWords.TryAdd({first.X, first.Y, second.X, second.Y}));
        }
    }
    outTableWord = static_cast<uint32>(tableWord);
    return Status::Ready;
}

float32 GetRasterMeshLightShadowTangent(uint32 faceTexels) noexcept
{
    const float32 interior = static_cast<float32>(faceTexels) - 2.0F * MESH_LIGHT_SHADOW_GUTTER_TEXELS;
    return interior > 0 ? static_cast<float32>(faceTexels) / interior : 1.0F;
}

Status MakeRasterMeshLightShadowFaceToClip(const MeshLightShadowCaster& caster,
                                           uint32 face,
                                           math::Matrix4& outWorldToClip) noexcept
{
    if (face >= MESH_LIGHT_SHADOW_FACE_COUNT || (caster.NearMeters > 0) == false ||
        (caster.RangeMeters > caster.NearMeters) == false)
    {
        return Status::InvalidDescription;
    }
    const math::Vector3 eyeWorld = caster.PositionWorld;
    const float32 fieldOfView = 2.0F * math::Atan2(GetRasterMeshLightShadowTangent(caster.FaceTexels), 1.0F);
    math::Matrix4 worldToView;
    math::Matrix4 viewToClip;
    if (math::TryLookAt(eyeWorld, eyeWorld + FACE_BASES[face].ForwardWorld, FACE_UP_HINTS[face], worldToView) !=
            math::MathStatus::Success ||
        math::TryPerspectiveReverseZ(fieldOfView, 1, caster.NearMeters, caster.RangeMeters, viewToClip) !=
            math::MathStatus::Success)
    {
        return Status::InvalidDescription;
    }
    // Conventional depth, as BuildPointShadowMatrix converts its reverse-Z projection.
    for (math::Vector4& column : viewToClip.Columns)
    {
        column.Z = column.W - column.Z;
    }
    outWorldToClip = viewToClip * worldToView;
    return Status::Ready;
}

bool LocateRasterMeshLightShadowTexel(const MeshLightShadowCaster& caster,
                                      math::Vector3 positionWorld,
                                      uint32& outFace,
                                      math::Vector2& outTexel,
                                      float32& outDepth) noexcept
{
    const math::Vector3 deltaWorld = positionWorld - caster.PositionWorld;
    const float32 distance = math::Length(deltaWorld);
    if ((distance > 0) == false || (caster.RangeMeters > caster.NearMeters) == false)
    {
        return false;
    }
    const StaticArray<float32, AXIS_COUNT> magnitudes{
        {math::Abs(deltaWorld.X), math::Abs(deltaWorld.Y), math::Abs(deltaWorld.Z)}};
    const StaticArray<float32, AXIS_COUNT> components{{deltaWorld.X, deltaWorld.Y, deltaWorld.Z}};
    uint32 axis = 0;
    for (uint32 candidate = 1; candidate < AXIS_COUNT; ++candidate)
    {
        axis = magnitudes[candidate] > magnitudes[axis] ? candidate : axis;
    }
    const uint32 face = 2 * axis + (components[axis] < 0 ? 1U : 0U);
    const FaceBasis& basis = FACE_BASES[face];
    const float32 forward = math::Dot(deltaWorld, basis.ForwardWorld);
    const float32 tangent = GetRasterMeshLightShadowTangent(caster.FaceTexels);
    const float32 ndcX = math::Dot(deltaWorld, basis.RightWorld) / (forward * tangent);
    const float32 ndcY = math::Dot(deltaWorld, basis.UpWorld) / (forward * tangent);
    const float32 texels = static_cast<float32>(caster.FaceTexels);
    outFace = face;
    outTexel = {static_cast<float32>(caster.FaceOrigins[face].X) + (ndcX * 0.5F + 0.5F) * texels,
                static_cast<float32>(caster.FaceOrigins[face].Y) + (0.5F - ndcY * 0.5F) * texels};
    // Conventional depth of the face projection: far * (z - near) / (z * (far - near)) for forward distance z.
    const float32 nearMeters = caster.NearMeters;
    const float32 farMeters = caster.RangeMeters;
    outDepth = farMeters * (forward - nearMeters) / (forward * (farMeters - nearMeters));
    return true;
}

uint32 EncodeRasterMeshLightShadowDepth(float32 depth) noexcept
{
    const float32 clamped = math::Clamp(math::IsFinite(depth) == true ? depth : 1.0F, 0.0F, 1.0F);
    // Round to nearest as renderer_shadow.slang does: floor(depth * 65535 + 0.5).
    return static_cast<uint32>(
        math::Floor(static_cast<float64>(clamped) * static_cast<float64>(DEPTH_CODE_MAXIMUM) + 0.5));
}

float32 DecodeRasterMeshLightShadowDepth(uint32 code) noexcept
{
    return static_cast<float32>(code) / DEPTH_CODE_MAXIMUM;
}
} // namespace ludus::graphics::renderer::internal
