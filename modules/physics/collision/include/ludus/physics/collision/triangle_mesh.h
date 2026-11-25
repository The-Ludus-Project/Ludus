#pragma once

/// @file
/// @brief Immutable static triangle-mesh collision geometry with a bounded hierarchy and closest-hit queries.
///
/// See docs/architecture/physics-collision.md for cooking rules, limits, query semantics and cost.

#include <ludus/foundation/base/core.h>

#include <ludus/foundation/containers/array.hpp>
#include <ludus/foundation/containers/borrowed_view.hpp>
#include <ludus/foundation/math/geometry.hpp>
#include <ludus/foundation/math/vector.hpp>
#include <ludus/physics/collision/queries.h>
#include <ludus/physics/collision/status.h>

namespace ludus::physics::collision
{
/// Largest number of source triangles one mesh admits (the Explore as Player preview bound).
///
/// A cooked mesh stores at most 80 bytes per triangle: 44 for the triangle with its feature flags and at most 36 for
/// hierarchy nodes.
inline constexpr uint32 MAX_TRIANGLE_MESH_TRIANGLES = 8000000;

/// Largest hierarchy depth, counting the root as depth one. The builder's balanced split keeps every child at
/// most three quarters of its parent, so MAX_TRIANGLE_MESH_TRIANGLES needs at most 50 levels.
inline constexpr uint32 MAX_TRIANGLE_MESH_HIERARCHY_DEPTH = 64;

/// @brief Scale-free degeneracy bound: a triangle is degenerate when twice its area is at most this ratio times
/// the square of its longest edge.
///
/// The ratio is the sine of the smallest admitted apex angle of a thin triangle; coincident or collinear corners
/// always fail it.
inline constexpr float64 MIN_TRIANGLE_SHAPE_RATIO = 1.0e-7;

/// Default of TriangleMeshCookSettings::ActiveEdgeCosine: the cosine of five degrees.
inline constexpr float64 DEFAULT_ACTIVE_EDGE_COSINE = 0.99619469809174555;

/// @brief Which triangle edges and corners generate their own contact normals, on each side of the triangle.
///
/// Cooking classifies every edge from the triangles that share it (see InternalEdgePolicy). A contact against an
/// *active* edge or corner may push along that feature's own direction; a contact against an inactive one uses the
/// triangle's face normal, so a shape sliding across the interior of a flat or concave surface never catches on the
/// seams between triangles. The front side is the side the winding normal points to. Edge `i` runs from corner `i`
/// to corner `(i + 1) % 3`. Read the bits with IsTriangleEdgeActive and IsTriangleCornerActive.
using TriangleFeatureFlags = uint16;

/// Number of corners, and of edges, of a triangle.
inline constexpr uint32 TRIANGLE_FEATURE_COUNT = 3;
/// First bit of the three "edge active on the front side" flags in TriangleFeatureFlags.
inline constexpr uint32 TRIANGLE_FRONT_EDGE_SHIFT = 0;
/// First bit of the three "edge active on the back side" flags.
inline constexpr uint32 TRIANGLE_BACK_EDGE_SHIFT = 3;
/// First bit of the three "corner active on the front side" flags.
inline constexpr uint32 TRIANGLE_FRONT_CORNER_SHIFT = 6;
/// First bit of the three "corner active on the back side" flags.
inline constexpr uint32 TRIANGLE_BACK_CORNER_SHIFT = 9;
/// Every edge and corner active on both sides: the conservative classification.
inline constexpr TriangleFeatureFlags ALL_TRIANGLE_FEATURES_ACTIVE = 0x0FFF;

/// @brief Whether edge `edge` (0 to 2) is active on the front side (`isFrontSide`) or the back side.
[[nodiscard]] constexpr bool IsTriangleEdgeActive(TriangleFeatureFlags flags, uint32 edge, bool isFrontSide) noexcept
{
    const uint32 shift = isFrontSide == true ? TRIANGLE_FRONT_EDGE_SHIFT : TRIANGLE_BACK_EDGE_SHIFT;
    return ((static_cast<uint32>(flags) >> (shift + edge)) & 1U) != 0;
}

/// @brief Whether corner `corner` (0 to 2) is active on the front side (`isFrontSide`) or the back side.
[[nodiscard]] constexpr bool
IsTriangleCornerActive(TriangleFeatureFlags flags, uint32 corner, bool isFrontSide) noexcept
{
    const uint32 shift = isFrontSide == true ? TRIANGLE_FRONT_CORNER_SHIFT : TRIANGLE_BACK_CORNER_SHIFT;
    return ((static_cast<uint32>(flags) >> (shift + corner)) & 1U) != 0;
}

/// @brief How cooking classifies triangle edges for contact generation.
enum class InternalEdgePolicy : uint8
{
    /// Find the triangles that share each edge by their source indices and classify the edge:
    ///
    /// - An edge of one triangle (a boundary), of three or more triangles (nonmanifold), or of two triangles with
    ///   inconsistent winding is active on both sides.
    /// - An edge of two consistently wound triangles whose normals differ by less than
    ///   TriangleMeshCookSettings::ActiveEdgeCosine allows is inactive on both sides.
    /// - Any other edge is active only on its convex side: the front for a ridge, the back for a valley.
    ///
    /// A corner is active on a side when any edge of the mesh that ends at its source vertex is active on that side.
    /// Vertices are matched by index, never by position, so an unwelded seam stays a pair of active boundaries.
    /// Classification allocates temporary storage of eight bytes per triangle edge plus five bytes per referenced
    /// vertex during the cook.
    Classify,
    /// Mark every edge and corner active on both sides without looking for neighbours. Queries are unaffected;
    /// contacts may catch on interior seams.
    KeepAll,
};

/// @brief What cooking does with a degenerate triangle.
enum class DegenerateTrianglePolicy : uint8
{
    Reject, ///< Fail cooking with Status::DegenerateTriangle, naming the first degenerate triangle.
    Skip,   ///< Leave degenerate triangles out and count them; other triangles keep their source indices.
};

/// @brief Borrowed source geometry for cooking, in mesh space and meters.
///
/// Every three consecutive indices form one triangle. The caller keeps both views alive and unchanged for the
/// duration of the cook call only; the cooked mesh copies what it keeps.
struct TriangleMeshSource final
{
    foundation::BorrowedView<const foundation::math::Vector3> PositionsMesh; ///< Vertex positions.
    foundation::BorrowedView<const uint32> Indices; ///< Triangle corner indices into PositionsMesh.
};

/// @brief Cooking options.
struct TriangleMeshCookSettings final
{
    /// Treatment of triangles that fail MIN_TRIANGLE_SHAPE_RATIO. Rejecting is the default so that unusable
    /// geometry is reported rather than silently dropped.
    DegenerateTrianglePolicy DegeneratePolicy = DegenerateTrianglePolicy::Reject;
    /// How edges are classified for contact generation. Skipped degenerate triangles are no one's neighbour.
    InternalEdgePolicy EdgePolicy = InternalEdgePolicy::Classify;
    /// Under InternalEdgePolicy::Classify, an edge between two consistently wound triangles is a crease, active on
    /// its convex side, when the cosine of the angle between their normals is below this value; in [-1, 1].
    float64 ActiveEdgeCosine = DEFAULT_ACTIVE_EDGE_COSINE;
};

/// @brief Outcome of TriangleMesh::TryCook.
///
/// TriangleIndex names the first offending source triangle for the per-triangle failures (IndexOutOfRange,
/// NonFiniteInput, OutOfRange, DegenerateTriangle) and is INVALID_TRIANGLE_INDEX otherwise.
struct [[nodiscard]] TriangleMeshCookResult final
{
    Status Result = Status::Success;               ///< Success, or why the mesh was not cooked.
    uint32 TriangleIndex = INVALID_TRIANGLE_INDEX; ///< Offending source triangle, when attributable.
};

/// @brief Immutable, cooked static triangle mesh in its own local space.
///
/// A mesh is created only by TryCook. It owns a copy of its admitted triangles and a bounding volume hierarchy
/// (BVH) whose leaves hold at most eight triangles; neither changes afterwards. Queries are `const`, perform no
/// allocation and never modify the mesh, so any number of threads may query one mesh concurrently while no thread
/// replaces or destroys it. A default-constructed or moved-from mesh holds no geometry; queries against it return
/// Status::NoGeometry.
///
/// @note Static environment geometry only: the mesh has no mass properties and is not a dynamic shape.
class TriangleMesh final
{
public:
    /// Construct an empty, uncooked mesh. Does not allocate.
    TriangleMesh() noexcept = default;
    /// Meshes are move-only so that a cooked mesh has exactly one owner.
    TriangleMesh(const TriangleMesh&) = delete;
    /// Meshes are move-only so that a cooked mesh has exactly one owner.
    TriangleMesh& operator=(const TriangleMesh&) = delete;
    /// Transfer the cooked geometry; `other` becomes empty.
    TriangleMesh(TriangleMesh&& other) noexcept = default;
    /// Release this mesh's geometry and take `other`'s; `other` becomes empty.
    TriangleMesh& operator=(TriangleMesh&& other) noexcept = default;
    /// Release the cooked geometry.
    ~TriangleMesh() noexcept = default;

    /// @brief Validate and cook borrowed triangles into a new mesh.
    ///
    /// Validation runs before any allocation and visits triangles in source order: the index count must be a
    /// multiple of three; the triangle count must be in [1, MAX_TRIANGLE_MESH_TRIANGLES]; every index must refer
    /// to a position; every referenced coordinate must be finite and within MAX_COORDINATE_METERS; and degenerate
    /// triangles follow `settings.DegeneratePolicy`. Cooking then copies the admitted triangles, classifies their
    /// edges under `settings.EdgePolicy` and builds the hierarchy. The result is deterministic for identical inputs.
    /// @param source Borrowed positions and indices, read only during this call.
    /// @param settings Degenerate-triangle policy, edge policy and crease threshold; an unknown policy or a
    /// threshold that is not finite or lies outside [-1, 1] fails with InvalidArgument.
    /// @param outMesh Receives the new mesh on success. Unchanged on any failure, including allocation failure.
    /// @return Success, or the failing status with the first offending triangle when attributable.
    [[nodiscard]] static TriangleMeshCookResult
    TryCook(const TriangleMeshSource& source, const TriangleMeshCookSettings& settings, TriangleMesh& outMesh) noexcept;

    /// @brief Find the nearest triangle along a ray.
    ///
    /// Uses a watertight ray/triangle test, so a ray through a shared edge or vertex of adjacent triangles hits at
    /// least one of them. A ray parallel to a triangle's plane misses that triangle, including a ray lying in it.
    /// Hits at distance zero (an origin on a triangle) and at exactly MaxDistanceMeters are admitted.
    /// @param input Ray, distance limit, face policy and traversal mode; see RaycastInput for admitted values.
    /// @param outResult Receives the hit, or an explicit miss, on Success. Unchanged on any failure.
    /// @return Success, NoGeometry for an uncooked mesh, or the validation failure.
    [[nodiscard]] Status RaycastClosest(const RaycastInput& input, RaycastResult& outResult) const noexcept;

    /// @brief Find the first triangle an axis-aligned box contacts while translating.
    ///
    /// Uses the separating axis test on the 13 axes of a box and a triangle. Triangles are two-sided. The
    /// initial-overlap policy decides whether starting overlaps are reported, ignored or rejected.
    /// @param input Box, translation, overlap policy and traversal mode; see BoxCastInput for admitted values.
    /// @param outResult Receives the hit, or an explicit miss, on Success. Unchanged on any failure.
    /// @return Success, InitialOverlap under InitialOverlapPolicy::Reject, NoGeometry for an uncooked mesh, or the
    /// validation failure.
    [[nodiscard]] Status CastBoxClosest(const BoxCastInput& input, BoxCastResult& outResult) const noexcept;

    /// @brief Find the first triangle a posed convex shape contacts while translating.
    ///
    /// The general convex path: a GJK ray cast on the Minkowski difference of the shape and each candidate triangle,
    /// with spheres and capsules cast as a point or segment core rounded by their radius. Triangles are two-sided.
    /// CastBoxClosest remains the exact separating-axis kernel for axis-aligned boxes; this cast agrees with it
    /// within the contact tolerance documented on ConvexCastResult.
    /// @param input Shape, pose, translation, overlap policy and traversal mode; see ConvexCastInput.
    /// @param outResult Receives the hit, or an explicit miss, on Success. Unchanged on any failure.
    /// @return Success, InitialOverlap under InitialOverlapPolicy::Reject, NoGeometry for an uncooked mesh,
    /// NotConverged when one triangle exhausts MAX_CONVEX_CAST_ITERATIONS, or the validation failure.
    [[nodiscard]] Status CastConvexClosest(const ConvexCastInput& input, ConvexCastResult& outResult) const noexcept;

    /// Whether the mesh holds cooked geometry.
    [[nodiscard]] LUDUS_INLINE bool IsCooked() const noexcept
    {
        return mNodes.IsEmpty() == false;
    }
    /// Number of admitted (non-skipped) triangles; zero for an uncooked mesh.
    [[nodiscard]] LUDUS_INLINE usize GetTriangleCount() const noexcept
    {
        return mTriangles.GetSize();
    }
    /// @brief Read the feature classification of one admitted source triangle.
    ///
    /// A diagnostic for tests and debug views: it searches the cooked triangles linearly.
    /// @param sourceTriangleIndex Index of the triangle in the cook source.
    /// @param outFlags Receives the flags on success; unchanged on failure.
    /// @return Success; NoGeometry for an uncooked mesh; or IndexOutOfRange when no admitted triangle has that
    /// source index (it was skipped as degenerate, or lies past the end of the source).
    [[nodiscard]] Status TryGetTriangleFeatures(uint32 sourceTriangleIndex,
                                                TriangleFeatureFlags& outFlags) const noexcept;
    /// Number of degenerate source triangles left out under DegenerateTrianglePolicy::Skip.
    [[nodiscard]] LUDUS_INLINE uint32 GetSkippedTriangleCount() const noexcept
    {
        return mSkippedTriangleCount;
    }
    /// Number of hierarchy nodes; at most GetTriangleCount() for a cooked mesh.
    [[nodiscard]] LUDUS_INLINE usize GetNodeCount() const noexcept
    {
        return mNodes.GetSize();
    }
    /// Hierarchy depth, root counted as one; at most MAX_TRIANGLE_MESH_HIERARCHY_DEPTH. Zero when uncooked.
    [[nodiscard]] LUDUS_INLINE uint32 GetHierarchyDepth() const noexcept
    {
        return mHierarchyDepth;
    }
    /// Exact bounds of the admitted triangles; the canonical empty box when uncooked.
    [[nodiscard]] LUDUS_INLINE foundation::math::Aabb3 GetBoundsMesh() const noexcept
    {
        return mNodes.IsEmpty() == true ? foundation::math::EmptyAabb3() : mNodes[0].BoundsMesh;
    }

private:
    /// One admitted triangle and its index in the source.
    struct Triangle final
    {
        foundation::math::Vector3 FirstMesh;                          ///< First corner in mesh space, meters.
        foundation::math::Vector3 SecondMesh;                         ///< Second corner in mesh space, meters.
        foundation::math::Vector3 ThirdMesh;                          ///< Third corner in mesh space, meters.
        uint32 SourceIndex = 0;                                       ///< Index of the triangle in the cook source.
        TriangleFeatureFlags Features = ALL_TRIANGLE_FEATURES_ACTIVE; ///< Active edges and corners per side.
    };
    /// Preorder hierarchy node: an internal node's left child follows it; Right names the right child.
    struct Node final
    {
        foundation::math::Aabb3 BoundsMesh; ///< Exact bounds of every triangle below this node.
        uint32 First = 0;                   ///< First triangle of a leaf in the cooked triangle array.
        uint32 Count = 0;                   ///< Triangles in a leaf; zero marks an internal node.
        uint32 Right = 0;                   ///< Right child of an internal node; unused by a leaf.
    };

    friend class TriangleMeshBuilder;
    friend class TriangleMeshRaycaster;
    friend class TriangleMeshBoxCaster;
    friend class TriangleMeshConvexCaster;
    friend class TriangleMeshContacts;

    foundation::Array<Triangle> mTriangles;
    foundation::Array<Node> mNodes;
    uint32 mSkippedTriangleCount = 0;
    uint32 mHierarchyDepth = 0;
};
} // namespace ludus::physics::collision
