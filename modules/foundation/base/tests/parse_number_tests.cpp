#include <ludus/foundation/base/parse_number.hpp>

#include <catch2/catch_test_macros.hpp>

#include <bit>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
using namespace ludus::foundation;
TEST_CASE("Owned decimal parser consumes JSON numbers and preserves failures", "[base][parse]")
{
    for (const auto* text :
         {"", "-", "+1", "01", "1.", ".5", "1e", "1e+", "1 2", " 1", "1 ", "nan", "inf", "0x1p0", "--1", "- 1"})
    {
        float32 value = 17;
        REQUIRE(ParseFloat32(text, std::strlen(text), value) == NumberParseStatus::InvalidSyntax);
        REQUIRE(value == 17);
    }
    float32 value = 17;
    REQUIRE(ParseFloat32(nullptr, 0, value) == NumberParseStatus::InvalidSyntax);
    const std::string oversized(1025, '1');
    REQUIRE(ParseFloat32(oversized.data(), oversized.size(), value) == NumberParseStatus::TooLong);
    REQUIRE(value == 17);
    const std::string bounded = "0." + std::string(1021, '0') + "1";
    REQUIRE(bounded.size() == 1024);
    REQUIRE(ParseFloat32(bounded.data(), bounded.size(), value) == NumberParseStatus::OutOfRange);
    REQUIRE(value == 17);
    for (const auto* text :
         {"1e99999999",
          "1e-99999999",
          "7.00649232162408535461864791644958065640130970938257885878534141944895541342930300743319094181060791015625e-"
          "46",
          "340282356779733661637539395458142568448"})
    {
        REQUIRE(ParseFloat32(text, std::strlen(text), value) == NumberParseStatus::OutOfRange);
        REQUIRE(value == 17);
    }
    REQUIRE(ParseFloat32("-0e999999", 9, value) == NumberParseStatus::Success);
    REQUIRE(std::bit_cast<uint32>(value) == 0x80000000U);
}
TEST_CASE("Owned binary32 conversion rounds exact midpoints to even", "[base][parse]")
{
    struct Case
    {
        const char* Text;
        uint32 Bits;
    };
    for (const auto test : {Case{"1.000000059604644775390625", 0x3f800000U},
                            Case{"1.000000059604644775390626", 0x3f800001U},
                            Case{"1.000000178813934326171875", 0x3f800002U},
                            Case{"1.999999940395355224609375", 0x40000000U},
                            Case{"340282346638528859811704183484516925440", 0x7f7fffffU},
                            Case{"1e-45", 1U},
                            Case{"-1e-45", 0x80000001U},
                            Case{"1.17549435082228750796873653722224568e-38", 0x800000U},
                            Case{"-123.5e+2", 0xc640f800U}})
    {
        CAPTURE(test.Text);
        float32 value = 0;
        REQUIRE(ParseFloat32(test.Text, std::strlen(test.Text), value) == NumberParseStatus::Success);
        REQUIRE(std::bit_cast<uint32>(value) == test.Bits);
    }
    // Long significant tails must still break an exact midpoint correctly.
    std::string tail = "1.000000059604644775390625";
    tail.append(900, '0');
    tail += '1';
    float32 value = 0;
    REQUIRE(ParseFloat32(tail.data(), tail.size(), value) == NumberParseStatus::Success);
    REQUIRE(std::bit_cast<uint32>(value) == 0x3f800001U);
}
TEST_CASE("Owned unsigned conversion detects overflow without changing output", "[base][parse]")
{
    uint64 value = 42;
    REQUIRE(ParseUint64("18446744073709551615", 20, value) == NumberParseStatus::Success);
    REQUIRE(value == 0xffffffffffffffffULL);
    REQUIRE(ParseUint64("18446744073709551616", 20, value) == NumberParseStatus::OutOfRange);
    REQUIRE(value == 0xffffffffffffffffULL);
    for (const auto* text : {"", "-1", "+1", "1x", " 1"})
    {
        REQUIRE(ParseUint64(text, std::strlen(text), value) == NumberParseStatus::InvalidSyntax);
    }
    REQUIRE(ParseUint64(nullptr, 0, value) == NumberParseStatus::InvalidSyntax);
    const std::string oversized(1025, '0');
    REQUIRE(ParseUint64(oversized.data(), oversized.size(), value) == NumberParseStatus::TooLong);
    REQUIRE(value == 0xffffffffffffffffULL);
    REQUIRE(ParseUint64("000", 3, value) == NumberParseStatus::Success);
    REQUIRE(value == 0);
}
TEST_CASE("Owned conversion agrees with independent CRT binary32 rounding", "[base][parse]")
{
    // Test-only CRT oracle, fixed decimal point and deterministic integer seed.
    uint32 state = 0x571234abU;
    for (usize index = 0; index < 3000; ++index)
    {
        state = state * 1664525U + 1013904223U;
        const uint32 mantissa = state;
        state = state * 1664525U + 1013904223U;
        const int32 exponent = static_cast<int32>(state % 80) - 48;
        char text[64] = {};
        const auto size = std::snprintf(text, sizeof(text), "%ue%d", mantissa, exponent);
        REQUIRE(size > 0);
        const float32 expected = std::strtof(text, nullptr);
        const uint32 bits = std::bit_cast<uint32>(expected);
        float32 actual = 17;
        const auto result = ParseFloat32(text, static_cast<usize>(size), actual);
        CAPTURE(text);
        if (bits == 0 || bits >= 0x7f800000U)
        {
            REQUIRE(result == NumberParseStatus::OutOfRange);
            REQUIRE(actual == 17);
        }
        else
        {
            REQUIRE(result == NumberParseStatus::Success);
            REQUIRE(std::bit_cast<uint32>(actual) == bits);
        }
    }
}

TEST_CASE("Owned conversion agrees with CRT binary32 rounding for long fractional inputs", "[base][parse]")
{
    // Test-only CRT oracle over long digit strings, fractions, subnormals and range edges,
    // which exercise multi-word arithmetic in the exact converter.
    constexpr usize CASE_COUNT = 20000;
    constexpr uint32 MAX_DIGITS = 60;
    constexpr uint32 DECIMAL_RADIX = 10;
    constexpr uint32 EXPONENT_SPAN = 100;
    constexpr int32 EXPONENT_OFFSET = 60;
    constexpr usize TEXT_BYTES = 96;
    constexpr uint32 LCG_MULTIPLIER = 1664525U;
    constexpr uint32 LCG_INCREMENT = 1013904223U;
    constexpr uint32 BINARY32_INFINITY_BITS = 0x7f800000U;
    constexpr float32 UNCHANGED = 17;
    uint32 state = 0x2468aceU;
    const auto next = [&state]() {
        state = state * LCG_MULTIPLIER + LCG_INCREMENT;
        return state >> 8;
    };
    for (usize index = 0; index < CASE_COUNT; ++index)
    {
        char text[TEXT_BYTES] = {};
        usize size = 0;
        const uint32 digits = 1 + next() % MAX_DIGITS;
        const uint32 point = next() % (digits + 1);
        text[size++] = static_cast<char>('1' + next() % (DECIMAL_RADIX - 1));
        for (uint32 digit = 1; digit < digits; ++digit)
        {
            if (digit == point)
            {
                text[size++] = '.';
            }
            text[size++] = static_cast<char>('0' + next() % DECIMAL_RADIX);
        }
        const int32 exponent = static_cast<int32>(next() % EXPONENT_SPAN) - EXPONENT_OFFSET;
        const int32 written = std::snprintf(text + size, sizeof(text) - size, "e%d", exponent);
        REQUIRE(written > 0);
        size += static_cast<usize>(written);
        const float32 expected = std::strtof(text, nullptr);
        const uint32 bits = std::bit_cast<uint32>(expected);
        float32 actual = UNCHANGED;
        const NumberParseStatus result = ParseFloat32(text, size, actual);
        CAPTURE(text);
        if (bits == 0 || bits >= BINARY32_INFINITY_BITS)
        {
            REQUIRE(result == NumberParseStatus::OutOfRange);
            REQUIRE(actual == UNCHANGED);
        }
        else
        {
            REQUIRE(result == NumberParseStatus::Success);
            REQUIRE(std::bit_cast<uint32>(actual) == bits);
        }
    }
}
