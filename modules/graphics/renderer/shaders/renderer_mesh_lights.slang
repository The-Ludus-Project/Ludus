#pragma once
#include "linearly_transformed_cosine.slang"

// Thanks to Heitz, Dupuy, Hill and Neubelt, "Real-Time Polygonal-Light Shading
// with Linearly Transformed Cosines", TOG 35(4), Article 41 (2016), section 3,
// https://doi.org/10.1145/2897824.2925895. The CPU check is raster_mesh_lights.cpp.
// Integrate real convex emitter polygons without point proxies, finite-range cutoffs or
// visibility. The host merges coplanar constant-emission triangles into these records;
// both integrals are additive over that partition.
//
// Each record streams its corners once, holding no corner array: a Sutherland-Hodgman
// clip to the receiver horizon feeds the spherical edge sum of the Lambert term (the
// sum ComputeProjectedSolidAngleIntegral evaluates) and, after the LTC's M^-1, a second
// horizon clip feeding the specular edge sum. This is the receiver clip, transform and
// transformed clip of the CPU check, so a record may carry any corner count up to
// MAX_MESH_LIGHT_POLYGON_CORNERS at the cost of one edge term per corner.
//
// A record far from the receiver relative to its bounding sphere is instead integrated as a
// point at the sphere's centre (TryEvaluateRasterMeshLightFar in raster_mesh_lights.h, which
// credits Cohen and Wallace's differential-area form factor). In view of Bistro most evaluated
// records are such distant lamps; the point changed under 1% of pixels by one 8-bit step.
static const uint MESH_LIGHT_TRIANGLE_CORNER_COUNT = 3;
static const float MESH_LIGHT_DIELECTRIC_F0 = 0.04;
static const float MESH_LIGHT_FRAME_AXIS_THRESHOLD = 0.9;

// Running spherical edge sum of a closed loop of directions.
struct MeshLightEdgeSum
{
    float3 FirstTangent;
    float3 PreviousTangent;
    uint Count;
    float Integral;
};
// One open Sutherland-Hodgman stage: the first and latest input corners.
struct MeshLightHorizonClip
{
    float3 FirstTangent;
    float3 PreviousTangent;
    uint Count;
};
struct MeshLightStream
{
    MeshLightEdgeSum Diffuse;
    MeshLightHorizonClip TransformedClip;
    MeshLightEdgeSum Specular;
};

void AddMeshLightEdgeVertex(inout MeshLightEdgeSum inoutSum, float3 positionTangent)
{
    float3 directionTangent = NormalizeProjectedDirection(positionTangent);
    if (inoutSum.Count > 0)
    {
        inoutSum.Integral += ComputeProjectedSolidAngleEdge(inoutSum.PreviousTangent, directionTangent);
    }
    else
    {
        inoutSum.FirstTangent = directionTangent;
    }
    inoutSum.PreviousTangent = directionTangent;
    ++inoutSum.Count;
}
// Projected solid angle in [0, PI] of the closed loop; fewer than three clipped corners enclose nothing.
float CloseMeshLightEdgeSum(MeshLightEdgeSum sum)
{
    if (sum.Count < MESH_LIGHT_TRIANGLE_CORNER_COUNT)
    {
        return 0;
    }
    float integral = sum.Integral + ComputeProjectedSolidAngleEdge(sum.PreviousTangent, sum.FirstTangent);
    return min(abs(integral) * 0.5, PI);
}
// Emit the clipped part of the edge start -> end into a sum: its horizon crossing, then end when not below.
void ClipMeshLightEdge(float3 startTangent, float3 endTangent, inout MeshLightEdgeSum inoutSum)
{
    if ((startTangent.z > 0 && endTangent.z < 0) || (startTangent.z < 0 && endTangent.z > 0))
    {
        float t = startTangent.z / (startTangent.z - endTangent.z);
        AddMeshLightEdgeVertex(inoutSum, float3(lerp(startTangent.xy, endTangent.xy, t), 0));
    }
    if (endTangent.z >= 0)
    {
        AddMeshLightEdgeVertex(inoutSum, endTangent);
    }
}
// A corner of the receiver-clipped polygon: add it to the Lambert sum and stream it, transformed, into the
// specular stage's own horizon clip.
void AddMeshLightClippedCorner(inout MeshLightStream inoutStream,
                               float3 positionTangent,
                               in LinearlyTransformedCosine distribution,
                               bool hasSpecular)
{
    AddMeshLightEdgeVertex(inoutStream.Diffuse, positionTangent);
    if (hasSpecular == false)
    {
        return;
    }
    float3 transformedTangent = MultiplyColumns(distribution.InverseTransform, positionTangent);
    if (inoutStream.TransformedClip.Count > 0)
    {
        ClipMeshLightEdge(inoutStream.TransformedClip.PreviousTangent, transformedTangent, inoutStream.Specular);
    }
    else
    {
        inoutStream.TransformedClip.FirstTangent = transformedTangent;
    }
    inoutStream.TransformedClip.PreviousTangent = transformedTangent;
    ++inoutStream.TransformedClip.Count;
}
// The receiver-horizon stage for the edge start -> end of the record.
void ClipMeshLightReceiverEdge(inout MeshLightStream inoutStream,
                               float3 startTangent,
                               float3 endTangent,
                               in LinearlyTransformedCosine distribution,
                               bool hasSpecular)
{
    if ((startTangent.z > 0 && endTangent.z < 0) || (startTangent.z < 0 && endTangent.z > 0))
    {
        float t = startTangent.z / (startTangent.z - endTangent.z);
        AddMeshLightClippedCorner(inoutStream,
                                  float3(lerp(startTangent.xy, endTangent.xy, t), 0),
                                  distribution,
                                  hasSpecular);
    }
    if (endTangent.z >= 0)
    {
        AddMeshLightClippedCorner(inoutStream, endTangent, distribution, hasSpecular);
    }
}
// Receiver terms shared by every record a shaded sample evaluates.
struct MeshLightReceiver
{
    float3 PositionWorld;
    float3 NormalWorld;
    float3 TangentWorld;
    float3 BitangentWorld;
    float3 DiffuseAlbedo;
    float3 SpecularAlbedo;
    LinearlyTransformedCosine Distribution;
    // GGX alpha, the squared roughness, which bounds the light size the far-field specular term accepts.
    float SpecularAlpha;
    bool HasSpecular;
};
// Whether the record whose header starts at word first can light the receiver: enough corners, the receiver off
// its plane and in front of a one-sided emitter, and, with a positive cull threshold, a Lambert bound at or above it.
// The polygon's solid angle is at most area / d^2 for its nearest distance d, which is at least both the distance to
// its plane and the distance to its bounding sphere; the first corner's w holds that area.
bool CanMeshLightRecordReach(uint first, float3 positionWorld, float cullThreshold)
{
    float4 plane = meshLightWords[first];
    float4 emission = meshLightWords[first + 1];
    float4 origin = meshLightWords[first + MESH_LIGHT_POLYGON_HEADER_WORDS];
    float facing = dot(plane.xyz, positionWorld - origin.xyz);
    float3 magnitudeWorld = max(abs(positionWorld), abs(origin.xyz));
    float planeTolerance =
        LUDUS_MESH_LIGHT_PLANE_EPSILON_METERS +
        LUDUS_MESH_LIGHT_PLANE_RELATIVE_EPSILON * max(magnitudeWorld.x, max(magnitudeWorld.y, magnitudeWorld.z));
    if (min(uint(emission.w), MAX_MESH_LIGHT_POLYGON_CORNERS) < MESH_LIGHT_TRIANGLE_CORNER_COUNT ||
        abs(facing) <= planeTolerance || (plane.w == 0 && facing < 0))
    {
        return false;
    }
    if (cullThreshold <= 0)
    {
        return true;
    }
    float4 bound = meshLightWords[first + MESH_LIGHT_POLYGON_BOUND_WORD];
    float nearest = max(abs(facing), length(positionWorld - bound.xyz) - bound.w);
    return max(emission.r, max(emission.g, emission.b)) * origin.w >= cullThreshold * PI * nearest * nearest;
}
// The far-field evaluation of the record whose header starts at word first, which CanMeshLightRecordReach accepted:
// the point form factor at its bounding sphere's centre, with the specular LTC density in that direction. False when
// the record is too near, or too large for the receiver's specular lobe; EvaluateMeshLightRecord then integrates it.
bool TryEvaluateMeshLightFar(in MeshLightReceiver receiver, uint first, out float3 outContribution)
{
    outContribution = float3(0);
    float4 bound = meshLightWords[first + MESH_LIGHT_POLYGON_BOUND_WORD];
    float3 toLightWorld = bound.xyz - receiver.PositionWorld;
    float distance = length(toLightWorld);
    if (distance <= LUDUS_MESH_LIGHT_FAR_DISTANCE_RATIO * bound.w ||
        (receiver.HasSpecular == true &&
         bound.w >= LUDUS_MESH_LIGHT_FAR_SPECULAR_ANGLE_PER_ALPHA * receiver.SpecularAlpha * distance))
    {
        return false;
    }
    float4 plane = meshLightWords[first];
    float4 emission = meshLightWords[first + 1];
    float area = meshLightWords[first + MESH_LIGHT_POLYGON_HEADER_WORDS].w;
    float3 directionWorld = toLightWorld / distance;
    float emitterCosine = -dot(plane.xyz, directionWorld);
    float solidAngle = area * (plane.w != 0 ? abs(emitterCosine) : max(emitterCosine, 0)) / (distance * distance);
    float3 directionTangent = float3(dot(directionWorld, receiver.TangentWorld),
                                     dot(directionWorld, receiver.BitangentWorld),
                                     dot(directionWorld, receiver.NormalWorld));
    if (directionTangent.z <= 0)
    {
        return true;
    }
    float3 contribution = receiver.DiffuseAlbedo * (directionTangent.z * solidAngle / PI);
    if (receiver.HasSpecular == true)
    {
        contribution += receiver.SpecularAlbedo *
                        (EvaluateLinearlyTransformedCosine(receiver.Distribution, directionTangent) * solidAngle);
    }
    outContribution = emission.rgb * contribution;
    return true;
}
// Integrate the record whose header starts at word first; CanMeshLightRecordReach must hold.
float3 EvaluateMeshLightRecord(in MeshLightReceiver receiver, uint first)
{
    float4 emission = meshLightWords[first + 1];
    uint cornerCount = min(uint(emission.w), MAX_MESH_LIGHT_POLYGON_CORNERS);
    uint cornerWord = first + MESH_LIGHT_POLYGON_HEADER_WORDS;
    MeshLightStream stream = (MeshLightStream)0;
    float3 deltaWorld = meshLightWords[cornerWord].xyz - receiver.PositionWorld;
    float3 firstTangent = float3(dot(deltaWorld, receiver.TangentWorld),
                                 dot(deltaWorld, receiver.BitangentWorld),
                                 dot(deltaWorld, receiver.NormalWorld));
    float3 previousTangent = firstTangent;
    for (uint corner = 1; corner < cornerCount; ++corner)
    {
        deltaWorld = meshLightWords[cornerWord + corner].xyz - receiver.PositionWorld;
        float3 positionTangent = float3(dot(deltaWorld, receiver.TangentWorld),
                                        dot(deltaWorld, receiver.BitangentWorld),
                                        dot(deltaWorld, receiver.NormalWorld));
        ClipMeshLightReceiverEdge(stream,
                                  previousTangent,
                                  positionTangent,
                                  receiver.Distribution,
                                  receiver.HasSpecular);
        previousTangent = positionTangent;
    }
    ClipMeshLightReceiverEdge(stream, previousTangent, firstTangent, receiver.Distribution, receiver.HasSpecular);
    float3 contribution = receiver.DiffuseAlbedo * (CloseMeshLightEdgeSum(stream.Diffuse) / PI);
    if (receiver.HasSpecular == true)
    {
        if (stream.TransformedClip.Count > 0)
        {
            ClipMeshLightEdge(stream.TransformedClip.PreviousTangent,
                              stream.TransformedClip.FirstTangent,
                              stream.Specular);
        }
        contribution += receiver.SpecularAlbedo * (CloseMeshLightEdgeSum(stream.Specular) / PI);
    }
    return emission.rgb * contribution;
}
// The receiver's tangent frame, albedos and specular LTC, built once per sample that a record can reach.
MeshLightReceiver MakeMeshLightReceiver(in SurfaceSample surface, float3 positionWorld, float nv)
{
    MeshLightReceiver receiver = (MeshLightReceiver)0;
    receiver.PositionWorld = positionWorld;
    receiver.NormalWorld = surface.NormalWorld;
    float3 helperWorld =
        abs(surface.NormalWorld.z) < MESH_LIGHT_FRAME_AXIS_THRESHOLD ? float3(0, 0, 1) : float3(1, 0, 0);
    receiver.TangentWorld = NormalizeOrZero(cross(helperWorld, surface.NormalWorld));
    receiver.BitangentWorld = cross(surface.NormalWorld, receiver.TangentWorld);
    float3 viewTangent = float3(dot(surface.ViewDirectionWorld, receiver.TangentWorld),
                                dot(surface.ViewDirectionWorld, receiver.BitangentWorld),
                                nv);
    float3 f0 = lerp(float3(MESH_LIGHT_DIELECTRIC_F0), surface.BaseColor.rgb, surface.Metallic);
    float3 fresnel = f0 + (1 - f0) * ComputeFifth(1 - nv);
    receiver.DiffuseAlbedo = surface.BaseColor.rgb * (1 - surface.Metallic) * (1 - fresnel);
    SpecularLtc specular = LookupSpecularLtc(surface.Roughness, viewTangent, f0);
    receiver.HasSpecular = TryMakeLinearlyTransformedCosine(specular.InverseTransform, receiver.Distribution);
    receiver.SpecularAlbedo = ComputeSpecularLtcAlbedo(specular, f0);
    receiver.SpecularAlpha = surface.Roughness * surface.Roughness;
    return receiver;
}
// Entry `entry` of a culling grid list (deferred_layout.h): the header word of one listed record.
uint GetMeshLightGridEntry(uint entryWord, uint entry)
{
    float4 packed = meshLightWords[entryWord + entry / MESH_LIGHT_GRID_ENTRIES_PER_WORD];
    uint lane = entry % MESH_LIGHT_GRID_ENTRIES_PER_WORD;
    return uint(lane == 0 ? packed.x : (lane == 1 ? packed.y : (lane == 2 ? packed.z : packed.w)));
}
// Sum every record's integral. With a culling grid (gridWord nonzero), a sample reads only its cell's list, which
// omits records whose bounded Lambert term stays below the grid's cull radiance there (raster_mesh_light_grid.cpp),
// and skips listed records whose bound at the sample is below it too. Every shaded surface lies inside the grid, so a
// position reconstructed outside it reads the nearest boundary cell. Evaluating every record there instead made a
// few floor pixels walk the whole record list and stall the pass.
float3 EvaluateRasterMeshLights(in SurfaceSample surface, in float3 positionWorld, uint polygonCount, uint gridWord)
{
    float nv = max(dot(surface.NormalWorld, surface.ViewDirectionWorld), 0);
    if (nv <= 0 || polygonCount == 0)
    {
        return float3(0);
    }
    uint firstEntry = 0;
    uint entryCount = polygonCount;
    uint entryWord = 0;
    float cullThreshold = 0;
    if (gridWord != 0)
    {
        float4 origin = meshLightWords[gridWord];
        float4 cells = meshLightWords[gridWord + 1];
        // The float clamp handles finite positions; the integer one bounds a nonfinite position's conversion, which
        // fast math leaves unspecified.
        uint3 lastCell = uint3(cells.xyz) - 1;
        uint3 cell =
            min(uint3(clamp(floor((positionWorld - origin.xyz) / origin.w), float3(0), cells.xyz - 1)), lastCell);
        uint index = cell.x + uint(cells.x) * (cell.y + uint(cells.y) * cell.z);
        float4 pair = meshLightWords[gridWord + MESH_LIGHT_GRID_HEADER_WORDS + index / MESH_LIGHT_GRID_CELLS_PER_WORD];
        bool isSecond = index % MESH_LIGHT_GRID_CELLS_PER_WORD != 0;
        firstEntry = uint(isSecond ? pair.z : pair.x);
        entryCount = uint(isSecond ? pair.w : pair.y);
        entryWord = uint(cells.w);
        cullThreshold = meshLightWords[gridWord + MESH_LIGHT_GRID_CULL_WORD].x;
        if (entryCount == 0)
        {
            return float3(0);
        }
    }
    // The receiver terms cost a table lookup and a matrix inverse; most samples reach no listed record.
    MeshLightReceiver receiver = (MeshLightReceiver)0;
    bool hasReceiver = false;
    float3 result = float3(0);
    uint first = 0;
    for (uint entry = 0; entry < entryCount; ++entry)
    {
        uint record = first;
        if (entryWord != 0)
        {
            record = GetMeshLightGridEntry(entryWord, firstEntry + entry);
        }
        else
        {
            first += MESH_LIGHT_POLYGON_HEADER_WORDS +
                     min(uint(meshLightWords[first + 1].w), MAX_MESH_LIGHT_POLYGON_CORNERS);
        }
        if (CanMeshLightRecordReach(record, positionWorld, cullThreshold) == false)
        {
            continue;
        }
        if (hasReceiver == false)
        {
            receiver = MakeMeshLightReceiver(surface, positionWorld, nv);
            hasReceiver = true;
        }
        float3 farContribution;
        if (TryEvaluateMeshLightFar(receiver, record, farContribution) == true)
        {
            result += farContribution;
        }
        else
        {
            result += EvaluateMeshLightRecord(receiver, record);
        }
    }
    return max(result, float3(0));
}
