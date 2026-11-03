// Thanks to Bing Xu et al., "A Generalizable Light Transport 3D Embedding for
// Global Illumination", SIGGRAPH 2026, sections 3.2-3.4, https://arxiv.org/abs/2510.18189.
// The released diffuse model predicts irradiance. Reconstruct Lambertian outgoing
// radiance once, then add authored direct emission, as described in figure 4.
float3 DecodeIrradiance(SceneContext scene, RayLaunch launch, inout RayQueryContext queries)
{
    Ray ray = MakeCameraRay(launch, float2(0.5));
    SurfaceHit hit;
    SceneQuery status = TraceClosest(scene, queries, ray, hit);
    if (status != SceneQuery::Surface)
        return float3(0);
    MaterialSample material = EvaluateMaterial(scene, hit);
    if (material.IsValid == false || material.Model != MaterialModel::Diffuse)
    {
        FailSceneQuery(queries, LUDUS_SCENE_FAILURE_PRIMARY_MATERIAL);
        return float3(0);
    }
    float4 worldToModel = asfloat(GraphBuffer0[1]);
    float3 positionModel = (hit.PositionWorld - worldToModel.xyz) * worldToModel.w;
    float3 prediction;
    if (TryEvaluateLightQuery(positionModel, material.ShadingNormalWorld, material.BaseColor, prediction) == false)
    {
        FailSceneQuery(queries, LUDUS_SCENE_FAILURE_STORED_RECORD);
        return float3(0);
    }
    return EvaluateEmission(scene, hit, -ray.DirectionWorld) + prediction * material.BaseColor / PI;
}
