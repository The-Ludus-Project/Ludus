#include <ludus/foundation/math/gaussian.hpp>
#include <ludus/foundation/math/scalar.hpp>

#include <catch2/catch_test_macros.hpp>

using namespace ludus::foundation;
using namespace ludus::foundation::math;
TEST_CASE("Gaussian axes reconstruct correlated and singular covariance", "[math][gaussian]")
{
    GaussianAxes axes;
    REQUIRE(TryComputeGaussianAxes({4, 5, 9}, {2, 2, 3}, axes) == true);
    const Vector3 diagonal =
        Hadamard(axes.Axis0, axes.Axis0) + Hadamard(axes.Axis1, axes.Axis1) + Hadamard(axes.Axis2, axes.Axis2);
    CHECK(Abs(diagonal.X - 4) < 0.00001F);
    CHECK(Abs(diagonal.Y - 5) < 0.00001F);
    CHECK(Abs(diagonal.Z - 9) < 0.00001F);
    CHECK(Abs(axes.Axis0.X * axes.Axis0.Y - 2) < 0.00001F);
    CHECK(Abs(axes.Axis0.Y * axes.Axis0.Z + axes.Axis1.Y * axes.Axis1.Z - 3) < 0.00001F);
    REQUIRE(TryComputeGaussianAxes({0, 1, 1}, {0, 0, 1}, axes) == true);
    CHECK(axes.Axis0.X == 0);
    CHECK(axes.Axis1.Z == 1);
    CHECK(axes.Axis2.Z == 0);
    const GaussianAxes previous = axes;
    CHECK(TryComputeGaussianAxes({1, 1, 1}, {2, 0, 0}, axes) == false);
    CHECK(axes.Axis1.Z == previous.Axis1.Z);
    CHECK(TryComputeGaussianAxes({Sqrt(-1.0F), 1, 1}, {0, 0, 0}, axes) == false);
    CHECK(axes.Axis1.Z == previous.Axis1.Z);
    CHECK(TryComputeGaussianAxes({0, 0, 0}, {0, 0, 0}, axes) == true);
    CHECK(axes.Axis2.Z == 0);
}
