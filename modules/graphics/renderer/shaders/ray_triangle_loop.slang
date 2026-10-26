// Thanks to Tomas Moller and Ben Trumbore, "Fast, Minimum Storage Ray-Triangle
// Intersection", Journal of Graphics Tools 2(1), 1997, pp. 21-28, section 2:
// https://doi.org/10.1080/10867651.1997.10487468 . Adapted from the simple
// compute template's two-sided test to the renderer's packed scene and alpha
// acceptance contract. Equal-distance hits retain the first packed triangle.
static const float TRIANGLE_PARALLEL_COSINE_TOLERANCE = 0.000001;

/// Trace packed World triangles in the ray interval [nearDistanceWorld, farDistanceWorld).
/// Return the nearest alpha-accepted hit, or any accepted hit for visibility.
/// A miss retains farDistanceWorld and the invalid-triangle sentinel.
Hit TraceTriangleLoop(float3 originWorld,
                      float3 directionWorld,
                      float nearDistanceWorld,
                      float farDistanceWorld,
                      bool isVisibility)
{
    Hit hit;
    hit.Triangle = LUDUS_GPU_PREVIEW_INVALID_TRIANGLE;
    hit.DistanceWorld = farDistanceWorld;
    hit.Barycentric = 0;
    for (uint triangle = 0; triangle < Counts.x; ++triangle)
    {
        uint firstWord = triangle * LUDUS_GPU_PREVIEW_TRIANGLE_WORDS;
        float3 positionWorld = SceneValue(firstWord).xyz;
        float3 edge1World = SceneValue(firstWord + 1).xyz - positionWorld;
        float3 edge2World = SceneValue(firstWord + 2).xyz - positionWorld;
        float3 crossDirectionWorld = cross(directionWorld, edge2World);
        float determinant = dot(edge1World, crossDirectionWorld);
        float threshold =
            TRIANGLE_PARALLEL_COSINE_TOLERANCE * length(cross(edge1World, edge2World)) * length(directionWorld);
        if (isfinite(determinant) == false || abs(determinant) <= threshold)
            continue;
        float inverseDeterminant = 1 / determinant;
        float3 offsetWorld = originWorld - positionWorld;
        float u = dot(offsetWorld, crossDirectionWorld) * inverseDeterminant;
        float3 crossOffsetWorld = cross(offsetWorld, edge1World);
        float v = dot(directionWorld, crossOffsetWorld) * inverseDeterminant;
        float distanceWorld = dot(edge2World, crossOffsetWorld) * inverseDeterminant;
        if (isfinite(u) == false || isfinite(v) == false || isfinite(distanceWorld) == false || u < 0 || v < 0 ||
            u + v > 1 || distanceWorld < nearDistanceWorld || distanceWorld >= hit.DistanceWorld)
            continue;
        float2 barycentric = float2(u, v);
        if ((Sampling.z & LUDUS_GPU_PREVIEW_OPAQUE_FLAG) == 0 &&
            asuint(Surface(triangle, LUDUS_GPU_PREVIEW_TEXTURE_OPTIONS_WORD).z) != 0 &&
            Texture(triangle, TexcoordUv(triangle, barycentric)).w * Surface(triangle, 2).w *
                    InterpolateTriangle(triangle, LUDUS_GPU_PREVIEW_VERTEX_COLOR_WORD, barycentric).w <
                Surface(triangle, 1).y)
            continue;
        hit.Triangle = triangle;
        hit.DistanceWorld = distanceWorld;
        hit.Barycentric = barycentric;
        if (isVisibility == true)
            return hit;
    }
    return hit;
}
