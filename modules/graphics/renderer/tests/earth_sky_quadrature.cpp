#include "earth_sky_quadrature.hpp"
#include "orbital_atmosphere_probes.hpp"

#include <ludus/foundation/containers/static_array.hpp>
#include <ludus/foundation/math/scalar.hpp>

#include <cmath>

namespace ludus::graphics::renderer::testing
{
using namespace ludus::foundation;
// Independent binary64 midpoint quadrature of the declared single-scattering
// integral. It uses planet-centred intersections and integrates each optical
// depth directly, without the production shader's texture parameterization.
float64 ComputeEarthExtinction(float64 altitude, usize channel)
{
    constexpr usize CHANNEL_COUNT = 3;
    constexpr StaticArray<float64, CHANNEL_COUNT> MOLECULAR{{0.005802, 0.013558, 0.033100}};
    constexpr StaticArray<float64, CHANNEL_COUNT> OZONE{{0.000650, 0.001881, 0.000085}};
    return MOLECULAR[channel] * std::exp(-altitude / 8.0) + 0.004440 * std::exp(-altitude / 1.2) +
           OZONE[channel] * math::Max(0.0, 1 - math::Abs(altitude - 25.0) / 15.0);
}

// NOLINTBEGIN(bugprone-easily-swappable-parameters): the channel and step count follow IntegrateEarthSky's order.
bool ComputeEarthSolarTransmittance(math::Vector3d pointKilometres,
                                    math::Vector3d sunWorld,
                                    usize channel,
                                    usize solarSteps,
                                    float64& outTransmittance)
// NOLINTEND(bugprone-easily-swappable-parameters)
{
    constexpr float64 GROUND = EARTH_GROUND_RADIUS_KILOMETRES;
    constexpr float64 TOP = EARTH_TOP_RADIUS_KILOMETRES;
    const float64 solarProjection = math::Dot(pointKilometres, sunWorld);
    const float64 squaredRadius = math::Dot(pointKilometres, pointKilometres);
    const float64 solarDiscriminant = solarProjection * solarProjection + GROUND * GROUND - squaredRadius;
    if (solarProjection < 0 && solarDiscriminant >= 0)
    {
        return false;
    }
    const float64 solarDistance =
        -solarProjection + std::sqrt(solarProjection * solarProjection + TOP * TOP - squaredRadius);
    const float64 solarStep = solarDistance / static_cast<float64>(solarSteps);
    float64 solarOpticalDepth = 0;
    for (usize solarSample = 0; solarSample < solarSteps; ++solarSample)
    {
        const math::Vector3d solarPointWorld =
            pointKilometres + sunWorld * ((static_cast<float64>(solarSample) + 0.5) * solarStep);
        solarOpticalDepth += ComputeEarthExtinction(math::Length(solarPointWorld) - GROUND, channel) * solarStep;
    }
    outTransmittance = std::exp(-solarOpticalDepth);
    return true;
}

// NOLINTBEGIN(bugprone-easily-swappable-parameters): the channel and step counts keep the existing quadrature
// signature.
EarthSkyIntegral IntegrateEarthSkySegment(math::Vector3d observerKilometres,
                                          math::Vector3d directionWorld,
                                          math::Vector3d sunWorld,
                                          usize channel,
                                          float64 maximumKilometres,
                                          usize viewSteps,
                                          usize solarSteps)
// NOLINTEND(bugprone-easily-swappable-parameters)
{
    constexpr float64 GROUND = EARTH_GROUND_RADIUS_KILOMETRES;
    constexpr float64 TOP = EARTH_TOP_RADIUS_KILOMETRES;
    constexpr usize CHANNEL_COUNT = 3;
    constexpr StaticArray<float64, CHANNEL_COUNT> MOLECULAR{{0.005802, 0.013558, 0.033100}};
    const math::Vector3d observerWorld = observerKilometres;
    const float64 projected = math::Dot(observerWorld, directionWorld);
    const float64 projectedSquared = projected * projected;
    // Thanks to Nishita, Sirai, Tadamura and Nakamae, “Display of the Earth Taking into Account
    // Atmospheric Scattering”, SIGGRAPH '93, pp. 175–182, https://doi.org/10.1145/166117.166140 .
    // Clip the view integral to the spherical atmosphere. Each solar segment below tests the
    // solid Earth's shadow independently, including along chords with both lit and unlit samples.
    const float64 squaredEyeRadius = math::Dot(observerWorld, observerWorld);
    const float64 topDiscriminant = projectedSquared + TOP * TOP - squaredEyeRadius;
    if (topDiscriminant <= 0)
    {
        return {};
    }
    const float64 topRoot = std::sqrt(topDiscriminant);
    const float64 entry = math::Max(0.0, -projected - topRoot);
    const float64 groundDiscriminant = projectedSquared + GROUND * GROUND - squaredEyeRadius;
    const float64 exitDistance = math::Min(
        projected < 0 && groundDiscriminant >= 0 ? -projected - std::sqrt(groundDiscriminant) : -projected + topRoot,
        maximumKilometres);
    if (exitDistance <= entry)
    {
        return {};
    }
    const float64 cosine = math::Dot(directionWorld, sunWorld);
    const float64 moleculePhase = 3 * (1 + cosine * cosine) / (16 * math::kPi);
    constexpr float64 ASYMMETRY = 0.8;
    const float64 aerosolPhase = 3 * (1 - ASYMMETRY * ASYMMETRY) * (1 + cosine * cosine) /
                                 (8 * math::kPi * (2 + ASYMMETRY * ASYMMETRY) *
                                  std::pow(1 + ASYMMETRY * ASYMMETRY - 2 * ASYMMETRY * cosine, 1.5));
    const float64 step = (exitDistance - entry) / static_cast<float64>(viewSteps);
    float64 opticalDepth = 0;
    EarthSkyIntegral result;
    for (usize sample = 0; sample < viewSteps; ++sample)
    {
        const math::Vector3d pointWorld =
            observerWorld + directionWorld * (entry + (static_cast<float64>(sample) + 0.5) * step);
        const float64 height = math::Length(pointWorld) - GROUND;
        const float64 extinction = ComputeEarthExtinction(height, channel) * step;
        const float64 weight = std::exp(-opticalDepth - 0.5 * extinction) * step;
        const float64 rayleigh = MOLECULAR[channel] * std::exp(-height / 8);
        const float64 aerosol = 0.003996 * std::exp(-height / 1.2);
        result.ReturnFraction += weight * (rayleigh + aerosol);
        float64 solarTransmittance = 0;
        if (ComputeEarthSolarTransmittance(pointWorld, sunWorld, channel, solarSteps, solarTransmittance) == true)
        {
            result.Radiance += weight * solarTransmittance * (rayleigh * moleculePhase + aerosol * aerosolPhase);
        }
        else
        {
            ++result.ShadowSamples;
        }
        opticalDepth += extinction;
    }
    result.Transmittance = std::exp(-opticalDepth);
    return result;
}

// NOLINTBEGIN(bugprone-easily-swappable-parameters): the channel and step counts keep the existing quadrature
// signature.
EarthSkyIntegral IntegrateEarthSky(float64 altitude,
                                   math::Vector3d directionWorld,
                                   math::Vector3d sunWorld,
                                   usize channel,
                                   usize viewSteps,
                                   usize solarSteps)
// NOLINTEND(bugprone-easily-swappable-parameters)
{
    // Longer than any chord of the shell.
    constexpr float64 UNCLIPPED_KILOMETRES = 2 * EARTH_TOP_RADIUS_KILOMETRES;
    return IntegrateEarthSkySegment({0, EARTH_GROUND_RADIUS_KILOMETRES + altitude, 0},
                                    directionWorld,
                                    sunWorld,
                                    channel,
                                    UNCLIPPED_KILOMETRES,
                                    viewSteps,
                                    solarSteps);
}

// Independent higher-order solution for the local isotropic approximation at
// ground level with a horizontal view and overhead Sun. Sum 24 explicit
// orders, integrating optical depths directly and using a denser sphere rule.
float64 IntegrateEarthBlueHigherOrders()
{
    constexpr usize BLUE_CHANNEL = 2;
    constexpr float64 GROUND = EARTH_GROUND_RADIUS_KILOMETRES;
    constexpr float64 TOP = EARTH_TOP_RADIUS_KILOMETRES;
    constexpr float64 BLUE_MOLECULAR_SCATTERING = 0.033100;
    constexpr usize VIEW_STEPS = 96;
    constexpr usize ZENITH_SAMPLES = 8;
    constexpr usize AZIMUTH_SAMPLES = 12;
    constexpr usize MULTIPLE_RAY_STEPS = 48;
    constexpr usize MULTIPLE_SOLAR_STEPS = 64;
    constexpr usize SCATTERING_ORDERS = 24;
    constexpr float64 SAMPLE_COUNT = ZENITH_SAMPLES * AZIMUTH_SAMPLES;
    const float64 distance = std::sqrt(TOP * TOP - GROUND * GROUND);
    float64 opticalDepth = 0;
    float64 radiance = 0;
    float64 previous = 0;
    for (usize sample = 1; sample <= VIEW_STEPS; ++sample)
    {
        const float64 fraction = static_cast<float64>(sample) / VIEW_STEPS;
        const float64 next = distance * fraction * fraction;
        const float64 midpoint = (next + previous) * 0.5;
        const float64 height = std::sqrt(GROUND * GROUND + midpoint * midpoint) - GROUND;
        const math::Vector3d sunWorld = {0, GROUND / (GROUND + height), -midpoint / (GROUND + height)};
        EarthSkyIntegral mean;
        for (usize row = 0; row < ZENITH_SAMPLES; ++row)
        {
            const float64 cosine = 2 * (static_cast<float64>(row) + 0.5) / ZENITH_SAMPLES - 1;
            const float64 sine = std::sqrt(1 - cosine * cosine);
            for (usize column = 0; column < AZIMUTH_SAMPLES; ++column)
            {
                const float64 azimuth = (static_cast<float64>(column) + 0.5) * 2 * math::kPi / AZIMUTH_SAMPLES;
                const math::Vector3d directionWorld = {sine * std::sin(azimuth), cosine, sine * std::cos(azimuth)};
                const EarthSkyIntegral ray = IntegrateEarthSky(height,
                                                               directionWorld,
                                                               sunWorld,
                                                               BLUE_CHANNEL,
                                                               MULTIPLE_RAY_STEPS,
                                                               MULTIPLE_SOLAR_STEPS);
                mean.Radiance += ray.Radiance / SAMPLE_COUNT;
                mean.ReturnFraction += ray.ReturnFraction / SAMPLE_COUNT;
            }
        }
        float64 order = mean.Radiance;
        float64 source = 0;
        for (usize i = 0; i < SCATTERING_ORDERS; ++i)
        {
            source += order;
            order *= mean.ReturnFraction;
        }
        const float64 step = next - previous;
        const float64 extinction = ComputeEarthExtinction(height, BLUE_CHANNEL) * step;
        const float64 scattering =
            BLUE_MOLECULAR_SCATTERING * std::exp(-height / 8) + 0.003996 * std::exp(-height / 1.2);
        radiance += std::exp(-opticalDepth - 0.5 * extinction) * scattering * step * source;
        opticalDepth += extinction;
        previous = next;
    }
    return radiance;
}
} // namespace ludus::graphics::renderer::testing
