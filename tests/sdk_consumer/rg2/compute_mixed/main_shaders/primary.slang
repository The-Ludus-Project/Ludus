#include "hit_record.slang"

// Scene compute kernel (ComputePipeline): one TraceClosest, no hit or miss callbacks.
float3 TracePrimary(SceneContext scene, RayLaunch launch, inout RayQueryContext queries)
{
    float2 jitterPixel = float2(SampleSceneRandom(launch, 0), SampleSceneRandom(launch, 1));
    // Recorded only so the shade pass can measure cross-pass stream correlation.
    float automaticDraw = SampleSceneRandom(queries);
    SurfaceHit hit;
    uint status = TraceClosest(scene, queries, MakeCameraRay(launch, jitterPixel), hit);
    uint base = GetHitRecordBase(launch);
    GraphBuffer0[base] = uint4(status == SCENE_QUERY_SURFACE ? hit.TriangleIndex + 1 : 0,
                               asuint(hit.DistanceWorld),
                               asuint(hit.Barycentric.x),
                               asuint(hit.Barycentric.y));
    GraphBuffer0[base + 1] =
        uint4(asuint(jitterPixel.x), asuint(jitterPixel.y), launch.SampleIndex, asuint(automaticDraw));
    return float3(0);
}
