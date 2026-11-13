// Any and All query modes for world rays and convex casts: completion,
// ordering, overlap policies, and consistency with the closest queries.

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

using namespace ludus::physics::collision;
using namespace ludus::physics::collision::test;
namespace math = ludus::foundation::math;

namespace
{
constexpr uint32 FLOOR_COUNT = 5;
constexpr usize LARGE_BUFFER = 64;

/// A store and world sized for one test.
struct Scene final
{
    GeometryStore Store;
    CollisionWorld World;
};

void InitializeOrFail(Scene& inoutScene,
                      const GeometryStoreSettings& storeSettings,
                      const CollisionWorldSettings& worldSettings)
{
    REQUIRE(inoutScene.Store.TryInitialize(storeSettings) == Status::Success);
    REQUIRE(inoutScene.World.TryInitialize(inoutScene.Store, worldSettings) == Status::Success);
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

math::Quaternion MakeRotation(Vector3d axis, float64 radians)
{
    math::Quaternion rotation;
    REQUIRE(math::TryFromAxisAngle(
                {static_cast<float32>(axis.X), static_cast<float32>(axis.Y), static_cast<float32>(axis.Z)},
                static_cast<float32>(radians),
                rotation) == math::MathStatus::Success);
    return rotation;
}

/// Floors at y = 0, 1, ..., FLOOR_COUNT - 1 in slot order, with their height as binding.
void AddStackedFloors(Scene& inoutScene)
{
    const ShapeHandle floor = AddFloorOrFail(inoutScene.Store);
    for (uint32 level = 0; level < FLOOR_COUNT; ++level)
    {
        (void)CreateOrFail(inoutScene.World,
                           {
                               .Shape = floor,
                               .Pose = { .PositionWorld = {0.0, static_cast<float64>(level), 0.0} },
                               .Binding = level,
                           });
    }
}

WorldRaycastInput MakeDownRay()
{
    return { .OriginWorld = {0.5, 10.0, 0.5}, .DirectionWorld = {0.0, -1.0, 0.0}, .MaxDistanceMeters = 20.0 };
}

bool AreEqual(const WorldRaycastResult& first, const WorldRaycastResult& second)
{
    return first.HasHit == second.HasHit && first.DistanceMeters == second.DistanceMeters &&
           first.PositionWorld == second.PositionWorld && first.HasNormal == second.HasNormal &&
           first.NormalWorld == second.NormalWorld && first.IsFrontFace == second.IsFrontFace &&
           first.TriangleIndex == second.TriangleIndex && first.Collider == second.Collider &&
           first.Binding == second.Binding;
}

bool AreEqual(const WorldConvexCastResult& first, const WorldConvexCastResult& second)
{
    return first.HasHit == second.HasHit && first.Fraction == second.Fraction &&
           first.HasStartedOverlapping == second.HasStartedOverlapping && first.HasNormal == second.HasNormal &&
           first.NormalWorld == second.NormalWorld && first.HasWitness == second.HasWitness &&
           first.WitnessPositionWorld == second.WitnessPositionWorld && first.TriangleIndex == second.TriangleIndex &&
           first.Collider == second.Collider && first.Binding == second.Binding;
}

bool IsRaySorted(const StaticArray<WorldRaycastResult, LARGE_BUFFER>& records, usize count)
{
    for (usize index = 1; index < count; ++index)
    {
        if (records[index - 1].DistanceMeters > records[index].DistanceMeters)
        {
            return false;
        }
    }
    return true;
}
} // namespace

TEST_CASE("RaycastAll reports every hit collider in distance order with completion", "[collision][world][modes]")
{
    Scene scene;
    InitializeOrFail(scene, { .MaxShapes = 1 }, { .MaxColliders = FLOOR_COUNT });
    AddStackedFloors(scene);
    CollisionReadView view;
    REQUIRE(scene.World.TryAcquireReadView(view) == Status::Success);
    StaticArray<WorldRaycastResult, LARGE_BUFFER> records{};
    WorldHitsResult hits;
    const WorldRaycastInput ray = MakeDownRay();
    REQUIRE(view.RaycastAll(ray, BorrowedView<WorldRaycastResult>(records.GetData(), LARGE_BUFFER), hits) ==
            Status::Success);
    CHECK(hits.HasHit == true);
    CHECK(hits.HitCount == FLOOR_COUNT);
    CHECK(hits.WrittenCount == FLOOR_COUNT);
    CHECK(hits.IsComplete == true);
    for (uint32 index = 0; index < FLOOR_COUNT; ++index)
    {
        // Highest floor first.
        CHECK(records[index].Binding == FLOOR_COUNT - 1 - index);
        CHECK(records[index].DistanceMeters == Catch::Approx(10.0 - static_cast<float64>(FLOOR_COUNT - 1 - index)));
    }
    WorldRaycastResult closest;
    REQUIRE(view.RaycastClosest(ray, closest) == Status::Success);
    CHECK(AreEqual(records[0], closest) == true);

    // A short buffer keeps counting; the written records are the first hits the search found, sorted.
    REQUIRE(view.RaycastAll(ray, BorrowedView<WorldRaycastResult>(records.GetData(), 2), hits) == Status::Success);
    CHECK(hits.HitCount == FLOOR_COUNT);
    CHECK(hits.WrittenCount == 2);
    CHECK(hits.IsComplete == false);
    CHECK(records[0].DistanceMeters < records[1].DistanceMeters);
    for (usize index = 0; index < 2; ++index)
    {
        CHECK(records[index].DistanceMeters == Catch::Approx(10.0 - static_cast<float64>(records[index].Binding)));
    }
    REQUIRE(view.RaycastAll(ray, BorrowedView<WorldRaycastResult>(), hits) == Status::Success);
    CHECK(hits.HasHit == true);
    CHECK(hits.WrittenCount == 0);
    CHECK(hits.IsComplete == false);

    // A miss is complete and empty; filters apply; an invalid buffer fails without changing the result.
    WorldRaycastInput away = ray;
    away.DirectionWorld = {0.0, 1.0, 0.0};
    REQUIRE(view.RaycastAll(away, BorrowedView<WorldRaycastResult>(records.GetData(), LARGE_BUFFER), hits) ==
            Status::Success);
    CHECK(hits.HasHit == false);
    CHECK(hits.HitCount == 0);
    CHECK(hits.IsComplete == true);
    WorldRaycastInput limited = ray;
    limited.MaxDistanceMeters = 7.5;
    REQUIRE(view.RaycastAll(limited, BorrowedView<WorldRaycastResult>(records.GetData(), LARGE_BUFFER), hits) ==
            Status::Success);
    CHECK(hits.HitCount == 2);
    const WorldHitsResult sentinel{ .HasHit = true, .HitCount = 9, .WrittenCount = 9, .IsComplete = false };
    WorldHitsResult unchanged = sentinel;
    CHECK(view.RaycastAll(ray, BorrowedView<WorldRaycastResult>(nullptr, 3), unchanged) == Status::InvalidArgument);
    CHECK(unchanged.HitCount == 9);
    CollisionReadView inactive;
    CHECK(inactive.RaycastAll(ray, BorrowedView<WorldRaycastResult>(), unchanged) == Status::InvalidPhase);
    CHECK(inactive.RaycastAny(ray, closest) == Status::InvalidPhase);
}

TEST_CASE("RaycastAny reports a genuine hit and certifies misses", "[collision][world][modes]")
{
    Scene scene;
    InitializeOrFail(scene, { .MaxShapes = 1 }, { .MaxColliders = FLOOR_COUNT });
    AddStackedFloors(scene);
    CollisionReadView view;
    REQUIRE(scene.World.TryAcquireReadView(view) == Status::Success);
    WorldRaycastResult any;
    REQUIRE(view.RaycastAny(MakeDownRay(), any) == Status::Success);
    REQUIRE(any.HasHit == true);
    // Any stops at the first hit the search finds: a genuine floor hit, not necessarily the nearest.
    CHECK(any.Binding < FLOOR_COUNT);
    CHECK(any.DistanceMeters == Catch::Approx(10.0 - static_cast<float64>(any.Binding)));
    CHECK(any.HasNormal == true);
    WorldRaycastInput away = MakeDownRay();
    away.DirectionWorld = {0.0, 1.0, 0.0};
    REQUIRE(view.RaycastAny(away, any) == Status::Success);
    CHECK(any.HasHit == false);
}

TEST_CASE("Convex cast modes follow the closest-hit rules and the overlap policies", "[collision][world][modes]")
{
    Scene scene;
    InitializeOrFail(scene, { .MaxShapes = 1 }, { .MaxColliders = FLOOR_COUNT });
    AddStackedFloors(scene);
    CollisionReadView view;
    REQUIRE(scene.World.TryAcquireReadView(view) == Status::Success);
    StaticArray<WorldConvexCastResult, LARGE_BUFFER> records{};
    WorldHitsResult hits;
    // A sphere falling through every floor contacts each one at its own first contact.
    WorldConvexCastInput falling
    {
        .Shape = MakeSphereShape(0.25),
        .PositionWorld = {0.5, 10.0, 0.5},
        .TranslationWorld = {0.0, -20.0, 0.0},
    };
    REQUIRE(view.CastConvexAll(falling, BorrowedView<WorldConvexCastResult>(records.GetData(), LARGE_BUFFER), hits) ==
            Status::Success);
    REQUIRE(hits.HitCount == FLOOR_COUNT);
    CHECK(hits.IsComplete == true);
    for (uint32 index = 0; index < FLOOR_COUNT; ++index)
    {
        const float64 height = static_cast<float64>(FLOOR_COUNT - 1 - index);
        CHECK(records[index].Binding == FLOOR_COUNT - 1 - index);
        CHECK(records[index].Fraction == Catch::Approx((10.0 - 0.25 - height) / 20.0).margin(1.0e-9));
    }
    WorldConvexCastResult closest;
    REQUIRE(view.CastConvexClosest(falling, closest) == Status::Success);
    CHECK(AreEqual(records[0], closest) == true);

    // Starting inside floor 2 and moving up: Closest reports the overlap, Any reports one of the two hits,
    // Ignore drops the overlapping floor, and Reject fails every mode.
    WorldConvexCastInput rising
    {
        .Shape = MakeSphereShape(0.25),
        .PositionWorld = {0.5, 2.1, 0.5},
        .TranslationWorld = {0.0, 1.5, 0.0},
    };
    REQUIRE(view.CastConvexClosest(rising, closest) == Status::Success);
    CHECK(closest.HasStartedOverlapping == true);
    CHECK(closest.Binding == 2);
    REQUIRE(view.CastConvexAll(rising, BorrowedView<WorldConvexCastResult>(records.GetData(), LARGE_BUFFER), hits) ==
            Status::Success);
    CHECK(hits.HitCount == 2);
    CHECK(records[0].HasStartedOverlapping == true);
    CHECK(records[0].Binding == 2);
    CHECK(records[1].Binding == 3);
    WorldConvexCastResult any;
    REQUIRE(view.CastConvexAny(rising, any) == Status::Success);
    CHECK(any.HasHit == true);
    CHECK((any.Binding == 2 || any.Binding == 3) == true);
    rising.OverlapPolicy = InitialOverlapPolicy::Ignore;
    REQUIRE(view.CastConvexAll(rising, BorrowedView<WorldConvexCastResult>(records.GetData(), LARGE_BUFFER), hits) ==
            Status::Success);
    CHECK(hits.HitCount == 1);
    CHECK(records[0].Binding == 3);
    rising.OverlapPolicy = InitialOverlapPolicy::Reject;
    WorldHitsResult unchanged{ .HitCount = 9 };
    CHECK(view.CastConvexAll(rising, BorrowedView<WorldConvexCastResult>(records.GetData(), LARGE_BUFFER), unchanged) ==
          Status::InitialOverlap);
    CHECK(unchanged.HitCount == 9);
    WorldConvexCastResult rejected;
    CHECK(view.CastConvexAny(rising, rejected) == Status::InitialOverlap);
    CHECK(rejected.HasHit == false);
}

TEST_CASE("Under Reject an Any cast still searches every collider", "[collision][world][modes]")
{
    // One floor is hit without overlap; the other overlaps the start. Report may stop at either, so Reject must
    // search on and find the overlap whichever it meets first.
    Scene scene;
    InitializeOrFail(scene, { .MaxShapes = 1 }, { .MaxColliders = 2 });
    const ShapeHandle floor = AddFloorOrFail(scene.Store);
    (void)CreateOrFail(scene.World, { .Shape = floor, .Pose = { .PositionWorld = {0.0, 5.0, 0.0} }, .Binding = 5 });
    (void)CreateOrFail(scene.World, { .Shape = floor, .Binding = 0 });
    CollisionReadView view;
    REQUIRE(scene.World.TryAcquireReadView(view) == Status::Success);
    WorldConvexCastInput cast
    {
        .Shape = MakeSphereShape(0.25),
        .PositionWorld = {0.5, 0.1, 0.5},
        .TranslationWorld = {0.0, 6.0, 0.0},
    };
    WorldConvexCastResult any;
    REQUIRE(view.CastConvexAny(cast, any) == Status::Success);
    CHECK(any.HasHit == true);
    cast.OverlapPolicy = InitialOverlapPolicy::Reject;
    CHECK(view.CastConvexAny(cast, any) == Status::InitialOverlap);
    // The brute-force traversal visits the clean floor first; it must still reject.
    cast.Traversal = QueryTraversal::AllTriangles;
    CHECK(view.CastConvexAny(cast, any) == Status::InitialOverlap);
}

TEST_CASE("Any and All agree with the closest queries on a mixed world", "[collision][world][modes][oracle]")
{
    Scene scene;
    InitializeOrFail(scene, { .MaxShapes = 4 }, { .MaxColliders = 24 });
    RandomStream heights;
    REQUIRE(heights.TryReseed(91, 0) == true);
    ShapeHandle terrain;
    REQUIRE(scene.Store.TryAddTriangleMesh(CookOrFail(MakeGrid(6, &heights)), terrain) == Status::Success);
    ShapeHandle soup;
    REQUIRE(scene.Store.TryAddTriangleMesh(
                CookOrFail(MakeTriangleSoup({ .Seed = 92, .TriangleCount = 40, .ExtentMeters = 2.0f })),
                soup) == Status::Success);
    ShapeHandle sphere;
    REQUIRE(scene.Store.TryAddPrimitive(MakeSphereShape(0.7), sphere) == Status::Success);
    ShapeHandle box;
    REQUIRE(scene.Store.TryAddPrimitive(MakeBoxShape({0.6, 0.4, 0.8}), box) == Status::Success);
    const StaticArray<ShapeHandle, 4> shapes{terrain, soup, sphere, box};
    RandomStream random;
    REQUIRE(random.TryReseed(93, 0) == true);
    constexpr uint32 COLLIDER_COUNT = 24;
    Array<Vector3d> positions;
    for (uint32 index = 0; index < COLLIDER_COUNT; ++index)
    {
        const ColliderDescription description
        {
            .Shape = shapes[index % shapes.GetSize()],
            .Pose =
            {
                .PositionWorld = Widen(NextPoint(random, 8.0f)),
                .OrientationWorld =
                    MakeRotation(NextDirection(random), static_cast<float64>(NextInRange(random, -3.0f, 3.0f))),
            },
            .Binding = index,
        };
        positions.Add(description.Pose.PositionWorld);
        (void)CreateOrFail(scene.World, description);
    }
    CollisionReadView view;
    REQUIRE(scene.World.TryAcquireReadView(view) == Status::Success);
    constexpr uint32 QUERY_COUNT = 300;
    constexpr usize SHORT_BUFFER = 2;
    uint32 multipleHits = 0;
    uint32 incomplete = 0;
    for (uint32 query = 0; query < QUERY_COUNT; ++query)
    {
        INFO("query " << query);
        const Vector3d origin = Widen(NextPoint(random, 12.0f));
        const Vector3d target = positions[query % COLLIDER_COUNT] + Widen(NextPoint(random, 1.0f));
        const Vector3d direction = (target - origin) * (1.0 / math::Length(target - origin));
        WorldRaycastInput ray{ .OriginWorld = origin, .DirectionWorld = direction, .MaxDistanceMeters = 30.0 };
        WorldRaycastResult closest;
        REQUIRE(view.RaycastClosest(ray, closest) == Status::Success);
        StaticArray<WorldRaycastResult, LARGE_BUFFER> all{};
        WorldHitsResult hits;
        REQUIRE(view.RaycastAll(ray, BorrowedView<WorldRaycastResult>(all.GetData(), LARGE_BUFFER), hits) ==
                Status::Success);
        REQUIRE(hits.IsComplete == true);
        REQUIRE(hits.HasHit == closest.HasHit);
        REQUIRE(IsRaySorted(all, hits.WrittenCount) == true);
        if (closest.HasHit == true)
        {
            REQUIRE(AreEqual(all[0], closest) == true);
        }
        multipleHits += hits.HitCount > 1 ? 1 : 0;
        // The brute-force traversal writes the same records.
        ray.Traversal = QueryTraversal::AllTriangles;
        StaticArray<WorldRaycastResult, LARGE_BUFFER> brute{};
        WorldHitsResult bruteHits;
        REQUIRE(view.RaycastAll(ray, BorrowedView<WorldRaycastResult>(brute.GetData(), LARGE_BUFFER), bruteHits) ==
                Status::Success);
        REQUIRE(bruteHits.HitCount == hits.HitCount);
        for (usize index = 0; index < hits.WrittenCount; ++index)
        {
            REQUIRE(AreEqual(brute[index], all[index]) == true);
        }
        ray.Traversal = QueryTraversal::Hierarchy;
        // Any hits exactly when Closest does, on a collider All found, no nearer than Closest and no farther than
        // that collider's own closest hit allows: a later triangle of the same mesh, within the limit.
        WorldRaycastResult any;
        REQUIRE(view.RaycastAny(ray, any) == Status::Success);
        REQUIRE(any.HasHit == closest.HasHit);
        if (any.HasHit == true)
        {
            bool isListed = false;
            for (usize index = 0; index < hits.WrittenCount; ++index)
            {
                if (all[index].Collider == any.Collider)
                {
                    isListed = true;
                    CHECK(any.DistanceMeters >= all[index].DistanceMeters);
                }
            }
            CHECK(isListed == true);
            CHECK(any.DistanceMeters >= closest.DistanceMeters);
            CHECK(any.DistanceMeters <= ray.MaxDistanceMeters);
        }
        // A short buffer keeps a sorted subset and the full count.
        StaticArray<WorldRaycastResult, LARGE_BUFFER> partial{};
        WorldHitsResult partialHits;
        REQUIRE(view.RaycastAll(ray, BorrowedView<WorldRaycastResult>(partial.GetData(), SHORT_BUFFER), partialHits) ==
                Status::Success);
        REQUIRE(partialHits.HitCount == hits.HitCount);
        REQUIRE(partialHits.WrittenCount == math::Min(hits.HitCount, SHORT_BUFFER));
        REQUIRE(partialHits.IsComplete == (hits.HitCount <= SHORT_BUFFER));
        REQUIRE(IsRaySorted(partial, partialHits.WrittenCount) == true);
        incomplete += partialHits.IsComplete == false ? 1 : 0;

        const WorldConvexCastInput cast
        {
            .Shape = query % 2 == 0 ? MakeCapsuleShape(0.3, 0.2) : MakeBoxShape({0.3, 0.2, 0.4}),
            .PositionWorld = origin,
            .OrientationWorld =
                MakeRotation(NextDirection(random), static_cast<float64>(NextInRange(random, -3.0f, 3.0f))),
            .TranslationWorld = direction * 25.0,
        };
        WorldConvexCastResult castClosest;
        REQUIRE(view.CastConvexClosest(cast, castClosest) == Status::Success);
        StaticArray<WorldConvexCastResult, LARGE_BUFFER> castAll{};
        WorldHitsResult castHits;
        REQUIRE(view.CastConvexAll(cast,
                                   BorrowedView<WorldConvexCastResult>(castAll.GetData(), LARGE_BUFFER),
                                   castHits) == Status::Success);
        REQUIRE(castHits.HasHit == castClosest.HasHit);
        if (castClosest.HasHit == true)
        {
            REQUIRE(AreEqual(castAll[0], castClosest) == true);
        }
        WorldConvexCastResult castAny;
        REQUIRE(view.CastConvexAny(cast, castAny) == Status::Success);
        REQUIRE(castAny.HasHit == castClosest.HasHit);
        if (castAny.HasHit == true)
        {
            CHECK(castAny.Fraction >= castClosest.Fraction);
            CHECK(castAny.Fraction < 1.0);
        }
    }
    UNSCOPED_INFO("multiple hits " << multipleHits << " incomplete " << incomplete);
    CHECK(multipleHits > QUERY_COUNT / 10);
    CHECK(incomplete > 0);
}
