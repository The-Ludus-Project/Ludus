#include "shadow_occlusion_oracle.h"

#include <ludus/foundation/math/scalar.hpp>
#include <ludus/foundation/math/vector.hpp>

namespace ludus::graphics::renderer::testing
{
using namespace foundation;
namespace math = foundation::math;
namespace
{
/// An occluder covers the emitter when the uncovered part is below this fraction of the emitter's projected solid
/// angle: float64 rounding of the clip, far below one part in 2^24 of a GPU estimate.
constexpr float64 COVERED_FRACTION_TOLERANCE = 1e-9;

/// Twice the vector area of a planar polygon; its direction is the normal its winding implies.
[[nodiscard]] math::Vector3d ComputeAreaVector(const OcclusionPolygon& polygon) noexcept
{
    math::Vector3d sum{};
    for (usize i = 0; i < polygon.VertexCount; ++i)
    {
        const math::Vector3d start = polygon.VerticesWorld[i];
        const math::Vector3d end = polygon.VerticesWorld[(i + 1) % polygon.VertexCount];
        sum = sum + math::Cross(start - polygon.VerticesWorld[0], end - polygon.VerticesWorld[0]);
    }
    return sum;
}
} // namespace

float64 ComputeLambertProjectedSolidAngle(
    const OcclusionPolygon& polygon,
    // NOLINTNEXTLINE(bugprone-easily-swappable-parameters): distinct receiver point and surface normal.
    math::Vector3d receiverWorld,
    math::Vector3d normalWorld) noexcept
{
    // Each edge contributes its angle seen from the receiver times the normal cosine of the plane it spans with the
    // receiver; the sum's sign follows the winding.
    float64 sum = 0;
    for (usize i = 0; i < polygon.VertexCount; ++i)
    {
        const math::Vector3d start = polygon.VerticesWorld[i] - receiverWorld;
        const math::Vector3d end = polygon.VerticesWorld[(i + 1) % polygon.VertexCount] - receiverWorld;
        const math::Vector3d edgeNormal = math::Cross(start, end);
        const float64 edgeNormalLength = math::Length(edgeNormal);
        if (edgeNormalLength > 0)
        {
            sum += math::Atan2(edgeNormalLength, math::Dot(start, end)) * math::Dot(normalWorld, edgeNormal) /
                   edgeNormalLength;
        }
    }
    return math::Abs(sum) / 2;
}

// NOLINTNEXTLINE(bugprone-easily-swappable-parameters): distinct subject and clip polygons.
bool TryClipConvexPolygon(const OcclusionPolygon& subject,
                          const OcclusionPolygon& clip,
                          OcclusionPolygon& outPolygon) noexcept
{
    const math::Vector3d clipArea = ComputeAreaVector(clip);
    if (clip.VertexCount < 3 || math::LengthSquared(clipArea) <= 0)
    {
        return false;
    }
    OcclusionPolygon result = subject;
    for (usize edge = 0; edge < clip.VertexCount && result.VertexCount > 0; ++edge)
    {
        const math::Vector3d edgeStart = clip.VerticesWorld[edge];
        const math::Vector3d edgeEnd = clip.VerticesWorld[(edge + 1) % clip.VertexCount];
        // Positive on the clip polygon's side of the edge, for either winding.
        const math::Vector3d inward = math::Cross(clipArea, edgeEnd - edgeStart);
        OcclusionPolygon kept;
        for (usize i = 0; i < result.VertexCount; ++i)
        {
            const math::Vector3d start = result.VerticesWorld[i];
            const math::Vector3d end = result.VerticesWorld[(i + 1) % result.VertexCount];
            const float64 startDistance = math::Dot(inward, start - edgeStart);
            const float64 endDistance = math::Dot(inward, end - edgeStart);
            if (startDistance >= 0)
            {
                if (kept.VertexCount == OCCLUSION_POLYGON_CAPACITY)
                {
                    return false;
                }
                kept.VerticesWorld[kept.VertexCount] = start;
                ++kept.VertexCount;
            }
            if ((startDistance >= 0) != (endDistance >= 0))
            {
                if (kept.VertexCount == OCCLUSION_POLYGON_CAPACITY)
                {
                    return false;
                }
                const float64 t = startDistance / (startDistance - endDistance);
                kept.VerticesWorld[kept.VertexCount] = start + (end - start) * t;
                ++kept.VertexCount;
            }
        }
        result = kept.VertexCount >= 3 ? kept : OcclusionPolygon{};
    }
    outPolygon = result;
    return true;
}

OcclusionOracleStatus TryComputeVisibleProjectedSolidAngle(const OccludedPolygonLight& light,
                                                           math::Vector3d receiverWorld,
                                                           math::Vector3d normalWorld,
                                                           VisibleProjectedSolidAngle& outSolidAngle)
{
    const OcclusionPolygon& emitter = light.Emitter;
    // Heights from the emitter's plane enter only as ratios and signs, so any finite nonzero emitting normal works.
    if (emitter.VertexCount < 3 || math::IsFinite(receiverWorld) == false || math::IsFinite(normalWorld) == false ||
        math::LengthSquared(normalWorld) <= 0 || math::IsFinite(light.EmittingNormalWorld) == false ||
        math::LengthSquared(light.EmittingNormalWorld) <= 0 || math::LengthSquared(ComputeAreaVector(emitter)) <= 0)
    {
        return OcclusionOracleStatus::InvalidInput;
    }
    // Signed distance from the emitter's plane, positive on its emitting side.
    const auto getHeight = [&light](math::Vector3d positionWorld) {
        return math::Dot(light.EmittingNormalWorld, positionWorld - light.Emitter.VerticesWorld[0]);
    };
    const float64 receiverHeight = getHeight(receiverWorld);
    VisibleProjectedSolidAngle result;
    if (receiverHeight <= 0)
    {
        outSolidAngle = result;
        return OcclusionOracleStatus::Success;
    }
    for (usize i = 0; i < emitter.VertexCount; ++i)
    {
        if (math::Dot(normalWorld, emitter.VerticesWorld[i] - receiverWorld) <= 0)
        {
            return OcclusionOracleStatus::BelowHorizon;
        }
    }
    result.Emitter = ComputeLambertProjectedSolidAngle(emitter, receiverWorld, normalWorld);
    result.Visible = result.Emitter;
    for (usize occluderIndex = 0; occluderIndex < light.Occluders.GetSize(); ++occluderIndex)
    {
        const OcclusionPolygon& occluder = light.Occluders[occluderIndex];
        if (occluder.VertexCount < 3)
        {
            return OcclusionOracleStatus::InvalidInput;
        }
        usize betweenCount = 0;
        usize outsideCount = 0;
        for (usize i = 0; i < occluder.VertexCount; ++i)
        {
            const float64 height = getHeight(occluder.VerticesWorld[i]);
            const bool isBetween = height > 0 && height < receiverHeight;
            betweenCount += isBetween == true ? 1 : 0;
            outsideCount += isBetween == true ? 0 : 1;
        }
        if (betweenCount == 0)
        {
            continue;
        }
        if (outsideCount != 0)
        {
            return OcclusionOracleStatus::StraddlingOccluder;
        }
        // Central projection from the receiver onto the emitter's plane.
        OcclusionPolygon projected;
        projected.VertexCount = occluder.VertexCount;
        for (usize i = 0; i < occluder.VertexCount; ++i)
        {
            const math::Vector3d vertex = occluder.VerticesWorld[i];
            const float64 scale = receiverHeight / (receiverHeight - getHeight(vertex));
            projected.VerticesWorld[i] = receiverWorld + (vertex - receiverWorld) * scale;
        }
        OcclusionPolygon hidden;
        if (TryClipConvexPolygon(emitter, projected, hidden) == false)
        {
            return OcclusionOracleStatus::InvalidInput;
        }
        const float64 hiddenSolidAngle = ComputeLambertProjectedSolidAngle(hidden, receiverWorld, normalWorld);
        result.Visible -= hiddenSolidAngle;
        if (hiddenSolidAngle >= (1 - COVERED_FRACTION_TOLERANCE) * result.Emitter)
        {
            result.CoveringOccluder = occluderIndex;
        }
    }
    result.Visible = math::Max(result.Visible, 0.0);
    outSolidAngle = result;
    return OcclusionOracleStatus::Success;
}

bool DoesSegmentCrossPolygon(math::Vector3d startWorld,
                             math::Vector3d endWorld,
                             const OcclusionPolygon& polygon) noexcept
{
    const math::Vector3d area = ComputeAreaVector(polygon);
    if (polygon.VertexCount < 3 || math::LengthSquared(area) <= 0)
    {
        return false;
    }
    const float64 startHeight = math::Dot(area, startWorld - polygon.VerticesWorld[0]);
    const float64 endHeight = math::Dot(area, endWorld - polygon.VerticesWorld[0]);
    if ((startHeight > 0) == (endHeight > 0) || startHeight == endHeight)
    {
        return false;
    }
    const math::Vector3d crossingWorld =
        startWorld + (endWorld - startWorld) * (startHeight / (startHeight - endHeight));
    for (usize i = 0; i < polygon.VertexCount; ++i)
    {
        const math::Vector3d edgeStart = polygon.VerticesWorld[i];
        const math::Vector3d edgeEnd = polygon.VerticesWorld[(i + 1) % polygon.VertexCount];
        if (math::Dot(math::Cross(area, edgeEnd - edgeStart), crossingWorld - edgeStart) < 0)
        {
            return false;
        }
    }
    return true;
}

OcclusionPolygon MakeCeilingEmitter() noexcept
{
    constexpr float64 HALF_WIDTH_WORLD = 0.8;
    constexpr float64 MINIMUM_Z_WORLD = -0.8;
    constexpr float64 MAXIMUM_Z_WORLD = 0.2;
    OcclusionPolygon emitter;
    emitter.VerticesWorld[0] = {-HALF_WIDTH_WORLD, EMITTER_HEIGHT_WORLD, MINIMUM_Z_WORLD};
    emitter.VerticesWorld[1] = {HALF_WIDTH_WORLD, EMITTER_HEIGHT_WORLD, MINIMUM_Z_WORLD};
    emitter.VerticesWorld[2] = {HALF_WIDTH_WORLD, EMITTER_HEIGHT_WORLD, MAXIMUM_Z_WORLD};
    emitter.VerticesWorld[3] = {-HALF_WIDTH_WORLD, EMITTER_HEIGHT_WORLD, MAXIMUM_Z_WORLD};
    emitter.VertexCount = 4;
    return emitter;
}

bool TryBuildLouvre(const LouvreDescription& description, Louvre& outLouvre) noexcept
{
    if (description.SlatWidthWorld <= 0 || description.PitchWorld <= description.SlatWidthWorld ||
        description.MinimumXWorld >= description.MaximumXWorld ||
        description.MinimumZWorld >= description.MaximumZWorld)
    {
        return false;
    }
    OcclusionPolygon box;
    box.VerticesWorld[0] = {description.MinimumXWorld, description.HeightWorld, description.MinimumZWorld};
    box.VerticesWorld[1] = {description.MaximumXWorld, description.HeightWorld, description.MinimumZWorld};
    box.VerticesWorld[2] = {description.MaximumXWorld, description.HeightWorld, description.MaximumZWorld};
    box.VerticesWorld[3] = {description.MinimumXWorld, description.HeightWorld, description.MaximumZWorld};
    box.VertexCount = 4;
    constexpr float64 DEGREES_PER_HALF_TURN = 180;
    const float64 angle = description.AngleDegrees * math::kPi / DEGREES_PER_HALF_TURN;
    const math::Vector3d along{math::Cos(angle), 0, math::Sin(angle)};
    const math::Vector3d across{-along.Z, 0, along.X};
    const math::Vector3d center{(description.MinimumXWorld + description.MaximumXWorld) / 2,
                                description.HeightWorld,
                                (description.MinimumZWorld + description.MaximumZWorld) / 2};
    // Every strip reaches past the box on both ends.
    const float64 halfLength = math::Length(box.VerticesWorld[2] - box.VerticesWorld[0]);
    const isize stripLimit = static_cast<isize>(halfLength / description.PitchWorld) + 1;
    Louvre louvre;
    for (isize strip = -stripLimit; strip <= stripLimit; ++strip)
    {
        const float64 leading = static_cast<float64>(strip) * description.PitchWorld;
        const float64 trailing = leading + description.SlatWidthWorld;
        OcclusionPolygon slat;
        slat.VerticesWorld[0] = center + along * -halfLength + across * leading;
        slat.VerticesWorld[1] = center + along * halfLength + across * leading;
        slat.VerticesWorld[2] = center + along * halfLength + across * trailing;
        slat.VerticesWorld[3] = center + along * -halfLength + across * trailing;
        slat.VertexCount = 4;
        OcclusionPolygon clipped;
        if (TryClipConvexPolygon(slat, box, clipped) == false)
        {
            return false;
        }
        if (clipped.VertexCount == 0)
        {
            continue;
        }
        if (louvre.SlatCount == LOUVRE_SLAT_CAPACITY)
        {
            return false;
        }
        louvre.Slats[louvre.SlatCount] = clipped;
        ++louvre.SlatCount;
    }
    outLouvre = louvre;
    return true;
}
} // namespace ludus::graphics::renderer::testing
