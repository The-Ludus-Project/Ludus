// Float64 span evaluation and Certified span boxes (see internal/span_math.hpp).

#include "internal/span_math.hpp"

#include "internal/arc_length.hpp"

#include <ludus/foundation/containers/static_array.hpp>
#include <ludus/foundation/math/enclosure.hpp>
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

[[nodiscard]] Vector3d Interpolate(const Vector3d& start, const Vector3d& end, float64 t) noexcept
{
    // (1 - t) * start + t * end returns each endpoint exactly at t = 0 and t = 1.
    const float64 oneMinusT = 1.0 - t;
    return Vector3d{oneMinusT * start.X + t * end.X, oneMinusT * start.Y + t * end.Y, oneMinusT * start.Z + t * end.Z};
}

// factor * (end - start): the enclosed difference scaled by the enclosed factor.
[[nodiscard]] MathStatus
TryScaleDifference(const EnclosureBox& start, const EnclosureBox& end, Enclosure factor, EnclosureBox& outBox) noexcept
{
    EnclosureBox difference;
    MathStatus status = TrySubtractBoxes(end, start, difference);
    if (status == MathStatus::Success)
    {
        status = TryScaleBox(difference, factor, outBox);
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

GeometryStatus TryEncloseSubspanHull(const PreparedSpan& span, float64 a, float64 b, EnclosureBox& outBox) noexcept
{
    core::StaticArray<EnclosureBox, SPAN_CONTROL_COUNT> controls;
    const GeometryStatus status = TryEncloseSubspanControls(span, a, b, controls);
    if (status != GeometryStatus::Success)
    {
        return status;
    }
    EnclosureBox box = controls[0];
    for (usize i = 1; i < SPAN_CONTROL_COUNT; ++i)
    {
        box = MakeBoxHull(box, controls[i]);
    }
    outBox = box;
    return GeometryStatus::Success;
}

GeometryStatus TryEnclosePoint(const PreparedSpan& span, float64 u, EnclosureBox& outBox) noexcept
{
    core::StaticArray<EnclosureBox, SPAN_CONTROL_COUNT> controls;
    const GeometryStatus status = TryEncloseSubspanControls(span, u, u, controls);
    if (status == GeometryStatus::Success)
    {
        // Every control of the degenerate subspan [u, u] is the blossom f(u, u, u), the point itself.
        outBox = controls[0];
    }
    return status;
}

GeometryStatus TryEncloseHodograph(const PreparedSpan& span, float64 a, float64 b, EnclosureBox& outBox) noexcept
{
    core::StaticArray<EnclosureBox, SPAN_CONTROL_COUNT> controls;
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
    outBox = MakeBoxHull(MakeBoxHull(hodograph[0], hodograph[1]), hodograph[2]);
    return GeometryStatus::Success;
}
} // namespace ludus::foundation::geometry::internal
