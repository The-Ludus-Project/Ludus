// GeometryStore leases and identities, CollisionWorld colliders, read-view
// protection, query filters and world-space queries.

#include "test_meshes.h"

#include <ludus/foundation/base/core.h>

#include <ludus/foundation/containers/array.hpp>
#include <ludus/foundation/containers/borrowed_view.hpp>
#include <ludus/foundation/containers/static_array.hpp>
#include <ludus/foundation/math/quaternion.hpp>
#include <ludus/foundation/math/random.hpp>
#include <ludus/foundation/math/scalar.hpp>
#include <ludus/foundation/math/vector.hpp>
#include <ludus/physics/collision/collision_world.h>
#include <ludus/physics/collision/convex_shape.h>
#include <ludus/physics/collision/geometry_store.h>
#include <ludus/physics/collision/queries.h>
#include <ludus/physics/collision/status.h>
#include <ludus/physics/collision/triangle_mesh.h>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <limits>

using namespace ludus::physics::collision;
using namespace ludus::physics::collision::test;
namespace math = ludus::foundation::math;

namespace
{
constexpr usize BOX_CORNER_COUNT = 8;

TriangleMesh MakeFloorMesh()
{
    MeshData data;
    AddFloorQuad(data);
    return CookOrFail(data);
}

ConvexHull MakeCubeHull()
{
    Array<Vector3> points;
    for (uint32 corner = 0; corner < BOX_CORNER_COUNT; ++corner)
    {
        points.Add(
            {(corner & 1U) == 0 ? -0.5f : 0.5f, (corner & 2U) == 0 ? -0.5f : 0.5f, (corner & 4U) == 0 ? -0.5f : 0.5f});
    }
    ConvexHull hull;
    REQUIRE(ConvexHull::TryCook(
                {
                    .PointsShape = BorrowedView<const Vector3>(points.GetData(), points.GetSize()),
                },
                hull)
                .Result == Status::Success);
    return hull;
}

ShapeHandle AddFloor(GeometryStore& inoutStore)
{
    ShapeHandle shape;
    REQUIRE(inoutStore.TryAddTriangleMesh(MakeFloorMesh(), shape) == Status::Success);
    return shape;
}

void InitializeOrFail(GeometryStore& inoutStore, uint32 capacity)
{
    REQUIRE(inoutStore.TryInitialize({ .MaxShapes = capacity }) == Status::Success);
}

void InitializeOrFail(CollisionWorld& inoutWorld, GeometryStore& inoutStore, uint32 capacity)
{
    REQUIRE(inoutWorld.TryInitialize(inoutStore, { .MaxColliders = capacity }) == Status::Success);
}

ColliderHandle CreateOrFail(CollisionWorld& inoutWorld, const ColliderDescription& description)
{
    ColliderHandle collider;
    REQUIRE(inoutWorld.TryCreateCollider(description, collider) == Status::Success);
    return collider;
}

math::Quaternion MakeRotation(Vector3 axis, float32 radians)
{
    math::Quaternion rotation;
    REQUIRE(math::TryFromAxisAngle(axis, radians, rotation) == math::MathStatus::Success);
    return rotation;
}

WorldRaycastInput MakeDownRay(Vector3d origin)
{
    return { .OriginWorld = origin, .DirectionWorld = {0.0, -1.0, 0.0}, .MaxDistanceMeters = 100.0 };
}

WorldRaycastResult RaycastOrFail(const CollisionReadView& view, const WorldRaycastInput& input)
{
    WorldRaycastResult result;
    REQUIRE(view.RaycastClosest(input, result) == Status::Success);
    return result;
}

WorldConvexCastResult CastOrFail(const CollisionReadView& view, const WorldConvexCastInput& input)
{
    WorldConvexCastResult result;
    REQUIRE(view.CastConvexClosest(input, result) == Status::Success);
    return result;
}

bool AreEqual(const WorldRaycastResult& first, const WorldRaycastResult& second)
{
    return first.HasHit == second.HasHit && first.DistanceMeters == second.DistanceMeters &&
           first.PositionWorld == second.PositionWorld && first.NormalWorld == second.NormalWorld &&
           first.IsFrontFace == second.IsFrontFace && first.TriangleIndex == second.TriangleIndex &&
           first.Collider == second.Collider && first.Binding == second.Binding;
}

bool AreEqual(const WorldConvexCastResult& first, const WorldConvexCastResult& second)
{
    return first.HasHit == second.HasHit && first.Fraction == second.Fraction &&
           first.DistanceMeters == second.DistanceMeters &&
           first.HasStartedOverlapping == second.HasStartedOverlapping && first.HasNormal == second.HasNormal &&
           first.NormalWorld == second.NormalWorld && first.HasWitness == second.HasWitness &&
           first.WitnessPositionWorld == second.WitnessPositionWorld && first.TriangleIndex == second.TriangleIndex &&
           first.Collider == second.Collider && first.Binding == second.Binding;
}
} // namespace

TEST_CASE("A geometry store issues leased shape handles and retires them on the last release", "[collision][store]")
{
    GeometryStore store;
    CHECK(store.IsInitialized() == false);
    ShapeHandle shape;
    CHECK(store.TryAddTriangleMesh(MakeFloorMesh(), shape) == Status::InvalidPhase);
    CHECK(store.TryInitialize({ .MaxShapes = 0 }) == Status::InvalidArgument);
    CHECK(store.TryInitialize({ .MaxShapes = MAX_GEOMETRY_STORE_SHAPES + 1 }) == Status::CapacityExceeded);
    InitializeOrFail(store, 2);
    CHECK(store.TryInitialize({ .MaxShapes = 2 }) == Status::InvalidPhase);

    shape = AddFloor(store);
    CHECK(shape.IsNull() == false);
    CHECK(store.IsAlive(shape) == true);
    CHECK(store.GetShapeCount() == 1);
    ShapeKind kind = ShapeKind::Sphere;
    REQUIRE(store.TryGetKind(shape, kind) == Status::Success);
    CHECK(kind == ShapeKind::TriangleMesh);
    uint32 leases = 0;
    REQUIRE(store.TryGetLeaseCount(shape, leases) == Status::Success);
    CHECK(leases == 1);
    REQUIRE(store.TryRetain(shape) == Status::Success);
    REQUIRE(store.TryGetLeaseCount(shape, leases) == Status::Success);
    CHECK(leases == 2);
    REQUIRE(store.TryRelease(shape) == Status::Success);
    CHECK(store.IsAlive(shape) == true);
    REQUIRE(store.TryRelease(shape) == Status::Success);
    CHECK(store.IsAlive(shape) == false);
    CHECK(store.GetShapeCount() == 0);
    CHECK(store.TryRelease(shape) == Status::InvalidHandle);
    CHECK(store.TryRetain(shape) == Status::InvalidHandle);
    CHECK(store.TryGetKind(shape, kind) == Status::InvalidHandle);

    // The slot is reused with a new generation; the stale handle stays stale.
    const ShapeHandle reused = AddFloor(store);
    CHECK(reused.Slot == shape.Slot);
    CHECK(reused.Generation == shape.Generation + 1);
    CHECK(store.IsAlive(shape) == false);

    // Null, malformed and foreign handles are rejected.
    CHECK(store.IsAlive(ShapeHandle{}) == false);
    ShapeHandle malformed = reused;
    malformed.Slot = 7;
    CHECK(store.IsAlive(malformed) == false);
    GeometryStore other;
    InitializeOrFail(other, 2);
    const ShapeHandle foreign = AddFloor(other);
    CHECK(foreign.Owner != reused.Owner);
    CHECK(store.IsAlive(foreign) == false);
    CHECK(store.TryRelease(foreign) == Status::InvalidHandle);
}

TEST_CASE("A geometry store validates shapes and preserves outputs on failure", "[collision][store]")
{
    GeometryStore store;
    InitializeOrFail(store, 3);
    const ShapeHandle sentinel{ .Owner = 9, .Slot = 9, .Generation = 9 };
    ShapeHandle shape = sentinel;
    CHECK(store.TryAddTriangleMesh(TriangleMesh{}, shape) == Status::NoGeometry);
    CHECK(store.TryAddConvexHull(ConvexHull{}, shape) == Status::NoGeometry);
    CHECK(store.TryAddPrimitive(MakeSphereShape(0.0), shape) == Status::InvalidArgument);
    CHECK(store.TryAddPrimitive(MakeSphereShape(std::numeric_limits<float64>::infinity()), shape) ==
          Status::NonFiniteInput);
    CHECK(store.TryAddPrimitive(MakeBoxShape({1.0, 2.0 * MAX_QUERY_DISTANCE_METERS, 1.0}), shape) ==
          Status::OutOfRange);
    const ConvexHull cube = MakeCubeHull();
    CHECK(store.TryAddPrimitive(MakeHullShape(cube), shape) == Status::InvalidArgument);
    CHECK(shape == sentinel);

    // A failed move leaves the source intact.
    TriangleMesh floor = MakeFloorMesh();
    ShapeHandle meshShape;
    REQUIRE(store.TryAddTriangleMesh(ludus::foundation::Move(floor), meshShape) == Status::Success);
    CHECK(floor.IsCooked() == false);
    ShapeHandle hullShape;
    REQUIRE(store.TryAddConvexHull(MakeCubeHull(), hullShape) == Status::Success);
    ShapeHandle capsule;
    REQUIRE(store.TryAddPrimitive(MakeCapsuleShape(0.5, 0.25), capsule) == Status::Success);
    ShapeKind kind = ShapeKind::TriangleMesh;
    REQUIRE(store.TryGetKind(capsule, kind) == Status::Success);
    CHECK(kind == ShapeKind::Capsule);
    REQUIRE(store.TryGetKind(hullShape, kind) == Status::Success);
    CHECK(kind == ShapeKind::Hull);

    // The store is full: a failed addition keeps the caller's mesh.
    TriangleMesh spare = MakeFloorMesh();
    shape = sentinel;
    CHECK(store.TryAddTriangleMesh(ludus::foundation::Move(spare), shape) == Status::CapacityExceeded);
    CHECK(spare.IsCooked() == true);
    CHECK(shape == sentinel);
    const TriangleMesh* meshOrNull = nullptr;
    CHECK(store.TryGetTriangleMesh(capsule, meshOrNull) == Status::Unsupported);
    CHECK(meshOrNull == nullptr);
    REQUIRE(store.TryGetTriangleMesh(meshShape, meshOrNull) == Status::Success);
    CHECK(meshOrNull != nullptr);
}

TEST_CASE("Colliders lease their shapes and stale handles are rejected", "[collision][world]")
{
    GeometryStore store;
    InitializeOrFail(store, 4);
    const ShapeHandle floor = AddFloor(store);
    ShapeHandle sphere;
    REQUIRE(store.TryAddPrimitive(MakeSphereShape(0.5), sphere) == Status::Success);
    CollisionWorld world;
    ColliderHandle collider;
    CHECK(world.TryCreateCollider({ .Shape = floor }, collider) == Status::InvalidPhase);
    CHECK(world.TryInitialize(store, { .MaxColliders = 0 }) == Status::InvalidArgument);
    GeometryStore uninitialized;
    CHECK(world.TryInitialize(uninitialized, { .MaxColliders = 2 }) == Status::InvalidPhase);
    InitializeOrFail(world, store, 2);
    CHECK(world.TryInitialize(store, { .MaxColliders = 2 }) == Status::InvalidPhase);

    collider = CreateOrFail(world, { .Shape = floor, .Binding = 42 });
    CHECK(world.IsAlive(collider) == true);
    CHECK(world.GetColliderCount() == 1);
    uint32 leases = 0;
    REQUIRE(store.TryGetLeaseCount(floor, leases) == Status::Success);
    CHECK(leases == 2);
    // The creator may drop its lease; the collider keeps the shape alive.
    REQUIRE(store.TryRelease(floor) == Status::Success);
    CHECK(store.IsAlive(floor) == true);
    ColliderDescription readBack;
    REQUIRE(world.TryGetCollider(collider, readBack) == Status::Success);
    CHECK(readBack.Binding == 42);
    CHECK(readBack.Shape == floor);

    REQUIRE(world.TryDestroyCollider(collider) == Status::Success);
    CHECK(world.IsAlive(collider) == false);
    CHECK(store.IsAlive(floor) == false);
    CHECK(world.TryDestroyCollider(collider) == Status::InvalidHandle);
    CHECK(world.TrySetColliderEnabled(collider, false) == Status::InvalidHandle);
    CHECK(world.TrySetColliderPose(collider, {}) == Status::InvalidHandle);
    CHECK(world.TryGetCollider(collider, readBack) == Status::InvalidHandle);

    // Every failing creation leaves the output and the lease count unchanged.
    const ShapeHandle second = AddFloor(store);
    const ColliderHandle sentinel{ .Owner = 9, .Slot = 9, .Generation = 9 };
    const auto expectFailure = [&](const ColliderDescription& description, Status status) {
        uint32 before = 0;
        REQUIRE(store.TryGetLeaseCount(second, before) == Status::Success);
        ColliderHandle output = sentinel;
        CHECK(world.TryCreateCollider(description, output) == status);
        CHECK(output == sentinel);
        uint32 after = 0;
        REQUIRE(store.TryGetLeaseCount(second, after) == Status::Success);
        CHECK(after == before);
    };
    expectFailure({ .Shape = floor }, Status::InvalidHandle);
    ColliderDescription invalid{ .Shape = second };
    invalid.Pose.PositionWorld.X = std::numeric_limits<float64>::quiet_NaN();
    expectFailure(invalid, Status::NonFiniteInput);
    invalid.Pose.PositionWorld.X = 2.0 * static_cast<float64>(MAX_COORDINATE_METERS);
    expectFailure(invalid, Status::OutOfRange);
    invalid.Pose = { .OrientationWorld = {0.0f, 0.0f, 0.0f, 3.0f} };
    expectFailure(invalid, Status::InvalidArgument);
    invalid.Pose = {};
    invalid.Participation = { .IsSolid = true, .IsSensor = true };
    expectFailure(invalid, Status::InvalidArgument);
    invalid.Participation = {};
    invalid.Filter.CategoryBits = 0;
    expectFailure(invalid, Status::InvalidArgument);

    const ColliderHandle first = CreateOrFail(world, { .Shape = second });
    const ColliderHandle again = CreateOrFail(world, { .Shape = second });
    CHECK(again.Slot != first.Slot);
    expectFailure({ .Shape = second }, Status::CapacityExceeded);
    REQUIRE(world.TryDestroyCollider(first) == Status::Success);
    REQUIRE(world.TryDestroyCollider(again) == Status::Success);
    uint32 count = 0;
    REQUIRE(store.TryGetLeaseCount(second, count) == Status::Success);
    CHECK(count == 1);

    // Handles from another world are rejected.
    CollisionWorld otherWorld;
    InitializeOrFail(otherWorld, store, 1);
    const ColliderHandle foreign = CreateOrFail(otherWorld, { .Shape = second });
    CHECK(world.IsAlive(foreign) == false);
    CHECK(world.TryDestroyCollider(foreign) == Status::InvalidHandle);
}

TEST_CASE("Read views protect a world from mutation until released", "[collision][world]")
{
    GeometryStore store;
    InitializeOrFail(store, 2);
    const ShapeHandle floor = AddFloor(store);
    CollisionWorld world;
    CollisionReadView view;
    CHECK(world.TryAcquireReadView(view) == Status::InvalidPhase);
    InitializeOrFail(world, store, 4);
    const ColliderHandle collider = CreateOrFail(world, { .Shape = floor });
    const uint64 epoch = world.GetEpoch();
    WorldRaycastResult result;
    CHECK(view.RaycastClosest(MakeDownRay({0.5, 1.0, 0.5}), result) == Status::InvalidPhase);

    REQUIRE(world.TryAcquireReadView(view) == Status::Success);
    CHECK(view.IsActive() == true);
    CHECK(view.GetEpoch() == epoch);
    CHECK(world.GetReaderCount() == 1);
    CHECK(world.TryAcquireReadView(view) == Status::InvalidArgument);
    CollisionReadView second;
    REQUIRE(world.TryAcquireReadView(second) == Status::Success);
    CHECK(world.GetReaderCount() == 2);
    ColliderHandle created;
    CHECK(world.TryCreateCollider({ .Shape = floor }, created) == Status::InvalidPhase);
    CHECK(world.TryDestroyCollider(collider) == Status::InvalidPhase);
    CHECK(world.TrySetColliderPose(collider, {}) == Status::InvalidPhase);
    CHECK(world.TrySetColliderEnabled(collider, false) == Status::InvalidPhase);
    CHECK(world.GetEpoch() == epoch);
    CHECK(RaycastOrFail(view, MakeDownRay({0.5, 1.0, 0.5})).HasHit == true);

    // Moving a view transfers its registration.
    CollisionReadView moved = ludus::foundation::Move(second);
    CHECK(second.IsActive() == false);
    CHECK(moved.IsActive() == true);
    CHECK(world.GetReaderCount() == 2);
    moved.Release();
    CHECK(world.GetReaderCount() == 1);
    CHECK(world.TryDestroyCollider(collider) == Status::InvalidPhase);
    view.Release();
    view.Release();
    CHECK(world.GetReaderCount() == 0);
    REQUIRE(world.TrySetColliderEnabled(collider, false) == Status::Success);
    CHECK(world.GetEpoch() == epoch + 1);
    {
        CollisionReadView scoped;
        REQUIRE(world.TryAcquireReadView(scoped) == Status::Success);
        CHECK(scoped.GetEpoch() == epoch + 1);
        CHECK(RaycastOrFail(scoped, MakeDownRay({0.5, 1.0, 0.5})).HasHit == false);
    }
    CHECK(world.GetReaderCount() == 0);
}

TEST_CASE("World queries place rotated colliders and report their identity", "[collision][world]")
{
    GeometryStore store;
    InitializeOrFail(store, 2);
    const ShapeHandle floor = AddFloor(store);
    CollisionWorld world;
    InitializeOrFail(world, store, 4);
    // A floor turned to stand as a wall facing -x, at x = 3: local +y becomes world -x.
    const ColliderHandle wall = CreateOrFail(world,
                                             {
                                                 .Shape = floor,
                                                 .Pose =
                                                 {
                                                     .PositionWorld = {3.0, 0.0, 0.0},
                                                     .OrientationWorld = MakeRotation({0, 0, 1}, math::kPiF * 0.5f),
                                                 },
                                                 .Binding = 7,
                                             });
    CollisionReadView view;
    REQUIRE(world.TryAcquireReadView(view) == Status::Success);
    const WorldRaycastInput ray
    {
        .OriginWorld = {0.0, 0.5, 0.5},
        .DirectionWorld = {1.0, 0.0, 0.0},
        .MaxDistanceMeters = 10.0,
    };
    const WorldRaycastResult hit = RaycastOrFail(view, ray);
    REQUIRE(hit.HasHit == true);
    CHECK(hit.DistanceMeters == Catch::Approx(3.0).margin(1.0e-6));
    CHECK(hit.PositionWorld.X == Catch::Approx(3.0).margin(1.0e-6));
    CHECK(hit.NormalWorld.X == Catch::Approx(-1.0).margin(1.0e-6));
    CHECK(hit.IsFrontFace == true);
    CHECK(hit.Collider == wall);
    CHECK(hit.Binding == 7);

    WorldConvexCastInput cast
    {
        .Shape = MakeSphereShape(0.25),
        .PositionWorld = {0.0, 0.5, 0.5},
        .TranslationWorld = {4.0, 0.0, 0.0},
    };
    const WorldConvexCastResult contact = CastOrFail(view, cast);
    REQUIRE(contact.HasHit == true);
    CHECK(contact.Fraction == Catch::Approx(2.75 / 4.0).margin(1.0e-6));
    CHECK(contact.NormalWorld.X == Catch::Approx(-1.0).margin(1.0e-6));
    CHECK(contact.WitnessPositionWorld.X == Catch::Approx(3.0).margin(1.0e-6));
    CHECK(contact.Collider == wall);
    // A rotated box keeps its rotation relative to the rotated collider.
    cast.Shape = MakeBoxShape({0.1, 0.4, 0.1});
    cast.OrientationWorld = MakeRotation({0, 0, 1}, math::kPiF * 0.5f);
    const WorldConvexCastResult box = CastOrFail(view, cast);
    REQUIRE(box.HasHit == true);
    CHECK(box.Fraction == Catch::Approx(2.6 / 4.0).margin(1.0e-6));
}

TEST_CASE("Query filters select categories, sensors, enabled and ignored colliders", "[collision][world]")
{
    GeometryStore store;
    InitializeOrFail(store, 2);
    const ShapeHandle floor = AddFloor(store);
    CollisionWorld world;
    InitializeOrFail(world, store, 8);
    // Stacked floors at y = 0, 1, 2, 3: the ray from above meets the highest reported one.
    const ColliderHandle solid = CreateOrFail(world, { .Shape = floor, .Filter = { .CategoryBits = 1 } });
    const ColliderHandle second =
        CreateOrFail(world,
                     { .Shape = floor, .Pose = { .PositionWorld = {0.0, 1.0, 0.0} }, .Filter = { .CategoryBits = 2 } });
    const ColliderHandle sensor = CreateOrFail(world,
                                               {
                                                   .Shape = floor,
                                                   .Pose = { .PositionWorld = {0.0, 2.0, 0.0} },
                                                   .Participation = { .IsSolid = false, .IsSensor = true },
                                               });
    const ColliderHandle hidden = CreateOrFail(world,
                                               {
                                                   .Shape = floor,
                                                   .Pose = { .PositionWorld = {0.0, 3.0, 0.0} },
                                                   .Participation = { .IsQueryable = false },
                                               });
    CollisionReadView view;
    REQUIRE(world.TryAcquireReadView(view) == Status::Success);
    WorldRaycastInput ray = MakeDownRay({0.5, 10.0, 0.5});
    CHECK(RaycastOrFail(view, ray).Collider == second);
    ray.Filter.ShouldIncludeSensors = true;
    CHECK(RaycastOrFail(view, ray).Collider == sensor);
    ray.Filter.CategoryMask = 1;
    CHECK(RaycastOrFail(view, ray).Collider == sensor);
    ray.Filter.ShouldIncludeSensors = false;
    CHECK(RaycastOrFail(view, ray).Collider == solid);
    ray.Filter.CategoryMask = ALL_COLLISION_CATEGORIES;
    const StaticArray<ColliderHandle, 1> ignored{second};
    ray.Filter.IgnoredColliders = BorrowedView<const ColliderHandle>(ignored.GetData(), 1);
    CHECK(RaycastOrFail(view, ray).Collider == solid);
    ray.Filter.IgnoredColliders = BorrowedView<const ColliderHandle>(nullptr, 1);
    WorldRaycastResult failed;
    CHECK(view.RaycastClosest(ray, failed) == Status::InvalidArgument);
    CHECK(failed.HasHit == false);
    ray.Filter = {};
    view.Release();
    REQUIRE(world.TrySetColliderEnabled(second, false) == Status::Success);
    REQUIRE(world.TryAcquireReadView(view) == Status::Success);
    CHECK(RaycastOrFail(view, ray).Collider == solid);
    (void)hidden;

    // The symmetric simulation pair filter.
    const CollisionFilter player{ .CategoryBits = 1, .CollisionMask = 2 | 4 };
    const CollisionFilter debris{ .CategoryBits = 4, .CollisionMask = 1 };
    const CollisionFilter ghost{ .CategoryBits = 8, .CollisionMask = 1 };
    CHECK(ShouldCollide(player, debris) == true);
    CHECK(ShouldCollide(debris, player) == true);
    CHECK(ShouldCollide(player, ghost) == false);
    CHECK(ShouldCollide(ghost, player) == false);
}

TEST_CASE("Equal world hits resolve to the lower collider slot", "[collision][world]")
{
    GeometryStore store;
    InitializeOrFail(store, 1);
    const ShapeHandle floor = AddFloor(store);
    CollisionWorld world;
    InitializeOrFail(world, store, 4);
    const ColliderHandle first = CreateOrFail(world, { .Shape = floor, .Binding = 1 });
    const ColliderHandle second = CreateOrFail(world, { .Shape = floor, .Binding = 2 });
    CHECK(first.Slot < second.Slot);
    CollisionReadView view;
    REQUIRE(world.TryAcquireReadView(view) == Status::Success);
    const WorldRaycastResult hit = RaycastOrFail(view, MakeDownRay({0.25, 1.0, 0.75}));
    CHECK(hit.Collider == first);
    CHECK(hit.Binding == 1);
    const WorldConvexCastResult contact =
        CastOrFail(view,
                   { .Shape = MakeSphereShape(0.2), .PositionWorld = {0.5, 1.0, 0.5}, .TranslationWorld = {0, -2, 0} });
    CHECK(contact.Collider == first);
}

TEST_CASE("Queries reach colliders whose local frame puts the start outside the envelope", "[collision][world]")
{
    GeometryStore store;
    InitializeOrFail(store, 1);
    const ShapeHandle floor = AddFloor(store);
    CollisionWorld world;
    InitializeOrFail(world, store, 2);
    constexpr float64 COLLIDER_HEIGHT_METERS = 9000.0;
    constexpr float64 START_HEIGHT_METERS = -99000.0;
    constexpr float64 RISE_METERS = 110000.0;
    const ColliderHandle far =
        CreateOrFail(world, { .Shape = floor, .Pose = { .PositionWorld = {0.0, COLLIDER_HEIGHT_METERS, 0.0} } });
    CollisionReadView view;
    REQUIRE(world.TryAcquireReadView(view) == Status::Success);
    // The start is 108 km below the collider: outside the coordinate envelope in its local space.
    const WorldRaycastInput ray
    {
        .OriginWorld = {0.5, START_HEIGHT_METERS, 0.5},
        .DirectionWorld = {0.0, 1.0, 0.0},
        .MaxDistanceMeters = RISE_METERS,
    };
    const WorldRaycastResult hit = RaycastOrFail(view, ray);
    REQUIRE(hit.HasHit == true);
    CHECK(hit.Collider == far);
    CHECK(hit.DistanceMeters == Catch::Approx(COLLIDER_HEIGHT_METERS - START_HEIGHT_METERS).margin(1.0e-6));
    CHECK(hit.PositionWorld.Y == Catch::Approx(COLLIDER_HEIGHT_METERS).margin(1.0e-6));
    CHECK(hit.IsFrontFace == false);
    CHECK(hit.NormalWorld.Y == Catch::Approx(1.0).margin(1.0e-9));
    constexpr float64 RADIUS_METERS = 0.25;
    const WorldConvexCastInput cast
    {
        .Shape = MakeSphereShape(RADIUS_METERS),
        .PositionWorld = {0.5, START_HEIGHT_METERS, 0.5},
        .TranslationWorld = {0.0, RISE_METERS, 0.0},
    };
    const WorldConvexCastResult contact = CastOrFail(view, cast);
    REQUIRE(contact.HasHit == true);
    CHECK(contact.Collider == far);
    CHECK(contact.HasStartedOverlapping == false);
    CHECK(contact.DistanceMeters ==
          Catch::Approx(COLLIDER_HEIGHT_METERS - START_HEIGHT_METERS - RADIUS_METERS).margin(1.0e-5));
    CHECK(contact.NormalWorld.Y == Catch::Approx(-1.0).margin(1.0e-9));
}

TEST_CASE("World queries match brute force over every collider and triangle", "[collision][world][oracle]")
{
    GeometryStore store;
    InitializeOrFail(store, 4);
    RandomStream heights;
    REQUIRE(heights.TryReseed(51, 0) == true);
    ShapeHandle terrain;
    REQUIRE(store.TryAddTriangleMesh(CookOrFail(MakeGrid(8, &heights)), terrain) == Status::Success);
    ShapeHandle soup;
    REQUIRE(store.TryAddTriangleMesh(CookOrFail(MakeTriangleSoup(
    {
        .Seed = 52,
        .TriangleCount = 60,
        .ExtentMeters = 3.0f,
    })),
                                     soup) == Status::Success);
    CollisionWorld world;
    constexpr uint32 COLLIDER_COUNT = 24;
    InitializeOrFail(world, store, COLLIDER_COUNT);
    RandomStream random;
    REQUIRE(random.TryReseed(53, 0) == true);
    Array<Vector3d> positions;
    for (uint32 index = 0; index < COLLIDER_COUNT; ++index)
    {
        const Vector3d axis = NextDirection(random);
        const ColliderDescription description
        {
            .Shape = index % 2 == 0 ? terrain : soup,
            .Pose =
            {
                .PositionWorld = Widen(NextPoint(random, 20.0f)),
                .OrientationWorld = MakeRotation(
                    {static_cast<float32>(axis.X), static_cast<float32>(axis.Y), static_cast<float32>(axis.Z)},
                    NextInRange(random, -3.0f, 3.0f)),
            },
            .Participation = { .IsSolid = index % 5 != 0, .IsSensor = index % 5 == 0 },
            .Filter = { .CategoryBits = 1U << (index % 3) },
            .IsEnabled = index % 7 != 0,
            .Binding = index,
        };
        positions.Add(description.Pose.PositionWorld);
        (void)CreateOrFail(world, description);
    }
    CollisionReadView view;
    REQUIRE(world.TryAcquireReadView(view) == Status::Success);
    uint32 rayHits = 0;
    uint32 castHits = 0;
    constexpr uint32 QUERY_COUNT = 400;
    for (uint32 query = 0; query < QUERY_COUNT; ++query)
    {
        const QueryFilter filter
        {
            .CategoryMask = query % 4 == 0 ? 3U : ALL_COLLISION_CATEGORIES,
            .ShouldIncludeSensors = query % 3 == 0,
        };
        // Aim near a collider so most queries reach geometry; some still pass between colliders.
        const Vector3d originWorld = Widen(NextPoint(random, 25.0f));
        const Vector3d targetWorld = positions[query % COLLIDER_COUNT] + Widen(NextPoint(random, 3.0f));
        WorldRaycastInput ray
        {
            .OriginWorld = originWorld,
            .DirectionWorld = (targetWorld - originWorld) * (1.0 / math::Length(targetWorld - originWorld)),
            .MaxDistanceMeters = 60.0,
            .Filter = filter,
        };
        WorldRaycastResult hierarchy;
        WorldRaycastResult brute;
        REQUIRE(view.RaycastClosest(ray, hierarchy) == Status::Success);
        ray.Traversal = QueryTraversal::AllTriangles;
        REQUIRE(view.RaycastClosest(ray, brute) == Status::Success);
        REQUIRE(AreEqual(hierarchy, brute) == true);
        rayHits += hierarchy.HasHit == true ? 1 : 0;

        const Vector3d axis = NextDirection(random);
        WorldConvexCastInput cast
        {
            .Shape = query % 2 == 0 ? MakeCapsuleShape(0.3, 0.2) : MakeBoxShape({0.3, 0.2, 0.4}),
            .PositionWorld = ray.OriginWorld,
            .OrientationWorld =
                MakeRotation({static_cast<float32>(axis.X), static_cast<float32>(axis.Y), static_cast<float32>(axis.Z)},
                             NextInRange(random, -3.0f, 3.0f)),
            .TranslationWorld = ray.DirectionWorld * 50.0,
            .OverlapPolicy = query % 2 == 0 ? InitialOverlapPolicy::Report : InitialOverlapPolicy::Ignore,
            .Filter = filter,
        };
        WorldConvexCastResult castHierarchy;
        WorldConvexCastResult castBrute;
        REQUIRE(view.CastConvexClosest(cast, castHierarchy) == Status::Success);
        cast.Traversal = QueryTraversal::AllTriangles;
        REQUIRE(view.CastConvexClosest(cast, castBrute) == Status::Success);
        REQUIRE(AreEqual(castHierarchy, castBrute) == true);
        castHits += castHierarchy.HasHit == true ? 1 : 0;
    }
    UNSCOPED_INFO("ray hits " << rayHits << " cast hits " << castHits);
    CHECK(rayHits > QUERY_COUNT / 4);
    CHECK(castHits > QUERY_COUNT / 4);
}

TEST_CASE("World queries agree with the same triangles baked into world space", "[collision][world][oracle]")
{
    RandomStream random;
    REQUIRE(random.TryReseed(61, 0) == true);
    const MeshData soup = MakeTriangleSoup({ .Seed = 62, .TriangleCount = 80, .ExtentMeters = 4.0f });
    GeometryStore store;
    InitializeOrFail(store, 1);
    ShapeHandle shape;
    REQUIRE(store.TryAddTriangleMesh(CookOrFail(soup), shape) == Status::Success);
    CollisionWorld world;
    InitializeOrFail(world, store, 1);
    const math::Quaternion rotation = MakeRotation({0.3f, 0.8f, -0.5f}, 1.1f);
    const RigidPose pose{ .PositionWorld = {12.0, -3.0, 7.0}, .OrientationWorld = rotation };
    (void)CreateOrFail(world, { .Shape = shape, .Pose = pose });
    // Bake: rotate and translate every vertex with the float32 math library, then cook a world-space mesh.
    MeshData baked;
    for (const Vector3 position : soup.Positions)
    {
        baked.Positions.Add(math::Rotate(rotation, position) + Vector3{12.0f, -3.0f, 7.0f});
    }
    for (const uint32 index : soup.Indices)
    {
        baked.Indices.Add(index);
    }
    const TriangleMesh bakedMesh = CookOrFail(baked);
    CollisionReadView view;
    REQUIRE(world.TryAcquireReadView(view) == Status::Success);
    uint32 compared = 0;
    constexpr uint32 QUERY_COUNT = 400;
    // float32 baking moves vertices by about 1e-6 m; distances agree to well within this.
    constexpr float64 BAKING_TOLERANCE_METERS = 1.0e-4;
    for (uint32 query = 0; query < QUERY_COUNT; ++query)
    {
        // Aim at a point near the soup so most rays reach it.
        const Vector3d originWorld = pose.PositionWorld + Widen(NextPoint(random, 8.0f));
        const Vector3d targetWorld = pose.PositionWorld + Widen(NextPoint(random, 3.0f));
        const WorldRaycastInput ray
        {
            .OriginWorld = originWorld,
            .DirectionWorld = (targetWorld - originWorld) * (1.0 / math::Length(targetWorld - originWorld)),
            .MaxDistanceMeters = 20.0,
        };
        const WorldRaycastResult worldHit = RaycastOrFail(view, ray);
        RaycastResult bakedHit;
        REQUIRE(bakedMesh.RaycastClosest(
                    {
                        .OriginMesh = ray.OriginWorld,
                        .DirectionMesh = ray.DirectionWorld,
                        .MaxDistanceMeters = ray.MaxDistanceMeters,
                    },
                    bakedHit) == Status::Success);
        if (worldHit.HasHit == false || bakedHit.HasHit == false)
        {
            // A ray grazing an edge may hit one and miss the other by the baking error.
            continue;
        }
        ++compared;
        CHECK(math::Abs(worldHit.DistanceMeters - bakedHit.DistanceMeters) < BAKING_TOLERANCE_METERS);
        if (worldHit.TriangleIndex == bakedHit.TriangleIndex)
        {
            CHECK(math::Dot(worldHit.NormalWorld, bakedHit.NormalMesh) > 0.9999);
            CHECK(worldHit.IsFrontFace == bakedHit.IsFrontFace);
        }
    }
    CHECK(compared > QUERY_COUNT / 4);
}
