#pragma once

// SceneIR v1: the format-neutral intermediate representation produced by the
// best-effort front-ends (pbrt-v4, glTF 2.0/GLB and Wavefront OBJ/MTL) and
// consumed by the budget planner. Geometry stays in object space with an
// object-to-world transform, and instancing (pbrt ObjectBegin/ObjectInstance,
// glTF meshes referenced by several nodes) is preserved so the planner decides
// whether to expand it. Materials are already mapped to the engine's PBR terms.
//
// The IR keeps the complete source data that a front-end can express, without
// the LUSCN v4 capacities: the planner, not the parsers, fits a scene to the
// cooked format. Fields marked "not stored by LUSCN v4" (tangents, secondary
// material textures, alpha, sidedness, camera clip planes, light range) are
// reported as dropped by the cook and kept for a later format revision.

#include <ludus/content/content.h>
#include <ludus/foundation/base/core.h>
#include <ludus/foundation/containers/array.hpp>
#include <ludus/foundation/math/matrix.hpp>
#include <ludus/foundation/math/vector.hpp>
#include <ludus/foundation/strings/string.hpp>

namespace ludus::content::scene_cook::importers
{
using foundation::Array;
using foundation::String;
using foundation::core::float32;
using foundation::core::float64;
using foundation::core::int32;
using foundation::core::uint32;
using foundation::core::uint8;
using foundation::core::usize;
namespace math = foundation::math;

/// Index value for "no entry" in IR cross references.
inline constexpr uint32 IR_NONE = ~uint32{0};
/// Channels of an IR linear RGB colour (BaseColour, Emission, Radiance, Scale).
inline constexpr usize IR_RGB_CHANNEL_COUNT = 3;
/// Components of an IR position, normal or direction (IrMesh::Positions/Normals stride).
inline constexpr usize IR_VECTOR_COMPONENT_COUNT = 3;
/// Components of an IR tangent: xyz and the bitangent sign w (IrMesh::Tangents stride).
inline constexpr usize IR_TANGENT_COMPONENT_COUNT = 4;
/// Corners of an IR triangle (IrMesh::Indices stride).
inline constexpr usize IR_TRIANGLE_CORNER_COUNT = 3;
/// Default decoding exponent for TextureEncoding::Gamma textures.
inline constexpr float64 IR_DEFAULT_TEXTURE_GAMMA = 2.2;
/// Default vertical field of view: 45 degrees in radians.
inline constexpr float64 IR_DEFAULT_VERTICAL_FOV_RADIANS = 0.785398163397448;
/// Default spot cone angles in degrees (pbrt spot coneangle 30, conedelta 5).
inline constexpr float64 IR_DEFAULT_SPOT_INNER_DEGREES = 25.0;
inline constexpr float64 IR_DEFAULT_SPOT_OUTER_DEGREES = 30.0;

/// Colour encoding of a texture's source texels.
enum class TextureEncoding : uint8
{
    Srgb,
    Linear,
    Gamma,
};

/// Encoded image bytes carried inside a source file (a GLB binary chunk, a glTF
/// buffer view or a base64 data URI) rather than in a separate image file.
struct IrEmbeddedImage final
{
    /// Report label, for example "scene.glb#images[0]".
    String Label;
    /// File name hint whose extension selects the decoder when the magic bytes do not
    /// ("embedded.png", "embedded.jpg"); derived from the declared media type.
    String DecoderName;
    /// Encoded image bytes.
    Array<uint8> Bytes;
};

/// A texture referenced by at least one material.
struct IrTexture final
{
    /// Declared name (pbrt `Texture "name"`, glTF texture name or MTL map file).
    String Name;
    /// Root-relative path of the image file; empty for constant and embedded textures.
    String File;
    /// Index into SceneIr::EmbeddedImages, or IR_NONE when the image is a file.
    uint32 EmbeddedImage = IR_NONE;
    /// Source texture coordinate set (glTF texCoord); only set 0 reaches the IR meshes.
    uint32 TexCoordSet = 0;
    /// Constant colour for constant-valued textures, or the multiplier for images.
    float64 Scale[IR_RGB_CHANNEL_COUNT] = {1.0, 1.0, 1.0};
    TextureEncoding Encoding = TextureEncoding::Srgb;
    float64 Gamma = IR_DEFAULT_TEXTURE_GAMMA;
    bool IsInverted = false;
    bool IsRepeated = true;
    /// Source location of the declaration.
    uint32 SourceFile = IR_NONE;
    uint32 Line = 0;
};

/// How a material's base-colour alpha is interpreted (glTF alphaMode, MTL d/Tr).
enum class IrAlphaMode : uint8
{
    Opaque,
    Mask,
    Blend,
};

/// How IrMaterial::NormalTexture encodes surface detail.
enum class IrNormalTextureKind : uint8
{
    /// Tangent-space normal map (glTF normalTexture, MTL norm).
    TangentSpace,
    /// Scalar height map (MTL bump/map_Bump).
    Height,
};

/// A surface material mapped to the engine's metallic-roughness model.
struct IrMaterial final
{
    /// Declared name; empty for anonymous `Material` directives.
    String Name;
    /// Source material type (diffuse, conductor, ...), kept for the report.
    String SourceType;
    /// Linear RGB base colour (multiplied with the texture when present).
    float64 BaseColour[IR_RGB_CHANNEL_COUNT] = {.5, .5, .5};
    /// Index into SceneIr::Textures or IR_NONE.
    uint32 BaseColourTexture = IR_NONE;
    float64 Metallic = 0.0;
    float64 Roughness = 1.0;
    /// Texture whose MetallicChannel scales Metallic (glTF metallicRoughnessTexture
    /// blue channel, MTL map_Pm); IR_NONE when absent. The cook averages it to a scalar.
    uint32 MetallicTexture = IR_NONE;
    uint8 MetallicChannel = 2;
    /// Texture whose RoughnessChannel scales Roughness (glTF green channel, MTL map_Pr).
    uint32 RoughnessTexture = IR_NONE;
    uint8 RoughnessChannel = 1;
    /// Linear RGB emitted radiance factor in nits (glTF emissiveFactor x
    /// KHR_materials_emissive_strength, MTL Ke); zero for non-emitters. Front-ends copy
    /// it to the shapes that use the material as optional emission.
    float64 Emission[IR_RGB_CHANNEL_COUNT] = {0.0, 0.0, 0.0};
    /// Emission texture multiplying Emission; not stored by LUSCN v4.
    uint32 EmissionTexture = IR_NONE;
    /// Normal or height detail texture; not stored by LUSCN v4.
    uint32 NormalTexture = IR_NONE;
    IrNormalTextureKind NormalTextureKind = IrNormalTextureKind::TangentSpace;
    /// glTF normalTexture.scale or MTL -bm multiplier.
    float64 NormalScale = 1.0;
    /// Ambient occlusion texture (red channel) and strength; not stored by LUSCN v4.
    uint32 OcclusionTexture = IR_NONE;
    float64 OcclusionStrength = 1.0;
    /// Specular colour or exponent textures of Phong-style sources (MTL map_Ks,
    /// map_Ns); kept for the report, not used by the metallic-roughness mapping.
    uint32 SpecularTexture = IR_NONE;
    uint32 SpecularExponentTexture = IR_NONE;
    /// Base-colour alpha factor in [0, 1] (glTF baseColorFactor[3], MTL d) and its
    /// separate opacity texture (MTL map_d); not stored by LUSCN v4.
    float64 Opacity = 1.0;
    uint32 OpacityTexture = IR_NONE;
    IrAlphaMode AlphaMode = IrAlphaMode::Opaque;
    float64 AlphaCutoff = 0.5;
    /// Both faces are shaded (glTF doubleSided); not stored by LUSCN v4.
    bool IsDoubleSided = false;
    /// True when the source material had no faithful mapping.
    bool IsApproximated = false;
    uint32 SourceFile = IR_NONE;
    uint32 Line = 0;
};

/// Indexed triangle mesh in object space.
struct IrMesh final
{
    /// xyz triples.
    Array<float32> Positions;
    /// xyz triples, or empty when the source has none (computed by the planner).
    Array<float32> Normals;
    /// uv pairs with v already flipped to the LUSCN top-down convention, or empty.
    Array<float32> Uvs;
    /// Tangent xyzw quadruples (w = bitangent sign), or empty; not stored by LUSCN v4.
    Array<float32> Tangents;
    /// Triangle list.
    Array<uint32> Indices;

    [[nodiscard]] usize GetVertexCount() const noexcept
    {
        return Positions.GetSize() / IR_VECTOR_COMPONENT_COUNT;
    }
};

/// One placed shape: a mesh with its material, emission and transform.
struct IrShape final
{
    uint32 Mesh = IR_NONE;
    /// Index into SceneIr::Materials (IR_NONE selects the default material).
    uint32 Material = IR_NONE;
    /// Linear RGB radiance from an enclosing AreaLightSource; zero otherwise.
    float64 Emission[IR_RGB_CHANNEL_COUNT] = {0.0, 0.0, 0.0};
    /// Source area-light identity; IR_NONE means no enclosing area light.
    uint32 AreaLight = IR_NONE;
    /// PBRT diffuse emitters default to front-side emission only.
    bool IsEmissionTwoSided = false;
    /// Decorative material emission (glTF emissive, MTL Ke). When emitter capacity or
    /// the radiance profile cannot hold it, the planner cooks the surface as a
    /// non-emitter with EMISSION_DROPPED instead of failing. pbrt area lights are required.
    bool IsEmissionOptional = false;
    /// Optional source photometric power in lumens; normalized after surface area is known.
    bool HasEmissionPower = false;
    float64 EmissionPowerLumens = 0.0;
    float64 EmissionScale = 1.0;
    /// Object-to-world (or object-to-instance inside ObjectBegin).
    math::Matrix4d Transform = math::Matrix4d::Identity();
    /// ReverseOrientation was active; the bake combines it with the handedness of
    /// the full placement transform (instance included).
    bool IsOrientationReversed = false;
    uint32 SourceFile = IR_NONE;
    uint32 Line = 0;
};

/// A named ObjectBegin/ObjectEnd shape group.
struct IrObject final
{
    String Name;
    /// Indices into SceneIr::Shapes owned by this object (not placed in the world directly).
    Array<uint32> Shapes;
};

/// One ObjectInstance placement.
struct IrInstance final
{
    uint32 Object = IR_NONE;
    /// Instance-to-world.
    math::Matrix4d Transform = math::Matrix4d::Identity();
};

/// Camera derived from the pbrt Camera directive.
struct IrCamera final
{
    bool IsPresent = false;
    math::Vector3d Eye{0.0, 0.0, 0.0};
    math::Vector3d Target{0.0, 0.0, 1.0};
    math::Vector3d Up{0.0, 1.0, 0.0};
    /// Vertical field of view in radians.
    float64 VerticalFov = IR_DEFAULT_VERTICAL_FOV_RADIANS;
    /// Declared width / height, or zero when the source leaves it to the viewport;
    /// not stored by LUSCN v4.
    float64 AspectRatio = 0.0;
    /// Declared clip distances in source units (zero far = infinite); not stored by LUSCN v4.
    float64 NearClip = 0.0;
    float64 FarClip = 0.0;
};

/// Environment fallback for infinite lights.
struct IrEnvironment final
{
    bool IsPresent = false;
    /// Mean linear RGB radiance (L x scale; image maps are averaged when decodable).
    float64 Radiance[IR_RGB_CHANNEL_COUNT] = {0.0, 0.0, 0.0};
    float64 Scale = 1.0;
    bool HasIlluminance = false;
    float64 IlluminanceLux = 0.0;
    math::Matrix4d LightToWorld = math::Matrix4d::Identity();
    /// Root-relative path of the environment image, empty for constant L.
    String File;
};

/// Source analytic model, independent of renderer/runtime ownership.
enum class IrLightKind : uint8
{
    Directional,
    Point,
    Spot,
};
/// Authored light terms retained before the frozen RGB/photometric cook conversion.
struct IrAnalyticLight final
{
    IrLightKind Kind = IrLightKind::Point;
    /// Point/spot RGB radiant intensity or directional RGB irradiance, multiplied by scale.
    math::Vector3d Strength{1.0, 1.0, 1.0};
    /// Retained separately because photometric spectrum normalization cancels RGB magnitude.
    float64 Scale = 1.0;
    /// World-space origin in source meters; direction points from the light into the scene.
    math::Vector3d PositionWorld{};
    math::Vector3d DirectionWorld{0.0, 0.0, 1.0};
    float64 InnerAngleDegrees = IR_DEFAULT_SPOT_INNER_DEGREES;
    float64 OuterAngleDegrees = IR_DEFAULT_SPOT_OUTER_DEGREES;
    /// Declared attenuation range in metres (glTF range); zero means unbounded.
    /// Not stored by LUSCN v4, whose cook derives one range from the scene extent.
    float64 RangeMetres = 0.0;
    /// Optional source photometric normalization: lumens for point/spot, lux for directional.
    bool HasPhotometricNormalization = false;
    float64 PhotometricNormalization = 0.0;
    uint32 SourceFile = IR_NONE;
    uint32 Line = 0;
};

/// Source directives counted by kind, independent of whether cooking retains them.
struct IrLightCounts final
{
    uint32 Area = 0;
    uint32 Infinite = 0;
    uint32 Point = 0;
    uint32 Spot = 0;
    uint32 Distant = 0;
    uint32 Other = 0;
};

/// Complete best-effort scene description.
struct SceneIr final
{
    /// Root-relative source files in first-visit order; shapes refer to them by index.
    Array<String> SourceFiles;
    /// Root-relative binary data files (pbrt PLY meshes, external glTF buffers) in
    /// first-use order.
    Array<String> DataFiles;
    /// SHA-256 of the bytes read from each SourceFiles entry (parallel array).
    Array<Digest> SourceDigests;
    /// SHA-256 of the bytes read from each DataFiles entry (parallel array).
    Array<Digest> DataDigests;
    Array<IrTexture> Textures;
    /// Images whose encoded bytes live inside a source file; see IrTexture::EmbeddedImage.
    Array<IrEmbeddedImage> EmbeddedImages;
    Array<IrMaterial> Materials;
    Array<IrMesh> Meshes;
    Array<IrShape> Shapes;
    /// Shapes placed directly in the world (not owned by an object).
    Array<uint32> WorldShapes;
    Array<IrObject> Objects;
    Array<IrInstance> Instances;
    IrCamera Camera;
    IrEnvironment Environment;
    IrLightCounts Lights;
    Array<IrAnalyticLight> AnalyticLights;
    /// Film resolution when declared; zero otherwise.
    uint32 FilmWidth = 0;
    uint32 FilmHeight = 0;
};
} // namespace ludus::content::scene_cook::importers
