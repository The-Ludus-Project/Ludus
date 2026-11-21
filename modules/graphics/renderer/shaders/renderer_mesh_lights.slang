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
// One record's two integrals before its emission scales them; the record's radiance is
// Emission * (Diffuse + Specular), in that order.
struct MeshLightLobes
{
    float3 Diffuse;
    float3 Specular;
    float3 Emission;
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
bool TryEvaluateMeshLightFar(in MeshLightReceiver receiver, uint first, out MeshLightLobes outLobes)
{
    outLobes = (MeshLightLobes)0;
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
    outLobes.Diffuse = receiver.DiffuseAlbedo * (directionTangent.z * solidAngle / PI);
    if (receiver.HasSpecular == true)
    {
        outLobes.Specular = receiver.SpecularAlbedo *
                            (EvaluateLinearlyTransformedCosine(receiver.Distribution, directionTangent) * solidAngle);
    }
    outLobes.Emission = emission.rgb;
    return true;
}
// Integrate the record whose header starts at word first; CanMeshLightRecordReach must hold.
MeshLightLobes EvaluateMeshLightRecord(in MeshLightReceiver receiver, uint first)
{
    MeshLightLobes lobes = (MeshLightLobes)0;
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
    lobes.Diffuse = receiver.DiffuseAlbedo * (CloseMeshLightEdgeSum(stream.Diffuse) / PI);
    if (receiver.HasSpecular == true)
    {
        if (stream.TransformedClip.Count > 0)
        {
            ClipMeshLightEdge(stream.TransformedClip.PreviousTangent,
                              stream.TransformedClip.FirstTangent,
                              stream.Specular);
        }
        lobes.Specular = receiver.SpecularAlbedo * (CloseMeshLightEdgeSum(stream.Specular) / PI);
    }
    lobes.Emission = emission.rgb;
    return lobes;
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
// red and green, the linear depth (forward - near) / (far - near) of the nearest occluder seen from its caster, so a
// code spans the same distance everywhere in the face and the near plane can sit just in front of the emitters. The
// face bases repeat FACE_BASES in raster_mesh_light_shadows.cpp, which tests them against math::TryLookAt.
// Thanks to William T. Reeves, David H. Salesin and Robert L. Cook, "Rendering Antialiased Shadows with Depth Maps",
// SIGGRAPH 1987, pp. 283-291, https://doi.org/10.1145/37401.37435, for filtering a receiver's depth comparison over
// nearby map samples, and to Randima Fernando, "Percentage-Closer Soft Shadows", ACM SIGGRAPH 2005 Sketches, p. 35,
// https://doi.org/10.1145/1187112.1187153, for widening that filter by the penumbra a light of the caster's radius
// casts from the depth of the blockers found around the receiver.
//
// The light is a disk of the caster's disk radius (MESH_LIGHT_SHADOW_DISK_WORDS): across a one-sided caster's emitter
// normal, else facing the receiver. The segment from the receiver to the light point at radius * u (u in the unit
// disk) reaches height z above the disk's plane where the caster sees the direction
// receiver + radius * u * (height / z - 1), receiver and height being the receiver's offset from the caster and its
// height above the plane. Every tap is such a direction, looked up on whichever cube face holds it, so the search and
// the filter cross face seams and widen with the light instead of stopping at the four-texel face gutter. An occluder
// behind the plane, such as the emitter itself, hides nothing. Thanks to Gael Guennebaud, Loic Barthe and Mathias
// Paulin, "Real-time Soft Shadow Mapping by Backprojection", Eurographics Symposium on Rendering 2006, pp. 227-234,
// https://doi.org/10.2312/EGWR/EGSR06/227-234, for backprojecting shadow-map samples from the receiver onto the light:
// here it only decides whether a found occluder can hide part of the light, not how much. Where the found occluders lie
// at two clearly different heights, each light point is tested at both, and an occluder counts only in its own height
// band, so a blocker near the light keeps its wide penumbra beside a sharp one near the receiver. Each light point's
// visibility is weighted by its diffuse form factor at the receiver, as the exact integral weights it.
//
// The taps are a golden-angle disk (Vogel, GOLDEN_ANGLE_RADIANS) rotated per pixel by Jorge Jimenez's interleaved
// gradient noise, "Next Generation Post Processing in Call of Duty: Advanced Warfare", SIGGRAPH 2014 Advances in
// Real-Time Rendering course, https://www.iryoku.com/next-generation-post-processing-in-call-of-duty-advanced-warfare,
// which turns the steps a fixed pattern leaves across a wide penumbra into a fine dither.
static const float3 MESH_LIGHT_SHADOW_FORWARD[MESH_LIGHT_SHADOW_FACE_COUNT] =
    {float3(1, 0, 0), float3(-1, 0, 0), float3(0, 1, 0), float3(0, -1, 0), float3(0, 0, 1), float3(0, 0, -1)};
static const float3 MESH_LIGHT_SHADOW_RIGHT[MESH_LIGHT_SHADOW_FACE_COUNT] =
    {float3(0, 0, 1), float3(0, 0, -1), float3(-1, 0, 0), float3(-1, 0, 0), float3(-1, 0, 0), float3(1, 0, 0)};
static const float3 MESH_LIGHT_SHADOW_UP[MESH_LIGHT_SHADOW_FACE_COUNT] =
    {float3(0, 1, 0), float3(0, 1, 0), float3(0, 0, -1), float3(0, 0, 1), float3(0, 1, 0), float3(0, 1, 0)};
// Search the receiver direction, then wide rings for blockers near the light and compact rings for
// narrow projections near the receiver direction. Filter the disk once per discovered height layer.
static const uint MESH_LIGHT_SHADOW_WIDE_RING_COUNT = 4;
static const uint MESH_LIGHT_SHADOW_COMPACT_RING_COUNT = 2;
static const uint MESH_LIGHT_SHADOW_SEARCH_RING_TAP_COUNT = 3;
static const uint MESH_LIGHT_SHADOW_FILTER_TAP_COUNT = 12;
// Largest lateral width over the receiver's distance, about the tangent of a tap's angle from the receiver's direction
// (63 degrees): an occluder nearer the light's plane than about half its radius is searched and filtered as if it lay
// farther, so its penumbra is too narrow.
static const float MESH_LIGHT_SHADOW_MAXIMUM_SPREAD = 2;
// The found occluders form two height layers when the lower one's lateral width exceeds the higher one's by this
// factor and by a texel: a light point's segment crosses them in directions the other layer's width would misplace.
static const float MESH_LIGHT_SHADOW_LAYER_SPREAD_RATIO = 1.5;
// A light whose widest penumbra at the receiver spans at most this many shadow texels searches only the receiver's own
// direction: its rings would read the same few texels.
static const float MESH_LIGHT_SHADOW_NARROW_TEXELS = 2;
// Interleaved gradient noise: frac(SCALE * frac(dot(pixel, WEIGHTS))), Jimenez 2014.
static const float2 MESH_LIGHT_SHADOW_NOISE_WEIGHTS = float2(0.06711056, 0.00583715);
static const float MESH_LIGHT_SHADOW_NOISE_SCALE = 52.9829189;
// One caster's lookup: its face slots and projection, and the receiver's tangent plane relative to the caster.
struct MeshLightShadowView
{
    // The atlas origins of faces (+X, -X), (+Y, -Y) and (+Z, -Z); negative for a face the caster does not render.
    float4 OriginsX;
    float4 OriginsY;
    float4 OriginsZ;
    float Texels;
    float Tangent;
    float NearMeters;
    float FarMeters;
    float InverseRange;
    // LUDUS_MESH_LIGHT_SHADOW_RASTER_DEPTH_ERROR / (near * range): a stored depth rebuilt from rasterized float32
    // depth is uncertain by this times forward^2.
    float RebuiltErrorScale;
    float3 NormalWorld;
    // dot(normal, caster - receiver), the caster's height above the receiver's plane, less how far that plane may
    // lie from the surface along its normal: the normal component of the uncertainty of a receiver rebuilt from depth.
    float PlaneReach;
};
// Two interleaved gradient noise values in [0, 1) of a pixel position: of the pixel and of its transpose. The second
// is not a shifted copy of the first, as the noise of a translated pixel would be.
float2 ComputeMeshLightShadowNoise(float2 positionPixel)
{
    float2 pixel = floor(positionPixel);
    return frac(MESH_LIGHT_SHADOW_NOISE_SCALE * frac(float2(dot(pixel, MESH_LIGHT_SHADOW_NOISE_WEIGHTS),
                                                            dot(pixel.yx, MESH_LIGHT_SHADOW_NOISE_WEIGHTS))));
}
// The cube face whose frustum holds a nonzero direction from the caster, and the direction's component along it.
uint GetMeshLightShadowFace(float3 directionWorld, out float outForward)
{
    float3 magnitude = abs(directionWorld);
    uint axis = magnitude.x >= magnitude.y && magnitude.x >= magnitude.z ? 0 : (magnitude.y >= magnitude.z ? 1 : 2);
    float component = axis == 0 ? directionWorld.x : (axis == 1 ? directionWorld.y : directionWorld.z);
    outForward = abs(component);
    return 2 * axis + (component < 0 ? 1 : 0);
}
// The atlas origin of a face of the view's caster, negative when the caster does not render it.
float2 GetMeshLightShadowOrigin(in MeshLightShadowView view, uint face)
{
    float4 origins = face < 2 ? view.OriginsX : (face < 4 ? view.OriginsY : view.OriginsZ);
    return face % 2 == 0 ? origins.xy : origins.zw;
}
// A 2D direction turned by the angle whose cosine and sine are `turn`.
float2 TurnMeshLightShadowTap(float2 direction, float2 turn)
{
    return float2(direction.x * turn.x - direction.y * turn.y, direction.x * turn.y + direction.y * turn.x);
}
// The occluder the caster sees in a nonzero direction, on whichever face holds it: whether it lies in front of the
// receiver's tangent plane along that texel's ray, and its offset from the caster, the texel's ray to its stored
// depth. Comparing with the plane rather than with the receiver point keeps a flat receiver from shadowing itself
// where its depth changes across the filter. The plane is taken as near the caster as the receiver's uncertainty
// allows, which widens the comparison by the depth that displacement spans along the ray. A face the caster does not
// render, behind its one-sided emitters, holds no occluder.
bool ReadMeshLightShadowDirection(in MeshLightShadowView view, float3 directionWorld, out float3 outOccluderWorld)
{
    outOccluderWorld = float3(0);
    float forward;
    uint face = GetMeshLightShadowFace(directionWorld, forward);
    float2 origin = GetMeshLightShadowOrigin(view, face);
    if (origin.x < 0)
    {
        return false;
    }
    float3 forwardWorld = MESH_LIGHT_SHADOW_FORWARD[face];
    float3 rightWorld = MESH_LIGHT_SHADOW_RIGHT[face];
    float3 upWorld = MESH_LIGHT_SHADOW_UP[face];
    float2 ndc = float2(dot(directionWorld, rightWorld), dot(directionWorld, upWorld)) / (forward * view.Tangent);
    float2 texel = origin + float2(ndc.x * 0.5 + 0.5, 0.5 - ndc.y * 0.5) * view.Texels;
    float2 position = clamp(floor(texel), origin, origin + view.Texels - 1);
    float stored = DecodeShadowDepth(meshLightShadowAtlas.Load(int3(int2(position), 0)).rg);
    float2 uv = (position + 0.5 - origin) / view.Texels;
    float2 texelNdc = float2(uv.x * 2 - 1, 1 - uv.y * 2);
    float3 texelDirectionWorld = forwardWorld + (rightWorld * texelNdc.x + upWorld * texelNdc.y) * view.Tangent;
    float along = -dot(view.NormalWorld, texelDirectionWorld);
    // A ray that never meets the plane in front of the caster sees no receiver there: nothing can shadow it.
    float planeForward = along > 0 ? view.PlaneReach / along : view.FarMeters;
    // The comparison allows for the 16-bit codes and for an opaque occluder's depth, rebuilt from its rasterized
    // float32 depth.
    planeForward = clamp(planeForward, view.NearMeters, view.FarMeters);
    float plane = (planeForward - view.NearMeters) * view.InverseRange -
                  LUDUS_MESH_LIGHT_SHADOW_DEPTH_BIAS_CODES / SHADOW_DEPTH_CODE_MAXIMUM -
                  view.RebuiltErrorScale * planeForward * planeForward;
    outOccluderWorld = texelDirectionWorld * (view.NearMeters + stored / view.InverseRange);
    return stored < plane;
}
// Fraction of the caster's light disk that no occluder hides from the receiver, each light point weighted by its
// diffuse form factor there; one without a caster or shadow table. positionUncertaintyWorld is how far positionWorld
// may lie from the surface, zero for an exact position, and noise in [0, 1)^2 turns the receiver's taps and offsets
// their search widths (ComputeMeshLightShadowNoise).
float ComputeMeshLightShadowVisibility(uint tableWord,
                                       uint casterKey,
                                       float3 positionWorld,
                                       float3 positionUncertaintyWorld,
                                       float3 normalWorld,
                                       float2 noise)
{
    if (tableWord == 0 || casterKey == 0)
    {
        return 1;
    }
    uint casterWord = tableWord + MESH_LIGHT_SHADOW_HEADER_WORDS + (casterKey - 1) * MESH_LIGHT_SHADOW_CASTER_WORDS;
    float4 placement = meshLightWords[casterWord];
    float4 shape = meshLightWords[casterWord + 1];
    MeshLightShadowView view;
    view.OriginsX = meshLightWords[casterWord + 2];
    view.OriginsY = meshLightWords[casterWord + 3];
    view.OriginsZ = meshLightWords[casterWord + 4];
    view.FarMeters = placement.w;
    view.NearMeters = shape.y * placement.w;
    view.Texels = shape.z;
    view.Tangent = shape.w;
    view.InverseRange = 1 / (view.FarMeters - view.NearMeters);
    view.RebuiltErrorScale = LUDUS_MESH_LIGHT_SHADOW_RASTER_DEPTH_ERROR * view.InverseRange / view.NearMeters;
    view.NormalWorld = normalWorld;
    // One texel spans 2 * tangent * forward / texels metres at forward distance; the distance bounds it.
    float texelSpread = 2 * view.Tangent / view.Texels;
    float texelMeters = texelSpread * length(positionWorld - placement.xyz);
    float3 deltaWorld =
        positionWorld + normalWorld * (LUDUS_MESH_LIGHT_SHADOW_NORMAL_OFFSET_TEXELS * texelMeters) - placement.xyz;
    float planeHeight = -dot(normalWorld, deltaWorld);
    // A receiver rebuilt from conventional depth lies centimetres off its surface tens of metres from the camera.
    // Behind the surface, its plane would meet the receiver's own stored depth first and shadow it in a pattern that
    // changes with every pixel and camera pose.
    view.PlaneReach = max(planeHeight - abs(dot(normalWorld, positionUncertaintyWorld)), 0);
    float forward;
    uint face = GetMeshLightShadowFace(deltaWorld, forward);
    // A receiver facing away from the caster, behind its one-sided emitters, inside its near distance or beyond its
    // range is left unshadowed.
    if (GetMeshLightShadowOrigin(view, face).x < 0 || planeHeight <= 0 || forward <= view.NearMeters ||
        forward >= view.FarMeters)
    {
        return 1;
    }
    float distance = length(deltaWorld);
    // The light disk: in the plane across a one-sided caster's emitter normal, else facing the receiver. A table
    // without disk words (no flag in its header) has only the enclosing sphere's radius.
    float4 header = meshLightWords[tableWord];
    float4 disk = float4(0, 0, 0, shape.x);
    if (header.w > 0)
    {
        disk = meshLightWords[tableWord + MESH_LIGHT_SHADOW_HEADER_WORDS +
                              uint(header.x) * MESH_LIGHT_SHADOW_CASTER_WORDS +
                              (casterKey - 1) * MESH_LIGHT_SHADOW_DISK_WORDS];
    }
    float3 lightNormalWorld = dot(disk.xyz, disk.xyz) > 0 ? disk.xyz : deltaWorld / distance;
    float lightRadius = disk.w;
    // The receiver's height above the light's plane; a segment to a light point reaches height z at fraction z /
    // height.
    float height = dot(deltaWorld, lightNormalWorld);
    if (height <= 0)
    {
        return 1;
    }
    float3 helperWorld = abs(lightNormalWorld.z) < MESH_LIGHT_FRAME_AXIS_THRESHOLD ? float3(0, 0, 1) : float3(1, 0, 0);
    float3 tangentWorld = normalize(cross(helperWorld, lightNormalWorld));
    float3 bitangentWorld = cross(lightNormalWorld, tangentWorld);
    float rotation = 2 * PI * noise.x;
    float2 rotationDirection = float2(cos(rotation), sin(rotation));
    float2 goldenTurn = float2(cos(GOLDEN_ANGLE_RADIANS), sin(GOLDEN_ANGLE_RADIANS));
    // The segment from the receiver to light point radius * u crosses height z where the caster sees the direction
    // deltaWorld + radius * u * (height / z - 1): its lateral width at z, in metres beside the receiver.
    float widestLateral = MESH_LIGHT_SHADOW_MAXIMUM_SPREAD * distance;
    float searchLateral = min(lightRadius * (height / view.NearMeters - 1), widestLateral);
    // The rings span at most an octave each; rings narrower than a texel would only repeat the receiver's own tap.
    float ringOctaves =
        min(log2(max(searchLateral / (texelSpread * distance), 1)) / MESH_LIGHT_SHADOW_WIDE_RING_COUNT, 1);
    float inverseHeightSum = 0;
    float blockerCount = 0;
    float nearestInverseHeight = 0;
    float farthestInverseHeight = 1 / view.NearMeters;
    float3 occluderWorld;
    // The receiver's own direction always counts, so a receiver the caster's point sees shadowed is never wholly lit.
    if (ReadMeshLightShadowDirection(view, deltaWorld, occluderWorld) == true)
    {
        float inverseHeight = 1 / clamp(dot(occluderWorld, lightNormalWorld), view.NearMeters, height);
        inverseHeightSum = inverseHeight;
        blockerCount = 1;
        nearestInverseHeight = inverseHeight;
        farthestInverseHeight = inverseHeight;
    }
    float2 spokeTurn = float2(cos(2 * PI / MESH_LIGHT_SHADOW_SEARCH_RING_TAP_COUNT),
                              sin(2 * PI / MESH_LIGHT_SHADOW_SEARCH_RING_TAP_COUNT));
    float2 ringDirection = rotationDirection;
    uint ringCount = searchLateral > MESH_LIGHT_SHADOW_NARROW_TEXELS * texelMeters
                         ? MESH_LIGHT_SHADOW_WIDE_RING_COUNT + MESH_LIGHT_SHADOW_COMPACT_RING_COUNT
                         : 0;
    for (uint ring = 0; ring < ringCount; ++ring)
    {
        bool isWideRing = ring < MESH_LIGHT_SHADOW_WIDE_RING_COUNT;
        // A clear centre texel and wide spokes can pass on either side of disjoint blocker projections.
        // Probe at half and quarter of the disk radius to find the missing depths before declaring full light.
        float lateral = isWideRing == true
                            ? searchLateral * exp2(-(float(ring) + noise.y) * ringOctaves)
                            : lightRadius * exp2(float(MESH_LIGHT_SHADOW_WIDE_RING_COUNT) - float(ring) - 1);
        float2 spokeDirection = ringDirection;
        ringDirection = TurnMeshLightShadowTap(ringDirection, goldenTurn);
        if (isWideRing == false && (lateral < texelMeters || lateral >= searchLateral))
        {
            continue;
        }
        for (uint spoke = 0; spoke < MESH_LIGHT_SHADOW_SEARCH_RING_TAP_COUNT; ++spoke)
        {
            float3 directionWorld =
                deltaWorld + (tangentWorld * spokeDirection.x + bitangentWorld * spokeDirection.y) * lateral;
            spokeDirection = TurnMeshLightShadowTap(spokeDirection, spokeTurn);
            if (ReadMeshLightShadowDirection(view, directionWorld, occluderWorld) == false)
            {
                continue;
            }
            // Only an occluder between the light's plane and the receiver can hide the light. Seen from the
            // receiver, it backprojects onto the plane at deltaWorld + (occluder - deltaWorld) * height / (height -
            // its height); outside the disk it hides none of it.
            float occluderHeight = dot(occluderWorld, lightNormalWorld);
            if (occluderHeight <= 0 || occluderHeight >= height)
            {
                continue;
            }
            float3 lightPointWorld = deltaWorld + (occluderWorld - deltaWorld) * (height / (height - occluderHeight));
            if (dot(lightPointWorld, lightPointWorld) > lightRadius * lightRadius)
            {
                continue;
            }
            float inverseHeight = 1 / max(occluderHeight, view.NearMeters);
            inverseHeightSum += inverseHeight;
            blockerCount += 1;
            nearestInverseHeight = max(nearestInverseHeight, inverseHeight);
            farthestInverseHeight = min(farthestInverseHeight, inverseHeight);
        }
    }
    if (blockerCount == 0)
    {
        return 1;
    }
    // Filters narrower than a texel would compare the same texel throughout.
    float meanLateral = clamp(lightRadius * (height * inverseHeightSum / blockerCount - 1), texelMeters, widestLateral);
    float nearLateral = clamp(lightRadius * (height * nearestInverseHeight - 1), texelMeters, widestLateral);
    float farLateral = clamp(lightRadius * (height * farthestInverseHeight - 1), texelMeters, widestLateral);
    bool isLayered =
        nearLateral > MESH_LIGHT_SHADOW_LAYER_SPREAD_RATIO * farLateral && nearLateral - farLateral > texelMeters;
    // Occluders lower than this height belong to the near layer.
    float splitHeight = 2 / (nearestInverseHeight + farthestInverseHeight);
    float lit = 0;
    float weightSum = 0;
    float2 tapDirection = rotationDirection;
    for (uint tap = 0; tap < MESH_LIGHT_SHADOW_FILTER_TAP_COUNT; ++tap)
    {
        float2 offset = tapDirection * sqrt((float(tap) + 0.5) / MESH_LIGHT_SHADOW_FILTER_TAP_COUNT);
        tapDirection = TurnMeshLightShadowTap(tapDirection, goldenTurn);
        float3 lateralWorld = tangentWorld * offset.x + bitangentWorld * offset.y;
        // The light point's diffuse form factor at the receiver weights its visibility; one below the receiver's
        // horizon needs no lookup.
        float3 towardLightWorld = lateralWorld * lightRadius - deltaWorld;
        float lengthSquared = dot(towardLightWorld, towardLightWorld);
        float weight = max(dot(normalWorld, towardLightWorld), 0) * max(-dot(lightNormalWorld, towardLightWorld), 0) /
                       (lengthSquared * lengthSquared);
        if (weight <= 0)
        {
            continue;
        }
        weightSum += weight;
        bool isBlocked = false;
        if (isLayered == false)
        {
            if (ReadMeshLightShadowDirection(view, deltaWorld + lateralWorld * meanLateral, occluderWorld) == true)
            {
                isBlocked = dot(occluderWorld, lightNormalWorld) > 0;
            }
        }
        else
        {
            if (ReadMeshLightShadowDirection(view, deltaWorld + lateralWorld * nearLateral, occluderWorld) == true)
            {
                float occluderHeight = dot(occluderWorld, lightNormalWorld);
                isBlocked = occluderHeight > 0 && occluderHeight < splitHeight;
            }
            if (isBlocked == false &&
                ReadMeshLightShadowDirection(view, deltaWorld + lateralWorld * farLateral, occluderWorld) == true)
            {
                isBlocked = dot(occluderWorld, lightNormalWorld) >= splitHeight;
            }
        }
        lit += isBlocked == true ? 0 : weight;
    }
    return weightSum > 0 ? lit / weightSum : 1;
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
// lie from the surface along the view ray; the shadow comparison allows for it. positionPixel, the sample's pixel
// position, turns its shadow taps. With shouldSeparateDiffuse, the diffuse integrals go to outDiffuse and the result
// holds only the specular ones, for a skin resolve that spreads the diffuse lighting; otherwise outDiffuse is zero.
float3 EvaluateRasterMeshLightLobes(in SurfaceSample surface,
                                    in float3 positionWorld,
                                    in float3 positionUncertaintyWorld,
                                    in float2 positionPixel,
                                    uint polygonCount,
                                    uint gridWord,
                                    bool shouldSeparateDiffuse,
                                    out float3 outDiffuse)
{
    outDiffuse = float3(0);
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
    float3 diffuse = float3(0);
    uint first = 0;
    // Each cell lists a caster's records together, so one visibility serves a run of them.
    uint visibleCaster = ~0u;
    float visibility = 1;
    float2 shadowNoise = ComputeMeshLightShadowNoise(positionPixel);
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
        MeshLightLobes lobes;
        if (TryEvaluateMeshLightFar(receiver, record, lobes) == false)
        {
            lobes = EvaluateMeshLightRecord(receiver, record);
        }
        float3 contribution = lobes.Emission * (lobes.Diffuse + lobes.Specular);
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
                                                          surface.NormalWorld,
                                                          shadowNoise);
            visibleCaster = casterKey;
        }
        if (shouldSeparateDiffuse == true)
        {
            result += lobes.Emission * lobes.Specular * visibility;
            diffuse += lobes.Emission * lobes.Diffuse * visibility;
        }
        else
        {
            result += contribution * visibility;
        }
    }
    outDiffuse = max(diffuse, float3(0));
    return max(result, float3(0));
}
float3 EvaluateRasterMeshLights(in SurfaceSample surface,
                                in float3 positionWorld,
                                in float3 positionUncertaintyWorld,
                                in float2 positionPixel,
                                uint polygonCount,
                                uint gridWord)
{
    float3 diffuse;
    return EvaluateRasterMeshLightLobes(surface,
                                        positionWorld,
                                        positionUncertaintyWorld,
                                        positionPixel,
                                        polygonCount,
                                        gridWord,
                                        false,
                                        diffuse);
}
