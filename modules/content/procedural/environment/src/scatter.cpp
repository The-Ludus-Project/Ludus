// Serial reference scatter kernel. Candidates are regenerated from their identity
// whenever a neighbour needs them, so no cell depends on generation order or on
// shared mutable state.
//
// Thanks to Josh Klint, "Vegetation Management in Leadwerks Game Engine 4", Game
// Engine Gems 3, chapter 4, pp. 53-71, for compact placement regenerated for
// rendering and physics; no chapter code is copied. Review:
// docs/architecture/procedural-content-generation-gems-review.md#vegetation-placement-and-collision. The fixed
// candidate set per cell with priority thinning is the Ludus local rule recorded in
// docs/architecture/procedural-content-generation.md#vegetation-and-scatter;
// it trades maximal Poisson-disk coverage for finite, order-independent support.

#include <ludus/content/procedural/environment/scatter.h>

#include <ludus/foundation/math/random_domains.hpp>

namespace ludus::content::procedural
{
namespace
{
using foundation::core::int64;
using foundation::math::MathStatus;
using foundation::math::PreparedBound32;
using foundation::math::RandomAddress;

/// Event of every base-generation sample; later events belong to world mutations such as regrowth.
constexpr uint32 BASE_GENERATION_EVENT = 0;
/// Placement-domain dimensions.
constexpr uint16 PLACEMENT_OFFSET_X_DIMENSION = 0;
constexpr uint16 PLACEMENT_OFFSET_Y_DIMENSION = 1;
constexpr uint16 PLACEMENT_PRIORITY_DIMENSION = 2;
/// Variant-domain dimension.
constexpr uint16 VARIANT_CHOICE_DIMENSION = 0;
/// Transform-domain dimensions.
constexpr uint16 TRANSFORM_ROTATION_DIMENSION = 0;
constexpr uint16 TRANSFORM_SCALE_DIMENSION = 1;
/// Shift that keeps the high 16 bits of a raw word as a rotation in 1/65536 turns.
constexpr uint32 ROTATION_SHIFT = 16;
constexpr int64 INT16_FIRST = -32768;
constexpr int64 INT16_LAST = 32767;

/// Bounds prepared once per layer so candidate evaluation does not divide.
struct LayerBounds final
{
    PreparedBound32 Cell;
    PreparedBound32 Variant;
    PreparedBound32 Scale;
};

/// One candidate's sampled state; details are filled only for eligible candidates.
struct Candidate final
{
    uint64 Scope = 0;
    int32 LatticeX = 0;
    int32 LatticeY = 0;
    bool IsEligible = false;
    uint32 Priority = 0;
    uint32 Variant = 0;
    uint32 ExclusionRadius = 0;
};

[[nodiscard]] int64 FloorDivide(int64 value, int64 divisor) noexcept
{
    int64 quotient = value / divisor;
    if (value % divisor != 0 && value < 0)
    {
        --quotient;
    }
    return quotient;
}

[[nodiscard]] bool IsEmpty(const ScatterRectangle& rectangle) noexcept
{
    return rectangle.MinimumX >= rectangle.MaximumX || rectangle.MinimumY >= rectangle.MaximumY;
}

[[nodiscard]] bool Contains(const ScatterRectangle& rectangle, int32 x, int32 y) noexcept
{
    return x >= rectangle.MinimumX && x < rectangle.MaximumX && y >= rectangle.MinimumY && y < rectangle.MaximumY;
}

/// Cells overlapping the region, half-open, kept in int64 so callers can range-check before narrowing.
struct LayerCellBounds final
{
    int64 MinimumX = 0;
    int64 MinimumY = 0;
    int64 MaximumX = 0;
    int64 MaximumY = 0;
};

[[nodiscard]] LayerCellBounds ComputeLayerCells(const ScatterRectangle& region, uint16 cellSize) noexcept
{
    LayerCellBounds bounds;
    bounds.MinimumX = FloorDivide(region.MinimumX, cellSize);
    bounds.MinimumY = FloorDivide(region.MinimumY, cellSize);
    bounds.MaximumX = FloorDivide(static_cast<int64>(region.MaximumX) - 1, cellSize) + 1;
    bounds.MaximumY = FloorDivide(static_cast<int64>(region.MaximumY) - 1, cellSize) + 1;
    return bounds;
}

[[nodiscard]] ScatterValidation MakeIssue(ScatterRecipeIssue issue, usize index) noexcept
{
    return {ScatterStatus::InvalidRecipe, issue, index};
}

[[nodiscard]] ScatterValidation ValidateLayer(const ScatterRecipe& recipe, usize index) noexcept
{
    const ScatterLayer& layer = recipe.Layers[index];
    for (usize earlier = 0; earlier < index; ++earlier)
    {
        if (recipe.Layers[earlier].Id == layer.Id)
        {
            return MakeIssue(ScatterRecipeIssue::DuplicateLayer, index);
        }
    }
    if (layer.CellSize == 0)
    {
        return MakeIssue(ScatterRecipeIssue::CellSize, index);
    }
    const LayerCellBounds cells = ComputeLayerCells(recipe.Region, layer.CellSize);
    // The last cell must itself fit int16; maximum is one past it.
    if (cells.MinimumX < INT16_FIRST || cells.MinimumY < INT16_FIRST || cells.MaximumX - 1 > INT16_LAST ||
        cells.MaximumY - 1 > INT16_LAST)
    {
        return MakeIssue(ScatterRecipeIssue::CellRange, index);
    }
    if (layer.CandidatesPerCell == 0 || layer.CandidatesPerCell > MAX_SCATTER_CANDIDATES_PER_CELL)
    {
        return MakeIssue(ScatterRecipeIssue::CandidateCount, index);
    }
    if (layer.Variants.IsValid() == false || layer.Variants.IsEmpty() == true ||
        layer.Variants.GetSize() > MAX_SCATTER_VARIANTS)
    {
        return MakeIssue(ScatterRecipeIssue::VariantCount, index);
    }
    for (usize variant = 0; variant < layer.Variants.GetSize(); ++variant)
    {
        if (layer.Variants[variant].ExclusionRadius > layer.CellSize)
        {
            return MakeIssue(ScatterRecipeIssue::ExclusionRadius, index);
        }
    }
    if (layer.MaximumScale < layer.MinimumScale)
    {
        return MakeIssue(ScatterRecipeIssue::ScaleRange, index);
    }
    return {};
}

[[nodiscard]] ScatterStatus ValidateRequest(const ScatterRecipe& recipe,
                                            usize layerIndex,
                                            ScatterCellRange cells,
                                            ScatterCellRange& outLayerCells) noexcept
{
    const ScatterStatus status = TryGetScatterCells(recipe, layerIndex, outLayerCells);
    if (status != ScatterStatus::Success)
    {
        return status;
    }
    if (cells.MinimumX >= cells.MaximumX || cells.MinimumY >= cells.MaximumY ||
        cells.MinimumX < outLayerCells.MinimumX || cells.MinimumY < outLayerCells.MinimumY ||
        cells.MaximumX > outLayerCells.MaximumX || cells.MaximumY > outLayerCells.MaximumY)
    {
        return ScatterStatus::InvalidRange;
    }
    return ScatterStatus::Success;
}

[[nodiscard]] bool Prepare(const ScatterLayer& layer, LayerBounds& outBounds) noexcept
{
    const uint32 scaleWidth = static_cast<uint32>(layer.MaximumScale) - layer.MinimumScale + 1u;
    return TryPrepareBound32(layer.CellSize, outBounds.Cell) == MathStatus::Success &&
           TryPrepareBound32(static_cast<uint32>(layer.Variants.GetSize()), outBounds.Variant) == MathStatus::Success &&
           TryPrepareBound32(scaleWidth, outBounds.Scale) == MathStatus::Success;
}

/// Sample a candidate's position and eligibility. Priority and variant follow only when it is eligible.
[[nodiscard]] ScatterStatus EvaluateCandidate(const ScatterRecipe& recipe,
                                              const ScatterKeys& keys,
                                              const ScatterLayer& layer,
                                              const LayerBounds& bounds,
                                              ScatterIdentity identity,
                                              Candidate& outCandidate) noexcept
{
    Candidate candidate;
    candidate.Scope = PackScatterScope(identity);
    uint32 offsetX = 0;
    uint32 offsetY = 0;
    if (TrySampleBounded(keys.Placement,
                         RandomAddress{candidate.Scope, BASE_GENERATION_EVENT, PLACEMENT_OFFSET_X_DIMENSION},
                         bounds.Cell,
                         offsetX) != MathStatus::Success ||
        TrySampleBounded(keys.Placement,
                         RandomAddress{candidate.Scope, BASE_GENERATION_EVENT, PLACEMENT_OFFSET_Y_DIMENSION},
                         bounds.Cell,
                         offsetY) != MathStatus::Success)
    {
        return ScatterStatus::SamplingExhausted;
    }
    // Validation keeps cells within int16 and the cell size within uint16, so positions fit int32.
    candidate.LatticeX = static_cast<int32>(static_cast<int64>(identity.CellX) * layer.CellSize + offsetX);
    candidate.LatticeY = static_cast<int32>(static_cast<int64>(identity.CellY) * layer.CellSize + offsetY);
    candidate.IsEligible = Contains(recipe.Region, candidate.LatticeX, candidate.LatticeY);
    for (usize i = 0; i < recipe.KeepOuts.GetSize() && candidate.IsEligible == true; ++i)
    {
        candidate.IsEligible = Contains(recipe.KeepOuts[i], candidate.LatticeX, candidate.LatticeY) == false;
    }
    if (candidate.IsEligible == true)
    {
        candidate.Priority =
            SampleUInt32(keys.Placement,
                         RandomAddress{candidate.Scope, BASE_GENERATION_EVENT, PLACEMENT_PRIORITY_DIMENSION});
        if (TrySampleBounded(keys.Variant,
                             RandomAddress{candidate.Scope, BASE_GENERATION_EVENT, VARIANT_CHOICE_DIMENSION},
                             bounds.Variant,
                             candidate.Variant) != MathStatus::Success)
        {
            return ScatterStatus::SamplingExhausted;
        }
        candidate.ExclusionRadius = layer.Variants[candidate.Variant].ExclusionRadius;
    }
    outCandidate = candidate;
    return ScatterStatus::Success;
}

/// Whether `other` outranks `candidate`: a higher sampled priority, then a higher packed identity.
[[nodiscard]] bool Outranks(const Candidate& other, const Candidate& candidate) noexcept
{
    return other.Priority > candidate.Priority ||
           (other.Priority == candidate.Priority && other.Scope > candidate.Scope);
}

[[nodiscard]] bool AreConflicting(const Candidate& a, const Candidate& b) noexcept
{
    const int64 dx = static_cast<int64>(a.LatticeX) - b.LatticeX;
    const int64 dy = static_cast<int64>(a.LatticeY) - b.LatticeY;
    const uint64 reach = static_cast<uint64>(a.ExclusionRadius) + b.ExclusionRadius;
    // Neighbours lie within SCATTER_NEIGHBOR_CELLS + 1 cells of 65535 units, so the squares fit 64 bits.
    return static_cast<uint64>(dx * dx + dy * dy) < reach * reach;
}

/// Accept an eligible candidate unless an eligible, conflicting neighbour outranks it.
[[nodiscard]] ScatterStatus DecideAcceptance(const ScatterRecipe& recipe,
                                             const ScatterKeys& keys,
                                             const ScatterLayer& layer,
                                             const LayerBounds& bounds,
                                             ScatterCellRange layerCells,
                                             ScatterIdentity identity,
                                             const Candidate& candidate,
                                             bool& outIsAccepted) noexcept
{
    // Cells outside the layer's cells lie wholly outside the region, so their candidates are never eligible.
    const int32 firstX = identity.CellX - SCATTER_NEIGHBOR_CELLS < layerCells.MinimumX
                             ? layerCells.MinimumX
                             : identity.CellX - SCATTER_NEIGHBOR_CELLS;
    const int32 firstY = identity.CellY - SCATTER_NEIGHBOR_CELLS < layerCells.MinimumY
                             ? layerCells.MinimumY
                             : identity.CellY - SCATTER_NEIGHBOR_CELLS;
    const int32 endX = identity.CellX + SCATTER_NEIGHBOR_CELLS + 1 > layerCells.MaximumX
                           ? layerCells.MaximumX
                           : identity.CellX + SCATTER_NEIGHBOR_CELLS + 1;
    const int32 endY = identity.CellY + SCATTER_NEIGHBOR_CELLS + 1 > layerCells.MaximumY
                           ? layerCells.MaximumY
                           : identity.CellY + SCATTER_NEIGHBOR_CELLS + 1;
    for (int32 y = firstY; y < endY; ++y)
    {
        for (int32 x = firstX; x < endX; ++x)
        {
            for (uint16 k = 0; k < layer.CandidatesPerCell; ++k)
            {
                ScatterIdentity neighborIdentity = identity;
                neighborIdentity.CellX = static_cast<int16>(x);
                neighborIdentity.CellY = static_cast<int16>(y);
                neighborIdentity.Candidate = static_cast<uint8>(k);
                if (neighborIdentity == identity)
                {
                    continue;
                }
                Candidate neighbor;
                const ScatterStatus status = EvaluateCandidate(recipe, keys, layer, bounds, neighborIdentity, neighbor);
                if (status != ScatterStatus::Success)
                {
                    return status;
                }
                if (neighbor.IsEligible == true && Outranks(neighbor, candidate) == true &&
                    AreConflicting(neighbor, candidate) == true)
                {
                    outIsAccepted = false;
                    return ScatterStatus::Success;
                }
            }
        }
    }
    outIsAccepted = true;
    return ScatterStatus::Success;
}
} // namespace

ScatterValidation ValidateScatterRecipe(const ScatterRecipe& recipe) noexcept
{
    if (recipe.Room == 0)
    {
        return MakeIssue(ScatterRecipeIssue::RoomZero, 0);
    }
    if (IsEmpty(recipe.Region) == true)
    {
        return MakeIssue(ScatterRecipeIssue::EmptyRegion, 0);
    }
    if (recipe.KeepOuts.IsValid() == false || recipe.KeepOuts.GetSize() > MAX_SCATTER_KEEP_OUTS)
    {
        return MakeIssue(ScatterRecipeIssue::KeepOutCount, 0);
    }
    for (usize i = 0; i < recipe.KeepOuts.GetSize(); ++i)
    {
        if (IsEmpty(recipe.KeepOuts[i]) == true)
        {
            return MakeIssue(ScatterRecipeIssue::EmptyKeepOut, i);
        }
    }
    if (recipe.Layers.IsValid() == false || recipe.Layers.IsEmpty() == true ||
        recipe.Layers.GetSize() > MAX_SCATTER_LAYERS)
    {
        return MakeIssue(ScatterRecipeIssue::LayerCount, 0);
    }
    for (usize i = 0; i < recipe.Layers.GetSize(); ++i)
    {
        const ScatterValidation validation = ValidateLayer(recipe, i);
        if (validation.Status != ScatterStatus::Success)
        {
            return validation;
        }
    }
    return {};
}

ScatterStatus TryMakeScatterKeys(uint64 worldSeed, ScatterDomains domains, ScatterKeys& outKeys) noexcept
{
    const foundation::math::RandomDomain manifest[] = {
        {domains.Placement, "scatter placement"},
        {domains.Variant, "scatter variant"},
        {domains.Transform, "scatter transform"},
    };
    if (CheckRandomDomains(manifest, foundation::math::RandomDomainOwner::Game).Status != MathStatus::Success)
    {
        return ScatterStatus::InvalidRecipe;
    }
    ScatterKeys keys;
    if (TryMakeRandomKey(worldSeed, domains.Placement, keys.Placement) != MathStatus::Success ||
        TryMakeRandomKey(worldSeed, domains.Variant, keys.Variant) != MathStatus::Success ||
        TryMakeRandomKey(worldSeed, domains.Transform, keys.Transform) != MathStatus::Success)
    {
        return ScatterStatus::InvalidRecipe;
    }
    outKeys = keys;
    return ScatterStatus::Success;
}

ScatterStatus TryGetScatterCells(const ScatterRecipe& recipe, usize layerIndex, ScatterCellRange& outCells) noexcept
{
    if (ValidateScatterRecipe(recipe).Status != ScatterStatus::Success || layerIndex >= recipe.Layers.GetSize())
    {
        return ScatterStatus::InvalidRecipe;
    }
    const LayerCellBounds cells = ComputeLayerCells(recipe.Region, recipe.Layers[layerIndex].CellSize);
    outCells = {static_cast<int32>(cells.MinimumX),
                static_cast<int32>(cells.MinimumY),
                static_cast<int32>(cells.MaximumX),
                static_cast<int32>(cells.MaximumY)};
    return ScatterStatus::Success;
}

ScatterStatus TryCountScatterCandidates(const ScatterRecipe& recipe,
                                        usize layerIndex,
                                        ScatterCellRange cells,
                                        usize& outCount) noexcept
{
    ScatterCellRange layerCells;
    const ScatterStatus status = ValidateRequest(recipe, layerIndex, cells, layerCells);
    if (status != ScatterStatus::Success)
    {
        return status;
    }
    // int16 cells bound the count to 2^16 x 2^16 x 64 = 2^38; 32-bit browser targets check it against usize.
    const uint64 count = static_cast<uint64>(cells.MaximumX - cells.MinimumX) *
                         static_cast<uint64>(cells.MaximumY - cells.MinimumY) *
                         recipe.Layers[layerIndex].CandidatesPerCell;
    if (count > static_cast<uint64>(static_cast<usize>(-1)))
    {
        return ScatterStatus::CapacityExceeded;
    }
    outCount = static_cast<usize>(count);
    return ScatterStatus::Success;
}

ScatterStatus TryGenerateScatter(const ScatterRecipe& recipe,
                                 const ScatterKeys& keys,
                                 usize layerIndex,
                                 ScatterCellRange cells,
                                 foundation::BorrowedView<ScatterPlacement> storage,
                                 usize& outCount) noexcept
{
    usize capacity = 0;
    const ScatterStatus countStatus = TryCountScatterCandidates(recipe, layerIndex, cells, capacity);
    if (countStatus != ScatterStatus::Success)
    {
        return countStatus;
    }
    if (storage.IsValid() == false || storage.GetSize() < capacity)
    {
        return ScatterStatus::CapacityExceeded;
    }
    ScatterCellRange layerCells;
    static_cast<void>(TryGetScatterCells(recipe, layerIndex, layerCells));
    const ScatterLayer& layer = recipe.Layers[layerIndex];
    LayerBounds bounds;
    if (Prepare(layer, bounds) == false)
    {
        return ScatterStatus::InvalidRecipe;
    }
    usize count = 0;
    for (int32 y = cells.MinimumY; y < cells.MaximumY; ++y)
    {
        for (int32 x = cells.MinimumX; x < cells.MaximumX; ++x)
        {
            for (uint16 k = 0; k < layer.CandidatesPerCell; ++k)
            {
                ScatterIdentity identity;
                identity.Room = recipe.Room;
                identity.Layer = layer.Id;
                identity.Candidate = static_cast<uint8>(k);
                identity.CellX = static_cast<int16>(x);
                identity.CellY = static_cast<int16>(y);
                Candidate candidate;
                ScatterStatus status = EvaluateCandidate(recipe, keys, layer, bounds, identity, candidate);
                if (status != ScatterStatus::Success)
                {
                    return status;
                }
                if (candidate.IsEligible == false)
                {
                    continue;
                }
                bool isAccepted = false;
                status = DecideAcceptance(recipe, keys, layer, bounds, layerCells, identity, candidate, isAccepted);
                if (status != ScatterStatus::Success)
                {
                    return status;
                }
                if (isAccepted == false)
                {
                    continue;
                }
                // Transforms are drawn after acceptance and never read by it.
                ScatterPlacement placement;
                placement.Identity = identity;
                placement.LatticeX = candidate.LatticeX;
                placement.LatticeY = candidate.LatticeY;
                placement.Variant = candidate.Variant;
                placement.Rotation = static_cast<uint16>(
                    SampleUInt32(keys.Transform,
                                 RandomAddress{candidate.Scope, BASE_GENERATION_EVENT, TRANSFORM_ROTATION_DIMENSION}) >>
                    ROTATION_SHIFT);
                uint32 scaleOffset = 0;
                if (TrySampleBounded(keys.Transform,
                                     RandomAddress{candidate.Scope, BASE_GENERATION_EVENT, TRANSFORM_SCALE_DIMENSION},
                                     bounds.Scale,
                                     scaleOffset) != MathStatus::Success)
                {
                    return ScatterStatus::SamplingExhausted;
                }
                placement.Scale = static_cast<uint16>(layer.MinimumScale + scaleOffset);
                storage[count] = placement;
                ++count;
            }
        }
    }
    outCount = count;
    return ScatterStatus::Success;
}
} // namespace ludus::content::procedural
