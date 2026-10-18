// Certified chord/control-polygon length enclosures (see internal/arc_length.hpp).

#include "internal/arc_length.hpp"

namespace ludus::foundation::geometry::internal
{
namespace
{
using math::MakePointEnclosure;
using math::MathStatus;

// Chord lower bound and control-polygon upper bound of one piece.
[[nodiscard]] MathStatus TryEnclosePieceLength(const StaticArray<EnclosureBox, SPAN_CONTROL_COUNT>& control,
                                               Enclosure& outLength) noexcept
{
    Enclosure chord;
    MathStatus status = TryEncloseDistance(control[0], control[SPAN_CONTROL_COUNT - 1], chord);
    Enclosure polygon = MakePointEnclosure(0.0);
    for (usize i = 0; i + 1 < SPAN_CONTROL_COUNT && status == MathStatus::Success; ++i)
    {
        Enclosure edge;
        status = TryEncloseDistance(control[i], control[i + 1], edge);
        if (status == MathStatus::Success)
        {
            status = math::TryAddOutward(polygon, edge, polygon);
        }
    }
    if (status == MathStatus::Success)
    {
        // The exact length lies between the exact chord and the exact polygon length.
        outLength = Enclosure{ .Lower = chord.Lower, .Upper = polygon.Upper };
    }
    return status;
}
} // namespace

GeometryStatus TryEncloseSubspanControls(const PreparedSpan& span,
                                         float64 a,
                                         float64 b,
                                         StaticArray<EnclosureBox, SPAN_CONTROL_COUNT>& outControls) noexcept
{
    StaticArray<EnclosureBox, SPAN_CONTROL_COUNT> exact;
    for (usize i = 0; i < SPAN_CONTROL_COUNT; ++i)
    {
        exact[i] = MakePointBox(span.Control[i]);
    }
    if (a == 0.0 && b == 1.0)
    {
        outControls = exact;
        return GeometryStatus::Success;
    }
    StaticArray<EnclosureBox, SPAN_CONTROL_COUNT> result;
    MathStatus status = MathStatus::Success;
    if (span.Kind == SpanKind::Line)
    {
        // A line's subsegment is its endpoints; the inner controls repeat them, as in PreparedSpan.
        status = TryInterpolateBox(exact[0], exact[SPAN_CONTROL_COUNT - 1], a, result[0]);
        if (status == MathStatus::Success)
        {
            status = TryInterpolateBox(exact[0], exact[SPAN_CONTROL_COUNT - 1], b, result[SPAN_CONTROL_COUNT - 1]);
        }
        result[1] = result[0];
        result[2] = result[SPAN_CONTROL_COUNT - 1];
    }
    else
    {
        status = TryEncloseSubcurveControls(exact, a, b, result);
    }
    if (status != MathStatus::Success)
    {
        return ToGeometryStatus(status);
    }
    outControls = result;
    return GeometryStatus::Success;
}

GeometryStatus
TryEncloseArcLength(const PreparedSpan& span, float64 a, float64 b, usize pieceCount, Enclosure& outLength) noexcept
{
    LUDUS_ASSERT(pieceCount > 0 && a <= b);
    Enclosure total = MakePointEnclosure(0.0);
    const float64 width = b - a;
    float64 pieceStart = a;
    for (usize piece = 0; piece < pieceCount; ++piece)
    {
        // Consecutive pieces share their exact boundary value, so they partition [a, b].
        const float64 pieceEnd = piece + 1 == pieceCount
                                     ? b
                                     : a + width * (static_cast<float64>(piece + 1) / static_cast<float64>(pieceCount));
        StaticArray<EnclosureBox, SPAN_CONTROL_COUNT> control;
        GeometryStatus status = TryEncloseSubspanControls(span, pieceStart, pieceEnd, control);
        Enclosure length;
        if (status == GeometryStatus::Success)
        {
            status = ToGeometryStatus(TryEnclosePieceLength(control, length));
        }
        if (status == GeometryStatus::Success)
        {
            status = ToGeometryStatus(math::TryAddOutward(total, length, total));
        }
        if (status != GeometryStatus::Success)
        {
            return status;
        }
        pieceStart = pieceEnd;
    }
    outLength = total;
    return GeometryStatus::Success;
}
} // namespace ludus::foundation::geometry::internal
