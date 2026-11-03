#pragma once

// Thanks to Jorge Garcia-Pueyo, Nestor Monzon, Adrian Jarabo and Adolfo Muñoz,
// "One-more-vertex Next-Event Estimation with Hierarchical Geometry Sampling", CGF (EGSR 2026),
// https://doi.org/10.1111/cgf.70539, sections 4-5, Table 1. Independent implementation of additive
// geometry statistics and the area-measure hierarchy. Seven Gaussian points use Cholesky axes
// rather than eigenvectors; the diffuse normal integral uses deterministic vMF quadrature instead
// of the authors' polynomial fit. Transport uses real triangles; PDF evaluation replays the same proxy-driven
// branch probabilities used by sampling.
static const uint OMV_STATISTIC_WORDS = LUDUS_GEOMETRY_SAMPLING_STATISTIC_WORDS;
static const uint OMV_HEADER_WORDS = LUDUS_GEOMETRY_SAMPLING_HEADER_WORDS;
static const uint OMV_STATISTIC_VERSION = 1;
static const uint OMV_TRIANGLE_CORNER_COUNT = 3;
static const uint OMV_NORMAL_WORD = 3;
static const uint OMV_COLOR_WORD = OMV_NORMAL_WORD + 1;
static const uint OMV_AXIS0_WORD = OMV_COLOR_WORD + 1;
static const uint OMV_AXIS1_WORD = OMV_AXIS0_WORD + 1;
static const uint OMV_AXIS2_WORD = OMV_AXIS1_WORD + 1;
static const float OMV_TRIANGLE_MOMENT_DIVISOR = 12;
static const float OMV_DIELECTRIC_REFLECTANCE = 0.04;
static const float3 OMV_LUMINANCE_WEIGHTS = float3(0.2126, 0.7152, 0.0722);

struct GeometryStatistics
{
    float AreaWorld;
    float3 PositionSumWorld;
    float3 DiagonalSumWorld;
    float3 CrossSumWorld;
    float3 NormalSumWorld;
    float3 ColorSum;
    float MetallicSum;
    float AlphaSquaredSum;
    float SpecularitySum;
    GaussianAxes AxesWorld;
};

GeometryStatistics LoadGeometryStatistics(uint nodeIndex)
{
    uint base = OMV_HEADER_WORDS + nodeIndex * OMV_STATISTIC_WORDS;
    float4 position = asfloat(GraphBuffer0[base]);
    float4 diagonal = asfloat(GraphBuffer0[base + 1]);
    float4 cross = asfloat(GraphBuffer0[base + 2]);
    float4 normal = asfloat(GraphBuffer0[base + OMV_NORMAL_WORD]);
    GeometryStatistics result;
    result.PositionSumWorld = position.xyz;
    result.AreaWorld = position.w;
    result.DiagonalSumWorld = diagonal.xyz;
    result.MetallicSum = diagonal.w;
    result.CrossSumWorld = cross.xyz;
    result.AlphaSquaredSum = cross.w;
    result.NormalSumWorld = normal.xyz;
    result.SpecularitySum = normal.w;
    result.ColorSum = asfloat(GraphBuffer0[base + OMV_COLOR_WORD].xyz);
    result.AxesWorld.Axis0 = asfloat(GraphBuffer0[base + OMV_AXIS0_WORD].xyz);
    result.AxesWorld.Axis1 = asfloat(GraphBuffer0[base + OMV_AXIS1_WORD].xyz);
    result.AxesWorld.Axis2 = asfloat(GraphBuffer0[base + OMV_AXIS2_WORD].xyz);
    return result;
}
void StoreGeometryStatistics(uint nodeIndex, GeometryStatistics statistics)
{
    uint base = OMV_HEADER_WORDS + nodeIndex * OMV_STATISTIC_WORDS;
    GraphBuffer0[base] = asuint(float4(statistics.PositionSumWorld, statistics.AreaWorld));
    GraphBuffer0[base + 1] = asuint(float4(statistics.DiagonalSumWorld, statistics.MetallicSum));
    GraphBuffer0[base + 2] = asuint(float4(statistics.CrossSumWorld, statistics.AlphaSquaredSum));
    GraphBuffer0[base + OMV_NORMAL_WORD] = asuint(float4(statistics.NormalSumWorld, statistics.SpecularitySum));
    GraphBuffer0[base + OMV_COLOR_WORD] = asuint(float4(statistics.ColorSum, 0));
    GraphBuffer0[base + OMV_AXIS0_WORD] = asuint(float4(statistics.AxesWorld.Axis0, 0));
    GraphBuffer0[base + OMV_AXIS1_WORD] = asuint(float4(statistics.AxesWorld.Axis1, 0));
    GraphBuffer0[base + OMV_AXIS2_WORD] = asuint(float4(statistics.AxesWorld.Axis2, 0));
}
GeometryStatistics AddGeometryStatistics(GeometryStatistics left, GeometryStatistics right)
{
    left.AreaWorld += right.AreaWorld;
    left.PositionSumWorld += right.PositionSumWorld;
    left.DiagonalSumWorld += right.DiagonalSumWorld;
    left.CrossSumWorld += right.CrossSumWorld;
    left.NormalSumWorld += right.NormalSumWorld;
    left.ColorSum += right.ColorSum;
    left.MetallicSum += right.MetallicSum;
    left.AlphaSquaredSum += right.AlphaSquaredSum;
    left.SpecularitySum += right.SpecularitySum;
    return left;
}
float3 GetMomentCross(float3 value)
{
    return float3(value.x * value.y, value.x * value.z, value.y * value.z);
}
GeometryStatistics GetTriangleStatistics(SceneContext scene, uint triangleIndex, float3 originWorld)
{
    TriangleData triangle = GetTriangle(scene, triangleIndex);
    SurfaceHit hit = GetSurfaceHit(scene, triangleIndex, float2(1, 1) / OMV_TRIANGLE_CORNER_COUNT);
    MaterialSample material = EvaluateMaterial(scene, hit);
    GeometryStatistics result = (GeometryStatistics)0;
    if (triangle.IsValid == false || material.IsValid == false || triangle.AreaWorld <= 0)
        return result;
    // Coordinates relative to the scene bounds' centre keep raw second moments well conditioned.
    float3 aWorld = triangle.Position0World - originWorld;
    float3 bWorld = triangle.Position1World - originWorld;
    float3 cWorld = triangle.Position2World - originWorld;
    float3 sumWorld = aWorld + bWorld + cWorld;
    result.AreaWorld = triangle.AreaWorld;
    result.PositionSumWorld = triangle.AreaWorld * sumWorld / OMV_TRIANGLE_CORNER_COUNT;
    result.DiagonalSumWorld = triangle.AreaWorld *
                              (aWorld * aWorld + bWorld * bWorld + cWorld * cWorld + sumWorld * sumWorld) /
                              OMV_TRIANGLE_MOMENT_DIVISOR;
    result.CrossSumWorld =
        triangle.AreaWorld *
        (GetMomentCross(aWorld) + GetMomentCross(bWorld) + GetMomentCross(cWorld) + GetMomentCross(sumWorld)) /
        OMV_TRIANGLE_MOMENT_DIVISOR;
    result.NormalSumWorld = triangle.AreaWorld * triangle.GeometricNormalWorld;
    result.ColorSum = triangle.AreaWorld * material.BaseColor;
    result.MetallicSum = triangle.AreaWorld * (material.Model == MaterialModel::Conductor ? 1 : material.Metallic);
    float alpha = ComputeMaterialAlpha(material.Model, material.Roughness, material.AlphaU);
    result.AlphaSquaredSum = triangle.AreaWorld * alpha * alpha;
    result.SpecularitySum =
        triangle.AreaWorld * (material.Model == MaterialModel::Diffuse ? 0 : OMV_DIELECTRIC_REFLECTANCE);
    return result;
}

bool HasGeometryStatistics(SceneContext scene)
{
    uint wordCount;
    uint stride;
    GraphBuffer0.GetDimensions(wordCount, stride);
    uint count = GetGeometryHierarchyNodeCount(scene);
    if (wordCount < OMV_HEADER_WORDS || count > (wordCount - OMV_HEADER_WORDS) / OMV_STATISTIC_WORDS ||
        GetSceneInfo(scene).TriangleCount > wordCount - OMV_HEADER_WORDS - count * OMV_STATISTIC_WORDS)
        return false;
    return all(GraphBuffer0[0] == uint4(scene.Generation, count, OMV_STATISTIC_VERSION)) == true;
}
