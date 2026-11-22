#pragma once

// Contact generation internals: shapes in a pair frame, penetration kernels, feature clipping and patch reduction.
//
// A pair is evaluated in one *pair frame*. Two convex colliders use world axes with the first collider's position as
// origin, so results do not depend on where in the world the pair sits. A convex collider against a triangle mesh
// uses the mesh's local frame. Every shape is a polytope core plus a rounding margin, as in convex_support.h.
//
// The steps are: a kernel finds the contact normal and the smallest separation (FindPenetration); the shapes' support
// features along that normal are clipped against each other into contact points (BuildManifold); and the points of
// each patch are reduced to at most MAX_CONTACT_PATCH_POINTS (PairAccumulator).

#include "convex_support.h"
#include "mesh_arithmetic.h"

#include <ludus/foundation/base/core.h>

#include <ludus/foundation/containers/static_array.hpp>
#include <ludus/foundation/math/vector.hpp>
#include <ludus/physics/collision/contacts.h>
#include <ludus/physics/collision/convex_shape.h>
#include <ludus/physics/collision/queries.h>
#include <ludus/physics/collision/triangle_mesh.h>

namespace ludus::physics::collision::internal
{
inline constexpr usize CONTACT_TRIANGLE_CORNERS = 3;
/// Most vertices of one support feature. A hull face with more vertices is thinned to this many of them.
inline constexpr usize MAX_FEATURE_VERTICES = 32;
/// Most points one manifold holds before reduction: clipping a feature against another adds at most one vertex per
/// clipping edge.
inline constexpr usize MAX_MANIFOLD_POINTS = 2 * MAX_FEATURE_VERTICES;
/// Directions in the patch plane along which a crowded patch keeps its farthest point.
inline constexpr usize PATCH_EXTREME_DIRECTIONS = 8;
/// Most points a crowded patch keeps: one per direction and the deepest.
inline constexpr usize PATCH_EXTREME_POINTS = PATCH_EXTREME_DIRECTIONS + 1;
/// Points a patch collects before it is thinned in place while a mesh pair is still being traversed: room for a
/// thinned patch and a thinned manifold.
inline constexpr usize PATCH_SCRATCH_POINTS = 2 * PATCH_EXTREME_POINTS;
/// Every geometric tolerance of contact generation is this ratio times the pair scale (GetPairScale): far above
/// float64 rounding of the pair's coordinates and far below any length a simulation resolves.
inline constexpr float64 CONTACT_RELATIVE_TOLERANCE = 1.0e-9;

/// One shape of a pair, in the pair frame: a posed convex core, or one mesh triangle.
struct ContactShape final
{
    PosedConvex Convex{};      ///< Core, axes in the pair frame; unused for a triangle.
    math::Vector3d Position{}; ///< Shape origin in the pair frame; zero for a triangle.
    bool IsTriangle = false;   ///< Whether Corners and TriangleNormal describe the shape instead of Convex.
    foundation::StaticArray<math::Vector3d, CONTACT_TRIANGLE_CORNERS> Corners{}; ///< Triangle corners.
    math::Vector3d TriangleNormal{}; ///< Unit winding normal of the triangle.
};

/// The core point farthest along `direction`, in the pair frame. Ties follow GetCoreSupport; a triangle keeps its
/// lowest corner.
[[nodiscard]] math::Vector3d GetContactSupport(const ContactShape& shape, math::Vector3d direction) noexcept;
/// Rounding radius of the shape; zero for boxes, hulls and triangles.
[[nodiscard]] float64 GetContactMargin(const ContactShape& shape) noexcept;
/// A point of the core, strictly inside it when the core has volume.
[[nodiscard]] math::Vector3d GetContactInterior(const ContactShape& shape) noexcept;
/// An upper bound on the distance from the interior point to any point of the rounded shape.
[[nodiscard]] float64 GetContactExtent(const ContactShape& shape) noexcept;
/// The length every tolerance of one pair is relative to.
[[nodiscard]] float64 GetPairScale(const ContactShape& first, const ContactShape& second) noexcept;

/// Two unit vectors that complete a unit normal to a right-handed frame: `First x Second = normal`.
struct ContactTangents final
{
    math::Vector3d First{};
    math::Vector3d Second{};
};

/// The tangents of the unit `normal`: the first is perpendicular to the frame axis the normal is least aligned with.
[[nodiscard]] ContactTangents MakeContactTangents(math::Vector3d normal) noexcept;

/// The part of a core that supports it along one direction: a vertex, an edge or a convex face.
struct FeaturePolygon final
{
    /// Vertices in the pair frame; a face is counter-clockwise about Normal.
    foundation::StaticArray<math::Vector3d, MAX_FEATURE_VERTICES> Vertices{};
    foundation::StaticArray<ContactFeature, MAX_FEATURE_VERTICES> VertexFeatures{}; ///< Feature of each vertex.
    /// Feature of the edge from vertex `i` to vertex `(i + 1) % Count`.
    foundation::StaticArray<ContactFeature, MAX_FEATURE_VERTICES> EdgeFeatures{};
    ContactFeature FaceFeature = 0; ///< Feature of the face when Count >= 3.
    math::Vector3d Normal{};        ///< Outward unit normal of a face; the query direction otherwise.
    uint32 Count = 0;               ///< One for a vertex, two for an edge, three or more for a face.
};

/// @brief Find the feature of a core that lies within `tolerance` of its support plane along the unit `direction`.
///
/// A corner is part of the feature when it lies at most `tolerance` behind the farthest corner. A hull caps the
/// tolerance at a fraction of its size so that its feature stays close to a real face.
void FindSupportFeature(const ContactShape& shape,
                        math::Vector3d direction,
                        float64 tolerance,
                        FeaturePolygon& outFeature) noexcept;

/// The contact normal and smallest separation of a pair.
struct Penetration final
{
    bool HasContact = false;      ///< Whether the surfaces are within the contact offset.
    math::Vector3d Normal{};      ///< Unit normal from the first shape toward the second.
    float64 Separation = 0.0;     ///< Smallest signed gap between the rounded surfaces along Normal.
    math::Vector3d FirstPoint{};  ///< A deepest point of the first core.
    math::Vector3d SecondPoint{}; ///< The matching point of the second core.
    bool IsApproximate = false;   ///< The penetration search stopped at a bound.
};

/// @brief Find the contact normal and separation of two shapes with the kernel for their kinds.
///
/// Sphere and capsule pairs are closed form; a box against a sphere, capsule, box or triangle uses closest features
/// and the separating axis test; any pair with a hull uses FindGeneralPenetration. A triangle may only be `second`.
void FindPenetration(const ContactShape& first,
                     const ContactShape& second,
                     float64 contactOffset,
                     Penetration& outPenetration) noexcept;

/// The general convex path: GJK distance on the cores, and expanding-polytope penetration when the cores overlap.
/// At least one core must have volume.
void FindGeneralPenetration(const ContactShape& first,
                            const ContactShape& second,
                            float64 contactOffset,
                            Penetration& outPenetration) noexcept;

/// A segment between two points; a point when they coincide.
struct ContactSegment final
{
    math::Vector3d Start{};
    math::Vector3d End{};
};

/// Closest points of two segments.
struct SegmentPair final
{
    math::Vector3d FirstPoint{};   ///< Closest point on the first segment.
    math::Vector3d SecondPoint{};  ///< Closest point on the second segment.
    float64 FirstParameter = 0.0;  ///< Position of FirstPoint along the first segment, in [0, 1].
    float64 SecondParameter = 0.0; ///< Position of SecondPoint along the second segment, in [0, 1].
    bool IsParallel = false;       ///< The segments are parallel within rounding, or one of them is a point.
};

/// Closest points of two segments; either may be a point.
[[nodiscard]] SegmentPair FindClosestSegmentPoints(const ContactSegment& first, const ContactSegment& second) noexcept;

/// The point of a triangle closest to `point`.
[[nodiscard]] math::Vector3d
FindClosestTrianglePoint(const foundation::StaticArray<math::Vector3d, CONTACT_TRIANGLE_CORNERS>& corners,
                         math::Vector3d point) noexcept;

/// One contact point of a manifold, in the pair frame.
struct ManifoldPoint final
{
    math::Vector3d Position{};                     ///< Midpoint between the two surface points.
    float64 Separation = 0.0;                      ///< Signed gap along the manifold normal.
    ContactFeature FirstFeature = 0;               ///< Feature on the first shape.
    ContactFeature SecondFeature = 0;              ///< Feature on the second shape.
    uint32 TriangleIndex = INVALID_TRIANGLE_INDEX; ///< Source triangle of a mesh contact.
};

/// The contact points of two convex shapes, or of a convex shape and one triangle, before reduction.
struct Manifold final
{
    math::Vector3d Normal{}; ///< Unit normal from the first shape toward the second.
    foundation::StaticArray<ManifoldPoint, MAX_MANIFOLD_POINTS> Points{};
    uint32 Count = 0;
    bool IsApproximate = false;
};

/// @brief Clip the two shapes' support features along the penetration normal into contact points.
///
/// Points further apart than the contact offset are dropped. When clipping leaves nothing, the kernel's deepest
/// point is kept, unless `shouldDropWhenClippedAway` asks for an empty manifold instead: a triangle whose inactive
/// edge was hit leaves the contact to the neighbour that owns the surface under the shape.
void BuildManifold(const ContactShape& first,
                   const ContactShape& second,
                   const Penetration& penetration,
                   float64 contactOffset,
                   bool shouldDropWhenClippedAway,
                   Manifold& outManifold) noexcept;

/// @brief Reduce `count` coplanar-normal points in place to at most MAX_CONTACT_PATCH_POINTS and return the new
/// count.
///
/// Coincident points are merged first. The kept points are the deepest one, the one farthest from it, and the two
/// that add the most area on either side of that diagonal; they keep their relative order. Points on one line
/// reduce to its two ends, and a point inside the triangle of three others is dropped.
[[nodiscard]] uint32
ReduceContactPoints(ManifoldPoint* points, uint32 count, math::Vector3d normal, float64 scale) noexcept;

/// @brief Thin `count` points in place to the deepest one and the farthest one along each of
/// PATCH_EXTREME_DIRECTIONS directions in the plane of `normal`, and return the new count.
///
/// Unlike ReduceContactPoints the choice does not depend on which points arrived first, so a patch that is thinned
/// while its triangles are still being visited keeps the corners that the final reduction needs.
[[nodiscard]] uint32
KeepExtremeContactPoints(ManifoldPoint* points, uint32 count, math::Vector3d normal, float64 scale) noexcept;

/// One patch of a pair while its points are still being collected.
struct PatchAccumulator final
{
    math::Vector3d Normal{};
    foundation::StaticArray<ManifoldPoint, PATCH_SCRATCH_POINTS> Points{};
    uint32 Count = 0;
    float64 DeepestSeparation = 0.0;
    bool IsApproximate = false;
};

/// The patches of one pair while its manifolds are still being collected.
struct PairAccumulator final
{
    foundation::StaticArray<PatchAccumulator, MAX_CONTACT_PAIR_PATCHES> Patches{};
    uint32 PatchCount = 0;
    uint32 GeneratedPointCount = 0;
    uint32 TriangleTestCount = 0;
    uint32 DiscardedPatchCount = 0;
    float64 Scale = 0.0; ///< Largest pair scale seen; merge distances are relative to it.
};

/// Add one manifold to the patch whose normal is compatible with it, or to a new patch.
void AddManifold(const Manifold& manifold, float64 scale, PairAccumulator& inoutPair) noexcept;

/// @brief Collect the contacts of a convex shape, posed in a mesh's local frame, against the mesh's triangles.
///
/// Normals are kept from the convex shape toward the triangle, and features with the convex shape first; the caller
/// mirrors the result when the mesh is the pair's first collider.
void CollectMeshContacts(const TriangleMesh& mesh,
                         const ContactShape& convex,
                         float64 contactOffset,
                         PairAccumulator& inoutPair) noexcept;
} // namespace ludus::physics::collision::internal
