#pragma once

// Vector values for FoundationMath: Vector2/3/4 (float32) and Vector3d
// (float64). Plain standard-layout aggregates with named, debugger-visible
// components and no hidden padding, SIMD fields, caches or swizzle proxies.
// Default construction zero-initialises every component.
//
// Conventions (ADR 0010 §3): right-handed, +Y up; Cross(+X,+Y)=+Z. Component
// multiply is the named Hadamard; there is deliberately NO Vector*Vector
// operator and NO fuzzy operator==/operator< (exact equality compares
// components: +0==-0, NaN!=NaN).
//
// Robust Length/TryNormalize use component scaling (design §5) so large finite
// vectors whose raw dot would overflow, and tiny vectors whose square would
// underflow, are still handled; those live in vector.cpp.

#include <ludus/foundation/base/core.h>

#include <ludus/foundation/math/scalar.hpp>
#include <ludus/foundation/math/status.hpp>

namespace ludus::foundation::math
{
using core::float32;
using core::float64;

// --- Vector2 ----------------------------------------------------------------
struct Vector2
{
    float32 X = 0.0f;
    float32 Y = 0.0f;

    [[nodiscard]] friend constexpr bool operator==(const Vector2&, const Vector2&) = default;
};

// --- Vector3 ----------------------------------------------------------------
struct Vector3
{
    float32 X = 0.0f;
    float32 Y = 0.0f;
    float32 Z = 0.0f;

    [[nodiscard]] friend constexpr bool operator==(const Vector3&, const Vector3&) = default;
};

// --- Vector4 ----------------------------------------------------------------
struct Vector4
{
    float32 X = 0.0f;
    float32 Y = 0.0f;
    float32 Z = 0.0f;
    float32 W = 0.0f;

    [[nodiscard]] friend constexpr bool operator==(const Vector4&, const Vector4&) = default;
};

// --- Vector3d (double; for large-coordinate work) ---------------------------
struct Vector3d
{
    float64 X = 0.0;
    float64 Y = 0.0;
    float64 Z = 0.0;

    [[nodiscard]] friend constexpr bool operator==(const Vector3d&, const Vector3d&) = default;
};

// ===========================================================================
// Vector2 arithmetic
// ===========================================================================
[[nodiscard]] constexpr Vector2 operator+(Vector2 a, Vector2 b) noexcept
{
    return Vector2{a.X + b.X, a.Y + b.Y};
}
[[nodiscard]] constexpr Vector2 operator-(Vector2 a, Vector2 b) noexcept
{
    return Vector2{a.X - b.X, a.Y - b.Y};
}
[[nodiscard]] constexpr Vector2 operator-(Vector2 v) noexcept
{
    return Vector2{-v.X, -v.Y};
}
[[nodiscard]] constexpr Vector2 operator*(Vector2 v, float32 s) noexcept
{
    return Vector2{v.X * s, v.Y * s};
}
[[nodiscard]] constexpr Vector2 operator*(float32 s, Vector2 v) noexcept
{
    return Vector2{v.X * s, v.Y * s};
}
[[nodiscard]] constexpr Vector2 operator/(Vector2 v, float32 s) noexcept
{
    return Vector2{v.X / s, v.Y / s};
}
[[nodiscard]] constexpr float32 Dot(Vector2 a, Vector2 b) noexcept
{
    return a.X * b.X + a.Y * b.Y;
}
/// 2D scalar cross (z of the 3D cross of (x,y,0)); signed parallelogram area.
[[nodiscard]] constexpr float32 Cross(Vector2 a, Vector2 b) noexcept
{
    return a.X * b.Y - a.Y * b.X;
}
[[nodiscard]] constexpr Vector2 Hadamard(Vector2 a, Vector2 b) noexcept
{
    return Vector2{a.X * b.X, a.Y * b.Y};
}

// ===========================================================================
// Vector3 arithmetic
// ===========================================================================
[[nodiscard]] constexpr Vector3 operator+(Vector3 a, Vector3 b) noexcept
{
    return Vector3{a.X + b.X, a.Y + b.Y, a.Z + b.Z};
}
[[nodiscard]] constexpr Vector3 operator-(Vector3 a, Vector3 b) noexcept
{
    return Vector3{a.X - b.X, a.Y - b.Y, a.Z - b.Z};
}
[[nodiscard]] constexpr Vector3 operator-(Vector3 v) noexcept
{
    return Vector3{-v.X, -v.Y, -v.Z};
}
[[nodiscard]] constexpr Vector3 operator*(Vector3 v, float32 s) noexcept
{
    return Vector3{v.X * s, v.Y * s, v.Z * s};
}
[[nodiscard]] constexpr Vector3 operator*(float32 s, Vector3 v) noexcept
{
    return Vector3{v.X * s, v.Y * s, v.Z * s};
}
[[nodiscard]] constexpr Vector3 operator/(Vector3 v, float32 s) noexcept
{
    return Vector3{v.X / s, v.Y / s, v.Z / s};
}
[[nodiscard]] constexpr float32 Dot(Vector3 a, Vector3 b) noexcept
{
    return a.X * b.X + a.Y * b.Y + a.Z * b.Z;
}
/// Right-handed cross product: Cross(+X,+Y) = +Z.
[[nodiscard]] constexpr Vector3 Cross(Vector3 a, Vector3 b) noexcept
{
    return Vector3{a.Y * b.Z - a.Z * b.Y, a.Z * b.X - a.X * b.Z, a.X * b.Y - a.Y * b.X};
}
[[nodiscard]] constexpr Vector3 Hadamard(Vector3 a, Vector3 b) noexcept
{
    return Vector3{a.X * b.X, a.Y * b.Y, a.Z * b.Z};
}

// ===========================================================================
// Vector4 arithmetic
// ===========================================================================
[[nodiscard]] constexpr Vector4 operator+(Vector4 a, Vector4 b) noexcept
{
    return Vector4{a.X + b.X, a.Y + b.Y, a.Z + b.Z, a.W + b.W};
}
[[nodiscard]] constexpr Vector4 operator-(Vector4 a, Vector4 b) noexcept
{
    return Vector4{a.X - b.X, a.Y - b.Y, a.Z - b.Z, a.W - b.W};
}
[[nodiscard]] constexpr Vector4 operator-(Vector4 v) noexcept
{
    return Vector4{-v.X, -v.Y, -v.Z, -v.W};
}
[[nodiscard]] constexpr Vector4 operator*(Vector4 v, float32 s) noexcept
{
    return Vector4{v.X * s, v.Y * s, v.Z * s, v.W * s};
}
[[nodiscard]] constexpr Vector4 operator*(float32 s, Vector4 v) noexcept
{
    return Vector4{v.X * s, v.Y * s, v.Z * s, v.W * s};
}
[[nodiscard]] constexpr Vector4 operator/(Vector4 v, float32 s) noexcept
{
    return Vector4{v.X / s, v.Y / s, v.Z / s, v.W / s};
}
[[nodiscard]] constexpr float32 Dot(Vector4 a, Vector4 b) noexcept
{
    return a.X * b.X + a.Y * b.Y + a.Z * b.Z + a.W * b.W;
}
[[nodiscard]] constexpr Vector4 Hadamard(Vector4 a, Vector4 b) noexcept
{
    return Vector4{a.X * b.X, a.Y * b.Y, a.Z * b.Z, a.W * b.W};
}

// ===========================================================================
// Vector3d arithmetic (ordinary; robust length in the .cpp)
// ===========================================================================
[[nodiscard]] constexpr Vector3d operator+(Vector3d a, Vector3d b) noexcept
{
    return Vector3d{a.X + b.X, a.Y + b.Y, a.Z + b.Z};
}
[[nodiscard]] constexpr Vector3d operator-(Vector3d a, Vector3d b) noexcept
{
    return Vector3d{a.X - b.X, a.Y - b.Y, a.Z - b.Z};
}
[[nodiscard]] constexpr Vector3d operator-(Vector3d v) noexcept
{
    return Vector3d{-v.X, -v.Y, -v.Z};
}
[[nodiscard]] constexpr Vector3d operator*(Vector3d v, float64 s) noexcept
{
    return Vector3d{v.X * s, v.Y * s, v.Z * s};
}
[[nodiscard]] constexpr Vector3d operator*(float64 s, Vector3d v) noexcept
{
    return Vector3d{v.X * s, v.Y * s, v.Z * s};
}
/// Componentwise binary64 product; does not normalize or change coordinate space.
[[nodiscard]] constexpr Vector3d Hadamard(Vector3d first, Vector3d second) noexcept
{
    return Vector3d{first.X * second.X, first.Y * second.Y, first.Z * second.Z};
}
[[nodiscard]] constexpr float64 Dot(Vector3d a, Vector3d b) noexcept
{
    return a.X * b.X + a.Y * b.Y + a.Z * b.Z;
}
/// Right-handed cross product at double precision: Cross(+X,+Y) = +Z.
[[nodiscard]] constexpr Vector3d Cross(Vector3d a, Vector3d b) noexcept
{
    return Vector3d{a.Y * b.Z - a.Z * b.Y, a.Z * b.X - a.X * b.Z, a.X * b.Y - a.Y * b.X};
}

// --- Finite checks ----------------------------------------------------------
[[nodiscard]] constexpr bool IsFinite(Vector2 v) noexcept
{
    return IsFinite(v.X) && IsFinite(v.Y);
}
[[nodiscard]] constexpr bool IsFinite(Vector3 v) noexcept
{
    return IsFinite(v.X) && IsFinite(v.Y) && IsFinite(v.Z);
}
[[nodiscard]] constexpr bool IsFinite(Vector4 v) noexcept
{
    return IsFinite(v.X) && IsFinite(v.Y) && IsFinite(v.Z) && IsFinite(v.W);
}
[[nodiscard]] constexpr bool IsFinite(Vector3d v) noexcept
{
    return IsFinite(v.X) && IsFinite(v.Y) && IsFinite(v.Z);
}

// --- Componentwise lerp (same endpoint-preserving behaviour as scalar Lerp) --
[[nodiscard]] Vector2 Lerp(Vector2 a, Vector2 b, float32 t) noexcept;
[[nodiscard]] Vector3 Lerp(Vector3 a, Vector3 b, float32 t) noexcept;
[[nodiscard]] Vector4 Lerp(Vector4 a, Vector4 b, float32 t) noexcept;

// --- LengthSquared / Length / Distance --------------------------------------
/// @brief LengthSquared and Dot are ordinary arithmetic and may overflow for extreme inputs (no range-robustness
/// claim).
///
/// Length uses scaled components so it does not overflow/underflow spuriously, but may still overflow if the true
/// length exceeds the return type's finite range.
[[nodiscard]] constexpr float32 LengthSquared(Vector2 v) noexcept
{
    return Dot(v, v);
}
[[nodiscard]] constexpr float32 LengthSquared(Vector3 v) noexcept
{
    return Dot(v, v);
}
[[nodiscard]] constexpr float32 LengthSquared(Vector4 v) noexcept
{
    return Dot(v, v);
}
[[nodiscard]] constexpr float64 LengthSquared(Vector3d v) noexcept
{
    return Dot(v, v);
}

[[nodiscard]] float32 Length(Vector2 v) noexcept;
[[nodiscard]] float32 Length(Vector3 v) noexcept;
[[nodiscard]] float32 Length(Vector4 v) noexcept;
[[nodiscard]] float64 Length(Vector3d v) noexcept;

[[nodiscard]] float32 Distance(Vector2 a, Vector2 b) noexcept;
[[nodiscard]] float32 Distance(Vector3 a, Vector3 b) noexcept;
[[nodiscard]] float64 Distance(Vector3d a, Vector3d b) noexcept;

// --- Checked normalization --------------------------------------------------
/// @brief TryNormalize: requires finite v and finite minLength >= 0.
///
/// Fails Degenerate for a zero vector (always) or a vector whose true length is <= minLength; fails NonFiniteInput
/// for non-finite v or minLength; fails InvalidArgument for negative minLength. minLength is a LENGTH, not a
/// squared length; default 0 admits any representable non-zero finite vector. On any failure *out is unchanged
/// (aliasing with v is supported). The unit result is always finite.
[[nodiscard]] MathStatus TryNormalize(Vector2 v, float32 minLength, Vector2& out) noexcept;
[[nodiscard]] MathStatus TryNormalize(Vector3 v, float32 minLength, Vector3& out) noexcept;
[[nodiscard]] MathStatus TryNormalize(Vector4 v, float32 minLength, Vector4& out) noexcept;
[[nodiscard]] MathStatus TryNormalize(Vector2 v, Vector2& out) noexcept;
[[nodiscard]] MathStatus TryNormalize(Vector3 v, Vector3& out) noexcept;
[[nodiscard]] MathStatus TryNormalize(Vector4 v, Vector4& out) noexcept;

/// @brief TryNormalizeOr: on a Degenerate v, writes the caller-supplied `fallback` (which must itself be a finite
/// unit vector within unitTol, else InvalidArgument).
///
/// It never invents a default direction. Non-finite v still fails NonFiniteInput. Prefer TryNormalize unless a
/// validated fallback exists.
[[nodiscard]] MathStatus
TryNormalizeOr(Vector3 v, float32 minLength, Vector3 fallback, float32 unitTol, Vector3& out) noexcept;
} // namespace ludus::foundation::math
