// Float64 rigid-transform arithmetic for root trajectories.
//
// Quaternion composition follows the FoundationMath Hamilton convention (ADR 0010):
// a * b rotates by b, then a.

#include "internal/root_math.hpp"

#include <ludus/foundation/math/scalar.hpp>

namespace ludus::animation::detail
{
using foundation::float64;
using foundation::int64;
namespace math = foundation::math;

namespace
{
QuaternionD Multiply(const QuaternionD& a, const QuaternionD& b) noexcept
{
    return
    {
        .X = a.W * b.X + b.W * a.X + (a.Y * b.Z - a.Z * b.Y),
        .Y = a.W * b.Y + b.W * a.Y + (a.Z * b.X - a.X * b.Z),
        .Z = a.W * b.Z + b.W * a.Z + (a.X * b.Y - a.Y * b.X),
        .W = a.W * b.W - (a.X * b.X + a.Y * b.Y + a.Z * b.Z),
    };
}

QuaternionD Conjugate(const QuaternionD& value) noexcept
{
    return { .X = -value.X, .Y = -value.Y, .Z = -value.Z, .W = value.W };
}

QuaternionD Normalize(const QuaternionD& value) noexcept
{
    const float64 squaredNorm = value.X * value.X + value.Y * value.Y + value.Z * value.Z + value.W * value.W;
    if (squaredNorm > 0 && math::IsFinite(squaredNorm) == true)
    {
        const float64 inverseNorm = 1.0 / math::Sqrt(squaredNorm);
        return
        {
            .X = value.X * inverseNorm,
            .Y = value.Y * inverseNorm,
            .Z = value.Z * inverseNorm,
            .W = value.W * inverseNorm,
        };
    }
    return value;
}

math::Vector3d Rotate(const QuaternionD& rotation, const math::Vector3d& value) noexcept
{
    // v' = v + 2w (u x v) + 2 u x (u x v) for unit q = (u, w).
    const math::Vector3d axis{rotation.X, rotation.Y, rotation.Z};
    const math::Vector3d first = math::Cross(axis, value);
    const math::Vector3d second = math::Cross(axis, first);
    return value + first * (2.0 * rotation.W) + second * 2.0;
}

bool IsQuaternionFinite(const QuaternionD& value) noexcept
{
    return math::IsFinite(value.X) == true && math::IsFinite(value.Y) == true && math::IsFinite(value.Z) == true &&
           math::IsFinite(value.W) == true;
}
} // namespace

RigidTransformD ConvertToRigid(const RootTransform& value) noexcept
{
    return
    {
        .Translation = {static_cast<float64>(value.Translation.X),
                        static_cast<float64>(value.Translation.Y),
                        static_cast<float64>(value.Translation.Z)},
        .Rotation =
        {
            .X = static_cast<float64>(value.Rotation.X),
            .Y = static_cast<float64>(value.Rotation.Y),
            .Z = static_cast<float64>(value.Rotation.Z),
            .W = static_cast<float64>(value.Rotation.W),
        },
    };
}

bool TryConvertToRootTransform(const RigidTransformD& value, RootTransform& outValue) noexcept
{
    if (IsFinite(value) == false)
    {
        return false;
    }
    RootTransform candidate{};
    const QuaternionD rotation = Normalize(value.Rotation);
    if (math::TryConvertToFloat32(value.Translation.X, candidate.Translation.X) == false ||
        math::TryConvertToFloat32(value.Translation.Y, candidate.Translation.Y) == false ||
        math::TryConvertToFloat32(value.Translation.Z, candidate.Translation.Z) == false ||
        math::TryConvertToFloat32(rotation.X, candidate.Rotation.X) == false ||
        math::TryConvertToFloat32(rotation.Y, candidate.Rotation.Y) == false ||
        math::TryConvertToFloat32(rotation.Z, candidate.Rotation.Z) == false ||
        math::TryConvertToFloat32(rotation.W, candidate.Rotation.W) == false)
    {
        return false;
    }
    if (math::TryNormalize(candidate.Rotation, candidate.Rotation) != math::MathStatus::Success)
    {
        return false;
    }
    outValue = candidate;
    return true;
}

RigidTransformD Compose(const RigidTransformD& left, const RigidTransformD& right) noexcept
{
    return
    {
        .Translation = Rotate(left.Rotation, right.Translation) + left.Translation,
        .Rotation = Normalize(Multiply(left.Rotation, right.Rotation)),
    };
}

RigidTransformD Invert(const RigidTransformD& value) noexcept
{
    const QuaternionD inverseRotation = Conjugate(value.Rotation);
    return
    {
        .Translation = -Rotate(inverseRotation, value.Translation),
        .Rotation = inverseRotation,
    };
}

RigidTransformD RaiseToPower(const RigidTransformD& value, int64 exponent) noexcept
{
    RigidTransformD base = exponent < 0 ? Invert(value) : value;
    // MAX_CLIP_TRAVERSAL_CYCLES bounds callers far below the int64 minimum, so negation is safe.
    int64 remaining = exponent < 0 ? -exponent : exponent;
    RigidTransformD result{};
    while (remaining > 0)
    {
        if ((remaining & 1) != 0)
        {
            result = Compose(result, base);
        }
        remaining >>= 1;
        if (remaining > 0)
        {
            base = Compose(base, base);
        }
    }
    return result;
}

bool IsFinite(const RigidTransformD& value) noexcept
{
    return math::IsFinite(value.Translation) == true && IsQuaternionFinite(value.Rotation) == true;
}
} // namespace ludus::animation::detail
