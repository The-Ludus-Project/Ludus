// Prepared path validation, narrowing, continuity classification and sampling.

#include <ludus/foundation/geometry/path.hpp>

#include "internal/arc_length.hpp"
#include "internal/prepare_support.hpp"

#include <ludus/foundation/math/scalar.hpp>

namespace ludus::foundation::geometry
{
using math::Aabb3;
using math::CubicSample3;
using math::Vector3;
using math::Vector3d;

namespace
{
using internal::ComputeRelativeDifference;
using internal::Fail;
using internal::GetMaxAbsComponent;
using internal::ToDouble;
using internal::TryNarrowVector;
using internal::WriteDiagnostic;

[[nodiscard]] bool IsFiniteVector(const Vector3d& value) noexcept
{
    return math::IsFinite(value.X) == true && math::IsFinite(value.Y) == true && math::IsFinite(value.Z) == true;
}

[[nodiscard]] Vector3d Subtract(const Vector3d& left, const Vector3d& right) noexcept
{
    return Vector3d{left.X - right.X, left.Y - right.Y, left.Z - right.Z};
}

[[nodiscard]] Vector3d Scale(const Vector3d& value, float64 factor) noexcept
{
    return Vector3d{value.X * factor, value.Y * factor, value.Z * factor};
}

// Derivatives of a span at one end, per normalized u.
struct EndDerivatives final
{
    Vector3d First;
    Vector3d Second;
};

[[nodiscard]] EndDerivatives ComputeEndDerivatives(const PreparedSpan& span, bool isStart) noexcept
{
    const Vector3d p0 = ToDouble(span.Control[0]);
    const Vector3d p1 = ToDouble(span.Control[1]);
    const Vector3d p2 = ToDouble(span.Control[2]);
    const Vector3d p3 = ToDouble(span.Control[3]);
    if (span.Kind == SpanKind::Line)
    {
        return EndDerivatives{ .First = Subtract(p3, p0), .Second = Vector3d{} };
    }
    if (isStart == true)
    {
        return EndDerivatives
        {
            .First = Scale(Subtract(p1, p0), 3.0),
            .Second = Scale(Subtract(Subtract(p2, p1), Subtract(p1, p0)), 6.0),
        };
    }
    return EndDerivatives
    {
        .First = Scale(Subtract(p3, p2), 3.0),
        .Second = Scale(Subtract(Subtract(p3, p2), Subtract(p2, p1)), 6.0),
    };
}

[[nodiscard]] JointContinuity
ClassifyJoint(const PreparedSpan& incoming, const PreparedSpan& outgoing, const ToleranceProfile& tolerance) noexcept
{
    const EndDerivatives end = ComputeEndDerivatives(incoming, false);
    const EndDerivatives start = ComputeEndDerivatives(outgoing, true);
    const Vector3d endFirst = Scale(end.First, 1.0 / incoming.DomainWidth);
    const Vector3d startFirst = Scale(start.First, 1.0 / outgoing.DomainWidth);
    const Vector3d endSecond = Scale(end.Second, 1.0 / (incoming.DomainWidth * incoming.DomainWidth));
    const Vector3d startSecond = Scale(start.Second, 1.0 / (outgoing.DomainWidth * outgoing.DomainWidth));

    JointContinuity joint;
    joint.PositionGap =
        math::Length(Subtract(ToDouble(outgoing.Control[0]), ToDouble(incoming.Control[SPAN_CONTROL_COUNT - 1])));
    joint.IsC0 = joint.PositionGap <= tolerance.ContinuityPosition;
    const bool hasDirections = math::Length(endFirst) > 0.0 && math::Length(startFirst) > 0.0;
    if (hasDirections == true)
    {
        joint.DirectionAngle =
            math::Atan2(math::Length(math::Cross(endFirst, startFirst)), math::Dot(endFirst, startFirst));
    }
    joint.IsG1 = hasDirections == true && joint.DirectionAngle <= tolerance.ContinuityAngle;
    joint.DerivativeDifference = ComputeRelativeDifference(endFirst, startFirst);
    joint.SecondDerivativeDifference = ComputeRelativeDifference(endSecond, startSecond);
    joint.IsC1 = joint.IsC0 == true && joint.DerivativeDifference <= tolerance.ContinuityDerivative;
    joint.IsC2 = joint.IsC1 == true && joint.SecondDerivativeDifference <= tolerance.ContinuityDerivative;
    return joint;
}

[[nodiscard]] GeometryStatus ValidateParameter(const PathView& path, SegmentParameter parameter) noexcept
{
    if (math::IsFinite(parameter.U) == false)
    {
        return GeometryStatus::NonFiniteInput;
    }
    if (parameter.SpanIndex >= path.Spans.GetSize() || parameter.U < 0.0 || parameter.U > 1.0)
    {
        return GeometryStatus::InvalidDomain;
    }
    return GeometryStatus::Success;
}

[[nodiscard]] math::CubicBezier3 ToCubic(const PreparedSpan& span) noexcept
{
    math::CubicBezier3 curve;
    for (usize i = 0; i < SPAN_CONTROL_COUNT; ++i)
    {
        curve.Control[i] = span.Control[i];
    }
    return curve;
}

[[nodiscard]] Vector3 InterpolateLine(const PreparedSpan& span, float64 u) noexcept
{
    const Vector3& start = span.Control[0];
    const Vector3& end = span.Control[SPAN_CONTROL_COUNT - 1];
    // Lerp preserves both endpoints exactly; values between float32 endpoints narrow without overflow.
    return Vector3{
        static_cast<float32>(math::Lerp(static_cast<float64>(start.X), static_cast<float64>(end.X), u)),
        static_cast<float32>(math::Lerp(static_cast<float64>(start.Y), static_cast<float64>(end.Y), u)),
        static_cast<float32>(math::Lerp(static_cast<float64>(start.Z), static_cast<float64>(end.Z), u)),
    };
}

} // namespace

PathView PreparedPath::GetView() const noexcept
{
    return PathView
    {
        .Spans = BorrowedView<const PreparedSpan>(mSpans.GetData(), mSpans.GetSize()),
        .Joints = BorrowedView<const JointContinuity>(mJoints.GetData(), mJoints.GetSize()),
        .Origin = mOrigin,
        .IsClosed = mIsClosed,
        .Revision = mRevision,
        .PositionFloor = mPositionFloor,
        .EffectivePositionError = mEffectivePositionError,
        .Tolerance = mTolerance,
    };
}

bool PreparedPath::IsEmpty() const noexcept
{
    return mSpans.IsEmpty();
}

GeometryStatus TryPreparePath(const PathSource& source,
                              const PathPreparation& preparation,
                              PreparedPath& outPath,
                              GeometryDiagnostic* outDiagnosticOrNull) noexcept
{
    const ToleranceProfile& tolerance = preparation.Tolerance;
    const GeometryStatus toleranceStatus = TryValidateToleranceProfile(tolerance);
    if (toleranceStatus != GeometryStatus::Success)
    {
        return Fail(outDiagnosticOrNull, toleranceStatus, 0, "Tolerance");
    }
    const usize spanCount = source.Spans.GetSize();
    if (source.Spans.IsValid() == false || spanCount == 0)
    {
        return Fail(outDiagnosticOrNull, GeometryStatus::InvalidArgument, 0, "Spans");
    }
    if (spanCount > preparation.Limits.MaxSpans)
    {
        return Fail(outDiagnosticOrNull, GeometryStatus::BudgetExceeded, 0, "Spans");
    }
    if (IsFiniteVector(source.Origin) == false || math::IsFinite(source.AssetScale) == false)
    {
        return Fail(outDiagnosticOrNull, GeometryStatus::NonFiniteInput, 0, "Origin");
    }
    if (source.AssetScale < 0.0)
    {
        return Fail(outDiagnosticOrNull, GeometryStatus::InvalidArgument, 0, "AssetScale");
    }
    const usize jointCount = source.IsClosed == true ? spanCount : spanCount - 1;
    const usize preparedBytes = spanCount * sizeof(PreparedSpan) + jointCount * sizeof(JointContinuity);
    if (preparedBytes > preparation.Limits.MaxPreparedBytes)
    {
        return Fail(outDiagnosticOrNull, GeometryStatus::BudgetExceeded, 0, "Limits.MaxPreparedBytes");
    }

    PreparedPath candidate;
    if (candidate.mSpans.TryResize(spanCount) == false || candidate.mJoints.TryResize(jointCount) == false)
    {
        return Fail(outDiagnosticOrNull, GeometryStatus::OutOfMemory, 0, "Spans");
    }

    float64 maxAbsComponent = 0.0;
    float64 maxNarrowingError = 0.0;
    for (usize spanIndex = 0; spanIndex < spanCount; ++spanIndex)
    {
        const PathSpanSource& span = source.Spans[spanIndex];
        if (span.Kind != SpanKind::Line && span.Kind != SpanKind::Cubic)
        {
            return Fail(outDiagnosticOrNull, GeometryStatus::InvalidArgument, span.SourceElement, "Spans.Kind");
        }
        if (math::IsFinite(span.DomainWidth) == false)
        {
            return Fail(outDiagnosticOrNull, GeometryStatus::NonFiniteInput, span.SourceElement, "Spans.DomainWidth");
        }
        if (span.DomainWidth <= 0.0)
        {
            return Fail(outDiagnosticOrNull, GeometryStatus::InvalidArgument, span.SourceElement, "Spans.DomainWidth");
        }
        PreparedSpan& prepared = candidate.mSpans[spanIndex];
        prepared.Kind = span.Kind;
        prepared.DomainWidth = span.DomainWidth;
        prepared.SourceElement = span.SourceElement;
        for (usize controlIndex = 0; controlIndex < SPAN_CONTROL_COUNT; ++controlIndex)
        {
            const bool isLineInterior =
                span.Kind == SpanKind::Line && controlIndex != 0 && controlIndex != SPAN_CONTROL_COUNT - 1;
            if (isLineInterior == true)
            {
                continue;
            }
            const Vector3d& control = span.Control[controlIndex];
            if (IsFiniteVector(control) == false)
            {
                return Fail(outDiagnosticOrNull, GeometryStatus::NonFiniteInput, span.SourceElement, "Spans.Control");
            }
            // Subtract the origin in float64 before narrowing.
            const Vector3d relative = Subtract(control, source.Origin);
            if (IsFiniteVector(relative) == false || TryNarrowVector(relative, prepared.Control[controlIndex]) == false)
            {
                return Fail(outDiagnosticOrNull, GeometryStatus::OutOfRange, span.SourceElement, "Spans.Control");
            }
            maxAbsComponent = math::Max(maxAbsComponent, GetMaxAbsComponent(prepared.Control[controlIndex]));
            maxNarrowingError = math::Max(maxNarrowingError,
                                          math::Length(Subtract(relative, ToDouble(prepared.Control[controlIndex]))));
        }
        if (span.Kind == SpanKind::Line)
        {
            prepared.Control[1] = prepared.Control[0];
            prepared.Control[2] = prepared.Control[SPAN_CONTROL_COUNT - 1];
        }
    }

    const float64 positionFloor = ComputePositionFloor(maxAbsComponent);
    const float64 effectiveError =
        math::Max(tolerance.PositionError, tolerance.RelativePositionError * source.AssetScale);
    if (effectiveError < positionFloor || maxNarrowingError > effectiveError)
    {
        WriteDiagnostic(outDiagnosticOrNull,
                        GeometryDiagnostic
                        {
                            .Code = GeometryStatus::UnsupportedPrecision,
                            .Property = "Tolerance.PositionError",
                            .RequestedTolerance = effectiveError,
                            .AchievedBound = math::Max(positionFloor, maxNarrowingError),
                            .AchievedGuarantee = GuaranteeClass::Rounded,
                        });
        return GeometryStatus::UnsupportedPrecision;
    }

    for (usize jointIndex = 0; jointIndex < jointCount; ++jointIndex)
    {
        const PreparedSpan& incoming = candidate.mSpans[jointIndex];
        const PreparedSpan& outgoing = candidate.mSpans[(jointIndex + 1) % spanCount];
        const JointContinuity joint = ClassifyJoint(incoming, outgoing, tolerance);
        if (joint.IsC0 == false)
        {
            WriteDiagnostic(outDiagnosticOrNull,
                            GeometryDiagnostic
                            {
                                .Code = GeometryStatus::InvalidTopology,
                                .SourceElement = outgoing.SourceElement,
                                .Property = "Spans.Control",
                                .RequestedTolerance = tolerance.ContinuityPosition,
                                .AchievedBound = joint.PositionGap,
                                .AchievedGuarantee = GuaranteeClass::Rounded,
                            });
            return GeometryStatus::InvalidTopology;
        }
        candidate.mJoints[jointIndex] = joint;
    }

    candidate.mOrigin = source.Origin;
    candidate.mIsClosed = source.IsClosed;
    candidate.mRevision = source.Revision;
    candidate.mPositionFloor = positionFloor;
    candidate.mEffectivePositionError = effectiveError;
    candidate.mTolerance = tolerance;
    outPath = Move(candidate);
    return GeometryStatus::Success;
}

GeometryStatus TryEvaluatePath(const PathView& path, SegmentParameter parameter, Vector3& outPosition) noexcept
{
    const GeometryStatus status = ValidateParameter(path, parameter);
    if (status != GeometryStatus::Success)
    {
        return status;
    }
    const PreparedSpan& span = path.Spans[parameter.SpanIndex];
    if (span.Kind == SpanKind::Line)
    {
        outPosition = InterpolateLine(span, parameter.U);
        return GeometryStatus::Success;
    }
    return internal::ToGeometryStatus(math::TryEvaluateCubic(ToCubic(span), parameter.U, outPosition));
}

GeometryStatus TryEvaluatePath(const PathView& path, SegmentParameter parameter, CubicSample3& outSample) noexcept
{
    const GeometryStatus status = ValidateParameter(path, parameter);
    if (status != GeometryStatus::Success)
    {
        return status;
    }
    const PreparedSpan& span = path.Spans[parameter.SpanIndex];
    if (span.Kind == SpanKind::Line)
    {
        CubicSample3 sample;
        const Vector3d derivative = Subtract(ToDouble(span.Control[SPAN_CONTROL_COUNT - 1]), ToDouble(span.Control[0]));
        if (TryNarrowVector(derivative, sample.FirstDerivative) == false)
        {
            return GeometryStatus::OutOfRange;
        }
        sample.Position = InterpolateLine(span, parameter.U);
        outSample = sample;
        return GeometryStatus::Success;
    }
    return internal::ToGeometryStatus(math::TryEvaluateCubic(ToCubic(span), parameter.U, outSample));
}

GeometryStatus TryEvaluatePathTangent(const PathView& path,
                                      SegmentParameter parameter,
                                      float64 minimumSpeed,
                                      Vector3& outTangent) noexcept
{
    if (math::IsFinite(minimumSpeed) == false)
    {
        return GeometryStatus::NonFiniteInput;
    }
    if (minimumSpeed < 0.0)
    {
        return GeometryStatus::InvalidArgument;
    }
    CubicSample3 sample;
    const GeometryStatus status = TryEvaluatePath(path, parameter, sample);
    if (status != GeometryStatus::Success)
    {
        return status;
    }
    const Vector3d derivative = ToDouble(sample.FirstDerivative);
    const float64 speed = math::Length(derivative);
    if (speed <= minimumSpeed)
    {
        return GeometryStatus::Degenerate;
    }
    const Vector3d direction = Scale(derivative, 1.0 / speed);
    outTangent = Vector3{
        static_cast<float32>(direction.X),
        static_cast<float32>(direction.Y),
        static_cast<float32>(direction.Z),
    };
    return GeometryStatus::Success;
}

GeometryStatus TryGetSpanControlBounds(const PathView& path, usize spanIndex, Aabb3& outBounds) noexcept
{
    if (spanIndex >= path.Spans.GetSize())
    {
        return GeometryStatus::InvalidDomain;
    }
    const PreparedSpan& span = path.Spans[spanIndex];
    Aabb3 bounds{ .Min = span.Control[0], .Max = span.Control[0] };
    for (usize i = 1; i < SPAN_CONTROL_COUNT; ++i)
    {
        const Vector3& control = span.Control[i];
        bounds.Min = Vector3{math::Min(bounds.Min.X, control.X),
                             math::Min(bounds.Min.Y, control.Y),
                             math::Min(bounds.Min.Z, control.Z)};
        bounds.Max = Vector3{math::Max(bounds.Max.X, control.X),
                             math::Max(bounds.Max.Y, control.Y),
                             math::Max(bounds.Max.Z, control.Z)};
    }
    outBounds = bounds;
    return GeometryStatus::Success;
}
} // namespace ludus::foundation::geometry
