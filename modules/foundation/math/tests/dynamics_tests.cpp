#include <ludus/foundation/math/dynamics.hpp>
#include <ludus/foundation/math/random.hpp>
#include <ludus/foundation/math/scalar.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <limits>

using namespace ludus::foundation::math;

TEST_CASE("Linear drag is exact and composes across steps", "[math][dynamics]")
{
    LinearDragStep full;
    LinearDragStep half;
    REQUIRE(IsSuccess(TryComputeLinearDragStep(0.7, 0.5, full)));
    REQUIRE(IsSuccess(TryComputeLinearDragStep(0.7, 0.25, half)));
    REQUIRE(NearlyEqual(full.VelocityScale, std::exp(-0.35), 1e-15, 1e-15));
    REQUIRE(NearlyEqual(full.DistanceScale, half.DistanceScale * (1.0 + half.VelocityScale), 1e-15, 1e-15));
    REQUIRE(NearlyEqual(full.VelocityScale, half.VelocityScale * half.VelocityScale, 1e-15, 1e-15));
    REQUIRE(IsSuccess(TryComputeLinearDragStep(0.0, 2.0, full)));
    REQUIRE(full.VelocityScale == 1.0);
    REQUIRE(full.DistanceScale == 2.0);
    REQUIRE(IsSuccess(TryComputeLinearDragStep(1e-20, 1.0, full)));
    REQUIRE(full.DistanceScale == 1.0); // Naive 1-exp(-x) would produce zero.
    REQUIRE(IsSuccess(TryComputeLinearDragStep(1e-300, 1e-300, full)));
    REQUIRE(full.DistanceScale == 1e-300); // Underflowing product is ballistic.
    REQUIRE(IsSuccess(TryComputeLinearDragStep(1e300, 1e300, full)));
    REQUIRE(full.VelocityScale == 0.0);
    REQUIRE(full.DistanceScale == 1e-300);
    REQUIRE(IsSuccess(TryComputeLinearDragStep(1.0, 0.0, full)));
    REQUIRE(full.DistanceScale == 0.0);
}

TEST_CASE("Invalid drag preserves caller output", "[math][dynamics]")
{
    LinearDragStep value{0.25, 12.0};
    REQUIRE(TryComputeLinearDragStep(-1.0, 1.0, value) == MathStatus::InvalidArgument);
    REQUIRE(TryComputeLinearDragStep(1.0, -1.0, value) == MathStatus::InvalidArgument);
    REQUIRE(TryComputeLinearDragStep(std::numeric_limits<float64>::infinity(), 1.0, value) ==
            MathStatus::NonFiniteInput);
    REQUIRE(value.VelocityScale == 0.25);
    REQUIRE(value.DistanceScale == 12.0);
}

TEST_CASE("Radial band covers outer inner shrinking and moving contact", "[math][dynamics]")
{
    RadialSweepContact hit;
    REQUIRE(IsSuccess(TrySweepRadialBand({10, 0, 0}, {10, 0, 0}, {}, 0, 20, 2, hit)));
    REQUIRE(hit.Hit);
    REQUIRE(NearlyEqual(hit.Fraction, 0.4, 1e-14, 1e-14));
    REQUIRE(NearlyEqual(hit.Radius, 8.0, 1e-14, 1e-14));
    REQUIRE(hit.Normal.X == 1.0);
    REQUIRE(IsSuccess(TrySweepRadialBand({}, {10, 0, 0}, {}, 5, 5, 1, hit)));
    REQUIRE(hit.Hit);
    REQUIRE(NearlyEqual(hit.Fraction, 0.4, 1e-14, 1e-14));
    REQUIRE(IsSuccess(TrySweepRadialBand({10, 0, 0}, {10, 0, 0}, {}, 20, 0, 2, hit)));
    REQUIRE(hit.Hit);
    REQUIRE(NearlyEqual(hit.Fraction, 0.4, 1e-14, 1e-14));
    REQUIRE(IsSuccess(TrySweepRadialBand({-10, 0, 0}, {10, 0, 0}, {}, 5, 5, 1, hit)));
    REQUIRE(hit.Hit);
    REQUIRE(NearlyEqual(hit.Fraction, 0.2, 1e-14, 1e-14));
    REQUIRE(hit.Normal.X == -1.0);
    REQUIRE(IsSuccess(TrySweepRadialBand({0, 0, 10}, {0, 0, 10}, {}, 0, 20, 2, hit)));
    REQUIRE(hit.Hit);
    REQUIRE(hit.Normal.Z == 1.0);
}

TEST_CASE("Radial band tangency initial overlap degenerate and miss", "[math][dynamics]")
{
    RadialSweepContact hit;
    REQUIRE(IsSuccess(TrySweepRadialBand({-3, 2, 0}, {3, 2, 0}, {}, 0, 0, 2, hit)));
    REQUIRE(hit.Hit);
    REQUIRE(NearlyEqual(hit.Fraction, 0.5, 1e-14, 1e-14));
    REQUIRE(IsSuccess(TrySweepRadialBand({}, {}, {}, 0, 1, 2, hit)));
    REQUIRE(hit.Hit);
    REQUIRE(hit.Fraction == 0.0);
    REQUIRE(hit.Normal == Vector3d{});
    REQUIRE(IsSuccess(TrySweepRadialBand({}, {}, {}, 0, 0, 0, hit)));
    REQUIRE(hit.Hit);
    REQUIRE(IsSuccess(TrySweepRadialBand({40, 0, 0}, {40, 0, 0}, {}, 0, 30, 2, hit)));
    REQUIRE_FALSE(hit.Hit);
    REQUIRE(IsSuccess(TrySweepRadialBand({}, {}, {}, 5, 10, 1, hit)));
    REQUIRE_FALSE(hit.Hit); // Negative inner-boundary roots must not become hits.
    REQUIRE(IsSuccess(TrySweepRadialBand({5, 0, 0}, {5, 0, 0}, {}, 2, 4, 1, hit)));
    REQUIRE(hit.Hit);
    REQUIRE(NearlyEqual(hit.Fraction, 1.0, 1e-14, 1e-14));
}

TEST_CASE("Radial contact respects geometry scale and stable small root", "[math][dynamics]")
{
    const float64 scales[]{1e-150, 1.0, 1e150};
    for (const float64 scale : scales)
    {
        RadialSweepContact hit;
        REQUIRE(
            IsSuccess(TrySweepRadialBand({10 * scale, 0, 0}, {10 * scale, 0, 0}, {}, 0, 20 * scale, 2 * scale, hit)));
        REQUIRE(hit.Hit);
        REQUIRE(NearlyEqual(hit.Fraction, 0.4, 1e-14, 1e-14));
    }
    RadialSweepContact hit;
    REQUIRE(IsSuccess(TrySweepRadialBand({2, 0, 0}, {1e9, 0, 0}, {}, 3, 3, 0.5, hit)));
    REQUIRE(hit.Hit);
    REQUIRE(NearlyEqual(hit.Fraction, 0.5 / (1e9 - 2.0), 1e-18, 1e-9));
}

TEST_CASE("Radial query validates input and preserves output on failure", "[math][dynamics]")
{
    RadialSweepContact hit{true, 0.75, 8.0, {0, 1, 0}};
    REQUIRE(TrySweepRadialBand({}, {}, {}, -1, 1, 1, hit) == MathStatus::InvalidArgument);
    REQUIRE(TrySweepRadialBand({}, {}, {}, 0, 1, -1, hit) == MathStatus::InvalidArgument);
    REQUIRE(TrySweepRadialBand({std::numeric_limits<float64>::quiet_NaN(), 0, 0}, {}, {}, 0, 1, 1, hit) ==
            MathStatus::NonFiniteInput);
    const float64 huge = std::numeric_limits<float64>::max();
    REQUIRE(TrySweepRadialBand({huge, 0, 0}, {}, {-huge, 0, 0}, 0, 1, 1, hit) == MathStatus::OutOfRange);
    REQUIRE(hit.Hit);
    REQUIRE(hit.Fraction == 0.75);
    REQUIRE(hit.Radius == 8.0);
    REQUIRE(hit.Normal.Y == 1.0);
}

TEST_CASE("Radial sweep agrees with independent sampled overlap", "[math][dynamics]")
{
    RandomStream random;
    for (int caseIndex = 0; caseIndex < 128; ++caseIndex)
    {
        const Vector3d start{40.0 * static_cast<float64>(random.NextFloat01()) - 20.0,
                             40.0 * static_cast<float64>(random.NextFloat01()) - 20.0,
                             0};
        const Vector3d end{40.0 * static_cast<float64>(random.NextFloat01()) - 20.0,
                           40.0 * static_cast<float64>(random.NextFloat01()) - 20.0,
                           0};
        const float64 r0 = 10.0 * static_cast<float64>(random.NextFloat01());
        const float64 r1 = 30.0 * static_cast<float64>(random.NextFloat01());
        const float64 width = 0.1 + 2.0 * static_cast<float64>(random.NextFloat01());
        RadialSweepContact hit;
        REQUIRE(IsSuccess(TrySweepRadialBand(start, end, {}, r0, r1, width, hit)));
        if (hit.Hit)
        {
            const auto point = start * (1.0 - hit.Fraction) + end * hit.Fraction;
            REQUIRE(std::abs(std::hypot(point.X, point.Y) - hit.Radius) <= width + 1e-12);
        }
        // Sampled overlap is only a one-way oracle: a narrow contact may fall
        // between samples, but every sampled overlap must have an earlier hit.
        for (int sample = 0; sample <= 512; ++sample)
        {
            const float64 t = static_cast<float64>(sample) / 512.0;
            const auto point = start * (1.0 - t) + end * t;
            const float64 radius = r0 * (1.0 - t) + r1 * t;
            if (std::abs(std::hypot(point.X, point.Y) - radius) <= width)
            {
                REQUIRE(hit.Hit);
                REQUIRE(hit.Fraction <= t + 1e-12);
                break;
            }
        }
    }
}

TEST_CASE("Exponential damping weight composes and snaps explicitly", "[math][dynamics]")
{
    constexpr float64 HALF_LIFE = 0.2;
    float64 full = -1.0;
    float64 half = -1.0;
    REQUIRE(IsSuccess(TryComputeExponentialDampingWeight(HALF_LIFE, 0.4, full)));
    REQUIRE(IsSuccess(TryComputeExponentialDampingWeight(HALF_LIFE, 0.2, half)));
    REQUIRE(NearlyEqual(half, 0.5, 1e-15, 1e-15));
    REQUIRE(NearlyEqual(full, 0.75, 1e-15, 1e-15));
    // Two half steps leave the same remaining fraction as one full step.
    REQUIRE(NearlyEqual(1.0 - full, (1.0 - half) * (1.0 - half), 1e-15, 1e-15));
    float64 tiny = -1.0;
    REQUIRE(IsSuccess(TryComputeExponentialDampingWeight(1.0, 1e-20, tiny)));
    REQUIRE(tiny > 0.0); // Naive 1-exp(-x) would produce zero.
    REQUIRE(NearlyEqual(tiny, 0.693147180559945309417e-20, 1e-30, 1e-15));
    float64 weight = -1.0;
    REQUIRE(IsSuccess(TryComputeExponentialDampingWeight(0.0, 0.1, weight)));
    REQUIRE(weight == 1.0);
    REQUIRE(IsSuccess(TryComputeExponentialDampingWeight(0.0, 0.0, weight)));
    REQUIRE(weight == 0.0);
    REQUIRE(IsSuccess(TryComputeExponentialDampingWeight(1e-300, 1e300, weight)));
    REQUIRE(weight == 1.0);
    weight = 0.25;
    REQUIRE(TryComputeExponentialDampingWeight(-1.0, 1.0, weight) == MathStatus::InvalidArgument);
    REQUIRE(TryComputeExponentialDampingWeight(1.0, -1.0, weight) == MathStatus::InvalidArgument);
    REQUIRE(TryComputeExponentialDampingWeight(std::numeric_limits<float64>::quiet_NaN(), 1.0, weight) ==
            MathStatus::NonFiniteInput);
    REQUIRE(weight == 0.25);
}

TEST_CASE("Critical spring steps are exact for a held target", "[math][dynamics]")
{
    constexpr float64 SMOOTH_TIME = 0.3;
    const Vector3d start{1.0, -2.0, 0.5};
    const Vector3d startVelocity{0.25, 0.0, -3.0};
    CriticalSpringStep full;
    CriticalSpringStep half;
    REQUIRE(IsSuccess(TryComputeCriticalSpringStep(SMOOTH_TIME, 0.5, full)));
    REQUIRE(IsSuccess(TryComputeCriticalSpringStep(SMOOTH_TIME, 0.25, half)));
    REQUIRE(full.Omega == 2.0 / SMOOTH_TIME);
    Vector3d displacement;
    Vector3d velocity;
    REQUIRE(IsSuccess(TryAdvanceCriticalSpring(full, start, startVelocity, displacement, velocity)));
    Vector3d halfDisplacement;
    Vector3d halfVelocity;
    REQUIRE(IsSuccess(TryAdvanceCriticalSpring(half, start, startVelocity, halfDisplacement, halfVelocity)));
    REQUIRE(IsSuccess(TryAdvanceCriticalSpring(half, halfDisplacement, halfVelocity, halfDisplacement, halfVelocity)));
    REQUIRE(Length(displacement - halfDisplacement) < 1e-14);
    REQUIRE(Length(velocity - halfVelocity) < 1e-13);
    // Closed form x(t) = (e0 + (v0 + omega e0) t) exp(-omega t) for one channel.
    const float64 omega = full.Omega;
    const float64 expected = (start.X + (startVelocity.X + omega * start.X) * 0.5) * std::exp(-omega * 0.5);
    REQUIRE(NearlyEqual(displacement.X, expected, 1e-15, 1e-14));
    // Zero delta advances nothing; a zero smooth time snaps and clears velocity.
    CriticalSpringStep step;
    REQUIRE(IsSuccess(TryComputeCriticalSpringStep(SMOOTH_TIME, 0.0, step)));
    REQUIRE(IsSuccess(TryAdvanceCriticalSpring(step, start, startVelocity, displacement, velocity)));
    REQUIRE(displacement == start);
    REQUIRE(velocity == startVelocity);
    REQUIRE(IsSuccess(TryComputeCriticalSpringStep(0.0, 0.1, step)));
    REQUIRE(step.IsSnap);
    REQUIRE(IsSuccess(TryAdvanceCriticalSpring(step, start, startVelocity, displacement, velocity)));
    REQUIRE(displacement == Vector3d{});
    REQUIRE(velocity == Vector3d{});
}

TEST_CASE("Invalid critical springs preserve caller output", "[math][dynamics]")
{
    CriticalSpringStep step{ .Omega = 7.0 };
    REQUIRE(TryComputeCriticalSpringStep(-1.0, 1.0, step) == MathStatus::InvalidArgument);
    REQUIRE(TryComputeCriticalSpringStep(1.0, -1.0, step) == MathStatus::InvalidArgument);
    REQUIRE(TryComputeCriticalSpringStep(std::numeric_limits<float64>::infinity(), 1.0, step) ==
            MathStatus::NonFiniteInput);
    REQUIRE(TryComputeCriticalSpringStep(std::numeric_limits<float64>::denorm_min(), 1.0, step) ==
            MathStatus::OutOfRange);
    REQUIRE(step.Omega == 7.0);
    REQUIRE(IsSuccess(TryComputeCriticalSpringStep(1.0, 1.0, step)));
    Vector3d displacement{1.0, 2.0, 3.0};
    Vector3d velocity{4.0, 5.0, 6.0};
    REQUIRE(TryAdvanceCriticalSpring(step,
                                     {std::numeric_limits<float64>::quiet_NaN(), 0.0, 0.0},
                                     {},
                                     displacement,
                                     velocity) == MathStatus::NonFiniteInput);
    REQUIRE(TryAdvanceCriticalSpring(step, {1e308, 0.0, 0.0}, {1e308, 0.0, 0.0}, displacement, velocity) ==
            MathStatus::OutOfRange);
    REQUIRE(displacement == Vector3d{1.0, 2.0, 3.0});
    REQUIRE(velocity == Vector3d{4.0, 5.0, 6.0});
}
