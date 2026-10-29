#pragma once
#include "linearly_transformed_cosine.slang"

// Thanks to Heitz, Dupuy, Hill and Neubelt, "Real-Time Polygonal-Light Shading
// with Linearly Transformed Cosines", TOG 35(4), Article 41 (2016), section 3,
// https://doi.org/10.1145/2897824.2925895. The CPU check is raster_mesh_lights.cpp.
// Integrate real triangles without point proxies, finite-range cutoffs or visibility.
static const uint MESH_LIGHT_CORNER_COUNT = 3;
static const float MESH_LIGHT_DIELECTRIC_F0 = 0.04;
static const float MESH_LIGHT_FRAME_AXIS_THRESHOLD = 0.9;
float3 EvaluateRasterMeshLights(in SurfaceSample surface, in float3 positionWorld, uint triangleCount)
{
    float nv = max(dot(surface.NormalWorld, surface.ViewDirectionWorld), 0);
    if (nv <= 0 || triangleCount == 0)
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
    for (uint triangle = 0; triangle < triangleCount; ++triangle)
    {
        uint first = triangle * MESH_LIGHT_TRIANGLE_WORDS;
        float4 a = meshLightWords[first];
        float4 b = meshLightWords[first + 1];
        float4 c = meshLightWords[first + 2];
        float4 emission = meshLightWords[first + MESH_LIGHT_CORNER_COUNT];
        float3 emitterNormalWorld = float3(a.w, b.w, c.w);
        float facing = dot(emitterNormalWorld, positionWorld - a.xyz);
        float3 magnitudeWorld = max(abs(positionWorld), abs(a.xyz));
        float planeTolerance =
            LUDUS_MESH_LIGHT_PLANE_EPSILON_METERS +
            LUDUS_MESH_LIGHT_PLANE_RELATIVE_EPSILON * max(magnitudeWorld.x, max(magnitudeWorld.y, magnitudeWorld.z));
        if (abs(facing) <= planeTolerance || (emission.w == 0 && facing < 0))
        {
            continue;
        }
        ProjectedPolygonInput polygon = (ProjectedPolygonInput)0;
        polygon.VertexCount = MESH_LIGHT_CORNER_COUNT;
        float3 deltaWorld = a.xyz - positionWorld;
        polygon.PositionsTangent[0] = float3(dot(deltaWorld, tangentWorld),
                                             dot(deltaWorld, bitangentWorld),
                                             dot(deltaWorld, surface.NormalWorld));
        deltaWorld = b.xyz - positionWorld;
        polygon.PositionsTangent[1] = float3(dot(deltaWorld, tangentWorld),
                                             dot(deltaWorld, bitangentWorld),
                                             dot(deltaWorld, surface.NormalWorld));
        deltaWorld = c.xyz - positionWorld;
        polygon.PositionsTangent[2] = float3(dot(deltaWorld, tangentWorld),
                                             dot(deltaWorld, bitangentWorld),
                                             dot(deltaWorld, surface.NormalWorld));
        float3 heightsTangent = float3(polygon.PositionsTangent[0].z,
                                         polygon.PositionsTangent[1].z,
                                         polygon.PositionsTangent[2].z);
        if (all(heightsTangent <= 0) == true)
        {
            continue;
        }
        bool isAboveHorizon = all(heightsTangent >= 0);
        if (isAboveHorizon == false)
        {
            ProjectedPolygonInput clipped = (ProjectedPolygonInput)0;
            if (TryClipProjectedPolygonToUpperHemisphere(polygon, clipped) == false ||
                clipped.VertexCount < MESH_LIGHT_CORNER_COUNT)
            {
                continue;
            }
            polygon = clipped;
        }
        float diffuseIntegral =
            isAboveHorizon == true
                ? ComputeProjectedTriangleIntegral(polygon.PositionsTangent[0],
                                                     polygon.PositionsTangent[1],
                                                     polygon.PositionsTangent[2])
                : ComputeProjectedSolidAngleIntegral(polygon);
        float3 contribution = diffuseAlbedo * (diffuseIntegral / PI);
        if (hasSpecular == true)
        {
            ProjectedPolygonInput transformed = (ProjectedPolygonInput)0;
            transformed.VertexCount = polygon.VertexCount;
            [unroll] for (uint corner = 0; corner < MAX_PROJECTED_POLYGON_INPUT_VERTICES; ++corner)
            {
                if (corner < polygon.VertexCount)
                {
                    transformed.PositionsTangent[corner] =
                        MultiplyColumns(distribution.InverseTransform, polygon.PositionsTangent[corner]);
                }
            }
            float specularIntegral =
                isAboveHorizon == true
                    ? ComputeProjectedTriangleIntegral(transformed.PositionsTangent[0],
                                                         transformed.PositionsTangent[1],
                                                         transformed.PositionsTangent[2])
                    : ComputeProjectedSolidAngleIntegral(transformed);
            contribution += specularAlbedo * (specularIntegral / PI);
        }
        result += emission.rgb * contribution;
    }
    return max(result, float3(0));
}
