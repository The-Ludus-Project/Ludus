#pragma once

#include <ludus/foundation/base/types.h>

#include <ludus/foundation/containers/static_array.hpp>
#include <ludus/foundation/math/addressed_random.hpp>

#include <ludus/content/foliage_cook/foliage_cook.h>

#include "foliage_terrain_sampler.h"

// Validated, prepared form of one layer, shared by validation, cooking and dirty-page expansion.
namespace ludus::content::foliage_cook::internal
{
/// Mask rule index that names no channel: the rule is optional and its channel is absent.
inline constexpr uint32 ABSENT_MASK_CHANNEL = ~uint32{0};

/// Prepared lattice, keys and thresholds of one layer.
struct LayerPlan final
{
    uint16 Id = 0;
    uint16 SlotsPerCell = 1;
    uint32 CellSize = 1;
    uint8 Stratum = 0;
    uint32 PaletteTotal = 0;
    uint64 AdmissionThreshold = 0; ///< 2^32 times the per-slot admission probability at suitability one.
    uint64 MaximumRadius = 0;      ///< Largest scaled exclusion radius of any palette species, lattice units.
    foundation::math::PreparedBound32 CellBound;
    foundation::math::PreparedBound32 PaletteBound;
    foundation::math::RandomKey PlacementKey;
    foundation::math::RandomKey SpeciesKey;
    foundation::math::RandomKey VariantKey;
    foundation::math::RandomKey TransformKey;
    /// Cells overlapping the terrain extent, half-open; every eligible candidate lies in them.
    FoliageLatticeRectangle TerrainCells;
    foundation::StaticArray<uint32, MAX_FOLIAGE_LAYER_MASKS> MaskChannels{};
};

/// @brief Validate every input rule.
/// @param outSampler Receives the prepared terrain sampler on success.
/// @return Ok, or the first failure with outDiagnostic naming it.
[[nodiscard]] FoliageCookStatus
ValidateInput(const FoliageCookInput& input, TerrainSampler& outSampler, FoliageCookDiagnostic& outDiagnostic) noexcept;

/// @brief Prepare one layer of a validated input.
/// @pre ValidateInput succeeded for the input.
[[nodiscard]] LayerPlan
PrepareLayer(const FoliageCookInput& input, const TerrainSampler& sampler, usize index) noexcept;

/// Record a failure in a diagnostic and return its status.
FoliageCookStatus SetFailure(FoliageCookDiagnostic& outDiagnostic,
                             FoliageCookStatus status,
                             FoliageCookIssue issue,
                             usize index) noexcept;
} // namespace ludus::content::foliage_cook::internal
