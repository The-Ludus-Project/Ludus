#pragma once

#include <ludus/foundation/base/core.h>

#include <ludus/foundation/math/scalar.hpp>
#include <ludus/foundation/math/status.hpp>
#include <ludus/foundation/math/vector.hpp>

#include <cmath>
#include <limits>

namespace ludus::foundation::math::internal
{
using core::usize;

[[nodiscard]] inline Vector3d Widen(float32 value) noexcept
{
    return {static_cast<float64>(value), 0, 0};
}
[[nodiscard]] inline Vector3d Widen(Vector2 value) noexcept
{
    return {static_cast<float64>(value.X), static_cast<float64>(value.Y), 0};
}
[[nodiscard]] inline Vector3d Widen(Vector3 value) noexcept
{
    return {static_cast<float64>(value.X), static_cast<float64>(value.Y), static_cast<float64>(value.Z)};
}
[[nodiscard]] inline bool Narrow(Vector3d value, float32& out) noexcept
{
    constexpr float64 kLimit = static_cast<float64>(std::numeric_limits<float32>::max());
    if (IsFinite(value.X) == false || Abs(value.X) > kLimit)
    {
        return false;
    }
    out = static_cast<float32>(value.X);
    return true;
}
[[nodiscard]] inline bool Narrow(Vector3d value, Vector2& out) noexcept
{
    Vector2 result;
    if (Narrow({value.X, 0, 0}, result.X) == false || Narrow({value.Y, 0, 0}, result.Y) == false)
    {
        return false;
    }
    out = result;
    return true;
}
[[nodiscard]] inline bool Narrow(Vector3d value, Vector3& out) noexcept
{
    Vector3 result;
    if (Narrow({value.X, 0, 0}, result.X) == false || Narrow({value.Y, 0, 0}, result.Y) == false ||
        Narrow({value.Z, 0, 0}, result.Z) == false)
    {
        return false;
    }
    out = result;
    return true;
}
[[nodiscard]] inline MathStatus CheckParameter(float64 value) noexcept
{
    if (IsFinite(value) == false)
    {
        return MathStatus::NonFiniteInput;
    }
    return value >= 0 && value <= 1 ? MathStatus::Success : MathStatus::InvalidArgument;
}

// Thanks to Philip J. Schneider, "A Bezier Curve-Based Root-Finder", Graphics
// Gems (1990), VIII.2, pp. 408-415, for the subdivision approach consulted here.
// Original Ludus implementation: fixed stack storage and arbitrary split u;
// no root isolation or source code is copied. Review and source locator:
// docs/architecture/curves-surfaces-gems-review.md#nearest-point-on-a-curve
// Publisher contents: https://www.sciencedirect.com/book/9780080507538/graphics-gems
[[nodiscard]] inline Vector3d Interpolate(Vector3d a, Vector3d b, float64 u) noexcept
{
    if (u == 0)
    {
        return a;
    }
    if (u == 1)
    {
        return b;
    }
    return {std::lerp(a.X, b.X, u), std::lerp(a.Y, b.Y, u), std::lerp(a.Z, b.Z, u)};
}
// Caller guarantees 1 <= count <= 4 and normalized u; all inputs are widened
// float32 values or their low-degree differences, so float64 has ample range.
[[nodiscard]] inline Vector3d Evaluate(usize count, const Vector3d* control, float64 u) noexcept
{
    Vector3d work[4]{};
    for (usize i = 0; i < count; ++i)
    {
        work[i] = control[i];
    }
    for (usize width = count; width > 1; --width)
    {
        for (usize i = 0; i + 1 < width; ++i)
        {
            work[i] = Interpolate(work[i], work[i + 1], u);
        }
    }
    return work[0];
}
[[nodiscard]] inline Vector3d EvaluateDerivative(const Vector3d* control, float64 u, usize order) noexcept
{
    Vector3d work[4]{};
    for (usize i = 0; i < 4; ++i)
    {
        work[i] = control[i];
    }
    for (usize degree = 3; degree > 3 - order; --degree)
    {
        for (usize i = 0; i < degree; ++i)
        {
            work[i] = (work[i + 1] - work[i]) * static_cast<float64>(degree);
        }
    }
    return Evaluate(4 - order, work, u);
}
struct CubicSplit final
{
    Vector3d Left[4]{};
    Vector3d Right[4]{};
};
[[nodiscard]] inline CubicSplit Split(const Vector3d* control, float64 u) noexcept
{
    CubicSplit result;
    Vector3d work[4]{};
    for (usize i = 0; i < 4; ++i)
    {
        work[i] = control[i];
    }
    result.Left[0] = work[0];
    result.Right[3] = work[3];
    for (usize width = 3; width > 0; --width)
    {
        for (usize i = 0; i < width; ++i)
        {
            work[i] = Interpolate(work[i], work[i + 1], u);
        }
        result.Left[4 - width] = work[0];
        result.Right[width - 1] = work[width - 1];
    }
    return result;
}
} // namespace ludus::foundation::math::internal
