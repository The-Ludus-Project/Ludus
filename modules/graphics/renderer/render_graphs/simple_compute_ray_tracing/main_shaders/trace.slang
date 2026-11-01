// One pixel, one camera ray, every World triangle, then shade only the closest hit.
// The scene API already flattens primitive instances into world-space triangles.
// This educational kernel deliberately has no acceleration structure or hit buffer.

// Thanks to Tomas Moller and Ben Trumbore, "Fast, Minimum Storage Ray-Triangle
// Intersection", Journal of Graphics Tools 2(1), 1997, pp. 21-28, section 2:
// https://doi.org/10.1080/10867651.1997.10487468 . Adapted here as a two-sided
// barycentric test with a relative parallel-ray tolerance and explicit ray bounds.
static const float PARALLEL_COSINE_TOLERANCE = 0.000001;

bool TryIntersectTriangle(Ray ray, TriangleData triangle, out float outDistanceWorld, out float2 outBarycentric)
{
    outDistanceWorld = 0;
    outBarycentric = float2(0);
    if (triangle.IsValid == false)
        return false;
    float3 edge1World = triangle.Position1World - triangle.Position0World;
    float3 edge2World = triangle.Position2World - triangle.Position0World;
    float3 normalWorld = cross(edge1World, edge2World);
    float3 crossDirectionWorld = cross(ray.DirectionWorld, edge2World);
    float determinant = dot(edge1World, crossDirectionWorld);
    float parallelThreshold = PARALLEL_COSINE_TOLERANCE * length(normalWorld) * length(ray.DirectionWorld);
    if (isfinite(determinant) == false || abs(determinant) <= parallelThreshold)
        return false;
    float inverseDeterminant = 1 / determinant;
    float3 offsetWorld = ray.OriginWorld - triangle.Position0World;
    float u = dot(offsetWorld, crossDirectionWorld) * inverseDeterminant;
    float3 crossOffsetWorld = cross(offsetWorld, edge1World);
    float v = dot(ray.DirectionWorld, crossOffsetWorld) * inverseDeterminant;
    float distanceWorld = dot(edge2World, crossOffsetWorld) * inverseDeterminant;
    if (isfinite(u) == false || isfinite(v) == false || isfinite(distanceWorld) == false || u < 0 || v < 0 ||
        u + v > 1 || distanceWorld < ray.MinDistanceWorld || distanceWorld >= ray.MaxDistanceWorld)
        return false;
    outDistanceWorld = distanceWorld;
    outBarycentric = float2(u, v);
    return true;
}

float3 TracePixel(SceneContext scene, RayLaunch launch, inout RayQueryContext queries)
{
    if (scene.Version != SCENE_INTERFACE_VERSION)
    {
        FailSceneQuery(queries, LUDUS_SCENE_FAILURE_INTERFACE_VERSION);
        return float3(0);
    }
    // Pixel centers keep this minimal template deterministic, without accumulation or random draws.
    Ray ray = MakeCameraRay(launch, float2(0.5));
    uint closestTriangle = SCENE_INVALID_INDEX;
    float2 closestBarycentric = float2(0);
    uint triangleCount = GetSceneInfo(scene).TriangleCount;
    for (uint triangleIndex = 0; triangleIndex < triangleCount; ++triangleIndex)
    {
        TriangleData triangle = GetTriangle(scene, triangleIndex);
        float distanceWorld;
        float2 barycentric;
        if (TryIntersectTriangle(ray, triangle, distanceWorld, barycentric) == true)
        {
            // Shrink the ray interval: a later, farther triangle cannot overwrite this surface.
            ray.MaxDistanceWorld = distanceWorld;
            closestTriangle = triangleIndex;
            closestBarycentric = barycentric;
        }
    }
    if (closestTriangle == SCENE_INVALID_INDEX)
        return EvaluateEnvironment(scene, ray.DirectionWorld);
    SurfaceHit hit = GetSurfaceHit(scene, closestTriangle, closestBarycentric);
    hit.IncomingRay = ray;
    hit.DistanceWorld = ray.MaxDistanceWorld;
    hit.IsFrontFace = dot(hit.GeometricNormalWorld, ray.DirectionWorld) < 0;
    MaterialSample material = EvaluateMaterial(scene, hit);
    if (hit.IsValid == false || material.IsValid == false)
    {
        FailSceneQuery(queries, LUDUS_SCENE_FAILURE_SCENE_DATA);
        return float3(0);
    }
    // Minimal unlit shading, showing authored materials without inventing scene lights.
    // Extend here with a light sample and a second triangle loop for a shadow ray.
    return material.BaseColor + EvaluateEmission(scene, hit, -ray.DirectionWorld);
}
