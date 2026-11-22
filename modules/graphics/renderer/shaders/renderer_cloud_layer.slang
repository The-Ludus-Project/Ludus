#pragma once

#include "math.slang"
#include "renderer_atmosphere_layout.h"
#include "renderer_atmosphere_transport.slang"

// One bounded, homogeneous cloud slab (CloudLayerDescription), composed with the clear-sky segment along a view
// ray. See docs/architecture/world-environment.md#atmospheric-transport-and-renderer-integration.
//
// Thanks to Sébastien Hillaire, Physically Based Sky, Atmosphere and Cloud Rendering in Frostbite, SIGGRAPH 2016
// course notes, §5.9.1: clouds are marched front to back for radiance and transmittance, and aerial perspective
// applies to them. Ludus places the cloud between the clear air in front of its entry and the clear-sky result of
// the whole ray, so the air is counted once instead of applying a second camera-to-cloud haze.
// https://media.contentapi.ea.com/content/dam/eacom/frostbite/files/s2016-pbs-frostbite-sky-clouds-new.pdf
// Thanks to Louis G. Henyey and Jesse L. Greenstein, Diffuse Radiation in the Galaxy, Astrophysical Journal 93,
// 1941, pp. 70-83, for the phase function. https://doi.org/10.1086/144246

static const uint CLOUD_LAYER_STEPS = LUDUS_CLOUD_LAYER_STEPS;
static const uint CLOUD_LAYER_AIR_STEPS = LUDUS_CLOUD_LAYER_AIR_STEPS;
static const float CLOUD_LAYER_MAXIMUM_PATH_METRES = LUDUS_CLOUD_LAYER_MAXIMUM_PATH_METRES;
static const float CLOUD_LAYER_METRES_PER_KILOMETRE = LUDUS_ATMOSPHERE_METRES_PER_KILOMETRE;
// A ray direction component below this is parallel to the slab faces normal to that axis.
static const float CLOUD_LAYER_PARALLEL_EPSILON = 0.000001;

static const uint CLOUD_LAYER_UNIFORM_ROWS = LUDUS_CLOUD_LAYER_UNIFORM_ROWS;

// Packed by TryPackCloudLayer (render_graph_atmosphere.cpp) as CLOUD_LAYER_UNIFORM_ROWS float4 rows, in this field
// order; uniforms carry the rows because browser raster uniforms admit no nested structures. A zero AxisXWorld.w
// omits the layer. A nonzero Phase.y replaces the homogeneous extinction with CloudLayerTestField's two regions.
struct CloudLayerParameters
{
    float4 AxisXWorld;  // xyz: unit layer +X, w: one when the layer is drawn.
    float4 UpWorld;     // xyz: unit layer +Y, the Atmosphere anchor's up; w: base height in metres.
    float4 AxisZWorld;  // xyz: unit layer +Z; w: top height in metres.
    float4 CameraLocal; // xyz: eye in the layer frame in metres; w: edge fade in metres.
    float4 Medium;      // xy: half sizes along X and Z in metres; z: extinction and w: scattering, per metre.
    float4 Phase;       // x: Henyey-Greenstein asymmetry; y: one with the test field; z: single-scattering albedo.
    float4 TestSplit;   // xyz: unit split normal in the layer frame; w: plane offset in metres.
    float4 TestRegion0; // Negative side of the plane. xyz: source radiance; w: extinction per metre.
    float4 TestRegion1; // Positive side of the plane, packed as TestRegion0.
};

// The coefficients of the layer at one point before the edge fade: extinction per metre and the test source
// radiance, which emits extinction * source per metre.
struct CloudLayerMedium
{
    float Extinction;
    float3 Source;
};

// The slab's part of a view ray: scene-linear radiance scattered toward the camera at the entry, grey
// transmittance, and the clipped interval in metres. An empty interval (ExitMetres <= EntryMetres) is a miss.
struct CloudLayerSegment
{
    float3 Radiance;
    float Transmittance;
    float EntryMetres;
    float ExitMetres;
};

CloudLayerParameters UnpackCloudLayer(in float4 rows[CLOUD_LAYER_UNIFORM_ROWS])
{
    CloudLayerParameters cloud;
    cloud.AxisXWorld = rows[0];
    cloud.UpWorld = rows[1];
    cloud.AxisZWorld = rows[2];
    cloud.CameraLocal = rows[3];
    cloud.Medium = rows[4];
    cloud.Phase = rows[5];
    cloud.TestSplit = rows[6];
    cloud.TestRegion0 = rows[7];
    cloud.TestRegion1 = rows[8];
    return cloud;
}

bool HasCloudLayerTestField(in CloudLayerParameters cloud)
{
    return cloud.Phase.y > 0;
}

// Signed distance of positionLocal from the test field's plane, negative in TestRegion0.
float GetCloudLayerSplitSide(in CloudLayerParameters cloud, in float3 positionLocal)
{
    return dot(positionLocal, cloud.TestSplit.xyz) - cloud.TestSplit.w;
}

CloudLayerMedium SampleCloudLayerMedium(in CloudLayerParameters cloud, in float3 positionLocal)
{
    CloudLayerMedium medium;
    medium.Extinction = cloud.Medium.z;
    medium.Source = 0;
    if (HasCloudLayerTestField(cloud) == true)
    {
        float4 region = GetCloudLayerSplitSide(cloud, positionLocal) < 0 ? cloud.TestRegion0 : cloud.TestRegion1;
        medium.Extinction = region.w;
        medium.Source = region.xyz;
    }
    return medium;
}

// Distance along originLocal + t * directionLocal, t > 0, where the ray crosses the test field's plane, or
// fallbackMetres when it has no test field, is parallel to the plane or crosses it behind the origin.
float GetCloudLayerSplitDistance(in CloudLayerParameters cloud,
                                 in float3 originLocal,
                                 in float3 directionLocal,
                                 float fallbackMetres)
{
    float rate = dot(directionLocal, cloud.TestSplit.xyz);
    if (HasCloudLayerTestField(cloud) == false || abs(rate) < CLOUD_LAYER_PARALLEL_EPSILON)
    {
        return fallbackMetres;
    }
    float distanceMetres = -GetCloudLayerSplitSide(cloud, originLocal) / rate;
    return distanceMetres > 0 ? distanceMetres : fallbackMetres;
}

// Optical depth before the edge fade from positionLocal along directionLocal for lengthMetres, where the point's
// own extinction is extinction. A test field's path is split at its plane.
float ComputeCloudLayerPathDepth(in CloudLayerParameters cloud,
                                 in float3 positionLocal,
                                 in float3 directionLocal,
                                 float extinction,
                                 float lengthMetres)
{
    float splitMetres = GetCloudLayerSplitDistance(cloud, positionLocal, directionLocal, lengthMetres);
    if (splitMetres >= lengthMetres)
    {
        return extinction * lengthMetres;
    }
    float beyond = GetCloudLayerSplitSide(cloud, positionLocal) < 0 ? cloud.TestRegion1.w : cloud.TestRegion0.w;
    return extinction * splitMetres + beyond * (lengthMetres - splitMetres);
}

float3 ToCloudLayerLocal(in CloudLayerParameters cloud, in float3 directionWorld)
{
    return float3(dot(directionWorld, cloud.AxisXWorld.xyz),
                  dot(directionWorld, cloud.UpWorld.xyz),
                  dot(directionWorld, cloud.AxisZWorld.xyz));
}

// Clip [inoutNear, inoutFar] to one axis's slab. A ray parallel to the faces and outside them empties the interval.
void ClipCloudLayerAxis(float origin,
                        float direction,
                        float minimum,
                        float maximum,
                        inout float inoutNear,
                        inout float inoutFar)
{
    if (abs(direction) < CLOUD_LAYER_PARALLEL_EPSILON)
    {
        if (origin < minimum || origin > maximum)
        {
            inoutNear = 1;
            inoutFar = 0;
        }
        return;
    }
    float first = (minimum - origin) / direction;
    float second = (maximum - origin) / direction;
    inoutNear = max(inoutNear, min(first, second));
    inoutFar = min(inoutFar, max(first, second));
}

// The part of [0, maximumMetres] along originLocal + t * directionLocal inside the slab's box.
bool IntersectCloudLayer(in CloudLayerParameters cloud,
                         in float3 originLocal,
                         in float3 directionLocal,
                         float maximumMetres,
                         out float outNear,
                         out float outFar)
{
    float near = 0;
    float far = maximumMetres;
    ClipCloudLayerAxis(originLocal.x, directionLocal.x, -cloud.Medium.x, cloud.Medium.x, near, far);
    ClipCloudLayerAxis(originLocal.y, directionLocal.y, cloud.UpWorld.w, cloud.AxisZWorld.w, near, far);
    ClipCloudLayerAxis(originLocal.z, directionLocal.z, -cloud.Medium.y, cloud.Medium.y, near, far);
    outNear = near;
    outFar = far;
    return near < far;
}

// Density multiplier: one inside the footprint, falling smoothly to zero over the edge fade at each side.
float ComputeCloudLayerFade(in CloudLayerParameters cloud, in float3 positionLocal)
{
    float inset = min(cloud.Medium.x - abs(positionLocal.x), cloud.Medium.y - abs(positionLocal.z));
    return cloud.CameraLocal.w > 0 ? smoothstep(0, cloud.CameraLocal.w, inset) : 1;
}

// Normalized over the sphere, in inverse steradians. The cosine is between the view direction and the direction
// toward the Sun, so positive asymmetry brightens the cloud around the Sun.
float EvaluateCloudLayerPhase(float asymmetry, float cosine)
{
    const float SPHERE_SOLID_ANGLE_PER_PI = 4;
    float asymmetrySquared = asymmetry * asymmetry;
    float base = 1 + asymmetrySquared - 2 * asymmetry * cosine;
    return (1 - asymmetrySquared) / (SPHERE_SOLID_ANGLE_PER_PI * PI * base * sqrt(base));
}

// The per-ray inputs of IntegrateCloudLayer shared by every part of its march.
struct CloudLayerRay
{
    float3 RayLocal; // Unit view ray in the layer frame (+X, +Y up, +Z).
    float3 SunLocal; // Unit direction toward the Sun in the layer frame.
    float3 SolarIlluminance;
    float Phase;
    float ScatteringCosine;
    float Height;
    float ZenithCosine;
    float SolarZenithCosine;
};

// Add one homogeneous part [startMetres, endMetres] of the view ray, sampled at its midpoint, to inoutSegment. Its
// transmittance is exact for the part's constant density. The source is the test field's emission plus the
// scattering coefficient times the phase function times the Sun reaching the midpoint through the air
// (transmittance table, zero in the planet's shadow) and through the slab.
void AccumulateCloudLayerPart(Texture2D<float4> transmittanceTable,
                              in CloudLayerParameters cloud,
                              in CloudLayerRay ray,
                              float startMetres,
                              float endMetres,
                              inout CloudLayerSegment inoutSegment)
{
    float partLength = endMetres - startMetres;
    if (partLength <= 0)
    {
        return;
    }
    float distanceMetres = 0.5 * (startMetres + endMetres);
    float3 positionLocal = cloud.CameraLocal.xyz + distanceMetres * ray.RayLocal;
    float fade = ComputeCloudLayerFade(cloud, positionLocal);
    CloudLayerMedium medium = SampleCloudLayerMedium(cloud, positionLocal);
    float extinction = fade * medium.Extinction;
    float scattering = HasCloudLayerTestField(cloud) == true ? extinction * cloud.Phase.z : fade * cloud.Medium.w;
    float opticalDepth = extinction * partLength;
    float partTransmittance = exp(-opticalDepth);
    float integrated =
        opticalDepth < OPTICAL_DEPTH_LINEAR_THRESHOLD ? 1 - opticalDepth * 0.5 : (1 - partTransmittance) / opticalDepth;
    // The emission integrates to source * (1 - partTransmittance).
    inoutSegment.Radiance += (inoutSegment.Transmittance * integrated * opticalDepth) * medium.Source;
    float distance = distanceMetres / CLOUD_LAYER_METRES_PER_KILOMETRE;
    float altitude = ComputeAltitude(ray.Height, ray.ZenithCosine, distance);
    float radius = GROUND_RADIUS + ray.Height;
    float sampleSolarCosine =
        clamp((radius * ray.SolarZenithCosine + distance * ray.ScatteringCosine) / (GROUND_RADIUS + altitude), -1, 1);
    float sunNear;
    float sunFar;
    if (scattering > 0 && sampleSolarCosine >= ComputeHorizonCosine(altitude) &&
        IntersectCloudLayer(cloud, positionLocal, ray.SunLocal, CLOUD_LAYER_MAXIMUM_PATH_METRES, sunNear, sunFar) ==
            true)
    {
        float sunDepth =
            fade * ComputeCloudLayerPathDepth(cloud, positionLocal, ray.SunLocal, medium.Extinction, sunFar);
        float3 incident = ray.SolarIlluminance * SampleTransmittance(transmittanceTable, altitude, sampleSolarCosine) *
                          exp(-sunDepth);
        inoutSegment.Radiance +=
            (inoutSegment.Transmittance * integrated * partLength * scattering * ray.Phase) * incident;
    }
    inoutSegment.Transmittance *= partTransmittance;
}

// Integrate the slab along a camera ray. height is the camera's altitude in kilometres and the cosines are measured
// at the camera against its radial up; maximumMetres clips the ray at the first opaque surface or ground. The
// clipped interval takes CLOUD_LAYER_STEPS uniform steps. A test field's plane splits the step it crosses into two
// homogeneous parts, so a piecewise-constant field is integrated exactly along the view.
CloudLayerSegment IntegrateCloudLayer(Texture2D<float4> transmittanceTable,
                                      in CloudLayerParameters cloud,
                                      in float3 directionWorld,
                                      in float3 sunWorld,
                                      in float3 solarIlluminance,
                                      float height,
                                      float zenithCosine,
                                      float solarZenithCosine,
                                      float maximumMetres)
{
    CloudLayerSegment segment;
    segment.Radiance = 0;
    segment.Transmittance = 1;
    segment.EntryMetres = 0;
    segment.ExitMetres = 0;
    CloudLayerRay ray;
    ray.RayLocal = ToCloudLayerLocal(cloud, directionWorld);
    float entryMetres;
    float exitMetres;
    if (cloud.AxisXWorld.w <= 0 ||
        IntersectCloudLayer(cloud, cloud.CameraLocal.xyz, ray.RayLocal, maximumMetres, entryMetres, exitMetres) ==
            false)
    {
        return segment;
    }
    segment.EntryMetres = entryMetres;
    segment.ExitMetres = exitMetres;
    ray.SunLocal = ToCloudLayerLocal(cloud, sunWorld);
    ray.SolarIlluminance = solarIlluminance;
    ray.ScatteringCosine = clamp(dot(directionWorld, sunWorld), -1, 1);
    ray.Phase = EvaluateCloudLayerPhase(cloud.Phase.x, ray.ScatteringCosine);
    ray.Height = height;
    ray.ZenithCosine = zenithCosine;
    ray.SolarZenithCosine = solarZenithCosine;
    float splitMetres = GetCloudLayerSplitDistance(cloud, cloud.CameraLocal.xyz, ray.RayLocal, exitMetres);
    float stepLength = (exitMetres - entryMetres) / CLOUD_LAYER_STEPS;
    for (uint step = 0; step < CLOUD_LAYER_STEPS; ++step)
    {
        float startMetres = entryMetres + float(step) * stepLength;
        float endMetres = step + 1 == CLOUD_LAYER_STEPS ? exitMetres : startMetres + stepLength;
        float middleMetres = clamp(splitMetres, startMetres, endMetres);
        AccumulateCloudLayerPart(transmittanceTable, cloud, ray, startMetres, middleMetres, segment);
        AccumulateCloudLayerPart(transmittanceTable, cloud, ray, middleMetres, endMetres, segment);
    }
    return segment;
}

// The clear-sky segment of the whole ray (clearRadiance) contains the air in front of the cloud's entry
// (airRadiance, airTransmittance). Compose air, cloud and the rest of the ray front to back:
// L = L_air + T_air * L_cloud + T_cloud * (L_clear - L_air). An empty cloud returns clearRadiance. The air inside
// the slab stays behind the cloud, attenuated by its full transmittance.
float3 ComposeCloudLayer(in CloudLayerSegment cloud,
                         in float3 airRadiance,
                         in float3 airTransmittance,
                         in float3 clearRadiance)
{
    return airRadiance + airTransmittance * cloud.Radiance +
           cloud.Transmittance * max(clearRadiance - airRadiance, float3(0));
}
