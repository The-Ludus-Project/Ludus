#include "linearly_transformed_cosine.slang"
#include "raster_bindings.slang"
#include "ris_projected_solid_angle_weights_layout.h"

// Test-only kernel for ris_projected_solid_angle_weight_tests.cpp: prepares the ris-projected-solid-angle template's
// receiver frame and light techniques (render_graphs/ris_projected_solid_angle/main_shaders/
// projected_solid_angle_light.slang) for receivers viewed from below the horizon of their shading normal, which
// FoundationMath's ClipShadingNormalToOutgoingHemisphere clips as shade.slang does (Peters 2021, section 6.1). The host
// compares the results with RT1's CPU twin of these functions. ris_projected_solid_angle_weights_layout.h owns the ABI.
// A kernel of its own: in ris_projected_solid_angle_weights.slang's ComputeMain, these receivers made the Metal results
// of both kernels' receivers vary between runs on Apple M5, as large thread arrays have before
// (render-graph-authoring.md, the line-sampling template's visibility intervals); each kernel alone repeats bit for
// bit.

LUDUS_SHADER_BINDING(0) RWStructuredBuffer<uint4> Results;
// The host binds the renderer's LTC table here. Declared after Results: Slang numbers Metal buffers in declaration
// order.
#define LUDUS_LTC_TABLE_BINDING LUDUS_RIS_WEIGHTS_LTC_TABLE_BINDING
#include "ltc_table.slang"
#include "projected_solid_angle_light.slang"
#include "shading_normal.slang"

static const uint2 CLIPPED_TEST_RANDOM_KEY = uint2(0x52495331u, 0x434c4950u);
// Views lie below this cosine with the shading normal +z, so every one is clipped; those between zero and the margin
// are grazing views from above the horizon.
static const float CLIPPED_VIEW_MAXIMUM_COSINE = SHADING_NORMAL_MINIMUM_OUTGOING_COSINE;
// A quarter of the receivers use the LTC table's smallest roughness, where grazing views give LTC slivers.
static const float CLIPPED_MINIMUM_ROUGHNESS_SHARE = 0.25;

struct ClippedTestRandom
{
    uint Invocation;
    uint Dimension;
};

float DrawUniform(inout ClippedTestRandom inoutRandom)
{
    RandomAddress address = MakeRandomAddress(uint2(inoutRandom.Invocation, 0), 0, inoutRandom.Dimension);
    inoutRandom.Dimension += 1;
    return SampleRandomFloat01(CLIPPED_TEST_RANDOM_KEY, address);
}

uint4 PackFloats(float x, float y, float z, float w)
{
    return uint4(asuint(x), asuint(y), asuint(z), asuint(w));
}

// Peters 2021, section 6.1: a receiver whose view lies below the horizon of its shading normal +z, as normal
// mapping produces, and a drawn triangle. TryLoadShadingPoint clips the normal towards the view before
// MakeReceiverLightingFrame.
[shader("compute")][numthreads(LUDUS_RIS_WEIGHTS_GROUP_SIZE, 1, 1)] void
ClippedReceiverMain(uint3 dispatchThreadId : SV_DispatchThreadID) {
    uint invocation = dispatchThreadId.x;
    uint base = invocation * LUDUS_RIS_CLIPPED_RECORDS_PER_INVOCATION;
    ClippedTestRandom random;
    random.Invocation = invocation;
    random.Dimension = 0;

    float viewCosine = -1 + DrawUniform(random) * (1 + CLIPPED_VIEW_MAXIMUM_COSINE);
    float viewAzimuth = DrawUniform(random) * 2 * PI;
    float viewSine = sqrt(max(1 - viewCosine * viewCosine, 0));
    float3 viewWorld = float3(viewSine * cos(viewAzimuth), viewSine * sin(viewAzimuth), viewCosine);
    float3 normalWorld = ClipShadingNormalToOutgoingHemisphere(float3(0, 0, 1), viewWorld);
    float3 baseColor = float3(DrawUniform(random), DrawUniform(random), DrawUniform(random));
    float metallic = DrawUniform(random);
    float roughness = LUDUS_LTC_TABLE_MINIMUM_ROUGHNESS + DrawUniform(random) * (1 - LUDUS_LTC_TABLE_MINIMUM_ROUGHNESS);
    if (DrawUniform(random) < CLIPPED_MINIMUM_ROUGHNESS_SHARE)
        roughness = LUDUS_LTC_TABLE_MINIMUM_ROUGHNESS;
    float visibility = DrawUniform(random);
    ReceiverLightingFrame frame =
        MakeReceiverLightingFrame(float3(0), normalWorld, viewWorld, baseColor, metallic, roughness);
    float3 centerWorld =
        float3(DrawUniform(random) * 4 - 2, DrawUniform(random) * 4 - 2, DrawUniform(random) * 2 - 0.5);
    float3 triangleWorld[3];
    [unroll] for (uint corner = 0; corner < 3; ++corner)
    {
        triangleWorld[corner] =
            centerWorld + float3(DrawUniform(random) - 0.5, DrawUniform(random) - 0.5, DrawUniform(random) - 0.5);
    }
    TriangleLightPolygons polygons;
    bool isPrepared =
        TryPrepareTriangleLightPolygons(frame, triangleWorld[0], triangleWorld[1], triangleWorld[2], polygons);
    float3 sampledTangent = float3(0);
    bool isSampled = false;
    float2 techniqueSample = float2(DrawUniform(random), DrawUniform(random));
    if (isPrepared == true && DrawUniform(random) < 0.5)
        isSampled = TrySampleProjectedSolidAnglePolygon(polygons.Diffuse, techniqueSample, sampledTangent);
    else if (isPrepared == true && polygons.SpecularIntegral > 0)
        isSampled = TrySampleLinearlyTransformedCosinePolygon(polygons.Specular, techniqueSample, sampledTangent);
    // As SampleResampledLightTechnique: densities at the normalized world direction.
    float3 directionWorld = NormalizeOrZero(TransformTangentToWorld(frame, sampledTangent));
    float2 densities = EvaluateLightTechniqueDensities(polygons, TransformWorldToTangent(frame, directionWorld));
    float2 weights =
        ComputeClampedOptimalMisWeights(ComputeLightTechniqueCoefficients(frame, polygons, 1), densities, visibility);
    uint flags = (isPrepared == true ? LUDUS_RIS_CLIPPED_PREPARED : 0) |
                 (frame.HasSpecular == true ? LUDUS_RIS_CLIPPED_HAS_SPECULAR : 0) |
                 (isSampled == true ? LUDUS_RIS_CLIPPED_SAMPLED : 0);
    Results[base + LUDUS_RIS_CLIPPED_VIEW_RECORD] = PackFloats(viewWorld.x, viewWorld.y, viewWorld.z, roughness);
    Results[base + LUDUS_RIS_CLIPPED_MATERIAL_RECORD] = PackFloats(baseColor.x, baseColor.y, baseColor.z, metallic);
    Results[base + LUDUS_RIS_CLIPPED_TRIANGLE_RECORD] =
        uint4(asuint(triangleWorld[0].x), asuint(triangleWorld[0].y), asuint(triangleWorld[0].z), flags);
    Results[base + LUDUS_RIS_CLIPPED_TRIANGLE_RECORD + 1] =
        PackFloats(triangleWorld[1].x, triangleWorld[1].y, triangleWorld[1].z, frame.DiffuseAlbedoLuminance);
    Results[base + LUDUS_RIS_CLIPPED_TRIANGLE_RECORD + 2] =
        PackFloats(triangleWorld[2].x, triangleWorld[2].y, triangleWorld[2].z, frame.SpecularAlbedoLuminance);
    Results[base + LUDUS_RIS_CLIPPED_NORMAL_RECORD] =
        PackFloats(normalWorld.x, normalWorld.y, normalWorld.z, weights.x + weights.y);
    Results[base + LUDUS_RIS_CLIPPED_POLYGON_RECORD] =
        PackFloats(polygons.DiffuseIntegral, polygons.SpecularIntegral, densities.x, densities.y);
    Results[base + LUDUS_RIS_CLIPPED_DIRECTION_RECORD] =
        PackFloats(directionWorld.x, directionWorld.y, directionWorld.z, visibility);
}
