#include "internal/directional_near_cascade.h"

#include "internal/camera_inverse.h"

#include <ludus/foundation/math/scalar.hpp>
#include <ludus/graphics/renderer/renderer.hpp>

namespace ludus::graphics::renderer::internal
{
using namespace foundation;
namespace
{
constexpr uint32 INTERIOR_TEXELS = DIRECTIONAL_NEAR_TILE_TEXELS - 2;
constexpr float32 METRES_PER_TEXEL = 0.09375F;
constexpr float32 RADIUS_METRES = INTERIOR_TEXELS * METRES_PER_TEXEL * 0.5F;
// Snap in whole groups of texels. The slice fits even at the maximum centre displacement.
constexpr float32 SNAP_METRES = 32 * METRES_PER_TEXEL;
constexpr float32 FILTER_GUARD_METRES = 2 * METRES_PER_TEXEL;
constexpr usize HOMOGENEOUS_ROW = 3;
math::Vector3 GetAxis(math::Vector4 row) noexcept
{
    return {row.X, row.Y, row.Z};
}
float32 SnapCenter(float32 metres) noexcept
{
    return static_cast<float32>(math::Floor(static_cast<float64>(metres) / static_cast<float64>(SNAP_METRES) + 0.5)) *
           SNAP_METRES;
}
rhi::RasterRectangle GetTile(uint32 slot) noexcept
{
    return {MESH_LIGHT_SHADOW_ATLAS_TEXELS - (1 + slot) * DIRECTIONAL_NEAR_TILE_TEXELS + 1,
            MESH_LIGHT_SHADOW_ATLAS_TEXELS - DIRECTIONAL_NEAR_TILE_TEXELS + 1,
            INTERIOR_TEXELS,
            INTERIOR_TEXELS};
}
} // namespace
DirectionalNearFootprint GetDirectionalNearFootprint(const ViewDescription& view) noexcept
{
    math::Matrix4 clipToEye;
    const math::Vector3 depthAxisWorld = GetAxis(math::Transpose(view.WorldToClip).Columns[HOMOGENEOUS_ROW]);
    // The resolve reconstructs forward distance from homogeneous W. Orthographic and non-metric projections
    // keep their whole-scene map.
    constexpr float32 UNIT_AXIS_TOLERANCE = 0.0001F;
    if (view.OrthographicCamera == true || math::Abs(math::Length(depthAxisWorld) - 1) > UNIT_AXIS_TOLERANCE ||
        TryMakeCameraClipToEye(view, clipToEye) == false)
    {
        return {};
    }
    constexpr uint32 CORNER_COUNT = 4;
    for (uint32 corner = 0; corner < CORNER_COUNT; ++corner)
    {
        const math::Vector4 pointEye =
            clipToEye * math::Vector4{(corner & 1U) == 0 ? -1.F : 1.F, (corner & 2U) == 0 ? -1.F : 1.F, 0.5F, 1};
        const math::Vector3 rayWorld = GetAxis(pointEye);
        const float32 depth = math::Dot(depthAxisWorld, rayWorld);
        if (depth <= 0 || math::Length(rayWorld) * DIRECTIONAL_NEAR_END_METRES >
                              depth * (RADIUS_METRES - SNAP_METRES * 0.5F - FILTER_GUARD_METRES))
        {
            return {};
        }
    }
    return {view.CameraPosition, true};
}
bool TryFitDirectionalNearCascade(const ShadowRegion& whole,
                                  const math::Aabb3& boundsWorld,
                                  const DirectionalNearFootprint& footprint,
                                  uint32 slot,
                                  ShadowRegion& outNear) noexcept
{
    if (footprint.IsSupported == false || slot >= MAX_DIRECTIONAL_SHADOW_REGIONS)
    {
        return false;
    }
    // Stabilized sphere and texel snapping, following Michal Valient, "Rendering Technology of Killzone 2",
    // https://www.guerrilla-games.com/media/News/Files/GDC09_Valient_Rendering_Technology_Of_KillZone_2_Extended_Presenter_Notes.pdf
    // GDC 2009 (shadow maps). Centre the sphere at the eye so turns preserve the entire XY lattice.
    const math::Matrix4 rows = math::Transpose(whole.WorldToClip);
    math::Vector3 rightWorld = GetAxis(rows.Columns[0]);
    math::Vector3 upWorld = GetAxis(rows.Columns[1]);
    math::Vector3 forwardWorld = GetAxis(rows.Columns[2]);
    const float32 coarseScale = math::Min(math::Length(rightWorld), math::Length(upWorld));
    if (coarseScale * static_cast<float32>(whole.Interior.Width) * METRES_PER_TEXEL >= 2 ||
        math::TryNormalize(rightWorld, rightWorld) != math::MathStatus::Success ||
        math::TryNormalize(upWorld, upWorld) != math::MathStatus::Success ||
        math::TryNormalize(forwardWorld, forwardWorld) != math::MathStatus::Success)
    {
        return false;
    }
    const float32 x = SnapCenter(math::Dot(footprint.CenterWorld, rightWorld));
    const float32 y = SnapCenter(math::Dot(footprint.CenterWorld, upWorld));
    const math::Vector3 originWorld = rightWorld * x + upWorld * y;
    const float32 depthScale = math::Length(GetAxis(rows.Columns[2]));
    float32 minimum = -rows.Columns[2].W / depthScale;
    float32 maximum = minimum + 1 / depthScale;
    // A slab bounds the projection of every World point in the extruded XY square.
    // The bounds of a flat kilometre-scale landscape can otherwise consume all 16 depth bits.
    const auto clipSlab = [&](float32 math::Vector3::* component) {
        const float32 direction = forwardWorld.*component;
        if (direction != 0)
        {
            const float32 pad = RADIUS_METRES * (math::Abs(rightWorld.*component) + math::Abs(upWorld.*component));
            const float32 origin = originWorld.*component;
            const float32 lower = (boundsWorld.Min.*component - origin - pad) / direction;
            const float32 upper = (boundsWorld.Max.*component - origin + pad) / direction;
            minimum = math::Max(minimum, math::Min(lower, upper));
            maximum = math::Min(maximum, math::Max(lower, upper));
        }
    };
    clipSlab(&math::Vector3::X);
    clipSlab(&math::Vector3::Y);
    clipSlab(&math::Vector3::Z);
    if (math::IsFinite(minimum) == false || math::IsFinite(maximum) == false || minimum > maximum)
    {
        return false;
    }
    minimum -= METRES_PER_TEXEL;
    maximum += METRES_PER_TEXEL;
    const float32 inverseDepth = 1 / (maximum - minimum);
    const auto makeRow = [](math::Vector3 axis, float32 origin, float32 scale) {
        return math::Vector4{axis.X * scale, axis.Y * scale, axis.Z * scale, -origin * scale};
    };
    math::Matrix4 nearRows = rows;
    nearRows.Columns[0] = makeRow(rightWorld, x, 1 / RADIUS_METRES);
    nearRows.Columns[1] = makeRow(upWorld, y, 1 / RADIUS_METRES);
    nearRows.Columns[2] = makeRow(forwardWorld, minimum, inverseDepth);
    outNear = whole;
    outNear.WorldToClip = math::Transpose(nearRows);
    outNear.Interior = GetTile(slot);
    constexpr float32 DEPTH_CODE_MAXIMUM = 65535;
    outNear.Bias = 2 / DEPTH_CODE_MAXIMUM;
    return true;
}
bool HasDirectionalNearSpace(BorrowedView<const MeshLightShadowCaster> casters, uint32 lightCount) noexcept
{
    for (const MeshLightShadowCaster& caster : casters)
    {
        for (uint32 face = 0; face < MESH_LIGHT_SHADOW_FACE_COUNT; ++face)
        {
            const MeshLightShadowFaceOrigin origin = caster.FaceOrigins[face];
            if ((caster.FaceMask & (1U << face)) != 0 &&
                origin.Y + caster.FaceTexels > MESH_LIGHT_SHADOW_ATLAS_TEXELS - DIRECTIONAL_NEAR_TILE_TEXELS &&
                origin.X + caster.FaceTexels >
                    MESH_LIGHT_SHADOW_ATLAS_TEXELS - lightCount * DIRECTIONAL_NEAR_TILE_TEXELS)
            {
                return false;
            }
        }
    }
    return true;
}
} // namespace ludus::graphics::renderer::internal
