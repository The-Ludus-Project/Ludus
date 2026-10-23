void ShadeMiss(SceneContext scene, Ray ray, inout RayQueryContext inoutQueries, inout ScenePayload inoutPayload)
{
    inoutPayload.Radiance = float3(0, 0, 1);
}
