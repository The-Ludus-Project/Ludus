#pragma once

// Shared fixtures for the contact generation tests: a two-collider scene builder and comparison helpers.

#include "test_meshes.h"

#include <ludus/foundation/base/core.h>

#include <ludus/foundation/containers/array.hpp>
#include <ludus/foundation/containers/borrowed_view.hpp>
#include <ludus/foundation/math/quaternion.hpp>
#include <ludus/foundation/math/scalar.hpp>
#include <ludus/foundation/math/vector.hpp>
#include <ludus/physics/collision/collision_world.h>
#include <ludus/physics/collision/contacts.h>
#include <ludus/physics/collision/convex_shape.h>
#include <ludus/physics/collision/geometry_store.h>
#include <ludus/physics/collision/status.h>
#include <ludus/physics/collision/triangle_mesh.h>

#include <catch2/catch_test_macros.hpp>

namespace ludus::physics::collision::test
{
namespace math = foundation::math;

inline constexpr usize BOX_HULL_POINT_COUNT = 8;
inline constexpr uint32 CONTACT_SCENE_SHAPES = 8;
inline constexpr uint32 CONTACT_SCENE_COLLIDERS = 8;

/// One shape of a contact test: a primitive, a box-shaped hull or a triangle mesh.
struct ShapeSpec final
{
    ConvexShape Primitive{};
    bool IsBoxHull = false;    ///< Cook an eight-point hull with half extents HullHalfExtents instead.
    Vector3 HullHalfExtents{}; ///< Half extents of the box hull.
    const Array<Vector3>* HullPointsOrNull = nullptr; ///< Cook a hull of these points instead.
    const MeshData* MeshOrNull = nullptr;             ///< Cook and use this mesh instead.
    TriangleMeshCookSettings MeshSettings{};
};

inline ShapeSpec MakeSphereSpec(float64 radius)
{
    return { .Primitive = MakeSphereShape(radius) };
}

inline ShapeSpec MakeCapsuleSpec(float64 halfHeight, float64 radius)
{
    return { .Primitive = MakeCapsuleShape(halfHeight, radius) };
}

inline ShapeSpec MakeBoxSpec(Vector3d halfExtents)
{
    return { .Primitive = MakeBoxShape(halfExtents) };
}

/// A hull with the corners of a box, in the corner order of the box vertex features: bit 0, 1, 2 set for +X, +Y, +Z.
inline ShapeSpec MakeBoxHullSpec(Vector3 halfExtents)
{
    return { .IsBoxHull = true, .HullHalfExtents = halfExtents };
}

inline ShapeSpec MakeHullSpec(const Array<Vector3>& points)
{
    return { .HullPointsOrNull = &points };
}

inline ShapeSpec MakeMeshSpec(const MeshData& mesh, const TriangleMeshCookSettings& settings = {})
{
    return { .MeshOrNull = &mesh, .MeshSettings = settings };
}

inline math::Quaternion MakeTurn(Vector3 axis, float32 angleRadians)
{
    math::Quaternion rotation;
    REQUIRE(math::TryFromAxisAngle(axis, angleRadians, rotation) == math::MathStatus::Success);
    return rotation;
}

/// A posed shape of a contact test.
struct PosedSpec final
{
    ShapeSpec Shape{};
    RigidPose Pose{};
};

/// A store and a world holding the colliders of one contact test.
struct ContactScene final
{
    GeometryStore Store;
    CollisionWorld World;

    ContactScene()
    {
        REQUIRE(Store.TryInitialize({ .MaxShapes = CONTACT_SCENE_SHAPES }) == Status::Success);
        REQUIRE(World.TryInitialize(Store, { .MaxColliders = CONTACT_SCENE_COLLIDERS }) == Status::Success);
    }

    ShapeHandle AddShape(const ShapeSpec& spec)
    {
        ShapeHandle shape;
        if (spec.MeshOrNull != nullptr)
        {
            REQUIRE(Store.TryAddTriangleMesh(CookOrFail(*spec.MeshOrNull, spec.MeshSettings), shape) ==
                    Status::Success);
        }
        else if (spec.IsBoxHull == true || spec.HullPointsOrNull != nullptr)
        {
            Array<Vector3> points;
            for (uint32 corner = 0; corner < BOX_HULL_POINT_COUNT && spec.IsBoxHull == true; ++corner)
            {
                points.Add({(corner & 1U) == 0 ? -spec.HullHalfExtents.X : spec.HullHalfExtents.X,
                            (corner & 2U) == 0 ? -spec.HullHalfExtents.Y : spec.HullHalfExtents.Y,
                            (corner & 4U) == 0 ? -spec.HullHalfExtents.Z : spec.HullHalfExtents.Z});
            }
            for (usize index = 0; spec.HullPointsOrNull != nullptr && index < spec.HullPointsOrNull->GetSize(); ++index)
            {
                points.Add((*spec.HullPointsOrNull)[index]);
            }
            ConvexHull hull;
            REQUIRE(
                ConvexHull::TryCook({ .PointsShape = BorrowedView<const Vector3>(points.GetData(), points.GetSize()) },
                                    hull)
                    .Result == Status::Success);
            REQUIRE(Store.TryAddConvexHull(foundation::Move(hull), shape) == Status::Success);
        }
        else
        {
            REQUIRE(Store.TryAddPrimitive(spec.Primitive, shape) == Status::Success);
        }
        return shape;
    }

    ColliderHandle AddCollider(const PosedSpec& posed, Vector3d translationWorld = {})
    {
        RigidPose pose = posed.Pose;
        pose.PositionWorld = pose.PositionWorld + translationWorld;
        ColliderHandle collider;
        REQUIRE(World.TryCreateCollider({ .Shape = AddShape(posed.Shape), .Pose = pose }, collider) == Status::Success);
        return collider;
    }
};

/// How a pair of a contact test is evaluated.
struct PairOptions final
{
    float64 ContactOffsetMeters = DEFAULT_CONTACT_OFFSET_METERS;
    Vector3d TranslationWorld{}; ///< Added to both poses.
    bool IsReversed = false;     ///< Query (second, first) instead of (first, second).
};

/// Generate the contacts of two posed shapes in a fresh scene.
inline ContactPairContacts
GeneratePair(const PosedSpec& first, const PosedSpec& second, const PairOptions& options = {})
{
    ContactScene scene;
    const ColliderHandle firstCollider = scene.AddCollider(first, options.TranslationWorld);
    const ColliderHandle secondCollider = scene.AddCollider(second, options.TranslationWorld);
    CollisionReadView view;
    REQUIRE(scene.World.TryAcquireReadView(view) == Status::Success);
    ContactPairContacts contacts;
    REQUIRE(view.GenerateContacts(options.IsReversed == true ? secondCollider : firstCollider,
                                  options.IsReversed == true ? firstCollider : secondCollider,
                                  { .ContactOffsetMeters = options.ContactOffsetMeters },
                                  contacts) == Status::Success);
    return contacts;
}

inline float64 GetDistance(Vector3d first, Vector3d second)
{
    return math::Length(first - second);
}

/// Index of the contact point nearest `positionWorld`, which must lie within `toleranceMeters` of one.
inline uint32 FindPointOrFail(const ContactPairContacts& contacts, Vector3d positionWorld, float64 toleranceMeters)
{
    uint32 best = 0;
    for (uint32 index = 1; index < contacts.PointCount; ++index)
    {
        if (GetDistance(contacts.Points[index].PositionWorld, positionWorld) <
            GetDistance(contacts.Points[best].PositionWorld, positionWorld))
        {
            best = index;
        }
    }
    REQUIRE(contacts.PointCount > 0);
    INFO("nearest point is " << GetDistance(contacts.Points[best].PositionWorld, positionWorld) << " m away");
    REQUIRE(GetDistance(contacts.Points[best].PositionWorld, positionWorld) <= toleranceMeters);
    return best;
}

/// Require that `mirrored` is `contacts` with the two colliders swapped: opposite normals, the same points and
/// separations, and the two features of every point exchanged.
inline void
RequireMirrored(const ContactPairContacts& contacts, const ContactPairContacts& mirrored, float64 toleranceMeters)
{
    REQUIRE(mirrored.PatchCount == contacts.PatchCount);
    REQUIRE(mirrored.PointCount == contacts.PointCount);
    for (uint32 patch = 0; patch < contacts.PatchCount; ++patch)
    {
        REQUIRE(GetDistance(mirrored.Patches[patch].NormalWorld, -contacts.Patches[patch].NormalWorld) <=
                toleranceMeters);
        REQUIRE(mirrored.Patches[patch].PointCount == contacts.Patches[patch].PointCount);
    }
    for (uint32 index = 0; index < contacts.PointCount; ++index)
    {
        const ContactPoint& point = contacts.Points[index];
        const ContactPoint& other = mirrored.Points[FindPointOrFail(mirrored, point.PositionWorld, toleranceMeters)];
        REQUIRE(math::Abs(other.SeparationMeters - point.SeparationMeters) <= toleranceMeters);
        REQUIRE(other.FirstFeature == point.SecondFeature);
        REQUIRE(other.SecondFeature == point.FirstFeature);
        REQUIRE(other.TriangleIndex == point.TriangleIndex);
    }
}

/// Require the same contacts within a tolerance, ignoring features: for a second route to the same geometry.
inline void RequireSameGeometry(const ContactPairContacts& contacts,
                                const ContactPairContacts& expected,
                                float64 toleranceMeters,
                                Vector3d translationWorld = {})
{
    REQUIRE(contacts.PatchCount == expected.PatchCount);
    REQUIRE(contacts.PointCount == expected.PointCount);
    for (uint32 patch = 0; patch < expected.PatchCount; ++patch)
    {
        REQUIRE(GetDistance(contacts.Patches[patch].NormalWorld, expected.Patches[patch].NormalWorld) <=
                toleranceMeters);
    }
    for (uint32 index = 0; index < expected.PointCount; ++index)
    {
        const ContactPoint& point = expected.Points[index];
        const ContactPoint& other =
            contacts.Points[FindPointOrFail(contacts, point.PositionWorld + translationWorld, toleranceMeters)];
        REQUIRE(math::Abs(other.SeparationMeters - point.SeparationMeters) <= toleranceMeters);
    }
}
} // namespace ludus::physics::collision::test
