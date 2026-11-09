#pragma once

#include "math.slang"
#include "renderer_atmosphere_layout.h"

// Thanks to Sébastien Hillaire, A Scalable and Production Ready Sky and
// Atmosphere Rendering Technique, CGF 2020, §§4–5, doi:10.1111/cgf.14050.
// The Earth inputs follow SetupEarthAtmosphere in the accompanying sample.
// Rays terminating at the ground and top boundary use separate filter domains.
static const float GROUND_RADIUS = LUDUS_ATMOSPHERE_GROUND_KILOMETRES;
static const float SHELL_HEIGHT = LUDUS_ATMOSPHERE_HEIGHT_KILOMETRES;
static const float TOP_RADIUS = GROUND_RADIUS + SHELL_HEIGHT;
static const float RAYLEIGH_SCALE_HEIGHT = 8.0;
static const float AEROSOL_SCALE_HEIGHT = 1.2;
static const float OZONE_PEAK_HEIGHT = 25.0;
static const float OZONE_HALF_WIDTH = 15.0;
static const float3 RAYLEIGH_SCATTERING = float3(0.005802, 0.013558, 0.033100);
static const float AEROSOL_SCATTERING = 0.003996;
static const float AEROSOL_EXTINCTION = 0.004440;
static const float3 OZONE_ABSORPTION = float3(0.000650, 0.001881, 0.000085);
static const float AEROSOL_ASYMMETRY = 0.8;
static const uint TRANSMITTANCE_STEPS = 96;
static const uint SKY_STEPS = 80;
static const uint SKY_HALF_HEIGHT = LUDUS_ATMOSPHERE_SKY_HALF_HEIGHT;
static const float OPTICAL_DEPTH_LINEAR_THRESHOLD = 0.001;
static const float DIRECTION_EPSILON = 0.000001;
static const int2 TRANSMITTANCE_SIZE =
    int2(LUDUS_ATMOSPHERE_TRANSMITTANCE_WIDTH, LUDUS_ATMOSPHERE_TRANSMITTANCE_HEIGHT);
static const int2 SKY_REGION_SIZE = int2(LUDUS_ATMOSPHERE_SKY_WIDTH, SKY_HALF_HEIGHT);

static const float ATMOSPHERE_KILOMETRES_PER_METRE = 1.0 / LUDUS_ATMOSPHERE_METRES_PER_KILOMETRE;
static const int ATMOSPHERE_MULTIPLE_SIZE = LUDUS_ATMOSPHERE_MULTIPLE_SIZE;

float ComputeSquaredHeightDifference(float height, float radius)
{
    return height * (radius + GROUND_RADIUS);
}

float ComputeHorizonCosine(float height)
{
    float radius = GROUND_RADIUS + height;
    return -sqrt(ComputeSquaredHeightDifference(height, radius)) / radius;
}

float GetDistanceToTop(float height, float zenithCosine)
{
    float radius = GROUND_RADIUS + height;
    float projectedRadius = radius * zenithCosine;
    float squaredDifference = (SHELL_HEIGHT - height) * (TOP_RADIUS + radius);
    float root = sqrt(max(0, projectedRadius * projectedRadius + squaredDifference));
    if (projectedRadius > 0)
    {
        return squaredDifference / (projectedRadius + root);
    }
    return root - projectedRadius;
}

float GetDistanceToGround(float height, float zenithCosine)
{
    float radius = GROUND_RADIUS + height;
    float projectedRadius = -radius * zenithCosine;
    float squaredDifference = ComputeSquaredHeightDifference(height, radius);
    float root = sqrt(max(0, projectedRadius * projectedRadius - squaredDifference));
    return squaredDifference > 0 ? squaredDifference / (projectedRadius + root) : 0;
}

float ComputeAltitude(float height, float zenithCosine, float distance)
{
    float radius = GROUND_RADIUS + height;
    float squaredChange = distance * (2 * radius * zenithCosine + distance);
    return clamp(height + squaredChange / (sqrt(radius * radius + squaredChange) + radius), 0, SHELL_HEIGHT);
}

float3 ComputeDensity(float height)
{
    return float3(exp(-height / RAYLEIGH_SCALE_HEIGHT),
                  exp(-height / AEROSOL_SCALE_HEIGHT),
                  saturate(1 - abs(height - OZONE_PEAK_HEIGHT) / OZONE_HALF_WIDTH));
}

float3 ComputeExtinction(in float3 density)
{
    return density.x * RAYLEIGH_SCATTERING + density.y * AEROSOL_EXTINCTION + density.z * OZONE_ABSORPTION;
}

float ComputeStepBoundary(float unitDistance)
{
    // Concentrate quadrature near both ends, including the ground on a downward ray.
    const float smoothCubicCoefficient = 3;
    return unitDistance * unitDistance * (smoothCubicCoefficient - 2 * unitDistance);
}

float3 IntegrateTransmittance(float height, float zenithCosine, float distance)
{
    float3 opticalDepth = 0;
    float previous = 0;
    for (uint step = 1; step <= TRANSMITTANCE_STEPS; ++step)
    {
        float next = distance * ComputeStepBoundary(float(step) / TRANSMITTANCE_STEPS);
        float altitude = ComputeAltitude(height, zenithCosine, (previous + next) * 0.5);
        opticalDepth += ComputeExtinction(ComputeDensity(altitude)) * (next - previous);
        previous = next;
    }
    return exp(-opticalDepth);
}

float2 ComputeTransmittanceCoordinates(float height, float zenithCosine)
{
    float radius = GROUND_RADIUS + height;
    float horizonDistance = sqrt(ComputeSquaredHeightDifference(height, radius));
    float topHorizonDistance = sqrt(ComputeSquaredHeightDifference(SHELL_HEIGHT, TOP_RADIUS));
    float minimumDistance = SHELL_HEIGHT - height;
    float maximumDistance = horizonDistance + topHorizonDistance;
    return saturate(
        float2((GetDistanceToTop(height, zenithCosine) - minimumDistance) / (maximumDistance - minimumDistance),
               horizonDistance / topHorizonDistance));
}

float3 SampleAtmosphereRegion(Texture2D<float4> table, in float2 coordinates, in int2 sizePixel, int offsetPixel)
{
    float2 positionPixel = saturate(coordinates) * float2(sizePixel - 1);
    int2 lowerPixel = int2(positionPixel);
    int2 upperPixel = min(lowerPixel + 1, sizePixel - 1);
    float2 fraction = frac(positionPixel);
    lowerPixel.y += offsetPixel;
    upperPixel.y += offsetPixel;
    float3 top =
        lerp(table.Load(int3(lowerPixel, 0)).rgb, table.Load(int3(upperPixel.x, lowerPixel.y, 0)).rgb, fraction.x);
    float3 bottom =
        lerp(table.Load(int3(lowerPixel.x, upperPixel.y, 0)).rgb, table.Load(int3(upperPixel, 0)).rgb, fraction.x);
    return lerp(top, bottom, fraction.y);
}

float3 SampleTransmittance(Texture2D<float4> table, float height, float zenithCosine)
{
    return SampleAtmosphereRegion(table, ComputeTransmittanceCoordinates(height, zenithCosine), TRANSMITTANCE_SIZE, 0);
}

struct AtmosphereSegment
{
    float3 Radiance;
    float3 Transmittance;
    float3 ScatteringFraction;
};

float3 SampleAtmosphereMultiple(Texture2D<float4> table, float altitude, float sunCosine)
{
    return SampleAtmosphereRegion(table,
                                  float2(sunCosine * 0.5 + 0.5, altitude / SHELL_HEIGHT),
                                  int2(ATMOSPHERE_MULTIPLE_SIZE),
                                  0);
}

AtmosphereSegment IntegrateAtmosphere(Texture2D<float4> transmittanceTable,
                                      Texture2D<float4> multipleTable,
                                      float height,
                                      float zenithCosine,
                                      float solarZenithCosine,
                                      float scatteringCosine,
                                      float distance,
                                      uint stepCount,
                                      bool hasMultiple)
{
    const float phaseNumerator = 3;
    const float rayleighDenominator = 16;
    const float aerosolDenominator = 8;
    float rayleighPhase = phaseNumerator * (1 + scatteringCosine * scatteringCosine) / (rayleighDenominator * PI);
    float asymmetrySquared = AEROSOL_ASYMMETRY * AEROSOL_ASYMMETRY;
    float aerosolBase = 1 + asymmetrySquared - 2 * AEROSOL_ASYMMETRY * scatteringCosine;
    float aerosolPhase = phaseNumerator * (1 - asymmetrySquared) * (1 + scatteringCosine * scatteringCosine) /
                         (aerosolDenominator * PI * (2 + asymmetrySquared) * aerosolBase * sqrt(aerosolBase));
    float radius = GROUND_RADIUS + height;
    AtmosphereSegment segment;
    segment.Transmittance = 1;
    segment.Radiance = 0;
    segment.ScatteringFraction = 0;
    float previous = 0;
    for (uint step = 1; step <= stepCount; ++step)
    {
        float next = distance * ComputeStepBoundary(float(step) / stepCount);
        float midpoint = (previous + next) * 0.5;
        float altitude = ComputeAltitude(height, zenithCosine, midpoint);
        float sampleSolarCosine =
            clamp((radius * solarZenithCosine + midpoint * scatteringCosine) / (GROUND_RADIUS + altitude), -1, 1);
        float3 density = ComputeDensity(altitude);
        float stepLength = next - previous;
        float3 opticalDepth = ComputeExtinction(density) * stepLength;
        float3 stepTransmittance = exp(-opticalDepth);
        float3 integrated = select(opticalDepth < OPTICAL_DEPTH_LINEAR_THRESHOLD,
                                   1 - opticalDepth * 0.5,
                                   (1 - stepTransmittance) / max(opticalDepth, OPTICAL_DEPTH_LINEAR_THRESHOLD));
        float3 rayleigh = density.x * RAYLEIGH_SCATTERING;
        float3 aerosol = density.y * AEROSOL_SCATTERING;
        float3 weightedStep = segment.Transmittance * integrated * stepLength;
        if (sampleSolarCosine >= ComputeHorizonCosine(altitude))
        {
            segment.Radiance += weightedStep * SampleTransmittance(transmittanceTable, altitude, sampleSolarCosine) *
                                (rayleigh * rayleighPhase + aerosol * aerosolPhase);
        }
        if (hasMultiple)
        {
            segment.Radiance += weightedStep * SampleAtmosphereMultiple(multipleTable, altitude, sampleSolarCosine) *
                                (rayleigh + aerosol);
        }
        segment.ScatteringFraction += weightedStep * (rayleigh + aerosol);
        segment.Transmittance *= stepTransmittance;
        previous = next;
    }
    return segment;
}

float3 SampleAtmosphereSky(Texture2D<float4> skyTable,
                           in float3 directionWorld,
                           in float3 upWorld,
                           in float3 sunWorld,
                           float height)
{
    float zenithCosine = clamp(dot(directionWorld, upWorld), -1, 1);
    float solarZenithCosine = clamp(dot(sunWorld, upWorld), -1, 1);
    float horizon = ComputeHorizonCosine(height);
    bool hitsGround = zenithCosine < horizon;
    float range = hitsGround ? horizon + 1 : 1 - horizon;
    float ordinate = sqrt(saturate(abs(zenithCosine - horizon) / range));
    float3 horizontalWorld = directionWorld - zenithCosine * upWorld;
    float3 sunHorizontalWorld = sunWorld - solarZenithCosine * upWorld;
    float horizontalProduct = length(horizontalWorld) * length(sunHorizontalWorld);
    float azimuth = horizontalProduct > DIRECTION_EPSILON
                        ? acos(clamp(dot(horizontalWorld, sunHorizontalWorld) / horizontalProduct, -1, 1)) / PI
                        : 0;
    return SampleAtmosphereRegion(skyTable,
                                  float2(azimuth, ordinate),
                                  SKY_REGION_SIZE,
                                  hitsGround ? SKY_HALF_HEIGHT : 0);
}
