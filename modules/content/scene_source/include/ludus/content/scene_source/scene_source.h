#pragma once

// Ludus scene source (`.luscene.json`), version 1: a hand-readable JSON scene
// that Ludus owns. It holds world-space convex polygon geometry, materials with
// emission and an optional camera, and is the source of the engine's test scenes.
// SceneCook's `ludus-scene-v1` importer cooks it into a LUSCN bundle like any
// other source; tests read it directly. The file layout, conventions and limits
// are documented in docs/architecture/scene-source-format.md.

#include <ludus/foundation/base/core.h>

#include <ludus/content/content.h>
#include <ludus/foundation/containers/array.hpp>
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

/// @brief A named material. Emission makes every polygon that uses it a light.
struct SourceMaterial final
{
    /// Unique, nonempty name that objects refer to.
    foundation::String Name;
    /// Reflection model; Metallic and Roughness apply to MetallicRoughness only.
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
    /// Constant emitted linear Rec. 709 radiance in W/(sr m^2) per channel, finite and
    /// nonnegative; all zero for a material that does not emit.
    foundation::math::Vector3d EmittedRadiance{};
    /// True when polygons emit from both faces; false emits from the front face only.
    bool IsEmissionTwoSided = false;
};

/// @brief Polygons of one material in world space.
///
/// Polygon p uses the PolygonVertexCounts[p] entries of PolygonIndices that follow
/// those of the polygons before it. Its front face is the side from which its
/// corners appear counterclockwise.
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
/// convex. Numbers are read as IEEE binary64.
/// @param text The whole document, at most MAX_SCENE_SOURCE_BYTES.
/// @param outScene Replaced on success; unchanged on failure.
/// @param outError Set on failure; unchanged on success.
/// @return Success, or the first failure found.
[[nodiscard]] SceneSourceStatus
ParseSceneSource(std::string_view text, SceneSource& outScene, SceneSourceError& outError) noexcept;
} // namespace ludus::content::scene_source
