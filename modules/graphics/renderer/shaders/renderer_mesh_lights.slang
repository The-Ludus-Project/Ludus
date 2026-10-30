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
float3 EvaluateRasterMeshLights(in SurfaceSample surface, in float3 positionWorld, uint polygonCount)
{
    float nv = max(dot(surface.NormalWorld, surface.ViewDirectionWorld), 0);
    if (nv <= 0 || polygonCount == 0)
    {
        return float3(0);
    }
    float3 helperWorld =
        abs(surface.NormalWorld.z) < MESH_LIGHT_FRAME_AXIS_THRESHOLD ? float3(0, 0, 1) : float3(1, 0, 0);
    float3 tangentWorld = NormalizeOrZero(cross(helperWorld, surface.NormalWorld));
    float3 bitangentWorld = cross(surface.NormalWorld, tangentWorld);
    float3 viewTangent =
        float3(dot(surface.ViewDirectionWorld, tangentWorld), dot(surface.ViewDirectionWorld, bitangentWorld), nv);
    float3 f0 = lerp(float3(MESH_LIGHT_DIELECTRIC_F0), surface.BaseColor.rgb, surface.Metallic);
    float3 fresnel = f0 + (1 - f0) * ComputeFifth(1 - nv);
    float3 diffuseAlbedo = surface.BaseColor.rgb * (1 - surface.Metallic) * (1 - fresnel);
    SpecularLtc specular = LookupSpecularLtc(surface.Roughness, viewTangent, f0);
    LinearlyTransformedCosine distribution;
    bool hasSpecular = TryMakeLinearlyTransformedCosine(specular.InverseTransform, distribution);
    float3 specularAlbedo = ComputeSpecularLtcAlbedo(specular, f0);
    float3 result = float3(0);
    uint first = 0;
    for (uint light = 0; light < polygonCount; ++light)
    {
        float4 plane = meshLightWords[first];
        float4 emission = meshLightWords[first + 1];
        uint cornerCount = min(uint(emission.w), MAX_MESH_LIGHT_POLYGON_CORNERS);
        uint cornerWord = first + MESH_LIGHT_POLYGON_HEADER_WORDS;
        first = cornerWord + cornerCount;
        float3 originWorld = meshLightWords[cornerWord].xyz;
        float facing = dot(plane.xyz, positionWorld - originWorld);
        float3 magnitudeWorld = max(abs(positionWorld), abs(originWorld));
        float planeTolerance =
            LUDUS_MESH_LIGHT_PLANE_EPSILON_METERS +
            LUDUS_MESH_LIGHT_PLANE_RELATIVE_EPSILON * max(magnitudeWorld.x, max(magnitudeWorld.y, magnitudeWorld.z));
        if (cornerCount < MESH_LIGHT_TRIANGLE_CORNER_COUNT || abs(facing) <= planeTolerance ||
            (plane.w == 0 && facing < 0))
        {
            continue;
        }
        MeshLightStream stream = (MeshLightStream)0;
        float3 deltaWorld = originWorld - positionWorld;
        float3 firstTangent = float3(dot(deltaWorld, tangentWorld),
                                     dot(deltaWorld, bitangentWorld),
                                     dot(deltaWorld, surface.NormalWorld));
        float3 previousTangent = firstTangent;
        for (uint corner = 1; corner < cornerCount; ++corner)
        {
            deltaWorld = meshLightWords[cornerWord + corner].xyz - positionWorld;
            float3 positionTangent = float3(dot(deltaWorld, tangentWorld),
                                            dot(deltaWorld, bitangentWorld),
                                            dot(deltaWorld, surface.NormalWorld));
            ClipMeshLightReceiverEdge(stream, previousTangent, positionTangent, distribution, hasSpecular);
            previousTangent = positionTangent;
        }
        ClipMeshLightReceiverEdge(stream, previousTangent, firstTangent, distribution, hasSpecular);
        float3 contribution = diffuseAlbedo * (CloseMeshLightEdgeSum(stream.Diffuse) / PI);
        if (hasSpecular == true)
        {
            if (stream.TransformedClip.Count > 0)
            {
                ClipMeshLightEdge(stream.TransformedClip.PreviousTangent,
                                  stream.TransformedClip.FirstTangent,
                                  stream.Specular);
            }
            contribution += specularAlbedo * (CloseMeshLightEdgeSum(stream.Specular) / PI);
        }
        result += emission.rgb * contribution;
    }
    return max(result, float3(0));
}
