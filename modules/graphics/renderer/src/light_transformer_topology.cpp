#include "internal/light_transformer_topology.h"

#include <ludus/foundation/containers/sort.hpp>
#include <ludus/foundation/geometry/point_search.hpp>
#include <ludus/foundation/geometry/status.hpp>
#include <ludus/foundation/math/queries.hpp>
#include <ludus/foundation/math/scalar.hpp>

// Thanks to Bing Xu et al., "A Generalizable Light Transport 3D Embedding for
// Global Illumination", SIGGRAPH 2026, section 3.3, https://arxiv.org/abs/2510.18189,
// and Xiaoyang Wu et al., "Point Transformer V3: Simpler, Faster, Stronger",
// CVPR 2024, https://arxiv.org/abs/2312.10035, for neighborhood embedding and
// serialized sparse hierarchy. This independent CPU preparation computes no
// learned features. Equal-distance/order ties use original indices, not CUDA
// scheduling; the four orders remain fixed rather than randomly shuffled.
namespace ludus::graphics::renderer::internal
{
using namespace foundation;
using Status = rhi::RasterStatus;
namespace
{
constexpr uint32 AXES = 3;
constexpr uint32 ORDER_TRANSPOSE = 1;
constexpr uint32 ORDER_HILBERT = 2;
constexpr float32 FPS_INITIAL_DISTANCE_SQUARED = 1e10F;
constexpr float32 FPS_ORIGIN_EXCLUSION_SQUARED = 0.001F;
struct OrderedPoint final
{
    uint64 Code = 0;
    uint32 Index = 0;
};
bool HasLowerOrder(const OrderedPoint& left, const OrderedPoint& right) noexcept
{
    return left.Code < right.Code || (left.Code == right.Code && left.Index < right.Index);
}
uint64 EncodeMorton(EncoderGrid coordinateGrid) noexcept
{
    uint64 code = 0;
    for (uint32 bit = 0; bit < LUDUS_LIGHT_ENCODER_COORDINATE_BITS; ++bit)
    {
        // Pointcept/OCNN z-order uses X in the most significant bit of each triple.
        code |= uint64{(coordinateGrid.X >> bit) & 1} << (bit * AXES + 2);
        code |= uint64{(coordinateGrid.Y >> bit) & 1} << (bit * AXES + 1);
        code |= uint64{(coordinateGrid.Z >> bit) & 1} << (bit * AXES);
    }
    return code;
}
uint64 EncodeHilbert(EncoderGrid coordinateGrid, uint32 depth) noexcept
{
    // Thanks to John Skilling, "Programming the Hilbert curve", AIP Conference
    // Proceedings 707, pp.381-387 (2004), https://doi.org/10.1063/1.1751381.
    // Apply exchanges/inversions to coordinate words, then Gray-decode the
    // interleaved transpose. This independently implements the integer mapping.
    StaticArray<uint32, AXES> words{{coordinateGrid.X, coordinateGrid.Y, coordinateGrid.Z}};
    for (uint32 bit = depth; bit > 0; --bit)
    {
        const uint32 mask = uint32{1} << (bit - 1);
        const uint32 lower = mask - 1;
        for (uint32 axis = 0; axis < AXES; ++axis)
        {
            if ((words[axis] & mask) != 0)
            {
                words[0] ^= lower;
            }
            else
            {
                const uint32 exchanged = (words[0] ^ words[axis]) & lower;
                words[axis] ^= exchanged;
                words[0] ^= exchanged;
            }
        }
    }
    uint64 gray = 0;
    for (uint32 bit = depth; bit > 0; --bit)
    {
        for (uint32 axis = 0; axis < AXES; ++axis)
        {
            gray = (gray << 1) | ((words[axis] >> (bit - 1)) & 1);
        }
    }
    uint64 binary = gray;
    for (uint64 remaining = gray >> 1; remaining != 0; remaining >>= 1)
    {
        binary ^= remaining;
    }
    return binary;
}
Status CopyPositions(BorrowedView<const math::Vector3> positionsModel, Array<math::Vector3>& outPositionsModel) noexcept
{
    if (outPositionsModel.TryResize(positionsModel.GetSize()) == false)
    {
        return Status::OutOfMemory;
    }
    for (usize i = 0; i < positionsModel.GetSize(); ++i)
    {
        outPositionsModel[i] = positionsModel[i];
    }
    return Status::Ready;
}
} // namespace
Status PrepareEncoderFps(BorrowedView<const math::Vector3> positionsModel,
                         Array<uint32>& outIndices,
                         Array<math::Vector3>& outPositionsModel) noexcept
{
    const usize count = positionsModel.GetSize() / 2;
    Array<float32> distancesSquared;
    if (count == 0 || outIndices.TryResize(count) == false || outPositionsModel.TryResize(count) == false ||
        distancesSquared.TryResize(positionsModel.GetSize()) == false)
    {
        return count == 0 ? Status::InvalidDescription : Status::OutOfMemory;
    }
    for (float32& distanceSquared : distancesSquared)
    {
        distanceSquared = FPS_INITIAL_DISTANCE_SQUARED;
    }
    uint32 selected = 0;
    for (usize sample = 0; sample < count; ++sample)
    {
        outIndices[sample] = selected;
        outPositionsModel[sample] = positionsModel[selected];
        float32 maximumDistanceSquared = -1;
        uint32 next = 0;
        for (usize i = 0; i < positionsModel.GetSize(); ++i)
        {
            if (math::Dot(positionsModel[i], positionsModel[i]) <= FPS_ORIGIN_EXCLUSION_SQUARED)
            {
                continue;
            }
            const math::Vector3 differenceModel = positionsModel[i] - positionsModel[selected];
            distancesSquared[i] = math::Min(distancesSquared[i], math::Dot(differenceModel, differenceModel));
            if (distancesSquared[i] > maximumDistanceSquared)
            {
                maximumDistanceSquared = distancesSquared[i];
                next = static_cast<uint32>(i);
            }
        }
        selected = next;
    }
    return Status::Ready;
}
Status PrepareEncoderNeighbors(const EncoderNeighborSources& sources, Array<uint32>& outNeighbors) noexcept
{
    constexpr usize NEIGHBORS = LUDUS_LIGHT_ENCODER_NEIGHBORS;
    geometry::PointSearchTree tree;
    const geometry::GeometryStatus built = tree.Build(sources.PositionsModel);
    if (built != geometry::GeometryStatus::Success)
    {
        return built == geometry::GeometryStatus::OutOfMemory ? Status::OutOfMemory : Status::InvalidDescription;
    }
    if (outNeighbors.TryResize(sources.QueriesModel.GetSize() * (NEIGHBORS + 1)) == false)
    {
        return Status::OutOfMemory;
    }
    StaticArray<geometry::PointNeighbor, NEIGHBORS> neighbors{};
    for (usize i = 0; i < sources.QueriesModel.GetSize(); ++i)
    {
        if (tree.FindNearest(sources.QueriesModel[i], {neighbors.GetData(), neighbors.GetSize()}) !=
            geometry::GeometryStatus::Success)
        {
            return Status::InvalidDescription;
        }
        for (usize neighbor = 0; neighbor < NEIGHBORS; ++neighbor)
        {
            outNeighbors[i * (NEIGHBORS + 1) + neighbor] = neighbors[neighbor].PointIndex;
        }
        outNeighbors[i * (NEIGHBORS + 1) + NEIGHBORS] = sources.Centers[i];
    }
    return Status::Ready;
}
Status PrepareEncoderLevels(BorrowedView<const math::Vector3> anchorsModel,
                            StaticArray<EncoderLevel, LUDUS_LIGHT_ENCODER_LEVELS>& outLevels) noexcept
{
    EncoderLevel& first = outLevels[0];
    if (CopyPositions(anchorsModel, first.PositionsModel) != Status::Ready ||
        first.CoordinatesGrid.TryResize(anchorsModel.GetSize()) == false)
    {
        return Status::OutOfMemory;
    }
    math::Vector3 minimumModel = anchorsModel[0];
    for (usize i = 0; i < anchorsModel.GetSize(); ++i)
    {
        const math::Vector3 positionModel = anchorsModel[i];
        minimumModel.X = math::Min(minimumModel.X, positionModel.X);
        minimumModel.Y = math::Min(minimumModel.Y, positionModel.Y);
        minimumModel.Z = math::Min(minimumModel.Z, positionModel.Z);
    }
    constexpr float32 GRID_SIZE = LUDUS_LIGHT_ENCODER_GRID_SIZE;
    constexpr uint32 MAX_GRID_COORDINATE = (uint32{1} << LUDUS_LIGHT_ENCODER_COORDINATE_BITS) - 1;
    uint32 maximum = 0;
    for (usize i = 0; i < anchorsModel.GetSize(); ++i)
    {
        const math::Vector3 coordinateGrid = (anchorsModel[i] - minimumModel) / GRID_SIZE;
        if (coordinateGrid.X > MAX_GRID_COORDINATE || coordinateGrid.Y > MAX_GRID_COORDINATE ||
            coordinateGrid.Z > MAX_GRID_COORDINATE)
        {
            return Status::CapacityExceeded;
        }
        first.CoordinatesGrid[i] = {static_cast<uint32>(coordinateGrid.X),
                                    static_cast<uint32>(coordinateGrid.Y),
                                    static_cast<uint32>(coordinateGrid.Z)};
        maximum = math::Max(
            maximum,
            math::Max(first.CoordinatesGrid[i].X, math::Max(first.CoordinatesGrid[i].Y, first.CoordinatesGrid[i].Z)));
    }
    for (uint32 value = maximum; value != 0; value >>= 1)
    {
        ++first.Depth;
    }
    for (usize stage = 1; stage < outLevels.GetSize(); ++stage)
    {
        const EncoderLevel& parent = outLevels[stage - 1];
        EncoderLevel& child = outLevels[stage];
        const uint32 shift = parent.Depth > 0 ? 1 : 0;
        child.Depth = parent.Depth - shift;
        Array<OrderedPoint> order;
        if (order.TryResize(parent.PositionsModel.GetSize()) == false ||
            child.ParentToChild.TryResize(order.GetSize()) == false ||
            child.Children.TryResize(order.GetSize()) == false)
        {
            return Status::OutOfMemory;
        }
        for (usize i = 0; i < order.GetSize(); ++i)
        {
            const EncoderGrid coordinateGrid = parent.CoordinatesGrid[i];
            order[i] = {EncodeMorton({coordinateGrid.X >> shift, coordinateGrid.Y >> shift, coordinateGrid.Z >> shift}),
                        static_cast<uint32>(i)};
        }
        Sort(order.GetData(), order.GetSize(), HasLowerOrder);
        usize cursor = 0;
        while (cursor < order.GetSize())
        {
            const usize begin = cursor;
            math::Vector3 positionModel{};
            const uint64 code = order[begin].Code;
            while (cursor < order.GetSize() && order[cursor].Code == code)
            {
                const uint32 index = order[cursor].Index;
                child.Children[cursor] = index;
                child.ParentToChild[index] = static_cast<uint32>(child.PositionsModel.GetSize());
                positionModel = positionModel + parent.PositionsModel[index];
                ++cursor;
            }
            const EncoderGrid coordinateGrid = parent.CoordinatesGrid[order[begin].Index];
            if (child.PositionsModel.TryAdd(positionModel / static_cast<float32>(cursor - begin)) == false ||
                child.CoordinatesGrid.TryAdd(
                    {coordinateGrid.X >> shift, coordinateGrid.Y >> shift, coordinateGrid.Z >> shift}) == false ||
                child.ChildStarts.TryAdd(static_cast<uint32>(begin)) == false)
            {
                return Status::OutOfMemory;
            }
        }
        if (child.ChildStarts.TryAdd(static_cast<uint32>(cursor)) == false)
        {
            return Status::OutOfMemory;
        }
    }
    return Status::Ready;
}
Status PrepareEncoderSparse(const EncoderLevel& level,
                            uint32 kernel,
                            Array<uint32>& outStarts,
                            Array<uint32>& outPairs) noexcept
{
    Array<OrderedPoint> order;
    if (order.TryResize(level.CoordinatesGrid.GetSize()) == false || outStarts.TryResize(order.GetSize() + 1) == false)
    {
        return Status::OutOfMemory;
    }
    for (usize i = 0; i < order.GetSize(); ++i)
    {
        order[i] = {EncodeMorton(level.CoordinatesGrid[i]), static_cast<uint32>(i)};
    }
    Sort(order.GetData(), order.GetSize(), HasLowerOrder);
    const int32 radius = static_cast<int32>(kernel / 2);
    constexpr int32 MAX_GRID_COORDINATE = (int32{1} << LUDUS_LIGHT_ENCODER_COORDINATE_BITS) - 1;
    for (usize i = 0; i < order.GetSize(); ++i)
    {
        outStarts[i] = static_cast<uint32>(outPairs.GetSize() / 2);
        const EncoderGrid coordinateGrid = level.CoordinatesGrid[i];
        for (uint32 x = 0; x < kernel; ++x)
        {
            for (uint32 y = 0; y < kernel; ++y)
            {
                for (uint32 z = 0; z < kernel; ++z)
                {
                    const int32 neighborX = static_cast<int32>(coordinateGrid.X) + static_cast<int32>(x) - radius;
                    const int32 neighborY = static_cast<int32>(coordinateGrid.Y) + static_cast<int32>(y) - radius;
                    const int32 neighborZ = static_cast<int32>(coordinateGrid.Z) + static_cast<int32>(z) - radius;
                    if (neighborX < 0 || neighborY < 0 || neighborZ < 0 || neighborX > MAX_GRID_COORDINATE ||
                        neighborY > MAX_GRID_COORDINATE || neighborZ > MAX_GRID_COORDINATE)
                    {
                        continue;
                    }
                    const uint64 code = EncodeMorton({static_cast<uint32>(neighborX),
                                                      static_cast<uint32>(neighborY),
                                                      static_cast<uint32>(neighborZ)});
                    usize lower = 0;
                    usize upper = order.GetSize();
                    while (lower < upper)
                    {
                        const usize middle = lower + (upper - lower) / 2;
                        if (order[middle].Code < code)
                        {
                            lower = middle + 1;
                        }
                        else
                        {
                            upper = middle;
                        }
                    }
                    while (lower < order.GetSize() && order[lower].Code == code)
                    {
                        if (outPairs.TryAdd(order[lower].Index) == false ||
                            outPairs.TryAdd((x * kernel + y) * kernel + z) == false)
                        {
                            return Status::OutOfMemory;
                        }
                        ++lower;
                    }
                }
            }
        }
    }
    outStarts[order.GetSize()] = static_cast<uint32>(outPairs.GetSize() / 2);
    return Status::Ready;
}
Status PrepareEncoderPatches(const EncoderLevel& level, uint32 orderIndex, Array<uint32>& outPatches) noexcept
{
    const usize count = level.CoordinatesGrid.GetSize();
    constexpr usize PATCH_SIZE = LUDUS_LIGHT_ENCODER_PATCH_SIZE;
    const usize paddedCount = count <= PATCH_SIZE ? count : ((count - 1) / PATCH_SIZE + 1) * PATCH_SIZE;
    Array<OrderedPoint> order;
    if (order.TryResize(count) == false || outPatches.TryResize(count * 2 + paddedCount) == false)
    {
        return Status::OutOfMemory;
    }
    for (usize i = 0; i < count; ++i)
    {
        EncoderGrid coordinateGrid = level.CoordinatesGrid[i];
        if ((orderIndex & ORDER_TRANSPOSE) != 0)
        {
            const uint32 x = coordinateGrid.X;
            coordinateGrid.X = coordinateGrid.Y;
            coordinateGrid.Y = x;
        }
        order[i] = {(orderIndex & ORDER_HILBERT) != 0 ? EncodeHilbert(coordinateGrid, level.Depth)
                                                      : EncodeMorton(coordinateGrid),
                    static_cast<uint32>(i)};
    }
    Sort(order.GetData(), order.GetSize(), HasLowerOrder);
    for (usize i = 0; i < count; ++i)
    {
        const usize begin = (i / PATCH_SIZE) * PATCH_SIZE;
        outPatches[usize{order[i].Index} * 2] = static_cast<uint32>(count * 2 + begin);
        outPatches[order[i].Index * 2 + 1] = static_cast<uint32>(math::Min(count, PATCH_SIZE));
    }
    for (usize i = 0; i < paddedCount; ++i)
    {
        // Last partial patch borrows from the preceding patch, exactly as PTv3.
        const usize source = i < count ? i : i - PATCH_SIZE;
        outPatches[count * 2 + i] = order[source].Index;
    }
    return Status::Ready;
}
} // namespace ludus::graphics::renderer::internal
