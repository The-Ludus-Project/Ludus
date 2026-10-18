#include <ludus/foundation/math/cubic.hpp>

#include "internal/bezier.hpp"

namespace ludus::foundation::math
{
namespace
{
using core::usize;

template <class Curve>
[[nodiscard]] MathStatus Load(const Curve& curve, float64 u, Vector3d* control) noexcept
{
    const MathStatus status = internal::CheckParameter(u);
    if (IsSuccess(status) == false)
    {
        return status;
    }
    for (usize i = 0; i < 4; ++i)
    {
        if (IsFinite(curve.Control[i]) == false)
        {
            return MathStatus::NonFiniteInput;
        }
        control[i] = internal::Widen(curve.Control[i]);
    }
    return MathStatus::Success;
}
template <class Curve, class Value>
[[nodiscard]] MathStatus Position(const Curve& curve, float64 u, Value& out) noexcept
{
    Vector3d control[4]{};
    const MathStatus status = Load(curve, u, control);
    if (IsSuccess(status) == false)
    {
        return status;
    }
    Value result{};
    if (internal::Narrow(internal::Evaluate(4, control, u), result) == false)
    {
        return MathStatus::OutOfRange;
    }
    out = result;
    return MathStatus::Success;
}
template <class Curve, class Sample>
[[nodiscard]] MathStatus EvaluateSample(const Curve& curve, float64 u, Sample& out) noexcept
{
    Vector3d control[4]{};
    const MathStatus status = Load(curve, u, control);
    if (IsSuccess(status) == false)
    {
        return status;
    }
    Sample result;
    if (internal::Narrow(internal::Evaluate(4, control, u), result.Position) == false ||
        internal::Narrow(internal::EvaluateDerivative(control, u, 1), result.FirstDerivative) == false ||
        internal::Narrow(internal::EvaluateDerivative(control, u, 2), result.SecondDerivative) == false)
    {
        return MathStatus::OutOfRange;
    }
    out = result;
    return MathStatus::Success;
}
// Thanks to Thomas Lowe, "Nonuniform Splines", Game Programming Gems 4 (2004),
// §2.4, pp. 171-181, for normalized Hermite derivative scaling. We adapt that
// domain contract to Bezier controls; timing, C2 solves and damping are deferred.
// docs/architecture/curves-surfaces-gems-review.md#nonuniform-splines
template <class Hermite, class Curve>
[[nodiscard]] MathStatus FromHermite(const Hermite& hermite, float64 width, Curve& out) noexcept
{
    if (IsFinite(width) == false || IsFinite(hermite.Start) == false || IsFinite(hermite.End) == false ||
        IsFinite(hermite.StartDerivative) == false || IsFinite(hermite.EndDerivative) == false)
    {
        return MathStatus::NonFiniteInput;
    }
    if (width <= 0)
    {
        return MathStatus::InvalidArgument;
    }
    Curve result;
    result.Control[0] = hermite.Start;
    result.Control[3] = hermite.End;
    const float64 scale = width / 3.0;
    if (internal::Narrow(internal::Widen(hermite.Start) + internal::Widen(hermite.StartDerivative) * scale,
                         result.Control[1]) == false ||
        internal::Narrow(internal::Widen(hermite.End) - internal::Widen(hermite.EndDerivative) * scale,
                         result.Control[2]) == false)
    {
        return MathStatus::OutOfRange;
    }
    out = result;
    return MathStatus::Success;
}
template <class Curve>
[[nodiscard]] MathStatus SplitCurve(const Curve& curve, float64 u, Curve& outLeft, Curve& outRight) noexcept
{
    if (&outLeft == &outRight)
    {
        return MathStatus::InvalidArgument;
    }
    Vector3d control[4]{};
    const MathStatus status = Load(curve, u, control);
    if (IsSuccess(status) == false)
    {
        return status;
    }
    const internal::CubicSplit split = internal::Split(control, u);
    Curve leftResult, rightResult;
    for (usize i = 0; i < 4; ++i)
    {
        if (internal::Narrow(split.Left[i], leftResult.Control[i]) == false ||
            internal::Narrow(split.Right[i], rightResult.Control[i]) == false)
        {
            return MathStatus::OutOfRange;
        }
    }
    outLeft = leftResult;
    outRight = rightResult;
    return MathStatus::Success;
}
} // namespace

MathStatus TryEvaluateCubic(const CubicBezier1& curve, float64 u, float32& out) noexcept
{
    return Position(curve, u, out);
}
MathStatus TryEvaluateCubic(const CubicBezier2& curve, float64 u, Vector2& out) noexcept
{
    return Position(curve, u, out);
}
MathStatus TryEvaluateCubic(const CubicBezier3& curve, float64 u, Vector3& out) noexcept
{
    return Position(curve, u, out);
}
MathStatus TryEvaluateCubic(const CubicBezier1& curve, float64 u, CubicSample1& out) noexcept
{
    return EvaluateSample(curve, u, out);
}
MathStatus TryEvaluateCubic(const CubicBezier2& curve, float64 u, CubicSample2& out) noexcept
{
    return EvaluateSample(curve, u, out);
}
MathStatus TryEvaluateCubic(const CubicBezier3& curve, float64 u, CubicSample3& out) noexcept
{
    return EvaluateSample(curve, u, out);
}
MathStatus TryFromHermite(const CubicHermite1& hermite, float64 domainWidth, CubicBezier1& out) noexcept
{
    return FromHermite(hermite, domainWidth, out);
}
MathStatus TryFromHermite(const CubicHermite2& hermite, float64 domainWidth, CubicBezier2& out) noexcept
{
    return FromHermite(hermite, domainWidth, out);
}
MathStatus TryFromHermite(const CubicHermite3& hermite, float64 domainWidth, CubicBezier3& out) noexcept
{
    return FromHermite(hermite, domainWidth, out);
}
MathStatus TrySplitCubic(const CubicBezier1& curve, float64 u, CubicBezier1& outLeft, CubicBezier1& outRight) noexcept
{
    return SplitCurve(curve, u, outLeft, outRight);
}
MathStatus TrySplitCubic(const CubicBezier2& curve, float64 u, CubicBezier2& outLeft, CubicBezier2& outRight) noexcept
{
    return SplitCurve(curve, u, outLeft, outRight);
}
MathStatus TrySplitCubic(const CubicBezier3& curve, float64 u, CubicBezier3& outLeft, CubicBezier3& outRight) noexcept
{
    return SplitCurve(curve, u, outLeft, outRight);
}
} // namespace ludus::foundation::math
