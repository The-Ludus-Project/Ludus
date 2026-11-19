#pragma once

#include "raster_mesh_light_shadows.h"

namespace ludus::graphics::renderer
{
struct ViewDescription;
namespace internal
{
inline constexpr foundation::uint32 DIRECTIONAL_NEAR_TILE_TEXELS = DIRECTIONAL_SHADOW_TEXELS / 2;
/// Fixed camera-space split, with a spherical guard containing the perspective slice during turns.
/// Empty when the view cannot be covered at the declared texel density.
struct DirectionalNearFootprint final
{
    foundation::math::Vector3 CenterWorld{};
    bool IsSupported = false;
    bool CanReserveTiles = false;
};
[[nodiscard]] DirectionalNearFootprint GetDirectionalNearFootprint(const ViewDescription& view) noexcept;
/// Fit the reserved tile only when it improves on the whole-scene projection. Intersect the
/// light-space caster column with the complete World bounds, including casters behind the eye.
[[nodiscard]] bool TryFitDirectionalNearCascade(const ShadowRegion& whole,
                                                const foundation::math::Aabb3& boundsWorld,
                                                const DirectionalNearFootprint& footprint,
                                                foundation::uint32 slot,
                                                ShadowRegion& outNear) noexcept;
/// Near tiles occupy the final two half-size cells of the atlas only when mesh faces leave both available.
[[nodiscard]] bool HasDirectionalNearSpace(foundation::BorrowedView<const MeshLightShadowCaster> casters,
                                           foundation::uint32 lightCount) noexcept;
} // namespace internal
} // namespace ludus::graphics::renderer
