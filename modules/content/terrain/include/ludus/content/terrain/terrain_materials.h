#pragma once

// Surface material rules of terrain slice T1: stable global material IDs chosen
// per sample from elevation, slope and wetness memberships, then gathered into
// bounded per-patch palettes with normalized, quantized channel weights. The
// cooker reports a palette overflow instead of dropping a material. Contracts:
// docs/architecture/terrain.md#surface-materials-and-attached-systems and
// docs/modules/content/terrain.md.
//
// Thanks to Ferenc Pintér, "Large-Scale Terrain Rendering for Outdoor Games",
// GPU Pro 2, chapter II.3, printed pp. 77-93, for combining procedural soil
// maps with artist paint. Ludus keeps material identities and weights separate
// instead of interpolating ordered soil indices; no chapter code is copied.

#include <ludus/foundation/base/core.h>

#include <ludus/content/terrain/terrain_fields.h>
#include <ludus/foundation/containers/borrowed_view.hpp>
#include <ludus/foundation/containers/static_array.hpp>

namespace ludus::content::terrain
{
using foundation::core::uint16;

/// Version of the membership, normalization, palette order and quantization below; store it with cooked data.
inline constexpr uint32 TERRAIN_MATERIALS_LAYOUT_VERSION = 1;
/// Most rules in one rule set.
inline constexpr usize MAX_TERRAIN_MATERIAL_RULES = 16;
/// Render materials one patch can blend; one palette channel each.
inline constexpr usize TERRAIN_PATCH_MATERIAL_CHANNELS = 4;
/// Material ID of an unused palette channel; rules may not use it.
inline constexpr uint16 TERRAIN_MATERIAL_NONE = 0;
/// Quantized channel weights of one sample sum to this value.
inline constexpr uint32 TERRAIN_MATERIAL_WEIGHT_ONE = 255;
/// Most cells along one patch edge.
inline constexpr uint32 MAX_TERRAIN_PATCH_CELLS = 256;
/// Default cells along one patch edge, the terrain systems design's starting patch size.
inline constexpr uint32 DEFAULT_TERRAIN_PATCH_CELLS = 32;
/// Most samples of halo gathered around a patch footprint.
inline constexpr uint32 MAX_TERRAIN_PATCH_HALO_SAMPLES = 4;
/// Range bound that admits every finite value on that side.
inline constexpr float32 TERRAIN_MATERIAL_RANGE_UNBOUNDED = 3.0e38F;

/// @brief Inclusive interval with a feathered membership outside it.
///
/// Membership is 1 inside [Minimum, Maximum] and falls smoothly to 0 over FeatherWidth beyond either bound. A
/// zero width makes a hard edge.
struct TerrainMaterialRange final
{
    /// Lower bound; finite, at most Maximum.
    float32 Minimum = -TERRAIN_MATERIAL_RANGE_UNBOUNDED;
    /// Upper bound; finite, at least Minimum.
    float32 Maximum = TERRAIN_MATERIAL_RANGE_UNBOUNDED;
    /// Fade distance outside the bounds, in the range's units; finite and nonnegative.
    float32 FeatherWidth = 0;
};

/// @brief One material's suitability: the product of its range memberships times its weight.
struct TerrainMaterialRule final
{
    /// Stable global material ID; never TERRAIN_MATERIAL_NONE. Several rules may share one ID.
    uint16 MaterialId = TERRAIN_MATERIAL_NONE;
    /// Relative strength against other rules; finite and positive.
    float32 Weight = 1;
    /// Admitted elevation, in metres.
    TerrainMaterialRange ElevationMetres;
    /// Admitted slope (rise over run).
    TerrainMaterialRange Slope;
    /// Admitted wetness, 0 through 1.
    TerrainMaterialRange Wetness;
};

/// @brief Fields the rules read; each holds one value per grid sample.
struct TerrainMaterialFields final
{
    /// Elevation in metres.
    foundation::BorrowedView<const float32> ElevationMetres;
    /// Dimensionless slope, as TryComputeTerrainSlope writes it.
    foundation::BorrowedView<const float32> Slope;
    /// Wetness in [0, 1], as TryComputeTerrainWetness writes it.
    foundation::BorrowedView<const float32> Wetness;
};

/// @brief Outcome of a material operation. Any failure leaves every output view unchanged.
enum class TerrainMaterialStatus : uint8
{
    /// The operation succeeded.
    Success,
    /// The grid fails IsTerrainFieldGridValid.
    InvalidGrid,
    /// A view is invalid or does not hold the documented count.
    InvalidStorage,
    /// A rule failed validation; see TerrainMaterialValidation.
    InvalidRule,
    /// The patch layout does not tile the grid's cells.
    InvalidLayout,
    /// A field sample is NaN or infinite.
    NonFiniteInput,
    /// A patch footprint needs more materials than TERRAIN_PATCH_MATERIAL_CHANNELS; see TerrainPaletteSummary.
    PaletteOverflow,
};

/// @brief The specific problem rule validation found.
enum class TerrainMaterialIssue : uint8
{
    /// No problem.
    None,
    /// The rule view is invalid, empty or has more than MAX_TERRAIN_MATERIAL_RULES entries.
    RuleCount,
    /// A rule uses TERRAIN_MATERIAL_NONE; Index names it.
    MaterialNone,
    /// A rule's weight is not finite and positive; Index names it.
    Weight,
    /// A range of the rule has a non-finite bound or width, a reversed interval or a negative width; Index names it.
    Range,
};

/// @brief Result of ValidateTerrainMaterialRules.
struct TerrainMaterialValidation final
{
    /// Success or InvalidRule.
    TerrainMaterialStatus Status = TerrainMaterialStatus::Success;
    /// The problem found.
    TerrainMaterialIssue Issue = TerrainMaterialIssue::None;
    /// Index of the rule the issue names; zero when it names none.
    usize Index = 0;
};

/// @brief Check a rule set against the documented ranges.
[[nodiscard]] TerrainMaterialValidation
ValidateTerrainMaterialRules(foundation::BorrowedView<const TerrainMaterialRule> rules) noexcept;

/// @brief Diagnostics of one weight evaluation.
struct TerrainMaterialWeightSummary final
{
    /// Samples where every rule's membership was zero; they received the first rule.
    usize UnmatchedSamples = 0;
};

/// @brief Normalized rule weight per sample: membership product times rule weight, scaled to sum to one.
///
/// A sample no rule admits takes the first rule with weight one, so every sample has a material.
/// @param grid The sample grid.
/// @param rules Validated rules, 1 through MAX_TERRAIN_MATERIAL_RULES.
/// @param fields Finite field samples the rules read.
/// @param outRuleWeights Rule-major storage of rules.GetSize() times the sample count; entry
///        [rule * samples + sample] is that rule's weight at that sample. Written only on Success.
/// @param outSummary Unmatched-sample count.
[[nodiscard]] TerrainMaterialStatus
TryEvaluateTerrainMaterialWeights(const TerrainFieldGrid& grid,
                                  foundation::BorrowedView<const TerrainMaterialRule> rules,
                                  const TerrainMaterialFields& fields,
                                  foundation::BorrowedView<float32> outRuleWeights,
                                  TerrainMaterialWeightSummary& outSummary) noexcept;

/// @brief How the grid's cells divide into render patches.
struct TerrainPatchLayout final
{
    /// Cells along each patch edge, 1 through MAX_TERRAIN_PATCH_CELLS; must divide the grid's cells on both axes.
    uint32 CellsPerPatch = DEFAULT_TERRAIN_PATCH_CELLS;
    /// Extra samples gathered around a patch when choosing its palette, 0 through MAX_TERRAIN_PATCH_HALO_SAMPLES.
    uint32 HaloSamples = 1;
};

/// @brief Patches along each axis for a grid and layout; false when the layout does not tile the grid.
[[nodiscard]] bool TryGetTerrainPatchCounts(const TerrainFieldGrid& grid,
                                            const TerrainPatchLayout& layout,
                                            uint32& outPatchesX,
                                            uint32& outPatchesZ) noexcept;

/// @brief Samples one patch stores: the cells plus the shared edge, squared.
[[nodiscard]] constexpr usize GetTerrainPatchSampleCount(const TerrainPatchLayout& layout) noexcept
{
    const usize samplesPerEdge = static_cast<usize>(layout.CellsPerPatch) + 1;
    return samplesPerEdge * samplesPerEdge;
}

/// @brief Material identities one patch blends, in ascending ID order; unused channels hold TERRAIN_MATERIAL_NONE.
struct TerrainPatchPalette final
{
    /// Material ID per channel.
    foundation::StaticArray<uint16, TERRAIN_PATCH_MATERIAL_CHANNELS> MaterialIds = {};
    /// Channels in use.
    uint8 Count = 0;
};

/// @brief Quantized channel weights of one sample; they sum to TERRAIN_MATERIAL_WEIGHT_ONE.
struct TerrainMaterialWeights final
{
    /// Weight per palette channel.
    foundation::StaticArray<uint8, TERRAIN_PATCH_MATERIAL_CHANNELS> Channels = {};
};

/// @brief One patch whose footprint needs too many materials.
struct TerrainPaletteOverflow final
{
    /// Patch column.
    uint32 PatchX = 0;
    /// Patch row.
    uint32 PatchZ = 0;
    /// Distinct materials with nonzero weight in the footprint.
    usize MaterialCount = 0;
};

/// @brief Diagnostics of one palette build.
struct TerrainPaletteSummary final
{
    /// Patches that overflowed; nonzero only with PaletteOverflow.
    usize OverflowPatches = 0;
    /// The first overflowing patch in row-major order; meaningful when OverflowPatches is nonzero.
    TerrainPaletteOverflow FirstOverflow;
    /// Largest difference between a quantized channel weight and its exact value, in weight units.
    float32 MaximumQuantizationError = 0;
};

/// @brief Gather each patch's palette from its footprint and quantize every sample's channel weights.
///
/// A patch footprint is its own samples plus HaloSamples on every side, clamped to the grid, so neighbouring
/// patches agree on the materials at their shared edge. Rules with one material ID share a channel. Quantization
/// distributes the rounding remainder to the channels with the largest fractions, so every sample sums to
/// TERRAIN_MATERIAL_WEIGHT_ONE.
/// @param grid The sample grid.
/// @param layout Patch size and halo.
/// @param rules The rules the weights were evaluated with.
/// @param ruleWeights Rule-major weights from TryEvaluateTerrainMaterialWeights.
/// @param outPalettes One palette per patch, row-major by patch row.
/// @param outSampleWeights Patch-major storage of patch count times GetTerrainPatchSampleCount(layout); within a
///        patch, samples are row-major from its minimum corner.
/// @param outSummary Overflow and quantization diagnostics; written on Success and on PaletteOverflow.
/// @return Success, PaletteOverflow with the summary naming the first patch, or a validation failure.
[[nodiscard]] TerrainMaterialStatus
TryBuildTerrainPatchPalettes(const TerrainFieldGrid& grid,
                             const TerrainPatchLayout& layout,
                             foundation::BorrowedView<const TerrainMaterialRule> rules,
                             foundation::BorrowedView<const float32> ruleWeights,
                             foundation::BorrowedView<TerrainPatchPalette> outPalettes,
                             foundation::BorrowedView<TerrainMaterialWeights> outSampleWeights,
                             TerrainPaletteSummary& outSummary) noexcept;
} // namespace ludus::content::terrain
