#pragma once

// Ludus::GeometryContent (curves and surfaces architecture, phase G5):
// authoring codecs, validation, cooking, loading and publication over
// Ludus::FoundationGeometry, through the shared Ludus::GeometryFormat cooked
// layout. See docs/architecture/curves-surfaces.md ("Content and failure
// contracts") and ADR 0024 for the frozen domains, statuses and boundaries.
//
// The design mirrors the content precedents: JSON source parsing through the
// checked content::Json facade (modules/content/json.h), SHA-256 digests through
// content::Hash, field-by-field little-endian cooking like modules/content/
// scene_cook, and loader validation like modules/runtime/resources SceneDefinition.
// A cooker never links the renderer; a runtime loader never links a cooker; both
// include Ludus::GeometryFormat.

#include <ludus/foundation/base/core.h>

#include <ludus/content/content.h>
#include <ludus/content/geometry_format/geometry_format.h>
#include <ludus/foundation/containers/array.hpp>
#include <ludus/foundation/geometry/path.hpp>
#include <ludus/foundation/geometry/status.hpp>
#include <ludus/foundation/geometry/surface.hpp>
#include <ludus/foundation/geometry/track.hpp>
#include <ludus/foundation/math/vector.hpp>

#include <span>
#include <string_view>

namespace ludus::geometry::content
{
using namespace ludus::foundation;
namespace fg = ludus::content::geometry_format;
namespace geom = ludus::foundation::geometry;

/// Geometry source schema version this reader and writer support.
inline constexpr uint64 SOURCE_SCHEMA_VERSION = 1;

/// @brief Cooperative cancellation hook polled between bounded cook work units.
///
/// Mirrors content::scene_cook::Cancellation. Requested must be thread-safe for
/// the caller; nullptr disables cancellation. A requested cancellation yields
/// ludus::content::Status::Cancelled and leaves no partial output.
struct Cancellation final
{
    bool (*Requested)(void* context) noexcept = nullptr; ///< True when the cook should stop.
    void* ContextOrNull = nullptr;                       ///< Opaque pointer passed back to Requested.
};

/// @brief Which source geometry a GeometrySource holds.
enum class GeometrySourceMode : uint8
{
    CubicPath,      ///< Line and cubic Bezier spans in asset coordinates.
    HermitePath,    ///< Hermite waypoints converted to cubic spans.
    CatmullRomPath, ///< Centripetal Catmull-Rom waypoints converted to cubic spans.
    ScalarTrack,    ///< Scalar-track keys over TrackTime.
    PatchSurface,   ///< Untrimmed bicubic patches with explicit adjacency.
};

/// @brief Editing mode of a waypoint handle.
///
/// Moving an automatic handle either preserves its mode or visibly converts it
/// to Manual (the editor adapter performs the visible conversion; the source
/// records the resulting mode).
enum class HandleMode : uint8
{
    Automatic, ///< Handle is derived from neighbours (Catmull-Rom or auto-smooth).
    Manual,    ///< Handle is authored directly.
};

/// @brief One authored line or cubic span with a stable author ID.
struct AuthoredSpan final
{
    uint64 AuthorId = 0;                                                 ///< Stable author ID.
    fg::SpanKindTag Kind = fg::SpanKindTag::Cubic;                       ///< Line or cubic.
    core::StaticArray<math::Vector3d, fg::SPAN_CONTROL_COUNT> Control{}; ///< Asset-space controls; 0 and 3 endpoints.
    float64 DomainWidth = 1.0;                                           ///< Source domain width; finite and > 0.
};

/// @brief One authored waypoint with tangent handles and their modes.
struct AuthoredWaypoint final
{
    uint64 AuthorId = 0;                     ///< Stable author ID.
    math::Vector3d Position{};               ///< Asset-space position.
    math::Vector3d InTangent{};              ///< Incoming tangent (Hermite), per domain unit.
    math::Vector3d OutTangent{};             ///< Outgoing tangent (Hermite), per domain unit.
    float64 OutgoingWidth = 1.0;             ///< Domain width of the span leaving this waypoint; finite and > 0.
    HandleMode Mode = HandleMode::Automatic; ///< Handle mode.
};

/// @brief One authored scalar-track key.
struct AuthoredKey final
{
    uint64 AuthorId = 0;     ///< Stable author ID.
    float64 Time = 0.0;      ///< Seconds; strictly increasing across keys.
    float32 Value = 0.0F;    ///< Value at Time.
    float32 InSlope = 0.0F;  ///< Incoming slope, per second.
    float32 OutSlope = 0.0F; ///< Outgoing slope, per second.
    fg::TrackInterpolationTag Interpolation = fg::TrackInterpolationTag::Linear; ///< Span starting at this key.
};

/// @brief One authored bicubic patch with a stable author ID.
struct AuthoredPatch final
{
    uint64 AuthorId = 0; ///< Stable author ID; unique within the surface.
    core::StaticArray<core::StaticArray<math::Vector3d, fg::PATCH_CONTROL_ORDER>, fg::PATCH_CONTROL_ORDER>
        Control{}; ///< Control[u index][v index], asset coordinates.
};

/// @brief Validated, owned geometry source document. Move-only.
///
/// Built by ParseGeometrySource, consumed by CookGeometry and WriteGeometrySource.
/// Exactly the mode-specific arrays for Mode are populated. Author IDs are stable
/// across edits; array indices are revision-local.
class GeometrySource final
{
public:
    /// Construct an empty source with no populated arrays.
    GeometrySource() noexcept = default;
    /// Transfer the owned arrays; the source becomes empty.
    GeometrySource(GeometrySource&&) noexcept = default;
    /// Release this document's arrays and take the other's; the source becomes empty.
    GeometrySource& operator=(GeometrySource&&) noexcept = default;
    /// Copying is disabled: one owner holds the source arrays.
    GeometrySource(const GeometrySource&) = delete;
    /// Copy assignment is disabled: one owner holds the source arrays.
    GeometrySource& operator=(const GeometrySource&) = delete;
    /// Release the owned source arrays.
    ~GeometrySource() = default;

    GeometrySourceMode Mode = GeometrySourceMode::CubicPath;           ///< Which arrays below are populated.
    uint64 Revision = 0;                                               ///< Owner-assigned snapshot revision.
    math::Vector3d Origin{};                                           ///< Float64 chunk origin.
    float64 AssetScale = 0.0;                                          ///< Declared asset scale; finite and >= 0.
    bool IsClosed = false;                                             ///< Closed path or surface boundary.
    geom::ToleranceProfile Tolerance{};                                ///< Validated as a whole.
    geom::WorkLimits Limits{};                                         ///< Preparation work limits.
    geom::PrecisionProfile Precision = geom::PrecisionProfile::Visual; ///< Strict or Visual.

    core::Array<AuthoredSpan> Spans;                     ///< CubicPath.
    core::Array<AuthoredWaypoint> Waypoints;             ///< HermitePath / CatmullRomPath.
    core::Array<AuthoredKey> Keys;                       ///< ScalarTrack.
    core::Array<AuthoredPatch> Patches;                  ///< PatchSurface.
    core::Array<geom::SurfaceAdjacencySource> Adjacency; ///< PatchSurface joins.
};

/// @brief Structured diagnostic for a content failure.
///
/// The transport status is the content::Status returned to callers; Detail
/// carries the ADR 0024 geometry diagnostic (code, source element, property
/// path, requested/achieved tolerance and offending range). Repair names a
/// concrete fix ("Errors offer solutions", AGENTS.md). Bad content never asserts.
struct GeometryDiagnosticReport final
{
    ludus::content::Status Transport =
        ludus::content::Status::Ok;      ///< Ok, Invalid, Limit, OutOfMemory, Conflict, Cancelled, Unsupported.
    geom::GeometryDiagnostic Detail{};   ///< Geometry-level details.
    const char* Repair = "";             ///< Static suggested repair, never null.
    ludus::content::MessageText Message; ///< Bounded human-readable wording.
};

/// @brief Cache key covering everything the architecture lists (ADR 0024, content-resources.md).
struct GeometryCacheKey final
{
    ludus::content::Digest SourceDigest{};     ///< SHA-256 of the canonical source bytes.
    ludus::content::Digest DependencyDigest{}; ///< SHA-256 of referenced dependencies (empty-hash when none).
    ludus::content::Digest TopologyDigest{};   ///< SHA-256 of closure/adjacency/key-domain structure.
    uint32 FormatVersion = 0;                  ///< Cooked format layout version.
    uint32 CookerVersion = 0;                  ///< Cooker version.
    uint64 SchemaVersion = 0;                  ///< Source schema version.
    geom::PrecisionProfile Precision = geom::PrecisionProfile::Visual; ///< Precision profile.
    geom::ToleranceProfile Tolerance{};                                ///< All tolerances.
};

/// @brief Immutable loaded geometry owner. Move-only; holds exactly one prepared owner.
class GeometryLoad final
{
public:
    /// Construct an empty load with no prepared owner.
    GeometryLoad() noexcept = default;
    /// Transfer the prepared owner and cache key; the source becomes empty.
    GeometryLoad(GeometryLoad&&) noexcept = default;
    /// Release this load's prepared owner and take the other's; the source becomes empty.
    GeometryLoad& operator=(GeometryLoad&&) noexcept = default;
    /// Copying is disabled: one owner holds the prepared storage.
    GeometryLoad(const GeometryLoad&) = delete;
    /// Copy assignment is disabled: one owner holds the prepared storage.
    GeometryLoad& operator=(const GeometryLoad&) = delete;
    /// Release the prepared owner; borrowed views into it become invalid.
    ~GeometryLoad() = default;

    fg::GeometryKindTag Kind = fg::GeometryKindTag::Path; ///< Which prepared owner below is valid.
    geom::PreparedPath Path;                              ///< Valid iff Kind == Path.
    geom::PreparedTrack Track;                            ///< Valid iff Kind == Track.
    geom::PreparedSurface Surface;                        ///< Valid iff Kind == Surface.
    GeometryCacheKey Key{};                               ///< Cache key recovered from the blob header.
};

/// @brief Parse a JSON geometry source into an owned, validated GeometrySource.
///
/// Validates the schema version, known mode and enum strings, finite numbers,
/// strictly increasing key times, unique author IDs, and the count ceilings from
/// Ludus::GeometryFormat limits. Unknown top-level fields, an unknown version,
/// duplicate IDs and invalid constraints return a structured diagnostic; nothing
/// asserts on bad content.
/// @param json Source text, at most MAX_SOURCE_BYTES.
/// @param outSource Replaced only on success.
/// @param outDiagnostic Set on every failure; cleared to Ok on success.
/// @return content::Status::Ok on success, else Invalid, Limit or OutOfMemory.
[[nodiscard]] ludus::content::Status
ParseGeometrySource(std::string_view json, GeometrySource& outSource, GeometryDiagnosticReport& outDiagnostic) noexcept;

/// @brief Write a GeometrySource to deterministic canonical JSON.
///
/// Fixed field order and reproducible numeric spelling, so Parse∘Write∘Parse is
/// stable. @param source Validated source. @param outJson Replaced on success.
/// @param outDiagnostic Set on failure. @return Ok, Invalid or OutOfMemory.
[[nodiscard]] ludus::content::Status WriteGeometrySource(const GeometrySource& source,
                                                         ludus::content::Bytes& outJson,
                                                         GeometryDiagnosticReport& outDiagnostic) noexcept;

/// @brief Cook a validated source into a LUGEO blob (Ludus::GeometryFormat layout).
///
/// Validates and dry-runs the FoundationGeometry preparation so a cook never
/// emits a blob that cannot load, then serialises the blob field by field in
/// little-endian order and computes the source, dependency and payload digests.
/// The cook is cooperatively cancellable; cancellation yields Cancelled with no
/// partial output. Identical source bytes produce identical blob bytes.
/// @param source Validated source.
/// @param outBlob Replaced only on success; preserved on failure or cancellation.
/// @param outDiagnostic Set on failure.
/// @param cancelOrNull Optional cancellation hook; nullptr disables it.
/// @return Ok, Invalid, Limit, OutOfMemory or Cancelled.
[[nodiscard]] ludus::content::Status CookGeometry(const GeometrySource& source,
                                                  core::Array<uint8>& outBlob,
                                                  GeometryDiagnosticReport& outDiagnostic,
                                                  const Cancellation* cancelOrNull = nullptr) noexcept;

/// @brief Validate a LUGEO blob and build the matching prepared owner.
///
/// Before exposing any view it validates, in order: the size and MAX_BLOB_BYTES
/// ceiling; the magic and format version; the kind and precision enum tags; every
/// count against its limit; each section's Count * ElementBytes byte product with
/// checked arithmetic, its four-byte alignment, in-range offset and non-overlap,
/// and that the required sections for the kind are present and no foreign section
/// is; the tolerance profile; strictly increasing track-key times; finiteness of
/// every origin, scale, control and tolerance; and the recomputed payload digest.
/// It then builds the borrowed source views and calls the FoundationGeometry
/// TryPrepare* owner, mapping any GeometryStatus into the diagnostic.
/// @param blob Cooked bytes, borrowed for the call only.
/// @param limits Preparation work limits (peak memory, leaves, depth, ...).
/// @param outLoad Replaced only on success; unchanged on every failure.
/// @param outDiagnostic Set on failure.
/// @return Ok, Invalid, Limit, OutOfMemory or Unsupported.
[[nodiscard]] ludus::content::Status LoadGeometry(std::span<const uint8> blob,
                                                  const geom::WorkLimits& limits,
                                                  GeometryLoad& outLoad,
                                                  GeometryDiagnosticReport& outDiagnostic) noexcept;

/// @brief The resource kind registered with the content catalog for cooked geometry.
///
/// Register with content::Catalog version 2 (see modules/content catalog). The
/// string used in a catalog document is "cooked-geometry".
inline constexpr std::string_view CATALOG_KIND = "cooked-geometry";
} // namespace ludus::geometry::content
