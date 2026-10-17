// Float64 span evaluation and Certified box bounds (see internal/span_math.hpp).

#include "internal/span_math.hpp"

#include "internal/arc_length.hpp"

#include <ludus/foundation/containers/static_array.hpp>
#include <ludus/foundation/math/scalar.hpp>

namespace ludus::foundation::geometry::internal
{
using math::Enclosure;
using math::MakePointEnclosure;
using math::MathStatus;
using math::Vector3d;

namespace
{
// Controls of the quadratic derivative (hodograph) of a cubic.
constexpr usize HODOGRAPH_CONTROL_COUNT = SPAN_CONTROL_COUNT - 1;
// Degree of a cubic span, the factor in its hodograph controls.
constexpr float64 CUBIC_DEGREE = 3.0;
// Spatial axes of a point.
constexpr usize AXIS_COUNT = 3;

[[nodiscard]] Vector3d Interpolate(const Vector3d& start, const Vector3d& end, float64 t) noexcept
{
    // (1 - t) * start + t * end returns each endpoint exactly at t = 0 and t = 1.
    const float64 oneMinusT = 1.0 - t;
    return Vector3d{oneMinusT * start.X + t * end.X, oneMinusT * start.Y + t * end.Y, oneMinusT * start.Z + t * end.Z};
}

[[nodiscard]] Enclosure Hull(Enclosure left, Enclosure right) noexcept
{
    return Enclosure{ .Lower = math::Min(left.Lower, right.Lower), .Upper = math::Max(left.Upper, right.Upper) };
}

[[nodiscard]] EnclosureBox Hull(const EnclosureBox& left, const EnclosureBox& right) noexcept
{
    return EnclosureBox{ .X = Hull(left.X, right.X), .Y = Hull(left.Y, right.Y), .Z = Hull(left.Z, right.Z) };
}

[[nodiscard]] EnclosureBox ToBox(const ControlBox& control) noexcept
{
    return EnclosureBox{ .X = control.X, .Y = control.Y, .Z = control.Z };
}

// factor * (end - start) for one axis, enclosing the exact value.
[[nodiscard]] MathStatus
TryScaleDifference(Enclosure start, Enclosure end, Enclosure factor, Enclosure& outValue) noexcept
{
    Enclosure difference;
    MathStatus status = math::TrySubtractOutward(end, start, difference);
    if (status == MathStatus::Success)
    {
        status = math::TryMultiplyOutward(difference, factor, outValue);
    }
    return status;
}

[[nodiscard]] MathStatus
TryScaleDifference(const ControlBox& start, const ControlBox& end, Enclosure factor, EnclosureBox& outBox) noexcept
{
    EnclosureBox result;
    MathStatus status = TryScaleDifference(start.X, end.X, factor, result.X);
    if (status == MathStatus::Success)
    {
        status = TryScaleDifference(start.Y, end.Y, factor, result.Y);
    }
    if (status == MathStatus::Success)
    {
        status = TryScaleDifference(start.Z, end.Z, factor, result.Z);
    }
    if (status == MathStatus::Success)
    {
        outBox = result;
    }
    return status;
}

// Smallest and largest |x - y| over x in left and y in right, for one axis.
// NOLINTBEGIN(bugprone-easily-swappable-parameters): axis enclosures share one type.
[[nodiscard]] MathStatus
TryBoundAxisDistance(Enclosure left, Enclosure right, float64& outSmallest, float64& outLargest) noexcept
// NOLINTEND(bugprone-easily-swappable-parameters)
{
    Enclosure leftBelow;
    MathStatus status =
        math::TrySubtractOutward(MakePointEnclosure(right.Lower), MakePointEnclosure(left.Upper), leftBelow);
    Enclosure rightBelow;
    if (status == MathStatus::Success)
    {
        status = math::TrySubtractOutward(MakePointEnclosure(left.Lower), MakePointEnclosure(right.Upper), rightBelow);
    }
    Enclosure leftSpread;
    if (status == MathStatus::Success)
    {
        status = math::TrySubtractOutward(MakePointEnclosure(left.Upper), MakePointEnclosure(right.Lower), leftSpread);
    }
    Enclosure rightSpread;
    if (status == MathStatus::Success)
    {
        status = math::TrySubtractOutward(MakePointEnclosure(right.Upper), MakePointEnclosure(left.Lower), rightSpread);
    }
    if (status == MathStatus::Success)
    {
        // The gap is positive only when the intervals are disjoint; the spread is never negative in total.
        outSmallest = math::Max(0.0, math::Max(leftBelow.Lower, rightBelow.Lower));
        outLargest = math::Max(0.0, math::Max(leftSpread.Upper, rightSpread.Upper));
    }
    return status;
}
} // namespace

Vector3d ToVector3d(const math::Vector3& value) noexcept
{
    return Vector3d{static_cast<float64>(value.X), static_cast<float64>(value.Y), static_cast<float64>(value.Z)};
}

SpanSample64 EvaluateSpan64(const PreparedSpan& span, float64 u) noexcept
{
    const Vector3d p0 = ToVector3d(span.Control[0]);
    const Vector3d p1 = ToVector3d(span.Control[1]);
    const Vector3d p2 = ToVector3d(span.Control[2]);
    const Vector3d p3 = ToVector3d(span.Control[SPAN_CONTROL_COUNT - 1]);
    if (span.Kind == SpanKind::Line)
    {
        return SpanSample64
        {
            .Position = Interpolate(p0, p3, u),
            .FirstDerivative = p3 - p0,
            .SecondDerivative = Vector3d{},
        };
    }
    const Vector3d a = Interpolate(p0, p1, u);
    const Vector3d b = Interpolate(p1, p2, u);
    const Vector3d c = Interpolate(p2, p3, u);
    const Vector3d ab = Interpolate(a, b, u);
    const Vector3d bc = Interpolate(b, c, u);
    return SpanSample64
    {
        .Position = Interpolate(ab, bc, u),
        .FirstDerivative = CUBIC_DEGREE * (bc - ab),
        .SecondDerivative = 6.0 * Interpolate((p2 - p1) - (p1 - p0), (p3 - p2) - (p2 - p1), u),
    };
}

bool TryNormalize64(const Vector3d& value, Vector3d& outUnit) noexcept
{
    const float64 length = math::Length(value);
    if (math::IsFinite(length) == false || length <= 0.0)
    {
        return false;
    }
    outUnit = value * (1.0 / length);
    return true;
}

Vector3d RotateAboutAxis(const Vector3d& value, const Vector3d& unitAxis, float64 angle) noexcept
{
    const float64 cosine = math::Cos(angle);
    const float64 sine = math::Sin(angle);
    return value * cosine + math::Cross(unitAxis, value) * sine +
           unitAxis * (math::Dot(unitAxis, value) * (1.0 - cosine));
}

float64 ComputeAngle(const Vector3d& left, const Vector3d& right) noexcept
{
    return math::Atan2(math::Length(math::Cross(left, right)), math::Dot(left, right));
}

float64 ComputeSignedAngle(const Vector3d& from, const Vector3d& to, const Vector3d& axis) noexcept
{
    return math::Atan2(math::Dot(axis, math::Cross(from, to)), math::Dot(from, to));
}

EnclosureBox MakePointBox(const Vector3d& point) noexcept
{
    return EnclosureBox
    {
        .X = MakePointEnclosure(point.X),
        .Y = MakePointEnclosure(point.Y),
        .Z = MakePointEnclosure(point.Z),
    };
}

GeometryStatus TryEncloseSubspanHull(const PreparedSpan& span, float64 a, float64 b, EnclosureBox& outBox) noexcept
{
    core::StaticArray<ControlBox, SPAN_CONTROL_COUNT> controls;
    const GeometryStatus status = TryEncloseSubspanControls(span, a, b, controls);
    if (status != GeometryStatus::Success)
    {
        return status;
    }
    EnclosureBox box = ToBox(controls[0]);
    for (usize i = 1; i < SPAN_CONTROL_COUNT; ++i)
    {
        box = Hull(box, ToBox(controls[i]));
    }
    outBox = box;
    return GeometryStatus::Success;
}

GeometryStatus TryEnclosePoint(const PreparedSpan& span, float64 u, EnclosureBox& outBox) noexcept
{
    core::StaticArray<ControlBox, SPAN_CONTROL_COUNT> controls;
    const GeometryStatus status = TryEncloseSubspanControls(span, u, u, controls);
    if (status == GeometryStatus::Success)
    {
        // Every control of the degenerate subspan [u, u] is the blossom f(u, u, u), the point itself.
        outBox = ToBox(controls[0]);
    }
    return status;
}

GeometryStatus TryEncloseHodograph(const PreparedSpan& span, float64 a, float64 b, EnclosureBox& outBox) noexcept
{
    core::StaticArray<ControlBox, SPAN_CONTROL_COUNT> controls;
    if (span.Kind == SpanKind::Line)
    {
        // A line's derivative is the constant Control[3] - Control[0], whatever the subinterval.
        const GeometryStatus status = TryEncloseSubspanControls(span, 0.0, 1.0, controls);
        if (status != GeometryStatus::Success)
        {
            return status;
        }
        return ToGeometryStatus(
            TryScaleDifference(controls[0], controls[SPAN_CONTROL_COUNT - 1], MakePointEnclosure(1.0), outBox));
    }
    GeometryStatus status = TryEncloseSubspanControls(span, a, b, controls);
    if (status != GeometryStatus::Success)
    {
        return status;
    }
    // The restricted cubic's hodograph per local t is 3 (Q[i+1] - Q[i]); dC/du = (dC/dt) / (b - a).
    Enclosure width;
    status = ToGeometryStatus(math::TrySubtractOutward(MakePointEnclosure(b), MakePointEnclosure(a), width));
    Enclosure factor;
    if (status == GeometryStatus::Success && width.Lower <= 0.0)
    {
        status = GeometryStatus::InvalidDomain;
    }
    if (status == GeometryStatus::Success)
    {
        Enclosure inverse = MakePointEnclosure(1.0 / width.Lower);
        if (width.Lower != width.Upper || width.Lower * inverse.Lower != 1.0)
        {
            // Not an exact power of two: enclose 3 / (b - a) from the width's enclosure.
            inverse = Enclosure
            {
                .Lower = math::NextFloat64Down(1.0 / width.Upper),
                .Upper = math::NextFloat64Up(1.0 / width.Lower),
            };
        }
        status = ToGeometryStatus(math::TryMultiplyOutward(MakePointEnclosure(CUBIC_DEGREE), inverse, factor));
    }
    core::StaticArray<EnclosureBox, HODOGRAPH_CONTROL_COUNT> hodograph;
    for (usize i = 0; i < HODOGRAPH_CONTROL_COUNT && status == GeometryStatus::Success; ++i)
    {
        status = ToGeometryStatus(TryScaleDifference(controls[i], controls[i + 1], factor, hodograph[i]));
    }
    if (status != GeometryStatus::Success)
    {
        return status;
    }
    outBox = Hull(Hull(hodograph[0], hodograph[1]), hodograph[2]);
    return GeometryStatus::Success;
}

GeometryStatus TryBoundBoxDistance(const EnclosureBox& left, const EnclosureBox& right, Enclosure& outDistance) noexcept
{
    const core::StaticArray<Enclosure, AXIS_COUNT> leftAxes{{left.X, left.Y, left.Z}};
    const core::StaticArray<Enclosure, AXIS_COUNT> rightAxes{{right.X, right.Y, right.Z}};
    Enclosure smallestSquare = MakePointEnclosure(0.0);
    Enclosure largestSquare = MakePointEnclosure(0.0);
    MathStatus status = MathStatus::Success;
    for (usize axis = 0; axis < leftAxes.GetSize() && status == MathStatus::Success; ++axis)
    {
        float64 smallest = 0.0;
        float64 largest = 0.0;
        status = TryBoundAxisDistance(leftAxes[axis], rightAxes[axis], smallest, largest);
        Enclosure smallestTerm;
        Enclosure largestTerm;
        if (status == MathStatus::Success)
        {
            status = math::TrySquareOutward(MakePointEnclosure(smallest), smallestTerm);
        }
        if (status == MathStatus::Success)
        {
            status = math::TrySquareOutward(MakePointEnclosure(largest), largestTerm);
        }
        if (status == MathStatus::Success)
        {
            status = math::TryAddOutward(smallestSquare, smallestTerm, smallestSquare);
        }
        if (status == MathStatus::Success)
        {
            status = math::TryAddOutward(largestSquare, largestTerm, largestSquare);
        }
    }
    Enclosure smallestRoot;
    Enclosure largestRoot;
    if (status == MathStatus::Success)
    {
        status = math::TrySquareRootOutward(smallestSquare, smallestRoot);
    }
    if (status == MathStatus::Success)
    {
        status = math::TrySquareRootOutward(largestSquare, largestRoot);
    }
    if (status != MathStatus::Success)
    {
        return ToGeometryStatus(status);
    }
    outDistance = Enclosure{ .Lower = smallestRoot.Lower, .Upper = largestRoot.Upper };
    return GeometryStatus::Success;
}
} // namespace ludus::foundation::geometry::internal
