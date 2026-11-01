// Scene compute kernel with several native ray queries per invocation: a camera path of up to PATH_VERTICES
// surfaces, each with one closest query, one light sample and one visibility query, carrying its throughput
// and radiance across the queries. Native queries must run out of line on Metal (TraceNative in
// renderer_gpu_preview.slang), however many the kernel issues.
static const uint PATH_VERTICES = 3;
static const float TRACE_DISTANCE_WORLD = 100;

/// A cosine-distributed direction about normalWorld from two uniform numbers.
float3 SampleCosineDirectionWorld(float3 normalWorld, float2 sample)
{
    float radius = sqrt(sample.x);
    float angle = 2 * PI * sample.y;
    float3 tangentWorld =
        normalize(abs(normalWorld.x) > 0.5 ? cross(normalWorld, float3(0, 1, 0)) : cross(normalWorld, float3(1, 0, 0)));
    float3 bitangentWorld = cross(normalWorld, tangentWorld);
    return normalize(tangentWorld * (radius * cos(angle)) + bitangentWorld * (radius * sin(angle)) +
                     normalWorld * sqrt(max(1 - sample.x, 0)));
}

float3 TraceMultiQueryPaths(SceneContext scene, RayLaunch launch, inout RayQueryContext queries)
{
    Ray ray = MakeCameraRay(launch, float2(0.5));
    float3 radiance = float3(0);
    float3 throughput = float3(1);
    [loop] for (uint vertex = 0; vertex < PATH_VERTICES; ++vertex)
    {
        SurfaceHit hit;
        if (TraceClosest(scene, queries, ray, hit) != SCENE_QUERY_SURFACE)
            break;
        MaterialSample material = EvaluateMaterial(scene, hit);
        if (material.IsValid == false)
        {
            FailSceneQuery(queries, LUDUS_SCENE_FAILURE_SCENE_DATA);
            break;
        }
        float3 normalWorld = hit.IsFrontFace == true ? hit.GeometricNormalWorld : -hit.GeometricNormalWorld;
        radiance += throughput * EvaluateEmission(scene, hit, -ray.DirectionWorld);
        SelectedLight selected = SelectLight(scene, queries);
        float3 shapeSample = float3(SampleSceneRandom(queries), SampleSceneRandom(queries), SampleSceneRandom(queries));
        LightSample light = SampleIncidentLight(scene, selected, hit, shapeSample);
        float density = light.SelectionPmf * light.ConditionalPdf;
        float cosine = dot(normalWorld, light.DirectionWorld);
        if (light.HasSample == true && density > 0 && cosine > 0 &&
            TestVisibility(scene, queries, hit, light) == SCENE_QUERY_MISS)
            radiance += throughput * material.BaseColor / PI * light.IncidentCoefficient * cosine / density;
        float2 directionSample = float2(SampleSceneRandom(queries), SampleSceneRandom(queries));
        throughput *= material.BaseColor;
        ray = SpawnSurfaceRay(hit, SampleCosineDirectionWorld(normalWorld, directionSample), TRACE_DISTANCE_WORLD);
    }
    return radiance;
}
