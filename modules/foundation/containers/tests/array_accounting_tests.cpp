// Array storage accounting through the Containers allocation domain (memory
// profiling phase 1). Array keeps its layout and allocation family; the domain
// observes capacity bytes. Each case compares counter deltas with what the
// Array itself reports, so the cases are independent of other Array use in the
// process as long as they run single-threaded.
#include <ludus/foundation/base/core.h>

#include <ludus/foundation/containers/array.hpp>
#include <ludus/foundation/containers/static_array.hpp>
#include <ludus/foundation/memory/allocation_domain.hpp>
#include <ludus/foundation/memory/allocation_statistics.hpp>

#include <thread>

#include <catch2/catch_test_macros.hpp>

namespace
{
using ludus::foundation::AccountingValidity;
using ludus::foundation::AllocationStatistics;
using ludus::foundation::Array;
using ludus::foundation::GetAllocationStatistics;
using ludus::foundation::GetContainerAllocationDomain;
using ludus::foundation::GetSystemAllocationDomain;
using ludus::foundation::Move;
using ludus::foundation::StaticArray;
using ludus::foundation::uint32;
using ludus::foundation::uint64;
using ludus::foundation::usize;

[[nodiscard]] AllocationStatistics SampleContainers() noexcept
{
    return GetAllocationStatistics(GetContainerAllocationDomain());
}

constexpr usize OVER_ALIGNED_PAYLOAD_WORDS = 3;

struct alignas(64) OverAligned
{
    uint64 Key = 0;
    StaticArray<uint64, OVER_ALIGNED_PAYLOAD_WORDS> Payload{};
};
} // namespace

TEST_CASE("Array growth charges capacity bytes to the Containers domain", "[containers][accounting]")
{
    const AllocationStatistics systemBefore = GetAllocationStatistics(GetSystemAllocationDomain());
    const AllocationStatistics before = SampleContainers();
    REQUIRE(before.Validity == AccountingValidity::Valid);

    uint64 expectedAllocatedBytes = 0;
    uint64 growthCount = 0;
    usize largestOverlapBytes = 0;
    {
        Array<uint32> values;
        REQUIRE(SampleContainers().AllocationCount == before.AllocationCount); // Construction allocates nothing.
        usize capacity = 0;
        for (uint32 i = 0; i < 1000; ++i)
        {
            values.Add(i);
            if (values.GetCapacity() != capacity)
            {
                const usize newBytes = values.GetCapacity() * sizeof(uint32);
                const usize oldBytes = capacity * sizeof(uint32);
                expectedAllocatedBytes += newBytes;
                ++growthCount;
                largestOverlapBytes =
                    oldBytes + newBytes > largestOverlapBytes ? oldBytes + newBytes : largestOverlapBytes;
                capacity = values.GetCapacity();
            }
        }
        const AllocationStatistics during = SampleContainers();
        REQUIRE(during.LiveRequestedBytes - before.LiveRequestedBytes == capacity * sizeof(uint32));
        REQUIRE(during.LiveBlocks - before.LiveBlocks == 1);
        REQUIRE(during.AllocatedBytes - before.AllocatedBytes == expectedAllocatedBytes);
        REQUIRE(during.AllocationCount - before.AllocationCount == growthCount);
        REQUIRE(during.FreeCount - before.FreeCount == growthCount - 1);
        // Growth holds the old and new buffers together; the high-water mark records that overlap.
        REQUIRE(during.PeakRequestedBytes >= before.LiveRequestedBytes + largestOverlapBytes);
    }
    const AllocationStatistics after = SampleContainers();
    REQUIRE(after.LiveRequestedBytes == before.LiveRequestedBytes);
    REQUIRE(after.LiveBlocks == before.LiveBlocks);
    REQUIRE(after.FreedBytes - before.FreedBytes == expectedAllocatedBytes);
    REQUIRE(after.FreeCount - before.FreeCount == growthCount);
    REQUIRE(after.Validity == AccountingValidity::Valid);
    // Array storage no longer appears in the general System domain.
    REQUIRE(GetAllocationStatistics(GetSystemAllocationDomain()).AllocationCount == systemBefore.AllocationCount);
}

TEST_CASE("Array ownership transfer creates no allocation or free events", "[containers][accounting]")
{
    Array<uint32> first;
    first.EnsureCapacity(64);
    Array<uint32> second;
    second.EnsureCapacity(16);
    const AllocationStatistics before = SampleContainers();

    first.Swap(second);
    Array<uint32> moved(Move(first));
    second = Move(moved);
    const AllocationStatistics afterTransfer = SampleContainers();
    REQUIRE(afterTransfer.AllocationCount == before.AllocationCount);
    // Swap moved the 64-element buffer into `second`; move-assignment then
    // released it and left `second` owning the 16-element buffer.
    REQUIRE(afterTransfer.LiveRequestedBytes == before.LiveRequestedBytes - 64 * sizeof(uint32));
    REQUIRE(second.GetCapacity() == 16);
    REQUIRE(afterTransfer.FreeCount == before.FreeCount + 1);
    REQUIRE(afterTransfer.FreedBytes == before.FreedBytes + 64 * sizeof(uint32));
}

TEST_CASE("Array Clear retains capacity and TrimCapacity of an empty array frees it", "[containers][accounting]")
{
    Array<uint32> values;
    values.EnsureCapacity(128);
    const AllocationStatistics reserved = SampleContainers();
    values.Add(1);
    values.Clear();
    REQUIRE(SampleContainers().LiveRequestedBytes == reserved.LiveRequestedBytes);

    values.TrimCapacity();
    const AllocationStatistics trimmed = SampleContainers();
    REQUIRE(trimmed.LiveRequestedBytes == reserved.LiveRequestedBytes - 128 * sizeof(uint32));
    REQUIRE(trimmed.LiveBlocks == reserved.LiveBlocks - 1);
    REQUIRE(trimmed.AllocationCount == reserved.AllocationCount);
}

TEST_CASE("Array copies allocate only the copied size", "[containers][accounting]")
{
    Array<uint32> source;
    source.EnsureCapacity(256);
    for (uint32 i = 0; i < 10; ++i)
    {
        source.Add(i);
    }
    const AllocationStatistics before = SampleContainers();
    {
        // The copy's own allocation is what this case measures.
        // NOLINTNEXTLINE(performance-unnecessary-copy-initialization)
        const Array<uint32> copy(source);
        const AllocationStatistics during = SampleContainers();
        REQUIRE(during.AllocationCount == before.AllocationCount + 1);
        REQUIRE(during.LiveRequestedBytes - before.LiveRequestedBytes == copy.GetCapacity() * sizeof(uint32));
        REQUIRE(copy.GetCapacity() == source.GetSize());
    }
    REQUIRE(SampleContainers().LiveRequestedBytes == before.LiveRequestedBytes);
}

TEST_CASE("Over-aligned Array storage is charged its requested capacity bytes", "[containers][accounting]")
{
    const AllocationStatistics before = SampleContainers();
    {
        Array<OverAligned> values;
        values.EnsureCapacity(5);
        REQUIRE(reinterpret_cast<usize>(values.GetData()) % alignof(OverAligned) == 0);
        REQUIRE(SampleContainers().LiveRequestedBytes - before.LiveRequestedBytes == 5 * sizeof(OverAligned));
    }
    REQUIRE(SampleContainers().LiveRequestedBytes == before.LiveRequestedBytes);
}

TEST_CASE("An Array destroyed on another thread charges the Containers domain", "[containers][accounting]")
{
    const AllocationStatistics before = SampleContainers();
    Array<uint32> values;
    values.EnsureCapacity(512);
    std::thread destroyer([owned = Move(values)]() mutable { owned.TrimCapacity(); });
    destroyer.join();
    const AllocationStatistics after = SampleContainers();
    REQUIRE(after.LiveRequestedBytes == before.LiveRequestedBytes);
    REQUIRE(after.LiveBlocks == before.LiveBlocks);
    REQUIRE(after.FreeCount == before.FreeCount + 1);
    REQUIRE(after.Validity == AccountingValidity::Valid);
}
