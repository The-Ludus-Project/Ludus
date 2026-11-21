#pragma once

#include <ludus/foundation/base/types.h>

#include <ludus/content/terrain_format/limits.h>

#include <string_view>

/// @file
/// Cooked terrain revision format (Ludus::TerrainFormat): a LUTRN manifest plus independently readable LUTPG pages.
///
/// This header is the contract between terrain cookers (Ludus::ContentTerrain, later the Editor composer) and
/// terrain readers (the ContentTerrain reader, later runtime residency). It is owned by neither side and depends
/// only on FoundationBase. See "Cooked format and validation" in docs/architecture/terrain-generation.md and
/// "Versioned page cooking" in docs/modules/content/terrain.md.
///
/// Every field is little endian. The wire structs below document the exact layout and are checked with
/// static_asserts; a codec serialises them field by field and never copies the structs, so host endianness and
/// padding never reach a file.
///
/// A revision is two byte sequences:
///
///   1. Manifest: ManifestHeader, then PageCount PageRecord entries in canonical order (rows of pages along +Z,
///      columns along +X within a row), then a 16-byte XXH3-128 digest (FoundationHash StableFingerprint128,
///      Low then High) of every preceding manifest byte.
///   2. Page pack: the page blobs concatenated in canonical order. Each record names its blob's byte range, so a
///      reader can fetch and decode one page from its range and the manifest alone.
///
/// A page blob is a PageHeader followed by the height payload. The payload stores the
/// (CellsPerSide + 1 + 2 * HaloSamples)^2 codes of the page's extended window: first the (CellsPerSide + 1)^2 core
/// codes in row-major order (exactly the ContentTerrain tile height layout version 1), then the halo codes, which
/// are the remaining samples of the extended window in row-major order. A halo sample inside the domain repeats
/// the code of the page that owns that sample; outside the domain it follows the manifest's boundary extension.
/// The record's payload digest covers the payload only, so a page recooked at a later revision with unchanged
/// heights keeps its digest.
namespace ludus::content::terrain_format
{
using foundation::float32;
using foundation::int32;
using foundation::int64;
using foundation::uint16;
using foundation::uint32;
using foundation::uint64;
using foundation::uint8;
using foundation::usize;

/// Bytes in a file magic, including the trailing NUL.
inline constexpr usize MAGIC_BYTES = 8;
/// Manifest magic.
inline constexpr std::string_view MANIFEST_MAGIC{"LUTRN01\0", MAGIC_BYTES};
/// Page blob magic.
inline constexpr std::string_view PAGE_MAGIC{"LUTPG01\0", MAGIC_BYTES};
/// Version 1: one level-zero height channel with a one-sample halo and clamp-to-edge domain boundaries.
inline constexpr uint32 FORMAT_VERSION_1 = 1;
/// Current wire layout version; readers reject any other value.
inline constexpr uint32 FORMAT_VERSION = FORMAT_VERSION_1;
/// Cooker version, bumped when the cooked bytes change for an unchanged source and layout. It is part of the
/// cache key and independent of FORMAT_VERSION; readers accept every cooker version of a supported format.
inline constexpr uint32 COOKER_VERSION = 1;
/// Width of an XXH3-128 digest: Low then High, eight little-endian bytes each.
inline constexpr usize DIGEST_BYTES = 16;

/// @brief Integer representation of one height code; matches ContentTerrain's TerrainHeightEncoding order.
// NOLINTNEXTLINE(performance-enum-size): LUTRN wire tags have a fixed uint32 representation.
enum class HeightEncodingTag : uint32
{
    Unsigned16 = 0, ///< Two-byte unsigned code.
    Signed32 = 1,   ///< Four-byte two's-complement code.
};

/// @brief Data carried by a page. Version 1 cooks heights only.
// NOLINTNEXTLINE(performance-enum-size): LUTRN wire tags have a fixed uint32 representation.
enum class ChannelTag : uint32
{
    Height = 0, ///< Height codes at lattice samples.
};

/// @brief How halo samples outside the terrain domain are filled.
// NOLINTNEXTLINE(performance-enum-size): LUTRN wire tags have a fixed uint32 representation.
enum class BoundaryExtensionTag : uint32
{
    ClampToEdge = 0, ///< Repeat the nearest domain sample, so a one-sided difference results at the boundary.
};

/// @brief Wire layout of the manifest header (MANIFEST_HEADER_BYTES).
struct ManifestHeader final
{
    char Magic[MAGIC_BYTES]{};          ///< MANIFEST_MAGIC.
    uint32 FormatVersion = 0;           ///< FORMAT_VERSION.
    uint32 CookerVersion = 0;           ///< COOKER_VERSION of the producer.
    uint32 RecipeVersion = 0;           ///< Caller's generation recipe version, recorded for replay.
    uint32 Encoding = 0;                ///< HeightEncodingTag shared by every page.
    uint32 CellsPerPage = 0;            ///< Page edge in cells, 1 through MAX_PAGE_AXIS_CELLS.
    uint32 HaloSamples = 0;             ///< PAGE_HALO_SAMPLES.
    uint32 BoundaryExtension = 0;       ///< BoundaryExtensionTag.
    uint32 PagesX = 0;                  ///< Page columns along +X.
    uint32 PagesZ = 0;                  ///< Page rows along +Z.
    uint32 Flags = 0;                   ///< Reserved; must be zero.
    uint64 Revision = 0;                ///< Terrain revision this manifest publishes.
    int64 FirstPageX = 0;               ///< Tile column of the first page.
    int64 FirstPageZ = 0;               ///< Tile row of the first page.
    float32 OriginMetres = 0;           ///< Height of code zero, finite.
    float32 QuantumMetres = 0;          ///< Height per code, finite and positive.
    float32 SpacingMetres = 0;          ///< Horizontal sample spacing on both axes, finite and positive.
    uint32 PageCount = 0;               ///< PagesX times PagesZ.
    uint8 SourceDigest[DIGEST_BYTES]{}; ///< Caller's digest of the source that produced the revision.
    uint64 PagePackBytes = 0;           ///< Sum of every record's ByteSize.
};

/// @brief Wire layout of one page record (PAGE_RECORD_BYTES).
struct PageRecord final
{
    int64 PageX = 0;                     ///< Tile column; equals FirstPageX plus the column in canonical order.
    int64 PageZ = 0;                     ///< Tile row; equals FirstPageZ plus the row in canonical order.
    uint64 ContentRevision = 0;          ///< Revision that last cooked this page; at most the manifest revision.
    uint64 ByteOffset = 0;               ///< Offset of the page blob in the page pack; blobs are contiguous.
    uint32 ByteSize = 0;                 ///< Page blob bytes; GetPageBlobBytes of the manifest layout.
    uint32 Channel = 0;                  ///< ChannelTag.
    uint32 Level = 0;                    ///< Detail level; version 1 cooks level zero only.
    int32 MinimumCode = 0;               ///< Lowest core code.
    int32 MaximumCode = 0;               ///< Highest core code.
    float32 MaximumErrorMetres = 0;      ///< Largest |decoded - source| over the core, rounded to binary32.
    uint8 PayloadDigest[DIGEST_BYTES]{}; ///< XXH3-128 of the page payload, excluding its header.
};

/// @brief Wire layout of a page blob header (PAGE_HEADER_BYTES).
struct PageHeader final
{
    char Magic[MAGIC_BYTES]{};  ///< PAGE_MAGIC.
    uint32 FormatVersion = 0;   ///< FORMAT_VERSION.
    uint32 Channel = 0;         ///< ChannelTag; equals the record.
    uint32 Level = 0;           ///< Equals the record.
    uint32 Encoding = 0;        ///< Equals the manifest.
    uint32 CellsPerSide = 0;    ///< Equals the manifest's CellsPerPage.
    uint32 HaloSamples = 0;     ///< Equals the manifest.
    int64 PageX = 0;            ///< Equals the record.
    int64 PageZ = 0;            ///< Equals the record.
    uint64 ContentRevision = 0; ///< Equals the record.
    float32 OriginMetres = 0;   ///< Equals the manifest.
    float32 QuantumMetres = 0;  ///< Equals the manifest.
    float32 SpacingMetres = 0;  ///< Equals the manifest.
    uint32 PayloadBytes = 0;    ///< Bytes after this header.
};

/// uint32 fields of ManifestHeader between the magic and the revision.
inline constexpr usize MANIFEST_HEADER_LEADING_WORDS = 10;
/// float32 fields of ManifestHeader and PageHeader: origin, quantum and spacing.
inline constexpr usize CODEC_SCALARS = 3;
/// uint32 fields of PageHeader between the magic and the page coordinates.
inline constexpr usize PAGE_HEADER_LEADING_WORDS = 6;
/// uint32 fields of PageRecord: byte size, channel and level.
inline constexpr usize PAGE_RECORD_WORDS = 3;

/// Serialized manifest header width, from the field schema: magic, leading words, revision, two page coordinates,
/// codec scalars, the page count, the source digest and the page pack size.
inline constexpr usize MANIFEST_HEADER_BYTES = MAGIC_BYTES + MANIFEST_HEADER_LEADING_WORDS * sizeof(uint32) +
                                               sizeof(uint64) + 2 * sizeof(int64) + CODEC_SCALARS * sizeof(float32) +
                                               sizeof(uint32) + DIGEST_BYTES + sizeof(uint64);
/// Serialized page record width: two coordinates, revision and offset, record words, two codes, the error and
/// the payload digest.
inline constexpr usize PAGE_RECORD_BYTES = 2 * sizeof(int64) + 2 * sizeof(uint64) + PAGE_RECORD_WORDS * sizeof(uint32) +
                                           2 * sizeof(int32) + sizeof(float32) + DIGEST_BYTES;
/// Serialized page header width: magic, leading words, two coordinates, the revision, codec scalars and the
/// payload size.
inline constexpr usize PAGE_HEADER_BYTES = MAGIC_BYTES + PAGE_HEADER_LEADING_WORDS * sizeof(uint32) +
                                           2 * sizeof(int64) + sizeof(uint64) + CODEC_SCALARS * sizeof(float32) +
                                           sizeof(uint32);

static_assert(sizeof(ManifestHeader) == MANIFEST_HEADER_BYTES, "ManifestHeader documents an unpadded layout");
static_assert(sizeof(PageRecord) == PAGE_RECORD_BYTES, "PageRecord documents an unpadded layout");
static_assert(sizeof(PageHeader) == PAGE_HEADER_BYTES, "PageHeader documents an unpadded layout");

/// @brief Bytes of one height code for an encoding tag; zero for an unknown tag.
[[nodiscard]] constexpr usize GetHeightCodeBytes(HeightEncodingTag encoding) noexcept
{
    switch (encoding)
    {
        case HeightEncodingTag::Unsigned16:
            return sizeof(uint16);
        case HeightEncodingTag::Signed32:
            return sizeof(int32);
    }
    return 0;
}

/// @brief Samples on one edge of a page's extended window: the core endpoints plus PAGE_HALO_SAMPLES on both sides.
/// @pre cellsPerSide is at most MAX_PAGE_AXIS_CELLS.
[[nodiscard]] constexpr usize GetExtendedPageAxisSamples(uint32 cellsPerSide) noexcept
{
    return static_cast<usize>(cellsPerSide) + 1 + (2 * static_cast<usize>(PAGE_HALO_SAMPLES));
}

/// @brief Payload bytes of one page: every extended-window code.
/// @pre cellsPerSide is at most MAX_PAGE_AXIS_CELLS; encoding is a known tag.
[[nodiscard]] constexpr usize GetPagePayloadBytes(HeightEncodingTag encoding, uint32 cellsPerSide) noexcept
{
    const usize samples = GetExtendedPageAxisSamples(cellsPerSide);
    return samples * samples * GetHeightCodeBytes(encoding);
}

/// @brief Bytes of one page blob: the header plus the payload.
/// @pre The arguments satisfy GetPagePayloadBytes.
[[nodiscard]] constexpr usize GetPageBlobBytes(HeightEncodingTag encoding, uint32 cellsPerSide) noexcept
{
    return PAGE_HEADER_BYTES + GetPagePayloadBytes(encoding, cellsPerSide);
}

/// @brief Bytes of a manifest naming pageCount pages.
/// @pre pageCount is at most MAX_REVISION_PAGES.
[[nodiscard]] constexpr usize GetManifestBytes(uint32 pageCount) noexcept
{
    return MANIFEST_HEADER_BYTES + (static_cast<usize>(pageCount) * PAGE_RECORD_BYTES) + DIGEST_BYTES;
}

/// Largest page blob: a 32-bit page of MAX_PAGE_AXIS_CELLS cells with its halo.
inline constexpr usize MAX_PAGE_BLOB_BYTES = GetPageBlobBytes(HeightEncodingTag::Signed32, MAX_PAGE_AXIS_CELLS);
/// Largest manifest: MAX_REVISION_PAGES records.
inline constexpr usize MAX_MANIFEST_BYTES = GetManifestBytes(MAX_REVISION_PAGES);

static_assert(MAX_PAGE_BLOB_BYTES <= ~uint32{0}, "PageRecord::ByteSize and PageHeader::PayloadBytes are uint32");
static_assert(MAX_DOMAIN_AXIS_CELLS % MAX_PAGE_AXIS_CELLS == 0, "the largest pages tile the largest domain");
} // namespace ludus::content::terrain_format
