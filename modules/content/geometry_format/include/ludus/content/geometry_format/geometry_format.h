#pragma once

#include <ludus/foundation/base/types.h>

#include <ludus/content/geometry_format/limits.h>

#include <string_view>

/// @file
/// LUGEO cooked geometry interchange format (Ludus::GeometryFormat).
///
/// This header is the single contract between the producer of cooked geometry
/// (Ludus::GeometryContent's cooker and the Editor composer) and the consumer
/// (Ludus::GeometryContent's loader, which rebuilds the Ludus::FoundationGeometry
/// prepared owners). It is owned by neither side: tools and runtime both depend
/// on it, and it depends on nothing but FoundationBase. See
/// docs/architecture/curves-surfaces.md ("Content and failure contracts") and
/// ADR 0024 for the frozen domains, statuses and format boundary. The layout
/// mirrors the precedents in modules/content/scene_format (LUSCN) and
/// modules/content/render_graph_format (the runtime package blob header).
///
/// A blob is little-endian and holds exactly one geometry kind (path, scalar
/// track or bicubic surface). Layout, in order:
///
///   1. Header (HEADER_BYTES): Magic[8]; FormatVersion; CookerVersion; KindTag;
///      PrecisionTag; Flags; SpanCount; JointCount; KeyCount; PatchCount;
///      AdjacencyCount; SourceIdentityCount; Revision (uint64); Origin[3]
///      (float64); AssetScale (float64); PositionFloor (float64);
///      EffectivePositionError (float64); the 12 ToleranceProfile fields
///      (float64); SectionCount (uint32) then SectionCount SectionEntry records;
///      and three SHA-256 digests (SourceDigest, DependencyDigest, PayloadDigest).
///   2. Payload sections, each at its directory Offset with Count records of
///      ElementBytes each, four-byte aligned and non-overlapping: path span and
///      joint records, track key records, patch and adjacency records, and the
///      source-identity table, as the KindTag selects (see GetKindSections).
///
/// The record structs document the exact wire layout and are checked with
/// static_asserts; a codec serialises them field by field in little-endian order
/// rather than copying the structs, so host endianness and padding never leak
/// into files. Serialized widths (*_BYTES) are derived from the field schema,
/// never from host sizeof padding. A reader validates the magic, both versions,
/// the enum tags, every count against the limits, the count-times-width byte
/// products, section offsets/alignment/overlap, monotone track domains, control
/// finiteness and the payload digest before exposing any prepared owner. Bad
/// content is a structured diagnostic, never an assertion.
namespace ludus::content::geometry_format
{
using foundation::float32;
using foundation::float64;
using foundation::uint32;
using foundation::uint64;
using foundation::uint8;
using foundation::usize;

/// Eight-byte file magic, including the trailing NUL.
inline constexpr std::string_view MAGIC{"LUGEO01\0", 8};
/// Version 1: the first cooked geometry layout (path, track and surface kinds).
inline constexpr uint32 FORMAT_VERSION_1 = 1;
/// Current cooked format layout version.
inline constexpr uint32 FORMAT_VERSION = FORMAT_VERSION_1;
/// Cooker version, bumped when the cook output changes for an unchanged source.
/// Part of the cache key (docs/architecture/content-resources.md); independent
/// of FORMAT_VERSION, which changes only when the wire layout changes.
inline constexpr uint32 COOKER_VERSION = 1;
/// SHA-256 digest width, matching content::Digest.
inline constexpr usize DIGEST_BYTES = 32;

/// @brief Which record sections a blob carries; one blob holds exactly one kind.
// NOLINTNEXTLINE(performance-enum-size): LUGEO wire tags have a fixed uint32 representation.
enum class GeometryKindTag : uint32
{
    Path = 0,    ///< Path span and joint sections.
    Track = 1,   ///< Scalar-track key section.
    Surface = 2, ///< Patch and adjacency sections.
};

/// @brief Polynomial kind of a cooked path span; maps to geometry::SpanKind.
// NOLINTNEXTLINE(performance-enum-size): LUGEO wire tags have a fixed uint32 representation.
enum class SpanKindTag : uint32
{
    Line = 0,  ///< Linear segment; only controls 0 and 3 are meaningful.
    Cubic = 1, ///< Cubic Bezier span with four controls.
};

/// @brief Interpolation of a cooked track span; maps to geometry::TrackInterpolation.
// NOLINTNEXTLINE(performance-enum-size): LUGEO wire tags have a fixed uint32 representation.
enum class TrackInterpolationTag : uint32
{
    Step = 0,    ///< Hold the left key value until the next key.
    Linear = 1,  ///< Linear between the two key values.
    Hermite = 2, ///< Cubic from the left OutSlope to the right InSlope, per second.
};

/// @brief Patch boundary side; maps to geometry::PatchSide.
// NOLINTNEXTLINE(performance-enum-size): LUGEO wire tags have a fixed uint32 representation.
enum class PatchSideTag : uint32
{
    U0 = 0, ///< u = 0, runs along v.
    U1 = 1, ///< u = 1, runs along v.
    V0 = 2, ///< v = 0, runs along u.
    V1 = 3, ///< v = 1, runs along u.
};

/// @brief Precision profile the asset was cooked for; maps to geometry::PrecisionProfile.
// NOLINTNEXTLINE(performance-enum-size): LUGEO wire tags have a fixed uint32 representation.
enum class PrecisionProfileTag : uint32
{
    Strict = 0, ///< Only Exact or Certified bounds.
    Visual = 1, ///< Estimated bounds accepted and reported.
};

/// @brief Header flag bits in the Flags field. Other bits must be zero.
// NOLINTNEXTLINE(performance-enum-size): LUGEO wire tags have a fixed uint32 representation.
enum class GeometryFlag : uint32
{
    IsClosed = 1U << 0, ///< The path or surface boundary is closed; enables Wrap.
};

/// @brief Stable section identifier recorded in the directory and required per kind.
// NOLINTNEXTLINE(performance-enum-size): LUGEO wire tags have a fixed uint32 representation.
enum class SectionId : uint32
{
    PathSpans = 0,        ///< PathSpanRecord array.
    PathJoints = 1,       ///< JointRecord array.
    TrackKeys = 2,        ///< TrackKeyRecord array.
    SurfacePatches = 3,   ///< PatchRecord array.
    SurfaceAdjacency = 4, ///< AdjacencyRecord array.
    SourceIdentities = 5, ///< SourceIdentityRecord array (optional, may be empty).
};

/// Number of controls stored for every path span; a line uses controls 0 and 3.
inline constexpr usize SPAN_CONTROL_COUNT = 4;
/// Spatial components in a control point.
inline constexpr usize CONTROL_COMPONENTS = 3;
/// Controls along each direction of a bicubic patch (16 controls total).
inline constexpr usize PATCH_CONTROL_ORDER = 4;
/// Continuity flags stored per joint (C0, G1, C1, C2).
inline constexpr usize JOINT_FLAG_COUNT = 4;
/// Tolerance profile fields serialized in the header (ADR 0024 ToleranceProfile).
inline constexpr usize TOLERANCE_FIELD_COUNT = 12;
/// Byte alignment every payload section's offset satisfies.
inline constexpr usize SECTION_ALIGNMENT = 4;
/// Reserved trailing bytes of a JointRecord that keep its width a multiple of eight.
inline constexpr usize JOINT_RESERVED_BYTES = 4;
/// SHA-256 digests stored at the end of the header (source, dependency, payload).
inline constexpr usize HEADER_DIGEST_COUNT = 3;

/// @brief One entry of the checked section directory.
///
/// Offset is measured from the blob start. A reader validates that
/// Count * ElementBytes does not overflow, that [Offset, Offset + Count *
/// ElementBytes) lies within the blob, is four-byte aligned, and does not
/// overlap any other section.
struct SectionEntry final
{
    uint32 Id = 0;           ///< SectionId value.
    uint32 Offset = 0;       ///< Byte offset from the blob start.
    uint32 Count = 0;        ///< Record count.
    uint32 ElementBytes = 0; ///< Serialized width of one record, from its field schema.
};
/// Serialized width of one SectionEntry (four uint32 fields).
inline constexpr usize SECTION_ENTRY_BYTES = 4 * sizeof(uint32);
static_assert(sizeof(SectionEntry) == SECTION_ENTRY_BYTES);

/// @brief One cooked path span with chunk-relative float32 controls.
struct PathSpanRecord final
{
    uint32 KindTag = 0;        ///< SpanKindTag.
    uint32 SourceElement = 0;  ///< Source waypoint or span index for diagnostics.
    float64 DomainWidth = 1.0; ///< Source domain width used for continuity scaling; > 0.
    float32 Control[SPAN_CONTROL_COUNT][CONTROL_COMPONENTS]{}; ///< Chunk-relative controls.
};
/// Serialized width of one PathSpanRecord.
inline constexpr usize PATH_SPAN_RECORD_BYTES =
    2 * sizeof(uint32) + sizeof(float64) + SPAN_CONTROL_COUNT * CONTROL_COMPONENTS * sizeof(float32);
static_assert(sizeof(PathSpanRecord) == PATH_SPAN_RECORD_BYTES);

/// @brief One cooked joint: continuity measurements and threshold flags.
struct JointRecord final
{
    float64 PositionGap = 0.0;                 ///< Distance between the two stored endpoints, asset units.
    float64 DirectionAngle = 0.0;              ///< Angle between first derivatives, radians.
    float64 DerivativeDifference = 0.0;        ///< Relative difference of domain-scaled first derivatives.
    float64 SecondDerivativeDifference = 0.0;  ///< Relative difference of domain-scaled second derivatives.
    uint8 ContinuityFlags[JOINT_FLAG_COUNT]{}; ///< C0, G1, C1, C2 as 0 or 1.
    uint8 Reserved[JOINT_RESERVED_BYTES]{};    ///< Zero; keeps the width a multiple of eight and reserved for growth.
};
/// Serialized width of one JointRecord. The reserved bytes make the width a
/// multiple of eight so the record has no host tail padding.
inline constexpr usize JOINT_RECORD_BYTES =
    JOINT_FLAG_COUNT * sizeof(float64) + JOINT_FLAG_COUNT * sizeof(uint8) + JOINT_RESERVED_BYTES * sizeof(uint8);
static_assert(sizeof(JointRecord) == JOINT_RECORD_BYTES);

/// @brief One cooked scalar-track key. Slopes are value units per second.
struct TrackKeyRecord final
{
    float64 Time = 0.0;          ///< Seconds from the local epoch; strictly increasing across keys.
    float32 Value = 0.0F;        ///< Value at Time.
    float32 InSlope = 0.0F;      ///< Derivative arriving at the key.
    float32 OutSlope = 0.0F;     ///< Derivative leaving the key.
    uint32 InterpolationTag = 0; ///< TrackInterpolationTag of the span starting here.
};
/// Serialized width of one TrackKeyRecord.
inline constexpr usize TRACK_KEY_RECORD_BYTES = sizeof(float64) + 3 * sizeof(float32) + sizeof(uint32);
static_assert(sizeof(TrackKeyRecord) == TRACK_KEY_RECORD_BYTES);

/// @brief One cooked bicubic patch with chunk-relative float32 controls.
struct PatchRecord final
{
    uint64 Id = 0; ///< Stable author ID; unique within the surface.
    float32 Control[PATCH_CONTROL_ORDER][PATCH_CONTROL_ORDER][CONTROL_COMPONENTS]{}; ///< Control[u][v].
};
/// Serialized width of one PatchRecord.
inline constexpr usize PATCH_RECORD_BYTES =
    sizeof(uint64) + PATCH_CONTROL_ORDER * PATCH_CONTROL_ORDER * CONTROL_COMPONENTS * sizeof(float32);
static_assert(sizeof(PatchRecord) == PATCH_RECORD_BYTES);

/// @brief One cooked adjacency join of two whole patch sides.
struct AdjacencyRecord final
{
    uint32 FirstPatch = 0;    ///< Index into the patch section.
    uint32 FirstSideTag = 0;  ///< PatchSideTag of the first patch.
    uint32 SecondPatch = 0;   ///< Index into the patch section.
    uint32 SecondSideTag = 0; ///< PatchSideTag of the second patch.
    uint32 IsReversed = 0;    ///< 1 when the second side runs opposite to the first.
};
/// Serialized width of one AdjacencyRecord.
inline constexpr usize ADJACENCY_RECORD_BYTES = 5 * sizeof(uint32);
static_assert(sizeof(AdjacencyRecord) == ADJACENCY_RECORD_BYTES);

/// @brief Maps a prepared element to its stable author ID for inspection.
struct SourceIdentityRecord final
{
    uint64 AuthorId = 0;      ///< Stable author ID of the source element.
    uint32 SourceElement = 0; ///< Prepared span, key or patch index.
    uint32 Reserved = 0;      ///< Zero; keeps the record four-byte aligned and reserved for growth.
};
/// Serialized width of one SourceIdentityRecord.
inline constexpr usize SOURCE_IDENTITY_RECORD_BYTES = sizeof(uint64) + 2 * sizeof(uint32);
static_assert(sizeof(SourceIdentityRecord) == SOURCE_IDENTITY_RECORD_BYTES);

/// Number of fixed uint32 header fields before the uint64/float64 block.
inline constexpr usize HEADER_UINT32_FIELD_COUNT = 10;
/// Serialized width of the fixed header prefix, before the variable section
/// directory and the three trailing digests. Derived from the field schema:
/// the 8-byte magic; HEADER_UINT32_FIELD_COUNT uint32 fields (FormatVersion,
/// CookerVersion, KindTag, PrecisionTag, Flags, SpanCount, JointCount, KeyCount,
/// PatchCount, AdjacencyCount); SourceIdentityCount (uint32); Revision (uint64);
/// Origin[3], AssetScale, PositionFloor, EffectivePositionError and the 12
/// tolerance fields (float64); and SectionCount (uint32).
inline constexpr usize HEADER_FIXED_BYTES = MAGIC.size() + HEADER_UINT32_FIELD_COUNT * sizeof(uint32) +
                                            sizeof(uint32) /*SourceIdentityCount*/ + sizeof(uint64) /*Revision*/ +
                                            (CONTROL_COMPONENTS + 3 + TOLERANCE_FIELD_COUNT) * sizeof(float64) +
                                            sizeof(uint32) /*SectionCount*/;
/// Bytes contributed by the trailing SHA-256 digests.
inline constexpr usize HEADER_DIGEST_BYTES = HEADER_DIGEST_COUNT * DIGEST_BYTES;

/// @brief Minimum header size with no section directory entries.
///
/// A concrete header adds SectionCount * SECTION_ENTRY_BYTES between the fixed
/// prefix and the digests. GetHeaderBytes computes the full header size.
inline constexpr usize HEADER_MIN_BYTES = HEADER_FIXED_BYTES + HEADER_DIGEST_BYTES;

/// @brief Serialized header size for a given section count.
/// @param sectionCount Number of SectionEntry records in the directory.
/// @return HEADER_FIXED_BYTES + sectionCount * SECTION_ENTRY_BYTES + HEADER_DIGEST_BYTES.
[[nodiscard]] constexpr usize GetHeaderBytes(uint32 sectionCount) noexcept
{
    return HEADER_FIXED_BYTES + static_cast<usize>(sectionCount) * SECTION_ENTRY_BYTES + HEADER_DIGEST_BYTES;
}

/// @brief Whether a cooked format version is supported by this build.
[[nodiscard]] constexpr bool IsSupportedVersion(uint32 version) noexcept
{
    return version == FORMAT_VERSION_1;
}

/// @brief Whether a tag names a known geometry kind.
[[nodiscard]] constexpr bool IsKnownKind(uint32 tag) noexcept
{
    return tag == static_cast<uint32>(GeometryKindTag::Path) || tag == static_cast<uint32>(GeometryKindTag::Track) ||
           tag == static_cast<uint32>(GeometryKindTag::Surface);
}

/// @brief Whether a tag names a known span kind.
[[nodiscard]] constexpr bool IsKnownSpanKind(uint32 tag) noexcept
{
    return tag == static_cast<uint32>(SpanKindTag::Line) || tag == static_cast<uint32>(SpanKindTag::Cubic);
}

/// @brief Whether a tag names a known track interpolation.
[[nodiscard]] constexpr bool IsKnownTrackInterpolation(uint32 tag) noexcept
{
    return tag == static_cast<uint32>(TrackInterpolationTag::Step) ||
           tag == static_cast<uint32>(TrackInterpolationTag::Linear) ||
           tag == static_cast<uint32>(TrackInterpolationTag::Hermite);
}

/// @brief Whether a tag names a known patch side.
[[nodiscard]] constexpr bool IsKnownPatchSide(uint32 tag) noexcept
{
    return tag == static_cast<uint32>(PatchSideTag::U0) || tag == static_cast<uint32>(PatchSideTag::U1) ||
           tag == static_cast<uint32>(PatchSideTag::V0) || tag == static_cast<uint32>(PatchSideTag::V1);
}

/// @brief Whether a tag names a known precision profile.
[[nodiscard]] constexpr bool IsKnownPrecision(uint32 tag) noexcept
{
    return tag == static_cast<uint32>(PrecisionProfileTag::Strict) ||
           tag == static_cast<uint32>(PrecisionProfileTag::Visual);
}

/// @brief Element width of a section, or 0 for an unknown section id.
[[nodiscard]] constexpr usize GetSectionElementBytes(SectionId id) noexcept
{
    switch (id)
    {
        case SectionId::PathSpans:
            return PATH_SPAN_RECORD_BYTES;
        case SectionId::PathJoints:
            return JOINT_RECORD_BYTES;
        case SectionId::TrackKeys:
            return TRACK_KEY_RECORD_BYTES;
        case SectionId::SurfacePatches:
            return PATCH_RECORD_BYTES;
        case SectionId::SurfaceAdjacency:
            return ADJACENCY_RECORD_BYTES;
        case SectionId::SourceIdentities:
            return SOURCE_IDENTITY_RECORD_BYTES;
    }
    return 0;
}
} // namespace ludus::content::geometry_format
