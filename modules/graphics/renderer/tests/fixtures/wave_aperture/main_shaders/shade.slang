#include "hit_record.slang"
#include "wave.slang"

// Scalar Fresnel thin-screen specialization of Steinberg and Pharr (2026), Section 3.1's bilinear path integral.
// The authored Gaussian illuminator is a coherent waist field on a selected plane. Only World triangles coplanar
// with that plane form the opaque mask, as a union; a hole must be real mesh geometry, not an alpha texture.
// All open midpoint cells contribute complex fields, so cross terms survive between different aperture regions.
// Primary camera visibility and receiver materials come from the current World; no CPU transport is uploaded.
// Other scene lights, multiple screens, reflected/transmitted wave paths, polarization and UTD are outside this
// profile. Cost: Resolution^2 * World triangle count per receiving pixel. Intended for small diffraction experiments.
static const float WORLD_UNITS_PER_MILLIMETER = 0.001;
static const float FRAME_HELPER_COSINE = 0.9;

struct ApertureFrame
{
    float3 OriginWorld;
    float3 DirectionWorld;
    float3 AxisXWorld;
    float3 AxisYWorld;
};

ApertureFrame MakeApertureFrame()
{
    ApertureFrame frame;
    frame.OriginWorld = float3(WT_SCREEN_X, WT_SCREEN_Y, WT_SCREEN_Z);
    frame.DirectionWorld = normalize(float3(WT_DIRECTION_X, WT_DIRECTION_Y, WT_DIRECTION_Z));
    float3 helperWorld = abs(frame.DirectionWorld.y) < FRAME_HELPER_COSINE ? float3(0, 1, 0) : float3(1, 0, 0);
    frame.AxisXWorld = normalize(cross(helperWorld, frame.DirectionWorld));
    frame.AxisYWorld = cross(frame.DirectionWorld, frame.AxisXWorld);
    return frame;
}

float3 TransformToApertureWorld(ApertureFrame frame, float3 positionWorld)
{
    float3 offsetWorld = positionWorld - frame.OriginWorld;
    return float3(dot(offsetWorld, frame.AxisXWorld),
                  dot(offsetWorld, frame.AxisYWorld),
                  dot(offsetWorld, frame.DirectionWorld));
}

/// The entire triangle must lie in the authored coplanarity slab. Remote receiver geometry is not a screen.
float EvaluateApertureTransmission(SceneContext scene, ApertureFrame frame, float2 positionApertureWorld)
{
    float toleranceWorld = WT_SCREEN_TOLERANCE_MILLIMETERS * WORLD_UNITS_PER_MILLIMETER;
    uint triangleCount = GetSceneInfo(scene).TriangleCount;
    for (uint index = 0; index < triangleCount; ++index)
    {
        TriangleData triangle = GetTriangle(scene, index);
        float3 firstApertureWorld = TransformToApertureWorld(frame, triangle.Position0World);
        float3 secondApertureWorld = TransformToApertureWorld(frame, triangle.Position1World);
        float3 thirdApertureWorld = TransformToApertureWorld(frame, triangle.Position2World);
        if (abs(firstApertureWorld.z) > toleranceWorld || abs(secondApertureWorld.z) > toleranceWorld ||
            abs(thirdApertureWorld.z) > toleranceWorld)
            continue;
        if (IsWavePointCovered(positionApertureWorld,
                               firstApertureWorld.xy,
                               secondApertureWorld.xy,
                               thirdApertureWorld.xy) == true)
            return 0;
    }
    return 1;
}

ScalarWaveAperture MakeWaveAperture()
{
    ScalarWaveAperture aperture;
    aperture.WavelengthWorld = WT_WAVELENGTH_MILLIMETERS * WORLD_UNITS_PER_MILLIMETER;
    float2 radiusWorld = float2(WT_SIGMA_X_MILLIMETERS, WT_SIGMA_Y_MILLIMETERS) * WORLD_UNITS_PER_MILLIMETER;
    aperture.SigmaWorldSquared = radiusWorld * radiusWorld;
    aperture.HalfExtentWorld =
        float2(WT_HALF_WIDTH_MILLIMETERS, WT_HALF_HEIGHT_MILLIMETERS) * WORLD_UNITS_PER_MILLIMETER;
    aperture.Resolution = WT_RESOLUTION;
    return aperture;
}

float3 EvaluateWaveReceiver(SceneContext scene, RayLaunch launch, inout RayQueryContext queries, uint mode)
{
    HitRecord record;
    if (HasHitRecordCapacity(launch) == false || TryLoadHitRecord(scene, launch, record) == false)
    {
        FailSceneQuery(queries, LUDUS_SCENE_FAILURE_STORED_RECORD);
        return float3(0);
    }
    Ray cameraRay = MakeCameraRay(launch, record.JitterPixel);
    if (record.IsHit == false)
        return EvaluateEnvironment(scene, cameraRay.DirectionWorld);
    SurfaceHit hit = GetSurfaceHit(scene, record.TriangleIndex, record.Barycentric);
    hit.IncomingRay = cameraRay;
    hit.DistanceWorld = record.DistanceWorld;
    hit.IsFrontFace = record.IsFrontFace;
    MaterialSample material = EvaluateMaterial(scene, hit);
    float3 directionWorld = float3(WT_DIRECTION_X, WT_DIRECTION_Y, WT_DIRECTION_Z);
    if (dot(directionWorld, directionWorld) == 0)
    {
        FailSceneQuery(queries, LUDUS_SCENE_FAILURE_WAVE_DIRECTION);
        return float3(0);
    }
    ApertureFrame frame = MakeApertureFrame();
    float3 receiverApertureWorld = TransformToApertureWorld(frame, hit.PositionWorld);
    float toleranceWorld = WT_SCREEN_TOLERANCE_MILLIMETERS * WORLD_UNITS_PER_MILLIMETER;
    if (receiverApertureWorld.z <= toleranceWorld)
        return material.Emission;
    ScalarWaveAperture aperture = MakeWaveAperture();
    if (mode == 2)
        return float3(EvaluateApertureTransmission(scene, frame, receiverApertureWorld.xy));
    float irradiance = 0;
    if (mode == 1)
    {
        bool isInside = all(abs(receiverApertureWorld.xy) <= aperture.HalfExtentWorld) == true;
        irradiance =
            isInside == true
                ? EvaluateApertureTransmission(scene, frame, receiverApertureWorld.xy) *
                      exp(-dot(receiverApertureWorld.xy * receiverApertureWorld.xy, 1 / aperture.SigmaWorldSquared))
                : 0;
    }
    else
    {
        if (CanResolveWavePhase(aperture, receiverApertureWorld) == false)
        {
            FailSceneQuery(queries, LUDUS_SCENE_FAILURE_WAVE_RESOLUTION);
            return float3(0);
        }
        float2 field = float2(0);
        float2 compensation = float2(0);
        for (uint y = 0; y < aperture.Resolution; ++y)
        {
            for (uint x = 0; x < aperture.Resolution; ++x)
            {
                float2 positionApertureWorld = GetWaveCellPosition(aperture, uint2(x, y));
                float transmission = EvaluateApertureTransmission(scene, frame, positionApertureWorld);
                float2 contribution =
                    transmission * EvaluateWaveCell(aperture, positionApertureWorld, receiverApertureWorld);
                float2 corrected = contribution - compensation;
                float2 sum = field + corrected;
                compensation = (sum - field) - corrected;
                field = sum;
            }
        }
        irradiance = dot(field, field);
    }
    // Lambertian receiver, monochromatic irradiance displayed in neutral RGB. No invented scene light is added;
    // the Gaussian illuminator's authored peak controls the only source used by this experimental profile.
    float cosine = abs(dot(hit.GeometricNormalWorld, frame.DirectionWorld));
    return material.Emission + material.BaseColor * (WT_PEAK_IRRADIANCE * irradiance * cosine / PI);
}

float3 ShadeWave(SceneContext scene, RayLaunch launch, inout RayQueryContext queries)
{
    return EvaluateWaveReceiver(scene, launch, queries, 0);
}
float3 ShadeGeometric(SceneContext scene, RayLaunch launch, inout RayQueryContext queries)
{
    return EvaluateWaveReceiver(scene, launch, queries, 1);
}
float3 MeasureAperture(SceneContext scene, RayLaunch launch, inout RayQueryContext queries)
{
    return EvaluateWaveReceiver(scene, launch, queries, 2);
}
