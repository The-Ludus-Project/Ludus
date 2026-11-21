#pragma once

#include <ludus/foundation/base/types.h>

#include <ludus/foundation/math/vector.hpp>

// Independent binary64 quadrature of the Earth clear-sky profile, shared by the GPU atmosphere
// pixel tests and the CPU reference-sky tests. It integrates planet-centred intersections and
// each optical depth directly, without the production tables' parameterization.
namespace ludus::graphics::renderer::testing
{
/// Single-scattered radiance per unit solar illuminance and the fraction scattered back toward the ray.
struct EarthSkyIntegral
{
    foundation::float64 Radiance = 0;
    foundation::float64 ReturnFraction = 0;
    /// Number of view quadrature points with the solar ray occluded by solid Earth.
    foundation::usize ShadowSamples = 0;
    /// Transmittance of the integrated view segment.
    foundation::float64 Transmittance = 1;
};

/// Extinction per kilometre in one RGB channel (0..2) at an altitude in kilometres.
foundation::float64 ComputeEarthExtinction(foundation::float64 altitude, foundation::usize channel);

/// Single scattering toward an observer at altitude (kilometres) from a unit direction, Sun in sunWorld, +Y up.
/// Outside the shell only its forward chord contributes. A miss or an outward ray gives zero.
EarthSkyIntegral IntegrateEarthSky(foundation::float64 altitude,
                                   foundation::math::Vector3d directionWorld,
                                   foundation::math::Vector3d sunWorld,
                                   foundation::usize channel,
                                   foundation::usize viewSteps = 768,
                                   foundation::usize solarSteps = 384);

/// Single scattering along the part of a ray inside the shell, from observerKilometres (planet-centred, kilometres)
/// up to maximumKilometres along a unit direction. IntegrateEarthSky is this ray from (0, ground + altitude, 0).
EarthSkyIntegral IntegrateEarthSkySegment(foundation::math::Vector3d observerKilometres,
                                          foundation::math::Vector3d directionWorld,
                                          foundation::math::Vector3d sunWorld,
                                          foundation::usize channel,
                                          foundation::float64 maximumKilometres,
                                          foundation::usize viewSteps = 768,
                                          foundation::usize solarSteps = 384);

/// Transmittance from a planet-centred point (kilometres) toward the Sun to the top of the shell, written to
/// outTransmittance. False, leaving it unchanged, when solid Earth blocks the Sun.
bool ComputeEarthSolarTransmittance(foundation::math::Vector3d pointKilometres,
                                    foundation::math::Vector3d sunWorld,
                                    foundation::usize channel,
                                    foundation::usize solarSteps,
                                    foundation::float64& outTransmittance);

/// Higher scattering orders in blue at ground level, horizontal view and overhead Sun, under the local
/// isotropic approximation: 24 explicit orders over a dense sphere rule.
foundation::float64 IntegrateEarthBlueHigherOrders();
} // namespace ludus::graphics::renderer::testing
