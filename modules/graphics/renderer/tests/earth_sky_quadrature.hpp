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
};

/// Extinction per kilometre in one RGB channel (0..2) at an altitude in kilometres.
foundation::float64 ComputeEarthExtinction(foundation::float64 altitude, foundation::usize channel);

/// Single scattering toward an observer at altitude (kilometres) from a unit direction, Sun in sunWorld, +Y up.
EarthSkyIntegral IntegrateEarthSky(foundation::float64 altitude,
                                   foundation::math::Vector3d directionWorld,
                                   foundation::math::Vector3d sunWorld,
                                   foundation::usize channel,
                                   foundation::usize viewSteps = 768,
                                   foundation::usize solarSteps = 384);

/// Higher scattering orders in blue at ground level, horizontal view and overhead Sun, under the local
/// isotropic approximation: 24 explicit orders over a dense sphere rule.
foundation::float64 IntegrateEarthBlueHigherOrders();
} // namespace ludus::graphics::renderer::testing
