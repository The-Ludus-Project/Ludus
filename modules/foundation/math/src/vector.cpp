// Vector length / distance / normalization reference implementations. Precise
// FP flags (see CMakeLists.txt). <cmath>/<limits> permitted here (ADR 0003/0010).

#include <ludus/foundation/math/vector.hpp>

#include <cmath>
#include <limits>

namespace ludus::foundation::math
{
namespace
{
// Scaled robust length for a float32 vector (design §5): s = max|component|,
// u = v/s, length = s * sqrt(dot(u,u)) with the final multiply in float64 so it
// does not overflow in float32. For a zero vector returns 0. Non-finite input
// yields a non-finite result; callers that need finiteness check separately.
[[nodiscard]] float64 ScaledLength2(float32 x, float32 y) noexcept
{
    const float32 s = std::fmax(std::fabs(x), std::fabs(y));
    if (s == 0.0f)
    {
        return 0.0;
    }
    const float64 ux = static_cast<float64>(x) / static_cast<float64>(s);
    const float64 uy = static_cast<float64>(y) / static_cast<float64>(s);
    return static_cast<float64>(s) * std::sqrt(ux * ux + uy * uy);
}
[[nodiscard]] float64 ScaledLength3(float32 x, float32 y, float32 z) noexcept
{
    const float32 s = std::fmax(std::fmax(std::fabs(x), std::fabs(y)), std::fabs(z));
    if (s == 0.0f)
    {
        return 0.0;
    }
    const float64 ux = static_cast<float64>(x) / static_cast<float64>(s);
    const float64 uy = static_cast<float64>(y) / static_cast<float64>(s);
    const float64 uz = static_cast<float64>(z) / static_cast<float64>(s);
    return static_cast<float64>(s) * std::sqrt(ux * ux + uy * uy + uz * uz);
}
[[nodiscard]] float64 ScaledLength4(float32 x, float32 y, float32 z, float32 w) noexcept
{
    const float32 s = std::fmax(std::fmax(std::fabs(x), std::fabs(y)), std::fmax(std::fabs(z), std::fabs(w)));
    if (s == 0.0f)
    {
        return 0.0;
    }
    const float64 ux = static_cast<float64>(x) / static_cast<float64>(s);
    const float64 uy = static_cast<float64>(y) / static_cast<float64>(s);
    const float64 uz = static_cast<float64>(z) / static_cast<float64>(s);
    const float64 uw = static_cast<float64>(w) / static_cast<float64>(s);
    return static_cast<float64>(s) * std::sqrt(ux * ux + uy * uy + uz * uz + uw * uw);
}
[[nodiscard]] float64 ScaledLength3d(float64 x, float64 y, float64 z) noexcept
{
    const float64 s = std::fmax(std::fmax(std::fabs(x), std::fabs(y)), std::fabs(z));
    if (s == 0.0)
    {
        return 0.0;
    }
    const float64 ux = x / s;
    const float64 uy = y / s;
    const float64 uz = z / s;
    return s * std::sqrt(ux * ux + uy * uy + uz * uz);
}
} // namespace

Vector2 Lerp(Vector2 a, Vector2 b, float32 t) noexcept
{
    return Vector2{Lerp(a.X, b.X, t), Lerp(a.Y, b.Y, t)};
}
Vector3 Lerp(Vector3 a, Vector3 b, float32 t) noexcept
{
    return Vector3{Lerp(a.X, b.X, t), Lerp(a.Y, b.Y, t), Lerp(a.Z, b.Z, t)};
}
Vector4 Lerp(Vector4 a, Vector4 b, float32 t) noexcept
{
    return Vector4{Lerp(a.X, b.X, t), Lerp(a.Y, b.Y, t), Lerp(a.Z, b.Z, t), Lerp(a.W, b.W, t)};
}

float32 Length(Vector2 v) noexcept
{
    return static_cast<float32>(ScaledLength2(v.X, v.Y));
}
float32 Length(Vector3 v) noexcept
{
    return static_cast<float32>(ScaledLength3(v.X, v.Y, v.Z));
}
float32 Length(Vector4 v) noexcept
{
    return static_cast<float32>(ScaledLength4(v.X, v.Y, v.Z, v.W));
}
float64 Length(Vector3d v) noexcept
{
    return ScaledLength3d(v.X, v.Y, v.Z);
}

float32 Distance(Vector2 a, Vector2 b) noexcept
{
    return Length(a - b);
}
float32 Distance(Vector3 a, Vector3 b) noexcept
{
    return Length(a - b);
}
float64 Distance(Vector3d a, Vector3d b) noexcept
{
    return Length(a - b);
}

namespace
{
// Shared normalize precondition check: validates finiteness of input and
// threshold, rejects negative threshold, and classifies a zero / too-short
// vector as Degenerate. Returns Success only when a unit result can be formed.
[[nodiscard]] MathStatus NormalizeCommon(float64 length, float32 minLength, bool finiteInput) noexcept
{
    if (finiteInput == false || std::isfinite(minLength) == false)
    {
        return MathStatus::NonFiniteInput;
    }
    if (minLength < 0.0f)
    {
        return MathStatus::InvalidArgument;
    }
    if (length == 0.0 || length <= static_cast<float64>(minLength))
    {
        return MathStatus::Degenerate;
    }
    return MathStatus::Success;
}
} // namespace

MathStatus TryNormalize(Vector2 v, float32 minLength, Vector2& out) noexcept
{
    const float64 length = ScaledLength2(v.X, v.Y);
    const MathStatus status = NormalizeCommon(length, minLength, IsFinite(v));
    if (status != MathStatus::Success)
    {
        return status;
    }
    const float32 ox = static_cast<float32>(static_cast<float64>(v.X) / length);
    const float32 oy = static_cast<float32>(static_cast<float64>(v.Y) / length);
    if (IsFinite(ox) == false || IsFinite(oy) == false)
    {
        return MathStatus::OutOfRange;
    }
    out = Vector2{ox, oy};
    return MathStatus::Success;
}
MathStatus TryNormalize(Vector3 v, float32 minLength, Vector3& out) noexcept
{
    const float64 length = ScaledLength3(v.X, v.Y, v.Z);
    const MathStatus status = NormalizeCommon(length, minLength, IsFinite(v));
    if (status != MathStatus::Success)
    {
        return status;
    }
    const float32 ox = static_cast<float32>(static_cast<float64>(v.X) / length);
    const float32 oy = static_cast<float32>(static_cast<float64>(v.Y) / length);
    const float32 oz = static_cast<float32>(static_cast<float64>(v.Z) / length);
    if (IsFinite(ox) == false || IsFinite(oy) == false || IsFinite(oz) == false)
    {
        return MathStatus::OutOfRange;
    }
    out = Vector3{ox, oy, oz};
    return MathStatus::Success;
}
MathStatus TryNormalize(Vector4 v, float32 minLength, Vector4& out) noexcept
{
    const float64 length = ScaledLength4(v.X, v.Y, v.Z, v.W);
    const MathStatus status = NormalizeCommon(length, minLength, IsFinite(v));
    if (status != MathStatus::Success)
    {
        return status;
    }
    const float32 ox = static_cast<float32>(static_cast<float64>(v.X) / length);
    const float32 oy = static_cast<float32>(static_cast<float64>(v.Y) / length);
    const float32 oz = static_cast<float32>(static_cast<float64>(v.Z) / length);
    const float32 ow = static_cast<float32>(static_cast<float64>(v.W) / length);
    if (IsFinite(ox) == false || IsFinite(oy) == false || IsFinite(oz) == false || IsFinite(ow) == false)
    {
        return MathStatus::OutOfRange;
    }
    out = Vector4{ox, oy, oz, ow};
    return MathStatus::Success;
}

MathStatus TryNormalize(Vector2 v, Vector2& out) noexcept
{
    return TryNormalize(v, 0.0f, out);
}
MathStatus TryNormalize(Vector3 v, Vector3& out) noexcept
{
    return TryNormalize(v, 0.0f, out);
}
MathStatus TryNormalize(Vector4 v, Vector4& out) noexcept
{
    return TryNormalize(v, 0.0f, out);
}

MathStatus TryNormalizeOr(Vector3 v, float32 minLength, Vector3 fallback, float32 unitTol, Vector3& out) noexcept
{
    Vector3 normalized{};
    const MathStatus status = TryNormalize(v, minLength, normalized);
    if (status == MathStatus::Success)
    {
        out = normalized;
        return MathStatus::Success;
    }
    if (status == MathStatus::NonFiniteInput)
    {
        return status;
    }
    // Degenerate / OutOfRange / InvalidArgument(minLength) -> try the fallback,
    // but validate it is a finite unit vector first; never invent a direction.
    if (IsFinite(fallback) == false || std::isfinite(unitTol) == false || unitTol < 0.0f)
    {
        return MathStatus::InvalidArgument;
    }
    const float64 fallbackLength = ScaledLength3(fallback.X, fallback.Y, fallback.Z);
    if (std::fabs(fallbackLength - 1.0) > static_cast<float64>(unitTol))
    {
        return MathStatus::InvalidArgument;
    }
    out = fallback;
    return MathStatus::Success;
}
} // namespace ludus::foundation::math
