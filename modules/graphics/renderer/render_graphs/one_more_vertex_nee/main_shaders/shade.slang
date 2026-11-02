#include "geometry_sampling.slang"

// Section 5's two competing constructions of the same three-vertex path, in area measure.
// The selected emitter is shared by both strategies, so its PDF cancels in the MIS weights.
// The camera integral is bounded to emission, one-scattering direct and two-scattering indirect
// mesh-emitter paths. No indirect term is counted again as a continuation's emission.
static const uint OMV_EMISSION_COMPONENT = 1;
static const uint OMV_DIRECT_COMPONENT = 2;
static const uint OMV_INDIRECT_COMPONENT = 3;
static const uint OMV_CAMERA_DIMENSION = 0;
static const uint OMV_EMITTER_DIMENSION = 2;
static const uint OMV_EMITTER_SHAPE_DIMENSION = 3;
static const uint OMV_GEOMETRY_DIMENSION = 6;
static const uint OMV_BSDF_DIMENSION = 9;
static const float OMV_MAXIMUM_DISTANCE_WORLD = 3.402823466e38;

float3 SampleOmvRandom3(RayLaunch launch, uint firstDimension)
{
    return float3(SampleSceneRandom(launch, firstDimension),
                  SampleSceneRandom(launch, firstDimension + 1),
                  SampleSceneRandom(launch, firstDimension + 2));
}

bool TryLoadOmvMaterial(SceneContext scene,
                        SurfaceHit hit,
                        inout RayQueryContext queries,
                        out MaterialSample outMaterial)
{
    outMaterial = EvaluateMaterial(scene, hit);
    if (outMaterial.IsValid == false || outMaterial.Model == LUDUS_MATERIAL_MODEL_COATED_DIFFUSE)
    {
        FailSceneQuery(queries,
                       outMaterial.IsValid == false ? LUDUS_SCENE_FAILURE_SCENE_DATA
                                                    : LUDUS_SCENE_FAILURE_PRIMARY_MATERIAL);
        return false;
    }
    return true;
}
bool TrySampleOmvEmitter(SceneContext scene, RayLaunch launch, out SurfaceHit outEmitter, out float outPdfArea)
{
    outEmitter = (SurfaceHit)0;
    outPdfArea = 0;
    uint count = GetEmitterCount(scene);
    if (count == 0)
        return false;
    uint emitterIndex = min(uint(SampleSceneRandom(launch, OMV_EMITTER_DIMENSION) * count), count - 1);
    float4 emitter = GetEmitter(scene, emitterIndex);
    float3 random = SampleOmvRandom3(launch, OMV_EMITTER_SHAPE_DIMENSION);
    float targetArea = random.x * emitter.z;
    TriangleData triangle = (TriangleData)0;
    [loop] for (uint primitive = 0; primitive < uint(emitter.y); ++primitive)
    {
        triangle = GetEmitterTriangle(scene, emitterIndex, primitive);
        targetArea -= triangle.AreaWorld;
        if (targetArea <= 0)
            break;
    }
    if (triangle.IsValid == false || emitter.z <= 0)
        return false;
    float root = sqrt(random.y);
    outEmitter = GetSurfaceHit(scene, triangle.TriangleIndex, float2(root * (1 - random.z), root * random.z));
    outPdfArea = 1 / (float(count) * emitter.z);
    return outEmitter.IsValid == true && IsSurfaceCovered(scene, outEmitter) == true;
}
LightSample MakeOmvConnection(SurfaceHit from, SurfaceHit to)
{
    LightSample result = (LightSample)0;
    float3 offsetWorld = to.PositionWorld - from.PositionWorld;
    result.DistanceWorld = length(offsetWorld);
    result.DirectionWorld = result.DistanceWorld > 0 ? offsetWorld / result.DistanceWorld : float3(0);
    result.PositionWorld = to.PositionWorld;
    result.TriangleIndex = to.TriangleIndex;
    result.Barycentric = to.Barycentric;
    result.HasFiniteEndpoint = true;
    result.HasSample = result.DistanceWorld > 0;
    return result;
}
float3 EvaluateOmvDirect(SceneContext scene, GeometrySamplingContext context, inout RayQueryContext queries)
{
    LightSample light = MakeOmvConnection(context.Receiver, context.Emitter);
    if (light.HasSample == false || dot(context.Receiver.GeometricNormalWorld, light.DirectionWorld) <= 0)
        return float3(0);
    float3 value;
    float pdf;
    EvaluateStandardBsdf(context.Material, context.ViewDirectionWorld, light.DirectionWorld, value, pdf);
    float3 emission = EvaluateEmission(scene, context.Emitter, -light.DirectionWorld);
    float cosineReceiver = max(0, dot(context.Material.ShadingNormalWorld, light.DirectionWorld));
    float cosineEmitter = abs(dot(context.Emitter.GeometricNormalWorld, -light.DirectionWorld));
    float3 contribution = value * emission * cosineReceiver * cosineEmitter / light.DistanceWorld / light.DistanceWorld;
    if (any(contribution > 0) == false || TestVisibility(scene, queries, context.Receiver, light) != SCENE_QUERY_MISS)
        return float3(0);
    return contribution;
}
float3 EvaluateOmvConnection(SceneContext scene,
                             GeometrySamplingContext context,
                             SurfaceHit middle,
                             bool shouldTestFirstVisibility,
                             inout RayQueryContext queries,
                             out float outBsdfPdfArea)
{
    outBsdfPdfArea = 0;
    if (IsSurfaceCovered(scene, middle) == false)
        return float3(0);
    LightSample first = MakeOmvConnection(context.Receiver, middle);
    LightSample second = MakeOmvConnection(middle, context.Emitter);
    if (first.HasSample == false || second.HasSample == false ||
        dot(context.Receiver.GeometricNormalWorld, first.DirectionWorld) <= 0 ||
        dot(middle.GeometricNormalWorld, -first.DirectionWorld) <= 0 ||
        dot(middle.GeometricNormalWorld, second.DirectionWorld) <= 0)
        return float3(0);
    float3 sensorValue;
    float sensorPdf;
    EvaluateStandardBsdf(context.Material, context.ViewDirectionWorld, first.DirectionWorld, sensorValue, sensorPdf);
    float firstJacobian =
        abs(dot(middle.GeometricNormalWorld, -first.DirectionWorld)) / first.DistanceWorld / first.DistanceWorld;
    outBsdfPdfArea = sensorPdf * firstJacobian;
    MaterialSample material;
    if (TryLoadOmvMaterial(scene, middle, queries, material) == false)
        return float3(0);
    float3 middleValue;
    float middlePdf;
    EvaluateStandardBsdf(material, -first.DirectionWorld, second.DirectionWorld, middleValue, middlePdf);
    float3 emission = EvaluateEmission(scene, context.Emitter, -second.DirectionWorld);
    float secondGeometry = max(0, dot(material.ShadingNormalWorld, second.DirectionWorld)) *
                           abs(dot(context.Emitter.GeometricNormalWorld, -second.DirectionWorld)) /
                           second.DistanceWorld / second.DistanceWorld;
    float3 contribution = sensorValue * max(0, dot(context.Material.ShadingNormalWorld, first.DirectionWorld)) *
                          firstJacobian * middleValue * secondGeometry * emission;
    if (any(contribution > 0) == false)
        return float3(0);
    if (shouldTestFirstVisibility == true &&
        TestVisibility(scene, queries, context.Receiver, first) != SCENE_QUERY_MISS)
        return float3(0);
    if (TestVisibility(scene, queries, middle, second) != SCENE_QUERY_MISS)
        return float3(0);
    return contribution;
}
float3 SelectOmvComponent(float3 emission, float3 direct, float3 indirect)
{
    if (OMV_OUTPUT_COMPONENT == OMV_EMISSION_COMPONENT)
        return emission;
    if (OMV_OUTPUT_COMPONENT == OMV_DIRECT_COMPONENT)
        return direct;
    if (OMV_OUTPUT_COMPONENT == OMV_INDIRECT_COMPONENT)
        return indirect;
    return emission + direct + indirect;
}
struct OmvTransport
{
    float3 Emission;
    float3 Direct;
    float3 IndirectMis;
    float3 IndirectGeometry;
    float3 IndirectBsdf;
};
OmvTransport EvaluateOmvTransport(SceneContext scene, RayLaunch launch, inout RayQueryContext queries)
{
    OmvTransport result = (OmvTransport)0;
    if (HasGeometryStatistics(scene) == false || GetLightCount(scene) != GetEmitterCount(scene))
    {
        FailSceneQuery(queries,
                       HasGeometryStatistics(scene) == false ? LUDUS_SCENE_FAILURE_GEOMETRY_STATISTICS_CAPACITY
                                                             : LUDUS_SCENE_FAILURE_GEOMETRY_SAMPLING_LIGHTS);
        return result;
    }
    float2 jitterPixel =
        float2(SampleSceneRandom(launch, OMV_CAMERA_DIMENSION), SampleSceneRandom(launch, OMV_CAMERA_DIMENSION + 1));
    Ray cameraRay = MakeCameraRay(launch, jitterPixel);
    GeometrySamplingContext context;
    uint status = TraceClosest(scene, queries, cameraRay, context.Receiver);
    if (status != SCENE_QUERY_SURFACE)
        return result;
    context.ViewDirectionWorld = -cameraRay.DirectionWorld;
    if (TryLoadOmvMaterial(scene, context.Receiver, queries, context.Material) == false)
        return result;
    result.Emission = EvaluateEmission(scene, context.Receiver, context.ViewDirectionWorld);
    if (dot(context.Receiver.GeometricNormalWorld, context.ViewDirectionWorld) <= 0)
        return result;
    float emitterPdfArea;
    if (TrySampleOmvEmitter(scene, launch, context.Emitter, emitterPdfArea) == false)
        return result;
    result.Direct = EvaluateOmvDirect(scene, context, queries) / emitterPdfArea;
    GeometrySample geometry = SampleGeometry(scene, context, SampleOmvRandom3(launch, OMV_GEOMETRY_DIMENSION));
    if (geometry.HasSample == false)
    {
        FailSceneQuery(queries, LUDUS_SCENE_FAILURE_INVALID_WEIGHT);
        return result;
    }
    float bsdfPdfArea;
    float3 contribution = EvaluateOmvConnection(scene, context, geometry.Hit, true, queries, bsdfPdfArea);
    result.IndirectMis = contribution / (geometry.PdfArea + bsdfPdfArea) / emitterPdfArea;
    result.IndirectGeometry = contribution / geometry.PdfArea / emitterPdfArea;
    float3 directionWorld =
        SampleStandardBsdf(context.Material, context.ViewDirectionWorld, SampleOmvRandom3(launch, OMV_BSDF_DIMENSION));
    if (dot(context.Receiver.GeometricNormalWorld, directionWorld) > 0)
    {
        SurfaceHit middle;
        Ray ray = SpawnSurfaceRay(context.Receiver, directionWorld, OMV_MAXIMUM_DISTANCE_WORLD);
        if (TraceClosest(scene, queries, ray, middle) == SCENE_QUERY_SURFACE)
        {
            contribution = EvaluateOmvConnection(scene, context, middle, false, queries, bsdfPdfArea);
            if (bsdfPdfArea > 0)
            {
                float geometryPdfArea = EvaluateGeometryPdfArea(scene, context, middle.TriangleIndex);
                result.IndirectMis += contribution / (bsdfPdfArea + geometryPdfArea) / emitterPdfArea;
                result.IndirectBsdf = contribution / bsdfPdfArea / emitterPdfArea;
            }
        }
    }
    return result;
}

#include "comparison.slang"
float3 ShadeOneMoreVertex(SceneContext scene, RayLaunch launch, inout RayQueryContext queries)
{
    if (HasOmvComparisonCapacity(launch) == false)
    {
        FailSceneQuery(queries, LUDUS_SCENE_FAILURE_STORAGE_CAPACITY);
        return float3(0);
    }
    OmvTransport transport = EvaluateOmvTransport(scene, launch, queries);
    float3 radiance = SelectOmvComponent(transport.Emission, transport.Direct, transport.IndirectMis);
    float3 geometry = SelectOmvComponent(transport.Emission, transport.Direct, transport.IndirectGeometry);
    float3 bsdf = SelectOmvComponent(transport.Emission, transport.Direct, transport.IndirectBsdf);
    if (all(isfinite(radiance)) == false || any(radiance < 0) == true || all(isfinite(geometry)) == false ||
        any(geometry < 0) == true || all(isfinite(bsdf)) == false || any(bsdf < 0) == true)
        FailSceneQuery(queries, LUDUS_SCENE_FAILURE_INVALID_RADIANCE);
    StoreOmvComparisons(scene, launch, geometry, bsdf, queries);
    return radiance;
}
float3 ShadeGeometryOnly(SceneContext scene, RayLaunch launch, inout RayQueryContext queries)
{
    return LoadOmvComparison(scene, launch, true, queries);
}
float3 ShadeBsdfOnly(SceneContext scene, RayLaunch launch, inout RayQueryContext queries)
{
    return LoadOmvComparison(scene, launch, false, queries);
}
