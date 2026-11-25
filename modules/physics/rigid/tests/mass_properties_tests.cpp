// Mass properties: analytic primitives, hull volume integrals, compound composition with the parallel-axis
// theorem, rotated tensors, principal frames and rejection of invalid tensors.

#include "rigid_test_support.h"

#include <ludus/foundation/base/core.h>

#include <ludus/foundation/containers/array.hpp>
#include <ludus/foundation/containers/borrowed_view.hpp>
#include <ludus/foundation/containers/static_array.hpp>
#include <ludus/foundation/math/scalar.hpp>
#include <ludus/foundation/math/vector.hpp>
#include <ludus/physics/collision/convex_shape.h>
#include <ludus/physics/rigid/mass_properties.h>
#include <ludus/physics/rigid/status.h>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <limits>

using namespace ludus::physics::rigid;
using namespace ludus::physics::rigid::test;
using Catch::Approx;
using ludus::foundation::Array;
using ludus::foundation::StaticArray;
using ludus::foundation::math::Vector3;
namespace collision = ludus::physics::collision;

namespace
{
constexpr float64 WATER = 1000.0;
constexpr float64 RELATIVE = 1.0e-12;

collision::ConvexHull CookHull(const Array<Vector3>& points)
{
    collision::ConvexHull hull;
    REQUIRE(collision::ConvexHull::TryCook(
                {
                    .PointsShape = BorrowedView<const Vector3>(points.GetData(), points.GetSize()),
                },
                hull)
                .Result == collision::Status::Success);
    return hull;
}

Array<Vector3> MakeBoxPoints(Vector3 halfExtents, Vector3 offset)
{
    Array<Vector3> points;
    for (uint32 corner = 0; corner < 8; ++corner)
    {
        points.Add({offset.X + ((corner & 1U) == 0 ? -halfExtents.X : halfExtents.X),
                    offset.Y + ((corner & 2U) == 0 ? -halfExtents.Y : halfExtents.Y),
                    offset.Z + ((corner & 4U) == 0 ? -halfExtents.Z : halfExtents.Z)});
    }
    return points;
}

MassProperties ComputeOrFail(const collision::ConvexShape& shape, float64 density)
{
    MassProperties properties;
    REQUIRE(TryComputeShapeMassProperties(shape, density, properties) == Status::Success);
    return properties;
}

void RequireTensor(const InertiaTensor& actual, const InertiaTensor& expected, float64 tolerance)
{
    const float64 scale = math::Max(math::Abs(expected.Xx), math::Max(math::Abs(expected.Yy), math::Abs(expected.Zz)));
    CHECK(math::Abs(actual.Xx - expected.Xx) <= tolerance * scale);
    CHECK(math::Abs(actual.Yy - expected.Yy) <= tolerance * scale);
    CHECK(math::Abs(actual.Zz - expected.Zz) <= tolerance * scale);
    CHECK(math::Abs(actual.Xy - expected.Xy) <= tolerance * scale);
    CHECK(math::Abs(actual.Xz - expected.Xz) <= tolerance * scale);
    CHECK(math::Abs(actual.Yz - expected.Yz) <= tolerance * scale);
}

/// Midpoint-rule oracle over a grid: mass and inertia of a Y-axis capsule, independent of the closed form.
struct CapsuleGrid final
{
    float64 HalfHeight = 0.0;
    float64 Radius = 0.0;
    float64 Density = 0.0;
    uint32 Cells = 0;
};

MassProperties IntegrateCapsuleOnGrid(const CapsuleGrid& grid)
{
    const float64 halfHeight = grid.HalfHeight;
    const float64 radius = grid.Radius;
    const float64 density = grid.Density;
    const uint32 cells = grid.Cells;
    const float64 halfX = radius;
    const float64 halfY = halfHeight + radius;
    const float64 stepX = 2.0 * halfX / cells;
    const float64 stepY = 2.0 * halfY / cells;
    const float64 cellVolume = stepX * stepY * stepX;
    float64 mass = 0.0;
    InertiaTensor inertia;
    for (uint32 i = 0; i < cells; ++i)
    {
        const float64 x = -halfX + (i + 0.5) * stepX;
        for (uint32 j = 0; j < cells; ++j)
        {
            const float64 y = -halfY + (j + 0.5) * stepY;
            const float64 axial = math::Max(0.0, math::Abs(y) - halfHeight);
            for (uint32 k = 0; k < cells; ++k)
            {
                const float64 z = -halfX + (k + 0.5) * stepX;
                if (x * x + z * z + axial * axial > radius * radius)
                {
                    continue;
                }
                const float64 dm = density * cellVolume;
                mass += dm;
                inertia.Xx += dm * (y * y + z * z);
                inertia.Yy += dm * (x * x + z * z);
                inertia.Zz += dm * (x * x + y * y);
            }
        }
    }
    return { .MassKilograms = mass, .InertiaAboutCenterOfMass = inertia };
}
} // namespace

TEST_CASE("Sphere, box and capsule mass properties are analytic", "[rigid][mass]")
{
    const MassProperties sphere = ComputeOrFail(collision::MakeSphereShape(0.5), WATER);
    const float64 sphereMass = WATER * 4.0 / 3.0 * math::kPi * 0.125;
    CHECK(sphere.MassKilograms == Approx(sphereMass).epsilon(RELATIVE));
    RequireTensor(sphere.InertiaAboutCenterOfMass,
                  { .Xx = 0.1 * sphereMass, .Yy = 0.1 * sphereMass, .Zz = 0.1 * sphereMass },
                  RELATIVE);
    CHECK(sphere.CenterOfMassLocal == Vector3d{});

    const MassProperties box = ComputeOrFail(collision::MakeBoxShape({0.5, 1.0, 1.5}), WATER);
    CHECK(box.MassKilograms == Approx(6000.0).epsilon(RELATIVE));
    RequireTensor(box.InertiaAboutCenterOfMass, { .Xx = 6500.0, .Yy = 5000.0, .Zz = 2500.0 }, RELATIVE);

    // A capsule with no cylinder is a sphere.
    const MassProperties ball = ComputeOrFail(collision::MakeCapsuleShape(0.0, 0.5), WATER);
    CHECK(ball.MassKilograms == Approx(sphereMass).epsilon(RELATIVE));
    RequireTensor(ball.InertiaAboutCenterOfMass, sphere.InertiaAboutCenterOfMass, RELATIVE);

    // The closed form agrees with an independent grid integration to its discretization error.
    const MassProperties capsule = ComputeOrFail(collision::MakeCapsuleShape(0.5, 0.25), WATER);
    const MassProperties grid =
        IntegrateCapsuleOnGrid({ .HalfHeight = 0.5, .Radius = 0.25, .Density = WATER, .Cells = 160 });
    CHECK(capsule.MassKilograms == Approx(grid.MassKilograms).epsilon(0.01));
    RequireTensor(capsule.InertiaAboutCenterOfMass, grid.InertiaAboutCenterOfMass, 0.01);
    CHECK(capsule.InertiaAboutCenterOfMass.Xx == Approx(capsule.InertiaAboutCenterOfMass.Zz).epsilon(RELATIVE));
}

TEST_CASE("Hull volume integrals match closed forms", "[rigid][mass][hull]")
{
    SECTION("a box hull equals the box primitive, offset by its centre")
    {
        const collision::ConvexHull hull = CookHull(MakeBoxPoints({0.5f, 1.0f, 1.5f}, {1.0f, 2.0f, 3.0f}));
        const MassProperties properties = ComputeOrFail(collision::MakeHullShape(hull), WATER);
        CHECK(properties.MassKilograms == Approx(6000.0).epsilon(RELATIVE));
        CHECK(GetMaxDifference(properties.CenterOfMassLocal, {1.0, 2.0, 3.0}) <= 1.0e-12);
        RequireTensor(properties.InertiaAboutCenterOfMass, { .Xx = 6500.0, .Yy = 5000.0, .Zz = 2500.0 }, 1.0e-12);
    }
    SECTION("the unit corner tetrahedron")
    {
        Array<Vector3> points;
        points.Add({0.0f, 0.0f, 0.0f});
        points.Add({1.0f, 0.0f, 0.0f});
        points.Add({0.0f, 1.0f, 0.0f});
        points.Add({0.0f, 0.0f, 1.0f});
        const collision::ConvexHull hull = CookHull(points);
        const MassProperties properties = ComputeOrFail(collision::MakeHullShape(hull), 1.0);
        CHECK(properties.MassKilograms == Approx(1.0 / 6.0).epsilon(RELATIVE));
        CHECK(GetMaxDifference(properties.CenterOfMassLocal, {0.25, 0.25, 0.25}) <= 1.0e-15);
        // Second moments 1/60 and 1/120 shifted to the centroid.
        RequireTensor(properties.InertiaAboutCenterOfMass,
                      {
                          .Xx = 1.0 / 80.0,
                          .Yy = 1.0 / 80.0,
                          .Zz = 1.0 / 80.0,
                          .Xy = 1.0 / 480.0,
                          .Xz = 1.0 / 480.0,
                          .Yz = 1.0 / 480.0,
                      },
                      1.0e-12);
    }
    SECTION("interior, duplicate and coplanar face points leave a box unchanged")
    {
        Array<Vector3> points = MakeBoxPoints({0.5f, 0.5f, 0.5f}, {});
        points.Add({0.0f, 0.0f, 0.0f});
        points.Add({0.5f, 0.0f, 0.0f});
        points.Add({0.0f, -0.5f, 0.25f});
        points.Add({0.5f, 0.5f, 0.5f});
        const collision::ConvexHull hull = CookHull(points);
        const MassProperties properties = ComputeOrFail(collision::MakeHullShape(hull), 1.0);
        CHECK(properties.MassKilograms == Approx(1.0).epsilon(RELATIVE));
        RequireTensor(properties.InertiaAboutCenterOfMass,
                      { .Xx = 1.0 / 6.0, .Yy = 1.0 / 6.0, .Zz = 1.0 / 6.0 },
                      1.0e-12);
    }
    SECTION("the octahedron |x| + |y| + |z| <= 1")
    {
        Array<Vector3> points;
        points.Add({1.0f, 0.0f, 0.0f});
        points.Add({-1.0f, 0.0f, 0.0f});
        points.Add({0.0f, 1.0f, 0.0f});
        points.Add({0.0f, -1.0f, 0.0f});
        points.Add({0.0f, 0.0f, 1.0f});
        points.Add({0.0f, 0.0f, -1.0f});
        const collision::ConvexHull hull = CookHull(points);
        const MassProperties properties = ComputeOrFail(collision::MakeHullShape(hull), 1.0);
        CHECK(properties.MassKilograms == Approx(4.0 / 3.0).epsilon(RELATIVE));
        const float64 moment = properties.MassKilograms / 5.0;
        RequireTensor(properties.InertiaAboutCenterOfMass, { .Xx = moment, .Yy = moment, .Zz = moment }, 1.0e-12);
    }
    SECTION("a dense random hull agrees with its point cloud's convex volume bounds")
    {
        // Points on a sphere: the hull volume approaches the ball's from below.
        Array<Vector3> points;
        constexpr uint32 POINT_COUNT = 256;
        for (uint32 index = 0; index < POINT_COUNT; ++index)
        {
            const float64 y = 1.0 - 2.0 * (index + 0.5) / POINT_COUNT;
            const float64 ring = math::Sqrt(1.0 - y * y);
            const float64 angle = index * 2.399963229728653; // golden angle
            points.Add({static_cast<float32>(ring * math::Cos(angle)),
                        static_cast<float32>(y),
                        static_cast<float32>(ring * math::Sin(angle))});
        }
        const collision::ConvexHull hull = CookHull(points);
        const MassProperties properties = ComputeOrFail(collision::MakeHullShape(hull), 1.0);
        const float64 ballVolume = 4.0 / 3.0 * math::kPi;
        CHECK(properties.MassKilograms < ballVolume);
        CHECK(properties.MassKilograms > 0.97 * ballVolume);
        CHECK(GetMaxDifference(properties.CenterOfMassLocal, {}) <= 1.0e-2);
        CHECK(TryValidateMassProperties(properties) == Status::Success);
    }
}

TEST_CASE("Shape mass properties reject invalid inputs", "[rigid][mass]")
{
    MassProperties properties{ .MassKilograms = 7.0 };
    const MassProperties original = properties;
    CHECK(TryComputeShapeMassProperties(collision::MakeSphereShape(0.5), 0.0, properties) == Status::InvalidArgument);
    CHECK(TryComputeShapeMassProperties(collision::MakeSphereShape(0.5), -1.0, properties) == Status::InvalidArgument);
    CHECK(TryComputeShapeMassProperties(collision::MakeSphereShape(0.5),
                                        std::numeric_limits<float64>::quiet_NaN(),
                                        properties) == Status::NonFiniteInput);
    CHECK(TryComputeShapeMassProperties(collision::MakeSphereShape(0.5), 1.0e7, properties) == Status::OutOfRange);
    CHECK(TryComputeShapeMassProperties(collision::MakeSphereShape(0.0), WATER, properties) == Status::InvalidArgument);
    CHECK(TryComputeShapeMassProperties(collision::MakeBoxShape({1.0, -1.0, 1.0}), WATER, properties) ==
          Status::InvalidArgument);
    CHECK(TryComputeShapeMassProperties(collision::MakeCapsuleShape(-0.5, 0.5), WATER, properties) ==
          Status::InvalidArgument);
    CHECK(TryComputeShapeMassProperties({ .Kind = collision::ConvexShapeKind::Hull }, WATER, properties) ==
          Status::InvalidArgument);
    const collision::ConvexHull empty;
    CHECK(TryComputeShapeMassProperties(collision::MakeHullShape(empty), WATER, properties) == Status::InvalidArgument);
    // A heavy enough body exceeds the mass limit even though every input is admitted.
    CHECK(TryComputeShapeMassProperties(collision::MakeBoxShape({1.0e3, 1.0e3, 1.0e3}), 1.0e5, properties) ==
          Status::InvalidMassProperties);
    CHECK(properties.MassKilograms == original.MassKilograms);
}

TEST_CASE("Compound composition uses rotated tensors and the parallel-axis theorem", "[rigid][mass][compound]")
{
    const MassProperties light = ComputeOrFail(collision::MakeSphereShape(0.5), WATER);
    const MassProperties heavy = ComputeOrFail(collision::MakeSphereShape(0.5), 2.0 * WATER);
    SECTION("asymmetric compound COM")
    {
        const StaticArray<MassPart, 2> parts{
            MassPart{ .Properties = light, .Pose = { .PositionBody = {-1.0, 0.0, 0.0} } },
            MassPart{ .Properties = heavy, .Pose = { .PositionBody = {2.0, 0.0, 0.0} } },
        };
        MassProperties composed;
        REQUIRE(TryComposeMassProperties(BorrowedView<const MassPart>(parts.GetData(), 2), composed) ==
                Status::Success);
        const float64 m1 = light.MassKilograms;
        const float64 m2 = heavy.MassKilograms;
        CHECK(composed.MassKilograms == Approx(m1 + m2).epsilon(RELATIVE));
        // (-1 m1 + 2 m2) / (m1 + m2) with m2 = 2 m1.
        CHECK(GetMaxDifference(composed.CenterOfMassLocal, {1.0, 0.0, 0.0}) <= 1.0e-12);
        const float64 own = light.InertiaAboutCenterOfMass.Xx + heavy.InertiaAboutCenterOfMass.Xx;
        const float64 transverse = own + m1 * 4.0 + m2 * 1.0;
        RequireTensor(composed.InertiaAboutCenterOfMass, { .Xx = own, .Yy = transverse, .Zz = transverse }, RELATIVE);
    }
    SECTION("rotated box inertia")
    {
        const MassProperties box = ComputeOrFail(collision::MakeBoxShape({0.5, 1.0, 1.5}), WATER);
        const MassPart part
        {
            .Properties = box,
            .Pose = { .OrientationBody = MakeAxisAngle({0.0, 0.0, 1.0}, 0.25 * math::kPi) },
        };
        MassProperties composed;
        REQUIRE(TryComposeMassProperties(BorrowedView<const MassPart>(&part, 1), composed) == Status::Success);
        // R I R^T about +Z by 45 degrees: (Ixx + Iyy) / 2 on the diagonal and (Ixx - Iyy) / 2 off it.
        RequireTensor(composed.InertiaAboutCenterOfMass,
                      { .Xx = 5750.0, .Yy = 5750.0, .Zz = 2500.0, .Xy = 750.0 },
                      1.0e-6);

        PrincipalMassFrame frame;
        REQUIRE(TryComputePrincipalMassFrame(composed, frame) == Status::Success);
        // The principal frame recovers the box's moments and axes up to their order.
        const Vector3d moments = frame.PrincipalMomentsKilogramMetersSquared;
        CHECK(moments.X + moments.Y + moments.Z == Approx(14000.0).epsilon(1.0e-9));
        CHECK(math::Min(moments.X, math::Min(moments.Y, moments.Z)) == Approx(2500.0).epsilon(1.0e-9));
        CHECK(math::Max(moments.X, math::Max(moments.Y, moments.Z)) == Approx(6500.0).epsilon(1.0e-9));
        const StaticArray<Vector3d, 3> axes{RotateVector(frame.BodyFromCenterOfMass.OrientationBody, {1.0, 0.0, 0.0}),
                                            RotateVector(frame.BodyFromCenterOfMass.OrientationBody, {0.0, 1.0, 0.0}),
                                            RotateVector(frame.BodyFromCenterOfMass.OrientationBody, {0.0, 0.0, 1.0})};
        const StaticArray<float64, 3> values{moments.X, moments.Y, moments.Z};
        for (uint32 axis = 0; axis < 3; ++axis)
        {
            // Each principal axis is an eigenvector of the composed tensor.
            const Vector3d a = axes[axis];
            const InertiaTensor& tensor = composed.InertiaAboutCenterOfMass;
            const Vector3d product{tensor.Xx * a.X + tensor.Xy * a.Y + tensor.Xz * a.Z,
                                   tensor.Xy * a.X + tensor.Yy * a.Y + tensor.Yz * a.Z,
                                   tensor.Xz * a.X + tensor.Yz * a.Y + tensor.Zz * a.Z};
            CHECK(GetMaxDifference(product, a * values[axis]) <= 1.0e-3 * 6500.0);
        }
    }
    SECTION("overlapping parts are additive")
    {
        const StaticArray<MassPart, 2> parts{MassPart{ .Properties = light }, MassPart{ .Properties = light }};
        MassProperties composed;
        REQUIRE(TryComposeMassProperties(BorrowedView<const MassPart>(parts.GetData(), 2), composed) ==
                Status::Success);
        CHECK(composed.MassKilograms == Approx(2.0 * light.MassKilograms).epsilon(RELATIVE));
    }
    SECTION("invalid composition input")
    {
        MassProperties composed{ .MassKilograms = 3.0 };
        CHECK(TryComposeMassProperties({}, composed) == Status::InvalidArgument);
        CHECK(TryComposeMassProperties(BorrowedView<const MassPart>(nullptr, 1), composed) == Status::InvalidArgument);
        Array<MassPart> many;
        REQUIRE(many.TryResize(MAX_MASS_PARTS + 1, MassPart{ .Properties = light }) == true);
        CHECK(TryComposeMassProperties(many.AsView(), composed) == Status::CapacityExceeded);
        const MassPart badPose{ .Properties = light, .Pose = { .OrientationBody = {0.0f, 0.0f, 0.0f, 2.0f} } };
        CHECK(TryComposeMassProperties(BorrowedView<const MassPart>(&badPose, 1), composed) == Status::InvalidArgument);
        const MassPart badMass{ .Properties = { .MassKilograms = -1.0 } };
        CHECK(TryComposeMassProperties(BorrowedView<const MassPart>(&badMass, 1), composed) ==
              Status::InvalidMassProperties);
        CHECK(composed.MassKilograms == 3.0);
    }
}

TEST_CASE("Explicit mass properties reject invalid tensors", "[rigid][mass]")
{
    const MassProperties valid{ .MassKilograms = 2.0, .InertiaAboutCenterOfMass = { .Xx = 1.0, .Yy = 1.0, .Zz = 1.0 } };
    CHECK(TryValidateMassProperties(valid) == Status::Success);

    MassProperties properties = valid;
    properties.MassKilograms = 0.0;
    CHECK(TryValidateMassProperties(properties) == Status::InvalidMassProperties);
    properties.MassKilograms = 2.0 * MAX_MASS_KILOGRAMS;
    CHECK(TryValidateMassProperties(properties) == Status::InvalidMassProperties);
    properties = valid;
    properties.InertiaAboutCenterOfMass.Yy = -1.0;
    CHECK(TryValidateMassProperties(properties) == Status::InvalidMassProperties);
    properties = valid;
    // Violates the triangle inequality: no rigid mass distribution has these moments.
    properties.InertiaAboutCenterOfMass.Xx = 10.0;
    CHECK(TryValidateMassProperties(properties) == Status::InvalidMassProperties);
    properties = valid;
    // Satisfies the triangle inequality but is ill-conditioned.
    properties.InertiaAboutCenterOfMass.Zz = 1.0e-8;
    CHECK(TryValidateMassProperties(properties) == Status::InvalidMassProperties);
    properties = valid;
    // A product of inertia that makes the tensor indefinite.
    properties.InertiaAboutCenterOfMass.Xy = 2.0;
    CHECK(TryValidateMassProperties(properties) == Status::InvalidMassProperties);
    properties = valid;
    properties.InertiaAboutCenterOfMass.Xz = std::numeric_limits<float64>::infinity();
    CHECK(TryValidateMassProperties(properties) == Status::NonFiniteInput);
    properties = valid;
    properties.CenterOfMassLocal = {0.0, 2.0e5, 0.0};
    CHECK(TryValidateMassProperties(properties) == Status::OutOfRange);

    PrincipalMassFrame frame{ .MassKilograms = 5.0 };
    CHECK(TryComputePrincipalMassFrame(properties, frame) == Status::OutOfRange);
    CHECK(frame.MassKilograms == 5.0);
}
