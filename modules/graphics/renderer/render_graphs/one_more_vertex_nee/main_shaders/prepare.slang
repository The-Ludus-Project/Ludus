#include "statistics.slang"

// One invocation builds the cached bottom-up statistics. The ordered graph edge publishes the
// complete buffer to shading; the header is written last. No cross-workgroup synchronization.
// Subsequent samples reuse the cache. Scene generation changes invalidate it, independently of camera/seed.
float3 PrepareGeometrySampling(SceneContext scene, RayLaunch launch, inout RayQueryContext queries)
{
    if (any(launch.PositionPixel != 0) == true)
        return float3(0);
    uint wordCount;
    uint stride;
    GraphBuffer0.GetDimensions(wordCount, stride);
    uint count = GetGeometryHierarchyNodeCount(scene);
    if (wordCount < OMV_HEADER_WORDS || count > (wordCount - OMV_HEADER_WORDS) / OMV_STATISTIC_WORDS ||
        GetSceneInfo(scene).TriangleCount > wordCount - OMV_HEADER_WORDS - count * OMV_STATISTIC_WORDS)
    {
        FailSceneQuery(queries, LUDUS_SCENE_FAILURE_GEOMETRY_STATISTICS_CAPACITY);
        return float3(0);
    }
    if (HasGeometryStatistics(scene) == true)
        return float3(0);
    GraphBuffer0[0] = uint4(0);
    GeometryHierarchyNode root = GetGeometryHierarchyNode(scene, 0);
    float3 originWorld = (root.MinimumWorld + root.MaximumWorld) * 0.5;
    [loop] for (uint remaining = count; remaining > 0; --remaining)
    {
        uint index = remaining - 1;
        GeometryHierarchyNode node = GetGeometryHierarchyNode(scene, index);
        GeometryStatistics statistics = (GeometryStatistics)0;
        if (node.TriangleCount > 0)
        {
            [loop] for (uint primitive = 0; primitive < node.TriangleCount; ++primitive)
            {
                uint triangleIndex = GetGeometryHierarchyTriangle(scene, node.FirstTriangle + primitive);
                statistics =
                    AddGeometryStatistics(statistics, GetTriangleStatistics(scene, triangleIndex, originWorld));
                uint mapBase = OMV_HEADER_WORDS + count * OMV_STATISTIC_WORDS;
                GraphBuffer0[mapBase + triangleIndex] = uint4(index, 0, 0, 0);
            }
        }
        else
        {
            if (node.Left <= index || node.Right <= index || node.Left >= count || node.Right >= count)
            {
                FailSceneQuery(queries, LUDUS_SCENE_FAILURE_SCENE_DATA);
                return float3(0);
            }
            statistics = AddGeometryStatistics(LoadGeometryStatistics(node.Left), LoadGeometryStatistics(node.Right));
        }
        if (statistics.AreaWorld <= 0 || isfinite(statistics.AreaWorld) == false)
        {
            FailSceneQuery(queries, LUDUS_SCENE_FAILURE_SCENE_DATA);
            return float3(0);
        }
        float3 meanWorld = statistics.PositionSumWorld / statistics.AreaWorld;
        float3 diagonalWorld = statistics.DiagonalSumWorld / statistics.AreaWorld - meanWorld * meanWorld;
        float3 crossWorld = statistics.CrossSumWorld / statistics.AreaWorld - GetMomentCross(meanWorld);
        // Raw-moment subtraction can lose the thin dimension of distant, nearly planar nodes.
        // A diagonal Gaussian is a safe proxy if its correlated covariance cannot be factored.
        if (TryComputeGaussianAxes(diagonalWorld, crossWorld, statistics.AxesWorld) == false)
        {
            statistics.AxesWorld.Axis0 = float3(sqrt(max(diagonalWorld.x, 0)), 0, 0);
            statistics.AxesWorld.Axis1 = float3(0, sqrt(max(diagonalWorld.y, 0)), 0);
            statistics.AxesWorld.Axis2 = float3(0, 0, sqrt(max(diagonalWorld.z, 0)));
        }
        StoreGeometryStatistics(index, statistics);
    }
    GraphBuffer0[0] = uint4(scene.Generation, count, OMV_STATISTIC_VERSION);
    return float3(0);
}
