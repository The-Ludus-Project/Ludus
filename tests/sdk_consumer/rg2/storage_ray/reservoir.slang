#define GenerateRay GenerateSceneRay
#include "generate.slang"
#undef GenerateRay

float3 GenerateRay(SceneContext scene, RayLaunch launch, inout RayQueryContext queries)
{
    float3 radiance = GenerateSceneRay(scene, launch, queries);
    uint pixel = launch.PositionPixel.y * launch.ExtentPixel.x + launch.PositionPixel.x;
    Reservoir previous;
    if ((launch.SampleIndex & 1) == 0)
        previous = ReservoirA[pixel];
    else
        previous = ReservoirB[pixel];
    Reservoir current = previous;
    current.Sample.x += 1;
    current.Weights = float4(launch.PositionPixel, launch.ExtentPixel);
    if ((launch.SampleIndex & 1) == 0)
        ReservoirB[pixel] = current;
    else
        ReservoirA[pixel] = current;
    return float3(current.Sample.x, radiance.yz);
}
