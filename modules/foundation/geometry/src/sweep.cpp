// Sweep mesh planning and emission (see sweep.hpp).

#include <ludus/foundation/geometry/sweep.hpp>

#include "internal/arc_length.hpp"
#include "internal/control_box.hpp"
#include "internal/span_math.hpp"

#include <ludus/foundation/containers/static_array.hpp>
#include <ludus/foundation/math/scalar.hpp>

namespace ludus::foundation::geometry
{
using math::Vector3d;

namespace
{
// Dyadic levels below one span whose interval ends stay exact float64 values in [0,1].
constexpr usize MAX_DYADIC_LEVEL = 52;
// Largest finite float32, the narrowing range of emitted positions.
constexpr float64 FLOAT32_LARGEST = 3.4028234663852886e38;
// Vertices that uint32 indices can name.
constexpr uint64 MAX_INDEXED_VERTEX_COUNT = uint64{1} << 32U;
// Smallest polygon that encloses area.
constexpr usize MIN_TUBE_SEGMENTS = 3;
// Vertices of one ribbon ring: the -Binormal and +Binormal edges.
constexpr usize RIBBON_RING_VERTEX_COUNT = 2;
// Indices of one triangle and of one quad split into two triangles.
constexpr usize TRIANGLE_INDEX_COUNT = 3;
constexpr usize QUAD_INDEX_COUNT = 6;
// Caps of an open tube: start and end.
constexpr usize CAP_COUNT = 2;
// Section half-extent per unit scale: a ribbon is Scale wide, a tube has radius Scale.
constexpr float64 RIBBON_HALF_EXTENT = 0.5;
constexpr float64 TUBE_HALF_EXTENT = 1.0;

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

[[nodiscard]] bool TryMultiply(usize left, usize right, usize& outProduct) noexcept
{
    if (left != 0 && right > static_cast<usize>(-1) / left)
    {
        return false;
    }
    outProduct = left * right;
    return true;
}

[[nodiscard]] bool TryAdd(usize left, usize right, usize& outSum) noexcept
{
    if (right > static_cast<usize>(-1) - left)
    {
        return false;
    }
    outSum = left + right;
    return true;
}

[[nodiscard]] float64 GetHalfExtent(SweepSection section) noexcept
{
    return section == SweepSection::Tube ? TUBE_HALF_EXTENT : RIBBON_HALF_EXTENT;
}

// Cosine and sine of section angle j of segmentCount; j = segmentCount repeats j = 0 exactly.
struct SectionDirection final
{
    float64 Cosine = 1.0;
    float64 Sine = 0.0;
};

[[nodiscard]] SectionDirection GetSectionDirection(usize j, usize segmentCount) noexcept
{
    if (j == 0 || j == segmentCount)
    {
        return SectionDirection{};
    }
    const float64 angle = math::kTau * static_cast<float64>(j) / static_cast<float64>(segmentCount);
    return SectionDirection{ .Cosine = math::Cos(angle), .Sine = math::Sin(angle) };
}

// Offset of section point j from the ring center. Ribbons use j = 0 (-Binormal) and j = 1 (+Binormal).
[[nodiscard]] Vector3d
ComputeSectionOffset(const SweepRing& ring, SweepSection section, usize j, usize segmentCount) noexcept
{
    if (section == SweepSection::Ribbon)
    {
        const float64 side = j == 0 ? -RIBBON_HALF_EXTENT : RIBBON_HALF_EXTENT;
        return ring.Binormal * (side * ring.Scale);
    }
    const SectionDirection direction = GetSectionDirection(j, segmentCount);
    return (ring.Binormal * direction.Cosine + ring.Normal * direction.Sine) * ring.Scale;
}

[[nodiscard]] math::Vector3 Narrow(const Vector3d& value) noexcept
{
    return math::Vector3{static_cast<float32>(value.X), static_cast<float32>(value.Y), static_cast<float32>(value.Z)};
}

// Profile scale and roll at an estimated distance: linear between keys, clamped outside them.
struct ProfileSample final
{
    float64 Scale = 0.0;
    float64 Roll = 0.0;
};

[[nodiscard]] ProfileSample EvaluateProfile(BorrowedView<const SweepProfileKey> profile, float64 distance) noexcept
{
    const usize count = profile.GetSize();
    if (distance <= profile[0].Distance.Value)
    {
        return ProfileSample{ .Scale = profile[0].Scale, .Roll = profile[0].RollRadians };
    }
    if (distance >= profile[count - 1].Distance.Value)
    {
        return ProfileSample{ .Scale = profile[count - 1].Scale, .Roll = profile[count - 1].RollRadians };
    }
    usize low = 0;
    usize high = count - 1;
    while (high - low > 1)
    {
        const usize middle = low + (high - low) / 2;
        if (profile[middle].Distance.Value <= distance)
        {
            low = middle;
        }
        else
        {
            high = middle;
        }
    }
    const SweepProfileKey& left = profile[low];
    const SweepProfileKey& right = profile[high];
    const float64 t = (distance - left.Distance.Value) / (right.Distance.Value - left.Distance.Value);
    return ProfileSample
    {
        .Scale = math::Lerp(left.Scale, right.Scale, t),
        .Roll = math::Lerp(left.RollRadians, right.RollRadians, t),
    };
}

[[nodiscard]] GeometryStatus
ValidateRequest(const PathView& path, const SweepRequest& request, GeometryDiagnostic* outDiagnosticOrNull) noexcept
{
    if (path.Spans.IsValid() == false || path.Spans.IsEmpty() == true)
    {
        return Fail(outDiagnosticOrNull, GeometryStatus::InvalidArgument, 0, "Path.Spans");
    }
    const bool hasKnownEnums =
        (request.Section == SweepSection::Ribbon || request.Section == SweepSection::Tube) &&
        (request.Caps == SweepCaps::None || request.Caps == SweepCaps::Flat) &&
        (request.Winding == SweepWinding::CounterClockwise || request.Winding == SweepWinding::Clockwise);
    if (hasKnownEnums == false)
    {
        return Fail(outDiagnosticOrNull, GeometryStatus::InvalidArgument, 0, "Section");
    }
    if (request.Caps == SweepCaps::Flat && (request.Section == SweepSection::Ribbon || path.IsClosed == true))
    {
        return Fail(outDiagnosticOrNull, GeometryStatus::InvalidArgument, 0, "Caps");
    }
    if (math::IsFinite(request.LongitudinalError) == false || math::IsFinite(request.CrossSectionError) == false)
    {
        return Fail(outDiagnosticOrNull, GeometryStatus::NonFiniteInput, 0, "LongitudinalError");
    }
    if (request.LongitudinalError <= 0.0 || request.CrossSectionError <= 0.0)
    {
        return Fail(outDiagnosticOrNull, GeometryStatus::InvalidArgument, 0, "LongitudinalError");
    }
    if (request.Profile.IsValid() == false || request.Profile.IsEmpty() == true)
    {
        return Fail(outDiagnosticOrNull, GeometryStatus::InvalidArgument, 0, "Profile");
    }
    for (usize i = 0; i < request.Profile.GetSize(); ++i)
    {
        const SweepProfileKey& key = request.Profile[i];
        if (math::IsFinite(key.Distance.Value) == false || math::IsFinite(key.Scale) == false ||
            math::IsFinite(key.RollRadians) == false)
        {
            return Fail(outDiagnosticOrNull, GeometryStatus::NonFiniteInput, i, "Profile");
        }
        // Zero width is a deliberate topology split, never something to normalize away.
        if (key.Scale <= 0.0 || (i > 0 && key.Distance.Value <= request.Profile[i - 1].Distance.Value))
        {
            return Fail(outDiagnosticOrNull, GeometryStatus::InvalidArgument, i, "Profile");
        }
    }
    return GeometryStatus::Success;
}

[[nodiscard]] GeometryStatus ValidateFrames(const PathView& path,
                                            const DistanceMapView& map,
                                            const FramesView& frames,
                                            GeometryDiagnostic* outDiagnosticOrNull) noexcept
{
    if (map.Leaves.IsEmpty() == true || frames.Samples.IsEmpty() == true)
    {
        return Fail(outDiagnosticOrNull, GeometryStatus::MissingCache, 0, "Frames");
    }
    const usize spanCount = path.Spans.GetSize();
    if (map.PathRevision != path.Revision || map.PathSpanCount != spanCount || frames.PathRevision != path.Revision ||
        frames.PathSpanCount != spanCount)
    {
        return Fail(outDiagnosticOrNull, GeometryStatus::StaleRevision, 0, "Frames");
    }
    const bool hasSingleRun = frames.Runs.GetSize() == 1 && frames.Runs[0].Start.SpanIndex == 0 &&
                              frames.Runs[0].Start.U == 0.0 && frames.Runs[0].End.SpanIndex == spanCount - 1 &&
                              frames.Runs[0].End.U == 1.0;
    if (hasSingleRun == false || (path.IsClosed == true && frames.IsPeriodic == false))
    {
        return Fail(outDiagnosticOrNull, GeometryStatus::DegenerateFrame, 0, "Frames.Runs");
    }
    for (usize joint = 0; joint < path.Joints.GetSize(); ++joint)
    {
        if (path.Joints[joint].IsG1 == false)
        {
            // A corner needs an authored miter or a split sweep; it is not smoothed implicitly.
            return Fail(outDiagnosticOrNull,
                        GeometryStatus::DegenerateFrame,
                        path.Spans[(joint + 1) % spanCount].SourceElement,
                        "Path.Joints");
        }
    }
    return GeometryStatus::Success;
}

// Mutable state of one TryPlanSweepMesh call.
struct Planner final
{
    const PathView& Path;
    const DistanceMapView& Map;
    const FramesView& Frames;
    const SweepRequest& Request;
    GeometryDiagnostic* DiagnosticOrNull = nullptr;
    core::Array<SweepRing>& Rings;
    usize SegmentCount = 1;
    usize MaxLevel = 0;
    usize SubdivisionCount = 0;
    float64 MaxError = 0.0;
    usize RiskCount = 0;
    float64 MaxBendRatio = 0.0;
};

[[nodiscard]] GeometryStatus TryMakeRing(Planner& inoutPlanner, SegmentParameter parameter, SweepRing& outRing) noexcept
{
    DistanceBound distance;
    GeometryStatus status = TryBoundDistance(inoutPlanner.Path, inoutPlanner.Map, parameter, distance);
    if (status != GeometryStatus::Success)
    {
        return Fail(inoutPlanner.DiagnosticOrNull, status, 0, "Map");
    }
    const ProfileSample profile = EvaluateProfile(inoutPlanner.Request.Profile, distance.Estimate);
    TransportedFrame frame;
    status = TryEvaluateFrame(inoutPlanner.Path, inoutPlanner.Frames, parameter, profile.Roll, frame);
    if (status != GeometryStatus::Success)
    {
        return Fail(inoutPlanner.DiagnosticOrNull,
                    status == GeometryStatus::Degenerate ? GeometryStatus::DegenerateFrame : status,
                    inoutPlanner.Path.Spans[parameter.SpanIndex].SourceElement,
                    "Frames");
    }
    outRing = SweepRing
    {
        .Parameter = parameter,
        .Distance = distance.Estimate,
        .Center = internal::ToVector3d(frame.Position),
        .Tangent = internal::ToVector3d(frame.Tangent),
        .Normal = internal::ToVector3d(frame.Normal),
        .Binormal = internal::ToVector3d(frame.Binormal),
        .Scale = profile.Scale,
    };
    return GeometryStatus::Success;
}

[[nodiscard]] GeometryStatus TryAddRing(Planner& inoutPlanner, const SweepRing& ring) noexcept
{
    const usize ringCount = inoutPlanner.Rings.GetSize() + 1;
    if (ringCount > inoutPlanner.Request.Limits.MaxLeaves ||
        ringCount * sizeof(SweepRing) > inoutPlanner.Request.Limits.MaxPreparedBytes)
    {
        return Fail(inoutPlanner.DiagnosticOrNull,
                    GeometryStatus::BudgetExceeded,
                    inoutPlanner.Path.Spans[ring.Parameter.SpanIndex].SourceElement,
                    ringCount > inoutPlanner.Request.Limits.MaxLeaves ? "Limits.MaxLeaves" : "Limits.MaxPreparedBytes");
    }
    // Every emitted position is center + offset with |offset| <= half extent x scale per component.
    const float64 extent = GetHalfExtent(inoutPlanner.Request.Section) * ring.Scale;
    const float64 reach =
        math::Max(math::Abs(ring.Center.X), math::Max(math::Abs(ring.Center.Y), math::Abs(ring.Center.Z))) + extent;
    if (math::IsFinite(reach) == false || reach > FLOAT32_LARGEST)
    {
        return Fail(inoutPlanner.DiagnosticOrNull, GeometryStatus::OutOfRange, 0, "Profile.Scale");
    }
    // Local self-intersection risk: half extent times curvature |C' x C''| / |C'|^3.
    const internal::SpanSample64 sample =
        internal::EvaluateSpan64(inoutPlanner.Path.Spans[ring.Parameter.SpanIndex], ring.Parameter.U);
    const float64 speed = math::Length(sample.FirstDerivative);
    if (speed > 0.0)
    {
        const float64 curvature =
            math::Length(math::Cross(sample.FirstDerivative, sample.SecondDerivative)) / (speed * speed * speed);
        const float64 ratio = extent * curvature;
        if (math::IsFinite(ratio) == true)
        {
            inoutPlanner.MaxBendRatio = math::Max(inoutPlanner.MaxBendRatio, ratio);
            if (ratio >= 1.0)
            {
                inoutPlanner.RiskCount += 1;
            }
        }
    }
    if (inoutPlanner.Rings.TryAdd(ring) == false)
    {
        return Fail(inoutPlanner.DiagnosticOrNull, GeometryStatus::OutOfMemory, 0, "Rings");
    }
    return GeometryStatus::Success;
}

// Distance of point from the segment from start to end.
[[nodiscard]] float64 ComputeSegmentDistance(const Vector3d& point, const Vector3d& start, const Vector3d& end) noexcept
{
    const Vector3d direction = end - start;
    const float64 squaredLength = math::Dot(direction, direction);
    float64 t = 0.0;
    if (squaredLength > 0.0)
    {
        t = math::Clamp(math::Dot(point - start, direction) / squaredLength, 0.0, 1.0);
    }
    return math::Length(point - (start + direction * t));
}

[[nodiscard]] Vector3d GetMidpoint(const internal::EnclosureBox& box) noexcept
{
    return Vector3d{(box.X.Lower + box.X.Upper) * 0.5,
                    (box.Y.Lower + box.Y.Upper) * 0.5,
                    (box.Z.Lower + box.Z.Upper) * 0.5};
}

[[nodiscard]] GeometryStatus TryEstimateIntervalError(Planner& inoutPlanner,
                                                      usize spanIndex,
                                                      float64 a,
                                                      float64 b,
                                                      const SweepRing& start,
                                                      const SweepRing& middle,
                                                      const SweepRing& end,
                                                      float64& outError) noexcept
{
    core::StaticArray<internal::EnclosureBox, SPAN_CONTROL_COUNT> controls;
    const GeometryStatus status =
        internal::TryEncloseSubspanControls(inoutPlanner.Path.Spans[spanIndex], a, b, controls);
    if (status != GeometryStatus::Success)
    {
        return Fail(inoutPlanner.DiagnosticOrNull, status, inoutPlanner.Path.Spans[spanIndex].SourceElement, "Spans");
    }
    // The span lies in its control hull, so its distance from the chord is at most the inner controls'.
    const Vector3d chordStart = GetMidpoint(controls[0]);
    const Vector3d chordEnd = GetMidpoint(controls[SPAN_CONTROL_COUNT - 1]);
    float64 error = math::Max(ComputeSegmentDistance(GetMidpoint(controls[1]), chordStart, chordEnd),
                              ComputeSegmentDistance(GetMidpoint(controls[2]), chordStart, chordEnd));
    // Sampled deviation of each section point from the straight edge between the two rings.
    const usize pointCount =
        inoutPlanner.Request.Section == SweepSection::Ribbon ? RIBBON_RING_VERTEX_COUNT : inoutPlanner.SegmentCount;
    for (usize j = 0; j < pointCount; ++j)
    {
        const SweepSection section = inoutPlanner.Request.Section;
        const usize segments = inoutPlanner.SegmentCount;
        const Vector3d startPoint = start.Center + ComputeSectionOffset(start, section, j, segments);
        const Vector3d middlePoint = middle.Center + ComputeSectionOffset(middle, section, j, segments);
        const Vector3d endPoint = end.Center + ComputeSectionOffset(end, section, j, segments);
        error = math::Max(error, math::Length(middlePoint - (startPoint + endPoint) * 0.5));
    }
    outError = error;
    return GeometryStatus::Success;
}

// NOLINTBEGIN(bugprone-easily-swappable-parameters): a value and its dyadic width share one type.
[[nodiscard]] bool IsEvenMultiple(float64 value, float64 width) noexcept
// NOLINTEND(bugprone-easily-swappable-parameters)
{
    const float64 doubleWidth = 2.0 * width;
    return math::Floor(value / doubleWidth) * doubleWidth == value;
}

[[nodiscard]] GeometryStatus TryPlanSpan(Planner& inoutPlanner, usize spanIndex) noexcept
{
    const PathView& path = inoutPlanner.Path;
    const usize lastSpan = path.Spans.GetSize() - 1;
    SweepRing ringA;
    GeometryStatus status = TryMakeRing(inoutPlanner, SegmentParameter{ .SpanIndex = spanIndex, .U = 0.0 }, ringA);
    if (status == GeometryStatus::Success)
    {
        status = TryAddRing(inoutPlanner, ringA);
    }
    float64 a = 0.0;
    float64 width = 1.0;
    usize level = 0;
    while (status == GeometryStatus::Success && a < 1.0)
    {
        const float64 b = a + width;
        inoutPlanner.SubdivisionCount += 1;
        if (inoutPlanner.SubdivisionCount > inoutPlanner.Request.Limits.MaxSubdivisions)
        {
            return Fail(inoutPlanner.DiagnosticOrNull,
                        GeometryStatus::BudgetExceeded,
                        path.Spans[spanIndex].SourceElement,
                        "Limits.MaxSubdivisions");
        }
        const bool isSeam = path.IsClosed == true && spanIndex == lastSpan && b == 1.0;
        SweepRing ringB;
        if (isSeam == true)
        {
            // A closed sweep's last ring copies the first one; only its V coordinate differs.
            ringB = inoutPlanner.Rings[0];
            ringB.Parameter = SegmentParameter{ .SpanIndex = lastSpan, .U = 1.0 };
            ringB.Distance = inoutPlanner.Map.TotalEstimate;
        }
        else
        {
            status = TryMakeRing(inoutPlanner, SegmentParameter{ .SpanIndex = spanIndex, .U = b }, ringB);
        }
        SweepRing ringM;
        if (status == GeometryStatus::Success)
        {
            status = TryMakeRing(inoutPlanner, SegmentParameter{ .SpanIndex = spanIndex, .U = a + width * 0.5 }, ringM);
        }
        float64 error = 0.0;
        if (status == GeometryStatus::Success)
        {
            status = TryEstimateIntervalError(inoutPlanner, spanIndex, a, b, ringA, ringM, ringB, error);
        }
        if (status != GeometryStatus::Success)
        {
            return status;
        }
        if (error <= inoutPlanner.Request.LongitudinalError)
        {
            inoutPlanner.MaxError = math::Max(inoutPlanner.MaxError, error);
            if (b < 1.0 || spanIndex == lastSpan)
            {
                // Interior joints take the next span's outgoing ring instead.
                status = TryAddRing(inoutPlanner, ringB);
            }
            ringA = ringB;
            a = b;
            while (level > 0 && IsEvenMultiple(a, width) == true)
            {
                level -= 1;
                width *= 2.0;
            }
        }
        else if (level < inoutPlanner.MaxLevel)
        {
            level += 1;
            width *= 0.5;
        }
        else
        {
            WriteDiagnostic(inoutPlanner.DiagnosticOrNull,
                            GeometryDiagnostic
                            {
                                .Code = GeometryStatus::BudgetExceeded,
                                .SourceElement = path.Spans[spanIndex].SourceElement,
                                .Property = "Limits.MaxDepth",
                                .RequestedTolerance = inoutPlanner.Request.LongitudinalError,
                                .AchievedBound = error,
                                .AchievedGuarantee = GuaranteeClass::Estimated,
                                .RangeStart = a,
                                .RangeEnd = b,
                            });
            return GeometryStatus::BudgetExceeded;
        }
    }
    return status;
}

// NOLINTBEGIN(bugprone-easily-swappable-parameters): a count and an error output are outputs of one choice.
[[nodiscard]] GeometryStatus TryChooseSegmentCount(const SweepRequest& request,
                                                   usize& outSegmentCount,
                                                   float64& outError,
                                                   GeometryDiagnostic* outDiagnosticOrNull) noexcept
// NOLINTEND(bugprone-easily-swappable-parameters)
{
    if (request.Section == SweepSection::Ribbon)
    {
        outSegmentCount = 1;
        outError = 0.0;
        return GeometryStatus::Success;
    }
    float64 largestScale = 0.0;
    for (usize i = 0; i < request.Profile.GetSize(); ++i)
    {
        // Linear interpolation never exceeds the largest key.
        largestScale = math::Max(largestScale, request.Profile[i].Scale);
    }
    float64 error = 0.0;
    for (usize segments = MIN_TUBE_SEGMENTS; segments <= request.MaxSectionSegments; ++segments)
    {
        // A regular polygon inscribed in a circle of radius r deviates from it by r (1 - cos(pi / n)).
        error = largestScale * (1.0 - math::Cos(math::kPi / static_cast<float64>(segments)));
        if (error <= request.CrossSectionError)
        {
            outSegmentCount = segments;
            outError = error;
            return GeometryStatus::Success;
        }
    }
    WriteDiagnostic(outDiagnosticOrNull,
                    GeometryDiagnostic
                    {
                        .Code = GeometryStatus::BudgetExceeded,
                        .Property = "MaxSectionSegments",
                        .RequestedTolerance = request.CrossSectionError,
                        .AchievedBound = error,
                        .AchievedGuarantee = GuaranteeClass::Estimated,
                    });
    return GeometryStatus::BudgetExceeded;
}

// Vertex and index counts for ringCount rings.
[[nodiscard]] bool TryCountOutput(const SweepRequest& request,
                                  usize ringCount,
                                  usize segmentCount,
                                  usize& outVertexCount,
                                  usize& outIndexCount) noexcept
{
    const usize quadCount = ringCount - 1;
    if (request.Section == SweepSection::Ribbon)
    {
        return TryMultiply(ringCount, RIBBON_RING_VERTEX_COUNT, outVertexCount) == true &&
               TryMultiply(quadCount, QUAD_INDEX_COUNT, outIndexCount) == true;
    }
    usize ringVertexCount = 0;
    usize vertexCount = 0;
    usize quadIndexCount = 0;
    usize indexCount = 0;
    bool isCounted = TryAdd(segmentCount, 1, ringVertexCount) == true &&
                     TryMultiply(ringCount, ringVertexCount, vertexCount) == true &&
                     TryMultiply(quadCount, segmentCount, quadIndexCount) == true &&
                     TryMultiply(quadIndexCount, QUAD_INDEX_COUNT, indexCount) == true;
    if (isCounted == true && request.Caps == SweepCaps::Flat)
    {
        // Each cap: a center plus one vertex per segment, and one triangle per segment.
        usize capVertices = 0;
        usize capIndices = 0;
        isCounted = TryMultiply(ringVertexCount, CAP_COUNT, capVertices) == true &&
                    TryAdd(vertexCount, capVertices, vertexCount) == true &&
                    TryMultiply(segmentCount, TRIANGLE_INDEX_COUNT * CAP_COUNT, capIndices) == true &&
                    TryAdd(indexCount, capIndices, indexCount) == true;
    }
    if (isCounted == true)
    {
        outVertexCount = vertexCount;
        outIndexCount = indexCount;
    }
    return isCounted;
}

void WriteTriangle(BorrowedView<uint32> outIndices,
                   usize& inoutCursor,
                   usize first,
                   usize second,
                   usize third,
                   SweepWinding winding) noexcept
{
    const bool isClockwise = winding == SweepWinding::Clockwise;
    outIndices[inoutCursor] = static_cast<uint32>(first);
    outIndices[inoutCursor + 1] = static_cast<uint32>(isClockwise == true ? third : second);
    outIndices[inoutCursor + 2] = static_cast<uint32>(isClockwise == true ? second : third);
    inoutCursor += TRIANGLE_INDEX_COUNT;
}

void EmitRibbon(const SweepMeshPlanView& plan,
                BorrowedView<SweepVertex> outVertices,
                BorrowedView<uint32> outIndices) noexcept
{
    const usize ringCount = plan.Rings.GetSize();
    for (usize i = 0; i < ringCount; ++i)
    {
        const SweepRing& ring = plan.Rings[i];
        for (usize side = 0; side < RIBBON_RING_VERTEX_COUNT; ++side)
        {
            outVertices[i * RIBBON_RING_VERTEX_COUNT + side] = SweepVertex
            {
                .Position = Narrow(ring.Center + ComputeSectionOffset(ring, SweepSection::Ribbon, side, 1)),
                .Normal = Narrow(ring.Normal),
                .Uv = math::Vector2{static_cast<float32>(side), static_cast<float32>(ring.Distance)},
            };
        }
    }
    usize cursor = 0;
    for (usize i = 0; i + 1 < ringCount; ++i)
    {
        const usize left = i * RIBBON_RING_VERTEX_COUNT;
        const usize right = left + 1;
        const usize nextLeft = left + RIBBON_RING_VERTEX_COUNT;
        const usize nextRight = nextLeft + 1;
        // Counter-clockwise seen from +Normal: (-B, +B) across, Tangent forward.
        WriteTriangle(outIndices, cursor, left, right, nextRight, plan.Winding);
        WriteTriangle(outIndices, cursor, left, nextRight, nextLeft, plan.Winding);
    }
}

void EmitTube(const SweepMeshPlanView& plan,
              BorrowedView<SweepVertex> outVertices,
              BorrowedView<uint32> outIndices) noexcept
{
    const usize ringCount = plan.Rings.GetSize();
    const usize segments = plan.Report.SectionSegmentCount;
    const usize ringVertexCount = segments + 1;
    for (usize i = 0; i < ringCount; ++i)
    {
        const SweepRing& ring = plan.Rings[i];
        for (usize j = 0; j <= segments; ++j)
        {
            const SectionDirection direction = GetSectionDirection(j, segments);
            outVertices[i * ringVertexCount + j] = SweepVertex
            {
                .Position = Narrow(ring.Center + ComputeSectionOffset(ring, SweepSection::Tube, j, segments)),
                .Normal = Narrow(ring.Binormal * direction.Cosine + ring.Normal * direction.Sine),
                .Uv = math::Vector2{static_cast<float32>(static_cast<float64>(j) / static_cast<float64>(segments)),
                                    static_cast<float32>(ring.Distance)},
            };
        }
    }
    usize cursor = 0;
    for (usize i = 0; i + 1 < ringCount; ++i)
    {
        for (usize j = 0; j < segments; ++j)
        {
            const usize current = i * ringVertexCount + j;
            const usize next = current + ringVertexCount;
            // Counter-clockwise seen from outside: Tangent then the section direction.
            WriteTriangle(outIndices, cursor, current, next, next + 1, plan.Winding);
            WriteTriangle(outIndices, cursor, current, next + 1, current + 1, plan.Winding);
        }
    }
    if (plan.Caps != SweepCaps::Flat)
    {
        return;
    }
    usize base = ringCount * ringVertexCount;
    for (usize cap = 0; cap < CAP_COUNT; ++cap)
    {
        const bool isStart = cap == 0;
        const SweepRing& ring = isStart == true ? plan.Rings[0] : plan.Rings[ringCount - 1];
        const math::Vector3 normal = Narrow(isStart == true ? -ring.Tangent : ring.Tangent);
        outVertices[base] =
            SweepVertex{ .Position = Narrow(ring.Center), .Normal = normal, .Uv = math::Vector2{0.5F, 0.5F} };
        for (usize j = 0; j < segments; ++j)
        {
            const SectionDirection direction = GetSectionDirection(j, segments);
            outVertices[base + 1 + j] = SweepVertex
            {
                // The same arithmetic as the ring vertex, so the cap edge matches it bitwise.
                .Position = Narrow(ring.Center + ComputeSectionOffset(ring, SweepSection::Tube, j, segments)),
                .Normal = normal,
                .Uv = math::Vector2{static_cast<float32>(0.5 + 0.5 * direction.Cosine),
                                    static_cast<float32>(0.5 + 0.5 * direction.Sine)},
            };
        }
        for (usize j = 0; j < segments; ++j)
        {
            const usize current = base + 1 + j;
            const usize following = base + 1 + (j + 1) % segments;
            // The section angle turns counter-clockwise seen from -Tangent, so the end cap reverses it.
            if (isStart == true)
            {
                WriteTriangle(outIndices, cursor, base, current, following, plan.Winding);
            }
            else
            {
                WriteTriangle(outIndices, cursor, base, following, current, plan.Winding);
            }
        }
        base += ringVertexCount;
    }
}
} // namespace

SweepMeshPlanView SweepMeshPlan::GetView() const noexcept
{
    return SweepMeshPlanView
    {
        .Rings = BorrowedView<const SweepRing>(mRings.GetData(), mRings.GetSize()),
        .PathRevision = mPathRevision,
        .PathSpanCount = mPathSpanCount,
        .Section = mSection,
        .Caps = mCaps,
        .Winding = mWinding,
        .IsClosed = mIsClosed,
        .Report = mReport,
    };
}

bool SweepMeshPlan::IsEmpty() const noexcept
{
    return mRings.IsEmpty();
}

GeometryStatus TryPlanSweepMesh(const PathView& path,
                                const DistanceMapView& map,
                                const FramesView& frames,
                                const SweepRequest& request,
                                SweepMeshPlan& outPlan,
                                GeometryDiagnostic* outDiagnosticOrNull) noexcept
{
    GeometryStatus status = ValidateRequest(path, request, outDiagnosticOrNull);
    if (status == GeometryStatus::Success)
    {
        status = ValidateFrames(path, map, frames, outDiagnosticOrNull);
    }
    usize segmentCount = 1;
    float64 crossSectionError = 0.0;
    if (status == GeometryStatus::Success)
    {
        status = TryChooseSegmentCount(request, segmentCount, crossSectionError, outDiagnosticOrNull);
    }
    if (status != GeometryStatus::Success)
    {
        return status;
    }

    SweepMeshPlan candidate;
    Planner planner
    {
        .Path = path,
        .Map = map,
        .Frames = frames,
        .Request = request,
        .DiagnosticOrNull = outDiagnosticOrNull,
        .Rings = candidate.mRings,
        .SegmentCount = segmentCount,
        .MaxLevel = math::Min(request.Limits.MaxDepth, MAX_DYADIC_LEVEL),
    };
    for (usize spanIndex = 0; spanIndex < path.Spans.GetSize(); ++spanIndex)
    {
        status = TryPlanSpan(planner, spanIndex);
        if (status != GeometryStatus::Success)
        {
            return status;
        }
    }

    usize vertexCount = 0;
    usize indexCount = 0;
    if (TryCountOutput(request, candidate.mRings.GetSize(), segmentCount, vertexCount, indexCount) == false ||
        static_cast<uint64>(vertexCount) > MAX_INDEXED_VERTEX_COUNT)
    {
        return Fail(outDiagnosticOrNull, GeometryStatus::OutOfRange, 0, "MaxVertexCount");
    }
    if (vertexCount > request.MaxVertexCount || indexCount > request.MaxIndexCount)
    {
        WriteDiagnostic(
            outDiagnosticOrNull,
            GeometryDiagnostic
            {
                .Code = GeometryStatus::BudgetExceeded,
                .Property = vertexCount > request.MaxVertexCount ? "MaxVertexCount" : "MaxIndexCount",
                .RequestedTolerance = static_cast<float64>(
                    vertexCount > request.MaxVertexCount ? request.MaxVertexCount : request.MaxIndexCount),
                .AchievedBound = static_cast<float64>(vertexCount > request.MaxVertexCount ? vertexCount : indexCount),
                .AchievedGuarantee = GuaranteeClass::Exact,
            });
        return GeometryStatus::BudgetExceeded;
    }
    candidate.mPathRevision = path.Revision;
    candidate.mPathSpanCount = path.Spans.GetSize();
    candidate.mSection = request.Section;
    candidate.mCaps = request.Caps;
    candidate.mWinding = request.Winding;
    candidate.mIsClosed = path.IsClosed;
    candidate.mReport = SweepMeshReport
    {
        .RingCount = candidate.mRings.GetSize(),
        .SectionSegmentCount = segmentCount,
        .VertexCount = vertexCount,
        .IndexCount = indexCount,
        .LongitudinalErrorEstimate = planner.MaxError,
        .CrossSectionErrorEstimate = crossSectionError,
        .SelfIntersectionRiskCount = planner.RiskCount,
        .MaxBendRatio = planner.MaxBendRatio,
    };
    outPlan = Move(candidate);
    return GeometryStatus::Success;
}

GeometryStatus TryEmitSweepMesh(const PathView& path,
                                const SweepMeshPlanView& plan,
                                BorrowedView<SweepVertex> outVertices,
                                BorrowedView<uint32> outIndices) noexcept
{
    if (plan.Rings.IsEmpty() == true)
    {
        return GeometryStatus::MissingCache;
    }
    if (plan.PathRevision != path.Revision || plan.PathSpanCount != path.Spans.GetSize())
    {
        return GeometryStatus::StaleRevision;
    }
    const bool hasVertexRoom = outVertices.IsValid() == true && outVertices.GetSize() >= plan.Report.VertexCount;
    const bool hasIndexRoom =
        plan.Report.IndexCount == 0 || (outIndices.IsValid() == true && outIndices.GetSize() >= plan.Report.IndexCount);
    if (hasVertexRoom == false || hasIndexRoom == false)
    {
        return GeometryStatus::CapacityExceeded;
    }
    if (plan.Section == SweepSection::Ribbon)
    {
        EmitRibbon(plan, outVertices, outIndices);
    }
    else
    {
        EmitTube(plan, outVertices, outIndices);
    }
    return GeometryStatus::Success;
}
} // namespace ludus::foundation::geometry
