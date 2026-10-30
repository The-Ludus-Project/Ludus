#pragma once

#include <ludus/content/content.h>
#include <ludus/content/scene_cook/scene_cook.h>
#include <ludus/foundation/containers/array.hpp>
#include <ludus/foundation/strings/string.hpp>

#include <span>
#include <string_view>

/// @file
/// Best-effort scene importers: pbrt-v4 (profile pbrt-best-effort-v1), glTF 2.0
/// and GLB (gltf-best-effort-v1) and Wavefront OBJ with MTL (obj-best-effort-v1).
///
/// Unlike the frozen pbrt-reference-v1 staircase2 cooker, these importers accept
/// general scenes with reported approximations: unsupported features become
/// documented fallbacks, and the scene is fitted to the LUSCN limits in
/// Ludus::SceneFormat (6 meshes, 8 materials, 8 textures, 500 k vertices, 128
/// objects, 64 MiB). Every compromise is recorded in a deterministic, versioned
/// JSON import report. Authored pbrt lighting that cannot fit without losing
/// sources or energy rejects explicitly; decorative glTF/OBJ emission is dropped
/// with a diagnostic instead. All front-ends share one format-neutral
/// intermediate representation and budget planner. See
/// docs/development/best-effort-scene-import.md.
namespace ludus::foundation::threading
{
class JobSystem;
} // namespace ludus::foundation::threading

namespace ludus::content::scene_cook
{
/// Profile identifier of the pbrt-v4 importer, recorded in the import report and the catalog.
inline constexpr std::string_view BEST_EFFORT_PROFILE = "pbrt-best-effort-v1";
/// Profile identifier of the glTF 2.0 / GLB importer.
inline constexpr std::string_view GLTF_BEST_EFFORT_PROFILE = "gltf-best-effort-v1";
/// Profile identifier of the Wavefront OBJ/MTL importer.
inline constexpr std::string_view OBJ_BEST_EFFORT_PROFILE = "obj-best-effort-v1";

/// @brief Scene source container recognised by DetectSceneSourceFormat.
enum class SceneSourceFormat : uint8
{
    /// Not recognised.
    Unknown,
    /// pbrt-v4 scene description text.
    Pbrt,
    /// glTF 2.0 JSON (.gltf) or binary container (.glb).
    Gltf,
    /// Wavefront OBJ text.
    Obj,
};
/// Version of the import report JSON schema.
inline constexpr uint32 IMPORT_REPORT_SCHEMA = 1;

/// @brief Options for the best-effort importers.
struct BestEffortOptions final
{
    /// Parse and plan only: no image decoding and no bundle. The report is complete
    /// except for texture sizes, and BestEffortResult::Bundle stays empty.
    bool IsDryRun = false;
    /// Optional caller-owned job system on which texture images are decoded in parallel;
    /// nullptr decodes them one at a time on the calling thread.
    ///
    /// The importer reads every image file on the calling thread, then decodes batches of
    /// up to min(GetWorkerCount() + 1, 8) images in jobs while the caller helps in Wait,
    /// so it holds at most that many extra decoded images. Bundles, reports and input
    /// digests are byte-identical for every worker count, including zero and nullptr.
    /// The system must be initialized, must have no graph in flight for the duration of
    /// the import (a busy system makes the import decode serially), and is used from the
    /// calling thread only. Browser builds pass a zero-worker system or nullptr. Not owned.
    foundation::threading::JobSystem* JobsOrNull = nullptr;
};

/// @brief Headline numbers of one import, also present in the report.
struct ImportSummary final
{
    /// Text source files read (the entry file plus pbrt includes or MTL libraries).
    uint32 SourceFiles = 0;
    /// Shape placements after instance expansion.
    uint32 SourcePlacements = 0;
    /// Vertex records across all placements before exact compaction and budgeting;
    /// counts above the uint32 range saturate at its maximum.
    uint32 SourceVertices = 0;
    /// Triangle indices across all placements before budgeting (three per triangle);
    /// counts above the uint32 range saturate at its maximum.
    uint32 SourceIndices = 0;
    /// Materials defined by the source (named and anonymous Material directives).
    uint32 SourceMaterials = 0;
    /// Image and spectrum textures defined by the source.
    uint32 SourceTextures = 0;
    /// pbrt AreaLightSource directives (each makes the shapes after it in its block
    /// emissive), or emissive glTF/MTL materials in use.
    uint32 SourceAreaLights = 0;
    /// Meshes in the cooked bundle (one per material slot).
    uint32 CookedMeshes = 0;
    /// Materials in the cooked bundle.
    uint32 CookedMaterials = 0;
    /// Textures in the cooked bundle, swatches included.
    uint32 CookedTextures = 0;
    /// Objects in the cooked bundle.
    uint32 CookedObjects = 0;
    /// Vertices across all cooked meshes.
    uint32 CookedVertices = 0;
    /// Triangle indices across all cooked meshes (three per retained triangle).
    uint32 CookedIndices = 0;
    /// Placements left out by the vertex/index budget.
    uint32 DroppedPlacements = 0;
    /// Bundle size in bytes (zero for a dry run).
    usize BundleBytes = 0;
    /// Warning-level diagnostics (occurrences).
    uint32 Warnings = 0;
    /// Error-level diagnostics (occurrences); nonzero means the import failed.
    uint32 Errors = 0;
};

/// @brief Outputs of the best-effort importers.
struct BestEffortResult final
{
    /// LUSCN version 3 bundle with authored photometric lighting and triangle bake coordinates (scene.luscn); empty for
    /// a dry run.
    foundation::Array<uint8> Bundle;
    /// Import report JSON (import-report.json) including its trailing newline.
    foundation::Array<uint8> Report;
    /// Source format that produced the result; Unknown before an import ran.
    SceneSourceFormat Format = SceneSourceFormat::Unknown;
    /// Importer profile identifier (one of the *_PROFILE constants); empty before an import ran.
    std::string_view Profile;
    /// Verbatim LICENSE.txt (or LICENSE.md) from the import root; empty when absent.
    foundation::Array<uint8> License;
    /// Headline numbers.
    ImportSummary Summary;
    /// Root-relative paths of every file the import read (text sources, PLY or glTF buffer files,
    /// referenced images and the licence), without duplicates, in first-use order.
    /// Callers re-hash these to detect a source that changed after the import.
    foundation::Array<foundation::String> Inputs;
    /// SHA-256 of the exact bytes the import consumed from each Inputs entry (parallel array).
    foundation::Array<Digest> InputDigests;
};

/// Import a pbrt-v4 entry file with the best-effort profile.
/// @param entryFile Native path of the .pbrt file. Its directory is the import root;
/// Include/Import, PLY and image paths must resolve inside it.
/// @param options Dry-run selection.
/// @param outResult Replaced on success. On a parse failure with diagnostics,
/// outResult.Report still holds the report so the caller can show why.
/// @param outError Set on failure: Invalid (syntax errors or nothing drawable),
/// NotFound/IoError (entry file), Cancelled, OutOfMemory.
/// @param cancelOrNull Optional cancellation hook polled per directive and per placement.
/// @return true on success.
/// @note Synchronous file I/O on Linux and macOS; other targets report Unsupported
/// until the browser source file system lands. Deterministic: equal inputs give equal bytes.
[[nodiscard]] bool ImportPbrtBestEffort(std::string_view entryFile,
                                        const BestEffortOptions& options,
                                        BestEffortResult& outResult,
                                        CookError& outError,
                                        const Cancellation* cancelOrNull = nullptr) noexcept;

/// Import a glTF 2.0 (.gltf) or GLB (.glb) entry file with the best-effort profile.
/// The static triangle-mesh subset (node hierarchy, instancing, core metallic-roughness
/// materials, perspective cameras, KHR_lights_punctual) is read; unsupported content
/// is reported. A file that requires an unsupported extension fails with Invalid.
/// @param entryFile Native path of the entry file. Its directory is the import root;
/// external buffers and images must resolve inside it. Data URIs are decoded in memory.
/// @param options Dry-run selection.
/// @param outResult Replaced. On a failure with diagnostics, outResult.Report still holds
/// the report so the caller can show why.
/// @param outError Set on failure: Invalid (malformed input, unsupported required
/// features or nothing drawable), NotFound/IoError (entry file), Cancelled, OutOfMemory.
/// @param cancelOrNull Optional cancellation hook polled per node, primitive and placement.
/// @return true on success.
/// @note Same platform and determinism contract as ImportPbrtBestEffort.
[[nodiscard]] bool ImportGltfBestEffort(std::string_view entryFile,
                                        const BestEffortOptions& options,
                                        BestEffortResult& outResult,
                                        CookError& outError,
                                        const Cancellation* cancelOrNull = nullptr) noexcept;

/// Import a Wavefront OBJ entry file and the MTL libraries it names.
/// Faces are fan-triangulated (at most 64 corners), Phong-style materials are mapped to
/// metallic-roughness with the documented conversion, and the result is fitted to LUSCN.
/// @param entryFile Native path of the .obj file; its directory is the import root and
/// material libraries and textures must resolve inside it.
/// @param options Dry-run selection.
/// @param outResult Replaced; on a failure with diagnostics the report is kept.
/// @param outError Set on failure: Invalid (malformed records, zero or out-of-range
/// indices, limits, nothing drawable), NotFound/IoError (entry file), Cancelled, OutOfMemory.
/// @param cancelOrNull Optional cancellation hook polled every few thousand lines.
/// @return true on success.
/// @note Same platform and determinism contract as ImportPbrtBestEffort. The entry is
/// streamed; memory grows with the retained geometry, not the file size.
[[nodiscard]] bool
ImportObjBestEffort(std::string_view entryFile, // ludus-style: abbreviation OBJ names the Wavefront file format
                    const BestEffortOptions& options,
                    BestEffortResult& outResult,
                    CookError& outError,
                    const Cancellation* cancelOrNull = nullptr) noexcept;

/// Classify a scene source from its first bytes, falling back to the file name extension
/// only when the content is not decisive (for example a comment-only prefix).
/// GLB magic or a leading '{' selects Gltf, an OBJ record keyword selects Obj and a
/// capitalised pbrt directive selects Pbrt.
/// @param prefix Leading bytes of the file (4 KiB is plenty); may be empty.
/// @param fileName Name used only for the extension fallback.
/// @return The detected format, or Unknown.
[[nodiscard]] SceneSourceFormat DetectSceneSourceFormat(std::span<const uint8> prefix,
                                                        std::string_view fileName) noexcept;

/// Read the first 4 KiB of entryFile and classify it with DetectSceneSourceFormat.
/// @param entryFile Native path.
/// @param outFormat Set only on Ok.
/// @return Ok, NotFound, IoError or Unsupported (no native file access on this target).
[[nodiscard]] Status DetectSceneSourceFile(std::string_view entryFile, SceneSourceFormat& outFormat) noexcept;

/// Import entryFile with the best-effort importer selected by DetectSceneSourceFile.
/// @return As the selected importer; Unsupported in outError when the format is unknown,
/// NotFound/IoError when the entry cannot be read.
[[nodiscard]] bool ImportSceneBestEffort(std::string_view entryFile,
                                         const BestEffortOptions& options,
                                         BestEffortResult& outResult,
                                         CookError& outError,
                                         const Cancellation* cancelOrNull = nullptr) noexcept;

/// Write scene.luscn, import-report.json and (when present) LICENSE.txt below output.
/// @param output Native directory; created with its parents when absent.
/// @param result A non-dry-run result.
/// @param outError Set on failure.
/// @return true when every file was atomically replaced.
[[nodiscard]] bool
WriteBestEffortImport(std::string_view output, const BestEffortResult& result, CookError& outError) noexcept;
} // namespace ludus::content::scene_cook
