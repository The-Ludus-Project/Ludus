// Read views freeze the shared GeometryStore, and query shapes named by
// stored ShapeHandle answer exactly as the same shapes passed by value.

#include "test_meshes.h"

#include <ludus/foundation/base/core.h>

#include <ludus/foundation/containers/array.hpp>
#include <ludus/foundation/containers/borrowed_view.hpp>
#include <ludus/foundation/containers/static_array.hpp>
#include <ludus/foundation/math/quaternion.hpp>
#include <ludus/foundation/math/random.hpp>
#include <ludus/foundation/math/vector.hpp>
#include <ludus/physics/collision/collision_world.h>
#include <ludus/physics/collision/convex_shape.h>
#include <ludus/physics/collision/geometry_store.h>
#include <ludus/physics/collision/queries.h>
#include <ludus/physics/collision/status.h>
#include <ludus/physics/collision/triangle_mesh.h>

#include <catch2/catch_test_macros.hpp>

using namespace ludus::physics::collision;
using namespace ludus::physics::collision::test;
namespace math = ludus::foundation::math;

namespace
{
constexpr usize BOX_CORNER_COUNT = 8;
constexpr usize RECORD_CAPACITY = 16;

ShapeHandle AddFloorOrFail(GeometryStore& inoutStore)
{
    MeshData data;
    AddFloorQuad(data);
    ShapeHandle handle;
    REQUIRE(inoutStore.TryAddTriangleMesh(CookOrFail(data), handle) == Status::Success);
    return handle;
}

ShapeHandle AddCubeHullOrFail(GeometryStore& inoutStore, float32 halfExtent)
{
    Array<Vector3> points;
    for (uint32 corner = 0; corner < BOX_CORNER_COUNT; ++corner)
    {
        points.Add({(corner & 1U) == 0 ? -halfExtent : halfExtent,
                    (corner & 2U) == 0 ? -halfExtent : halfExtent,
                    (corner & 4U) == 0 ? -halfExtent : halfExtent});
    }
    ConvexHull hull;
    REQUIRE(ConvexHull::TryCook(
                {
                    .PointsShape = BorrowedView<const Vector3>(points.GetData(), points.GetSize()),
                },
                hull)
                .Result == Status::Success);
    ShapeHandle handle;
    REQUIRE(inoutStore.TryAddConvexHull(ludus::foundation::Move(hull), handle) == Status::Success);
    return handle;
}

ColliderHandle CreateOrFail(CollisionWorld& inoutWorld, const ColliderDescription& description)
{
    ColliderHandle collider;
    REQUIRE(inoutWorld.TryCreateCollider(description, collider) == Status::Success);
    return collider;
}

bool AreEqual(const WorldConvexCastResult& first, const WorldConvexCastResult& second)
{
    return first.HasHit == second.HasHit && first.Fraction == second.Fraction &&
           first.HasStartedOverlapping == second.HasStartedOverlapping && first.HasNormal == second.HasNormal &&
           first.NormalWorld == second.NormalWorld && first.HasWitness == second.HasWitness &&
           first.WitnessPositionWorld == second.WitnessPositionWorld && first.TriangleIndex == second.TriangleIndex &&
           first.Collider == second.Collider;
}
} // namespace

TEST_CASE("A read view of any world freezes the shared geometry store", "[collision][world][store]")
{
    GeometryStore store;
    REQUIRE(store.TryInitialize({ .MaxShapes = 4 }) == Status::Success);
    const ShapeHandle floor = AddFloorOrFail(store);
    CollisionWorld first;
    CollisionWorld second;
    REQUIRE(first.TryInitialize(store, { .MaxColliders = 4 }) == Status::Success);
    REQUIRE(second.TryInitialize(store, { .MaxColliders = 4 }) == Status::Success);
    (void)CreateOrFail(first, { .Shape = floor });
    const ColliderHandle other = CreateOrFail(second, { .Shape = floor });
    uint32 leases = 0;
    REQUIRE(store.TryGetLeaseCount(floor, leases) == Status::Success);
    CHECK(leases == 3);

    CollisionReadView view;
    REQUIRE(first.TryAcquireReadView(view) == Status::Success);
    CHECK(store.GetReaderCount() == 1);
    const ShapeHandle sentinel{ .Owner = 9, .Slot = 9, .Generation = 9 };
    ShapeHandle added = sentinel;
    CHECK(store.TryAddPrimitive(MakeSphereShape(0.5), added) == Status::InvalidPhase);
    CHECK(store.TryAddTriangleMesh(TriangleMesh{}, added) == Status::InvalidPhase);
    CHECK(added == sentinel);
    CHECK(store.TryRetain(floor) == Status::InvalidPhase);
    CHECK(store.TryRelease(floor) == Status::InvalidPhase);
    REQUIRE(store.TryGetLeaseCount(floor, leases) == Status::Success);
    CHECK(leases == 3);
    // The other world cannot lease from or release to the frozen store, but may move its colliders.
    ColliderHandle created;
    CHECK(second.TryCreateCollider({ .Shape = floor }, created) == Status::InvalidPhase);
    CHECK(second.TryDestroyCollider(other) == Status::InvalidPhase);
    CHECK(second.IsAlive(other) == true);
    CHECK(second.TrySetColliderPose(other, { .PositionWorld = {0.0, 1.0, 0.0} }) == Status::Success);
    // Reads stay available.
    ShapeKind kind = ShapeKind::Sphere;
    CHECK(store.TryGetKind(floor, kind) == Status::Success);

    view.Release();
    CHECK(store.GetReaderCount() == 0);
    REQUIRE(store.TryRetain(floor) == Status::Success);
    REQUIRE(store.TryRelease(floor) == Status::Success);
    REQUIRE(second.TryDestroyCollider(other) == Status::Success);
    REQUIRE(second.TryCreateCollider({ .Shape = floor }, created) == Status::Success);
    REQUIRE(store.TryAddPrimitive(MakeSphereShape(0.5), added) == Status::Success);
}

TEST_CASE("Stored query shapes answer exactly as the same shapes by value", "[collision][world][store]")
{
    GeometryStore store;
    REQUIRE(store.TryInitialize({ .MaxShapes = 6 }) == Status::Success);
    RandomStream heights;
    REQUIRE(heights.TryReseed(141, 0) == true);
    ShapeHandle terrain;
    REQUIRE(store.TryAddTriangleMesh(CookOrFail(MakeGrid(6, &heights)), terrain) == Status::Success);
    ShapeHandle ball;
    REQUIRE(store.TryAddPrimitive(MakeSphereShape(0.8), ball) == Status::Success);
    const ShapeHandle cube = AddCubeHullOrFail(store, 0.3f);
    ShapeHandle capsule;
    REQUIRE(store.TryAddPrimitive(MakeCapsuleShape(0.4, 0.2), capsule) == Status::Success);
    ConvexShape cubeValue;
    REQUIRE(store.TryGetConvexShape(cube, cubeValue) == Status::Success);
    CollisionWorld world;
    REQUIRE(world.TryInitialize(store, { .MaxColliders = 16 }) == Status::Success);
    RandomStream random;
    REQUIRE(random.TryReseed(142, 0) == true);
    for (uint32 index = 0; index < 12; ++index)
    {
        (void)CreateOrFail(world,
                           {
                               .Shape = index % 2 == 0 ? terrain : ball,
                               .Pose = { .PositionWorld = Widen(NextPoint(random, 6.0f)) },
                               .Binding = index,
                           });
    }
    CollisionReadView view;
    REQUIRE(world.TryAcquireReadView(view) == Status::Success);
    const StaticArray<ShapeHandle, 2> handles{cube, capsule};
    const StaticArray<ConvexShape, 2> values{cubeValue, MakeCapsuleShape(0.4, 0.2)};
    uint32 hits = 0;
    constexpr uint32 QUERY_COUNT = 120;
    for (uint32 query = 0; query < QUERY_COUNT; ++query)
    {
        const usize kind = query % handles.GetSize();
        const Vector3d origin = Widen(NextPoint(random, 9.0f));
        WorldConvexCastInput byValue
        {
            .Shape = values[kind],
            .PositionWorld = origin,
            .TranslationWorld = NextDirection(random) * 12.0,
        };
        // A stored shape takes precedence; the value shape is then not even validated.
        WorldConvexCastInput byHandle = byValue;
        byHandle.Shape = MakeSphereShape(0.0);
        byHandle.QueryShape = handles[kind];
        WorldConvexCastResult valueResult;
        WorldConvexCastResult handleResult;
        REQUIRE(view.CastConvexClosest(byValue, valueResult) == Status::Success);
        REQUIRE(view.CastConvexClosest(byHandle, handleResult) == Status::Success);
        REQUIRE(AreEqual(valueResult, handleResult) == true);
        hits += valueResult.HasHit == true ? 1 : 0;
        StaticArray<WorldConvexCastResult, RECORD_CAPACITY> valueAll{};
        StaticArray<WorldConvexCastResult, RECORD_CAPACITY> handleAll{};
        WorldHitsResult valueHits;
        WorldHitsResult handleHits;
        REQUIRE(view.CastConvexAll(byValue,
                                   BorrowedView<WorldConvexCastResult>(valueAll.GetData(), RECORD_CAPACITY),
                                   valueHits) == Status::Success);
        REQUIRE(view.CastConvexAll(byHandle,
                                   BorrowedView<WorldConvexCastResult>(handleAll.GetData(), RECORD_CAPACITY),
                                   handleHits) == Status::Success);
        REQUIRE(valueHits.HitCount == handleHits.HitCount);
        for (usize index = 0; index < valueHits.WrittenCount; ++index)
        {
            REQUIRE(AreEqual(valueAll[index], handleAll[index]) == true);
        }
        WorldConvexCastResult valueAny;
        WorldConvexCastResult handleAny;
        REQUIRE(view.CastConvexAny(byValue, valueAny) == Status::Success);
        REQUIRE(view.CastConvexAny(byHandle, handleAny) == Status::Success);
        REQUIRE(AreEqual(valueAny, handleAny) == true);

        WorldOverlapInput overlapByValue{ .Shape = values[kind], .PositionWorld = origin };
        WorldOverlapInput overlapByHandle = overlapByValue;
        overlapByHandle.Shape = {};
        overlapByHandle.QueryShape = handles[kind];
        StaticArray<ColliderOverlap, RECORD_CAPACITY> valueRecords{};
        StaticArray<ColliderOverlap, RECORD_CAPACITY> handleRecords{};
        WorldOverlapResult valueOverlap;
        WorldOverlapResult handleOverlap;
        REQUIRE(view.Overlap(overlapByValue,
                             BorrowedView<ColliderOverlap>(valueRecords.GetData(), RECORD_CAPACITY),
                             valueOverlap) == Status::Success);
        REQUIRE(view.Overlap(overlapByHandle,
                             BorrowedView<ColliderOverlap>(handleRecords.GetData(), RECORD_CAPACITY),
                             handleOverlap) == Status::Success);
        REQUIRE(valueOverlap.OverlapCount == handleOverlap.OverlapCount);
        for (usize index = 0; index < valueOverlap.WrittenCount; ++index)
        {
            REQUIRE(valueRecords[index].Collider == handleRecords[index].Collider);
        }
    }
    CHECK(hits > QUERY_COUNT / 10);
}

TEST_CASE("Stored query shapes must be live convex shapes of the world's store", "[collision][world][store]")
{
    GeometryStore store;
    REQUIRE(store.TryInitialize({ .MaxShapes = 4 }) == Status::Success);
    const ShapeHandle floor = AddFloorOrFail(store);
    ShapeHandle released;
    REQUIRE(store.TryAddPrimitive(MakeSphereShape(0.5), released) == Status::Success);
    REQUIRE(store.TryRelease(released) == Status::Success);
    GeometryStore otherStore;
    REQUIRE(otherStore.TryInitialize({ .MaxShapes = 1 }) == Status::Success);
    ShapeHandle foreign;
    REQUIRE(otherStore.TryAddPrimitive(MakeSphereShape(0.5), foreign) == Status::Success);
    CollisionWorld world;
    REQUIRE(world.TryInitialize(store, { .MaxColliders = 2 }) == Status::Success);
    (void)CreateOrFail(world, { .Shape = floor });
    CollisionReadView view;
    REQUIRE(world.TryAcquireReadView(view) == Status::Success);
    struct Expectation final
    {
        ShapeHandle Shape;
        Status Result;
    };
    const StaticArray<Expectation, 3> expectations{Expectation{released, Status::InvalidHandle},
                                                   Expectation{foreign, Status::InvalidHandle},
                                                   Expectation{floor, Status::Unsupported}};
    for (const Expectation& expectation : expectations)
    {
        const WorldConvexCastInput cast
        {
            .QueryShape = expectation.Shape,
            .PositionWorld = {0.5, 2.0, 0.5},
            .TranslationWorld = {0.0, -3.0, 0.0},
        };
        WorldConvexCastResult result;
        CHECK(view.CastConvexClosest(cast, result) == expectation.Result);
        CHECK(result.HasHit == false);
        CHECK(view.CastConvexAny(cast, result) == expectation.Result);
        WorldHitsResult hits{ .HitCount = 7 };
        CHECK(view.CastConvexAll(cast, BorrowedView<WorldConvexCastResult>(), hits) == expectation.Result);
        CHECK(hits.HitCount == 7);
        WorldOverlapResult overlap{ .OverlapCount = 7 };
        CHECK(view.Overlap({ .QueryShape = expectation.Shape, .PositionWorld = {0.5, 0.0, 0.5} },
                           BorrowedView<ColliderOverlap>(),
                           overlap) == expectation.Result);
        CHECK(overlap.OverlapCount == 7);
    }
}
