// Debug extraction: shape wires, collider bounds and filter decisions, and
// the lines that describe ray, cast and overlap answers.

#include "test_meshes.h"

#include <ludus/foundation/base/core.h>

#include <ludus/foundation/containers/array.hpp>
#include <ludus/foundation/containers/borrowed_view.hpp>
#include <ludus/foundation/containers/static_array.hpp>
#include <ludus/foundation/math/quaternion.hpp>
#include <ludus/foundation/math/scalar.hpp>
#include <ludus/foundation/math/vector.hpp>
#include <ludus/physics/collision/collision_world.h>
#include <ludus/physics/collision/convex_shape.h>
#include <ludus/physics/collision/debug_overlay.h>
#include <ludus/physics/collision/geometry_store.h>
#include <ludus/physics/collision/queries.h>
#include <ludus/physics/collision/status.h>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

using namespace ludus::physics::collision;
using namespace ludus::physics::collision::test;
namespace math = ludus::foundation::math;

namespace
{
constexpr usize LINE_CAPACITY = 1024;
constexpr usize BOX_LINES = 12;
constexpr usize CIRCLE_LINES = DEBUG_CIRCLE_SEGMENTS;
constexpr usize SPHERE_LINES = 3 * CIRCLE_LINES;
constexpr usize CAPSULE_LINES = 2 * CIRCLE_LINES + 4 + 4 * (CIRCLE_LINES / 2);
constexpr usize MARKER_LINES = 3;
constexpr float64 GEOMETRY_TOLERANCE = 1.0e-9;

/// Owned line storage and a buffer over it.
struct Lines final
{
    StaticArray<DebugLine, LINE_CAPACITY> Storage{};
    DebugLineBuffer Buffer{BorrowedView<DebugLine>(Storage.GetData(), LINE_CAPACITY)};

    [[nodiscard]] usize Count(DebugLineKind kind) const
    {
        usize count = 0;
        for (const DebugLine& line : Buffer.GetLines())
        {
            count += line.Kind == kind ? 1 : 0;
        }
        return count;
    }
};

math::Quaternion MakeRotation(Vector3 axis, float32 radians)
{
    math::Quaternion rotation;
    REQUIRE(math::TryFromAxisAngle(axis, radians, rotation) == math::MathStatus::Success);
    return rotation;
}

float64 GetDistanceToSegment(Vector3d point, Vector3d start, Vector3d end)
{
    const Vector3d edge = end - start;
    const float64 t = math::Clamp(math::Dot(point - start, edge) / math::Dot(edge, edge), 0.0, 1.0);
    return math::Length(point - (start + edge * t));
}

ShapeHandle AddFloorOrFail(GeometryStore& inoutStore)
{
    MeshData data;
    AddFloorQuad(data);
    ShapeHandle handle;
    REQUIRE(inoutStore.TryAddTriangleMesh(CookOrFail(data), handle) == Status::Success);
    return handle;
}

ColliderHandle CreateOrFail(CollisionWorld& inoutWorld, const ColliderDescription& description)
{
    ColliderHandle collider;
    REQUIRE(inoutWorld.TryCreateCollider(description, collider) == Status::Success);
    return collider;
}
} // namespace

TEST_CASE("Shape wires have their documented lines and lie on the shape", "[collision][debug]")
{
    const math::Quaternion turned = MakeRotation({0.3f, 0.8f, -0.5f}, 1.1f);
    const RigidPose pose{ .PositionWorld = {1.0, 2.0, 3.0}, .OrientationWorld = turned };
    // Rotated shape axes, from the same quaternion through the math library.
    const auto toWorld = [&](Vector3d local) {
        const Vector3 rotated =
            math::Rotate(turned,
                         {static_cast<float32>(local.X), static_cast<float32>(local.Y), static_cast<float32>(local.Z)});
        return pose.PositionWorld + Widen(rotated);
    };
    constexpr float64 ROTATION_TOLERANCE = 1.0e-6;

    Lines box;
    REQUIRE(AppendShapeLines(MakeBoxShape({0.5, 0.25, 0.75}), pose, DebugLineKind::Shape, {}, box.Buffer) ==
            Status::Success);
    REQUIRE(box.Buffer.GetWrittenCount() == BOX_LINES);
    CHECK(box.Buffer.IsComplete() == true);
    for (const DebugLine& line : box.Buffer.GetLines())
    {
        CHECK(line.Kind == DebugLineKind::Shape);
        CHECK(line.Collider.IsNull() == true);
        // Each edge has length 1.0, 0.5 or 1.5 and both ends on a corner.
        const float64 length = math::Length(line.EndWorld - line.StartWorld);
        CHECK((math::Abs(length - 1.0) < ROTATION_TOLERANCE || math::Abs(length - 0.5) < ROTATION_TOLERANCE ||
               math::Abs(length - 1.5) < ROTATION_TOLERANCE) == true);
    }
    const Vector3d corner = toWorld({0.5, 0.25, 0.75});
    bool hasCorner = false;
    for (const DebugLine& line : box.Buffer.GetLines())
    {
        hasCorner = hasCorner == true || math::Length(line.StartWorld - corner) < ROTATION_TOLERANCE ||
                    math::Length(line.EndWorld - corner) < ROTATION_TOLERANCE;
    }
    CHECK(hasCorner == true);

    Lines sphere;
    REQUIRE(AppendShapeLines(MakeSphereShape(0.4), pose, DebugLineKind::Shape, {}, sphere.Buffer) == Status::Success);
    REQUIRE(sphere.Buffer.GetWrittenCount() == SPHERE_LINES);
    for (const DebugLine& line : sphere.Buffer.GetLines())
    {
        CHECK(math::Length(line.StartWorld - pose.PositionWorld) == Catch::Approx(0.4).margin(GEOMETRY_TOLERANCE));
        CHECK(math::Length(line.EndWorld - pose.PositionWorld) == Catch::Approx(0.4).margin(GEOMETRY_TOLERANCE));
    }

    Lines capsule;
    REQUIRE(AppendShapeLines(MakeCapsuleShape(0.6, 0.2), pose, DebugLineKind::Shape, {}, capsule.Buffer) ==
            Status::Success);
    REQUIRE(capsule.Buffer.GetWrittenCount() == CAPSULE_LINES);
    const Vector3d top = toWorld({0.0, 0.6, 0.0});
    const Vector3d bottom = toWorld({0.0, -0.6, 0.0});
    for (const DebugLine& line : capsule.Buffer.GetLines())
    {
        CHECK(GetDistanceToSegment(line.StartWorld, top, bottom) == Catch::Approx(0.2).margin(ROTATION_TOLERANCE));
        CHECK(GetDistanceToSegment(line.EndWorld, top, bottom) == Catch::Approx(0.2).margin(ROTATION_TOLERANCE));
    }

    Array<Vector3> points;
    points.Add({0.0f, 0.0f, 0.0f});
    points.Add({1.0f, 0.0f, 0.0f});
    points.Add({0.0f, 1.0f, 0.0f});
    points.Add({0.0f, 0.0f, 1.0f});
    points.Add({0.2f, 0.2f, 0.2f});
    ConvexHull hull;
    REQUIRE(ConvexHull::TryCook(
                {
                    .PointsShape = BorrowedView<const Vector3>(points.GetData(), points.GetSize()),
                },
                hull)
                .Result == Status::Success);
    Lines hullLines;
    REQUIRE(AppendShapeLines(MakeHullShape(hull), pose, DebugLineKind::Shape, {}, hullLines.Buffer) == Status::Success);
    CHECK(hullLines.Buffer.GetWrittenCount() == points.GetSize() * MARKER_LINES + BOX_LINES);
    // The first marker is centred on the first point, the pose origin.
    const DebugLine& first = hullLines.Buffer.GetLines()[0];
    CHECK(math::Length((first.StartWorld + first.EndWorld) * 0.5 - pose.PositionWorld) < GEOMETRY_TOLERANCE);
}

TEST_CASE("A full buffer keeps counting and failures append nothing", "[collision][debug]")
{
    StaticArray<DebugLine, 5> storage{};
    DebugLineBuffer buffer(BorrowedView<DebugLine>(storage.GetData(), storage.GetSize()));
    REQUIRE(AppendShapeLines(MakeBoxShape({1.0, 1.0, 1.0}), {}, DebugLineKind::Shape, {}, buffer) == Status::Success);
    CHECK(buffer.GetWrittenCount() == storage.GetSize());
    CHECK(buffer.GetRequiredCount() == BOX_LINES);
    CHECK(buffer.IsComplete() == false);
    CHECK(buffer.GetLines().GetSize() == storage.GetSize());
    buffer.Clear();
    CHECK(buffer.GetRequiredCount() == 0);
    CHECK(buffer.IsComplete() == true);

    CHECK(AppendShapeLines(MakeSphereShape(0.0), {}, DebugLineKind::Shape, {}, buffer) == Status::InvalidArgument);
    CHECK(AppendShapeLines(MakeSphereShape(1.0),
                           { .OrientationWorld = {0.0f, 0.0f, 0.0f, 3.0f} },
                           DebugLineKind::Shape,
                           {},
                           buffer) == Status::InvalidArgument);
    CHECK(AppendShapeLines(MakeSphereShape(1.0), {}, DebugLineKind::Shape, { .MarkerSizeMeters = 0.0 }, buffer) ==
          Status::InvalidArgument);
    CHECK(AppendRaycastLines({ .DirectionWorld = {0, 0, 2}, .MaxDistanceMeters = 1.0 }, {}, {}, buffer) ==
          Status::InvalidArgument);
    DebugLineBuffer invalid(BorrowedView<DebugLine>(nullptr, 3));
    CHECK(AppendShapeLines(MakeSphereShape(1.0), {}, DebugLineKind::Shape, {}, invalid) == Status::InvalidArgument);
    CHECK(buffer.GetRequiredCount() == 0);
    CHECK(invalid.GetRequiredCount() == 0);
}

TEST_CASE("Query lines describe ray, cast and overlap answers", "[collision][debug]")
{
    GeometryStore store;
    REQUIRE(store.TryInitialize({ .MaxShapes = 4 }) == Status::Success);
    const ShapeHandle floor = AddFloorOrFail(store);
    ShapeHandle ball;
    REQUIRE(store.TryAddPrimitive(MakeSphereShape(0.25), ball) == Status::Success);
    CollisionWorld world;
    REQUIRE(world.TryInitialize(store, { .MaxColliders = 4 }) == Status::Success);
    const ColliderHandle ground = CreateOrFail(world, { .Shape = floor });
    CollisionReadView view;
    REQUIRE(world.TryAcquireReadView(view) == Status::Success);
    const DebugLineStyle style{ .MarkerSizeMeters = 0.1, .NormalLengthMeters = 0.5 };

    // A ray hitting the floor: the path to the hit, the rest beyond it, a marker and the normal.
    const WorldRaycastInput ray
    {
        .OriginWorld = {0.5, 2.0, 0.5},
        .DirectionWorld = {0, -1, 0},
        .MaxDistanceMeters = 4.0,
    };
    WorldRaycastResult hit;
    REQUIRE(view.RaycastClosest(ray, hit) == Status::Success);
    REQUIRE(hit.HasHit == true);
    Lines rayLines;
    REQUIRE(AppendRaycastLines(ray, hit, style, rayLines.Buffer) == Status::Success);
    CHECK(rayLines.Buffer.GetWrittenCount() == 2 + MARKER_LINES + 1);
    CHECK(rayLines.Count(DebugLineKind::QueryPath) == 1);
    CHECK(rayLines.Count(DebugLineKind::QueryPathBeyondHit) == 1);
    CHECK(rayLines.Count(DebugLineKind::HitPoint) == MARKER_LINES);
    for (const DebugLine& line : rayLines.Buffer.GetLines())
    {
        if (line.Kind == DebugLineKind::HitNormal)
        {
            CHECK(line.Collider == ground);
            CHECK(line.StartWorld.Y == Catch::Approx(0.0).margin(GEOMETRY_TOLERANCE));
            CHECK(line.EndWorld.Y == Catch::Approx(0.5).margin(GEOMETRY_TOLERANCE));
        }
        if (line.Kind == DebugLineKind::QueryPath)
        {
            CHECK(line.EndWorld == hit.PositionWorld);
        }
    }
    // A miss draws only the path to the limit.
    Lines missLines;
    REQUIRE(AppendRaycastLines(ray, {}, style, missLines.Buffer) == Status::Success);
    REQUIRE(missLines.Buffer.GetWrittenCount() == 1);
    CHECK(missLines.Buffer.GetLines()[0].EndWorld.Y == Catch::Approx(-2.0));

    // A stored sphere cast: the start wire, the path, the contact wire, the witness and the normal.
    const WorldConvexCastInput cast
    {
        .QueryShape = ball,
        .PositionWorld = {0.5, 2.0, 0.5},
        .TranslationWorld = {0.0, -4.0, 0.0},
    };
    WorldConvexCastResult contact;
    REQUIRE(view.CastConvexClosest(cast, contact) == Status::Success);
    REQUIRE(contact.HasHit == true);
    Lines castLines;
    REQUIRE(AppendConvexCastLines(view, cast, contact, style, castLines.Buffer) == Status::Success);
    CHECK(castLines.Count(DebugLineKind::QueryShape) == SPHERE_LINES);
    CHECK(castLines.Count(DebugLineKind::QueryPath) == 1);
    CHECK(castLines.Count(DebugLineKind::HitShape) == SPHERE_LINES);
    CHECK(castLines.Count(DebugLineKind::HitPoint) == MARKER_LINES);
    CHECK(castLines.Count(DebugLineKind::HitNormal) == 1);
    const Vector3d contactCenter = cast.PositionWorld + cast.TranslationWorld * contact.Fraction;
    for (const DebugLine& line : castLines.Buffer.GetLines())
    {
        if (line.Kind == DebugLineKind::HitShape)
        {
            CHECK(line.Collider == ground);
            CHECK(math::Length(line.StartWorld - contactCenter) == Catch::Approx(0.25).margin(GEOMETRY_TOLERANCE));
        }
    }

    // An overlap: the shape and the bounds of every reported collider.
    const WorldOverlapInput overlap{ .Shape = MakeBoxShape({0.5, 0.2, 0.5}), .PositionWorld = {0.5, 0.0, 0.5} };
    StaticArray<ColliderOverlap, 4> records{};
    WorldOverlapResult overlapResult;
    REQUIRE(view.Overlap(overlap, BorrowedView<ColliderOverlap>(records.GetData(), records.GetSize()), overlapResult) ==
            Status::Success);
    REQUIRE(overlapResult.WrittenCount == 1);
    Lines overlapLines;
    REQUIRE(AppendOverlapLines(view,
                               overlap,
                               BorrowedView<const ColliderOverlap>(records.GetData(), overlapResult.WrittenCount),
                               style,
                               overlapLines.Buffer) == Status::Success);
    CHECK(overlapLines.Count(DebugLineKind::QueryShape) == BOX_LINES);
    CHECK(overlapLines.Count(DebugLineKind::HitBounds) == BOX_LINES);

    // A stale record or an inactive view fails without appending.
    StaticArray<ColliderOverlap, 1> stale{ColliderOverlap{ .Collider = { .Owner = 1, .Slot = 3, .Generation = 9 } }};
    Lines failed;
    CHECK(AppendOverlapLines(view,
                             overlap,
                             BorrowedView<const ColliderOverlap>(stale.GetData(), 1),
                             style,
                             failed.Buffer) == Status::InvalidHandle);
    CollisionReadView inactive;
    CHECK(AppendConvexCastLines(inactive, cast, contact, style, failed.Buffer) == Status::InvalidPhase);
    CHECK(AppendColliderLines(inactive, {}, false, failed.Buffer) == Status::InvalidPhase);
    CHECK(failed.Buffer.GetRequiredCount() == 0);
}

TEST_CASE("Collider lines classify every live collider by the query filter", "[collision][debug]")
{
    GeometryStore store;
    REQUIRE(store.TryInitialize({ .MaxShapes = 1 }) == Status::Success);
    const ShapeHandle floor = AddFloorOrFail(store);
    CollisionWorld world;
    constexpr uint32 COLLIDER_COUNT = 6;
    REQUIRE(world.TryInitialize(store, { .MaxColliders = COLLIDER_COUNT }) == Status::Success);
    const ColliderHandle plain = CreateOrFail(world, { .Shape = floor });
    (void)CreateOrFail(world, { .Shape = floor, .Pose = { .PositionWorld = {2.0, 0.0, 0.0} }, .IsEnabled = false });
    (void)CreateOrFail(world,
                       {
                           .Shape = floor,
                           .Pose = { .PositionWorld = {4.0, 0.0, 0.0} },
                           .Participation = { .IsSolid = false, .IsSensor = true },
                       });
    (void)CreateOrFail(world,
                       {
                           .Shape = floor,
                           .Pose =
                           {
                               .PositionWorld = {6.0, 0.0, 0.0},
                           },
                           .Filter =
                           {
                               .CategoryBits = 2,
                           },
                       });
    CollisionReadView view;
    REQUIRE(world.TryAcquireReadView(view) == Status::Success);
    Lines lines;
    REQUIRE(AppendColliderLines(view, { .CategoryMask = 1 }, false, lines.Buffer) == Status::Success);
    // The plain floor is reported; the disabled, sensor and category-2 floors are filtered out.
    CHECK(lines.Count(DebugLineKind::ColliderBounds) == BOX_LINES);
    CHECK(lines.Count(DebugLineKind::FilteredColliderBounds) == 3 * BOX_LINES);
    CHECK(lines.Buffer.GetLines()[0].Collider == plain);
    lines.Buffer.Clear();
    const StaticArray<ColliderHandle, 1> ignored{plain};
    REQUIRE(AppendColliderLines(view,
                                {
                                    .ShouldIncludeSensors = true,
                                    .IgnoredColliders = BorrowedView<const ColliderHandle>(ignored.GetData(), 1),
                                },
                                true,
                                lines.Buffer) == Status::Success);
    CHECK(lines.Count(DebugLineKind::ColliderBounds) == 2 * BOX_LINES);
    CHECK(lines.Count(DebugLineKind::FilteredColliderBounds) == 2 * BOX_LINES);
    // Four leaves have three internal nodes.
    CHECK(lines.Count(DebugLineKind::HierarchyBounds) == 3 * BOX_LINES);
    CHECK(AppendColliderLines(view,
                              { .IgnoredColliders = BorrowedView<const ColliderHandle>(nullptr, 1) },
                              false,
                              lines.Buffer) == Status::InvalidArgument);
}
