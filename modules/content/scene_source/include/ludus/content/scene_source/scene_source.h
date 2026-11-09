#pragma once

// Ludus scene source (`.luscene.json`), version 1: a hand-readable JSON scene
// that Ludus owns. It holds world-space convex polygon geometry, parametric
// aperture screens, materials with emission and an optional camera, and is the
// source of the engine's test scenes.
// SceneCook's `ludus-scene-v1` importer cooks it into a LUSCN bundle like any
// other source; tests read it directly. The file layout, conventions and limits
// are documented in docs/architecture/scene-source-format.md.

#include <ludus/foundation/base/core.h>

#include <ludus/content/content.h>
#include <ludus/foundation/containers/array.hpp>
#include <ludus/foundation/containers/static_array.hpp>
#include <ludus/foundation/math/vector.hpp>
#include <ludus/foundation/strings/string.hpp>

#include <string_view>

namespace ludus::content::scene_source
{
/// Value of the document's `format` member, which must be its first member.
inline constexpr std::string_view SCENE_SOURCE_FORMAT = "ludus.scene";
/// The only document `version` this reader supports.
inline constexpr uint64 SCENE_SOURCE_VERSION = 1;
/// Conventional file name suffix of a scene source.
inline constexpr std::string_view SCENE_SOURCE_EXTENSION = ".luscene.json";
/// Largest accepted document, in bytes.
inline constexpr usize MAX_SCENE_SOURCE_BYTES = usize{16} * 1024 * 1024;
/// Longest scene, material or object name and scene description, in bytes.
inline constexpr usize MAX_SCENE_SOURCE_TEXT_BYTES = 1024;
/// Most materials in one document.
inline constexpr usize MAX_SCENE_SOURCE_MATERIALS = 256;
/// Most objects in one document.
inline constexpr usize MAX_SCENE_SOURCE_OBJECTS = 4096;
/// Most vertices of one polygon.
inline constexpr usize MAX_SCENE_SOURCE_POLYGON_VERTICES = 64;
/// Most positions across all objects of one document.
inline constexpr usize MAX_SCENE_SOURCE_POSITIONS = usize{1} << 20;
/// Most polygon corners (index entries) across all objects of one document.
inline constexpr usize MAX_SCENE_SOURCE_CORNERS = usize{1} << 22;
/// Largest absolute position coordinate, in metres; keeps every position exactly
/// representable after narrowing to float32 within the cooked format's range.
inline constexpr float64 MAX_SCENE_SOURCE_COORDINATE_METRES = 1000000.0;

/// Most quads an aperture screen expands to: the screen beside, above and below its slit.
inline constexpr usize MAX_APERTURE_QUADS = 4;
/// Corners of one aperture quad.
inline constexpr usize APERTURE_QUAD_CORNERS = 4;
/// Largest aperture screen width or height, in metres: twice MAX_SCENE_SOURCE_COORDINATE_METRES.
inline constexpr float64 MAX_APERTURE_SCREEN_METRES = 2 * MAX_SCENE_SOURCE_COORDINATE_METRES;
/// Opening of a default-constructed ApertureShape, in metres: a 5 cm slit.
inline constexpr float64 DEFAULT_APERTURE_OPENING_METRES = 0.05;

/// @brief Size of a flat, opaque rectangular screen with a centred rectangular slit.
///
/// The screen lies in the z = 0 plane of its own frame. It spans
/// [-ScreenWidth / 2, ScreenWidth / 2] across the slit (x) and
/// [-ScreenHeight / 2, ScreenHeight / 2] along it (y); its front face looks along +z.
/// The slit spans [-Opening / 2, Opening / 2] by [-SlitLength / 2, SlitLength / 2]. Its
/// long edges are knife edges: the screen has no thickness. All lengths are metres.
struct ApertureShape final
{
    /// Screen extent across the slit (local x), in (0, MAX_APERTURE_SCREEN_METRES].
    float64 ScreenWidth = 1.0;
    /// Screen extent along the slit (local y), in (0, MAX_APERTURE_SCREEN_METRES].
    float64 ScreenHeight = 1.0;
    /// Slit extent along local y, in (0, ScreenHeight]. A slit as long as the screen cuts it in two.
    float64 SlitLength = 0.5;
    /// Slit width across local x: the aperture opening, in [0, ScreenWidth). Zero closes the slit.
    float64 Opening = DEFAULT_APERTURE_OPENING_METRES;
};

/// One quad of an aperture screen: corners in the aperture frame (z = 0), counterclockwise seen from +z.
using ApertureQuad = foundation::StaticArray<foundation::math::Vector3d, APERTURE_QUAD_CORNERS>;

/// @brief The quads an aperture screen expands to.
struct ApertureQuads final
{
    /// The first Count entries are valid: the screen left and right of the slit, then the
    /// parts above and below it, each omitted when it has no area.
    foundation::StaticArray<ApertureQuad, MAX_APERTURE_QUADS> Quads{};
    /// Number of valid quads, in [2, MAX_APERTURE_QUADS].
    usize Count = 0;
};

/// @brief Report whether every field of an aperture is finite and within its documented range.
[[nodiscard]] bool IsApertureShapeValid(const ApertureShape& shape) noexcept;

/// @brief Expand an aperture screen into strictly convex quads in its own frame.
///
/// Scene source import, the editor's World apertures and tests share this expansion, so
/// the same shape always produces the same corners in binary64.
/// @param shape The screen and slit sizes.
/// @param outQuads Replaced on success; unchanged on failure.
/// @return false when IsApertureShapeValid(shape) is false.
[[nodiscard]] bool BuildApertureQuads(const ApertureShape& shape, ApertureQuads& outQuads) noexcept;

/// Outcome of ParseSceneSource.
enum class SceneSourceStatus : uint8
{
    /// The document is valid and the scene is available.
    Success,
    /// The text is not JSON, or a member is missing, unknown, repeated or of the wrong type.
    Malformed,
    /// `format` is not SCENE_SOURCE_FORMAT, is not the first member, or `version` is unsupported.
    UnsupportedVersion,
    /// A number is out of its documented range, or a name is empty or repeated.
    InvalidValue,
    /// An object names a material the document does not declare.
    UnresolvedReference,
    /// A polygon has too few or repeated corners, an index out of range, or is not planar and strictly convex.
    InvalidGeometry,
    /// The document exceeds one of the MAX_SCENE_SOURCE_* limits.
    Limit,
    /// Allocation failed.
    OutOfMemory,
};

/// Return a stable lowercase name for a status, for tools and logs.
[[nodiscard]] std::string_view GetSceneSourceStatusName(SceneSourceStatus status) noexcept;

/// @brief Bounded diagnostic of a failed parse.
///
/// `Message` names the failing member by its JSON path, for example
/// `objects[2].polygons[0]`, when one is known.
struct SceneSourceError final
{
    /// Failure category; Success when nothing failed.
    SceneSourceStatus Status = SceneSourceStatus::Success;
    /// Human-readable detail.
    MessageText Message;
};

/// Reflection model of a source material.
enum class SourceMaterialModel : uint8
{
    /// Ideal Lambertian reflection of BaseColor (`"model": "diffuse"`).
    Diffuse,
    /// The engine's metallic-roughness model (`"model": "metallic_roughness"`).
    MetallicRoughness,
    /// Weighted Lambertian and rough conductor reflection (`"model": "mix"`).
    Mix,
    /// Authored cosine-power reflection program (`"model": "project_brdf"`).
    ProjectBrdf,
};

/// Constant rough conductor lobe of a source Mix. RGB channels use the engine's optical proxy.
struct SourceRoughConductor final
{
    /// Isotropic GGX alpha, in the scene material alpha range.
    float64 Alpha = 1.0;
    /// Positive, bounded real part of the index of refraction.
    foundation::math::Vector3d Eta{1, 1, 1};
    /// Nonnegative, bounded imaginary part of the index of refraction.
    foundation::math::Vector3d Extinction{};
};

/// Version-1 program for three weighted reciprocal cosine-power reflection terms.
struct SourceProjectBrdf final
{
    foundation::math::Vector3d Weights{};     ///< Relative nonnegative weights, normalized at evaluation.
    foundation::math::Vector3d Powers{};      ///< Nonnegative cosine exponents at zero roughness.
    bool HasFixedDimensionReflection = false; ///< Declares the fixed three-random-value reflection contract.
};

/// @brief A named material. Emission makes every polygon that uses it a light.
struct SourceMaterial final
{
    /// Unique, nonempty name that objects refer to.
    foundation::String Name;
    /// Reflection model; roughness also applies to project BRDF programs.
    SourceMaterialModel Model = SourceMaterialModel::Diffuse;
    /// Linear Rec. 709 reflectance, each channel in [0, 1].
    foundation::math::Vector3d BaseColor{0.5, 0.5, 0.5};
    /// Metallic fraction in [0, 1]; zero for Diffuse.
    float64 Metallic = 0.0;
    /// Perceptual roughness in [0, 1]; one for Diffuse.
    float64 Roughness = 1.0;
    /// Conductor weight in [0, 1]. The Lambertian lobe uses the complementary weight and BaseColor.
    float64 MixWeight = 0.0;
    /// Conductor inputs for Mix materials.
    SourceRoughConductor Conductor;
    /// Authored program for ProjectBrdf materials.
    SourceProjectBrdf Brdf;
    /// Constant emitted linear Rec. 709 radiance in W/(sr m^2) per channel, finite and
    /// nonnegative; all zero for a material that does not emit.
    foundation::math::Vector3d EmittedRadiance{};
    /// True when polygons emit from both faces; false emits from the front face only.
    bool IsEmissionTwoSided = false;
};

/// @brief Placement and size of an `aperture` object's screen.
struct SourceAperture final
{
    /// False for an object written as positions and polygons; the other fields then keep their defaults.
    bool IsPresent = false;
    /// World-space origin of the aperture frame: the centre of the slit, in metres.
    foundation::math::Vector3d CenterWorld{};
    /// Unit direction the screen's front face looks along: the frame's +z.
    foundation::math::Vector3d NormalWorld{0.0, 0.0, 1.0};
    /// Unit direction along the slit, perpendicular to NormalWorld: the frame's +y.
    /// The frame's +x, across the slit, is Cross(UpWorld, NormalWorld).
    foundation::math::Vector3d UpWorld{0.0, 1.0, 0.0};
    /// Screen and slit sizes in the frame.
    ApertureShape Shape;
};

/// @brief Polygons of one material in world space.
///
/// Polygon p uses the PolygonVertexCounts[p] entries of PolygonIndices that follow
/// those of the polygons before it. Its front face is the side from which its
/// corners appear counterclockwise. An `aperture` object also carries its
/// parameters in Aperture; its polygons are BuildApertureQuads placed in its frame.
struct SourceObject final
{
    /// Unique, nonempty name.
    foundation::String Name;
    /// Index into SceneSource::Materials.
    usize Material = 0;
    /// World-space positions in metres.
    foundation::Array<foundation::math::Vector3d> PositionsWorld;
    /// Corner count of each polygon, in [3, MAX_SCENE_SOURCE_POLYGON_VERTICES].
    foundation::Array<uint32> PolygonVertexCounts;
    /// Corners of all polygons, as indices into PositionsWorld.
    foundation::Array<uint32> PolygonIndices;
    /// Parameters of an `aperture` object; Aperture.IsPresent is false otherwise.
    SourceAperture Aperture;
};

/// @brief Pinhole camera that becomes the imported World's start view.
struct SourceCamera final
{
    /// False when the document has no `camera` member; the other fields then keep their defaults.
    bool IsPresent = false;
    /// Centre of projection in world space, metres.
    foundation::math::Vector3d EyeWorld{0.0, 0.0, 0.0};
    /// A point the camera looks at, distinct from EyeWorld.
    foundation::math::Vector3d TargetWorld{0.0, 0.0, -1.0};
    /// Approximate up direction, not parallel to the view direction; need not be unit length.
    foundation::math::Vector3d UpWorld{0.0, 1.0, 0.0};
    /// Full vertical field of view in radians, in (0, pi).
    float64 VerticalFovRadians = 0.0;
};

/// @brief A complete, validated scene source.
///
/// Move-only; it owns all of its storage and has no internal synchronization.
struct SceneSource final
{
    /// Display name of the scene.
    foundation::String Name;
    /// Optional free-text description; empty when absent.
    foundation::String Description;
    /// Optional camera.
    SourceCamera Camera;
    /// Materials in document order; at least one.
    foundation::Array<SourceMaterial> Materials;
    /// Objects in document order; at least one.
    foundation::Array<SourceObject> Objects;
};

/// @brief Report whether text begins like a scene source.
///
/// True when, after optional whitespace, the text opens an object whose first
/// member is `"format": "ludus.scene"`. Every valid document satisfies this, so
/// importers can classify a file from a short prefix without parsing it.
/// @param prefix The first bytes of a file; may be truncated anywhere after the format value.
[[nodiscard]] bool IsSceneSourcePrefix(std::string_view prefix) noexcept;

/// @brief Parse and validate a complete scene source document.
///
/// Rejects unknown, missing or repeated members, values outside their documented
/// ranges, unresolved material names and polygons that are not planar and strictly
/// convex. Numbers are read as IEEE binary64. Aperture objects are expanded into
/// their polygons, which count towards the position and corner limits.
/// @param text The whole document, at most MAX_SCENE_SOURCE_BYTES.
/// @param outScene Replaced on success; unchanged on failure.
/// @param outError Set on failure; unchanged on success.
/// @return Success, or the first failure found.
[[nodiscard]] SceneSourceStatus
ParseSceneSource(std::string_view text, SceneSource& outScene, SceneSourceError& outError) noexcept;
} // namespace ludus::content::scene_source
