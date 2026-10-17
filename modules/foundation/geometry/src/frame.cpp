// Rotation-minimizing frame baking and random-access evaluation (see frame.hpp).
//
// Thanks to Wenping Wang, Bert Juttler, Dayue Zheng and Yang Liu, "Computation
// of Rotation Minimizing Frames", ACM Transactions on Graphics 27(1), article 2,
// 2008 (https://hub.hku.hk/handle/10722/152386). TransportNormal implements their
// double-reflection step: reflect through the plane bisecting the two sample
// points, then through the plane that maps the reflected tangent onto the next
// tangent. Departures: when the two points coincide (a joint) the first
// reflection uses the incoming tangent, which makes the pair the minimal
// rotation between the tangents; the paper's uniform sampling is replaced by
// the dyadic one-step versus two-half-step refinement below, whose differences
// form the Estimated angular error. Carl Dougan's parallel transport frame
// (Game Programming Gems 2, section 2.5, printed pages 215-219) motivates
// baking the history once and failing visibly at undefined tangents.

#include <ludus/foundation/geometry/frame.hpp>

#include "internal/span_math.hpp"

#include <ludus/foundation/math/scalar.hpp>

namespace ludus::foundation::geometry
{
using math::Vector3;
using math::Vector3d;

namespace
{
// Dyadic levels below one span whose interval ends stay exact float64 values in [0,1].
constexpr usize MAX_DYADIC_LEVEL = 52;
// Largest finite float32, the narrowing range of stored frame positions.
constexpr float64 FLOAT32_LARGEST = 3.4028234663852886e38;

// Float64 transport state at one parameter.
struct FrameState final
{
    Vector3d Position;
    Vector3d Tangent;
    Vector3d Normal;
};

void WriteDiagnostic(GeometryDiagnostic* outDiagnosticOrNull, const GeometryDiagnostic& diagnostic) noexcept
{
    if (outDiagnosticOrNull != nullptr)
    {
        *outDiagnosticOrNull = diagnostic;
    }
}

[[nodiscard]] GeometryStatus
Fail(GeometryDiagnostic* outDiagnosticOrNull, GeometryStatus status, usize sourceElement, const char* property) noexcept
{
    WriteDiagnostic(outDiagnosticOrNull,
                    GeometryDiagnostic{ .Code = status, .SourceElement = sourceElement, .Property = property });
    return status;
}

[[nodiscard]] bool IsFiniteVector(const Vector3d& value) noexcept
{
    return math::IsFinite(value.X) == true && math::IsFinite(value.Y) == true && math::IsFinite(value.Z) == true;
}

// Reflect value through the plane through the origin with normal axis; squaredLength = |axis|^2 > 0.
[[nodiscard]] Vector3d Reflect(const Vector3d& value, const Vector3d& axis, float64 squaredLength) noexcept
{
    return value - axis * (2.0 * math::Dot(axis, value) / squaredLength);
}

// Remove the tangent component and normalize; keeps the input when it cannot be normalized.
[[nodiscard]] Vector3d Orthonormalize(const Vector3d& normal, const Vector3d& tangent) noexcept
{
    Vector3d result = normal;
    if (internal::TryNormalize64(normal - tangent * math::Dot(normal, tangent), result) == false)
    {
        return normal;
    }
    return result;
}

// Second reflection of the double-reflection step: maps reflectedTangent onto tangent.
// NOLINTBEGIN(bugprone-easily-swappable-parameters): frame vectors share one type.
[[nodiscard]] Vector3d
ReflectOntoTangent(const Vector3d& reflectedNormal, const Vector3d& reflectedTangent, const Vector3d& tangent) noexcept
// NOLINTEND(bugprone-easily-swappable-parameters)
{
    const Vector3d axis = tangent - reflectedTangent;
    const float64 squaredLength = math::Dot(axis, axis);
    const Vector3d normal = squaredLength == 0.0 ? reflectedNormal : Reflect(reflectedNormal, axis, squaredLength);
    return Orthonormalize(normal, tangent);
}

// Double-reflection transport of normal from (position0, tangent0) to (position1, tangent1).
// NOLINTBEGIN(bugprone-easily-swappable-parameters): target position and tangent share one type.
[[nodiscard]] Vector3d
TransportNormal(const FrameState& from, const Vector3d& position, const Vector3d& tangent) noexcept
// NOLINTEND(bugprone-easily-swappable-parameters)
{
    Vector3d chord = position - from.Position;
    float64 squaredLength = math::Dot(chord, chord);
    if (squaredLength == 0.0)
    {
        // Coincident points: the chord's limit direction is the tangent itself.
        chord = from.Tangent;
        squaredLength = math::Dot(chord, chord);
    }
    const Vector3d reflectedNormal = Reflect(from.Normal, chord, squaredLength);
    const Vector3d reflectedTangent = Reflect(from.Tangent, chord, squaredLength);
    return ReflectOntoTangent(reflectedNormal, reflectedTangent, tangent);
}

// Minimal rotation of normal from tangentIn to tangentOut at one point (a joint).
[[nodiscard]] Vector3d
RotateBetweenTangents(const Vector3d& normal, const Vector3d& tangentIn, const Vector3d& tangentOut) noexcept
{
    const float64 squaredLength = math::Dot(tangentIn, tangentIn);
    return ReflectOntoTangent(Reflect(normal, tangentIn, squaredLength),
                              Reflect(tangentIn, tangentIn, squaredLength),
                              tangentOut);
}

// Whether the rotation between two unit tangents is defined: cos(angle / 2) above the threshold.
[[nodiscard]] bool
IsRotationDefined(const Vector3d& tangentIn, const Vector3d& tangentOut, float64 minimumSinAngle) noexcept
{
    return math::Length(tangentIn + tangentOut) * 0.5 > minimumSinAngle;
}

[[nodiscard]] bool TryNarrow(const Vector3d& value, Vector3& outValue) noexcept
{
    if (IsFiniteVector(value) == false || math::Abs(value.X) > FLOAT32_LARGEST ||
        math::Abs(value.Y) > FLOAT32_LARGEST || math::Abs(value.Z) > FLOAT32_LARGEST)
    {
        return false;
    }
    outValue = Vector3{static_cast<float32>(value.X), static_cast<float32>(value.Y), static_cast<float32>(value.Z)};
    return true;
}

// Unit tangent from a float64 sample; false when the derivative vanishes.
[[nodiscard]] bool TryGetTangent(const internal::SpanSample64& sample, Vector3d& outTangent) noexcept
{
    return internal::TryNormalize64(sample.FirstDerivative, outTangent);
}

// Normal from a seed direction projected perpendicular to the tangent, or false when degenerate.
// NOLINTBEGIN(bugprone-easily-swappable-parameters): seed and tangent are both directions.
[[nodiscard]] bool
TryProjectSeed(const Vector3d& seed, const Vector3d& tangent, float64 minimumSinAngle, Vector3d& outNormal) noexcept
// NOLINTEND(bugprone-easily-swappable-parameters)
{
    Vector3d unitSeed;
    if (internal::TryNormalize64(seed, unitSeed) == false)
    {
        return false;
    }
    if (math::Length(math::Cross(unitSeed, tangent)) <= minimumSinAngle)
    {
        return false;
    }
    return internal::TryNormalize64(unitSeed - tangent * math::Dot(unitSeed, tangent), outNormal);
}

[[nodiscard]] bool IsLexicographicallyAfter(const SegmentParameter& left, const SegmentParameter& right) noexcept
{
    return left.SpanIndex != right.SpanIndex ? left.SpanIndex > right.SpanIndex : left.U > right.U;
}

[[nodiscard]] GeometryStatus ValidatePreparation(const PathView& path,
                                                 const FramePreparation& preparation,
                                                 GeometryDiagnostic* outDiagnosticOrNull) noexcept
{
    if (path.Spans.IsValid() == false || path.Spans.IsEmpty() == true)
    {
        return Fail(outDiagnosticOrNull, GeometryStatus::InvalidArgument, 0, "Path.Spans");
    }
    if (preparation.Precision != PrecisionProfile::Strict && preparation.Precision != PrecisionProfile::Visual)
    {
        return Fail(outDiagnosticOrNull, GeometryStatus::InvalidArgument, 0, "Precision");
    }
    if (preparation.Precision == PrecisionProfile::Strict)
    {
        // Frame angular error stays Estimated until derivative enclosures are validated (ADR 0024).
        WriteDiagnostic(outDiagnosticOrNull,
                        GeometryDiagnostic
                        {
                            .Code = GeometryStatus::UnsupportedPrecision,
                            .Property = "Precision",
                            .RequestedTolerance = path.Tolerance.TangentAngleError,
                            .AchievedGuarantee = GuaranteeClass::Estimated,
                        });
        return GeometryStatus::UnsupportedPrecision;
    }
    const bool isFallbackSeeded = preparation.SeedPolicy == FrameSeedPolicy::UseFallbackAxis;
    if (IsFiniteVector(preparation.SeedUp) == false ||
        (isFallbackSeeded == true && IsFiniteVector(preparation.FallbackAxis) == false))
    {
        return Fail(outDiagnosticOrNull, GeometryStatus::NonFiniteInput, 0, "SeedUp");
    }
    if (math::IsFinite(preparation.MinimumSinAngle) == false || math::IsFinite(preparation.MinimumSpeed) == false)
    {
        return Fail(outDiagnosticOrNull, GeometryStatus::NonFiniteInput, 0, "MinimumSpeed");
    }
    const bool hasKnownPolicies =
        (preparation.SeedPolicy == FrameSeedPolicy::RejectDegenerateSeed || isFallbackSeeded == true) &&
        (preparation.SingularityPolicy == FrameSingularityPolicy::Reject ||
         preparation.SingularityPolicy == FrameSingularityPolicy::SplitRuns) &&
        (preparation.CornerPolicy == FrameCornerPolicy::Reject ||
         preparation.CornerPolicy == FrameCornerPolicy::RotateMinimally ||
         preparation.CornerPolicy == FrameCornerPolicy::SplitRuns) &&
        (preparation.ClosedPolicy == ClosedFramePolicy::DistributeTwist ||
         preparation.ClosedPolicy == ClosedFramePolicy::PreserveHolonomy);
    if (hasKnownPolicies == false)
    {
        return Fail(outDiagnosticOrNull, GeometryStatus::InvalidArgument, 0, "Policy");
    }
    const Vector3d zero{};
    if (preparation.SeedUp == zero || (isFallbackSeeded == true && preparation.FallbackAxis == zero))
    {
        return Fail(outDiagnosticOrNull, GeometryStatus::InvalidArgument, 0, "SeedUp");
    }
    if (preparation.MinimumSinAngle < 0.0 || preparation.MinimumSinAngle >= 1.0 || preparation.MinimumSpeed < 0.0)
    {
        return Fail(outDiagnosticOrNull, GeometryStatus::InvalidArgument, 0, "MinimumSinAngle");
    }
    return GeometryStatus::Success;
}

// Mutable state of one TryBuildFrames call.
struct Baker final
{
    const PathView& Path;
    const DistanceMapView& Map;
    const FramePreparation& Preparation;
    GeometryDiagnostic* DiagnosticOrNull = nullptr;
    core::Array<FrameSample>& Samples;
    core::Array<FrameRun>& Runs;
    usize MaxLevel = 0;
    bool HasRun = false;
    FrameState Current{};
    FrameState RunStart{};
    float64 ErrorSum = 0.0;
    float64 ErrorMax = 0.0;
    usize SubdivisionCount = 0;
    usize DeepestLevel = 0;
    float64 ResidualTwist = 0.0;
    float64 AppliedTwist = 0.0;
    bool IsPeriodic = false;
};

[[nodiscard]] GeometryStatus
TryAddSample(Baker& inoutBaker, SegmentParameter parameter, const FrameState& state) noexcept
{
    const usize sampleCount = inoutBaker.Samples.GetSize() + 1;
    const usize bytes = sampleCount * sizeof(FrameSample) + inoutBaker.Runs.GetSize() * sizeof(FrameRun);
    if (sampleCount > inoutBaker.Preparation.Limits.MaxLeaves || bytes > inoutBaker.Preparation.Limits.MaxPreparedBytes)
    {
        return Fail(inoutBaker.DiagnosticOrNull,
                    GeometryStatus::BudgetExceeded,
                    inoutBaker.Path.Spans[parameter.SpanIndex].SourceElement,
                    sampleCount > inoutBaker.Preparation.Limits.MaxLeaves ? "Limits.MaxLeaves"
                                                                          : "Limits.MaxPreparedBytes");
    }
    DistanceBound distance;
    GeometryStatus status = TryBoundDistance(inoutBaker.Path, inoutBaker.Map, parameter, distance);
    if (status != GeometryStatus::Success)
    {
        return Fail(inoutBaker.DiagnosticOrNull, status, 0, "Map");
    }
    FrameSample sample
    {
        .Parameter = parameter,
        .Distance = distance.Estimate,
        .RunIndex = inoutBaker.Runs.GetSize() - 1,
    };
    if (TryNarrow(state.Position, sample.Position) == false || TryNarrow(state.Tangent, sample.Tangent) == false ||
        TryNarrow(state.Normal, sample.Normal) == false)
    {
        return Fail(inoutBaker.DiagnosticOrNull, GeometryStatus::OutOfRange, 0, "Position");
    }
    if (inoutBaker.Samples.TryAdd(sample) == false)
    {
        return Fail(inoutBaker.DiagnosticOrNull, GeometryStatus::OutOfMemory, 0, "Samples");
    }
    FrameRun& run = inoutBaker.Runs[inoutBaker.Runs.GetSize() - 1];
    run.SampleCount += 1;
    run.End = parameter;
    inoutBaker.Current = state;
    return GeometryStatus::Success;
}

[[nodiscard]] GeometryStatus TryStartRun(Baker& inoutBaker, SegmentParameter parameter, FrameState& inoutState) noexcept
{
    const FramePreparation& preparation = inoutBaker.Preparation;
    bool isFallbackSeeded = false;
    if (TryProjectSeed(preparation.SeedUp, inoutState.Tangent, preparation.MinimumSinAngle, inoutState.Normal) == false)
    {
        isFallbackSeeded =
            preparation.SeedPolicy == FrameSeedPolicy::UseFallbackAxis && TryProjectSeed(preparation.FallbackAxis,
                                                                                         inoutState.Tangent,
                                                                                         preparation.MinimumSinAngle,
                                                                                         inoutState.Normal) == true;
        if (isFallbackSeeded == false)
        {
            WriteDiagnostic(inoutBaker.DiagnosticOrNull,
                            GeometryDiagnostic
                            {
                                .Code = GeometryStatus::DegenerateFrame,
                                .SourceElement = inoutBaker.Path.Spans[parameter.SpanIndex].SourceElement,
                                .Property = "SeedUp",
                                .RequestedTolerance = preparation.MinimumSinAngle,
                                .AchievedGuarantee = GuaranteeClass::Rounded,
                                .RangeStart = parameter.U,
                                .RangeEnd = parameter.U,
                            });
            return GeometryStatus::DegenerateFrame;
        }
    }
    const usize bytes =
        inoutBaker.Samples.GetSize() * sizeof(FrameSample) + (inoutBaker.Runs.GetSize() + 1) * sizeof(FrameRun);
    if (bytes > preparation.Limits.MaxPreparedBytes)
    {
        return Fail(inoutBaker.DiagnosticOrNull, GeometryStatus::BudgetExceeded, 0, "Limits.MaxPreparedBytes");
    }
    const FrameRun run
    {
        .FirstSample = inoutBaker.Samples.GetSize(),
        .Start = parameter,
        .End = parameter,
        .IsFallbackSeeded = isFallbackSeeded,
    };
    if (inoutBaker.Runs.TryAdd(run) == false)
    {
        return Fail(inoutBaker.DiagnosticOrNull, GeometryStatus::OutOfMemory, 0, "Runs");
    }
    if (inoutBaker.Runs.GetSize() == 1)
    {
        inoutBaker.RunStart = inoutState;
    }
    inoutBaker.HasRun = true;
    return TryAddSample(inoutBaker, parameter, inoutState);
}

[[nodiscard]] GeometryStatus
RejectRange(Baker& inoutBaker, usize spanIndex, float64 start, float64 end, const char* property) noexcept
{
    WriteDiagnostic(inoutBaker.DiagnosticOrNull,
                    GeometryDiagnostic
                    {
                        .Code = GeometryStatus::DegenerateFrame,
                        .SourceElement = inoutBaker.Path.Spans[spanIndex].SourceElement,
                        .Property = property,
                        .RequestedTolerance = inoutBaker.Preparation.MinimumSpeed,
                        .AchievedGuarantee = GuaranteeClass::Certified,
                        .Domain = DomainKind::SegmentParameter,
                        .RangeStart = start,
                        .RangeEnd = end,
                    });
    return GeometryStatus::DegenerateFrame;
}

// Carry an active run across the joint into span spanIndex > 0.
[[nodiscard]] GeometryStatus TryCrossJoint(Baker& inoutBaker, usize spanIndex) noexcept
{
    const PreparedSpan& span = inoutBaker.Path.Spans[spanIndex];
    const internal::SpanSample64 start = internal::EvaluateSpan64(span, 0.0);
    Vector3d tangent;
    if (math::Length(start.FirstDerivative) <= inoutBaker.Preparation.MinimumSpeed ||
        TryGetTangent(start, tangent) == false)
    {
        // A singular start ends the run; the span's stepping applies the singularity policy.
        inoutBaker.HasRun = false;
        return GeometryStatus::Success;
    }
    const JointContinuity& joint = inoutBaker.Path.Joints[spanIndex - 1];
    const Vector3d& incoming = inoutBaker.Current.Tangent;
    if (joint.IsG1 == false)
    {
        switch (inoutBaker.Preparation.CornerPolicy)
        {
            case FrameCornerPolicy::SplitRuns:
                inoutBaker.HasRun = false;
                return GeometryStatus::Success;
            case FrameCornerPolicy::RotateMinimally:
                if (IsRotationDefined(incoming, tangent, inoutBaker.Preparation.MinimumSinAngle) == false)
                {
                    return RejectRange(inoutBaker, spanIndex, 0.0, 0.0, "CornerPolicy");
                }
                break;
            case FrameCornerPolicy::Reject:
                return RejectRange(inoutBaker, spanIndex, 0.0, 0.0, "CornerPolicy");
        }
    }
    const FrameState state
    {
        .Position = start.Position,
        .Tangent = tangent,
        .Normal = RotateBetweenTangents(inoutBaker.Current.Normal, incoming, tangent),
    };
    return TryAddSample(inoutBaker, SegmentParameter{ .SpanIndex = spanIndex, .U = 0.0 }, state);
}

// NOLINTBEGIN(bugprone-easily-swappable-parameters): a value and its dyadic width share one type.
[[nodiscard]] bool IsEvenMultiple(float64 value, float64 width) noexcept
// NOLINTEND(bugprone-easily-swappable-parameters)
{
    const float64 doubleWidth = 2.0 * width;
    return math::Floor(value / doubleWidth) * doubleWidth == value;
}

[[nodiscard]] GeometryStatus TryBakeSpan(Baker& inoutBaker, usize spanIndex) noexcept
{
    const PreparedSpan& span = inoutBaker.Path.Spans[spanIndex];
    const FramePreparation& preparation = inoutBaker.Preparation;
    const float64 spanCount = static_cast<float64>(inoutBaker.Path.Spans.GetSize());
    const internal::EnclosureBox origin = internal::MakePointBox(Vector3d{});
    float64 a = 0.0;
    float64 width = 1.0;
    usize level = 0;
    while (a < 1.0)
    {
        const float64 b = a + width;
        inoutBaker.SubdivisionCount += 1;
        inoutBaker.DeepestLevel = math::Max(inoutBaker.DeepestLevel, level);
        if (inoutBaker.SubdivisionCount > preparation.Limits.MaxSubdivisions)
        {
            return Fail(inoutBaker.DiagnosticOrNull,
                        GeometryStatus::BudgetExceeded,
                        span.SourceElement,
                        "Limits.MaxSubdivisions");
        }
        internal::EnclosureBox hodograph;
        GeometryStatus status = internal::TryEncloseHodograph(span, a, b, hodograph);
        math::Enclosure speed;
        if (status == GeometryStatus::Success)
        {
            status = internal::TryBoundBoxDistance(hodograph, origin, speed);
        }
        if (status != GeometryStatus::Success)
        {
            return Fail(inoutBaker.DiagnosticOrNull, status, span.SourceElement, "Spans.Control");
        }
        bool isAccepted = false;
        if (speed.Lower > preparation.MinimumSpeed)
        {
            const internal::SpanSample64 sampleA = internal::EvaluateSpan64(span, a);
            const internal::SpanSample64 sampleM = internal::EvaluateSpan64(span, a + width * 0.5);
            const internal::SpanSample64 sampleB = internal::EvaluateSpan64(span, b);
            FrameState stateA{ .Position = sampleA.Position, .Tangent = Vector3d{}, .Normal = Vector3d{} };
            Vector3d tangentM;
            FrameState stateB{ .Position = sampleB.Position, .Tangent = Vector3d{}, .Normal = Vector3d{} };
            if (TryGetTangent(sampleA, stateA.Tangent) == false || TryGetTangent(sampleM, tangentM) == false ||
                TryGetTangent(sampleB, stateB.Tangent) == false)
            {
                return Fail(inoutBaker.DiagnosticOrNull,
                            GeometryStatus::OutOfRange,
                            span.SourceElement,
                            "Spans.Control");
            }
            if (inoutBaker.HasRun == true)
            {
                stateA.Normal = inoutBaker.Current.Normal;
            }
            else
            {
                status = TryStartRun(inoutBaker, SegmentParameter{ .SpanIndex = spanIndex, .U = a }, stateA);
                if (status != GeometryStatus::Success)
                {
                    return status;
                }
            }
            const Vector3d oneStep = TransportNormal(stateA, stateB.Position, stateB.Tangent);
            const FrameState stateM
            {
                .Position = sampleM.Position,
                .Tangent = tangentM,
                .Normal = TransportNormal(stateA, sampleM.Position, tangentM),
            };
            stateB.Normal = TransportNormal(stateM, stateB.Position, stateB.Tangent);
            const float64 difference = internal::ComputeAngle(oneStep, stateB.Normal);
            const float64 budget = inoutBaker.Path.Tolerance.TangentAngleError * width / spanCount;
            if (difference <= budget)
            {
                inoutBaker.ErrorSum += difference;
                inoutBaker.ErrorMax = math::Max(inoutBaker.ErrorMax, difference);
                status = TryAddSample(inoutBaker, SegmentParameter{ .SpanIndex = spanIndex, .U = b }, stateB);
                if (status != GeometryStatus::Success)
                {
                    return status;
                }
                isAccepted = true;
            }
            else if (level >= inoutBaker.MaxLevel)
            {
                WriteDiagnostic(inoutBaker.DiagnosticOrNull,
                                GeometryDiagnostic
                                {
                                    .Code = GeometryStatus::BudgetExceeded,
                                    .SourceElement = span.SourceElement,
                                    .Property = "Limits.MaxDepth",
                                    .RequestedTolerance = budget,
                                    .AchievedBound = difference,
                                    .AchievedGuarantee = GuaranteeClass::Estimated,
                                    .RangeStart = a,
                                    .RangeEnd = b,
                                });
                return GeometryStatus::BudgetExceeded;
            }
        }
        else if (speed.Upper <= preparation.MinimumSpeed || level >= inoutBaker.MaxLevel)
        {
            // Stationary throughout, or not provably regular at the depth limit: a singular range.
            if (preparation.SingularityPolicy == FrameSingularityPolicy::Reject)
            {
                return RejectRange(inoutBaker, spanIndex, a, b, "SingularityPolicy");
            }
            inoutBaker.HasRun = false;
            isAccepted = true;
        }
        if (isAccepted == true)
        {
            a = b;
            while (level > 0 && IsEvenMultiple(a, width) == true)
            {
                level -= 1;
                width *= 2.0;
            }
        }
        else
        {
            level += 1;
            width *= 0.5;
        }
    }
    return GeometryStatus::Success;
}

[[nodiscard]] GeometryStatus TryCloseLoop(Baker& inoutBaker) noexcept
{
    const PathView& path = inoutBaker.Path;
    const usize lastSpan = path.Spans.GetSize() - 1;
    const bool isLoopCovered = inoutBaker.Runs.GetSize() == 1 && inoutBaker.Runs[0].Start.SpanIndex == 0 &&
                               inoutBaker.Runs[0].Start.U == 0.0 && inoutBaker.Runs[0].End.SpanIndex == lastSpan &&
                               inoutBaker.Runs[0].End.U == 1.0 && path.Joints[lastSpan].IsG1 == true;
    if (isLoopCovered == false)
    {
        if (inoutBaker.Preparation.ClosedPolicy == ClosedFramePolicy::DistributeTwist)
        {
            return RejectRange(inoutBaker, lastSpan, 1.0, 1.0, "ClosedPolicy");
        }
        return GeometryStatus::Success;
    }
    const FrameState& start = inoutBaker.RunStart;
    const Vector3d arrived =
        RotateBetweenTangents(inoutBaker.Current.Normal, inoutBaker.Current.Tangent, start.Tangent);
    const float64 residual = internal::ComputeSignedAngle(start.Normal, arrived, start.Tangent);
    inoutBaker.ResidualTwist = residual;
    if (inoutBaker.Preparation.ClosedPolicy == ClosedFramePolicy::PreserveHolonomy)
    {
        return GeometryStatus::Success;
    }
    const float64 applied = -residual + math::kTau * static_cast<float64>(inoutBaker.Preparation.WindingTurns);
    const float64 total = inoutBaker.Map.TotalEstimate;
    for (usize i = 0; i < inoutBaker.Samples.GetSize(); ++i)
    {
        FrameSample& sample = inoutBaker.Samples[i];
        sample.TwistCorrection = total > 0.0 ? applied * (sample.Distance / total) : 0.0;
    }
    // The seam sample carries exactly the whole compensation.
    inoutBaker.Samples[inoutBaker.Samples.GetSize() - 1].TwistCorrection = applied;
    inoutBaker.AppliedTwist = applied;
    inoutBaker.IsPeriodic = true;
    return GeometryStatus::Success;
}

// Last sample index whose parameter is at or before parameter, or the sample count when none is.
[[nodiscard]] usize FindPrecedingSample(const FramesView& frames, SegmentParameter parameter) noexcept
{
    usize low = 0;
    usize high = frames.Samples.GetSize();
    while (low < high)
    {
        const usize middle = low + (high - low) / 2;
        if (IsLexicographicallyAfter(frames.Samples[middle].Parameter, parameter) == true)
        {
            high = middle;
        }
        else
        {
            low = middle + 1;
        }
    }
    return low == 0 ? frames.Samples.GetSize() : low - 1;
}
} // namespace

FramesView PreparedFrames::GetView() const noexcept
{
    return FramesView
    {
        .Samples = BorrowedView<const FrameSample>(mSamples.GetData(), mSamples.GetSize()),
        .Runs = BorrowedView<const FrameRun>(mRuns.GetData(), mRuns.GetSize()),
        .PathRevision = mPathRevision,
        .PathSpanCount = mPathSpanCount,
        .AngularErrorEstimate = mAngularErrorEstimate,
        .ResidualTwist = mResidualTwist,
        .AppliedTwist = mAppliedTwist,
        .IsPeriodic = mIsPeriodic,
        .ClosedPolicy = mClosedPolicy,
        .SubdivisionCount = mSubdivisionCount,
        .DeepestLevel = mDeepestLevel,
    };
}

bool PreparedFrames::IsEmpty() const noexcept
{
    return mSamples.IsEmpty();
}

GeometryStatus TryBuildFrames(const PathView& path,
                              const DistanceMapView& map,
                              const FramePreparation& preparation,
                              PreparedFrames& outFrames,
                              GeometryDiagnostic* outDiagnosticOrNull) noexcept
{
    GeometryStatus status = ValidatePreparation(path, preparation, outDiagnosticOrNull);
    if (status != GeometryStatus::Success)
    {
        return status;
    }
    if (map.Leaves.IsEmpty() == true)
    {
        return Fail(outDiagnosticOrNull, GeometryStatus::MissingCache, 0, "Map");
    }
    if (map.PathRevision != path.Revision || map.PathSpanCount != path.Spans.GetSize())
    {
        return Fail(outDiagnosticOrNull, GeometryStatus::StaleRevision, 0, "Map");
    }

    PreparedFrames candidate;
    Baker baker
    {
        .Path = path,
        .Map = map,
        .Preparation = preparation,
        .DiagnosticOrNull = outDiagnosticOrNull,
        .Samples = candidate.mSamples,
        .Runs = candidate.mRuns,
        .MaxLevel = math::Min(preparation.Limits.MaxDepth, MAX_DYADIC_LEVEL),
    };
    for (usize spanIndex = 0; spanIndex < path.Spans.GetSize(); ++spanIndex)
    {
        if (spanIndex > 0 && baker.HasRun == true)
        {
            status = TryCrossJoint(baker, spanIndex);
            if (status != GeometryStatus::Success)
            {
                return status;
            }
        }
        status = TryBakeSpan(baker, spanIndex);
        if (status != GeometryStatus::Success)
        {
            return status;
        }
    }
    if (baker.Runs.IsEmpty() == true)
    {
        return RejectRange(baker, 0, 0.0, 1.0, "SingularityPolicy");
    }
    if (path.IsClosed == true)
    {
        status = TryCloseLoop(baker);
        if (status != GeometryStatus::Success)
        {
            return status;
        }
    }
    candidate.mPathRevision = path.Revision;
    candidate.mPathSpanCount = path.Spans.GetSize();
    candidate.mAngularErrorEstimate = baker.ErrorSum + baker.ErrorMax;
    candidate.mResidualTwist = baker.ResidualTwist;
    candidate.mAppliedTwist = baker.AppliedTwist;
    candidate.mIsPeriodic = baker.IsPeriodic;
    candidate.mClosedPolicy = preparation.ClosedPolicy;
    candidate.mSubdivisionCount = baker.SubdivisionCount;
    candidate.mDeepestLevel = baker.DeepestLevel;
    outFrames = Move(candidate);
    return GeometryStatus::Success;
}

GeometryStatus TryEvaluateFrame(const PathView& path,
                                const FramesView& frames,
                                SegmentParameter parameter,
                                float64 rollRadians,
                                TransportedFrame& outFrame) noexcept
{
    if (frames.Samples.IsEmpty() == true)
    {
        return GeometryStatus::MissingCache;
    }
    if (frames.PathRevision != path.Revision || frames.PathSpanCount != path.Spans.GetSize())
    {
        return GeometryStatus::StaleRevision;
    }
    if (math::IsFinite(parameter.U) == false || math::IsFinite(rollRadians) == false)
    {
        return GeometryStatus::NonFiniteInput;
    }
    if (parameter.SpanIndex >= path.Spans.GetSize() || parameter.U < 0.0 || parameter.U > 1.0)
    {
        return GeometryStatus::InvalidDomain;
    }
    const usize index = FindPrecedingSample(frames, parameter);
    if (index == frames.Samples.GetSize() || frames.Samples[index].Parameter.SpanIndex != parameter.SpanIndex)
    {
        return GeometryStatus::Degenerate;
    }
    const FrameSample& sample = frames.Samples[index];
    const PreparedSpan& span = path.Spans[parameter.SpanIndex];
    Vector3d tangent = internal::ToVector3d(sample.Tangent);
    Vector3d normal = internal::ToVector3d(sample.Normal);
    float64 twist = sample.TwistCorrection;
    if (sample.Parameter.U != parameter.U)
    {
        const usize nextIndex = index + 1;
        if (nextIndex == frames.Samples.GetSize() || frames.Samples[nextIndex].RunIndex != sample.RunIndex ||
            frames.Samples[nextIndex].Parameter.SpanIndex != parameter.SpanIndex)
        {
            return GeometryStatus::Degenerate;
        }
        const FrameSample& next = frames.Samples[nextIndex];
        const internal::SpanSample64 from = internal::EvaluateSpan64(span, sample.Parameter.U);
        const internal::SpanSample64 to = internal::EvaluateSpan64(span, parameter.U);
        Vector3d fromTangent;
        if (TryGetTangent(from, fromTangent) == false || TryGetTangent(to, tangent) == false)
        {
            return GeometryStatus::Degenerate;
        }
        const FrameState state
        {
            .Position = from.Position,
            .Tangent = fromTangent,
            .Normal = Orthonormalize(normal, fromTangent),
        };
        normal = TransportNormal(state, to.Position, tangent);
        const float64 fraction = (parameter.U - sample.Parameter.U) / (next.Parameter.U - sample.Parameter.U);
        twist = sample.TwistCorrection + (next.TwistCorrection - sample.TwistCorrection) * fraction;
    }
    else
    {
        normal = Orthonormalize(normal, tangent);
    }
    const Vector3d rolled = internal::RotateAboutAxis(normal, tangent, twist + rollRadians);
    TransportedFrame frame{ .RunIndex = sample.RunIndex };
    const GeometryStatus status = TryEvaluatePath(path, parameter, frame.Position);
    if (status != GeometryStatus::Success)
    {
        return status;
    }
    if (TryNarrow(tangent, frame.Tangent) == false || TryNarrow(rolled, frame.Normal) == false ||
        TryNarrow(math::Cross(tangent, rolled), frame.Binormal) == false)
    {
        return GeometryStatus::OutOfRange;
    }
    outFrame = frame;
    return GeometryStatus::Success;
}
} // namespace ludus::foundation::geometry
