#pragma once

// Room-scoped scatter of static placements: the serial reference kernel of
// procedural content generation slice P0. It samples FoundationMath's addressed
// Philox by persistent placement identity, so a cell's result is independent of
// traversal order, chunking and its neighbours' generation. Contracts:
// docs/architecture/procedural-content-generation.md#p0-room-scoped-scatter and
// docs/architecture/randomness.md#milestone-1-procedural-placement.

#include <ludus/foundation/base/core.h>

#include <ludus/foundation/containers/borrowed_view.hpp>
#include <ludus/foundation/math/addressed_random.hpp>

namespace ludus::content::procedural
{
using foundation::core::int16;
using foundation::core::int32;
using foundation::core::uint16;
using foundation::core::uint32;
using foundation::core::uint64;
using foundation::core::uint8;
using foundation::core::usize;

/// Version of the placement identity packing and random layout below; store it with generated content.
inline constexpr uint32 SCATTER_LAYOUT_VERSION = 1;
/// Most layers in one recipe.
inline constexpr usize MAX_SCATTER_LAYERS = 16;
/// Most keep-out rectangles in one recipe.
inline constexpr usize MAX_SCATTER_KEEP_OUTS = 64;
/// Most variants in one layer.
inline constexpr usize MAX_SCATTER_VARIANTS = 64;
/// Most candidates in one cell; candidate indices are stored in 8 bits.
inline constexpr uint16 MAX_SCATTER_CANDIDATES_PER_CELL = 64;
/// Cells searched in each direction for conflicts; follows from exclusion radius <= cell size.
inline constexpr int32 SCATTER_NEIGHBOR_CELLS = 2;

/// @brief Persistent identity of one placement candidate.
///
/// The tuple is authoritative; PackScatterScope maps it injectively to the 64-bit address scope.
struct ScatterIdentity final
{
    /// Nonzero room ordinal from the game's room catalog, never reused.
    uint16 Room = 0;
    /// Authored layer ID, unique within the room's recipe.
    uint8 Layer = 0;
    /// Candidate index within its cell, below the layer's candidate count.
    uint8 Candidate = 0;
    /// Cell column.
    int16 CellX = 0;
    /// Cell row.
    int16 CellY = 0;

    /// Whether every field matches.
    [[nodiscard]] constexpr bool operator==(const ScatterIdentity&) const noexcept = default;
};

/// @brief Pack an identity into an address scope: room, layer, candidate, cell X, cell Y from the high bits.
[[nodiscard]] constexpr uint64 PackScatterScope(ScatterIdentity identity) noexcept
{
    return (static_cast<uint64>(identity.Room) << 48u) | (static_cast<uint64>(identity.Layer) << 40u) |
           (static_cast<uint64>(identity.Candidate) << 32u) |
           (static_cast<uint64>(static_cast<uint16>(identity.CellX)) << 16u) |
           static_cast<uint64>(static_cast<uint16>(identity.CellY));
}

/// @brief Inverse of PackScatterScope for every 64-bit value.
[[nodiscard]] constexpr ScatterIdentity UnpackScatterScope(uint64 scope) noexcept
{
    ScatterIdentity identity;
    identity.Room = static_cast<uint16>(scope >> 48u);
    identity.Layer = static_cast<uint8>(scope >> 40u);
    identity.Candidate = static_cast<uint8>(scope >> 32u);
    identity.CellX = static_cast<int16>(static_cast<uint16>(scope >> 16u));
    identity.CellY = static_cast<int16>(static_cast<uint16>(scope));
    return identity;
}

/// @brief Half-open integer rectangle in lattice units: Minimum <= p < Maximum on each axis.
struct ScatterRectangle final
{
    /// Inclusive lower X bound.
    int32 MinimumX = 0;
    /// Inclusive lower Y bound.
    int32 MinimumY = 0;
    /// Exclusive upper X bound.
    int32 MaximumX = 0;
    /// Exclusive upper Y bound.
    int32 MaximumY = 0;
};

/// @brief Half-open range of cell coordinates: Minimum <= cell < Maximum on each axis.
struct ScatterCellRange final
{
    /// Inclusive lowest cell column.
    int32 MinimumX = 0;
    /// Inclusive lowest cell row.
    int32 MinimumY = 0;
    /// Exclusive highest cell column.
    int32 MaximumX = 0;
    /// Exclusive highest cell row.
    int32 MaximumY = 0;
};

/// @brief One choice a candidate can take.
struct ScatterVariant final
{
    /// Exclusion radius in lattice units, at most the layer's cell size. Zero never conflicts.
    uint32 ExclusionRadius = 0;
};

/// @brief One independently thinned set of placements.
struct ScatterLayer final
{
    /// Authored layer ID, unique within the recipe; part of every placement identity.
    uint8 Id = 0;
    /// Cell edge length in lattice units, at least one.
    uint16 CellSize = 1;
    /// Candidates per cell, 1 through MAX_SCATTER_CANDIDATES_PER_CELL.
    uint16 CandidatesPerCell = 1;
    /// Equally likely variants, 1 through MAX_SCATTER_VARIANTS. Storage is borrowed from the caller.
    foundation::BorrowedView<const ScatterVariant> Variants;
    /// Smallest scale, in units the game chooses (for example thousandths).
    uint16 MinimumScale = 1000;
    /// Largest scale, at least MinimumScale.
    uint16 MaximumScale = 1000;
};

/// @brief Validated input for one room. The recipe borrows its arrays; the caller keeps them alive.
struct ScatterRecipe final
{
    /// Nonzero room ordinal.
    uint16 Room = 0;
    /// Area that candidate positions must lie in; must be nonempty.
    ScatterRectangle Region;
    /// Areas candidate positions must avoid, at most MAX_SCATTER_KEEP_OUTS; each must be nonempty.
    foundation::BorrowedView<const ScatterRectangle> KeepOuts;
    /// Layers, 1 through MAX_SCATTER_LAYERS.
    foundation::BorrowedView<const ScatterLayer> Layers;
};

/// @brief Domain IDs from the game's random domain manifest.
struct ScatterDomains final
{
    /// Positions and thinning priority.
    uint32 Placement = 0;
    /// Variant choice.
    uint32 Variant = 0;
    /// Rotation and scale.
    uint32 Transform = 0;
};

/// @brief Keys derived from the world seed, one per scatter domain.
struct ScatterKeys final
{
    /// Key for positions and thinning priority.
    foundation::math::RandomKey Placement;
    /// Key for variant choice.
    foundation::math::RandomKey Variant;
    /// Key for rotation and scale.
    foundation::math::RandomKey Transform;
};

/// @brief One accepted placement.
struct ScatterPlacement final
{
    /// Persistent identity.
    ScatterIdentity Identity;
    /// X position in lattice units.
    int32 LatticeX = 0;
    /// Y position in lattice units.
    int32 LatticeY = 0;
    /// Index into the layer's variants.
    uint32 Variant = 0;
    /// Rotation in 1/65536 turns.
    uint16 Rotation = 0;
    /// Scale between the layer's minimum and maximum, inclusive.
    uint16 Scale = 0;
};

/// @brief Result of a scatter operation.
enum class ScatterStatus : uint8
{
    /// The operation succeeded.
    Success,
    /// The recipe, domains or a layer index failed validation.
    InvalidRecipe,
    /// A cell range is empty or lies outside the layer's cells.
    InvalidRange,
    /// The caller's storage cannot hold every candidate of the range.
    CapacityExceeded,
    /// A bounded draw exhausted its 65,536 attempts.
    SamplingExhausted,
};

/// @brief The specific problem validation found.
enum class ScatterRecipeIssue : uint8
{
    /// No problem.
    None,
    /// The room ordinal is zero.
    RoomZero,
    /// The region is empty.
    EmptyRegion,
    /// The keep-out view is invalid or has too many entries.
    KeepOutCount,
    /// A keep-out rectangle is empty; Index names it.
    EmptyKeepOut,
    /// The layer view is invalid, empty or has too many entries.
    LayerCount,
    /// A layer repeats an earlier layer's ID; Index names the later layer.
    DuplicateLayer,
    /// A layer's cell size is zero; Index names it.
    CellSize,
    /// A layer's region cells do not fit int16 coordinates; Index names it.
    CellRange,
    /// A layer's candidate count is zero or too large; Index names it.
    CandidateCount,
    /// A layer's variant view is invalid, empty or has too many entries; Index names it.
    VariantCount,
    /// A variant's exclusion radius exceeds its layer's cell size; Index names the layer.
    ExclusionRadius,
    /// A layer's maximum scale is below its minimum; Index names it.
    ScaleRange,
    /// A domain ID is outside the game range or repeats another scatter domain.
    Domain,
};

/// @brief Validation outcome with the first problem found.
struct ScatterValidation final
{
    /// Success or InvalidRecipe.
    ScatterStatus Status = ScatterStatus::Success;
    /// What was wrong; None on success.
    ScatterRecipeIssue Issue = ScatterRecipeIssue::None;
    /// Index of the offending keep-out or layer, when the issue names one; otherwise zero.
    usize Index = 0;
};

/// @brief Check a recipe before generation. Pure and allocation-free.
[[nodiscard]] ScatterValidation ValidateScatterRecipe(const ScatterRecipe& recipe) noexcept;

/// @brief Derive the three scatter keys from a world seed.
/// @param worldSeed The session's recorded root seed; zero is a valid deterministic seed.
/// @param domains Distinct IDs in the game domain range.
/// @param outKeys Written on success; unchanged on failure.
/// @return Success, or InvalidRecipe when a domain is out of range or repeated.
[[nodiscard]] ScatterStatus TryMakeScatterKeys(uint64 worldSeed, ScatterDomains domains, ScatterKeys& outKeys) noexcept;

/// @brief Every cell of a layer whose area overlaps the region.
/// @param recipe The room's recipe; validated by this call.
/// @param layerIndex Index into recipe.Layers.
/// @param outCells Written on success; unchanged on failure.
/// @return Success, or InvalidRecipe when the recipe is invalid or layerIndex is out of bounds.
[[nodiscard]] ScatterStatus
TryGetScatterCells(const ScatterRecipe& recipe, usize layerIndex, ScatterCellRange& outCells) noexcept;

/// @brief Storage TryGenerateScatter needs for a range: its cell count times the layer's candidate count.
/// @param recipe The room's recipe; validated by this call.
/// @param layerIndex Index into recipe.Layers.
/// @param cells A nonempty range inside TryGetScatterCells.
/// @param outCount Written on success; unchanged on failure.
/// @return Success, InvalidRecipe or InvalidRange.
[[nodiscard]] ScatterStatus TryCountScatterCandidates(const ScatterRecipe& recipe,
                                                      usize layerIndex,
                                                      ScatterCellRange cells,
                                                      usize& outCount) noexcept;

/// @brief Generate the accepted placements of one layer in a range of cells.
///
/// Placements are written in canonical order: cell Y, then cell X, then candidate, ascending. Acceptance depends
/// only on placement identities, never on the range, so generating a layer's cells in several ranges and joining
/// the results in canonical order equals one call over all of them. Neighbouring cells outside the range are
/// evaluated but never written. Pure, allocation-free and safe to call concurrently on distinct storage.
/// @param recipe A recipe that passes ValidateScatterRecipe.
/// @param keys Keys from TryMakeScatterKeys.
/// @param layerIndex Index into recipe.Layers.
/// @param cells A nonempty range inside TryGetScatterCells.
/// @param storage Receives the placements; must hold TryCountScatterCandidates elements.
/// @param outCount Number of placements written, on success; unchanged on failure.
/// @return Success; InvalidRecipe, InvalidRange or CapacityExceeded before anything is written; or
///         SamplingExhausted, after which the storage contents are unspecified.
[[nodiscard]] ScatterStatus TryGenerateScatter(const ScatterRecipe& recipe,
                                               const ScatterKeys& keys,
                                               usize layerIndex,
                                               ScatterCellRange cells,
                                               foundation::BorrowedView<ScatterPlacement> storage,
                                               usize& outCount) noexcept;
} // namespace ludus::content::procedural
