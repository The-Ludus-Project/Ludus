#pragma once

// Private outward-rounded boxes shared by every Certified bound of
// FoundationGeometry: the length bounds of prepared spans (G2), the speed,
// projection and sweep bounds of prepared paths (G3) and the tessellation bounds
// of prepared patches (G4). A box holds one axis enclosure per coordinate, so it
// can contain a control, a query point, a derivative or a difference of points.
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

/// Axis-aligned box whose axis enclosures contain an exact point set.
struct EnclosureBox final
{
    Enclosure X;
    Enclosure Y;
    Enclosure Z;
};

/// Enclosed controls of one cubic curve.
using CubicControlBoxes = StaticArray<EnclosureBox, CUBIC_CONTROL_COUNT>;

/// Translate a FoundationMath status at the Geometry boundary (ADR 0024).
[[nodiscard]] GeometryStatus ToGeometryStatus(math::MathStatus status) noexcept;

/// Box that contains exactly one stored float32 point, widened exactly.
[[nodiscard]] EnclosureBox MakePointBox(const Vector3& point) noexcept;

/// Box that contains exactly one exactly known float64 point.
[[nodiscard]] EnclosureBox MakePointBox(const math::Vector3d& point) noexcept;

/// Smallest box that contains both boxes.
[[nodiscard]] EnclosureBox MakeBoxHull(const EnclosureBox& left, const EnclosureBox& right) noexcept;

/// Enclose (1 - t) * start + t * end for an exact parameter t.
[[nodiscard]] math::MathStatus
TryInterpolateBox(const EnclosureBox& start, const EnclosureBox& end, float64 t, EnclosureBox& outBox) noexcept;

/// Enclose the blossom f(first, second, third) of a cubic whose exact controls lie in control.
[[nodiscard]] math::MathStatus TryEvaluateBlossom(const CubicControlBoxes& control,
                                                  float64 first,
                                                  float64 second,
                                                  float64 third,
                                                  EnclosureBox& outPoint) noexcept;

/// Enclose the controls of the cubic restricted to [a, b], with 0 <= a <= b <= 1 exact.
[[nodiscard]] math::MathStatus TryEncloseSubcurveControls(const CubicControlBoxes& control,
                                                          float64 a,
                                                          float64 b,
                                                          CubicControlBoxes& outControls) noexcept;

/// Enclose left - right componentwise for every pair of points inside the boxes.
[[nodiscard]] math::MathStatus
TrySubtractBoxes(const EnclosureBox& left, const EnclosureBox& right, EnclosureBox& outBox) noexcept;

/// Enclose left + right componentwise for every pair of points inside the boxes.
[[nodiscard]] math::MathStatus
TryAddBoxes(const EnclosureBox& left, const EnclosureBox& right, EnclosureBox& outBox) noexcept;

/// Enclose factor * point componentwise.
[[nodiscard]] math::MathStatus TryScaleBox(const EnclosureBox& box, Enclosure factor, EnclosureBox& outBox) noexcept;

/// Enclose the Euclidean length of every vector inside the box.
[[nodiscard]] math::MathStatus TryEncloseBoxLength(const EnclosureBox& box, Enclosure& outLength) noexcept;

/// Enclose the distance between every pair of points inside the boxes: [smallest, largest].
[[nodiscard]] math::MathStatus
TryEncloseDistance(const EnclosureBox& start, const EnclosureBox& end, Enclosure& outDistance) noexcept;
} // namespace ludus::foundation::geometry::internal
