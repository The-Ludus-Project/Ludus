// Write the base colour of the surface the producers left visible at each pixel; blue where none is.
float3 ResolveBaseColor(SceneContext scene, RayLaunch launch, inout RayQueryContext inoutQueries)
{
    SurfaceHit hit;
    const SceneQuery status = LoadVisibilitySurface(scene, launch, inoutQueries, hit);
    if (status == SceneQuery::Miss)
        return float3(0, 0, 1);
    if (status != SceneQuery::Surface)
        return float3(1, 0, 1);
    return EvaluateMaterial(scene, hit).BaseColor;
}
