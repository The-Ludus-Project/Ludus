#include <ludus/foundation/base/types.h>

#include <ludus/foundation/containers/array.hpp>
#include <ludus/foundation/math/scalar.hpp>
#include <ludus/foundation/math/vector.hpp>
#include <ludus/graphics/renderer/atmosphere.hpp>

#include "../shaders/renderer_atmosphere_layout.h"
#include "internal/atmosphere_sky_tables.h"
#include "internal/reference_sky.h"
#include "internal/render_graph_atmosphere.h"

// CPU evaluation of the raster's clear-sky tables for distant reference lighting. Thanks to
// Sébastien Hillaire, A Scalable and Production Ready Sky and Atmosphere Rendering Technique,
// CGF 2020, §§4–5, doi:10.1111/cgf.14050. Each function mirrors the same-named function in
// renderer_atmosphere_transport.slang or entry point in renderer_atmosphere.slang, including
// their table parameterizations and bilinear lookups, so the reference sees the raster's sky.
// It differs only in evaluating in double precision and storing the tables in float64.
namespace ludus::graphics::renderer
{
using namespace foundation;
namespace
{
constexpr float64 GROUND_RADIUS = LUDUS_ATMOSPHERE_GROUND_KILOMETRES;
constexpr float64 SHELL_HEIGHT = LUDUS_ATMOSPHERE_HEIGHT_KILOMETRES;
constexpr float64 TOP_RADIUS = GROUND_RADIUS + SHELL_HEIGHT;
constexpr float64 METRES_PER_KILOMETRE = LUDUS_ATMOSPHERE_METRES_PER_KILOMETRE;
constexpr math::Vector3d RAYLEIGH_SCATTERING{LUDUS_ATMOSPHERE_RAYLEIGH_SCATTERING_RED,
                                             LUDUS_ATMOSPHERE_RAYLEIGH_SCATTERING_GREEN,
                                             LUDUS_ATMOSPHERE_RAYLEIGH_SCATTERING_BLUE};
constexpr math::Vector3d OZONE_ABSORPTION{LUDUS_ATMOSPHERE_OZONE_ABSORPTION_RED,
                                          LUDUS_ATMOSPHERE_OZONE_ABSORPTION_GREEN,
                                          LUDUS_ATMOSPHERE_OZONE_ABSORPTION_BLUE};
constexpr uint32 TRANSMITTANCE_WIDTH = LUDUS_ATMOSPHERE_TRANSMITTANCE_WIDTH;
constexpr uint32 TRANSMITTANCE_HEIGHT = LUDUS_ATMOSPHERE_TRANSMITTANCE_HEIGHT;
constexpr uint32 MULTIPLE_SIZE = LUDUS_ATMOSPHERE_MULTIPLE_SIZE;
constexpr uint32 SKY_WIDTH = LUDUS_ATMOSPHERE_SKY_WIDTH;
constexpr uint32 SKY_HALF_HEIGHT = LUDUS_ATMOSPHERE_SKY_HALF_HEIGHT;
/// Subsamples along each axis of one environment texel; averages the sky across the texel footprint.
constexpr uint32 TEXEL_SUBSAMPLES = 2;
/// Horizontal lengths below this are treated as a vertical direction, as DIRECTION_EPSILON in the shader.
constexpr float64 DIRECTION_EPSILON = 0.000001;

using Table = internal::AtmosphereTable;

struct Segment final
{
    math::Vector3d Radiance{};
    math::Vector3d Transmittance{1, 1, 1};
    math::Vector3d ScatteringFraction{};
};

[[nodiscard]] float64 Saturate(float64 value) noexcept
{
    return math::Clamp(value, 0.0, 1.0);
}

[[nodiscard]] math::Vector3d Exponentiate(math::Vector3d value) noexcept
{
    return {math::Exp(value.X), math::Exp(value.Y), math::Exp(value.Z)};
}

[[nodiscard]] math::Vector3d Lerp(math::Vector3d first, math::Vector3d second, float64 t) noexcept
{
    return first + (second - first) * t;
}

[[nodiscard]] float64 ComputeSquaredHeightDifference(float64 height, float64 radius) noexcept
{
    return height * (radius + GROUND_RADIUS);
}

[[nodiscard]] float64 ComputeHorizonCosine(float64 height) noexcept
{
    const float64 radius = GROUND_RADIUS + height;
    return -math::Sqrt(ComputeSquaredHeightDifference(height, radius)) / radius;
}

// NOLINTBEGIN(bugprone-easily-swappable-parameters): these helpers keep the parameter order of their
// renderer_atmosphere_transport.slang counterparts, so a side-by-side review compares like with like.
[[nodiscard]] float64 GetDistanceToTop(float64 height, float64 zenithCosine) noexcept
{
    const float64 radius = GROUND_RADIUS + height;
    const float64 projectedRadius = radius * zenithCosine;
    const float64 squaredDifference = (SHELL_HEIGHT - height) * (TOP_RADIUS + radius);
    const float64 root = math::Sqrt(math::Max(0.0, projectedRadius * projectedRadius + squaredDifference));
    if (projectedRadius > 0)
    {
        return squaredDifference / (projectedRadius + root);
    }
    return root - projectedRadius;
}

[[nodiscard]] float64 GetDistanceToGround(float64 height, float64 zenithCosine) noexcept
{
    const float64 radius = GROUND_RADIUS + height;
    const float64 projectedRadius = -radius * zenithCosine;
    const float64 squaredDifference = ComputeSquaredHeightDifference(height, radius);
    const float64 root = math::Sqrt(math::Max(0.0, projectedRadius * projectedRadius - squaredDifference));
    return squaredDifference > 0 ? squaredDifference / (projectedRadius + root) : 0;
}

[[nodiscard]] float64 ComputeAltitude(float64 height, float64 zenithCosine, float64 distance) noexcept
{
    const float64 radius = GROUND_RADIUS + height;
    const float64 squaredChange = distance * (2 * radius * zenithCosine + distance);
    return math::Clamp(height + squaredChange / (math::Sqrt(radius * radius + squaredChange) + radius),
                       0.0,
                       SHELL_HEIGHT);
}

/// Rayleigh, aerosol and ozone densities relative to their sea-level or peak values.
[[nodiscard]] math::Vector3d ComputeDensity(float64 height) noexcept
{
    return {math::Exp(-height / LUDUS_ATMOSPHERE_RAYLEIGH_SCALE_HEIGHT_KILOMETRES),
            math::Exp(-height / LUDUS_ATMOSPHERE_AEROSOL_SCALE_HEIGHT_KILOMETRES),
            Saturate(1 - math::Abs(height - LUDUS_ATMOSPHERE_OZONE_PEAK_HEIGHT_KILOMETRES) /
                             LUDUS_ATMOSPHERE_OZONE_HALF_WIDTH_KILOMETRES)};
}

[[nodiscard]] math::Vector3d ComputeExtinction(math::Vector3d density) noexcept
{
    constexpr float64 AEROSOL_EXTINCTION = LUDUS_ATMOSPHERE_AEROSOL_EXTINCTION;
    return RAYLEIGH_SCATTERING * density.X + math::Vector3d{1, 1, 1} * (density.Y * AEROSOL_EXTINCTION) +
           OZONE_ABSORPTION * density.Z;
}

/// Concentrate quadrature near both ends, including the ground on a downward ray.
[[nodiscard]] float64 ComputeStepBoundary(float64 unitDistance) noexcept
{
    constexpr float64 SMOOTH_CUBIC_COEFFICIENT = 3;
    return unitDistance * unitDistance * (SMOOTH_CUBIC_COEFFICIENT - 2 * unitDistance);
}

[[nodiscard]] math::Vector3d IntegrateTransmittance(float64 height, float64 zenithCosine, float64 distance) noexcept
{
    constexpr uint32 STEPS = LUDUS_ATMOSPHERE_TRANSMITTANCE_STEPS;
    math::Vector3d opticalDepth{};
    float64 previous = 0;
    for (uint32 step = 1; step <= STEPS; ++step)
    {
        const float64 next = distance * ComputeStepBoundary(static_cast<float64>(step) / STEPS);
        const float64 altitude = ComputeAltitude(height, zenithCosine, (previous + next) * 0.5);
        opticalDepth = opticalDepth + ComputeExtinction(ComputeDensity(altitude)) * (next - previous);
        previous = next;
    }
    return Exponentiate(-opticalDepth);
}

[[nodiscard]] math::Vector3d
SampleRegion(const Table& table, float64 u, float64 v, uint32 width, uint32 height, uint32 offsetPixel) noexcept
{
    const float64 x = Saturate(u) * static_cast<float64>(width - 1);
    const float64 y = Saturate(v) * static_cast<float64>(height - 1);
    const uint32 lowerX = static_cast<uint32>(x);
    const uint32 lowerY = static_cast<uint32>(y);
    const uint32 upperX = math::Min(lowerX + 1, width - 1);
    const uint32 upperY = math::Min(lowerY + 1, height - 1);
    const float64 fractionX = x - static_cast<float64>(lowerX);
    const float64 fractionY = y - static_cast<float64>(lowerY);
    const math::Vector3d top =
        Lerp(table.Load(lowerX, lowerY + offsetPixel), table.Load(upperX, lowerY + offsetPixel), fractionX);
    const math::Vector3d bottom =
        Lerp(table.Load(lowerX, upperY + offsetPixel), table.Load(upperX, upperY + offsetPixel), fractionX);
    return Lerp(top, bottom, fractionY);
}

[[nodiscard]] math::Vector3d SampleTransmittance(const Table& table, float64 height, float64 zenithCosine) noexcept
{
    const float64 radius = GROUND_RADIUS + height;
    const float64 horizonDistance = math::Sqrt(ComputeSquaredHeightDifference(height, radius));
    const float64 topHorizonDistance = math::Sqrt(ComputeSquaredHeightDifference(SHELL_HEIGHT, TOP_RADIUS));
    const float64 minimumDistance = SHELL_HEIGHT - height;
    const float64 maximumDistance = horizonDistance + topHorizonDistance;
    return SampleRegion(table,
                        (GetDistanceToTop(height, zenithCosine) - minimumDistance) /
                            (maximumDistance - minimumDistance),
                        horizonDistance / topHorizonDistance,
                        TRANSMITTANCE_WIDTH,
                        TRANSMITTANCE_HEIGHT,
                        0);
}

[[nodiscard]] math::Vector3d SampleMultiple(const Table& table, float64 altitude, float64 sunCosine) noexcept
{
    return SampleRegion(table, sunCosine * 0.5 + 0.5, altitude / SHELL_HEIGHT, MULTIPLE_SIZE, MULTIPLE_SIZE, 0);
}

/// Mirrors IntegrateAtmosphere. multipleOrNull null omits the multiple-scattering source.
[[nodiscard]] Segment IntegrateAtmosphere(const Table& transmittance,
                                          const Table* multipleOrNull,
                                          float64 height,
                                          float64 zenithCosine,
                                          float64 solarZenithCosine,
                                          float64 scatteringCosine,
                                          float64 distance,
                                          uint32 stepCount) noexcept
{
    constexpr float64 PHASE_NUMERATOR = 3;
    constexpr float64 RAYLEIGH_DENOMINATOR = 16;
    constexpr float64 AEROSOL_DENOMINATOR = 8;
    constexpr float64 ASYMMETRY = LUDUS_ATMOSPHERE_AEROSOL_ASYMMETRY;
    constexpr float64 AEROSOL_SCATTERING = LUDUS_ATMOSPHERE_AEROSOL_SCATTERING;
    constexpr float64 LINEAR_THRESHOLD = LUDUS_ATMOSPHERE_OPTICAL_DEPTH_LINEAR_THRESHOLD;
    const float64 cosineSquared = scatteringCosine * scatteringCosine;
    const float64 rayleighPhase = PHASE_NUMERATOR * (1 + cosineSquared) / (RAYLEIGH_DENOMINATOR * math::kPi);
    const float64 asymmetrySquared = ASYMMETRY * ASYMMETRY;
    const float64 aerosolBase = 1 + asymmetrySquared - 2 * ASYMMETRY * scatteringCosine;
    const float64 aerosolPhase =
        PHASE_NUMERATOR * (1 - asymmetrySquared) * (1 + cosineSquared) /
        (AEROSOL_DENOMINATOR * math::kPi * (2 + asymmetrySquared) * aerosolBase * math::Sqrt(aerosolBase));
    const float64 radius = GROUND_RADIUS + height;
    Segment segment;
    float64 previous = 0;
    for (uint32 step = 1; step <= stepCount; ++step)
    {
        const float64 next = distance * ComputeStepBoundary(static_cast<float64>(step) / stepCount);
        const float64 midpoint = (previous + next) * 0.5;
        const float64 altitude = ComputeAltitude(height, zenithCosine, midpoint);
        const float64 sampleSolarCosine =
            math::Clamp((radius * solarZenithCosine + midpoint * scatteringCosine) / (GROUND_RADIUS + altitude),
                        -1.0,
                        1.0);
        const math::Vector3d density = ComputeDensity(altitude);
        const float64 stepLength = next - previous;
        const math::Vector3d opticalDepth = ComputeExtinction(density) * stepLength;
        const math::Vector3d stepTransmittance = Exponentiate(-opticalDepth);
        const auto integrate = [](float64 depth, float64 transmitted) noexcept {
            return depth < LINEAR_THRESHOLD ? 1 - depth * 0.5 : (1 - transmitted) / math::Max(depth, LINEAR_THRESHOLD);
        };
        const math::Vector3d integrated{integrate(opticalDepth.X, stepTransmittance.X),
                                        integrate(opticalDepth.Y, stepTransmittance.Y),
                                        integrate(opticalDepth.Z, stepTransmittance.Z)};
        const math::Vector3d rayleigh = RAYLEIGH_SCATTERING * density.X;
        const math::Vector3d aerosol = math::Vector3d{1, 1, 1} * (density.Y * AEROSOL_SCATTERING);
        const math::Vector3d weightedStep = math::Hadamard(segment.Transmittance, integrated) * stepLength;
        if (sampleSolarCosine >= ComputeHorizonCosine(altitude))
        {
            segment.Radiance =
                segment.Radiance +
                math::Hadamard(
                    math::Hadamard(weightedStep, SampleTransmittance(transmittance, altitude, sampleSolarCosine)),
                    rayleigh * rayleighPhase + aerosol * aerosolPhase);
        }
        if (multipleOrNull != nullptr)
        {
            segment.Radiance =
                segment.Radiance +
                math::Hadamard(
                    math::Hadamard(weightedStep, SampleMultiple(*multipleOrNull, altitude, sampleSolarCosine)),
                    rayleigh + aerosol);
        }
        segment.ScatteringFraction = segment.ScatteringFraction + math::Hadamard(weightedStep, rayleigh + aerosol);
        segment.Transmittance = math::Hadamard(segment.Transmittance, stepTransmittance);
        previous = next;
    }
    return segment;
}

// NOLINTEND(bugprone-easily-swappable-parameters)

/// Mirrors AtmosphereTransmittanceMain.
void BuildTransmittance(Table& inoutTable) noexcept
{
    const float64 topHorizonDistance = math::Sqrt(ComputeSquaredHeightDifference(SHELL_HEIGHT, TOP_RADIUS));
    for (uint32 y = 0; y < TRANSMITTANCE_HEIGHT; ++y)
    {
        const float64 horizonDistance = static_cast<float64>(y) / (TRANSMITTANCE_HEIGHT - 1) * topHorizonDistance;
        const float64 height = math::Min(
            horizonDistance * horizonDistance /
                (math::Sqrt(GROUND_RADIUS * GROUND_RADIUS + horizonDistance * horizonDistance) + GROUND_RADIUS),
            SHELL_HEIGHT);
        const float64 radius = GROUND_RADIUS + height;
        const float64 squaredDifference = (SHELL_HEIGHT - height) * (TOP_RADIUS + radius);
        for (uint32 x = 0; x < TRANSMITTANCE_WIDTH; ++x)
        {
            const float64 u = static_cast<float64>(x) / (TRANSMITTANCE_WIDTH - 1);
            const float64 distance = (SHELL_HEIGHT - height) * (1 - u) + (horizonDistance + topHorizonDistance) * u;
            const float64 zenithCosine =
                distance > 0
                    ? math::Clamp((squaredDifference - distance * distance) / (2 * radius * distance), -1.0, 1.0)
                    : 1;
            inoutTable.Texels[static_cast<usize>(y) * TRANSMITTANCE_WIDTH + x] =
                IntegrateTransmittance(height, zenithCosine, distance);
        }
    }
}

/// Mirrors AtmosphereMultipleMain.
void BuildMultiple(const Table& transmittance, Table& inoutTable) noexcept
{
    constexpr uint32 ZENITH_SAMPLES = LUDUS_ATMOSPHERE_MULTIPLE_ZENITH_SAMPLES;
    constexpr uint32 AZIMUTH_SAMPLES = LUDUS_ATMOSPHERE_MULTIPLE_AZIMUTH_SAMPLES;
    constexpr float64 SAMPLE_COUNT = ZENITH_SAMPLES * AZIMUTH_SAMPLES;
    constexpr float64 MINIMUM_ESCAPE = LUDUS_ATMOSPHERE_MULTIPLE_MINIMUM_ESCAPE;
    for (uint32 y = 0; y < MULTIPLE_SIZE; ++y)
    {
        const float64 height = static_cast<float64>(y) / (MULTIPLE_SIZE - 1) * SHELL_HEIGHT;
        const float64 horizon = ComputeHorizonCosine(height);
        for (uint32 x = 0; x < MULTIPLE_SIZE; ++x)
        {
            const float64 solarZenithCosine = static_cast<float64>(x) / (MULTIPLE_SIZE - 1) * 2 - 1;
            const float64 solarSine = math::Sqrt(Saturate(1 - solarZenithCosine * solarZenithCosine));
            math::Vector3d radiance{};
            math::Vector3d fraction{};
            for (uint32 row = 0; row < ZENITH_SAMPLES; ++row)
            {
                const float64 zenithCosine = (static_cast<float64>(row) + 0.5) * 2 / ZENITH_SAMPLES - 1;
                const float64 sine = math::Sqrt(Saturate(1 - zenithCosine * zenithCosine));
                const float64 distance = zenithCosine < horizon ? GetDistanceToGround(height, zenithCosine)
                                                                : GetDistanceToTop(height, zenithCosine);
                for (uint32 column = 0; column < AZIMUTH_SAMPLES; ++column)
                {
                    const float64 azimuth = (static_cast<float64>(column) + 0.5) * 2 * math::kPi / AZIMUTH_SAMPLES;
                    const float64 scatteringCosine =
                        math::Clamp(zenithCosine * solarZenithCosine + sine * solarSine * math::Cos(azimuth),
                                    -1.0,
                                    1.0);
                    const Segment segment = IntegrateAtmosphere(transmittance,
                                                                nullptr,
                                                                height,
                                                                zenithCosine,
                                                                solarZenithCosine,
                                                                scatteringCosine,
                                                                distance,
                                                                LUDUS_ATMOSPHERE_MULTIPLE_STEPS);
                    radiance = radiance + segment.Radiance;
                    fraction = fraction + segment.ScatteringFraction;
                }
            }
            const auto close = [](float64 sum, float64 returned) noexcept {
                return sum / math::Max(SAMPLE_COUNT - returned, SAMPLE_COUNT * MINIMUM_ESCAPE);
            };
            inoutTable.Texels[static_cast<usize>(y) * MULTIPLE_SIZE + x] = {close(radiance.X, fraction.X),
                                                                            close(radiance.Y, fraction.Y),
                                                                            close(radiance.Z, fraction.Z)};
        }
    }
}

/// Mirrors AtmosphereSkyMain for one observer altitude and solar zenith cosine.
void BuildSky(const Table& transmittance,
              const Table* multipleOrNull,
              float64 height,
              float64 solarZenithCosine,
              Table& inoutTable) noexcept
{
    const float64 horizon = ComputeHorizonCosine(height);
    const float64 solarSine = math::Sqrt(Saturate(1 - solarZenithCosine * solarZenithCosine));
    for (uint32 y = 0; y < 2 * SKY_HALF_HEIGHT; ++y)
    {
        const bool isGroundHit = y >= SKY_HALF_HEIGHT;
        const float64 v = static_cast<float64>(y - (isGroundHit == true ? SKY_HALF_HEIGHT : 0)) / (SKY_HALF_HEIGHT - 1);
        const float64 zenithCosine = horizon + ((isGroundHit == true ? -1.0 : 1.0) - horizon) * (v * v);
        const float64 sine = math::Sqrt(Saturate(1 - zenithCosine * zenithCosine));
        const float64 distance =
            isGroundHit == true ? GetDistanceToGround(height, zenithCosine) : GetDistanceToTop(height, zenithCosine);
        for (uint32 x = 0; x < SKY_WIDTH; ++x)
        {
            const float64 u = static_cast<float64>(x) / (SKY_WIDTH - 1);
            const float64 scatteringCosine =
                math::Clamp(zenithCosine * solarZenithCosine + sine * solarSine * math::Cos(math::kPi * u), -1.0, 1.0);
            inoutTable.Texels[static_cast<usize>(y) * SKY_WIDTH + x] = IntegrateAtmosphere(transmittance,
                                                                                           multipleOrNull,
                                                                                           height,
                                                                                           zenithCosine,
                                                                                           solarZenithCosine,
                                                                                           scatteringCosine,
                                                                                           distance,
                                                                                           LUDUS_ATMOSPHERE_SKY_STEPS)
                                                                           .Radiance;
        }
    }
}

/// Mirrors SampleAtmosphereSky.
[[nodiscard]] math::Vector3d SampleSkyTable(const Table& sky,
                                            math::Vector3d directionWorld,
                                            math::Vector3d upWorld,
                                            math::Vector3d sunWorld,
                                            float64 height) noexcept
{
    const float64 zenithCosine = math::Clamp(math::Dot(directionWorld, upWorld), -1.0, 1.0);
    const float64 solarZenithCosine = math::Clamp(math::Dot(sunWorld, upWorld), -1.0, 1.0);
    const float64 horizon = ComputeHorizonCosine(height);
    const bool isGroundHit = zenithCosine < horizon;
    const float64 range = isGroundHit == true ? horizon + 1 : 1 - horizon;
    const float64 ordinate = math::Sqrt(Saturate(math::Abs(zenithCosine - horizon) / range));
    const math::Vector3d horizontalWorld = directionWorld - upWorld * zenithCosine;
    const math::Vector3d sunHorizontalWorld = sunWorld - upWorld * solarZenithCosine;
    const float64 horizontalProduct = math::Length(horizontalWorld) * math::Length(sunHorizontalWorld);
    float64 azimuth = 0;
    if (horizontalProduct > DIRECTION_EPSILON)
    {
        const float64 cosine =
            math::Clamp(math::Dot(horizontalWorld, sunHorizontalWorld) / horizontalProduct, -1.0, 1.0);
        azimuth = math::Atan2(math::Sqrt(1 - cosine * cosine), cosine) / math::kPi;
    }
    return SampleRegion(sky, azimuth, ordinate, SKY_WIDTH, SKY_HALF_HEIGHT, isGroundHit == true ? SKY_HALF_HEIGHT : 0);
}

[[nodiscard]] math::Vector3d Widen(math::Vector3 value) noexcept
{
    return {static_cast<float64>(value.X), static_cast<float64>(value.Y), static_cast<float64>(value.Z)};
}
} // namespace

namespace internal
{
bool AtmosphereTable::TryAllocate(uint32 width, uint32 height) noexcept
{
    Width = width;
    Height = height;
    return Texels.TryResize(static_cast<usize>(width) * height) == true;
}

const math::Vector3d& AtmosphereTable::Load(uint32 x, uint32 y) const noexcept
{
    return Texels[static_cast<usize>(y) * Width + x];
}

bool AtmosphereSkyTables::TryBuild(float64 heightKilometres, float64 solarZenithCosine, bool hasMultiple) noexcept
{
    if (mTransmittance.TryAllocate(TRANSMITTANCE_WIDTH, TRANSMITTANCE_HEIGHT) == false ||
        mMultiple.TryAllocate(MULTIPLE_SIZE, MULTIPLE_SIZE) == false ||
        mSky.TryAllocate(SKY_WIDTH, 2 * SKY_HALF_HEIGHT) == false)
    {
        return false;
    }
    BuildTransmittance(mTransmittance);
    BuildMultiple(mTransmittance, mMultiple);
    BuildSky(mTransmittance, hasMultiple == true ? &mMultiple : nullptr, heightKilometres, solarZenithCosine, mSky);
    mHeightKilometres = heightKilometres;
    return true;
}

math::Vector3d AtmosphereSkyTables::SampleSky(math::Vector3d directionWorld,
                                              math::Vector3d upWorld,
                                              math::Vector3d sunWorld) const noexcept
{
    return SampleSkyTable(mSky, directionWorld, upWorld, sunWorld, mHeightKilometres);
}

math::Vector3d AtmosphereSkyTables::SampleTransmittance(float64 heightKilometres, float64 zenithCosine) const noexcept
{
    return renderer::SampleTransmittance(mTransmittance, heightKilometres, zenithCosine);
}

float64 AtmosphereSkyTables::GetHorizonCosine(float64 heightKilometres) noexcept
{
    return ComputeHorizonCosine(heightKilometres);
}
} // namespace internal

float64 internal::WriteReferenceSkyTransmittance(float64 altitudeMetres, ReferenceSkyWords& inoutWords) noexcept
{
    const float64 height = altitudeMetres / METRES_PER_KILOMETRE;
    const float64 horizon = ComputeHorizonCosine(height);
    constexpr uint32 LAST_SAMPLE = LUDUS_REFERENCE_SKY_TRANSMITTANCE_SAMPLES - 1;
    for (uint32 sample = 0; sample <= LAST_SAMPLE; ++sample)
    {
        const float64 fraction = static_cast<float64>(sample) / LAST_SAMPLE;
        const float64 zenithCosine = horizon + (1 - horizon) * fraction * fraction;
        const math::Vector3d value =
            IntegrateTransmittance(height, zenithCosine, GetDistanceToTop(height, zenithCosine));
        inoutWords[LUDUS_REFERENCE_SKY_TRANSMITTANCE + sample] = {static_cast<float32>(value.X),
                                                                  static_cast<float32>(value.Y),
                                                                  static_cast<float32>(value.Z),
                                                                  0};
    }
    return horizon;
}

AtmosphereEnvironmentStatus ResolveAtmosphereEnvironment(const AtmosphereDescription& atmosphere,
                                                         math::Vector3 eyeWorld,
                                                         uint32 width,
                                                         uint32 height,
                                                         BorrowedView<math::Vector4> outPixels,
                                                         math::Vector3& outSunIlluminance) noexcept
{
    if (atmosphere.IsEnabled == false || atmosphere.ShouldLightScene == false)
    {
        return AtmosphereEnvironmentStatus::NotLit;
    }
    float64 altitudeMetres = 0;
    math::Vector3 upWorld;
    math::Vector3 sunWorld;
    const math::Vector3 illuminance = atmosphere.SolarIlluminance;
    const float32 radius = atmosphere.SolarAngularRadiusRadians;
    if (width == 0 || height == 0 || width > MAX_ATMOSPHERE_ENVIRONMENT_SIDE ||
        height > MAX_ATMOSPHERE_ENVIRONMENT_SIDE || outPixels.IsValid() == false ||
        outPixels.GetSize() != static_cast<usize>(width) * height ||
        internal::TryGetAtmosphereObserver(atmosphere, eyeWorld, altitudeMetres, upWorld) == false ||
        math::TryNormalize(atmosphere.DirectionToSunWorld, sunWorld) != math::MathStatus::Success ||
        math::IsFinite(illuminance) == false || illuminance.X < 0 || illuminance.Y < 0 || illuminance.Z < 0 ||
        math::IsFinite(radius) == false || radius <= 0 || radius >= math::kHalfPiF)
    {
        return AtmosphereEnvironmentStatus::InvalidDescription;
    }
    if (altitudeMetres < 0 || altitudeMetres > GetAtmosphereTopAltitude())
    {
        return AtmosphereEnvironmentStatus::NotLit;
    }
    // The render graph rounds the observer frame to float32 uniforms; use the same values.
    const float64 heightKilometres = static_cast<float64>(static_cast<float32>(altitudeMetres / METRES_PER_KILOMETRE));
    const math::Vector3d wideUpWorld = Widen(upWorld);
    const math::Vector3d wideSunWorld = Widen(sunWorld);
    const float64 solarZenithCosine = static_cast<float64>(math::Clamp(math::Dot(upWorld, sunWorld), -1.0F, 1.0F));
    internal::AtmosphereSkyTables tables;
    if (tables.TryBuild(heightKilometres, solarZenithCosine, true) == false)
    {
        return AtmosphereEnvironmentStatus::OutOfMemory;
    }

    const math::Vector3d solar = Widen(illuminance);
    constexpr float64 SUBSAMPLE_WEIGHT = 1.0 / (TEXEL_SUBSAMPLES * TEXEL_SUBSAMPLES);
    for (uint32 y = 0; y < height; ++y)
    {
        for (uint32 x = 0; x < width; ++x)
        {
            math::Vector3d radiance{};
            for (uint32 j = 0; j < TEXEL_SUBSAMPLES; ++j)
            {
                const float64 v = (static_cast<float64>(y) + (static_cast<float64>(j) + 0.5) / TEXEL_SUBSAMPLES) /
                                  static_cast<float64>(height);
                const float64 polar = math::kPi * v;
                const float64 ringRadius = math::Sin(polar);
                for (uint32 i = 0; i < TEXEL_SUBSAMPLES; ++i)
                {
                    const float64 u = (static_cast<float64>(x) + (static_cast<float64>(i) + 0.5) / TEXEL_SUBSAMPLES) /
                                      static_cast<float64>(width);
                    const float64 longitude = 2 * math::kPi * u;
                    const math::Vector3d directionWorld{ringRadius * math::Cos(longitude),
                                                        math::Cos(polar),
                                                        ringRadius * math::Sin(longitude)};
                    radiance =
                        radiance + tables.SampleSky(directionWorld, wideUpWorld, wideSunWorld) * SUBSAMPLE_WEIGHT;
                }
            }
            const math::Vector3d scaled = math::Hadamard(radiance, solar);
            outPixels[static_cast<usize>(y) * width + x] = {static_cast<float32>(scaled.X),
                                                            static_cast<float32>(scaled.Y),
                                                            static_cast<float32>(scaled.Z),
                                                            1};
        }
    }
    // Mirrors EvaluateAtmosphereSun at the observer, inside the shell.
    math::Vector3 sunIlluminance{};
    if (solarZenithCosine >= ComputeHorizonCosine(heightKilometres))
    {
        const math::Vector3d attenuated =
            math::Hadamard(solar, tables.SampleTransmittance(heightKilometres, solarZenithCosine));
        sunIlluminance = {static_cast<float32>(attenuated.X),
                          static_cast<float32>(attenuated.Y),
                          static_cast<float32>(attenuated.Z)};
    }
    outSunIlluminance = sunIlluminance;
    return AtmosphereEnvironmentStatus::Ready;
}
} // namespace ludus::graphics::renderer
