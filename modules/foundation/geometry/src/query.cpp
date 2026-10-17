// Strict and approximate closest-point projection (see query.hpp).
//
// Thanks to Philip J. Schneider, "Solving the Nearest-Point-on-Curve Problem",
// Graphics Gems (1990), chapter XI.7, printed pages 607-611: the stationary
// equation is generally degree five, so every candidate and both endpoints must
// be considered. This file adapts that requirement to a branch-and-bound over
// control-hull boxes (the convex-hull property of Bezier spans) with
// FoundationMath's outward-rounded enclosures; it does not use the chapter's
// root finder.

#include <ludus/foundation/geometry/query.hpp>

#include "internal/arc_length.hpp"
#include "internal/span_math.hpp"

#include <ludus/foundation/math/enclosure.hpp>
#include <ludus/foundation/math/scalar.hpp>

namespace ludus::foundation::geometry
{
namespace internal
{
/// Grants the projection search access to ProjectionScratch storage.
struct ProjectionAccess final
{
    [[nodiscard]] static core::Array<ProjectionInterval>& GetIntervals(ProjectionScratch& inoutScratch) noexcept
    {
        return inoutScratch.mIntervals;
    }
    [[nodiscard]] static core::Array<ProjectionCandidate>& GetCandidates(ProjectionScratch& inoutScratch) noexcept
    {
        return inoutScratch.mCandidates;
    }
};
} // namespace internal

namespace
{
using core::Array;
using math::Enclosure;
using math::MakePointEnclosure;
using math::Vector3d;

// The main search closes to this fraction of the gap, so every global minimizer lies in an interval that the
// tie search examines; candidates are then accepted within the full gap.
constexpr float64 TIE_GAP_FRACTION = 0.5;

// Whether left is split before right: smaller lower bound, then earlier span and parameter.
[[nodiscard]] bool IsSearchedBefore(const ProjectionInterval& left, const ProjectionInterval& right) noexcept
{
    if (left.LowerBound != right.LowerBound)
    {
        return left.LowerBound < right.LowerBound;
    }
    if (left.SpanIndex != right.SpanIndex)
    {
        return left.SpanIndex < right.SpanIndex;
    }
    return left.U0 < right.U0;
}

[[nodiscard]] bool IsEarlier(const SegmentParameter& left, const SegmentParameter& right) noexcept
{
    return left.SpanIndex != right.SpanIndex ? left.SpanIndex < right.SpanIndex : left.U < right.U;
}

void SwapIntervals(ProjectionInterval& inoutLeft, ProjectionInterval& inoutRight) noexcept
{
    const ProjectionInterval held = inoutLeft;
    inoutLeft = inoutRight;
    inoutRight = held;
}

void PushInterval(Array<ProjectionInterval>& inoutHeap, const ProjectionInterval& interval) noexcept
{
    // Capacity was checked against the work limit before the search, so this never allocates or fails.
    const bool isAdded = inoutHeap.TryAdd(interval);
    LUDUS_ASSERT(isAdded == true);
    (void)isAdded;
    usize position = inoutHeap.GetSize() - 1;
    while (position > 0)
    {
        const usize parent = (position - 1) / 2;
        if (IsSearchedBefore(inoutHeap[position], inoutHeap[parent]) == false)
        {
            return;
        }
        SwapIntervals(inoutHeap[position], inoutHeap[parent]);
        position = parent;
    }
}

[[nodiscard]] ProjectionInterval PopInterval(Array<ProjectionInterval>& inoutHeap) noexcept
{
    const ProjectionInterval first = inoutHeap[0];
    SwapIntervals(inoutHeap[0], inoutHeap[inoutHeap.GetSize() - 1]);
    inoutHeap.RemoveLast();
    const usize count = inoutHeap.GetSize();
    usize position = 0;
    while (true)
    {
        const usize left = 2 * position + 1;
        if (left >= count)
        {
            break;
        }
        const usize right = left + 1;
        usize smallest = left;
        if (right < count && IsSearchedBefore(inoutHeap[right], inoutHeap[left]) == true)
        {
            smallest = right;
        }
        if (IsSearchedBefore(inoutHeap[smallest], inoutHeap[position]) == false)
        {
            break;
        }
        SwapIntervals(inoutHeap[position], inoutHeap[smallest]);
        position = smallest;
    }
    return first;
}

// Upper bound of upper - lower, never negative.
[[nodiscard]] GeometryStatus TryBoundGap(float64 upper, float64 lower, float64& outGap) noexcept
{
    Enclosure difference;
    const math::MathStatus status =
        math::TrySubtractOutward(MakePointEnclosure(upper), MakePointEnclosure(lower), difference);
    if (status != math::MathStatus::Success)
    {
        return internal::ToGeometryStatus(status);
    }
    outGap = math::Max(0.0, difference.Upper);
    return GeometryStatus::Success;
}

// Mutable state of one projection.
struct Search final
{
    const PathView& Path;
    internal::EnclosureBox Query;
    Array<ProjectionInterval>& Heap;
    Array<ProjectionCandidate>& Candidates;
    float64 GapTolerance = 0.0;
    float64 Best = 0.0;
    bool HasBest = false;
};

[[nodiscard]] GeometryStatus TryAddCandidate(Search& inoutSearch, usize spanIndex, float64 u) noexcept
{
    internal::EnclosureBox point;
    GeometryStatus status = internal::TryEnclosePoint(inoutSearch.Path.Spans[spanIndex], u, point);
    Enclosure distance;
    if (status == GeometryStatus::Success)
    {
        status = internal::TryBoundBoxDistance(inoutSearch.Query, point, distance);
    }
    if (status != GeometryStatus::Success)
    {
        return status;
    }
    const bool isAdded = inoutSearch.Candidates.TryAdd(ProjectionCandidate
    {
        .Parameter = SegmentParameter{ .SpanIndex = spanIndex, .U = u },
        .Distance = distance.Upper,
    });
    LUDUS_ASSERT(isAdded == true);
    (void)isAdded;
    if (inoutSearch.HasBest == false || distance.Upper < inoutSearch.Best)
    {
        inoutSearch.Best = distance.Upper;
        inoutSearch.HasBest = true;
    }
    return GeometryStatus::Success;
}

[[nodiscard]] GeometryStatus TryAddInterval(Search& inoutSearch, usize spanIndex, float64 u0, float64 u1) noexcept
{
    internal::EnclosureBox hull;
    GeometryStatus status = internal::TryEncloseSubspanHull(inoutSearch.Path.Spans[spanIndex], u0, u1, hull);
    Enclosure distance;
    if (status == GeometryStatus::Success)
    {
        status = internal::TryBoundBoxDistance(inoutSearch.Query, hull, distance);
    }
    if (status != GeometryStatus::Success)
    {
        return status;
    }
    // Keep intervals that may hold a point within the gap of the best one, so earlier ties survive.
    if (distance.Lower <= inoutSearch.Best + inoutSearch.GapTolerance)
    {
        PushInterval(inoutSearch.Heap,
                     ProjectionInterval{ .SpanIndex = spanIndex, .U0 = u0, .U1 = u1, .LowerBound = distance.Lower });
    }
    return GeometryStatus::Success;
}

// Whether a distance upper bound is within the gap of the global lower bound.
// NOLINTBEGIN(bugprone-easily-swappable-parameters): distance bounds share one type.
[[nodiscard]] GeometryStatus
TryIsAcceptable(float64 distance, float64 lowerBound, float64 gapTolerance, bool& outIsAcceptable) noexcept
// NOLINTEND(bugprone-easily-swappable-parameters)
{
    float64 gap = 0.0;
    const GeometryStatus status = TryBoundGap(distance, lowerBound, gap);
    outIsAcceptable = status == GeometryStatus::Success && gap <= gapTolerance;
    return status;
}

// Whether an interval's lower bound leaves room for a point within half the gap (rounded toward keeping it).
// The half-gap margin makes the tie search terminate: such an interval either shrinks until its candidates
// fall within the full gap or its bound rises above the half gap.
// NOLINTBEGIN(bugprone-easily-swappable-parameters): distance bounds share one type.
[[nodiscard]] bool
MayHoldAcceptable(const ProjectionInterval& interval, float64 lowerBound, float64 gapTolerance) noexcept
// NOLINTEND(bugprone-easily-swappable-parameters)
{
    Enclosure difference;
    const math::MathStatus status =
        math::TrySubtractOutward(MakePointEnclosure(interval.LowerBound), MakePointEnclosure(lowerBound), difference);
    return status != math::MathStatus::Success || difference.Lower <= gapTolerance * TIE_GAP_FRACTION;
}

// Whether a candidate lies in [U0, U1) of an interval, or is the terminal endpoint closing it. The end of any
// other interval is the start of the next one, so it is examined there.
[[nodiscard]] bool
IsInside(const SegmentParameter& parameter, const ProjectionInterval& interval, bool isTerminal) noexcept
{
    return parameter.SpanIndex == interval.SpanIndex && interval.U0 <= parameter.U &&
           (parameter.U < interval.U1 || (isTerminal == true && parameter.U == interval.U1));
}

// After the gap is met, every acceptable point lies in a live interval whose lower bound allows it. Split the
// earliest such interval until it holds an acceptable candidate or its bound rules it out, so ties resolve to
// the earliest minimum rather than to the first minimum the search happened to evaluate.
// NOLINTBEGIN(bugprone-easily-swappable-parameters): a work limit and a counter share one type.
[[nodiscard]] GeometryStatus TryResolveEarliestTie(Search& inoutSearch,
                                                   float64 lowerBound,
                                                   usize maxIterations,
                                                   bool isStrict,
                                                   usize& inoutIterations) noexcept
// NOLINTEND(bugprone-easily-swappable-parameters)
{
    Array<ProjectionInterval>& live = inoutSearch.Heap;
    const float64 gapTolerance = inoutSearch.GapTolerance;
    while (true)
    {
        usize next = live.GetSize();
        for (usize i = 0; i < live.GetSize(); ++i)
        {
            if (MayHoldAcceptable(live[i], lowerBound, gapTolerance) == false)
            {
                continue;
            }
            const SegmentParameter start{ .SpanIndex = live[i].SpanIndex, .U = live[i].U0 };
            if (next == live.GetSize() ||
                IsEarlier(start, SegmentParameter{ .SpanIndex = live[next].SpanIndex, .U = live[next].U0 }) == true)
            {
                next = i;
            }
        }
        if (next == live.GetSize())
        {
            return GeometryStatus::Success;
        }
        const ProjectionInterval interval = live[next];
        const bool isTerminal = inoutSearch.Path.IsClosed == false &&
                                interval.SpanIndex + 1 == inoutSearch.Path.Spans.GetSize() && interval.U1 == 1.0;
        for (usize i = 0; i < inoutSearch.Candidates.GetSize(); ++i)
        {
            const ProjectionCandidate& candidate = inoutSearch.Candidates[i];
            if (IsInside(candidate.Parameter, interval, isTerminal) == false)
            {
                continue;
            }
            bool isAcceptable = false;
            const GeometryStatus status = TryIsAcceptable(candidate.Distance, lowerBound, gapTolerance, isAcceptable);
            if (status != GeometryStatus::Success || isAcceptable == true)
            {
                return status;
            }
        }
        if (inoutIterations == maxIterations)
        {
            return isStrict == true ? GeometryStatus::BudgetExceeded : GeometryStatus::Success;
        }
        const float64 middle = (interval.U0 + interval.U1) * 0.5;
        if ((interval.U0 < middle && middle < interval.U1) == false)
        {
            return isStrict == true ? GeometryStatus::UnsupportedPrecision : GeometryStatus::Success;
        }
        SwapIntervals(live[next], live[live.GetSize() - 1]);
        live.RemoveLast();
        inoutIterations += 1;
        GeometryStatus status = TryAddCandidate(inoutSearch, interval.SpanIndex, middle);
        if (status == GeometryStatus::Success)
        {
            status = TryAddInterval(inoutSearch, interval.SpanIndex, interval.U0, middle);
        }
        if (status == GeometryStatus::Success)
        {
            status = TryAddInterval(inoutSearch, interval.SpanIndex, middle, interval.U1);
        }
        if (status != GeometryStatus::Success)
        {
            return status;
        }
    }
}

[[nodiscard]] GeometryStatus
TryEncloseQuery(const PathView& path, const Vector3d& point, internal::EnclosureBox& outBox) noexcept
{
    internal::EnclosureBox box;
    math::MathStatus status =
        math::TrySubtractOutward(MakePointEnclosure(point.X), MakePointEnclosure(path.Origin.X), box.X);
    if (status == math::MathStatus::Success)
    {
        status = math::TrySubtractOutward(MakePointEnclosure(point.Y), MakePointEnclosure(path.Origin.Y), box.Y);
    }
    if (status == math::MathStatus::Success)
    {
        status = math::TrySubtractOutward(MakePointEnclosure(point.Z), MakePointEnclosure(path.Origin.Z), box.Z);
    }
    if (status != math::MathStatus::Success)
    {
        return internal::ToGeometryStatus(status);
    }
    outBox = box;
    return GeometryStatus::Success;
}

[[nodiscard]] GeometryStatus RunProjection(const PathView& path,
                                           const ProjectionQuery& query,
                                           bool isStrict,
                                           ProjectionScratch& inoutScratch,
                                           ProjectionResult& outResult) noexcept
{
    if (path.Spans.IsValid() == false || path.Spans.IsEmpty() == true)
    {
        return GeometryStatus::InvalidArgument;
    }
    if (math::IsFinite(query.Point) == false || math::IsFinite(path.Origin) == false)
    {
        return GeometryStatus::NonFiniteInput;
    }
    usize capacity = 0;
    GeometryStatus status = ComputeProjectionScratchCapacity(path, query.MaxIterations, capacity);
    if (status != GeometryStatus::Success)
    {
        return status;
    }
    if (inoutScratch.GetCapacity() < capacity)
    {
        return GeometryStatus::CapacityExceeded;
    }
    internal::EnclosureBox queryBox;
    status = TryEncloseQuery(path, query.Point, queryBox);
    if (status != GeometryStatus::Success)
    {
        return status;
    }
    Array<ProjectionInterval>& heap = internal::ProjectionAccess::GetIntervals(inoutScratch);
    Array<ProjectionCandidate>& candidates = internal::ProjectionAccess::GetCandidates(inoutScratch);
    heap.Clear();
    candidates.Clear();
    Search search
    {
        .Path = path,
        .Query = queryBox,
        .Heap = heap,
        .Candidates = candidates,
        .GapTolerance = path.Tolerance.ProjectionGap,
    };

    const usize spanCount = path.Spans.GetSize();
    for (usize spanIndex = 0; spanIndex < spanCount && status == GeometryStatus::Success; ++spanIndex)
    {
        status = TryAddCandidate(search, spanIndex, 0.0);
    }
    if (status == GeometryStatus::Success && path.IsClosed == false)
    {
        // The terminal endpoint of an open path is named by the last span at u = 1.
        status = TryAddCandidate(search, spanCount - 1, 1.0);
    }
    for (usize spanIndex = 0; spanIndex < spanCount && status == GeometryStatus::Success; ++spanIndex)
    {
        status = TryAddInterval(search, spanIndex, 0.0, 1.0);
    }

    const float64 gapTolerance = path.Tolerance.ProjectionGap;
    usize iterations = 0;
    bool isGapMet = false;
    float64 lowerBound = 0.0;
    while (status == GeometryStatus::Success)
    {
        lowerBound = heap.IsEmpty() == true ? search.Best : math::Min(heap[0].LowerBound, search.Best);
        float64 gap = 0.0;
        status = TryBoundGap(search.Best, lowerBound, gap);
        if (status != GeometryStatus::Success)
        {
            break;
        }
        if (gap <= gapTolerance * TIE_GAP_FRACTION)
        {
            isGapMet = true;
            break;
        }
        if (iterations == query.MaxIterations)
        {
            if (isStrict == true)
            {
                status = GeometryStatus::BudgetExceeded;
            }
            break;
        }
        const ProjectionInterval interval = heap[0];
        const float64 middle = (interval.U0 + interval.U1) * 0.5;
        if ((interval.U0 < middle && middle < interval.U1) == false)
        {
            // The interval is at float64 parameter resolution; its lower bound cannot improve.
            if (isStrict == true)
            {
                status = GeometryStatus::UnsupportedPrecision;
            }
            break;
        }
        (void)PopInterval(heap);
        iterations += 1;
        status = TryAddCandidate(search, interval.SpanIndex, middle);
        if (status == GeometryStatus::Success)
        {
            status = TryAddInterval(search, interval.SpanIndex, interval.U0, middle);
        }
        if (status == GeometryStatus::Success)
        {
            status = TryAddInterval(search, interval.SpanIndex, middle, interval.U1);
        }
    }
    if (status == GeometryStatus::Success && isGapMet == true)
    {
        status = TryResolveEarliestTie(search, lowerBound, query.MaxIterations, isStrict, iterations);
    }
    if (status != GeometryStatus::Success)
    {
        return status;
    }

    // Earliest acceptable candidate: within the gap of the global lower bound, or the best one found.
    usize chosen = candidates.GetSize();
    for (usize i = 0; i < candidates.GetSize() && status == GeometryStatus::Success; ++i)
    {
        const ProjectionCandidate& candidate = candidates[i];
        bool isAcceptable = candidate.Distance == search.Best;
        if (isGapMet == true && isAcceptable == false)
        {
            float64 gap = 0.0;
            status = TryBoundGap(candidate.Distance, lowerBound, gap);
            isAcceptable = gap <= gapTolerance;
        }
        if (isAcceptable == true &&
            (chosen == candidates.GetSize() || IsEarlier(candidate.Parameter, candidates[chosen].Parameter) == true))
        {
            chosen = i;
        }
    }
    if (status != GeometryStatus::Success)
    {
        return status;
    }
    LUDUS_ASSERT(chosen < candidates.GetSize());
    const ProjectionCandidate& candidate = candidates[chosen];
    ProjectionResult result
    {
        .Parameter = candidate.Parameter,
        .Distance = candidate.Distance,
        .LowerBound = lowerBound,
        .IsGapMet = isGapMet,
        .Guarantee = isStrict == true ? GuaranteeClass::Certified : GuaranteeClass::Approximate,
        .IterationCount = iterations,
    };
    status = TryBoundGap(candidate.Distance, lowerBound, result.Gap);
    if (status == GeometryStatus::Success)
    {
        status = TryEvaluatePath(path, candidate.Parameter, result.Position);
    }
    if (status != GeometryStatus::Success)
    {
        return status;
    }
    outResult = result;
    return GeometryStatus::Success;
}
} // namespace

GeometryStatus ProjectionScratch::TryReserve(usize capacity) noexcept
{
    if (mIntervals.TryEnsureCapacity(capacity) == false || mCandidates.TryEnsureCapacity(capacity) == false)
    {
        return GeometryStatus::OutOfMemory;
    }
    return GeometryStatus::Success;
}

usize ProjectionScratch::GetCapacity() const noexcept
{
    return math::Min(mIntervals.GetCapacity(), mCandidates.GetCapacity());
}

GeometryStatus ComputeProjectionScratchCapacity(const PathView& path, usize maxIterations, usize& outCapacity) noexcept
{
    const usize fixed = path.Spans.GetSize() + 1;
    if (fixed == 0 || maxIterations > static_cast<usize>(-1) - fixed)
    {
        return GeometryStatus::OutOfRange;
    }
    outCapacity = fixed + maxIterations;
    return GeometryStatus::Success;
}

GeometryStatus TryProjectPoint(const PathView& path,
                               const ProjectionQuery& query,
                               ProjectionScratch& inoutScratch,
                               ProjectionResult& outResult) noexcept
{
    return RunProjection(path, query, true, inoutScratch, outResult);
}

GeometryStatus ProjectPointApproximate(const PathView& path,
                                       const ProjectionQuery& query,
                                       ProjectionScratch& inoutScratch,
                                       ProjectionResult& outResult) noexcept
{
    return RunProjection(path, query, false, inoutScratch, outResult);
}
} // namespace ludus::foundation::geometry
