#include "math.slang"
#include "raster_bindings.slang"
// Test facade for the scene-independent production math; no renderer storage/traversal.
struct RayLaunch
{
    uint2 PositionPixel;
    uint2 ExtentPixel;
    uint2 Seed;
    uint SampleIndex;
    float4x4 CameraClipToWorld;
    float3 CameraPositionWorld;
    bool IsOrthographic;
};
static const float FG_GATHER_RADIUS_WORLD = 0.25;
#include "core.slang"
#include "projection.slang"
LUDUS_SHADER_BINDING(0) RWStructuredBuffer<uint4> Results;
static const uint CASE_COUNT = 512;
static const uint CASE_WORDS = 3;
static const uint GROUP_WIDTH = 64;
[numthreads(GROUP_WIDTH, 1, 1)] void ComputeMain(uint3 dispatch : SV_DispatchThreadID)
{
    uint index = dispatch.x;
    if (index >= CASE_COUNT) return;
    float x = float(index % 19) * 0.1 - 0.91;
    float jacobian = ComputeFgReconnectJacobian(float3(0, 0, 1), float3(1 + float(index % 5) * 0.1, 0, 2), float3(0, 0, 1));
    float weight = ComputeFgPairWeight(2, 3, 0.5 + float(index % 11) * 0.1, 1);
    Results[index * CASE_WORDS] = asuint(float4(ComputeFgFluxScale(index % 17 + 1, index % 7), jacobian, weight, 0));
    RayLaunch launch = (RayLaunch)0;
    launch.ExtentPixel = uint2(64);
    launch.CameraClipToWorld = float4x4(1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 1, 0, 0, 0, 1);
    launch.IsOrthographic = index % 13 == 0;
    uint pixel;
    float projection;
    bool hasProjection = TryProjectFgPhoton(launch, float3(index % 7 == 0 ? 8 : x, 0.1, 2), float3(0, 0, -1), pixel, projection);
    Results[index * CASE_WORDS + 1] = uint4(pixel, asuint(projection), hasProjection == true ? 1 : 0,
        ComputeFgDirectionCount(1 / float(FG_DIRECTION_CELLS), index % 2 == 0 ? 256 : 16));
    Results[index * CASE_WORDS + 2] = uint4(asuint(EvaluateFgPhotonDensity(float3(1 / PI), float3(2, 3, 4))), 0);
}
