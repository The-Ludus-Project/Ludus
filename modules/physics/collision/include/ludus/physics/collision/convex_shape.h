#pragma once

/// @file
/// @brief Convex query shapes: sphere, box, capsule and a cooked convex hull.
///
/// Shapes are described in their own local space (suffix `Shape`), in meters. A cast places a shape with a rigid
/// pose; see ConvexCastInput. The contracts are in docs/architecture/physics-collision.md.

#include <ludus/foundation/base/core.h>

#include <ludus/foundation/containers/array.hpp>
#include <ludus/foundation/containers/borrowed_view.hpp>
#include <ludus/foundation/math/geometry.hpp>
#include <ludus/foundation/math/vector.hpp>
#include <ludus/physics/collision/status.h>

namespace ludus::physics::collision
{
/// Fewest points a hull source may hold; four is the smallest set that encloses a volume.
inline constexpr uint32 MIN_CONVEX_HULL_POINTS = 4;

/// Largest number of points a hull source may hold. Support queries visit every stored point.
inline constexpr uint32 MAX_CONVEX_HULL_POINTS = 256;

/// @brief Scale-free flatness bound for hull cooking.
///
/// Cooking finds the two points farthest apart along the widest axis, the point farthest from their line and the
/// point farthest from the plane of those three. A hull is degenerate when either of those distances is at most this
/// ratio times the first pair's separation, so the same shape is admitted or rejected at any size.
inline constexpr float64 MIN_CONVEX_HULL_THICKNESS_RATIO = 1.0e-6;

/// Sentinel for "no point": reported when a hull cook failure is not attributable to one source point.
inline constexpr uint32 INVALID_POINT_INDEX = 0xFFFFFFFFU;

/// @brief Borrowed source points for hull cooking, in shape space and meters.
///
/// The caller keeps the view alive and unchanged for the duration of the cook call only; the cooked hull copies the
/// points it keeps.
struct ConvexHullSource final
{
    foundation::BorrowedView<const foundation::math::Vector3> PointsShape; ///< Points whose convex hull is the shape.
};

/// @brief Outcome of ConvexHull::TryCook.
///
/// PointIndex names the first offending source point for NonFiniteInput and OutOfRange and is INVALID_POINT_INDEX
/// otherwise.
struct [[nodiscard]] ConvexHullCookResult final
{
    Status Result = Status::Success;         ///< Success, or why the hull was not cooked.
    uint32 PointIndex = INVALID_POINT_INDEX; ///< Offending source point, when attributable.
};

/// @brief Immutable convex hull of a bounded point set, in its own shape space.
///
/// A hull is created only by TryCook. It stores a copy of the source points and answers support queries from them,
/// which is exact for the convex hull of the set: interior points never change a support result. It builds no face
/// list; faces and mass properties belong to later physics gates. A default-constructed or moved-from hull holds no
/// points, and a cast with it fails with Status::InvalidArgument. A cooked hull never changes, so any number of
/// threads may read it concurrently while no thread replaces or destroys it.
class ConvexHull final
{
public:
    /// Construct an empty, uncooked hull. Does not allocate.
    ConvexHull() noexcept = default;
    /// Hulls are move-only so that a cooked hull has exactly one owner.
    ConvexHull(const ConvexHull&) = delete;
    /// Hulls are move-only so that a cooked hull has exactly one owner.
    ConvexHull& operator=(const ConvexHull&) = delete;
    /// Transfer the cooked points; `other` becomes empty.
    ConvexHull(ConvexHull&& other) noexcept = default;
    /// Release this hull's points and take `other`'s; `other` becomes empty.
    ConvexHull& operator=(ConvexHull&& other) noexcept = default;
    /// Release the cooked points.
    ~ConvexHull() noexcept = default;

    /// @brief Validate and cook borrowed points into a new hull.
    ///
    /// Validation runs before any allocation and visits points in source order: the view must be valid; the count
    /// must be in [MIN_CONVEX_HULL_POINTS, MAX_CONVEX_HULL_POINTS]; every coordinate must be finite and within
    /// MAX_COORDINATE_METERS; and the set must pass MIN_CONVEX_HULL_THICKNESS_RATIO. Duplicate and interior points
    /// are admitted. The result is deterministic for identical inputs.
    /// @param source Borrowed points, read only during this call.
    /// @param outHull Receives the new hull on success. Unchanged on any failure, including allocation failure.
    /// @return Success; InvalidArgument for an invalid view; NoGeometry for zero points; DegenerateShape for one to
    /// three points or a flat set; CapacityExceeded above the point limit; NonFiniteInput or OutOfRange with the
    /// offending point; or AllocationFailure.
    [[nodiscard]] static ConvexHullCookResult TryCook(const ConvexHullSource& source, ConvexHull& outHull) noexcept;

    /// Whether the hull holds cooked points.
    [[nodiscard]] LUDUS_INLINE bool IsCooked() const noexcept
    {
        return mPoints.IsEmpty() == false;
    }
    /// The stored points in shape space, in source order; empty when uncooked. Borrowed from the hull.
    [[nodiscard]] LUDUS_INLINE foundation::BorrowedView<const foundation::math::Vector3> GetPointsShape() const noexcept
    {
        return {mPoints.GetData(), mPoints.GetSize()};
    }
    /// Exact bounds of the stored points; the canonical empty box when uncooked.
    [[nodiscard]] LUDUS_INLINE foundation::math::Aabb3 GetBoundsShape() const noexcept
    {
        return IsCooked() == true ? mBoundsShape : foundation::math::EmptyAabb3();
    }
    /// A point strictly inside the hull (the centroid of the four cooking extremes); zero when uncooked.
    [[nodiscard]] LUDUS_INLINE foundation::math::Vector3d GetInteriorPointShape() const noexcept
    {
        return IsCooked() == true ? mInteriorPointShape : foundation::math::Vector3d{};
    }

private:
    foundation::Array<foundation::math::Vector3> mPoints;
    foundation::math::Aabb3 mBoundsShape = foundation::math::EmptyAabb3();
    foundation::math::Vector3d mInteriorPointShape{};
};

/// @brief Kind of a convex query shape.
enum class ConvexShapeKind : uint8
{
    Sphere,  ///< A ball of RadiusMeters about the shape origin.
    Box,     ///< A box of HalfExtentsMeters centred on the shape origin, aligned with the shape axes.
    Capsule, ///< A segment of half length HalfHeightMeters along shape +Y, swept by a ball of RadiusMeters.
    Hull,    ///< The convex hull of a cooked ConvexHull's points.
};

/// @brief Value description of one convex query shape.
///
/// Only the fields of the selected kind are read. Admitted values, checked by each query: RadiusMeters in
/// (0, MAX_QUERY_DISTANCE_METERS] for spheres and capsules; HalfHeightMeters in [0, MAX_QUERY_DISTANCE_METERS] for
/// capsules (zero is a sphere); every HalfExtentsMeters component in (0, MAX_QUERY_DISTANCE_METERS] for boxes; and a
/// non-null, cooked hull for hulls.
struct ConvexShape final
{
    ConvexShapeKind Kind = ConvexShapeKind::Sphere; ///< Which shape the remaining fields describe.
    float64 RadiusMeters = 0.0;                     ///< Sphere radius, or the capsule's rounding radius.
    float64 HalfHeightMeters = 0.0;                 ///< Half the distance between a capsule's two ball centres.
    foundation::math::Vector3d HalfExtentsMeters{}; ///< Box half size along each shape axis.
    /// Borrowed hull for ConvexShapeKind::Hull. The caller keeps it alive and unchanged for each query that uses it.
    const ConvexHull* HullOrNull = nullptr;
};

/// Describe a sphere of the given radius in meters.
[[nodiscard]] constexpr ConvexShape MakeSphereShape(float64 radiusMeters) noexcept
{
    return { .Kind = ConvexShapeKind::Sphere, .RadiusMeters = radiusMeters };
}

/// Describe a box of the given half extents in meters.
[[nodiscard]] constexpr ConvexShape MakeBoxShape(foundation::math::Vector3d halfExtentsMeters) noexcept
{
    return { .Kind = ConvexShapeKind::Box, .HalfExtentsMeters = halfExtentsMeters };
}

/// Describe an upright capsule: a segment from -halfHeightMeters to +halfHeightMeters along shape +Y, rounded by
/// radiusMeters. Its total height is `2 * (halfHeightMeters + radiusMeters)`.
[[nodiscard]] constexpr ConvexShape MakeCapsuleShape(float64 halfHeightMeters, float64 radiusMeters) noexcept
{
    return { .Kind = ConvexShapeKind::Capsule, .RadiusMeters = radiusMeters, .HalfHeightMeters = halfHeightMeters };
}

/// Describe the convex hull `hull`, borrowed for every query that uses the returned value.
[[nodiscard]] constexpr ConvexShape MakeHullShape(const ConvexHull& hull) noexcept
{
    return { .Kind = ConvexShapeKind::Hull, .HullOrNull = &hull };
}
} // namespace ludus::physics::collision
