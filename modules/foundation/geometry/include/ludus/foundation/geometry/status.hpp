#pragma once

// FoundationGeometry vocabulary frozen by ADR 0024: status codes, guarantee
// classes, domain values, span sides, boundary policies, the tolerance profile,
// work limits and the float32 position floor. This header includes only Base so
// every Geometry header and consumer can name results without parsing paths,
// tracks or containers.

#include <ludus/foundation/base/core.h>

namespace ludus::foundation::geometry
{
using core::float32;
using core::float64;
using core::uint64;
using core::uint8;
using core::usize;

/// @brief Result of a Geometry preparation, sampling or query operation (ADR 0024).
///
/// Enumerator names are frozen; numeric values and multi-failure precedence are
/// not an ABI or file format, so serialized diagnostics store GetStatusName().
/// Every failing operation leaves its outputs unchanged. A geometric miss is
/// Success with a classification in the result record.
enum class GeometryStatus : uint8
{
    Success,         ///< Outputs are written and meaningful.
    NonFiniteInput,  ///< A NaN or infinity where a finite value is required.
    InvalidArgument, ///< Malformed request or profile: negative tolerance, unknown policy, Wrap on an open path.
    InvalidDomain,   ///< Outside the domain under Reject; nonincreasing keys; coincident consecutive waypoints.
    InvalidTopology, ///< Unsupported or inconsistent adjacency, edge or corner records, such as disconnected spans.
    MissingCache,    ///< The snapshot lacks the cache the request needs; sampling never builds it.
    StaleRevision,   ///< A cursor, view or cache revision does not match its snapshot.
    Degenerate,      ///< A requested direction or normal is undefined there; position may still be valid.
    DegenerateFrame, ///< A framed profile cannot define a transported frame run under its policy.
    IllConditioned,  ///< Below a named conditioning threshold.
    OutOfRange,      ///< Finite inputs whose result cannot be represented.
    UnsupportedRepresentation, ///< Valid source geometry this version cannot represent.
    UnsupportedPrecision,      ///< Tolerance below the arithmetic floor, or a strict profile lacks a certificate.
    BudgetExceeded,            ///< A work, depth, iteration or output budget was exhausted.
    CapacityExceeded,          ///< A caller-owned output range is too small; detected before writing.
    OutOfMemory,               ///< A fallible allocation failed.
    Cancelled,                 ///< The caller cancelled preparation.
};

/// @brief Whether status is GeometryStatus::Success.
[[nodiscard]] constexpr bool IsSuccess(GeometryStatus status) noexcept
{
    return status == GeometryStatus::Success;
}

/// @brief Frozen enumerator name for serialized diagnostics, or "Unknown" for an out-of-range value.
/// @return A static, null-terminated string; never null.
[[nodiscard]] const char* GetStatusName(GeometryStatus status) noexcept;

/// @brief How a reported bound or value is guaranteed (ADR 0024).
enum class GuaranteeClass : uint8
{
    Exact,       ///< Bitwise defined: stored endpoints, decisions on stored values, exact identities.
    Rounded,     ///< One float64 computation narrowed to float32; no error bound is promised.
    Certified,   ///< Enclosure from reviewed outward-rounded arithmetic.
    Estimated,   ///< Heuristic, padded or sampled bound, reported with its class.
    Approximate, ///< Separately named mode that returns its candidate and achieved residual.
};

/// @brief Frozen enumerator name of a guarantee class, or "Unknown" for an out-of-range value.
[[nodiscard]] const char* GetGuaranteeClassName(GuaranteeClass guarantee) noexcept;

/// @brief Whether a strict preparation profile accepts a bound of this class (Exact or Certified only).
[[nodiscard]] constexpr bool IsStrictGuarantee(GuaranteeClass guarantee) noexcept
{
    return guarantee == GuaranteeClass::Exact || guarantee == GuaranteeClass::Certified;
}

/// @brief Which bounds a preparation accepts.
enum class PrecisionProfile : uint8
{
    Strict, ///< Only Exact or Certified bounds; otherwise UnsupportedPrecision and no output change.
    Visual, ///< Estimated bounds are accepted and reported with their class.
};

/// @brief Kind of domain a value or diagnostic range is expressed in.
enum class DomainKind : uint8
{
    SegmentParameter, ///< Span index and local u in [0,1].
    SurfaceParameter, ///< Patch index and local (u, v) in [0,1]^2.
    PathDistance,     ///< Prefix arc length in declared asset units.
    TrackTime,        ///< Seconds from the track's local epoch.
};

/// @brief Span index and local cubic parameter.
///
/// U is dimensionless in [0,1]. An exact span boundary is named by the outgoing
/// span at U = 0, except the terminal endpoint of an open path, which is the
/// last span at U = 1.
struct SegmentParameter final
{
    usize SpanIndex = 0; ///< Prepared span index; revision-local, not an author ID.
    float64 U = 0.0;     ///< Local parameter in [0,1].
};

/// @brief Patch index and local surface parameters; never a texture coordinate.
struct SurfaceParameter final
{
    usize PatchIndex = 0; ///< Prepared patch index.
    float64 U = 0.0;      ///< Local U in [0,1].
    float64 V = 0.0;      ///< Local V in [0,1].
};

/// @brief Prefix arc length from the path start, in the asset's declared local units.
struct PathDistance final
{
    float64 Value = 0.0; ///< Asset units; the domain is [0, total length].
};

/// @brief Seconds from a track's local epoch.
struct TrackTime final
{
    float64 Seconds = 0.0; ///< Seconds; the domain is [first key, last key].
};

/// @brief Which span an exact interior key or span boundary selects.
enum class SpanSide : uint8
{
    Outgoing, ///< The span that starts there, at u = 0 (the default).
    Incoming, ///< The previous span at u = 1; at the first endpoint both sides select the first span.
};

/// @brief Behavior outside the closed domain [start, end] of duration D; inside it every policy is identical.
enum class BoundaryPolicy : uint8
{
    Reject,   ///< InvalidDomain; outputs unchanged.
    Clamp,    ///< Nearest endpoint; derivatives are the one-sided ones from inside the domain.
    Hold,     ///< Tracks only: nearest endpoint value with zero derivatives.
    Loop,     ///< Tracks only: start + mod(x - start, D), in [start, end).
    PingPong, ///< Tracks only: reflect with period 2D; the reflected half negates the first derivative.
    Wrap,     ///< Closed paths only: Loop over distance; an open path rejects it as InvalidArgument.
};

/// @brief Tolerance profile with units (ADR 0024). There are no engine defaults.
///
/// A profile is validated as a whole by TryValidateToleranceProfile even when a
/// phase reads only some fields. Distances are asset units; angles are radians.
struct ToleranceProfile final
{
    float64 PositionError = 0.0;             ///< Asset units; finite, > 0 and at least the position floor.
    float64 RelativePositionError = 0.0;     ///< In [0,1); effective error is max(PositionError, this x asset scale).
    float64 DistanceError = 0.0;             ///< Asset units; finite, > 0; whole-path prefix gap.
    float64 TangentAngleError = 0.0;         ///< Radians in (0, pi/2); frame and tangent accuracy.
    float64 ProjectionGap = 0.0;             ///< Asset units; finite, > 0; global projection distance gap.
    float64 ContinuityPosition = 0.0;        ///< Asset units; finite, >= 0; C0 classification.
    float64 ContinuityDerivative = 0.0;      ///< In [0,1); relative difference of domain-scaled derivatives.
    float64 ContinuityAngle = 0.0;           ///< Radians in [0, pi/2); G1 direction classification.
    float64 NormalMinDerivativeLength = 0.0; ///< Asset units per unit parameter; finite, >= 0.
    float64 NormalMinSinAngle = 0.0;         ///< In [0,1); as PatchNormalPolicy.
    float64 NormalAngleError = 0.0;          ///< Radians in (0, pi/2); tessellation normal accuracy.
    float64 CoincidenceDistance = 0.0;       ///< Asset units; finite, > 0; source waypoint coincidence.
};

/// @brief Check every ToleranceProfile field against its ADR 0024 range.
///
/// The position floor depends on the prepared controls and is checked by the
/// preparation that knows them, not here.
/// @return Success, NonFiniteInput for a NaN or infinite field, or InvalidArgument for a finite out-of-range field.
[[nodiscard]] GeometryStatus TryValidateToleranceProfile(const ToleranceProfile& profile) noexcept;

/// @brief Work limits for one preparation or query. There are no engine defaults; zero allows no work.
struct WorkLimits final
{
    usize MaxSpans = 0;         ///< Source spans or keys accepted.
    usize MaxLeaves = 0;        ///< Cache leaves kept after refinement.
    usize MaxDepth = 0;         ///< Subdivision depth below one source span.
    usize MaxSubdivisions = 0;  ///< Total leaf splits during refinement.
    usize MaxIterations = 0;    ///< Iterations of one query's refinement loop.
    usize MaxPreparedBytes = 0; ///< Bytes owned by one prepared owner or cache, checked before allocation.
    usize MaxPatches = 0;       ///< Source patches accepted by a surface preparation.
};

/// @brief Float32 position floor 2 * ulp32(E) for chunk-relative controls of largest absolute component E.
///
/// ulp32(E) = 2^(floor(log2 E) - 23) for E in the float32 normal range. Below
/// it the floor is two subnormal units (2^-148); E = 0 also returns that value.
/// The floor is an Exact policy value, not a promise that a computation achieves it.
/// @pre E is finite and nonnegative.
[[nodiscard]] float64 ComputePositionFloor(float64 maxAbsComponent) noexcept;

/// @brief Structured preparation diagnostic; consumers log and attach resource identity.
///
/// Property is a static string naming the offending source property, never
/// owned text. Unused numeric fields stay zero.
struct GeometryDiagnostic final
{
    GeometryStatus Code = GeometryStatus::Success;            ///< Failure that produced the diagnostic.
    usize SourceElement = 0;                                  ///< Source span, waypoint or key index.
    const char* Property = "";                                ///< Static property path, such as "Keys.Time".
    float64 RequestedTolerance = 0.0;                         ///< Requested tolerance in the property's unit.
    float64 AchievedBound = 0.0;                              ///< Best bound reached, in the same unit.
    GuaranteeClass AchievedGuarantee = GuaranteeClass::Exact; ///< Class of AchievedBound.
    DomainKind Domain = DomainKind::SegmentParameter;         ///< Domain of RangeStart and RangeEnd.
    float64 RangeStart = 0.0;                                 ///< Start of the offending range.
    float64 RangeEnd = 0.0;                                   ///< End of the offending range.
};
} // namespace ludus::foundation::geometry
