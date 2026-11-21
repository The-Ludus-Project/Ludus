float3 ProduceDiffraction(SceneContext scene, RayLaunch launch, inout RayQueryContext queries)
{
    uint width;
    uint height;
    GraphStorageTexture.GetDimensions(width, height);
    for (uint y = launch.PositionPixel.y; y < height; y += launch.ExtentPixel.y)
    {
        for (uint x = launch.PositionPixel.x; x < width; x += launch.ExtentPixel.x)
        {
            const uint2 positionPixel = uint2(x, y);
            const float4 previous = GraphStorageTexture[positionPixel];
            const float4 stripe = (x & 1) == 0 ? float4(1, 0, 0, 1) : float4(0, 1, 0, 1);
            GraphStorageTexture[positionPixel] = saturate(previous + stripe);
        }
    }
    return float3(0);
}

void DiffractionMiss(SceneContext scene, Ray ray, inout RayQueryContext queries, inout ScenePayload inoutPayload) {}

void DiffractionClosest(SceneContext scene,
                        SurfaceHit hit,
                        inout RayQueryContext queries,
                        inout ScenePayload inoutPayload)
{
}
