// Test-only kernel for polygonal_light_sampling_parity_tests.cpp: runs the polygonal-light-sampling template's four
// estimators (render_graphs/polygonal_light_sampling/main_shaders/polygonal_light.slang) on drawn receivers, materials
// and emissive triangles, so the host can compare their raw linear radiance with the CPU oracle
// (polygonal_light_sampling_oracle.cpp). The triangle is unoccluded and its emission constant, which is what the
// template's EvaluateEmission and TestVisibility return for such a scene. polygonal_light_sampling_estimators_layout.h
// owns the ABI. The kernel includes the renderer's scene shader as graph wrappers do, for EvaluateStandardBsdf, and
// binds only the results and the LTC table (see the layout for their slots). renderer_gpu_preview.slang has its own
// ComputeMain, so this entry point has another name.
#include "polygonal_light_sampling_estimators_layout.h"
#include "renderer_gpu_preview.slang"
#include "scene_shader.slang"

LUDUS_SHADER_BINDING(LUDUS_GRAPH_BUFFER0_BINDING) RWStructuredBuffer<uint4> Results;
// Declared after Results: Slang numbers Metal buffers in declaration order.
#define LUDUS_LTC_TABLE_BINDING LUDUS_POLYGONAL_LIGHT_ESTIMATORS_LTC_TABLE_BINDING
#include "linearly_transformed_cosine.slang"
#include "ltc_table.slang"
#include "polygonal_light.slang"
#include "projected_solid_angle.slang"

static const uint2 ESTIMATOR_TEST_RANDOM_KEY = uint2(0x504c5331u, 0x45535431u);
// Shares of drawn materials: pure dielectric, pure metal, and otherwise a uniform metallic fraction.
static const float DIELECTRIC_SHARE = 1.0 / 3;
static const float METAL_SHARE = 2.0 / 3;
// A quarter of the receivers use the LTC table's smallest roughness, where the specular technique dominates.
static const float MINIMUM_ROUGHNESS_SHARE = 0.25;
static const float MINIMUM_ROUGHNESS = LUDUS_LTC_TABLE_MINIMUM_ROUGHNESS;
// Views stay at least this cosine above the shading horizon.
static const float MINIMUM_VIEW_COSINE = 0.05;
// Triangle centers lie at elevations down to this cosine, so some triangles straddle or sink below the horizon.
static const float MINIMUM_LIGHT_COSINE = -0.3;
static const float MINIMUM_LIGHT_DISTANCE_WORLD = 0.5;
static const float MAXIMUM_LIGHT_DISTANCE_WORLD = 4;
// Vertices lie within this fraction of the center's distance from it.
static const float MINIMUM_LIGHT_SIZE = 0.05;
static const float MAXIMUM_LIGHT_SIZE = 0.8;
static const float MINIMUM_EMISSION = 0.2;
static const float MAXIMUM_EMISSION = 4;
// Visibility estimates: the balance heuristic (0), the weighted balance heuristic (1), Peters' default (1/2) and a
// uniform value, a quarter each.
static const uint VISIBILITY_CHOICE_COUNT = 4;

struct EstimatorTestRandom
{
    uint Invocation;
    uint Dimension;
};

float DrawUniform(inout EstimatorTestRandom inoutRandom)
{
    RandomAddress address = MakeRandomAddress(uint2(inoutRandom.Invocation, 0), 0, inoutRandom.Dimension);
    inoutRandom.Dimension += 1;
    return SampleRandomFloat01(ESTIMATOR_TEST_RANDOM_KEY, address);
}

float DrawBetween(inout EstimatorTestRandom inoutRandom, float minimum, float maximum)
{
    return minimum + (maximum - minimum) * DrawUniform(inoutRandom);
}

/// A unit direction at a drawn azimuth whose cosine against axisWorld lies in [minimumCosine, 1].
float3 DrawDirection(inout EstimatorTestRandom inoutRandom, float3 axisWorld, float minimumCosine)
{
    float cosine = DrawBetween(inoutRandom, minimumCosine, 1);
    float sine = sqrt(max(1 - cosine * cosine, 0));
    float azimuth = DrawUniform(inoutRandom) * 2 * PI;
    float3 helperWorld = abs(axisWorld.z) < 0.5 ? float3(0, 0, 1) : float3(1, 0, 0);
    float3 tangentWorld = NormalizeOrZero(cross(helperWorld, axisWorld));
    float3 bitangentWorld = cross(axisWorld, tangentWorld);
    return NormalizeOrZero(axisWorld * cosine + (tangentWorld * cos(azimuth) + bitangentWorld * sin(azimuth)) * sine);
}

uint4 PackFloats(float3 xyz, float w)
{
    return uint4(asuint(xyz), asuint(w));
}

[shader("compute")][numthreads(LUDUS_POLYGONAL_LIGHT_ESTIMATORS_GROUP_SIZE, 1, 1)] void
PolygonalLightEstimatorsMain(uint3 dispatchThreadId : SV_DispatchThreadID) {
    uint invocation = dispatchThreadId.x;
    uint base = invocation * LUDUS_POLYGONAL_LIGHT_ESTIMATORS_RECORDS_PER_INVOCATION;
    EstimatorTestRandom random;
    random.Invocation = invocation;
    random.Dimension = 0;

    float3 normalWorld = DrawDirection(random, float3(0, 0, 1), -1);
    float3 viewWorld = DrawDirection(random, normalWorld, MINIMUM_VIEW_COSINE);
    MaterialSample material = (MaterialSample)0;
    material.BaseColor = float3(DrawUniform(random), DrawUniform(random), DrawUniform(random));
    float metallicSelector = DrawUniform(random);
    float metallic = DrawUniform(random);
    material.Metallic = metallicSelector < DIELECTRIC_SHARE ? 0 : (metallicSelector < METAL_SHARE ? 1 : metallic);
    float roughness = DrawBetween(random, MINIMUM_ROUGHNESS, 1);
    material.Roughness = DrawUniform(random) < MINIMUM_ROUGHNESS_SHARE ? MINIMUM_ROUGHNESS : roughness;
    material.ShadingNormalWorld = normalWorld;
    material.IsValid = true;
    float visibilityChoices[VISIBILITY_CHOICE_COUNT] = {0, 1, 0.5, DrawUniform(random)};
    float visibility =
        visibilityChoices[min(uint(DrawUniform(random) * VISIBILITY_CHOICE_COUNT), VISIBILITY_CHOICE_COUNT - 1)];

    float distanceWorld = DrawBetween(random, MINIMUM_LIGHT_DISTANCE_WORLD, MAXIMUM_LIGHT_DISTANCE_WORLD);
    float3 centerWorld = DrawDirection(random, normalWorld, MINIMUM_LIGHT_COSINE) * distanceWorld;
    float sizeWorld = DrawBetween(random, MINIMUM_LIGHT_SIZE, MAXIMUM_LIGHT_SIZE) * distanceWorld;
    float3 positionsWorld[3];
    [unroll] for (uint vertex = 0; vertex < 3; ++vertex)
    {
        positionsWorld[vertex] = centerWorld + DrawDirection(random, float3(0, 0, 1), -1) * sizeWorld;
    }
    float3 emission = float3(DrawBetween(random, MINIMUM_EMISSION, MAXIMUM_EMISSION),
                             DrawBetween(random, MINIMUM_EMISSION, MAXIMUM_EMISSION),
                             DrawBetween(random, MINIMUM_EMISSION, MAXIMUM_EMISSION));
    bool isEmissionTwoSided = DrawUniform(random) < 0.5;
    float2 diffuseSample = float2(DrawUniform(random), DrawUniform(random));
    float2 specularSample = float2(DrawUniform(random), DrawUniform(random));

    Results[base + LUDUS_POLYGONAL_LIGHT_ESTIMATORS_NORMAL_RECORD] = PackFloats(normalWorld, material.Metallic);
    Results[base + LUDUS_POLYGONAL_LIGHT_ESTIMATORS_VIEW_RECORD] = PackFloats(viewWorld, material.Roughness);
    Results[base + LUDUS_POLYGONAL_LIGHT_ESTIMATORS_MATERIAL_RECORD] = PackFloats(material.BaseColor, visibility);
    Results[base + LUDUS_POLYGONAL_LIGHT_ESTIMATORS_VERTEX0_RECORD] =
        PackFloats(positionsWorld[0], isEmissionTwoSided == true ? 1 : 0);
    Results[base + LUDUS_POLYGONAL_LIGHT_ESTIMATORS_VERTEX1_RECORD] = PackFloats(positionsWorld[1], diffuseSample.x);
    Results[base + LUDUS_POLYGONAL_LIGHT_ESTIMATORS_VERTEX2_RECORD] = PackFloats(positionsWorld[2], diffuseSample.y);
    Results[base + LUDUS_POLYGONAL_LIGHT_ESTIMATORS_EMISSION_RECORD] = PackFloats(emission, specularSample.x);

    // ShadePolygonalLights for one triangle, with EvaluateEmission's constant emission and no occluder.
    PolygonalLightReceiver receiver = MakePolygonalLightReceiver(float3(0),
                                                                 normalWorld,
                                                                 viewWorld,
                                                                 material.BaseColor,
                                                                 material.Metallic,
                                                                 material.Roughness);
    float3 geometricNormalWorld = cross(positionsWorld[1] - positionsWorld[0], positionsWorld[2] - positionsWorld[0]);
    PolygonalLight light;
    bool isPrepared = TryPreparePolygonalLight(receiver,
                                               positionsWorld[0],
                                               positionsWorld[1],
                                               positionsWorld[2],
                                               geometricNormalWorld,
                                               emission,
                                               isEmissionTwoSided,
                                               light);
    float3 radiance[POLYGONAL_LIGHT_ESTIMATOR_COUNT];
    [unroll] for (uint estimator = 0; estimator < POLYGONAL_LIGHT_ESTIMATOR_COUNT; ++estimator)
    {
        radiance[estimator] = float3(0);
    }
    if (isPrepared == true && any(light.DiffuseCoefficient > 0) == true)
    {
        for (uint technique = 0; technique < POLYGONAL_LIGHT_TECHNIQUE_COUNT; ++technique)
        {
            PolygonalLightSample lightSample;
            if (TrySamplePolygonalLight(receiver,
                                        light,
                                        technique,
                                        technique == PROJECTED_SOLID_ANGLE_TECHNIQUE ? diffuseSample : specularSample,
                                        lightSample) == false)
                continue;
            for (uint estimator = 0; estimator < POLYGONAL_LIGHT_ESTIMATOR_COUNT; ++estimator)
                radiance[estimator] += EstimatePolygonalLightSample(estimator,
                                                                    technique,
                                                                    material,
                                                                    viewWorld,
                                                                    light,
                                                                    lightSample,
                                                                    visibility) *
                                       emission;
        }
    }
    Results[base + LUDUS_POLYGONAL_LIGHT_ESTIMATORS_INTEGRAL_RECORD] = uint4(asuint(specularSample.y),
                                                                             asuint(light.DiffuseIntegral),
                                                                             asuint(light.SpecularIntegral),
                                                                             isPrepared == true ? 1 : 0);
    [unroll] for (uint estimator = 0; estimator < POLYGONAL_LIGHT_ESTIMATOR_COUNT; ++estimator)
    {
        Results[base + LUDUS_POLYGONAL_LIGHT_ESTIMATORS_FIRST_RADIANCE_RECORD + estimator] =
            PackFloats(radiance[estimator], 0);
    }
    Results[base + LUDUS_POLYGONAL_LIGHT_ESTIMATORS_DIFFUSE_COEFFICIENT_RECORD] =
        PackFloats(light.DiffuseCoefficient, receiver.HasSpecular == true ? 1 : 0);
    Results[base + LUDUS_POLYGONAL_LIGHT_ESTIMATORS_SPECULAR_COEFFICIENT_RECORD] =
        PackFloats(light.SpecularCoefficient, 0);
}
