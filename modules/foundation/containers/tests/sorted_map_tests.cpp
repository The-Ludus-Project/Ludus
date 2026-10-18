#include "lifetime_type.hpp"

#include <ludus/foundation/containers/array.hpp>
#include <ludus/foundation/containers/sorted_map.hpp>
#include <ludus/foundation/containers/static_array.hpp>

#include <catch2/catch_test_macros.hpp>

#include <type_traits>
#include <utility>

using ludus::foundation::Array;
using ludus::foundation::int32;
using ludus::foundation::SortedMap;
using ludus::foundation::StaticArray;
using ludus::foundation::uint32;
using ludus::foundation::uint64;
using ludus::foundation::usize;
namespace tst = ludus::containers::testing;

namespace
{
struct MoveOnly
{
    int32 Value;
    explicit MoveOnly(int32 value) noexcept : Value(value) {}
    MoveOnly(const MoveOnly&) = delete;
    MoveOnly& operator=(const MoveOnly&) = delete;
    MoveOnly(MoveOnly&& other) noexcept : Value(other.Value)
    {
        other.Value = -1;
    }
    // The self-check preserves ownership on self move, as real owning values do.
    MoveOnly& operator=(MoveOnly&& other) noexcept
    {
        if (this != &other)
        {
            Value = other.Value;
            other.Value = -1;
        }
        return *this;
    }
};
struct ConstructOnly
{
    int32 Value;
    explicit ConstructOnly(int32 value) noexcept : Value(value) {}
    ConstructOnly(const ConstructOnly&) = delete;
    ConstructOnly& operator=(const ConstructOnly&) = delete;
    ConstructOnly(ConstructOnly&&) noexcept = default;
    ConstructOnly& operator=(ConstructOnly&&) = delete;
};
struct Descending
{
    [[nodiscard]] bool operator()(uint32 left, uint32 right) const noexcept
    {
        return left > right;
    }
};
struct DecadeOrder
{
    [[nodiscard]] bool operator()(uint32 left, uint32 right) const noexcept
    {
        return left / 10 < right / 10;
    }
};
} // namespace

TEST_CASE("SortedMap lookup, replacement and sorted iteration", "[containers][sorted-map]")
{
    SortedMap<uint64, int32> map;
    REQUIRE(map.IsEmpty());
    REQUIRE(map.begin() == map.end());
    REQUIRE(map.Find(42) == nullptr);
    REQUIRE_FALSE(map.Remove(42));
    REQUIRE(map.TryAddInPlace(30, 3).Inserted);
    REQUIRE(map.TryAddInPlace(10, 1).Inserted);
    REQUIRE(map.TryAddInPlace(20, 2).Inserted);
    auto duplicate = map.TryAddInPlace(20, 999);
    REQUIRE_FALSE(duplicate.Inserted);
    REQUIRE(*duplicate.Value == 2);
    *map.Find(20) = 7;
    const auto& readOnly = map;
    REQUIRE(*readOnly.Find(20) == 7);
    REQUIRE(readOnly.Find(0) == nullptr);
    REQUIRE(readOnly.Find(40) == nullptr);
    REQUIRE_FALSE(readOnly.Contains(15));
    STATIC_REQUIRE(std::is_same_v<decltype(readOnly.Find(20)), const int32*>);
    STATIC_REQUIRE(std::is_const_v<std::remove_reference_t<decltype(*map.begin())>>);
    const auto entries = readOnly.AsSpan();
    REQUIRE(entries.size() == 3);
    REQUIRE(entries[0].Key == 10);
    REQUIRE(entries[1].Key == 20);
    REQUIRE(entries[2].Key == 30);
    REQUIRE(map.Remove(20));
    REQUIRE(map.GetSize() == 2);
    REQUIRE(map.Remove(10));
    REQUIRE(map.Remove(30));
    REQUIRE(map.IsEmpty());
    const usize capacity = map.GetCapacity();
    map.Clear();
    REQUIRE(map.GetCapacity() == capacity);
    map.Reset();
    REQUIRE(map.GetCapacity() == 0);
    REQUIRE(map.begin() == nullptr);
}

TEST_CASE("SortedMap comparator defines both ordering and equivalence", "[containers][sorted-map]")
{
    SortedMap<uint32, int32, Descending> descending;
    descending.AddInPlace(3, 30);
    descending.AddInPlace(8, 80);
    descending.AddInPlace(1, 10);
    REQUIRE(descending.AsSpan()[0].Key == 8);
    REQUIRE(*descending.Find(3) == 30);
    REQUIRE(descending.Find(9) == nullptr);
    REQUIRE(descending.Find(0) == nullptr);

    SortedMap<uint32, int32, DecadeOrder> equivalent;
    equivalent.AddInPlace(11, 1);
    auto result = equivalent.TryAddInPlace(19, 9);
    REQUIRE_FALSE(result.Inserted);
    REQUIRE(*result.Value == 1);
    REQUIRE(equivalent.AsSpan()[0].Key == 11);
    REQUIRE(equivalent.Remove(15));
    REQUIRE(equivalent.IsEmpty());
}

TEST_CASE("SortedMap move-only values and duplicate arguments", "[containers][sorted-map]")
{
    STATIC_REQUIRE_FALSE(std::is_copy_constructible_v<SortedMap<uint32, MoveOnly>>);
    STATIC_REQUIRE_FALSE(std::is_copy_assignable_v<SortedMap<uint32, MoveOnly>>);
    STATIC_REQUIRE(std::is_nothrow_move_constructible_v<SortedMap<uint32, MoveOnly>>);
    SortedMap<uint32, MoveOnly> map;
    map.AddInPlace(5, 50);
    MoveOnly supplied(99);
    auto duplicate = map.TryAddInPlace(5, std::move(supplied));
    REQUIRE_FALSE(duplicate.Inserted);
    // NOLINTNEXTLINE(bugprone-use-after-move): duplicate insertion must not consume the argument.
    REQUIRE(supplied.Value == 99);
    auto added = map.TryAddInPlace(3, std::move(supplied));
    REQUIRE(added.Inserted);
    // NOLINTNEXTLINE(bugprone-use-after-move): this test type explicitly exposes its moved-from marker.
    REQUIRE(supplied.Value == -1);
    REQUIRE(added.Value->Value == 99);
    REQUIRE(map.Remove(3));
    REQUIRE(map.Find(5)->Value == 50);
    SortedMap<uint32, MoveOnly> moved(std::move(map));
    // NOLINTNEXTLINE(bugprone-use-after-move): verify the container's specified empty moved-from state.
    REQUIRE(map.IsEmpty());
    REQUIRE(map.GetCapacity() == 0);
    REQUIRE(moved.Find(5)->Value == 50);
    map = std::move(moved);
    // NOLINTNEXTLINE(bugprone-use-after-move): verify the specified empty moved-from state.
    REQUIRE(moved.IsEmpty());
    REQUIRE(map.Find(5)->Value == 50);
}

TEST_CASE("SortedMap aliased keys and values survive growth and shifts", "[containers][sorted-map]")
{
    for (const bool reserve : {false, true})
    {
        SortedMap<uint32, uint32> map;
        if (reserve)
        {
            map.EnsureCapacity(16);
        }
        else
        {
            REQUIRE(map.TryEnsureCapacity(2));
        }
        map.AddInPlace(10, 30U);
        map.AddInPlace(20, 40U);
        // Both arguments borrow existing storage and the new key is after it.
        const uint32& key = *map.Find(10);
        const uint32& value = *map.Find(20);
        REQUIRE(map.TryAddInPlace(key, value).Inserted);
        REQUIRE(*map.Find(30) == 40);
        // This insertion shifts the borrowed source to a new position.
        REQUIRE(map.TryAddInPlace(5, *map.Find(30)).Inserted);
        REQUIRE(*map.Find(5) == 40);
    }
}

TEST_CASE("SortedMap lifetimes balance through copy, move and removal", "[containers][sorted-map]")
{
    tst::LifetimeLedger ledger;
    {
        SortedMap<uint32, tst::Tracked> map;
        for (uint32 key = 32; key != 0; --key)
        {
            map.AddInPlace(key, &ledger, static_cast<int32>(key));
        }
        SortedMap<uint32, tst::Tracked> copy(map);
        SortedMap<uint32, tst::Tracked> assigned;
        assigned = copy;
        REQUIRE(copy.Remove(12));
        assigned.Swap(copy);
        map.Clear();
        REQUIRE(map.GetSize() == 0);
        assigned.Reset();
        copy = std::move(map);
    }
    REQUIRE(ledger.Balanced());
}

TEST_CASE("SortedMap seeded operations match an independent direct-index model", "[containers][sorted-map]")
{
    StaticArray<bool, 64> present{};
    StaticArray<uint32, 64> values{};
    SortedMap<uint32, uint32> map;
    uint32 state = 0x31415926U;
    for (usize operation = 0; operation < 10000; ++operation)
    {
        state = state * 1664525U + 1013904223U;
        const uint32 key = (state >> 16U) % 64U;
        if ((state & 3U) == 0)
        {
            REQUIRE(map.Remove(key) == present[key]);
            present[key] = false;
        }
        else
        {
            const auto result = map.TryAddInPlace(key, state);
            REQUIRE(result.Value != nullptr);
            REQUIRE(result.Inserted == (present[key] == false));
            if (present[key] == false)
            {
                present[key] = true;
                values[key] = state;
            }
            REQUIRE(*result.Value == values[key]);
        }
        usize index = 0;
        for (uint32 expected = 0; expected < 64; ++expected)
        {
            REQUIRE(map.Contains(expected) == present[expected]);
            if (present[expected])
            {
                REQUIRE(map.AsSpan()[index].Key == expected);
                REQUIRE(map.AsSpan()[index].Value == values[expected]);
                ++index;
            }
        }
        REQUIRE(map.GetSize() == index);
    }
}

TEST_CASE("Array fallible insertion preserves lifetime and aliased values", "[containers][array][insert]")
{
    tst::LifetimeLedger ledger;
    {
        Array<tst::Tracked> values;
        REQUIRE(values.TryInsertAtInPlace(0, &ledger, 10) != nullptr);
        values.TrimCapacity();
        REQUIRE(values.TryInsertAt(0, values[0]) != nullptr);
        values.EnsureCapacity(16);
        REQUIRE(values.TryInsertAt(1, values[0]) != nullptr);
        REQUIRE(values.TryInsertAt(3, values[0]) != nullptr);
        REQUIRE(values.GetSize() == 4);
        REQUIRE(ledger.Live() == 4);
    }
    REQUIRE(ledger.Balanced());
}

TEST_CASE("Array fallible append still supports non-assignable values", "[containers][array][insert]")
{
    Array<ConstructOnly> values;
    values.EnsureCapacity(1);
    REQUIRE(values.TryAddInPlace(10) != nullptr);
    REQUIRE(values.TryAddInPlace(20) != nullptr);
    REQUIRE(values.GetSize() == 2);
    REQUIRE(values[0].Value == 10);
    REQUIRE(values[1].Value == 20);
}
