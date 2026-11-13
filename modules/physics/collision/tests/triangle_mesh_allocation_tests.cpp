// Allocation injection for every TriangleMesh creation path, and proof that
// queries never allocate. This executable replaces the global allocation
// functions, which FoundationMemory's container domain reaches through the
// aligned nothrow operator new; sanitizer builds use the ordinary suite
// because sanitizer runtimes replace the same functions.

#include "test_meshes.h"

#include <ludus/foundation/base/core.h>

#include <ludus/physics/collision/collision_world.h>
#include <ludus/physics/collision/convex_shape.h>
#include <ludus/physics/collision/debug_overlay.h>
#include <ludus/physics/collision/geometry_store.h>
#include <ludus/physics/collision/queries.h>
#include <ludus/physics/collision/status.h>
#include <ludus/physics/collision/triangle_mesh.h>

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <cstdlib>
#include <new>

namespace
{
struct AllocationCounters final
{
    long Allocations = 0;
    long Frees = 0;
};
AllocationCounters gCounters;
bool isTrackingAllocations = false;
long gFailAfter = -1;

void* AllocateAligned(std::size_t bytes, std::size_t alignment) noexcept
{
    if (isTrackingAllocations == true)
    {
        ++gCounters.Allocations;
    }
    if (gFailAfter == 0)
    {
        return nullptr;
    }
    if (gFailAfter > 0)
    {
        --gFailAfter;
    }
    if (alignment < alignof(std::max_align_t))
    {
        alignment = alignof(std::max_align_t);
    }
    std::size_t rounded = (bytes + alignment - 1) & ~(alignment - 1);
    if (rounded == 0)
    {
        rounded = alignment;
    }
    return std::aligned_alloc(alignment, rounded);
}

void FreeAligned(void* pointerOrNull) noexcept
{
    if (pointerOrNull != nullptr && isTrackingAllocations == true)
    {
        ++gCounters.Frees;
    }
    std::free(pointerOrNull);
}
} // namespace

// libc++ names the replaceable allocation functions' parameters with reserved
// identifiers (`__sz`), so these replacements cannot match its declarations.
// NOLINTBEGIN(readability-inconsistent-declaration-parameter-name)
void* operator new(std::size_t bytes)
{
    void* pointerOrNull = AllocateAligned(bytes, alignof(std::max_align_t));
    if (pointerOrNull == nullptr)
    {
        throw std::bad_alloc{};
    }
    return pointerOrNull;
}
void* operator new(std::size_t bytes, std::align_val_t alignment)
{
    void* pointerOrNull = AllocateAligned(bytes, static_cast<std::size_t>(alignment));
    if (pointerOrNull == nullptr)
    {
        throw std::bad_alloc{};
    }
    return pointerOrNull;
}
void* operator new(std::size_t bytes, const std::nothrow_t&) noexcept
{
    return AllocateAligned(bytes, alignof(std::max_align_t));
}
void* operator new(std::size_t bytes, std::align_val_t alignment, const std::nothrow_t&) noexcept
{
    return AllocateAligned(bytes, static_cast<std::size_t>(alignment));
}
void operator delete(void* pointerOrNull) noexcept
{
    FreeAligned(pointerOrNull);
}
void operator delete(void* pointerOrNull, std::size_t) noexcept
{
    FreeAligned(pointerOrNull);
}
void operator delete(void* pointerOrNull, std::align_val_t) noexcept
{
    FreeAligned(pointerOrNull);
}
void operator delete(void* pointerOrNull, std::size_t, std::align_val_t) noexcept
{
    FreeAligned(pointerOrNull);
}
void operator delete(void* pointerOrNull, const std::nothrow_t&) noexcept
{
    FreeAligned(pointerOrNull);
}
void operator delete(void* pointerOrNull, std::align_val_t, const std::nothrow_t&) noexcept
{
    FreeAligned(pointerOrNull);
}
// NOLINTEND(readability-inconsistent-declaration-parameter-name)

using namespace ludus::physics::collision;
using namespace ludus::physics::collision::test;

namespace
{
constexpr usize HULL_CORNER_COUNT = 8;

Array<Vector3> MakeHullPoints(float32 halfExtent)
{
    Array<Vector3> points;
    for (uint32 corner = 0; corner < HULL_CORNER_COUNT; ++corner)
    {
        points.Add({(corner & 1U) == 0 ? -halfExtent : halfExtent,
                    (corner & 2U) == 0 ? -halfExtent : halfExtent,
                    (corner & 4U) == 0 ? -halfExtent : halfExtent});
    }
    return points;
}

ConvexHullSource GetHullSource(const Array<Vector3>& points)
{
    return { .PointsShape = BorrowedView<const Vector3>(points.GetData(), points.GetSize()) };
}

/// Cook a hull while failing each allocation in turn; every failure must report AllocationFailure and leave the
/// hull unchanged. Returns the number of allocations cooking needs.
long RequireEveryAllocationFailurePreservesHull(const ConvexHullSource& source, ConvexHull& inoutHull)
{
    const bool wasCooked = inoutHull.IsCooked();
    const usize pointCount = inoutHull.GetPointsShape().GetSize();
    const Vector3 maximum = inoutHull.GetBoundsShape().Max;
    for (long failedAllocation = 0;; ++failedAllocation)
    {
        gFailAfter = failedAllocation;
        const ConvexHullCookResult result = ConvexHull::TryCook(source, inoutHull);
        gFailAfter = -1;
        if (result.Result == Status::Success)
        {
            return failedAllocation;
        }
        REQUIRE(result.Result == Status::AllocationFailure);
        REQUIRE(result.PointIndex == INVALID_POINT_INDEX);
        REQUIRE(inoutHull.IsCooked() == wasCooked);
        REQUIRE(inoutHull.GetPointsShape().GetSize() == pointCount);
        REQUIRE(inoutHull.GetBoundsShape().Max == maximum);
    }
}

RaycastInput MakeProbeRay()
{
    return { .OriginMesh = {0.25, 1.0, 0.75}, .DirectionMesh = {0.0, -1.0, 0.0}, .MaxDistanceMeters = 10.0 };
}

/// Cook `source` while failing each allocation in turn. Every failure must
/// report AllocationFailure and leave `inoutMesh` exactly as it was; the first
/// run with no failure must succeed. Returns the number of allocations cooking needs.
long RequireEveryAllocationFailurePreservesMesh(const TriangleMeshSource& source,
                                                const TriangleMeshCookSettings& settings,
                                                TriangleMesh& inoutMesh)
{
    const bool wasCooked = inoutMesh.IsCooked();
    const usize triangleCount = inoutMesh.GetTriangleCount();
    const usize nodeCount = inoutMesh.GetNodeCount();
    RaycastResult before;
    const Status beforeStatus = inoutMesh.RaycastClosest(MakeProbeRay(), before);
    for (long failedAllocation = 0;; ++failedAllocation)
    {
        gFailAfter = failedAllocation;
        const TriangleMeshCookResult result = TriangleMesh::TryCook(source, settings, inoutMesh);
        gFailAfter = -1;
        if (result.Result == Status::Success)
        {
            // The first success is the first run whose injected failure lies past every allocation cooking makes.
            return failedAllocation;
        }
        REQUIRE(result.Result == Status::AllocationFailure);
        REQUIRE(result.TriangleIndex == INVALID_TRIANGLE_INDEX);
        REQUIRE(inoutMesh.IsCooked() == wasCooked);
        REQUIRE(inoutMesh.GetTriangleCount() == triangleCount);
        REQUIRE(inoutMesh.GetNodeCount() == nodeCount);
        RaycastResult after;
        REQUIRE(inoutMesh.RaycastClosest(MakeProbeRay(), after) == beforeStatus);
        REQUIRE(after.HasHit == before.HasHit);
        REQUIRE(after.DistanceMeters == before.DistanceMeters);
        REQUIRE(after.TriangleIndex == before.TriangleIndex);
    }
}
} // namespace

TEST_CASE("Every cook allocation failure preserves an empty or cooked mesh", "[collision][alloc]")
{
    MeshData floorData;
    AddFloorQuad(floorData);
    RandomStream heights;
    REQUIRE(heights.TryReseed(11, 0) == true);
    const MeshData terrainData = MakeGrid(16, &heights);
    MeshData degenerateData;
    degenerateData.AddTriangle({0.0f, 0.0f, 0.0f}, {1.0f, 0.0f, 0.0f}, {2.0f, 0.0f, 0.0f});
    AddFloorQuad(degenerateData);
    const TriangleMeshCookSettings reject{};
    const TriangleMeshCookSettings skip{ .DegeneratePolicy = DegenerateTrianglePolicy::Skip };

    SECTION("into an empty mesh")
    {
        TriangleMesh mesh;
        CHECK(RequireEveryAllocationFailurePreservesMesh(floorData.GetSource(), reject, mesh) > 0);
        CHECK(mesh.GetTriangleCount() == 2);
    }
    SECTION("replacing a cooked mesh")
    {
        TriangleMesh mesh = CookOrFail(floorData);
        CHECK(RequireEveryAllocationFailurePreservesMesh(terrainData.GetSource(), reject, mesh) > 0);
        CHECK(mesh.GetTriangleCount() == static_cast<usize>(16) * 16 * 2);
    }
    SECTION("with skipped degenerate triangles")
    {
        TriangleMesh mesh = CookOrFail(terrainData);
        CHECK(RequireEveryAllocationFailurePreservesMesh(degenerateData.GetSource(), skip, mesh) > 0);
        CHECK(mesh.GetTriangleCount() == 2);
        CHECK(mesh.GetSkippedTriangleCount() == 1);
    }
}

TEST_CASE("Every hull cook allocation failure preserves an empty or cooked hull", "[collision][alloc][hull]")
{
    const Array<Vector3> small = MakeHullPoints(0.5f);
    const Array<Vector3> large = MakeHullPoints(2.0f);
    SECTION("into an empty hull")
    {
        ConvexHull hull;
        CHECK(RequireEveryAllocationFailurePreservesHull(GetHullSource(small), hull) > 0);
        CHECK(hull.GetBoundsShape().Max == Vector3{0.5f, 0.5f, 0.5f});
    }
    SECTION("replacing a cooked hull")
    {
        ConvexHull hull;
        REQUIRE(ConvexHull::TryCook(GetHullSource(small), hull).Result == Status::Success);
        CHECK(RequireEveryAllocationFailurePreservesHull(GetHullSource(large), hull) > 0);
        CHECK(hull.GetBoundsShape().Max == Vector3{2.0f, 2.0f, 2.0f});
    }
}

TEST_CASE("Rejected hull sources allocate nothing", "[collision][alloc][hull]")
{
    Array<Vector3> flat = MakeHullPoints(1.0f);
    for (Vector3& point : flat)
    {
        point.Z = 0.0f;
    }
    ConvexHull hull;
    gCounters = {};
    isTrackingAllocations = true;
    const ConvexHullCookResult result = ConvexHull::TryCook(GetHullSource(flat), hull);
    isTrackingAllocations = false;
    CHECK(result.Result == Status::DegenerateShape);
    CHECK(gCounters.Allocations == 0);
}

TEST_CASE("Rejected sources allocate nothing", "[collision][alloc]")
{
    MeshData data;
    AddFloorQuad(data);
    data.Indices[0] = 99;
    TriangleMesh mesh;
    gCounters = {};
    isTrackingAllocations = true;
    const TriangleMeshCookResult result = TriangleMesh::TryCook(data.GetSource(), {}, mesh);
    isTrackingAllocations = false;
    CHECK(result.Result == Status::IndexOutOfRange);
    CHECK(gCounters.Allocations == 0);
}

TEST_CASE("Queries allocate nothing", "[collision][alloc]")
{
    RandomStream heights;
    REQUIRE(heights.TryReseed(12, 0) == true);
    const MeshData data = MakeGrid(32, &heights);
    const TriangleMesh mesh = CookOrFail(data);
    RandomStream random;
    REQUIRE(random.TryReseed(13, 0) == true);
    ConvexHull hull;
    REQUIRE(ConvexHull::TryCook(GetHullSource(MakeHullPoints(0.3f)), hull).Result == Status::Success);
    gCounters = {};
    isTrackingAllocations = true;
    bool isEverySuccess = true;
    constexpr uint32 QUERY_COUNT = 500;
    for (uint32 query = 0; query < QUERY_COUNT; ++query)
    {
        const RaycastInput ray
        {
            .OriginMesh = {static_cast<float64>(NextInRange(random, 0.0f, 32.0f)),
                           2.0,
                           static_cast<float64>(NextInRange(random, 0.0f, 32.0f))},
            .DirectionMesh = NextDirection(random),
            .MaxDistanceMeters = 50.0,
        };
        RaycastResult rayResult;
        isEverySuccess = isEverySuccess == true && mesh.RaycastClosest(ray, rayResult) == Status::Success;
        const BoxCastInput box
        {
            .CenterMesh = ray.OriginMesh,
            .HalfExtentsMesh = {0.3, 0.9, 0.3},
            .TranslationMesh = ray.DirectionMesh * 4.0,
            .Traversal = query % 2 == 0 ? QueryTraversal::Hierarchy : QueryTraversal::AllTriangles,
        };
        BoxCastResult boxResult;
        isEverySuccess = isEverySuccess == true && mesh.CastBoxClosest(box, boxResult) == Status::Success;
        constexpr uint32 KIND_COUNT = 4;
        const StaticArray<ConvexShape, KIND_COUNT> shapes{MakeSphereShape(0.4),
                                                          MakeCapsuleShape(0.5, 0.3),
                                                          MakeBoxShape(box.HalfExtentsMesh),
                                                          MakeHullShape(hull)};
        const ConvexCastInput convex
        {
            .Shape = shapes[query % KIND_COUNT],
            .PositionMesh = ray.OriginMesh,
            .TranslationMesh = box.TranslationMesh,
            .Traversal = box.Traversal,
        };
        ConvexCastResult convexResult;
        isEverySuccess = isEverySuccess == true && mesh.CastConvexClosest(convex, convexResult) == Status::Success;
    }
    isTrackingAllocations = false;
    CHECK(isEverySuccess == true);
    CHECK(gCounters.Allocations == 0);
    CHECK(gCounters.Frees == 0);
}

TEST_CASE("Store and world initialization fail allocations without installing state", "[collision][alloc][world]")
{
    constexpr uint32 CAPACITY = 16;
    GeometryStore store;
    for (long failedAllocation = 0;; ++failedAllocation)
    {
        gFailAfter = failedAllocation;
        const Status status = store.TryInitialize({ .MaxShapes = CAPACITY });
        gFailAfter = -1;
        if (status == Status::Success)
        {
            CHECK(failedAllocation > 0);
            break;
        }
        REQUIRE(status == Status::AllocationFailure);
        REQUIRE(store.IsInitialized() == false);
    }
    CollisionWorld world;
    for (long failedAllocation = 0;; ++failedAllocation)
    {
        gFailAfter = failedAllocation;
        const Status status = world.TryInitialize(store, { .MaxColliders = CAPACITY });
        gFailAfter = -1;
        if (status == Status::Success)
        {
            CHECK(failedAllocation > 0);
            break;
        }
        REQUIRE(status == Status::AllocationFailure);
        REQUIRE(world.IsInitialized() == false);
    }
}

TEST_CASE("Collider changes and world queries allocate nothing", "[collision][alloc][world]")
{
    RandomStream heights;
    REQUIRE(heights.TryReseed(21, 0) == true);
    GeometryStore store;
    REQUIRE(store.TryInitialize({ .MaxShapes = 4 }) == Status::Success);
    TriangleMesh terrain = CookOrFail(MakeGrid(16, &heights));
    CollisionWorld world;
    REQUIRE(world.TryInitialize(store, { .MaxColliders = 8 }) == Status::Success);
    ConvexHull hull;
    REQUIRE(ConvexHull::TryCook(GetHullSource(MakeHullPoints(0.3f)), hull).Result == Status::Success);
    gCounters = {};
    isTrackingAllocations = true;
    ShapeHandle shape;
    bool isEverySuccess = store.TryAddTriangleMesh(ludus::foundation::Move(terrain), shape) == Status::Success;
    ColliderHandle collider;
    isEverySuccess = isEverySuccess == true && world.TryCreateCollider({ .Shape = shape }, collider) == Status::Success;
    isEverySuccess = isEverySuccess == true &&
                     world.TrySetColliderPose(collider, { .PositionWorld = {1.0, 0.0, 2.0} }) == Status::Success;
    {
        CollisionReadView view;
        isEverySuccess = isEverySuccess == true && world.TryAcquireReadView(view) == Status::Success;
        RandomStream random;
        isEverySuccess = isEverySuccess == true && random.TryReseed(22, 0) == true;
        constexpr uint32 QUERY_COUNT = 200;
        for (uint32 query = 0; query < QUERY_COUNT; ++query)
        {
            const WorldRaycastInput ray
            {
                .OriginWorld = {static_cast<float64>(NextInRange(random, 1.0f, 17.0f)),
                                2.0,
                                static_cast<float64>(NextInRange(random, 2.0f, 18.0f))},
                .DirectionWorld = NextDirection(random),
                .MaxDistanceMeters = 30.0,
            };
            WorldRaycastResult rayResult;
            isEverySuccess = isEverySuccess == true && view.RaycastClosest(ray, rayResult) == Status::Success;
            const WorldConvexCastInput cast
            {
                .Shape = query % 2 == 0 ? MakeHullShape(hull) : MakeCapsuleShape(0.4, 0.2),
                .PositionWorld = ray.OriginWorld,
                .TranslationWorld = ray.DirectionWorld * 5.0,
            };
            WorldConvexCastResult castResult;
            isEverySuccess = isEverySuccess == true && view.CastConvexClosest(cast, castResult) == Status::Success;
        }
    }
    isEverySuccess = isEverySuccess == true && world.TryDestroyCollider(collider) == Status::Success;
    isEverySuccess = isEverySuccess == true && store.TryRelease(shape) == Status::Success;
    isTrackingAllocations = false;
    CHECK(isEverySuccess == true);
    CHECK(gCounters.Allocations == 0);
    // Releasing the last lease frees the mesh's storage: frees are expected, allocations are not.
}

TEST_CASE("Convex colliders, overlap queries and Any/All modes allocate nothing", "[collision][alloc][world]")
{
    GeometryStore store;
    REQUIRE(store.TryInitialize({ .MaxShapes = 4 }) == Status::Success);
    CollisionWorld world;
    REQUIRE(world.TryInitialize(store, { .MaxColliders = 8 }) == Status::Success);
    ConvexHull hull;
    REQUIRE(ConvexHull::TryCook(GetHullSource(MakeHullPoints(0.4f)), hull).Result == Status::Success);
    MeshData floorData;
    AddFloorQuad(floorData);
    TriangleMesh floor = CookOrFail(floorData);
    constexpr usize RECORD_COUNT = 4;
    StaticArray<ColliderOverlap, RECORD_COUNT> records{};
    gCounters = {};
    isTrackingAllocations = true;
    ShapeHandle sphere;
    ShapeHandle hullShape;
    ShapeHandle floorShape;
    bool isEverySuccess = store.TryAddPrimitive(MakeSphereShape(0.5), sphere) == Status::Success &&
                          store.TryAddConvexHull(ludus::foundation::Move(hull), hullShape) == Status::Success &&
                          store.TryAddTriangleMesh(ludus::foundation::Move(floor), floorShape) == Status::Success;
    ColliderHandle first;
    ColliderHandle second;
    ColliderHandle third;
    isEverySuccess = isEverySuccess == true && world.TryCreateCollider({ .Shape = sphere }, first) == Status::Success &&
                     world.TryCreateCollider({ .Shape = hullShape, .Pose = { .PositionWorld = {2.0, 0.0, 0.0} } },
                                             second) == Status::Success &&
                     world.TryCreateCollider({ .Shape = floorShape }, third) == Status::Success;
    {
        CollisionReadView view;
        isEverySuccess = isEverySuccess == true && world.TryAcquireReadView(view) == Status::Success;
        constexpr uint32 QUERY_COUNT = 100;
        for (uint32 query = 0; query < QUERY_COUNT; ++query)
        {
            const float64 offset = static_cast<float64>(query) * 0.03;
            WorldRaycastResult ray;
            isEverySuccess = isEverySuccess == true && view.RaycastClosest(
                                                           {
                                                               .OriginWorld = {offset - 1.0, 0.2, 0.1},
                                                               .DirectionWorld = {1.0, 0.0, 0.0},
                                                               .MaxDistanceMeters = 5.0,
                                                           },
                                                           ray) == Status::Success;
            WorldConvexCastResult cast;
            isEverySuccess = isEverySuccess == true && view.CastConvexClosest(
                                                           {
                                                               .Shape = MakeCapsuleShape(0.3, 0.2),
                                                               .PositionWorld = {offset, 2.0, 0.2},
                                                               .TranslationWorld = {0.0, -3.0, 0.0},
                                                           },
                                                           cast) == Status::Success;
            StaticArray<WorldRaycastResult, RECORD_COUNT> rayHits{};
            StaticArray<WorldConvexCastResult, RECORD_COUNT> castHits{};
            WorldHitsResult hits;
            WorldRaycastResult anyRay;
            WorldConvexCastResult anyCast;
            const WorldRaycastInput across
            {
                .OriginWorld = {offset - 1.0, 0.2, 0.1},
                .DirectionWorld = {1.0, 0.0, 0.0},
                .MaxDistanceMeters = 5.0,
            };
            const WorldConvexCastInput falling
            {
                .Shape = MakeCapsuleShape(0.3, 0.2),
                .PositionWorld = {offset, 2.0, 0.2},
                .TranslationWorld = {0.0, -3.0, 0.0},
            };
            isEverySuccess =
                isEverySuccess == true && view.RaycastAny(across, anyRay) == Status::Success &&
                view.RaycastAll(across, BorrowedView<WorldRaycastResult>(rayHits.GetData(), RECORD_COUNT), hits) ==
                    Status::Success &&
                view.CastConvexAny(falling, anyCast) == Status::Success &&
                view.CastConvexAll(falling,
                                   BorrowedView<WorldConvexCastResult>(castHits.GetData(), RECORD_COUNT),
                                   hits) == Status::Success;
            constexpr usize DEBUG_LINE_COUNT = 256;
            StaticArray<DebugLine, DEBUG_LINE_COUNT> debugStorage{};
            DebugLineBuffer debugLines(BorrowedView<DebugLine>(debugStorage.GetData(), DEBUG_LINE_COUNT));
            isEverySuccess = isEverySuccess == true &&
                             AppendColliderLines(view, {}, true, debugLines) == Status::Success &&
                             AppendRaycastLines(across, anyRay, {}, debugLines) == Status::Success &&
                             AppendConvexCastLines(view, falling, anyCast, {}, debugLines) == Status::Success;
            WorldOverlapResult overlap;
            isEverySuccess =
                isEverySuccess == true && view.Overlap(
                                              {
                                                  .Shape = MakeBoxShape({0.4, 0.4, 0.4}),
                                                  .PositionWorld = {offset, 0.1, 0.3},
                                              },
                                              BorrowedView<ColliderOverlap>(records.GetData(), RECORD_COUNT),
                                              overlap) == Status::Success;
        }
    }
    isTrackingAllocations = false;
    CHECK(isEverySuccess == true);
    CHECK(gCounters.Allocations == 0);
}
