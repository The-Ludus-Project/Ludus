#pragma once
#include "statistics.slang"

static const uint OMV_NORMAL_QUADRATURE_COUNT = 12;
static const float OMV_QUADRATURE_AZIMUTH_STEP = 2.399963229728653;
static const float OMV_MAXIMUM_CONCENTRATION = 10000;
static const float OMV_DIRECTION_TOLERANCE = 0.000001;
static const float OMV_GAUSSIAN_OFFSET_WEIGHT = 0.606530659712633;
static const float OMV_MINIMUM_BRANCH_PROBABILITY = 0.000001;
static const float OMV_MAXIMUM_IMPORTANCE = 1e30;
static const uint OMV_SPATIAL_AXIS_COUNT = 3;
static const float OMV_NORMAL_SPHERE_SOLID_ANGLE = 4 * PI;
static const float OMV_SPECULAR_GEOMETRY_FACTOR = 0.25;
static const float OMV_BOUND_RADIUS_SQUARED_FACTOR = 0.25;
static const uint OMV_SCHLICK_EXPONENT = 5;
static const float OMV_MAXIMUM_UNIT_SAMPLE = 0.9999999403953552;

struct GeometrySamplingContext
{
    SurfaceHit Receiver;
    MaterialSample Material;
    float3 ViewDirectionWorld;
    SurfaceHit Emitter;
};
struct GeometrySample
{
    SurfaceHit Hit;
    float PdfArea;
    bool HasSample;
};
float ComputeNormalConcentration(float meanLength)
{
    float r = min(meanLength, 1 - OMV_DIRECTION_TOLERANCE);
    // Rational inverse of A3(kappa); the cap also represents the exactly aligned limit.
    return min(OMV_MAXIMUM_CONCENTRATION,
               r * (OMV_SPATIAL_AXIS_COUNT - r * r) / max(1 - r * r, OMV_DIRECTION_TOLERANCE));
}
float EvaluateVmfDensity(float cosine, float concentration)
{
    if (concentration < OMV_DIRECTION_TOLERANCE)
        return 1 / OMV_NORMAL_SPHERE_SOLID_ANGLE;
    return concentration * exp(concentration * (cosine - 1)) / (2 * PI * (1 - exp(-2 * concentration)));
}
float EvaluateDiffuseNormalIntegral(float3 normalAxisWorld,
                                    float concentration,
                                    float3 directionSensorWorld,
                                    float3 directionEmitterWorld)
{
    float3 tangentWorld =
        NormalizeOrZero(cross(abs(normalAxisWorld.z) < 0.5 ? float3(0, 0, 1) : float3(0, 1, 0), normalAxisWorld));
    float3 bitangentWorld = cross(normalAxisWorld, tangentWorld);
    float result = 0;
    [loop] for (uint index = 0; index < OMV_NORMAL_QUADRATURE_COUNT; ++index)
    {
        float u = (float(index) + 0.5) / OMV_NORMAL_QUADRATURE_COUNT;
        float z = concentration < OMV_DIRECTION_TOLERANCE
                      ? 2 * u - 1
                      : 1 + log(u + (1 - u) * exp(-2 * concentration)) / concentration;
        float radius = sqrt(max(0, 1 - z * z));
        float angle = float(index) * OMV_QUADRATURE_AZIMUTH_STEP;
        float3 normalWorld = z * normalAxisWorld + radius * (cos(angle) * tangentWorld + sin(angle) * bitangentWorld);
        result += max(0, dot(normalWorld, directionSensorWorld)) * max(0, dot(normalWorld, directionEmitterWorld));
    }
    return result / OMV_NORMAL_QUADRATURE_COUNT;
}
float EvaluateProxyMasking(float cosine, float alphaSquared)
{
    cosine = abs(cosine);
    return 2 * cosine /
           max(cosine + sqrt(alphaSquared + (1 - alphaSquared) * cosine * cosine), OMV_DIRECTION_TOLERANCE);
}
float3
EvaluateDirectionalProxy(GeometryStatistics statistics, float3 directionSensorWorld, float3 directionEmitterWorld)
{
    float inverseArea = 1 / statistics.AreaWorld;
    float3 color = statistics.ColorSum * inverseArea;
    float metallic = statistics.MetallicSum * inverseArea;
    float normalLength = length(statistics.NormalSumWorld);
    float3 normalAxisWorld = normalLength > 0 ? statistics.NormalSumWorld / normalLength : float3(0, 0, 1);
    float concentration = ComputeNormalConcentration(normalLength * inverseArea);
    float diffuse =
        EvaluateDiffuseNormalIntegral(normalAxisWorld, concentration, directionSensorWorld, directionEmitterWorld);
    float3 result = (1 - metallic) * color * (diffuse / PI);
    float3 halfWorld = NormalizeOrZero(directionSensorWorld + directionEmitterWorld);
    float alphaSquared = max(statistics.AlphaSquaredSum * inverseArea, OMV_DIRECTION_TOLERANCE);
    float materialConcentration = 2 / alphaSquared;
    float convolvedConcentration = concentration * materialConcentration / (concentration + materialConcentration);
    float fresnel = pow(1 - max(0, dot(halfWorld, directionSensorWorld)), OMV_SCHLICK_EXPONENT);
    float3 reflectance = metallic * color + (1 - metallic) * statistics.SpecularitySum * inverseArea;
    float3 specularAlbedo = ((1 - fresnel) * reflectance + fresnel) * OMV_SPECULAR_GEOMETRY_FACTOR;
    // A purely diffuse node has no specular component, even at grazing angles.
    if (statistics.SpecularitySum > 0 || metallic > 0)
        result += specularAlbedo * EvaluateProxyMasking(dot(normalAxisWorld, directionSensorWorld), alphaSquared) *
                  EvaluateProxyMasking(dot(normalAxisWorld, directionEmitterWorld), alphaSquared) *
                  EvaluateVmfDensity(dot(normalAxisWorld, halfWorld), convolvedConcentration);
    return result;
}
float3 EvaluateSpatialProxy(GeometryStatistics statistics, float3 positionWorld, GeometrySamplingContext context)
{
    float3 offsetSensorWorld = context.Receiver.PositionWorld - positionWorld;
    float3 offsetEmitterWorld = context.Emitter.PositionWorld - positionWorld;
    float sensorDistanceSquaredWorld = dot(offsetSensorWorld, offsetSensorWorld);
    float emitterDistanceSquaredWorld = dot(offsetEmitterWorld, offsetEmitterWorld);
    if (sensorDistanceSquaredWorld <= 0 || emitterDistanceSquaredWorld <= 0)
        return float3(0);
    float3 directionSensorWorld = offsetSensorWorld * rsqrt(sensorDistanceSquaredWorld);
    float3 directionEmitterWorld = offsetEmitterWorld * rsqrt(emitterDistanceSquaredWorld);
    float3 sensorValue;
    float sensorPdf;
    EvaluateStandardBsdf(context.Material, context.ViewDirectionWorld, -directionSensorWorld, sensorValue, sensorPdf);
    float sensorCosine = max(0, dot(context.Material.ShadingNormalWorld, -directionSensorWorld));
    float emitterCosine = abs(dot(context.Emitter.GeometricNormalWorld, -directionEmitterWorld));
    return sensorValue * sensorCosine * emitterCosine *
           EvaluateDirectionalProxy(statistics, directionSensorWorld, directionEmitterWorld) /
           sensorDistanceSquaredWorld / emitterDistanceSquaredWorld;
}
float EvaluateNodeImportance(SceneContext scene, uint nodeIndex, GeometrySamplingContext context)
{
    GeometryStatistics statistics = LoadGeometryStatistics(nodeIndex);
    GeometryHierarchyNode root = GetGeometryHierarchyNode(scene, 0);
    float3 originWorld = (root.MinimumWorld + root.MaximumWorld) * 0.5;
    float3 meanWorld = originWorld + statistics.PositionSumWorld / statistics.AreaWorld;
    float3 value = EvaluateSpatialProxy(statistics, meanWorld, context) *
                   EvaluateEmission(scene, context.Emitter, NormalizeOrZero(meanWorld - context.Emitter.PositionWorld));
    GeometryHierarchyNode node = GetGeometryHierarchyNode(scene, nodeIndex);
    float3 extentWorld = node.MaximumWorld - node.MinimumWorld;
    float radiusSquaredWorld = dot(extentWorld, extentWorld) * OMV_BOUND_RADIUS_SQUARED_FACTOR;
    float3 toSensorWorld = context.Receiver.PositionWorld - meanWorld;
    float3 toEmitterWorld = context.Emitter.PositionWorld - meanWorld;
    float minimumDistanceSquaredWorld = min(dot(toSensorWorld, toSensorWorld), dot(toEmitterWorld, toEmitterWorld));
    bool isFar = OMV_FAR_SOLID_ANGLE > 0 && minimumDistanceSquaredWorld > 0 &&
                 PI * radiusSquaredWorld < OMV_FAR_SOLID_ANGLE * minimumDistanceSquaredWorld;
    if (isFar == false)
    {
        [loop] for (uint axis = 0; axis < OMV_SPATIAL_AXIS_COUNT; ++axis)
        {
            float3 offsetWorld = axis == 0 ? statistics.AxesWorld.Axis0
                                           : (axis == 1 ? statistics.AxesWorld.Axis1 : statistics.AxesWorld.Axis2);
            float3 lowerWorld = meanWorld - offsetWorld;
            float3 upperWorld = meanWorld + offsetWorld;
            value += OMV_GAUSSIAN_OFFSET_WEIGHT *
                     (EvaluateSpatialProxy(statistics, lowerWorld, context) *
                          EvaluateEmission(scene,
                                           context.Emitter,
                                           NormalizeOrZero(lowerWorld - context.Emitter.PositionWorld)) +
                      EvaluateSpatialProxy(statistics, upperWorld, context) *
                          EvaluateEmission(scene,
                                           context.Emitter,
                                           NormalizeOrZero(upperWorld - context.Emitter.PositionWorld)));
        }
        value /= 1 + 2 * OMV_SPATIAL_AXIS_COUNT * OMV_GAUSSIAN_OFFSET_WEIGHT;
    }
    float importance = dot(value, OMV_LUMINANCE_WEIGHTS);
    // A singular/overflowing proxy is not transport: bound it without removing proposal support.
    importance = isfinite(importance) == true ? clamp(importance, 0, OMV_MAXIMUM_IMPORTANCE) : OMV_MAXIMUM_IMPORTANCE;
    return min(statistics.AreaWorld * (importance + OMV_DEFENSIVE_WEIGHT), OMV_MAXIMUM_IMPORTANCE);
}
float ComputeLeftProbability(SceneContext scene, GeometryHierarchyNode node, GeometrySamplingContext context)
{
    float left = EvaluateNodeImportance(scene, node.Left, context);
    float right = EvaluateNodeImportance(scene, node.Right, context);
    float scale = max(left, right);
    float probability = scale > 0 ? (left / scale) / (left / scale + right / scale) : 0.5;
    return clamp(probability, OMV_MINIMUM_BRANCH_PROBABILITY, 1 - OMV_MINIMUM_BRANCH_PROBABILITY);
}
GeometrySample SampleGeometry(SceneContext scene, GeometrySamplingContext context, float3 random)
{
    GeometrySample result = (GeometrySample)0;
    if (GetGeometryHierarchyNodeCount(scene) == 0)
        return result;
    uint index = 0;
    float probability = 1;
    GeometryHierarchyNode node = GetGeometryHierarchyNode(scene, index);
    [loop] while (node.TriangleCount == 0)
    {
        float left = ComputeLeftProbability(scene, node, context);
        bool isLeft = random.x < left;
        probability *= isLeft == true ? left : 1 - left;
        random.x = isLeft == true ? random.x / left : (random.x - left) / (1 - left);
        index = isLeft == true ? node.Left : node.Right;
        node = GetGeometryHierarchyNode(scene, index);
    }
    GeometryStatistics statistics = LoadGeometryStatistics(index);
    float targetArea = min(random.x, OMV_MAXIMUM_UNIT_SAMPLE) * statistics.AreaWorld;
    uint triangleIndex = SCENE_INVALID_INDEX;
    [loop] for (uint primitive = 0; primitive < node.TriangleCount; ++primitive)
    {
        triangleIndex = GetGeometryHierarchyTriangle(scene, node.FirstTriangle + primitive);
        targetArea -= GetTriangle(scene, triangleIndex).AreaWorld;
        if (targetArea <= 0)
            break;
    }
    float root = sqrt(random.y);
    result.Hit = GetSurfaceHit(scene, triangleIndex, float2(root * (1 - random.z), root * random.z));
    result.PdfArea = probability / statistics.AreaWorld;
    result.HasSample = result.Hit.IsValid == true && result.PdfArea > 0 && isfinite(result.PdfArea) == true;
    return result;
}
float EvaluateGeometryPdfArea(SceneContext scene, GeometrySamplingContext context, uint triangleIndex)
{
    uint count = GetGeometryHierarchyNodeCount(scene);
    uint mapBase = OMV_HEADER_WORDS + count * OMV_STATISTIC_WORDS;
    uint leaf = GraphBuffer0[mapBase + triangleIndex].x;
    uint index = 0;
    float probability = 1;
    GeometryHierarchyNode node = GetGeometryHierarchyNode(scene, index);
    [loop] while (node.TriangleCount == 0)
    {
        float left = ComputeLeftProbability(scene, node, context);
        bool isLeft = leaf < node.Right;
        probability *= isLeft == true ? left : 1 - left;
        index = isLeft == true ? node.Left : node.Right;
        node = GetGeometryHierarchyNode(scene, index);
    }
    return index == leaf ? probability / LoadGeometryStatistics(index).AreaWorld : 0;
}
