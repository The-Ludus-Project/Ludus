#include <ludus/foundation/containers/borrowed_view.hpp>
#include <ludus/foundation/containers/static_array.hpp>
#include <ludus/foundation/math/vector.hpp>
#include <ludus/physics/collision/contacts.h>
#include <ludus/physics/collision/convex_shape.h>
#include <ludus/physics/collision/geometry_store.h>
#include <ludus/physics/rigid/body.h>
#include <ludus/physics/rigid/commands.h>
#include <ludus/physics/rigid/contacts.h>
#include <ludus/physics/rigid/rigid_world.h>
#include <ludus/physics/rigid/status.h>

namespace
{
/// Generate and export contacts headlessly: a cube sunk one centimeter into a floor keeps a four-point patch, and
/// the same four points are matched to their cached predecessors on the next tick.
bool ExerciseInstalledContacts() noexcept
{
    using namespace ludus::physics;
    using ludus::foundation::BorrowedView;
    using ludus::foundation::StaticArray;
    constexpr ludus::foundation::uint32 CAPACITY = 4;
    constexpr ludus::foundation::uint32 POINT_CAPACITY = 16;
    collision::GeometryStore store;
    collision::ShapeHandle cube;
    collision::ShapeHandle floor;
    if (store.TryInitialize({ .MaxShapes = CAPACITY }) != collision::Status::Success ||
        store.TryAddPrimitive(collision::MakeBoxShape({0.5, 0.5, 0.5}), cube) != collision::Status::Success ||
        store.TryAddPrimitive(collision::MakeBoxShape({4.0, 0.5, 4.0}), floor) != collision::Status::Success)
    {
        return false;
    }
    rigid::RigidWorld world;
    if (world.TryInitialize(store,
                            {
                                .MaxBodies = CAPACITY,
                                .MaxColliders = CAPACITY,
                                .MaxPendingCommands = CAPACITY,
                                .GravityWorld = {},
                                .MaxContactPairs = CAPACITY,
                                .MaxContactPoints = POINT_CAPACITY,
                            }) != rigid::Status::Success)
    {
        return false;
    }
    const rigid::BodyColliderDescription floorCollider{ .Shape = floor };
    const rigid::BodyColliderDescription cubeCollider{ .Shape = cube };
    rigid::BodyHandle floorBody;
    rigid::BodyHandle cubeBody;
    if (world.TryCreateBody(
            {
                .Motion = rigid::MotionType::Static,
                .Pose = { .PositionWorld = {0.0, -0.5, 0.0} },
                .Colliders = BorrowedView<const rigid::BodyColliderDescription>(&floorCollider, 1),
            },
            floorBody) != rigid::Status::Success ||
        world.TryCreateBody(
            {
                .Pose = { .PositionWorld = {0.0, 0.49, 0.0} },
                .Colliders = BorrowedView<const rigid::BodyColliderDescription>(&cubeCollider, 1),
            },
            cubeBody) != rigid::Status::Success)
    {
        return false;
    }
    StaticArray<rigid::ContactPointState, POINT_CAPACITY> points{};
    rigid::ContactExportResult exported;
    if (world.Step().Result != rigid::Status::Success || world.Step().Result != rigid::Status::Success ||
        world.TryExportContacts(BorrowedView<rigid::ContactPointState>(points.GetData(), points.GetSize()), exported) !=
            rigid::Status::Success)
    {
        return false;
    }
    bool isEveryPointResting =
        exported.IsComplete == true && exported.PointCount == collision::MAX_CONTACT_PATCH_POINTS;
    for (ludus::foundation::usize index = 0; index < exported.WrittenCount; ++index)
    {
        isEveryPointResting = isEveryPointResting == true && points[index].SeparationMeters < -0.009 &&
                              points[index].SeparationMeters > -0.011 && points[index].PersistedTickCount == 1;
    }
    const rigid::ContactDiagnostics& diagnostics = world.GetContactDiagnostics();
    return isEveryPointResting == true && diagnostics.PairCount == 1 &&
           diagnostics.PersistedPointCount == collision::MAX_CONTACT_PATCH_POINTS && diagnostics.NewPointCount == 0;
}
} // namespace

/// Step a rigid world headlessly: a falling sphere and an impulse, exported as plain values every tick, then a
/// resting cube's contacts.
bool ExerciseInstalledRigid() noexcept
{
    using namespace ludus::physics;
    using ludus::foundation::BorrowedView;
    constexpr ludus::foundation::uint32 CAPACITY = 4;
    constexpr ludus::foundation::uint32 TICK_COUNT = 60;
    collision::GeometryStore store;
    collision::ShapeHandle sphere;
    if (store.TryInitialize({ .MaxShapes = CAPACITY }) != collision::Status::Success ||
        store.TryAddPrimitive(collision::MakeSphereShape(0.5), sphere) != collision::Status::Success)
    {
        return false;
    }
    rigid::RigidWorld world;
    if (world.TryInitialize(store,
                            {
                                .MaxBodies = CAPACITY,
                                .MaxColliders = CAPACITY,
                                .MaxPendingCommands = CAPACITY,
                            }) != rigid::Status::Success)
    {
        return false;
    }
    const rigid::BodyColliderDescription collider{ .Shape = sphere };
    rigid::BodyHandle body;
    if (world.TryCreateBody(
            {
                .Pose = { .PositionWorld = {0.0, 10.0, 0.0} },
                .Colliders = BorrowedView<const rigid::BodyColliderDescription>(&collider, 1),
            },
            body) != rigid::Status::Success ||
        world.TryEnqueueImpulse(
        {
            .Key =
            {
                .TargetTick = 0,
            },
            .Body = body,
            .LinearImpulseInFrame = {10.0, 0.0, 0.0},
        }) != rigid::Status::Success)
    {
        return false;
    }
    rigid::BodyState state;
    for (ludus::foundation::uint32 tick = 0; tick < TICK_COUNT; ++tick)
    {
        rigid::BodyExportResult exported;
        if (world.Step().Result != rigid::Status::Success ||
            world.TryExportBodyStates(BorrowedView<rigid::BodyState>(&state, 1), exported) != rigid::Status::Success ||
            exported.IsComplete == false)
        {
            return false;
        }
    }
    // One second of free fall from 10 m, with a sideways push.
    return state.Body == body && state.Pose.PositionWorld.Y < 5.2 && state.Pose.PositionWorld.Y > 5.0 &&
           state.LinearVelocityWorld.X > 0.0 && world.GetNextTick() == TICK_COUNT &&
           ExerciseInstalledContacts() == true;
}
