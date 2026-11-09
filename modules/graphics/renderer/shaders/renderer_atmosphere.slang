#include "display_tone_mapping.slang"
#include "renderer_deferred_fullscreen.slang"

#include "renderer_atmosphere_transport.slang"

struct AtmosphereViewParameters
{
    float4 ClipToEyeRows[4];
    float4 RadialUpWorld;       // xyz: unit radial up, w: altitude in kilometres
    float4 DirectionToSunWorld; // xyz: direction, w: solar zenith cosine
    float4 SolarIlluminance;    // xyz: linear RGB perpendicular illuminance, w: disk chord radius
    float4 CameraForwardWorld;  // xyz: orthographic ray, w: radiance per unit disk illuminance
    float4 Lighting;            // x enables multiple scattering.
    float4 Output;              // xy: viewport dimensions, z: exposure multiplier, w: display curve
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

[shader("fragment")] float4 AtmosphereDisplayMain(in DeferredVarying input) : SV_Target0
{
    float3 directionWorld = GetViewDirectionWorld(input.PositionClip.xy);
    float3 upWorld = atmosphereView.RadialUpWorld.xyz;
    float3 sunWorld = atmosphereView.DirectionToSunWorld.xyz;
    float height = atmosphereView.RadialUpWorld.w;
    float zenithCosine = clamp(dot(directionWorld, upWorld), -1, 1);
    float horizon = ComputeHorizonCosine(height);
    bool hitsGround = zenithCosine < horizon;
    float3 radiance = SampleAtmosphereSky(atmosphereSky, directionWorld, upWorld, sunWorld, height);
    float chord = length(directionWorld - sunWorld);
    float edge = max(fwidth(chord), DIRECTION_EPSILON);
    float diskCoverage = saturate(0.5 + (atmosphereView.SolarIlluminance.w - chord) / edge);
    if (diskCoverage > 0 && hitsGround == false)
    {
        radiance += SampleTransmittance(atmosphereTransmittance, height, zenithCosine) *
                    (diskCoverage * atmosphereView.CameraForwardWorld.w);
    }
    radiance *= atmosphereView.SolarIlluminance.xyz;
    if (atmosphereView.Output.z > 0)
    {
        radiance = MapSceneToDisplay(radiance, atmosphereView.Output.z, uint(atmosphereView.Output.w + 0.5));
    }
    return float4(radiance, 1);
}
