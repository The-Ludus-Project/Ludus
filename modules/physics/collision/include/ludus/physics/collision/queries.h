#pragma once

/// @file
/// @brief Input and result values for PhysicsCollision closest-hit queries.
///
/// Queries run in the local space of the geometry they test (suffix `Mesh`), in meters. The contracts follow
/// "Ray, shape and overlap queries" in docs/architecture/physics-systems.md; the triangle-mesh specifics are in
/// docs/architecture/physics-collision.md.

#include <ludus/foundation/base/types.h>

#include <ludus/foundation/math/quaternion.hpp>
#include <ludus/foundation/math/vector.hpp>
#include <ludus/physics/collision/convex_shape.h>
#include <ludus/physics/collision/status.h>

namespace ludus::physics::collision
{
/// Largest admitted deviation of a ray direction's squared length from one.
inline constexpr float64 UNIT_DIRECTION_SQUARED_TOLERANCE = 1.0e-6;

/// Largest admitted deviation of a cast orientation's squared norm from one. A float32 quaternion normalized in
/// float32 deviates by about 1e-7; the cast renormalizes the admitted value in float64.
inline constexpr float64 UNIT_QUATERNION_SQUARED_TOLERANCE = 1.0e-5;

/// @brief Convex cast contact tolerance relative to the query's scale.
///
/// The scale is the largest coordinate magnitude, relative to the shape's starting position, of the mesh bounds, plus
/// the largest translation component and the shape's largest posed half extent. Shapes closer than the tolerance
/// count as in contact; see ConvexCastResult.
inline constexpr float64 CONVEX_CAST_RELATIVE_TOLERANCE = 1.0e-9;

/// Most support iterations a convex cast spends on one triangle before failing with Status::NotConverged. Polytope
/// cores converge in finitely many steps; the bound only stops numerical cycling.
inline constexpr uint32 MAX_CONVEX_CAST_ITERATIONS = 128;

/// @brief Which triangle faces a ray may hit.
///
/// A front face is one whose corners appear counterclockwise from the side the ray comes from, so that the ray
/// direction points against the right-handed winding normal `Cross(second - first, third - first)`.
enum class TriangleFacePolicy : uint8
{
    TwoSided,       ///< Hit front and back faces alike.
    FrontFacesOnly, ///< Hit only faces whose winding normal points against the ray direction.
    BackFacesOnly,  ///< Hit only faces whose winding normal points along the ray direction.
};

/// @brief What a cast does with triangles that already overlap the shape at its starting pose.
///
/// Touching (a shared boundary with no shared interior) is not an overlap.
enum class InitialOverlapPolicy : uint8
{
    Report, ///< Report the overlap as a hit at fraction zero, with no surface normal.
    Ignore, ///< Exclude initially overlapping triangles from the whole cast.
    Reject, ///< Fail the query with Status::InitialOverlap when any triangle overlaps initially.
};

/// @brief How a query visits candidate triangles.
///
/// Both modes run the same per-triangle arithmetic and the same closest/tie ordering, so their results are
/// identical; the brute-force mode exists as the correctness oracle for hierarchy traversal.
enum class QueryTraversal : uint8
{
    Hierarchy,    ///< Traverse the bounding volume hierarchy, pruning only provably farther nodes.
    AllTriangles, ///< Test every triangle without pruning. Linear cost; intended for tests and diagnostics.
};

/// @brief Closest raycast request in mesh space.
///
/// Admitted values: a finite origin with components within MAX_COORDINATE_METERS; a unit direction (squared
/// length within UNIT_DIRECTION_SQUARED_TOLERANCE of one); MaxDistanceMeters in (0, MAX_QUERY_DISTANCE_METERS].
/// A zero distance or a zero direction is a zero-length ray and is rejected with Status::InvalidArgument.
struct RaycastInput final
{
    foundation::math::Vector3d OriginMesh{};                      ///< Ray origin in mesh space, meters.
    foundation::math::Vector3d DirectionMesh{0.0, 0.0, -1.0};     ///< Unit ray direction in mesh space.
    float64 MaxDistanceMeters = 0.0;                              ///< Inclusive distance limit along the ray.
    TriangleFacePolicy FacePolicy = TriangleFacePolicy::TwoSided; ///< Faces the ray may hit.
    QueryTraversal Traversal = QueryTraversal::Hierarchy;         ///< Hierarchy or brute-force oracle traversal.
};

/// @brief Closest raycast answer.
///
/// On a miss `HasHit` is false and every other field keeps its default value. On a hit the fields describe the
/// nearest admitted triangle; equal distances resolve to the lowest source triangle index.
struct RaycastResult final
{
    bool HasHit = false;                       ///< Whether any admitted triangle lies within the distance limit.
    float64 DistanceMeters = 0.0;              ///< Ray parameter of the hit along the unit direction, in meters.
    foundation::math::Vector3d PositionMesh{}; ///< `OriginMesh + DirectionMesh * DistanceMeters`.
    /// Unit geometric normal of the hit triangle from its winding, not oriented toward the ray. A ray that starts
    /// on a triangle reports distance zero with this same triangle normal.
    foundation::math::Vector3d NormalMesh{};
    uint32 TriangleIndex = INVALID_TRIANGLE_INDEX; ///< Source triangle index (index-buffer offset divided by 3).
    bool IsFrontFace = false;                      ///< Whether the ray direction points against NormalMesh (strictly).
};

/// @brief Closest translation-only cast of an axis-aligned box in mesh space.
///
/// The box keeps the mesh axes; there is no rotation in this baseline. Admitted values: finite center components
/// within MAX_COORDINATE_METERS; half extents in (0, MAX_QUERY_DISTANCE_METERS]; translation components within
/// MAX_QUERY_DISTANCE_METERS. A zero translation performs only the selected initial-overlap test. Triangles are
/// two-sided for casts.
struct BoxCastInput final
{
    foundation::math::Vector3d CenterMesh{};      ///< Box center at the start of the cast, meters.
    foundation::math::Vector3d HalfExtentsMesh{}; ///< Positive half size along each mesh axis, meters.
    foundation::math::Vector3d TranslationMesh{}; ///< Full cast displacement, meters.
    InitialOverlapPolicy OverlapPolicy = InitialOverlapPolicy::Report; ///< Treatment of starting overlaps.
    QueryTraversal Traversal = QueryTraversal::Hierarchy;              ///< Hierarchy or brute-force oracle traversal.
};

/// @brief Closest box cast answer.
///
/// On a miss `HasHit` is false and every other field keeps its default value. A contact that would begin exactly
/// at the end of the translation is a miss: the box can complete its translation, ending in contact. Ordering is
/// smaller fraction first, then an initial overlap before a touching contact at the same fraction, then the lowest
/// source triangle index.
struct BoxCastResult final
{
    bool HasHit = false;                ///< Whether the box contacts or overlaps an admitted triangle.
    float64 Fraction = 0.0;             ///< Fraction of the translation in [0, 1) at first contact.
    float64 DistanceMeters = 0.0;       ///< `Fraction * Length(TranslationMesh)`.
    bool HasStartedOverlapping = false; ///< The box overlapped the triangle at its starting pose.
    bool HasNormal = false;             ///< Whether NormalMesh is valid; false for an initial overlap.
    /// Unit separating normal at first contact, pointing from the triangle toward the box. Zero unless HasNormal.
    foundation::math::Vector3d NormalMesh{};
    uint32 TriangleIndex = INVALID_TRIANGLE_INDEX; ///< Source triangle index (index-buffer offset divided by 3).
};

/// @brief Closest translation-only cast of a posed convex shape in mesh space.
///
/// The shape is placed by `PositionMesh` and `OrientationMesh` (shape space to mesh space) and translates by
/// `TranslationMesh` without rotating. Admitted values: a valid ConvexShape (see its documentation); a finite position
/// with components within MAX_COORDINATE_METERS; a finite orientation whose squared norm is within
/// UNIT_QUATERNION_SQUARED_TOLERANCE of one; translation components within MAX_QUERY_DISTANCE_METERS. A zero
/// translation performs only the selected initial-overlap test. Triangles are two-sided for casts.
struct ConvexCastInput final
{
    ConvexShape Shape{};                            ///< Shape to cast; a hull is borrowed for this call.
    foundation::math::Vector3d PositionMesh{};      ///< Shape origin at the start of the cast, meters.
    foundation::math::Quaternion OrientationMesh{}; ///< Rotation from shape axes to mesh axes.
    foundation::math::Vector3d TranslationMesh{};   ///< Full cast displacement, meters.
    InitialOverlapPolicy OverlapPolicy = InitialOverlapPolicy::Report; ///< Treatment of starting overlaps.
    QueryTraversal Traversal = QueryTraversal::Hierarchy;              ///< Hierarchy or brute-force oracle traversal.
};

/// @brief Closest convex cast answer.
///
/// On a miss `HasHit` is false and every other field keeps its default value. Contact is decided with the absolute
/// tolerance `CONVEX_CAST_RELATIVE_TOLERANCE` times the query scale: a triangle starts overlapping when it is
/// provably deeper than the tolerance inside the shape (or the support search encloses the origin exactly), it is
/// touching when its distance lies within the tolerance of zero, and a translating shape first contacts it at the
/// smallest fraction where the distance falls to the tolerance. Touching at the start and moving into the triangle
/// is a hit at fraction zero; moving away or parallel is a miss. A contact that would begin at or after the end of the
/// translation is a miss. Ordering matches BoxCastResult: smaller fraction, then an initial overlap before a touching
/// contact, then the lowest source triangle index.
struct ConvexCastResult final
{
    bool HasHit = false;                ///< Whether the shape contacts or overlaps an admitted triangle.
    float64 Fraction = 0.0;             ///< Fraction of the translation in [0, 1) at first contact.
    float64 DistanceMeters = 0.0;       ///< `Fraction * Length(TranslationMesh)`.
    bool HasStartedOverlapping = false; ///< The shape overlapped the triangle at its starting pose.
    bool HasNormal = false;             ///< Whether NormalMesh is valid; false for an initial overlap.
    /// Unit separating normal at first contact, pointing from the triangle toward the shape. Zero unless HasNormal.
    foundation::math::Vector3d NormalMesh{};
    bool HasWitness = false; ///< Whether WitnessPositionMesh is valid; false for an initial overlap.
    /// Point on the hit triangle closest to the shape at first contact, in mesh space. Zero unless HasWitness.
    foundation::math::Vector3d WitnessPositionMesh{};
    uint32 TriangleIndex = INVALID_TRIANGLE_INDEX; ///< Source triangle index (index-buffer offset divided by 3).
};
} // namespace ludus::physics::collision
