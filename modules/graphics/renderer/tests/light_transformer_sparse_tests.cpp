#include "../src/internal/light_transformer_topology.h"
#include <catch2/catch_test_macros.hpp>
#include <cstring>
#include <iostream>
#include <ludus/foundation/containers/sort.hpp>
#include <ludus/foundation/memory/allocation_domain.hpp>
#include <ludus/foundation/memory/allocation_statistics.hpp>
#include <ludus/foundation/time/time.hpp>
namespace
{
using namespace ludus::foundation;
namespace internal = ludus::graphics::renderer::internal;
using internal::EncoderGrid;
using internal::EncoderLevel;
using Status = ludus::graphics::rhi::RasterStatus;
constexpr uint32 AXES = 3;
// Snapshot of the append builder used before occupancy admission, for byte and cost comparison.
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
Status BuildAppendReference(const EncoderLevel& level,
                            uint32 kernel,
                            Array<uint32>& outStarts,
                            Array<uint32>& outPairs,
                            usize& outPeakBytes) noexcept
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
                        const usize previousCapacity = outPairs.GetCapacity();
                        if (outPairs.TryAdd(order[lower].Index) == false ||
                            outPairs.TryAdd((x * kernel + y) * kernel + z) == false)
                        {
                            return Status::OutOfMemory;
                        }
                        if (outPairs.GetCapacity() != previousCapacity)
                        {
                            outPeakBytes =
                                (previousCapacity + outPairs.GetCapacity() + outStarts.GetCapacity()) * sizeof(uint32) +
                                order.GetCapacity() * sizeof(OrderedPoint);
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
} // namespace
TEST_CASE("Portable encoder topology covers sparse duplicates, pooling and partial patches",
          "[renderer][light-transformer]")
{
    internal::EncoderLevel level;
    level.Depth = 12;
    constexpr usize COUNT = 1031;
    REQUIRE(level.CoordinatesGrid.TryResize(COUNT) == true);
    for (usize i = 0; i < COUNT; ++i)
    {
        level.CoordinatesGrid[i] = {static_cast<uint32>(i), 0, 0};
    }
    for (uint32 order = 0; order < 4; ++order)
    {
        Array<uint32> patches;
        REQUIRE(internal::PrepareEncoderPatches(level, order, patches) == Status::Ready);
        for (usize i = 0; i < COUNT; ++i)
        {
            REQUIRE(patches[i * 2 + 1] == 1024);
            const usize begin = patches[i * 2];
            bool hasSelf = false;
            for (usize neighbor = 0; neighbor < 1024; ++neighbor)
            {
                REQUIRE(patches[begin + neighbor] < COUNT);
                hasSelf = hasSelf == true || patches[begin + neighbor] == i;
            }
            REQUIRE(hasSelf == true);
        }
    }
    level.CoordinatesGrid.Resize(3);
    level.CoordinatesGrid[0] = {1, 1, 1};
    level.CoordinatesGrid[1] = {1, 1, 1};
    level.CoordinatesGrid[2] = {2, 1, 1};
    internal::EncoderSparseRows rows;
    REQUIRE(internal::CountEncoderSparse(level, 3, rows) == Status::Ready);
    const Array<uint32>& starts = rows.Starts;
    REQUIRE(starts.GetSize() == 4);
    CHECK(starts[0] == 0);
    CHECK(starts[1] == 3);
    CHECK(starts[2] == 6);
    CHECK(starts[3] == 9);
}

TEST_CASE("Sparse row preparation preserves the append layout across occupancies",
          "[renderer][light-transformer][occupancy]")
{
    constexpr uint32 COUNT = 256;
    struct Fixture final
    {
        const char* Name;
        uint32 Voxels;
        uint32 Spacing;
    };
    const Fixture fixtures[] = {{"spread", COUNT, 4}, {"concentrated", 1, 1}, {"adjacent", 4, 1}};
    for (const Fixture& fixture : fixtures)
    {
        EncoderLevel level;
        level.CoordinatesGrid.Resize(COUNT);
        // Permute original indices so sorted neighbors differ from traversal order.
        for (uint32 i = 0; i < COUNT; ++i)
        {
            const uint32 shuffled = (i * 73) % COUNT;
            const uint32 x = (shuffled % fixture.Voxels) * fixture.Spacing;
            level.CoordinatesGrid[i] = {x, 2, 2};
        }
        for (const uint32 kernel : {3U, 5U})
        {
            usize expectedPairs = 0;
            const uint32 population = COUNT / fixture.Voxels;
            for (uint32 x = 0; x < fixture.Voxels; ++x)
            {
                for (uint32 neighbor = 0; neighbor < fixture.Voxels; ++neighbor)
                {
                    const uint32 distance = (x > neighbor ? x - neighbor : neighbor - x) * fixture.Spacing;
                    if (distance <= kernel / 2)
                    {
                        expectedPairs += population * population;
                    }
                }
            }
            Array<uint32> starts;
            Array<uint32> reference;
            usize appendPeak = 0;
            const uint64 allocationsBefore = GetAllocationStatistics(GetContainerAllocationDomain()).AllocationCount;
            const uint64 start = time::NowTicks();
            const Status appended = BuildAppendReference(level, kernel, starts, reference, appendPeak);
            const uint64 appendTime = time::NowTicks() - start;
            REQUIRE(appended == Status::Ready);
            const uint64 afterAppend = GetAllocationStatistics(GetContainerAllocationDomain()).AllocationCount;
            internal::EncoderSparseRows rows;
            const uint64 countStart = time::NowTicks();
            const Status counted = internal::CountEncoderSparse(level, kernel, rows);
            const uint64 countTime = time::NowTicks() - countStart;
            REQUIRE(counted == Status::Ready);
            REQUIRE(rows.Starts.GetLast() == expectedPairs);
            const uint64 afterCount = GetAllocationStatistics(GetContainerAllocationDomain()).AllocationCount;
            Array<uint32> pairs;
            const uint64 fillStart = time::NowTicks();
            pairs.Resize(expectedPairs * internal::SPARSE_PAIR_WORDS);
            internal::FillEncoderSparse(level, rows, {pairs.GetData(), pairs.GetSize()});
            const uint64 fillTime = time::NowTicks() - fillStart;
            const uint64 afterFill = GetAllocationStatistics(GetContainerAllocationDomain()).AllocationCount;
            REQUIRE(starts.GetSize() == rows.Starts.GetSize());
            REQUIRE(reference.GetSize() == pairs.GetSize());
            REQUIRE(std::memcmp(starts.GetData(), rows.Starts.GetData(), starts.GetSize() * sizeof(uint32)) == 0);
            REQUIRE(std::memcmp(reference.GetData(), pairs.GetData(), pairs.GetSize() * sizeof(uint32)) == 0);
            const usize topologyBytes = (pairs.GetSize() + starts.GetSize()) * sizeof(uint32);
            const usize peakBytes = topologyBytes + rows.Order.GetCapacity() * sizeof(internal::SparseOrderedPoint);
            std::cout << "layout=" << fixture.Name << " kernel=" << kernel << " pairs=" << expectedPairs
                      << " topology_bytes=" << topologyBytes << " append_ns=" << appendTime << " count_ns=" << countTime
                      << " fill_ns=" << fillTime << " append_peak_bytes=" << appendPeak << " peak_bytes=" << peakBytes
                      << " append_allocations=" << afterAppend - allocationsBefore
                      << " count_allocations=" << afterCount - afterAppend
                      << " fill_allocations=" << afterFill - afterCount << '\n';
        }
    }
}

TEST_CASE("Dense upper-count sparse sizing uses linear scratch", "[renderer][light-transformer][occupancy]")
{
    constexpr usize COUNT = LUDUS_LIGHT_ENCODER_MAX_POINTS / LUDUS_LIGHT_ENCODER_ANCHOR_DIVISOR;
    EncoderLevel level;
    level.CoordinatesGrid.Resize(COUNT);
    internal::EncoderSparseRows rows;
    const uint64 start = time::NowTicks();
    REQUIRE(internal::CountEncoderSparse(level, 5, rows) == Status::Ready);
    const uint64 elapsed = time::NowTicks() - start;
    REQUIRE(rows.Starts.GetLast() == COUNT * COUNT);
    const usize bytes =
        rows.Order.GetCapacity() * sizeof(internal::SparseOrderedPoint) + rows.Starts.GetCapacity() * sizeof(uint32);
    REQUIRE(bytes == COUNT * sizeof(internal::SparseOrderedPoint) + (COUNT + 1) * sizeof(uint32));
    std::cout << "dense pairs=" << rows.Starts.GetLast() << " count_ns=" << elapsed << " peak_bytes=" << bytes << '\n';
}
