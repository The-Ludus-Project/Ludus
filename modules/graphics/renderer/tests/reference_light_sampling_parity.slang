#include "linearly_transformed_cosine.slang"
#include "raster_bindings.slang"
#include "reference_light_sampling_parity_layout.h"
#include "renderer_reference_layout.h"

// Test-only kernel for reference_light_sampling_parity_tests.cpp: runs the GPU-compute reference's scene-independent
// resampled-light pieces (shaders/reference_light_sampling.slang) on drawn inputs, so the host can check that the
// technique weights partition unity, that Z-hat = 0 applies the inclusion bound and that an empty LTC-space clip
// disables the specular technique, and compare the receiver frame and polygons with src/reference_light_sampling.cpp.

LUDUS_SHADER_BINDING(0) RWStructuredBuffer<uint4> Results;
// The host binds the renderer's LTC table here. Declared after Results: Slang numbers Metal buffers in declaration
// order.
#define LUDUS_LTC_TABLE_BINDING LUDUS_REFERENCE_LIGHT_PARITY_LTC_TABLE_BINDING
#include "ltc_table.slang"
#include "reference_light_sampling.slang"

static const uint2 PARITY_RANDOM_KEY = uint2(0x52545632u, 0x4c494748u);
// Shares of drawn values set to exactly zero, so zero-density and zero-coefficient branches are covered.
static const float ZERO_INPUT_SHARE = 0.25;
// Drawn densities and probabilities span 2^-12 to 2^12 around one.
static const float LOG2_INPUT_RANGE = 24;

struct ParityRandom
{
    uint Invocation;
    uint Dimension;
};

float DrawUniform(inout ParityRandom inoutRandom)
{
    RandomAddress address = MakeRandomAddress(uint2(inoutRandom.Invocation, 0), 0, inoutRandom.Dimension);
    inoutRandom.Dimension += 1;
    return SampleRandomFloat01(PARITY_RANDOM_KEY, address);
}

// Zero with ZERO_INPUT_SHARE, otherwise log-uniform over LOG2_INPUT_RANGE octaves.
float DrawNonnegative(inout ParityRandom inoutRandom)
{
    float selector = DrawUniform(inoutRandom);
    float magnitude = exp2((DrawUniform(inoutRandom) - 0.5) * LOG2_INPUT_RANGE);
    return selector < ZERO_INPUT_SHARE ? 0 : magnitude;
}

uint4 PackFloats(float x, float y, float z, float w)
{
    return uint4(asuint(x), asuint(y), asuint(z), asuint(w));
}

[shader("compute")][numthreads(LUDUS_REFERENCE_LIGHT_PARITY_GROUP_SIZE, 1, 1)] void
ComputeMain(uint3 dispatchThreadId : SV_DispatchThreadID) {
    uint invocation = dispatchThreadId.x;
    uint base = invocation * LUDUS_REFERENCE_LIGHT_PARITY_RECORDS_PER_INVOCATION;
    ParityRandom random;
    random.Invocation = invocation;
    random.Dimension = 0;

    // Clamped optimal MIS over arbitrary nonnegative inputs.
    float2 coefficients = float2(DrawNonnegative(random), DrawNonnegative(random));
    float2 densities = float2(DrawNonnegative(random), DrawNonnegative(random));
    float visibility = DrawUniform(random);
    float2 weights = ComputeReferenceLightTechniqueWeights(coefficients, densities, visibility);
    Results[base + LUDUS_REFERENCE_LIGHT_PARITY_WEIGHT_INPUT_RECORD] =
        PackFloats(coefficients.x, coefficients.y, densities.x, densities.y);
    Results[base + LUDUS_REFERENCE_LIGHT_PARITY_WEIGHT_RECORD] = PackFloats(visibility, weights.x, weights.y, 0);

    // The surrogate density, with Z-hat = 0 for every second invocation.
    float emissiveCategoryProbability = DrawUniform(random);
    float target = DrawNonnegative(random);
    float sourcePmf = DrawUniform(random) * DrawUniform(random);
    float targetTotal = invocation % 2 == 0 ? 0 : DrawNonnegative(random);
    Results[base + LUDUS_REFERENCE_LIGHT_PARITY_SURROGATE_INPUT_RECORD] =
        PackFloats(emissiveCategoryProbability, target, sourcePmf, targetTotal);
    float surrogate = EvaluateReferenceSurrogateProbability(emissiveCategoryProbability,
                                                            target,
                                                            sourcePmf,
                                                            targetTotal,
                                                            LUDUS_REFERENCE_RIS_CANDIDATE_COUNT);
    Results[base + LUDUS_REFERENCE_LIGHT_PARITY_SURROGATE_RECORD] = PackFloats(surrogate, 0, 0, 0);

    // The receiver chain: a drawn view, material and triangle, sampled by a drawn technique.
    float viewCosine = max(DrawUniform(random), 0.05);
    float viewAzimuth = DrawUniform(random) * 2 * PI;
    float viewSine = sqrt(1 - viewCosine * viewCosine);
    float3 viewWorld = float3(viewSine * cos(viewAzimuth), viewSine * sin(viewAzimuth), viewCosine);
    float roughness = 0.045 + 0.955 * DrawUniform(random);
    float3 baseColor = float3(DrawUniform(random), DrawUniform(random), DrawUniform(random));
    float metallic = DrawUniform(random) < 0.5 ? 1 : DrawUniform(random);
    Results[base + LUDUS_REFERENCE_LIGHT_PARITY_VIEW_RECORD] =
        PackFloats(viewWorld.x, viewWorld.y, viewWorld.z, roughness);
    Results[base + LUDUS_REFERENCE_LIGHT_PARITY_MATERIAL_RECORD] =
        PackFloats(baseColor.x, baseColor.y, baseColor.z, metallic);
    ReferenceReceiverFrame frame =
        MakeReferenceReceiverFrame(float3(0), float3(0, 0, 1), viewWorld, baseColor, metallic, roughness);
    float3 centerWorld =
        float3(DrawUniform(random) * 4 - 2, DrawUniform(random) * 4 - 2, DrawUniform(random) * 2 - 0.5);
    float3 triangleWorld[3];
    [unroll] for (uint corner = 0; corner < 3; ++corner)
    {
        triangleWorld[corner] =
            centerWorld + float3(DrawUniform(random) - 0.5, DrawUniform(random) - 0.5, DrawUniform(random) - 0.5);
        Results[base + LUDUS_REFERENCE_LIGHT_PARITY_VERTEX_RECORD + corner] =
            PackFloats(triangleWorld[corner].x, triangleWorld[corner].y, triangleWorld[corner].z, 0);
    }
    ReferenceTriangleLight light;
    bool isPrepared =
        TryPrepareReferenceTriangleLight(frame, triangleWorld[0], triangleWorld[1], triangleWorld[2], light);
    Results[base + LUDUS_REFERENCE_LIGHT_PARITY_POLYGON_RECORD] = uint4(isPrepared == true ? 1 : 0,
                                                                        asuint(light.DiffuseIntegral),
                                                                        asuint(light.SpecularIntegral),
                                                                        asuint(frame.DiffuseAlbedoLuminance));
    float2 techniqueRandom = float2(DrawUniform(random), DrawUniform(random));
    uint technique = DrawUniform(random) < 0.5 ? REFERENCE_DIFFUSE_TECHNIQUE : REFERENCE_SPECULAR_TECHNIQUE;
    float3 directionTangent = float3(0);
    bool isSampled = false;
    if (isPrepared == true && technique == REFERENCE_DIFFUSE_TECHNIQUE)
        isSampled = TrySampleProjectedSolidAnglePolygon(light.Diffuse, techniqueRandom, directionTangent);
    else if (isPrepared == true && light.SpecularIntegral > 0)
        isSampled = TrySampleLinearlyTransformedCosinePolygon(light.Specular, techniqueRandom, directionTangent);
    float3 directionWorld = NormalizeOrZero(TransformReferenceTangentToWorld(frame, directionTangent));
    float2 sampledDensities = EvaluateReferenceLightTechniqueDensities(frame, light, directionWorld);
    Results[base + LUDUS_REFERENCE_LIGHT_PARITY_DENSITY_RECORD] = uint4(asuint(frame.SpecularAlbedoLuminance),
                                                                        frame.HasSpecular == true ? 1 : 0,
                                                                        asuint(sampledDensities.x),
                                                                        asuint(sampledDensities.y));
    Results[base + LUDUS_REFERENCE_LIGHT_PARITY_DIRECTION_RECORD] =
        uint4(asuint(directionWorld.x), asuint(directionWorld.y), asuint(directionWorld.z), isSampled == true ? 1 : 0);
    Results[base + LUDUS_REFERENCE_LIGHT_PARITY_RANDOM_RECORD] =
        uint4(asuint(techniqueRandom.x), asuint(techniqueRandom.y), technique, 0);
    float distanceWorld;
    float2 barycentric;
    bool isHit = TryIntersectReferenceTrianglePlane(float3(0),
                                                    directionWorld,
                                                    triangleWorld[0],
                                                    triangleWorld[1],
                                                    triangleWorld[2],
                                                    distanceWorld,
                                                    barycentric);
    Results[base + LUDUS_REFERENCE_LIGHT_PARITY_PLANE_RECORD] =
        uint4(isHit == true ? 1 : 0, asuint(distanceWorld), asuint(barycentric.x), asuint(barycentric.y));

    // An empty LTC-space clip: the LTC is zero over a triangle that lies above the shading horizon.
    float3 inverseTransform[3];
    inverseTransform[0] = float3(1, 0, 1);
    inverseTransform[1] = float3(0, 1, 0);
    inverseTransform[2] = float3(0, 0, LUDUS_REFERENCE_LIGHT_PARITY_EMPTY_CLIP_M22);
    ReferenceReceiverFrame clipped = frame;
    clipped.HasSpecular = TryMakeLinearlyTransformedCosine(inverseTransform, clipped.Specular);
    float clipHeight = 0.1 + 0.2 * DrawUniform(random);
    ReferenceTriangleLight emptyClip;
    bool isClipPrepared = TryPrepareReferenceTriangleLight(clipped,
                                                           float3(-2, -0.2, clipHeight),
                                                           float3(-2, 0.2, clipHeight),
                                                           float3(-2, 0, clipHeight + 0.2),
                                                           emptyClip);
    float3 clipTangent;
    bool isClipSampled = TrySampleProjectedSolidAnglePolygon(emptyClip.Diffuse,
                                                             float2(DrawUniform(random), DrawUniform(random)),
                                                             clipTangent);
    float2 clipDensities = EvaluateReferenceLightTechniqueDensities(clipped, emptyClip, clipTangent);
    float2 clipWeights =
        ComputeReferenceLightTechniqueWeights(float2(emptyClip.DiffuseIntegral, emptyClip.SpecularIntegral),
                                              clipDensities,
                                              visibility);
    Results[base + LUDUS_REFERENCE_LIGHT_PARITY_EMPTY_CLIP_RECORD] =
        uint4((isClipPrepared == true && isClipSampled == true && clipped.HasSpecular == true) ? 1 : 0,
              asuint(emptyClip.DiffuseIntegral),
              asuint(emptyClip.SpecularIntegral),
              asuint(clipWeights.y));
}
