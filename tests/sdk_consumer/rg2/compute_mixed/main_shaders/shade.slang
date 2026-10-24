#include "hit_record.slang"

// World-space and barycentric agreement allowed between pass 0's hit and this pass's re-trace of the same ray.
static const float RETRACE_TOLERANCE_WORLD = 1e-5;
static const float RETRACE_TOLERANCE_BARYCENTRIC = 1e-5;

// Mixed sequence probe, not a shading model. Pass 0 is a scene compute kernel; this is a ray pass.
// Red: pass 0's packed triangle index names the same triangle in this pass's separately built
// scene and BLAS/TLAS, and TraceAndShade dispatches this pass's own hit or miss callback.
// Green: hit triangle (+1) over the triangle count, zero on a miss. Blue: this pass's first
// automatic draw repeats pass 0's, while an explicit draw past the primary dimensions does not.
float3 ShadeRecordedHit(SceneContext scene, RayLaunch launch, inout RayQueryContext queries)
{
    uint base = GetHitRecordBase(launch);
    uint4 surfaceWord = GraphBuffer0[base];
    uint4 sampleWord = GraphBuffer0[base + 1];
    bool isCurrent = sampleWord.z == launch.SampleIndex;
    SurfaceHit traced;
    uint status = TraceClosest(scene, queries, MakeCameraRay(launch, asfloat(sampleWord.xy)), traced);
    bool isHit = surfaceWord.x != 0;
    bool isSameIdentity = status == SCENE_QUERY_MISS;
    if (isHit == true)
    {
        uint triangleIndex = surfaceWord.x - 1;
        float2 barycentric = asfloat(surfaceWord.zw);
        SurfaceHit recorded = GetSurfaceHit(scene, triangleIndex, barycentric);
        isSameIdentity = status == SCENE_QUERY_SURFACE && recorded.IsValid == true &&
                         traced.TriangleIndex == triangleIndex && traced.Primitive == recorded.Primitive &&
                         traced.InstanceIndex == recorded.InstanceIndex &&
                         all(abs(traced.Barycentric - barycentric) <= RETRACE_TOLERANCE_BARYCENTRIC) &&
                         abs(traced.DistanceWorld - asfloat(surfaceWord.y)) <= RETRACE_TOLERANCE_WORLD &&
                         all(abs(traced.PositionWorld - recorded.PositionWorld) <= RETRACE_TOLERANCE_WORLD);
    }
    ScenePayload payload = (ScenePayload)0;
    uint shadedStatus = TraceAndShade(scene, queries, MakeCameraRay(launch, asfloat(sampleWord.xy)), payload);
    isSameIdentity = isSameIdentity == true && shadedStatus == status &&
                     payload.Radiance.x == (status == SCENE_QUERY_SURFACE ? 1 : 0);
    bool isCorrelated = asuint(SampleSceneRandom(queries)) == sampleWord.w;
    bool isOffsetIndependent = SampleSceneRandom(launch, PRIMARY_RANDOM_DIMENSION_COUNT) != asfloat(sampleWord.w);
    float triangleShade = isHit == true ? float(surfaceWord.x) / float(GetSceneInfo(scene).TriangleCount) : 0;
    return float3(isCurrent == true && isSameIdentity == true ? 1 : 0,
                  triangleShade,
                  isCorrelated == true && isOffsetIndependent == true ? 1 : 0);
}
