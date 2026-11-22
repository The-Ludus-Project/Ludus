#include "internal/foliage_cook_validation.h"

#include <ludus/foundation/base/checked_integer.hpp>

// Input validation for the foliage cook: every rule is checked before a page is
// cooked, so a rejected input writes nothing. Original implementation.

namespace ludus::content::foliage_cook::internal
{
namespace
{
namespace format = foliage_format;
namespace math = foundation::math;

constexpr int64 INT32_FIRST = -(int64{1} << 31);
constexpr int64 INT32_LAST = (int64{1} << 31) - 1;

bool IsHeightRangeValid(const FoliageRange& range) noexcept
{
    return range.IsEnabled == false ||
           (range.Minimum <= range.Maximum && range.Minimum >= -FOLIAGE_MAXIMUM_ABSOLUTE_HEIGHT &&
            range.Maximum <= FOLIAGE_MAXIMUM_ABSOLUTE_HEIGHT);
}

bool IsUnitRangeValid(const FoliageRange& range, int64 maximum) noexcept
{
    return range.IsEnabled == false ||
           (range.Minimum >= 0 && range.Minimum <= range.Maximum && range.Maximum <= maximum);
}

/// Cells of a layer overlapping a half-open lattice rectangle.
FoliageLatticeRectangle GetOverlappingCells(const FoliageLatticeRectangle& rectangle, uint32 cellSize) noexcept
{
    return
    {
        .MinimumX = FloorDivide(rectangle.MinimumX, cellSize),
        .MinimumZ = FloorDivide(rectangle.MinimumZ, cellSize),
        .MaximumX = FloorDivide(rectangle.MaximumX - 1, cellSize) + 1,
        .MaximumZ = FloorDivide(rectangle.MaximumZ - 1, cellSize) + 1,
    };
}

bool AreCellsInt32(const FoliageLatticeRectangle& cells) noexcept
{
    return cells.MinimumX >= INT32_FIRST && cells.MinimumZ >= INT32_FIRST && cells.MaximumX - 1 <= INT32_LAST &&
           cells.MaximumZ - 1 <= INT32_LAST;
}

FoliageCookStatus ValidateSpecies(const FoliageCookInput& input, FoliageCookDiagnostic& outDiagnostic) noexcept
{
    if (input.Species.IsValid() == false || input.Species.IsEmpty() == true ||
        input.Species.GetSize() > format::MAX_SPECIES)
    {
        return SetFailure(outDiagnostic, FoliageCookStatus::LimitExceeded, FoliageCookIssue::SpeciesCount, 0);
    }
    for (usize i = 0; i < input.Species.GetSize(); ++i)
    {
        const FoliageCookSpecies& species = input.Species[i];
        if (species.Key == 0 || (i > 0 && species.Key <= input.Species[i - 1].Key))
        {
            return SetFailure(outDiagnostic, FoliageCookStatus::InvalidInput, FoliageCookIssue::SpeciesKey, i);
        }
        if (species.VariantCount == 0 || species.VariantCount > format::MAX_VARIANTS_PER_SPECIES)
        {
            return SetFailure(outDiagnostic, FoliageCookStatus::InvalidInput, FoliageCookIssue::SpeciesVariants, i);
        }
        if (species.MinimumScale == 0 || species.MaximumScale < species.MinimumScale)
        {
            return SetFailure(outDiagnostic, FoliageCookStatus::InvalidInput, FoliageCookIssue::SpeciesScale, i);
        }
        bool hasValidRadii =
            species.BoundsRadius <= MAX_FOLIAGE_RADIUS && species.BoundsHeight <= MAX_FOLIAGE_BOUNDS_HEIGHT;
        for (const uint32 radius : species.ExclusionRadii)
        {
            hasValidRadii = hasValidRadii == true && radius <= MAX_FOLIAGE_RADIUS;
        }
        if (hasValidRadii == false)
        {
            return SetFailure(outDiagnostic, FoliageCookStatus::InvalidInput, FoliageCookIssue::SpeciesRadius, i);
        }
    }
    return FoliageCookStatus::Ok;
}

FoliageCookStatus ValidatePalette(const FoliageCookInput& input,
                                  const FoliageCookLayer& layer,
                                  usize index,
                                  FoliageCookDiagnostic& outDiagnostic) noexcept
{
    if (layer.Palette.IsValid() == false || layer.Palette.IsEmpty() == true ||
        layer.Palette.GetSize() > MAX_FOLIAGE_PALETTE_ENTRIES)
    {
        return SetFailure(outDiagnostic, FoliageCookStatus::InvalidInput, FoliageCookIssue::LayerPalette, index);
    }
    uint32 total = 0;
    for (const FoliagePaletteEntry& entry : layer.Palette)
    {
        if (entry.Species >= input.Species.GetSize() || entry.Weight == 0 ||
            foundation::TryAdd(total, entry.Weight, total) == false)
        {
            return SetFailure(outDiagnostic, FoliageCookStatus::InvalidInput, FoliageCookIssue::LayerPalette, index);
        }
    }
    return FoliageCookStatus::Ok;
}

FoliageCookStatus ValidateLayerRules(const FoliageCookInput& input,
                                     const FoliageCookLayer& layer,
                                     usize index,
                                     FoliageCookDiagnostic& outDiagnostic) noexcept
{
    if (IsHeightRangeValid(layer.Height) == false ||
        IsUnitRangeValid(layer.Slope, FOLIAGE_MAXIMUM_SLOPE_UNITS) == false ||
        IsUnitRangeValid(layer.Wetness, FOLIAGE_FIELD_UNITS_ONE) == false)
    {
        return SetFailure(outDiagnostic, FoliageCookStatus::InvalidInput, FoliageCookIssue::LayerRange, index);
    }
    if (layer.Wetness.IsEnabled == true && layer.IsWetnessRequired == true && input.Terrain.Wetness.IsEmpty() == true)
    {
        return SetFailure(outDiagnostic, FoliageCookStatus::MissingField, FoliageCookIssue::MissingWetness, index);
    }
    if (layer.Masks.IsValid() == false || layer.Masks.GetSize() > MAX_FOLIAGE_LAYER_MASKS)
    {
        return SetFailure(outDiagnostic, FoliageCookStatus::LimitExceeded, FoliageCookIssue::LayerMasks, index);
    }
    for (usize i = 0; i < layer.Masks.GetSize(); ++i)
    {
        const FoliageMaskRule& rule = layer.Masks[i];
        if (rule.MaskId == 0 || (i > 0 && rule.MaskId <= layer.Masks[i - 1].MaskId))
        {
            return SetFailure(outDiagnostic, FoliageCookStatus::InvalidInput, FoliageCookIssue::LayerMasks, index);
        }
        bool isPresent = false;
        for (const FoliageMaskChannel& channel : input.Terrain.Masks)
        {
            isPresent = isPresent == true || channel.Id == rule.MaskId;
        }
        if (rule.IsRequired == true && isPresent == false)
        {
            outDiagnostic.FieldId = rule.MaskId;
            return SetFailure(outDiagnostic, FoliageCookStatus::MissingField, FoliageCookIssue::MissingMask, index);
        }
    }
    return FoliageCookStatus::Ok;
}

FoliageCookStatus ValidateLayer(const FoliageCookInput& input,
                                const TerrainSampler& sampler,
                                usize index,
                                FoliageCookDiagnostic& outDiagnostic) noexcept
{
    const FoliageCookLayer& layer = input.Layers[index];
    if (index > 0 && layer.Id <= input.Layers[index - 1].Id)
    {
        return SetFailure(outDiagnostic, FoliageCookStatus::InvalidInput, FoliageCookIssue::LayerOrder, index);
    }
    if (layer.SlotsPerCell == 0 || layer.SlotsPerCell > format::MAX_SLOTS_PER_CELL)
    {
        return SetFailure(outDiagnostic, FoliageCookStatus::InvalidInput, FoliageCookIssue::LayerSlots, index);
    }
    if (layer.CellSize == 0 || layer.CellSize > format::MAX_CELL_SIZE || input.Domain.PageSize % layer.CellSize != 0)
    {
        return SetFailure(outDiagnostic, FoliageCookStatus::InvalidInput, FoliageCookIssue::LayerCellSize, index);
    }
    const FoliageCookStatus paletteStatus = ValidatePalette(input, layer, index, outDiagnostic);
    if (paletteStatus != FoliageCookStatus::Ok)
    {
        return paletteStatus;
    }
    uint64 threshold = 0;
    if (TryComputeAdmissionThreshold(layer.Density,
                                     layer.CellSize,
                                     sampler.SpacingMetres,
                                     layer.SlotsPerCell,
                                     threshold) == false)
    {
        return SetFailure(outDiagnostic, FoliageCookStatus::InvalidInput, FoliageCookIssue::LayerDensity, index);
    }
    const FoliageCookStatus ruleStatus = ValidateLayerRules(input, layer, index, outDiagnostic);
    if (ruleStatus != FoliageCookStatus::Ok)
    {
        return ruleStatus;
    }
    if (AreCellsInt32(GetOverlappingCells(sampler.Extent, layer.CellSize)) == false)
    {
        return SetFailure(outDiagnostic,
                          FoliageCookStatus::CoordinateOutOfRange,
                          FoliageCookIssue::TerrainCoordinates,
                          index);
    }
    return FoliageCookStatus::Ok;
}

FoliageCookStatus ValidateSuppressions(const FoliageCookInput& input, FoliageCookDiagnostic& outDiagnostic) noexcept
{
    if (input.Suppressions.IsValid() == false || input.Suppressions.GetSize() > MAX_FOLIAGE_SUPPRESSIONS)
    {
        return SetFailure(outDiagnostic, FoliageCookStatus::LimitExceeded, FoliageCookIssue::SuppressionCount, 0);
    }
    for (usize i = 1; i < input.Suppressions.GetSize(); ++i)
    {
        if (IsFoliageKeyBefore(input.Suppressions[i - 1], input.Suppressions[i]) == false)
        {
            return SetFailure(outDiagnostic, FoliageCookStatus::InvalidInput, FoliageCookIssue::SuppressionOrder, i);
        }
    }
    return FoliageCookStatus::Ok;
}
} // namespace

FoliageCookStatus
SetFailure(FoliageCookDiagnostic& outDiagnostic, FoliageCookStatus status, FoliageCookIssue issue, usize index) noexcept
{
    outDiagnostic.Status = status;
    outDiagnostic.Issue = issue;
    outDiagnostic.Index = index;
    return status;
}

FoliageCookStatus
ValidateInput(const FoliageCookInput& input, TerrainSampler& outSampler, FoliageCookDiagnostic& outDiagnostic) noexcept
{
    outDiagnostic = {};
    if (input.Domain.Id == 0)
    {
        return SetFailure(outDiagnostic, FoliageCookStatus::InvalidInput, FoliageCookIssue::Domain, 0);
    }
    if (input.Domain.PageSize == 0 || input.Domain.PageSize > format::MAX_PAGE_SIZE)
    {
        return SetFailure(outDiagnostic, FoliageCookStatus::InvalidInput, FoliageCookIssue::PageSize, 0);
    }
    TerrainSampler sampler;
    FoliageCookIssue terrainIssue = FoliageCookIssue::None;
    usize terrainIndex = 0;
    const FoliageCookStatus terrainStatus = PrepareTerrainSampler(input, sampler, terrainIssue, terrainIndex);
    if (terrainStatus != FoliageCookStatus::Ok)
    {
        return SetFailure(outDiagnostic, terrainStatus, terrainIssue, terrainIndex);
    }
    const FoliageLatticeRectangle pages = GetOverlappingCells(sampler.Extent, input.Domain.PageSize);
    if (AreCellsInt32(pages) == false)
    {
        return SetFailure(outDiagnostic,
                          FoliageCookStatus::CoordinateOutOfRange,
                          FoliageCookIssue::TerrainCoordinates,
                          0);
    }
    const FoliageCookStatus speciesStatus = ValidateSpecies(input, outDiagnostic);
    if (speciesStatus != FoliageCookStatus::Ok)
    {
        return speciesStatus;
    }
    if (input.Layers.IsValid() == false || input.Layers.IsEmpty() == true ||
        input.Layers.GetSize() > MAX_FOLIAGE_COOK_LAYERS)
    {
        return SetFailure(outDiagnostic, FoliageCookStatus::LimitExceeded, FoliageCookIssue::LayerCount, 0);
    }
    for (usize i = 0; i < input.Layers.GetSize(); ++i)
    {
        const FoliageCookStatus layerStatus = ValidateLayer(input, sampler, i, outDiagnostic);
        if (layerStatus != FoliageCookStatus::Ok)
        {
            return layerStatus;
        }
    }
    const FoliageCookStatus suppressionStatus = ValidateSuppressions(input, outDiagnostic);
    if (suppressionStatus != FoliageCookStatus::Ok)
    {
        return suppressionStatus;
    }
    outSampler = sampler;
    return FoliageCookStatus::Ok;
}

LayerPlan PrepareLayer(const FoliageCookInput& input, const TerrainSampler& sampler, usize index) noexcept
{
    const FoliageCookLayer& layer = input.Layers[index];
    LayerPlan plan;
    plan.Id = layer.Id;
    plan.SlotsPerCell = layer.SlotsPerCell;
    plan.CellSize = layer.CellSize;
    plan.Stratum = layer.Stratum;
    for (const FoliagePaletteEntry& entry : layer.Palette)
    {
        plan.PaletteTotal += entry.Weight;
        const FoliageCookSpecies& species = input.Species[entry.Species];
        for (const uint32 radius : species.ExclusionRadii)
        {
            const uint64 scaled = ScaleRadius(radius, species.MaximumScale);
            plan.MaximumRadius = scaled > plan.MaximumRadius ? scaled : plan.MaximumRadius;
        }
    }
    // Validation proved every value below is admitted, so the checked preparations succeed.
    [[maybe_unused]] const bool isPrepared =
        TryComputeAdmissionThreshold(layer.Density,
                                     layer.CellSize,
                                     sampler.SpacingMetres,
                                     layer.SlotsPerCell,
                                     plan.AdmissionThreshold) == true &&
        math::TryPrepareBound32(layer.CellSize, plan.CellBound) == math::MathStatus::Success &&
        math::TryPrepareBound32(plan.PaletteTotal, plan.PaletteBound) == math::MathStatus::Success &&
        math::TryMakeRandomKey(layer.Seed, FOLIAGE_PLACEMENT_RANDOM_DOMAIN, plan.PlacementKey) ==
            math::MathStatus::Success &&
        math::TryMakeRandomKey(layer.Seed, FOLIAGE_SPECIES_RANDOM_DOMAIN, plan.SpeciesKey) ==
            math::MathStatus::Success &&
        math::TryMakeRandomKey(layer.Seed, FOLIAGE_VARIANT_RANDOM_DOMAIN, plan.VariantKey) ==
            math::MathStatus::Success &&
        math::TryMakeRandomKey(layer.Seed, FOLIAGE_TRANSFORM_RANDOM_DOMAIN, plan.TransformKey) ==
            math::MathStatus::Success;
    LUDUS_ASSERT(isPrepared == true);
    plan.TerrainCells = GetOverlappingCells(sampler.Extent, layer.CellSize);
    for (usize i = 0; i < MAX_FOLIAGE_LAYER_MASKS; ++i)
    {
        plan.MaskChannels[i] = ABSENT_MASK_CHANNEL;
    }
    for (usize i = 0; i < layer.Masks.GetSize(); ++i)
    {
        for (usize channel = 0; channel < input.Terrain.Masks.GetSize(); ++channel)
        {
            if (input.Terrain.Masks[channel].Id == layer.Masks[i].MaskId)
            {
                plan.MaskChannels[i] = static_cast<uint32>(channel);
            }
        }
    }
    return plan;
}
} // namespace ludus::content::foliage_cook::internal

namespace ludus::content::foliage_cook
{
FoliageCookStatus ValidateFoliageCookInput(const FoliageCookInput& input, FoliageCookDiagnostic& outDiagnostic) noexcept
{
    internal::TerrainSampler sampler;
    return internal::ValidateInput(input, sampler, outDiagnostic);
}
} // namespace ludus::content::foliage_cook
