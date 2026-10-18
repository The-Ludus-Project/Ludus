#pragma once

// Private helpers shared by Geometry preparation code: diagnostics, float64
// widening and checked float32 narrowing of chunk-relative controls.

#include <ludus/foundation/base/core.h>

#include <ludus/foundation/geometry/status.hpp>
#include <ludus/foundation/math/scalar.hpp>
#include <ludus/foundation/math/vector.hpp>

namespace ludus::foundation::geometry::internal
{
/// Largest finite float32, the narrowing range of chunk-relative controls.
inline constexpr float64 FLOAT32_LARGEST = 3.4028234663852886e38;

/// Copy diagnostic into the optional output.
inline void WriteDiagnostic(GeometryDiagnostic* outDiagnosticOrNull, const GeometryDiagnostic& diagnostic) noexcept
{
    if (outDiagnosticOrNull != nullptr)
    {
        *outDiagnosticOrNull = diagnostic;
    }
}

/// Report status with its source element and static property path, and return it.
[[nodiscard]] inline GeometryStatus
Fail(GeometryDiagnostic* outDiagnosticOrNull, GeometryStatus status, usize sourceElement, const char* property) noexcept
{
    WriteDiagnostic(outDiagnosticOrNull,
                    GeometryDiagnostic{ .Code = status, .SourceElement = sourceElement, .Property = property });
    return status;
}

/// Exact float64 widening of a float32 vector.
[[nodiscard]] inline math::Vector3d ToDouble(const math::Vector3& value) noexcept
{
    return math::Vector3d{static_cast<float64>(value.X), static_cast<float64>(value.Y), static_cast<float64>(value.Z)};
}

/// |left - right| / max(|left|, |right|); zero when both vanish (Rounded).
[[nodiscard]] inline float64 ComputeRelativeDifference(const math::Vector3d& left, const math::Vector3d& right) noexcept
{
    const float64 scale = math::Max(math::Length(left), math::Length(right));
    return scale == 0.0 ? 0.0 : math::Length(left - right) / scale;
}

/// Largest absolute component of a stored control.
[[nodiscard]] inline float64 GetMaxAbsComponent(const math::Vector3& value) noexcept
{
    return math::Max(math::Abs(static_cast<float64>(value.X)),
                     math::Max(math::Abs(static_cast<float64>(value.Y)), math::Abs(static_cast<float64>(value.Z))));
}

/// Narrow one float64 component to the nearest float32; false when it is outside the float32 range.
[[nodiscard]] inline bool TryNarrowComponent(float64 value, float32& outValue) noexcept
{
    if (math::Abs(value) > FLOAT32_LARGEST)
    {
        return false;
    }
    outValue = static_cast<float32>(value);
    return true;
}

/// Narrow a finite float64 vector; outValue is unchanged when a component is outside the float32 range.
[[nodiscard]] inline bool TryNarrowVector(const math::Vector3d& value, math::Vector3& outValue) noexcept
{
    math::Vector3 result;
    const bool isNarrowed = TryNarrowComponent(value.X, result.X) == true &&
                            TryNarrowComponent(value.Y, result.Y) == true &&
                            TryNarrowComponent(value.Z, result.Z) == true;
    if (isNarrowed == true)
    {
        outValue = result;
    }
    return isNarrowed;
}
} // namespace ludus::foundation::geometry::internal
