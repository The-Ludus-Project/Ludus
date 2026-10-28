#include "linearly_transformed_cosine.slang"
#include "ltc_table.slang"
#include "projected_solid_angle_light.slang"
#include "raster_bindings.slang"
#include "ris_projected_solid_angle_weights_layout.h"

// Test-only kernel for ris_projected_solid_angle_weight_tests.cpp: runs the ris-projected-solid-angle template's
// scene-independent weights (render_graphs/ris_projected_solid_angle/main_shaders/projected_solid_angle_light.slang)
// on drawn inputs, so the host can check that they partition unity, that Z-hat = 0 applies the inclusion bound and
// that an empty LTC-space clip disables the specular technique. ris_projected_solid_angle_weights_layout.h owns the
// ABI.

LUDUS_SHADER_BINDING(0) RWStructuredBuffer<uint4> Results;

static const uint2 WEIGHT_TEST_RANDOM_KEY = uint2(0x52495331u, 0x50534131u);
// Shares of drawn values set to exactly zero, so zero-density and zero-coefficient branches are covered.
static const float ZERO_INPUT_SHARE = 0.25;
// Drawn densities and probabilities span 2^-12 to 2^12 around one.
static const float LOG2_INPUT_RANGE = 24;
static const uint RIS_TEST_CANDIDATE_COUNT = 32;
// The empty-clip LTC rotates the shading frame by 150 degrees about x, so the zenith maps below the horizon.
static const float EMPTY_CLIP_ROTATION_COSINE = -0.8660254037844386;
static const float EMPTY_CLIP_ROTATION_SINE = 0.5;
static const float EMPTY_CLIP_HEIGHT_WORLD = 2;
static const float EMPTY_CLIP_RADIUS_WORLD = 0.3;

struct WeightTestRandom
{
    uint Invocation;
    uint Dimension;
};

float DrawUniform(inout WeightTestRandom inoutRandom)
{
    RandomAddress address = MakeRandomAddress(uint2(inoutRandom.Invocation, 0), 0, inoutRandom.Dimension);
    inoutRandom.Dimension += 1;
    return SampleRandomFloat01(WEIGHT_TEST_RANDOM_KEY, address);
}

// Zero with ZERO_INPUT_SHARE, otherwise log-uniform over LOG2_INPUT_RANGE octaves.
float DrawNonnegative(inout WeightTestRandom inoutRandom)
{
    float selector = DrawUniform(inoutRandom);
    float magnitude = exp2((DrawUniform(inoutRandom) - 0.5) * LOG2_INPUT_RANGE);
    return selector < ZERO_INPUT_SHARE ? 0 : magnitude;
}

uint4 PackFloats(float x, float y, float z, float w)
{
    return uint4(asuint(x), asuint(y), asuint(z), asuint(w));
}

// A frame at the origin with +z up, its specular LTC replaced by inverseTransform.
ReceiverLightingFrame MakeTestFrame(float3 inverseTransform[3])
{
    ReceiverLightingFrame frame =
        MakeReceiverLightingFrame(float3(0), float3(0, 0, 1), float3(0, 0, 1), float3(0.5), 0, 0.5);
    frame.HasSpecular = TryMakeLinearlyTransformedCosine(inverseTransform, frame.Specular);
    return frame;
}

[shader("compute")][numthreads(LUDUS_RIS_WEIGHTS_GROUP_SIZE, 1, 1)] void
ComputeMain(uint3 dispatchThreadId : SV_DispatchThreadID) {
    uint invocation = dispatchThreadId.x;
    uint base = invocation * LUDUS_RIS_WEIGHTS_RECORDS_PER_INVOCATION;
    WeightTestRandom random;
    random.Invocation = invocation;
    random.Dimension = 0;

    // Clamped optimal MIS over arbitrary nonnegative inputs.
    float2 coefficients = float2(DrawNonnegative(random), DrawNonnegative(random));
    float2 densities = float2(DrawNonnegative(random), DrawNonnegative(random));
    float visibility = DrawUniform(random);
    float2 weights = ComputeClampedOptimalMisWeights(coefficients, densities, visibility);
    Results[base + LUDUS_RIS_WEIGHTS_CLAMPED_RECORD] = PackFloats(weights.x, weights.y, densities.x, densities.y);
    Results[base + LUDUS_RIS_WEIGHTS_CLAMPED_INPUT_RECORD] = PackFloats(coefficients.x, coefficients.y, visibility, 0);

    // The light-group/BSDF split.
    float lightDensity = DrawNonnegative(random);
    float bsdfDensity = DrawNonnegative(random);
    Results[base + LUDUS_RIS_WEIGHTS_POWER_RECORD] = PackFloats(ComputePowerHeuristicWeight(lightDensity, bsdfDensity),
                                                                ComputePowerHeuristicWeight(bsdfDensity, lightDensity),
                                                                lightDensity,
                                                                bsdfDensity);

    // The surrogate density, with Z-hat = 0 for every second invocation.
    float emissiveCategoryProbability = DrawUniform(random);
    float target = DrawNonnegative(random);
    float sourcePmf = DrawUniform(random) * DrawUniform(random);
    float normalizationEstimate = invocation % 2 == 0 ? 0 : DrawNonnegative(random);
    Results[base + LUDUS_RIS_WEIGHTS_SURROGATE_INPUT_RECORD] =
        PackFloats(emissiveCategoryProbability, target, sourcePmf, normalizationEstimate);
    float surrogate = EvaluateSurrogateTriangleProbability(emissiveCategoryProbability,
                                                           target,
                                                           sourcePmf,
                                                           normalizationEstimate,
                                                           RIS_TEST_CANDIDATE_COUNT);
    Results[base + LUDUS_RIS_WEIGHTS_SURROGATE_RECORD] = uint4(asuint(surrogate), RIS_TEST_CANDIDATE_COUNT, 0, 0);

    // An empty LTC-space clip: the rotated LTC maps a triangle near the zenith below its horizon.
    float3 rotation[3];
    rotation[0] = float3(1, 0, 0);
    rotation[1] = float3(0, EMPTY_CLIP_ROTATION_COSINE, EMPTY_CLIP_ROTATION_SINE);
    rotation[2] = float3(0, -EMPTY_CLIP_ROTATION_SINE, EMPTY_CLIP_ROTATION_COSINE);
    ReceiverLightingFrame rotated = MakeTestFrame(rotation);
    float phase = DrawUniform(random) * 2 * PI;
    float3 corners[3];
    [unroll] for (uint corner = 0; corner < 3; ++corner)
    {
        float angle = phase + float(corner) * 2 * PI / 3;
        corners[corner] =
            float3(cos(angle) * EMPTY_CLIP_RADIUS_WORLD, sin(angle) * EMPTY_CLIP_RADIUS_WORLD, EMPTY_CLIP_HEIGHT_WORLD);
    }
    TriangleLightPolygons emptyClip;
    bool isPrepared = TryPrepareTriangleLightPolygons(rotated, corners[0], corners[1], corners[2], emptyClip);
    float3 diffuseTangent;
    bool isDiffuseSampled = TrySampleProjectedSolidAnglePolygon(emptyClip.Diffuse,
                                                                float2(DrawUniform(random), DrawUniform(random)),
                                                                diffuseTangent);
    float2 emptyClipDensities = EvaluateLightTechniqueDensities(emptyClip, diffuseTangent);
    float2 emptyClipWeights = ComputeClampedOptimalMisWeights(ComputeLightTechniqueCoefficients(rotated, emptyClip, 1),
                                                              emptyClipDensities,
                                                              visibility);
    Results[base + LUDUS_RIS_WEIGHTS_EMPTY_CLIP_RECORD] = uint4(isPrepared == true ? 1 : 0,
                                                                asuint(emptyClip.DiffuseIntegral),
                                                                asuint(emptyClip.SpecularIntegral),
                                                                asuint(emptyClipDensities.y));
    Results[base + LUDUS_RIS_WEIGHTS_EMPTY_CLIP_WEIGHT_RECORD] = uint4(asuint(emptyClipWeights.x),
                                                                       asuint(emptyClipWeights.y),
                                                                       isDiffuseSampled == true ? 1 : 0,
                                                                       asuint(emptyClipDensities.x));

    // The full receiver chain: a drawn view, material and triangle above the receiver, sampled by a drawn technique.
    float viewCosine = max(DrawUniform(random), 0.05);
    float viewAzimuth = DrawUniform(random) * 2 * PI;
    float viewSine = sqrt(1 - viewCosine * viewCosine);
    float3 viewWorld = float3(viewSine * cos(viewAzimuth), viewSine * sin(viewAzimuth), viewCosine);
    ReceiverLightingFrame frame = MakeReceiverLightingFrame(float3(0),
                                                            float3(0, 0, 1),
                                                            viewWorld,
                                                            float3(DrawUniform(random), DrawUniform(random), 0.5),
                                                            DrawUniform(random) < 0.5 ? 1 : DrawUniform(random),
                                                            DrawUniform(random));
    float3 centerWorld =
        float3(DrawUniform(random) * 4 - 2, DrawUniform(random) * 4 - 2, DrawUniform(random) * 2 - 0.5);
    float3 triangleWorld[3];
    [unroll] for (uint corner = 0; corner < 3; ++corner)
    {
        triangleWorld[corner] =
            centerWorld + float3(DrawUniform(random) - 0.5, DrawUniform(random) - 0.5, DrawUniform(random) - 0.5);
    }
    TriangleLightPolygons polygons;
    bool isReceiverPrepared =
        TryPrepareTriangleLightPolygons(frame, triangleWorld[0], triangleWorld[1], triangleWorld[2], polygons);
    float3 sampledTangent = float3(0);
    bool isSampled = false;
    float2 techniqueSample = float2(DrawUniform(random), DrawUniform(random));
    if (isReceiverPrepared == true && DrawUniform(random) < 0.5)
        isSampled = TrySampleProjectedSolidAnglePolygon(polygons.Diffuse, techniqueSample, sampledTangent);
    else if (isReceiverPrepared == true)
        isSampled = TrySampleLinearlyTransformedCosinePolygon(polygons.Specular, techniqueSample, sampledTangent);
    float2 receiverDensities = EvaluateLightTechniqueDensities(polygons, sampledTangent);
    float2 receiverWeights = ComputeClampedOptimalMisWeights(ComputeLightTechniqueCoefficients(frame, polygons, 1),
                                                             receiverDensities,
                                                             visibility);
    Results[base + LUDUS_RIS_WEIGHTS_RECEIVER_RECORD] = uint4(asuint(receiverWeights.x + receiverWeights.y),
                                                              asuint(receiverDensities.x),
                                                              asuint(receiverDensities.y),
                                                              isSampled == true ? 1 : 0);
}
