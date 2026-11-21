#include "lighting_layout.h"
#include "material_scattering.slang"
#include "math.slang"
#include "raster_bindings.slang"
#include "renderer_environment_layout.h"
#include "renderer_shadow_depth.slang"
#include "shading_normal.slang"
// Thanks to Google/Filament authors, Physically Based Rendering in Filament,
// sections 4.4 and 5.2.2, https://google.github.io/filament/main/filament.html.
// GGX + correlated Smith + Schlick, Fresnel-weighted Lambert, single scattering.
// Constant environment is a declared Fresnel approximation, not a DFG/LUT integrator.
struct Parameters
{
    float4 Tint;
    float4 UvTransform;
    float4 Options;
    float4 Pbr;
    float4 Emission;
    float4 EnvironmentIrradiance;
    float4 EnvironmentRadiance;
    float4 BakeScale;
    float4 BakeUv;
#ifdef LUDUS_CLUSTERED
    float4 Lights[CLUSTERED_LIGHT_COUNT * FORWARD_LIGHT_WORDS];
#else
    float4 Lights[32];
#endif
    float4 ShadowData[SHADOW_DATA_WORDS];
#ifdef LUDUS_CLUSTERED
    float4 GridRows[CLUSTERED_GRID_ROWS];
    float4 GridMinimum;
    float4 GridScale;
#endif
#ifdef LUDUS_SURFACE_FIDELITY
    float4 SurfaceMap;
#endif
#ifdef LUDUS_AUTHORED_INPUTS
    float4 Authored;
    float4 IrradianceSh[LUDUS_ENVIRONMENT_SH_COEFFICIENTS];
    // X: one for a hair card. Y: strand direction, radians from the tangent toward the bitangent.
    // Z, W: primary and secondary highlight shifts in radians toward the card normal.
    float4 HairStrand;
    // X, Y: primary and base-colour-tinted secondary highlight weights.
    float4 HairHighlights;
    // XYZ: a skin material's linear RGB scattering distance in metres. W: its one-based skin profile slot
    // (DEFERRED_MAX_SKIN_PROFILES), or zero for every other material.
    float4 SkinScattering;
#endif
};
LUDUS_SHADER_BINDING(0) ConstantBuffer<Parameters> material : register(b0);
#ifdef LUDUS_DEFERRED_MESH_LIGHTS
#    include "deferred_layout.h"
// The resolve samples no material texture: the base-colour slot holds the mesh-light shadow atlas.
LUDUS_SHADER_BINDING(LUDUS_DEFERRED_MESH_LIGHT_SHADOW_BINDING)
Texture2D<float4> meshLightShadowAtlas LUDUS_SHADER_TEXTURE_REGISTER(LUDUS_DEFERRED_MESH_LIGHT_SHADOW_BINDING);
#else
LUDUS_SHADER_BINDING(1) Texture2D<float4> colorTexture : register(t1);
#endif
LUDUS_SHADER_BINDING(2) SamplerState colorSampler : register(s2);
#ifdef LUDUS_DEFERRED_MESH_LIGHTS
LUDUS_SHADER_BINDING(LUDUS_DEFERRED_MESH_LIGHT_BINDING)
StructuredBuffer<float4> meshLightWords LUDUS_SHADER_TEXTURE_REGISTER(LUDUS_DEFERRED_MESH_LIGHT_BINDING);
#    define LUDUS_LTC_TABLE_BINDING LUDUS_DEFERRED_LTC_BINDING
#    include "ltc_table.slang"
#else
#    ifndef LUDUS_GRAPH_REALTIME
LUDUS_SHADER_BINDING(3) Texture2D<float4> bakeTexture : register(t3);
#    endif
LUDUS_SHADER_BINDING(4) Texture2D<float4> shadowTexture : register(t4);
#endif
LUDUS_SHADER_BINDING(5) SamplerState shadowSampler : register(s5);
#ifdef LUDUS_CLUSTERED
LUDUS_SHADER_BINDING(6) Texture2D<float4> clusterTexture : register(t6);
#    if defined(LUDUS_ATMOSPHERE_SURFACE)
#        include "renderer_atmosphere_layout.h"
LUDUS_SHADER_BINDING(LUDUS_ATMOSPHERE_DEFERRED_MULTIPLE_SLOT)
Texture2D<float4> atmosphereMultipleTexture LUDUS_SHADER_TEXTURE_REGISTER(LUDUS_ATMOSPHERE_DEFERRED_MULTIPLE_SLOT);
#    elif defined(LUDUS_SURFACE_FIDELITY)
LUDUS_SHADER_BINDING(7) Texture2D<float4> surfaceTexture : register(t7);
#    else
LUDUS_SHADER_BINDING(7) SamplerState clusterSampler : register(s7);
#    endif
#endif
#if defined(LUDUS_SURFACE_FIDELITY) && !defined(LUDUS_CLUSTERED)
LUDUS_SHADER_BINDING(6) Texture2D<float4> surfaceTexture : register(t6);
#endif
#ifdef LUDUS_SURFACE_FIDELITY
#    include "renderer_surface_functions.slang"
#endif
#ifdef LUDUS_AUTHORED_INPUTS
#    if defined(LUDUS_ATMOSPHERE_SURFACE)
LUDUS_SHADER_BINDING(LUDUS_ATMOSPHERE_DEFERRED_SKY_SLOT)
Texture2D<float4> atmosphereSkyTexture LUDUS_SHADER_TEXTURE_REGISTER(LUDUS_ATMOSPHERE_DEFERRED_SKY_SLOT);
LUDUS_SHADER_BINDING(LUDUS_ATMOSPHERE_DEFERRED_TRANSMITTANCE_SLOT)
Texture2D<float4>
    atmosphereTransmittanceTexture LUDUS_SHADER_TEXTURE_REGISTER(LUDUS_ATMOSPHERE_DEFERRED_TRANSMITTANCE_SLOT);
#    elif defined(LUDUS_CLUSTERED)
LUDUS_SHADER_BINDING(8) Texture2D<float4> environmentTexture : register(t8);
LUDUS_SHADER_BINDING(9) SamplerState environmentSampler : register(s9);
LUDUS_SHADER_BINDING(10) Texture2D<float4> emissionTexture : register(t10);
#    else
LUDUS_SHADER_BINDING(7) Texture2D<float4> environmentTexture : register(t7);
LUDUS_SHADER_BINDING(8) SamplerState environmentSampler : register(s8);
LUDUS_SHADER_BINDING(9) Texture2D<float4> emissionTexture : register(t9);
#    endif
#endif
struct Input
{
    LUDUS_SHADER_LOCATION(0) float3 PositionMesh : POSITION;
    LUDUS_SHADER_LOCATION(1) float4 MeshToClipRow0 : TEXCOORD0;
    LUDUS_SHADER_LOCATION(2) float4 MeshToClipRow1 : TEXCOORD1;
    LUDUS_SHADER_LOCATION(3) float4 MeshToClipRow2 : TEXCOORD2;
    LUDUS_SHADER_LOCATION(4) float4 MeshToClipRow3 : TEXCOORD3;
    LUDUS_SHADER_LOCATION(5) float4 Color : COLOR0;
    LUDUS_SHADER_LOCATION(6) float2 TexcoordUv : TEXCOORD4;
    LUDUS_SHADER_LOCATION(7) float3 NormalMesh : NORMAL;
    LUDUS_SHADER_LOCATION(8) float4 MeshToWorldRow0 : TEXCOORD5;
    LUDUS_SHADER_LOCATION(9) float4 MeshToWorldRow1 : TEXCOORD6;
    LUDUS_SHADER_LOCATION(10) float4 MeshToWorldRow2 : TEXCOORD7;
#ifdef LUDUS_AUTHORED_INPUTS
    LUDUS_SHADER_LOCATION(11) float4 TangentMesh : TEXCOORD8;
    LUDUS_SHADER_LOCATION(12) float4 ColorVertex : TEXCOORD9;
    LUDUS_SHADER_LOCATION(13) float2 MaterialUv1 : TEXCOORD10;
#else
    LUDUS_SHADER_LOCATION(11) float4 NormalMeshToWorldRow0 : TEXCOORD8;
    LUDUS_SHADER_LOCATION(12) float4 NormalMeshToWorldRow1 : TEXCOORD9;
    LUDUS_SHADER_LOCATION(13) float4 NormalMeshToWorldRow2 : TEXCOORD10;
#endif
    LUDUS_SHADER_LOCATION(14) float4 EyeWorld : TEXCOORD11;
    LUDUS_SHADER_LOCATION(15) float2 BakeUv : TEXCOORD12;
};
struct Varying
{
    float4 PositionClip : SV_Position;
    float2 TexcoordUv : TEXCOORD0;
    float3 PositionWorld : TEXCOORD1;
    float3 NormalWorld : TEXCOORD2;
    float4 EyeWorld : TEXCOORD3;
    float2 BakeUv : TEXCOORD4;
    float IsBakeValid : TEXCOORD5;
#ifdef LUDUS_AUTHORED_INPUTS
    float4 TangentWorld : TEXCOORD7;
    float4 ColorVertex : TEXCOORD8;
    float2 MaterialUv1 : TEXCOORD9;
#endif
#ifdef LUDUS_CLUSTERED
    float IsDirectReference : TEXCOORD6;
#endif
#ifdef LUDUS_DEFERRED_MESH_LIGHTS
    // How far PositionWorld may lie from the surface along the view ray, in metres. The resolve rebuilds the
    // position from the depth buffer and fills this from that buffer's precision; interpolated positions leave zero.
    float3 PositionUncertaintyWorld : TEXCOORD11;
    float ForwardDepthMetres : TEXCOORD12;
#endif
#ifdef LUDUS_DEBUG_VIEW
    // Last member, so ordinary shadow fragments still match the earlier varying locations.
    float2 DebugDepthRange : TEXCOORD10;
#endif
};

[shader("vertex")] Varying VertexMain(Input input) {
    Varying output;
    float4 p = float4(input.PositionMesh, 1);
    output.PositionClip = float4(dot(input.MeshToClipRow0, p),
                                 dot(input.MeshToClipRow1, p),
                                 dot(input.MeshToClipRow2, p),
                                 dot(input.MeshToClipRow3, p));
    output.PositionWorld =
        float3(dot(input.MeshToWorldRow0, p), dot(input.MeshToWorldRow1, p), dot(input.MeshToWorldRow2, p));
#ifdef LUDUS_AUTHORED_INPUTS
    float3 column0World = float3(input.MeshToWorldRow0.x, input.MeshToWorldRow1.x, input.MeshToWorldRow2.x);
    float3 column1World = float3(input.MeshToWorldRow0.y, input.MeshToWorldRow1.y, input.MeshToWorldRow2.y);
    float3 column2World = float3(input.MeshToWorldRow0.z, input.MeshToWorldRow1.z, input.MeshToWorldRow2.z);
    // Positive common rescaling keeps cross/determinant products finite without
    // changing normal direction or mirrored handedness.
    float magnitude = max(max(max(abs(column0World.x), abs(column0World.y)), abs(column0World.z)),
                          max(max(max(abs(column1World.x), abs(column1World.y)), abs(column1World.z)),
                              max(max(abs(column2World.x), abs(column2World.y)), abs(column2World.z))));
    column0World /= magnitude;
    column1World /= magnitude;
    column2World /= magnitude;
    float determinant = dot(column0World, cross(column1World, column2World));
    output.NormalWorld = NormalizeOrZero((cross(column1World, column2World) * input.NormalMesh.x +
                                          cross(column2World, column0World) * input.NormalMesh.y +
                                          cross(column0World, column1World) * input.NormalMesh.z) /
                                         determinant);
    float3 tangentWorld =
        column0World * input.TangentMesh.x + column1World * input.TangentMesh.y + column2World * input.TangentMesh.z;
    output.TangentWorld = float4(tangentWorld, input.TangentMesh.w * (determinant < 0 ? -1 : 1));
    output.ColorVertex = input.ColorVertex;
    output.MaterialUv1 = input.MaterialUv1;
#else
    output.NormalWorld = NormalizeOrZero(float3(dot(input.NormalMeshToWorldRow0.xyz, input.NormalMesh),
                                                dot(input.NormalMeshToWorldRow1.xyz, input.NormalMesh),
                                                dot(input.NormalMeshToWorldRow2.xyz, input.NormalMesh)));
#endif
    output.TexcoordUv = input.TexcoordUv;
    output.EyeWorld = input.EyeWorld;
    output.BakeUv = input.BakeUv;
    output.IsBakeValid = input.Color.x;
#ifdef LUDUS_CLUSTERED
    output.IsDirectReference = input.Color.y;
#endif
#ifdef LUDUS_DEFERRED_MESH_LIGHTS
    output.PositionUncertaintyWorld = float3(0);
#endif
#ifdef LUDUS_DEBUG_VIEW
    output.DebugDepthRange = input.Color.zw;
#endif
    return output;
} float ComputeFifth(float x)
{
    float square = x * x;
    return square * square * x;
}
float3 EvaluateBrdf(float3 baseColor,
                    float3 normalWorld,
                    float3 viewDirectionWorld,
                    float3 lightDirectionWorld,
                    float metallic,
                    float roughness,
                    bool isBakedDiffuse)
{
    float nv = max(dot(normalWorld, viewDirectionWorld), 0);
    float nl = max(dot(normalWorld, lightDirectionWorld), 0);
    float3 halfSumWorld = viewDirectionWorld + lightDirectionWorld;
    if (nv <= 0 || nl <= 0 || dot(halfSumWorld, halfSumWorld) < 1e-12)
        return float3(0);
    float3 halfDirectionWorld = NormalizeOrZero(halfSumWorld);
    float lh = saturate(dot(lightDirectionWorld, halfDirectionWorld));
    float a = roughness * roughness;
    float a2 = a * a;
    float distribution = ComputeGgxNormalDistribution(normalWorld, halfDirectionWorld, a2);
    float visibility = 0.5 / max(nl * sqrt(nv * nv * (1 - a2) + a2) + nv * sqrt(nl * nl * (1 - a2) + a2), 1e-6);
    float3 f0 = lerp(float3(0.04), baseColor, metallic);
    float3 fresnel = f0 + (1 - f0) * ComputeFifth(1 - lh);
    return nl * (distribution * visibility * fresnel +
                 baseColor * (1 - metallic) * (1 - fresnel) * (isBakedDiffuse ? 0 : 1) / PI);
}
// EvaluateBrdf's two lobes apart: returns the specular lobe and writes the diffuse one, for a skin resolve that
// spreads only diffuse lighting (renderer_deferred_lighting.slang).
float3 EvaluateBrdfLobes(float3 baseColor,
                         float3 normalWorld,
                         float3 viewDirectionWorld,
                         float3 lightDirectionWorld,
                         float metallic,
                         float roughness,
                         bool isBakedDiffuse,
                         out float3 outDiffuse)
{
    outDiffuse = float3(0);
    float nv = max(dot(normalWorld, viewDirectionWorld), 0);
    float nl = max(dot(normalWorld, lightDirectionWorld), 0);
    float3 halfSumWorld = viewDirectionWorld + lightDirectionWorld;
    if (nv <= 0 || nl <= 0 || dot(halfSumWorld, halfSumWorld) < 1e-12)
        return float3(0);
    float3 halfDirectionWorld = NormalizeOrZero(halfSumWorld);
    float lh = saturate(dot(lightDirectionWorld, halfDirectionWorld));
    float a = roughness * roughness;
    float a2 = a * a;
    float distribution = ComputeGgxNormalDistribution(normalWorld, halfDirectionWorld, a2);
    float visibility = 0.5 / max(nl * sqrt(nv * nv * (1 - a2) + a2) + nv * sqrt(nl * nl * (1 - a2) + a2), 1e-6);
    float3 f0 = lerp(float3(0.04), baseColor, metallic);
    float3 fresnel = f0 + (1 - f0) * ComputeFifth(1 - lh);
    outDiffuse = nl * baseColor * (1 - metallic) * (1 - fresnel) * (isBakedDiffuse ? 0 : 1) / PI;
    return nl * distribution * visibility * fresnel;
}
#ifdef LUDUS_AUTHORED_INPUTS
// Raster hair cards (content::scene_format::MaterialModel::HairCard). hair_card.cpp mirrors every operation for
// tests; keep the two together. Thanks to James T. Kajiya and Timothy L. Kay, "Rendering Fur with Three Dimensional
// Textures", SIGGRAPH '89, pp. 271-280, "The specular component", Fig. 8, for the highlight on a cone around the fibre
// tangent. Ludus evaluates it around a card's strand, tilted by two authored shifts, with a (p + 2) / (2 pi)
// normalization and card-side gating; see hair_card.hpp.
static const float HAIR_CARD_MIN_EXPONENT = 1;     // MIN_HAIR_CARD_EXPONENT
static const float HAIR_CARD_MAX_EXPONENT = 65536; // MAX_HAIR_CARD_EXPONENT
// Squared length below which a hair-card direction is treated as vanishing.
static const float HAIR_CARD_MIN_DIRECTION_LENGTH_SQUARED = 1e-12;
// Sine between the normal and +Y below which the fallback tangent uses +Z instead.
static const float HAIR_CARD_MIN_TANGENT_SINE = 1e-3;
bool IsHairCard()
{
    return material.HairStrand.x > .5;
}
float3 NormalizeHairCardDirection(in float3 value)
{
    float lengthSquared = dot(value, value);
    return lengthSquared > HAIR_CARD_MIN_DIRECTION_LENGTH_SQUARED ? value * rsqrt(lengthSquared) : float3(0);
}
float ComputeHairCardExponent(float roughness)
{
    float alpha = roughness * roughness;
    float alphaSquared = alpha * alpha;
    return alphaSquared > 0 ? clamp(2 / alphaSquared - 2, HAIR_CARD_MIN_EXPONENT, HAIR_CARD_MAX_EXPONENT)
                            : HAIR_CARD_MIN_EXPONENT;
}
float3 ComputeHairCardFallbackTangent(in float3 normalWorld)
{
    float3 horizontal = cross(float3(0, 1, 0), normalWorld);
    if (dot(horizontal, horizontal) > HAIR_CARD_MIN_TANGENT_SINE * HAIR_CARD_MIN_TANGENT_SINE)
        return NormalizeHairCardDirection(horizontal);
    return NormalizeHairCardDirection(cross(normalWorld, float3(0, 0, 1)));
}
float3 ComputeHairCardStrand(in float3 normalWorld, in float3 tangentWorld, float handedness, float strandRadians)
{
    float3 planarTangentWorld = NormalizeHairCardDirection(tangentWorld - normalWorld * dot(normalWorld, tangentWorld));
    float sign = handedness < 0 ? -1 : 1;
    if (dot(planarTangentWorld, planarTangentWorld) == 0)
    {
        planarTangentWorld = ComputeHairCardFallbackTangent(normalWorld);
        sign = 1;
    }
    float3 bitangentWorld = cross(normalWorld, planarTangentWorld) * sign;
    return NormalizeHairCardDirection(planarTangentWorld * cos(strandRadians) + bitangentWorld * sin(strandRadians));
}
float EvaluateHairCardHighlight(in float3 strandWorld,
                                in float3 facingNormalWorld,
                                in float3 halfDirectionWorld,
                                float shiftRadians,
                                float exponent)
{
    float3 shiftedStrandWorld =
        NormalizeHairCardDirection(strandWorld * cos(shiftRadians) + facingNormalWorld * sin(shiftRadians));
    float cosine = dot(shiftedStrandWorld, halfDirectionWorld);
    float sine = sqrt(max(0, 1 - cosine * cosine));
    // Render stages compile with fast math: keep pow away from log2(0).
    return sine > 0 ? (exponent + 2) / (2 * PI) * pow(sine, exponent) : 0;
}
float3 EvaluateHairCard(in float3 baseColor,
                        float roughness,
                        in float3 facingNormalWorld,
                        in float3 strandWorld,
                        in float3 viewDirectionWorld,
                        in float3 lightDirectionWorld,
                        bool isBakedDiffuse)
{
    float nl = dot(facingNormalWorld, lightDirectionWorld);
    float nv = dot(facingNormalWorld, viewDirectionWorld);
    if (nl <= 0 || nv <= 0)
        return float3(0);
    float3 result = isBakedDiffuse == true ? float3(0) : baseColor / PI;
    float3 halfDirectionWorld = NormalizeHairCardDirection(viewDirectionWorld + lightDirectionWorld);
    if (dot(halfDirectionWorld, halfDirectionWorld) > 0)
    {
        float exponent = ComputeHairCardExponent(roughness);
        float primary = material.HairHighlights.x * EvaluateHairCardHighlight(strandWorld,
                                                                              facingNormalWorld,
                                                                              halfDirectionWorld,
                                                                              material.HairStrand.z,
                                                                              exponent);
        float secondary = material.HairHighlights.y * EvaluateHairCardHighlight(strandWorld,
                                                                                facingNormalWorld,
                                                                                halfDirectionWorld,
                                                                                material.HairStrand.w,
                                                                                exponent);
        result += float3(primary) + baseColor * secondary;
    }
    return result * nl;
}
#endif
// Instance flags in EyeWorld.w: bit 0 receives shadows, bit 1 orthographic eye. The value is
// constant per instance but reaches the fragment through a perspective-interpolated varying,
// so it may arrive as 0.9999...; round instead of truncating or whole pixel rows lose a flag.
int GetEyeFlags(Varying input)
{
    return int(input.EyeWorld.w + 0.5);
}
// The encoded depth and slope a shadow map stores at uv: the mesh-light atlas in the deferred resolve.
float4 ReadShadowCode(float2 uv)
{
#ifdef LUDUS_DEFERRED_MESH_LIGHTS
    return meshLightShadowAtlas.SampleLevel(shadowSampler, uv, 0);
#else
    return shadowTexture.SampleLevel(shadowSampler, uv, 0);
#endif
}
// One shadow region lookup: the receiver's place in the map and how far its position may be off its surface.
struct ShadowLookup
{
    float4 Rectangle;
    float4 Settings;
    float2 ReceiverTexel;
    float ReceiverDepth;
    float2 UncertaintyTexel;
    float UncertaintyDepth;
    bool IsNearCascade;
};
// The depth the caster stored at sampleUv has at the receiver's location, and the tolerance its comparison allows.
float ProjectShadowCaster(in ShadowLookup lookup, float2 sampleUv, out float outTolerance, out float outStored)
{
    float4 code = ReadShadowCode(sampleUv);
    float2 centerTexel = sampleUv / lookup.Settings.zw;
    // Thanks to Microsoft, "Cascaded Shadow Maps", Depth Bias,
    // https://learn.microsoft.com/en-us/windows/win32/dxtecharts/cascaded-shadow-maps,
    // for comparing depths at matching locations across a PCF filter.
    // Light projections map planes to planes, so each caster plane is projected to the receiver's
    // location from the slope stored at the sampled texel. A fixed bias alone cannot cover the depth
    // a grazing light spans across one texel: Sun-lit floors then shadowed themselves in texel stripes
    // that aliased into blotches moving with the camera.
    float2 slope = DecodeShadowSlope(code.ba);
    outStored = DecodeShadowDepth(code.rg);
    // The displaced receiver changes its own depth and where it meets the caster's plane. Allow both,
    // or a surface lit at a grazing angle shadows itself wherever the depth buffer rounds unfavourably.
    outTolerance = lookup.Settings.x + lookup.UncertaintyDepth + dot(abs(slope), lookup.UncertaintyTexel);
    float2 offsetTexels = lookup.ReceiverTexel - centerTexel;
    if (lookup.IsNearCascade == true)
    {
        // Each signed square-root slope rounded to an 8-bit code. Bound its squared endpoint
        // error before extrapolating the caster plane to the receiver.
        float halfStep = 0.5 / SHADOW_SLOPE_CODE_RADIUS;
        float2 slopeError = halfStep * (2 * sqrt(abs(slope)) + halfStep);
        outTolerance += dot(abs(offsetTexels), slopeError);
    }
    return outStored + dot(slope, offsetTexels);
}
// Visibility of the receiver against the four map texels nearest sampleTexel, bilinearly weighted.
float CompareShadowTexels(in ShadowLookup lookup, float2 sampleTexel)
{
    float2 lowerTexel = floor(sampleTexel - 0.5) + 0.5;
    float2 fraction = sampleTexel - lowerTexel;
    float visible = 0;
    const int SHADOW_FILTER_TAP_COUNT = 4;
    for (int tap = 0; tap < SHADOW_FILTER_TAP_COUNT; ++tap)
    {
        float2 corner = float2(tap & 1, (tap >> 1) & 1);
        float2 sampleUv = clamp((lowerTexel + corner) * lookup.Settings.zw, lookup.Rectangle.xy, lookup.Rectangle.zw);
        float tolerance;
        float stored;
        float projected = ProjectShadowCaster(lookup, sampleUv, tolerance, stored);
        float2 weights = lerp(1 - fraction, fraction, corner);
        float weight = weights.x * weights.y;
        visible += lookup.ReceiverDepth - tolerance <= projected ? weight : 0;
    }
    return visible;
}
// Directional penumbrae (DirectLight::AngularRadiusRadians). Thanks to Randima Fernando, "Percentage-Closer Soft
// Shadows", ACM SIGGRAPH 2005 Sketches, p. 35, https://doi.org/10.1145/1187112.1187153, for estimating the penumbra
// from the average blocker depth and widening the comparison filter by it, and to Helmut Vogel, "A better way to
// construct the sunflower head", Mathematical Biosciences 44(3-4), 1979, pp. 179-189,
// https://doi.org/10.1016/0025-5564(79)90080-4, for the evenly spread golden-angle disk of taps. A disk of angular
// radius r casts a penumbra of radius tan(r) times the distance from the blocker to the receiver along the light; the
// orthographic map turns depth and texel offsets into metres through its projection rows. The filter radius is
// clamped to DIRECTIONAL_PENUMBRA_MAXIMUM_TEXELS, which bounds the cost and keeps wider penumbrae soft but too narrow.
static const int DIRECTIONAL_PENUMBRA_TAP_COUNT = 16;
static const float DIRECTIONAL_PENUMBRA_MAXIMUM_TEXELS = 32;
// Golden-angle disk tap `tap` of DIRECTIONAL_PENUMBRA_TAP_COUNT, inside the unit disk.
float2 GetDirectionalPenumbraTap(int tap)
{
    float radius = sqrt((float(tap) + 0.5) / float(DIRECTIONAL_PENUMBRA_TAP_COUNT));
    float angle = float(tap) * GOLDEN_ANGLE_RADIANS;
    return radius * float2(cos(angle), sin(angle));
}
// Visibility of the receiver from a directional light's disk whose angular radius has tangent diskTangent.
// texelsPerDepth converts a map depth difference into map texels of penumbra radius per unit disk tangent.
float FilterDirectionalPenumbra(in ShadowLookup lookup, float diskTangent, float2 texelsPerDepth)
{
    // Nothing nearer than the map's near plane can block: it bounds the widest penumbra the receiver can be in.
    float2 searchTexels = clamp(diskTangent * lookup.ReceiverDepth * texelsPerDepth,
                                float2(1),
                                float2(DIRECTIONAL_PENUMBRA_MAXIMUM_TEXELS));
    float blockerSum = 0;
    float blockerCount = 0;
    for (int tap = 0; tap < DIRECTIONAL_PENUMBRA_TAP_COUNT; ++tap)
    {
        float2 sampleTexel = floor(lookup.ReceiverTexel + GetDirectionalPenumbraTap(tap) * searchTexels) + 0.5;
        float2 sampleUv = clamp(sampleTexel * lookup.Settings.zw, lookup.Rectangle.xy, lookup.Rectangle.zw);
        float tolerance;
        float stored;
        float projected = ProjectShadowCaster(lookup, sampleUv, tolerance, stored);
        if (lookup.ReceiverDepth - tolerance > projected)
        {
            blockerSum += stored;
            blockerCount += 1;
        }
    }
    if (blockerCount == 0)
        return CompareShadowTexels(lookup, lookup.ReceiverTexel);
    float blockerDepth = blockerSum / blockerCount;
    float2 radiusTexels = min(diskTangent * max(lookup.ReceiverDepth - blockerDepth, 0) * texelsPerDepth,
                              float2(DIRECTIONAL_PENUMBRA_MAXIMUM_TEXELS));
    // A penumbra narrower than a texel is the bilinear comparison's own transition.
    if (max(radiusTexels.x, radiusTexels.y) < 1)
        return CompareShadowTexels(lookup, lookup.ReceiverTexel);
    float visible = 0;
    for (int tap = 0; tap < DIRECTIONAL_PENUMBRA_TAP_COUNT; ++tap)
        visible += CompareShadowTexels(lookup, lookup.ReceiverTexel + GetDirectionalPenumbraTap(tap) * radiusTexels);
    return visible / float(DIRECTIONAL_PENUMBRA_TAP_COUNT);
}
// Nonnegative indices address material regions. A negative index encodes the near table word offset.
float4 ReadShadowWord(int region, int word)
{
#ifdef LUDUS_DEFERRED_MESH_LIGHTS
    if (region < 0)
        return meshLightWords[uint(-region) + uint(word)];
#endif
    return material.ShadowData[region * SHADOW_REGION_WORDS + word];
}
// Visibility of positionWorld in shadow region region, bilinearly filtered over its four nearest texels, or over a
// directional light's penumbra when its disk has a positive angular radius.
// outIsInside is false, and the result one, when the position falls outside the region's projection.
float SampleShadowRegion(int region, int light, float3 positionWorld, Varying input, out bool outIsInside)
{
    outIsInside = false;
    float4 settings = ReadShadowWord(region, 5);
    float4 p = float4(positionWorld, 1);
    float4 clip = float4(dot(ReadShadowWord(region, 0), p),
                         dot(ReadShadowWord(region, 1), p),
                         dot(ReadShadowWord(region, 2), p),
                         dot(ReadShadowWord(region, 3), p));
    if (clip.w <= 1e-6)
        return 1;
    float3 ndc = clip.xyz / clip.w;
    if (abs(ndc.x) > 1 || abs(ndc.y) > 1 || ndc.z < 0 || ndc.z > 1)
        return 1;
    outIsInside = true;
    ShadowLookup lookup;
    lookup.Rectangle = ReadShadowWord(region, 4);
    lookup.Settings = settings;
    lookup.IsNearCascade = region < 0;
    float4 rect = lookup.Rectangle;
    float2 projectedUv = float2(ndc.x * 0.5 + 0.5, 0.5 - ndc.y * 0.5);
    // Every region is drawn with its interior as the viewport, so the projection maps clip edges to viewport
    // edges. The atlas rectangle supplied by the host bounds legal texel centres.
    float2 uv = lerp(rect.xy - settings.zw * 0.5, rect.zw + settings.zw * 0.5, projectedUv);
    lookup.ReceiverTexel = uv / settings.zw;
    lookup.ReceiverDepth = ndc.z;
    // Map texels per unit of normalized device coordinates.
    float2 texelsPerNdc = 0.5 * (rect.zw - rect.xy + settings.zw) / settings.zw;
#ifdef LUDUS_DEFERRED_MESH_LIGHTS
    // A receiver rebuilt from depth may sit off its surface by PositionUncertaintyWorld. Express that
    // displacement in map texels and light depth, so each tap can allow for it below.
    float3 uncertaintyClip = float3(dot(ReadShadowWord(region, 0).xyz, input.PositionUncertaintyWorld),
                                    dot(ReadShadowWord(region, 1).xyz, input.PositionUncertaintyWorld),
                                    dot(ReadShadowWord(region, 2).xyz, input.PositionUncertaintyWorld)) /
                             clip.w;
    lookup.UncertaintyTexel = abs(uncertaintyClip.xy) * texelsPerNdc;
    lookup.UncertaintyDepth = abs(uncertaintyClip.z);
#else
    // Forward passes interpolate the receiver's exact position.
    lookup.UncertaintyTexel = float2(0);
    lookup.UncertaintyDepth = 0;
#endif
    float diskTangent = material.Lights[light * 4 + 3].z;
    // Only an orthographic directional region has a disk: its projection's last row is (0, 0, 0, 1).
    if (diskTangent > 0 && material.Lights[light * 4].w == 0 &&
        dot(ReadShadowWord(region, 3).xyz, ReadShadowWord(region, 3).xyz) == 0)
    {
        // Orthographic rows are the light basis scaled by NDC units per metre: a depth difference spans
        // 1 / |depth row| metres, and one metre across spans |row| NDC units.
        float metresPerDepth = 1 / length(ReadShadowWord(region, 2).xyz);
        float2 texelsPerMetre =
            texelsPerNdc * float2(length(ReadShadowWord(region, 0).xyz), length(ReadShadowWord(region, 1).xyz));
        return FilterDirectionalPenumbra(lookup, diskTangent, metresPerDepth * texelsPerMetre);
    }
    return CompareShadowTexels(lookup, lookup.ReceiverTexel);
}
// Shadow-map coverage of a light at a receiver, for the shadow coverage debug view.
static const int SHADOW_COVERAGE_NONE = 0;    // The light has no map.
static const int SHADOW_COVERAGE_INSIDE = 1;  // A map of the light covers the receiver.
static const int SHADOW_COVERAGE_OUTSIDE = 2; // The light has maps, but none covers the receiver.
float ComputeShadowCoverage(int light, Varying input, out int outCoverage)
{
    outCoverage = SHADOW_COVERAGE_NONE;
    if ((GetEyeFlags(input) & 1) == 0)
        return 1;
    bool hasLocalShadowCoverage = false;
    for (int region = 0; region < SHADOW_REGION_CAPACITY; ++region)
    {
        if (region >= int(material.Pbr.w))
            break;
        float4 settings = material.ShadowData[region * SHADOW_REGION_WORDS + 5];
#ifdef LUDUS_DEFERRED_MESH_LIGHTS
        // Sky visibility regions trail every light's.
        if (int(settings.y + 0.5) == int(SKY_VISIBILITY_REGION_TAG))
            break;
#endif
        if (int(settings.y) != light)
            continue;
        hasLocalShadowCoverage = hasLocalShadowCoverage || material.Lights[light * 4].w >= 1;
        outCoverage = SHADOW_COVERAGE_OUTSIDE;
        bool isInside;
#ifdef LUDUS_DEFERRED_MESH_LIGHTS
        int nearTable = int(deferredView.LightingView.w);
        if (nearTable > 0 && input.ForwardDepthMetres < DIRECTIONAL_NEAR_END_METRES)
        {
            int nearRegion = -(nearTable + region * int(SHADOW_REGION_WORDS));
            if (ReadShadowWord(nearRegion, 5).z > 0)
            {
                float nearVisibility = SampleShadowRegion(nearRegion, light, input.PositionWorld, input, isInside);
                if (isInside == true)
                {
                    outCoverage = SHADOW_COVERAGE_INSIDE;
                    float blend = smoothstep(float(DIRECTIONAL_NEAR_BLEND_METRES),
                                             float(DIRECTIONAL_NEAR_END_METRES),
                                             input.ForwardDepthMetres);
                    if (blend <= 0)
                        return nearVisibility;
                    float farVisibility = SampleShadowRegion(region, light, input.PositionWorld, input, isInside);
                    return isInside == true ? lerp(nearVisibility, farVisibility, blend) : nearVisibility;
                }
            }
        }
#endif
        float visible = SampleShadowRegion(region, light, input.PositionWorld, input, isInside);
        if (isInside == true)
        {
            outCoverage = SHADOW_COVERAGE_INSIDE;
            return visible;
        }
    }
    return hasLocalShadowCoverage
               ? 0
               : 1; // Local coverage gaps conservatively occlude; directional cascade contract stays explicit.
}
float ComputeShadowVisibility(int light, Varying input)
{
    int coverage;
    return ComputeShadowCoverage(light, input, coverage);
}
#ifdef LUDUS_DEFERRED_MESH_LIGHTS
// Sky visibility (render-graph-authoring.md, "Sky visibility"). The host renders SKY_VISIBILITY_DIRECTION_COUNT
// orthographic depth maps of the scene from fixed upper-hemisphere directions and appends them after the directional
// regions. A receiver sees the sky in a direction when that map does not hold a nearer caster, so sky and
// environment light reach it only through openings: without them every interior surface received the whole sky.
// The receiver moves this many map texels along its normal first, so a surface does not occlude its own sky where
// the coarse map's depth steps across it.
static const float SKY_VISIBILITY_NORMAL_OFFSET_TEXELS = 1.5;
// Upper bound of the cosine-power lobe that weights sky directions about a reflection vector. The directions are
// about 25 degrees apart, so a sharper lobe would switch between single directions and band along reflections.
static const float SKY_SPECULAR_LOBE_EXPONENT_MAXIMUM = 8;
// Guards the reciprocal map scale and lobe width against degenerate inputs.
static const float SKY_VISIBILITY_SCALE_EPSILON = 1e-6;
// Rec. 709 luminance of linear RGB, which weights the reflection lobe's sky directions by brightness.
static const float3 SKY_LUMINANCE_WEIGHTS = float3(0.2126, 0.7152, 0.0722);
// First sky visibility region, or -1 when the scene has no sky visibility maps.
int GetSkyVisibilityFirstRegion()
{
    int first = int(material.Pbr.w) - int(SKY_VISIBILITY_DIRECTION_COUNT);
    if (first < 0)
        return -1;
    return int(material.ShadowData[first * SHADOW_REGION_WORDS + 5].y + 0.5) == int(SKY_VISIBILITY_REGION_TAG) ? first
                                                                                                               : -1;
}
// Unit world direction toward the sky of a sky visibility region. Its projection's depth row is the direction the
// map looks along, scaled by the reciprocal depth range.
float3 GetSkyVisibilityDirection(int region)
{
    return -NormalizeOrZero(material.ShadowData[region * SHADOW_REGION_WORDS + 2].xyz);
}
// Fraction of positionWorld's neighbourhood that sees past the scene along a sky visibility region's direction,
// bilinearly filtered over the four nearest texels' caster planes. Positions outside the map see the sky. Unlike a
// light's lookup it allows no depth-buffer uncertainty: the normal offset already exceeds it.
float SampleSkyVisibilityRegion(int region,
                                in float3 positionWorld,
                                in float2 localClip,
                                in float2 atlasOffsetUv,
                                bool isLocal)
{
    float4 p = float4(positionWorld, 1);
    float4 rowX = material.ShadowData[region * SHADOW_REGION_WORDS];
    float4 rowY = material.ShadowData[region * SHADOW_REGION_WORDS + 1];
    float3 clip = float3(dot(rowX, p), dot(rowY, p), dot(material.ShadowData[region * SHADOW_REGION_WORDS + 2], p));
    float4 rect = material.ShadowData[region * SHADOW_REGION_WORDS + 4];
    float4 settings = material.ShadowData[region * SHADOW_REGION_WORDS + 5];
    if (isLocal == true)
    {
        clip.xy = localClip;
        rect += atlasOffsetUv.xyxy;
        // The full terrain depth interval can be kilometres long. Allow half a packed depth code
        // for rasterization and rounding, in addition to the local world-space normal offset.
        settings.x = 0.5 / SHADOW_DEPTH_CODE_MAXIMUM;
    }
    if (abs(clip.x) > 1 || abs(clip.y) > 1 || clip.z < 0 || clip.z > 1)
        return 1;
    float2 projectedUv = float2(clip.x * 0.5 + 0.5, 0.5 - clip.y * 0.5);
    float2 receiverTexel = lerp(rect.xy - settings.zw * 0.5, rect.zw + settings.zw * 0.5, projectedUv) / settings.zw;
    float2 lowerTexel = floor(receiverTexel - 0.5) + 0.5;
    float2 fraction = receiverTexel - lowerTexel;
    float visible = 0;
    const int SKY_FILTER_TAP_COUNT = 4;
    for (int tap = 0; tap < SKY_FILTER_TAP_COUNT; ++tap)
    {
        float2 corner = float2(tap & 1, (tap >> 1) & 1);
        float2 sampleUv = clamp((lowerTexel + corner) * settings.zw, rect.xy, rect.zw);
        float4 code = meshLightShadowAtlas.SampleLevel(shadowSampler, sampleUv, 0);
        float stored =
            DecodeShadowDepth(code.rg) + dot(DecodeShadowSlope(code.ba), receiverTexel - sampleUv / settings.zw);
        float2 weights = lerp(1 - fraction, fraction, corner);
        visible += clip.z - settings.x <= stored ? weights.x * weights.y : 0;
    }
    return visible;
}
#endif
#ifndef LUDUS_DEFERRED_MESH_LIGHTS
float4 SampleBaseTexel(Varying input)
{
    float2 materialUv = input.TexcoordUv;
#    ifdef LUDUS_AUTHORED_INPUTS
    if (material.Authored.y > .5)
        materialUv = input.MaterialUv1;
#    endif
    float2 uv = materialUv * material.UvTransform.xy + material.UvTransform.zw;
    float4 texel = material.Options.y < 0 ? colorTexture.Sample(colorSampler, uv)
                                          : colorTexture.SampleLevel(colorSampler, uv, material.Options.y);
    float4 baseColor = texel * material.Tint;
#    ifdef LUDUS_AUTHORED_INPUTS
    if (material.Authored.x > .5)
        baseColor *= input.ColorVertex;
#    endif
    // The opaque deferred G-buffer variant (DeferredOpaqueGBufferFragment) defines LUDUS_OPAQUE_COVERAGE: it draws only
    // opaque materials, and a fragment function that merely contains a discard loses hidden-surface removal on
    // tile-based GPUs (renderer.cpp, Renderer::drawView).
#    ifndef LUDUS_OPAQUE_COVERAGE
    if (material.Options.z == 1 && baseColor.a < material.Options.x)
        discard;
#    endif
    return baseColor;
}
#endif
#ifdef LUDUS_AUTHORED_INPUTS
// Thanks to Guy/Agopian, Filament sections 5.3 and 9.6 (URL above): SH diffuse
// and prefiltered GGX split-sum IBL. DFG uses Lazarov's fit documented by Filament.
float2 ComputeEnvironmentUv(in float3 directionWorld)
{
    float longitude = atan2(directionWorld.z, directionWorld.x) / 6.283185307179586;
    return float2(longitude - floor(longitude), atan2(length(directionWorld.xz), directionWorld.y) / PI);
}
float3 EvaluateEnvironmentIrradiance(in float3 normalWorld)
{
    float basis[LUDUS_ENVIRONMENT_SH_COEFFICIENTS] = {
        .2820947918,
        .4886025119 * normalWorld.y,
        .4886025119 * normalWorld.z,
        .4886025119 * normalWorld.x,
        1.0925484306 * normalWorld.x * normalWorld.y,
        1.0925484306 * normalWorld.y * normalWorld.z,
        .3153915653 * (3 * normalWorld.z * normalWorld.z - 1),
        1.0925484306 * normalWorld.x * normalWorld.z,
        .5462742153 * (normalWorld.x * normalWorld.x - normalWorld.y * normalWorld.y)};
    float3 irradiance = float3(0);
    for (int coefficient = 0; coefficient < LUDUS_ENVIRONMENT_SH_COEFFICIENTS; ++coefficient)
        irradiance += material.IrradianceSh[coefficient].rgb * basis[coefficient];
    return max(irradiance, float3(0));
}
#    if !defined(LUDUS_DEFERRED_MESH_LIGHTS) && !defined(LUDUS_GRAPH_REALTIME) && !defined(LUDUS_GRAPH_SURFACE_CHART)
#        include "probe_volume_layout.h"
float4 ReadGiProbeTexel(int probe, int texel)
{
    int index = probe * LUDUS_GI_PROBE_TEXELS + texel;
    return bakeTexture.Load(int3(index % LUDUS_GI_PROBE_ATLAS_WIDTH, index / LUDUS_GI_PROBE_ATLAS_WIDTH, 0));
}

// Thanks to Majercik, Guertin, Nowrouzezahrai and McGuire, "Dynamic Diffuse Global
// Illumination with Ray-Traced Irradiance Fields", JCGT 8(2), 2019, section 5,
// https://jcgt.org/published/0008/02/01/, for visibility-aware probe weighting.
// Compare each stored axial distance before filtering to preserve depth discontinuities.
float ComputeGiProbeVisibility(int probe, in float3 offsetWorldInCells)
{
    float3 magnitude = abs(offsetWorldInCells);
    int axis = magnitude.x >= magnitude.y && magnitude.x >= magnitude.z ? 0 : (magnitude.y >= magnitude.z ? 1 : 2);
    float distanceInCells = magnitude[axis];
    if (distanceInCells == 0)
        return 1;
    static const int AXIS_COUNT = 3;
    int face = 2 * axis + (offsetWorldInCells[axis] < 0 ? 1 : 0);
    float2 directionUv =
        float2(offsetWorldInCells[(axis + 1) % AXIS_COUNT], offsetWorldInCells[(axis + 2) % AXIS_COUNT]);
    float2 footprintTexel = clamp((directionUv / distanceInCells * 0.5 + 0.5) * LUDUS_GI_PROBE_FACE_WIDTH - 0.5,
                                  0,
                                  LUDUS_GI_PROBE_FACE_WIDTH - 1);
    int2 texel = int2(footprintTexel);
    float2 fraction = footprintTexel - float2(texel);
    int faceTexel = face * LUDUS_GI_PROBE_FACE_TEXELS + texel.y * LUDUS_GI_PROBE_FACE_WIDTH + texel.x;
    float4 depthsInCells = ReadGiProbeTexel(probe, LUDUS_GI_PROBE_FACES + faceTexel) * LUDUS_GI_PROBE_DEPTH_CELLS;
    float4 visibleTaps = step(distanceInCells - LUDUS_GI_PROBE_DEPTH_BIAS_CELLS, depthsInCells);
    float2 rows = lerp(visibleTaps.xz, visibleTaps.yw, fraction.x);
    float coverage = lerp(rows.x, rows.y, fraction.y);
    return saturate((coverage - LUDUS_GI_PROBE_MIN_VISIBLE_COVERAGE) / (1 - LUDUS_GI_PROBE_MIN_VISIBLE_COVERAGE));
}

float3 EvaluateGiProbeIrradiance(in float3 positionWorld, in float3 normalWorld)
{
    int3 counts = int3(material.BakeUv.xyz);
    float3 positionInGrid = (positionWorld - material.BakeScale.xyz) * -material.BakeScale.w;
    if (any(positionInGrid < 0) || any(positionInGrid > counts - 1))
        return float3(0);
    int3 first = min(int3(positionInGrid), counts - 2);
    float3 fraction = positionInGrid - first;
    float3 cosineSquared = normalWorld * normalWorld;
    int3 faces = int3(normalWorld.x < 0 ? 1 : 0, normalWorld.y < 0 ? 3 : 2, normalWorld.z < 0 ? 5 : 4);
    float3 sum = float3(0);
    float totalWeight = 0;
    for (int z = 0; z < 2; ++z)
        for (int y = 0; y < 2; ++y)
            for (int x = 0; x < 2; ++x)
            {
                int3 offset = int3(x, y, z);
                int3 cell = first + offset;
                int probe = cell.x + counts.x * (cell.y + counts.y * cell.z);
                float3 weights = lerp(1 - fraction, fraction, float3(offset));
                float weight = weights.x * weights.y * weights.z;
                if (weight <= 0)
                    continue;
                float4 irradianceX = ReadGiProbeTexel(probe, faces.x);
                if (irradianceX.a == 0)
                    continue;
                weight *= ComputeGiProbeVisibility(probe, positionInGrid - cell);
                if (weight > 0)
                {
                    sum += weight *
                           (irradianceX.rgb * cosineSquared.x + ReadGiProbeTexel(probe, faces.y).rgb * cosineSquared.y +
                            ReadGiProbeTexel(probe, faces.z).rgb * cosineSquared.z);
                    totalWeight += weight;
                }
            }
    return totalWeight > 0 ? sum * (material.BakeUv.w / totalWeight) : float3(0);
}
#    endif
float2 EvaluateEnvironmentDfg(float roughness, float cosineView)
{
    float4 coefficients = roughness * float4(-1, -.0275, -.572, .022) + float4(1, .0425, 1.04, -.04);
    float approximation =
        min(coefficients.x * coefficients.x, exp2(-9.28 * cosineView)) * coefficients.x + coefficients.y;
    return float2(-1.04, 1.04) * approximation + coefficients.zw;
}
#endif
#ifdef LUDUS_DEBUG_VIEW
// Largest display-linear value the inverse reproduces (about 0.998 sRGB); mirrors PBR_NEUTRAL_INVERSE_LIMIT.
static const float DEBUG_DISPLAY_LINEAR_LIMIT = 0.995;
// Display sRGB wireframe colour of edges on surfaces facing the camera.
static const float3 DEBUG_WIREFRAME_COLOUR =
    float3(MATERIAL_DEBUG_WIREFRAME_RED, MATERIAL_DEBUG_WIREFRAME_GREEN, MATERIAL_DEBUG_WIREFRAME_BLUE) / 255.0;
// Fraction of that colour kept on surfaces seen exactly edge-on.
static const float DEBUG_WIREFRAME_EDGE_ON_SHADE = MATERIAL_DEBUG_WIREFRAME_EDGE_ON_PERCENT / 100.0;
// Khronos PBR Neutral constants, shared with renderer_output.slang.
#    include "display_tone_mapping.slang"
// Scene-linear colour that renderer_output.slang's PBR Neutral curve maps to a wanted display-linear colour;
// mirrors InvertPbrNeutral in tone_mapping.hpp.
float3 InvertPbrNeutral(float3 display)
{
    float peak = max(display.x, max(display.y, display.z));
    float3 shifted = display;
    if (peak > PBR_NEUTRAL_START)
    {
        float headroom = 1 - PBR_NEUTRAL_START;
        float oldPeak = headroom * headroom / (1 - peak) - 1 + 2 * PBR_NEUTRAL_START;
        float mixAmount = 1 / (PBR_NEUTRAL_DESATURATION * (oldPeak - peak) + 1);
        float scale = peak / oldPeak * mixAmount;
        shifted = max((display - peak * (1 - mixAmount)) / scale, float3(0));
    }
    float smallest = min(shifted.x, min(shifted.y, shifted.z));
    float original =
        smallest < PBR_NEUTRAL_FRESNEL ? 2 * sqrt(PBR_NEUTRAL_FRESNEL * smallest) : smallest + PBR_NEUTRAL_FRESNEL;
    float offset = original <= 2 * PBR_NEUTRAL_FRESNEL ? original - original * original / (4 * PBR_NEUTRAL_FRESNEL)
                                                       : PBR_NEUTRAL_FRESNEL;
    return shifted + offset;
}
// Inverts RasterViewport's presentation (renderer_output.slang: PBR Neutral, then sRGB encode at
// exposure one) so a diagnostic colour authored in display sRGB appears unchanged on screen.
float3 EncodeDebugDisplay(float3 displaySrgb)
{
    float3 encoded = saturate(displaySrgb);
    float3 linearColor = float3(encoded.x <= 0.04045 ? encoded.x / 12.92 : pow((encoded.x + 0.055) / 1.055, 2.4),
                                encoded.y <= 0.04045 ? encoded.y / 12.92 : pow((encoded.y + 0.055) / 1.055, 2.4),
                                encoded.z <= 0.04045 ? encoded.z / 12.92 : pow((encoded.z + 0.055) / 1.055, 2.4));
    return InvertPbrNeutral(min(linearColor, float3(DEBUG_DISPLAY_LINEAR_LIMIT)));
}
// Perceptually uniform viridis ramp over t in [0,1], dark purple through teal to yellow, as display-space sRGB.
// Thanks to Matt Zucker, "Viridis approximation" (Shadertoy WlfXRN), a degree-six polynomial fit of
// matplotlib's viridis (Nathaniel Smith and Stefan van der Walt, CC0).
float3 ComputeRampColour(float t)
{
    const float3 coefficient0 = float3(0.2777273272234177, 0.005407344544966578, 0.3340998053353061);
    const float3 coefficient1 = float3(0.1050930431085774, 1.404613529898575, 1.384590162594685);
    const float3 coefficient2 = float3(-0.3308618287255563, 0.214847559468213, 0.09509516302823659);
    const float3 coefficient3 = float3(-4.634230498983486, -5.799100973351585, -19.33244095627987);
    const float3 coefficient4 = float3(6.228269936347081, 14.17993336680509, 56.69055260068105);
    const float3 coefficient5 = float3(4.776384997670288, -13.74514537774601, -65.35303263337234);
    const float3 coefficient6 = float3(-5.435455855934631, 4.645852612178535, 26.3124352495832);
    float x = saturate(t);
    return saturate(
        coefficient0 +
        x * (coefficient1 +
             x * (coefficient2 + x * (coefficient3 + x * (coefficient4 + x * (coefficient5 + x * coefficient6))))));
}
float3 ComputeDebugColour(int mode,
                          Varying input,
                          float3 normalWorld,
                          float metallic,
                          float roughness,
                          int lightCount,
                          float3 shaded)
{
    if (mode == int(MATERIAL_DEBUG_VIEW_NORMAL_WORLD))
        return normalWorld * 0.5 + 0.5;
    if (mode == int(MATERIAL_DEBUG_VIEW_LINEAR_DEPTH))
    {
        // Perspective: radial distance from the eye. Orthographic (eye flag bit 1): distance
        // along the view direction; the renderer shifted the range into this frame.
        float distance = (GetEyeFlags(input) & 2) != 0 ? -dot(input.PositionWorld, input.EyeWorld.xyz)
                                                       : length(input.PositionWorld - input.EyeWorld.xyz);
        float span = input.DebugDepthRange.y - input.DebugDepthRange.x;
        return ComputeRampColour(span > 0 ? (distance - input.DebugDepthRange.x) / span : 0);
    }
    if (mode == int(MATERIAL_DEBUG_VIEW_ROUGHNESS))
        return float3(roughness);
    if (mode == int(MATERIAL_DEBUG_VIEW_METALLIC))
        return float3(metallic);
    if (mode == int(MATERIAL_DEBUG_VIEW_TEXCOORD_UV))
        return float3(frac(input.TexcoordUv), 0);
    if (mode == int(MATERIAL_DEBUG_VIEW_LIGHT_COUNT))
        return ComputeRampColour(float(lightCount) / float(MATERIAL_DEBUG_LIGHT_COUNT_SATURATION));
    if (mode == int(MATERIAL_DEBUG_VIEW_WIREFRAME))
    {
        // Edges of surfaces seen edge-on dim, so overlapping wire layers keep a sense of shape.
        float3 viewDirectionWorld = (GetEyeFlags(input) & 2) != 0
                                        ? NormalizeOrZero(input.EyeWorld.xyz)
                                        : NormalizeOrZero(input.EyeWorld.xyz - input.PositionWorld);
        float facing = abs(dot(normalWorld, viewDirectionWorld));
        return DEBUG_WIREFRAME_COLOUR * lerp(DEBUG_WIREFRAME_EDGE_ON_SHADE, 1, facing);
    }
    return shaded;
}
#endif
struct SurfaceSample
{
    float4 BaseColor;
    float3 NormalWorld;
    float3 ViewDirectionWorld;
    float Metallic;
    float Roughness;
    float3 Emission;
    float Alpha;
    // Unit hair-card strand direction; zero for every other material.
    float3 StrandWorld = float3(0);
    // One-based skin profile slot of a skin material with a scattering distance; zero for every other material.
    float SkinProfile = 0;
};
#ifndef LUDUS_DEFERRED_MESH_LIGHTS
SurfaceSample SampleMaterial(in Varying input)
{
    // Thanks to the W3C GPU for the Web Working Group, WGSL, "Derivative Built-in Functions":
    // https://www.w3.org/TR/WGSL/#derivative-builtin-functions
    // Evaluate quad derivatives before alpha discard and the per-fragment tangent-frame choice.
    float3 positionDerivativeXWorld = ddx(input.PositionWorld);
    float3 positionDerivativeYWorld = ddy(input.PositionWorld);
#    ifdef LUDUS_AUTHORED_INPUTS
    float2 hairUv = (material.Authored.y > .5 ? input.MaterialUv1 : input.TexcoordUv) * material.UvTransform.xy +
                    material.UvTransform.zw;
    float2 hairUvDerivativeX = ddx(hairUv);
    float2 hairUvDerivativeY = ddy(hairUv);
#    endif
    float4 baseColor = SampleBaseTexel(input);
    float3 normalWorld = NormalizeOrZero(input.NormalWorld);
    float3 viewDirectionWorld = (GetEyeFlags(input) & 2) != 0
                                    ? NormalizeOrZero(input.EyeWorld.xyz)
                                    : NormalizeOrZero(input.EyeWorld.xyz - input.PositionWorld);
    float metallic = material.Pbr.x;
    float roughness = material.Pbr.y;
#    ifdef LUDUS_SURFACE_FIDELITY
    if (material.SurfaceMap.y > .5)
    {
        float2 materialUv = input.TexcoordUv;
#        ifdef LUDUS_AUTHORED_INPUTS
        if (material.Authored.y > .5)
            materialUv = input.MaterialUv1;
#        endif
        float2 uv = materialUv * material.UvTransform.xy + material.UvTransform.zw;
        float4 texel = material.Options.y < 0 ? surfaceTexture.Sample(colorSampler, uv)
                                              : surfaceTexture.SampleLevel(colorSampler, uv, material.Options.y);
        SurfaceParameters parameters = DecodeSurfaceParameters(texel, metallic, roughness, material.SurfaceMap.x);
        metallic = parameters.Metallic;
        roughness = parameters.Roughness;
        float2 uvDerivativeXMaterial = ddx(uv);
        float2 uvDerivativeYMaterial = ddy(uv);
#        ifdef LUDUS_AUTHORED_INPUTS
        if (material.Authored.x > .5 && abs(input.TangentWorld.w) > .5)
        {
            float3 tangentWorld =
                NormalizeOrZero(input.TangentWorld.xyz - normalWorld * dot(normalWorld, input.TangentWorld.xyz));
            float3 bitangentWorld = cross(normalWorld, tangentWorld) * input.TangentWorld.w;
            normalWorld =
                NormalizeOrZero(tangentWorld * parameters.NormalTangent.x +
                                bitangentWorld * parameters.NormalTangent.y + normalWorld * parameters.NormalTangent.z);
        }
        else
#        endif
            normalWorld = ApplyNormalWorld(normalWorld,
                                           parameters.NormalTangent,
                                           positionDerivativeXWorld,
                                           positionDerivativeYWorld,
                                           uvDerivativeXMaterial,
                                           uvDerivativeYMaterial);
    }
#    endif
    float3 geometricNormalWorld = cross(positionDerivativeXWorld, positionDerivativeYWorld);
    if (dot(geometricNormalWorld, input.NormalWorld) < 0)
        geometricNormalWorld = -geometricNormalWorld;
    bool isGeometricFront = dot(geometricNormalWorld, viewDirectionWorld) > 0;
    if (isGeometricFront == true && dot(normalWorld, normalWorld) > 0)
    {
        // Smooth or mapped normals can turn past the camera on a visible face.
        normalWorld = ClipShadingNormalToOutgoingHemisphere(normalWorld, viewDirectionWorld);
    }
#    ifdef LUDUS_AUTHORED_INPUTS
    float3 strandWorld = float3(0);
    if (IsHairCard() == true && dot(normalWorld, normalWorld) > 0)
    {
        // The strand comes from the front-side frame, so both sides of a card share one physical direction.
        // An authored tangent wins; otherwise the texture-coordinate derivatives give dP/du and the handedness,
        // and a card without texture coordinates takes ComputeHairCardFallbackTangent's frame.
        float3 tangentWorld = float3(0);
        float handedness = 1;
        if (abs(input.TangentWorld.w) > .5)
        {
            tangentWorld = input.TangentWorld.xyz;
            handedness = input.TangentWorld.w;
        }
        else
        {
            float determinant = hairUvDerivativeX.x * hairUvDerivativeY.y - hairUvDerivativeX.y * hairUvDerivativeY.x;
            // Only the directions matter, so multiply by the determinant's sign instead of dividing by it.
            float orientation = determinant < 0 ? -1 : (determinant > 0 ? 1 : 0);
            tangentWorld =
                (positionDerivativeXWorld * hairUvDerivativeY.y - positionDerivativeYWorld * hairUvDerivativeX.y) *
                orientation;
            float3 bitangentWorld =
                (positionDerivativeYWorld * hairUvDerivativeX.x - positionDerivativeXWorld * hairUvDerivativeY.x) *
                orientation;
            handedness = dot(cross(normalWorld, tangentWorld), bitangentWorld) < 0 ? -1 : 1;
        }
        strandWorld = ComputeHairCardStrand(normalWorld, tangentWorld, handedness, material.HairStrand.y);
        // Two-sided: the back of a card faces the viewer with the opposite normal.
        if (isGeometricFront == false)
            normalWorld = ClipShadingNormalToOutgoingHemisphere(-normalWorld, viewDirectionWorld);
    }
#    endif
    float3 result = material.Emission.w > .5 || isGeometricFront == true ? material.Emission.rgb : float3(0);
#    ifdef LUDUS_AUTHORED_INPUTS
    if (material.SurfaceMap.z > .5)
    {
        float2 emissionUv =
            (material.Authored.y > .5 ? input.MaterialUv1 : input.TexcoordUv) * material.UvTransform.xy +
            material.UvTransform.zw;
        float3 emission = material.Options.y < 0
                              ? emissionTexture.Sample(colorSampler, emissionUv).rgb
                              : emissionTexture.SampleLevel(colorSampler, emissionUv, material.Options.y).rgb;
        result *= emission;
    }
#    endif
    SurfaceSample output;
    output.BaseColor = baseColor;
    output.NormalWorld = normalWorld;
    output.ViewDirectionWorld = viewDirectionWorld;
    output.Metallic = metallic;
    output.Roughness = roughness;
    output.Emission = result;
    output.Alpha = material.Options.z == 2 ? baseColor.a : 1;
#    ifdef LUDUS_AUTHORED_INPUTS
    output.StrandWorld = strandWorld;
    output.SkinProfile = material.SkinScattering.w;
#    endif
    return output;
}
#endif
#ifdef LUDUS_ATMOSPHERE_SURFACE
#    include "renderer_atmosphere_surface.slang"
#    include "renderer_atmosphere_transport.slang"
#endif

#ifdef LUDUS_DEFERRED_MESH_LIGHTS
// Sky visibility of one receiver, accumulated in a single pass over the directions.
struct SkyVisibility
{
    bool IsAvailable;
    // Cosine-weighted fraction of the sky directions in front of the surface that see past the scene; scales
    // environment irradiance from above and below the horizon alike.
    float Irradiance;
    // Visibility about the reflection vector, weighted by a cosine-power lobe that narrows with smoothness.
    float Specular;
    // Visibility applied to light from below the horizon: the same cosine-weighted visibility.
    float Ground;
#    ifdef LUDUS_ATMOSPHERE_SURFACE
    // Atmosphere sky irradiance through the visible directions above the horizon.
    float3 Radiance;
#    endif
};
SkyVisibility EvaluateSkyVisibility(in float3 normalWorld, in float3 reflectionWorld, float roughness, Varying input)
{
    SkyVisibility sky;
    sky.IsAvailable = false;
    sky.Irradiance = 1;
    sky.Specular = 1;
    sky.Ground = 1;
#    ifdef LUDUS_ATMOSPHERE_SURFACE
    sky.Radiance = 0;
#    endif
    int first = GetSkyVisibilityFirstRegion();
    if (first < 0 || (GetEyeFlags(input) & 1) == 0)
        return sky;
    sky.IsAvailable = true;
    // Every map spans the same padded scene bounds, so one texel size, from the first map's horizontal row (two
    // over its width in metres) and interior texels, moves the receiver for all of them.
    float texelWorld =
        2 / (max(length(material.ShadowData[first * SHADOW_REGION_WORDS].xyz), SKY_VISIBILITY_SCALE_EPSILON) *
             float(SKY_VISIBILITY_SLOT_TEXELS - 2));
    float3 positionWorld = input.PositionWorld + normalWorld * (SKY_VISIBILITY_NORMAL_OFFSET_TEXELS * texelWorld);
    const int HOMOGENEOUS_ROW = 3;
    float4 anchor = material.ShadowData[first * SHADOW_REGION_WORDS + HOMOGENEOUS_ROW];
    float3 relativeWorld = input.PositionWorld - anchor.xyz;
    float localTexelMetres = 2 * float(SKY_VISIBILITY_LOCAL_RADIUS_METRES) / float(SKY_VISIBILITY_SLOT_TEXELS - 2);
    float3 localOffsetWorld = normalWorld * (SKY_VISIBILITY_NORMAL_OFFSET_TEXELS * localTexelMetres);
    // Bound use of the local volume along the ray as well as across it. Inside it, each
    // direction keeps its fine sample until the actual filter footprint approaches its tile edge.
    float localRangeWeight =
        anchor.w < 0 ? saturate(2 * float(SKY_VISIBILITY_LOCAL_RADIUS_METRES) - length(relativeWorld)) : 0;
    float2 localAtlasOffsetUv = material.ShadowData[(first + 1) * SHADOW_REGION_WORDS + HOMOGENEOUS_ROW].xy;
    float alpha = max(roughness * roughness, SKY_VISIBILITY_SCALE_EPSILON);
    float exponent = clamp(2 / (alpha * alpha) - 2, 1, SKY_SPECULAR_LOBE_EXPONENT_MAXIMUM);
    float solidAngle = 2 * PI / float(SKY_VISIBILITY_DIRECTION_COUNT);
    float visibleCosine = 0;
    float totalCosine = 0;
    float visibleLobe = 0;
    float totalLobe = 0;
    for (int direction = 0; direction < int(SKY_VISIBILITY_DIRECTION_COUNT); ++direction)
    {
        int region = first + direction;
        float3 skyWorld = GetSkyVisibilityDirection(region);
        float cosine = max(dot(normalWorld, skyWorld), 0) * solidAngle;
        // Metal compiles render stages with fast math, where pow(0, y) may read log2(0); never take it.
        float lobeCosine = dot(reflectionWorld, skyWorld);
        float lobe = lobeCosine > 0 ? pow(lobeCosine, exponent) : 0;
        // Directions behind both the surface and its reflection lobe change neither integral.
        if (cosine <= 0 && lobe <= 0)
            continue;
        float localWeight = 0;
        float2 localClip = 0;
        if (localRangeWeight > 0)
        {
            float3 localPositionWorld = relativeWorld + localOffsetWorld;
            localClip =
                float2(dot(normalize(material.ShadowData[region * SHADOW_REGION_WORDS].xyz), localPositionWorld),
                       dot(normalize(material.ShadowData[region * SHADOW_REGION_WORDS + 1].xyz), localPositionWorld)) /
                float(SKY_VISIBILITY_LOCAL_RADIUS_METRES);
            float edgeTexels =
                (1 - max(abs(localClip.x), abs(localClip.y))) * (0.5 * float(SKY_VISIBILITY_SLOT_TEXELS - 2));
            localWeight = localRangeWeight * saturate(edgeTexels - 1);
        }
        float visible = 0;
        if (localWeight < 1)
            visible += (1 - localWeight) * SampleSkyVisibilityRegion(region, positionWorld, 0, 0, false);
        if (localWeight > 0)
            visible += localWeight * SampleSkyVisibilityRegion(region,
                                                               input.PositionWorld + localOffsetWorld,
                                                               localClip,
                                                               localAtlasOffsetUv,
                                                               true);
        visibleCosine += visible * cosine;
        totalCosine += cosine;
#    ifdef LUDUS_ATMOSPHERE_SURFACE
        float3 skyRadiance = SampleAtmosphereEnvironment(skyWorld);
        sky.Radiance += skyRadiance * (visible * cosine);
        // Weight the reflection lobe by the sky's own brightness, so a blocked bright direction removes its share:
        // under an arcade the glowing sunset horizon is hidden while the dim sky overhead is open, and an
        // unweighted lobe let the reflected horizon through.
        float lobeRadiance = lobe * dot(skyRadiance, SKY_LUMINANCE_WEIGHTS);
        visibleLobe += visible * lobeRadiance;
        totalLobe += lobeRadiance;
#    else
        visibleLobe += visible * lobe;
        totalLobe += lobe;
#    endif
    }
    // The cosine-weighted visibility of the sky directions in front of the surface also scales the light it receives
    // from below the horizon, which no map measures. A surface facing straight down sees no sky direction and none.
    sky.Ground = totalCosine > 0 ? visibleCosine / totalCosine : 0;
    sky.Irradiance = sky.Ground;
    sky.Specular = totalLobe > 0 ? visibleLobe / totalLobe : sky.Ground;
    return sky;
}
#endif
// Separate lighting terms of one shaded surface, for the deferred resolve's lighting views.
struct LightingTerms
{
    float3 SkyDiffuse;
    float3 SkySpecular;
    // Shadowed direct radiance of every analytic light, and of the isolated one.
    float3 Direct;
    float3 Isolated;
    // Largest analytic light by luminance, or -1, and that luminance.
    int DominantLight;
    float DominantLuminance;
    // Cosine-weighted sky visibility about the normal, or -1 without sky visibility maps.
    float SkyVisibility;
    // With shouldSeparateDiffuse: the sky and analytic-light diffuse radiance left out of the result, for a skin
    // resolve that spreads it. Zero otherwise.
    float3 Diffuse;
};
float ComputeLightingLuminance(in float3 radiance)
{
    return dot(radiance, float3(0.2126, 0.7152, 0.0722));
}
float ComputePunctualAttenuation(float distanceSquaredMetres, float rangeMetres)
{
    static const float MINIMUM_DISTANCE_SQUARED_METRES = 1e-4;
    float ratio = distanceSquaredMetres / (rangeMetres * rangeMetres);
    float edge = max(1 - ratio * ratio, 0);
    return edge * edge / max(distanceSquaredMetres, MINIMUM_DISTANCE_SQUARED_METRES);
}
float ComputeSpotConeAttenuation(float directionCosine, float innerCosine, float outerCosine)
{
    static const float MINIMUM_SPOT_COSINE_DELTA = 1e-6;
    float cone = saturate((directionCosine - outerCosine) / max(innerCosine - outerCosine, MINIMUM_SPOT_COSINE_DELTA));
    return cone * cone;
}
// Shade the surface and report its lighting terms. With shouldSeparateDiffuse the diffuse terms go to outTerms.Diffuse
// instead of the returned radiance; otherwise the radiance keeps the summation order of the combined path.
float4 ShadeMaterialTerms(in SurfaceSample surface,
                          in Varying input,
                          int isolatedLight,
                          bool shouldSeparateDiffuse,
                          out LightingTerms outTerms)
{
    outTerms.Diffuse = 0;
    outTerms.SkyDiffuse = 0;
    outTerms.SkySpecular = 0;
    outTerms.Direct = 0;
    outTerms.Isolated = 0;
    outTerms.DominantLight = -1;
    outTerms.DominantLuminance = 0;
    outTerms.SkyVisibility = -1;
    float4 baseColor = surface.BaseColor;
    float3 normalWorld = surface.NormalWorld;
    float3 viewDirectionWorld = surface.ViewDirectionWorld;
    float metallic = surface.Metallic;
    float roughness = surface.Roughness;
    float nv = max(dot(normalWorld, viewDirectionWorld), 0);
    float3 f0 = lerp(float3(0.04), baseColor.rgb, metallic);
    float3 f = f0 + (1 - f0) * ComputeFifth(1 - nv);
#ifdef LUDUS_DEFERRED_MESH_LIGHTS
    SkyVisibility sky = EvaluateSkyVisibility(normalWorld, reflect(-viewDirectionWorld, normalWorld), roughness, input);
#endif
#ifdef LUDUS_ATMOSPHERE_SURFACE
    float3 irradiance = 0;
    float3 specularEnvironment = 0;
    if (nv > 0 && sky.IsAvailable == true)
    {
        EvaluateVisibleAtmosphereEnvironment(normalWorld,
                                             viewDirectionWorld,
                                             roughness,
                                             f0,
                                             nv,
                                             sky.Radiance,
                                             sky.Ground,
                                             sky.Specular,
                                             irradiance,
                                             specularEnvironment);
    }
    else if (nv > 0)
    {
        EvaluateAtmosphereEnvironment(normalWorld,
                                      viewDirectionWorld,
                                      roughness,
                                      f0,
                                      nv,
                                      irradiance,
                                      specularEnvironment);
    }
#else
    // Hemispheric constant environment: sky irradiance above, scaled by w toward the ground (+Y is up).
    float3 irradiance =
        material.EnvironmentIrradiance.rgb * lerp(material.EnvironmentIrradiance.w, 1, normalWorld.y * 0.5 + 0.5);
    float3 specularEnvironment = material.EnvironmentRadiance.rgb * f;
#    ifdef LUDUS_AUTHORED_INPUTS
    if (material.Authored.z > .5)
    {
        irradiance = EvaluateEnvironmentIrradiance(normalWorld);
        float3 reflectionWorld = reflect(-viewDirectionWorld, normalWorld);
        float3 radiance =
            environmentTexture
                .SampleLevel(environmentSampler, ComputeEnvironmentUv(reflectionWorld), roughness * material.Authored.w)
                .rgb *
            material.IrradianceSh[0].w;
        float2 dfg = EvaluateEnvironmentDfg(roughness, nv);
        specularEnvironment = radiance * (f0 * dfg.x + dfg.y);
    }
#    endif
#    ifdef LUDUS_DEFERRED_MESH_LIGHTS
    if (sky.IsAvailable == true)
    {
        irradiance *= sky.Irradiance;
        specularEnvironment *= sky.Specular;
    }
#    endif
#endif
#if !defined(LUDUS_DEFERRED_MESH_LIGHTS) && !defined(LUDUS_GRAPH_REALTIME)
#    ifndef LUDUS_GRAPH_SURFACE_CHART
    if (input.IsBakeValid > 0.5)
#    endif
    {
        float3 bakedIrradiance =
            bakeTexture.SampleLevel(colorSampler, saturate(input.BakeUv) * material.BakeUv.xy + material.BakeUv.zw, 0)
                .rgb *
            material.BakeScale.rgb;
        irradiance = irradiance * material.BakeScale.w + bakedIrradiance;
    }
#    if defined(LUDUS_AUTHORED_INPUTS) && !defined(LUDUS_GRAPH_SURFACE_CHART)
    else if (material.BakeScale.w < 0)
    {
        irradiance += EvaluateGiProbeIrradiance(input.PositionWorld, normalWorld);
    }
#    endif
#endif
    float3 result = surface.Emission;
    if (nv > 0)
    {
        outTerms.SkyDiffuse = irradiance * baseColor.rgb * (1 - metallic) * (1 - f) / PI;
        outTerms.SkySpecular = specularEnvironment;
        if (shouldSeparateDiffuse == true)
        {
            outTerms.Diffuse += outTerms.SkyDiffuse;
            result += outTerms.SkySpecular;
        }
        else
        {
            result += outTerms.SkyDiffuse + outTerms.SkySpecular;
        }
    }
#ifdef LUDUS_DEFERRED_MESH_LIGHTS
    if (sky.IsAvailable == true)
        outTerms.SkyVisibility = sky.Irradiance;
#endif
#ifdef LUDUS_DEBUG_VIEW
    int lightCount = 0;
#endif
#ifdef LUDUS_CLUSTERED
    float4 world = float4(input.PositionWorld, 1);
    float3 grid =
        (float3(dot(material.GridRows[0], world), dot(material.GridRows[1], world), dot(material.GridRows[2], world)) -
         material.GridMinimum.xyz) *
        material.GridScale.xyz;
    bool direct = input.IsDirectReference > 0.5 || any(grid < 0) || any(grid >= 8);
    int3 cell = int3(clamp(grid, float3(0), float3(7)));
    int start = (cell.x + cell.y * 8 + cell.z * 64) * 32;
    float4 first = clusterTexture.SampleLevel(
#    ifdef LUDUS_SURFACE_FIDELITY
        shadowSampler,
#    else
        clusterSampler,
#    endif
        (float2((start / 4) % 64, (start / 4) / 64) + 0.5) / 64,
        0);
    direct = direct || int(first.x * 255 + 0.5) == 255;
    for (int slot = 0; slot < 128; ++slot)
    {
        int i = slot;
        if (direct)
        {
            if (slot >= int(material.Pbr.z))
                break;
        }
        else
        {
            if (slot >= 32)
                break;
            int at = start + slot;
            float4 packed = clusterTexture.SampleLevel(
#    ifdef LUDUS_SURFACE_FIDELITY
                shadowSampler,
#    else
                clusterSampler,
#    endif
                (float2((at / 4) % 64, (at / 4) / 64) + 0.5) / 64,
                0);
            i = int(packed[at % 4] * 255 + 0.5) - 1;
            if (i < 0)
                break;
        }
#else
    for (int i = 0; i < 8; ++i)
    {
        if (i >= int(material.Pbr.z))
            break;
#endif
        float4 color = material.Lights[i * 4];
        float4 position = material.Lights[i * 4 + 1];
        float4 direction = material.Lights[i * 4 + 2];
        float4 extra = material.Lights[i * 4 + 3];
        float3 lightDirectionWorld = -direction.xyz;
        float attenuation = 1;
        if (color.w != 0)
        {
            float3 deltaWorld = position.xyz - input.PositionWorld;
            float distanceSquaredMetres = dot(deltaWorld, deltaWorld);
            lightDirectionWorld = NormalizeOrZero(deltaWorld);
            attenuation = ComputePunctualAttenuation(distanceSquaredMetres, position.w);
            if (color.w == 2)
            {
                attenuation *=
                    ComputeSpotConeAttenuation(dot(-lightDirectionWorld, direction.xyz), extra.x, direction.w);
            }
            if (color.w == 3 || color.w == 4)
            {
                float emitterCosine = dot(-lightDirectionWorld, direction.xyz);
                attenuation *= color.w == 4 ? abs(emitterCosine) : max(emitterCosine, 0);
            }
        }
        float3 lightColor = color.rgb;
#ifdef LUDUS_ATMOSPHERE_SURFACE
        if (i + 1 == int(deferredView.AtmosphereSunWorld.w) && color.w == 0)
        {
            lightColor = EvaluateAtmosphereSun(input.PositionWorld);
            lightDirectionWorld = deferredView.AtmosphereSunWorld.xyz;
        }
#endif
#if defined(LUDUS_GRAPH_REALTIME)
        const bool shouldReplaceDiffuse = false;
#elif defined(LUDUS_GRAPH_SURFACE_CHART)
        const bool shouldReplaceDiffuse = extra.y > 0.5;
#else
    const bool shouldReplaceDiffuse = input.IsBakeValid > 0.5 && extra.y > 0.5;
#endif
        float3 diffuseReflectance = float3(0);
        float3 reflectance;
        if (shouldSeparateDiffuse == true)
        {
            reflectance = EvaluateBrdfLobes(baseColor.rgb,
                                            normalWorld,
                                            viewDirectionWorld,
                                            lightDirectionWorld,
                                            metallic,
                                            roughness,
                                            shouldReplaceDiffuse,
                                            diffuseReflectance);
        }
        else
        {
#ifdef LUDUS_AUTHORED_INPUTS
            reflectance = IsHairCard() == true ? EvaluateHairCard(baseColor.rgb,
                                                                  roughness,
                                                                  normalWorld,
                                                                  surface.StrandWorld,
                                                                  viewDirectionWorld,
                                                                  lightDirectionWorld,
                                                                  shouldReplaceDiffuse)
                                               : EvaluateBrdf(baseColor.rgb,
                                                              normalWorld,
                                                              viewDirectionWorld,
                                                              lightDirectionWorld,
                                                              metallic,
                                                              roughness,
                                                              shouldReplaceDiffuse);
#else
            reflectance = EvaluateBrdf(baseColor.rgb,
                                       normalWorld,
                                       viewDirectionWorld,
                                       lightDirectionWorld,
                                       metallic,
                                       roughness,
                                       shouldReplaceDiffuse);
#endif
        }
        float shadowVisibility = ComputeShadowVisibility(i, input);
        float3 contribution = reflectance * lightColor * attenuation * shadowVisibility;
        outTerms.Diffuse += diffuseReflectance * lightColor * attenuation * shadowVisibility;
        result += contribution;
        outTerms.Direct += contribution;
        if (i == isolatedLight)
            outTerms.Isolated = contribution;
        float luminance = ComputeLightingLuminance(contribution);
        if (luminance > outTerms.DominantLuminance)
        {
            outTerms.DominantLight = i;
            outTerms.DominantLuminance = luminance;
        }
#ifdef LUDUS_DEBUG_VIEW
        lightCount += any(contribution > 0) ? 1 : 0;
#endif
    }
#ifdef LUDUS_DEBUG_VIEW
    int debugView = int(material.Options.w + 0.5);
    if (debugView != int(MATERIAL_DEBUG_VIEW_NONE))
        return float4(EncodeDebugDisplay(
                          ComputeDebugColour(debugView, input, normalWorld, metallic, roughness, lightCount, result)),
                      1);
#endif
    float alpha = surface.Alpha;
    outTerms.Diffuse = min(max(outTerms.Diffuse, float3(0)), float3(65504)) * alpha;
    return float4(min(max(result, float3(0)), float3(65504)) * alpha, alpha);
}
float4 ShadeMaterial(in SurfaceSample surface, in Varying input)
{
    LightingTerms terms;
    return ShadeMaterialTerms(surface, input, -1, false, terms);
}
#ifndef LUDUS_DEFERRED_MESH_LIGHTS
[shader("fragment")] float4 FragmentMain(Varying input) : SV_Target
{
    return ShadeMaterial(SampleMaterial(input), input);
}
// Masked coverage for a shadow map. A mesh-light cube face's instance tags its colour red, which IsBakeValid carries,
// and gives in EyeWorld the world-space row of its face projection's clip z, the linear depth it stores
// (SelectShadowDepth); PositionWorld is exact at the fragment, so the row needs no further varying.
[shader("fragment")] float4 ShadowMain(Varying input) : SV_Target
{
    float linearDepth = dot(input.PositionWorld, input.EyeWorld.xyz) + input.EyeWorld.w;
    float4 encoded = EncodeShadowDepth(SelectShadowDepth(input.PositionClip.z, input.IsBakeValid, linearDepth));
    SampleBaseTexel(input);
    return encoded;
}
#endif

#ifdef LUDUS_CLUSTERED
// The prepass retains the exact color vertex entry and evaluates no material terms.
// This profile accepts opaque coverage only; masked coverage needs its own qualified entry.
void OpaqueDepthMain(in Varying input) {}
#endif
