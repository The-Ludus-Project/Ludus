#pragma once

// Shared fixtures for PhysicsRigid tests: a store with primitive shapes and a world over it.

#include <ludus/foundation/base/core.h>

#include <ludus/foundation/containers/borrowed_view.hpp>
#include <ludus/foundation/math/quaternion.hpp>
#include <ludus/foundation/math/scalar.hpp>
#include <ludus/foundation/math/vector.hpp>
#include <ludus/physics/collision/collision_world.h>
#include <ludus/physics/collision/convex_shape.h>
#include <ludus/physics/collision/geometry_store.h>
#include <ludus/physics/rigid/body.h>
#include <ludus/physics/rigid/commands.h>
#include <ludus/physics/rigid/mass_properties.h>
#include <ludus/physics/rigid/rigid_world.h>
#include <ludus/physics/rigid/status.h>

#include <catch2/catch_test_macros.hpp>

namespace ludus::physics::rigid::test
{
namespace math = foundation::math;
using foundation::BorrowedView;
using math::Quaternion;
using math::Vector3d;

inline constexpr uint32 TEST_SHAPE_CAPACITY = 16;
inline constexpr uint32 TEST_BODY_CAPACITY = 16;
inline constexpr uint32 TEST_COLLIDER_CAPACITY = 32;
inline constexpr uint32 TEST_COMMAND_CAPACITY = 32;

inline Quaternion MakeAxisAngle(Vector3d axis, float64 angleRadians)
{
    const float64 length = math::Length(axis);
    const float64 sine = math::Sin(0.5 * angleRadians) / length;
    return {static_cast<float32>(axis.X * sine),
            static_cast<float32>(axis.Y * sine),
            static_cast<float32>(axis.Z * sine),
            static_cast<float32>(math::Cos(0.5 * angleRadians))};
}

inline Vector3d RotateVector(Quaternion orientation, Vector3d value)
{
    const Vector3d axis{static_cast<float64>(orientation.X),
                        static_cast<float64>(orientation.Y),
                        static_cast<float64>(orientation.Z)};
    const float64 w = static_cast<float64>(orientation.W);
    const Vector3d twiceCross = math::Cross(axis, value) * 2.0;
    return value + twiceCross * w + math::Cross(axis, twiceCross);
}

inline float64 GetMaxDifference(Vector3d first, Vector3d second)
{
    const Vector3d difference = first - second;
    return math::Max(math::Abs(difference.X), math::Max(math::Abs(difference.Y), math::Abs(difference.Z)));
}

/// Rotation angle, radians, between two unit quaternions; accurate for small angles.
inline float64 GetAngleBetween(Quaternion first, Quaternion second)
{
    const float64 dot = static_cast<float64>(first.X) * static_cast<float64>(second.X) +
                        static_cast<float64>(first.Y) * static_cast<float64>(second.Y) +
                        static_cast<float64>(first.Z) * static_cast<float64>(second.Z) +
                        static_cast<float64>(first.W) * static_cast<float64>(second.W);
    const float64 sign = dot < 0.0 ? -1.0 : 1.0;
    const float64 dx = static_cast<float64>(first.X) - sign * static_cast<float64>(second.X);
    const float64 dy = static_cast<float64>(first.Y) - sign * static_cast<float64>(second.Y);
    const float64 dz = static_cast<float64>(first.Z) - sign * static_cast<float64>(second.Z);
    const float64 dw = static_cast<float64>(first.W) - sign * static_cast<float64>(second.W);
    const float64 sx = static_cast<float64>(first.X) + sign * static_cast<float64>(second.X);
    const float64 sy = static_cast<float64>(first.Y) + sign * static_cast<float64>(second.Y);
    const float64 sz = static_cast<float64>(first.Z) + sign * static_cast<float64>(second.Z);
    const float64 sw = static_cast<float64>(first.W) + sign * static_cast<float64>(second.W);
    // For unit q and p, |q - p| = 2 sin(theta / 4) and |q + p| = 2 cos(theta / 4).
    return 4.0 * math::Atan2(math::Sqrt(dx * dx + dy * dy + dz * dz + dw * dw),
                             math::Sqrt(sx * sx + sy * sy + sz * sz + sw * sw));
}

/// A store with a 0.5 m sphere, a 1 x 2 x 3 m box and a capsule, and a world over it.
struct WorldFixture final
{
    collision::GeometryStore Store;
    collision::ShapeHandle Sphere{};
    collision::ShapeHandle Box{};
    collision::ShapeHandle Capsule{};
    RigidWorld World;

    explicit WorldFixture(const RigidWorldSettings& settingsOverride = {})
    {
        REQUIRE(Store.TryInitialize({ .MaxShapes = TEST_SHAPE_CAPACITY }) == collision::Status::Success);
        REQUIRE(Store.TryAddPrimitive(collision::MakeSphereShape(0.5), Sphere) == collision::Status::Success);
        REQUIRE(Store.TryAddPrimitive(collision::MakeBoxShape({0.5, 1.0, 1.5}), Box) == collision::Status::Success);
        REQUIRE(Store.TryAddPrimitive(collision::MakeCapsuleShape(0.5, 0.25), Capsule) == collision::Status::Success);
        RigidWorldSettings settings = settingsOverride;
        settings.MaxBodies = settings.MaxBodies == 0 ? TEST_BODY_CAPACITY : settings.MaxBodies;
        settings.MaxColliders = settings.MaxColliders == 0 ? TEST_COLLIDER_CAPACITY : settings.MaxColliders;
        settings.MaxPendingCommands =
            settings.MaxPendingCommands == 0 ? TEST_COMMAND_CAPACITY : settings.MaxPendingCommands;
        REQUIRE(World.TryInitialize(Store, settings) == Status::Success);
    }

    BodyHandle CreateDynamic(collision::ShapeHandle shape,
                             Vector3d positionWorld,
                             Quaternion orientationWorld = {},
                             float64 densityKilogramsPerCubicMeter = DEFAULT_DENSITY_KILOGRAMS_PER_CUBIC_METER)
    {
        const BodyColliderDescription collider
        {
            .Shape = shape,
            .DensityKilogramsPerCubicMeter = densityKilogramsPerCubicMeter,
        };
        BodyHandle body;
        REQUIRE(World.TryCreateBody(
                    {
                        .Pose = { .PositionWorld = positionWorld, .OrientationWorld = orientationWorld },
                        .Colliders = BorrowedView<const BodyColliderDescription>(&collider, 1),
                    },
                    body) == Status::Success);
        return body;
    }

    BodyHandle CreateKinematic(Vector3d positionWorld, Quaternion orientationWorld = {})
    {
        const BodyColliderDescription collider{ .Shape = Box };
        BodyHandle body;
        REQUIRE(World.TryCreateBody(
                    {
                        .Motion = MotionType::Kinematic,
                        .Pose = { .PositionWorld = positionWorld, .OrientationWorld = orientationWorld },
                        .Colliders = BorrowedView<const BodyColliderDescription>(&collider, 1),
                    },
                    body) == Status::Success);
        return body;
    }

    [[nodiscard]] BodyState GetState(BodyHandle body) const
    {
        BodyState state;
        REQUIRE(World.TryGetBodyState(body, state) == Status::Success);
        return state;
    }

    void StepTicks(uint32 tickCount)
    {
        for (uint32 tick = 0; tick < tickCount; ++tick)
        {
            REQUIRE(World.Step().Result == Status::Success);
        }
    }
};
} // namespace ludus::physics::rigid::test
