#include "math.slang"
#include "portable_flow.slang"
#include "raster_bindings.slang"
#include "slice_sampling.slang"
// Execute the exported graph's exact flow code on raw inputs, independently of a scene or display conversion.
LUDUS_SHADER_BINDING(0) StructuredBuffer<float4> Inputs;
LUDUS_SHADER_BINDING(1) RWStructuredBuffer<float4> Results;
static const uint VALIDATION_GROUP_SIZE = 32;
static const uint VALIDATION_INPUT_RECORDS = 2;
static const uint VALIDATION_OUTPUT_RECORDS = 5;
static const uint VALIDATION_RADIANCE_RECORD = 3;

[shader("compute")][numthreads(VALIDATION_GROUP_SIZE, 1, 1)] void
ValidatePortableFlow(uint3 threadIndex : SV_DispatchThreadID) {
    uint count;
    uint stride;
    Inputs.GetDimensions(count, stride);
    uint index = threadIndex.x;
    if (index >= count / VALIDATION_INPUT_RECORDS)
        return;
    float4 input = Inputs[index * VALIDATION_INPUT_RECORDS];
    float3 positionWorld = input.xyz;
    float2 seed = float2(input.w, Inputs[index * VALIDATION_INPUT_RECORDS + 1].x);
    float distanceWorld = length(positionWorld - LUMINAIRE_CENTER_WORLD);
    float thetaCap = asin(PURELIGHT_RADIUS_WORLD / distanceWorld);
    float logDensity;
    float2 value = TransformPureLightFlow(positionWorld, thetaCap, seed, true, logDensity);
    float replayDensity;
    float2 restored = TransformPureLightFlow(positionWorld, thetaCap, value, false, replayDensity);
    float densitySolidAngle;
    float3 directionWorld = SamplePureLightCap(positionWorld, seed, densitySolidAngle);
    LuminaireProfile profile;
    profile.RadianceScale = PURELIGHT_PROFILE_RADIANCE_SCALE;
    profile.BeamWidth = PURELIGHT_PROFILE_BEAM_WIDTH;
    profile.BeamSteering = PURELIGHT_PROFILE_BEAM_STEERING;
    profile.BeamSpread = PURELIGHT_PROFILE_BEAM_SPREAD;
    profile.BeamTwist = PURELIGHT_PROFILE_BEAM_TWIST;
    LuminaireLightSample light = SamplePortablePureLight(profile, MakeLuminaireReceiver(positionWorld), seed);
    Results[index * VALIDATION_OUTPUT_RECORDS] = float4(value, logDensity, replayDensity);
    Results[index * VALIDATION_OUTPUT_RECORDS + 1] = float4(restored, directionWorld.xy);
    Results[index * VALIDATION_OUTPUT_RECORDS + 2] = float4(directionWorld.z, densitySolidAngle, 0, 0);
    Results[index * VALIDATION_OUTPUT_RECORDS + VALIDATION_RADIANCE_RECORD] =
        float4(light.RadianceOverDensity, light.HasSample == true ? 1 : 0);
    Results[index * VALIDATION_OUTPUT_RECORDS + VALIDATION_OUTPUT_RECORDS - 1] = float4(light.DirectionWorld, 0);
}
