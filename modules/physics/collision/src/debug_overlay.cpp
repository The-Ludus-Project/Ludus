#include <ludus/physics/collision/debug_overlay.h>

#include "internal/collider_hierarchy.h"
#include "internal/convex_cast.h"
#include "internal/convex_support.h"
#include "internal/mesh_arithmetic.h"
#include "internal/query_validation.h"

#include <ludus/foundation/base/core.h>

#include <ludus/foundation/containers/static_array.hpp>
#include <ludus/foundation/math/scalar.hpp>

namespace ludus::physics::collision
{
namespace
{
namespace math = foundation::math;
using foundation::StaticArray;
using internal::Widen;

constexpr uint32 BOX_CORNER_COUNT = 8;
constexpr uint32 AXIS_BIT_COUNT = 3;
constexpr float64 HALF_TURN = math::kPi;
constexpr float64 FULL_TURN = 2.0 * math::kPi;
constexpr uint32 HALF_CIRCLE_SEGMENTS = DEBUG_CIRCLE_SEGMENTS / 2;

/// A rigid frame: world position and rotation columns.
struct Frame final
{
    math::Vector3d PositionWorld{};
    math::Vector3d AxisXWorld{1.0, 0.0, 0.0};
    math::Vector3d AxisYWorld{0.0, 1.0, 0.0};
    math::Vector3d AxisZWorld{0.0, 0.0, 1.0};

    [[nodiscard]] math::Vector3d ToWorld(math::Vector3d pointLocal) const noexcept
    {
        return PositionWorld + AxisXWorld * pointLocal.X + AxisYWorld * pointLocal.Y + AxisZWorld * pointLocal.Z;
    }
};

Frame MakeFrame(math::Vector3d positionWorld, math::Quaternion orientationWorld) noexcept
{
    internal::PosedConvex rotation;
    const bool isRotated = internal::TrySetRotation(orientationWorld, rotation);
    LUDUS_ASSERT(isRotated == true);
    (void)isRotated;
    return
    {
        .PositionWorld = positionWorld,
        .AxisXWorld = rotation.AxisXMesh,
        .AxisYWorld = rotation.AxisYMesh,
        .AxisZWorld = rotation.AxisZMesh,
    };
}

/// The line kind and owner shared by every line one shape or box draws.
struct LineTag final
{
    DebugLineKind Kind = DebugLineKind::Shape;
    ColliderHandle Collider{};
};

void AppendLine(math::Vector3d startWorld,
                math::Vector3d endWorld,
                const LineTag& tag,
                DebugLineBuffer& inoutLines) noexcept
{
    inoutLines.Append({ .StartWorld = startWorld, .EndWorld = endWorld, .Kind = tag.Kind, .Collider = tag.Collider });
}

/// A box in a frame: its centre and half extents.
struct LocalBox final
{
    math::Vector3d CenterLocal{};
    math::Vector3d HalfExtentsLocal{};
};

/// The 12 edges of a box in a frame.
void AppendBox(const Frame& frame, const LocalBox& box, const LineTag& tag, DebugLineBuffer& inoutLines) noexcept
{
    const math::Vector3d centerLocal = box.CenterLocal;
    const math::Vector3d halfExtentsLocal = box.HalfExtentsLocal;
    StaticArray<math::Vector3d, BOX_CORNER_COUNT> corners{};
    for (uint32 corner = 0; corner < BOX_CORNER_COUNT; ++corner)
    {
        const math::Vector3d offset{(corner & 1U) == 0 ? -halfExtentsLocal.X : halfExtentsLocal.X,
                                    (corner & 2U) == 0 ? -halfExtentsLocal.Y : halfExtentsLocal.Y,
                                    (corner & 4U) == 0 ? -halfExtentsLocal.Z : halfExtentsLocal.Z};
        corners[corner] = frame.ToWorld(centerLocal + offset);
    }
    // Each edge joins two corners that differ in exactly one axis bit.
    for (uint32 corner = 0; corner < BOX_CORNER_COUNT; ++corner)
    {
        for (uint32 bit = 0; bit < AXIS_BIT_COUNT; ++bit)
        {
            const uint32 other = corner ^ (1U << bit);
            if (other > corner)
            {
                AppendLine(corners[corner], corners[other], tag, inoutLines);
            }
        }
    }
}

/// An axis-aligned world box.
void AppendBounds(math::Vector3d minimumWorld,
                  math::Vector3d maximumWorld,
                  const LineTag& tag,
                  DebugLineBuffer& inoutLines) noexcept
{
    AppendBox(Frame{},
              {
                  .CenterLocal = (minimumWorld + maximumWorld) * 0.5,
                  .HalfExtentsLocal = (maximumWorld - minimumWorld) * 0.5,
              },
              tag,
              inoutLines);
}

/// The axes of a drawn arc, in the shape frame.
struct ArcPlane final
{
    math::Vector3d CenterLocal{};
    math::Vector3d FirstAxisLocal{};  ///< Direction at angle zero.
    math::Vector3d SecondAxisLocal{}; ///< Direction at a quarter turn.
};

/// An arc of a circle: its plane, radius, angular range and line count.
struct Arc final
{
    ArcPlane Plane{};
    float64 RadiusMeters = 0.0;
    float64 StartRadians = 0.0;
    float64 SweepRadians = FULL_TURN;
    uint32 Segments = DEBUG_CIRCLE_SEGMENTS;
};

void AppendArc(const Frame& frame, const Arc& arc, const LineTag& tag, DebugLineBuffer& inoutLines) noexcept
{
    const auto pointAt = [&frame, &arc](uint32 step) {
        const float64 angle =
            arc.StartRadians + arc.SweepRadians * static_cast<float64>(step) / static_cast<float64>(arc.Segments);
        return frame.ToWorld(arc.Plane.CenterLocal + arc.Plane.FirstAxisLocal * (arc.RadiusMeters * math::Cos(angle)) +
                             arc.Plane.SecondAxisLocal * (arc.RadiusMeters * math::Sin(angle)));
    };
    math::Vector3d previous = pointAt(0);
    for (uint32 step = 1; step <= arc.Segments; ++step)
    {
        const math::Vector3d next = pointAt(step);
        AppendLine(previous, next, tag, inoutLines);
        previous = next;
    }
}

/// Three axis-aligned lines crossing at a point.
void AppendMarker(math::Vector3d pointWorld, float64 size, const LineTag& tag, DebugLineBuffer& inoutLines) noexcept
{
    const StaticArray<math::Vector3d, AXIS_BIT_COUNT> axes{math::Vector3d{size, 0.0, 0.0},
                                                           math::Vector3d{0.0, size, 0.0},
                                                           math::Vector3d{0.0, 0.0, size}};
    for (const math::Vector3d axis : axes)
    {
        AppendLine(pointWorld - axis, pointWorld + axis, tag, inoutLines);
    }
}

void AppendShapeWire(const ConvexShape& shape,
                     const Frame& frame,
                     float64 markerSize,
                     const LineTag& tag,
                     DebugLineBuffer& inoutLines) noexcept
{
    const math::Vector3d unitX{1.0, 0.0, 0.0};
    const math::Vector3d unitY{0.0, 1.0, 0.0};
    const math::Vector3d unitZ{0.0, 0.0, 1.0};
    switch (shape.Kind)
    {
        case ConvexShapeKind::Box:
            AppendBox(frame, { .HalfExtentsLocal = shape.HalfExtentsMeters }, tag, inoutLines);
            return;
        case ConvexShapeKind::Sphere: {
            const StaticArray<ArcPlane, AXIS_BIT_COUNT> planes{
                ArcPlane{ .FirstAxisLocal = unitX, .SecondAxisLocal = unitY },
                ArcPlane{ .FirstAxisLocal = unitY, .SecondAxisLocal = unitZ },
                ArcPlane{ .FirstAxisLocal = unitZ, .SecondAxisLocal = unitX }};
            for (const ArcPlane& plane : planes)
            {
                AppendArc(frame, { .Plane = plane, .RadiusMeters = shape.RadiusMeters }, tag, inoutLines);
            }
            return;
        }
        case ConvexShapeKind::Capsule: {
            const float64 radius = shape.RadiusMeters;
            const math::Vector3d top = unitY * shape.HalfHeightMeters;
            const StaticArray<math::Vector3d, 2> ends{top, -top};
            for (const math::Vector3d end : ends)
            {
                const ArcPlane ring{ .CenterLocal = end, .FirstAxisLocal = unitZ, .SecondAxisLocal = unitX };
                AppendArc(frame, { .Plane = ring, .RadiusMeters = radius }, tag, inoutLines);
            }
            constexpr usize SIDE_COUNT = 4;
            const math::Vector3d sideX = unitX * radius;
            const math::Vector3d sideZ = unitZ * radius;
            const StaticArray<math::Vector3d, SIDE_COUNT> sides{sideX, -sideX, sideZ, -sideZ};
            for (const math::Vector3d side : sides)
            {
                AppendLine(frame.ToWorld(top + side), frame.ToWorld(side - top), tag, inoutLines);
            }
            const StaticArray<math::Vector3d, 2> acrosses{unitX, unitZ};
            for (const math::Vector3d across : acrosses)
            {
                const ArcPlane upper{ .CenterLocal = top, .FirstAxisLocal = across, .SecondAxisLocal = unitY };
                const ArcPlane lower{ .CenterLocal = -top, .FirstAxisLocal = across, .SecondAxisLocal = unitY };
                AppendArc(frame,
                          {
                              .Plane = upper,
                              .RadiusMeters = radius,
                              .SweepRadians = HALF_TURN,
                              .Segments = HALF_CIRCLE_SEGMENTS,
                          },
                          tag,
                          inoutLines);
                AppendArc(frame,
                          {
                              .Plane = lower,
                              .RadiusMeters = radius,
                              .StartRadians = HALF_TURN,
                              .SweepRadians = HALF_TURN,
                              .Segments = HALF_CIRCLE_SEGMENTS,
                          },
                          tag,
                          inoutLines);
            }
            return;
        }
        case ConvexShapeKind::Hull: {
            // No face list: mark every stored point and draw the hull's oriented bounds.
            for (const math::Vector3 pointShape : shape.HullOrNull->GetPointsShape())
            {
                AppendMarker(frame.ToWorld(Widen(pointShape)), markerSize, tag, inoutLines);
            }
            const math::Aabb3 bounds = shape.HullOrNull->GetBoundsShape();
            AppendBox(frame,
                      {
                          .CenterLocal = (Widen(bounds.Min) + Widen(bounds.Max)) * 0.5,
                          .HalfExtentsLocal = (Widen(bounds.Max) - Widen(bounds.Min)) * 0.5,
                      },
                      tag,
                      inoutLines);
            return;
        }
    }
}

bool IsStyleValid(const DebugLineStyle& style) noexcept
{
    return math::IsFinite(style.MarkerSizeMeters) == true && style.MarkerSizeMeters > 0.0 &&
           math::IsFinite(style.NormalLengthMeters) == true && style.NormalLengthMeters > 0.0;
}

Status
ValidateShapePose(const ConvexShape& shape, math::Vector3d positionWorld, math::Quaternion orientationWorld) noexcept
{
    const ConvexCastInput placement
    {
        .Shape = shape,
        .PositionMesh = positionWorld,
        .OrientationMesh = orientationWorld,
    };
    return internal::ValidateConvexCast(placement);
}
} // namespace

/// Debug extraction with access to world and view internals.
class CollisionDebugExtraction final
{
public:
    [[nodiscard]] static const CollisionWorld* GetWorld(const CollisionReadView& view) noexcept
    {
        return view.mWorldOrNull;
    }
    [[nodiscard]] static Status ResolveShape(const CollisionWorld& world,
                                             const ConvexShape& shape,
                                             ShapeHandle stored,
                                             ConvexShape& outShape) noexcept
    {
        if (stored.IsNull() == true)
        {
            outShape = shape;
            return Status::Success;
        }
        return world.mStoreOrNull->TryGetConvexShape(stored, outShape);
    }
    static Status AppendColliders(const CollisionWorld& world,
                                  const QueryFilter& filter,
                                  bool shouldIncludeHierarchy,
                                  DebugLineBuffer& inoutLines) noexcept;
    [[nodiscard]] static Status
    AppendColliderBounds(const CollisionWorld& world, ColliderHandle collider, DebugLineBuffer& inoutLines) noexcept;
};

Status CollisionDebugExtraction::AppendColliders(const CollisionWorld& world,
                                                 const QueryFilter& filter,
                                                 bool shouldIncludeHierarchy,
                                                 DebugLineBuffer& inoutLines) noexcept
{
    for (uint32 slotIndex = 0; slotIndex < world.mSlots.GetSize(); ++slotIndex)
    {
        const CollisionWorld::Slot& slot = world.mSlots[slotIndex];
        if (slot.IsLive == false)
        {
            continue;
        }
        const ColliderHandle handle{ .Owner = world.mOwner, .Slot = slotIndex, .Generation = slot.Generation };
        bool isReported = slot.Description.IsEnabled == true && slot.Description.Participation.IsQueryable == true &&
                          (slot.Description.Filter.CategoryBits & filter.CategoryMask) != 0 &&
                          (slot.Description.Participation.IsSensor == false || filter.ShouldIncludeSensors == true);
        for (const ColliderHandle ignored : filter.IgnoredColliders)
        {
            isReported = isReported == true && ignored != handle;
        }
        const LineTag tag
        {
            .Kind = isReported == true ? DebugLineKind::ColliderBounds : DebugLineKind::FilteredColliderBounds,
            .Collider = handle,
        };
        AppendBounds(slot.BoundsMinWorld, slot.BoundsMaxWorld, tag, inoutLines);
    }
    if (shouldIncludeHierarchy == false || world.mRoot == NULL_HIERARCHY_NODE)
    {
        return Status::Success;
    }
    StaticArray<uint32, MAX_HIERARCHY_TRAVERSAL_NODES> pending{};
    usize pendingCount = 0;
    pending[pendingCount++] = world.mRoot;
    while (pendingCount > 0)
    {
        const CollisionWorld::HierarchyNode& node = world.mNodes[pending[--pendingCount]];
        if (node.Height == 0)
        {
            continue;
        }
        AppendBounds(node.BoundsMinWorld, node.BoundsMaxWorld, { .Kind = DebugLineKind::HierarchyBounds }, inoutLines);
        LUDUS_ASSERT(pendingCount + 2 <= pending.GetSize());
        pending[pendingCount++] = node.Right;
        pending[pendingCount++] = node.Left;
    }
    return Status::Success;
}

Status CollisionDebugExtraction::AppendColliderBounds(const CollisionWorld& world,
                                                      ColliderHandle collider,
                                                      DebugLineBuffer& inoutLines) noexcept
{
    const CollisionWorld::Slot* slotOrNull = world.findLive(collider);
    if (slotOrNull == nullptr)
    {
        return Status::InvalidHandle;
    }
    AppendBounds(slotOrNull->BoundsMinWorld,
                 slotOrNull->BoundsMaxWorld,
                 { .Kind = DebugLineKind::HitBounds, .Collider = collider },
                 inoutLines);
    return Status::Success;
}

Status AppendShapeLines(const ConvexShape& shape,
                        const RigidPose& pose,
                        DebugLineKind kind,
                        const DebugLineStyle& style,
                        DebugLineBuffer& inoutLines) noexcept
{
    if (inoutLines.IsValid() == false || IsStyleValid(style) == false)
    {
        return Status::InvalidArgument;
    }
    const Status validation = ValidateShapePose(shape, pose.PositionWorld, pose.OrientationWorld);
    if (validation != Status::Success)
    {
        return validation;
    }
    AppendShapeWire(shape,
                    MakeFrame(pose.PositionWorld, pose.OrientationWorld),
                    style.MarkerSizeMeters,
                    { .Kind = kind },
                    inoutLines);
    return Status::Success;
}

Status AppendColliderLines(const CollisionReadView& view,
                           const QueryFilter& filter,
                           bool shouldIncludeHierarchy,
                           DebugLineBuffer& inoutLines) noexcept
{
    const CollisionWorld* worldOrNull = CollisionDebugExtraction::GetWorld(view);
    if (worldOrNull == nullptr)
    {
        return Status::InvalidPhase;
    }
    if (inoutLines.IsValid() == false || filter.IgnoredColliders.IsValid() == false ||
        filter.IgnoredColliders.GetSize() > MAX_QUERY_IGNORED_COLLIDERS)
    {
        return Status::InvalidArgument;
    }
    return CollisionDebugExtraction::AppendColliders(*worldOrNull, filter, shouldIncludeHierarchy, inoutLines);
}

Status AppendRaycastLines(const WorldRaycastInput& input,
                          const WorldRaycastResult& result,
                          const DebugLineStyle& style,
                          DebugLineBuffer& inoutLines) noexcept
{
    if (inoutLines.IsValid() == false || IsStyleValid(style) == false)
    {
        return Status::InvalidArgument;
    }
    const Status validation = internal::ValidateRaycast(
    {
        .OriginMesh = input.OriginWorld,
        .DirectionMesh = input.DirectionWorld,
        .MaxDistanceMeters = input.MaxDistanceMeters,
        .FacePolicy = input.FacePolicy,
        .Traversal = input.Traversal,
    });
    if (validation != Status::Success)
    {
        return validation;
    }
    const math::Vector3d limitWorld = input.OriginWorld + input.DirectionWorld * input.MaxDistanceMeters;
    if (result.HasHit == false)
    {
        AppendLine(input.OriginWorld, limitWorld, { .Kind = DebugLineKind::QueryPath }, inoutLines);
        return Status::Success;
    }
    const LineTag hit{ .Kind = DebugLineKind::HitPoint, .Collider = result.Collider };
    AppendLine(input.OriginWorld, result.PositionWorld, { .Kind = DebugLineKind::QueryPath }, inoutLines);
    if (result.DistanceMeters < input.MaxDistanceMeters)
    {
        AppendLine(result.PositionWorld, limitWorld, { .Kind = DebugLineKind::QueryPathBeyondHit }, inoutLines);
    }
    AppendMarker(result.PositionWorld, style.MarkerSizeMeters, hit, inoutLines);
    if (result.HasNormal == true)
    {
        AppendLine(result.PositionWorld,
                   result.PositionWorld + result.NormalWorld * style.NormalLengthMeters,
                   { .Kind = DebugLineKind::HitNormal, .Collider = result.Collider },
                   inoutLines);
    }
    return Status::Success;
}

Status AppendConvexCastLines(const CollisionReadView& view,
                             const WorldConvexCastInput& input,
                             const WorldConvexCastResult& result,
                             const DebugLineStyle& style,
                             DebugLineBuffer& inoutLines) noexcept
{
    const CollisionWorld* worldOrNull = CollisionDebugExtraction::GetWorld(view);
    if (worldOrNull == nullptr)
    {
        return Status::InvalidPhase;
    }
    ConvexShape shape;
    const Status resolved = CollisionDebugExtraction::ResolveShape(*worldOrNull, input.Shape, input.QueryShape, shape);
    if (resolved != Status::Success)
    {
        return resolved;
    }
    if (inoutLines.IsValid() == false || IsStyleValid(style) == false)
    {
        return Status::InvalidArgument;
    }
    const ConvexCastInput cast
    {
        .Shape = shape,
        .PositionMesh = input.PositionWorld,
        .OrientationMesh = input.OrientationWorld,
        .TranslationMesh = input.TranslationWorld,
    };
    const Status validation = internal::ValidateConvexCast(cast);
    if (validation != Status::Success)
    {
        return validation;
    }
    const Frame start = MakeFrame(input.PositionWorld, input.OrientationWorld);
    AppendShapeWire(shape, start, style.MarkerSizeMeters, { .Kind = DebugLineKind::QueryShape }, inoutLines);
    AppendLine(input.PositionWorld,
               input.PositionWorld + input.TranslationWorld,
               { .Kind = DebugLineKind::QueryPath },
               inoutLines);
    if (result.HasHit == false)
    {
        return Status::Success;
    }
    Frame contact = start;
    contact.PositionWorld = input.PositionWorld + input.TranslationWorld * result.Fraction;
    AppendShapeWire(shape,
                    contact,
                    style.MarkerSizeMeters,
                    { .Kind = DebugLineKind::HitShape, .Collider = result.Collider },
                    inoutLines);
    if (result.HasWitness == true)
    {
        AppendMarker(result.WitnessPositionWorld,
                     style.MarkerSizeMeters,
                     { .Kind = DebugLineKind::HitPoint, .Collider = result.Collider },
                     inoutLines);
        if (result.HasNormal == true)
        {
            AppendLine(result.WitnessPositionWorld,
                       result.WitnessPositionWorld + result.NormalWorld * style.NormalLengthMeters,
                       { .Kind = DebugLineKind::HitNormal, .Collider = result.Collider },
                       inoutLines);
        }
    }
    return Status::Success;
}

Status AppendOverlapLines(const CollisionReadView& view,
                          const WorldOverlapInput& input,
                          foundation::BorrowedView<const ColliderOverlap> records,
                          const DebugLineStyle& style,
                          DebugLineBuffer& inoutLines) noexcept
{
    const CollisionWorld* worldOrNull = CollisionDebugExtraction::GetWorld(view);
    if (worldOrNull == nullptr)
    {
        return Status::InvalidPhase;
    }
    ConvexShape shape;
    const Status resolved = CollisionDebugExtraction::ResolveShape(*worldOrNull, input.Shape, input.QueryShape, shape);
    if (resolved != Status::Success)
    {
        return resolved;
    }
    if (inoutLines.IsValid() == false || records.IsValid() == false || IsStyleValid(style) == false)
    {
        return Status::InvalidArgument;
    }
    const Status validation = ValidateShapePose(shape, input.PositionWorld, input.OrientationWorld);
    if (validation != Status::Success)
    {
        return validation;
    }
    for (const ColliderOverlap& record : records)
    {
        if (worldOrNull->IsAlive(record.Collider) == false)
        {
            return Status::InvalidHandle;
        }
    }
    AppendShapeWire(shape,
                    MakeFrame(input.PositionWorld, input.OrientationWorld),
                    style.MarkerSizeMeters,
                    { .Kind = DebugLineKind::QueryShape },
                    inoutLines);
    for (const ColliderOverlap& record : records)
    {
        const Status appended =
            CollisionDebugExtraction::AppendColliderBounds(*worldOrNull, record.Collider, inoutLines);
        LUDUS_ASSERT(appended == Status::Success);
        (void)appended;
    }
    return Status::Success;
}
} // namespace ludus::physics::collision
