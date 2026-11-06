// Portable Int128 tests. Fixed vectors run on every compiler, including MSVC;
// where the compiler has a native 128-bit integer (Clang, GCC), a seeded sweep
// over edge and random words compares every operation with it, including the
// compiler-rt float conversions. Review: docs/architecture/primitive-types.md.
#include <ludus/foundation/base/int128.hpp>

#include <limits>

#include <catch2/catch_test_macros.hpp>

using namespace ludus::foundation;

namespace
{
constexpr int64 INT64_MAXIMUM_VALUE = 0x7FFF'FFFF'FFFF'FFFFLL;
constexpr int64 INT64_MINIMUM_VALUE = -INT64_MAXIMUM_VALUE - 1;
constexpr uint64 ALL_BITS = ~uint64{0};
constexpr uint64 SIGN_BIT = uint64{1} << 63;
constexpr float64 TWO_POW_63 = 9223372036854775808.0;
constexpr float64 TWO_POW_64 = 18446744073709551616.0;
constexpr float64 TWO_POW_127 = 170141183460469231731687303715884105728.0;

constexpr Int128 Make(int64 value) noexcept
{
    return Int128(value);
}

// Construction, words and ordering.
static_assert(Int128().GetHighWord() == 0 && Int128().GetLowWord() == 0);
static_assert(Make(-1).GetHighWord() == ALL_BITS && Make(-1).GetLowWord() == ALL_BITS);
static_assert(Make(-1).IsNegative() == true && Make(0).IsNegative() == false);
static_assert(INT128_MAXIMUM.GetHighWord() == ALL_BITS >> 1 && INT128_MAXIMUM.GetLowWord() == ALL_BITS);
static_assert(INT128_MINIMUM.GetHighWord() == SIGN_BIT && INT128_MINIMUM.GetLowWord() == 0);
static_assert(INT128_MINIMUM < Make(-1) && Make(-1) < Make(0) && Make(0) < INT128_MAXIMUM);
static_assert(Make(INT64_MAXIMUM_VALUE) < Int128::FromWords(0, SIGN_BIT));
static_assert(Make(INT64_MINIMUM_VALUE) > Int128::FromWords(ALL_BITS, SIGN_BIT - 1));
static_assert(Make(2) >= Make(2) && Make(2) <= Make(2) && (Make(2) > Make(2)) == false);

constexpr Int128 Add(Int128 left, Int128 right) noexcept
{
    Int128 result;
    return TryAdd(left, right, result) == true ? result : Int128::FromWords(0xBAD, 0xBAD);
}

constexpr Int128 Multiply(Int128 left, Int128 right) noexcept
{
    Int128 result;
    return TryMultiply(left, right, result) == true ? result : Int128::FromWords(0xBAD, 0xBAD);
}

// Carries and borrows cross the word boundary.
static_assert(Add(Int128::FromWords(0, ALL_BITS), Make(1)) == Int128::FromWords(1, 0));
static_assert(Add(Make(-1), Make(1)) == Make(0));
static_assert(Multiply(Make(INT64_MINIMUM_VALUE), Make(INT64_MINIMUM_VALUE)) == Int128::FromWords(SIGN_BIT >> 1, 0));
static_assert(Multiply(Make(INT64_MINIMUM_VALUE), Make(-1)) == Int128::FromWords(0, SIGN_BIT));
static_assert(Multiply(Int128::FromWords(1, 0), Make(-3)) == Int128::FromWords(ALL_BITS - 2, 0));
static_assert(ConvertToFloat64(INT128_MINIMUM) == -TWO_POW_127);
static_assert(ConvertToFloat64(INT128_MAXIMUM) == TWO_POW_127);

} // namespace

TEST_CASE("Int128 checked arithmetic reports overflow and preserves output", "[foundation][int128]")
{
    const Int128 sentinel = Make(42);
    Int128 output = sentinel;
    CHECK(TryAdd(INT128_MAXIMUM, Make(1), output) == false);
    CHECK(TryAdd(INT128_MINIMUM, Make(-1), output) == false);
    CHECK(TrySubtract(INT128_MINIMUM, Make(1), output) == false);
    CHECK(TrySubtract(Make(0), INT128_MINIMUM, output) == false);
    CHECK(TryMultiply(INT128_MINIMUM, Make(-1), output) == false);
    CHECK(TryMultiply(INT128_MAXIMUM, Make(2), output) == false);
    CHECK(TryMultiply(Int128::FromWords(1, 0), Int128::FromWords(1, 0), output) == false);
    CHECK(TryMultiply(Int128::FromWords(0, SIGN_BIT), Int128::FromWords(0, SIGN_BIT), output) == true);
    CHECK(output == Int128::FromWords(SIGN_BIT >> 1, 0));
    output = sentinel;
    CHECK(TryMultiply(Int128::FromWords(0, SIGN_BIT), Make(2), output) == true);
    CHECK(output == Int128::FromWords(1, 0));
    output = sentinel;
    // 2^127 is out of range positive, but -2^127 is the minimum.
    CHECK(TryMultiply(Int128::FromWords(SIGN_BIT >> 1, 0), Make(2), output) == false);
    CHECK(output == sentinel);
    CHECK(TryMultiply(Int128::FromWords(SIGN_BIT >> 1, 0), Make(-2), output) == true);
    CHECK(output == INT128_MINIMUM);

    // Output may alias an operand.
    Int128 value = Make(INT64_MAXIMUM_VALUE);
    CHECK(TryMultiply(value, value, value) == true);
    CHECK(value == Int128::FromWords((SIGN_BIT >> 1) - 1, 1));
    CHECK(TryAdd(value, value, value) == true);
    CHECK(TryAdd(value, value, value) == false);
    CHECK(value == Int128::FromWords(SIGN_BIT - 2, 2));
}

TEST_CASE("Int128 narrows only values in range", "[foundation][int128]")
{
    int64 signedValue = 7;
    CHECK(TryIntegerCast(Make(INT64_MINIMUM_VALUE), signedValue) == true);
    CHECK(signedValue == INT64_MINIMUM_VALUE);
    CHECK(TryIntegerCast(Int128::FromWords(0, SIGN_BIT), signedValue) == false);
    CHECK(TryIntegerCast(Int128::FromWords(ALL_BITS, SIGN_BIT - 1), signedValue) == false);
    CHECK(TryIntegerCast(Int128::FromWords(ALL_BITS, 0), signedValue) == false);
    CHECK(signedValue == INT64_MINIMUM_VALUE);

    uint64 unsignedValue = 7;
    CHECK(TryIntegerCast(Int128::FromWords(0, ALL_BITS), unsignedValue) == true);
    CHECK(unsignedValue == ALL_BITS);
    CHECK(TryIntegerCast(Make(-1), unsignedValue) == false);
    CHECK(TryIntegerCast(Int128::FromWords(1, 0), unsignedValue) == false);
    CHECK(unsignedValue == ALL_BITS);

    uint8 byteValue = 7;
    CHECK(TryIntegerCast(Make(255), byteValue) == true);
    CHECK(TryIntegerCast(Make(256), byteValue) == false);
    CHECK(byteValue == 255);
}

TEST_CASE("Int128 float64 conversion rounds to nearest even", "[foundation][int128]")
{
    CHECK(ConvertToFloat64(Make(0)) == 0.0);
    CHECK(ConvertToFloat64(Make(-3)) == -3.0);
    CHECK(ConvertToFloat64(Int128::FromWords(0, SIGN_BIT)) == TWO_POW_63);
    CHECK(ConvertToFloat64(Int128::FromWords(1, 0)) == TWO_POW_64);
    // 2^64 + 2^11 is a tie between 2^64 and 2^64 + 2^12: even wins.
    CHECK(ConvertToFloat64(Int128::FromWords(1, uint64{1} << 11)) == TWO_POW_64);
    // A sticky bit below the tie rounds up.
    CHECK(ConvertToFloat64(Int128::FromWords(1, (uint64{1} << 11) + 1)) == TWO_POW_64 + 4096.0);
    // 2^64 + 3 * 2^11 is a tie between odd and even neighbours: rounds up to even.
    CHECK(ConvertToFloat64(Int128::FromWords(1, uint64{3} << 11)) == TWO_POW_64 + 8192.0);
    // Discarded low-word bits are sticky when the high word spans all 64 bits.
    CHECK(ConvertToFloat64(Int128::FromWords(SIGN_BIT >> 1 | (uint64{1} << 9), 1)) ==
          TWO_POW_127 / 2.0 + TWO_POW_64 * 1024.0);
    CHECK(ConvertToFloat64(Int128::FromWords(ALL_BITS, 0)) == -TWO_POW_64);
}

TEST_CASE("Int128 truncates float64 toward zero inside its range", "[foundation][int128]")
{
    Int128 value = Make(9);
    CHECK(TryTruncateToInt128(-2.75, value) == true);
    CHECK(value == Make(-2));
    CHECK(TryTruncateToInt128(-0.0, value) == true);
    CHECK(value == Make(0));
    CHECK(TryTruncateToInt128(TWO_POW_64 + 4096.0, value) == true);
    CHECK(value == Int128::FromWords(1, 4096));
    CHECK(TryTruncateToInt128(-TWO_POW_127, value) == true);
    CHECK(value == INT128_MINIMUM);
    CHECK(TryTruncateToInt128(TWO_POW_127, value) == false);
    CHECK(TryTruncateToInt128(-TWO_POW_127 * 2.0, value) == false);
    CHECK(TryTruncateToInt128(std::numeric_limits<float64>::infinity(), value) == false);
    CHECK(TryTruncateToInt128(std::numeric_limits<float64>::quiet_NaN(), value) == false);
    CHECK(value == INT128_MINIMUM);
}

#if defined(__SIZEOF_INT128__)
namespace
{
/// Test-only SplitMix64 sequence; deterministic so a failure reproduces.
class TestWords final
{
public:
    uint64 Next() noexcept
    {
        // Thanks to Sebastiano Vigna, SplitMix64, https://prng.di.unimi.it/splitmix64.c.
        mState += 0x9E37'79B9'7F4A'7C15ULL;
        uint64 value = mState;
        value = (value ^ (value >> 30)) * 0xBF58'476D'1CE4'E5B9ULL;
        value = (value ^ (value >> 27)) * 0x94D0'49BB'1331'11EBULL;
        return value ^ (value >> 31);
    }

private:
    uint64 mState = 0x1234'5678'9ABC'DEF0ULL;
};

constexpr uint64 EDGE_WORDS[] = {
    0,
    1,
    2,
    3,
    0x7FFF'FFFFULL,
    0xFFFF'FFFFULL,
    0x1'0000'0000ULL,
    SIGN_BIT - 1,
    SIGN_BIT,
    SIGN_BIT + 1,
    ALL_BITS - 1,
    ALL_BITS,
    0x0010'0000'0000'0000ULL,
    0x001F'FFFF'FFFF'FFFFULL,
    0x0020'0000'0000'0001ULL,
};
constexpr usize RANDOM_CASES = 20000;
using NativeInt128 = __int128;

Int128 FromNative(NativeInt128 value) noexcept
{
    const unsigned __int128 bits = static_cast<unsigned __int128>(value);
    return Int128::FromWords(static_cast<uint64>(bits >> 64), static_cast<uint64>(bits));
}

NativeInt128 ToNative(Int128 value) noexcept
{
    const unsigned __int128 bits = (static_cast<unsigned __int128>(value.GetHighWord()) << 64) | value.GetLowWord();
    return static_cast<NativeInt128>(bits);
}

uint64 PickWord(TestWords& inoutWords) noexcept
{
    const uint64 choice = inoutWords.Next();
    constexpr usize EDGE_COUNT = sizeof(EDGE_WORDS) / sizeof(EDGE_WORDS[0]);
    // Half the words are edges (optionally inverted), half are random.
    if ((choice & 1) == 0)
    {
        const uint64 edge = EDGE_WORDS[(choice >> 1) % EDGE_COUNT];
        return (choice & 2) == 0 ? edge : ~edge;
    }
    return inoutWords.Next();
}

Int128 PickValue(TestWords& inoutWords) noexcept
{
    const uint64 high = PickWord(inoutWords);
    const uint64 low = PickWord(inoutWords);
    // Bias toward values that fit in 64 bits, where products stay in range.
    switch (inoutWords.Next() % 3)
    {
        case 0:
            return Make(static_cast<int64>(low));
        case 1:
            // About 96 significant bits, either sign.
            return Int128::FromWords(static_cast<uint64>(static_cast<int64>(high) >> 32), low);
        default:
            return Int128::FromWords(high, low);
    }
}
} // namespace

TEST_CASE("Int128 matches the native 128-bit integer", "[foundation][int128]")
{
    TestWords words;
    for (usize i = 0; i < RANDOM_CASES; ++i)
    {
        const Int128 left = PickValue(words);
        const Int128 right = PickValue(words);
        const NativeInt128 nativeLeft = ToNative(left);
        const NativeInt128 nativeRight = ToNative(right);
        INFO("left " << left.GetHighWord() << ':' << left.GetLowWord() << " right " << right.GetHighWord() << ':'
                     << right.GetLowWord());
        REQUIRE(FromNative(nativeLeft) == left);
        CHECK((left < right) == (nativeLeft < nativeRight));
        CHECK((left == right) == (nativeLeft == nativeRight));

        NativeInt128 expected = 0;
        Int128 actual = Make(-7);
        bool hasOverflowed = __builtin_add_overflow(nativeLeft, nativeRight, &expected);
        CHECK(TryAdd(left, right, actual) == (hasOverflowed == false));
        CHECK(actual == (hasOverflowed == true ? Make(-7) : FromNative(expected)));

        actual = Make(-7);
        hasOverflowed = __builtin_sub_overflow(nativeLeft, nativeRight, &expected);
        CHECK(TrySubtract(left, right, actual) == (hasOverflowed == false));
        CHECK(actual == (hasOverflowed == true ? Make(-7) : FromNative(expected)));

        actual = Make(-7);
        hasOverflowed = __builtin_mul_overflow(nativeLeft, nativeRight, &expected);
        CHECK(TryMultiply(left, right, actual) == (hasOverflowed == false));
        CHECK(actual == (hasOverflowed == true ? Make(-7) : FromNative(expected)));

        const float64 converted = ConvertToFloat64(left);
        CHECK(converted == static_cast<float64>(nativeLeft));

        // Truncation round-trips the converted value whenever it is in range.
        Int128 truncated = Make(-7);
        const bool isInRange = converted >= -TWO_POW_127 && converted < TWO_POW_127;
        CHECK(TryTruncateToInt128(converted, truncated) == isInRange);
        if (isInRange == true)
        {
            CHECK(truncated == FromNative(static_cast<NativeInt128>(converted)));
        }
        const float64 fraction = converted * 0.5 + 0.25;
        if (fraction >= -TWO_POW_127 && fraction < TWO_POW_127)
        {
            CHECK(TryTruncateToInt128(fraction, truncated) == true);
            CHECK(truncated == FromNative(static_cast<NativeInt128>(fraction)));
        }
    }
}
#endif
