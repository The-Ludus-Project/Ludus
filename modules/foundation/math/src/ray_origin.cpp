#include <ludus/foundation/math/queries.hpp>

#include <cmath>
#include <limits>

// Thanks to Matt Pharr, Wenzel Jakob and Greg Humphreys, "Physically Based
// Rendering: From Theory to Implementation", 4th edition (2023), section 6.1,
// https://pbr-book.org/4ed/Geometry_and_Transformations/Interactions:
// project position error on the oriented geometric normal and round away.
// Ludus additionally uses a minimum *normal* step near zero for GPU FTZ behavior;
// callers retain responsibility for conservative reconstruction error bounds.
namespace ludus::foundation::math
{
MathStatus TryOffsetRayOrigin(const RayOriginOffset& input, Vector3& output) noexcept
{
    if (IsFinite(input.Position) == false || IsFinite(input.Error) == false ||
        IsFinite(input.GeometricNormal) == false || IsFinite(input.Direction) == false)
    {
        return MathStatus::NonFiniteInput;
    }
    const auto n2 = Dot(input.GeometricNormal, input.GeometricNormal), d2 = Dot(input.Direction, input.Direction);
    if (input.Error.X < 0 || input.Error.Y < 0 || input.Error.Z < 0 || n2 < 0.99999F || n2 > 1.00001F ||
        d2 < 0.99999F || d2 > 1.00001F)
    {
        return MathStatus::InvalidArgument;
    }
    auto normal = input.GeometricNormal;
    if (Dot(normal, input.Direction) < 0)
    {
        normal = -normal;
    }
    const float64 distance = std::fabs(float64(normal.X)) * float64(input.Error.X) +
                             std::fabs(float64(normal.Y)) * float64(input.Error.Y) +
                             std::fabs(float64(normal.Z)) * float64(input.Error.Z);
    const float32 position[3]{input.Position.X, input.Position.Y, input.Position.Z};
    const float32 direction[3]{normal.X, normal.Y, normal.Z};
    float32 values[3]{};
    for (core::usize axis = 0; axis < 3; ++axis)
    {
        const float64 shifted = float64(position[axis]) + float64(direction[axis]) * distance;
        if (std::isfinite(shifted) == false || std::fabs(shifted) > float64(std::numeric_limits<float32>::max()))
        {
            return MathStatus::OutOfRange;
        }
        auto value = static_cast<float32>(shifted);
        if (direction[axis] != 0)
        {
            value = std::nextafter(value,
                                   direction[axis] > 0 ? std::numeric_limits<float32>::infinity()
                                                       : -std::numeric_limits<float32>::infinity());
            if (std::fabs(value) < std::numeric_limits<float32>::min())
            {
                value =
                    direction[axis] > 0 ? std::numeric_limits<float32>::min() : -std::numeric_limits<float32>::min();
            }
        }
        if (std::isfinite(value) == false)
        {
            return MathStatus::OutOfRange;
        }
        values[axis] = value;
    }
    output = {values[0], values[1], values[2]};
    return MathStatus::Success;
}
} // namespace ludus::foundation::math
