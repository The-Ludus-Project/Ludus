// Thanks to Eric Lengyel, "Bit Hacks for Games", Game Engine Gems 2, chapter 24,
// pp. 391-401: width and signed-minimum counterexamples motivate these independent
// widened oracles and exhaustive 8-bit cases. No chapter listing is copied.
// Review: docs/architecture/primitive-types.md.
#include <ludus/foundation/base/checked_integer.hpp>

#include <initializer_list>
#include <limits>
#include <type_traits>

#include <catch2/catch_template_test_macros.hpp>
#include <catch2/catch_test_macros.hpp>

using namespace ludus::foundation;

namespace
{
enum class ExampleEnum : uint8
{
    Zero,
};
static_assert(Integer<bool> == false && Integer<char> == false && Integer<char8_t> == false &&
              Integer<char16_t> == false && Integer<char32_t> == false && Integer<wchar_t> == false &&
              Integer<ExampleEnum> == false && Integer<float32> == false && Integer<float64> == false);
static_assert(Integer<const uint32> == false && Integer<volatile uint32> == false && Integer<uint32&> == false);

template <typename ValueType>
concept HasCheckedAdd = requires(ValueType value, ValueType& out) { TryAdd(value, value, out); };
static_assert(HasCheckedAdd<bool> == false && HasCheckedAdd<char> == false && HasCheckedAdd<float32> == false &&
              HasCheckedAdd<ExampleEnum> == false);

template <typename DestinationType, typename SourceType>
constexpr bool CanRepresent(SourceType value) noexcept
{
    // Test oracle: widen into separate signed/unsigned 64-bit domains. This is
    // independent of std::in_range and cannot overflow for our input aliases.
    if constexpr (std::is_signed_v<SourceType>)
    {
        if (value < 0)
        {
            if constexpr (std::is_unsigned_v<DestinationType>)
            {
                return false;
            }
            else
            {
                return static_cast<int64>(value) >= static_cast<int64>(std::numeric_limits<DestinationType>::min());
            }
        }
    }
    return static_cast<uint64>(value) <= static_cast<uint64>(std::numeric_limits<DestinationType>::max());
}

template <typename DestinationType, typename SourceType>
void VerifyCast(SourceType value)
{
    DestinationType out = 42;
    const bool expected = CanRepresent<DestinationType>(value);
    REQUIRE(TryIntegerCast(value, out) == expected);
    REQUIRE(out == (expected ? static_cast<DestinationType>(value) : DestinationType{42}));
}

template <typename DestinationType, typename SourceType>
void VerifyCastPair()
{
    constexpr auto lo = std::numeric_limits<SourceType>::min();
    constexpr auto hi = std::numeric_limits<SourceType>::max();
    for (const SourceType value :
         {lo, static_cast<SourceType>(lo + 1), SourceType{0}, SourceType{1}, static_cast<SourceType>(hi - 1), hi})
    {
        VerifyCast<DestinationType>(value);
    }
    constexpr uint64 destinationMax = static_cast<uint64>(std::numeric_limits<DestinationType>::max());
    if constexpr (destinationMax < static_cast<uint64>(hi))
    {
        VerifyCast<DestinationType>(static_cast<SourceType>(destinationMax));
        VerifyCast<DestinationType>(static_cast<SourceType>(destinationMax + 1));
    }
    if constexpr (std::is_signed_v<SourceType>)
    {
        VerifyCast<DestinationType>(SourceType{-1});
        if constexpr (std::is_signed_v<DestinationType>)
        {
            // NOLINTNEXTLINE(bugprone-signed-char-misuse): the int8 numeric minimum must widen with its sign intact.
            constexpr int64 destinationMin = std::numeric_limits<DestinationType>::min();
            if constexpr (destinationMin > static_cast<int64>(lo))
            {
                VerifyCast<DestinationType>(static_cast<SourceType>(destinationMin));
                VerifyCast<DestinationType>(static_cast<SourceType>(destinationMin - 1));
            }
        }
    }
}

template <typename ValueType>
constexpr bool VerifyConstexprArithmetic() noexcept
{
    constexpr auto lo = std::numeric_limits<ValueType>::min();
    constexpr auto hi = std::numeric_limits<ValueType>::max();
    ValueType out = 42;
    if (TryAdd(hi, ValueType{1}, out) || out != 42 || TrySubtract(lo, ValueType{1}, out) || out != 42 ||
        TryMultiply(hi, ValueType{2}, out) || out != 42)
    {
        return false;
    }
    if (TryAdd(ValueType{20}, ValueType{22}, out) == false || out != 42 ||
        TrySubtract(out, ValueType{2}, out) == false || out != 40 || TryMultiply(ValueType{2}, out, out) == false ||
        out != 80)
    {
        return false;
    }
    if constexpr (std::is_signed_v<ValueType>)
    {
        return TryMultiply(lo, ValueType{-1}, out) == false && out == 80 && TryMultiply(lo, ValueType{1}, out) &&
               out == lo;
    }
    return true;
}

static_assert(VerifyConstexprArithmetic<uint8>() && VerifyConstexprArithmetic<uint16>() &&
              VerifyConstexprArithmetic<uint32>() && VerifyConstexprArithmetic<uint64>() &&
              VerifyConstexprArithmetic<int8>() && VerifyConstexprArithmetic<int16>() &&
              VerifyConstexprArithmetic<int32>() && VerifyConstexprArithmetic<int64>() &&
              VerifyConstexprArithmetic<usize>() && VerifyConstexprArithmetic<isize>());
constexpr bool VerifyConstexprCasts() noexcept
{
    uint8 small = 42;
    int64 signedValue = 42;
    int16 wide{};
    return TryIntegerCast(int8{-1}, small) == false && small == 42 && TryIntegerCast(uint16{256}, small) == false &&
           small == 42 && TryIntegerCast(~uint64{0}, signedValue) == false && signedValue == 42 &&
           TryIntegerCast(uint8{255}, wide) && wide == 255;
}
static_assert(VerifyConstexprCasts());

static_assert(noexcept(TryIntegerCast(uint64{}, *static_cast<uint32*>(nullptr))));
static_assert(noexcept(TryAdd(int64{}, int64{}, *static_cast<int64*>(nullptr))));
static_assert(noexcept(TrySubtract(uint64{}, uint64{}, *static_cast<uint64*>(nullptr))));
static_assert(noexcept(TryMultiply(int64{}, int64{}, *static_cast<int64*>(nullptr))));
} // namespace

TEMPLATE_TEST_CASE("integer casts preserve values or leave output unchanged",
                   "[primitive][integer]",
                   uint8,
                   uint16,
                   uint32,
                   uint64,
                   int8,
                   int16,
                   int32,
                   int64,
                   usize,
                   isize)
{
    VerifyCastPair<uint8, TestType>();
    VerifyCastPair<uint16, TestType>();
    VerifyCastPair<uint32, TestType>();
    VerifyCastPair<uint64, TestType>();
    VerifyCastPair<int8, TestType>();
    VerifyCastPair<int16, TestType>();
    VerifyCastPair<int32, TestType>();
    VerifyCastPair<int64, TestType>();
    VerifyCastPair<usize, TestType>();
    VerifyCastPair<isize, TestType>();
}

TEMPLATE_TEST_CASE("checked arithmetic handles every native width and output aliasing",
                   "[primitive][integer]",
                   uint8,
                   uint16,
                   uint32,
                   uint64,
                   int8,
                   int16,
                   int32,
                   int64,
                   usize,
                   isize)
{
    REQUIRE(VerifyConstexprArithmetic<TestType>());
    TestType value = std::numeric_limits<TestType>::max();
    REQUIRE_FALSE(TryAdd(value, TestType{1}, value));
    REQUIRE(value == std::numeric_limits<TestType>::max());
    REQUIRE_FALSE(TryMultiply(TestType{2}, value, value));
    REQUIRE(value == std::numeric_limits<TestType>::max());
    REQUIRE(TryIntegerCast(value, value));
    REQUIRE(TrySubtract(value, value, value));
    REQUIRE(value == 0);
    REQUIRE(TryMultiply(value, std::numeric_limits<TestType>::max(), value));
    REQUIRE(value == 0);
}

TEMPLATE_TEST_CASE("all 8-bit arithmetic matches a widened oracle", "[primitive][integer]", uint8, int8)
{
    // NOLINTNEXTLINE(bugprone-signed-char-misuse): the int8 oracle intentionally widens the numeric minimum.
    constexpr int32 lo = std::numeric_limits<TestType>::min();
    constexpr int32 hi = std::numeric_limits<TestType>::max();
    for (int32 left = lo; left <= hi; ++left)
    {
        for (int32 right = lo; right <= hi; ++right)
        {
            const auto a = static_cast<TestType>(left);
            const auto b = static_cast<TestType>(right);
            const int32 expected[] = {left + right, left - right, left * right};
            TestType outputs[] = {42, 42, 42};
            const bool results[] = {TryAdd(a, b, outputs[0]),
                                    TrySubtract(a, b, outputs[1]),
                                    TryMultiply(a, b, outputs[2])};
            for (usize i = 0; i < 3; ++i)
            {
                const bool fits = expected[i] >= lo && expected[i] <= hi;
                REQUIRE(results[i] == fits);
                REQUIRE(outputs[i] == (fits ? static_cast<TestType>(expected[i]) : TestType{42}));
            }
        }
    }
}
