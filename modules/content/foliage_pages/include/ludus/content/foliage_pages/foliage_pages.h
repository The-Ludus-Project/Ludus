#pragma once

#include <ludus/foundation/base/compiler.h>
#include <ludus/foundation/base/types.h>

#include <ludus/foundation/containers/borrowed_view.hpp>
#include <ludus/foundation/containers/static_array.hpp>

#include <ludus/content/foliage_format/lufol.h>

/// @file
/// Encoder and validating reader for LUFOL foliage placement page sets (Ludus::FoliagePages).
///
/// The wire contract is ludus/content/foliage_format/lufol.h; the design is in
/// docs/architecture/foliage.md, "Cooked pages and publication". Encoding and
/// reading apply the same rules, so every page set the encoder accepts reads back
/// unchanged and every page set the reader accepts re-encodes to identical bytes.
/// Neither operation allocates, performs I/O or depends on host endianness.
namespace ludus::content::foliage_pages
{
using foundation::int16;
using foundation::int32;
using foundation::int64;
using foundation::uint16;
using foundation::uint32;
using foundation::uint64;
using foundation::uint8;
using foundation::usize;

/// Result of a page set encode, read or lookup.
enum class FoliagePageStatus : uint8
{
    Ok,                 ///< The operation succeeded.
    InvalidArgument,    ///< A view, limit or output argument violates the API contract.
    Malformed,          ///< The page set violates a structural or canonical rule of the format.
    UnsupportedVersion, ///< The page set names a format version or domain schema this reader does not implement.
    RevisionMismatch,   ///< The page set belongs to a different terrain domain or revision than expected.
    LimitExceeded,      ///< A count or byte size exceeds a format or configured admission limit.
    CapacityExceeded,   ///< The caller's output storage is smaller than the encoded page set.
    InvalidHandle,      ///< The view is empty or an index is out of range.
    NotFound,           ///< No page has the requested address.
};

/// The rule a rejected page set violates; names the table and field to repair.
enum class FoliagePageIssue : uint8
{
    None,               ///< No problem.
    Limits,             ///< A configured limit exceeds its format limit.
    OutputView,         ///< The output view is invalid.
    InputView,          ///< The input bytes or a description table view is invalid.
    Size,               ///< The byte size is wrong for the header counts, or exceeds the byte limit.
    Magic,              ///< The magic bytes differ from LUFOL01.
    FormatVersion,      ///< The format version is not implemented.
    GeneratorVersion,   ///< The generator version is zero.
    DomainSchema,       ///< The domain schema is not implemented.
    DomainId,           ///< The domain ID is zero, or differs from the expected domain.
    TerrainRevision,    ///< The terrain revision differs from the expected revision.
    PageSize,           ///< The page size is zero or exceeds MAX_PAGE_SIZE.
    LayerCount,         ///< Too many layers.
    SpeciesCount,       ///< Too many species.
    PageCount,          ///< Too many pages.
    PlacementCount,     ///< Too many placements, or the pages do not own every placement exactly once.
    LayerOrder,         ///< A layer ID does not exceed the previous one; Index names the later layer.
    LayerSlots,         ///< A layer's slot count is zero or too large; Index names it.
    LayerCellSize,      ///< A layer's cell size is zero, too large or does not divide the page size.
    SpeciesKey,         ///< A species key is zero or does not exceed the previous one; Index names it.
    SpeciesVariants,    ///< A species' variant count is zero or too large; Index names it.
    PageOrder,          ///< A page address does not follow the previous one in (Z, X) order; Index names it.
    PageRange,          ///< A page's first placement does not continue the previous page's run; Index names it.
    PagePlacementCount, ///< A page owns too many placements; Index names it.
    PageBounds,         ///< A page's bounds are inverted, or nonzero for an empty page; Index names it.
    PlacementLayer,     ///< A placement names an absent layer; Index names the placement.
    PlacementSlot,      ///< A placement's slot is not below its layer's slot count.
    PlacementOffset,    ///< A placement's cell offset is not below its layer's cell size.
    PlacementPage,      ///< A placement's root is outside its page's half-open extent.
    PlacementOrder,     ///< A placement does not follow the previous one in identity order, or repeats it.
    PlacementScale,     ///< A placement's scale is zero.
    PlacementNormal,    ///< A placement's up direction is longer than one.
    PlacementSpecies,   ///< A placement names an absent species.
    PlacementVariant,   ///< A placement names an absent variant of its species.
    PlacementBounds,    ///< A placement's root lies outside its page's bounds.
};

/// Failure detail without owned text.
struct FoliagePageDiagnostic final
{
    FoliagePageStatus Status = FoliagePageStatus::Ok; ///< Same value as the returned status.
    FoliagePageIssue Issue = FoliagePageIssue::None;  ///< Violated rule; None on success.
    usize Index = 0;                                  ///< Offending table record, when Issue names one; otherwise zero.
    usize Offset = 0; ///< Reads only: first byte of the offending header field or record; otherwise zero.
};

/// Producer-supplied identity of every placement-affecting input; the page set's cache key.
using FoliageInputDigest = foundation::StaticArray<uint8, foliage_format::INPUT_DIGEST_BYTES>;

/// @brief Admission limits a consumer applies to one page set.
///
/// The defaults are the format limits. A profile may lower any of them; values
/// above the format limits are rejected as InvalidArgument.
struct FoliagePageLimits final
{
    usize MaximumBytes = foliage_format::MAX_PAGE_SET_BYTES;                   ///< Encoded byte ceiling.
    uint32 MaximumLayers = foliage_format::MAX_LAYERS;                         ///< Layer table ceiling.
    uint32 MaximumSpecies = foliage_format::MAX_SPECIES;                       ///< Species table ceiling.
    uint32 MaximumPages = foliage_format::MAX_PAGES;                           ///< Page directory ceiling.
    uint32 MaximumPlacements = foliage_format::MAX_PLACEMENTS;                 ///< Decoded placement ceiling.
    uint32 MaximumPlacementsPerPage = foliage_format::MAX_PLACEMENTS_PER_PAGE; ///< Per-page placement ceiling.
};

/// @brief Page set header fields other than the format version, domain schema and table counts.
struct FoliagePageSetHeader final
{
    uint32 GeneratorVersion = 0;      ///< Nonzero cooker version; part of the cache key.
    uint32 PageSize = 0;              ///< Page edge in lattice units, 1..MAX_PAGE_SIZE.
    uint64 DomainId = 0;              ///< Nonzero persistent terrain domain ID.
    int64 OriginX = 0;                ///< Terrain sample column of the lattice origin.
    int64 OriginZ = 0;                ///< Terrain sample row of the lattice origin.
    uint64 TerrainRevision = 0;       ///< Terrain revision the page set was cooked against.
    FoliageInputDigest InputDigest{}; ///< Digest of every placement-affecting input across the halo.
};

/// @brief One placement layer's lattice.
struct FoliageLayer final
{
    uint16 Id = 0;           ///< Authored layer ID, unique in the page set.
    uint16 SlotsPerCell = 0; ///< Candidate slots per cell, 1..MAX_SLOTS_PER_CELL.
    uint32 CellSize = 0;     ///< Cell edge in lattice units, 1..MAX_CELL_SIZE; divides the page size.
};

/// @brief One species referenced by placements.
///
/// The key identifies a species asset through the World's species binding; the
/// page set does not name meshes or materials, so render-only edits keep its bytes.
struct FoliageSpecies final
{
    uint64 Key = 0;               ///< Nonzero persistent species key, unique in the page set.
    uint64 PlacementRevision = 0; ///< Revision of the species' placement-affecting properties.
    uint32 VariantCount = 0;      ///< Mesh/material variants, 1..MAX_VARIANTS_PER_SPECIES.
};

/// @brief Conservative bounds of one page's instances; inclusive on every axis.
struct FoliagePageBounds final
{
    int64 MinimumX = 0;      ///< Lowest X in lattice units.
    int64 MinimumZ = 0;      ///< Lowest Z in lattice units.
    int64 MaximumX = 0;      ///< Highest X in lattice units.
    int64 MaximumZ = 0;      ///< Highest Z in lattice units.
    int32 MinimumHeight = 0; ///< Lowest height in root height units.
    int32 MaximumHeight = 0; ///< Highest height in root height units.

    /// Whether every field matches.
    [[nodiscard]] constexpr bool operator==(const FoliagePageBounds&) const noexcept = default;
};

/// @brief One page directory entry.
struct FoliagePage final
{
    int32 PageX = 0;            ///< Page column; owns lattice X in [PageX * PageSize, (PageX + 1) * PageSize).
    int32 PageZ = 0;            ///< Page row; owns lattice Z likewise.
    uint32 FirstPlacement = 0;  ///< Index of the page's first placement.
    uint32 PlacementCount = 0;  ///< Placements owned by the page.
    FoliagePageBounds Bounds{}; ///< Conservative instance bounds; all zero when the page is empty.
};

/// @brief One cooked placement in canonical form.
///
/// (Layer, CellX, CellZ, Slot) with the page set's domain is the persistent
/// candidate identity. Render and collision derive their transforms from the
/// same record.
struct FoliagePlacement final
{
    uint16 Layer = 0;   ///< Layer ID.
    uint16 Slot = 0;    ///< Slot ordinal within the cell.
    int32 CellX = 0;    ///< Cell column in the layer's lattice.
    int32 CellZ = 0;    ///< Cell row in the layer's lattice.
    uint16 OffsetX = 0; ///< Root X offset within the cell, lattice units.
    uint16 OffsetZ = 0; ///< Root Z offset within the cell, lattice units.
    int32 Height = 0;   ///< Root height in root height units.
    uint16 Yaw = 0;     ///< Yaw in 1/YAW_UNITS_PER_TURN turns.
    uint16 Scale = 0;   ///< Nonzero uniform scale in SCALE_UNITS_PER_ONE fixed point.
    int16 NormalX = 0;  ///< Up-direction X in NORMAL_UNITS fixed point; zero with NormalZ means vertical.
    int16 NormalZ = 0;  ///< Up-direction Z in NORMAL_UNITS fixed point.
    uint16 Species = 0; ///< Species table index.
    uint16 Variant = 0; ///< Variant index within the species.

    /// Whether every field matches.
    [[nodiscard]] constexpr bool operator==(const FoliagePlacement&) const noexcept = default;
};

/// @brief A placement root in lattice units relative to the domain origin.
struct FoliageRoot final
{
    int64 X = 0; ///< Lattice X.
    int64 Z = 0; ///< Lattice Z.
};

/// @brief Compute a placement's root from its layer's cell size.
///
/// Exact for every admitted record: |cell| < 2^31 and CellSize <= 2^16 keep the result below 2^48.
/// @param cellSize The placement's layer CellSize, 1..MAX_CELL_SIZE.
/// @param placement The placement.
[[nodiscard]] constexpr FoliageRoot ComputeFoliageRoot(uint32 cellSize, const FoliagePlacement& placement) noexcept
{
    return
    {
        .X = int64{placement.CellX} * int64{cellSize} + int64{placement.OffsetX},
        .Z = int64{placement.CellZ} * int64{cellSize} + int64{placement.OffsetZ},
    };
}

/// @brief Borrowed input to the encoder. The caller keeps every table alive during the call.
struct FoliagePageSetDescription final
{
    FoliagePageSetHeader Header{};                               ///< Header fields.
    foundation::BorrowedView<const FoliageLayer> Layers;         ///< Layers, ID ascending.
    foundation::BorrowedView<const FoliageSpecies> Species;      ///< Species, key ascending.
    foundation::BorrowedView<const FoliagePage> Pages;           ///< Pages in (Z, X) order with contiguous runs.
    foundation::BorrowedView<const FoliagePlacement> Placements; ///< Placements in page runs, identity ascending.
};

/// @brief Terrain binding a reader requires of a page set.
struct FoliagePageBinding final
{
    uint64 DomainId = 0;        ///< Expected terrain domain ID.
    uint64 TerrainRevision = 0; ///< Expected terrain revision.
};

/// @brief Validated, read-only view of a LUFOL page set held in caller-owned bytes.
///
/// Records are decoded field by field on access; nothing is copied. Whoever owns
/// the bytes must keep them alive and unmodified while the view is used. Views
/// are trivially copyable and need no synchronization for concurrent reads. A
/// default-constructed view is empty and every lookup reports InvalidHandle.
class FoliagePageSetView final
{
public:
    /// Constructs an empty view.
    constexpr FoliagePageSetView() noexcept = default;

    /// Whether the view holds a validated page set.
    [[nodiscard]] LUDUS_INLINE bool IsValid() const noexcept
    {
        return mBytesOrNull != nullptr;
    }
    /// Layer records; zero when empty.
    [[nodiscard]] LUDUS_INLINE uint32 GetLayerCount() const noexcept
    {
        return mLayerCount;
    }
    /// Species records; zero when empty.
    [[nodiscard]] LUDUS_INLINE uint32 GetSpeciesCount() const noexcept
    {
        return mSpeciesCount;
    }
    /// Page records; zero when empty.
    [[nodiscard]] LUDUS_INLINE uint32 GetPageCount() const noexcept
    {
        return mPageCount;
    }
    /// Placement records; zero when empty.
    [[nodiscard]] LUDUS_INLINE uint32 GetPlacementCount() const noexcept
    {
        return mPlacementCount;
    }

    /// @brief Decodes the header.
    /// @return The header fields, or default values for an empty view.
    [[nodiscard]] FoliagePageSetHeader GetHeader() const noexcept;
    /// @brief Decodes one layer.
    /// @param index Layer index below GetLayerCount().
    /// @param outLayer Receives the layer; unchanged on failure.
    /// @return Ok, or InvalidHandle for an empty view or an out-of-range index.
    [[nodiscard]] FoliagePageStatus TryGetLayer(usize index, FoliageLayer& outLayer) const noexcept;
    /// @brief Decodes one species.
    /// @param index Species index below GetSpeciesCount().
    /// @param outSpecies Receives the species; unchanged on failure.
    /// @return Ok, or InvalidHandle for an empty view or an out-of-range index.
    [[nodiscard]] FoliagePageStatus TryGetSpecies(usize index, FoliageSpecies& outSpecies) const noexcept;
    /// @brief Decodes one page directory entry.
    /// @param index Page index below GetPageCount().
    /// @param outPage Receives the page; unchanged on failure.
    /// @return Ok, or InvalidHandle for an empty view or an out-of-range index.
    [[nodiscard]] FoliagePageStatus TryGetPage(usize index, FoliagePage& outPage) const noexcept;
    /// @brief Decodes one placement.
    /// @param index Placement index below GetPlacementCount(); a page's run is
    /// [FirstPlacement, FirstPlacement + PlacementCount).
    /// @param outPlacement Receives the placement; unchanged on failure.
    /// @return Ok, or InvalidHandle for an empty view or an out-of-range index.
    [[nodiscard]] FoliagePageStatus TryGetPlacement(usize index, FoliagePlacement& outPlacement) const noexcept;
    /// @brief Finds a page by address with a binary search of the directory.
    /// @param pageX Page column.
    /// @param pageZ Page row.
    /// @param outIndex Receives the page index; unchanged on failure.
    /// @return Ok, NotFound, or InvalidHandle for an empty view.
    [[nodiscard]] FoliagePageStatus TryFindPage(int32 pageX, int32 pageZ, usize& outIndex) const noexcept;

private:
    const uint8* mBytesOrNull = nullptr;
    uint32 mLayerCount = 0;
    uint32 mSpeciesCount = 0;
    uint32 mPageCount = 0;
    uint32 mPlacementCount = 0;

    friend FoliagePageStatus ReadFoliagePageSet(foundation::BorrowedView<const uint8> bytes,
                                                const FoliagePageBinding& binding,
                                                const FoliagePageLimits& limits,
                                                FoliagePageSetView& outView,
                                                FoliagePageDiagnostic& outDiagnostic) noexcept;
};

/// @brief Validates a description and returns its encoded size.
/// @param description Page set to encode; every table view must be valid (an empty table may be empty).
/// @param limits Admission limits, at most the format limits.
/// @param outBytes Receives the encoded size; unchanged on failure.
/// @param outDiagnostic Receives the status and, on failure, the violated rule and record index.
/// @return Ok, InvalidArgument, Malformed or LimitExceeded.
[[nodiscard]] FoliagePageStatus GetFoliagePageSetBytes(const FoliagePageSetDescription& description,
                                                       const FoliagePageLimits& limits,
                                                       usize& outBytes,
                                                       FoliagePageDiagnostic& outDiagnostic) noexcept;

/// @brief Validates a description and encodes it as a canonical LUFOL page set.
///
/// The description is checked completely before the first byte is written, so a
/// rejected description leaves the output untouched.
/// @param description Page set to encode.
/// @param limits Admission limits, at most the format limits.
/// @param outBytes Receives the page set in its prefix; must hold GetFoliagePageSetBytes bytes.
/// @param outWritten Receives the encoded size; unchanged on failure.
/// @param outDiagnostic Receives the status and, on failure, the violated rule and record index.
/// @return Ok, InvalidArgument, Malformed, LimitExceeded or CapacityExceeded.
[[nodiscard]] FoliagePageStatus WriteFoliagePageSet(const FoliagePageSetDescription& description,
                                                    const FoliagePageLimits& limits,
                                                    foundation::BorrowedView<uint8> outBytes,
                                                    usize& outWritten,
                                                    FoliagePageDiagnostic& outDiagnostic) noexcept;

/// @brief Validates a LUFOL page set and binds a view to the caller's bytes.
///
/// The byte limit is checked before any field is read. Then every header field,
/// table count, record field, cross reference, ordering and ownership rule is
/// checked before the view becomes visible. Unaligned input is supported.
/// @param bytes The complete page set; trailing bytes are rejected. The caller keeps them alive and unmodified
/// while the view is used.
/// @param binding Terrain domain and revision the caller expects; a page set for any other is rejected.
/// @param limits Admission limits, at most the format limits.
/// @param outView Receives the view on success; unchanged on failure.
/// @param outDiagnostic Receives the status and, on failure, the violated rule, record index and byte offset.
/// @return Ok, InvalidArgument, Malformed, UnsupportedVersion, RevisionMismatch or LimitExceeded.
[[nodiscard]] FoliagePageStatus ReadFoliagePageSet(foundation::BorrowedView<const uint8> bytes,
                                                   const FoliagePageBinding& binding,
                                                   const FoliagePageLimits& limits,
                                                   FoliagePageSetView& outView,
                                                   FoliagePageDiagnostic& outDiagnostic) noexcept;
} // namespace ludus::content::foliage_pages
