// Single-pass reference for the compute-ray-tracing template's direct-lighting policy
// (render_graph_ray_tests.cpp). One RayTracingPipeline pass draws everything from the automatic
// stream in the order the template's explicit ranges use: jitter x, y (dimensions 0, 1), then light
// selection, the three light-shape and the three BSDF coordinates (2 to 8) in the closest-hit callback.
float3 GenerateCameraRay(SceneContext scene, RayLaunch launch, inout RayQueryContext queries)
{
    float2 jitterPixel;
    jitterPixel.x = SampleSceneRandom(queries);
    jitterPixel.y = SampleSceneRandom(queries);
    ScenePayload payload = (ScenePayload)0;
    TraceAndShade(scene, queries, MakeCameraRay(launch, jitterPixel), payload);
    return payload.Radiance;
}
