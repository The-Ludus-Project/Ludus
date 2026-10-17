#pragma once

// Bounded sweep mesh generation (curves and surfaces architecture,
// "Orientation frames and sweeps", phase G3).
//
// A sweep places a 2D section in the normal plane of transported frames along a
// prepared path. TryPlanSweepMesh chooses rings under explicit longitudinal and
// cross-section error budgets, validates every count, limit and numeric range,
// and stores the ring frames in an immutable plan. TryEmitSweepMesh then checks
// the caller's capacities and writes vertices and indices with no further
// failure point. Planning never writes caller output, and emission never
// allocates, so a failed request leaves caller output unchanged.

#include <ludus/foundation/base/core.h>

#include <ludus/foundation/containers/array.hpp>
#include <ludus/foundation/containers/borrowed_view.hpp>
#include <ludus/foundation/geometry/distance.hpp>
#include <ludus/foundation/geometry/frame.hpp>
#include <ludus/foundation/geometry/path.hpp>
#include <ludus/foundation/geometry/status.hpp>
#include <ludus/foundation/math/vector.hpp>

namespace ludus::foundation::geometry
{
using core::uint32;

/// @brief Cross-section swept along the path.
enum class SweepSection : uint8
{
    Ribbon, ///< Flat strip along the frame binormal, Scale wide; faces the frame normal; no caps.
    Tube,   ///< Closed circle of radius Scale approximated by a regular polygon with a duplicated UV seam column.
};

/// @brief Ends of an open tube.
enum class SweepCaps : uint8
{
    None, ///< Leave the ends open.
    Flat, ///< Close each end with a flat fan; cap vertices copy ring positions with their own normals and UVs.
};

/// @brief Front-face winding of emitted triangles, seen from the side their normals point to.
enum class SweepWinding : uint8
{
    CounterClockwise, ///< Counter-clockwise front faces.
    Clockwise,        ///< Clockwise front faces.
};

/// @brief One key of the section profile, linear in path distance and clamped outside the keys.
struct SweepProfileKey final
{
    PathDistance Distance{};   ///< Estimated path distance of the key; keys strictly increase.
    float64 Scale = 0.0;       ///< Ribbon width or tube radius, asset units; finite and > 0.
    float64 RollRadians = 0.0; ///< Authored roll about the tangent, radians; finite.
};

/// @brief Section, budgets and limits of TryPlanSweepMesh. There are no engine defaults.
struct SweepRequest final
{
    SweepSection Section = SweepSection::Ribbon;           ///< Ribbon or tube.
    BorrowedView<const SweepProfileKey> Profile;           ///< At least one key, borrowed for the call only.
    SweepCaps Caps = SweepCaps::None;                      ///< Flat is valid only for an open tube.
    SweepWinding Winding = SweepWinding::CounterClockwise; ///< Front-face winding.
    float64 LongitudinalError = 0.0; ///< Asset units; finite and > 0. Estimated deviation between rings.
    float64 CrossSectionError = 0.0; ///< Asset units; finite and > 0. Tube polygon error at the largest radius.
    usize MaxSectionSegments = 0;    ///< Tube polygon segments allowed; a ribbon uses one.
    usize MaxVertexCount = 0;        ///< Output vertex budget.
    usize MaxIndexCount = 0;         ///< Output index budget.
    /// MaxLeaves (rings), MaxDepth (dyadic level below a span, at most 52 is used), MaxSubdivisions (interval
    /// tests) and MaxPreparedBytes (ring storage) apply.
    WorkLimits Limits{};
};

/// @brief One planned ring: a frame, its path distance and section scale (Rounded).
struct SweepRing final
{
    SegmentParameter Parameter{}; ///< Ring parameter; joints use the outgoing span.
    float64 Distance = 0.0;       ///< Estimated path distance; the V texture coordinate.
    math::Vector3d Center{};      ///< Chunk-relative path position.
    math::Vector3d Tangent{};     ///< Unit tangent.
    math::Vector3d Normal{};      ///< Unit normal after twist compensation and roll.
    math::Vector3d Binormal{};    ///< Tangent x Normal.
    float64 Scale = 0.0;          ///< Profile scale at Distance.
};

/// @brief Counts and estimated errors of a planned sweep.
struct SweepMeshReport final
{
    usize RingCount = 0;           ///< Rings, including a closed sweep's duplicated seam ring.
    usize SectionSegmentCount = 0; ///< Tube polygon segments, or 1 for a ribbon.
    usize VertexCount = 0;         ///< Vertices TryEmitSweepMesh writes.
    usize IndexCount = 0;          ///< Indices TryEmitSweepMesh writes (three per triangle).
    /// Largest accepted deviation between rings, asset units: the control-hull distance from the chord plus the
    /// sampled midpoint deviation of the section points.
    float64 LongitudinalErrorEstimate = 0.0;
    float64 CrossSectionErrorEstimate = 0.0; ///< Largest scale x (1 - cos(pi / segments)); 0 for a ribbon.
    GuaranteeClass ErrorGuarantee = GuaranteeClass::Estimated; ///< Class of both error values.
    /// Rings whose section half-extent times path curvature reaches 1, a local self-intersection risk.
    usize SelfIntersectionRiskCount = 0;
    float64 MaxBendRatio = 0.0; ///< Largest section half-extent times curvature over the rings.
    /// Estimated: a sampled local check, never a proof that the swept surface is simple.
    GuaranteeClass RiskGuarantee = GuaranteeClass::Estimated;
};

/// @brief Borrowed view of a SweepMeshPlan; valid while the plan is alive and unmodified.
struct SweepMeshPlanView final
{
    BorrowedView<const SweepRing> Rings;                   ///< Rings in path order; empty for an unplanned sweep.
    uint64 PathRevision = 0;                               ///< Pinned path revision.
    usize PathSpanCount = 0;                               ///< Pinned path span count.
    SweepSection Section = SweepSection::Ribbon;           ///< Planned section.
    SweepCaps Caps = SweepCaps::None;                      ///< Planned caps.
    SweepWinding Winding = SweepWinding::CounterClockwise; ///< Planned winding.
    bool IsClosed = false;                                 ///< Whether the last ring copies the first one's positions.
    SweepMeshReport Report{};                              ///< Counts and estimated errors.
};

/// @brief Immutable, validated sweep emission plan; move-only.
///
/// Created only by TryPlanSweepMesh. It pins the path revision; emission
/// against another revision fails with StaleRevision.
class SweepMeshPlan final
{
public:
    /// Construct an empty plan; emission reports MissingCache.
    SweepMeshPlan() noexcept = default;
    /// Transfer the plan; the source becomes empty.
    SweepMeshPlan(SweepMeshPlan&& other) noexcept = default;
    /// Release this plan and take the other's; the source becomes empty.
    SweepMeshPlan& operator=(SweepMeshPlan&& other) noexcept = default;
    /// Copying is disabled: one owner holds the ring storage.
    SweepMeshPlan(const SweepMeshPlan&) = delete;
    /// Copy assignment is disabled: one owner holds the ring storage.
    SweepMeshPlan& operator=(const SweepMeshPlan&) = delete;
    /// Release the ring storage; views into it become invalid.
    ~SweepMeshPlan() = default;

    /// Borrow the plan; invalidated by moving or destroying the owner.
    [[nodiscard]] SweepMeshPlanView GetView() const noexcept;
    /// Whether no sweep has been planned into this owner.
    [[nodiscard]] bool IsEmpty() const noexcept;

private:
    friend GeometryStatus TryPlanSweepMesh(const PathView& path,
                                           const DistanceMapView& map,
                                           const FramesView& frames,
                                           const SweepRequest& request,
                                           SweepMeshPlan& outPlan,
                                           GeometryDiagnostic* outDiagnosticOrNull) noexcept;

    core::Array<SweepRing> mRings;
    uint64 mPathRevision = 0;
    usize mPathSpanCount = 0;
    SweepSection mSection = SweepSection::Ribbon;
    SweepCaps mCaps = SweepCaps::None;
    SweepWinding mWinding = SweepWinding::CounterClockwise;
    bool mIsClosed = false;
    SweepMeshReport mReport{};
};

/// @brief Plan a ribbon or tube sweep along a framed path.
///
/// Requires frames with one run over the whole path and G1 joints; a closed
/// path requires periodic frames (ClosedFramePolicy::DistributeTwist), and its
/// last ring copies the first ring with V = the path's estimated total length,
/// so the UV seam is explicit and positions match bitwise. Each span is stepped
/// over dyadic intervals, starting a ring at every span start; an interval is
/// accepted when the distance of its control hull from the chord plus the
/// midpoint deviation of every section point is at most LongitudinalError.
/// A tube uses the fewest segments whose polygon error at the largest profile
/// scale is at most CrossSectionError.
/// @param path Borrowed prepared path; its revision is pinned in the plan.
/// @param map Distance map of the same revision, for ring distances and profile lookup.
/// @param frames Frames of the same revision.
/// @param request Section, profile, budgets and limits.
/// @param outPlan Replaced by the new plan only on Success.
/// @param outDiagnosticOrNull Optional; receives the failure's details.
/// @return Success; InvalidArgument for an empty path, malformed request, unordered or nonpositive profile, or
/// caps on a ribbon or closed path; NonFiniteInput; MissingCache or StaleRevision for the map or frames;
/// DegenerateFrame when the frames have gaps, corners or an aperiodic closed seam; BudgetExceeded for ring,
/// depth, test, byte, segment, vertex or index limits; OutOfRange when an output would exceed float32 or
/// uint32 indices; OutOfMemory. outPlan is replaced only on Success.
[[nodiscard]] GeometryStatus TryPlanSweepMesh(const PathView& path,
                                              const DistanceMapView& map,
                                              const FramesView& frames,
                                              const SweepRequest& request,
                                              SweepMeshPlan& outPlan,
                                              GeometryDiagnostic* outDiagnosticOrNull) noexcept;

/// @brief One emitted vertex (Rounded).
struct SweepVertex final
{
    math::Vector3 Position{}; ///< Chunk-relative position.
    math::Vector3 Normal{};   ///< Unit geometric normal of the section (ribbon: frame normal; tube: radial).
    math::Vector2 Uv{};       ///< U across the section in [0,1]; V is path distance in asset units; caps are planar.
};

/// @brief Write a planned sweep into caller-owned ranges.
///
/// Vertices are ring-major. A ribbon ring has the -Binormal then +Binormal
/// edge; a tube ring has SectionSegmentCount + 1 vertices, the last repeating
/// the first position with U = 1. Flat caps follow the rings: each has a center
/// then one vertex per segment. Indices form a triangle list.
/// @param path Path whose revision the plan pinned.
/// @param plan Borrowed plan.
/// @param outVertices At least Report.VertexCount entries; must not overlap outIndices.
/// @param outIndices At least Report.IndexCount entries.
/// @return Success; MissingCache for an empty plan; StaleRevision when path is not the pinned revision;
/// CapacityExceeded when either range is too small. Nothing is written on failure, and emission never fails
/// after it starts writing.
[[nodiscard]] GeometryStatus TryEmitSweepMesh(const PathView& path,
                                              const SweepMeshPlanView& plan,
                                              BorrowedView<SweepVertex> outVertices,
                                              BorrowedView<uint32> outIndices) noexcept;
} // namespace ludus::foundation::geometry
