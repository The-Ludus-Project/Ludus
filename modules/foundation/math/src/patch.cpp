#include <ludus/foundation/math/patch.hpp>

#include "internal/bezier.hpp"

namespace ludus::foundation::math
{
namespace
{
using core::usize;
using ControlNet = Vector3d[4][4];

[[nodiscard]] MathStatus Load(const BicubicBezierPatch& patch, PatchParameter parameter, ControlNet& control) noexcept
{
    const MathStatus uStatus = internal::CheckParameter(parameter.U);
    if (IsSuccess(uStatus) == false)
    {
        return uStatus;
    }
    const MathStatus vStatus = internal::CheckParameter(parameter.V);
    if (IsSuccess(vStatus) == false)
    {
        return vStatus;
    }
    for (usize u = 0; u < 4; ++u)
    {
        for (usize v = 0; v < 4; ++v)
        {
            if (IsFinite(patch.Control[u][v]) == false)
            {
                return MathStatus::NonFiniteInput;
            }
            control[u][v] = internal::Widen(patch.Control[u][v]);
        }
    }
    return MathStatus::Success;
}
[[nodiscard]] Vector3d Evaluate(const ControlNet& control, PatchParameter parameter, usize du, usize dv) noexcept
{
    Vector3d rows[4]{};
    for (usize u = 0; u < 4; ++u)
    {
        rows[u] = internal::EvaluateDerivative(control[u], parameter.V, dv);
    }
    return internal::EvaluateDerivative(rows, parameter.U, du);
}
[[nodiscard]] bool Narrow(const ControlNet& control, BicubicBezierPatch& out) noexcept
{
    for (usize u = 0; u < 4; ++u)
    {
        for (usize v = 0; v < 4; ++v)
        {
            if (internal::Narrow(control[u][v], out.Control[u][v]) == false)
            {
                return false;
            }
        }
    }
    return true;
}
struct PatchSplit final
{
    ControlNet Low{};
    ControlNet High{};
};
[[nodiscard]] PatchSplit SplitV(const ControlNet& control, float64 v) noexcept
{
    PatchSplit result;
    for (usize u = 0; u < 4; ++u)
    {
        const internal::CubicSplit split = internal::Split(control[u], v);
        for (usize i = 0; i < 4; ++i)
        {
            result.Low[u][i] = split.Left[i];
            result.High[u][i] = split.Right[i];
        }
    }
    return result;
}
[[nodiscard]] Vector3d Divide(Vector3d value, float64 scale) noexcept
{
    return {value.X / scale, value.Y / scale, value.Z / scale};
}
} // namespace

MathStatus TryEvaluatePatch(const BicubicBezierPatch& patch, PatchParameter parameter, Vector3& out) noexcept
{
    ControlNet control{};
    const MathStatus status = Load(patch, parameter, control);
    if (IsSuccess(status) == false)
    {
        return status;
    }
    Vector3 result;
    if (internal::Narrow(Evaluate(control, parameter, 0, 0), result) == false)
    {
        return MathStatus::OutOfRange;
    }
    out = result;
    return MathStatus::Success;
}
MathStatus TryEvaluatePatch(const BicubicBezierPatch& patch, PatchParameter parameter, PatchSample& out) noexcept
{
    ControlNet control{};
    const MathStatus status = Load(patch, parameter, control);
    if (IsSuccess(status) == false)
    {
        return status;
    }
    // Reuse V rows across the six outputs instead of evaluating each tensor
    // partial from scratch. Fixed stack storage; no prepared or mutable cache.
    Vector3d positions[4]{}, dv[4]{}, dvv[4]{};
    for (usize u = 0; u < 4; ++u)
    {
        positions[u] = internal::EvaluateDerivative(control[u], parameter.V, 0);
        dv[u] = internal::EvaluateDerivative(control[u], parameter.V, 1);
        dvv[u] = internal::EvaluateDerivative(control[u], parameter.V, 2);
    }
    PatchSample result;
    if (internal::Narrow(internal::EvaluateDerivative(positions, parameter.U, 0), result.Position) == false ||
        internal::Narrow(internal::EvaluateDerivative(positions, parameter.U, 1), result.DU) == false ||
        internal::Narrow(internal::EvaluateDerivative(dv, parameter.U, 0), result.DV) == false ||
        internal::Narrow(internal::EvaluateDerivative(positions, parameter.U, 2), result.DUU) == false ||
        internal::Narrow(internal::EvaluateDerivative(dv, parameter.U, 1), result.DUV) == false ||
        internal::Narrow(internal::EvaluateDerivative(dvv, parameter.U, 0), result.DVV) == false)
    {
        return MathStatus::OutOfRange;
    }
    out = result;
    return MathStatus::Success;
}
MathStatus TrySplitPatch(const BicubicBezierPatch& patch, PatchParameter parameter, PatchSubdivision& out) noexcept
{
    ControlNet control{};
    const MathStatus status = Load(patch, parameter, control);
    if (IsSuccess(status) == false)
    {
        return status;
    }
    ControlNet lowU{}, highU{};
    for (usize v = 0; v < 4; ++v)
    {
        Vector3d column[4]{};
        for (usize u = 0; u < 4; ++u)
        {
            column[u] = control[u][v];
        }
        const internal::CubicSplit split = internal::Split(column, parameter.U);
        for (usize u = 0; u < 4; ++u)
        {
            lowU[u][v] = split.Left[u];
            highU[u][v] = split.Right[u];
        }
    }
    PatchSubdivision result;
    PatchSplit split = SplitV(lowU, parameter.V);
    if (Narrow(split.Low, result.U0V0) == false || Narrow(split.High, result.U0V1) == false)
    {
        return MathStatus::OutOfRange;
    }
    split = SplitV(highU, parameter.V);
    if (Narrow(split.Low, result.U1V0) == false || Narrow(split.High, result.U1V1) == false)
    {
        return MathStatus::OutOfRange;
    }
    out = result;
    return MathStatus::Success;
}

// Thanks to Martin Brownlow, "Fast Patch Normals", Game Programming Gems 3
// (2002), §4.3, pp. 349-352, for distinguishing approximate control-normal
// interpolation from geometric normals. Our deliberate departure uses analytic
// partials, with explicit singularity/conditioning checks; no shading field.
// docs/architecture/curves-surfaces-gems-review.md#fast-patch-normals
MathStatus TryPatchNormal(const PatchSample& sample, PatchNormalPolicy policy, Vector3& out) noexcept
{
    if (IsFinite(sample.DU) == false || IsFinite(sample.DV) == false || IsFinite(policy.MinDerivativeLength) == false ||
        IsFinite(policy.MinSinAngle) == false)
    {
        return MathStatus::NonFiniteInput;
    }
    if (policy.MinDerivativeLength < 0 || policy.MinSinAngle < 0 || policy.MinSinAngle >= 1)
    {
        return MathStatus::InvalidArgument;
    }
    const Vector3d du = internal::Widen(sample.DU), dv = internal::Widen(sample.DV);
    const float64 uLength = std::hypot(du.X, du.Y, du.Z), vLength = std::hypot(dv.X, dv.Y, dv.Z);
    if (uLength == 0 || vLength == 0)
    {
        return MathStatus::Degenerate;
    }
    // Products of widened float32 components fit exactly in float64, even at
    // extreme/subnormal magnitudes. Cross before normalization preserves exact
    // parallelism; normalizing each partial first could create a spurious cross.
    const Vector3d cross{du.Y * dv.Z - du.Z * dv.Y, du.Z * dv.X - du.X * dv.Z, du.X * dv.Y - du.Y * dv.X};
    const float64 crossLength = std::hypot(cross.X, cross.Y, cross.Z);
    if (crossLength == 0)
    {
        return MathStatus::Degenerate;
    }
    const float64 sinAngle = (crossLength / uLength) / vLength;
    if (uLength <= policy.MinDerivativeLength || vLength <= policy.MinDerivativeLength ||
        sinAngle <= policy.MinSinAngle)
    {
        return MathStatus::IllConditioned;
    }
    Vector3 result;
    if (internal::Narrow(Divide(cross, crossLength), result) == false)
    {
        return MathStatus::OutOfRange;
    }
    out = result;
    return MathStatus::Success;
}
} // namespace ludus::foundation::math
