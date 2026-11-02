#include "figure17.slang"

// Thanks to Shlomi Steinberg, "Slice Sampling: Importance sampling with an auxiliary map", draft,
// sections 3-5. Enumerate ALL inverse triangles of the explicitly triangulated surrogate, with
// inverse-determinant branch probabilities and retained null outcomes. This is NOT a finite-start
// Newton density for the nonlinear physical map. Physical radiance is evaluated independently.
LUDUS_SHADER_BINDING(4) StructuredBuffer<float4> GuideVertices;
LUDUS_SHADER_BINDING(6) StructuredBuffer<float4> GuideAcceleration;
LUDUS_SHADER_BINDING(7) StructuredBuffer<uint2> GuideBins;
LUDUS_SHADER_BINDING(8) StructuredBuffer<uint> GuideTriangles;

struct RootList
{
    uint Offset;
    uint Count;
    bool IsExplicit;
};

RootList GetRootList(float2 residual, uint resolution, uint accelerationGuide)
{
    RootList result;
    result.Offset = 0;
    result.Count = 2 * resolution * resolution;
    result.IsExplicit = false;
    if (accelerationGuide == ACCELERATION_DISABLED)
        return result;
    result.IsExplicit = true;
    result.Count = 0;
    uint record = accelerationGuide * ACCELERATION_RECORD_COUNT;
    float4 bounds = GuideAcceleration[record];
    if (any(residual < bounds.xy) || any(residual > bounds.zw))
        return result;
    uint2 positionBin = min(uint2((residual - bounds.xy) / (bounds.zw - bounds.xy) * ACCELERATION_GRID_SIDE),
                            ACCELERATION_GRID_SIDE - 1);
    uint binOffset = uint(GuideAcceleration[record + 1].x);
    uint2 bin = GuideBins[binOffset + positionBin.y * ACCELERATION_GRID_SIDE + positionBin.x];
    result.Offset = bin.x;
    result.Count = bin.y;
    return result;
}

struct AffineRoot
{
    float2 CoordinatesUv;
    float Weight;
    bool HasRoot;
};

float ComputeDeterminant(float2 first, float2 second)
{
    return first.x * second.y - first.y * second.x;
}

void ReadGuideTriangle(uint triangle,
                       uint resolution,
                       uint offset,
                       out float2 outFirst,
                       out float2 outSecond,
                       out float2 outThird,
                       out float2 outOrigin,
                       out float outSign)
{
    uint cell = triangle / 2;
    uint column = cell % resolution;
    uint row = cell / resolution;
    uint vertex = offset + row * (resolution + 1) + column;
    outSign = (triangle % 2) == 0 ? 1 : -1;
    if (outSign > 0)
    {
        outFirst = GuideVertices[vertex].xy;
        outSecond = GuideVertices[vertex + 1].xy;
        outThird = GuideVertices[vertex + resolution + 1].xy;
        outOrigin = float2(column, row) / resolution;
    }
    else
    {
        outFirst = GuideVertices[vertex + resolution + 2].xy;
        outSecond = GuideVertices[vertex + resolution + 1].xy;
        outThird = GuideVertices[vertex + 1].xy;
        outOrigin = float2(column + 1, row + 1) / resolution;
    }
}

AffineRoot FindAffineRoot(float2 residual, uint triangle, uint resolution, uint offset)
{
    float2 first;
    float2 second;
    float2 third;
    float2 origin;
    float sign;
    ReadGuideTriangle(triangle, resolution, offset, first, second, third, origin, sign);
    float2 firstColumn = second - first;
    float2 secondColumn = third - first;
    float determinant = ComputeDeterminant(firstColumn, secondColumn);
    AffineRoot result;
    result.CoordinatesUv = float2(0);
    result.Weight = 0;
    result.HasRoot = false;
    float absoluteJacobian = abs(determinant) * resolution * resolution;
    if (absoluteJacobian < MINIMUM_TRIANGLE_DETERMINANT)
        return result;
    float2 difference = residual - first;
    float2 barycentric =
        float2(ComputeDeterminant(difference, secondColumn), ComputeDeterminant(firstColumn, difference)) / determinant;
    bool isInside = sign > 0 ? (all(barycentric >= 0) && barycentric.x + barycentric.y < 1)
                             : (all(barycentric > 0) && barycentric.x + barycentric.y <= 1);
    if (isInside == false)
        return result;
    result.CoordinatesUv = origin + sign * barycentric / resolution;
    result.Weight = 1 / absoluteJacobian;
    result.HasRoot = true;
    return result;
}

float2 EvaluateAffineMap(float2 coordinates, uint resolution, uint offset, out bool outIsRegular)
{
    float2 positionGrid = coordinates * resolution;
    uint2 cell = min(uint2(floor(positionGrid)), resolution - 1);
    float2 barycentric = positionGrid - cell;
    uint triangle = (cell.y * resolution + cell.x) * 2;
    if (barycentric.x + barycentric.y >= 1)
    {
        triangle += 1;
        barycentric = 1 - barycentric;
    }
    float2 first;
    float2 second;
    float2 third;
    float2 origin;
    float sign;
    ReadGuideTriangle(triangle, resolution, offset, first, second, third, origin, sign);
    outIsRegular = abs(ComputeDeterminant(second - first, third - first)) * resolution * resolution >=
                   MINIMUM_TRIANGLE_DETERMINANT;
    return first + barycentric.x * (second - first) + barycentric.y * (third - first);
}

float SumBranchWeights(float2 residual,
                       uint resolution,
                       uint offset,
                       out uint outRootCount,
                       uint accelerationGuide = ACCELERATION_DISABLED)
{
    float totalWeight = 0;
    outRootCount = 0;
    RootList list = GetRootList(residual, resolution, accelerationGuide);
    for (uint index = 0; index < list.Count; ++index)
    {
        uint triangle = list.IsExplicit == true ? GuideTriangles[list.Offset + index] : index;
        AffineRoot root = FindAffineRoot(residual, triangle, resolution, offset);
        totalWeight += root.Weight;
        outRootCount += root.HasRoot == true ? 1 : 0;
    }
    return totalWeight;
}

float ComputeAffineDensity(float2 residual, float alpha, float totalWeight)
{
    return DEFENSIVE_FRACTION + (1 - DEFENSIVE_FRACTION) * exp(-dot(residual, residual) / (alpha * alpha)) /
                                    (PI * alpha * alpha * totalWeight);
}

float EvaluateAffineDensity(float2 coordinates,
                            float alpha,
                            uint resolution,
                            uint offset,
                            uint accelerationGuide = ACCELERATION_DISABLED)
{
    bool isRegular;
    float2 residual = EvaluateAffineMap(coordinates, resolution, offset, isRegular);
    if (isRegular == false)
        return DEFENSIVE_FRACTION;
    uint rootCount;
    float totalWeight = SumBranchWeights(residual, resolution, offset, rootCount, accelerationGuide);
    return totalWeight > 0 ? ComputeAffineDensity(residual, alpha, totalWeight) : DEFENSIVE_FRACTION;
}

float4 SampleAffineSlice(float2 residual,
                         float branchUniform,
                         float alpha,
                         uint resolution,
                         uint offset,
                         out uint outRootCount,
                         out float outTotalWeight,
                         uint accelerationGuide = ACCELERATION_DISABLED)
{
    float totalWeight = SumBranchWeights(residual, resolution, offset, outRootCount, accelerationGuide);
    outTotalWeight = totalWeight;
    if (totalWeight <= 0)
        return float4(0);
    float target = branchUniform * totalWeight;
    float cumulativeWeight = 0;
    float2 selected = float2(0);
    RootList list = GetRootList(residual, resolution, accelerationGuide);
    for (uint index = 0; index < list.Count; ++index)
    {
        uint triangle = list.IsExplicit == true ? GuideTriangles[list.Offset + index] : index;
        AffineRoot root = FindAffineRoot(residual, triangle, resolution, offset);
        if (root.HasRoot == false)
            continue;
        selected = root.CoordinatesUv;
        cumulativeWeight += root.Weight;
        if (target < cumulativeWeight)
            break;
    }
    return float4(selected, ComputeAffineDensity(residual, alpha, totalWeight), 1);
}

FIGURE17_PARITY_ENTRY
void ValidateAffineSlice(uint3 threadIndex : SV_DispatchThreadID)
{
    uint count;
    uint stride;
    Inputs.GetDimensions(count, stride);
    uint index = threadIndex.x;
    if (index >= count / INPUT_RECORD_COUNT)
        return;
    float4 first = Inputs[index * INPUT_RECORD_COUNT];
    float4 second = Inputs[index * INPUT_RECORD_COUNT + 1];
    uint resolution = uint(second.z);
    uint offset = uint(second.w);
    uint accelerationGuide = uint(Inputs[index * INPUT_RECORD_COUNT + 2].x);
    uint rootCount;
    float totalWeight;
    float4 sample =
        SampleAffineSlice(first.xy, first.z, first.w, resolution, offset, rootCount, totalWeight, accelerationGuide);
    Results[index * OUTPUT_RECORD_COUNT] = sample;
    Results[index * OUTPUT_RECORD_COUNT + 1] =
        float4(float(rootCount),
               totalWeight,
               EvaluateAffineDensity(second.xy, first.w, resolution, offset, accelerationGuide),
               0);
    Results[index * OUTPUT_RECORD_COUNT + 2] = float4(0);
}
