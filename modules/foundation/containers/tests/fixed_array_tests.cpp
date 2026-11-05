#include "lifetime_type.hpp"

#include <ludus/foundation/containers/fixed_array.hpp>

#include <ludus/foundation/containers/static_array.hpp>

#include <catch2/catch_test_macros.hpp>

#include <span>
#include <type_traits>
#include <utility>

using ludus::foundation::FixedArray;
using ludus::foundation::int32;
using ludus::foundation::uint32;
using ludus::foundation::usize;
namespace tst = ludus::containers::testing;

namespace
{
constexpr usize SMALL_CAPACITY = 4;
constexpr usize MODEL_CAPACITY = 16;

// Neither default-constructible nor copyable: only occupied slots may exist.
struct ConstructOnly
{
    int32 Value;
    explicit ConstructOnly(int32 value) noexcept : Value(value) {}
    ConstructOnly(const ConstructOnly&) = delete;
    ConstructOnly& operator=(const ConstructOnly&) = delete;
    ConstructOnly(ConstructOnly&&) noexcept = default;
    ConstructOnly& operator=(ConstructOnly&&) noexcept = default;
};

constexpr usize OVER_ALIGNMENT = 32;
constexpr usize OVER_ALIGNED_PADDING = OVER_ALIGNMENT - sizeof(uint32);

struct alignas(OVER_ALIGNMENT) OverAligned
{
    uint32 Value = 0;
    ludus::foundation::StaticArray<ludus::foundation::uint8, OVER_ALIGNED_PADDING> Padding{};
};

static_assert(std::is_trivially_destructible_v<FixedArray<uint32, SMALL_CAPACITY>>);
static_assert(std::is_trivially_destructible_v<FixedArray<uint32, 0>>);
static_assert(std::is_trivially_destructible_v<FixedArray<tst::Tracked, SMALL_CAPACITY>> == false);
static_assert(std::is_copy_constructible_v<FixedArray<tst::MoveOnly, SMALL_CAPACITY>> == false);
static_assert(std::is_nothrow_move_constructible_v<FixedArray<tst::MoveOnly, SMALL_CAPACITY>>);
static_assert(FixedArray<uint32, SMALL_CAPACITY>::GetCapacity() == SMALL_CAPACITY);
static_assert(alignof(FixedArray<OverAligned, SMALL_CAPACITY>) >= alignof(OverAligned));
} // namespace

TEST_CASE("FixedArray reports full as a status and leaves the array unchanged", "[containers][fixed-array]")
{
    FixedArray<uint32, SMALL_CAPACITY> values;
    REQUIRE(values.IsEmpty());
    REQUIRE(values.IsFull() == false);
    for (uint32 i = 0; i < SMALL_CAPACITY; ++i)
    {
        REQUIRE(values.TryAdd(i * 10U));
    }
    REQUIRE(values.IsFull());
    REQUIRE(values.TryAdd(99U) == false);
    REQUIRE(values.TryAddInPlace(99U) == nullptr);
    REQUIRE(values.TryInsertAt(0, 99U) == nullptr);
    REQUIRE(values.GetSize() == SMALL_CAPACITY);
    for (uint32 i = 0; i < SMALL_CAPACITY; ++i)
    {
        REQUIRE(values[i] == i * 10U);
    }
    REQUIRE(values.GetFirst() == 0U);
    REQUIRE(values.GetLast() == 30U);

    values.RemoveLast();
    REQUIRE(values.IsFull() == false);
    REQUIRE(values.TryAdd(77U));
    REQUIRE(values.GetLast() == 77U);
    values.Clear();
    REQUIRE(values.IsEmpty());
    REQUIRE(values.GetCapacity() == SMALL_CAPACITY);
}

TEST_CASE("FixedArray with zero capacity is empty and always full", "[containers][fixed-array]")
{
    FixedArray<tst::Tracked, 0> values;
    REQUIRE(values.GetSize() == 0);
    REQUIRE(values.GetCapacity() == 0);
    REQUIRE(values.IsEmpty());
    REQUIRE(values.IsFull());
    REQUIRE(values.GetData() == nullptr);
    REQUIRE(values.begin() == values.end());
    REQUIRE(values.AsSpan().empty());
    tst::LifetimeLedger ledger;
    {
        tst::Tracked candidate(&ledger, 1);
        REQUIRE(values.TryAdd(candidate) == false);
        const bool isMovedIn = values.TryAdd(std::move(candidate));
        REQUIRE(isMovedIn == false);
        // NOLINTNEXTLINE(bugprone-use-after-move): a rejected add must not consume the argument.
        REQUIRE(candidate.Value() == 1);
        // NOLINTNEXTLINE(bugprone-use-after-move): the unconsumed argument is still readable.
        REQUIRE(values.TryAddRange(std::span<const tst::Tracked>(&candidate, 1)) == false);
        REQUIRE(values.TryAddRange({}));
    }
    REQUIRE(ledger.Constructions() == 1);
    REQUIRE(ledger.Balanced());
    const FixedArray<tst::Tracked, 0> copy = values;
    REQUIRE(copy == values);
}

TEST_CASE("FixedArray constructs only occupied slots", "[containers][fixed-array][lifetime]")
{
    tst::LifetimeLedger ledger;
    {
        FixedArray<tst::Tracked, MODEL_CAPACITY> values;
        REQUIRE(ledger.Constructions() == 0);
        REQUIRE(values.TryAddInPlace(&ledger, 1) != nullptr);
        REQUIRE(values.TryAddInPlace(&ledger, 2) != nullptr);
        REQUIRE(values.TryAddInPlace(&ledger, 3) != nullptr);
        REQUIRE(ledger.ValueCtor == 3);
        REQUIRE(ledger.Live() == 3);
        values.RemoveAt(0);
        REQUIRE(ledger.Live() == 2);
        REQUIRE(values[0].Value() == 2);
        REQUIRE(values[1].Value() == 3);
    }
    REQUIRE(ledger.Balanced());
}

TEST_CASE("FixedArray holds non-default-constructible and move-only values", "[containers][fixed-array]")
{
    FixedArray<ConstructOnly, SMALL_CAPACITY> constructOnly;
    REQUIRE(constructOnly.TryAddInPlace(5) != nullptr);
    REQUIRE(constructOnly.TryInsertAtInPlace(0, 4) != nullptr);
    REQUIRE(constructOnly[0].Value == 4);
    REQUIRE(constructOnly[1].Value == 5);

    tst::LifetimeLedger ledger;
    {
        FixedArray<tst::MoveOnly, 2> moveOnly;
        tst::MoveOnly first(&ledger, 1);
        tst::MoveOnly second(&ledger, 2);
        tst::MoveOnly third(&ledger, 3);
        const bool isFirstAdded = moveOnly.TryAdd(std::move(first));
        const bool isSecondAdded = moveOnly.TryAdd(std::move(second));
        const bool isThirdAdded = moveOnly.TryAdd(std::move(third));
        REQUIRE(isFirstAdded);
        REQUIRE(isSecondAdded);
        REQUIRE(isThirdAdded == false);
        // NOLINTNEXTLINE(bugprone-use-after-move): a rejected add must not consume the argument.
        REQUIRE(third.Value() == 3);

        FixedArray<tst::MoveOnly, 2> moved(std::move(moveOnly));
        // NOLINTNEXTLINE(bugprone-use-after-move): verify the specified empty moved-from state.
        REQUIRE(moveOnly.IsEmpty());
        REQUIRE(moved.GetSize() == 2);
        REQUIRE(moved[0].Value() == 1);
        REQUIRE(moved[1].Value() == 2);
    }
    REQUIRE(ledger.Balanced());
}

TEST_CASE("FixedArray aliased arguments survive append and ordered insertion", "[containers][fixed-array][alias]")
{
    tst::LifetimeLedger ledger;
    {
        FixedArray<tst::Tracked, SMALL_CAPACITY> values;
        REQUIRE(values.TryAddInPlace(&ledger, 1) != nullptr);
        REQUIRE(values.TryAddInPlace(&ledger, 2) != nullptr);
        REQUIRE(values.TryAdd(values[0]));
        REQUIRE(values[2].Value() == 1);
        REQUIRE(values.TryInsertAt(0, values.GetLast()) != nullptr);
        REQUIRE(values.GetSize() == 4);
        REQUIRE(values[0].Value() == 1);
        REQUIRE(values[1].Value() == 1);
        REQUIRE(values[2].Value() == 2);
        REQUIRE(values[3].Value() == 1);

        // Full: the aliased argument and every element stay intact.
        REQUIRE(values.TryInsertAt(1, std::move(values[2])) == nullptr);
        REQUIRE(values[2].Value() == 2);
        REQUIRE(values.TryAdd(values[0]) == false);
    }
    REQUIRE(ledger.Balanced());
}

TEST_CASE("FixedArray ordered and swap removal", "[containers][fixed-array]")
{
    FixedArray<int32, MODEL_CAPACITY> values;
    for (int32 i = 0; i < 6; ++i)
    {
        REQUIRE(values.TryAdd(i));
    }
    values.RemoveAt(1);     // 0 2 3 4 5
    values.RemoveAtSwap(0); // 5 2 3 4
    values.RemoveAtSwap(3); // 5 2 3
    REQUIRE(values.GetSize() == 3);
    REQUIRE(values[0] == 5);
    REQUIRE(values[1] == 2);
    REQUIRE(values[2] == 3);
    REQUIRE(values.TryInsertAt(3, 9) != nullptr);
    REQUIRE(values.TryInsertAt(1, 8) != nullptr); // 5 8 2 3 9
    const int32 expected[] = {5, 8, 2, 3, 9};
    REQUIRE(values.GetSize() == 5);
    for (usize i = 0; i < values.GetSize(); ++i)
    {
        REQUIRE(values[i] == expected[i]);
    }
}

TEST_CASE("FixedArray range append is all or nothing", "[containers][fixed-array]")
{
    FixedArray<uint32, SMALL_CAPACITY> values;
    const uint32 three[] = {1, 2, 3};
    REQUIRE(values.TryAddRange(three));
    REQUIRE(values.TryAddRange(three) == false);
    REQUIRE(values.GetSize() == 3);
    const uint32 one[] = {4};
    REQUIRE(values.TryAddRange(one));
    REQUIRE(values.IsFull());
    REQUIRE(values.TryAddRange({}));
    const std::span<const uint32> view = values;
    REQUIRE(view.size() == SMALL_CAPACITY);
    REQUIRE(view[3] == 4U);
}

TEST_CASE("FixedArray copy and move keep lifetimes balanced", "[containers][fixed-array][lifetime]")
{
    tst::LifetimeLedger ledger;
    {
        FixedArray<tst::Tracked, SMALL_CAPACITY> source;
        REQUIRE(source.TryAddInPlace(&ledger, 1) != nullptr);
        REQUIRE(source.TryAddInPlace(&ledger, 2) != nullptr);
        REQUIRE(source.TryAddInPlace(&ledger, 3) != nullptr);

        FixedArray<tst::Tracked, SMALL_CAPACITY> copy(source);
        REQUIRE(copy == source);

        FixedArray<tst::Tracked, SMALL_CAPACITY> shorter;
        REQUIRE(shorter.TryAddInPlace(&ledger, 9) != nullptr);
        shorter = source; // grows: assign one, construct two
        REQUIRE(shorter == source);

        FixedArray<tst::Tracked, SMALL_CAPACITY> longer = source;
        REQUIRE(longer.TryAddInPlace(&ledger, 4) != nullptr);
        longer = shorter; // shrinks: destroys the surplus
        REQUIRE(longer.GetSize() == 3);

        const FixedArray<tst::Tracked, SMALL_CAPACITY>& self = copy;
        copy = self;
        REQUIRE(copy.GetSize() == 3);

        FixedArray<tst::Tracked, SMALL_CAPACITY> moved(std::move(copy));
        // NOLINTNEXTLINE(bugprone-use-after-move): verify the specified empty moved-from state.
        REQUIRE(copy.IsEmpty());
        REQUIRE(moved == source);

        FixedArray<tst::Tracked, SMALL_CAPACITY> moveTarget;
        REQUIRE(moveTarget.TryAddInPlace(&ledger, 7) != nullptr);
        moveTarget = std::move(moved);
        // NOLINTNEXTLINE(bugprone-use-after-move): verify the specified empty moved-from state.
        REQUIRE(moved.IsEmpty());
        REQUIRE(moveTarget == source);

        FixedArray<tst::Tracked, SMALL_CAPACITY> moveShrink = source;
        REQUIRE(moveShrink.TryAddInPlace(&ledger, 5) != nullptr);
        FixedArray<tst::Tracked, SMALL_CAPACITY> one;
        REQUIRE(one.TryAddInPlace(&ledger, 6) != nullptr);
        moveShrink = std::move(one);
        REQUIRE(moveShrink.GetSize() == 1);
        REQUIRE(moveShrink[0].Value() == 6);
        // NOLINTNEXTLINE(bugprone-use-after-move): verify the specified empty moved-from state.
        REQUIRE(one.IsEmpty());
    }
    REQUIRE(ledger.Balanced());
}

TEST_CASE("FixedArray over-aligned elements are aligned", "[containers][fixed-array][alignment]")
{
    FixedArray<OverAligned, SMALL_CAPACITY> values;
    REQUIRE(values.TryAdd(OverAligned{ .Value = 1 }));
    REQUIRE(values.TryAdd(OverAligned{ .Value = 2 }));
    for (const OverAligned& value : values)
    {
        REQUIRE(reinterpret_cast<usize>(&value) % alignof(OverAligned) == 0);
    }
}

TEST_CASE("FixedArray seeded operations match an independent model", "[containers][fixed-array][model]")
{
    // The model is a plain array plus count with hand-written shifts.
    int32 model[MODEL_CAPACITY] = {};
    usize modelSize = 0;
    tst::LifetimeLedger ledger;
    {
        FixedArray<tst::Tracked, MODEL_CAPACITY> values;
        uint32 state = 0x2545F491U;
        for (int32 operation = 0; operation < 20000; ++operation)
        {
            state = state * 1664525U + 1013904223U;
            const uint32 choice = (state >> 24U) % 6U;
            const usize position = modelSize == 0 ? 0 : (state >> 8U) % (modelSize + 1);
            if (choice <= 1)
            {
                const bool isAdded = values.TryAddInPlace(&ledger, operation) != nullptr;
                REQUIRE(isAdded == (modelSize < MODEL_CAPACITY));
                if (isAdded == true)
                {
                    model[modelSize] = operation;
                    ++modelSize;
                }
            }
            else if (choice == 2)
            {
                const bool isInserted = values.TryInsertAtInPlace(position, &ledger, operation) != nullptr;
                REQUIRE(isInserted == (modelSize < MODEL_CAPACITY));
                if (isInserted == true)
                {
                    for (usize i = modelSize; i > position; --i)
                    {
                        model[i] = model[i - 1];
                    }
                    model[position] = operation;
                    ++modelSize;
                }
            }
            else if (choice == 3 && modelSize > 0)
            {
                const usize index = position == modelSize ? modelSize - 1 : position;
                values.RemoveAt(index);
                for (usize i = index; i + 1 < modelSize; ++i)
                {
                    model[i] = model[i + 1];
                }
                --modelSize;
            }
            else if (choice == 4 && modelSize > 0)
            {
                const usize index = position == modelSize ? modelSize - 1 : position;
                values.RemoveAtSwap(index);
                model[index] = model[modelSize - 1];
                --modelSize;
            }
            else if (choice == 5 && (state & 0xFFU) == 0)
            {
                values.Clear();
                modelSize = 0;
            }
            REQUIRE(values.GetSize() == modelSize);
            REQUIRE(ledger.Live() == static_cast<long>(modelSize));
            for (usize i = 0; i < modelSize; ++i)
            {
                REQUIRE(values[i].Value() == model[i]);
            }
        }
    }
    REQUIRE(ledger.Balanced());
}
