#include <ludus/foundation/math/gaussian.hpp>

#include <ludus/foundation/math/scalar.hpp>

namespace ludus::foundation::math
{
namespace
{
constexpr float32 COVARIANCE_RELATIVE_TOLERANCE = 0.00001F;
}
bool TryComputeGaussianAxes(Vector3 diagonal, Vector3 cross, GaussianAxes& outAxes) noexcept
{
    if (IsFinite(diagonal) == false || IsFinite(cross) == false)
    {
        return false;
    }
    const float32 scale = Max(Max(Abs(diagonal.X), Abs(diagonal.Y)), Abs(diagonal.Z));
    const float32 tolerance = scale * COVARIANCE_RELATIVE_TOLERANCE;
    if (diagonal.X < -tolerance || diagonal.Y < -tolerance || diagonal.Z < -tolerance)
    {
        return false;
    }
    GaussianAxes axes;
    axes.Axis0.X = Sqrt(Max(diagonal.X, 0.0F));
    if (axes.Axis0.X > 0)
    {
        axes.Axis0.Y = cross.X / axes.Axis0.X;
        axes.Axis0.Z = cross.Y / axes.Axis0.X;
    }
    else if (Abs(cross.X) > tolerance || Abs(cross.Y) > tolerance)
    {
        return false;
    }
    const float32 varianceY = diagonal.Y - axes.Axis0.Y * axes.Axis0.Y;
    if (varianceY < -tolerance)
    {
        return false;
    }
    axes.Axis1.Y = Sqrt(Max(varianceY, 0.0F));
    const float32 covarianceYz = cross.Z - axes.Axis0.Y * axes.Axis0.Z;
    if (axes.Axis1.Y > 0)
    {
        axes.Axis1.Z = covarianceYz / axes.Axis1.Y;
    }
    else if (Abs(covarianceYz) > tolerance)
    {
        return false;
    }
    const float32 varianceZ = diagonal.Z - axes.Axis0.Z * axes.Axis0.Z - axes.Axis1.Z * axes.Axis1.Z;
    if (varianceZ < -tolerance)
    {
        return false;
    }
    axes.Axis2.Z = Sqrt(Max(varianceZ, 0.0F));
    if (IsFinite(axes.Axis0) == false || IsFinite(axes.Axis1) == false || IsFinite(axes.Axis2) == false)
    {
        return false;
    }
    outAxes = axes;
    return true;
}
} // namespace ludus::foundation::math
