// Large-coordinate relative conversion (design §10). Precise FP flags;
// <cmath>/<limits> permitted (ADR 0003/0010).

#include <ludus/foundation/math/precision.hpp>

#include <cmath>

namespace ludus::foundation::math
{
// Thanks to Peter Freese, "Solving Accuracy Problems in Large World Coordinates",
// Game Programming Gems 4 (2004), section 2.3, pp. 157-170, for using a shared
// origin and subtracting before local arithmetic. Ludus uses double world
// coordinates and checked float narrowing, not the chapter's cell-plus-offset
// representation. Source scope: docs/architecture/math-research.md.
MathStatus TryMakeRelative(Vector3d position, Vector3d origin, float64 maxAbsComponent, Vector3& out) noexcept
{
    if (IsFinite(position) == false || IsFinite(origin) == false || std::isfinite(maxAbsComponent) == false)
    {
        return MathStatus::NonFiniteInput;
    }
    if ((maxAbsComponent > 0.0) == false)
    {
        return MathStatus::InvalidArgument;
    }
    // Subtract in double FIRST (this is the whole point).
    const float64 dx = position.X - origin.X;
    const float64 dy = position.Y - origin.Y;
    const float64 dz = position.Z - origin.Z;
    if (std::isfinite(dx) == false || std::isfinite(dy) == false || std::isfinite(dz) == false)
    {
        return MathStatus::OutOfRange;
    }
    if (std::fabs(dx) > maxAbsComponent || std::fabs(dy) > maxAbsComponent || std::fabs(dz) > maxAbsComponent)
    {
        return MathStatus::OutOfRange;
    }
    const float32 fx = static_cast<float32>(dx);
    const float32 fy = static_cast<float32>(dy);
    const float32 fz = static_cast<float32>(dz);
    if (IsFinite(fx) == false || IsFinite(fy) == false || IsFinite(fz) == false)
    {
        return MathStatus::OutOfRange;
    }
    out = Vector3{fx, fy, fz};
    return MathStatus::Success;
}
} // namespace ludus::foundation::math
