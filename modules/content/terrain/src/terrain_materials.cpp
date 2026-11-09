// Material rules and patch palettes; see terrain_materials.h for contracts and attribution.

#include <ludus/content/terrain/terrain_materials.h>

#include <ludus/foundation/math/scalar.hpp>

namespace ludus::content::terrain
{
namespace
{
namespace math = foundation::math;
using foundation::BorrowedView;

[[nodiscard]] bool IsRangeValid(const TerrainMaterialRange& range) noexcept
{
    return math::IsFinite(range.Minimum) == true && math::IsFinite(range.Maximum) == true &&
           math::IsFinite(range.FeatherWidth) == true && range.Minimum <= range.Maximum && range.FeatherWidth >= 0;
}

/// Membership of a value: one inside the bounds, a smooth fall to zero over the feather width outside them.
[[nodiscard]] float32 ComputeMembership(const TerrainMaterialRange& range, float32 value) noexcept
{
    float32 outside = 0;
    if (value < range.Minimum)
    {
        outside = range.Minimum - value;
    }
    else if (value > range.Maximum)
    {
        outside = value - range.Maximum;
    }
    if (outside == 0)
    {
        return 1;
    }
    if (range.FeatherWidth == 0 || outside >= range.FeatherWidth)
    {
        return 0;
    }
    return 1 - math::SmoothStep01(outside / range.FeatherWidth);
}

[[nodiscard]] bool AreAllFinite(BorrowedView<const float32> values, usize samples) noexcept
{
    if (values.IsValid() == false || values.GetSize() != samples)
    {
        return false;
    }
    for (usize index = 0; index < samples; ++index)
    {
        if (math::IsFinite(values[index]) == false)
        {
            return false;
        }
    }
    return true;
}

/// Half-open sample rectangle of one patch footprint, clamped to the grid.
struct Footprint final
{
    uint32 MinimumX = 0;
    uint32 MinimumZ = 0;
    uint32 MaximumX = 0;
    uint32 MaximumZ = 0;
};

[[nodiscard]] Footprint
ComputeFootprint(const TerrainFieldGrid& grid, const TerrainPatchLayout& layout, uint32 patchX, uint32 patchZ) noexcept
{
    const uint32 firstX = patchX * layout.CellsPerPatch;
    const uint32 firstZ = patchZ * layout.CellsPerPatch;
    Footprint footprint;
    footprint.MinimumX = firstX > layout.HaloSamples ? firstX - layout.HaloSamples : 0;
    footprint.MinimumZ = firstZ > layout.HaloSamples ? firstZ - layout.HaloSamples : 0;
    footprint.MaximumX = math::Min(firstX + layout.CellsPerPatch + 1 + layout.HaloSamples, grid.Columns);
    footprint.MaximumZ = math::Min(firstZ + layout.CellsPerPatch + 1 + layout.HaloSamples, grid.Rows);
    return footprint;
}

/// Whether any sample of the footprint gives the rule a positive weight.
[[nodiscard]] bool IsRulePresent(const TerrainFieldGrid& grid,
                                 const Footprint& footprint,
                                 BorrowedView<const float32> ruleWeights,
                                 usize rule) noexcept
{
    const usize samples = GetTerrainSampleCount(grid);
    for (uint32 z = footprint.MinimumZ; z < footprint.MaximumZ; ++z)
    {
        for (uint32 x = footprint.MinimumX; x < footprint.MaximumX; ++x)
        {
            if (ruleWeights[rule * samples + z * grid.Columns + x] > 0)
            {
                return true;
            }
        }
    }
    return false;
}

/// Gather the patch's palette; outDistinct counts every distinct present material, even past the channel limit.
[[nodiscard]] bool TryGatherPalette(const TerrainFieldGrid& grid,
                                    const Footprint& footprint,
                                    BorrowedView<const TerrainMaterialRule> rules,
                                    BorrowedView<const float32> ruleWeights,
                                    TerrainPatchPalette& outPalette,
                                    usize& outDistinct) noexcept
{
    foundation::StaticArray<uint16, MAX_TERRAIN_MATERIAL_RULES> present = {};
    usize distinct = 0;
    for (usize rule = 0; rule < rules.GetSize(); ++rule)
    {
        if (IsRulePresent(grid, footprint, ruleWeights, rule) == false)
        {
            continue;
        }
        const uint16 materialId = rules[rule].MaterialId;
        usize position = 0;
        while (position < distinct && present[position] < materialId)
        {
            ++position;
        }
        if (position < distinct && present[position] == materialId)
        {
            continue;
        }
        for (usize shift = distinct; shift > position; --shift)
        {
            present[shift] = present[shift - 1];
        }
        present[position] = materialId;
        ++distinct;
    }
    TerrainPatchPalette palette;
    for (usize channel = 0; channel < distinct && channel < TERRAIN_PATCH_MATERIAL_CHANNELS; ++channel)
    {
        palette.MaterialIds[channel] = present[channel];
        ++palette.Count;
    }
    outPalette = palette;
    outDistinct = distinct;
    return distinct <= TERRAIN_PATCH_MATERIAL_CHANNELS;
}

/// Quantize normalized channel weights so they sum exactly to TERRAIN_MATERIAL_WEIGHT_ONE; returns the error.
[[nodiscard]] float32 Quantize(const foundation::StaticArray<float32, TERRAIN_PATCH_MATERIAL_CHANNELS>& exact,
                               uint8 channelCount,
                               TerrainMaterialWeights& outWeights) noexcept
{
    foundation::StaticArray<float32, TERRAIN_PATCH_MATERIAL_CHANNELS> fractions = {};
    uint32 assigned = 0;
    for (usize channel = 0; channel < channelCount; ++channel)
    {
        const float32 scaled = exact[channel] * static_cast<float32>(TERRAIN_MATERIAL_WEIGHT_ONE);
        const uint32 whole = static_cast<uint32>(scaled);
        fractions[channel] = scaled - static_cast<float32>(whole);
        outWeights.Channels[channel] = static_cast<uint8>(whole);
        assigned += whole;
    }
    while (assigned < TERRAIN_MATERIAL_WEIGHT_ONE)
    {
        usize largest = 0;
        for (usize channel = 1; channel < channelCount; ++channel)
        {
            if (fractions[channel] > fractions[largest])
            {
                largest = channel;
            }
        }
        ++outWeights.Channels[largest];
        fractions[largest] = -1;
        ++assigned;
    }
    float32 error = 0;
    for (usize channel = 0; channel < channelCount; ++channel)
    {
        const float32 quantized =
            static_cast<float32>(outWeights.Channels[channel]) / static_cast<float32>(TERRAIN_MATERIAL_WEIGHT_ONE);
        error = math::Max(error, math::Abs(quantized - exact[channel]));
    }
    return error;
}

void WritePatchWeights(const TerrainFieldGrid& grid,
                       const TerrainPatchLayout& layout,
                       uint32 patchX,
                       uint32 patchZ,
                       BorrowedView<const TerrainMaterialRule> rules,
                       BorrowedView<const float32> ruleWeights,
                       const TerrainPatchPalette& palette,
                       BorrowedView<TerrainMaterialWeights> outPatchWeights,
                       float32& inoutMaximumError) noexcept
{
    const usize samples = GetTerrainSampleCount(grid);
    const uint32 samplesPerEdge = layout.CellsPerPatch + 1;
    const uint32 firstX = patchX * layout.CellsPerPatch;
    const uint32 firstZ = patchZ * layout.CellsPerPatch;
    for (uint32 localZ = 0; localZ < samplesPerEdge; ++localZ)
    {
        for (uint32 localX = 0; localX < samplesPerEdge; ++localX)
        {
            const usize sample = static_cast<usize>(firstZ + localZ) * grid.Columns + firstX + localX;
            foundation::StaticArray<float32, TERRAIN_PATCH_MATERIAL_CHANNELS> exact = {};
            float32 total = 0;
            for (usize rule = 0; rule < rules.GetSize(); ++rule)
            {
                const float32 weight = ruleWeights[rule * samples + sample];
                if (weight <= 0)
                {
                    continue;
                }
                for (usize channel = 0; channel < palette.Count; ++channel)
                {
                    if (palette.MaterialIds[channel] == rules[rule].MaterialId)
                    {
                        exact[channel] += weight;
                        total += weight;
                    }
                }
            }
            if (total > 0)
            {
                for (usize channel = 0; channel < palette.Count; ++channel)
                {
                    exact[channel] /= total;
                }
            }
            TerrainMaterialWeights weights;
            const float32 error = Quantize(exact, palette.Count, weights);
            inoutMaximumError = math::Max(inoutMaximumError, error);
            outPatchWeights[static_cast<usize>(localZ) * samplesPerEdge + localX] = weights;
        }
    }
}
} // namespace

TerrainMaterialValidation ValidateTerrainMaterialRules(BorrowedView<const TerrainMaterialRule> rules) noexcept
{
    if (rules.IsValid() == false || rules.IsEmpty() == true || rules.GetSize() > MAX_TERRAIN_MATERIAL_RULES)
    {
        return {TerrainMaterialStatus::InvalidRule, TerrainMaterialIssue::RuleCount, 0};
    }
    for (usize index = 0; index < rules.GetSize(); ++index)
    {
        const TerrainMaterialRule& rule = rules[index];
        if (rule.MaterialId == TERRAIN_MATERIAL_NONE)
        {
            return {TerrainMaterialStatus::InvalidRule, TerrainMaterialIssue::MaterialNone, index};
        }
        if (math::IsFinite(rule.Weight) == false || rule.Weight <= 0)
        {
            return {TerrainMaterialStatus::InvalidRule, TerrainMaterialIssue::Weight, index};
        }
        if (IsRangeValid(rule.ElevationMetres) == false || IsRangeValid(rule.Slope) == false ||
            IsRangeValid(rule.Wetness) == false)
        {
            return {TerrainMaterialStatus::InvalidRule, TerrainMaterialIssue::Range, index};
        }
    }
    return {};
}

TerrainMaterialStatus TryEvaluateTerrainMaterialWeights(const TerrainFieldGrid& grid,
                                                        BorrowedView<const TerrainMaterialRule> rules,
                                                        const TerrainMaterialFields& fields,
                                                        BorrowedView<float32> outRuleWeights,
                                                        TerrainMaterialWeightSummary& outSummary) noexcept
{
    if (IsTerrainFieldGridValid(grid) == false)
    {
        return TerrainMaterialStatus::InvalidGrid;
    }
    const TerrainMaterialValidation validation = ValidateTerrainMaterialRules(rules);
    if (validation.Status != TerrainMaterialStatus::Success)
    {
        return validation.Status;
    }
    const usize samples = GetTerrainSampleCount(grid);
    if (fields.ElevationMetres.IsValid() == false || fields.ElevationMetres.GetSize() != samples ||
        fields.Slope.IsValid() == false || fields.Slope.GetSize() != samples || fields.Wetness.IsValid() == false ||
        fields.Wetness.GetSize() != samples || outRuleWeights.IsValid() == false ||
        outRuleWeights.GetSize() != rules.GetSize() * samples)
    {
        return TerrainMaterialStatus::InvalidStorage;
    }
    if (AreAllFinite(fields.ElevationMetres, samples) == false || AreAllFinite(fields.Slope, samples) == false ||
        AreAllFinite(fields.Wetness, samples) == false)
    {
        return TerrainMaterialStatus::NonFiniteInput;
    }
    TerrainMaterialWeightSummary summary;
    for (usize sample = 0; sample < samples; ++sample)
    {
        float32 total = 0;
        for (usize rule = 0; rule < rules.GetSize(); ++rule)
        {
            const TerrainMaterialRule& current = rules[rule];
            const float32 weight = current.Weight *
                                   ComputeMembership(current.ElevationMetres, fields.ElevationMetres[sample]) *
                                   ComputeMembership(current.Slope, fields.Slope[sample]) *
                                   ComputeMembership(current.Wetness, fields.Wetness[sample]);
            outRuleWeights[rule * samples + sample] = weight;
            total += weight;
        }
        if (total > 0)
        {
            for (usize rule = 0; rule < rules.GetSize(); ++rule)
            {
                outRuleWeights[rule * samples + sample] /= total;
            }
        }
        else
        {
            ++summary.UnmatchedSamples;
            outRuleWeights[sample] = 1;
        }
    }
    outSummary = summary;
    return TerrainMaterialStatus::Success;
}

bool TryGetTerrainPatchCounts(const TerrainFieldGrid& grid,
                              const TerrainPatchLayout& layout,
                              uint32& outPatchesX,
                              uint32& outPatchesZ) noexcept
{
    if (IsTerrainFieldGridValid(grid) == false || layout.CellsPerPatch == 0 ||
        layout.CellsPerPatch > MAX_TERRAIN_PATCH_CELLS || layout.HaloSamples > MAX_TERRAIN_PATCH_HALO_SAMPLES)
    {
        return false;
    }
    const uint32 cellsX = grid.Columns - 1;
    const uint32 cellsZ = grid.Rows - 1;
    if (cellsX % layout.CellsPerPatch != 0 || cellsZ % layout.CellsPerPatch != 0)
    {
        return false;
    }
    outPatchesX = cellsX / layout.CellsPerPatch;
    outPatchesZ = cellsZ / layout.CellsPerPatch;
    return true;
}

TerrainMaterialStatus TryBuildTerrainPatchPalettes(const TerrainFieldGrid& grid,
                                                   const TerrainPatchLayout& layout,
                                                   BorrowedView<const TerrainMaterialRule> rules,
                                                   BorrowedView<const float32> ruleWeights,
                                                   BorrowedView<TerrainPatchPalette> outPalettes,
                                                   BorrowedView<TerrainMaterialWeights> outSampleWeights,
                                                   TerrainPaletteSummary& outSummary) noexcept
{
    if (IsTerrainFieldGridValid(grid) == false)
    {
        return TerrainMaterialStatus::InvalidGrid;
    }
    uint32 patchesX = 0;
    uint32 patchesZ = 0;
    if (TryGetTerrainPatchCounts(grid, layout, patchesX, patchesZ) == false)
    {
        return TerrainMaterialStatus::InvalidLayout;
    }
    const TerrainMaterialValidation validation = ValidateTerrainMaterialRules(rules);
    if (validation.Status != TerrainMaterialStatus::Success)
    {
        return validation.Status;
    }
    const usize samples = GetTerrainSampleCount(grid);
    const usize patches = static_cast<usize>(patchesX) * patchesZ;
    const usize patchSamples = GetTerrainPatchSampleCount(layout);
    if (ruleWeights.IsValid() == false || ruleWeights.GetSize() != rules.GetSize() * samples ||
        outPalettes.IsValid() == false || outPalettes.GetSize() != patches || outSampleWeights.IsValid() == false ||
        outSampleWeights.GetSize() != patches * patchSamples)
    {
        return TerrainMaterialStatus::InvalidStorage;
    }
    if (AreAllFinite(ruleWeights, ruleWeights.GetSize()) == false)
    {
        return TerrainMaterialStatus::NonFiniteInput;
    }
    // First pass: find every overflow without writing an output.
    TerrainPaletteSummary summary;
    for (uint32 patchZ = 0; patchZ < patchesZ; ++patchZ)
    {
        for (uint32 patchX = 0; patchX < patchesX; ++patchX)
        {
            TerrainPatchPalette palette;
            usize distinct = 0;
            const Footprint footprint = ComputeFootprint(grid, layout, patchX, patchZ);
            if (TryGatherPalette(grid, footprint, rules, ruleWeights, palette, distinct) == false)
            {
                if (summary.OverflowPatches == 0)
                {
                    summary.FirstOverflow = {patchX, patchZ, distinct};
                }
                ++summary.OverflowPatches;
            }
        }
    }
    if (summary.OverflowPatches != 0)
    {
        outSummary = summary;
        return TerrainMaterialStatus::PaletteOverflow;
    }
    // Second pass: palettes and quantized weights.
    for (uint32 patchZ = 0; patchZ < patchesZ; ++patchZ)
    {
        for (uint32 patchX = 0; patchX < patchesX; ++patchX)
        {
            const usize patch = static_cast<usize>(patchZ) * patchesX + patchX;
            TerrainPatchPalette palette;
            usize distinct = 0;
            const Footprint footprint = ComputeFootprint(grid, layout, patchX, patchZ);
            (void)TryGatherPalette(grid, footprint, rules, ruleWeights, palette, distinct);
            outPalettes[patch] = palette;
            BorrowedView<TerrainMaterialWeights> patchWeights(outSampleWeights.GetDataOrNull() + patch * patchSamples,
                                                              patchSamples);
            WritePatchWeights(grid,
                              layout,
                              patchX,
                              patchZ,
                              rules,
                              ruleWeights,
                              palette,
                              patchWeights,
                              summary.MaximumQuantizationError);
        }
    }
    outSummary = summary;
    return TerrainMaterialStatus::Success;
}
} // namespace ludus::content::terrain
