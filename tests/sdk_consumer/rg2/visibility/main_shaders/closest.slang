void ShadeClosest(SceneContext scene,
                  SurfaceHit hit,
                  inout RayQueryContext inoutQueries,
                  inout ScenePayload inoutPayload)
{
    MaterialSample material = EvaluateMaterial(scene, hit);
    inoutPayload.Radiance = material.BaseColor;
}
