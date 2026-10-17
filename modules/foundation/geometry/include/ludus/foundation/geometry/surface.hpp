#pragma once

// Prepared bicubic patch surfaces (curves and surfaces architecture, "Surface
// evaluation and topology", phase G4).
//
// A source surface is a set of untrimmed bicubic Bezier patches in float64
// coordinates plus explicit adjacency records. TryPrepareSurface subtracts the
// chunk origin, narrows controls to float32 and builds the topology: stable
// patch, edge and corner identities, one canonical control curve per edge and
// one position per corner. V1 accepts only matching whole-patch boundaries with
// at most two incident patches per edge. Partial, nonmanifold and mismatched
// joins are rejected, and nothing is welded by a distance threshold. Mesh
// preparation lives in tessellation.hpp.

#include <ludus/foundation/base/core.h>

#include <ludus/foundation/containers/array.hpp>
#include <ludus/foundation/containers/borrowed_view.hpp>
#include <ludus/foundation/containers/static_array.hpp>
#include <ludus/foundation/geometry/status.hpp>
#include <ludus/foundation/math/patch.hpp>
#include <ludus/foundation/math/vector.hpp>

namespace ludus::foundation::geometry
{
/// Controls along each direction of a bicubic patch and along each boundary curve.
inline constexpr usize PATCH_CONTROL_ORDER = 4;
/// Boundary sides of one patch.
inline constexpr usize PATCH_SIDE_COUNT = 4;
/// Corners of one patch.
inline constexpr usize PATCH_CORNER_COUNT = 4;
/// Largest number of patch sides incident to one edge (V1 manifold rule).
inline constexpr usize MAX_EDGE_INCIDENCES = 2;
/// Corner records at the start and end of one edge.
inline constexpr usize EDGE_CORNER_COUNT = 2;

/// @brief One boundary side of a patch, named by its fixed parameter.
///
/// Each side has an along-edge parameter s in [0,1]: v for U0 and U1, u for V0
/// and V1. Its control curve in increasing s is Control[0][k] (U0),
/// Control[3][k] (U1), Control[k][0] (V0) or Control[k][3] (V1).
enum class PatchSide : uint8
{
    U0, ///< u = 0; runs along v.
    U1, ///< u = 1; runs along v.
    V0, ///< v = 0; runs along u.
    V1, ///< v = 1; runs along u.
};

/// @brief One corner of a patch, named by its (u, v) parameters.
enum class PatchCorner : uint8
{
    U0V0, ///< (0, 0): Control[0][0].
    U1V0, ///< (1, 0): Control[3][0].
    U0V1, ///< (0, 1): Control[0][3].
    U1V1, ///< (1, 1): Control[3][3].
};

/// @brief One source patch in float64 asset coordinates.
struct SurfacePatchSource final
{
    uint64 Id = 0; ///< Stable author ID; unique within the surface. Edge and corner IDs derive from it.
    core::StaticArray<core::StaticArray<math::Vector3d, PATCH_CONTROL_ORDER>, PATCH_CONTROL_ORDER>
        Control{}; ///< Control[u index][v index], asset coordinates.
};

/// @brief Explicit join of two whole patch sides.
///
/// The second side's along-edge parameter equals the first side's s, or 1 - s
/// when IsReversed. Both control curves must be identical after narrowing and
/// that mapping. A side joined to another side of the same patch is allowed
/// (for example a closed cylinder); a side joined to itself is not.
struct SurfaceAdjacencySource final
{
    usize FirstPatch = 0;                 ///< Index into SurfaceSource::Patches.
    PatchSide FirstSide = PatchSide::U0;  ///< Side of the first patch.
    usize SecondPatch = 0;                ///< Index into SurfaceSource::Patches.
    PatchSide SecondSide = PatchSide::U0; ///< Side of the second patch.
    bool IsReversed = false;              ///< The second side runs opposite to the first.
};

/// @brief Whole source surface for TryPrepareSurface.
struct SurfaceSource final
{
    math::Vector3d Origin{};                              ///< Float64 chunk origin subtracted before narrowing.
    BorrowedView<const SurfacePatchSource> Patches;       ///< At least one patch, borrowed for the call only.
    BorrowedView<const SurfaceAdjacencySource> Adjacency; ///< Shared edges; unlisted sides are boundary edges.
    float64 AssetScale = 0.0; ///< Declared asset scale for RelativePositionError; finite and >= 0.
    uint64 Revision = 0;      ///< Owner-assigned snapshot revision copied into meshes built from this surface.
};

/// @brief Profile and limits for TryPrepareSurface.
struct SurfacePreparation final
{
    ToleranceProfile Tolerance{}; ///< Validated as a whole; copied into the prepared surface.
    WorkLimits Limits{};          ///< MaxPatches and MaxPreparedBytes apply.
};

/// @brief Stable identity of an edge: its incidence with the smallest (patch ID, side).
///
/// It does not depend on array order, so it survives reordering of the source
/// patches as long as author IDs and joins are unchanged.
struct SurfaceEdgeId final
{
    uint64 PatchId = 0;             ///< Author ID of the owning patch.
    PatchSide Side = PatchSide::U0; ///< Side of the owning patch.
};

/// @brief Stable identity of a corner: its incidence with the smallest (patch ID, corner).
struct SurfaceCornerId final
{
    uint64 PatchId = 0;                     ///< Author ID of the owning patch.
    PatchCorner Corner = PatchCorner::U0V0; ///< Corner of the owning patch.
};

/// @brief One patch side incident to an edge.
struct SurfaceEdgeIncidence final
{
    usize PatchIndex = 0;           ///< Prepared patch index; revision-local.
    PatchSide Side = PatchSide::U0; ///< Side of that patch.
};

/// @brief One prepared edge with its canonical boundary curve.
///
/// Incidence[0] is the owner named by Id. The canonical curve runs in the
/// owner's along-edge parameter t; Incidence[1], when present, has s = t, or
/// s = 1 - t when IsReversed. Continuity values are threshold decisions on
/// Rounded values; a boundary edge leaves them at their defaults.
struct SurfaceEdge final
{
    SurfaceEdgeId Id{};                                                       ///< Stable identity.
    core::StaticArray<SurfaceEdgeIncidence, MAX_EDGE_INCIDENCES> Incidence{}; ///< Owner first.
    usize IncidenceCount = 1;                                                 ///< 1 for a boundary edge, 2 if shared.
    bool IsReversed = false; ///< Incidence[1] runs opposite to the canonical curve.
    /// Canonical chunk-relative control curve in the owner's orientation (Exact stored values).
    core::StaticArray<math::Vector3, PATCH_CONTROL_ORDER> Control{};
    core::StaticArray<usize, EDGE_CORNER_COUNT> Corner{}; ///< Corner records at t = 0 and t = 1.
    /// Shared edges only: each patch traverses the edge in the opposite direction along its counterclockwise
    /// (u, v) boundary, so the two geometric normals Cross(DU, DV) agree in orientation.
    bool IsOrientationConsistent = false;
    /// Shared edges only: largest relative difference between the two outward cross-boundary derivative
    /// controls, compared as O_first(t) = -O_second(t) after the parameter mapping.
    float64 CrossDerivativeDifference = 0.0;
    bool IsC1 = false; ///< Shared edges only: CrossDerivativeDifference <= ContinuityDerivative.
};

/// @brief One patch corner incident to a corner record.
struct SurfaceCornerIncidence final
{
    usize PatchIndex = 0;                   ///< Prepared patch index; revision-local.
    PatchCorner Corner = PatchCorner::U0V0; ///< Corner of that patch.
};

/// @brief One corner record shared by every patch corner joined through edges.
///
/// Patch corners that meet only geometrically, without a declared edge chain,
/// stay separate records with equal positions; nothing is welded by distance.
struct SurfaceCorner final
{
    SurfaceCornerId Id{};     ///< Stable identity.
    math::Vector3 Position{}; ///< Chunk-relative stored corner control (Exact), equal for every incidence.
    usize FirstIncidence = 0; ///< First entry in SurfaceView::CornerIncidences.
    usize IncidenceCount = 0; ///< Number of incident patch corners, at least one.
};

/// @brief One prepared patch with its topology references.
struct PreparedPatch final
{
    uint64 Id = 0;                                         ///< Stable author ID.
    math::BicubicBezierPatch Control{};                    ///< Chunk-relative float32 controls, Control[u][v].
    core::StaticArray<usize, PATCH_SIDE_COUNT> Edge{};     ///< Edge index for each PatchSide.
    core::StaticArray<usize, PATCH_CORNER_COUNT> Corner{}; ///< Corner record index for each PatchCorner.
};

/// @brief Borrowed, allocation-free view of a PreparedSurface.
///
/// The view does not own storage; it is valid while its PreparedSurface is
/// alive and unmodified. A default view has no patches.
struct SurfaceView final
{
    BorrowedView<const PreparedPatch> Patches;                   ///< Patches in source order.
    BorrowedView<const SurfaceEdge> Edges;                       ///< Edges ordered by the owner's patch index and side.
    BorrowedView<const SurfaceCorner> Corners;                   ///< Corner records in first-incidence order.
    BorrowedView<const SurfaceCornerIncidence> CornerIncidences; ///< Grouped by corner record.
    math::Vector3d Origin{};                                     ///< Float64 chunk origin of every control.
    uint64 Revision = 0;                                         ///< Owner-assigned revision from SurfaceSource.
    float64 PositionFloor = 0.0;                                 ///< ADR 0024 floor of the prepared controls (Exact).
    float64 EffectivePositionError = 0.0; ///< max(PositionError, RelativePositionError x AssetScale).
    ToleranceProfile Tolerance{};         ///< Profile the surface was prepared with, including normal thresholds.
};

/// @brief Immutable prepared surface owner; move-only.
///
/// Created only by TryPrepareSurface. Evaluation and mesh preparation borrow
/// GetView() and never mutate the owner. Single owner; external
/// synchronization is required to replace it while readers hold views.
class PreparedSurface final
{
public:
    /// Construct an empty owner with no patches.
    PreparedSurface() noexcept = default;
    /// Transfer the prepared storage; the source becomes empty.
    PreparedSurface(PreparedSurface&& other) noexcept = default;
    /// Release this owner's storage and take the other's; the source becomes empty.
    PreparedSurface& operator=(PreparedSurface&& other) noexcept = default;
    /// Copying is disabled: one owner holds the prepared storage.
    PreparedSurface(const PreparedSurface&) = delete;
    /// Copy assignment is disabled: one owner holds the prepared storage.
    PreparedSurface& operator=(const PreparedSurface&) = delete;
    /// Release the prepared storage; views into it become invalid.
    ~PreparedSurface() = default;

    /// Borrow the prepared data; the view is invalidated by moving or destroying the owner.
    [[nodiscard]] SurfaceView GetView() const noexcept;
    /// Whether no surface has been prepared into this owner.
    [[nodiscard]] bool IsEmpty() const noexcept;

private:
    friend GeometryStatus TryPrepareSurface(const SurfaceSource& source,
                                            const SurfacePreparation& preparation,
                                            PreparedSurface& outSurface,
                                            GeometryDiagnostic* outDiagnosticOrNull) noexcept;

    core::Array<PreparedPatch> mPatches;
    core::Array<SurfaceEdge> mEdges;
    core::Array<SurfaceCorner> mCorners;
    core::Array<SurfaceCornerIncidence> mCornerIncidences;
    math::Vector3d mOrigin{};
    uint64 mRevision = 0;
    float64 mPositionFloor = 0.0;
    float64 mEffectivePositionError = 0.0;
    ToleranceProfile mTolerance{};
};

/// @brief Validate a source surface and build a new prepared owner with its topology.
///
/// Checks: a valid tolerance profile; at least one patch within Limits.MaxPatches; unique patch IDs; finite
/// controls, origin and asset scale; the float32 position floor of the narrowed controls and their narrowing
/// error against the effective position error; adjacency records that name existing patches and known sides,
/// use each patch side at most once, never join a side to itself, and whose narrowed control curves are equal
/// after the declared orientation mapping; and the prepared byte size against Limits.MaxPreparedBytes.
/// @param source Source patches, adjacency and origin, borrowed for the call only.
/// @param preparation Tolerance profile and limits; the profile is copied into the surface.
/// @param outSurface Replaced by the new owner only on Success.
/// @param outDiagnosticOrNull Optional; receives the first failure's details. SourceElement is the patch index,
/// or the adjacency index for "Adjacency" properties.
/// @return Success; NonFiniteInput; InvalidArgument for an unknown side, a bad asset scale or no patches;
/// InvalidTopology for a duplicate patch ID, an out-of-range patch index, a side joined to itself, a third
/// incidence (nonmanifold) or mismatched curves (including partial joins); UnsupportedPrecision; OutOfRange when
/// a relative control exceeds float32; BudgetExceeded; OutOfMemory. outSurface is replaced only on Success.
[[nodiscard]] GeometryStatus TryPrepareSurface(const SurfaceSource& source,
                                               const SurfacePreparation& preparation,
                                               PreparedSurface& outSurface,
                                               GeometryDiagnostic* outDiagnosticOrNull) noexcept;

/// @brief Evaluate a chunk-relative position at a surface parameter (Rounded; corners Exact).
/// @return InvalidDomain for an out-of-range patch index or a parameter outside [0,1]; NonFiniteInput for a
/// nonfinite parameter; OutOfRange. outPosition is unchanged on failure.
[[nodiscard]] GeometryStatus
TryEvaluateSurface(const SurfaceView& surface, SurfaceParameter parameter, math::Vector3& outPosition) noexcept;
/// @brief Evaluate position and first/second partials per normalized U and V, with the position overload's
/// contract.
[[nodiscard]] GeometryStatus
TryEvaluateSurface(const SurfaceView& surface, SurfaceParameter parameter, math::PatchSample& outSample) noexcept;

/// @brief Geometric unit normal normalize(Cross(DU, DV)) at a surface parameter (Rounded).
///
/// Uses the surface's NormalMinDerivativeLength and NormalMinSinAngle as the
/// math::PatchNormalPolicy. The position remains valid where the normal is not.
/// @return Degenerate for exactly zero or parallel partials; IllConditioned at or below the thresholds; plus the
/// TryEvaluateSurface statuses. outNormal is unchanged on failure.
[[nodiscard]] GeometryStatus
TryEvaluateSurfaceNormal(const SurfaceView& surface, SurfaceParameter parameter, math::Vector3& outNormal) noexcept;

/// @brief Map a canonical edge parameter t to an incident patch's surface parameter.
///
/// Incidence 0 uses s = t; incidence 1 uses s = 1 - t when the edge is
/// reversed. Exact for dyadic t, because 1 - t is then exact.
/// @param surface Borrowed prepared surface.
/// @param edgeIndex Prepared edge index.
/// @param incidence 0 for the owner, 1 for the other side of a shared edge.
/// @param edgeParameter Canonical t in [0,1].
/// @param outParameter Written only on Success.
/// @return InvalidDomain for a bad edge index, incidence or t outside [0,1]; NonFiniteInput for a nonfinite t.
[[nodiscard]] GeometryStatus TryMapEdgeParameter(const SurfaceView& surface,
                                                 usize edgeIndex,
                                                 usize incidence,
                                                 float64 edgeParameter,
                                                 SurfaceParameter& outParameter) noexcept;
} // namespace ludus::foundation::geometry
