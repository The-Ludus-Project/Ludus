#pragma once

// Private float64 span evaluation, vector helpers and Certified span boxes shared
// by transported frames, projection and sweeps (phase G3).
//
// Float64 evaluation is a Rounded reference for preparation and transport; it
// keeps sub-step directions accurate where float32 positions would quantize.
// The span boxes are built from the outward-rounded box arithmetic of
// internal/control_box.hpp, so their bounds are Certified for the stored
// float32 controls.

#include <ludus/foundation/base/core.h>

#include "internal/control_box.hpp"

#include <ludus/foundation/geometry/path.hpp>
#include <ludus/foundation/geometry/status.hpp>
#include <ludus/foundation/math/vector.hpp>

namespace ludus::foundation::geometry::internal
{
/// Float64 position and derivatives per normalized u of one prepared span.
struct SpanSample64 final
{
    math::Vector3d Position;
    math::Vector3d FirstDerivative;
    math::Vector3d SecondDerivative;
};

/// Widen float32 components exactly.
[[nodiscard]] math::Vector3d ToVector3d(const math::Vector3& value) noexcept;

/// Evaluate a prepared span in float64 by de Casteljau; u in [0,1] is not checked.
///
/// Stored endpoints are returned exactly at u = 0 and u = 1. A line has the
/// constant derivative Control[3] - Control[0] and no second derivative.
[[nodiscard]] SpanSample64 EvaluateSpan64(const PreparedSpan& span, float64 u) noexcept;

/// Write value / |value| when the length is positive and finite.
[[nodiscard]] bool TryNormalize64(const math::Vector3d& value, math::Vector3d& outUnit) noexcept;

/// Rotate value about a unit axis by angle radians, right-handed (Rodrigues' formula).
[[nodiscard]] math::Vector3d
RotateAboutAxis(const math::Vector3d& value, const math::Vector3d& unitAxis, float64 angle) noexcept;

/// Angle in [0, pi] between two nonzero vectors, from atan2(|a x b|, a . b).
[[nodiscard]] float64 ComputeAngle(const math::Vector3d& left, const math::Vector3d& right) noexcept;

/// Signed angle from unit `from` to unit `to` about unit `axis`; both are assumed perpendicular to it.
[[nodiscard]] float64
ComputeSignedAngle(const math::Vector3d& from, const math::Vector3d& to, const math::Vector3d& axis) noexcept;

/// Box containing the exact span restricted to [a, b]: the hull of its enclosed subspan controls.
[[nodiscard]] GeometryStatus
TryEncloseSubspanHull(const PreparedSpan& span, float64 a, float64 b, EnclosureBox& outBox) noexcept;

/// Box containing the exact span point at u.
[[nodiscard]] GeometryStatus TryEnclosePoint(const PreparedSpan& span, float64 u, EnclosureBox& outBox) noexcept;

/// Box containing dC/du (per the span's normalized u) for every u in [a, b], 0 <= a < b <= 1.
[[nodiscard]] GeometryStatus
TryEncloseHodograph(const PreparedSpan& span, float64 a, float64 b, EnclosureBox& outBox) noexcept;
} // namespace ludus::foundation::geometry::internal
