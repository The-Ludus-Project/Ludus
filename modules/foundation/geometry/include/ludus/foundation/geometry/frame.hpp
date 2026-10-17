#pragma once

// Prepared rotation-minimizing frames (curves and surfaces architecture,
// "Orientation frames and sweeps", phase G3).
//
// TryBuildFrames bakes the sequential transport history of a prepared path once,
// with the double-reflection method of Wenping Wang, Bert Juttler, Dayue Zheng
// and Yang Liu, "Computation of Rotation Minimizing Frames", ACM Transactions on
// Graphics 27(1), article 2, 2008 (https://hub.hku.hk/handle/10722/152386).
// Carl Dougan, "The Parallel Transport Frame", Game Programming Gems 2, section
// 2.5, printed pages 215-219, motivates baking that history and its explicit
// singularity and loop policies (reviewed in
// docs/architecture/curves-surfaces-gems-review.md). Departures: sample steps
// are chosen adaptively by comparing one reflection step with two half steps,
// regularity is checked with outward-rounded derivative boxes rather than
// assumed, and random access transports from the preceding baked sample to the
// analytic tangent instead of interpolating quaternions.

#include <ludus/foundation/base/core.h>

#include <ludus/foundation/containers/array.hpp>
#include <ludus/foundation/containers/borrowed_view.hpp>
#include <ludus/foundation/geometry/distance.hpp>
#include <ludus/foundation/geometry/path.hpp>
#include <ludus/foundation/geometry/status.hpp>
#include <ludus/foundation/math/vector.hpp>

namespace ludus::foundation::geometry
{
using core::int32;

/// @brief What TryBuildFrames does when the seed direction is nearly parallel to the starting tangent.
enum class FrameSeedPolicy : uint8
{
    RejectDegenerateSeed, ///< Fail with DegenerateFrame.
    UseFallbackAxis,      ///< Project FramePreparation::FallbackAxis instead and record that choice in the run.
};

/// @brief What TryBuildFrames does where the tangent is undefined: zero speed, cusps and reversals inside a span.
enum class FrameSingularityPolicy : uint8
{
    Reject,    ///< Fail with DegenerateFrame and report the offending parameter range.
    SplitRuns, ///< End the current frame run before the singular range and seed a new run after it.
};

/// @brief What TryBuildFrames does at a joint that is not G1 under the path's ContinuityAngle.
enum class FrameCornerPolicy : uint8
{
    Reject,          ///< Fail with DegenerateFrame.
    RotateMinimally, ///< Rotate about the incoming x outgoing tangent axis; an (almost) exact reversal fails.
    SplitRuns,       ///< End the run at the joint and seed a new run on the outgoing span.
};

/// @brief Residual-twist policy of a closed path, part of the frame cache key.
enum class ClosedFramePolicy : uint8
{
    DistributeTwist,  ///< Add a compensating roll proportional to estimated arc distance so the seam is periodic.
    PreserveHolonomy, ///< Keep the transported frames; the seam frame differs by ResidualTwist.
};

/// @brief Inputs and limits of TryBuildFrames. Directions are chunk-relative and need not be unit length.
struct FramePreparation final
{
    /// Strict is rejected with UnsupportedPrecision: frame angular error is Estimated (ADR 0024).
    PrecisionProfile Precision = PrecisionProfile::Visual;
    math::Vector3d SeedUp{}; ///< Up direction projected onto the first normal plane.
    FrameSeedPolicy SeedPolicy = FrameSeedPolicy::RejectDegenerateSeed; ///< Degenerate-seed handling.
    math::Vector3d FallbackAxis{}; ///< Deterministic fallback; used only by UseFallbackAxis.
    /// Dimensionless in [0,1): a seed whose sine with the tangent is at or below this is degenerate. A corner
    /// rotation is undefined when the cosine of half its angle is at or below it.
    float64 MinimumSinAngle = 0.0;
    float64 MinimumSpeed = 0.0; ///< Asset units per unit u; finite and >= 0. The tangent is undefined at or below it.
    FrameSingularityPolicy SingularityPolicy = FrameSingularityPolicy::Reject; ///< Zero tangents, cusps, reversals.
    FrameCornerPolicy CornerPolicy = FrameCornerPolicy::Reject;                ///< Non-G1 joints.
    ClosedFramePolicy ClosedPolicy = ClosedFramePolicy::DistributeTwist;       ///< Closed paths only.
    int32 WindingTurns = 0; ///< Authored extra full turns added to the closed-loop compensation (DistributeTwist).
    /// MaxLeaves (samples), MaxDepth (dyadic level below a span, at most 52 is used), MaxSubdivisions (interval
    /// tests) and MaxPreparedBytes apply.
    WorkLimits Limits{};
};

/// @brief One baked frame sample. Vectors are unit length and chunk-relative (Rounded).
///
/// Normal is the transported reference direction, perpendicular to Tangent.
/// Samples are ordered by span then parameter; each run starts and ends with
/// a sample, and a joint inside a run has samples at both span sides.
struct FrameSample final
{
    SegmentParameter Parameter{};  ///< Where the sample lies; joints store the incoming u = 1 and outgoing u = 0.
    math::Vector3 Position{};      ///< Chunk-relative position (Rounded; stored endpoints Exact).
    math::Vector3 Tangent{};       ///< Unit tangent of the sample's span.
    math::Vector3 Normal{};        ///< Unit transported normal before roll and twist compensation.
    float64 Distance = 0.0;        ///< Estimated prefix arc length, asset units.
    float64 TwistCorrection = 0.0; ///< Closed-loop compensating roll at this sample, radians; 0 otherwise.
    usize RunIndex = 0;            ///< Frame run containing the sample.
};

/// @brief A contiguous parameter range with a defined transported frame.
struct FrameRun final
{
    usize FirstSample = 0;         ///< Index of the run's first sample.
    usize SampleCount = 0;         ///< Number of samples in the run, at least two.
    SegmentParameter Start{};      ///< First parameter of the run.
    SegmentParameter End{};        ///< Last parameter of the run.
    bool IsFallbackSeeded = false; ///< Whether the run was seeded from FallbackAxis rather than SeedUp.
};

/// @brief Borrowed view of PreparedFrames; valid while the owner is alive and unmodified.
struct FramesView final
{
    BorrowedView<const FrameSample> Samples; ///< Empty for frames that were never built.
    BorrowedView<const FrameRun> Runs;       ///< Frame runs in path order.
    uint64 PathRevision = 0;                 ///< Revision of the path the frames were built from.
    usize PathSpanCount = 0;                 ///< Span count of that path.
    /// Estimated angular error of a transported normal, radians: the summed one-step versus two-half-step
    /// differences of every accepted sample interval plus the largest one (for the final sub-step of a query).
    float64 AngularErrorEstimate = 0.0;
    GuaranteeClass AngularErrorGuarantee = GuaranteeClass::Estimated; ///< Always Estimated in G3.
    float64 ResidualTwist = 0.0; ///< Closed paths: signed transport holonomy in (-pi, pi], radians; 0 when open.
    float64 AppliedTwist = 0.0;  ///< Closed DistributeTwist: total compensating roll at the seam, radians.
    bool IsPeriodic = false;     ///< Whether the seam frame equals the start frame (closed DistributeTwist).
    ClosedFramePolicy ClosedPolicy = ClosedFramePolicy::DistributeTwist; ///< Policy the frames were built with.
    usize SubdivisionCount = 0;                                          ///< Interval tests performed while baking.
    usize DeepestLevel = 0;                                              ///< Deepest dyadic level used.
};

/// @brief Immutable frame cache for one path revision; move-only.
///
/// Built only by TryBuildFrames. A default owner is empty, and evaluation
/// through its view returns MissingCache. Single owner; synchronize replacement externally.
class PreparedFrames final
{
public:
    /// Construct an empty cache; evaluation reports MissingCache.
    PreparedFrames() noexcept = default;
    /// Transfer the cache; the source becomes empty.
    PreparedFrames(PreparedFrames&& other) noexcept = default;
    /// Release this cache and take the other's; the source becomes empty.
    PreparedFrames& operator=(PreparedFrames&& other) noexcept = default;
    /// Copying is disabled: one owner holds the prepared storage.
    PreparedFrames(const PreparedFrames&) = delete;
    /// Copy assignment is disabled: one owner holds the prepared storage.
    PreparedFrames& operator=(const PreparedFrames&) = delete;
    /// Release the prepared storage; views into it become invalid.
    ~PreparedFrames() = default;

    /// Borrow the cache; invalidated by moving or destroying the owner.
    [[nodiscard]] FramesView GetView() const noexcept;
    /// Whether no frames have been built into this owner.
    [[nodiscard]] bool IsEmpty() const noexcept;

private:
    friend GeometryStatus TryBuildFrames(const PathView& path,
                                         const DistanceMapView& map,
                                         const FramePreparation& preparation,
                                         PreparedFrames& outFrames,
                                         GeometryDiagnostic* outDiagnosticOrNull) noexcept;

    core::Array<FrameSample> mSamples;
    core::Array<FrameRun> mRuns;
    uint64 mPathRevision = 0;
    usize mPathSpanCount = 0;
    float64 mAngularErrorEstimate = 0.0;
    float64 mResidualTwist = 0.0;
    float64 mAppliedTwist = 0.0;
    bool mIsPeriodic = false;
    ClosedFramePolicy mClosedPolicy = ClosedFramePolicy::DistributeTwist;
    usize mSubdivisionCount = 0;
    usize mDeepestLevel = 0;
};

/// @brief Bake rotation-minimizing frames for a prepared path.
///
/// Each span is stepped over dyadic intervals. An interval is regular when an
/// outward-rounded box of its derivative stays farther than MinimumSpeed from
/// zero (a Certified decision). A regular interval is accepted when one
/// double-reflection step and two half steps differ by at most
/// path.Tolerance.TangentAngleError x width / span count; otherwise it is halved.
/// Intervals that cannot be shown regular at the depth limit are singular and
/// follow SingularityPolicy; intervals whose derivative box lies inside the
/// MinimumSpeed ball are skipped whole. The first run starts from SeedUp
/// projected perpendicular to the first tangent, or from FallbackAxis.
/// Non-G1 joints follow CornerPolicy. A closed path needs a G1 closing joint and
/// a single run for DistributeTwist, which adds -ResidualTwist + 2 pi
/// WindingTurns of roll in proportion to estimated distance.
/// @param path Borrowed prepared path.
/// @param map Distance map of the same path revision; provides sample distances and the twist distribution.
/// @param preparation Seed, policies and limits.
/// @param outFrames Replaced by the new cache only on Success.
/// @param outDiagnosticOrNull Optional; receives the failure, its span and parameter range.
/// @return Success; InvalidArgument for an empty path or a malformed preparation; NonFiniteInput;
/// MissingCache or StaleRevision for the distance map; UnsupportedPrecision for a Strict profile;
/// DegenerateFrame when a policy rejects a seed, singularity, corner or closed seam; BudgetExceeded when a
/// sample, depth, test or byte limit stops baking; OutOfRange; OutOfMemory. outFrames is replaced only on Success.
[[nodiscard]] GeometryStatus TryBuildFrames(const PathView& path,
                                            const DistanceMapView& map,
                                            const FramePreparation& preparation,
                                            PreparedFrames& outFrames,
                                            GeometryDiagnostic* outDiagnosticOrNull) noexcept;

/// @brief Right-handed orthonormal frame at a path parameter (Rounded; angular error Estimated).
///
/// Binormal = Tangent x Normal, so (Binormal, Normal, Tangent) is a
/// right-handed basis: Binormal points right and Normal up when looking along
/// Tangent.
struct TransportedFrame final
{
    math::Vector3 Position{}; ///< Chunk-relative position.
    math::Vector3 Tangent{};  ///< Unit analytic tangent of the evaluated span.
    math::Vector3 Normal{};   ///< Unit normal after twist compensation and roll.
    math::Vector3 Binormal{}; ///< Tangent x Normal.
    usize RunIndex = 0;       ///< Frame run that defined the frame.
};

/// @brief Evaluate a transported frame with an authored roll about the tangent.
///
/// The result depends only on the path, the frames and the request, never on
/// earlier queries. It transports the preceding baked sample of the same span
/// and run to the analytic tangent with one double-reflection step, then
/// rotates by the interpolated twist compensation plus rollRadians
/// (right-handed about Tangent). An exact sample parameter returns that sample.
/// @param path Borrowed path the frames were built from.
/// @param frames Borrowed frame cache.
/// @param parameter Span index and u in [0,1]; u = 1 selects the incoming side of a joint.
/// @param rollRadians Finite authored roll.
/// @param outFrame Written only on Success.
/// @return MissingCache for empty frames; StaleRevision for frames of another revision; NonFiniteInput;
/// InvalidDomain for a bad parameter; Degenerate where no run covers the parameter, such as at a cusp or
/// zero-speed endpoint; OutOfRange. outFrame is unchanged on failure.
[[nodiscard]] GeometryStatus TryEvaluateFrame(const PathView& path,
                                              const FramesView& frames,
                                              SegmentParameter parameter,
                                              float64 rollRadians,
                                              TransportedFrame& outFrame) noexcept;
} // namespace ludus::foundation::geometry
