#pragma once
#include <ludus/foundation/base/core.h>

#include "wave_edge_diffraction.h"

// Fraunhofer edge-diffraction connection operator of the wave-tracing profile. See
// wave_fraunhofer_diffraction.cpp for the declared field, attribution and limits.
namespace ludus::graphics::renderer::internal
{
/// A connection's paraxial Gaussian beam: a waist at the source with SigmaWorldSquared = DistanceWorld /
/// Wavenumber, observed on the axis DistanceWorld away, as the angular-spectrum operator models it.
struct FraunhoferBeam final
{
    /// 2 pi / wavelength, in radians per metre.
    float64 Wavenumber = 0;
    float64 SigmaWorldSquared = 0;
    float64 DistanceWorld = 0;
};
/// One straight boundary edge of an obstacle projected along the connection axis, in the connection frame.
struct FraunhoferEdge final
{
    ProjectedPointFrustum BeginFrustum{};
    ProjectedPointFrustum EndFrustum{};
    /// Distances of the edge's ends along the axis from the source, each in (0, DistanceWorld).
    float64 BeginDepthWorld = 0;
    float64 EndDepthWorld = 0;
    /// The obstacle lies to the left of the edge, seen along the axis from begin to end; otherwise to the right.
    bool IsObstacleLeft = false;
};
/// Field that one boundary edge adds at the receiver, relative to the unobstructed beam: the edge's Kirchhoff
/// boundary-diffraction wave, evaluated over graded segments as angle-weighted Fraunhofer edge waves. The edge is
/// clipped to the beam's envelope radius. isAxisBlocked decides the edge's half-turn when it passes exactly
/// through the axis. Returns Unsupported when a side needs more than LUDUS_WAVE_FRAUNHOFER_SIDE_SEGMENTS segments
/// and InvalidDescription for a nonpositive beam, a depth outside (0, DistanceWorld) or a non-finite field;
/// failure preserves outField.
[[nodiscard]] rhi::RasterStatus EvaluateFraunhoferEdgeField(const FraunhoferEdge& edge,
                                                            const FraunhoferBeam& beam,
                                                            bool isAxisBlocked,
                                                            WaveField& outField) noexcept;
/// Fraunhofer connection through the current scene: the ballistic field (one when the straight segment is
/// clear, otherwise zero) plus, coherently, the boundary-diffraction waves of every interaction region's projected
/// obstacle whose stationary points the source and receiver both see. Fields are relative to the unobstructed
/// Gaussian beam. Accepts the same inputs as EvaluateWaveConnectionToReceiver and fails the same way: invalid
/// input returns InvalidDescription, a full interaction region CapacityExceeded, and too many regions, escape
/// attempts or edge segments Unsupported. Failure preserves outField. No allocation or shared state.
[[nodiscard]] rhi::RasterStatus EvaluateFraunhoferConnectionToReceiver(const ReferenceSceneData& scene,
                                                                       math::Vector3d sourceWorld,
                                                                       math::Vector3d receiverWorld,
                                                                       math::Vector3d directionWorld,
                                                                       float64 distanceWorld,
                                                                       float64 wavelengthWorld,
                                                                       WaveField& outField) noexcept;
} // namespace ludus::graphics::renderer::internal
