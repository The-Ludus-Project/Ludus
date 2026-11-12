#include <ludus/foundation/containers/static_bit_set.hpp>

#include <catch2/catch_test_macros.hpp>

#include <type_traits>

using ludus::foundation::StaticBitSet;
using ludus::foundation::uint32;
using ludus::foundation::uint64;
using ludus::foundation::usize;

namespace
{
static_assert(std::is_trivially_copyable_v<StaticBitSet<65>>);
static_assert(StaticBitSet<0>::WORD_COUNT == 0);
static_assert(StaticBitSet<1>::WORD_COUNT == 1);
static_assert(StaticBitSet<64>::WORD_COUNT == 1);
static_assert(StaticBitSet<65>::WORD_COUNT == 2);
static_assert(sizeof(StaticBitSet<64>) == sizeof(uint64));
static_assert(sizeof(StaticBitSet<65>) == 2 * sizeof(uint64));

// The bit-count helpers are checked against direct loops at compile time.
[[nodiscard]] constexpr usize CountByLoop(uint64 word) noexcept
{
    usize count = 0;
    for (usize i = 0; i < 64; ++i)
    {
        count += static_cast<usize>((word >> i) & 1U);
    }
    return count;
}
static_assert(ludus::foundation::core::detail::CountSetBits(0) == 0);
static_assert(ludus::foundation::core::detail::CountSetBits(~uint64{0}) == 64);
static_assert(ludus::foundation::core::detail::CountSetBits(0x8000000000000001ULL) == 2);
static_assert(ludus::foundation::core::detail::CountSetBits(0x0123456789ABCDEFULL) ==
              CountByLoop(0x0123456789ABCDEFULL));
static_assert(ludus::foundation::core::detail::CountTrailingZeroBits(1) == 0);
static_assert(ludus::foundation::core::detail::CountTrailingZeroBits(0x8000000000000000ULL) == 63);
static_assert(ludus::foundation::core::detail::CountTrailingZeroBits(0x0000000100000000ULL) == 32);

// Usable in constant evaluation.
constexpr StaticBitSet<70> MakeConstant() noexcept
{
    StaticBitSet<70> bits;
    bits.Set(0);
    bits.Set(69);
    bits <<= 1;
    return bits;
}
static_assert(MakeConstant().CountSet() == 1);
static_assert(MakeConstant().IsSet(1));
static_assert(MakeConstant().FindNextSet(0) == 1);

// An independent model: one bool per bit, plus a 1-slot pad so 0 bits compile.
template <usize BitCount>
struct Model
{
    bool Bits[BitCount + 1] = {};
};

template <usize BitCount>
void RequireMatches(const StaticBitSet<BitCount>& bits, const Model<BitCount>& model)
{
    usize count = 0;
    for (usize i = 0; i < BitCount; ++i)
    {
        REQUIRE(bits.IsSet(i) == model.Bits[i]);
        count += model.Bits[i] == true ? 1 : 0;
    }
    REQUIRE(bits.CountSet() == count);
    REQUIRE(bits.IsAnySet() == (count != 0));
    REQUIRE(bits.IsNoneSet() == (count == 0));
    REQUIRE(bits.AreAllSet() == (count == BitCount));
    // Tail bits past BitCount stay zero in the exposed words.
    constexpr usize TAIL = BitCount % StaticBitSet<BitCount>::BITS_PER_WORD;
    if constexpr (TAIL != 0)
    {
        REQUIRE((bits.GetWords()[StaticBitSet<BitCount>::WORD_COUNT - 1] >> TAIL) == 0);
    }
    // FindNextSet visits exactly the set bits in order.
    usize expected = 0;
    for (usize found = bits.FindNextSet(0); found < BitCount; found = bits.FindNextSet(found + 1))
    {
        while (expected < BitCount && model.Bits[expected] == false)
        {
            ++expected;
        }
        REQUIRE(found == expected);
        ++expected;
    }
    while (expected < BitCount && model.Bits[expected] == false)
    {
        ++expected;
    }
    REQUIRE(expected == BitCount);
}

template <usize BitCount>
void CheckBoundaries()
{
    StaticBitSet<BitCount> bits;
    Model<BitCount> model;
    RequireMatches(bits, model);
    REQUIRE(bits.FindNextSet(BitCount) == BitCount);
    REQUIRE(bits.FindNextSet(BitCount + 7) == BitCount);

    // First, last and word-edge bits.
    const usize candidates[] = {0, 1, 62, 63, 64, 65, 127, 128, 129};
    for (const usize index : candidates)
    {
        if (index < BitCount)
        {
            bits.Set(index);
            model.Bits[index] = true;
        }
    }
    RequireMatches(bits, model);

    bits.FlipAll();
    for (usize i = 0; i < BitCount; ++i)
    {
        model.Bits[i] = model.Bits[i] == false;
    }
    RequireMatches(bits, model);
    REQUIRE((~bits).CountSet() == BitCount - bits.CountSet());

    bits.SetAll();
    for (usize i = 0; i < BitCount; ++i)
    {
        model.Bits[i] = true;
    }
    RequireMatches(bits, model);

    // Every shift amount, including 0, word sizes and amounts past the end.
    StaticBitSet<BitCount> pattern;
    Model<BitCount> patternModel;
    for (usize i = 0; i < BitCount; ++i)
    {
        const bool isSet = ((i * 7U + 3U) % 5U) < 2U;
        pattern.Assign(i, isSet);
        patternModel.Bits[i] = isSet;
    }
    for (usize count = 0; count <= BitCount + 2; ++count)
    {
        Model<BitCount> left;
        Model<BitCount> right;
        for (usize i = 0; i < BitCount; ++i)
        {
            left.Bits[i] = i >= count && patternModel.Bits[i - count];
            right.Bits[i] = i + count < BitCount && patternModel.Bits[i + count];
        }
        RequireMatches(pattern << count, left);
        RequireMatches(pattern >> count, right);
    }

    bits.ResetAll();
    for (usize i = 0; i < BitCount; ++i)
    {
        model.Bits[i] = false;
    }
    RequireMatches(bits, model);
}
} // namespace

TEST_CASE("StaticBitSet word boundaries, tail masking and every shift", "[containers][bit-set]")
{
    CheckBoundaries<0>();
    CheckBoundaries<1>();
    CheckBoundaries<63>();
    CheckBoundaries<64>();
    CheckBoundaries<65>();
    CheckBoundaries<130>();
}

TEST_CASE("StaticBitSet single-bit operations", "[containers][bit-set]")
{
    StaticBitSet<65> bits;
    bits.Set(64);
    bits.Assign(3, true);
    REQUIRE(bits.IsSet(64));
    REQUIRE(bits.IsSet(3));
    bits.Flip(3);
    REQUIRE(bits.IsSet(3) == false);
    bits.Flip(3);
    bits.Assign(3, false);
    REQUIRE(bits.IsSet(3) == false);
    bits.Reset(64);
    REQUIRE(bits.IsNoneSet());
    REQUIRE(bits.GetWords().GetSize() == 2);
}

TEST_CASE("StaticBitSet set operations combine word by word", "[containers][bit-set]")
{
    StaticBitSet<100> left;
    StaticBitSet<100> right;
    for (usize i = 0; i < 100; ++i)
    {
        left.Assign(i, i % 2 == 0);
        right.Assign(i, i % 3 == 0);
    }
    const StaticBitSet<100> both = left & right;
    const StaticBitSet<100> either = left | right;
    const StaticBitSet<100> exactlyOne = left ^ right;
    for (usize i = 0; i < 100; ++i)
    {
        REQUIRE(both.IsSet(i) == (i % 6 == 0));
        REQUIRE(either.IsSet(i) == (i % 2 == 0 || i % 3 == 0));
        REQUIRE(exactlyOne.IsSet(i) == ((i % 2 == 0) != (i % 3 == 0)));
    }
    REQUIRE((left ^ left).IsNoneSet());
    REQUIRE((left | ~left).AreAllSet());
    REQUIRE(both == (right & left));
    REQUIRE((both == either) == false);
}

TEST_CASE("StaticBitSet seeded operations match an independent model", "[containers][bit-set][model]")
{
    constexpr usize BIT_COUNT = 193;
    StaticBitSet<BIT_COUNT> bits;
    Model<BIT_COUNT> model;
    uint32 state = 0x6D2B79F5U;
    for (usize operation = 0; operation < 20000; ++operation)
    {
        state = state * 1664525U + 1013904223U;
        const usize index = (state >> 8U) % BIT_COUNT;
        const uint32 choice = (state >> 28U) % 8U;
        if (choice <= 2)
        {
            bits.Set(index);
            model.Bits[index] = true;
        }
        else if (choice <= 4)
        {
            bits.Reset(index);
            model.Bits[index] = false;
        }
        else if (choice == 5)
        {
            bits.Flip(index);
            model.Bits[index] = model.Bits[index] == false;
        }
        else if (choice == 6)
        {
            const usize count = index % 70;
            bits <<= count;
            for (usize i = BIT_COUNT; i-- > 0;)
            {
                model.Bits[i] = i >= count && model.Bits[i - count];
            }
        }
        else
        {
            const usize count = index % 70;
            bits >>= count;
            for (usize i = 0; i < BIT_COUNT; ++i)
            {
                model.Bits[i] = i + count < BIT_COUNT && model.Bits[i + count];
            }
        }
        if (operation % 16 == 0)
        {
            RequireMatches(bits, model);
        }
        else
        {
            REQUIRE(bits.IsSet(index) == model.Bits[index]);
        }
    }
    RequireMatches(bits, model);
}
