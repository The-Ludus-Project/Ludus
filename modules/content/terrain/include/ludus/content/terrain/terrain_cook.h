#pragma once

// Versioned terrain page cooking of slice T1: quantize one complete elevation field into a revision manifest and
// independently readable height pages (Ludus::TerrainFormat), validate shared borders and halos, read one page
// from its byte range, and recook only the pages an edit reaches. Contracts:
// docs/architecture/terrain-generation.md#cooked-format-and-validation and
// docs/modules/content/terrain.md#versioned-page-cooking.

#include <ludus/foundation/base/core.h>

#include <ludus/content/terrain/terrain_fields.h>
#include <ludus/content/terrain/terrain_tile.h>
#include <ludus/foundation/containers/array.hpp>
#include <ludus/foundation/containers/borrowed_view.hpp>
#include <ludus/foundation/hash/hash.hpp>

namespace ludus::content::terrain
{
/// Outcome of a cook, read or recook. Every non-success status preserves the caller's outputs.
enum class TerrainCookStatus : uint8
{
    /// The requested result was written.
    Success,
    /// A layout, page address, region or the relationship between inputs violates the operation's contract.
    InvalidArgument,
    /// An input view is invalid, or the elevation field does not hold one value per domain sample.
    InvalidStorage,
    /// Page or sample coordinates exceed the signed 64-bit lattice.
    CoordinateOutOfRange,
    /// A source height is nonfinite or falls outside the revision codec's range.
    HeightOutOfRange,
    /// The domain, page count or a caller-owned output view exceeds the admitted size.
    LimitExceeded,
    /// Storage for the result could not be allocated.
    AllocationFailed,
    /// Pages disagree on a shared core sample or a halo sample.
    BorderMismatch,
    /// The bytes name an unsupported format version.
    UnsupportedVersion,
    /// The bytes are truncated, carry trailing data, a wrong magic, an unknown tag or an invalid field or range.
    MalformedData,
    /// A manifest or page payload digest does not match its bytes.
    DigestMismatch,
    /// A page blob is well formed but belongs to another page, revision or layout than its manifest record.
    PageMismatch,
};

/// @brief Layout and identity shared by every page of one terrain revision.
///
/// The domain covers PagesX by PagesZ pages of CellsPerPage cells, starting at tile FirstPage, so its elevation
/// field has PagesX * CellsPerPage + 1 columns and PagesZ * CellsPerPage + 1 rows. Field sample (0, 0) is the
/// first page's minimum global sample.
struct TerrainRevisionLayout final
{
    /// Revision the manifest publishes. A recook must name a later revision than its base.
    uint64 Revision = 0;
    /// Caller's generation recipe version, recorded for replay and not interpreted by the cooker. A field generated
    /// by terrain_noise.h records its TerrainNoiseLayout version (TERRAIN_NOISE_LAYOUT_VERSION for new content), so
    /// a replay regenerates it with the same fade.
    uint32 RecipeVersion = 0;
    /// Caller's digest of the source (recipe, imports and ordered edits) that produced the field.
    foundation::Fingerprint128 SourceDigest{};
    /// Tile address of the page with the smallest X and Z.
    TerrainTileAddress FirstPage{};
    /// Page columns along +X; at least one.
    uint32 PagesX = 0;
    /// Page rows along +Z; at least one.
    uint32 PagesZ = 0;
    /// Page edge in cells, 1 through terrain_format::MAX_PAGE_AXIS_CELLS.
    uint32 CellsPerPage = 0;
    /// Common positive, finite horizontal sample spacing in metres.
    float32 SpacingMetres = 1;
    /// Height codec shared by every page.
    TerrainHeightCodec Codec{};
};

/// @brief Manifest entry of one page.
struct TerrainPageRecord final
{
    /// Tile address of the page.
    TerrainTileAddress Address{};
    /// Revision that last cooked the page; at most the manifest revision.
    uint64 ContentRevision = 0;
    /// Offset of the page blob in the page pack. Zero for a page that has not been assembled yet.
    uint64 ByteOffset = 0;
    /// Bytes of the page blob.
    uint32 ByteSize = 0;
    /// Lowest core height code.
    int32 MinimumCode = 0;
    /// Highest core height code.
    int32 MaximumCode = 0;
    /// Largest |decoded height - source elevation| over the core samples in metres, rounded to binary32.
    float32 MaximumErrorMetres = 0;
    /// XXH3-128 of the page payload, excluding its header. Equal heights give an equal digest at any revision.
    foundation::Fingerprint128 PayloadDigest{};
};

/// @brief Move-only owner of one cooked page blob and its record, before or after assembly.
class TerrainCookedPage final
{
public:
    /// Create an empty page without allocation.
    TerrainCookedPage() noexcept = default;
    /// Transfer the blob and record, leaving the source empty.
    TerrainCookedPage(TerrainCookedPage&& other) noexcept = default;
    /// Replace this page by transferring the blob and record, leaving a distinct source empty.
    TerrainCookedPage& operator=(TerrainCookedPage&& other) noexcept = default;

    /// Whether no blob is held.
    [[nodiscard]] LUDUS_INLINE bool IsEmpty() const noexcept
    {
        return mBytes.IsEmpty();
    }

    /// The page's record. ByteOffset is meaningful only for a page extracted from an assembled revision.
    [[nodiscard]] LUDUS_INLINE const TerrainPageRecord& GetRecord() const noexcept
    {
        return mRecord;
    }

    /// Borrow the complete page blob for as long as this page remains alive and unchanged.
    [[nodiscard]] LUDUS_INLINE foundation::BorrowedView<const uint8> GetBytes() const noexcept
    {
        return {mBytes.GetData(), mBytes.GetSize()};
    }

private:
    friend struct TerrainCookedPageAccess;

    TerrainPageRecord mRecord;
    foundation::Array<uint8> mBytes;
};

/// @brief Move-only owner of a complete cooked revision: the manifest and the page pack it indexes.
class TerrainCookedRevision final
{
public:
    /// Create an empty revision without allocation.
    TerrainCookedRevision() noexcept = default;
    /// Transfer both byte sequences, leaving the source empty.
    TerrainCookedRevision(TerrainCookedRevision&& other) noexcept = default;
    /// Replace this revision by transferring both byte sequences, leaving a distinct source empty.
    TerrainCookedRevision& operator=(TerrainCookedRevision&& other) noexcept = default;

    /// Whether no manifest is held.
    [[nodiscard]] LUDUS_INLINE bool IsEmpty() const noexcept
    {
        return mManifestBytes.IsEmpty();
    }

    /// Borrow the manifest bytes for as long as this revision remains alive and unchanged.
    [[nodiscard]] LUDUS_INLINE foundation::BorrowedView<const uint8> GetManifestBytes() const noexcept
    {
        return {mManifestBytes.GetData(), mManifestBytes.GetSize()};
    }

    /// Borrow the page pack for as long as this revision remains alive and unchanged.
    [[nodiscard]] LUDUS_INLINE foundation::BorrowedView<const uint8> GetPagePackBytes() const noexcept
    {
        return {mPagePackBytes.GetData(), mPagePackBytes.GetSize()};
    }

private:
    friend struct TerrainCookedRevisionAccess;

    foundation::Array<uint8> mManifestBytes;
    foundation::Array<uint8> mPagePackBytes;
};

/// @brief A validated manifest: the revision layout and one record per page in canonical order.
///
/// Canonical order lists rows of pages along +Z, and the columns of a row along +X. Page index
/// row * PagesX + column addresses tile (FirstPage.X + column, FirstPage.Z + row).
class TerrainManifest final
{
public:
    /// Create an empty manifest without allocation.
    TerrainManifest() noexcept = default;
    /// Transfer the layout and records, leaving the source empty.
    TerrainManifest(TerrainManifest&& other) noexcept = default;
    /// Replace this manifest by transferring the layout and records, leaving a distinct source empty.
    TerrainManifest& operator=(TerrainManifest&& other) noexcept = default;

    /// Whether no manifest has been read.
    [[nodiscard]] LUDUS_INLINE bool IsEmpty() const noexcept
    {
        return mRecords.IsEmpty();
    }

    /// The revision layout. Meaningless for an empty manifest.
    [[nodiscard]] LUDUS_INLINE const TerrainRevisionLayout& GetLayout() const noexcept
    {
        return mLayout;
    }

    /// Records in canonical order; empty for an empty manifest.
    [[nodiscard]] LUDUS_INLINE foundation::BorrowedView<const TerrainPageRecord> GetPageRecords() const noexcept
    {
        return {mRecords.GetData(), mRecords.GetSize()};
    }

private:
    friend struct TerrainManifestAccess;

    TerrainRevisionLayout mLayout;
    foundation::Array<TerrainPageRecord> mRecords;
};

/// @brief One decoded page: the core as an immutable tile plus the halo around it.
///
/// The tile's description names the manifest revision, so pages read from one manifest pass CheckTerrainTileSeam
/// with each other even when their content revisions differ.
class TerrainPage final
{
public:
    /// Create an empty page without allocation.
    TerrainPage() noexcept = default;
    /// Transfer the tile, halo and record, leaving the source empty.
    TerrainPage(TerrainPage&& other) noexcept = default;
    /// Replace this page by transferring the tile, halo and record, leaving a distinct source empty.
    TerrainPage& operator=(TerrainPage&& other) noexcept = default;

    /// Whether no page has been read.
    [[nodiscard]] LUDUS_INLINE bool IsEmpty() const noexcept
    {
        return mTile.IsEmpty();
    }

    /// The core samples as a tile; views into it need this page alive and unmoved.
    [[nodiscard]] LUDUS_INLINE const TerrainTile& GetTile() const noexcept
    {
        return mTile;
    }

    /// The manifest record the page was read against.
    [[nodiscard]] LUDUS_INLINE const TerrainPageRecord& GetRecord() const noexcept
    {
        return mRecord;
    }

private:
    friend struct TerrainPageAccess;

    TerrainTile mTile;
    foundation::Array<uint8> mHaloCodes;
    TerrainPageRecord mRecord;
};

/// @brief Inclusive rectangle of global lattice samples.
struct TerrainSampleRegion final
{
    /// Sample with the smallest X and Z.
    TerrainSampleAddress Minimum{};
    /// Sample with the largest X and Z; at least Minimum on both axes.
    TerrainSampleAddress Maximum{};
};

/// @brief What a recook rebuilt and what it reused.
struct TerrainRecookSummary final
{
    /// Pages cooked from the new field at the new revision.
    usize RecookedPages = 0;
    /// Pages copied unchanged from the base revision after validating their digests.
    usize ReusedPages = 0;
};

/// @brief Digest a complete elevation field, for a revision whose source is the field itself.
///
/// The digest is FoundationHash's StableFingerprint128 (XXH3-128) of Columns and Rows as little-endian unsigned
/// 32-bit integers, SpacingXMetres and SpacingZMetres, then every elevation in row-major order, each binary32 value
/// as its little-endian bits. Equal fields give equal digests on every host, so a caller that regenerates a field
/// from the same recipe reproduces a revision's SourceDigest and its manifest bytes.
/// @param grid The field's grid; valid, with at most terrain_format::MAX_DOMAIN_AXIS_CELLS cells per axis.
/// @param elevationsMetres One elevation per grid sample, row-major. Values are hashed as stored, nonfinite included.
/// @param outDigest Written only on Success.
/// @return Success, InvalidArgument for an invalid grid, InvalidStorage for a view of the wrong size, LimitExceeded
/// for a grid larger than a revision domain, or AllocationFailed.
[[nodiscard]] TerrainCookStatus TryComputeTerrainFieldDigest(const TerrainFieldGrid& grid,
                                                             foundation::BorrowedView<const float32> elevationsMetres,
                                                             foundation::Fingerprint128& outDigest) noexcept;

/// @brief Quantize one page of a complete domain field into a page blob and its record.
///
/// The payload holds the page's core and its terrain_format::PAGE_HALO_SAMPLES halo, read from the same field, so
/// every page of one field agrees on shared samples. Halo samples outside the domain repeat the nearest domain
/// sample. Pages of one layout can be cooked in any order or concurrently from the same immutable field; the
/// result depends only on the inputs. The page's ContentRevision is layout.Revision.
/// @param layout Revision layout; validated.
/// @param grid The domain field; PagesX * CellsPerPage + 1 by PagesZ * CellsPerPage + 1 samples at SpacingMetres.
/// @param elevationsMetres One finite elevation per field sample, row-major, inside the codec's range.
/// @param page Tile address of a page inside the domain.
/// @param outPage Replaced only on Success.
[[nodiscard]] TerrainCookStatus TryCookTerrainPage(const TerrainRevisionLayout& layout,
                                                   const TerrainFieldGrid& grid,
                                                   foundation::BorrowedView<const float32> elevationsMetres,
                                                   TerrainTileAddress page,
                                                   TerrainCookedPage& outPage) noexcept;

/// @brief Validate cooked pages and write the manifest and page pack in canonical order.
///
/// Every domain page must appear exactly once, in any order. Each blob's header must match the layout and its
/// payload digest its record, and its ContentRevision may not exceed layout.Revision. Every shared core sample and
/// every halo sample must equal the code of the page that owns the sample; a mismatch is BorderMismatch. The bytes
/// do not depend on the order of pages.
/// @param layout Revision layout; validated.
/// @param pages One cooked page per domain page.
/// @param outRevision Replaced only on Success.
[[nodiscard]] TerrainCookStatus TryAssembleTerrainRevision(const TerrainRevisionLayout& layout,
                                                           foundation::BorrowedView<const TerrainCookedPage> pages,
                                                           TerrainCookedRevision& outRevision) noexcept;

/// @brief Cook every page of a domain field serially and assemble the revision.
/// @param layout Revision layout; validated.
/// @param grid The domain field, as for TryCookTerrainPage.
/// @param elevationsMetres One finite elevation per field sample.
/// @param outRevision Replaced only on Success.
[[nodiscard]] TerrainCookStatus TryCookTerrainRevision(const TerrainRevisionLayout& layout,
                                                       const TerrainFieldGrid& grid,
                                                       foundation::BorrowedView<const float32> elevationsMetres,
                                                       TerrainCookedRevision& outRevision) noexcept;

/// @brief Validate a manifest: exact size, magic, version, tags, layout, canonical records, contiguous byte ranges
/// and the manifest digest.
/// @param manifestBytes Borrowed only for the call; at most terrain_format::MAX_MANIFEST_BYTES.
/// @param outManifest Replaced only on Success.
[[nodiscard]] TerrainCookStatus TryReadTerrainManifest(foundation::BorrowedView<const uint8> manifestBytes,
                                                       TerrainManifest& outManifest) noexcept;

/// @brief Decode one page from its byte range alone, checked against its manifest record.
///
/// No other page is read. The blob must have the record's exact size, a header that matches the manifest layout
/// and record, the record's payload digest and core code bounds.
/// @param manifest A manifest read by TryReadTerrainManifest.
/// @param pageIndex Canonical index of the page.
/// @param pageBytes Exactly the record's byte range of the page pack; borrowed only for the call.
/// @param outPage Replaced only on Success.
[[nodiscard]] TerrainCookStatus TryReadTerrainPage(const TerrainManifest& manifest,
                                                   usize pageIndex,
                                                   foundation::BorrowedView<const uint8> pageBytes,
                                                   TerrainPage& outPage) noexcept;

/// @brief Decode a page sample in metres, including its halo.
/// @param page A page read by TryReadTerrainPage.
/// @param column Page-local column, from -PAGE_HALO_SAMPLES through CellsPerPage + PAGE_HALO_SAMPLES.
/// @param row Page-local row, over the same range.
/// @param outHeightMetres Written only on Success.
/// @return Success, InvalidStorage for an empty page or InvalidArgument outside the extended window.
[[nodiscard]] TerrainCookStatus
TryGetTerrainPageHeight(const TerrainPage& page, int32 column, int32 row, float64& outHeightMetres) noexcept;

/// @brief List the pages whose core or halo contains a sample of an edited region, in canonical order.
///
/// These are the pages a height edit invalidates. Derived regional products such as drainage can depend on the
/// whole domain and are not covered. Parts of the region outside the domain are ignored.
/// @param layout Revision layout; validated.
/// @param region Edited samples in global lattice coordinates.
/// @param outPageIndices Canonical indices; written only on Success.
/// @param outCount Pages written; written only on Success, zero when the region misses every page.
/// @return Success, LimitExceeded when outPageIndices is too small, or a layout failure.
[[nodiscard]] TerrainCookStatus TryCollectTerrainEditPages(const TerrainRevisionLayout& layout,
                                                           TerrainSampleRegion region,
                                                           foundation::BorrowedView<uint32> outPageIndices,
                                                           usize& outCount) noexcept;

/// @brief Copy one validated page blob and its record out of an assembled revision.
/// @param manifest A manifest read by TryReadTerrainManifest.
/// @param pagePackBytes The complete page pack the manifest indexes; its size must equal the manifest's total.
/// @param pageIndex Canonical index of the page.
/// @param outPage Replaced only on Success.
[[nodiscard]] TerrainCookStatus TryExtractTerrainCookedPage(const TerrainManifest& manifest,
                                                            foundation::BorrowedView<const uint8> pagePackBytes,
                                                            usize pageIndex,
                                                            TerrainCookedPage& outPage) noexcept;

/// @brief Cook a later revision that rebuilds only the pages an edit reaches.
///
/// Pages listed by TryCollectTerrainEditPages for the region are cooked from the new field at layout.Revision.
/// Every other page is copied from the base revision after its digest is checked, keeping its content revision.
/// The caller declares the edit: every sample whose elevation differs from the base revision's source must lie in
/// the region. The assembled revision then decodes to the same heights as a full cook of the new field, and the
/// border check rejects reused pages that disagree with recooked neighbours.
/// @param baseManifest Manifest of the base revision.
/// @param basePagePackBytes The base revision's complete page pack.
/// @param layout The base layout with a later Revision; RecipeVersion and SourceDigest may change.
/// @param grid The complete new domain field.
/// @param elevationsMetres One finite elevation per new field sample.
/// @param region Edited samples in global lattice coordinates.
/// @param outRevision Replaced only on Success.
/// @param outSummary Written only on Success.
[[nodiscard]] TerrainCookStatus TryRecookTerrainRevision(const TerrainManifest& baseManifest,
                                                         foundation::BorrowedView<const uint8> basePagePackBytes,
                                                         const TerrainRevisionLayout& layout,
                                                         const TerrainFieldGrid& grid,
                                                         foundation::BorrowedView<const float32> elevationsMetres,
                                                         TerrainSampleRegion region,
                                                         TerrainCookedRevision& outRevision,
                                                         TerrainRecookSummary& outSummary) noexcept;
} // namespace ludus::content::terrain
