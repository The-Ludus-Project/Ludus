// Convex colliders and overlap queries in a CollisionWorld: analytic answers,
// independent oracles for convex-against-convex contact, Any/All completion,
// and exact agreement with the brute-force traversal.

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
constexpr float64 ANSWER_TOLERANCE = 1.0e-7;
constexpr usize BOX_CORNER_COUNT = 8;
constexpr usize AXIS_COUNT = 3;
constexpr uint32 SEARCH_ITERATIONS = 200;
constexpr float64 GOLDEN_SECTION = 0.6180339887498949;
/// Oracle separations within this many meters of zero are treated as tangent and skipped.
constexpr float64 TANGENCY_METERS = 1.0e-6;
constexpr float64 CONTACT_DISTANCE_TOLERANCE_METERS = 1.0e-6;

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

ShapeHandle AddPrimitiveOrFail(GeometryStore& inoutStore, const ConvexShape& shape)
{
    ShapeHandle handle;
    REQUIRE(inoutStore.TryAddPrimitive(shape, handle) == Status::Success);
    return handle;
}

ShapeHandle AddFloorOrFail(GeometryStore& inoutStore)
{
    MeshData data;
    AddFloorQuad(data);
    ShapeHandle handle;
    REQUIRE(inoutStore.TryAddTriangleMesh(CookOrFail(data), handle) == Status::Success);
    return handle;
}

Array<Vector3> MakeBoxPoints(Vector3 halfExtents)
{
    Array<Vector3> points;
    for (uint32 corner = 0; corner < BOX_CORNER_COUNT; ++corner)
    {
        points.Add({(corner & 1U) == 0 ? -halfExtents.X : halfExtents.X,
                    (corner & 2U) == 0 ? -halfExtents.Y : halfExtents.Y,
                    (corner & 4U) == 0 ? -halfExtents.Z : halfExtents.Z});
    }
    return points;
}

ShapeHandle AddBoxHullOrFail(GeometryStore& inoutStore, Vector3 halfExtents)
{
    const Array<Vector3> points = MakeBoxPoints(halfExtents);
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

math::Quaternion MakeRotation(Vector3d axis, float64 radians)
{
    math::Quaternion rotation;
    REQUIRE(math::TryFromAxisAngle(
                {static_cast<float32>(axis.X), static_cast<float32>(axis.Y), static_cast<float32>(axis.Z)},
                static_cast<float32>(radians),
                rotation) == math::MathStatus::Success);
    return rotation;
}

/// Rotation columns by the float64 conversion the module documents.
StaticArray<Vector3d, AXIS_COUNT> GetAxes(math::Quaternion orientation)
{
    const float64 x = static_cast<float64>(orientation.X);
    const float64 y = static_cast<float64>(orientation.Y);
    const float64 z = static_cast<float64>(orientation.Z);
    const float64 w = static_cast<float64>(orientation.W);
    const float64 scale = 2.0 / (x * x + y * y + z * z + w * w);
    return {Vector3d{1.0 - scale * (y * y + z * z), scale * (x * y + w * z), scale * (x * z - w * y)},
            Vector3d{scale * (x * y - w * z), 1.0 - scale * (x * x + z * z), scale * (y * z + w * x)},
            Vector3d{scale * (x * z + w * y), scale * (y * z - w * x), 1.0 - scale * (x * x + y * y)}};
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

/// The posed shape as the oracles see it, in world space.
struct OracleShape final
{
    ConvexShapeKind Kind = ConvexShapeKind::Sphere;
    Vector3d Center{};
    StaticArray<Vector3d, AXIS_COUNT> Axes{};
    Vector3d HalfExtents{};
    float64 HalfHeight = 0.0;
    float64 Radius = 0.0;
};

struct Segment final
{
    Vector3d Start;
    Vector3d End;
};

/// Closest distance between two segments, after Ericson, "Real-Time Collision Detection", section 5.1.9.
float64 GetSegmentDistance(const Segment& first, const Segment& second)
{
    const Vector3d d1 = first.End - first.Start;
    const Vector3d d2 = second.End - second.Start;
    const Vector3d r = first.Start - second.Start;
    const float64 a = math::Dot(d1, d1);
    const float64 e = math::Dot(d2, d2);
    const float64 f = math::Dot(d2, r);
    float64 s = 0.0;
    float64 t = 0.0;
    if (a <= 0.0 && e <= 0.0)
    {
        return math::Length(r);
    }
    if (a <= 0.0)
    {
        t = math::Clamp(f / e, 0.0, 1.0);
    }
    else
    {
        const float64 c = math::Dot(d1, r);
        if (e <= 0.0)
        {
            s = math::Clamp(-c / a, 0.0, 1.0);
        }
        else
        {
            const float64 b = math::Dot(d1, d2);
            const float64 denominator = a * e - b * b;
            s = denominator > 0.0 ? math::Clamp((b * f - c * e) / denominator, 0.0, 1.0) : 0.0;
            t = (b * s + f) / e;
            if (t < 0.0)
            {
                t = 0.0;
                s = math::Clamp(-c / a, 0.0, 1.0);
            }
            else if (t > 1.0)
            {
                t = 1.0;
                s = math::Clamp((b - c) / a, 0.0, 1.0);
            }
        }
    }
    return math::Length((first.Start + d1 * s) - (second.Start + d2 * t));
}

/// The rounded core of a sphere or capsule: a segment (possibly a point) and a radius.
Segment GetCore(const OracleShape& shape, Vector3d center)
{
    const Vector3d offset = shape.Kind == ConvexShapeKind::Capsule ? shape.Axes[1] * shape.HalfHeight : Vector3d{};
    return { .Start = center - offset, .End = center + offset };
}

/// Largest separating-axis gap between two oriented boxes over their 15 axes; positive exactly when disjoint.
float64 GetBoxBoxGap(const OracleShape& first, Vector3d firstCenter, const OracleShape& second, Vector3d secondCenter)
{
    Array<Vector3d> axes;
    for (const Vector3d axis : first.Axes)
    {
        axes.Add(axis);
    }
    for (const Vector3d axis : second.Axes)
    {
        axes.Add(axis);
        for (const Vector3d other : first.Axes)
        {
            axes.Add(math::Cross(other, axis));
        }
    }
    const StaticArray<float64, AXIS_COUNT> firstHalves{first.HalfExtents.X, first.HalfExtents.Y, first.HalfExtents.Z};
    const StaticArray<float64, AXIS_COUNT> secondHalves{second.HalfExtents.X,
                                                        second.HalfExtents.Y,
                                                        second.HalfExtents.Z};
    float64 gap = -1.0e300;
    for (const Vector3d rawAxis : axes)
    {
        const float64 length = math::Length(rawAxis);
        if (length < 1.0e-9)
        {
            continue;
        }
        const Vector3d axis = rawAxis * (1.0 / length);
        float64 firstRadius = 0.0;
        float64 secondRadius = 0.0;
        for (usize index = 0; index < AXIS_COUNT; ++index)
        {
            firstRadius += firstHalves[index] * math::Abs(math::Dot(first.Axes[index], axis));
            secondRadius += secondHalves[index] * math::Abs(math::Dot(second.Axes[index], axis));
        }
        gap = math::Max(gap, math::Abs(math::Dot(secondCenter - firstCenter, axis)) - firstRadius - secondRadius);
    }
    return gap;
}

/// Separation between a moving shape at `center` and a stationary target: exact for spheres and capsules, the
/// separating-axis gap for two boxes.
float64 GetSeparation(const OracleShape& shape, Vector3d center, const OracleShape& target)
{
    if (shape.Kind == ConvexShapeKind::Box)
    {
        return GetBoxBoxGap(shape, center, target, target.Center);
    }
    return GetSegmentDistance(GetCore(shape, center), GetCore(target, target.Center)) - shape.Radius - target.Radius;
}

/// Oracle answer for one pair.
struct OracleContact final
{
    bool IsTangent = false;
    bool IsOverlapping = false;
    bool HasContact = false;
    float64 Fraction = 1.0;
};

OracleContact FindFirstContact(const OracleShape& shape, Vector3d translation, const OracleShape& target)
{
    OracleContact contact;
    const auto separationAt = [&](float64 t) { return GetSeparation(shape, shape.Center + translation * t, target); };
    const float64 start = separationAt(0.0);
    if (math::Abs(start) <= TANGENCY_METERS)
    {
        contact.IsTangent = true;
        return contact;
    }
    if (start < 0.0)
    {
        contact.IsOverlapping = true;
        return contact;
    }
    float64 low = 0.0;
    float64 high = 1.0;
    for (uint32 iteration = 0; iteration < SEARCH_ITERATIONS; ++iteration)
    {
        const float64 left = high - (high - low) * GOLDEN_SECTION;
        const float64 right = low + (high - low) * GOLDEN_SECTION;
        if (separationAt(left) <= separationAt(right))
        {
            high = right;
        }
        else
        {
            low = left;
        }
    }
    const float64 minimumTime = 0.5 * (low + high);
    const float64 minimum = separationAt(minimumTime);
    if (math::Abs(minimum) <= TANGENCY_METERS)
    {
        contact.IsTangent = true;
        return contact;
    }
    if (minimum > 0.0)
    {
        return contact;
    }
    float64 before = 0.0;
    float64 after = minimumTime;
    for (uint32 iteration = 0; iteration < SEARCH_ITERATIONS; ++iteration)
    {
        const float64 middle = 0.5 * (before + after);
        (separationAt(middle) > 0.0 ? before : after) = middle;
    }
    contact.HasContact = true;
    contact.Fraction = after;
    contact.IsTangent = (1.0 - after) * math::Length(translation) <= TANGENCY_METERS;
    return contact;
}

ConvexShape MakeShape(const OracleShape& shape)
{
    switch (shape.Kind)
    {
        case ConvexShapeKind::Sphere:
            return MakeSphereShape(shape.Radius);
        case ConvexShapeKind::Capsule:
            return MakeCapsuleShape(shape.HalfHeight, shape.Radius);
        case ConvexShapeKind::Box:
        case ConvexShapeKind::Hull:
            return MakeBoxShape(shape.HalfExtents);
    }
    return {};
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
           first.Collider == second.Collider;
}
} // namespace

TEST_CASE("Rays enter convex colliders at their outward surface", "[collision][world][convex]")
{
    Scene scene;
    InitializeOrFail(scene, { .MaxShapes = 4 }, { .MaxColliders = 4 });
    const ShapeHandle sphere = AddPrimitiveOrFail(scene.Store, MakeSphereShape(0.5));
    const ShapeHandle box = AddPrimitiveOrFail(scene.Store, MakeBoxShape({0.5, 0.25, 0.5}));
    const ShapeHandle capsule = AddPrimitiveOrFail(scene.Store, MakeCapsuleShape(0.5, 0.25));
    const ShapeHandle hull = AddBoxHullOrFail(scene.Store, {0.5f, 0.25f, 0.5f});
    // Four colliders along x, each struck by a ray falling from y = 5.
    const ColliderHandle sphereCollider =
        CreateOrFail(scene.World, { .Shape = sphere, .Pose = { .PositionWorld = {0.0, 0.0, 0.0} } });
    const ColliderHandle boxCollider = CreateOrFail(scene.World,
                                                    {
                                                        .Shape = box,
                                                        .Pose =
                                                        {
                                                            .PositionWorld = {3.0, 0.0, 0.0},
                                                            .OrientationWorld = MakeRotation({0, 1, 0}, 0.6),
                                                        },
                                                    });
    // A capsule lying along world x: its axis rotated from +y to -x.
    const ColliderHandle capsuleCollider = CreateOrFail(
        scene.World,
        {
            .Shape = capsule,
            .Pose = { .PositionWorld = {6.0, 0.0, 0.0}, .OrientationWorld = MakeRotation({0, 0, 1}, math::kHalfPi) },
        });
    const ColliderHandle hullCollider =
        CreateOrFail(scene.World, { .Shape = hull, .Pose = { .PositionWorld = {9.0, 1.0, 0.0} } });
    CollisionReadView view;
    REQUIRE(scene.World.TryAcquireReadView(view) == Status::Success);
    struct Expected final
    {
        float64 X;
        float64 SurfaceY;
        ColliderHandle Collider;
    };
    const StaticArray<Expected, 4> expectations{Expected{0.0, 0.5, sphereCollider},
                                                Expected{3.0, 0.25, boxCollider},
                                                Expected{6.4, 0.25, capsuleCollider},
                                                Expected{9.2, 1.25, hullCollider}};
    for (const Expected& expected : expectations)
    {
        INFO("x " << expected.X);
        const WorldRaycastResult hit = RaycastOrFail(
            view,
            { .OriginWorld = {expected.X, 5.0, 0.0}, .DirectionWorld = {0, -1, 0}, .MaxDistanceMeters = 10.0 });
        REQUIRE(hit.HasHit == true);
        CHECK(hit.Collider == expected.Collider);
        CHECK(hit.DistanceMeters == Catch::Approx(5.0 - expected.SurfaceY).margin(ANSWER_TOLERANCE));
        CHECK(hit.PositionWorld.Y == Catch::Approx(expected.SurfaceY).margin(ANSWER_TOLERANCE));
        REQUIRE(hit.HasNormal == true);
        CHECK(hit.NormalWorld.Y == Catch::Approx(1.0).margin(ANSWER_TOLERANCE));
        CHECK(hit.IsFrontFace == true);
        CHECK(hit.TriangleIndex == INVALID_TRIANGLE_INDEX);
    }
    // An oblique ray meets the sphere where the quadratic says.
    const WorldRaycastResult oblique =
        RaycastOrFail(view, { .OriginWorld = {-2.0, 0.3, 0.0}, .DirectionWorld = {1, 0, 0}, .MaxDistanceMeters = 3.0 });
    REQUIRE(oblique.HasHit == true);
    CHECK(oblique.DistanceMeters == Catch::Approx(2.0 - math::Sqrt(0.25 - 0.09)).margin(ANSWER_TOLERANCE));
    CHECK(oblique.NormalWorld.Y == Catch::Approx(0.6).margin(ANSWER_TOLERANCE));

    // Starting inside reports distance zero without a normal; a back-face-only ray never reports a solid.
    WorldRaycastInput inside{ .OriginWorld = {0.1, 0.0, 0.0}, .DirectionWorld = {0, 1, 0}, .MaxDistanceMeters = 2.0 };
    const WorldRaycastResult start = RaycastOrFail(view, inside);
    REQUIRE(start.HasHit == true);
    CHECK(start.DistanceMeters == 0.0);
    CHECK(start.HasNormal == false);
    CHECK(start.IsFrontFace == false);
    inside.FacePolicy = TriangleFacePolicy::BackFacesOnly;
    CHECK(RaycastOrFail(view, inside).HasHit == false);
    // Passing beside the sphere misses.
    CHECK(RaycastOrFail(view,
                        {
                            .OriginWorld = {-2.0, 0.51, 0.0},
                            .DirectionWorld = {1, 0, 0},
                            .MaxDistanceMeters = 4.0,
                        })
              .HasHit == false);
}

TEST_CASE("Convex casts against convex colliders land at the analytic fraction", "[collision][world][convex]")
{
    Scene scene;
    InitializeOrFail(scene, { .MaxShapes = 2 }, { .MaxColliders = 2 });
    const ShapeHandle sphere = AddPrimitiveOrFail(scene.Store, MakeSphereShape(0.5));
    const ShapeHandle box = AddPrimitiveOrFail(scene.Store, MakeBoxShape({1.0, 0.5, 1.0}));
    const ColliderHandle ball = CreateOrFail(scene.World, { .Shape = sphere, .Binding = 3 });
    const ColliderHandle block = CreateOrFail(scene.World,
                                              {
                                                  .Shape = box,
                                                  .Pose =
                                                  {
                                                      .PositionWorld = {5.0, 0.0, 0.0},
                                                  },
                                              });
    CollisionReadView view;
    REQUIRE(scene.World.TryAcquireReadView(view) == Status::Success);
    // A falling 0.25 m sphere touches the 0.5 m sphere when their centres are 0.75 m apart.
    const WorldConvexCastResult spheres = CastOrFail(view,
                                                     {
                                                         .Shape = MakeSphereShape(0.25),
                                                         .PositionWorld = {0.0, 3.0, 0.0},
                                                         .TranslationWorld = {0, -4, 0},
                                                     });
    REQUIRE(spheres.HasHit == true);
    CHECK(spheres.Collider == ball);
    CHECK(spheres.Binding == 3);
    CHECK(spheres.Fraction == Catch::Approx((3.0 - 0.75) / 4.0).margin(ANSWER_TOLERANCE));
    CHECK(spheres.NormalWorld.Y == Catch::Approx(1.0).margin(ANSWER_TOLERANCE));
    REQUIRE(spheres.HasWitness == true);
    // The witness is on the target's surface, not its core.
    CHECK(spheres.WitnessPositionWorld.Y == Catch::Approx(0.5).margin(ANSWER_TOLERANCE));
    // A capsule dropped onto the block lands on its top face.
    const WorldConvexCastResult landed = CastOrFail(
        view,
        { .Shape = MakeCapsuleShape(0.4, 0.1), .PositionWorld = {5.2, 2.0, 0.3}, .TranslationWorld = {0, -2, 0} });
    REQUIRE(landed.HasHit == true);
    CHECK(landed.Collider == block);
    CHECK(landed.Fraction == Catch::Approx((2.0 - 0.5 - 0.5) / 2.0).margin(ANSWER_TOLERANCE));
    CHECK(landed.WitnessPositionWorld.Y == Catch::Approx(0.5).margin(ANSWER_TOLERANCE));
    // Starting inside the block: the policy decides, as for meshes.
    WorldConvexCastInput sunk
    {
        .Shape = MakeSphereShape(0.25),
        .PositionWorld = {5.0, 0.2, 0.0},
        .TranslationWorld = {0, 1, 0},
    };
    const WorldConvexCastResult reported = CastOrFail(view, sunk);
    CHECK(reported.HasStartedOverlapping == true);
    CHECK(reported.HasNormal == false);
    sunk.OverlapPolicy = InitialOverlapPolicy::Ignore;
    CHECK(CastOrFail(view, sunk).HasHit == false);
    sunk.OverlapPolicy = InitialOverlapPolicy::Reject;
    WorldConvexCastResult rejected;
    CHECK(view.CastConvexClosest(sunk, rejected) == Status::InitialOverlap);
    CHECK(rejected.HasHit == false);
}

TEST_CASE("Overlap queries report every overlapping collider with completion", "[collision][world][overlap]")
{
    Scene scene;
    InitializeOrFail(scene, { .MaxShapes = 4 }, { .MaxColliders = 8 });
    const ShapeHandle floor = AddFloorOrFail(scene.Store);
    const ShapeHandle sphere = AddPrimitiveOrFail(scene.Store, MakeSphereShape(0.25));
    const ColliderHandle ground = CreateOrFail(scene.World, { .Shape = floor, .Binding = 10 });
    const ColliderHandle first =
        CreateOrFail(scene.World, { .Shape = sphere, .Pose = { .PositionWorld = {0.3, 0.2, 0.3} }, .Binding = 11 });
    const ColliderHandle second =
        CreateOrFail(scene.World, { .Shape = sphere, .Pose = { .PositionWorld = {0.7, 0.2, 0.7} }, .Binding = 12 });
    // Exactly touching the query box from above: touching is not overlapping.
    (void)CreateOrFail(scene.World, { .Shape = sphere, .Pose = { .PositionWorld = {0.5, 0.75, 0.5} } });
    // Far away.
    (void)CreateOrFail(scene.World, { .Shape = sphere, .Pose = { .PositionWorld = {5.0, 0.0, 0.0} } });
    CollisionReadView view;
    REQUIRE(scene.World.TryAcquireReadView(view) == Status::Success);
    // A box over the whole floor quad, from y = -0.1 to 0.5, overlaps both floor triangles and both low spheres.
    WorldOverlapInput input{ .Shape = MakeBoxShape({0.5, 0.3, 0.5}), .PositionWorld = {0.5, 0.2, 0.5} };
    StaticArray<ColliderOverlap, 4> records{};
    WorldOverlapResult result;
    REQUIRE(view.Overlap(input, BorrowedView<ColliderOverlap>(records.GetData(), records.GetSize()), result) ==
            Status::Success);
    CHECK(result.HasOverlap == true);
    CHECK(result.OverlapCount == 3);
    CHECK(result.WrittenCount == 3);
    CHECK(result.IsComplete == true);
    CHECK(records[0].Collider == ground);
    CHECK(records[0].Binding == 10);
    CHECK(records[0].TriangleCount == 2);
    CHECK(records[0].TriangleIndex == 0);
    CHECK(records[1].Collider == first);
    CHECK(records[1].TriangleIndex == INVALID_TRIANGLE_INDEX);
    CHECK(records[1].TriangleCount == 0);
    CHECK(records[2].Collider == second);

    // A buffer too small: the first records in slot order and the full count.
    REQUIRE(view.Overlap(input, BorrowedView<ColliderOverlap>(records.GetData(), 2), result) == Status::Success);
    CHECK(result.OverlapCount == 3);
    CHECK(result.WrittenCount == 2);
    CHECK(result.IsComplete == false);
    REQUIRE(view.Overlap(input, BorrowedView<ColliderOverlap>(), result) == Status::Success);
    CHECK(result.HasOverlap == true);
    CHECK(result.WrittenCount == 0);
    CHECK(result.IsComplete == false);

    // Any stops at one.
    input.Mode = OverlapMode::Any;
    REQUIRE(view.Overlap(input, BorrowedView<ColliderOverlap>(records.GetData(), records.GetSize()), result) ==
            Status::Success);
    CHECK(result.OverlapCount == 1);
    CHECK(result.WrittenCount == 1);
    CHECK(result.IsComplete == true);

    // Filters apply; an empty result is a complete miss.
    input.Mode = OverlapMode::All;
    const StaticArray<ColliderHandle, 1> ignored{ground};
    input.Filter.IgnoredColliders = BorrowedView<const ColliderHandle>(ignored.GetData(), 1);
    REQUIRE(view.Overlap(input, BorrowedView<ColliderOverlap>(records.GetData(), records.GetSize()), result) ==
            Status::Success);
    CHECK(result.OverlapCount == 2);
    input.Filter = {};
    input.PositionWorld = {0.5, 3.0, 0.5};
    REQUIRE(view.Overlap(input, BorrowedView<ColliderOverlap>(records.GetData(), records.GetSize()), result) ==
            Status::Success);
    CHECK(result.HasOverlap == false);
    CHECK(result.IsComplete == true);

    // Invalid requests fail without changing the result.
    const WorldOverlapResult sentinel{ .HasOverlap = true, .OverlapCount = 7, .WrittenCount = 7, .IsComplete = false };
    WorldOverlapResult unchanged = sentinel;
    input.Mode = static_cast<OverlapMode>(9);
    CHECK(view.Overlap(input, BorrowedView<ColliderOverlap>(), unchanged) == Status::InvalidArgument);
    input.Mode = OverlapMode::All;
    CHECK(view.Overlap(input, BorrowedView<ColliderOverlap>(nullptr, 3), unchanged) == Status::InvalidArgument);
    input.Shape = MakeSphereShape(0.0);
    CHECK(view.Overlap(input, BorrowedView<ColliderOverlap>(), unchanged) == Status::InvalidArgument);
    CHECK(unchanged.OverlapCount == 7);
    CollisionReadView inactive;
    CHECK(inactive.Overlap(input, BorrowedView<ColliderOverlap>(), unchanged) == Status::InvalidPhase);
}

TEST_CASE("Convex contact against convex colliders matches independent oracles", "[collision][world][convex][oracle]")
{
    RandomStream random;
    REQUIRE(random.TryReseed(71, 0) == true);
    constexpr uint32 QUERY_COUNT = 900;
    constexpr uint32 PAIR_KINDS = 3;
    uint32 hits = 0;
    uint32 overlaps = 0;
    uint32 misses = 0;
    uint32 rayHits = 0;
    for (uint32 query = 0; query < QUERY_COUNT; ++query)
    {
        // Pairs: sphere-sphere, capsule-capsule (both exact), box-box (separating-axis gap).
        const uint32 pair = query % PAIR_KINDS;
        const ConvexShapeKind kind =
            pair == 0 ? ConvexShapeKind::Sphere : (pair == 1 ? ConvexShapeKind::Capsule : ConvexShapeKind::Box);
        const auto makeOracle = [&random, kind](Vector3d center) {
            OracleShape shape;
            shape.Kind = kind;
            shape.Center = center;
            shape.Radius = static_cast<float64>(NextInRange(random, 0.1f, 0.8f));
            shape.HalfHeight = static_cast<float64>(NextInRange(random, 0.0f, 0.8f));
            shape.HalfExtents = {static_cast<float64>(NextInRange(random, 0.1f, 0.8f)),
                                 static_cast<float64>(NextInRange(random, 0.1f, 0.8f)),
                                 static_cast<float64>(NextInRange(random, 0.1f, 0.8f))};
            return shape;
        };
        OracleShape target = makeOracle(Widen(NextPoint(random, 3.0f)));
        const math::Quaternion targetOrientation =
            MakeRotation(NextDirection(random), static_cast<float64>(NextInRange(random, -3.0f, 3.0f)));
        target.Axes = GetAxes(targetOrientation);
        // Every fourth shape starts close to its target, so starting overlaps are common.
        const float32 startSpread = query % 4 == 0 ? 0.8f : 4.0f;
        OracleShape moving = makeOracle(target.Center + Widen(NextPoint(random, startSpread)));
        const math::Quaternion movingOrientation =
            MakeRotation(NextDirection(random), static_cast<float64>(NextInRange(random, -3.0f, 3.0f)));
        moving.Axes = GetAxes(movingOrientation);
        const Vector3d towardTarget = target.Center + Widen(NextPoint(random, 1.0f)) - moving.Center;
        const Vector3d translation = towardTarget * static_cast<float64>(NextInRange(random, 0.3f, 1.6f));

        Scene scene;
        InitializeOrFail(scene, { .MaxShapes = 1 }, { .MaxColliders = 1 });
        const ShapeHandle shape = AddPrimitiveOrFail(scene.Store, MakeShape(target));
        const ColliderHandle collider = CreateOrFail(
            scene.World,
            { .Shape = shape, .Pose = { .PositionWorld = target.Center, .OrientationWorld = targetOrientation } });
        CollisionReadView view;
        REQUIRE(scene.World.TryAcquireReadView(view) == Status::Success);
        WorldConvexCastInput cast
        {
            .Shape = MakeShape(moving),
            .PositionWorld = moving.Center,
            .OrientationWorld = movingOrientation,
            .TranslationWorld = translation,
        };
        const WorldConvexCastResult hierarchy = CastOrFail(view, cast);
        cast.Traversal = QueryTraversal::AllTriangles;
        REQUIRE(AreEqual(hierarchy, CastOrFail(view, cast)) == true);

        const OracleContact oracle = FindFirstContact(moving, translation, target);
        INFO("query " << query << " pair " << pair);
        if (oracle.IsTangent == false)
        {
            if (oracle.IsOverlapping == true)
            {
                CHECK(hierarchy.HasStartedOverlapping == true);
                ++overlaps;
            }
            else
            {
                REQUIRE(hierarchy.HasHit == oracle.HasContact);
                if (oracle.HasContact == true)
                {
                    CHECK(hierarchy.Collider == collider);
                    CHECK(math::Abs(hierarchy.Fraction - oracle.Fraction) * math::Length(translation) <=
                          CONTACT_DISTANCE_TOLERANCE_METERS);
                    CHECK(math::Dot(hierarchy.NormalWorld, translation) < 0.0);
                    ++hits;
                }
                else
                {
                    ++misses;
                }
            }
        }

        // The overlap query agrees with the oracle's sign at the start.
        const float64 startSeparation = GetSeparation(moving, moving.Center, target);
        if (math::Abs(startSeparation) > TANGENCY_METERS)
        {
            WorldOverlapResult overlap;
            REQUIRE(view.Overlap(
                        {
                            .Shape = MakeShape(moving),
                            .PositionWorld = moving.Center,
                            .OrientationWorld = movingOrientation,
                        },
                        BorrowedView<ColliderOverlap>(),
                        overlap) == Status::Success);
            CHECK(overlap.HasOverlap == (startSeparation < 0.0));
        }

        // A ray against a sphere or box target, by the quadratic and the slab test.
        if (kind != ConvexShapeKind::Capsule)
        {
            const Vector3d direction = translation * (1.0 / math::Length(translation));
            const WorldRaycastResult ray =
                RaycastOrFail(view,
                              { .OriginWorld = moving.Center, .DirectionWorld = direction, .MaxDistanceMeters = 10.0 });
            float64 entry = -1.0;
            bool isInside = false;
            // Rays that only graze the target are skipped: their chord is shorter than this.
            constexpr float64 MIN_CHORD_METERS = 1.0e-3;
            bool isGrazing = false;
            if (kind == ConvexShapeKind::Sphere)
            {
                const Vector3d offset = moving.Center - target.Center;
                const float64 b = math::Dot(offset, direction);
                const float64 c = math::Dot(offset, offset) - target.Radius * target.Radius;
                const float64 discriminant = b * b - c;
                isInside = c < 0.0;
                entry = discriminant >= 0.0 ? -b - math::Sqrt(discriminant) : -1.0;
                isGrazing = discriminant >= 0.0 && 2.0 * math::Sqrt(discriminant) < MIN_CHORD_METERS;
            }
            else
            {
                float64 enter = -1.0e300;
                float64 exit = 1.0e300;
                const StaticArray<float64, AXIS_COUNT> halves{target.HalfExtents.X,
                                                              target.HalfExtents.Y,
                                                              target.HalfExtents.Z};
                for (usize axis = 0; axis < AXIS_COUNT; ++axis)
                {
                    const float64 origin = math::Dot(moving.Center - target.Center, target.Axes[axis]);
                    const float64 step = math::Dot(direction, target.Axes[axis]);
                    const float64 low = (-halves[axis] - origin) / step;
                    const float64 high = (halves[axis] - origin) / step;
                    enter = math::Max(enter, math::Min(low, high));
                    exit = math::Min(exit, math::Max(low, high));
                }
                isInside = enter < 0.0 && exit > 0.0;
                entry = enter <= exit ? enter : -1.0;
                isGrazing = enter <= exit && exit - enter < MIN_CHORD_METERS;
            }
            if (isInside == true)
            {
                CHECK(ray.HasHit == true);
                CHECK(ray.DistanceMeters == 0.0);
                CHECK(ray.HasNormal == false);
            }
            else if (isGrazing == false && entry > TANGENCY_METERS && entry < 10.0 - TANGENCY_METERS)
            {
                // Grazing rays are skipped by requiring a clear entry; the tolerance is about 1e-8 m here.
                REQUIRE(ray.HasHit == true);
                CHECK(math::Abs(ray.DistanceMeters - entry) < CONTACT_DISTANCE_TOLERANCE_METERS);
                ++rayHits;
            }
        }
    }
    UNSCOPED_INFO("hits " << hits << " overlaps " << overlaps << " misses " << misses << " ray hits " << rayHits);
    CHECK(hits > QUERY_COUNT / 10);
    CHECK(overlaps > QUERY_COUNT / 20);
    CHECK(misses > QUERY_COUNT / 20);
    CHECK(rayHits > QUERY_COUNT / 10);
}

TEST_CASE("Mixed worlds of convex and mesh colliders match the brute-force traversal",
          "[collision][world][convex][oracle]")
{
    Scene scene;
    InitializeOrFail(scene, { .MaxShapes = 6 }, { .MaxColliders = 30 });
    RandomStream heights;
    REQUIRE(heights.TryReseed(81, 0) == true);
    ShapeHandle terrain;
    REQUIRE(scene.Store.TryAddTriangleMesh(CookOrFail(MakeGrid(6, &heights)), terrain) == Status::Success);
    const StaticArray<ShapeHandle, 5> shapes{terrain,
                                             AddPrimitiveOrFail(scene.Store, MakeSphereShape(0.6)),
                                             AddPrimitiveOrFail(scene.Store, MakeBoxShape({0.5, 0.8, 0.3})),
                                             AddPrimitiveOrFail(scene.Store, MakeCapsuleShape(0.7, 0.3)),
                                             AddBoxHullOrFail(scene.Store, {0.6f, 0.4f, 0.5f})};
    RandomStream random;
    REQUIRE(random.TryReseed(82, 0) == true);
    constexpr uint32 COLLIDER_COUNT = 30;
    Array<Vector3d> positions;
    for (uint32 index = 0; index < COLLIDER_COUNT; ++index)
    {
        const ColliderDescription description
        {
            .Shape = shapes[index % shapes.GetSize()],
            .Pose =
            {
                .PositionWorld = Widen(NextPoint(random, 12.0f)),
                .OrientationWorld =
                    MakeRotation(NextDirection(random), static_cast<float64>(NextInRange(random, -3.0f, 3.0f))),
            },
            .Participation = { .IsSolid = index % 4 != 0, .IsSensor = index % 4 == 0 },
            .Filter = { .CategoryBits = 1U << (index % 3) },
            .Binding = index,
        };
        positions.Add(description.Pose.PositionWorld);
        (void)CreateOrFail(scene.World, description);
    }
    CollisionReadView view;
    REQUIRE(scene.World.TryAcquireReadView(view) == Status::Success);
    constexpr uint32 QUERY_COUNT = 300;
    constexpr uint32 BUFFER_SIZE = 4;
    uint32 rayHits = 0;
    uint32 castHits = 0;
    uint32 overlapHits = 0;
    for (uint32 query = 0; query < QUERY_COUNT; ++query)
    {
        const QueryFilter filter
        {
            .CategoryMask = query % 4 == 0 ? 5U : ALL_COLLISION_CATEGORIES,
            .ShouldIncludeSensors = query % 3 == 0,
        };
        const Vector3d origin = Widen(NextPoint(random, 15.0f));
        const Vector3d target = positions[query % COLLIDER_COUNT] + Widen(NextPoint(random, 1.5f));
        const Vector3d direction = (target - origin) * (1.0 / math::Length(target - origin));
        WorldRaycastInput ray
        {
            .OriginWorld = origin,
            .DirectionWorld = direction,
            .MaxDistanceMeters = 40.0,
            .Filter = filter,
        };
        WorldRaycastResult rayHierarchy;
        REQUIRE(view.RaycastClosest(ray, rayHierarchy) == Status::Success);
        ray.Traversal = QueryTraversal::AllTriangles;
        REQUIRE(AreEqual(rayHierarchy, RaycastOrFail(view, ray)) == true);
        rayHits += rayHierarchy.HasHit == true ? 1 : 0;

        WorldConvexCastInput cast
        {
            .Shape = query % 2 == 0 ? MakeCapsuleShape(0.3, 0.2) : MakeBoxShape({0.3, 0.2, 0.4}),
            .PositionWorld = origin,
            .OrientationWorld =
                MakeRotation(NextDirection(random), static_cast<float64>(NextInRange(random, -3.0f, 3.0f))),
            .TranslationWorld = direction * 30.0,
            .Filter = filter,
        };
        const WorldConvexCastResult castHierarchy = CastOrFail(view, cast);
        cast.Traversal = QueryTraversal::AllTriangles;
        REQUIRE(AreEqual(castHierarchy, CastOrFail(view, cast)) == true);
        castHits += castHierarchy.HasHit == true ? 1 : 0;

        WorldOverlapInput overlap
        {
            .Shape = query % 2 == 0 ? MakeSphereShape(1.5) : MakeBoxShape({1.0, 1.2, 0.8}),
            .PositionWorld = target,
            .OrientationWorld = cast.OrientationWorld,
            .Filter = filter,
        };
        StaticArray<ColliderOverlap, BUFFER_SIZE> hierarchyRecords{};
        StaticArray<ColliderOverlap, BUFFER_SIZE> bruteRecords{};
        WorldOverlapResult hierarchyResult;
        WorldOverlapResult bruteResult;
        REQUIRE(view.Overlap(overlap,
                             BorrowedView<ColliderOverlap>(hierarchyRecords.GetData(), BUFFER_SIZE),
                             hierarchyResult) == Status::Success);
        overlap.Traversal = QueryTraversal::AllTriangles;
        REQUIRE(view.Overlap(overlap,
                             BorrowedView<ColliderOverlap>(bruteRecords.GetData(), BUFFER_SIZE),
                             bruteResult) == Status::Success);
        REQUIRE(hierarchyResult.OverlapCount == bruteResult.OverlapCount);
        REQUIRE(hierarchyResult.WrittenCount == bruteResult.WrittenCount);
        REQUIRE(hierarchyResult.IsComplete == bruteResult.IsComplete);
        for (usize index = 0; index < hierarchyResult.WrittenCount; ++index)
        {
            REQUIRE(hierarchyRecords[index].Collider == bruteRecords[index].Collider);
            REQUIRE(hierarchyRecords[index].TriangleIndex == bruteRecords[index].TriangleIndex);
            REQUIRE(hierarchyRecords[index].TriangleCount == bruteRecords[index].TriangleCount);
        }
        overlapHits += hierarchyResult.HasOverlap == true ? 1 : 0;
    }
    UNSCOPED_INFO("ray hits " << rayHits << " cast hits " << castHits << " overlaps " << overlapHits);
    CHECK(rayHits > QUERY_COUNT / 6);
    CHECK(castHits > QUERY_COUNT / 4);
    CHECK(overlapHits > QUERY_COUNT / 4);
}

TEST_CASE("A rotated collider admits every admitted world translation", "[collision][world][convex]")
{
    // A translation whose largest component is at the limit grows to sqrt(2) of it in a collider rotated by 45
    // degrees; the local cast must still accept it.
    Scene scene;
    InitializeOrFail(scene, { .MaxShapes = 2 }, { .MaxColliders = 2 });
    const ShapeHandle floor = AddFloorOrFail(scene.Store);
    const ShapeHandle box = AddPrimitiveOrFail(scene.Store, MakeBoxShape({1.0, 1.0, 1.0}));
    const math::Quaternion turned = MakeRotation({0, 0, 1}, math::kPi * 0.25);
    (void)CreateOrFail(scene.World, { .Shape = floor, .Pose = { .OrientationWorld = turned } });
    (void)CreateOrFail(scene.World,
                       { .Shape = box, .Pose = { .PositionWorld = {10.0, 0, 0}, .OrientationWorld = turned } });
    CollisionReadView view;
    REQUIRE(scene.World.TryAcquireReadView(view) == Status::Success);
    const WorldConvexCastInput cast
    {
        .Shape = MakeSphereShape(0.1),
        .PositionWorld = {-5.0, 0.5, 0.5},
        .TranslationWorld = {MAX_QUERY_DISTANCE_METERS, MAX_QUERY_DISTANCE_METERS, 0.0},
    };
    WorldConvexCastResult result;
    CHECK(view.CastConvexClosest(cast, result) == Status::Success);
}
