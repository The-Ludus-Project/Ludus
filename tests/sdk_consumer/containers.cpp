#include <ludus/foundation/containers/array.hpp>
#include <ludus/foundation/containers/fixed_array.hpp>
#include <ludus/foundation/containers/ring_buffer.hpp>
#include <ludus/foundation/containers/sorted_map.hpp>
#include <ludus/foundation/containers/static_bit_set.hpp>

int ExerciseInstalledContainers() noexcept
{
    using namespace ludus::foundation;
    SortedMap<uint64, uint32> resources;
    if (resources.TryEnsureCapacity(2) == false || resources.TryAddInPlace(30, 300U).Inserted == false ||
        resources.TryAddInPlace(10, 100U).Inserted == false)
    {
        return 30;
    }
    const uint32* value = resources.Find(30);
    if (value == nullptr || *value != 300 || resources.AsSpan()[0].Key != 10 || resources.Remove(10) == false)
    {
        return 31;
    }
    Array<uint32> values;
    if (values.TryInsertAtInPlace(0, 7U) == nullptr || values.TryInsertAt(0, values[0]) == nullptr ||
        values.GetSize() != 2 || values[1] != 7)
    {
        return 32;
    }
    constexpr usize FIXED_CAPACITY = 2;
    FixedArray<uint32, FIXED_CAPACITY> bounded;
    if (bounded.TryAdd(5U) == false || bounded.TryInsertAt(0, bounded[0]) == nullptr || bounded.IsFull() == false ||
        bounded.TryAdd(9U) == true || bounded[0] != 5 || bounded[1] != 5)
    {
        return 33;
    }
    constexpr usize RING_CAPACITY = 2;
    RingBuffer<uint32, RING_CAPACITY> ring;
    if (ring.TryAdd(1U) == false || ring.TryAdd(2U) == false || ring.TryAdd(3U) == true)
    {
        return 34;
    }
    ring.RemoveFirst();
    if (ring.TryAdd(3U) == false || ring.GetFirst() != 2 || ring.GetLast() != 3 || ring.GetTrailingSpan().size() != 1)
    {
        return 35;
    }
    constexpr usize FLAG_COUNT = 65;
    StaticBitSet<FLAG_COUNT> flags;
    flags.Set(64);
    flags <<= 0;
    if (flags.IsSet(64) == false || flags.CountSet() != 1 || flags.FindNextSet(0) != 64 || (~flags).CountSet() != 64)
    {
        return 36;
    }
    return 0;
}
