// Outward-rounded control-point boxes (see internal/control_box.hpp).

#include "internal/control_box.hpp"

namespace ludus::foundation::geometry::internal
{
namespace
{
using math::MakePointEnclosure;
using math::MathStatus;

// A cubic blossom takes one parameter per degree.
constexpr usize BLOSSOM_ARGUMENT_COUNT = 3;
// Spatial axes of a point.
constexpr usize AXIS_COUNT = 3;

// (1 - t) * start + t * end for one axis, enclosing the exact value for exact t.
// NOLINTBEGIN(bugprone-easily-swappable-parameters): interpolation endpoints and weights share one type.
[[nodiscard]] MathStatus
TryInterpolateAxis(Enclosure start, Enclosure end, Enclosure oneMinusT, Enclosure t, Enclosure& outValue) noexcept
// NOLINTEND(bugprone-easily-swappable-parameters)
{
    Enclosure startPart;
    MathStatus status = math::TryMultiplyOutward(oneMinusT, start, startPart);
    Enclosure endPart;
    if (status == MathStatus::Success)
    {
        status = math::TryMultiplyOutward(t, end, endPart);
    }
    if (status == MathStatus::Success)
    {
        status = math::TryAddOutward(startPart, endPart, outValue);
    }
    return status;
}
} // namespace

GeometryStatus ToGeometryStatus(math::MathStatus status) noexcept
{
    switch (status)
    {
        case MathStatus::Success:
            return GeometryStatus::Success;
        case MathStatus::NonFiniteInput:
            return GeometryStatus::NonFiniteInput;
        case MathStatus::InvalidArgument:
            // A kernel parameter outside its domain (ADR 0024 boundary mapping).
            return GeometryStatus::InvalidDomain;
        case MathStatus::Degenerate:
            return GeometryStatus::Degenerate;
        case MathStatus::IllConditioned:
            return GeometryStatus::IllConditioned;
        case MathStatus::OutOfRange:
            return GeometryStatus::OutOfRange;
        case MathStatus::SizeMismatch:
            return GeometryStatus::InvalidArgument;
    }
    return GeometryStatus::InvalidArgument;
}

ControlBox MakePointBox(const Vector3& point) noexcept
{
    return ControlBox
    {
        .X = MakePointEnclosure(static_cast<float64>(point.X)),
        .Y = MakePointEnclosure(static_cast<float64>(point.Y)),
        .Z = MakePointEnclosure(static_cast<float64>(point.Z)),
    };
}

MathStatus TryInterpolateBox(const ControlBox& start, const ControlBox& end, float64 t, ControlBox& outBox) noexcept
{
    Enclosure oneMinusT;
    MathStatus status = math::TrySubtractOutward(MakePointEnclosure(1.0), MakePointEnclosure(t), oneMinusT);
    const Enclosure parameter = MakePointEnclosure(t);
    ControlBox result;
    if (status == MathStatus::Success)
    {
        status = TryInterpolateAxis(start.X, end.X, oneMinusT, parameter, result.X);
    }
    if (status == MathStatus::Success)
    {
        status = TryInterpolateAxis(start.Y, end.Y, oneMinusT, parameter, result.Y);
    }
    if (status == MathStatus::Success)
    {
        status = TryInterpolateAxis(start.Z, end.Z, oneMinusT, parameter, result.Z);
    }
    if (status == MathStatus::Success)
    {
        outBox = result;
    }
    return status;
}

MathStatus TryEvaluateBlossom(const CubicControlBoxes& control,
                              float64 first,
                              float64 second,
                              float64 third,
                              ControlBox& outPoint) noexcept
{
    CubicControlBoxes level = control;
    const StaticArray<float64, BLOSSOM_ARGUMENT_COUNT> parameters{{first, second, third}};
    MathStatus status = MathStatus::Success;
    for (usize round = 0; round < parameters.GetSize() && status == MathStatus::Success; ++round)
    {
        const usize count = CUBIC_CONTROL_COUNT - 1 - round;
        for (usize i = 0; i < count && status == MathStatus::Success; ++i)
        {
            status = TryInterpolateBox(level[i], level[i + 1], parameters[round], level[i]);
        }
    }
    if (status == MathStatus::Success)
    {
        outPoint = level[0];
    }
    return status;
}

MathStatus TryEncloseSubcurveControls(const CubicControlBoxes& control,
                                      float64 a,
                                      float64 b,
                                      CubicControlBoxes& outControls) noexcept
{
    if (a == 0.0 && b == 1.0)
    {
        // The whole curve keeps its stored controls exactly.
        outControls = control;
        return MathStatus::Success;
    }
    CubicControlBoxes result;
    MathStatus status = TryEvaluateBlossom(control, a, a, a, result[0]);
    if (status == MathStatus::Success)
    {
        status = TryEvaluateBlossom(control, a, a, b, result[1]);
    }
    if (status == MathStatus::Success)
    {
        status = TryEvaluateBlossom(control, a, b, b, result[2]);
    }
    if (status == MathStatus::Success)
    {
        status = TryEvaluateBlossom(control, b, b, b, result[3]);
    }
    if (status == MathStatus::Success)
    {
        outControls = result;
    }
    return status;
}

MathStatus TrySubtractBoxes(const ControlBox& left, const ControlBox& right, ControlBox& outBox) noexcept
{
    ControlBox result;
    MathStatus status = math::TrySubtractOutward(left.X, right.X, result.X);
    if (status == MathStatus::Success)
    {
        status = math::TrySubtractOutward(left.Y, right.Y, result.Y);
    }
    if (status == MathStatus::Success)
    {
        status = math::TrySubtractOutward(left.Z, right.Z, result.Z);
    }
    if (status == MathStatus::Success)
    {
        outBox = result;
    }
    return status;
}

MathStatus TryAddBoxes(const ControlBox& left, const ControlBox& right, ControlBox& outBox) noexcept
{
    ControlBox result;
    MathStatus status = math::TryAddOutward(left.X, right.X, result.X);
    if (status == MathStatus::Success)
    {
        status = math::TryAddOutward(left.Y, right.Y, result.Y);
    }
    if (status == MathStatus::Success)
    {
        status = math::TryAddOutward(left.Z, right.Z, result.Z);
    }
    if (status == MathStatus::Success)
    {
        outBox = result;
    }
    return status;
}

MathStatus TryScaleBox(const ControlBox& box, Enclosure factor, ControlBox& outBox) noexcept
{
    ControlBox result;
    MathStatus status = math::TryMultiplyOutward(factor, box.X, result.X);
    if (status == MathStatus::Success)
    {
        status = math::TryMultiplyOutward(factor, box.Y, result.Y);
    }
    if (status == MathStatus::Success)
    {
        status = math::TryMultiplyOutward(factor, box.Z, result.Z);
    }
    if (status == MathStatus::Success)
    {
        outBox = result;
    }
    return status;
}

MathStatus TryEncloseBoxLength(const ControlBox& box, Enclosure& outLength) noexcept
{
    const StaticArray<Enclosure, AXIS_COUNT> axes{{box.X, box.Y, box.Z}};
    Enclosure sum = MakePointEnclosure(0.0);
    MathStatus status = MathStatus::Success;
    for (usize axis = 0; axis < axes.GetSize() && status == MathStatus::Success; ++axis)
    {
        Enclosure square;
        status = math::TrySquareOutward(axes[axis], square);
        if (status == MathStatus::Success)
        {
            status = math::TryAddOutward(sum, square, sum);
        }
    }
    if (status == MathStatus::Success)
    {
        status = math::TrySquareRootOutward(sum, outLength);
    }
    return status;
}

MathStatus TryEncloseDistance(const ControlBox& start, const ControlBox& end, Enclosure& outDistance) noexcept
{
    ControlBox difference;
    MathStatus status = TrySubtractBoxes(end, start, difference);
    if (status == MathStatus::Success)
    {
        status = TryEncloseBoxLength(difference, outDistance);
    }
    return status;
}
} // namespace ludus::foundation::geometry::internal
