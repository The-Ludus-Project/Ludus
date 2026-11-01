// Emission plus one light sample with a shadow ray, evaluated at the traced hit.
void ShadeDirectLighting(SceneContext scene, SurfaceHit hit, inout RayQueryContext queries, inout ScenePayload payload)
{
    float3 viewDirectionWorld = -hit.IncomingRay.DirectionWorld;
    payload.Radiance = EvaluateEmission(scene, hit, viewDirectionWorld);
    MaterialSample material = EvaluateMaterial(scene, hit);
    if (material.IsValid == false)
    {
        FailSceneQuery(queries, LUDUS_SCENE_FAILURE_SCENE_DATA);
        return;
    }
    SelectedLight selected = SelectLight(scene, queries);
    float3 shapeSample;
    shapeSample.x = SampleSceneRandom(queries);
    shapeSample.y = SampleSceneRandom(queries);
    shapeSample.z = SampleSceneRandom(queries);
    LightSample light = SampleIncidentLight(scene, selected, hit, shapeSample);
    float density = light.SelectionPmf * light.ConditionalPdf;
    if (light.HasSample == false || density <= 0)
        return;
    float3 normalWorld = normalize(material.ShadingNormalWorld);
    float cosine = dot(normalWorld, light.DirectionWorld);
    float3 reflectance;
    float bsdfPdf;
    EvaluateStandardBsdf(material, viewDirectionWorld, light.DirectionWorld, reflectance, bsdfPdf);
    if (cosine <= 0 || any(reflectance * light.IncidentCoefficient > 0) == false)
        return;
    if (TestVisibility(scene, queries, hit, light) == SCENE_QUERY_MISS)
        payload.Radiance += reflectance * light.IncidentCoefficient * cosine / density;
}
