#include "production_reservoir.slang"

// Section 5.2: generate 128 independent pools of 1024 light points, once per frame. Uniform emitter/triangle
// proposals replace the paper's power proposal; every record retains its complete area PDF. Pixels in an 8x8 tile
// later draw uniformly from one shared pool. No receiver-dependent emitter scan occurs in the pixel loop.
float3 PresampleProductionLights(SceneContext scene, RayLaunch launch, inout RayQueryContext queries)
{
    uint words;
    uint stride;
    GraphBuffer0.GetDimensions(words, stride);
    if (words < PRODUCTION_POOL_RECORD_COUNT)
    {
        FailSceneQuery(queries, LUDUS_SCENE_FAILURE_STORAGE_CAPACITY);
        return float3(0);
    }
    const uint pixelCount = launch.ExtentPixel.x * launch.ExtentPixel.y;
    const uint emitterCount = GetEmitterCount(scene);
    const uint pixel = launch.PositionPixel.y * launch.ExtentPixel.x + launch.PositionPixel.x;
    [loop] for (uint record = pixel; record < PRODUCTION_POOL_RECORD_COUNT; record += pixelCount)
    {
        uint4 encoded = uint4(SCENE_INVALID_INDEX, 0, 0, 0);
        if (emitterCount > 0)
        {
            RandomAddress address = MakeRandomAddress(uint2(record, GetGraphPassIndex()), launch.SampleIndex, 0);
            const uint emitter = SampleProductionIndex(launch, address, emitterCount, queries);
            const uint triangleCount = uint(GetEmitter(scene, emitter).y);
            if (triangleCount == 0)
            {
                FailSceneQuery(queries, LUDUS_SCENE_FAILURE_LIGHT_SAMPLE);
            }
            else
            {
                address.Dimension = 1;
                const uint triangleOffset = SampleProductionIndex(launch, address, triangleCount, queries);
                TriangleData triangle = GetEmitterTriangle(scene, emitter, triangleOffset);
                address.Dimension = 2;
                const float root = sqrt(SampleRandomFloat01(launch.Seed, address));
                const uint BARYCENTRIC_SECOND_DIMENSION = 3;
                address.Dimension = BARYCENTRIC_SECOND_DIMENSION;
                const float v = SampleRandomFloat01(launch.Seed, address);
                const float2 barycentric = float2(root * (1 - v), root * v);
                const float inversePdfArea = float(emitterCount) * float(triangleCount) * triangle.AreaWorld;
                if (triangle.IsValid == false || isfinite(inversePdfArea) == false || inversePdfArea <= 0)
                    FailSceneQuery(queries, LUDUS_SCENE_FAILURE_INVALID_WEIGHT);
                else
                    encoded = uint4(triangle.TriangleIndex, asuint(barycentric), asuint(inversePdfArea));
            }
        }
        if (queries.HasFailed == true)
            encoded = uint4(SCENE_INVALID_INDEX, 0, 0, asuint(-1.0f));
        GraphBuffer0[record] = encoded;
    }
    return float3(0);
}
