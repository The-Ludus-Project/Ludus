#pragma once

#include <ludus/foundation/base/types.h>

#include <ludus/foundation/containers/array.hpp>
#include <ludus/foundation/containers/borrowed_view.hpp>
#include <ludus/foundation/math/vector.hpp>
#include <ludus/graphics/renderer/reference.hpp>
#include <ludus/graphics/rhi/raster.h>

// Bounded homogeneous absorbing medium of the RT1 reference tracers. The CPU oracle and
// shaders/reference_medium.slang evaluate the same ordered boundary crossings with the same
// float32 arithmetic; see ReferenceMediumDescription for the declared contract.
namespace ludus::graphics::renderer::internal
{
/// Admitted copy of a capture's medium. An empty corner list means vacuum.
struct ReferenceMediumData final
{
    /// World-space boundary corners, three per triangle, in the caller's order.
    foundation::core::Array<foundation::math::Vector3> CornersWorld;
    /// Linear RGB absorption coefficient per world metre.
    foundation::math::Vector3 AbsorptionPerWorld{};
};
/// A ray or segment origin and its unit direction, in world space.
struct ReferenceMediumRay final
{
    foundation::math::Vector3 OriginWorld{};
    foundation::math::Vector3 DirectionWorld{};
};
/// Report whether a capture carries a medium.
[[nodiscard]] inline bool HasReferenceMedium(const ReferenceMediumData& medium) noexcept
{
    return medium.CornersWorld.GetSize() != 0;
}
/// Transient bytes ValidateReferenceMedium allocates for a boundary of `triangles` triangles.
[[nodiscard]] foundation::usize GetReferenceMediumValidationBytes(foundation::usize triangles) noexcept;
/// Validate a borrowed medium: triangle count, finite bounded corners with positive area, a closed and
/// consistently oriented boundary, and finite bounded absorption. Returns InvalidDescription for a
/// contract violation, OutOfMemory when the edge scratch cannot be allocated, otherwise Ready.
[[nodiscard]] rhi::RasterStatus ValidateReferenceMedium(const ReferenceMediumDescription& medium) noexcept;
/// Copy a validated medium. Failure (OutOfMemory) leaves outMedium empty.
[[nodiscard]] rhi::RasterStatus CopyReferenceMedium(const ReferenceMediumDescription& medium,
                                                    ReferenceMediumData& outMedium) noexcept;
/// Compare a retained medium with a borrowed description, or with vacuum when mediumOrNull is null.
[[nodiscard]] bool IsSameReferenceMedium(const ReferenceMediumData& retained,
                                         const ReferenceMediumDescription* mediumOrNull) noexcept;
/// Float32 intersection of a ray with one boundary triangle: Woop, Benthin and Wald's watertight test with a
/// tie rule that assigns a ray through a shared edge or corner to exactly one of the triangles meeting there.
/// On a hit at a positive distance, writes it and returns true.
[[nodiscard]] bool IntersectReferenceMediumTriangle(const foundation::math::Vector3* cornersWorld,
                                                    const ReferenceMediumRay& ray,
                                                    foundation::float32& outDistanceWorld) noexcept;
/// Attenuate one segment of the ray over [0, lengthWorld] through the medium.
/// inoutInside holds the inside count (0 or 1) at the origin; when shouldDeriveStart is true it is
/// replaced first by the count implied by every crossing along the whole ray. On success writes the
/// Beer-Lambert transmittance and the inside count at the segment end. Returns false, preserving both
/// outputs, when either count leaves {0, 1}.
[[nodiscard]] bool AttenuateReferenceMediumSegment(const ReferenceMediumData& medium,
                                                   const ReferenceMediumRay& ray,
                                                   foundation::float32 lengthWorld,
                                                   bool shouldDeriveStart,
                                                   foundation::uint32& inoutInside,
                                                   foundation::math::Vector3& outTransmittance) noexcept;
} // namespace ludus::graphics::renderer::internal
