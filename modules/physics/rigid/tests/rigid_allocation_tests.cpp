// Allocation injection for every RigidWorld creation path, and proof that body creation, commands, an active tick
// and state export never allocate. This executable replaces the global allocation functions, which
// FoundationMemory's container domain reaches through the aligned nothrow operator new; sanitizer builds use the
// ordinary suite because sanitizer runtimes replace the same functions.

#include <ludus/foundation/base/core.h>

#include <ludus/foundation/containers/array.hpp>
#include <ludus/foundation/containers/borrowed_view.hpp>
#include <ludus/foundation/containers/static_array.hpp>
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

#include <cstddef>
#include <cstdlib>
#include <new>

namespace
{
using ludus::foundation::int64;

struct AllocationCounters final
{
    int64 Allocations = 0;
    int64 Frees = 0;
};
AllocationCounters gCounters;
bool isTrackingAllocations = false;
int64 gFailAfter = -1;

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

using namespace ludus::physics::rigid;
namespace collision = ludus::physics::collision;
using ludus::foundation::Array;
using ludus::foundation::BorrowedView;
using ludus::foundation::StaticArray;
using ludus::foundation::math::Vector3;
using ludus::foundation::math::Vector3d;

namespace
{
constexpr uint32 CAPACITY = 8;

collision::ConvexHull MakeTetrahedronHull()
{
    const StaticArray<Vector3, 4> points{Vector3{0.0f, 0.0f, 0.0f},
                                         Vector3{1.0f, 0.0f, 0.0f},
                                         Vector3{0.0f, 1.0f, 0.0f},
                                         Vector3{0.0f, 0.0f, 1.0f}};
    collision::ConvexHull hull;
    REQUIRE(collision::ConvexHull::TryCook({ .PointsShape = BorrowedView<const Vector3>(points.GetData(), 4) }, hull)
                .Result == collision::Status::Success);
    return hull;
}
} // namespace

TEST_CASE("Rigid world initialization fails every allocation without installing state", "[rigid][alloc]")
{
    collision::GeometryStore store;
    REQUIRE(store.TryInitialize({ .MaxShapes = CAPACITY }) == collision::Status::Success);
    const RigidWorldSettings settings
    {
        .MaxBodies = CAPACITY,
        .MaxColliders = CAPACITY,
        .MaxPendingCommands = CAPACITY,
    };
    RigidWorld world;
    int64 failedAllocation = 0;
    for (;; ++failedAllocation)
    {
        gFailAfter = failedAllocation;
        const Status status = world.TryInitialize(store, settings);
        gFailAfter = -1;
        if (status == Status::Success)
        {
            break;
        }
        REQUIRE(status == Status::AllocationFailure);
        REQUIRE(world.IsInitialized() == false);
        REQUIRE(world.GetCollisionWorld().IsInitialized() == false);
    }
    // Five rigid tables and the collision world's four.
    CHECK(failedAllocation >= 5);
    CHECK(world.GetPhase() == WorldPhase::Idle);
}

TEST_CASE("Bodies, commands, active ticks and export allocate nothing", "[rigid][alloc]")
{
    collision::GeometryStore store;
    REQUIRE(store.TryInitialize({ .MaxShapes = CAPACITY }) == collision::Status::Success);
    collision::ConvexHull hull = MakeTetrahedronHull();
    collision::ShapeHandle box;
    collision::ShapeHandle hullShape;
    REQUIRE(store.TryAddPrimitive(collision::MakeBoxShape({0.5, 1.0, 1.5}), box) == collision::Status::Success);
    REQUIRE(store.TryAddConvexHull(ludus::foundation::Move(hull), hullShape) == collision::Status::Success);
    RigidWorld world;
    REQUIRE(world.TryInitialize(store,
                                {
                                    .MaxBodies = CAPACITY,
                                    .MaxColliders = CAPACITY,
                                    .MaxPendingCommands = CAPACITY,
                                }) == Status::Success);
    const StaticArray<BodyColliderDescription, 2> colliders{
        BodyColliderDescription{ .Shape = box },
        BodyColliderDescription{ .Shape = hullShape, .Pose = { .PositionBody = {1.0, 0.0, 0.0} } },
    };
    StaticArray<BodyState, CAPACITY> states{};

    gCounters = {};
    isTrackingAllocations = true;
    BodyHandle dynamic;
    const Status createdDynamic = world.TryCreateBody(
        {
            .AngularVelocityWorld = {0.1, 5.0, 0.1},
            .Colliders = BorrowedView<const BodyColliderDescription>(colliders.GetData(), 2),
        },
        dynamic);
    BodyHandle kinematic;
    const Status createdKinematic = world.TryCreateBody(
        {
            .Motion = MotionType::Kinematic,
            .Colliders = BorrowedView<const BodyColliderDescription>(colliders.GetData(), 1),
        },
        kinematic);
    bool isEveryStepSuccessful = true;
    for (uint32 tick = 0; tick < CAPACITY; ++tick)
    {
        const Status force =
            world.TryEnqueueForce({ .Key = { .TargetTick = tick }, .Body = dynamic, .ForceInFrame = {1.0, 0.0, 0.0} });
        const Status impulse = world.TryEnqueueImpulse(
        {
            .Key = { .TargetTick = tick, .Sequence = 1 },
            .Body = dynamic,
            .LinearImpulseInFrame = {0.0, 1.0, 0.0},
            .HasApplicationPoint = true,
            .ApplicationPointInFrame = {0.0, 0.0, 1.0},
        });
        const Status target = world.TrySetKinematicTarget(
        {
            .Key = { .TargetTick = tick, .Sequence = 2 },
            .Body = kinematic,
            .TargetPose = { .PositionWorld = {0.0, 0.1 * tick, 0.0} },
        });
        const StepResult step = world.Step();
        BodyExportResult exported;
        const Status exportStatus =
            world.TryExportBodyStates(BorrowedView<BodyState>(states.GetData(), CAPACITY), exported);
        isEveryStepSuccessful = isEveryStepSuccessful == true && force == Status::Success &&
                                impulse == Status::Success && target == Status::Success &&
                                step.Result == Status::Success && exportStatus == Status::Success &&
                                exported.IsComplete == true;
    }
    const Status teleported = world.TryTeleportBody(dynamic, { .PositionWorld = {0.0, 5.0, 0.0} });
    const Status destroyed = world.TryDestroyBody(kinematic);
    isTrackingAllocations = false;

    REQUIRE(createdDynamic == Status::Success);
    REQUIRE(createdKinematic == Status::Success);
    CHECK(isEveryStepSuccessful == true);
    CHECK(teleported == Status::Success);
    CHECK(destroyed == Status::Success);
    CHECK(gCounters.Allocations == 0);
    CHECK(gCounters.Frees == 0);
}
