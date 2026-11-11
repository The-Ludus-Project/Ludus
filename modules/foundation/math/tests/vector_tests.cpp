// Vector arithmetic, dot/cross/hadamard, robust length and checked
// normalization including extremes, signed zero, failure-unchanged and aliasing.

#include <ludus/foundation/math/vector.hpp>

#include "math_oracle.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <limits>

using namespace ludus::foundation::math;

TEST_CASE("Cross(X,Y)=Z and dot/hadamard", "[math][vector]")
{
    const Vector3 x{1, 0, 0};
    const Vector3 y{0, 1, 0};
    const Vector3 z = Cross(x, y);
    REQUIRE((z.X == 0.0f && z.Y == 0.0f && z.Z == 1.0f));
    REQUIRE(Dot(x, y) == 0.0f);
    REQUIRE((Hadamard(Vector3{2, 3, 4}, Vector3{5, 6, 7}) == Vector3{10, 18, 28}));
}

TEST_CASE("Robust length handles huge and tiny finite vectors", "[math][vector]")
{
    const Vector3 huge{1e30f, 2e30f, 2e30f}; // true length 3e30, finite
    const float32 l = Length(huge);
    REQUIRE(std::isfinite(l));
    REQUIRE(std::fabs(l - 3e30f) / 3e30f < 1e-5f);
    const Vector3 tiny{1e-25f, 0, 0};
    REQUIRE(std::fabs(Length(tiny) - 1e-25f) / 1e-25f < 1e-5f);
}

TEST_CASE("TryNormalize success, unit error, and aliasing", "[math][vector]")
{
    Vector3 out{};
    REQUIRE(TryNormalize(Vector3{3, 0, 4}, out) == MathStatus::Success);
    REQUIRE(std::fabs(ludus::math_test::VecLen(out) - 1.0) < 2e-6);
    REQUIRE(std::fabs(out.X - 0.6f) < 1e-6f);
    REQUIRE(std::fabs(out.Z - 0.8f) < 1e-6f);

    // In-place aliasing.
    Vector3 a{0, 5, 0};
    REQUIRE(TryNormalize(a, a) == MathStatus::Success);
    REQUIRE((a == Vector3{0, 1, 0}));
}

TEST_CASE("TryNormalize accepts finite vectors across the binary32 range", "[math][vector]")
{
    const float32 smallest = std::numeric_limits<float32>::denorm_min();
    const float32 largest = std::numeric_limits<float32>::max();
    const Vector3 unitX{1, 0, 0};

    Vector3 normalized{};
    REQUIRE(TryNormalize(Vector3{smallest, 0, 0}, normalized) == MathStatus::Success);
    REQUIRE(normalized == unitX);
    REQUIRE(TryNormalize(Vector3{smallest, 0, 0}, smallest, normalized) == MathStatus::Degenerate);
    REQUIRE(normalized == unitX);

    constexpr float64 UNIT_LENGTH_TOLERANCE = 2e-6;
    REQUIRE(TryNormalize(Vector3{largest, largest, 0}, largest, normalized) == MathStatus::Success);
    REQUIRE(normalized.X > 0.0f);
    REQUIRE(normalized.X == normalized.Y);
    REQUIRE(normalized.Z == 0.0f);
    REQUIRE(std::fabs(ludus::math_test::VecLen(normalized) - 1.0) < UNIT_LENGTH_TOLERANCE);
}

TEST_CASE("TryNormalize failures leave output unchanged", "[math][vector]")
{
    const Vector3 keep{9, 9, 9};
    Vector3 out = keep;
    REQUIRE(TryNormalize(Vector3{0, 0, 0}, out) == MathStatus::Degenerate);
    REQUIRE(out == keep); // zero vector -> unchanged

    const float nan = std::numeric_limits<float>::quiet_NaN();
    out = keep;
    REQUIRE(TryNormalize(Vector3{nan, 0, 0}, out) == MathStatus::NonFiniteInput);
    REQUIRE(out == keep);

    // Per-component infinities.
    const float inf = std::numeric_limits<float>::infinity();
    out = keep;
    REQUIRE(TryNormalize(Vector3{0, inf, 0}, out) == MathStatus::NonFiniteInput);
    REQUIRE(out == keep);

    // minLength threshold rejects a too-short vector.
    out = keep;
    REQUIRE(TryNormalize(Vector3{0.1f, 0, 0}, 0.5f, out) == MathStatus::Degenerate);
    REQUIRE(out == keep);

    // Negative minLength -> InvalidArgument.
    out = keep;
    REQUIRE(TryNormalize(Vector3{1, 0, 0}, -1.0f, out) == MathStatus::InvalidArgument);
    REQUIRE(out == keep);
}

TEST_CASE("Signed zero vector still fails normalize", "[math][vector]")
{
    Vector3 out{1, 2, 3};
    REQUIRE(TryNormalize(Vector3{-0.0f, -0.0f, -0.0f}, out) == MathStatus::Degenerate);
    REQUIRE((out == Vector3{1, 2, 3}));
}

TEST_CASE("TryNormalizeOr validates fallback, never invents a direction", "[math][vector]")
{
    Vector3 out{};
    // Degenerate input with a valid unit fallback -> fallback.
    REQUIRE(TryNormalizeOr(Vector3{0, 0, 0}, 0.0f, Vector3{0, 1, 0}, 1e-4f, out) == MathStatus::Success);
    REQUIRE((out == Vector3{0, 1, 0}));
    // Non-unit fallback rejected.
    const Vector3 keep{5, 5, 5};
    out = keep;
    REQUIRE(TryNormalizeOr(Vector3{0, 0, 0}, 0.0f, Vector3{0, 2, 0}, 1e-4f, out) == MathStatus::InvalidArgument);
    REQUIRE(out == keep);
    // Non-finite input still fails, never uses fallback.
    out = keep;
    const float nan = std::numeric_limits<float>::quiet_NaN();
    REQUIRE(TryNormalizeOr(Vector3{nan, 0, 0}, 0.0f, Vector3{0, 1, 0}, 1e-4f, out) == MathStatus::NonFiniteInput);
    REQUIRE(out == keep);
}

TEST_CASE("Componentwise lerp preserves endpoints", "[math][vector]")
{
    REQUIRE((Lerp(Vector3{1, 2, 3}, Vector3{4, 5, 6}, 0.0f) == Vector3{1, 2, 3}));
    REQUIRE((Lerp(Vector3{1, 2, 3}, Vector3{4, 5, 6}, 1.0f) == Vector3{4, 5, 6}));
}
