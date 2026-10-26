// Scalar helpers: constants, conversion, lerp endpoints/extrapolation,
// smoothstep, NearlyEqual edge cases, exponential approach.

#include <ludus/foundation/math/scalar.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <limits>

using namespace ludus::foundation::math;

TEST_CASE("Angle conversion round-trips", "[math][scalar]")
{
    REQUIRE(std::fabs(DegreesToRadians(180.0f) - kPiF) < 1e-5f);
    REQUIRE(std::fabs(RadiansToDegrees(kPiF) - 180.0f) < 1e-3f);
    REQUIRE(std::fabs(DegreesToRadians(90.0) - kHalfPi) < 1e-12);
}

TEST_CASE("Lerp preserves endpoints and extrapolates", "[math][scalar]")
{
    REQUIRE(Lerp(2.0f, 5.0f, 0.0f) == 2.0f);
    REQUIRE(Lerp(2.0f, 5.0f, 1.0f) == 5.0f);
    REQUIRE(Lerp(0.0f, 10.0f, 0.5f) == 5.0f);
    REQUIRE(Lerp(0.0f, 10.0f, 2.0f) == 20.0f); // extrapolation
}

TEST_CASE("SmoothStep01 clamps and is symmetric", "[math][scalar]")
{
    REQUIRE(SmoothStep01(-1.0f) == 0.0f);
    REQUIRE(SmoothStep01(2.0f) == 1.0f);
    REQUIRE(std::fabs(SmoothStep01(0.5f) - 0.5f) < 1e-6f);
    REQUIRE(std::isnan(SmoothStep01(std::numeric_limits<float>::quiet_NaN())));
}

TEST_CASE("TrySmoothStep ranged rejects bad bounds", "[math][scalar]")
{
    float out = -1.0f;
    REQUIRE(TrySmoothStep(0.0f, 1.0f, 0.5f, out) == MathStatus::Success);
    REQUIRE(std::fabs(out - 0.5f) < 1e-6f);
    const float keep = 42.0f;
    float v = keep;
    REQUIRE(TrySmoothStep(1.0f, 1.0f, 0.5f, v) == MathStatus::InvalidArgument); // equal
    REQUIRE(v == keep);
    REQUIRE(TrySmoothStep(2.0f, 1.0f, 0.5f, v) == MathStatus::InvalidArgument); // reversed
    REQUIRE(v == keep);
}

TEST_CASE("NearlyEqual tolerance and infinity rules", "[math][scalar]")
{
    REQUIRE(NearlyEqual(1.0f, 1.0f + 1e-7f, 0.0f, 1e-5f));
    REQUIRE_FALSE(NearlyEqual(1.0f, 2.0f, 0.0f, 1e-5f));
    const float inf = std::numeric_limits<float>::infinity();
    REQUIRE_FALSE(NearlyEqual(inf, inf, 1.0f, 1.0f));    // infinities never nearly equal
    REQUIRE_FALSE(NearlyEqual(1.0f, 1.0f, -1.0f, 0.0f)); // negative tol -> false
    REQUIRE(NearlyEqual(1000.0f, 1000.1f, 0.0f, 1e-3f)); // relative tolerance
}

TEST_CASE("TryApproachExponential decay behaviour", "[math][scalar]")
{
    float out = 0.0f;
    REQUIRE(TryApproachExponential(1.0f, 5.0f, 0.0f, 0.1f, out) == MathStatus::Success);
    REQUIRE(out == 1.0f); // rate 0 -> no movement
    REQUIRE(TryApproachExponential(1.0f, 5.0f, 2.0f, 0.0f, out) == MathStatus::Success);
    REQUIRE(out == 1.0f); // dt 0 -> no movement
    REQUIRE(TryApproachExponential(0.0f, 10.0f, 1e6f, 1.0f, out) == MathStatus::Success);
    REQUIRE(std::fabs(out - 10.0f) < 1e-3f); // huge rate saturates to target
    const float keep = 7.0f;
    float v = keep;
    REQUIRE(TryApproachExponential(1.0f, 5.0f, -1.0f, 0.1f, v) == MathStatus::InvalidArgument);
    REQUIRE(v == keep);
}

TEST_CASE("Double scalar exponential and rounding preserve mathematical direction", "[math][scalar]")
{
    CHECK(Exp(0.0) == 1.0);
    CHECK(std::abs(Exp(-1.0) - 0.36787944117144233) < 1e-15);
    CHECK(Floor(-1.25) == -2.0);
    CHECK(Ceil(-1.25) == -1.0);
    CHECK(Floor(1.25) == 1.0);
    CHECK(Ceil(1.25) == 2.0);
}

TEST_CASE("sRGB transfer and FP32 predecessor honor their bounded domains", "[math][color]")
{
    using ludus::foundation::float32;
    CHECK(DecodeSrgb(0) == 0);
    CHECK(DecodeSrgb(1) == 1);
    CHECK(Abs(DecodeSrgb(.04045F) - .0031308F) < 1e-7F);
    for (ludus::foundation::usize i = 0; i <= 255; ++i)
    {
        const float32 channel = float32(i) / 255;
        CHECK(DecodeSrgb(channel) >= 0);
        CHECK(DecodeSrgb(channel) <= 1);
        CHECK(Abs(EncodeSrgb(DecodeSrgb(channel)) - channel) < 2e-7F);
    }
    CHECK_FALSE(IsFinite(DecodeSrgb(-.1F)));
    CHECK_FALSE(IsFinite(EncodeSrgb(1.1F)));
    CHECK(NextFloatDown(1) < 1);
    CHECK(NextFloatDown(0) < 0);
}
