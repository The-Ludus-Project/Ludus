#pragma once
#include "linearly_transformed_cosine.slang"

// Thanks to Heitz, Dupuy, Hill and Neubelt, "Real-Time Polygonal-Light Shading
// with Linearly Transformed Cosines", TOG 35(4), Article 41 (2016), section 3,
// https://doi.org/10.1145/2897824.2925895. The CPU check is raster_mesh_lights.cpp.
// Integrate real convex emitter polygons without point proxies or finite-range cutoffs. The
// host merges coplanar constant-emission triangles into these records; both integrals are
// additive over that partition. Visibility is a per-caster factor from cached cube shadow
// faces (ComputeMeshLightShadowVisibility), applied to the whole unshadowed integral.
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
        inoutSum.Integral += ComputeRasterProjectedSolidAngleEdge(inoutSum.PreviousTangent, directionTangent);
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
    float integral = sum.Integral + ComputeRasterProjectedSolidAngleEdge(sum.PreviousTangent, sum.FirstTangent);
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
// Cube shadow faces of the mesh-light shadow casters (raster_mesh_light_shadows.h). Each face stores, as 16 bits in
// red and green, the conventional depth of the nearest occluder seen from its caster; the face bases repeat
// FACE_BASES in raster_mesh_light_shadows.cpp, which tests them against math::TryLookAt.
// Thanks to William T. Reeves, David H. Salesin and Robert L. Cook, "Rendering Antialiased Shadows with Depth Maps",
// SIGGRAPH 1987, pp. 283-291, https://doi.org/10.1145/37401.37435, for filtering a receiver's depth comparison over
// nearby map samples, and to Randima Fernando, "Percentage-Closer Soft Shadows", ACM SIGGRAPH 2005 Sketches, p. 35,
// https://doi.org/10.1145/1187112.1187153, for widening that filter by the penumbra a light of the caster's radius
// casts from the average blocker depth. Here the filter is clamped to each face's gutter.
static const float3 MESH_LIGHT_SHADOW_FORWARD[MESH_LIGHT_SHADOW_FACE_COUNT] =
    {float3(1, 0, 0), float3(-1, 0, 0), float3(0, 1, 0), float3(0, -1, 0), float3(0, 0, 1), float3(0, 0, -1)};
static const float3 MESH_LIGHT_SHADOW_RIGHT[MESH_LIGHT_SHADOW_FACE_COUNT] =
    {float3(0, 0, 1), float3(0, 0, -1), float3(-1, 0, 0), float3(-1, 0, 0), float3(-1, 0, 0), float3(1, 0, 0)};
static const float3 MESH_LIGHT_SHADOW_UP[MESH_LIGHT_SHADOW_FACE_COUNT] =
    {float3(0, 1, 0), float3(0, 1, 0), float3(0, 0, -1), float3(0, 0, 1), float3(0, 1, 0), float3(0, 1, 0)};
// Blocker search and filter taps: the centre and the corners, then a ring of eight, in units of the filter radius.
static const uint MESH_LIGHT_SHADOW_BLOCKER_TAP_COUNT = 5;
static const uint MESH_LIGHT_SHADOW_FILTER_TAP_COUNT = 9;
static const float2 MESH_LIGHT_SHADOW_BLOCKER_TAPS[MESH_LIGHT_SHADOW_BLOCKER_TAP_COUNT] = {float2(0, 0),
                                                                                           float2(-1, -1),
                                                                                           float2(1, -1),
                                                                                           float2(-1, 1),
                                                                                           float2(1, 1)};
static const float2 MESH_LIGHT_SHADOW_FILTER_TAPS[MESH_LIGHT_SHADOW_FILTER_TAP_COUNT] = {float2(0, 0),
                                                                                         float2(1, 0),
                                                                                         float2(0.7071068, 0.7071068),
                                                                                         float2(0, 1),
                                                                                         float2(-0.7071068, 0.7071068),
                                                                                         float2(-1, 0),
                                                                                         float2(-0.7071068, -0.7071068),
                                                                                         float2(0, -1),
                                                                                         float2(0.7071068, -0.7071068)};
// One face lookup: the slot, the projection and the receiver's tangent plane relative to the caster.
struct MeshLightShadowFace
{
    float2 Origin;
    float2 Maximum;
    float Texels;
    float Tangent;
    float NearMeters;
    float FarMeters;
    float3 ForwardWorld;
    float3 RightWorld;
    float3 UpWorld;
    float3 NormalWorld;
    // dot(normal, caster - receiver): the caster's height above the receiver's plane.
    float PlaneHeight;
    // How far the receiver's plane may lie from the surface along its normal, in metres: the normal component of
    // the uncertainty of a receiver rebuilt from depth.
    float PlaneUncertainty;
};
// Conventional depth of a forward distance in the face projection.
float ComputeMeshLightShadowDepth(in MeshLightShadowFace face, float forward)
{
    return face.FarMeters * (forward - face.NearMeters) / (forward * (face.FarMeters - face.NearMeters));
}
// The stored occluder depth at a texel, clamped into the face slot, and the depth the receiver's tangent plane has
// along that texel's ray: comparing with the plane rather than with the receiver point keeps a flat receiver from
// shadowing itself where its depth changes across the filter. The plane is taken as near the caster as the
// receiver's uncertainty allows, which widens the comparison by the depth that displacement spans along the ray.
void ReadMeshLightShadowTexel(in MeshLightShadowFace face, float2 texel, out float outStored, out float outPlane)
{
    float2 position = clamp(floor(texel), face.Origin, face.Maximum);
    float2 code = meshLightShadowAtlas.Load(int3(int2(position), 0)).rg;
    outStored = DecodeShadowDepth(code);
    float2 uv = (position + 0.5 - face.Origin) / face.Texels;
    float2 ndc = float2(uv.x * 2 - 1, 1 - uv.y * 2);
    float3 directionWorld = face.ForwardWorld + (face.RightWorld * ndc.x + face.UpWorld * ndc.y) * face.Tangent;
    float along = -dot(face.NormalWorld, directionWorld);
    // A ray that never meets the plane in front of the caster sees no receiver there: nothing can shadow it.
    float forward = along > 0 ? max(face.PlaneHeight - face.PlaneUncertainty, 0) / along : face.FarMeters;
    outPlane = ComputeMeshLightShadowDepth(face, clamp(forward, face.NearMeters, face.FarMeters)) -
               LUDUS_MESH_LIGHT_SHADOW_DEPTH_BIAS_CODES / SHADOW_DEPTH_CODE_MAXIMUM;
}
// Fraction of the caster's view of the receiver that no occluder blocks; one without a caster or shadow table.
// positionUncertaintyWorld is how far positionWorld may lie from the surface, zero for an exact position.
float ComputeMeshLightShadowVisibility(uint tableWord,
                                       uint casterKey,
                                       float3 positionWorld,
                                       float3 positionUncertaintyWorld,
                                       float3 normalWorld)
{
    if (tableWord == 0 || casterKey == 0)
    {
        return 1;
    }
    uint casterWord = tableWord + MESH_LIGHT_SHADOW_HEADER_WORDS + (casterKey - 1) * MESH_LIGHT_SHADOW_CASTER_WORDS;
    float4 placement = meshLightWords[casterWord];
    float4 shape = meshLightWords[casterWord + 1];
    MeshLightShadowFace face;
    face.FarMeters = placement.w;
    face.NearMeters = shape.y * placement.w;
    face.Texels = shape.z;
    face.Tangent = shape.w;
    face.NormalWorld = normalWorld;
    // One texel spans 2 * tangent * forward / texels metres at forward distance; the distance bounds it.
    float texelMeters = 2 * face.Tangent * length(positionWorld - placement.xyz) / face.Texels;
    float3 deltaWorld =
        positionWorld + normalWorld * (LUDUS_MESH_LIGHT_SHADOW_NORMAL_OFFSET_TEXELS * texelMeters) - placement.xyz;
    face.PlaneHeight = -dot(normalWorld, deltaWorld);
    // A receiver rebuilt from conventional depth lies centimetres off its surface tens of metres from the camera.
    // Behind the surface, its plane would meet the receiver's own stored depth first and shadow it in a pattern that
    // changes with every pixel and camera pose.
    face.PlaneUncertainty = abs(dot(normalWorld, positionUncertaintyWorld));
    float3 magnitude = abs(deltaWorld);
    uint axis = magnitude.x >= magnitude.y && magnitude.x >= magnitude.z ? 0 : (magnitude.y >= magnitude.z ? 1 : 2);
    float component = axis == 0 ? deltaWorld.x : (axis == 1 ? deltaWorld.y : deltaWorld.z);
    uint index = 2 * axis + (component < 0 ? 1 : 0);
    float4 origins = meshLightWords[casterWord + 2 + index / 2];
    face.Origin = index % 2 == 0 ? origins.xy : origins.zw;
    face.Maximum = face.Origin + face.Texels - 1;
    face.ForwardWorld = MESH_LIGHT_SHADOW_FORWARD[index];
    face.RightWorld = MESH_LIGHT_SHADOW_RIGHT[index];
    face.UpWorld = MESH_LIGHT_SHADOW_UP[index];
    float forward = dot(deltaWorld, face.ForwardWorld);
    // A receiver facing away from the caster, inside its near distance or beyond its range is left unshadowed.
    if (face.Origin.x < 0 || face.PlaneHeight <= 0 || forward <= face.NearMeters || forward >= face.FarMeters)
    {
        return 1;
    }
    float2 ndc = float2(dot(deltaWorld, face.RightWorld), dot(deltaWorld, face.UpWorld)) / (forward * face.Tangent);
    float2 texel = face.Origin + float2(ndc.x * 0.5 + 0.5, 0.5 - ndc.y * 0.5) * face.Texels;
    // Average depth of the occluders around the receiver.
    float blockerSum = 0;
    float blockerCount = 0;
    for (uint tap = 0; tap < MESH_LIGHT_SHADOW_BLOCKER_TAP_COUNT; ++tap)
    {
        float stored;
        float plane;
        ReadMeshLightShadowTexel(face,
                                 texel + MESH_LIGHT_SHADOW_BLOCKER_TAPS[tap] * MESH_LIGHT_SHADOW_GUTTER_TEXELS * 0.5,
                                 stored,
                                 plane);
        if (stored < plane)
        {
            blockerSum += stored;
            ++blockerCount;
        }
    }
    if (blockerCount == 0)
    {
        return 1;
    }
    // The blocker's forward distance inverts the depth mapping; its penumbra scales with the light radius.
    float blockerDepth = blockerSum / blockerCount;
    float blockerForward =
        face.NearMeters * face.FarMeters / (face.FarMeters - blockerDepth * (face.FarMeters - face.NearMeters));
    float penumbraMeters = shape.x * max(forward - blockerForward, 0) / max(blockerForward, face.NearMeters);
    float radius =
        clamp(penumbraMeters / (2 * face.Tangent * forward / face.Texels), 1, MESH_LIGHT_SHADOW_GUTTER_TEXELS);
    float lit = 0;
    for (uint tap = 0; tap < MESH_LIGHT_SHADOW_FILTER_TAP_COUNT; ++tap)
    {
        float stored;
        float plane;
        ReadMeshLightShadowTexel(face, texel + MESH_LIGHT_SHADOW_FILTER_TAPS[tap] * radius, stored, plane);
        lit += stored < plane ? 0 : 1;
    }
    return lit / MESH_LIGHT_SHADOW_FILTER_TAP_COUNT;
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
// few floor pixels walk the whole record list and stall the pass. positionUncertaintyWorld is how far positionWorld may
// lie from the surface along the view ray; the shadow comparison allows for it.
float3 EvaluateRasterMeshLights(in SurfaceSample surface,
                                in float3 positionWorld,
                                in float3 positionUncertaintyWorld,
                                uint polygonCount,
                                uint gridWord)
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
    uint shadowTableWord = 0;
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
        float4 cull = meshLightWords[gridWord + MESH_LIGHT_GRID_CULL_WORD];
        cullThreshold = cull.x;
        shadowTableWord = uint(cull.y);
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
    // Each cell lists a caster's records together, so one visibility serves a run of them.
    uint visibleCaster = ~0u;
    float visibility = 1;
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
        float3 contribution;
        if (TryEvaluateMeshLightFar(receiver, record, contribution) == false)
        {
            contribution = EvaluateMeshLightRecord(receiver, record);
        }
        if (any(contribution > 0) == false)
        {
            continue;
        }
        uint casterKey =
            uint(meshLightWords[record + MESH_LIGHT_POLYGON_HEADER_WORDS + MESH_LIGHT_SHADOW_CASTER_CORNER].w);
        if (casterKey != visibleCaster)
        {
            visibility = ComputeMeshLightShadowVisibility(shadowTableWord,
                                                          casterKey,
                                                          positionWorld,
                                                          positionUncertaintyWorld,
                                                          surface.NormalWorld);
            visibleCaster = casterKey;
        }
        result += contribution * visibility;
    }
    return max(result, float3(0));
}
