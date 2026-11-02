#pragma once

#include <ludus/foundation/base/core.h>
#include <ludus/foundation/containers/borrowed_view.hpp>
#include <ludus/foundation/containers/static_array.hpp>
#include <ludus/foundation/math/vector.hpp>
#include <ludus/graphics/renderer/reference.hpp>

// Independent CPU photon transport/density oracle for the reflective FG+ subset.
// Thanks to Kern, Brull, Kastning and Grosch, "Guided ReSTIR FG+: Photon Resampling for
// Large Scenes and Many Lights", EGSR 2026, sections 3-4, https://doi.org/10.2312/sr.20261001 .
// Geometry/intersection and estimator assembly are independent of the GPU hash grid.
// The existing CPU reference BSDF is shared as the declared material model.
namespace ludus::graphics::renderer::testing
{
inline constexpr foundation::usize FG_ORACLE_DEPTH = 4;
struct FgOracleTriangle final
{
    foundation::math::Vector3d AWorld;
    foundation::math::Vector3d BWorld;
    foundation::math::Vector3d CWorld;
    ReferenceBsdf Material;
};
struct FgOraclePhoton final
{
    foundation::math::Vector3d PositionWorld;
    foundation::math::Vector3d NormalWorld;
    foundation::math::Vector3d IncomingWorld;
    foundation::math::Vector3d Flux;
    bool IsValid = false;
    bool IsCaustic = false;
};
using FgOraclePhotons = foundation::StaticArray<FgOraclePhoton, FG_ORACLE_DEPTH>;
using FgOracleSamples = foundation::StaticArray<foundation::math::Vector3, FG_ORACLE_DEPTH>;
/// Trace at most four reflection events, recording diffuse endpoints before scattering.
/// Inputs are borrowed for the call; no allocation. Unsupported coatings/invalid BSDFs
/// return false and preserve output. Direction is unit length; samples are in [0,1).
[[nodiscard]] bool TraceFgOraclePhotons(foundation::BorrowedView<const FgOracleTriangle> triangles,
                                        foundation::math::Vector3d originWorld,
                                        foundation::math::Vector3d directionWorld,
                                        foundation::math::Vector3d flux,
                                        const FgOracleSamples& samples,
                                        FgOraclePhotons& outPhotons) noexcept;
/// Brute-force disc density estimate, Eq. 4; onlyCaustics restricts the path class.
[[nodiscard]] foundation::math::Vector3d GatherFgOraclePhotons(foundation::BorrowedView<const FgOraclePhoton> photons,
                                                               foundation::math::Vector3d positionWorld,
                                                               foundation::math::Vector3d normalWorld,
                                                               foundation::math::Vector3d viewDirectionWorld,
                                                               const ReferenceBsdf& material,
                                                               foundation::float64 radiusWorld,
                                                               bool shouldGatherOnlyCaustics) noexcept;
/// Exact reciprocal proposal for integer direction quotas plus uniform leftovers.
[[nodiscard]] foundation::float64 ComputeFgOracleFluxScale(foundation::uint32 cellPaths,
                                                           foundation::uint32 uniformPaths) noexcept;
/// Solid-angle reconnection Jacobian, returning zero on degenerate geometry.
[[nodiscard]] foundation::float64 ComputeFgOracleJacobian(foundation::math::Vector3d oldOffsetWorld,
                                                          foundation::math::Vector3d newOffsetWorld,
                                                          foundation::math::Vector3d normalWorld) noexcept;
/// Two-strategy balance MIS in the common domain; zero when both targets vanish.
[[nodiscard]] foundation::float64 ComputeFgOraclePairWeight(foundation::float64 confidence,
                                                            foundation::float64 otherConfidence,
                                                            foundation::float64 target,
                                                            foundation::float64 otherTarget) noexcept;
} // namespace ludus::graphics::renderer::testing
