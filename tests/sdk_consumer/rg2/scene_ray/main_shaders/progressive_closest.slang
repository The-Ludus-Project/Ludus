// Test-only stochastic output: repeated renders of one scene must change the accumulated image.
void ShadeClosest(SceneContext scene, SurfaceHit hit, inout RayQueryContext queries, inout ScenePayload payload)
{
    MaterialSample material = EvaluateMaterial(scene, hit);
    float sample = SampleSceneRandom(queries);
    payload.Radiance = material.BaseColor * sample + EvaluateEmission(scene, hit, -hit.IncomingRay.DirectionWorld);
}
