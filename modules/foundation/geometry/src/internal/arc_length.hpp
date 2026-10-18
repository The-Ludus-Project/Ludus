#pragma once

// Private Certified arc-length enclosures for prepared spans.
//
// Thanks to Jens Gravesen, "The Length of Bezier Curves", Graphics Gems V,
// chapter IV.7, printed pages 199-205 (reviewed in
// docs/architecture/curves-surfaces-gems-review.md): a Bezier span's arc length
// lies between its chord and its control-polygon length, and subdivision
// shrinks the gap. Departure: the chapter's weighted estimate (the midpoint for
// cubics) is only an Estimated value; Ludus bounds use the chord/polygon
// interval with outward rounding and a whole-path gap budget.
//
// Subspan controls are enclosed with the blossoms in internal/control_box.hpp,
// so the boxes contain the exact subspan controls for exact parameters.

#include <ludus/foundation/base/core.h>

#include "internal/control_box.hpp"

#include <ludus/foundation/containers/static_array.hpp>
#include <ludus/foundation/geometry/path.hpp>
#include <ludus/foundation/geometry/status.hpp>
#include <ludus/foundation/math/enclosure.hpp>

namespace ludus::foundation::geometry::internal
{
using core::StaticArray;
using math::Enclosure;

/// Boxes that contain the exact controls of span restricted to [a, b], 0 <= a <= b <= 1.
[[nodiscard]] GeometryStatus
TryEncloseSubspanControls(const PreparedSpan& span,
                          float64 a,
                          float64 b,
                          StaticArray<ControlBox, SPAN_CONTROL_COUNT>& outControls) noexcept;

/// Enclosure of the exact arc length of span over [a, b], from pieceCount equal chord/polygon pieces.
///
/// Lower sums the pieces' chord lower bounds and Upper their control-polygon
/// upper bounds, both with outward accumulation. pieceCount must be at least one.
[[nodiscard]] GeometryStatus
TryEncloseArcLength(const PreparedSpan& span, float64 a, float64 b, usize pieceCount, Enclosure& outLength) noexcept;
} // namespace ludus::foundation::geometry::internal
