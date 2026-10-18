#pragma once

// Private outward-rounded control-point boxes shared by the Certified length
// bounds of prepared spans (G2) and the Certified tessellation bounds of
// prepared patches (G4).
//
// Subcurve controls come from the polar form (blossom) of the cubic, following
// Lyle Ramshaw, "Blossoming: A Connect-the-Dots Approach to Splines", Digital
// Systems Research Center Report 19, 1987: the controls of the subcurve [a, b]
// are f(a,a,a), f(a,a,b), f(a,b,b) and f(b,b,b). Each blossom value is three
// rounds of linear interpolation evaluated with enclosures, so the boxes
// contain the exact subcurve controls for exact float64 parameters a and b.
// A tensor-product patch applies the same construction to its rows and then to
// the resulting columns, because its blossom is multi-affine in each direction.

#include <ludus/foundation/base/core.h>

#include <ludus/foundation/containers/static_array.hpp>
#include <ludus/foundation/geometry/status.hpp>
#include <ludus/foundation/math/enclosure.hpp>
#include <ludus/foundation/math/status.hpp>
#include <ludus/foundation/math/vector.hpp>

namespace ludus::foundation::geometry::internal
{
using core::StaticArray;
using math::Enclosure;
using math::Vector3;

/// Controls of one cubic Bezier curve.
inline constexpr usize CUBIC_CONTROL_COUNT = 4;

/// Axis enclosures of one exact point.
struct ControlBox final
{
    Enclosure X;
    Enclosure Y;
    Enclosure Z;
};

/// Enclosed controls of one cubic curve.
using CubicControlBoxes = StaticArray<ControlBox, CUBIC_CONTROL_COUNT>;

/// Translate a FoundationMath status at the Geometry boundary (ADR 0024).
[[nodiscard]] GeometryStatus ToGeometryStatus(math::MathStatus status) noexcept;

/// Box that contains exactly one stored point.
[[nodiscard]] ControlBox MakePointBox(const Vector3& point) noexcept;

/// Enclose (1 - t) * start + t * end for an exact parameter t.
[[nodiscard]] math::MathStatus
TryInterpolateBox(const ControlBox& start, const ControlBox& end, float64 t, ControlBox& outBox) noexcept;

/// Enclose the blossom f(first, second, third) of a cubic whose exact controls lie in control.
[[nodiscard]] math::MathStatus TryEvaluateBlossom(const CubicControlBoxes& control,
                                                  float64 first,
                                                  float64 second,
                                                  float64 third,
                                                  ControlBox& outPoint) noexcept;

/// Enclose the controls of the cubic restricted to [a, b], with 0 <= a <= b <= 1 exact.
[[nodiscard]] math::MathStatus TryEncloseSubcurveControls(const CubicControlBoxes& control,
                                                          float64 a,
                                                          float64 b,
                                                          CubicControlBoxes& outControls) noexcept;

/// Enclose left - right componentwise for every pair of points inside the boxes.
[[nodiscard]] math::MathStatus
TrySubtractBoxes(const ControlBox& left, const ControlBox& right, ControlBox& outBox) noexcept;

/// Enclose left + right componentwise for every pair of points inside the boxes.
[[nodiscard]] math::MathStatus
TryAddBoxes(const ControlBox& left, const ControlBox& right, ControlBox& outBox) noexcept;

/// Enclose factor * point componentwise.
[[nodiscard]] math::MathStatus TryScaleBox(const ControlBox& box, Enclosure factor, ControlBox& outBox) noexcept;

/// Enclose the Euclidean length of every vector inside the box.
[[nodiscard]] math::MathStatus TryEncloseBoxLength(const ControlBox& box, Enclosure& outLength) noexcept;

/// Enclose the distance between every pair of points inside the boxes.
[[nodiscard]] math::MathStatus
TryEncloseDistance(const ControlBox& start, const ControlBox& end, Enclosure& outDistance) noexcept;
} // namespace ludus::foundation::geometry::internal
