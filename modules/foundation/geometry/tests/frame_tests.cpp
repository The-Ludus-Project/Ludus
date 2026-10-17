// Rotation-minimizing frames against the G0/G3 reference corpus: straight, inflected, vertical-start, cusped
// and reversing paths, corners, random-access order independence and closed-loop residual twist.

#include "geometry_test_support.hpp"

#include <ludus/foundation/geometry/frame.hpp>
#include <ludus/foundation/math/scalar.hpp>

#include <catch2/catch_test_macros.hpp>

using namespace ludus::foundation::geometry;
using namespace ludus::foundation::geometry::test_support;
using ludus::foundation::BorrowedView;
using ludus::foundation::core::StaticArray;
namespace math = ludus::foundation::math;
using math::Vector3;
using math::Vector3d;

namespace
{
constexpr usize ORDER_SAMPLE_COUNT = 64;
// Float32 storage of unit vectors: a few units of 2^-24 per component (regression envelope, not a bound).
constexpr float64 UNIT_TOLERANCE = 1e-6;
// Angular agreement with the integrated Bishop references, radians: the profile's TangentAngleError.
constexpr float64 REFERENCE_ANGLE_TOLERANCE = 1e-3;
// Width of a singular range at FRAME_TEST_DEPTH: 2^-30.
constexpr float64 SINGULAR_WIDTH = 1.0 / static_cast<float64>(1U << FRAME_TEST_DEPTH);
// Near a simple zero the derivative box over an interval of width w can contain zero up to a few w away, and
// float64 rounding of the box widens that further; parameters this close to a cusp are treated as singular.
constexpr float64 SINGULARITY_REACH = 1e-6;

[[nodiscard]] Vector3d Widen(const Vector3& value) noexcept
{
    return Vector3d{static_cast<float64>(value.X), static_cast<float64>(value.Y), static_cast<float64>(value.Z)};
}

[[nodiscard]] Vector3d ToVector(const fixtures::ReferencePoint& point) noexcept
{
    return Vector3d{point.X, point.Y, point.Z};
}

[[nodiscard]] float64 ComputeAngle(const Vector3d& left, const Vector3d& right) noexcept
{
    return math::Atan2(math::Length(math::Cross(left, right)), math::Dot(left, right));
}

[[nodiscard]] bool IsNear(const Vector3d& actual, const Vector3d& expected, float64 tolerance) noexcept
{
    return math::Length(actual - expected) <= tolerance;
}

// Unit, orthogonal and right-handed (Binormal = Tangent x Normal).
[[nodiscard]] bool IsOrthonormal(const TransportedFrame& frame) noexcept
{
    const Vector3d tangent = Widen(frame.Tangent);
    const Vector3d normal = Widen(frame.Normal);
    const Vector3d binormal = Widen(frame.Binormal);
    return math::Abs(math::Length(tangent) - 1.0) <= UNIT_TOLERANCE &&
           math::Abs(math::Length(normal) - 1.0) <= UNIT_TOLERANCE &&
           math::Abs(math::Dot(tangent, normal)) <= UNIT_TOLERANCE &&
           IsNear(binormal, math::Cross(tangent, normal), UNIT_TOLERANCE);
}

[[nodiscard]] bool IsSameFrame(const TransportedFrame& left, const TransportedFrame& right) noexcept
{
    return left.Position == right.Position && left.Tangent == right.Tangent && left.Normal == right.Normal &&
           left.Binormal == right.Binormal && left.RunIndex == right.RunIndex;
}

[[nodiscard]] TransportedFrame Evaluate(const PreparedFixture& fixture,
                                        const PreparedFrames& frames,
                                        usize spanIndex,
                                        float64 u,
                                        GeometryStatus& outStatus,
                                        float64 roll = 0.0) noexcept
{
    TransportedFrame frame;
    outStatus = TryEvaluateFrame(fixture.Path.GetView(),
                                 frames.GetView(),
                                 SegmentParameter{ .SpanIndex = spanIndex, .U = u },
                                 roll,
                                 frame);
    return frame;
}

[[nodiscard]] PathSpanSource MakeLine(const Vector3d& start, const Vector3d& end, usize sourceElement) noexcept
{
    PathSpanSource span;
    span.Kind = SpanKind::Line;
    span.Control[0] = start;
    span.Control[SPAN_CONTROL_COUNT - 1] = end;
    span.SourceElement = sourceElement;
    return span;
}
} // namespace

TEST_CASE("Straight paths keep the projected seed and report an Estimated error", "[geometry][frame]")
{
    PreparedFixture fixture;
    REQUIRE(PrepareCubic(FindCubic("line_uniform"), fixture) == GeometryStatus::Success);
    PreparedFrames frames;
    REQUIRE(TryBuildFrames(fixture.Path.GetView(),
                           fixture.Map.GetView(),
                           MakeFramePreparation(Vector3d{1.0, 1.0, 0.0}),
                           frames,
                           nullptr) == GeometryStatus::Success);
    const FramesView view = frames.GetView();
    REQUIRE(view.Runs.GetSize() == 1);
    REQUIRE(view.AngularErrorGuarantee == GuaranteeClass::Estimated);
    REQUIRE(view.AngularErrorEstimate <= UNIT_TOLERANCE);
    REQUIRE(view.ResidualTwist == 0.0);
    for (usize step = 0; step <= 16; ++step)
    {
        GeometryStatus status = GeometryStatus::Success;
        const TransportedFrame frame = Evaluate(fixture, frames, 0, static_cast<float64>(step) / 16.0, status);
        REQUIRE(status == GeometryStatus::Success);
        REQUIRE(IsOrthonormal(frame) == true);
        // The +X+Y seed projected perpendicular to the +X tangent is +Y; a straight path never rotates it.
        REQUIRE(IsNear(Widen(frame.Normal), Vector3d{0.0, 1.0, 0.0}, UNIT_TOLERANCE) == true);
        REQUIRE(IsNear(Widen(frame.Binormal), Vector3d{0.0, 0.0, 1.0}, UNIT_TOLERANCE) == true);
    }

    SECTION("A strict profile is rejected because frame angular error is Estimated")
    {
        FramePreparation strict = MakeFramePreparation(Vector3d{0.0, 1.0, 0.0});
        strict.Precision = PrecisionProfile::Strict;
        GeometryDiagnostic diagnostic;
        REQUIRE(TryBuildFrames(fixture.Path.GetView(), fixture.Map.GetView(), strict, frames, &diagnostic) ==
                GeometryStatus::UnsupportedPrecision);
        REQUIRE(diagnostic.AchievedGuarantee == GuaranteeClass::Estimated);
        REQUIRE(frames.GetView().Samples.GetSize() == view.Samples.GetSize());
    }
}

TEST_CASE("Inflected paths transport without the Frenet flip", "[geometry][frame]")
{
    const fixtures::FrameFixture& reference = FindFrameFixture("inflection_rmf");
    PreparedFixture fixture;
    REQUIRE(PrepareFrameFixture(reference, fixture) == GeometryStatus::Success);

    PreparedFrames frames;
    REQUIRE(TryBuildFrames(fixture.Path.GetView(),
                           fixture.Map.GetView(),
                           MakeFramePreparation(ToVector(reference.SeedUp)),
                           frames,
                           nullptr) == GeometryStatus::Success);
    for (usize referenceIndex = 0; referenceIndex < reference.Frames.GetSize(); ++referenceIndex)
    {
        const fixtures::FrameReference& expected = reference.Frames[referenceIndex];
        GeometryStatus status = GeometryStatus::Success;
        const TransportedFrame frame = Evaluate(fixture, frames, expected.SpanIndex, expected.U, status);
        REQUIRE(status == GeometryStatus::Success);
        REQUIRE(IsNear(Widen(frame.Normal), ToVector(expected.Normal), UNIT_TOLERANCE) == true);
    }

    // With an in-plane seed the normal stays in the plane and on the same side of the tangent through u = 1/2.
    PreparedFrames inPlane;
    REQUIRE(TryBuildFrames(fixture.Path.GetView(),
                           fixture.Map.GetView(),
                           MakeFramePreparation(Vector3d{0.0, 1.0, 0.0}),
                           inPlane,
                           nullptr) == GeometryStatus::Success);
    for (usize step = 0; step <= 32; ++step)
    {
        GeometryStatus status = GeometryStatus::Success;
        const TransportedFrame frame = Evaluate(fixture, inPlane, 0, static_cast<float64>(step) / 32.0, status);
        REQUIRE(status == GeometryStatus::Success);
        REQUIRE(IsOrthonormal(frame) == true);
        REQUIRE(math::Abs(static_cast<float64>(frame.Normal.Z)) <= UNIT_TOLERANCE);
        // Binormal = Tangent x Normal stays +Z: a Frenet frame would switch sides at the inflection.
        REQUIRE(IsNear(Widen(frame.Binormal), Vector3d{0.0, 0.0, 1.0}, UNIT_TOLERANCE) == true);
    }
}

TEST_CASE("Transported normals match the integrated Bishop frame of the twisted cubic", "[geometry][frame]")
{
    const fixtures::FrameFixture& reference = FindFrameFixture("twisted_rmf");
    PreparedFixture fixture;
    REQUIRE(PrepareFrameFixture(reference, fixture) == GeometryStatus::Success);
    PreparedFrames frames;
    REQUIRE(TryBuildFrames(fixture.Path.GetView(),
                           fixture.Map.GetView(),
                           MakeFramePreparation(ToVector(reference.SeedUp)),
                           frames,
                           nullptr) == GeometryStatus::Success);
    const FramesView view = frames.GetView();
    REQUIRE(view.AngularErrorEstimate <= fixture.Path.GetView().Tolerance.TangentAngleError * 2.0);
    for (usize referenceIndex = 0; referenceIndex < reference.Frames.GetSize(); ++referenceIndex)
    {
        const fixtures::FrameReference& expected = reference.Frames[referenceIndex];
        GeometryStatus status = GeometryStatus::Success;
        const TransportedFrame frame = Evaluate(fixture, frames, expected.SpanIndex, expected.U, status);
        REQUIRE(status == GeometryStatus::Success);
        REQUIRE(IsOrthonormal(frame) == true);
        const float64 error = ComputeAngle(Widen(frame.Normal), ToVector(expected.Normal));
        REQUIRE(error <= REFERENCE_ANGLE_TOLERANCE);
        // The Estimated bound is conservative on this fixture (it is still not a certificate).
        REQUIRE(error <= view.AngularErrorEstimate + UNIT_TOLERANCE);
    }
}

TEST_CASE("A seed parallel to the start tangent needs the declared fallback axis", "[geometry][frame]")
{
    PreparedFixture fixture;
    REQUIRE(PrepareCubic(FindCubic("vertical_start"), fixture) == GeometryStatus::Success);
    PreparedFrames frames;
    REQUIRE(TryBuildFrames(fixture.Path.GetView(),
                           fixture.Map.GetView(),
                           MakeFramePreparation(Vector3d{0.0, 0.0, 1.0}),
                           frames,
                           nullptr) == GeometryStatus::Success);
    const usize previousSampleCount = frames.GetView().Samples.GetSize();

    FramePreparation preparation = MakeFramePreparation(Vector3d{0.0, 1.0, 0.0});
    GeometryDiagnostic diagnostic;
    REQUIRE(TryBuildFrames(fixture.Path.GetView(), fixture.Map.GetView(), preparation, frames, &diagnostic) ==
            GeometryStatus::DegenerateFrame);
    REQUIRE(diagnostic.Code == GeometryStatus::DegenerateFrame);
    REQUIRE(frames.GetView().Samples.GetSize() == previousSampleCount);
    REQUIRE(frames.GetView().Runs[0].IsFallbackSeeded == false);

    preparation.SeedPolicy = FrameSeedPolicy::UseFallbackAxis;
    preparation.FallbackAxis = Vector3d{0.0, -1.0, 0.0};
    REQUIRE(TryBuildFrames(fixture.Path.GetView(), fixture.Map.GetView(), preparation, frames, nullptr) ==
            GeometryStatus::DegenerateFrame);

    preparation.FallbackAxis = Vector3d{0.0, 0.0, 2.0};
    REQUIRE(TryBuildFrames(fixture.Path.GetView(), fixture.Map.GetView(), preparation, frames, nullptr) ==
            GeometryStatus::Success);
    REQUIRE(frames.GetView().Runs[0].IsFallbackSeeded == true);
    GeometryStatus status = GeometryStatus::Success;
    const TransportedFrame start = Evaluate(fixture, frames, 0, 0.0, status);
    REQUIRE(status == GeometryStatus::Success);
    REQUIRE(IsNear(Widen(start.Normal), Vector3d{0.0, 0.0, 1.0}, UNIT_TOLERANCE) == true);
    // The planar path keeps the out-of-plane normal everywhere.
    const TransportedFrame end = Evaluate(fixture, frames, 0, 1.0, status);
    REQUIRE(status == GeometryStatus::Success);
    REQUIRE(IsNear(Widen(end.Normal), Vector3d{0.0, 0.0, 1.0}, UNIT_TOLERANCE) == true);
}

TEST_CASE("Cusps and zero-speed endpoints follow the singularity policy", "[geometry][frame]")
{
    PreparedFixture cusp;
    REQUIRE(PrepareCubic(FindCubic("cusp"), cusp) == GeometryStatus::Success);
    FramePreparation preparation = MakeFramePreparation(Vector3d{0.0, 0.0, 1.0});
    PreparedFrames frames;
    GeometryDiagnostic diagnostic;
    REQUIRE(TryBuildFrames(cusp.Path.GetView(), cusp.Map.GetView(), preparation, frames, &diagnostic) ==
            GeometryStatus::DegenerateFrame);
    REQUIRE(frames.IsEmpty() == true);
    REQUIRE(diagnostic.Domain == DomainKind::SegmentParameter);
    // The first range that cannot be shown regular lies at the cusp, within a few derivative-box widths.
    REQUIRE(math::Abs(diagnostic.RangeStart - 0.5) <= SINGULARITY_REACH);
    REQUIRE(diagnostic.RangeEnd - diagnostic.RangeStart == SINGULAR_WIDTH);

    preparation.SingularityPolicy = FrameSingularityPolicy::SplitRuns;
    REQUIRE(TryBuildFrames(cusp.Path.GetView(), cusp.Map.GetView(), preparation, frames, nullptr) ==
            GeometryStatus::Success);
    const FramesView view = frames.GetView();
    REQUIRE(view.Runs.GetSize() == 2);
    REQUIRE(view.Runs[0].End.U < 0.5);
    REQUIRE(view.Runs[1].Start.U > 0.5);
    REQUIRE(0.5 - view.Runs[0].End.U <= SINGULARITY_REACH);
    REQUIRE(view.Runs[1].Start.U - 0.5 <= SINGULARITY_REACH);
    GeometryStatus status = GeometryStatus::Success;
    TransportedFrame unchanged{ .RunIndex = 7 };
    REQUIRE(TryEvaluateFrame(cusp.Path.GetView(), view, SegmentParameter{ .U = 0.5 }, 0.0, unchanged) ==
            GeometryStatus::Degenerate);
    REQUIRE(unchanged.RunIndex == 7);
    const TransportedFrame before = Evaluate(cusp, frames, 0, 0.25, status);
    REQUIRE(status == GeometryStatus::Success);
    REQUIRE(before.RunIndex == 0);
    const TransportedFrame after = Evaluate(cusp, frames, 0, 0.75, status);
    REQUIRE(status == GeometryStatus::Success);
    REQUIRE(after.RunIndex == 1);
    REQUIRE(IsNear(Widen(before.Normal), Vector3d{0.0, 0.0, 1.0}, UNIT_TOLERANCE) == true);
    REQUIRE(IsNear(Widen(after.Normal), Vector3d{0.0, 0.0, 1.0}, UNIT_TOLERANCE) == true);

    PreparedFixture ease;
    REQUIRE(PrepareCubic(FindCubic("line_ease"), ease) == GeometryStatus::Success);
    REQUIRE(TryBuildFrames(ease.Path.GetView(), ease.Map.GetView(), preparation, frames, nullptr) ==
            GeometryStatus::Success);
    REQUIRE(frames.GetView().Runs.GetSize() == 1);
    REQUIRE(frames.GetView().Runs[0].Start.U > 0.0);
    REQUIRE(frames.GetView().Runs[0].End.U < 1.0);
    (void)Evaluate(ease, frames, 0, 0.0, status);
    REQUIRE(status == GeometryStatus::Degenerate);
    (void)Evaluate(ease, frames, 0, 1.0, status);
    REQUIRE(status == GeometryStatus::Degenerate);
    (void)Evaluate(ease, frames, 0, 0.5, status);
    REQUIRE(status == GeometryStatus::Success);
}

TEST_CASE("Opposite tangents never reuse a frame across the reversal", "[geometry][frame]")
{
    PreparedFixture backtrack;
    REQUIRE(PrepareCubic(FindCubic("line_backtrack"), backtrack) == GeometryStatus::Success);
    FramePreparation preparation = MakeFramePreparation(Vector3d{0.0, 1.0, 0.0});
    PreparedFrames frames;
    REQUIRE(TryBuildFrames(backtrack.Path.GetView(), backtrack.Map.GetView(), preparation, frames, nullptr) ==
            GeometryStatus::DegenerateFrame);
    preparation.SingularityPolicy = FrameSingularityPolicy::SplitRuns;
    REQUIRE(TryBuildFrames(backtrack.Path.GetView(), backtrack.Map.GetView(), preparation, frames, nullptr) ==
            GeometryStatus::Success);
    REQUIRE(frames.GetView().Runs.GetSize() == 3);
    for (const fixtures::CubicFixture& fixture : fixtures::CUBIC_FIXTURES)
    {
        if (std::strcmp(fixture.Name, "line_backtrack") != 0)
        {
            continue;
        }
        for (usize zeroIndex = 0; zeroIndex < fixture.ZeroSpeedU.GetSize(); ++zeroIndex)
        {
            const float64 zeroSpeed = fixture.ZeroSpeedU[zeroIndex];
            GeometryStatus status = GeometryStatus::Success;
            (void)Evaluate(backtrack, frames, 0, zeroSpeed, status);
            REQUIRE(status == GeometryStatus::Degenerate);
        }
    }
    GeometryStatus status = GeometryStatus::Success;
    const TransportedFrame middle = Evaluate(backtrack, frames, 0, 0.5, status);
    REQUIRE(status == GeometryStatus::Success);
    REQUIRE(middle.RunIndex == 1);
    REQUIRE(IsNear(Widen(middle.Tangent), Vector3d{-1.0, 0.0, 0.0}, UNIT_TOLERANCE) == true);
    // Each run is reseeded: the middle run's +Y normal makes its binormal -Z, the opposite of the outer runs.
    REQUIRE(IsNear(Widen(middle.Binormal), Vector3d{0.0, 0.0, -1.0}, UNIT_TOLERANCE) == true);
    const TransportedFrame first = Evaluate(backtrack, frames, 0, 0.05, status);
    REQUIRE(status == GeometryStatus::Success);
    REQUIRE(IsNear(Widen(first.Binormal), Vector3d{0.0, 0.0, 1.0}, UNIT_TOLERANCE) == true);

    // An exact reversal at a joint has no unique minimal rotation axis.
    PreparedFixture joint;
    joint.Spans[0] = MakeLine(Vector3d{0.0, 0.0, 0.0}, Vector3d{1.0, 0.0, 0.0}, 0);
    joint.Spans[1] = MakeLine(Vector3d{1.0, 0.0, 0.0}, Vector3d{0.0, 0.0, 0.0}, 1);
    REQUIRE(PrepareStoredSpans(2, false, joint) == GeometryStatus::Success);
    preparation.CornerPolicy = FrameCornerPolicy::RotateMinimally;
    REQUIRE(TryBuildFrames(joint.Path.GetView(), joint.Map.GetView(), preparation, frames, nullptr) ==
            GeometryStatus::DegenerateFrame);
    preparation.CornerPolicy = FrameCornerPolicy::Reject;
    REQUIRE(TryBuildFrames(joint.Path.GetView(), joint.Map.GetView(), preparation, frames, nullptr) ==
            GeometryStatus::DegenerateFrame);
    preparation.CornerPolicy = FrameCornerPolicy::SplitRuns;
    REQUIRE(TryBuildFrames(joint.Path.GetView(), joint.Map.GetView(), preparation, frames, nullptr) ==
            GeometryStatus::Success);
    REQUIRE(frames.GetView().Runs.GetSize() == 2);
    const TransportedFrame incoming = Evaluate(joint, frames, 0, 1.0, status);
    REQUIRE(status == GeometryStatus::Success);
    const TransportedFrame outgoing = Evaluate(joint, frames, 1, 0.0, status);
    REQUIRE(status == GeometryStatus::Success);
    REQUIRE(incoming.RunIndex == 0);
    REQUIRE(outgoing.RunIndex == 1);
    REQUIRE(IsNear(Widen(outgoing.Tangent), Vector3d{-1.0, 0.0, 0.0}, UNIT_TOLERANCE) == true);
}

TEST_CASE("A minimal rotation carries the frame around a corner", "[geometry][frame]")
{
    PreparedFixture corner;
    corner.Spans[0] = MakeLine(Vector3d{0.0, 0.0, 0.0}, Vector3d{1.0, 0.0, 0.0}, 0);
    corner.Spans[1] = MakeLine(Vector3d{1.0, 0.0, 0.0}, Vector3d{1.0, 1.0, 0.0}, 1);
    REQUIRE(PrepareStoredSpans(2, false, corner) == GeometryStatus::Success);
    FramePreparation preparation = MakeFramePreparation(Vector3d{0.0, 1.0, 0.0});
    PreparedFrames frames;
    GeometryDiagnostic diagnostic;
    REQUIRE(TryBuildFrames(corner.Path.GetView(), corner.Map.GetView(), preparation, frames, &diagnostic) ==
            GeometryStatus::DegenerateFrame);
    REQUIRE(std::strcmp(diagnostic.Property, "CornerPolicy") == 0);
    preparation.CornerPolicy = FrameCornerPolicy::RotateMinimally;
    REQUIRE(TryBuildFrames(corner.Path.GetView(), corner.Map.GetView(), preparation, frames, nullptr) ==
            GeometryStatus::Success);
    REQUIRE(frames.GetView().Runs.GetSize() == 1);
    GeometryStatus status = GeometryStatus::Success;
    // Rotating +X onto +Y about +Z carries the +Y normal to -X; the incoming side keeps +Y.
    const TransportedFrame incoming = Evaluate(corner, frames, 0, 1.0, status);
    REQUIRE(status == GeometryStatus::Success);
    REQUIRE(IsNear(Widen(incoming.Normal), Vector3d{0.0, 1.0, 0.0}, UNIT_TOLERANCE) == true);
    const TransportedFrame outgoing = Evaluate(corner, frames, 1, 0.5, status);
    REQUIRE(status == GeometryStatus::Success);
    REQUIRE(IsNear(Widen(outgoing.Normal), Vector3d{-1.0, 0.0, 0.0}, UNIT_TOLERANCE) == true);
    REQUIRE(IsNear(Widen(outgoing.Binormal), Vector3d{0.0, 0.0, 1.0}, UNIT_TOLERANCE) == true);
}

TEST_CASE("Random-access frames do not depend on query order and apply roll", "[geometry][frame]")
{
    const fixtures::FrameFixture& reference = FindFrameFixture("loop_holonomy");
    PreparedFixture fixture;
    REQUIRE(PrepareFrameFixture(reference, fixture) == GeometryStatus::Success);
    PreparedFrames frames;
    REQUIRE(TryBuildFrames(fixture.Path.GetView(),
                           fixture.Map.GetView(),
                           MakeFramePreparation(ToVector(reference.SeedUp)),
                           frames,
                           nullptr) == GeometryStatus::Success);
    const usize spanCount = fixture.SpanCount;
    StaticArray<TransportedFrame, ORDER_SAMPLE_COUNT> forward;
    StaticArray<TransportedFrame, ORDER_SAMPLE_COUNT> backward;
    StaticArray<TransportedFrame, ORDER_SAMPLE_COUNT> strided;
    const auto parameterAt = [spanCount](usize i) noexcept // ludus-style: inferred-type: closure
    {
        const float64 position =
            static_cast<float64>(i) * static_cast<float64>(spanCount) / static_cast<float64>(ORDER_SAMPLE_COUNT - 1);
        const usize span = math::Min(static_cast<usize>(position), spanCount - 1);
        return SegmentParameter{ .SpanIndex = span, .U = math::Min(position - static_cast<float64>(span), 1.0) };
    };
    bool isEverySuccess = true;
    for (usize i = 0; i < ORDER_SAMPLE_COUNT; ++i)
    {
        isEverySuccess = isEverySuccess == true &&
                         TryEvaluateFrame(fixture.Path.GetView(), frames.GetView(), parameterAt(i), 0.25, forward[i]) ==
                             GeometryStatus::Success;
    }
    for (usize i = ORDER_SAMPLE_COUNT; i > 0; --i)
    {
        isEverySuccess =
            isEverySuccess == true &&
            TryEvaluateFrame(fixture.Path.GetView(), frames.GetView(), parameterAt(i - 1), 0.25, backward[i - 1]) ==
                GeometryStatus::Success;
    }
    // A coprime stride visits every sample in an interleaved order.
    constexpr usize STRIDE = 37;
    for (usize k = 0; k < ORDER_SAMPLE_COUNT; ++k)
    {
        const usize i = (k * STRIDE) % ORDER_SAMPLE_COUNT;
        isEverySuccess = isEverySuccess == true &&
                         TryEvaluateFrame(fixture.Path.GetView(), frames.GetView(), parameterAt(i), 0.25, strided[i]) ==
                             GeometryStatus::Success;
    }
    REQUIRE(isEverySuccess == true);
    for (usize i = 0; i < ORDER_SAMPLE_COUNT; ++i)
    {
        REQUIRE(IsSameFrame(forward[i], backward[i]) == true);
        REQUIRE(IsSameFrame(forward[i], strided[i]) == true);
        REQUIRE(IsOrthonormal(forward[i]) == true);
    }

    // A quarter turn of roll moves the normal onto the unrolled binormal.
    GeometryStatus status = GeometryStatus::Success;
    const TransportedFrame plain = Evaluate(fixture, frames, 1, 0.3, status);
    REQUIRE(status == GeometryStatus::Success);
    const TransportedFrame rolled = Evaluate(fixture, frames, 1, 0.3, status, math::kHalfPi);
    REQUIRE(status == GeometryStatus::Success);
    REQUIRE(IsNear(Widen(rolled.Normal), Widen(plain.Binormal), UNIT_TOLERANCE) == true);
    REQUIRE(IsNear(Widen(rolled.Binormal), -Widen(plain.Normal), UNIT_TOLERANCE) == true);
}

TEST_CASE("Closed loops measure residual twist and distribute or preserve it", "[geometry][frame]")
{
    const fixtures::FrameFixture& reference = FindFrameFixture("loop_holonomy");
    REQUIRE(math::Abs(reference.ResidualTwist) > 0.1);
    PreparedFixture fixture;
    REQUIRE(PrepareFrameFixture(reference, fixture) == GeometryStatus::Success);
    const usize lastSpan = fixture.SpanCount - 1;

    FramePreparation preparation = MakeFramePreparation(ToVector(reference.SeedUp));
    preparation.ClosedPolicy = ClosedFramePolicy::PreserveHolonomy;
    PreparedFrames preserved;
    REQUIRE(TryBuildFrames(fixture.Path.GetView(), fixture.Map.GetView(), preparation, preserved, nullptr) ==
            GeometryStatus::Success);
    REQUIRE(preserved.GetView().IsPeriodic == false);
    REQUIRE(math::Abs(preserved.GetView().ResidualTwist - reference.ResidualTwist) <= REFERENCE_ANGLE_TOLERANCE);
    for (usize referenceIndex = 0; referenceIndex < reference.Frames.GetSize(); ++referenceIndex)
    {
        const fixtures::FrameReference& expected = reference.Frames[referenceIndex];
        GeometryStatus status = GeometryStatus::Success;
        const TransportedFrame frame = Evaluate(fixture, preserved, expected.SpanIndex, expected.U, status);
        REQUIRE(status == GeometryStatus::Success);
        REQUIRE(ComputeAngle(Widen(frame.Normal), ToVector(expected.Normal)) <= REFERENCE_ANGLE_TOLERANCE);
    }

    preparation.ClosedPolicy = ClosedFramePolicy::DistributeTwist;
    PreparedFrames distributed;
    REQUIRE(TryBuildFrames(fixture.Path.GetView(), fixture.Map.GetView(), preparation, distributed, nullptr) ==
            GeometryStatus::Success);
    const FramesView view = distributed.GetView();
    REQUIRE(view.IsPeriodic == true);
    REQUIRE(view.AppliedTwist == -view.ResidualTwist);
    GeometryStatus status = GeometryStatus::Success;
    const TransportedFrame start = Evaluate(fixture, distributed, 0, 0.0, status);
    REQUIRE(status == GeometryStatus::Success);
    const TransportedFrame seam = Evaluate(fixture, distributed, lastSpan, 1.0, status);
    REQUIRE(status == GeometryStatus::Success);
    // The compensated seam returns to the start frame up to the closing joint's G1 angle and float32 storage.
    REQUIRE(ComputeAngle(Widen(seam.Normal), Widen(start.Normal)) <= UNIT_TOLERANCE * 10.0);
    REQUIRE(ComputeAngle(Widen(seam.Tangent), Widen(start.Tangent)) <= UNIT_TOLERANCE);

    // Authored winding adds whole turns, which still closes the seam.
    preparation.WindingTurns = 1;
    REQUIRE(TryBuildFrames(fixture.Path.GetView(), fixture.Map.GetView(), preparation, distributed, nullptr) ==
            GeometryStatus::Success);
    REQUIRE(distributed.GetView().AppliedTwist > math::kPi);
    const TransportedFrame wound = Evaluate(fixture, distributed, lastSpan, 1.0, status);
    REQUIRE(status == GeometryStatus::Success);
    REQUIRE(ComputeAngle(Widen(wound.Normal), Widen(start.Normal)) <= UNIT_TOLERANCE * 10.0);

    // A closed seam that is not G1 cannot be periodic.
    PreparedFixture square;
    square.Spans[0] = MakeLine(Vector3d{0.0, 0.0, 0.0}, Vector3d{1.0, 0.0, 0.0}, 0);
    square.Spans[1] = MakeLine(Vector3d{1.0, 0.0, 0.0}, Vector3d{1.0, 1.0, 0.0}, 1);
    square.Spans[2] = MakeLine(Vector3d{1.0, 1.0, 0.0}, Vector3d{0.0, 0.0, 0.0}, 2);
    REQUIRE(PrepareStoredSpans(3, true, square) == GeometryStatus::Success);
    FramePreparation cornered = MakeFramePreparation(Vector3d{0.0, 0.0, 1.0});
    cornered.CornerPolicy = FrameCornerPolicy::RotateMinimally;
    PreparedFrames squareFrames;
    REQUIRE(TryBuildFrames(square.Path.GetView(), square.Map.GetView(), cornered, squareFrames, nullptr) ==
            GeometryStatus::DegenerateFrame);
    cornered.ClosedPolicy = ClosedFramePolicy::PreserveHolonomy;
    REQUIRE(TryBuildFrames(square.Path.GetView(), square.Map.GetView(), cornered, squareFrames, nullptr) ==
            GeometryStatus::Success);
    REQUIRE(squareFrames.GetView().IsPeriodic == false);
}

TEST_CASE("Frame caches report missing, stale and invalid inputs and exhausted limits", "[geometry][frame]")
{
    PreparedFixture fixture;
    REQUIRE(PrepareCubic(FindCubic("twisted"), fixture) == GeometryStatus::Success);
    const FramePreparation preparation = MakeFramePreparation(Vector3d{0.0, 1.0, 0.0});
    PreparedFrames frames;
    TransportedFrame frame;
    REQUIRE(TryEvaluateFrame(fixture.Path.GetView(), frames.GetView(), SegmentParameter{}, 0.0, frame) ==
            GeometryStatus::MissingCache);
    REQUIRE(TryBuildFrames(fixture.Path.GetView(), PreparedDistanceMap{}.GetView(), preparation, frames, nullptr) ==
            GeometryStatus::MissingCache);
    REQUIRE(TryBuildFrames(fixture.Path.GetView(), fixture.Map.GetView(), preparation, frames, nullptr) ==
            GeometryStatus::Success);
    const usize sampleCount = frames.GetView().Samples.GetSize();

    PathView other = fixture.Path.GetView();
    other.Revision = 2;
    REQUIRE(TryEvaluateFrame(other, frames.GetView(), SegmentParameter{}, 0.0, frame) == GeometryStatus::StaleRevision);
    REQUIRE(TryBuildFrames(other, fixture.Map.GetView(), preparation, frames, nullptr) ==
            GeometryStatus::StaleRevision);
    REQUIRE(TryEvaluateFrame(fixture.Path.GetView(), frames.GetView(), SegmentParameter{ .U = 1.5 }, 0.0, frame) ==
            GeometryStatus::InvalidDomain);
    REQUIRE(TryEvaluateFrame(fixture.Path.GetView(), frames.GetView(), SegmentParameter{}, math::kPi / 0.0, frame) ==
            GeometryStatus::NonFiniteInput);

    FramePreparation invalid = preparation;
    invalid.MinimumSinAngle = 1.0;
    REQUIRE(TryBuildFrames(fixture.Path.GetView(), fixture.Map.GetView(), invalid, frames, nullptr) ==
            GeometryStatus::InvalidArgument);
    invalid = preparation;
    invalid.SeedUp = Vector3d{};
    REQUIRE(TryBuildFrames(fixture.Path.GetView(), fixture.Map.GetView(), invalid, frames, nullptr) ==
            GeometryStatus::InvalidArgument);
    invalid.SeedUp = Vector3d{0.0, math::kPi / 0.0, 0.0};
    REQUIRE(TryBuildFrames(fixture.Path.GetView(), fixture.Map.GetView(), invalid, frames, nullptr) ==
            GeometryStatus::NonFiniteInput);

    FramePreparation limited = preparation;
    limited.Limits.MaxLeaves = 2;
    REQUIRE(TryBuildFrames(fixture.Path.GetView(), fixture.Map.GetView(), limited, frames, nullptr) ==
            GeometryStatus::BudgetExceeded);
    limited = preparation;
    limited.Limits.MaxDepth = 1;
    GeometryDiagnostic diagnostic;
    REQUIRE(TryBuildFrames(fixture.Path.GetView(), fixture.Map.GetView(), limited, frames, &diagnostic) ==
            GeometryStatus::BudgetExceeded);
    REQUIRE(diagnostic.AchievedGuarantee == GuaranteeClass::Estimated);
    REQUIRE(diagnostic.AchievedBound > diagnostic.RequestedTolerance);
    limited = preparation;
    limited.Limits.MaxSubdivisions = 3;
    REQUIRE(TryBuildFrames(fixture.Path.GetView(), fixture.Map.GetView(), limited, frames, nullptr) ==
            GeometryStatus::BudgetExceeded);
    limited = preparation;
    limited.Limits.MaxPreparedBytes = sizeof(FrameSample);
    REQUIRE(TryBuildFrames(fixture.Path.GetView(), fixture.Map.GetView(), limited, frames, nullptr) ==
            GeometryStatus::BudgetExceeded);
    REQUIRE(frames.GetView().Samples.GetSize() == sampleCount);
}
