#pragma once
#include <ludus/foundation/base/core.h>

#include "wave_scene_queries.h"
#include <ludus/graphics/renderer/wave_tracing.hpp>

namespace ludus::graphics::renderer::internal
{
/// Most triangles a ballistic segment may ignore: the two faces of a diffracting wedge.
inline constexpr usize WAVE_MAX_EXCLUDED_TRIANGLES = 2;
/// Triangles a ballistic segment ignores, such as the faces whose shared edge the segment starts or ends on.
struct WaveExcludedTriangles final
{
    StaticArray<uint32, WAVE_MAX_EXCLUDED_TRIANGLES> Triangles{};
    uint32 Count = 0;
    /// Whether `triangleIndex` is one of the first Count entries.
    [[nodiscard]] LUDUS_INLINE bool Contains(uint32 triangleIndex) const noexcept
    {
        for (uint32 slot = 0; slot < Count; ++slot)
        {
            if (Triangles[slot] == triangleIndex)
            {
                return true;
            }
        }
        return false;
    }
};
/// Coordinates in the interaction frustum's transverse basis, in world-distance units.
struct ProjectedPointFrustum final
{
    float64 X = 0;
    float64 Y = 0;
};
/// Whether any triangle of `region`, projected along its frustum direction onto the transverse plane through the
/// frustum origin, covers `positionFrustum` with nonzero opacity. Degenerate projections cover nothing.
[[nodiscard]] bool IsProjectedPointBlocked(const ReferenceSceneData& scene,
                                           const WaveInteractionRegion& region,
                                           const ProjectedPointFrustum& positionFrustum) noexcept;
/// Outcome of escaping a connection endpoint past nearby geometry.
struct WaveOriginEscape final
{
    /// Distance from the cone origin at which the envelope is first clear; valid when neither flag is set.
    float64 DistanceWorld = 0;
    /// The centre ray was blocked inside the escaped span.
    bool IsBlocked = false;
    /// The escape cap was reached; the rest of the connection is treated as ballistic.
    bool IsEntirelyBallistic = false;
};
/// Trace the zero-radius centre ray of rayCone over [NearWorld, FarWorld], ignoring `excluded`. A covered
/// triangle blocks the ray. Ready publishes outIsBlocked; a malformed hierarchy returns InvalidDescription.
[[nodiscard]] rhi::RasterStatus TraceWaveBallistic(const ReferenceSceneData& scene,
                                                   math::EllipticalCone rayCone,
                                                   const WaveExcludedTriangles& excluded,
                                                   bool& outIsBlocked) noexcept;
/// Grow the distance from cone.NearWorld by LUDUS_WAVE_CONNECTION_BALLISTIC_SCALE until the envelope over the
/// next cone.NearWorld is clear of geometry, up to LUDUS_WAVE_CONNECTION_MAX_BALLISTIC_WAVELENGTHS wavelengths.
/// shouldTraceBallistic also tests the centre ray across the skipped span and stops when it is blocked.
/// Returns Unsupported after LUDUS_WAVE_CONNECTION_ESCAPE_ATTEMPTS attempts.
[[nodiscard]] rhi::RasterStatus EscapeWaveOrigin(const ReferenceSceneData& scene,
                                                 const math::EllipticalCone& cone,
                                                 float64 wavelengthWorld,
                                                 bool shouldTraceBallistic,
                                                 WaveOriginEscape& outEscape) noexcept;
/// Coherent scalar Gaussian-mode connection through the current scene. Source emits a Gaussian waist
/// with Sigma = distance / wavenumber; detector samples the field on the mean axis. Geometry in each
/// interaction frustum is projected along propagation onto its midpoint screen. Sequential angular-spectrum
/// propagation accounts for interference across all open cells and successive regions; surfaces are opaque
/// according to their fixed-mip coverage. Returns field divided by the unobstructed analytic Gaussian field.
/// No reflection or polarization is performed here. Caller supplies a finite unit direction, positive metres,
/// receiverWorld retains the independently offset receiver for ballistic visibility; direction/distance define
/// the propagation axis. wavelength/distance <= 1/32 and valid scene data. Failure preserves outField. No
/// allocation/shared state.
[[nodiscard]] rhi::RasterStatus EvaluateWaveConnectionToReceiver(const ReferenceSceneData& scene,
                                                                 math::Vector3d sourceWorld,
                                                                 math::Vector3d receiverWorld,
                                                                 math::Vector3d directionWorld,
                                                                 float64 distanceWorld,
                                                                 float64 wavelengthWorld,
                                                                 WaveField& outField) noexcept;

[[nodiscard]] rhi::RasterStatus EvaluateWaveConnection(const ReferenceSceneData& scene,
                                                       math::Vector3d sourceWorld,
                                                       math::Vector3d directionWorld,
                                                       float64 distanceWorld,
                                                       float64 wavelengthWorld,
                                                       WaveField& outField) noexcept;
} // namespace ludus::graphics::renderer::internal
