#pragma once
// Shading owns transport. Resolve passes read its two matched comparison estimates, with no repeated rays.
static const uint OMV_COMPARISON_WORDS = LUDUS_GEOMETRY_COMPARISON_WORDS;
static const uint OMV_COMPARISON_VERSION = 1;
static const uint OMV_COMPARISON_IDENTITY_WORD = 2;
static const uint OMV_COMPARISON_STATUS_WORD = 3;

bool HasOmvComparisonCapacity(RayLaunch launch)
{
    uint wordCount;
    uint stride;
    GraphBuffer1.GetDimensions(wordCount, stride);
    uint capacity = wordCount / OMV_COMPARISON_WORDS;
    return launch.ExtentPixel.x > 0 && launch.ExtentPixel.y > 0 &&
           launch.ExtentPixel.x <= capacity / launch.ExtentPixel.y;
}
uint GetOmvComparisonBase(RayLaunch launch)
{
    return (launch.PositionPixel.y * launch.ExtentPixel.x + launch.PositionPixel.x) * OMV_COMPARISON_WORDS;
}
void StoreOmvComparisons(SceneContext scene, RayLaunch launch, float3 geometry, float3 bsdf, RayQueryContext queries)
{
    uint base = GetOmvComparisonBase(launch);
    GraphBuffer1[base] = asuint(float4(geometry, 0));
    GraphBuffer1[base + 1] = asuint(float4(bsdf, 0));
    GraphBuffer1[base + OMV_COMPARISON_IDENTITY_WORD] =
        uint4(scene.Generation, launch.SampleIndex, OMV_COMPARISON_VERSION);
    GraphBuffer1[base + OMV_COMPARISON_STATUS_WORD] =
        uint4(queries.HasFailed == true ? 1 : 0, queries.FailureReason, launch.ExtentPixel);
}
float3 LoadOmvComparison(SceneContext scene, RayLaunch launch, bool isGeometry, inout RayQueryContext queries)
{
    if (HasOmvComparisonCapacity(launch) == false)
    {
        FailSceneQuery(queries, LUDUS_SCENE_FAILURE_STORAGE_CAPACITY);
        return float3(0);
    }
    uint base = GetOmvComparisonBase(launch);
    uint4 identity = GraphBuffer1[base + OMV_COMPARISON_IDENTITY_WORD];
    uint4 status = GraphBuffer1[base + OMV_COMPARISON_STATUS_WORD];
    if (all(identity == uint4(scene.Generation, launch.SampleIndex, OMV_COMPARISON_VERSION)) == false ||
        all(status.zw == launch.ExtentPixel) == false)
    {
        FailSceneQuery(queries, LUDUS_SCENE_FAILURE_STORED_RECORD);
        return float3(0);
    }
    if (status.x != 0)
    {
        FailSceneQuery(queries, status.y);
        return float3(0);
    }
    return asfloat(GraphBuffer1[base + (isGeometry == true ? 0 : 1)].xyz);
}
