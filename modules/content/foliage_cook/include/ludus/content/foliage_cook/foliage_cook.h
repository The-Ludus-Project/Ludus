#pragma once

#include <ludus/foundation/base/compiler.h>
#include <ludus/foundation/base/types.h>

#include <ludus/foundation/containers/array.hpp>
#include <ludus/foundation/containers/borrowed_view.hpp>
#include <ludus/foundation/containers/static_array.hpp>
#include <ludus/foundation/math/addressed_random.hpp>

#include <ludus/content/foliage_format/limits.h>
#include <ludus/content/foliage_format/lufol.h>
#include <ludus/content/foliage_pages/foliage_pages.h>
#include <ludus/content/terrain/terrain_tile.h>

/// @file
/// Bounded terrain foliage cook (Ludus::FoliageCook): addressed placement over a terrain-local lattice.
///
/// The cook turns one finite terrain snapshot, an authored layer and species recipe and a list of
/// suppressions into canonical LUFOL placement records (ludus/content/foliage_format/lufol.h). It
/// extends the room-scoped scatter of ProceduralEnvironment to signed 32-bit cells, ordered strata,
/// exclusion classes and terrain suitability. Every horizontal quantity is an integer lattice unit
/// and every vertical quantity is fixed point, so native and browser builds cook identical bytes.
///
/// A candidate is decided only by its identity and the inputs inside its finite dependency halo,
/// so cooking pages alone, together, in any order or on several threads gives the same records.
/// The contract, including the fixed-point rounding rules, is in docs/architecture/foliage.md,
/// "Terrain cook".
namespace ludus::content::foliage_cook
{
using foundation::int32;
using foundation::int64;
using foundation::uint16;
using foundation::uint32;
using foundation::uint64;
using foundation::uint8;
using foundation::usize;

/// Generator version written to cooked page sets; changes whenever cook output changes for unchanged inputs.
inline constexpr uint32 FOLIAGE_COOK_GENERATOR_VERSION = 1;
/// Version of the random address packing, domains and dimensions below.
inline constexpr uint32 FOLIAGE_RANDOM_ADDRESS_LAYOUT_VERSION = 1;

/// Engine-owned random domain of slot offsets, density admission and thinning priority ('FOLP').
inline constexpr uint32 FOLIAGE_PLACEMENT_RANDOM_DOMAIN = 0x464f4c50U;
/// Engine-owned random domain of the species choice and scale ('FOLS').
inline constexpr uint32 FOLIAGE_SPECIES_RANDOM_DOMAIN = 0x464f4c53U;
/// Engine-owned random domain of the variant choice ('FOLV').
inline constexpr uint32 FOLIAGE_VARIANT_RANDOM_DOMAIN = 0x464f4c56U;
/// Engine-owned random domain of the yaw ('FOLT').
inline constexpr uint32 FOLIAGE_TRANSFORM_RANDOM_DOMAIN = 0x464f4c54U;

/// Placement domain dimension: X offset within the cell, bounded by the cell size.
inline constexpr uint16 FOLIAGE_OFFSET_X_DIMENSION = 0;
/// Placement domain dimension: Z offset within the cell, bounded by the cell size.
inline constexpr uint16 FOLIAGE_OFFSET_Z_DIMENSION = 1;
/// Placement domain dimension: density admission word, compared with the suitability threshold.
inline constexpr uint16 FOLIAGE_ADMISSION_DIMENSION = 2;
/// Placement domain dimension: thinning priority word.
inline constexpr uint16 FOLIAGE_PRIORITY_DIMENSION = 3;
/// Species domain dimension: weighted palette entry, bounded by the palette weight total.
inline constexpr uint16 FOLIAGE_SPECIES_CHOICE_DIMENSION = 0;
/// Species domain dimension: scale offset above the species minimum.
inline constexpr uint16 FOLIAGE_SCALE_DIMENSION = 1;
/// Variant domain dimension: variant index, bounded by the species variant count.
inline constexpr uint16 FOLIAGE_VARIANT_CHOICE_DIMENSION = 0;
/// Transform domain dimension: yaw, the high 16 bits of the raw word.
inline constexpr uint16 FOLIAGE_YAW_DIMENSION = 0;

/// Field and mask units: an unsigned 16-bit value v means v / FOLIAGE_FIELD_UNITS_ONE in [0, 1].
inline constexpr uint32 FOLIAGE_FIELD_UNITS_ONE = 65535;
/// Suitability multiplier units: FOLIAGE_SUITABILITY_ONE is a multiplier of one.
inline constexpr uint32 FOLIAGE_SUITABILITY_ONE = uint32{1} << 16;
/// Slope units per unit of rise over run (the tangent of the slope angle).
inline constexpr uint32 FOLIAGE_SLOPE_UNITS_PER_ONE = 4096;
/// Largest slope the cook distinguishes, rise over run 64; steeper surfaces report this value.
inline constexpr uint32 FOLIAGE_MAXIMUM_SLOPE_UNITS = uint32{64} * FOLIAGE_SLOPE_UNITS_PER_ONE;
/// Density units per expected placement per square metre of the XZ projection.
inline constexpr uint32 FOLIAGE_DENSITY_UNITS_PER_PLACEMENT = uint32{1} << 16;
/// Fraction bits of the internal sample heights: root height units times 2^16.
inline constexpr uint32 FOLIAGE_HEIGHT_FRACTION_BITS = 16;
/// Largest absolute root height in LUFOL root height units (about 524 km).
inline constexpr int64 FOLIAGE_MAXIMUM_ABSOLUTE_HEIGHT = int64{1} << 29;

/// Most layers in one cook; the page set limit.
inline constexpr uint32 MAX_FOLIAGE_COOK_LAYERS = foliage_format::MAX_LAYERS;
/// Most weighted species in one layer's palette.
inline constexpr uint32 MAX_FOLIAGE_PALETTE_ENTRIES = 16;
/// Most mask rules in one layer.
inline constexpr uint32 MAX_FOLIAGE_LAYER_MASKS = 4;
/// Most mask channels the terrain input supplies.
inline constexpr uint32 MAX_FOLIAGE_MASK_CHANNELS = 16;
/// Exclusion classes, for example trunk clearance and canopy occupancy.
inline constexpr uint32 FOLIAGE_EXCLUSION_CLASS_COUNT = 4;
/// Largest exclusion or bounds radius in lattice units at scale one (256 terrain samples).
inline constexpr uint32 MAX_FOLIAGE_RADIUS = uint32{1} << 16;
/// Largest bounds height in root height units at scale one (1,024 m).
inline constexpr uint32 MAX_FOLIAGE_BOUNDS_HEIGHT = uint32{1} << 20;
/// Most tiles in one terrain snapshot.
inline constexpr uint32 MAX_FOLIAGE_TERRAIN_TILES = 4096;
/// Most suppressions in one cook.
inline constexpr uint32 MAX_FOLIAGE_SUPPRESSIONS = uint32{1} << 16;
/// Most candidates one page cook evaluates, including its halo; bounds temporary cook memory.
inline constexpr uint32 MAX_FOLIAGE_COOK_CANDIDATES = uint32{1} << 20;
/// Most pages one call cooks or reports.
inline constexpr uint32 MAX_FOLIAGE_COOK_PAGES = foliage_format::MAX_PAGES;

static_assert(MAX_FOLIAGE_PALETTE_ENTRIES <= foliage_format::UINT16_VALUE_COUNT, "palette indices fit 16 bits");

/// @brief Persistent identity of one candidate within a terrain domain.
///
/// The full tuple is authoritative for equality and ordering; the domain is the page set's.
struct FoliageCandidateKey final
{
    uint16 Layer = 0; ///< Layer ID.
    uint16 Slot = 0;  ///< Slot ordinal within the cell.
    int32 CellX = 0;  ///< Cell column in the layer's lattice.
    int32 CellZ = 0;  ///< Cell row in the layer's lattice.

    /// Whether every field matches.
    [[nodiscard]] constexpr bool operator==(const FoliageCandidateKey&) const noexcept = default;
};

/// @brief Whether `left` orders before `right` in identity order: layer, cell Z, cell X, then slot, signed.
[[nodiscard]] constexpr bool IsFoliageKeyBefore(const FoliageCandidateKey& left,
                                                const FoliageCandidateKey& right) noexcept
{
    if (left.Layer != right.Layer)
    {
        return left.Layer < right.Layer;
    }
    if (left.CellZ != right.CellZ)
    {
        return left.CellZ < right.CellZ;
    }
    if (left.CellX != right.CellX)
    {
        return left.CellX < right.CellX;
    }
    return left.Slot < right.Slot;
}

/// Bit position of cell Z in the address scope; cell X occupies the low 32 bits.
inline constexpr uint32 FOLIAGE_SCOPE_CELL_Z_SHIFT = 32;
/// Bit position of the layer in the address event; the slot occupies the low 16 bits.
inline constexpr uint32 FOLIAGE_EVENT_LAYER_SHIFT = 16;
/// Mask of the low 16 bits of an address event.
inline constexpr uint32 FOLIAGE_EVENT_SLOT_MASK = 0xffffU;

/// @brief Pack a candidate and a dimension into a Philox address (layout version 1).
///
/// The scope holds the two's-complement bits of cell Z (high word) and cell X (low word); the event
/// holds the layer (high half) and slot (low half). Every key maps to a distinct (scope, event) pair,
/// so the packing is injective over the full int32 cells and 16-bit layers and slots, and nothing is
/// truncated or hashed. The room-scoped scatter's 16-bit cell packing is a separate layout.
/// @param key Candidate identity.
/// @param dimension Stable sample meaning within the candidate's domain.
[[nodiscard]] constexpr foundation::math::RandomAddress PackFoliageRandomAddress(const FoliageCandidateKey& key,
                                                                                 uint16 dimension) noexcept
{
    foundation::math::RandomAddress address;
    address.Scope = (static_cast<uint64>(static_cast<uint32>(key.CellZ)) << FOLIAGE_SCOPE_CELL_Z_SHIFT) |
                    static_cast<uint64>(static_cast<uint32>(key.CellX));
    address.Event = (static_cast<uint32>(key.Layer) << FOLIAGE_EVENT_LAYER_SHIFT) | static_cast<uint32>(key.Slot);
    address.Dimension = dimension;
    return address;
}

/// @brief Inverse of PackFoliageRandomAddress for every address; the dimension is dropped.
[[nodiscard]] constexpr FoliageCandidateKey UnpackFoliageRandomAddress(foundation::math::RandomAddress address) noexcept
{
    FoliageCandidateKey key;
    key.CellZ = static_cast<int32>(static_cast<uint32>(address.Scope >> FOLIAGE_SCOPE_CELL_Z_SHIFT));
    key.CellX = static_cast<int32>(static_cast<uint32>(address.Scope));
    key.Layer = static_cast<uint16>(address.Event >> FOLIAGE_EVENT_LAYER_SHIFT);
    key.Slot = static_cast<uint16>(address.Event & FOLIAGE_EVENT_SLOT_MASK);
    return key;
}

/// @brief Rectangular grid of tiles from one terrain revision.
///
/// Tiles are row-major: tile i has address (MinimumTile.X + i % TileColumns, MinimumTile.Z + i / TileColumns).
/// Every tile must be nonempty and share the revision, cell count, spacing and height codec, and shared
/// edges must carry equal codes. The caller keeps the tiles alive and unchanged during every call.
struct FoliageTerrainGrid final
{
    foundation::BorrowedView<const terrain::TerrainTile> Tiles; ///< TileColumns * TileRows tiles.
    terrain::TerrainTileAddress MinimumTile{};                  ///< Address of the first tile.
    uint32 TileColumns = 0;                                     ///< Tiles along X, at least one.
    uint32 TileRows = 0;                                        ///< Tiles along Z, at least one.
};

/// @brief One authored mask: a value in [0, 1] per terrain sample.
struct FoliageMaskChannel final
{
    uint16 Id = 0; ///< Nonzero persistent mask ID, strictly increasing across the channels.
    /// FOLIAGE_FIELD_UNITS_ONE fixed point per grid sample, row-major from the grid's minimum sample.
    foundation::BorrowedView<const uint16> Values;
};

/// @brief Canonical terrain surface and the optional channels that modulate placement.
///
/// The grid's samples are the (TileColumns * CellsPerSide + 1) by (TileRows * CellsPerSide + 1) tile
/// samples including the shared edges, row-major from the minimum sample. An empty view means the
/// channel is absent; a rule that requires an absent channel fails validation and names it.
struct FoliageTerrainInput final
{
    FoliageTerrainGrid Grid{};                   ///< Canonical quantized surface.
    foundation::BorrowedView<const uint8> Holes; ///< One byte per grid cell, nonzero for a hole; empty for none.
    /// Wetness per grid sample in FOLIAGE_FIELD_UNITS_ONE fixed point, for example quantized from
    /// ContentTerrain's TryComputeTerrainWetness; empty when absent.
    foundation::BorrowedView<const uint16> Wetness;
    foundation::BorrowedView<const FoliageMaskChannel> Masks; ///< Mask channels, ID ascending.
};

/// @brief A suitability range with linear feathers outside it.
///
/// Inside [Minimum, Maximum] the multiplier is one. At distance d outside, it is
/// floor((Feather - d) * FOLIAGE_SUITABILITY_ONE / Feather) while d < Feather, and zero beyond; a zero
/// feather is a hard edge. A disabled range is neutral.
struct FoliageRange final
{
    bool IsEnabled = false; ///< Whether the range applies.
    int64 Minimum = 0;      ///< Lowest value with multiplier one, in the quantity's units.
    int64 Maximum = 0;      ///< Highest value with multiplier one; at least Minimum.
    uint32 Feather = 0;     ///< Ramp width beyond each end, in the quantity's units.
};

/// @brief A mask that multiplies a layer's suitability by its interpolated value.
struct FoliageMaskRule final
{
    uint16 MaskId = 0;       ///< Mask channel ID.
    bool IsRequired = false; ///< Whether an absent channel fails validation; otherwise it is neutral.
};

/// @brief One weighted species of a layer palette.
struct FoliagePaletteEntry final
{
    uint16 Species = 0; ///< Index into FoliageCookInput::Species.
    uint32 Weight = 0;  ///< Nonzero relative weight; the palette total must fit 32 bits.
};

/// @brief One placement layer: a lattice, a density and suitability rules over a weighted palette.
///
/// Layers in the same stratum thin each other by priority. A layer in a later stratum also loses
/// every candidate that conflicts with an accepted placement of an earlier stratum, so trees and
/// obstacles claim space before understory.
struct FoliageCookLayer final
{
    uint16 Id = 0;           ///< Persistent layer ID, strictly increasing across the layers.
    uint8 Stratum = 0;       ///< Evaluation order; lower strata are decided first.
    uint16 SlotsPerCell = 1; ///< Fixed candidate slots per cell, 1..MAX_SLOTS_PER_CELL.
    uint32 CellSize = 1;     ///< Cell edge in lattice units, 1..MAX_CELL_SIZE; divides the page size.
    uint64 Seed = 0;         ///< Root seed of the layer's random keys.
    /// Expected placements per square metre at suitability one, in FOLIAGE_DENSITY_UNITS_PER_PLACEMENT;
    /// at most SlotsPerCell per cell.
    uint32 Density = 0;
    foundation::BorrowedView<const FoliagePaletteEntry> Palette; ///< 1..MAX_FOLIAGE_PALETTE_ENTRIES species.
    FoliageRange Height;                                         ///< Root height range in root height units.
    FoliageRange Slope;                                          ///< Slope range in FOLIAGE_SLOPE_UNITS_PER_ONE.
    FoliageRange Wetness;                                        ///< Wetness range in FOLIAGE_FIELD_UNITS_ONE.
    bool IsWetnessRequired = false; ///< Whether an enabled wetness range fails validation without wetness.
    foundation::BorrowedView<const FoliageMaskRule> Masks; ///< Up to MAX_FOLIAGE_LAYER_MASKS rules, ID ascending.
};

/// @brief Placement-affecting properties of one species asset.
///
/// Radii are lattice units at scale one; a candidate's radius is ceil(radius * scale / SCALE_UNITS_PER_ONE).
/// Two candidates conflict when, for some class both occupy (nonzero radius), the squared distance
/// between their roots is below the square of the sum of their radii in that class.
struct FoliageCookSpecies final
{
    uint64 Key = 0;               ///< Nonzero persistent species key, strictly increasing across the species.
    uint64 PlacementRevision = 0; ///< Revision of these placement-affecting properties.
    /// Revision of meshes, materials and other render-only properties. Never read by the cook, so a
    /// render-only edit keeps every placement and page set byte.
    uint64 RenderRevision = 0;
    uint32 VariantCount = 1;                                   ///< Mesh/material variants, 1..MAX_VARIANTS_PER_SPECIES.
    uint16 MinimumScale = foliage_format::SCALE_UNITS_PER_ONE; ///< Smallest scale, nonzero.
    uint16 MaximumScale = foliage_format::SCALE_UNITS_PER_ONE; ///< Largest scale, at least MinimumScale.
    bool IsAlignedToTerrain = false; ///< Whether the up direction follows the terrain normal; otherwise vertical.
    /// Exclusion radius per class, at most MAX_FOLIAGE_RADIUS; zero does not occupy the class.
    foundation::StaticArray<uint32, FOLIAGE_EXCLUSION_CLASS_COUNT> ExclusionRadii{};
    uint32 BoundsRadius = 0; ///< Horizontal radius of a sphere enclosing every LOD and deformation, lattice units.
    uint32 BoundsHeight = 0; ///< The same radius in root height units, at most MAX_FOLIAGE_BOUNDS_HEIGHT.
};

/// @brief Persistent lattice domain of one terrain.
struct FoliageCookDomain final
{
    uint64 Id = 0;       ///< Nonzero persistent terrain domain ID; a lattice change starts a new domain.
    int64 OriginX = 0;   ///< Terrain sample column of the lattice origin.
    int64 OriginZ = 0;   ///< Terrain sample row of the lattice origin.
    uint32 PageSize = 0; ///< Page edge in lattice units, 1..MAX_PAGE_SIZE.
};

/// @brief Borrowed cook input. The caller keeps every view alive and unchanged during each call.
struct FoliageCookInput final
{
    FoliageCookDomain Domain{};                                 ///< Lattice domain.
    FoliageTerrainInput Terrain{};                              ///< Terrain snapshot.
    foundation::BorrowedView<const FoliageCookLayer> Layers;    ///< 1..MAX_FOLIAGE_COOK_LAYERS, ID ascending.
    foundation::BorrowedView<const FoliageCookSpecies> Species; ///< 1..MAX_SPECIES, key ascending.
    /// Suppressed candidates in identity order. A key whose candidate is currently absent stays dormant
    /// and applies again when the candidate returns.
    foundation::BorrowedView<const FoliageCandidateKey> Suppressions;
};

/// Result of a cook operation.
enum class FoliageCookStatus : uint8
{
    Ok,                   ///< The operation succeeded.
    InvalidArgument,      ///< A view, request or output argument violates the API contract.
    InvalidInput,         ///< The recipe, species or terrain input violates a rule; Issue names it.
    MissingField,         ///< A rule requires an absent channel; Issue and FieldId name it.
    CoordinateOutOfRange, ///< A lattice, cell, page or height coordinate exceeds its checked range.
    LimitExceeded,        ///< A count exceeds a cook or format limit.
    SamplingExhausted,    ///< A bounded draw exhausted its 65,536 attempts; Candidate names it.
    AllocationFailed,     ///< Cook storage could not be allocated.
    EncodeFailed,         ///< The page set encoder rejected the records; Encode holds its diagnostic.
};

/// The rule a rejected input violates.
enum class FoliageCookIssue : uint8
{
    None,               ///< No problem.
    Domain,             ///< The domain ID is zero.
    PageSize,           ///< The page size is zero or exceeds MAX_PAGE_SIZE.
    TerrainGrid,        ///< The tile grid is empty, too large or its view does not hold every tile.
    TerrainTile,        ///< A tile is empty or its metadata differs from the first tile; Index names it.
    TerrainSeam,        ///< A tile's codes differ from its neighbour's on a shared edge; Index names it.
    TerrainSpacing,     ///< The sample spacing is outside the fixed-point range.
    TerrainHeight,      ///< A sample height is outside the fixed-point range; Index names the tile.
    TerrainCoordinates, ///< Terrain lattice coordinates, cells or pages do not fit their checked range.
    Holes,              ///< The hole view does not hold one byte per grid cell.
    Wetness,            ///< The wetness view does not hold one value per sample, or a value exceeds one.
    MaskChannels,       ///< A mask channel is unordered, has ID zero, a wrong size or a value above one.
    SpeciesCount,       ///< No species, or too many.
    SpeciesKey,         ///< A species key is zero or not increasing; Index names it.
    SpeciesVariants,    ///< A species' variant count is zero or too large; Index names it.
    SpeciesScale,       ///< A species' scale range is empty or starts at zero; Index names it.
    SpeciesRadius,      ///< A species' exclusion or bounds radius exceeds its limit; Index names it.
    LayerCount,         ///< No layers, or too many.
    LayerOrder,         ///< A layer ID is not increasing; Index names the later layer.
    LayerSlots,         ///< A layer's slot count is zero or too large; Index names it.
    LayerCellSize,      ///< A layer's cell size is zero, too large or does not divide the page size.
    LayerPalette,       ///< A layer's palette is empty, too large, names an absent species or has bad weights.
    LayerDensity,       ///< A layer's density exceeds one placement per slot; Index names it.
    LayerRange,         ///< A layer's height, slope or wetness range is inverted or out of range.
    LayerMasks,         ///< A layer has too many mask rules or they are not ID ascending.
    MissingWetness,     ///< A layer requires wetness that the terrain input lacks; Index names the layer.
    MissingMask,        ///< A layer requires a mask the terrain input lacks; Index names the layer, FieldId the mask.
    SuppressionCount,   ///< Too many suppressions.
    SuppressionOrder,   ///< Suppressions are not strictly increasing; Index names the later one.
    PageRequest,        ///< A requested page is repeated, or the request is empty or too large.
    CandidateLimit,     ///< A page and its halo need more than MAX_FOLIAGE_COOK_CANDIDATES candidates.
    Sampling,           ///< A bounded draw exhausted its attempts.
    Allocation,         ///< Storage could not be allocated.
    Encode,             ///< The page set encoder rejected the records.
};

/// Failure detail without owned text.
struct FoliageCookDiagnostic final
{
    FoliageCookStatus Status = FoliageCookStatus::Ok; ///< Same value as the returned status.
    FoliageCookIssue Issue = FoliageCookIssue::None;  ///< Violated rule; None on success.
    usize Index = 0;                                  ///< Offending tile, species, layer or suppression.
    uint16 FieldId = 0;                               ///< Missing mask ID for MissingMask; otherwise zero.
    FoliageCandidateKey Candidate{};                  ///< Candidate for Sampling; otherwise zero.
    foliage_pages::FoliagePageDiagnostic Encode{};    ///< Encoder detail for EncodeFailed.
};

/// @brief One page address.
struct FoliagePageAddress final
{
    int32 X = 0; ///< Page column.
    int32 Z = 0; ///< Page row.

    /// Whether both fields match.
    [[nodiscard]] constexpr bool operator==(const FoliagePageAddress&) const noexcept = default;
};

/// @brief Half-open lattice rectangle relative to the domain origin: Minimum <= p < Maximum on each axis.
struct FoliageLatticeRectangle final
{
    int64 MinimumX = 0; ///< Inclusive lower X.
    int64 MinimumZ = 0; ///< Inclusive lower Z.
    int64 MaximumX = 0; ///< Exclusive upper X.
    int64 MaximumZ = 0; ///< Exclusive upper Z.
};

/// @brief Cooked pages and their placements, in any page order.
///
/// Each page's FirstPlacement indexes GetPlacements(). Pages cooked separately, on any thread, merge
/// into one set with TryMergeFoliageCookedPages; TryWriteFoliageCookedPageSet orders them canonically.
/// Move-only owner; not thread-safe.
class FoliageCookedPages final
{
public:
    /// Constructs an empty set without allocating.
    FoliageCookedPages() noexcept = default;
    /// Transfers the pages, leaving the source empty.
    FoliageCookedPages(FoliageCookedPages&& other) noexcept = default;
    /// Replaces the pages by transfer, leaving the source empty.
    FoliageCookedPages& operator=(FoliageCookedPages&& other) noexcept = default;

    /// Cooked page directory entries, in the order they were cooked or merged.
    [[nodiscard]] LUDUS_INLINE foundation::BorrowedView<const foliage_pages::FoliagePage> GetPages() const noexcept
    {
        return {mPages.GetData(), mPages.GetSize()};
    }
    /// Placements of every page; page i owns [FirstPlacement, FirstPlacement + PlacementCount).
    [[nodiscard]] LUDUS_INLINE foundation::BorrowedView<const foliage_pages::FoliagePlacement>
    GetPlacements() const noexcept
    {
        return {mPlacements.GetData(), mPlacements.GetSize()};
    }
    /// Owned candidates that an authored suppression removed in these pages; dormant suppressions are not counted.
    [[nodiscard]] usize GetActiveSuppressionCount() const noexcept;

private:
    foundation::Array<foliage_pages::FoliagePage> mPages;
    foundation::Array<foliage_pages::FoliagePlacement> mPlacements;
    foundation::Array<uint32> mActiveSuppressions;

    friend FoliageCookStatus TryCookFoliagePages(const FoliageCookInput& input,
                                                 foundation::BorrowedView<const FoliagePageAddress> pages,
                                                 FoliageCookedPages& outPages,
                                                 FoliageCookDiagnostic& outDiagnostic) noexcept;
    friend FoliageCookStatus TryMergeFoliageCookedPages(const FoliageCookedPages& source,
                                                        FoliageCookedPages& inoutDestination,
                                                        FoliageCookDiagnostic& outDiagnostic) noexcept;
};

/// @brief Validate every rule of a cook input before cooking. Pure and allocation-free.
/// @param input The cook input.
/// @param outDiagnostic Receives the status and, on failure, the first violated rule.
/// @return Ok, InvalidArgument, InvalidInput, MissingField, CoordinateOutOfRange or LimitExceeded.
[[nodiscard]] FoliageCookStatus ValidateFoliageCookInput(const FoliageCookInput& input,
                                                         FoliageCookDiagnostic& outDiagnostic) noexcept;

/// @brief Every page whose square overlaps the terrain, in (Z, X) order.
/// @param input A valid cook input.
/// @param outPages Replaced on success; unchanged on failure.
/// @param outDiagnostic Receives the status and, on failure, the violated rule.
/// @return Ok, a validation failure, LimitExceeded or AllocationFailed.
[[nodiscard]] FoliageCookStatus TryGetFoliageTerrainPages(const FoliageCookInput& input,
                                                          foundation::Array<FoliagePageAddress>& outPages,
                                                          FoliageCookDiagnostic& outDiagnostic) noexcept;

/// @brief Cook the owned placements of the requested pages.
///
/// Each page is decided from its candidates' identities and the inputs inside its dependency halo,
/// so the records do not depend on which other pages are requested, their order or the thread.
/// Pages outside the terrain cook empty. Safe to call concurrently with distinct outputs.
/// @param input A valid cook input.
/// @param pages Distinct page addresses, 1..MAX_FOLIAGE_COOK_PAGES, in any order.
/// @param outPages Replaced on success with the requested pages in request order; unchanged on failure.
/// @param outDiagnostic Receives the status and, on failure, the violated rule or candidate.
/// @return Ok, a validation failure, LimitExceeded, SamplingExhausted or AllocationFailed.
[[nodiscard]] FoliageCookStatus TryCookFoliagePages(const FoliageCookInput& input,
                                                    foundation::BorrowedView<const FoliagePageAddress> pages,
                                                    FoliageCookedPages& outPages,
                                                    FoliageCookDiagnostic& outDiagnostic) noexcept;

/// @brief Merge cooked pages: a source page replaces the destination page with the same address, others append.
///
/// Merging disjoint cooks joins them; merging a dirty recook replaces its pages.
/// @param source Pages cooked from the same input as the destination's replacement.
/// @param inoutDestination Receives the merged pages; unchanged on failure.
/// @param outDiagnostic Receives the status.
/// @return Ok, LimitExceeded or AllocationFailed.
[[nodiscard]] FoliageCookStatus TryMergeFoliageCookedPages(const FoliageCookedPages& source,
                                                           FoliageCookedPages& inoutDestination,
                                                           FoliageCookDiagnostic& outDiagnostic) noexcept;

/// @brief SHA-256 of every placement-affecting input: versions, domain, terrain, channels, recipe and suppressions.
///
/// Render-only species properties are excluded, so a material-only edit keeps the digest.
/// @param input A valid cook input.
/// @param outDigest Receives the digest; unchanged on failure.
/// @param outDiagnostic Receives the status.
/// @return Ok or a validation failure.
[[nodiscard]] FoliageCookStatus TryComputeFoliageInputDigest(const FoliageCookInput& input,
                                                             foliage_pages::FoliageInputDigest& outDigest,
                                                             FoliageCookDiagnostic& outDiagnostic) noexcept;

/// @brief Encode cooked pages as one canonical LUFOL page set.
///
/// Pages are written in (Z, X) order with every layer and species of the input. The header carries
/// FOLIAGE_COOK_GENERATOR_VERSION, the domain, the terrain revision and TryComputeFoliageInputDigest.
/// @param input The valid input the pages were cooked from.
/// @param pages Cooked pages with distinct addresses.
/// @param outBytes Replaced on success with the encoded page set; unchanged on failure.
/// @param outDiagnostic Receives the status and, for EncodeFailed, the encoder diagnostic.
/// @return Ok, a validation failure, InvalidArgument for repeated pages, EncodeFailed or AllocationFailed.
[[nodiscard]] FoliageCookStatus TryWriteFoliageCookedPageSet(const FoliageCookInput& input,
                                                             const FoliageCookedPages& pages,
                                                             foundation::Array<uint8>& outBytes,
                                                             FoliageCookDiagnostic& outDiagnostic) noexcept;

/// @brief Roots whose placement inputs read any sample in an edited inclusive sample rectangle.
///
/// Sampling reads the corners of the root's terrain cell, so the result is every root in a cell that
/// touches an edited sample. Use it for height, wetness and mask edits; a hole edit of cells
/// [first, last] passes the same rectangle.
/// @param input A valid cook input.
/// @param firstSample Lowest edited global sample (column, row).
/// @param lastSample Highest edited global sample, inclusive; at least firstSample on each axis.
/// @param outRoots Receives the half-open lattice rectangle; unchanged on failure.
/// @param outDiagnostic Receives the status.
/// @return Ok, a validation failure, InvalidArgument or CoordinateOutOfRange.
[[nodiscard]] FoliageCookStatus TryGetFoliageSampleEditRoots(const FoliageCookInput& input,
                                                             terrain::TerrainSampleAddress firstSample,
                                                             terrain::TerrainSampleAddress lastSample,
                                                             FoliageLatticeRectangle& outRoots,
                                                             FoliageCookDiagnostic& outDiagnostic) noexcept;

/// @brief Roots a candidate can occupy: its cell's lattice square. Use it for a suppression edit.
/// @param input A valid cook input.
/// @param key Candidate identity; its layer must exist.
/// @param outRoots Receives the half-open lattice rectangle; unchanged on failure.
/// @param outDiagnostic Receives the status.
/// @return Ok, a validation failure, InvalidArgument or CoordinateOutOfRange.
[[nodiscard]] FoliageCookStatus TryGetFoliageCandidateRoots(const FoliageCookInput& input,
                                                            const FoliageCandidateKey& key,
                                                            FoliageLatticeRectangle& outRoots,
                                                            FoliageCookDiagnostic& outDiagnostic) noexcept;

/// @brief Pages whose records may change when the inputs of roots in a rectangle change.
///
/// Expands the rectangle by the same dependency reach the cook halo uses: same-stratum thinning and
/// occupancy of every later stratum. Pages outside the result keep their records, so recooking the
/// result and merging it over a previous cook equals a clean cook. Layer, species and domain edits
/// invalidate every page instead.
/// @param input The valid input after the edit; its rules must match the input before it.
/// @param changedRoots Nonempty half-open lattice rectangle of roots whose inputs changed.
/// @param outPages Replaced on success with the dirty pages in (Z, X) order; unchanged on failure.
/// @param outDiagnostic Receives the status.
/// @return Ok, a validation failure, InvalidArgument, CoordinateOutOfRange, LimitExceeded or AllocationFailed.
[[nodiscard]] FoliageCookStatus TryGetFoliageDirtyPages(const FoliageCookInput& input,
                                                        const FoliageLatticeRectangle& changedRoots,
                                                        foundation::Array<FoliagePageAddress>& outPages,
                                                        FoliageCookDiagnostic& outDiagnostic) noexcept;
} // namespace ludus::content::foliage_cook
