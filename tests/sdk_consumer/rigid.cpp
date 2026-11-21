#include <ludus/foundation/containers/borrowed_view.hpp>
#include <ludus/foundation/math/vector.hpp>
#include <ludus/physics/collision/convex_shape.h>
#include <ludus/physics/collision/geometry_store.h>
#include <ludus/physics/rigid/body.h>
#include <ludus/physics/rigid/commands.h>
#include <ludus/physics/rigid/rigid_world.h>
#include <ludus/physics/rigid/status.h>

/// Step a rigid world headlessly: a falling sphere and an impulse, exported as plain values every tick.
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
           state.LinearVelocityWorld.X > 0.0 && world.GetNextTick() == TICK_COUNT;
}
