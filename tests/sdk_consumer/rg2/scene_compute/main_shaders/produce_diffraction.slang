float3 ProduceDiffraction(SceneContext scene, RayLaunch launch, inout RayQueryContext queries)
{
    if ((launch.PositionPixel.x & 1) == 0)
        return float3(1, 0, 0);
    return float3(0, 1, 0);
}

void DiffractionMiss(SceneContext scene, Ray ray, inout RayQueryContext queries, inout ScenePayload inoutPayload) {}

void DiffractionClosest(SceneContext scene,
                        SurfaceHit hit,
                        inout RayQueryContext queries,
                        inout ScenePayload inoutPayload)
{
}
