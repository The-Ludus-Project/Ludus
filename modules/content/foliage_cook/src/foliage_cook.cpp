#include <ludus/content/foliage_cook/foliage_cook.h>

#include <ludus/foundation/base/checked_integer.hpp>
#include <ludus/foundation/containers/sort.hpp>

#include "internal/foliage_cook_validation.h"
#include "internal/foliage_fixed_point.h"
#include "internal/foliage_terrain_sampler.h"

// Bounded terrain foliage cook. Every candidate is regenerated from its persistent
// key, and a page cook evaluates exactly the halo its owned decisions depend on,
// so pages cooked alone, together, shuffled or in parallel give identical records.
//
// Thanks to Josh Klint, "Vegetation Management in Leadwerks Game Engine 4", Game
// Engine Gems 3, chapter 4, pp. 53-71, for compact placement shared by rendering
// and collision; no chapter code is copied. Review:
// docs/architecture/procedural-content-generation-gems-review.md#vegetation-placement-and-collision.
// The fixed slot set per cell with priority thinning is the Ludus finite-neighborhood
// rule of docs/architecture/procedural-content-generation.md#vegetation-and-scatter,
// extended here with ordered strata and exclusion classes; it trades maximal
// Poisson-disk coverage for finite, order-independent support. Design:
// docs/architecture/foliage.md, "Terrain cook".

namespace ludus::content::foliage_cook
{
namespace
{
namespace format = foliage_format;
namespace math = foundation::math;
using foliage_pages::FoliagePage;
using foliage_pages::FoliagePageBounds;
using foliage_pages::FoliagePlacement;
using foundation::Array;
using foundation::BorrowedView;
using internal::LayerPlan;
using internal::RootSample;
using internal::TerrainSampler;

/// Shift that keeps the high 16 bits of a raw word as a yaw in 1/65536 turns.
constexpr uint32 YAW_SHIFT = 16;
/// log2 of FOLIAGE_SUITABILITY_ONE.
constexpr uint32 SUITABILITY_SHIFT = 16;
static_assert((uint32{1} << SUITABILITY_SHIFT) == FOLIAGE_SUITABILITY_ONE);

constexpr uint8 STATE_ELIGIBLE = 1U << 0U;
constexpr uint8 STATE_SUPPRESSED = 1U << 1U;
constexpr uint8 STATE_OCCUPANCY_DECIDED = 1U << 2U;
constexpr uint8 STATE_OCCUPIED = 1U << 3U;
constexpr uint8 STATE_DECIDED = 1U << 4U;
constexpr uint8 STATE_ACCEPTED = 1U << 5U;

/// One candidate's sampled state; the species fields are filled only for eligible candidates.
struct CandidateEntry final
{
    FoliageCandidateKey Key{};
    int64 RootX = 0;
    int64 RootZ = 0;
    uint32 Priority = 0;
    foundation::StaticArray<uint32, FOLIAGE_EXCLUSION_CLASS_COUNT> Radii{};
    uint16 Species = 0;
    uint16 Scale = 0;
    uint8 State = 0;
};

/// A layer's evaluated cells: entries [First, First + cells * slots), row-major then slot.
struct LayerTable final
{
    FoliageLatticeRectangle Cells{};
    usize First = 0;
    usize Count = 0;
};

/// Prepared plan of a validated input.
struct CookPlan final
{
    TerrainSampler Sampler;
    Array<LayerPlan> Layers;
    /// Stratum index of each layer: the rank of its stratum among the distinct strata.
    Array<usize> LayerStrata;
    /// Largest scaled exclusion radius of each stratum, lattice units.
    Array<uint64> StratumReach;
};

/// Pages under construction; moved into a FoliageCookedPages on success.
struct CookedPagesBuilder final
{
    Array<FoliagePage> Pages;
    Array<FoliagePlacement> Placements;
    /// Active suppressions of each page.
    Array<uint32> ActiveSuppressions;
};

/// Reusable storage of one page cook.
struct PageWork final
{
    Array<CandidateEntry> Entries;
    Array<LayerTable> Tables;
    /// Roots whose acceptance each stratum must decide.
    Array<FoliageLatticeRectangle> Decisions;
    /// Roots whose occupancy each stratum must decide: the decisions expanded by the thinning reach.
    Array<FoliageLatticeRectangle> Occupancy;
};

bool IsEmpty(const FoliageLatticeRectangle& rectangle) noexcept
{
    return rectangle.MinimumX >= rectangle.MaximumX || rectangle.MinimumZ >= rectangle.MaximumZ;
}

bool Contains(const FoliageLatticeRectangle& rectangle, int64 x, int64 z) noexcept
{
    return x >= rectangle.MinimumX && x < rectangle.MaximumX && z >= rectangle.MinimumZ && z < rectangle.MaximumZ;
}

int64 GetMinimum(int64 left, int64 right) noexcept
{
    return left < right ? left : right;
}

int64 GetMaximum(int64 left, int64 right) noexcept
{
    return left > right ? left : right;
}

FoliageLatticeRectangle Intersect(const FoliageLatticeRectangle& left, const FoliageLatticeRectangle& right) noexcept
{
    return
    {
        .MinimumX = GetMaximum(left.MinimumX, right.MinimumX),
        .MinimumZ = GetMaximum(left.MinimumZ, right.MinimumZ),
        .MaximumX = GetMinimum(left.MaximumX, right.MaximumX),
        .MaximumZ = GetMinimum(left.MaximumZ, right.MaximumZ),
    };
}

FoliageLatticeRectangle Unite(const FoliageLatticeRectangle& left, const FoliageLatticeRectangle& right) noexcept
{
    if (IsEmpty(left) == true)
    {
        return right;
    }
    if (IsEmpty(right) == true)
    {
        return left;
    }
    return
    {
        .MinimumX = GetMinimum(left.MinimumX, right.MinimumX),
        .MinimumZ = GetMinimum(left.MinimumZ, right.MinimumZ),
        .MaximumX = GetMaximum(left.MaximumX, right.MaximumX),
        .MaximumZ = GetMaximum(left.MaximumZ, right.MaximumZ),
    };
}

bool TryExpand(const FoliageLatticeRectangle& rectangle, uint64 reach, FoliageLatticeRectangle& outRectangle) noexcept
{
    int64 distance = 0;
    FoliageLatticeRectangle expanded;
    if (foundation::TryIntegerCast(reach, distance) == false ||
        foundation::TrySubtract(rectangle.MinimumX, distance, expanded.MinimumX) == false ||
        foundation::TrySubtract(rectangle.MinimumZ, distance, expanded.MinimumZ) == false ||
        foundation::TryAdd(rectangle.MaximumX, distance, expanded.MaximumX) == false ||
        foundation::TryAdd(rectangle.MaximumZ, distance, expanded.MaximumZ) == false)
    {
        return false;
    }
    outRectangle = expanded;
    return true;
}

/// Cells of a layer overlapping a nonempty half-open lattice rectangle.
FoliageLatticeRectangle GetOverlappingCells(const FoliageLatticeRectangle& rectangle, uint32 cellSize) noexcept
{
    return
    {
        .MinimumX = internal::FloorDivide(rectangle.MinimumX, cellSize),
        .MinimumZ = internal::FloorDivide(rectangle.MinimumZ, cellSize),
        .MaximumX = internal::FloorDivide(rectangle.MaximumX - 1, cellSize) + 1,
        .MaximumZ = internal::FloorDivide(rectangle.MaximumZ - 1, cellSize) + 1,
    };
}

FoliageLatticeRectangle GetPageSquare(FoliagePageAddress page, uint32 pageSize) noexcept
{
    return
    {
        .MinimumX = int64{page.X} * pageSize,
        .MinimumZ = int64{page.Z} * pageSize,
        .MaximumX = (int64{page.X} + 1) * pageSize,
        .MaximumZ = (int64{page.Z} + 1) * pageSize,
    };
}

bool IsPageBefore(const FoliagePageAddress& left, const FoliagePageAddress& right) noexcept
{
    return left.Z < right.Z || (left.Z == right.Z && left.X < right.X);
}

FoliageCookStatus
Fail(FoliageCookDiagnostic& outDiagnostic, FoliageCookStatus status, FoliageCookIssue issue, usize index) noexcept
{
    return internal::SetFailure(outDiagnostic, status, issue, index);
}

FoliageCookStatus
TryPreparePlan(const FoliageCookInput& input, CookPlan& outPlan, FoliageCookDiagnostic& outDiagnostic) noexcept
{
    const FoliageCookStatus status = internal::ValidateInput(input, outPlan.Sampler, outDiagnostic);
    if (status != FoliageCookStatus::Ok)
    {
        return status;
    }
    const usize layerCount = input.Layers.GetSize();
    Array<uint8> strata;
    if (outPlan.Layers.TryResize(layerCount) == false || outPlan.LayerStrata.TryResize(layerCount) == false ||
        strata.TryEnsureCapacity(layerCount) == false)
    {
        return Fail(outDiagnostic, FoliageCookStatus::AllocationFailed, FoliageCookIssue::Allocation, 0);
    }
    for (usize i = 0; i < layerCount; ++i)
    {
        outPlan.Layers[i] = internal::PrepareLayer(input, outPlan.Sampler, i);
        bool isKnown = false;
        for (const uint8 stratum : strata)
        {
            isKnown = isKnown == true || stratum == input.Layers[i].Stratum;
        }
        if (isKnown == false)
        {
            [[maybe_unused]] const bool isAdded = strata.TryAdd(input.Layers[i].Stratum);
            LUDUS_ASSERT(isAdded == true);
        }
    }
    foundation::Sort(strata.GetData(), strata.GetSize(), [](uint8 left, uint8 right) { return left < right; });
    if (outPlan.StratumReach.TryResize(strata.GetSize(), uint64{0}) == false)
    {
        return Fail(outDiagnostic, FoliageCookStatus::AllocationFailed, FoliageCookIssue::Allocation, 0);
    }
    for (usize i = 0; i < layerCount; ++i)
    {
        for (usize stratum = 0; stratum < strata.GetSize(); ++stratum)
        {
            if (strata[stratum] == input.Layers[i].Stratum)
            {
                outPlan.LayerStrata[i] = stratum;
                const uint64 radius = outPlan.Layers[i].MaximumRadius;
                outPlan.StratumReach[stratum] =
                    radius > outPlan.StratumReach[stratum] ? radius : outPlan.StratumReach[stratum];
            }
        }
    }
    return FoliageCookStatus::Ok;
}

uint32 ComputeRangeMultiplier(const FoliageRange& range, int64 value) noexcept
{
    if (range.IsEnabled == false || (value >= range.Minimum && value <= range.Maximum))
    {
        return FOLIAGE_SUITABILITY_ONE;
    }
    const uint64 distance =
        value < range.Minimum ? static_cast<uint64>(range.Minimum - value) : static_cast<uint64>(value - range.Maximum);
    if (distance >= range.Feather)
    {
        return 0;
    }
    return static_cast<uint32>(((range.Feather - distance) << SUITABILITY_SHIFT) / range.Feather);
}

uint64 ApplyMultiplier(uint64 suitability, uint32 multiplier) noexcept
{
    return (suitability * multiplier) >> SUITABILITY_SHIFT;
}

bool IsSuppressed(const FoliageCookInput& input, const FoliageCandidateKey& key) noexcept
{
    usize lowest = 0;
    usize highest = input.Suppressions.GetSize();
    while (lowest < highest)
    {
        const usize middle = lowest + (highest - lowest) / 2;
        if (IsFoliageKeyBefore(input.Suppressions[middle], key) == true)
        {
            lowest = middle + 1;
        }
        else
        {
            highest = middle;
        }
    }
    return lowest < input.Suppressions.GetSize() && input.Suppressions[lowest] == key;
}

/// Suitability in FOLIAGE_SUITABILITY_ONE units: height, slope, wetness, then masks in rule order.
uint64 ComputeSuitability(const FoliageCookInput& input,
                          const TerrainSampler& sampler,
                          const FoliageCookLayer& layer,
                          const LayerPlan& plan,
                          const RootSample& sample) noexcept
{
    uint64 suitability = FOLIAGE_SUITABILITY_ONE;
    suitability = ApplyMultiplier(suitability, ComputeRangeMultiplier(layer.Height, sample.Height));
    if (layer.Slope.IsEnabled == true)
    {
        const uint32 slope = internal::ComputeSlopeUnits(sample.GradientX, sample.GradientZ, sampler.Spacing);
        suitability = ApplyMultiplier(suitability, ComputeRangeMultiplier(layer.Slope, slope));
    }
    if (layer.Wetness.IsEnabled == true && input.Terrain.Wetness.IsEmpty() == false)
    {
        const uint32 wetness = internal::InterpolateField(input.Terrain.Wetness, sample);
        suitability = ApplyMultiplier(suitability, ComputeRangeMultiplier(layer.Wetness, wetness));
    }
    for (usize i = 0; i < layer.Masks.GetSize(); ++i)
    {
        if (plan.MaskChannels[i] == internal::ABSENT_MASK_CHANNEL)
        {
            continue;
        }
        const uint32 value = internal::InterpolateField(input.Terrain.Masks[plan.MaskChannels[i]].Values, sample);
        const uint32 multiplier = static_cast<uint32>((uint64{value} << SUITABILITY_SHIFT) / FOLIAGE_FIELD_UNITS_ONE);
        suitability = ApplyMultiplier(suitability, multiplier);
    }
    return suitability;
}

FoliageCookStatus FailSampling(FoliageCookDiagnostic& outDiagnostic, const FoliageCandidateKey& key) noexcept
{
    outDiagnostic.Candidate = key;
    return Fail(outDiagnostic, FoliageCookStatus::SamplingExhausted, FoliageCookIssue::Sampling, 0);
}

/// Sample a candidate's root, suitability, admission and suppression, then its species, scale and priority.
FoliageCookStatus EvaluateCandidate(const FoliageCookInput& input,
                                    const CookPlan& plan,
                                    usize layerIndex,
                                    const FoliageCandidateKey& key,
                                    CandidateEntry& outEntry,
                                    FoliageCookDiagnostic& outDiagnostic) noexcept
{
    const FoliageCookLayer& layer = input.Layers[layerIndex];
    const LayerPlan& layerPlan = plan.Layers[layerIndex];
    CandidateEntry entry;
    entry.Key = key;
    uint32 offsetX = 0;
    uint32 offsetZ = 0;
    if (math::TrySampleBounded(layerPlan.PlacementKey,
                               PackFoliageRandomAddress(key, FOLIAGE_OFFSET_X_DIMENSION),
                               layerPlan.CellBound,
                               offsetX) != math::MathStatus::Success ||
        math::TrySampleBounded(layerPlan.PlacementKey,
                               PackFoliageRandomAddress(key, FOLIAGE_OFFSET_Z_DIMENSION),
                               layerPlan.CellBound,
                               offsetZ) != math::MathStatus::Success)
    {
        return FailSampling(outDiagnostic, key);
    }
    // Validated cells fit int32 and cell sizes 2^16, so roots stay below 2^48.
    entry.RootX = int64{key.CellX} * layer.CellSize + offsetX;
    entry.RootZ = int64{key.CellZ} * layer.CellSize + offsetZ;
    const RootSample sample = internal::SampleRoot(plan.Sampler, entry.RootX, entry.RootZ);
    if (sample.IsInside == false || sample.IsHole == true)
    {
        outEntry = entry;
        return FoliageCookStatus::Ok;
    }
    const uint64 suitability = ComputeSuitability(input, plan.Sampler, layer, layerPlan, sample);
    const uint64 threshold = (layerPlan.AdmissionThreshold * suitability) >> SUITABILITY_SHIFT;
    const uint32 admission =
        math::SampleUInt32(layerPlan.PlacementKey, PackFoliageRandomAddress(key, FOLIAGE_ADMISSION_DIMENSION));
    if (uint64{admission} >= threshold)
    {
        outEntry = entry;
        return FoliageCookStatus::Ok;
    }
    if (IsSuppressed(input, key) == true)
    {
        entry.State = STATE_SUPPRESSED;
        outEntry = entry;
        return FoliageCookStatus::Ok;
    }
    uint32 choice = 0;
    if (math::TrySampleBounded(layerPlan.SpeciesKey,
                               PackFoliageRandomAddress(key, FOLIAGE_SPECIES_CHOICE_DIMENSION),
                               layerPlan.PaletteBound,
                               choice) != math::MathStatus::Success)
    {
        return FailSampling(outDiagnostic, key);
    }
    usize paletteIndex = 0;
    uint32 cumulative = layer.Palette[0].Weight;
    while (choice >= cumulative)
    {
        ++paletteIndex;
        cumulative += layer.Palette[paletteIndex].Weight;
    }
    entry.Species = layer.Palette[paletteIndex].Species;
    const FoliageCookSpecies& species = input.Species[entry.Species];
    uint32 scaleOffset = 0;
    const uint32 scaleWidth = uint32{species.MaximumScale} - species.MinimumScale + 1;
    if (math::TrySampleBounded(layerPlan.SpeciesKey,
                               PackFoliageRandomAddress(key, FOLIAGE_SCALE_DIMENSION),
                               scaleWidth,
                               scaleOffset) != math::MathStatus::Success)
    {
        return FailSampling(outDiagnostic, key);
    }
    entry.Scale = static_cast<uint16>(species.MinimumScale + scaleOffset);
    for (usize i = 0; i < FOLIAGE_EXCLUSION_CLASS_COUNT; ++i)
    {
        entry.Radii[i] = static_cast<uint32>(internal::ScaleRadius(species.ExclusionRadii[i], entry.Scale));
    }
    entry.Priority =
        math::SampleUInt32(layerPlan.PlacementKey, PackFoliageRandomAddress(key, FOLIAGE_PRIORITY_DIMENSION));
    entry.State = STATE_ELIGIBLE;
    outEntry = entry;
    return FoliageCookStatus::Ok;
}

/// Whether two candidates occupy a common exclusion class closer than the sum of their radii in it.
bool AreConflicting(const CandidateEntry& left, const CandidateEntry& right) noexcept
{
    // Neighbours lie within a few cells of one another, so the differences stay below 2^24.
    const int64 differenceX = left.RootX - right.RootX;
    const int64 differenceZ = left.RootZ - right.RootZ;
    const int64 distanceSquared = differenceX * differenceX + differenceZ * differenceZ;
    for (usize i = 0; i < FOLIAGE_EXCLUSION_CLASS_COUNT; ++i)
    {
        if (left.Radii[i] == 0 || right.Radii[i] == 0)
        {
            continue;
        }
        const int64 reach = int64{left.Radii[i]} + right.Radii[i];
        if (distanceSquared < reach * reach)
        {
            return true;
        }
    }
    return false;
}

/// Whether `other` has greater total priority: a higher sampled word, then a later identity.
bool Outranks(const CandidateEntry& other, const CandidateEntry& candidate) noexcept
{
    return other.Priority > candidate.Priority ||
           (other.Priority == candidate.Priority && IsFoliageKeyBefore(candidate.Key, other.Key) == true);
}

// Column, row, slot is the documented table order of every caller.
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
usize GetEntryIndex(const LayerTable& table, const LayerPlan& layer, int64 cellX, int64 cellZ, uint32 slot) noexcept
{
    const usize width = static_cast<usize>(table.Cells.MaximumX - table.Cells.MinimumX);
    const usize row = static_cast<usize>(cellZ - table.Cells.MinimumZ);
    const usize column = static_cast<usize>(cellX - table.Cells.MinimumX);
    return table.First + (row * width + column) * layer.SlotsPerCell + slot;
}

/// Visit the entries of a layer whose cells overlap the lattice box [x - reach, x + reach] on each axis.
/// The visitor returns true to stop; the result reports whether it did.
template <typename Visitor>
bool VisitNeighbors(const PageWork& work,
                    const CookPlan& plan,
                    usize layerIndex,
                    const CandidateEntry& center,
                    uint64 reach,
                    const Visitor& visitor) noexcept
{
    const LayerPlan& layer = plan.Layers[layerIndex];
    const LayerTable& table = work.Tables[layerIndex];
    const int64 distance = static_cast<int64>(reach);
    const FoliageLatticeRectangle box
    {
        .MinimumX = center.RootX - distance,
        .MinimumZ = center.RootZ - distance,
        .MaximumX = center.RootX + distance + 1,
        .MaximumZ = center.RootZ + distance + 1,
    };
    // Cells outside the table lie outside the terrain, so their candidates are never eligible.
    const FoliageLatticeRectangle cells = Intersect(GetOverlappingCells(box, layer.CellSize), table.Cells);
    for (int64 cellZ = cells.MinimumZ; cellZ < cells.MaximumZ; ++cellZ)
    {
        for (int64 cellX = cells.MinimumX; cellX < cells.MaximumX; ++cellX)
        {
            for (uint32 slot = 0; slot < layer.SlotsPerCell; ++slot)
            {
                if (visitor(work.Entries[GetEntryIndex(table, layer, cellX, cellZ, slot)]) == true)
                {
                    return true;
                }
            }
        }
    }
    return false;
}

/// Whether an accepted placement of an earlier stratum conflicts with an eligible candidate.
bool IsOccupied(const PageWork& work, const CookPlan& plan, usize stratum, const CandidateEntry& candidate) noexcept
{
    for (usize earlier = 0; earlier < stratum; ++earlier)
    {
        const uint64 reach = plan.StratumReach[stratum] + plan.StratumReach[earlier];
        if (reach == 0)
        {
            continue;
        }
        for (usize layer = 0; layer < plan.Layers.GetSize(); ++layer)
        {
            if (plan.LayerStrata[layer] != earlier)
            {
                continue;
            }
            const bool isOccupied =
                VisitNeighbors(work, plan, layer, candidate, reach, [&](const CandidateEntry& other) noexcept {
                    if ((other.State & STATE_ELIGIBLE) == 0 || AreConflicting(other, candidate) == false)
                    {
                        return false;
                    }
                    // Every conflicting root lies inside the earlier stratum's decision region.
                    LUDUS_ASSERT((other.State & STATE_DECIDED) != 0);
                    return (other.State & STATE_ACCEPTED) != 0;
                });
            if (isOccupied == true)
            {
                return true;
            }
        }
    }
    return false;
}

/// Whether an unoccupied eligible candidate of the same stratum outranks and conflicts with a candidate.
bool IsOutranked(const PageWork& work, const CookPlan& plan, usize stratum, const CandidateEntry& candidate) noexcept
{
    const uint64 reach = plan.StratumReach[stratum] * 2;
    if (reach == 0)
    {
        return false;
    }
    for (usize layer = 0; layer < plan.Layers.GetSize(); ++layer)
    {
        if (plan.LayerStrata[layer] != stratum)
        {
            continue;
        }
        const bool isOutranked =
            VisitNeighbors(work, plan, layer, candidate, reach, [&](const CandidateEntry& other) noexcept {
                if ((other.State & STATE_ELIGIBLE) == 0 || other.Key == candidate.Key ||
                    Outranks(other, candidate) == false || AreConflicting(other, candidate) == false)
                {
                    return false;
                }
                // Every conflicting root lies inside the stratum's occupancy region.
                LUDUS_ASSERT((other.State & STATE_OCCUPANCY_DECIDED) != 0);
                return (other.State & STATE_OCCUPIED) == 0;
            });
        if (isOutranked == true)
        {
            return true;
        }
    }
    return false;
}

/// Derive each stratum's decision and occupancy regions for a page, from the last stratum to the first.
bool TryComputeHalo(const CookPlan& plan, const FoliageLatticeRectangle& owned, PageWork& inoutWork) noexcept
{
    const usize strata = plan.StratumReach.GetSize();
    for (usize index = strata; index > 0; --index)
    {
        const usize stratum = index - 1;
        FoliageLatticeRectangle decisions = owned;
        for (usize later = stratum + 1; later < strata; ++later)
        {
            FoliageLatticeRectangle reached;
            if (TryExpand(inoutWork.Occupancy[later], plan.StratumReach[stratum] + plan.StratumReach[later], reached) ==
                false)
            {
                return false;
            }
            decisions = Unite(decisions, reached);
        }
        inoutWork.Decisions[stratum] = decisions;
        if (TryExpand(decisions, plan.StratumReach[stratum] * 2, inoutWork.Occupancy[stratum]) == false)
        {
            return false;
        }
    }
    return true;
}

FoliageCookStatus
TryBuildTables(const CookPlan& plan, PageWork& inoutWork, FoliageCookDiagnostic& outDiagnostic) noexcept
{
    uint64 total = 0;
    for (usize layer = 0; layer < plan.Layers.GetSize(); ++layer)
    {
        const LayerPlan& layerPlan = plan.Layers[layer];
        const FoliageLatticeRectangle& region = inoutWork.Occupancy[plan.LayerStrata[layer]];
        FoliageLatticeRectangle cells =
            Intersect(GetOverlappingCells(region, layerPlan.CellSize), layerPlan.TerrainCells);
        if (IsEmpty(cells) == true)
        {
            cells = {};
        }
        inoutWork.Tables[layer].Cells = cells;
        inoutWork.Tables[layer].First = static_cast<usize>(total);
        inoutWork.Tables[layer].Count = 0;
        const uint64 count = static_cast<uint64>(cells.MaximumX - cells.MinimumX) *
                             static_cast<uint64>(cells.MaximumZ - cells.MinimumZ) * layerPlan.SlotsPerCell;
        if (count > MAX_FOLIAGE_COOK_CANDIDATES || total + count > MAX_FOLIAGE_COOK_CANDIDATES)
        {
            return Fail(outDiagnostic, FoliageCookStatus::LimitExceeded, FoliageCookIssue::CandidateLimit, layer);
        }
        inoutWork.Tables[layer].Count = static_cast<usize>(count);
        total += count;
    }
    if (inoutWork.Entries.TryResize(static_cast<usize>(total)) == false)
    {
        return Fail(outDiagnostic, FoliageCookStatus::AllocationFailed, FoliageCookIssue::Allocation, 0);
    }
    return FoliageCookStatus::Ok;
}

/// Visit every entry of the layers in one stratum.
template <typename Visitor>
void VisitStratumEntries(PageWork& inoutWork, const CookPlan& plan, usize stratum, const Visitor& visitor) noexcept
{
    for (usize layer = 0; layer < plan.Layers.GetSize(); ++layer)
    {
        if (plan.LayerStrata[layer] != stratum)
        {
            continue;
        }
        const LayerTable& table = inoutWork.Tables[layer];
        for (usize i = table.First; i < table.First + table.Count; ++i)
        {
            visitor(inoutWork.Entries[i]);
        }
    }
}

/// Sample every candidate of a stratum, then decide occupancy by earlier strata, then thinning.
FoliageCookStatus EvaluateStratum(const FoliageCookInput& input,
                                  const CookPlan& plan,
                                  usize stratum,
                                  PageWork& inoutWork,
                                  FoliageCookDiagnostic& outDiagnostic) noexcept
{
    for (usize layer = 0; layer < plan.Layers.GetSize(); ++layer)
    {
        if (plan.LayerStrata[layer] != stratum)
        {
            continue;
        }
        const LayerTable& table = inoutWork.Tables[layer];
        const LayerPlan& layerPlan = plan.Layers[layer];
        for (int64 cellZ = table.Cells.MinimumZ; cellZ < table.Cells.MaximumZ; ++cellZ)
        {
            for (int64 cellX = table.Cells.MinimumX; cellX < table.Cells.MaximumX; ++cellX)
            {
                for (uint32 slot = 0; slot < layerPlan.SlotsPerCell; ++slot)
                {
                    const FoliageCandidateKey key
                    {
                        .Layer = layerPlan.Id,
                        .Slot = static_cast<uint16>(slot),
                        .CellX = static_cast<int32>(cellX),
                        .CellZ = static_cast<int32>(cellZ),
                    };
                    CandidateEntry& entry = inoutWork.Entries[GetEntryIndex(table, layerPlan, cellX, cellZ, slot)];
                    const FoliageCookStatus status = EvaluateCandidate(input, plan, layer, key, entry, outDiagnostic);
                    if (status != FoliageCookStatus::Ok)
                    {
                        return status;
                    }
                }
            }
        }
    }
    const FoliageLatticeRectangle occupancy = inoutWork.Occupancy[stratum];
    VisitStratumEntries(inoutWork, plan, stratum, [&](CandidateEntry& inoutEntry) noexcept {
        if ((inoutEntry.State & STATE_ELIGIBLE) == 0 ||
            Contains(occupancy, inoutEntry.RootX, inoutEntry.RootZ) == false)
        {
            return;
        }
        inoutEntry.State |= STATE_OCCUPANCY_DECIDED;
        if (IsOccupied(inoutWork, plan, stratum, inoutEntry) == true)
        {
            inoutEntry.State |= STATE_OCCUPIED;
        }
    });
    const FoliageLatticeRectangle decisions = inoutWork.Decisions[stratum];
    VisitStratumEntries(inoutWork, plan, stratum, [&](CandidateEntry& inoutEntry) noexcept {
        if ((inoutEntry.State & STATE_ELIGIBLE) == 0 ||
            Contains(decisions, inoutEntry.RootX, inoutEntry.RootZ) == false)
        {
            return;
        }
        inoutEntry.State |= STATE_DECIDED;
        if ((inoutEntry.State & STATE_OCCUPIED) == 0 && IsOutranked(inoutWork, plan, stratum, inoutEntry) == false)
        {
            inoutEntry.State |= STATE_ACCEPTED;
        }
    });
    return FoliageCookStatus::Ok;
}

void IncludeBounds(FoliagePageBounds& inoutBounds, const FoliagePageBounds& instance, bool isFirst) noexcept
{
    if (isFirst == true)
    {
        inoutBounds = instance;
        return;
    }
    inoutBounds.MinimumX = GetMinimum(inoutBounds.MinimumX, instance.MinimumX);
    inoutBounds.MinimumZ = GetMinimum(inoutBounds.MinimumZ, instance.MinimumZ);
    inoutBounds.MaximumX = GetMaximum(inoutBounds.MaximumX, instance.MaximumX);
    inoutBounds.MaximumZ = GetMaximum(inoutBounds.MaximumZ, instance.MaximumZ);
    inoutBounds.MinimumHeight =
        inoutBounds.MinimumHeight < instance.MinimumHeight ? inoutBounds.MinimumHeight : instance.MinimumHeight;
    inoutBounds.MaximumHeight =
        inoutBounds.MaximumHeight > instance.MaximumHeight ? inoutBounds.MaximumHeight : instance.MaximumHeight;
}

/// Draw the accepted candidate's yaw and variant and sample its height and up direction.
FoliageCookStatus MakePlacement(const FoliageCookInput& input,
                                const CookPlan& plan,
                                usize layerIndex,
                                const CandidateEntry& entry,
                                FoliagePlacement& outPlacement,
                                FoliagePageBounds& outBounds,
                                FoliageCookDiagnostic& outDiagnostic) noexcept
{
    const LayerPlan& layerPlan = plan.Layers[layerIndex];
    const FoliageCookSpecies& species = input.Species[entry.Species];
    uint32 variant = 0;
    if (math::TrySampleBounded(layerPlan.VariantKey,
                               PackFoliageRandomAddress(entry.Key, FOLIAGE_VARIANT_CHOICE_DIMENSION),
                               species.VariantCount,
                               variant) != math::MathStatus::Success)
    {
        return FailSampling(outDiagnostic, entry.Key);
    }
    const uint32 yaw =
        math::SampleUInt32(layerPlan.TransformKey, PackFoliageRandomAddress(entry.Key, FOLIAGE_YAW_DIMENSION)) >>
        YAW_SHIFT;
    const RootSample sample = internal::SampleRoot(plan.Sampler, entry.RootX, entry.RootZ);
    FoliagePlacement placement;
    placement.Layer = entry.Key.Layer;
    placement.Slot = entry.Key.Slot;
    placement.CellX = entry.Key.CellX;
    placement.CellZ = entry.Key.CellZ;
    placement.OffsetX = static_cast<uint16>(entry.RootX - int64{entry.Key.CellX} * layerPlan.CellSize);
    placement.OffsetZ = static_cast<uint16>(entry.RootZ - int64{entry.Key.CellZ} * layerPlan.CellSize);
    placement.Height = sample.Height;
    placement.Yaw = static_cast<uint16>(yaw);
    placement.Scale = entry.Scale;
    if (species.IsAlignedToTerrain == true)
    {
        const int64 spacing = plan.Sampler.Spacing;
        placement.NormalX =
            internal::ComputeNormalComponent(sample.GradientX, sample.GradientX, sample.GradientZ, spacing);
        placement.NormalZ =
            internal::ComputeNormalComponent(sample.GradientZ, sample.GradientX, sample.GradientZ, spacing);
    }
    placement.Species = entry.Species;
    placement.Variant = static_cast<uint16>(variant);
    // Radii of at most 2^16 at scales below 16 and heights below 2^29 keep every bound inside int32.
    const int64 horizontal = static_cast<int64>(internal::ScaleRadius(species.BoundsRadius, entry.Scale));
    const int32 vertical = static_cast<int32>(internal::ScaleRadius(species.BoundsHeight, entry.Scale));
    outBounds =
    {
        .MinimumX = entry.RootX - horizontal,
        .MinimumZ = entry.RootZ - horizontal,
        .MaximumX = entry.RootX + horizontal,
        .MaximumZ = entry.RootZ + horizontal,
        .MinimumHeight = sample.Height - vertical,
        .MaximumHeight = sample.Height + vertical,
    };
    outPlacement = placement;
    return FoliageCookStatus::Ok;
}

/// Cook one page and append its directory entry and placements.
FoliageCookStatus CookPage(const FoliageCookInput& input,
                           const CookPlan& plan,
                           FoliagePageAddress page,
                           PageWork& inoutWork,
                           CookedPagesBuilder& inoutBuilder,
                           FoliageCookDiagnostic& outDiagnostic) noexcept
{
    const FoliageLatticeRectangle owned = GetPageSquare(page, input.Domain.PageSize);
    FoliagePage record;
    record.PageX = page.X;
    record.PageZ = page.Z;
    record.FirstPlacement = static_cast<uint32>(inoutBuilder.Placements.GetSize());
    uint32 activeSuppressions = 0;
    if (IsEmpty(Intersect(owned, plan.Sampler.Extent)) == false)
    {
        if (TryComputeHalo(plan, owned, inoutWork) == false)
        {
            return Fail(outDiagnostic,
                        FoliageCookStatus::CoordinateOutOfRange,
                        FoliageCookIssue::TerrainCoordinates,
                        0);
        }
        const FoliageCookStatus tableStatus = TryBuildTables(plan, inoutWork, outDiagnostic);
        if (tableStatus != FoliageCookStatus::Ok)
        {
            return tableStatus;
        }
        for (usize stratum = 0; stratum < plan.StratumReach.GetSize(); ++stratum)
        {
            const FoliageCookStatus status = EvaluateStratum(input, plan, stratum, inoutWork, outDiagnostic);
            if (status != FoliageCookStatus::Ok)
            {
                return status;
            }
        }
        for (usize layer = 0; layer < plan.Layers.GetSize(); ++layer)
        {
            const LayerPlan& layerPlan = plan.Layers[layer];
            const LayerTable& table = inoutWork.Tables[layer];
            const FoliageLatticeRectangle cells =
                Intersect(GetOverlappingCells(owned, layerPlan.CellSize), table.Cells);
            for (int64 cellZ = cells.MinimumZ; cellZ < cells.MaximumZ; ++cellZ)
            {
                for (int64 cellX = cells.MinimumX; cellX < cells.MaximumX; ++cellX)
                {
                    for (uint32 slot = 0; slot < layerPlan.SlotsPerCell; ++slot)
                    {
                        const CandidateEntry& entry =
                            inoutWork.Entries[GetEntryIndex(table, layerPlan, cellX, cellZ, slot)];
                        if ((entry.State & STATE_SUPPRESSED) != 0)
                        {
                            ++activeSuppressions;
                        }
                        if ((entry.State & STATE_ACCEPTED) == 0)
                        {
                            continue;
                        }
                        FoliagePlacement placement;
                        FoliagePageBounds bounds;
                        const FoliageCookStatus status =
                            MakePlacement(input, plan, layer, entry, placement, bounds, outDiagnostic);
                        if (status != FoliageCookStatus::Ok)
                        {
                            return status;
                        }
                        if (inoutBuilder.Placements.TryAdd(placement) == false)
                        {
                            return Fail(outDiagnostic,
                                        FoliageCookStatus::AllocationFailed,
                                        FoliageCookIssue::Allocation,
                                        0);
                        }
                        IncludeBounds(record.Bounds, bounds, record.PlacementCount == 0);
                        ++record.PlacementCount;
                    }
                }
            }
        }
    }
    if (inoutBuilder.Pages.TryAdd(record) == false ||
        inoutBuilder.ActiveSuppressions.TryAdd(activeSuppressions) == false)
    {
        return Fail(outDiagnostic, FoliageCookStatus::AllocationFailed, FoliageCookIssue::Allocation, 0);
    }
    return FoliageCookStatus::Ok;
}

bool TryAllocateWork(const CookPlan& plan, PageWork& outWork) noexcept
{
    const usize strata = plan.StratumReach.GetSize();
    return outWork.Tables.TryResize(plan.Layers.GetSize()) == true && outWork.Decisions.TryResize(strata) == true &&
           outWork.Occupancy.TryResize(strata) == true;
}

/// Pages overlapping a nonempty lattice rectangle, in (Z, X) order.
FoliageCookStatus TryListPages(const FoliageLatticeRectangle& rectangle,
                               uint32 pageSize,
                               Array<FoliagePageAddress>& outPages,
                               FoliageCookDiagnostic& outDiagnostic) noexcept
{
    Array<FoliagePageAddress> pages;
    if (IsEmpty(rectangle) == false)
    {
        const FoliageLatticeRectangle range = GetOverlappingCells(rectangle, pageSize);
        const uint64 count =
            static_cast<uint64>(range.MaximumX - range.MinimumX) * static_cast<uint64>(range.MaximumZ - range.MinimumZ);
        if (count > MAX_FOLIAGE_COOK_PAGES)
        {
            return Fail(outDiagnostic, FoliageCookStatus::LimitExceeded, FoliageCookIssue::PageRequest, 0);
        }
        if (pages.TryEnsureCapacity(static_cast<usize>(count)) == false)
        {
            return Fail(outDiagnostic, FoliageCookStatus::AllocationFailed, FoliageCookIssue::Allocation, 0);
        }
        // The validated terrain extent keeps every page overlapping it inside int32.
        for (int64 pageZ = range.MinimumZ; pageZ < range.MaximumZ; ++pageZ)
        {
            for (int64 pageX = range.MinimumX; pageX < range.MaximumX; ++pageX)
            {
                [[maybe_unused]] const bool isAdded =
                    pages.TryAdd(FoliagePageAddress{ .X = static_cast<int32>(pageX), .Z = static_cast<int32>(pageZ) });
                LUDUS_ASSERT(isAdded == true);
            }
        }
    }
    outPages = foundation::Move(pages);
    return FoliageCookStatus::Ok;
}

FoliageCookStatus ValidatePageRequest(BorrowedView<const FoliagePageAddress> pages,
                                      FoliageCookDiagnostic& outDiagnostic) noexcept
{
    if (pages.IsValid() == false || pages.IsEmpty() == true || pages.GetSize() > MAX_FOLIAGE_COOK_PAGES)
    {
        return Fail(outDiagnostic, FoliageCookStatus::InvalidArgument, FoliageCookIssue::PageRequest, 0);
    }
    Array<FoliagePageAddress> sorted;
    if (sorted.TryAddRange(pages) == false)
    {
        return Fail(outDiagnostic, FoliageCookStatus::AllocationFailed, FoliageCookIssue::Allocation, 0);
    }
    foundation::Sort(sorted.GetData(), sorted.GetSize(), IsPageBefore);
    for (usize i = 1; i < sorted.GetSize(); ++i)
    {
        if (sorted[i] == sorted[i - 1])
        {
            return Fail(outDiagnostic, FoliageCookStatus::InvalidArgument, FoliageCookIssue::PageRequest, i);
        }
    }
    return FoliageCookStatus::Ok;
}
} // namespace

FoliageCookStatus TryGetFoliageTerrainPages(const FoliageCookInput& input,
                                            Array<FoliagePageAddress>& outPages,
                                            FoliageCookDiagnostic& outDiagnostic) noexcept
{
    TerrainSampler sampler;
    const FoliageCookStatus status = internal::ValidateInput(input, sampler, outDiagnostic);
    if (status != FoliageCookStatus::Ok)
    {
        return status;
    }
    return TryListPages(sampler.Extent, input.Domain.PageSize, outPages, outDiagnostic);
}

FoliageCookStatus TryCookFoliagePages(const FoliageCookInput& input,
                                      BorrowedView<const FoliagePageAddress> pages,
                                      FoliageCookedPages& outPages,
                                      FoliageCookDiagnostic& outDiagnostic) noexcept
{
    CookPlan plan;
    FoliageCookStatus status = TryPreparePlan(input, plan, outDiagnostic);
    if (status != FoliageCookStatus::Ok)
    {
        return status;
    }
    status = ValidatePageRequest(pages, outDiagnostic);
    if (status != FoliageCookStatus::Ok)
    {
        return status;
    }
    PageWork work;
    CookedPagesBuilder builder;
    if (TryAllocateWork(plan, work) == false || builder.Pages.TryEnsureCapacity(pages.GetSize()) == false)
    {
        return Fail(outDiagnostic, FoliageCookStatus::AllocationFailed, FoliageCookIssue::Allocation, 0);
    }
    for (const FoliagePageAddress& page : pages)
    {
        status = CookPage(input, plan, page, work, builder, outDiagnostic);
        if (status != FoliageCookStatus::Ok)
        {
            return status;
        }
    }
    outPages.mPages = foundation::Move(builder.Pages);
    outPages.mPlacements = foundation::Move(builder.Placements);
    outPages.mActiveSuppressions = foundation::Move(builder.ActiveSuppressions);
    return FoliageCookStatus::Ok;
}

usize FoliageCookedPages::GetActiveSuppressionCount() const noexcept
{
    usize count = 0;
    for (const uint32 suppressions : mActiveSuppressions)
    {
        count += suppressions;
    }
    return count;
}

FoliageCookStatus TryMergeFoliageCookedPages(const FoliageCookedPages& source,
                                             FoliageCookedPages& inoutDestination,
                                             FoliageCookDiagnostic& outDiagnostic) noexcept
{
    outDiagnostic = {};
    CookedPagesBuilder builder;
    const auto append = [&builder](const FoliageCookedPages& pages, usize index) noexcept {
        FoliagePage copy = pages.mPages[index];
        const BorrowedView<const FoliagePlacement> placements{
            pages.mPlacements.GetData() + copy.FirstPlacement,
            copy.PlacementCount,
        };
        copy.FirstPlacement = static_cast<uint32>(builder.Placements.GetSize());
        return builder.Pages.TryAdd(copy) == true && builder.Placements.TryAddRange(placements) == true &&
               builder.ActiveSuppressions.TryAdd(pages.mActiveSuppressions[index]) == true;
    };
    for (usize i = 0; i < inoutDestination.mPages.GetSize(); ++i)
    {
        const FoliagePage& page = inoutDestination.mPages[i];
        bool isReplaced = false;
        for (const FoliagePage& replacement : source.mPages)
        {
            isReplaced = isReplaced == true || (replacement.PageX == page.PageX && replacement.PageZ == page.PageZ);
        }
        if (isReplaced == false && append(inoutDestination, i) == false)
        {
            return Fail(outDiagnostic, FoliageCookStatus::AllocationFailed, FoliageCookIssue::Allocation, 0);
        }
    }
    for (usize i = 0; i < source.mPages.GetSize(); ++i)
    {
        if (append(source, i) == false)
        {
            return Fail(outDiagnostic, FoliageCookStatus::AllocationFailed, FoliageCookIssue::Allocation, 0);
        }
    }
    if (builder.Pages.GetSize() > MAX_FOLIAGE_COOK_PAGES || builder.Placements.GetSize() > format::MAX_PLACEMENTS)
    {
        return Fail(outDiagnostic, FoliageCookStatus::LimitExceeded, FoliageCookIssue::PageRequest, 0);
    }
    inoutDestination.mPages = foundation::Move(builder.Pages);
    inoutDestination.mPlacements = foundation::Move(builder.Placements);
    inoutDestination.mActiveSuppressions = foundation::Move(builder.ActiveSuppressions);
    return FoliageCookStatus::Ok;
}

FoliageCookStatus TryGetFoliageSampleEditRoots(const FoliageCookInput& input,
                                               terrain::TerrainSampleAddress firstSample,
                                               terrain::TerrainSampleAddress lastSample,
                                               FoliageLatticeRectangle& outRoots,
                                               FoliageCookDiagnostic& outDiagnostic) noexcept
{
    TerrainSampler sampler;
    const FoliageCookStatus status = internal::ValidateInput(input, sampler, outDiagnostic);
    if (status != FoliageCookStatus::Ok)
    {
        return status;
    }
    if (firstSample.X > lastSample.X || firstSample.Z > lastSample.Z)
    {
        return Fail(outDiagnostic, FoliageCookStatus::InvalidArgument, FoliageCookIssue::None, 0);
    }
    // A root reads the corners of its cell, so cells [first - 1, last] touch the edited samples.
    const int64 latticeUnits = format::LATTICE_UNITS_PER_TERRAIN_SAMPLE;
    const auto toLattice = [&](int64 sample, int64 origin, int64 shift, int64& outLattice) noexcept {
        int64 offset = 0;
        return foundation::TrySubtract(sample, origin, offset) == true &&
               foundation::TryAdd(offset, shift, offset) == true &&
               foundation::TryMultiply(offset, latticeUnits, outLattice) == true;
    };
    FoliageLatticeRectangle roots;
    if (toLattice(firstSample.X, input.Domain.OriginX, -1, roots.MinimumX) == false ||
        toLattice(firstSample.Z, input.Domain.OriginZ, -1, roots.MinimumZ) == false ||
        toLattice(lastSample.X, input.Domain.OriginX, 1, roots.MaximumX) == false ||
        toLattice(lastSample.Z, input.Domain.OriginZ, 1, roots.MaximumZ) == false)
    {
        return Fail(outDiagnostic, FoliageCookStatus::CoordinateOutOfRange, FoliageCookIssue::TerrainCoordinates, 0);
    }
    outRoots = roots;
    return FoliageCookStatus::Ok;
}

FoliageCookStatus TryGetFoliageCandidateRoots(const FoliageCookInput& input,
                                              const FoliageCandidateKey& key,
                                              FoliageLatticeRectangle& outRoots,
                                              FoliageCookDiagnostic& outDiagnostic) noexcept
{
    TerrainSampler sampler;
    const FoliageCookStatus status = internal::ValidateInput(input, sampler, outDiagnostic);
    if (status != FoliageCookStatus::Ok)
    {
        return status;
    }
    for (const FoliageCookLayer& layer : input.Layers)
    {
        if (layer.Id == key.Layer)
        {
            const int64 cellSize = layer.CellSize;
            outRoots =
            {
                .MinimumX = int64{key.CellX} * cellSize,
                .MinimumZ = int64{key.CellZ} * cellSize,
                .MaximumX = (int64{key.CellX} + 1) * cellSize,
                .MaximumZ = (int64{key.CellZ} + 1) * cellSize,
            };
            return FoliageCookStatus::Ok;
        }
    }
    return Fail(outDiagnostic, FoliageCookStatus::InvalidArgument, FoliageCookIssue::None, 0);
}

FoliageCookStatus TryGetFoliageDirtyPages(const FoliageCookInput& input,
                                          const FoliageLatticeRectangle& changedRoots,
                                          Array<FoliagePageAddress>& outPages,
                                          FoliageCookDiagnostic& outDiagnostic) noexcept
{
    CookPlan plan;
    const FoliageCookStatus status = TryPreparePlan(input, plan, outDiagnostic);
    if (status != FoliageCookStatus::Ok)
    {
        return status;
    }
    if (IsEmpty(changedRoots) == true)
    {
        return Fail(outDiagnostic, FoliageCookStatus::InvalidArgument, FoliageCookIssue::None, 0);
    }
    // A changed root changes its own occupancy input, which reaches acceptance within the thinning reach and,
    // through acceptance, the occupancy of every later stratum within the cross-stratum reach.
    const usize strata = plan.StratumReach.GetSize();
    Array<FoliageLatticeRectangle> accepted;
    if (accepted.TryResize(strata) == false)
    {
        return Fail(outDiagnostic, FoliageCookStatus::AllocationFailed, FoliageCookIssue::Allocation, 0);
    }
    FoliageLatticeRectangle dirty;
    for (usize stratum = 0; stratum < strata; ++stratum)
    {
        FoliageLatticeRectangle occupancy = changedRoots;
        for (usize earlier = 0; earlier < stratum; ++earlier)
        {
            FoliageLatticeRectangle reached;
            if (TryExpand(accepted[earlier], plan.StratumReach[stratum] + plan.StratumReach[earlier], reached) == false)
            {
                return Fail(outDiagnostic,
                            FoliageCookStatus::CoordinateOutOfRange,
                            FoliageCookIssue::TerrainCoordinates,
                            0);
            }
            occupancy = Unite(occupancy, reached);
        }
        if (TryExpand(occupancy, plan.StratumReach[stratum] * 2, accepted[stratum]) == false)
        {
            return Fail(outDiagnostic,
                        FoliageCookStatus::CoordinateOutOfRange,
                        FoliageCookIssue::TerrainCoordinates,
                        0);
        }
        dirty = Unite(dirty, accepted[stratum]);
    }
    return TryListPages(Intersect(dirty, plan.Sampler.Extent), input.Domain.PageSize, outPages, outDiagnostic);
}
} // namespace ludus::content::foliage_cook
