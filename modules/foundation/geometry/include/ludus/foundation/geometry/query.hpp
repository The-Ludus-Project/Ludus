#pragma once

// Closest-point projection onto prepared paths (curves and surfaces
// architecture, "Closest point and spatial queries", phase G3).
//
// The squared distance from a point to a cubic is a degree-six polynomial whose
// stationary equation Dot(C(u) - Q, C'(u)) = 0 is generally degree five, so one
// Newton solve can miss the global minimum. Philip J. Schneider, "Solving the
// Nearest-Point-on-Curve Problem", Graphics Gems, chapter XI.7, printed pages
// 607-611, motivates covering every candidate and both endpoints (reviewed in
// docs/architecture/curves-surfaces-gems-review.md). Departure: instead of
// isolating Bernstein roots, Ludus runs a bounded branch-and-bound over
// outward-rounded control-hull boxes, which yields a Certified global distance
// gap.

#include <ludus/foundation/base/core.h>

#include <ludus/foundation/containers/array.hpp>
#include <ludus/foundation/geometry/path.hpp>
#include <ludus/foundation/geometry/status.hpp>
#include <ludus/foundation/math/vector.hpp>

namespace ludus::foundation::geometry
{
namespace internal
{
struct ProjectionAccess;
} // namespace internal

/// @brief A point projection request.
struct ProjectionQuery final
{
    math::Vector3d Point{};  ///< Query point in asset coordinates; the path origin is subtracted with outward rounding.
    usize MaxIterations = 0; ///< Interval splits allowed; zero allows only the initial span and endpoint bounds.
};

/// @brief Closest point found by a projection, with its global distance enclosure.
///
/// Distance and LowerBound bracket the problem: the true distance from the
/// query to the returned parameter's exact point is at most Distance, and the
/// global minimum over the whole path is at least LowerBound.
struct ProjectionResult final
{
    SegmentParameter
        Parameter{};          ///< Chosen parameter; the terminal endpoint of an open path is the last span at u = 1.
    math::Vector3 Position{}; ///< Chunk-relative position at Parameter (Rounded; endpoints Exact).
    float64 Distance = 0.0;   ///< Upper bound of the distance from the query to the exact point, asset units.
    float64 LowerBound = 0.0; ///< Lower bound of the global minimum distance, asset units.
    float64 Gap = 0.0;        ///< Distance - LowerBound rounded up; at most ProjectionGap when IsGapMet.
    bool IsGapMet = false;    ///< Whether Gap <= the path's ToleranceProfile::ProjectionGap.
    GuaranteeClass Guarantee = GuaranteeClass::Certified; ///< Certified for TryProjectPoint, Approximate otherwise.
    usize IterationCount = 0;                             ///< Interval splits performed.
};

/// @brief Parameter interval waiting in a projection search; scratch storage only.
struct ProjectionInterval final
{
    usize SpanIndex = 0;      ///< Prepared span.
    float64 U0 = 0.0;         ///< Start parameter; a dyadic rational.
    float64 U1 = 0.0;         ///< End parameter.
    float64 LowerBound = 0.0; ///< Lower bound of the distance from the query to the span over [U0, U1].
};

/// @brief Evaluated projection candidate; scratch storage only.
struct ProjectionCandidate final
{
    SegmentParameter Parameter{}; ///< Candidate parameter.
    float64 Distance = 0.0;       ///< Upper bound of its distance to the query.
};

/// @brief Caller-owned, reusable search storage, so a projection never allocates.
///
/// Reserve it once with TryReserve; queries only use reserved capacity. Its
/// contents are unspecified after any query, successful or not. Move-only.
class ProjectionScratch final
{
public:
    /// Construct scratch with no reserved capacity.
    ProjectionScratch() noexcept = default;
    /// Transfer the reserved storage; the source becomes empty.
    ProjectionScratch(ProjectionScratch&& other) noexcept = default;
    /// Release this storage and take the other's; the source becomes empty.
    ProjectionScratch& operator=(ProjectionScratch&& other) noexcept = default;
    /// Copying is disabled: one owner holds the storage.
    ProjectionScratch(const ProjectionScratch&) = delete;
    /// Copy assignment is disabled: one owner holds the storage.
    ProjectionScratch& operator=(const ProjectionScratch&) = delete;
    /// Release the storage.
    ~ProjectionScratch() = default;

    /// @brief Reserve room for capacity intervals and capacity candidates.
    /// @param capacity Entries of each kind; see ComputeProjectionScratchCapacity.
    /// @return Success, or OutOfMemory with the previous reservation kept.
    [[nodiscard]] GeometryStatus TryReserve(usize capacity) noexcept;
    /// Entries of each kind that a query may use without allocating.
    [[nodiscard]] usize GetCapacity() const noexcept;

private:
    /// Lets the projection search use the reserved storage; an implementation detail, not a public API.
    friend struct internal::ProjectionAccess;

    core::Array<ProjectionInterval> mIntervals;
    core::Array<ProjectionCandidate> mCandidates;
};

/// @brief Scratch entries a query needs: span count + 1 + maxIterations (each split adds at most one).
/// @return Success, or OutOfRange when the sum overflows. outCapacity is unchanged on failure.
[[nodiscard]] GeometryStatus
ComputeProjectionScratchCapacity(const PathView& path, usize maxIterations, usize& outCapacity) noexcept;

/// @brief Strict closest-point projection with a Certified global distance gap.
///
/// Every span starts as one interval whose control-hull box bounds its distance
/// from below; span starts and the terminal endpoint are candidates. Each
/// iteration splits the interval with the smallest lower bound (ties: earlier
/// span, then smaller parameter) at its midpoint, evaluates the midpoint as a
/// candidate and discards intervals whose lower bound exceeds the best
/// candidate. The search stops when the best candidate's distance minus the
/// smallest live lower bound is at most half of path.Tolerance.ProjectionGap; the
/// other half is the tie margin below. All bounds use outward-rounded enclosures,
/// so the gap is Certified.
///
/// Ties: every point whose distance is within ProjectionGap of the global lower
/// bound is acceptable. Once the search stops, the earliest live interval whose
/// lower bound allows a point within half the gap is split until it contains an
/// acceptable candidate or its bound rules it out. Every global minimizer lies in
/// such an interval, and the margin makes this search terminate. The result is the earliest
/// acceptable candidate by span then parameter, so symmetric minima resolve to the
/// earlier minimum and an endpoint within the gap wins over later interiors. This
/// tie search shares MaxIterations; intervals are kept while their lower bound is
/// within ProjectionGap of the best candidate.
/// @param path Borrowed prepared path.
/// @param query Point and iteration limit.
/// @param inoutScratch Reserved storage of at least ComputeProjectionScratchCapacity entries; contents are scratch.
/// @param outResult Written only on Success, with Guarantee Certified and IsGapMet true.
/// @return Success; InvalidArgument for an empty path; NonFiniteInput; CapacityExceeded when the scratch is too
/// small (checked before searching); BudgetExceeded when MaxIterations ends before the gap is met or the tie is
/// resolved; UnsupportedPrecision when an
/// interval reaches float64 parameter resolution with the gap still open; OutOfRange. outResult is unchanged on
/// failure.
[[nodiscard]] GeometryStatus TryProjectPoint(const PathView& path,
                                             const ProjectionQuery& query,
                                             ProjectionScratch& inoutScratch,
                                             ProjectionResult& outResult) noexcept;

/// @brief Approximate projection: TryProjectPoint's search that returns its candidate when work runs out.
///
/// When the gap is not met, the result is the earliest candidate with the
/// smallest distance found, IsGapMet is false and Gap reports the remaining
/// outward-rounded gap. Guarantee is always Approximate, which never satisfies a
/// strict profile.
/// @return TryProjectPoint's input and capacity statuses, OutOfRange, or Success; work exhaustion is Success.
/// outResult is unchanged on failure.
[[nodiscard]] GeometryStatus ProjectPointApproximate(const PathView& path,
                                                     const ProjectionQuery& query,
                                                     ProjectionScratch& inoutScratch,
                                                     ProjectionResult& outResult) noexcept;
} // namespace ludus::foundation::geometry
