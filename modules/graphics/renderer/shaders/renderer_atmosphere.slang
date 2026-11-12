#include "display_tone_mapping.slang"
#include "lighting_layout.h"
#include "renderer_deferred_fullscreen.slang"

#include "renderer_atmosphere_celestial.slang"
#include "renderer_atmosphere_transport.slang"

struct AtmosphereViewParameters
{
    float4 ClipToEyeRows[4];
    float4 RadialUpWorld;       // xyz: unit radial up, w: altitude in kilometres
    float4 DirectionToSunWorld; // xyz: direction, w: solar zenith cosine
    float4 SolarIlluminance;    // xyz: linear RGB perpendicular illuminance, w: disk chord radius
    float4 CameraForwardWorld;  // xyz: orthographic ray, w: radiance per unit disk illuminance
    float4 Lighting;            // x enables multiple scattering.
    float4 MoonDirectionWorld;  // xyz: direction, w: sine of the angular radius.
    float4 LunarRadiance;       // xyz: full-phase peak luminance, w: disk chord radius.
    float4 Celestial;           // Observer altitude (km), star peak luminance, orbital flag, planet count.
    float4 Output;              // xy: viewport dimensions, z: exposure multiplier, w: display curve
    float4 GalacticNorthWorld;  // xyz: north galactic pole, w: Milky Way brightness.
    float4 GalacticCenterWorld; // xyz: galactic center, orthogonal to GalacticNorthWorld.
    float4 PlanetDirectionsWorld[ATMOSPHERE_PLANET_CAPACITY];
    float4 PlanetIlluminance[ATMOSPHERE_PLANET_CAPACITY];
};
LUDUS_SHADER_BINDING(0)
ConstantBuffer<AtmosphereViewParameters> atmosphereView LUDUS_SHADER_UNIFORM_REGISTER(0);
LUDUS_SHADER_BINDING(1)
Texture2D<float4> atmosphereTransmittance LUDUS_SHADER_TEXTURE_REGISTER(1);
LUDUS_SHADER_BINDING(2)
Texture2D<float4> atmosphereSky LUDUS_SHADER_TEXTURE_REGISTER(2);
LUDUS_SHADER_BINDING(3)
Texture2D<float4> atmosphereMultiple LUDUS_SHADER_TEXTURE_REGISTER(3);

[shader("fragment")] float4 AtmosphereTransmittanceMain(in DeferredVarying input) : SV_Target0
{
    float2 coordinates = (input.PositionClip.xy - 0.5) / float2(TRANSMITTANCE_SIZE - 1);
    float topHorizonDistance = sqrt(ComputeSquaredHeightDifference(SHELL_HEIGHT, TOP_RADIUS));
    float horizonDistance = coordinates.y * topHorizonDistance;
    float height = horizonDistance * horizonDistance /
                   (sqrt(GROUND_RADIUS * GROUND_RADIUS + horizonDistance * horizonDistance) + GROUND_RADIUS);
    height = min(height, SHELL_HEIGHT);
    float radius = GROUND_RADIUS + height;
    float distance = lerp(SHELL_HEIGHT - height, horizonDistance + topHorizonDistance, coordinates.x);
    float squaredDifference = (SHELL_HEIGHT - height) * (TOP_RADIUS + radius);
    float zenithCosine =
        distance > 0 ? clamp((squaredDifference - distance * distance) / (2 * radius * distance), -1, 1) : 1;
    return float4(IntegrateTransmittance(height, zenithCosine, distance), 1);
}

[shader("fragment")] float4 AtmosphereSkyMain(in DeferredVarying input) : SV_Target0
{
    float2 positionPixel = input.PositionClip.xy - 0.5;
    bool hitsGround = positionPixel.y >= SKY_HALF_HEIGHT;
    float2 coordinates = (positionPixel - float2(0, hitsGround ? SKY_HALF_HEIGHT : 0)) / float2(SKY_REGION_SIZE - 1);
    float height = atmosphereView.RadialUpWorld.w;
    float horizon = ComputeHorizonCosine(height);
    float zenithCosine = lerp(horizon, hitsGround ? -1.0 : 1.0, coordinates.y * coordinates.y);
    float solarZenithCosine = atmosphereView.DirectionToSunWorld.w;
    float sine = sqrt(saturate(1 - zenithCosine * zenithCosine));
    float solarSine = sqrt(saturate(1 - solarZenithCosine * solarZenithCosine));
    float scatteringCosine =
        clamp(zenithCosine * solarZenithCosine + sine * solarSine * cos(PI * coordinates.x), -1, 1);
    float distance = hitsGround ? GetDistanceToGround(height, zenithCosine) : GetDistanceToTop(height, zenithCosine);
    AtmosphereSegment segment = IntegrateAtmosphere(atmosphereTransmittance,
                                                    atmosphereMultiple,
                                                    height,
                                                    zenithCosine,
                                                    solarZenithCosine,
                                                    scatteringCosine,
                                                    distance,
                                                    SKY_STEPS,
                                                    atmosphereView.Lighting.x > 0);
    return float4(segment.Radiance, 1);
}

float3 GetViewDirectionWorld(in float2 positionPixel)
{
    if (any(atmosphereView.CameraForwardWorld.xyz != 0))
    {
        return atmosphereView.CameraForwardWorld.xyz;
    }
    float2 positionClip = positionPixel / atmosphereView.Output.xy * float2(2, -2) + float2(-1, 1);
    float4 positionHomogeneousClip = float4(positionClip, 0.5, 1);
    return normalize(float3(dot(atmosphereView.ClipToEyeRows[0], positionHomogeneousClip),
                            dot(atmosphereView.ClipToEyeRows[1], positionHomogeneousClip),
                            dot(atmosphereView.ClipToEyeRows[2], positionHomogeneousClip)) *
                     sign(dot(atmosphereView.ClipToEyeRows[3], positionHomogeneousClip)));
}

// Thanks to Sébastien Hillaire, A Scalable and Production Ready Sky and
// Atmosphere Rendering Technique, CGF 2020, §5.5, doi:10.1111/cgf.14050.
// Average first-order radiance and the return fraction over a uniform sphere.
// Close higher orders locally with the isotropic geometric series and a dark ground.
[shader("fragment")] float4 AtmosphereMultipleMain(in DeferredVarying input) : SV_Target0
{
    const uint ZENITH_SAMPLES = 4;
    const uint AZIMUTH_SAMPLES = 8;
    const uint MULTIPLE_STEPS = 24;
    const float MINIMUM_ESCAPE = 0.01;
    float2 coordinates = (input.PositionClip.xy - 0.5) / (ATMOSPHERE_MULTIPLE_SIZE - 1);
    float height = coordinates.y * SHELL_HEIGHT;
    float solarZenithCosine = coordinates.x * 2 - 1;
    float solarSine = sqrt(saturate(1 - solarZenithCosine * solarZenithCosine));
    float horizon = ComputeHorizonCosine(height);
    float3 radiance = 0;
    float3 fraction = 0;
    for (uint row = 0; row < ZENITH_SAMPLES; ++row)
    {
        float zenithCosine = (float(row) + 0.5) * 2 / ZENITH_SAMPLES - 1;
        float sine = sqrt(saturate(1 - zenithCosine * zenithCosine));
        float distance =
            zenithCosine < horizon ? GetDistanceToGround(height, zenithCosine) : GetDistanceToTop(height, zenithCosine);
        for (uint column = 0; column < AZIMUTH_SAMPLES; ++column)
        {
            float azimuth = (float(column) + 0.5) * 2 * PI / AZIMUTH_SAMPLES;
            float scatteringCosine = clamp(zenithCosine * solarZenithCosine + sine * solarSine * cos(azimuth), -1, 1);
            AtmosphereSegment segment = IntegrateAtmosphere(atmosphereTransmittance,
                                                            atmosphereTransmittance,
                                                            height,
                                                            zenithCosine,
                                                            solarZenithCosine,
                                                            scatteringCosine,
                                                            distance,
                                                            MULTIPLE_STEPS,
                                                            false);
            radiance += segment.Radiance;
            fraction += segment.ScatteringFraction;
        }
    }
    float sampleCount = float(ZENITH_SAMPLES * AZIMUTH_SAMPLES);
    return float4(radiance / max(sampleCount - fraction, sampleCount * MINIMUM_ESCAPE), 1);
}

struct AtmosphereBackgroundTransport
{
    float3 Radiance;
    float3 Transmittance;
};

AtmosphereBackgroundTransport EvaluateAtmosphereBackground(in float3 directionWorld)
{
    const uint ORBITAL_STEPS = 64;
    AtmosphereBackgroundTransport result;
    result.Radiance = 0;
    result.Transmittance = 1;
    float3 upWorld = atmosphereView.RadialUpWorld.xyz;
    float3 sunWorld = atmosphereView.DirectionToSunWorld.xyz;
    float height = atmosphereView.Celestial.x;
    float zenithCosine = clamp(dot(directionWorld, upWorld), -1, 1);
    if (atmosphereView.Celestial.z == 0)
    {
        result.Radiance = SampleAtmosphereSky(atmosphereSky, directionWorld, upWorld, sunWorld, height);
        result.Transmittance = zenithCosine < ComputeHorizonCosine(height)
                                   ? float3(0)
                                   : SampleTransmittance(atmosphereTransmittance, height, zenithCosine);
        return result;
    }

    // Work from the perpendicular point and near root, which avoids subtracting
    // two long distances when reconstructing the radial direction at shell entry.
    float radius = GROUND_RADIUS + height;
    float projection = radius * zenithCosine;
    float discriminant = projection * projection - (height - SHELL_HEIGHT) * (radius + TOP_RADIUS);
    if (projection >= 0 || discriminant <= 0)
    {
        return result;
    }
    float root = sqrt(discriminant);
    float3 perpendicularWorld = radius * (upWorld - zenithCosine * directionWorld);
    float3 entryUpWorld = normalize(perpendicularWorld - root * directionWorld);
    float entryCosine = -root / TOP_RADIUS;
    bool hitsGround = entryCosine < ComputeHorizonCosine(SHELL_HEIGHT);
    float distance = hitsGround ? GetDistanceToGround(SHELL_HEIGHT, entryCosine) : 2 * root;
    AtmosphereSegment segment = IntegrateAtmosphere(atmosphereTransmittance,
                                                    atmosphereMultiple,
                                                    SHELL_HEIGHT,
                                                    entryCosine,
                                                    clamp(dot(entryUpWorld, sunWorld), -1, 1),
                                                    clamp(dot(directionWorld, sunWorld), -1, 1),
                                                    distance,
                                                    ORBITAL_STEPS,
                                                    atmosphereView.Lighting.x > 0);
    result.Radiance = segment.Radiance;
    result.Transmittance = hitsGround ? float3(0) : segment.Transmittance;
    return result;
}

float3 EvaluateNightSky(in float3 directionWorld, float angularFootprint)
{
    float3 background = 0;
    float3 directionCelestial = directionWorld;
    if (atmosphereView.GalacticNorthWorld.w > 0)
    {
        float3 northWorld = atmosphereView.GalacticNorthWorld.xyz;
        float3 centerWorld = atmosphereView.GalacticCenterWorld.xyz;
        directionCelestial = float3(dot(directionWorld, centerWorld),
                                    dot(directionWorld, cross(northWorld, centerWorld)),
                                    dot(directionWorld, northWorld));
        background += EvaluateMilkyWay(directionCelestial, atmosphereView.GalacticNorthWorld.w);
    }
    if (atmosphereView.Celestial.y > 0)
    {
        background += EvaluateCelestialStars(directionCelestial, angularFootprint, atmosphereView.Celestial.y);
    }
    uint planetCount = uint(atmosphereView.Celestial.w);
    for (uint i = 0; i < planetCount; ++i)
    {
        float weight =
            ComputePlanetPointWeight(directionWorld, atmosphereView.PlanetDirectionsWorld[i].xyz, angularFootprint);
        background += weight * atmosphereView.PlanetIlluminance[i].xyz;
    }
    return background;
}

[shader("fragment")] float4 AtmosphereDisplayMain(in DeferredVarying input) : SV_Target0
{
    float3 directionWorld = GetViewDirectionWorld(input.PositionClip.xy);
    float3 sunWorld = atmosphereView.DirectionToSunWorld.xyz;
    float3 moonWorld = atmosphereView.MoonDirectionWorld.xyz;
    float angularFootprint = max(length(ddx(directionWorld)), length(ddy(directionWorld)));
    float sunCoverage =
        ComputeCelestialDiskCoverage(length(directionWorld - sunWorld), atmosphereView.SolarIlluminance.w);
    float moonCoverage =
        ComputeCelestialDiskCoverage(length(directionWorld - moonWorld), atmosphereView.LunarRadiance.w);
    float3 boundary = sunCoverage * atmosphereView.CameraForwardWorld.w * atmosphereView.SolarIlluminance.xyz;
    boundary += (1 - sunCoverage) * EvaluateNightSky(directionWorld, angularFootprint);
    if (atmosphereView.MoonDirectionWorld.w > 0 && moonCoverage > 0)
    {
        float phase = EvaluateLunarPhase(directionWorld, moonWorld, sunWorld, atmosphereView.MoonDirectionWorld.w);
        boundary = lerp(boundary, phase * atmosphereView.LunarRadiance.xyz, moonCoverage);
    }
    AtmosphereBackgroundTransport transport = EvaluateAtmosphereBackground(directionWorld);
    float3 radiance = transport.Radiance * atmosphereView.SolarIlluminance.xyz + transport.Transmittance * boundary;
    if (atmosphereView.Output.z > 0)
    {
        radiance = MapSceneToDisplay(radiance, atmosphereView.Output.z, uint(atmosphereView.Output.w + 0.5));
    }
    return float4(radiance, 1);
}
