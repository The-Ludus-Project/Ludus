#pragma once

#include <ludus/foundation/base/types.h>

#include <ludus/content/scene_format/limits.h>

#include <cstddef>
#include <string_view>
#include <type_traits>

/// @file
/// LUSCN cooked scene interchange format (Ludus::SceneFormat).
///
/// This header is the single contract between producers of cooked scenes
/// (Ludus::SceneCook, the Editor World composer) and consumers
/// (Ludus::Resources SceneDefinition::Decode and the renderer that installs it).
/// It is owned by neither side: tools and runtime both depend on it, and it
/// depends on nothing but FoundationBase.
///
/// A bundle is little-endian and tightly packed, in this order:
/// Header; Header::TextureCount x (TextureRecord + RGBA8 bytes);
/// Header::MaterialCount x versioned MaterialRecord;
/// Header::MeshCount x (MeshRecord + MeshRecord::VertexCount x VertexRecord +
/// MeshRecord::IndexCount x uint32); Header::ObjectCount x versioned ObjectRecord.
/// Version 3 uses VertexRecordV3 and ends with LightingRecord, analytic lights
/// and its optional float32 RGB environment payload. Version 4 keeps every
/// version 3 record except the vertices, which are VertexRecord again: bake
/// chart coordinates are implicit (see TryGetBakeChartUv), so meshes stay
/// indexed instead of storing one vertex per triangle corner.
///
/// The record structs document the exact wire layout and are checked with
/// static_asserts; serialise them field by field in little-endian order rather
/// than copying the structs, so host endianness and padding never leak into files.
namespace ludus::content::scene_format
{
using foundation::core::float32;
using foundation::core::uint32;
using foundation::core::uint64;
using foundation::core::usize;

/// Eight-byte file magic, including the trailing NUL.
inline constexpr std::string_view MAGIC{"LUSCN01\0", 8};
/// Version 1: materials without an address mode (MaterialRecordV1).
inline constexpr uint32 VERSION_1 = 1;
/// Version 2: materials carry an explicit address mode (MaterialRecordV2).
inline constexpr uint32 VERSION_2 = 2;
/// Version 3: sided emission, source IDs, bake coordinates and authored lighting.
inline constexpr uint32 VERSION_3 = 3;
/// Version 4: version 3 records with indexed VertexRecord meshes. Bake chart
/// coordinates are not stored; consumers derive them per triangle corner.
inline constexpr uint32 VERSION_4 = 4;
/// Linear Rec.709 RGB photometric proxy; not a spectral PBRT representation.
inline constexpr uint32 LIGHTING_PROFILE_REC709_PHOTOMETRIC = 1;
/// Largest analytic light list; selection must never silently discard sources.
inline constexpr uint32 MAX_LIGHTS = 128;
/// Largest HDR environment dimension; its RGB payload is also bundle-size bounded.
inline constexpr uint32 MAX_ENVIRONMENT_DIMENSION = 4096;
/// Three tightly packed float32 channels per environment texel.
inline constexpr uint32 ENVIRONMENT_TEXEL_BYTES = 3 * sizeof(float32);
/// Material emission is visible and transports light on both geometric sides.
inline constexpr uint32 EMISSION_TWO_SIDED = 1;
/// EmissionPower is an unresolved per-instance total luminous flux in lumens.
inline constexpr uint32 EMISSION_HAS_POWER = 2;
/// All admitted material emission flags.
inline constexpr uint32 EMISSION_FLAGS = EMISSION_TWO_SIDED | EMISSION_HAS_POWER;
/// Analytic directional illuminance in RGB lux.
inline constexpr uint32 LIGHT_DIRECTIONAL = 0;
/// Analytic point intensity in RGB candela.
inline constexpr uint32 LIGHT_POINT = 1;
/// Analytic spot intensity in RGB candela, with cosine cone falloff.
inline constexpr uint32 LIGHT_SPOT = 2;
/// Enabled authored light. Disabled sources retain identity and authoring values.
inline constexpr uint32 LIGHT_ENABLED = 1;
/// Largest admitted analytic RGB intensity or unresolved luminous flux.
inline constexpr float32 MAX_LIGHT_INTENSITY = 1000000.0F;

/// RGBA8 bytes per texel.
inline constexpr uint32 TEXEL_BYTES = 4;
/// MaterialRecord::Texture value for an untextured surface.
inline constexpr uint32 NO_TEXTURE = ~uint32{0};
/// MaterialRecordV2::AddressMode: clamp texture coordinates at the borders.
inline constexpr uint32 ADDRESS_CLAMP = 0;
/// MaterialRecordV2::AddressMode: repeat texture coordinates.
inline constexpr uint32 ADDRESS_REPEAT = 1;
/// Smallest admitted material roughness.
inline constexpr float32 MIN_ROUGHNESS = .045F;
/// Largest admitted emission per channel.
inline constexpr float32 MAX_EMISSION = 10000.0F;

/// Bundle header.
struct Header final
{
    char Magic[8];        ///< MAGIC.
    uint32 Version;       ///< VERSION_1 through VERSION_4.
    uint32 MeshCount;     ///< 1..MAX_MESHES.
    uint32 MaterialCount; ///< 1..MAX_MATERIALS.
    uint32 TextureCount;  ///< 0..MAX_TEXTURES.
    uint32 ObjectCount;   ///< 1..MAX_OBJECTS.
    float32 Eye[3];       ///< Camera position in world units.
    float32 Target[3];    ///< Camera look-at target.
    float32 Up[3];        ///< Camera up direction.
    float32 VerticalFov;  ///< Vertical field of view in radians, in (0, pi).
};
/// Per-texture header, followed by ByteCount RGBA8 sRGB bytes (rows top to bottom).
struct TextureRecord final
{
    uint32 Width;     ///< 1..MAX_TEXTURE_DIMENSION.
    uint32 Height;    ///< 1..MAX_TEXTURE_DIMENSION.
    uint32 ByteCount; ///< Width * Height * TEXEL_BYTES.
};
/// Version 1 material.
struct MaterialRecordV1 final
{
    uint32 Texture;      ///< Texture index or NO_TEXTURE.
    float32 Metallic;    ///< [0, 1].
    float32 Roughness;   ///< [MIN_ROUGHNESS, 1].
    float32 Emission[3]; ///< Linear RGB radiance, [0, MAX_EMISSION].
};
/// Version 2 material: adds AddressMode after Texture.
struct MaterialRecordV2 final
{
    uint32 Texture;      ///< Texture index or NO_TEXTURE.
    uint32 AddressMode;  ///< ADDRESS_CLAMP or ADDRESS_REPEAT.
    float32 Metallic;    ///< [0, 1].
    float32 Roughness;   ///< [MIN_ROUGHNESS, 1].
    float32 Emission[3]; ///< Linear RGB radiance, [0, MAX_EMISSION].
};
/// Version 3 and 4 material. Power is resolved independently for each placed emitter;
/// otherwise Emission is already resolved RGB luminance in nits (cd/m²).
/// Source IDs identify authored equivalence groups, not individual triangles.
struct MaterialRecordV3 final
{
    uint32 Texture;        ///< Texture index or NO_TEXTURE.
    uint32 AddressMode;    ///< ADDRESS_CLAMP or ADDRESS_REPEAT.
    float32 Metallic;      ///< [0, 1].
    float32 Roughness;     ///< [MIN_ROUGHNESS, 1].
    float32 Emission[3];   ///< Nonnegative linear Rec.709 RGB, at most MAX_EMISSION.
    uint32 EmissionFlags;  ///< EMISSION_TWO_SIDED and/or EMISSION_HAS_POWER.
    uint64 AreaSourceId;   ///< Nonzero for emissive surfaces; zero for nonemitters.
    float32 EmissionPower; ///< Lumens after authoring scale; zero without HAS_POWER.
    uint32 Reserved;       ///< Must be zero.
};
/// Indices per triangle in a mesh's triangle list; MeshRecord::IndexCount is a multiple.
inline constexpr uint32 TRIANGLE_CORNERS = 3;
/// Per-mesh header, followed by its vertices and uint32 triangle-list indices.
struct MeshRecord final
{
    uint32 VertexCount; ///< 1..MAX_VERTICES.
    uint32 IndexCount;  ///< 1..MAX_INDICES, a multiple of three.
};
/// Interleaved mesh vertex (versions 1, 2 and 4).
struct VertexRecord final
{
    float32 Position[3]; ///< Mesh-local position.
    float32 Normal[3];   ///< Nonzero shading normal.
    float32 Uv[2];       ///< Mip-zero texture coordinate.
};
/// Version 3 vertex with separate finite [0,1] irradiance bake coordinates.
/// Nonoverlapping charts and sufficient gutters are a producer/consumer contract.
struct VertexRecordV3 final
{
    float32 Position[3]; ///< Mesh-local position.
    float32 Normal[3];   ///< Nonzero shading normal.
    float32 Uv[2];       ///< Mip-zero surface texture coordinate.
    float32 BakeUv[2];   ///< Independent normalized bake chart coordinate.
};
/// Number of float32 components in a VertexRecord.
inline constexpr usize VERTEX_COMPONENTS = 8;
/// Placed mesh instance.
struct ObjectRecord final
{
    uint32 Mesh;            ///< Index into the meshes.
    uint32 Material;        ///< Index into the materials.
    float32 Linear[9];      ///< Column-major 3x3 linear part (X, Y, Z columns).
    float32 Translation[3]; ///< World translation.
};
/// Version 3 and 4 placed instance, adding a stable nonzero scene-local identity.
struct ObjectRecordV3 final
{
    uint32 Mesh;            ///< Mesh index.
    uint32 Material;        ///< Material index.
    float32 Linear[9];      ///< Mesh-to-world linear columns.
    float32 Translation[3]; ///< World translation.
    uint64 SourceId;        ///< Unique among placed objects; stable across recooking.
};
/// Version 3 and 4 trailer followed by LightCount LightRecords, then optional HDR RGB.
/// Environment pixels are equirectangular, rows north to south, with linear RGB
/// radiance in nits multiplied by EnvironmentRgb. Zero dimensions select the
/// constant EnvironmentRgb radiance; zero RGB means no authored environment.
struct LightingRecord final
{
    uint32 Profile;            ///< LIGHTING_PROFILE_REC709_PHOTOMETRIC.
    uint32 LightCount;         ///< 0..MAX_LIGHTS.
    uint32 EnvironmentWidth;   ///< Both dimensions zero, or 1..MAX_ENVIRONMENT_DIMENSION.
    uint32 EnvironmentHeight;  ///< Same dimension contract as EnvironmentWidth.
    float32 EnvironmentRgb[3]; ///< Nonnegative finite RGB scale or constant nits.
};
/// Version 3 and 4 analytic light. Scene world distances are metres.
/// RGB values are a fixed Rec.709 luminance-normalized proxy, rather than PBRT
/// spectral power. Spot inner cosine is strictly greater than outer cosine.
struct LightRecord final
{
    uint64 SourceId;           ///< Stable nonzero identity unique among analytic lights.
    uint32 Kind;               ///< LIGHT_DIRECTIONAL, LIGHT_POINT or LIGHT_SPOT.
    uint32 Flags;              ///< Zero or LIGHT_ENABLED; unknown bits are invalid.
    float32 IntensityRgb[3];   ///< Nonnegative lux (directional) or candela (point/spot).
    float32 PositionWorld[3];  ///< Position in metres, components [-1e6,1e6]; irrelevant for directional.
    float32 DirectionWorld[3]; ///< Nonzero outgoing direction; normalized by adapter.
    float32 Range;             ///< Attenuation/shadow range in metres, [.01,1e6].
    float32 InnerCos;          ///< (-1,1], inner full-intensity cone cosine.
    float32 OuterCos;          ///< (-1,InnerCos), outer zero-intensity cone cosine.
};
/// Number of float32 components in ObjectRecord::Linear plus Translation.
inline constexpr usize OBJECT_TRANSFORM_COMPONENTS = 12;

/// Fixed triangle chart tile width: a two-texel interior and one-texel gutter.
inline constexpr uint32 BAKE_CHART_TILE_PIXELS = 4;
/// Largest bake atlas width/height; the producer must also budget its payload.
inline constexpr uint32 MAX_BAKE_CHART_DIMENSION = 4096;
/// Allocation-free chart layout shared by tools and runtime baking.
/// Each indexed triangle receives its own tile, so a triangle corner's chart
/// coordinate depends only on its triangle and corner number. Version 3
/// producers duplicate vertices and store the coordinates; version 4 consumers
/// duplicate vertices when they need charts and assign the same coordinates.
/// Different placements need independent irradiance payloads because world
/// transforms and visibility differ.
struct BakeChartLayout final
{
    uint32 Width = 0;         ///< Power-of-two atlas width in texels.
    uint32 Height = 0;        ///< Power-of-two atlas height in texels.
    uint32 TriangleCount = 0; ///< Number of densely packed row-major chart tiles.
};
/// Choose the smallest square power-of-two width and smallest sufficient
/// power-of-two height for a positive triangle count. No allocation occurs.
/// @return False on zero count or capacity overflow, preserving outLayout.
[[nodiscard]] constexpr bool TryMakeBakeChartLayout(uint32 triangleCount, BakeChartLayout& outLayout) noexcept
{
    if (triangleCount == 0)
    {
        return false;
    }
    uint32 width = BAKE_CHART_TILE_PIXELS;
    while (uint64(width / BAKE_CHART_TILE_PIXELS) * (width / BAKE_CHART_TILE_PIXELS) < triangleCount)
    {
        if (width == MAX_BAKE_CHART_DIMENSION)
        {
            return false;
        }
        width *= 2;
    }
    uint32 height = BAKE_CHART_TILE_PIXELS;
    const uint32 columns = width / BAKE_CHART_TILE_PIXELS;
    while (uint64(columns) * (height / BAKE_CHART_TILE_PIXELS) < triangleCount)
    {
        height *= 2;
    }
    outLayout = { .Width = width, .Height = height, .TriangleCount = triangleCount };
    return true;
}
/// Return a triangle corner's normalized atlas coordinate at interior texel
/// centres. Corners 0/1/2 are respectively lower-left/lower-right/upper-left
/// in increasing row coordinates. The fourth interior texel and gutter must
/// receive extrapolated chart irradiance; sample this atlas without mipmaps.
/// @pre layout was returned by TryMakeBakeChartLayout and is unchanged.
/// @return False for an invalid triangle/corner, preserving both outputs.
[[nodiscard]] constexpr bool TryGetBakeChartUv(const BakeChartLayout& layout,
                                               uint32 triangleIndex,
                                               uint32 corner,
                                               float32& outU,
                                               float32& outV) noexcept
{
    if (layout.Width < BAKE_CHART_TILE_PIXELS || layout.Height < BAKE_CHART_TILE_PIXELS ||
        triangleIndex >= layout.TriangleCount || corner >= 3)
    {
        return false;
    }
    const uint32 columns = layout.Width / BAKE_CHART_TILE_PIXELS;
    const uint32 x = (triangleIndex % columns) * BAKE_CHART_TILE_PIXELS;
    const uint32 y = (triangleIndex / columns) * BAKE_CHART_TILE_PIXELS;
    outU = (float32(x) + (corner == 1 ? 2.5F : 1.5F)) / float32(layout.Width);
    outV = (float32(y) + (corner == 2 ? 2.5F : 1.5F)) / float32(layout.Height);
    return true;
}

static_assert(sizeof(float32) == 4 && sizeof(uint32) == 4);
static_assert(std::is_trivially_copyable_v<Header> && std::is_standard_layout_v<Header>);
static_assert(sizeof(Header) == 68 && offsetof(Header, Version) == 8 && offsetof(Header, Eye) == 28 &&
              offsetof(Header, VerticalFov) == 64);
static_assert(sizeof(TextureRecord) == 12);
static_assert(sizeof(MaterialRecordV1) == 24 && offsetof(MaterialRecordV1, Emission) == 12);
static_assert(sizeof(MaterialRecordV2) == 28 && offsetof(MaterialRecordV2, Metallic) == 8);
static_assert(sizeof(MeshRecord) == 8);
static_assert(sizeof(VertexRecord) == 32 && sizeof(VertexRecord) == VERTEX_COMPONENTS * sizeof(float32) &&
              offsetof(VertexRecord, Normal) == 12 && offsetof(VertexRecord, Uv) == 24);
static_assert(sizeof(ObjectRecord) == 56 && offsetof(ObjectRecord, Translation) == 44 &&
              sizeof(ObjectRecord) == 2 * sizeof(uint32) + OBJECT_TRANSFORM_COMPONENTS * sizeof(float32));
static_assert(sizeof(MaterialRecordV3) == 48 && offsetof(MaterialRecordV3, EmissionFlags) == 28 &&
              offsetof(MaterialRecordV3, AreaSourceId) == 32 && offsetof(MaterialRecordV3, EmissionPower) == 40);
static_assert(sizeof(VertexRecordV3) == 40 && offsetof(VertexRecordV3, BakeUv) == 32);
static_assert(sizeof(ObjectRecordV3) == 64 && offsetof(ObjectRecordV3, SourceId) == 56);
static_assert(sizeof(LightingRecord) == 28 && offsetof(LightingRecord, EnvironmentRgb) == 16);
static_assert(sizeof(LightRecord) == 64 && offsetof(LightRecord, IntensityRgb) == 16 &&
              offsetof(LightRecord, PositionWorld) == 28 && offsetof(LightRecord, DirectionWorld) == 40 &&
              offsetof(LightRecord, Range) == 52);
static_assert(MAGIC.size() == sizeof(Header::Magic));

/// True for a version this header describes.
[[nodiscard]] constexpr bool IsSupportedVersion(uint32 version) noexcept
{
    return version >= VERSION_1 && version <= VERSION_4;
}
/// True when the version carries version 3 materials, object source IDs and the
/// lighting trailer (versions 3 and 4).
[[nodiscard]] constexpr bool HasLightingRecords(uint32 version) noexcept
{
    return version >= VERSION_3;
}
/// True when each vertex stores its bake chart coordinate (VertexRecordV3).
[[nodiscard]] constexpr bool HasStoredBakeCoordinates(uint32 version) noexcept
{
    return version == VERSION_3;
}
/// Bytes of one serialized vertex for a supported version.
[[nodiscard]] constexpr usize GetVertexRecordBytes(uint32 version) noexcept
{
    return HasStoredBakeCoordinates(version) == true ? sizeof(VertexRecordV3) : sizeof(VertexRecord);
}
} // namespace ludus::content::scene_format
