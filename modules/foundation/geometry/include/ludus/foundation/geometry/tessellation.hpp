#pragma once

// CPU surface mesh preparation (curves and surfaces architecture, "Tessellation
// and error ownership", phase G4).
//
// Each patch is tessellated as a rectangular dyadic grid of 2^LevelU x 2^LevelV
// cells, split along the (u0, v0)-(u1, v1) diagonal. Levels are refined until a
// Certified bound on the distance between every cell and its triangle pair fits
// the profile. Shared edges then take the larger of their incident levels, and
// the affected patches are rechecked until the network is consistent. Canonical
// corner and edge samples are emitted once and referenced by every render
// vertex that shares them, so neighbouring patches cannot crack.
//
// The cell bound follows the architecture: the control-net distance to the
// degree-elevated bilinear interpolant of the cell's corners, plus the twisted
// bilinear quad's distance to its triangle pair, Length(P00 - P10 - P01 + P11)/4
// for the corners P of that cell, plus the rounding error of the emitted
// vertices. Every term is evaluated with FoundationMath's outward-rounded
// enclosures, so the position bound is Certified (ADR 0024). The normal-angle
// estimate samples analytic normals and is Estimated.

#include <ludus/foundation/base/core.h>

#include <ludus/foundation/containers/array.hpp>
#include <ludus/foundation/containers/borrowed_view.hpp>
#include <ludus/foundation/geometry/status.hpp>
#include <ludus/foundation/geometry/surface.hpp>
#include <ludus/foundation/math/vector.hpp>

namespace ludus::foundation::geometry
{
/// Largest dyadic level per patch direction a mesh profile may request (2^16 cells per direction).
inline constexpr usize MAX_SURFACE_MESH_LEVEL = 16;
/// Indices per emitted triangle.
inline constexpr usize SURFACE_MESH_TRIANGLE_INDEX_COUNT = 3;

/// @brief How mesh preparation treats singular normals and collapsed triangles.
enum class SurfaceDegeneracyPolicy : uint8
{
    Reject, ///< Fail with Degenerate or IllConditioned and keep the previous mesh.
    Report, ///< Emit a zero normal with its status, keep collapsed triangles, and count both in the report.
};

/// @brief Explicit mesh work and output budgets; there are no engine defaults and zero allows nothing.
struct SurfaceMeshLimits final
{
    usize MaxLevel = 0;       ///< Dyadic level per patch direction; at most MAX_SURFACE_MESH_LEVEL.
    usize MaxCellTests = 0;   ///< Cell bound evaluations during level selection, including rechecks.
    usize MaxVertices = 0;    ///< Render vertices; also limited by the uint32 index range.
    usize MaxTriangles = 0;   ///< Emitted triangles.
    usize MaxOutputBytes = 0; ///< Bytes owned by the mesh, checked before its arrays are allocated.
};

/// @brief Mesh accuracy, failure behavior and budgets for TryBuildSurfaceMesh.
///
/// Errors are measured against the prepared surface, the exact patches of its
/// stored float32 controls, in chunk-relative asset units. Source narrowing
/// error is checked separately by TryPrepareSurface.
struct SurfaceMeshProfile final
{
    PrecisionProfile Precision = PrecisionProfile::Strict; ///< Strict accepts only Certified or Exact bounds.
    float64 PositionError = 0.0; ///< Asset units; finite, > 0 and at least the surface's PositionFloor.
    /// Radians; 0 requests no normal bound, otherwise in (0, pi/2). The only available normal bound is
    /// Estimated, so a Strict profile with a nonzero value fails with UnsupportedPrecision.
    float64 NormalAngleError = 0.0;
    SurfaceDegeneracyPolicy Degeneracy = SurfaceDegeneracyPolicy::Reject; ///< Singular normal treatment.
    /// Visual only: when MaxLevel stops refinement, emit the finest grids reached and report the achieved
    /// bounds instead of failing. A Strict profile must leave it false.
    bool IsCoarserMeshAccepted = false;
    SurfaceMeshLimits Limits{}; ///< Work and output budgets.
};

/// @brief Topological owner of one emitted position.
enum class SurfaceSampleKind : uint8
{
    Corner,   ///< A corner record; the position is its stored control (Exact).
    Edge,     ///< An interior sample of an edge, evaluated once on the canonical curve.
    Interior, ///< An interior sample of one patch.
};

/// @brief One unique geometric position, emitted once and shared by every render vertex at that point.
struct SurfaceMeshPosition final
{
    math::Vector3 Position{};                           ///< Chunk-relative float32 position.
    SurfaceSampleKind Kind = SurfaceSampleKind::Corner; ///< Topological owner kind.
    usize OwnerIndex = 0;                               ///< Corner, edge or patch index of that owner.
    float64 EdgeParameter = 0.0;                        ///< Canonical t for an Edge sample; otherwise 0.
    float64 ErrorBound = 0.0; ///< Certified upper bound of |Position - exact surface point|; 0 for a corner.
};

/// @brief One render vertex: a shared position plus per-patch shading data.
///
/// Vertices of different patches at one point copy the same Position bitwise
/// and name the same PositionIndex, while their normals and parameters may
/// differ (a normal or parameter seam). Parameter is never a texture coordinate.
struct SurfaceMeshVertex final
{
    math::Vector3 Position{};     ///< Bitwise copy of the referenced SurfaceMeshPosition.
    math::Vector3 Normal{};       ///< Unit normal of the vertex's patch (Rounded), or zero when NormalStatus fails.
    usize PositionIndex = 0;      ///< Index into SurfaceMeshView::Positions.
    SurfaceParameter Parameter{}; ///< Patch index and exact dyadic (U, V) of the vertex.
    GeometryStatus NormalStatus = GeometryStatus::Success; ///< Success, Degenerate or IllConditioned.
};

/// @brief Grid and error record of one tessellated patch.
struct SurfaceMeshPatch final
{
    usize LevelU = 0;                  ///< 2^LevelU cells along u.
    usize LevelV = 0;                  ///< 2^LevelV cells along v.
    usize FirstVertex = 0;             ///< First render vertex; vertex (i, j) is FirstVertex + i * (2^LevelV + 1) + j.
    usize VertexCount = 0;             ///< (2^LevelU + 1) * (2^LevelV + 1).
    usize FirstIndex = 0;              ///< First entry of this patch's triangles in SurfaceMeshView::Indices.
    usize IndexCount = 0;              ///< 6 * 2^LevelU * 2^LevelV.
    float64 PositionBound = 0.0;       ///< Certified largest distance between a cell and its two triangles.
    float64 NormalAngleEstimate = 0.0; ///< Estimated largest sampled normal angle in a cell, radians.
    bool IsWithinTolerance = false;    ///< Both bounds fit the profile; false only for an accepted coarser grid.
};

/// @brief Achieved accuracy, degeneracy counts and work of a built mesh.
struct SurfaceMeshReport final
{
    float64 RequestedPositionError = 0.0;                         ///< Profile PositionError.
    float64 PositionBound = 0.0;                                  ///< Largest patch PositionBound.
    GuaranteeClass PositionGuarantee = GuaranteeClass::Certified; ///< Class of PositionBound.
    bool IsWithinPositionError = false;                           ///< PositionBound <= RequestedPositionError.
    float64 RequestedNormalAngleError = 0.0;                      ///< Profile NormalAngleError; 0 if not requested.
    bool HasNormalAngleEstimate = false;                          ///< Whether normals were sampled for refinement.
    float64 NormalAngleEstimate = 0.0;                            ///< Largest patch NormalAngleEstimate.
    GuaranteeClass NormalGuarantee = GuaranteeClass::Estimated;   ///< Class of NormalAngleEstimate.
    usize DegenerateNormalCount = 0;                              ///< Vertices whose normal is Degenerate.
    usize IllConditionedNormalCount = 0;                          ///< Vertices whose normal is IllConditioned.
    usize CollapsedTriangleCount = 0; ///< Triangles with two bitwise-equal vertex positions.
    usize CellTestCount = 0;          ///< Cell bound evaluations performed.
    usize DeepestLevel = 0;           ///< Largest LevelU or LevelV.
};

/// @brief Borrowed view of a SurfaceMesh; valid while the mesh is alive and unmodified.
struct SurfaceMeshView final
{
    BorrowedView<const SurfaceMeshPosition> Positions; ///< Corners, then edge samples, then patch interiors.
    BorrowedView<const SurfaceMeshVertex> Vertices;    ///< Render vertices, grouped by patch.
    /// Triangle vertex indices, three per triangle, counterclockwise in (u, v): (00, 10, 11) and (00, 11, 01).
    BorrowedView<const core::uint32> Indices;
    BorrowedView<const SurfaceMeshPatch> Patches; ///< One record per surface patch.
    BorrowedView<const usize> EdgeLevels;         ///< Dyadic level of each surface edge, shared by its incidences.
    uint64 SurfaceRevision = 0;                   ///< Revision of the surface the mesh was built from.
    SurfaceMeshReport Report{};                   ///< Achieved accuracy and counts.
};

/// @brief Staging owner of one surface mesh; move-only.
///
/// Built only by TryBuildSurfaceMesh, which fills a complete candidate before
/// moving it into the output, so a failed rebuild keeps the previous mesh.
/// Consumers copy or upload from the view. Single owner; synchronize
/// replacement externally.
class SurfaceMesh final
{
public:
    /// Construct an empty mesh.
    SurfaceMesh() noexcept = default;
    /// Transfer the mesh; the source becomes empty.
    SurfaceMesh(SurfaceMesh&& other) noexcept = default;
    /// Release this mesh and take the other's; the source becomes empty.
    SurfaceMesh& operator=(SurfaceMesh&& other) noexcept = default;
    /// Copying is disabled: one owner holds the mesh storage.
    SurfaceMesh(const SurfaceMesh&) = delete;
    /// Copy assignment is disabled: one owner holds the mesh storage.
    SurfaceMesh& operator=(const SurfaceMesh&) = delete;
    /// Release the mesh storage; views into it become invalid.
    ~SurfaceMesh() = default;

    /// Borrow the mesh; invalidated by moving or destroying it.
    [[nodiscard]] SurfaceMeshView GetView() const noexcept;
    /// Whether no mesh has been built into this owner.
    [[nodiscard]] bool IsEmpty() const noexcept;

private:
    friend GeometryStatus TryBuildSurfaceMesh(const SurfaceView& surface,
                                              const SurfaceMeshProfile& profile,
                                              SurfaceMesh& outMesh,
                                              GeometryDiagnostic* outDiagnosticOrNull) noexcept;

    core::Array<SurfaceMeshPosition> mPositions;
    core::Array<SurfaceMeshVertex> mVertices;
    core::Array<core::uint32> mIndices;
    core::Array<SurfaceMeshPatch> mPatches;
    core::Array<usize> mEdgeLevels;
    uint64 mSurfaceRevision = 0;
    SurfaceMeshReport mReport{};
};

/// @brief Select dyadic grid levels, verify their Certified error bounds and build a crack-free mesh.
///
/// Every patch starts at level (0, 0). While its largest cell bound misses the
/// profile, the direction whose refinement gives the clearly smaller normalized
/// bound is refined. When the two candidates agree within a relative 1e-9 (for
/// example mirror-symmetric patches whose enclosures differ only by rounding), the
/// coarser direction is refined, and u when both levels are equal, so isotropic
/// patches stay balanced. Each shared edge then takes the larger of its two
/// along-edge levels; patches whose levels changed are rechecked and refined
/// again until all shared edges agree. Corner positions are the stored controls;
/// edge samples are evaluated once on the canonical curve; interior samples on
/// their patch. Each emitted position is the float32 rounding of an outward
/// enclosure, and its rounding error is part of every cell bound.
/// @param surface Borrowed prepared surface; its Revision is recorded in the mesh.
/// @param profile Accuracy, precision, degeneracy policy and budgets.
/// @param outMesh Replaced by the new mesh only on Success.
/// @param outDiagnosticOrNull Optional; on BudgetExceeded it receives the achieved Certified bound and the patch.
/// @return Success; InvalidArgument for an empty surface, an invalid profile or a coarser-mesh request on a
/// Strict profile; NonFiniteInput; UnsupportedPrecision when PositionError is below the surface floor or a Strict
/// profile requests a normal bound; BudgetExceeded when MaxLevel (without an accepted coarser mesh),
/// MaxCellTests, MaxVertices, MaxTriangles or MaxOutputBytes is exhausted; Degenerate or IllConditioned under
/// SurfaceDegeneracyPolicy::Reject; OutOfRange when an enclosure overflows or the vertex count exceeds the
/// uint32 index range; OutOfMemory. outMesh is replaced only on Success.
[[nodiscard]] GeometryStatus TryBuildSurfaceMesh(const SurfaceView& surface,
                                                 const SurfaceMeshProfile& profile,
                                                 SurfaceMesh& outMesh,
                                                 GeometryDiagnostic* outDiagnosticOrNull) noexcept;
} // namespace ludus::foundation::geometry
