// Peters 2021, section 6.1: shading normals clipped to the hemisphere of the outgoing direction, checked against the
// geometric contract and a float64 evaluation of the same construction.

#include <ludus/foundation/math/random.hpp>
#include <ludus/foundation/math/scalar.hpp>
#include <ludus/foundation/math/shading_normal.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <limits>

using namespace ludus::foundation::math;
using ludus::foundation::core::float32;
using ludus::foundation::core::float64;
using ludus::foundation::core::uint32;
using ludus::foundation::core::uint64;

namespace
{
constexpr uint64 RANDOM_SEED = 0x5E6A1C1EULL;
constexpr uint32 CASE_COUNT = 4096;
// A few float32 roundings of unit-length results and their cosines.
constexpr float32 UNIT_TOLERANCE = 1.0e-6F;

/// A uniformly distributed unit vector, from z uniform in [-1, 1] and a uniform azimuth.
Vector3 DrawUnitVector(RandomStream& inoutRandom) noexcept
{
    const float64 z = 2.0 * static_cast<float64>(inoutRandom.NextFloat01()) - 1.0;
    const float64 azimuth = 2.0 * kPi * static_cast<float64>(inoutRandom.NextFloat01());
    const float64 radius = std::sqrt(1.0 - z * z);
    return {static_cast<float32>(radius * std::cos(azimuth)),
            static_cast<float32>(radius * std::sin(azimuth)),
            static_cast<float32>(z)};
}

float32 GetMinimumClippedCosine() noexcept
{
    return SHADING_NORMAL_MINIMUM_OUTGOING_COSINE /
           Sqrt(1.0F + SHADING_NORMAL_MINIMUM_OUTGOING_COSINE * SHADING_NORMAL_MINIMUM_OUTGOING_COSINE);
}
} // namespace

TEST_CASE("Shading normals are clipped to the outgoing direction's hemisphere", "[math][shading-normal]")
{
    const Vector3 outgoing{0.0F, 0.6F, 0.8F};
    Vector3 clipped;

    SECTION("a normal inside the hemisphere is unchanged, including at the margin")
    {
        const Vector3 facing{0.0F, 0.0F, 1.0F};
        REQUIRE(TryClipShadingNormalToOutgoingHemisphere(facing, outgoing, clipped) == MathStatus::Success);
        CHECK(clipped.X == facing.X);
        CHECK(clipped.Y == facing.Y);
        CHECK(clipped.Z == facing.Z);
        // Cosine exactly SHADING_NORMAL_MINIMUM_OUTGOING_COSINE against +z.
        const float32 margin = SHADING_NORMAL_MINIMUM_OUTGOING_COSINE;
        const Vector3 atMargin{Sqrt(1.0F - margin * margin), 0.0F, margin};
        REQUIRE(TryClipShadingNormalToOutgoingHemisphere(atMargin, {0.0F, 0.0F, 1.0F}, clipped) == MathStatus::Success);
        CHECK(clipped.X == atMargin.X);
        CHECK(clipped.Z == atMargin.Z);
    }
    SECTION("a normal facing directly away becomes the outgoing direction")
    {
        REQUIRE(TryClipShadingNormalToOutgoingHemisphere(outgoing * -1.0F, outgoing, clipped) == MathStatus::Success);
        CHECK(Abs(Dot(clipped, outgoing) - 1.0F) <= UNIT_TOLERANCE);
    }
    SECTION("a grazing normal ends the margin inside the hemisphere")
    {
        REQUIRE(TryClipShadingNormalToOutgoingHemisphere({0.0F, 0.8F, -0.6F}, outgoing, clipped) ==
                MathStatus::Success);
        CHECK(Abs(Length(clipped) - 1.0F) <= UNIT_TOLERANCE);
        CHECK(Dot(clipped, outgoing) >= GetMinimumClippedCosine() - UNIT_TOLERANCE);
        CHECK(Dot(clipped, outgoing) <= SHADING_NORMAL_MINIMUM_OUTGOING_COSINE + UNIT_TOLERANCE);
    }
    SECTION("a nonfinite input fails and preserves the output")
    {
        const Vector3 sentinel{7.0F, 8.0F, 9.0F};
        clipped = sentinel;
        const float32 notANumber = std::numeric_limits<float32>::quiet_NaN();
        CHECK(TryClipShadingNormalToOutgoingHemisphere({notANumber, 0.0F, 1.0F}, outgoing, clipped) ==
              MathStatus::NonFiniteInput);
        CHECK(TryClipShadingNormalToOutgoingHemisphere({0.0F, 0.0F, 1.0F},
                                                       {std::numeric_limits<float32>::infinity(), 0.0F, 0.0F},
                                                       clipped) == MathStatus::NonFiniteInput);
        CHECK(clipped.X == sentinel.X);
        CHECK(clipped.Y == sentinel.Y);
        CHECK(clipped.Z == sentinel.Z);
    }
}

TEST_CASE("Clipped shading normals match a float64 evaluation over random unit vectors", "[math][shading-normal]")
{
    RandomStream random;
    REQUIRE(random.TryReseed(RANDOM_SEED, 0) == true);
    uint32 clippedCount = 0;
    for (uint32 i = 0; i < CASE_COUNT; ++i)
    {
        const Vector3 normal = DrawUnitVector(random);
        const Vector3 outgoing = DrawUnitVector(random);
        CAPTURE(i, normal.X, normal.Y, normal.Z, outgoing.X, outgoing.Y, outgoing.Z);
        Vector3 clipped;
        REQUIRE(TryClipShadingNormalToOutgoingHemisphere(normal, outgoing, clipped) == MathStatus::Success);
        const float32 cosine = Dot(normal, outgoing);
        if (cosine >= SHADING_NORMAL_MINIMUM_OUTGOING_COSINE)
        {
            CHECK(clipped.X == normal.X);
            CHECK(clipped.Y == normal.Y);
            CHECK(clipped.Z == normal.Z);
            continue;
        }
        ++clippedCount;
        // n + (m - c) v in float64, from the same float32 cosine, then normalized.
        const float64 shift =
            static_cast<float64>(SHADING_NORMAL_MINIMUM_OUTGOING_COSINE) - static_cast<float64>(cosine);
        const float64 x = static_cast<float64>(normal.X) + shift * static_cast<float64>(outgoing.X);
        const float64 y = static_cast<float64>(normal.Y) + shift * static_cast<float64>(outgoing.Y);
        const float64 z = static_cast<float64>(normal.Z) + shift * static_cast<float64>(outgoing.Z);
        const float64 length = std::sqrt(x * x + y * y + z * z);
        // Normalization divides the float32 sum's rounding by its length, which shrinks for a normal facing away.
        const float64 tolerance = static_cast<float64>(UNIT_TOLERANCE) / length;
        CHECK(std::abs(static_cast<float64>(clipped.X) - x / length) <= tolerance);
        CHECK(std::abs(static_cast<float64>(clipped.Y) - y / length) <= tolerance);
        CHECK(std::abs(static_cast<float64>(clipped.Z) - z / length) <= tolerance);
        // The clipped cosine is m / sqrt(1 + m^2 - c^2): its minimum at c = 0, and one for a normal facing away.
        const float64 clippedCosine = static_cast<float64>(Dot(clipped, outgoing));
        CHECK(clippedCosine >= static_cast<float64>(GetMinimumClippedCosine() - UNIT_TOLERANCE));
        CHECK(std::abs(clippedCosine - static_cast<float64>(SHADING_NORMAL_MINIMUM_OUTGOING_COSINE) / length) <=
              tolerance);
    }
    // About half of the uniformly drawn pairs face apart.
    CHECK(clippedCount > CASE_COUNT / 3);
    CHECK(clippedCount < CASE_COUNT * 2 / 3);
}
