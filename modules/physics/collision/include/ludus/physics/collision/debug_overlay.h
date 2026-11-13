#pragma once

/// @file
/// @brief Debug extraction for collision queries: bounded world-space wire lines that an editor or renderer draws.
///
/// PhysicsCollision depends on no renderer. These functions describe shapes, collider bounds, filter decisions and
/// query answers as plain line values; a consumer maps each DebugLineKind to its own colour and drawing path. See
/// "Query debug lines" in docs/architecture/physics-collision.md.

#include <ludus/foundation/base/core.h>

#include <ludus/foundation/containers/borrowed_view.hpp>
#include <ludus/foundation/math/vector.hpp>
#include <ludus/physics/collision/collision_world.h>
#include <ludus/physics/collision/convex_shape.h>
#include <ludus/physics/collision/status.h>

namespace ludus::physics::collision
{
/// Segments in each full circle of a sphere or capsule wire.
inline constexpr uint32 DEBUG_CIRCLE_SEGMENTS = 16;

/// Default half size of a point marker, in meters.
inline constexpr float64 DEFAULT_DEBUG_MARKER_SIZE_METERS = 0.05;

/// Default length of a drawn normal, in meters.
inline constexpr float64 DEFAULT_DEBUG_NORMAL_LENGTH_METERS = 0.25;

/// @brief What a debug line shows. A consumer chooses the colour for each kind.
enum class DebugLineKind : uint8
{
    ColliderBounds,         ///< World bounds of a collider the filter reports.
    FilteredColliderBounds, ///< World bounds of a live collider the filter excludes (disabled, category, sensor,
                            ///< ignored).
    HierarchyBounds,        ///< Bounds of an internal node of the collider hierarchy.
    QueryShape,             ///< The query shape at its starting pose.
    QueryPath,              ///< A ray up to its hit or limit, or a cast's path from start to end.
    QueryPathBeyondHit,     ///< The rest of a ray after its hit.
    HitShape,               ///< A cast shape at its first-contact pose.
    HitPoint,               ///< A marker at a ray hit or a cast witness point.
    HitNormal,              ///< The hit normal, drawn from the hit point.
    HitBounds,              ///< World bounds of a collider an overlap query reported.
    Shape,                  ///< A shape drawn on its own by AppendShapeLines.
};

/// @brief One world-space line segment.
struct DebugLine final
{
    foundation::math::Vector3d StartWorld{};   ///< First endpoint, meters.
    foundation::math::Vector3d EndWorld{};     ///< Second endpoint, meters.
    DebugLineKind Kind = DebugLineKind::Shape; ///< What the line shows.
    ColliderHandle Collider{};                 ///< The collider the line belongs to; null for query geometry.
};

/// @brief Caller storage for debug lines, filled by the Append functions.
///
/// Lines are appended until the storage is full; later lines are counted but not written, so RequiredCount tells
/// the caller how many to provide next time. The buffer borrows its storage; keep it alive while appending.
class DebugLineBuffer final
{
public:
    /// Construct an empty buffer over borrowed storage.
    explicit DebugLineBuffer(foundation::BorrowedView<DebugLine> storage) noexcept : mStorage(storage) {}

    /// Write `line` if there is room; always count it.
    LUDUS_INLINE void Append(const DebugLine& line) noexcept
    {
        if (mWrittenCount < mStorage.GetSize())
        {
            mStorage[mWrittenCount++] = line;
        }
        ++mRequiredCount;
    }
    /// Forget every line, keeping the storage.
    LUDUS_INLINE void Clear() noexcept
    {
        mWrittenCount = 0;
        mRequiredCount = 0;
    }
    /// Whether the storage view is usable.
    [[nodiscard]] LUDUS_INLINE bool IsValid() const noexcept
    {
        return mStorage.IsValid();
    }
    /// The written lines, a prefix of the storage.
    [[nodiscard]] LUDUS_INLINE foundation::BorrowedView<const DebugLine> GetLines() const noexcept
    {
        return {mStorage.GetDataOrNull(), mWrittenCount};
    }
    /// Lines written to the storage.
    [[nodiscard]] LUDUS_INLINE usize GetWrittenCount() const noexcept
    {
        return mWrittenCount;
    }
    /// Lines appended, written or not.
    [[nodiscard]] LUDUS_INLINE usize GetRequiredCount() const noexcept
    {
        return mRequiredCount;
    }
    /// Whether every appended line was written.
    [[nodiscard]] LUDUS_INLINE bool IsComplete() const noexcept
    {
        return mWrittenCount == mRequiredCount;
    }

private:
    foundation::BorrowedView<DebugLine> mStorage;
    usize mWrittenCount = 0;
    usize mRequiredCount = 0;
};

/// @brief Sizes of drawn markers and normals.
struct DebugLineStyle final
{
    float64 MarkerSizeMeters = DEFAULT_DEBUG_MARKER_SIZE_METERS;     ///< Half size of a point marker; positive.
    float64 NormalLengthMeters = DEFAULT_DEBUG_NORMAL_LENGTH_METERS; ///< Drawn normal length; positive.
};

/// @brief Append the wire of a posed convex shape.
///
/// A box draws its 12 edges; a sphere three great circles; a capsule its two end circles, four side lines and four
/// half circles. A hull has no face list, so it draws a marker at each stored point and its oriented bounds.
/// @param shape A valid shape; a hull is borrowed for the call.
/// @param pose Placement of the shape's local space.
/// @param kind Kind given to every line.
/// @param style Marker size for hull points.
/// @param inoutLines Receives the lines.
/// @return Success; InvalidArgument for an invalid buffer or style; or the ConvexCastInput validation failure for
/// the shape and pose. Nothing is appended on failure.
[[nodiscard]] Status AppendShapeLines(const ConvexShape& shape,
                                      const RigidPose& pose,
                                      DebugLineKind kind,
                                      const DebugLineStyle& style,
                                      DebugLineBuffer& inoutLines) noexcept;

/// @brief Append the world bounds of every live collider, classified by a query filter.
/// @param view Active read view of the world to draw.
/// @param filter Colliders it reports draw as ColliderBounds; the other live colliders as FilteredColliderBounds.
/// @param shouldIncludeHierarchy Also draw every internal hierarchy node as HierarchyBounds.
/// @param inoutLines Receives 12 lines per box, in collider slot order then hierarchy order.
/// @return Success; InvalidPhase for an inactive view; or InvalidArgument for an invalid buffer or filter.
[[nodiscard]] Status AppendColliderLines(const CollisionReadView& view,
                                         const QueryFilter& filter,
                                         bool shouldIncludeHierarchy,
                                         DebugLineBuffer& inoutLines) noexcept;

/// @brief Append a ray and its answer: the path to the hit (or the limit), the rest beyond the hit, a hit marker and
/// the hit normal.
/// @param input The query input.
/// @param result The answer of RaycastClosest, RaycastAny or one record of RaycastAll for that input.
/// @param style Marker and normal sizes.
/// @param inoutLines Receives the lines; hit lines carry the hit collider.
/// @return Success; InvalidArgument for an invalid buffer or style; or the RaycastInput validation failure.
[[nodiscard]] Status AppendRaycastLines(const WorldRaycastInput& input,
                                        const WorldRaycastResult& result,
                                        const DebugLineStyle& style,
                                        DebugLineBuffer& inoutLines) noexcept;

/// @brief Append a convex cast and its answer: the shape at its start, the path of its origin, and on a hit the shape
/// at its first-contact pose, a witness marker and the contact normal.
/// @param view Active read view of the world the cast ran in; resolves a stored QueryShape.
/// @param input The query input.
/// @param result The answer for that input.
/// @param style Marker and normal sizes.
/// @param inoutLines Receives the lines.
/// @return Success; InvalidPhase for an inactive view; InvalidHandle or Unsupported for the QueryShape;
/// InvalidArgument for an invalid buffer or style; or the ConvexCastInput validation failure.
[[nodiscard]] Status AppendConvexCastLines(const CollisionReadView& view,
                                           const WorldConvexCastInput& input,
                                           const WorldConvexCastResult& result,
                                           const DebugLineStyle& style,
                                           DebugLineBuffer& inoutLines) noexcept;

/// @brief Append an overlap query and its answer: the shape at its pose and the bounds of every reported collider.
/// @param view Active read view of the world the overlap ran in.
/// @param input The query input.
/// @param records The records the query wrote.
/// @param style Marker size for hull points.
/// @param inoutLines Receives the lines.
/// @return Success; InvalidPhase for an inactive view; InvalidHandle for a record whose collider is not live, or a bad
/// QueryShape; Unsupported for a triangle-mesh QueryShape; InvalidArgument for an invalid buffer, records view or
/// style; or the ConvexCastInput validation failure.
[[nodiscard]] Status AppendOverlapLines(const CollisionReadView& view,
                                        const WorldOverlapInput& input,
                                        foundation::BorrowedView<const ColliderOverlap> records,
                                        const DebugLineStyle& style,
                                        DebugLineBuffer& inoutLines) noexcept;
} // namespace ludus::physics::collision
