// Exact rounding rules of the foliage cook's fixed-point helpers.

#include "internal/foliage_fixed_point.h"

#include <ludus/content/foliage_cook/foliage_cook.h>
#include <ludus/content/foliage_format/lufol.h>

#include <catch2/catch_test_macros.hpp>

#include <cstring>

namespace
{
using namespace ludus::content::foliage_cook;
using namespace ludus::content::foliage_cook::internal;
namespace format = ludus::content::foliage_format;
using ludus::foundation::Int128;

int64 Round(DyadicValue origin, DyadicValue quantum, int64 code, int32 scaleBits)
{
    int64 value = 0;
    REQUIRE(TryRoundDyadicSum(origin, quantum, code, scaleBits, value) == true);
    return value;
}
} // namespace

TEST_CASE("binary32 values decompose exactly", "[foliage][cook][fixed-point]")
{
    DyadicValue value;
    REQUIRE(TryDecomposeFloat32(0.5F, value) == true);
    CHECK(value.Mantissa == (int64{1} << 23));
    CHECK(value.Exponent == -24);
    REQUIRE(TryDecomposeFloat32(-3.0F, value) == true);
    CHECK(value.Mantissa == -(int64{3} << 22));
    CHECK(value.Exponent == -22);
    REQUIRE(TryDecomposeFloat32(0.0F, value) == true);
    CHECK(value.Mantissa == 0);
    // The smallest subnormal is 2^-149.
    REQUIRE(TryDecomposeFloat32(1.40129846e-45F, value) == true);
    CHECK(value.Mantissa == 1);
    CHECK(value.Exponent == -149);
    // 0.01 is not dyadic; its binary32 value is 10737418 * 2^-30.
    REQUIRE(TryDecomposeFloat32(0.01F, value) == true);
    CHECK(value.Mantissa == 10737418);
    CHECK(value.Exponent == -30);

    const DyadicValue unchanged = value;
    constexpr uint32 INFINITY_BITS = 0x7f800000U;
    float32 infinity = 0;
    std::memcpy(&infinity, &INFINITY_BITS, sizeof(infinity));
    CHECK(TryDecomposeFloat32(infinity, value) == false);
    CHECK(value.Mantissa == unchanged.Mantissa);
}

TEST_CASE("dyadic sums round half-way values toward positive infinity", "[foliage][cook][fixed-point]")
{
    const DyadicValue half{ .Mantissa = 1, .Exponent = -1 };
    const DyadicValue one{ .Mantissa = 1, .Exponent = 0 };
    const DyadicValue zero{};
    CHECK(Round(half, zero, 0, 0) == 1);
    CHECK(Round({ .Mantissa = -1, .Exponent = -1 }, zero, 0, 0) == 0);
    CHECK(Round({ .Mantissa = 3, .Exponent = -1 }, zero, 0, 0) == 2);
    CHECK(Round({ .Mantissa = -3, .Exponent = -1 }, zero, 0, 0) == -1);
    CHECK(Round({ .Mantissa = 5, .Exponent = -2 }, zero, 0, 0) == 1);
    CHECK(Round({ .Mantissa = -5, .Exponent = -2 }, zero, 0, 0) == -1);
    // origin + quantum * code, scaled by 2^4: (0.5 + 1 * 3) * 16 = 56.
    CHECK(Round(half, one, 3, 4) == 56);
    // Far-apart exponents align exactly before rounding: 2^40 + 2^-40 rounds to 2^40.
    CHECK(Round({ .Mantissa = 1, .Exponent = 40 }, { .Mantissa = 1, .Exponent = -40 }, 1, 0) == (int64{1} << 40));
    int64 value = 7;
    CHECK(TryRoundDyadicSum({ .Mantissa = 1, .Exponent = 63 }, zero, 0, 0, value) == false);
    CHECK(value == 7);
}

TEST_CASE("Int128 shifts floor and round at every width", "[foliage][cook][fixed-point]")
{
    const Int128 minusThree{int64{-3}};
    CHECK(ShiftRightFloor(minusThree, 1) == Int128{int64{-2}});
    CHECK(ShiftRightRoundHalfUp(minusThree, 1) == Int128{int64{-1}});
    CHECK(ShiftRightRoundHalfUp(Int128{int64{3}}, 1) == Int128{int64{2}});
    const Int128 large = Int128::FromWords(uint64{1} << 62, 0);
    CHECK(ShiftRightFloor(large, 126) == Int128{int64{1}});
    CHECK(ShiftRightRoundHalfUp(large, 127) == Int128{int64{1}});
    CHECK(ShiftRightRoundHalfUp(large, 128) == Int128{});
    CHECK(ShiftRightRoundHalfUp(ludus::foundation::INT128_MINIMUM, 127) == Int128{int64{-1}});
    Int128 shifted;
    REQUIRE(TryShiftLeft(Int128{int64{1}}, 100, shifted) == true);
    CHECK(shifted == Int128::FromWords(uint64{1} << 36, 0));
    CHECK(TryShiftLeft(Int128{int64{1}}, 127, shifted) == false);
    CHECK(FloorDivide(-1, 256) == -1);
    CHECK(FloorDivide(-256, 256) == -1);
    CHECK(FloorDivide(-257, 256) == -2);
    CHECK(FloorModulo(-1, 256) == 255);
    CHECK(FloorModulo(256, 256) == 0);
}

TEST_CASE("slopes and up directions truncate exact values", "[foliage][cook][fixed-point]")
{
    constexpr int64 SPACING = int64{1} << 25;
    CHECK(ComputeSlopeUnits(0, 0, SPACING) == 0);
    CHECK(ComputeSlopeUnits(SPACING, 0, SPACING) == FOLIAGE_SLOPE_UNITS_PER_ONE);
    // A 3-4-5 gradient has slope 5/4 exactly.
    CHECK(ComputeSlopeUnits(3 * SPACING / 4, SPACING, SPACING) == 5 * FOLIAGE_SLOPE_UNITS_PER_ONE / 4);
    CHECK(ComputeSlopeUnits(SPACING * 100, 0, SPACING) == FOLIAGE_MAXIMUM_SLOPE_UNITS);
    // Slope one: the up direction is (-1, 1, 0) / sqrt(2), and 32767 / sqrt(2) = 23169.77.
    CHECK(ComputeNormalComponent(SPACING, SPACING, 0, SPACING) == -23169);
    CHECK(ComputeNormalComponent(-SPACING, -SPACING, 0, SPACING) == 23169);
    CHECK(ComputeNormalComponent(0, 0, SPACING, SPACING) == 0);
    // Near-vertical surfaces stay no longer than one after truncation.
    constexpr int64 STEEP = int64{1} << 44;
    const int16 normalX = ComputeNormalComponent(STEEP, STEEP, STEEP, SPACING);
    const int16 normalZ = ComputeNormalComponent(STEEP, STEEP, STEEP, SPACING);
    CHECK(int64{normalX} * normalX + int64{normalZ} * normalZ <= int64{format::NORMAL_UNITS} * format::NORMAL_UNITS);
}

TEST_CASE("admission thresholds are exact and reject densities above slot capacity", "[foliage][cook][fixed-point]")
{
    const DyadicValue oneMetre{ .Mantissa = 1, .Exponent = 0 };
    uint64 threshold = 0;
    // One placement per square metre, one-sample cells of one metre, one slot: probability one.
    REQUIRE(TryComputeAdmissionThreshold(FOLIAGE_DENSITY_UNITS_PER_PLACEMENT, 256, oneMetre, 1, threshold) == true);
    CHECK(threshold == (uint64{1} << 32));
    REQUIRE(TryComputeAdmissionThreshold(FOLIAGE_DENSITY_UNITS_PER_PLACEMENT, 256, oneMetre, 4, threshold) == true);
    CHECK(threshold == (uint64{1} << 30));
    CHECK(TryComputeAdmissionThreshold(FOLIAGE_DENSITY_UNITS_PER_PLACEMENT + 1, 256, oneMetre, 1, threshold) == false);
    CHECK(threshold == (uint64{1} << 30));
    // Three slots floor the exact quotient.
    REQUIRE(TryComputeAdmissionThreshold(FOLIAGE_DENSITY_UNITS_PER_PLACEMENT, 256, oneMetre, 3, threshold) == true);
    CHECK(threshold == (uint64{1} << 32) / 3);
    // Half-metre spacing quarters the cell area.
    const DyadicValue halfMetre{ .Mantissa = 1, .Exponent = -1 };
    REQUIRE(TryComputeAdmissionThreshold(FOLIAGE_DENSITY_UNITS_PER_PLACEMENT, 256, halfMetre, 1, threshold) == true);
    CHECK(threshold == (uint64{1} << 30));
    CHECK(ScaleRadius(100, format::SCALE_UNITS_PER_ONE) == 100);
    CHECK(ScaleRadius(100, format::SCALE_UNITS_PER_ONE / 3) == 34);
    CHECK(ScaleRadius(0, format::SCALE_UNITS_PER_ONE) == 0);
}
