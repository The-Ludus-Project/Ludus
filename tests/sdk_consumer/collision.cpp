#include <ludus/foundation/containers/borrowed_view.hpp>
#include <ludus/foundation/containers/static_array.hpp>
#include <ludus/foundation/math/vector.hpp>
#include <ludus/physics/collision/collision_world.h>
#include <ludus/physics/collision/convex_shape.h>
#include <ludus/physics/collision/debug_overlay.h>
#include <ludus/physics/collision/geometry_store.h>
#include <ludus/physics/collision/triangle_mesh.h>

bool ExerciseInstalledCollision() noexcept
{
    using namespace ludus::physics::collision;
    using ludus::foundation::BorrowedView;
    using ludus::foundation::StaticArray;
    using ludus::foundation::math::Vector3;
    constexpr ludus::foundation::usize CORNER_COUNT = 4;
    constexpr ludus::foundation::usize INDEX_COUNT = 6;
    const StaticArray<Vector3, CORNER_COUNT> positions{Vector3{0.0f, 0.0f, 0.0f},
                                                       Vector3{0.0f, 0.0f, 1.0f},
                                                       Vector3{1.0f, 0.0f, 1.0f},
                                                       Vector3{1.0f, 0.0f, 0.0f}};
    const StaticArray<uint32, INDEX_COUNT> indices{0, 1, 2, 0, 2, 3};
    const TriangleMeshSource source
    {
        .PositionsMesh = BorrowedView<const Vector3>(positions.GetData(), CORNER_COUNT),
        .Indices = BorrowedView<const uint32>(indices.GetData(), INDEX_COUNT),
    };
    TriangleMesh mesh;
    if (TriangleMesh::TryCook(source, {}, mesh).Result != Status::Success)
    {
        return false;
    }
    const RaycastInput ray
    {
        .OriginMesh = {0.25, 2.0, 0.75},
        .DirectionMesh = {0.0, -1.0, 0.0},
        .MaxDistanceMeters = 5.0,
    };
    RaycastResult hit;
    const BoxCastInput box
    {
        .CenterMesh = {0.5, 2.0, 0.5},
        .HalfExtentsMesh = {0.25, 0.5, 0.25},
        .TranslationMesh = {0.0, -3.0, 0.0},
    };
    BoxCastResult contact;
    if (mesh.RaycastClosest(ray, hit) != Status::Success || hit.HasHit == false || hit.DistanceMeters != 2.0 ||
        hit.TriangleIndex != 0 || mesh.CastBoxClosest(box, contact) != Status::Success || contact.HasHit == false ||
        contact.Fraction != 0.5)
    {
        return false;
    }
    constexpr ludus::foundation::usize HULL_POINT_COUNT = 4;
    const StaticArray<Vector3, HULL_POINT_COUNT> tetrahedron{Vector3{0.0f, 0.0f, 0.0f},
                                                             Vector3{0.5f, 0.0f, 0.0f},
                                                             Vector3{0.0f, 0.5f, 0.0f},
                                                             Vector3{0.0f, 0.0f, 0.5f}};
    ConvexHull hull;
    if (ConvexHull::TryCook(
            {
                .PointsShape = BorrowedView<const Vector3>(tetrahedron.GetData(), HULL_POINT_COUNT),
            },
            hull)
            .Result != Status::Success)
    {
        return false;
    }
    const ConvexCastInput sphere
    {
        .Shape = MakeSphereShape(0.5),
        .PositionMesh = {0.5, 2.0, 0.5},
        .TranslationMesh = {0.0, -3.0, 0.0},
    };
    const ConvexCastInput tetrahedronCast
    {
        .Shape = MakeHullShape(hull),
        .PositionMesh = {0.25, 1.0, 0.25},
        .TranslationMesh = {0.0, -2.0, 0.0},
    };
    ConvexCastResult sphereContact;
    ConvexCastResult hullContact;
    // Both land when their lowest point reaches the floor, half way along the translation.
    constexpr float64 CONTACT_FRACTION = 0.5;
    constexpr float64 FRACTION_TOLERANCE = 1.0e-9;
    if (mesh.CastConvexClosest(sphere, sphereContact) != Status::Success || sphereContact.HasHit == false ||
        sphereContact.Fraction < CONTACT_FRACTION - FRACTION_TOLERANCE ||
        sphereContact.Fraction > CONTACT_FRACTION + FRACTION_TOLERANCE ||
        mesh.CastConvexClosest(tetrahedronCast, hullContact) != Status::Success || hullContact.HasHit == false ||
        hullContact.Fraction < CONTACT_FRACTION - FRACTION_TOLERANCE ||
        hullContact.Fraction > CONTACT_FRACTION + FRACTION_TOLERANCE)
    {
        return false;
    }
    // A world with one collider that leases the quad, raised by one meter, queried through a read view.
    GeometryStore store;
    CollisionWorld world;
    ShapeHandle shape;
    ColliderHandle collider;
    constexpr ludus::foundation::uint64 BINDING = 17;
    if (store.TryInitialize({ .MaxShapes = 1 }) != Status::Success ||
        store.TryAddTriangleMesh(ludus::foundation::Move(mesh), shape) != Status::Success ||
        world.TryInitialize(store, { .MaxColliders = 1 }) != Status::Success ||
        world.TryCreateCollider({ .Shape = shape, .Pose = { .PositionWorld = {0.0, 1.0, 0.0} }, .Binding = BINDING },
                                collider) != Status::Success)
    {
        return false;
    }
    CollisionReadView view;
    WorldRaycastResult worldHit;
    const bool isHit = world.TryAcquireReadView(view) == Status::Success &&
                       view.RaycastClosest(
                           {
                               .OriginWorld = {0.25, 3.0, 0.75},
                               .DirectionWorld = {0.0, -1.0, 0.0},
                               .MaxDistanceMeters = 5.0,
                           },
                           worldHit) == Status::Success &&
                       worldHit.HasHit == true && worldHit.DistanceMeters == 2.0 && worldHit.Collider == collider &&
                       worldHit.Binding == BINDING;
    // A box resting across the raised quad overlaps it.
    constexpr ludus::foundation::usize RECORD_COUNT = 2;
    StaticArray<ColliderOverlap, RECORD_COUNT> records{};
    WorldOverlapResult overlap;
    const bool isOverlapping =
        isHit == true &&
        view.Overlap({ .Shape = MakeBoxShape({0.25, 0.25, 0.25}), .PositionWorld = {0.5, 1.0, 0.5} },
                     BorrowedView<ColliderOverlap>(records.GetData(), RECORD_COUNT),
                     overlap) == Status::Success &&
        overlap.WrittenCount == 1 && overlap.IsComplete == true && records[0].Collider == collider;
    StaticArray<WorldRaycastResult, RECORD_COUNT> rayHits{};
    WorldHitsResult hits;
    const bool isListed = isOverlapping == true &&
                          view.RaycastAll(
                              {
                                  .OriginWorld = {0.25, 3.0, 0.75},
                                  .DirectionWorld = {0.0, -1.0, 0.0},
                                  .MaxDistanceMeters = 5.0,
                              },
                              BorrowedView<WorldRaycastResult>(rayHits.GetData(), RECORD_COUNT),
                              hits) == Status::Success &&
                          hits.HitCount == 1 && hits.IsComplete == true && rayHits[0].Collider == collider;
    // The collider's bounds draw as one box of 12 debug lines.
    constexpr ludus::foundation::usize BOX_LINE_COUNT = 12;
    StaticArray<DebugLine, BOX_LINE_COUNT> debugStorage{};
    DebugLineBuffer debugLines(BorrowedView<DebugLine>(debugStorage.GetData(), BOX_LINE_COUNT));
    const bool isDrawn = isListed == true && AppendColliderLines(view, {}, false, debugLines) == Status::Success &&
                         debugLines.IsComplete() == true && debugLines.GetWrittenCount() == BOX_LINE_COUNT &&
                         debugStorage[0].Collider == collider;
    view.Release();
    return isDrawn == true && world.TryDestroyCollider(collider) == Status::Success;
}
