// Serial reference kernels for derived terrain fields; see terrain_fields.h for
// contracts and attribution. Every function validates, allocates scratch, then
// writes its outputs, so a failure never leaves a partially written field.

#include <ludus/content/terrain/terrain_fields.h>

#include <ludus/foundation/containers/array.hpp>
#include <ludus/foundation/containers/static_array.hpp>
#include <ludus/foundation/math/scalar.hpp>

namespace ludus::content::terrain
{
namespace
{
namespace math = foundation::math;
using foundation::Array;
using foundation::BorrowedView;
using foundation::core::float64;

/// Marks a flood root (a border sample) that no neighbour pushed.
constexpr uint32 NO_PARENT = 0xffffffffU;

[[nodiscard]] bool AreAllFinite(BorrowedView<const float32> values) noexcept
{
    for (usize index = 0; index < values.GetSize(); ++index)
    {
        if (math::IsFinite(values[index]) == false)
        {
            return false;
        }
    }
    return true;
}

template <typename ElementType>
[[nodiscard]] bool HoldsOnePerSample(BorrowedView<ElementType> view, usize samples) noexcept
{
    return view.IsValid() == true && view.GetSize() == samples;
}

/// Metric distance to each D8 neighbour, in TerrainFlowDirection order; D8 drops divide by it.
using NeighbourDistances = foundation::StaticArray<float32, TERRAIN_FLOW_NEIGHBOURS>;

[[nodiscard]] NeighbourDistances ComputeNeighbourDistances(const TerrainFieldGrid& grid) noexcept
{
    NeighbourDistances distances = {};
    for (uint32 ordinal = 0; ordinal < TERRAIN_FLOW_NEIGHBOURS; ++ordinal)
    {
        const TerrainFlowOffset offset = GetTerrainFlowOffset(static_cast<TerrainFlowDirection>(ordinal));
        const float32 x = static_cast<float32>(offset.X) * grid.SpacingXMetres;
        const float32 z = static_cast<float32>(offset.Z) * grid.SpacingZMetres;
        distances[ordinal] = math::Sqrt(x * x + z * z);
    }
    return distances;
}

/// Flow direction from a sample to one of its eight neighbours, given the column and row steps.
[[nodiscard]] TerrainFlowDirection GetDirectionToOffset(int32 stepX, int32 stepZ) noexcept
{
    for (uint32 ordinal = 0; ordinal < TERRAIN_FLOW_NEIGHBOURS; ++ordinal)
    {
        const TerrainFlowDirection direction = static_cast<TerrainFlowDirection>(ordinal);
        const TerrainFlowOffset offset = GetTerrainFlowOffset(direction);
        if (offset.X == stepX && offset.Z == stepZ)
        {
            return direction;
        }
    }
    return TerrainFlowDirection::Outlet;
}

/// Binary min-heap of sample indices keyed by filled elevation, then by index, so equal heights pop in a fixed order.
class FloodQueue final
{
public:
    FloodQueue(Array<uint32>& inoutStorage, const Array<float32>& filledElevation) noexcept
        : mStorage(inoutStorage), mFilledElevation(filledElevation)
    {
    }

    [[nodiscard]] bool IsEmpty() const noexcept
    {
        return mCount == 0;
    }

    void Push(uint32 sample) noexcept
    {
        LUDUS_ASSERT(mCount < mStorage.GetSize());
        usize position = mCount;
        ++mCount;
        mStorage[position] = sample;
        while (position > 0)
        {
            const usize parent = (position - 1) / 2;
            if (isBefore(mStorage[position], mStorage[parent]) == false)
            {
                break;
            }
            swapEntries(position, parent);
            position = parent;
        }
    }

    [[nodiscard]] uint32 Pop() noexcept
    {
        LUDUS_ASSERT(mCount > 0);
        const uint32 first = mStorage[0];
        --mCount;
        mStorage[0] = mStorage[mCount];
        usize position = 0;
        while (true)
        {
            const usize left = 2 * position + 1;
            if (left >= mCount)
            {
                break;
            }
            const usize right = left + 1;
            usize smallest = left;
            if (right < mCount && isBefore(mStorage[right], mStorage[left]) == true)
            {
                smallest = right;
            }
            if (isBefore(mStorage[smallest], mStorage[position]) == false)
            {
                break;
            }
            swapEntries(position, smallest);
            position = smallest;
        }
        return first;
    }

private:
    [[nodiscard]] bool isBefore(uint32 a, uint32 b) const noexcept
    {
        const float32 elevationA = mFilledElevation[a];
        const float32 elevationB = mFilledElevation[b];
        if (elevationA != elevationB)
        {
            return elevationA < elevationB;
        }
        return a < b;
    }

    void swapEntries(usize a, usize b) noexcept
    {
        const uint32 held = mStorage[a];
        mStorage[a] = mStorage[b];
        mStorage[b] = held;
    }

    Array<uint32>& mStorage;
    const Array<float32>& mFilledElevation;
    usize mCount = 0;
};

/// Scratch of one drainage solve, allocated before any output is written.
struct DrainageScratch final
{
    Array<float32> FilledElevation;
    Array<uint32> Parent;
    Array<uint8> IsVisited;
    Array<uint32> Queue;
    Array<uint32> Inflow;
    Array<uint32> Accumulation;
    Array<TerrainFlowDirection> Direction;

    [[nodiscard]] bool TryAllocate(usize samples)
    {
        return FilledElevation.TryResize(samples) == true && Parent.TryResize(samples, NO_PARENT) == true &&
               IsVisited.TryResize(samples, 0) == true && Queue.TryResize(samples) == true &&
               Inflow.TryResize(samples, 0) == true && Accumulation.TryResize(samples, 1) == true &&
               Direction.TryResize(samples, TerrainFlowDirection::Outlet) == true;
    }
};

[[nodiscard]] bool IsBorderSample(const TerrainFieldGrid& grid, uint32 x, uint32 z) noexcept
{
    return x == 0 || z == 0 || x + 1 == grid.Columns || z + 1 == grid.Rows;
}

[[nodiscard]] bool TryGetNeighbour(const TerrainFieldGrid& grid,
                                   uint32 x,
                                   uint32 z,
                                   TerrainFlowDirection direction,
                                   uint32& outNeighbour) noexcept
{
    const TerrainFlowOffset offset = GetTerrainFlowOffset(direction);
    const int32 neighbourX = static_cast<int32>(x) + offset.X;
    const int32 neighbourZ = static_cast<int32>(z) + offset.Z;
    if (neighbourX < 0 || neighbourZ < 0 || static_cast<uint32>(neighbourX) >= grid.Columns ||
        static_cast<uint32>(neighbourZ) >= grid.Rows)
    {
        return false;
    }
    outNeighbour = static_cast<uint32>(neighbourZ) * grid.Columns + static_cast<uint32>(neighbourX);
    return true;
}

/// Priority-Flood from the border: filled[n] = max(elevation[n], filled[parent]) in pop order.
void FillDepressions(const TerrainFieldGrid& grid,
                     BorrowedView<const float32> elevationMetres,
                     DrainageScratch& inoutScratch) noexcept
{
    FloodQueue queue(inoutScratch.Queue, inoutScratch.FilledElevation);
    for (uint32 z = 0; z < grid.Rows; ++z)
    {
        for (uint32 x = 0; x < grid.Columns; ++x)
        {
            if (IsBorderSample(grid, x, z) == false)
            {
                continue;
            }
            const uint32 sample = z * grid.Columns + x;
            inoutScratch.FilledElevation[sample] = elevationMetres[sample];
            inoutScratch.IsVisited[sample] = 1;
            queue.Push(sample);
        }
    }
    while (queue.IsEmpty() == false)
    {
        const uint32 sample = queue.Pop();
        const uint32 x = sample % grid.Columns;
        const uint32 z = sample / grid.Columns;
        for (uint32 ordinal = 0; ordinal < TERRAIN_FLOW_NEIGHBOURS; ++ordinal)
        {
            uint32 neighbour = 0;
            if (TryGetNeighbour(grid, x, z, static_cast<TerrainFlowDirection>(ordinal), neighbour) == false ||
                inoutScratch.IsVisited[neighbour] != 0)
            {
                continue;
            }
            inoutScratch.IsVisited[neighbour] = 1;
            inoutScratch.FilledElevation[neighbour] =
                math::Max(elevationMetres[neighbour], inoutScratch.FilledElevation[sample]);
            inoutScratch.Parent[neighbour] = sample;
            queue.Push(neighbour);
        }
    }
}

/// D8 on the filled surface; a sample without a strictly lower neighbour follows its flood parent.
void RouteFlow(const TerrainFieldGrid& grid, DrainageScratch& inoutScratch) noexcept
{
    const NeighbourDistances distances = ComputeNeighbourDistances(grid);
    for (uint32 z = 0; z < grid.Rows; ++z)
    {
        for (uint32 x = 0; x < grid.Columns; ++x)
        {
            const uint32 sample = z * grid.Columns + x;
            const float32 elevation = inoutScratch.FilledElevation[sample];
            TerrainFlowDirection chosen = TerrainFlowDirection::Outlet;
            float32 steepestDrop = 0;
            for (uint32 ordinal = 0; ordinal < TERRAIN_FLOW_NEIGHBOURS; ++ordinal)
            {
                const TerrainFlowDirection direction = static_cast<TerrainFlowDirection>(ordinal);
                uint32 neighbour = 0;
                if (TryGetNeighbour(grid, x, z, direction, neighbour) == false)
                {
                    continue;
                }
                const float32 drop = (elevation - inoutScratch.FilledElevation[neighbour]) / distances[ordinal];
                if (drop > steepestDrop)
                {
                    steepestDrop = drop;
                    chosen = direction;
                }
            }
            if (chosen == TerrainFlowDirection::Outlet && inoutScratch.Parent[sample] != NO_PARENT)
            {
                const uint32 parent = inoutScratch.Parent[sample];
                const int32 stepX = static_cast<int32>(parent % grid.Columns) - static_cast<int32>(x);
                const int32 stepZ = static_cast<int32>(parent / grid.Columns) - static_cast<int32>(z);
                chosen = GetDirectionToOffset(stepX, stepZ);
            }
            inoutScratch.Direction[sample] = chosen;
        }
    }
}

/// Contributing samples by topological order over the routing; returns the processed count.
[[nodiscard]] usize AccumulateFlow(const TerrainFieldGrid& grid, DrainageScratch& inoutScratch) noexcept
{
    const usize samples = GetTerrainSampleCount(grid);
    for (uint32 sample = 0; sample < samples; ++sample)
    {
        uint32 target = 0;
        if (inoutScratch.Direction[sample] != TerrainFlowDirection::Outlet &&
            TryGetNeighbour(grid,
                            sample % grid.Columns,
                            sample / grid.Columns,
                            inoutScratch.Direction[sample],
                            target) == true)
        {
            ++inoutScratch.Inflow[target];
        }
    }
    usize head = 0;
    usize tail = 0;
    for (uint32 sample = 0; sample < samples; ++sample)
    {
        if (inoutScratch.Inflow[sample] == 0)
        {
            inoutScratch.Queue[tail] = sample;
            ++tail;
        }
    }
    while (head < tail)
    {
        const uint32 sample = inoutScratch.Queue[head];
        ++head;
        uint32 target = 0;
        if (inoutScratch.Direction[sample] == TerrainFlowDirection::Outlet ||
            TryGetNeighbour(grid,
                            sample % grid.Columns,
                            sample / grid.Columns,
                            inoutScratch.Direction[sample],
                            target) == false)
        {
            continue;
        }
        inoutScratch.Accumulation[target] += inoutScratch.Accumulation[sample];
        --inoutScratch.Inflow[target];
        if (inoutScratch.Inflow[target] == 0)
        {
            inoutScratch.Queue[tail] = target;
            ++tail;
        }
    }
    return head;
}
} // namespace

bool IsTerrainFieldGridValid(const TerrainFieldGrid& grid) noexcept
{
    return grid.Columns >= MIN_TERRAIN_FIELD_AXIS_SAMPLES && grid.Columns <= MAX_TERRAIN_FIELD_AXIS_SAMPLES &&
           grid.Rows >= MIN_TERRAIN_FIELD_AXIS_SAMPLES && grid.Rows <= MAX_TERRAIN_FIELD_AXIS_SAMPLES &&
           math::IsFinite(grid.SpacingXMetres) == true && grid.SpacingXMetres > 0 &&
           math::IsFinite(grid.SpacingZMetres) == true && grid.SpacingZMetres > 0;
}

TerrainFieldStatus TryComputeTerrainSlope(const TerrainFieldGrid& grid,
                                          BorrowedView<const float32> elevationMetres,
                                          BorrowedView<float32> outSlope) noexcept
{
    if (IsTerrainFieldGridValid(grid) == false)
    {
        return TerrainFieldStatus::InvalidGrid;
    }
    const usize samples = GetTerrainSampleCount(grid);
    if (HoldsOnePerSample(elevationMetres, samples) == false || HoldsOnePerSample(outSlope, samples) == false)
    {
        return TerrainFieldStatus::InvalidStorage;
    }
    if (AreAllFinite(elevationMetres) == false)
    {
        return TerrainFieldStatus::NonFiniteInput;
    }
    for (uint32 z = 0; z < grid.Rows; ++z)
    {
        const uint32 previousZ = z == 0 ? z : z - 1;
        const uint32 nextZ = math::Min(z + 1, grid.Rows - 1);
        for (uint32 x = 0; x < grid.Columns; ++x)
        {
            const uint32 previousX = x == 0 ? x : x - 1;
            const uint32 nextX = math::Min(x + 1, grid.Columns - 1);
            const float32 runX = static_cast<float32>(nextX - previousX) * grid.SpacingXMetres;
            const float32 runZ = static_cast<float32>(nextZ - previousZ) * grid.SpacingZMetres;
            const float32 riseX =
                elevationMetres[z * grid.Columns + nextX] - elevationMetres[z * grid.Columns + previousX];
            const float32 riseZ =
                elevationMetres[nextZ * grid.Columns + x] - elevationMetres[previousZ * grid.Columns + x];
            const float32 gradientX = riseX / runX;
            const float32 gradientZ = riseZ / runZ;
            outSlope[z * grid.Columns + x] = math::Sqrt(gradientX * gradientX + gradientZ * gradientZ);
        }
    }
    return TerrainFieldStatus::Success;
}

TerrainFieldStatus TryComputeTerrainDrainage(const TerrainFieldGrid& grid,
                                             BorrowedView<const float32> elevationMetres,
                                             BorrowedView<float32> outFilledElevationMetres,
                                             BorrowedView<TerrainFlowDirection> outFlowDirection,
                                             BorrowedView<uint32> outAccumulation,
                                             TerrainDrainageSummary& outSummary) noexcept
{
    if (IsTerrainFieldGridValid(grid) == false)
    {
        return TerrainFieldStatus::InvalidGrid;
    }
    const usize samples = GetTerrainSampleCount(grid);
    if (HoldsOnePerSample(elevationMetres, samples) == false ||
        HoldsOnePerSample(outFilledElevationMetres, samples) == false ||
        HoldsOnePerSample(outFlowDirection, samples) == false || HoldsOnePerSample(outAccumulation, samples) == false)
    {
        return TerrainFieldStatus::InvalidStorage;
    }
    if (AreAllFinite(elevationMetres) == false)
    {
        return TerrainFieldStatus::NonFiniteInput;
    }
    DrainageScratch scratch;
    if (scratch.TryAllocate(samples) == false)
    {
        return TerrainFieldStatus::AllocationFailed;
    }
    FillDepressions(grid, elevationMetres, scratch);
    RouteFlow(grid, scratch);
    const usize processed = AccumulateFlow(grid, scratch);
    // The D8 edges strictly descend and the parent edges form the flood's forest, so the routing is acyclic.
    LUDUS_ASSERT(processed == samples);
    (void)processed;

    TerrainDrainageSummary summary;
    for (usize sample = 0; sample < samples; ++sample)
    {
        const float32 fill = scratch.FilledElevation[sample] - elevationMetres[sample];
        if (fill > 0)
        {
            ++summary.FilledSamples;
            summary.MaximumFillMetres = math::Max(summary.MaximumFillMetres, fill);
        }
        if (scratch.Direction[sample] == TerrainFlowDirection::Outlet)
        {
            ++summary.OutletSamples;
            summary.OutletAccumulation += scratch.Accumulation[sample];
        }
        outFilledElevationMetres[sample] = scratch.FilledElevation[sample];
        outFlowDirection[sample] = scratch.Direction[sample];
        outAccumulation[sample] = scratch.Accumulation[sample];
    }
    outSummary = summary;
    return TerrainFieldStatus::Success;
}

TerrainFieldStatus TryComputeTerrainWetness(const TerrainFieldGrid& grid,
                                            BorrowedView<const float32> slope,
                                            BorrowedView<const uint32> accumulation,
                                            const TerrainWetnessParameters& parameters,
                                            BorrowedView<float32> outWetness) noexcept
{
    if (IsTerrainFieldGridValid(grid) == false)
    {
        return TerrainFieldStatus::InvalidGrid;
    }
    const usize samples = GetTerrainSampleCount(grid);
    if (HoldsOnePerSample(slope, samples) == false || HoldsOnePerSample(accumulation, samples) == false ||
        HoldsOnePerSample(outWetness, samples) == false)
    {
        return TerrainFieldStatus::InvalidStorage;
    }
    if (math::IsFinite(parameters.MinimumIndex) == false || math::IsFinite(parameters.MaximumIndex) == false ||
        math::IsFinite(parameters.MinimumSlope) == false)
    {
        return TerrainFieldStatus::NonFiniteInput;
    }
    if (parameters.MaximumIndex <= parameters.MinimumIndex || parameters.MinimumSlope <= 0)
    {
        return TerrainFieldStatus::InvalidArgument;
    }
    if (AreAllFinite(slope) == false)
    {
        return TerrainFieldStatus::NonFiniteInput;
    }
    for (usize sample = 0; sample < samples; ++sample)
    {
        if (accumulation[sample] == 0 || slope[sample] < 0)
        {
            return TerrainFieldStatus::InvalidArgument;
        }
    }
    const float64 minimumIndex = static_cast<float64>(parameters.MinimumIndex);
    const float64 indexRange = static_cast<float64>(parameters.MaximumIndex) - minimumIndex;
    const float64 minimumSlope = static_cast<float64>(parameters.MinimumSlope);
    const float64 sampleAreaSquareMetres = static_cast<float64>(GetTerrainSampleAreaSquareMetres(grid));
    const float64 contourWidthMetres = math::Sqrt(sampleAreaSquareMetres);
    for (usize sample = 0; sample < samples; ++sample)
    {
        const float64 catchmentArea =
            static_cast<float64>(accumulation[sample]) * sampleAreaSquareMetres / contourWidthMetres;
        const float64 tangent = math::Max(static_cast<float64>(slope[sample]), minimumSlope);
        const float64 index = math::Log(catchmentArea / tangent);
        outWetness[sample] = static_cast<float32>(math::Clamp((index - minimumIndex) / indexRange, 0.0, 1.0));
    }
    return TerrainFieldStatus::Success;
}
} // namespace ludus::content::terrain
