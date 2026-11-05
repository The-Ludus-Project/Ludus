#include "lifetime_type.hpp"

#include <ludus/foundation/containers/ring_buffer.hpp>

#include <ludus/foundation/containers/static_array.hpp>

#include <catch2/catch_test_macros.hpp>

#include <span>
#include <type_traits>
#include <utility>

using ludus::foundation::int32;
using ludus::foundation::RingBuffer;
using ludus::foundation::uint32;
using ludus::foundation::usize;
namespace tst = ludus::containers::testing;

namespace
{
constexpr usize SMALL_CAPACITY = 4;
constexpr usize MODEL_CAPACITY = 7; // not a power of two, so wrapping is not masking

struct ConstructOnly
{
    int32 Value;
    explicit ConstructOnly(int32 value) noexcept : Value(value) {}
    ConstructOnly(const ConstructOnly&) = delete;
    ConstructOnly& operator=(const ConstructOnly&) = delete;
    ConstructOnly(ConstructOnly&&) noexcept = default;
    ConstructOnly& operator=(ConstructOnly&&) = delete;
};

constexpr usize OVER_ALIGNMENT = 32;
constexpr usize OVER_ALIGNED_PADDING = OVER_ALIGNMENT - sizeof(uint32);

struct alignas(OVER_ALIGNMENT) OverAligned
{
    uint32 Value = 0;
    ludus::foundation::StaticArray<ludus::foundation::uint8, OVER_ALIGNED_PADDING> Padding{};
};

static_assert(std::is_trivially_destructible_v<RingBuffer<uint32, SMALL_CAPACITY>>);
static_assert(std::is_trivially_destructible_v<RingBuffer<uint32, 0>>);
static_assert(std::is_trivially_destructible_v<RingBuffer<tst::Tracked, SMALL_CAPACITY>> == false);
static_assert(std::is_copy_constructible_v<RingBuffer<tst::MoveOnly, SMALL_CAPACITY>> == false);
static_assert(std::is_nothrow_move_constructible_v<RingBuffer<tst::MoveOnly, SMALL_CAPACITY>>);
static_assert(RingBuffer<uint32, SMALL_CAPACITY>::GetCapacity() == SMALL_CAPACITY);

// Collect the two spans in FIFO order.
template <usize Count>
[[nodiscard]] bool SpansEqual(const RingBuffer<int32, Count>& ring, std::span<const int32> expected)
{
    const std::span<const int32> leading = ring.GetLeadingSpan();
    const std::span<const int32> trailing = ring.GetTrailingSpan();
    if (leading.size() + trailing.size() != expected.size())
    {
        return false;
    }
    for (usize i = 0; i < leading.size(); ++i)
    {
        if (leading[i] != expected[i])
        {
            return false;
        }
    }
    for (usize i = 0; i < trailing.size(); ++i)
    {
        if (trailing[i] != expected[leading.size() + i])
        {
            return false;
        }
    }
    return true;
}
} // namespace

TEST_CASE("RingBuffer is first in, first out and reports full as a status", "[containers][ring-buffer]")
{
    RingBuffer<int32, SMALL_CAPACITY> ring;
    REQUIRE(ring.IsEmpty());
    REQUIRE(ring.IsFull() == false);
    for (int32 i = 1; i <= 4; ++i)
    {
        REQUIRE(ring.TryAdd(i));
    }
    REQUIRE(ring.IsFull());
    REQUIRE(ring.TryAdd(5) == false);
    REQUIRE(ring.TryAddInPlace(5) == nullptr);
    REQUIRE(ring.GetSize() == 4);
    REQUIRE(ring.GetFirst() == 1);
    REQUIRE(ring.GetLast() == 4);

    ring.RemoveFirst();
    REQUIRE(ring.TryAdd(5)); // wraps into the first slot
    REQUIRE(ring.GetFirst() == 2);
    REQUIRE(ring.GetLast() == 5);
    for (usize i = 0; i < ring.GetSize(); ++i)
    {
        REQUIRE(ring[i] == static_cast<int32>(i) + 2);
    }
    ring.Clear();
    REQUIRE(ring.IsEmpty());
    REQUIRE(ring.GetLeadingSpan().empty());
    REQUIRE(ring.GetTrailingSpan().empty());
}

TEST_CASE("RingBuffer exposes wrapped storage as two contiguous spans", "[containers][ring-buffer]")
{
    RingBuffer<int32, SMALL_CAPACITY> ring;
    const int32 none[] = {0};
    REQUIRE(SpansEqual(ring, std::span<const int32>(none, 0)));
    REQUIRE(ring.TryAdd(1));
    REQUIRE(ring.TryAdd(2));
    REQUIRE(ring.TryAdd(3));
    const int32 unwrapped[] = {1, 2, 3};
    REQUIRE(ring.GetTrailingSpan().empty());
    REQUIRE(SpansEqual(ring, unwrapped));

    ring.RemoveFirst();
    ring.RemoveFirst();
    REQUIRE(ring.TryAdd(4));
    REQUIRE(ring.TryAdd(5));
    REQUIRE(ring.TryAdd(6));
    const int32 wrapped[] = {3, 4, 5, 6};
    REQUIRE(ring.IsFull());
    REQUIRE(ring.GetLeadingSpan().size() == 2);
    REQUIRE(ring.GetTrailingSpan().size() == 2);
    REQUIRE(SpansEqual(ring, wrapped));

    // Draining restarts at the first slot, so the next run is contiguous again.
    while (ring.IsEmpty() == false)
    {
        ring.RemoveFirst();
    }
    REQUIRE(ring.TryAdd(7));
    REQUIRE(ring.GetLeadingSpan().data() == &ring.GetFirst());
    REQUIRE(ring.GetTrailingSpan().empty());
}

TEST_CASE("RingBuffer with zero capacity is empty and always full", "[containers][ring-buffer]")
{
    RingBuffer<tst::Tracked, 0> ring;
    REQUIRE(ring.IsEmpty());
    REQUIRE(ring.IsFull());
    REQUIRE(ring.GetCapacity() == 0);
    REQUIRE(ring.GetLeadingSpan().empty());
    REQUIRE(ring.GetTrailingSpan().empty());
    tst::LifetimeLedger ledger;
    {
        tst::Tracked candidate(&ledger, 1);
        REQUIRE(ring.TryAdd(candidate) == false);
        const bool isMovedIn = ring.TryAdd(std::move(candidate));
        REQUIRE(isMovedIn == false);
        // NOLINTNEXTLINE(bugprone-use-after-move): a rejected add must not consume the argument.
        REQUIRE(candidate.Value() == 1);
    }
    REQUIRE(ledger.Balanced());
    ring.Clear();
    const RingBuffer<tst::Tracked, 0> copy = ring;
    REQUIRE(copy == ring);
}

TEST_CASE("RingBuffer constructs only occupied slots and holds move-only values", "[containers][ring-buffer][lifetime]")
{
    tst::LifetimeLedger ledger;
    {
        RingBuffer<tst::Tracked, MODEL_CAPACITY> ring;
        REQUIRE(ledger.Constructions() == 0);
        REQUIRE(ring.TryAddInPlace(&ledger, 1) != nullptr);
        REQUIRE(ring.TryAddInPlace(&ledger, 2) != nullptr);
        REQUIRE(ledger.Live() == 2);
        ring.RemoveFirst();
        REQUIRE(ledger.Live() == 1);
        REQUIRE(ring.GetFirst().Value() == 2);
    }
    REQUIRE(ledger.Balanced());

    RingBuffer<ConstructOnly, SMALL_CAPACITY> constructOnly;
    REQUIRE(constructOnly.TryAddInPlace(3) != nullptr);
    REQUIRE(constructOnly.GetFirst().Value == 3);

    {
        RingBuffer<tst::MoveOnly, 2> moveOnly;
        tst::MoveOnly first(&ledger, 1);
        tst::MoveOnly second(&ledger, 2);
        tst::MoveOnly third(&ledger, 3);
        const bool isFirstAdded = moveOnly.TryAdd(std::move(first));
        const bool isSecondAdded = moveOnly.TryAdd(std::move(second));
        const bool isThirdAdded = moveOnly.TryAdd(std::move(third));
        REQUIRE(isFirstAdded);
        REQUIRE(isSecondAdded);
        REQUIRE(isThirdAdded == false);
        // NOLINTNEXTLINE(bugprone-use-after-move): a full ring must not consume the argument.
        REQUIRE(third.Value() == 3);
        moveOnly.RemoveFirst();
        REQUIRE(moveOnly.TryAdd(tst::MoveOnly(&ledger, 4))); // wraps
        RingBuffer<tst::MoveOnly, 2> moved(std::move(moveOnly));
        // NOLINTNEXTLINE(bugprone-use-after-move): verify the specified empty moved-from state.
        REQUIRE(moveOnly.IsEmpty());
        REQUIRE(moved.GetFirst().Value() == 2);
        REQUIRE(moved.GetLast().Value() == 4);
    }
    REQUIRE(ledger.Balanced());
}

TEST_CASE("RingBuffer aliased arguments and stable borrows", "[containers][ring-buffer][alias]")
{
    tst::LifetimeLedger ledger;
    {
        RingBuffer<tst::Tracked, SMALL_CAPACITY> ring;
        REQUIRE(ring.TryAddInPlace(&ledger, 1) != nullptr);
        REQUIRE(ring.TryAddInPlace(&ledger, 2) != nullptr);
        const tst::Tracked* second = &ring[1];
        REQUIRE(ring.TryAdd(ring.GetFirst()));
        ring.RemoveFirst();
        REQUIRE(ring.TryAdd(ring.GetLast()));
        REQUIRE(ring.TryAdd(ring[0])); // wraps; no live element moves
        REQUIRE(&ring[0] == second);
        REQUIRE(ring.IsFull());
        REQUIRE(ring.TryAdd(ring.GetFirst()) == false);
        REQUIRE(ring[0].Value() == 2);
        REQUIRE(ring[1].Value() == 1);
        REQUIRE(ring[2].Value() == 1);
        REQUIRE(ring[3].Value() == 2);
    }
    REQUIRE(ledger.Balanced());
}

TEST_CASE("RingBuffer copy and move of wrapped contents keep lifetimes balanced", "[containers][ring-buffer][lifetime]")
{
    tst::LifetimeLedger ledger;
    {
        RingBuffer<tst::Tracked, SMALL_CAPACITY> source;
        for (int32 i = 0; i < 4; ++i)
        {
            REQUIRE(source.TryAddInPlace(&ledger, i) != nullptr);
        }
        source.RemoveFirst();
        source.RemoveFirst();
        REQUIRE(source.TryAddInPlace(&ledger, 4) != nullptr); // contents 2 3 4, wrapped
        REQUIRE(source.GetTrailingSpan().size() == 1);

        RingBuffer<tst::Tracked, SMALL_CAPACITY> copy(source);
        REQUIRE(copy == source);
        REQUIRE(copy.GetTrailingSpan().empty()); // copies start at the first slot

        RingBuffer<tst::Tracked, SMALL_CAPACITY> assigned;
        REQUIRE(assigned.TryAddInPlace(&ledger, 9) != nullptr);
        assigned = source;
        REQUIRE(assigned == source);
        const RingBuffer<tst::Tracked, SMALL_CAPACITY>& self = assigned;
        assigned = self;
        REQUIRE(assigned.GetSize() == 3);

        RingBuffer<tst::Tracked, SMALL_CAPACITY> moved(std::move(copy));
        // NOLINTNEXTLINE(bugprone-use-after-move): verify the specified empty moved-from state.
        REQUIRE(copy.IsEmpty());
        REQUIRE(moved == source);

        RingBuffer<tst::Tracked, SMALL_CAPACITY> target;
        REQUIRE(target.TryAddInPlace(&ledger, 8) != nullptr);
        target = std::move(moved);
        // NOLINTNEXTLINE(bugprone-use-after-move): verify the specified empty moved-from state.
        REQUIRE(moved.IsEmpty());
        REQUIRE(target == source);
    }
    REQUIRE(ledger.Balanced());
}

TEST_CASE("RingBuffer over-aligned elements are aligned after wrapping", "[containers][ring-buffer][alignment]")
{
    RingBuffer<OverAligned, 3> ring;
    for (uint32 i = 0; i < 5; ++i)
    {
        if (ring.IsFull() == true)
        {
            ring.RemoveFirst();
        }
        REQUIRE(ring.TryAdd(OverAligned{ .Value = i }));
        REQUIRE(reinterpret_cast<usize>(&ring.GetLast()) % alignof(OverAligned) == 0);
    }
}

TEST_CASE("RingBuffer seeded operations match an independent model", "[containers][ring-buffer][model]")
{
    // The model is a plain array that shifts on every removal.
    int32 model[MODEL_CAPACITY] = {};
    usize modelSize = 0;
    tst::LifetimeLedger ledger;
    {
        RingBuffer<tst::Tracked, MODEL_CAPACITY> ring;
        uint32 state = 0x9E3779B9U;
        for (int32 operation = 0; operation < 20000; ++operation)
        {
            state = state * 1664525U + 1013904223U;
            const uint32 choice = (state >> 24U) % 8U;
            if (choice <= 3)
            {
                const bool isAdded = ring.TryAddInPlace(&ledger, operation) != nullptr;
                REQUIRE(isAdded == (modelSize < MODEL_CAPACITY));
                if (isAdded == true)
                {
                    model[modelSize] = operation;
                    ++modelSize;
                }
            }
            else if (choice <= 6 && modelSize > 0)
            {
                REQUIRE(ring.GetFirst().Value() == model[0]);
                ring.RemoveFirst();
                for (usize i = 0; i + 1 < modelSize; ++i)
                {
                    model[i] = model[i + 1];
                }
                --modelSize;
            }
            else if (choice == 7 && (state & 0x3FU) == 0)
            {
                ring.Clear();
                modelSize = 0;
            }
            REQUIRE(ring.GetSize() == modelSize);
            REQUIRE(ring.IsFull() == (modelSize == MODEL_CAPACITY));
            REQUIRE(ledger.Live() == static_cast<long>(modelSize));
            REQUIRE(ring.GetLeadingSpan().size() + ring.GetTrailingSpan().size() == modelSize);
            for (usize i = 0; i < modelSize; ++i)
            {
                REQUIRE(ring[i].Value() == model[i]);
            }
            const std::span<const tst::Tracked> leading = std::as_const(ring).GetLeadingSpan();
            const std::span<const tst::Tracked> trailing = std::as_const(ring).GetTrailingSpan();
            for (usize i = 0; i < leading.size(); ++i)
            {
                REQUIRE(leading[i].Value() == model[i]);
            }
            for (usize i = 0; i < trailing.size(); ++i)
            {
                REQUIRE(trailing[i].Value() == model[leading.size() + i]);
            }
        }
    }
    REQUIRE(ledger.Balanced());
}
