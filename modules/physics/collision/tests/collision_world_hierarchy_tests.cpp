// The collider hierarchy: balance under adversarial insertion, consistency
// through create, move and destroy churn, and exact agreement with the
// brute-force traversal.

#include "test_meshes.h"

#include <ludus/foundation/base/core.h>

#include <ludus/foundation/containers/array.hpp>
#include <ludus/foundation/containers/borrowed_view.hpp>
#include <ludus/foundation/containers/static_array.hpp>
#include <ludus/foundation/math/random.hpp>
#include <ludus/foundation/math/scalar.hpp>
#include <ludus/foundation/math/vector.hpp>
#include <ludus/foundation/time/time.hpp>
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
constexpr usize RECORD_CAPACITY = 64;

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

ColliderHandle CreateOrFail(CollisionWorld& inoutWorld, const ColliderDescription& description)
{
    ColliderHandle collider;
    REQUIRE(inoutWorld.TryCreateCollider(description, collider) == Status::Success);
    return collider;
}

/// The AVL bound on the height of a balanced tree of `leafCount` leaves, counting a single leaf as one: about
/// 1.44 log2(n + 2), checked here with a margin of one level.
uint32 GetBalancedHeightBound(usize leafCount)
{
    constexpr float64 AVL_FACTOR = 1.4405;
    const float64 levels = math::Log(static_cast<float64>(leafCount + 2)) / math::Log(2.0);
    return static_cast<uint32>(AVL_FACTOR * levels) + 2;
}

bool AreEqual(const WorldRaycastResult& first, const WorldRaycastResult& second)
{
    return first.HasHit == second.HasHit && first.DistanceMeters == second.DistanceMeters &&
           first.NormalWorld == second.NormalWorld && first.TriangleIndex == second.TriangleIndex &&
           first.Collider == second.Collider;
}

bool AreEqual(const WorldConvexCastResult& first, const WorldConvexCastResult& second)
{
    return first.HasHit == second.HasHit && first.Fraction == second.Fraction &&
           first.HasStartedOverlapping == second.HasStartedOverlapping && first.NormalWorld == second.NormalWorld &&
           first.TriangleIndex == second.TriangleIndex && first.Collider == second.Collider;
}

/// Compare hierarchy and brute-force answers for rays, casts, All rays and overlaps from random starts.
void RequireHierarchyMatchesBruteForce(const CollisionReadView& view, RandomStream& inoutRandom, uint32 queryCount)
{
    for (uint32 query = 0; query < queryCount; ++query)
    {
        const Vector3d origin = Widen(NextPoint(inoutRandom, 30.0f));
        const Vector3d direction = NextDirection(inoutRandom);
        WorldRaycastInput ray{ .OriginWorld = origin, .DirectionWorld = direction, .MaxDistanceMeters = 60.0 };
        WorldRaycastResult hierarchy;
        WorldRaycastResult brute;
        REQUIRE(view.RaycastClosest(ray, hierarchy) == Status::Success);
        ray.Traversal = QueryTraversal::AllTriangles;
        REQUIRE(view.RaycastClosest(ray, brute) == Status::Success);
        REQUIRE(AreEqual(hierarchy, brute) == true);

        StaticArray<WorldRaycastResult, RECORD_CAPACITY> bruteAll{};
        StaticArray<WorldRaycastResult, RECORD_CAPACITY> hierarchyAll{};
        WorldHitsResult bruteHits;
        WorldHitsResult hierarchyHits;
        REQUIRE(view.RaycastAll(ray,
                                BorrowedView<WorldRaycastResult>(bruteAll.GetData(), RECORD_CAPACITY),
                                bruteHits) == Status::Success);
        ray.Traversal = QueryTraversal::Hierarchy;
        REQUIRE(view.RaycastAll(ray,
                                BorrowedView<WorldRaycastResult>(hierarchyAll.GetData(), RECORD_CAPACITY),
                                hierarchyHits) == Status::Success);
        REQUIRE(hierarchyHits.HitCount == bruteHits.HitCount);
        REQUIRE(hierarchyHits.IsComplete == bruteHits.IsComplete);
        if (hierarchyHits.IsComplete == true)
        {
            for (usize index = 0; index < hierarchyHits.WrittenCount; ++index)
            {
                REQUIRE(AreEqual(hierarchyAll[index], bruteAll[index]) == true);
            }
        }

        WorldConvexCastInput cast
        {
            .Shape = MakeCapsuleShape(0.4, 0.3),
            .PositionWorld = origin,
            .TranslationWorld = direction * 40.0,
        };
        WorldConvexCastResult castHierarchy;
        WorldConvexCastResult castBrute;
        REQUIRE(view.CastConvexClosest(cast, castHierarchy) == Status::Success);
        cast.Traversal = QueryTraversal::AllTriangles;
        REQUIRE(view.CastConvexClosest(cast, castBrute) == Status::Success);
        REQUIRE(AreEqual(castHierarchy, castBrute) == true);

        WorldOverlapInput overlap{ .Shape = MakeBoxShape({2.0, 2.0, 2.0}), .PositionWorld = origin };
        StaticArray<ColliderOverlap, RECORD_CAPACITY> hierarchyRecords{};
        StaticArray<ColliderOverlap, RECORD_CAPACITY> bruteRecords{};
        WorldOverlapResult hierarchyOverlap;
        WorldOverlapResult bruteOverlap;
        REQUIRE(view.Overlap(overlap,
                             BorrowedView<ColliderOverlap>(hierarchyRecords.GetData(), RECORD_CAPACITY),
                             hierarchyOverlap) == Status::Success);
        overlap.Traversal = QueryTraversal::AllTriangles;
        REQUIRE(view.Overlap(overlap,
                             BorrowedView<ColliderOverlap>(bruteRecords.GetData(), RECORD_CAPACITY),
                             bruteOverlap) == Status::Success);
        REQUIRE(hierarchyOverlap.OverlapCount == bruteOverlap.OverlapCount);
        if (hierarchyOverlap.IsComplete == true)
        {
            for (usize index = 0; index < hierarchyOverlap.WrittenCount; ++index)
            {
                REQUIRE(hierarchyRecords[index].Collider == bruteRecords[index].Collider);
            }
        }
    }
}
} // namespace

TEST_CASE("The collider hierarchy stays balanced under sorted and random insertion", "[collision][world][hierarchy]")
{
    constexpr uint32 COLLIDER_COUNT = 1024;
    Scene scene;
    InitializeOrFail(scene, { .MaxShapes = 1 }, { .MaxColliders = COLLIDER_COUNT });
    ShapeHandle sphere;
    REQUIRE(scene.Store.TryAddPrimitive(MakeSphereShape(0.4), sphere) == Status::Success);
    CHECK(scene.World.GetColliderHierarchyHeight() == 0);
    // Sorted along a line, the worst order for an unbalanced tree.
    Array<ColliderHandle> colliders;
    for (uint32 index = 0; index < COLLIDER_COUNT; ++index)
    {
        colliders.Add(
            CreateOrFail(scene.World,
                         { .Shape = sphere, .Pose = { .PositionWorld = {static_cast<float64>(index), 0.0, 0.0} } }));
        if (index == 0)
        {
            CHECK(scene.World.GetColliderHierarchyHeight() == 1);
        }
        REQUIRE(scene.World.GetColliderHierarchyHeight() <= GetBalancedHeightBound(index + 1));
    }
    // Remove every other collider, then move the rest to random places.
    RandomStream random;
    REQUIRE(random.TryReseed(101, 0) == true);
    for (uint32 index = 0; index < COLLIDER_COUNT; index += 2)
    {
        REQUIRE(scene.World.TryDestroyCollider(colliders[index]) == Status::Success);
    }
    for (uint32 index = 1; index < COLLIDER_COUNT; index += 2)
    {
        REQUIRE(scene.World.TrySetColliderPose(colliders[index],
                                               {
                                                   .PositionWorld = Widen(NextPoint(random, 50.0f)),
                                               }) == Status::Success);
    }
    CHECK(scene.World.GetColliderHierarchyHeight() <= GetBalancedHeightBound(COLLIDER_COUNT / 2));
    // Emptying the world empties the hierarchy, and it refills to capacity.
    for (uint32 index = 1; index < COLLIDER_COUNT; index += 2)
    {
        REQUIRE(scene.World.TryDestroyCollider(colliders[index]) == Status::Success);
    }
    CHECK(scene.World.GetColliderHierarchyHeight() == 0);
    for (uint32 index = 0; index < COLLIDER_COUNT; ++index)
    {
        (void)CreateOrFail(scene.World,
                           {
                               .Shape = sphere,
                               .Pose =
                               {
                                   .PositionWorld = Widen(NextPoint(random, 50.0f)),
                               },
                           });
    }
    CHECK(scene.World.GetColliderHierarchyHeight() <= GetBalancedHeightBound(COLLIDER_COUNT));
    ColliderHandle overflow;
    CHECK(scene.World.TryCreateCollider({ .Shape = sphere }, overflow) == Status::CapacityExceeded);
}

TEST_CASE("Queries through the hierarchy match brute force through create, move and destroy churn",
          "[collision][world][hierarchy][oracle]")
{
    constexpr uint32 CAPACITY = 160;
    Scene scene;
    InitializeOrFail(scene, { .MaxShapes = 4 }, { .MaxColliders = CAPACITY });
    RandomStream heights;
    REQUIRE(heights.TryReseed(111, 0) == true);
    ShapeHandle terrain;
    REQUIRE(scene.Store.TryAddTriangleMesh(CookOrFail(MakeGrid(4, &heights)), terrain) == Status::Success);
    ShapeHandle sphere;
    REQUIRE(scene.Store.TryAddPrimitive(MakeSphereShape(0.8), sphere) == Status::Success);
    ShapeHandle box;
    REQUIRE(scene.Store.TryAddPrimitive(MakeBoxShape({0.5, 1.5, 0.7}), box) == Status::Success);
    const StaticArray<ShapeHandle, 3> shapes{terrain, sphere, box};
    RandomStream random;
    REQUIRE(random.TryReseed(112, 0) == true);
    Array<ColliderHandle> live;
    constexpr uint32 ROUND_COUNT = 12;
    constexpr uint32 CHANGES_PER_ROUND = 40;
    constexpr uint32 QUERIES_PER_ROUND = 40;
    for (uint32 round = 0; round < ROUND_COUNT; ++round)
    {
        INFO("round " << round);
        for (uint32 change = 0; change < CHANGES_PER_ROUND; ++change)
        {
            const uint32 action = random.NextUInt32() % 3;
            if ((action == 0 || live.IsEmpty() == true) && live.GetSize() < CAPACITY)
            {
                live.Add(CreateOrFail(scene.World,
                                      {
                                          .Shape = shapes[random.NextUInt32() % shapes.GetSize()],
                                          .Pose = { .PositionWorld = Widen(NextPoint(random, 25.0f)) },
                                          .Binding = change,
                                      }));
            }
            else if (action == 1 && live.IsEmpty() == false)
            {
                const usize index = random.NextUInt32() % live.GetSize();
                REQUIRE(
                    scene.World.TrySetColliderPose(live[index], { .PositionWorld = Widen(NextPoint(random, 25.0f)) }) ==
                    Status::Success);
            }
            else if (live.IsEmpty() == false)
            {
                const usize index = random.NextUInt32() % live.GetSize();
                REQUIRE(scene.World.TryDestroyCollider(live[index]) == Status::Success);
                live[index] = live[live.GetSize() - 1];
                live.RemoveLast();
            }
        }
        REQUIRE(scene.World.GetColliderCount() == live.GetSize());
        REQUIRE(scene.World.GetColliderHierarchyHeight() <= GetBalancedHeightBound(live.GetSize()));
        CollisionReadView view;
        REQUIRE(scene.World.TryAcquireReadView(view) == Status::Success);
        RequireHierarchyMatchesBruteForce(view, random, QUERIES_PER_ROUND);
    }
}

TEST_CASE("Benchmark: hierarchy against brute-force collider traversal", "[.benchmark][collision][world]")
{
    // Run with the "[.benchmark]" tag; prints the time of 2000 closest rays over 4000 sphere colliders.
    constexpr uint32 COLLIDER_COUNT = 4000;
    constexpr uint32 RAY_COUNT = 2000;
    Scene scene;
    InitializeOrFail(scene, { .MaxShapes = 1 }, { .MaxColliders = COLLIDER_COUNT });
    ShapeHandle sphere;
    REQUIRE(scene.Store.TryAddPrimitive(MakeSphereShape(0.3), sphere) == Status::Success);
    RandomStream random;
    REQUIRE(random.TryReseed(121, 0) == true);
    for (uint32 index = 0; index < COLLIDER_COUNT; ++index)
    {
        (void)CreateOrFail(scene.World,
                           {
                               .Shape = sphere,
                               .Pose =
                               {
                                   .PositionWorld = Widen(NextPoint(random, 100.0f)),
                               },
                           });
    }
    CollisionReadView view;
    REQUIRE(scene.World.TryAcquireReadView(view) == Status::Success);
    for (const QueryTraversal traversal : {QueryTraversal::Hierarchy, QueryTraversal::AllTriangles})
    {
        REQUIRE(random.TryReseed(122, 0) == true);
        const ludus::foundation::time::Timestamp start = ludus::foundation::time::Now();
        uint32 hits = 0;
        for (uint32 ray = 0; ray < RAY_COUNT; ++ray)
        {
            WorldRaycastResult result;
            REQUIRE(view.RaycastClosest(
                        {
                            .OriginWorld = Widen(NextPoint(random, 100.0f)),
                            .DirectionWorld = NextDirection(random),
                            .MaxDistanceMeters = 50.0,
                            .Traversal = traversal,
                        },
                        result) == Status::Success);
            hits += result.HasHit == true ? 1 : 0;
        }
        ludus::foundation::time::Duration elapsed;
        REQUIRE(ludus::foundation::time::TryElapsed(start, ludus::foundation::time::Now(), elapsed) ==
                ludus::foundation::time::TimeStatus::Ok);
        WARN((traversal == QueryTraversal::Hierarchy ? "hierarchy " : "brute force ")
             << ludus::foundation::time::ToSeconds(elapsed) * 1000.0 << " ms, hits " << hits << ", height "
             << scene.World.GetColliderHierarchyHeight());
    }
}

TEST_CASE("Closest queries stay exact among nearly tied colliders", "[collision][world][hierarchy][oracle]")
{
    // Floors 5 mm apart, created in shuffled order: pruning by the best distance must never skip a nearer one.
    constexpr uint32 FLOOR_COUNT = 60;
    constexpr float64 SPACING_METERS = 0.005;
    Scene scene;
    InitializeOrFail(scene, { .MaxShapes = 1 }, { .MaxColliders = FLOOR_COUNT });
    MeshData floorData;
    AddFloorQuad(floorData);
    ShapeHandle floor;
    REQUIRE(scene.Store.TryAddTriangleMesh(CookOrFail(floorData), floor) == Status::Success);
    Array<uint32> order;
    for (uint32 index = 0; index < FLOOR_COUNT; ++index)
    {
        order.Add(index);
    }
    RandomStream random;
    REQUIRE(random.TryReseed(131, 0) == true);
    for (uint32 index = FLOOR_COUNT - 1; index > 0; --index)
    {
        const uint32 other = random.NextUInt32() % (index + 1);
        const uint32 saved = order[index];
        order[index] = order[other];
        order[other] = saved;
    }
    for (const uint32 level : order)
    {
        (void)CreateOrFail(scene.World,
                           {
                               .Shape = floor,
                               .Pose = { .PositionWorld = {0.0, static_cast<float64>(level) * SPACING_METERS, 0.0} },
                               .Binding = level,
                           });
    }
    CollisionReadView view;
    REQUIRE(scene.World.TryAcquireReadView(view) == Status::Success);
    constexpr uint32 QUERY_COUNT = 200;
    for (uint32 query = 0; query < QUERY_COUNT; ++query)
    {
        const Vector3d origin{static_cast<float64>(NextInRange(random, 0.05f, 0.95f)),
                              1.0,
                              static_cast<float64>(NextInRange(random, 0.05f, 0.95f))};
        WorldRaycastInput ray{ .OriginWorld = origin, .DirectionWorld = {0.0, -1.0, 0.0}, .MaxDistanceMeters = 2.0 };
        WorldRaycastResult hit;
        REQUIRE(view.RaycastClosest(ray, hit) == Status::Success);
        REQUIRE(hit.Binding == FLOOR_COUNT - 1);
        ray.Traversal = QueryTraversal::AllTriangles;
        WorldRaycastResult brute;
        REQUIRE(view.RaycastClosest(ray, brute) == Status::Success);
        REQUIRE(AreEqual(hit, brute) == true);
        const WorldConvexCastInput cast
        {
            .Shape = MakeSphereShape(0.02),
            .PositionWorld = origin,
            .TranslationWorld = {0.0, -2.0, 0.0},
        };
        WorldConvexCastResult contact;
        REQUIRE(view.CastConvexClosest(cast, contact) == Status::Success);
        REQUIRE(contact.Binding == FLOOR_COUNT - 1);
    }
}
