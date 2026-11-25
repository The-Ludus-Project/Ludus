// Contacts of a RigidWorld: which pairs make contacts, an all-pairs oracle, resting patches, sliding across mesh
// seams, capacity exhaustion before any state changes, and observable cache reuse and retirement.

#include "rigid_test_support.h"

#include <ludus/foundation/base/core.h>

#include <ludus/foundation/containers/array.hpp>
#include <ludus/foundation/containers/borrowed_view.hpp>
#include <ludus/foundation/containers/static_array.hpp>
#include <ludus/foundation/math/quaternion.hpp>
#include <ludus/foundation/math/random.hpp>
#include <ludus/foundation/math/scalar.hpp>
#include <ludus/foundation/math/vector.hpp>
#include <ludus/physics/collision/collision_world.h>
#include <ludus/physics/collision/contacts.h>
#include <ludus/physics/collision/convex_shape.h>
#include <ludus/physics/collision/geometry_store.h>
#include <ludus/physics/collision/triangle_mesh.h>
#include <ludus/physics/rigid/body.h>
#include <ludus/physics/rigid/commands.h>
#include <ludus/physics/rigid/contacts.h>
#include <ludus/physics/rigid/rigid_world.h>
#include <ludus/physics/rigid/status.h>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <limits>

using namespace ludus::physics::rigid;
using namespace ludus::physics::rigid::test;
namespace collision = ludus::physics::collision;
using ludus::foundation::Array;
using ludus::foundation::StaticArray;
using ludus::foundation::math::Vector3;

namespace
{
constexpr float64 EXACT_TOLERANCE = 1.0e-9;
constexpr uint32 SHAPE_CAPACITY = 16;
constexpr uint32 BODY_CAPACITY = 96;
constexpr uint32 COLLIDER_CAPACITY = 192;
constexpr uint32 PAIR_CAPACITY = 2048;
constexpr uint32 POINT_CAPACITY = 8192;
constexpr float64 CUBE_HALF = 0.5;
constexpr float64 OVERLAP = 0.01;
/// Height of a unit cube's centre that sinks it OVERLAP meters into a floor whose top is at y = 0.
constexpr float64 RESTING_HEIGHT = CUBE_HALF - OVERLAP;

/// A world without gravity, so bodies stay where a test puts them, and shapes to put in it. The floor shapes have
/// their top at y = 0 when a floor body stands at the origin.
struct ContactFixture final
{
    collision::GeometryStore Store;
    collision::ShapeHandle Cube{};
    collision::ShapeHandle Sphere{};
    collision::ShapeHandle Capsule{};
    collision::ShapeHandle FloorBox{};
    collision::ShapeHandle FloorMesh{};
    collision::ShapeHandle RoughFloorMesh{};
    RigidWorld World;

    explicit ContactFixture(uint32 maxContactPairs = PAIR_CAPACITY, uint32 maxContactPoints = POINT_CAPACITY)
    {
        REQUIRE(Store.TryInitialize({ .MaxShapes = SHAPE_CAPACITY }) == collision::Status::Success);
        REQUIRE(Store.TryAddPrimitive(collision::MakeBoxShape({CUBE_HALF, CUBE_HALF, CUBE_HALF}), Cube) ==
                collision::Status::Success);
        REQUIRE(Store.TryAddPrimitive(collision::MakeSphereShape(0.5), Sphere) == collision::Status::Success);
        REQUIRE(Store.TryAddPrimitive(collision::MakeCapsuleShape(0.5, 0.25), Capsule) == collision::Status::Success);
        REQUIRE(Store.TryAddPrimitive(collision::MakeBoxShape({4.0, 0.5, 4.0}), FloorBox) ==
                collision::Status::Success);
        FloorMesh = AddGridFloor({});
        RoughFloorMesh = AddGridFloor({ .EdgePolicy = collision::InternalEdgePolicy::KeepAll });
        REQUIRE(World.TryInitialize(Store,
                                    {
                                        .MaxBodies = BODY_CAPACITY,
                                        .MaxColliders = COLLIDER_CAPACITY,
                                        .MaxPendingCommands = TEST_COMMAND_CAPACITY,
                                        .GravityWorld = {},
                                        .MaxContactPairs = maxContactPairs,
                                        .MaxContactPoints = maxContactPoints,
                                    }) == Status::Success);
    }

    /// A flat grid of one-meter CELLS over [-4, 4] in x and z at y = 0, facing +y, two triangles per cell.
    collision::ShapeHandle AddGridFloor(const collision::TriangleMeshCookSettings& settings)
    {
        constexpr uint32 CELLS = 8;
        constexpr float32 HALF = 4.0f;
        Array<Vector3> positions;
        Array<uint32> indices;
        for (uint32 z = 0; z <= CELLS; ++z)
        {
            for (uint32 x = 0; x <= CELLS; ++x)
            {
                positions.Add({static_cast<float32>(x) - HALF, 0.0f, static_cast<float32>(z) - HALF});
            }
        }
        for (uint32 z = 0; z < CELLS; ++z)
        {
            for (uint32 x = 0; x < CELLS; ++x)
            {
                const uint32 corner = z * (CELLS + 1) + x;
                for (const uint32 index :
                     {corner, corner + CELLS + 1, corner + CELLS + 2, corner, corner + CELLS + 2, corner + 1})
                {
                    indices.Add(index);
                }
            }
        }
        collision::TriangleMesh mesh;
        REQUIRE(collision::TriangleMesh::TryCook(
                    {
                        .PositionsMesh = BorrowedView<const Vector3>(positions.GetData(), positions.GetSize()),
                        .Indices = BorrowedView<const uint32>(indices.GetData(), indices.GetSize()),
                    },
                    settings,
                    mesh)
                    .Result == collision::Status::Success);
        collision::ShapeHandle shape;
        REQUIRE(Store.TryAddTriangleMesh(ludus::foundation::Move(mesh), shape) == collision::Status::Success);
        return shape;
    }

    BodyHandle
    Create(MotionType motion, const BodyColliderDescription& collider, Vector3d position, Quaternion orientation = {})
    {
        BodyHandle body;
        REQUIRE(World.TryCreateBody(
                    {
                        .Motion = motion,
                        .Pose = { .PositionWorld = position, .OrientationWorld = orientation },
                        .Colliders = BorrowedView<const BodyColliderDescription>(&collider, 1),
                    },
                    body) == Status::Success);
        return body;
    }

    BodyHandle Create(MotionType motion, collision::ShapeHandle shape, Vector3d position, Quaternion orientation = {})
    {
        // Triangle meshes are no mass source; static bodies ignore the flag.
        return Create(motion,
                      BodyColliderDescription{ .Shape = shape, .ShouldContributeMass = motion == MotionType::Dynamic },
                      position,
                      orientation);
    }

    /// A static floor box whose top face is the plane y = 0.
    BodyHandle CreateFloorBox()
    {
        return Create(MotionType::Static, FloorBox, {0.0, -0.5, 0.0});
    }

    [[nodiscard]] collision::ColliderHandle GetCollider(BodyHandle body) const
    {
        collision::ColliderHandle collider;
        usize count = 0;
        REQUIRE(World.TryGetBodyColliders(body, BorrowedView<collision::ColliderHandle>(&collider, 1), count) ==
                Status::Success);
        REQUIRE(count == 1);
        return collider;
    }

    [[nodiscard]] Array<ContactPointState> ExportContacts() const
    {
        ContactExportResult counted;
        REQUIRE(World.TryExportContacts({}, counted) == Status::Success);
        Array<ContactPointState> points;
        REQUIRE(points.TryResize(counted.PointCount) == true);
        ContactExportResult exported;
        REQUIRE(World.TryExportContacts(BorrowedView<ContactPointState>(points.GetData(), points.GetSize()),
                                        exported) == Status::Success);
        REQUIRE(exported.IsComplete == true);
        REQUIRE(exported.PointCount == counted.PointCount);
        return points;
    }

    [[nodiscard]] BodyState GetState(BodyHandle body) const
    {
        BodyState state;
        REQUIRE(World.TryGetBodyState(body, state) == Status::Success);
        return state;
    }

    void StepOrFail()
    {
        const StepResult step = World.Step();
        REQUIRE(step.Result == Status::Success);
        REQUIRE(step.ExhaustedTable == ContactTable::None);
    }
};

usize GetRetiredTotal(const ContactDiagnostics& diagnostics)
{
    usize total = 0;
    for (const usize count : diagnostics.RetiredPointCounts)
    {
        total += count;
    }
    return total;
}

void RequireSameState(const BodyState& first, const BodyState& second)
{
    REQUIRE(first.Pose.PositionWorld == second.Pose.PositionWorld);
    REQUIRE(first.Pose.OrientationWorld.X == second.Pose.OrientationWorld.X);
    REQUIRE(first.Pose.OrientationWorld.Y == second.Pose.OrientationWorld.Y);
    REQUIRE(first.Pose.OrientationWorld.Z == second.Pose.OrientationWorld.Z);
    REQUIRE(first.Pose.OrientationWorld.W == second.Pose.OrientationWorld.W);
    REQUIRE(first.PreviousPose.PositionWorld == second.PreviousPose.PositionWorld);
    REQUIRE(first.CenterOfMassWorld == second.CenterOfMassWorld);
    REQUIRE(first.LinearVelocityWorld == second.LinearVelocityWorld);
    REQUIRE(first.AngularVelocityWorld == second.AngularVelocityWorld);
}
} // namespace

TEST_CASE("A box resting on a floor keeps a four-point patch that persists", "[rigid][contact]")
{
    ContactFixture fixture;
    const bool isMeshFloor = GENERATE(false, true);
    INFO("the floor is a " << (isMeshFloor == true ? "triangle mesh" : "box"));
    const BodyHandle floor =
        isMeshFloor == true ? fixture.Create(MotionType::Static, fixture.FloorMesh, {}) : fixture.CreateFloorBox();
    const BodyHandle cube = fixture.Create(MotionType::Dynamic, fixture.Cube, {0.3, RESTING_HEIGHT, -0.2});
    constexpr uint32 TICK_COUNT = 3;
    for (uint32 tick = 0; tick < TICK_COUNT; ++tick)
    {
        fixture.StepOrFail();
        const Array<ContactPointState> points = fixture.ExportContacts();
        REQUIRE(points.GetSize() == collision::MAX_CONTACT_PATCH_POINTS);
        // The first collider is the one with the lower slot; the normal points from it toward the second.
        const float64 up = points[0].FirstBody == floor ? 1.0 : -1.0;
        for (const ContactPointState& point : points)
        {
            REQUIRE((point.FirstBody == floor ? point.SecondBody == cube
                                              : (point.FirstBody == cube && point.SecondBody == floor)));
            REQUIRE(point.FirstCollider.Slot < point.SecondCollider.Slot);
            REQUIRE(point.PatchIndex == 0);
            REQUIRE(GetMaxDifference(point.NormalWorld, {0.0, up, 0.0}) <= EXACT_TOLERANCE);
            REQUIRE(math::Abs(point.SeparationMeters + OVERLAP) <= EXACT_TOLERANCE);
            REQUIRE(math::Abs(point.PositionWorld.Y + 0.5 * OVERLAP) <= EXACT_TOLERANCE);
            REQUIRE(math::Abs(math::Abs(point.PositionWorld.X - 0.3) - CUBE_HALF) <= EXACT_TOLERANCE);
            REQUIRE(math::Abs(math::Abs(point.PositionWorld.Z + 0.2) - CUBE_HALF) <= EXACT_TOLERANCE);
            REQUIRE((point.TriangleIndex != collision::INVALID_TRIANGLE_INDEX) == isMeshFloor);
            REQUIRE(point.PersistedTickCount == tick);
            REQUIRE(point.IsApproximate == false);
        }
        // All four corners, not one of them four times.
        for (usize first = 0; first < points.GetSize(); ++first)
        {
            for (usize second = first + 1; second < points.GetSize(); ++second)
            {
                REQUIRE(GetMaxDifference(points[first].PositionWorld, points[second].PositionWorld) > CUBE_HALF);
            }
        }
        const ContactDiagnostics& diagnostics = fixture.World.GetContactDiagnostics();
        REQUIRE(diagnostics.HasDetected == true);
        REQUIRE(diagnostics.Tick == tick);
        REQUIRE(diagnostics.TestedPairCount == 1);
        REQUIRE(diagnostics.PairCount == 1);
        REQUIRE(diagnostics.PatchCount == 1);
        REQUIRE(diagnostics.PointCount == collision::MAX_CONTACT_PATCH_POINTS);
        REQUIRE(diagnostics.GeneratedPointCount >= diagnostics.PointCount);
        REQUIRE((diagnostics.TriangleTestCount > 0) == isMeshFloor);
        REQUIRE(diagnostics.PersistedPointCount == (tick == 0 ? 0U : collision::MAX_CONTACT_PATCH_POINTS));
        REQUIRE(diagnostics.NewPointCount == (tick == 0 ? collision::MAX_CONTACT_PATCH_POINTS : 0U));
        REQUIRE(GetRetiredTotal(diagnostics) == 0);
        REQUIRE(diagnostics.ExhaustedTable == ContactTable::None);
        ContactExportResult exported;
        REQUIRE(fixture.World.TryExportContacts({}, exported) == Status::Success);
        REQUIRE(exported.Tick == tick);
        REQUIRE(exported.PairCount == 1);
        REQUIRE(exported.PatchCount == 1);
    }
    // Contacts are generated, not solved: the cube has not moved.
    REQUIRE(fixture.World.GetNextTick() == TICK_COUNT);
    REQUIRE(GetMaxDifference(fixture.GetState(cube).Pose.PositionWorld, {0.3, RESTING_HEIGHT, -0.2}) <=
            EXACT_TOLERANCE);
}

TEST_CASE("Only pairs with a dynamic body on two different bodies make contacts", "[rigid][contact]")
{
    ContactFixture fixture;
    // A static, a kinematic and another kinematic body, all overlapping at the origin.
    fixture.Create(MotionType::Static, fixture.Cube, {});
    fixture.Create(MotionType::Kinematic, fixture.Cube, {0.2, 0.0, 0.0});
    fixture.Create(MotionType::Kinematic, fixture.Cube, {0.0, 0.2, 0.0});
    fixture.StepOrFail();
    REQUIRE(fixture.World.GetContactDiagnostics().HasDetected == true);
    REQUIRE(fixture.World.GetContactDiagnostics().CandidatePairCount == 0);
    REQUIRE(fixture.World.GetContactDiagnostics().TestedPairCount == 0);
    REQUIRE(fixture.World.GetContactDiagnostics().PairCount == 0);
    REQUIRE(fixture.ExportContacts().IsEmpty() == true);

    SECTION("a dynamic body contacts each of them")
    {
        fixture.Create(MotionType::Dynamic, fixture.Cube, {0.0, 0.0, 0.2});
        fixture.StepOrFail();
        REQUIRE(fixture.World.GetContactDiagnostics().TestedPairCount == 3);
        REQUIRE(fixture.World.GetContactDiagnostics().PairCount == 3);
    }
    SECTION("two dynamic bodies make one pair, not one from each side")
    {
        const BodyHandle first = fixture.Create(MotionType::Dynamic, fixture.Sphere, {5.0, 0.0, 0.0});
        const BodyHandle second = fixture.Create(MotionType::Dynamic, fixture.Sphere, {5.9, 0.0, 0.0});
        fixture.StepOrFail();
        const ContactDiagnostics& diagnostics = fixture.World.GetContactDiagnostics();
        REQUIRE(diagnostics.CandidatePairCount == 2);
        REQUIRE(diagnostics.TestedPairCount == 1);
        REQUIRE(diagnostics.PairCount == 1);
        const Array<ContactPointState> points = fixture.ExportContacts();
        REQUIRE(points.GetSize() == 1);
        REQUIRE(((points[0].FirstBody == first && points[0].SecondBody == second) ||
                 (points[0].FirstBody == second && points[0].SecondBody == first)));
        REQUIRE(math::Abs(points[0].SeparationMeters + 0.1) <= EXACT_TOLERANCE);
    }
    SECTION("colliders of one body never contact each other")
    {
        const StaticArray<BodyColliderDescription, 2> colliders{
            BodyColliderDescription{ .Shape = fixture.Sphere, .Pose = { .PositionBody = {5.0, 0.0, 0.0} } },
            BodyColliderDescription{ .Shape = fixture.Sphere, .Pose = { .PositionBody = {5.5, 0.0, 0.0} } }};
        BodyHandle body;
        REQUIRE(fixture.World.TryCreateBody(
                    { .Colliders = BorrowedView<const BodyColliderDescription>(colliders.GetData(), 2) },
                    body) == Status::Success);
        fixture.StepOrFail();
        REQUIRE(fixture.World.GetContactDiagnostics().CandidatePairCount == 2);
        REQUIRE(fixture.World.GetContactDiagnostics().TestedPairCount == 0);
    }
    SECTION("sensors, disabled colliders and rejected categories make no contacts")
    {
        fixture.Create(MotionType::Dynamic, fixture.Sphere, {5.0, 0.0, 0.0});
        fixture.Create(
            MotionType::Static,
            BodyColliderDescription{ .Shape = fixture.Sphere, .Participation = { .IsSolid = false, .IsSensor = true } },
            {5.3, 0.0, 0.0});
        fixture.Create(MotionType::Static,
                       BodyColliderDescription{ .Shape = fixture.Sphere, .IsEnabled = false },
                       {5.0, 0.3, 0.0});
        fixture.Create(
            MotionType::Static,
            BodyColliderDescription{ .Shape = fixture.Sphere, .Filter = { .CategoryBits = 2, .CollisionMask = 2 } },
            {5.0, 0.0, 0.3});
        fixture.StepOrFail();
        REQUIRE(fixture.World.GetContactDiagnostics().CandidatePairCount == 0);
        REQUIRE(fixture.World.GetContactDiagnostics().PairCount == 0);
    }
}

TEST_CASE("An all-pairs oracle finds the same contact pairs as the world", "[rigid][contact][oracle]")
{
    constexpr uint32 BODY_COUNT = 72;
    constexpr float32 EXTENT = 2.5f;
    constexpr uint32 CATEGORY_COUNT = 3;
    ContactFixture fixture;
    math::RandomStream random;
    REQUIRE(random.TryReseed(2027, 1) == true);
    const auto nextInRange = [&](float32 minimum, float32 maximum) {
        return minimum + (maximum - minimum) * random.NextFloat01();
    };
    const auto nextPosition = [&]() {
        return Vector3d{static_cast<float64>(nextInRange(-EXTENT, EXTENT)),
                        static_cast<float64>(nextInRange(-EXTENT, EXTENT)),
                        static_cast<float64>(nextInRange(-EXTENT, EXTENT))};
    };

    /// What the test knows about one collider, independently of the world.
    struct Known final
    {
        collision::ColliderHandle Collider{};
        uint32 Body = 0;
        MotionType Motion = MotionType::Static;
        collision::CollisionFilter Filter{};
        bool IsSolid = true;
        bool IsEnabled = true;
    };
    Array<Known> known;
    Array<BodyHandle> bodies;
    const StaticArray<collision::ShapeHandle, 3> shapes{fixture.Cube, fixture.Sphere, fixture.Capsule};
    for (uint32 bodyIndex = 0; bodyIndex < BODY_COUNT; ++bodyIndex)
    {
        const float32 motionDraw = random.NextFloat01();
        const MotionType motion =
            motionDraw < 0.5f ? MotionType::Dynamic : (motionDraw < 0.75f ? MotionType::Kinematic : MotionType::Static);
        StaticArray<BodyColliderDescription, 2> colliders{};
        const uint32 colliderCount = random.NextFloat01() < 0.3f ? 2 : 1;
        for (uint32 index = 0; index < colliderCount; ++index)
        {
            const uint32 category =
                static_cast<uint32>(nextInRange(0.0f, static_cast<float32>(CATEGORY_COUNT))) % CATEGORY_COUNT;
            const uint32 rejected = static_cast<uint32>(nextInRange(0.0f, static_cast<float32>(CATEGORY_COUNT + 1)));
            // A dynamic body needs a mass source, so its first collider is never a sensor.
            const bool isSensor = random.NextFloat01() < 0.1f && (motion != MotionType::Dynamic || index > 0);
            colliders[index] =
            {
                .Shape = shapes[static_cast<uint32>(nextInRange(0.0f, 3.0f)) % 3],
                .Pose =
                {
                    .PositionBody = {static_cast<float64>(index) * 0.4, 0.0, 0.0},
                    .OrientationBody =
                        MakeAxisAngle({0.3, 1.0, 0.2}, static_cast<float64>(nextInRange(0.0f, 3.0f))),
                },
                .Participation = { .IsSolid = isSensor == false, .IsSensor = isSensor },
                .Filter =
                {
                    .CategoryBits = 1U << category,
                    .CollisionMask =
                        rejected < CATEGORY_COUNT ? ~(1U << rejected) : collision::ALL_COLLISION_CATEGORIES,
                },
                .IsEnabled = random.NextFloat01() >= 0.1f,
            };
        }
        BodyHandle body;
        REQUIRE(fixture.World.TryCreateBody(
                    {
                        .Motion = motion,
                        .Pose =
                        {
                            .PositionWorld = nextPosition(),
                            .OrientationWorld =
                                MakeAxisAngle({1.0, 0.2, 0.5}, static_cast<float64>(nextInRange(0.0f, 3.0f))),
                        },
                        .Colliders = BorrowedView<const BodyColliderDescription>(colliders.GetData(), colliderCount),
                    },
                    body) == Status::Success);
        bodies.Add(body);
        StaticArray<collision::ColliderHandle, 2> handles{};
        usize handleCount = 0;
        REQUIRE(fixture.World.TryGetBodyColliders(body,
                                                  BorrowedView<collision::ColliderHandle>(handles.GetData(), 2),
                                                  handleCount) == Status::Success);
        for (uint32 index = 0; index < colliderCount; ++index)
        {
            known.Add(
            {
                .Collider = handles[index],
                .Body = bodyIndex,
                .Motion = motion,
                .Filter = colliders[index].Filter,
                .IsSolid = colliders[index].Participation.IsSolid,
                .IsEnabled = colliders[index].IsEnabled,
            });
        }
    }
    constexpr uint32 ROUND_COUNT = 3;
    for (uint32 round = 0; round < ROUND_COUNT; ++round)
    {
        INFO("round " << round);
        // The oracle: every pair of colliders, the documented rule, and the narrowphase asked directly.
        Array<ContactPointState> expected;
        usize expectedPairCount = 0;
        {
            collision::CollisionReadView view;
            REQUIRE(fixture.World.TryAcquireCollisionReadView(view) == Status::Success);
            for (usize first = 0; first < known.GetSize(); ++first)
            {
                for (usize second = 0; second < known.GetSize(); ++second)
                {
                    const Known& low = known[first];
                    const Known& high = known[second];
                    if (low.Collider.Slot >= high.Collider.Slot || low.Body == high.Body ||
                        (low.Motion != MotionType::Dynamic && high.Motion != MotionType::Dynamic) ||
                        low.IsSolid == false || high.IsSolid == false || low.IsEnabled == false ||
                        high.IsEnabled == false || collision::ShouldCollide(low.Filter, high.Filter) == false)
                    {
                        continue;
                    }
                    collision::ContactPairContacts contacts;
                    REQUIRE(view.GenerateContacts(low.Collider, high.Collider, {}, contacts) ==
                            collision::Status::Success);
                    expectedPairCount += contacts.PointCount > 0 ? 1 : 0;
                    for (uint32 patch = 0; patch < contacts.PatchCount; ++patch)
                    {
                        for (uint32 point = 0; point < contacts.Patches[patch].PointCount; ++point)
                        {
                            const collision::ContactPoint& contact =
                                contacts.Points[contacts.Patches[patch].FirstPoint + point];
                            expected.Add(
                            {
                                .FirstCollider = low.Collider,
                                .SecondCollider = high.Collider,
                                .NormalWorld = contacts.Patches[patch].NormalWorld,
                                .PositionWorld = contact.PositionWorld,
                                .SeparationMeters = contact.SeparationMeters,
                                .FirstFeature = contact.FirstFeature,
                                .SecondFeature = contact.SecondFeature,
                            });
                        }
                    }
                }
            }
        }
        fixture.StepOrFail();
        const Array<ContactPointState> points = fixture.ExportContacts();
        REQUIRE(fixture.World.GetContactDiagnostics().PairCount == expectedPairCount);
        REQUIRE(points.GetSize() == expected.GetSize());
        // The scene must exercise the rule: many pairs, and far fewer than all of them.
        REQUIRE(expectedPairCount > BODY_COUNT / 2);
        REQUIRE(expectedPairCount < known.GetSize() * known.GetSize() / 8);
        // The export is sorted by pair; so is the oracle's double loop only within a first collider. Match by value.
        for (const ContactPointState& point : points)
        {
            bool isExpected = false;
            for (const ContactPointState& candidate : expected)
            {
                isExpected =
                    isExpected == true ||
                    (candidate.FirstCollider == point.FirstCollider &&
                     candidate.SecondCollider == point.SecondCollider && candidate.FirstFeature == point.FirstFeature &&
                     candidate.SecondFeature == point.SecondFeature && candidate.PositionWorld == point.PositionWorld &&
                     candidate.NormalWorld == point.NormalWorld &&
                     candidate.SeparationMeters == point.SeparationMeters);
            }
            REQUIRE(isExpected == true);
        }
        // Move a third of the bodies so the next round runs against an updated hierarchy.
        for (uint32 bodyIndex = round; bodyIndex < BODY_COUNT; bodyIndex += 3)
        {
            BodyState state;
            REQUIRE(fixture.World.TryGetBodyState(bodies[bodyIndex], state) == Status::Success);
            REQUIRE(fixture.World.TryTeleportBody(
                        bodies[bodyIndex],
                        { .PositionWorld = nextPosition(), .OrientationWorld = state.Pose.OrientationWorld }) ==
                    Status::Success);
        }
    }
}

TEST_CASE("A sliding capsule crosses internal triangle edges without catching on them", "[rigid][contact][mesh]")
{
    // A capsule lies on a flat floor of one-meter CELLS, sunk one centimeter, and slides diagonally across cell
    // edges and cell diagonals. Every contact normal must stay the floor's normal and every depth the same: a
    // SIDEWAYS normal is what catches a sliding body on a seam. The same floor cooked without edge classification
    // is the control that shows the test can fail.
    const bool isClassified = GENERATE(true, false);
    INFO("the floor is cooked " << (isClassified == true ? "with" : "without") << " edge classification");
    ContactFixture fixture;
    const BodyHandle floor =
        fixture.Create(MotionType::Static, isClassified == true ? fixture.FloorMesh : fixture.RoughFloorMesh, {});
    constexpr float64 RADIUS = 0.25;
    const BodyHandle capsule = fixture.Create(MotionType::Dynamic,
                                              fixture.Capsule,
                                              {-2.6, RADIUS - OVERLAP, -1.7},
                                              MakeAxisAngle({0.0, 0.0, 1.0}, math::kHalfPi));
    // 1.3 m/s along x and 0.7 m/s along z: about two centimeters a tick, so no seam is stepped over.
    REQUIRE(fixture.World.TrySetBodyVelocity(capsule, {1.3, 0.0, 0.7}, {}) == Status::Success);
    constexpr uint32 TICK_COUNT = 240;
    constexpr float64 SIDEWAYS = 1.0e-6;
    usize sidewaysNormalCount = 0;
    usize persistedTotal = 0;
    usize featureChangeTotal = 0;
    usize normalChangeTotal = 0;
    for (uint32 tick = 0; tick < TICK_COUNT; ++tick)
    {
        fixture.StepOrFail();
        const Array<ContactPointState> points = fixture.ExportContacts();
        REQUIRE(points.GetSize() >= 2);
        for (const ContactPointState& point : points)
        {
            const float64 up = point.FirstBody == floor ? 1.0 : -1.0;
            const bool isSideways =
                math::Abs(point.NormalWorld.X) > SIDEWAYS || math::Abs(point.NormalWorld.Z) > SIDEWAYS;
            sidewaysNormalCount += isSideways == true ? 1 : 0;
            if (isClassified == true)
            {
                REQUIRE(GetMaxDifference(point.NormalWorld, {0.0, up, 0.0}) <= SIDEWAYS);
                REQUIRE(math::Abs(point.SeparationMeters + OVERLAP) <= SIDEWAYS);
            }
        }
        if (isClassified == true)
        {
            // Exactly the two ends of the capsule, whichever triangles lie under it.
            REQUIRE(points.GetSize() == 2);
            REQUIRE(fixture.World.GetContactDiagnostics().PatchCount == 1);
        }
        const ContactDiagnostics& diagnostics = fixture.World.GetContactDiagnostics();
        persistedTotal += diagnostics.PersistedPointCount;
        featureChangeTotal += diagnostics.GetRetiredPointCount(ContactRetirement::FeatureChanged);
        normalChangeTotal += diagnostics.GetRetiredPointCount(ContactRetirement::NormalChanged);
    }
    // The capsule moved 5.2 m along x and 2.8 m along z.
    REQUIRE(fixture.GetState(capsule).Pose.PositionWorld.X > 2.5);
    if (isClassified == true)
    {
        REQUIRE(sidewaysNormalCount == 0);
        REQUIRE(normalChangeTotal == 0);
        // Points persist while an end stays over one triangle and are re-keyed when it crosses into the next.
        REQUIRE(persistedTotal > TICK_COUNT);
        REQUIRE(featureChangeTotal > 0);
    }
    else
    {
        REQUIRE(sidewaysNormalCount > 0);
    }
}

TEST_CASE("Contact capacity exhaustion reports the table and changes nothing", "[rigid][contact][capacity]")
{
    constexpr uint32 TWO_PAIRS = 2;
    constexpr uint32 TEN_POINTS = 10;
    ContactFixture fixture(TWO_PAIRS, TEN_POINTS);
    fixture.CreateFloorBox();
    const BodyHandle first = fixture.Create(MotionType::Dynamic, fixture.Cube, {-2.0, RESTING_HEIGHT, 0.0});
    REQUIRE(fixture.World.TrySetBodyVelocity(first, {0.25, 0.0, 0.0}, {0.0, 0.1, 0.0}) == Status::Success);
    fixture.StepOrFail();
    REQUIRE(fixture.World.GetContactDiagnostics().PointCount == 4);
    const BodyHandle second = fixture.Create(MotionType::Dynamic, fixture.Cube, {0.0, RESTING_HEIGHT, 0.0});
    const BodyHandle third = fixture.Create(MotionType::Dynamic, fixture.Cube, {2.0, RESTING_HEIGHT, 0.0});
    REQUIRE(fixture.World.TryEnqueueImpulse(
                { .Key = { .TargetTick = 1 }, .Body = first, .LinearImpulseInFrame = {0.0, 0.0, 500.0} }) ==
            Status::Success);

    const auto requireUnchanged = [&](const StaticArray<BodyState, 3>& before,
                                      const Array<ContactPointState>& contactsBefore) {
        REQUIRE(fixture.World.GetNextTick() == 1);
        REQUIRE(fixture.World.GetPhase() == WorldPhase::Idle);
        REQUIRE(fixture.World.GetDiagnostics().PendingCommandCount == 1);
        RequireSameState(fixture.GetState(first), before[0]);
        RequireSameState(fixture.GetState(second), before[1]);
        RequireSameState(fixture.GetState(third), before[2]);
        // The contacts of the last completed tick are still published, and still tick 0's.
        const Array<ContactPointState> contactsAfter = fixture.ExportContacts();
        REQUIRE(contactsAfter.GetSize() == contactsBefore.GetSize());
        for (usize index = 0; index < contactsAfter.GetSize(); ++index)
        {
            REQUIRE(contactsAfter[index].PositionWorld == contactsBefore[index].PositionWorld);
            REQUIRE(contactsAfter[index].PersistedTickCount == contactsBefore[index].PersistedTickCount);
        }
        ContactExportResult exported;
        REQUIRE(fixture.World.TryExportContacts({}, exported) == Status::Success);
        REQUIRE(exported.Tick == 0);
    };
    const StaticArray<BodyState, 3> before{fixture.GetState(first), fixture.GetState(second), fixture.GetState(third)};
    const Array<ContactPointState> contactsBefore = fixture.ExportContacts();

    // Three cubes on the floor are three pairs and twelve points: the pair table is exhausted first.
    const StepResult pairs = fixture.World.Step();
    REQUIRE(pairs.Result == Status::CapacityExceeded);
    REQUIRE(pairs.Phase == WorldPhase::Idle);
    REQUIRE(pairs.ExhaustedTable == ContactTable::Pairs);
    REQUIRE(pairs.RequiredCount == 3);
    REQUIRE(pairs.AppliedCommandCount == 0);
    REQUIRE(fixture.World.GetContactDiagnostics().ExhaustedTable == ContactTable::Pairs);
    REQUIRE(fixture.World.GetContactDiagnostics().RequiredCount == 3);
    REQUIRE(fixture.World.GetContactDiagnostics().PairCount == 3);
    REQUIRE(fixture.World.GetContactDiagnostics().PointCount == 12);
    REQUIRE(fixture.World.GetContactDiagnostics().PairHighWater == 3);
    REQUIRE(fixture.World.GetContactDiagnostics().PointHighWater == 12);
    requireUnchanged(before, contactsBefore);
    // The same tick fails the same way again: nothing was consumed.
    REQUIRE(fixture.World.Step().RequiredCount == 3);
    requireUnchanged(before, contactsBefore);

    SECTION("removing load lets the tick run, with its command applied once and its cache intact")
    {
        REQUIRE(fixture.World.TryDestroyBody(third) == Status::Success);
        const StepResult step = fixture.World.Step();
        REQUIRE(step.Result == Status::Success);
        REQUIRE(step.Tick == 1);
        REQUIRE(step.AppliedCommandCount == 1);
        REQUIRE(step.ExhaustedTable == ContactTable::None);
        const ContactDiagnostics& diagnostics = fixture.World.GetContactDiagnostics();
        REQUIRE(diagnostics.ExhaustedTable == ContactTable::None);
        REQUIRE(diagnostics.RequiredCount == 0);
        REQUIRE(diagnostics.PairCount == 2);
        REQUIRE(diagnostics.PointCount == 8);
        // The first cube's four points were cached at tick 0 and are matched now, after two failed attempts.
        REQUIRE(diagnostics.PersistedPointCount == 4);
        REQUIRE(diagnostics.NewPointCount == 4);
        REQUIRE(diagnostics.PairHighWater == 3);
        // A 1 m cube of water weighs 1000 kg: the impulse added 0.5 m/s along z, once.
        REQUIRE(GetMaxDifference(fixture.GetState(first).LinearVelocityWorld, {0.25, 0.0, 0.5}) <= EXACT_TOLERANCE);
    }
}

TEST_CASE("The point table is checked after the pair table", "[rigid][contact][capacity]")
{
    constexpr uint32 FOUR_PAIRS = 4;
    constexpr uint32 TEN_POINTS = 10;
    ContactFixture fixture(FOUR_PAIRS, TEN_POINTS);
    fixture.CreateFloorBox();
    const BodyHandle first = fixture.Create(MotionType::Dynamic, fixture.Cube, {-2.0, RESTING_HEIGHT, 0.0});
    fixture.Create(MotionType::Dynamic, fixture.Cube, {0.0, RESTING_HEIGHT, 0.0});
    fixture.Create(MotionType::Dynamic, fixture.Cube, {2.0, RESTING_HEIGHT, 0.0});
    const BodyState before = fixture.GetState(first);
    const StepResult step = fixture.World.Step();
    REQUIRE(step.Result == Status::CapacityExceeded);
    REQUIRE(step.ExhaustedTable == ContactTable::Points);
    REQUIRE(step.RequiredCount == 12);
    REQUIRE(fixture.World.GetNextTick() == 0);
    RequireSameState(fixture.GetState(first), before);
    REQUIRE(fixture.ExportContacts().IsEmpty() == true);
    REQUIRE(fixture.World.GetContactDiagnostics().PersistedPointCount == 0);
    REQUIRE(fixture.World.GetContactDiagnostics().NewPointCount == 0);
}

TEST_CASE("Cache reuse and every reachable retirement reason are observable", "[rigid][contact][cache]")
{
    ContactFixture fixture;
    const BodyHandle floor = fixture.CreateFloorBox();
    const Vector3d resting{0.0, RESTING_HEIGHT, 0.0};
    BodyHandle cube = fixture.Create(MotionType::Dynamic, fixture.Cube, resting);
    fixture.StepOrFail();
    fixture.StepOrFail();
    REQUIRE(fixture.World.GetContactDiagnostics().PersistedPointCount == 4);
    const auto requireRetired = [&](ContactRetirement reason, usize retired, usize persisted, usize created) {
        const ContactDiagnostics& diagnostics = fixture.World.GetContactDiagnostics();
        REQUIRE(diagnostics.GetRetiredPointCount(reason) == retired);
        REQUIRE(GetRetiredTotal(diagnostics) == retired);
        REQUIRE(diagnostics.PersistedPointCount == persisted);
        REQUIRE(diagnostics.NewPointCount == created);
        // Every current point is new or persisted; every cached point was persisted or retired.
        REQUIRE(diagnostics.PointCount == persisted + created);
    };

    SECTION("a teleport retires the cache even when the pose is unchanged")
    {
        REQUIRE(fixture.World.TryTeleportBody(cube, { .PositionWorld = resting }) == Status::Success);
        fixture.StepOrFail();
        requireRetired(ContactRetirement::Teleported, 4, 0, 4);
        REQUIRE(fixture.ExportContacts()[0].PersistedTickCount == 0);
        fixture.StepOrFail();
        requireRetired(ContactRetirement::Teleported, 0, 4, 0);
    }
    SECTION("a teleport of the other body retires it too")
    {
        REQUIRE(fixture.World.TryTeleportBody(floor, { .PositionWorld = {0.0, -0.5, 0.0} }) == Status::Success);
        fixture.StepOrFail();
        requireRetired(ContactRetirement::Teleported, 4, 0, 4);
    }
    SECTION("a pair that moves apart retires as separated, and returns as new")
    {
        REQUIRE(fixture.World.TryTeleportBody(cube, { .PositionWorld = {0.0, 3.0, 0.0} }) == Status::Success);
        fixture.StepOrFail();
        requireRetired(ContactRetirement::Separated, 4, 0, 0);
        REQUIRE(fixture.ExportContacts().IsEmpty() == true);
        REQUIRE(fixture.World.TryTeleportBody(cube, { .PositionWorld = resting }) == Status::Success);
        fixture.StepOrFail();
        requireRetired(ContactRetirement::Separated, 0, 0, 4);
    }
    SECTION("a destroyed body's contacts retire, and a new body in its slot is a new pair")
    {
        REQUIRE(fixture.World.TryDestroyBody(cube) == Status::Success);
        cube = fixture.Create(MotionType::Dynamic, fixture.Cube, resting);
        fixture.StepOrFail();
        requireRetired(ContactRetirement::Separated, 4, 0, 4);
    }
    SECTION("a material change retires the cache")
    {
        constexpr uint32 ICE = 7;
        REQUIRE(fixture.World.TrySetColliderMaterial(fixture.GetCollider(cube), ICE) == Status::Success);
        fixture.StepOrFail();
        requireRetired(ContactRetirement::MaterialChanged, 4, 0, 4);
        fixture.StepOrFail();
        requireRetired(ContactRetirement::MaterialChanged, 0, 4, 0);
        REQUIRE(fixture.World.TrySetColliderMaterial({}, ICE) == Status::InvalidHandle);
    }
    SECTION("corners that slide off the floor's edge change features; the others persist")
    {
        // The floor's top reaches x = 4. Two meters a second carries the cube's leading corners over the edge.
        REQUIRE(fixture.World.TryTeleportBody(cube,
                                              {
                                                  .PositionWorld = {3.48, RESTING_HEIGHT, 0.0},
                                              }) == Status::Success);
        REQUIRE(fixture.World.TrySetBodyVelocity(cube, {2.0, 0.0, 0.0}, {}) == Status::Success);
        fixture.StepOrFail(); // Detected at x = 3.48, all four corners on the floor; the cube then moves to 3.513.
        requireRetired(ContactRetirement::Teleported, 4, 0, 4);
        fixture.StepOrFail(); // Detected at x = 3.513: the leading corners are past x = 4.
        requireRetired(ContactRetirement::FeatureChanged, 2, 2, 2);
        fixture.StepOrFail();
        requireRetired(ContactRetirement::FeatureChanged, 0, 4, 0);
    }
    SECTION("the same features under a turned normal do not reuse cached state")
    {
        // A sphere passing another: the features never change, the direction between the centres does.
        fixture.Create(MotionType::Static, fixture.Sphere, {10.0, 5.0, 0.0});
        const BodyHandle passing = fixture.Create(MotionType::Dynamic, fixture.Sphere, {10.0, 5.99, 0.0});
        fixture.StepOrFail();
        fixture.StepOrFail();
        REQUIRE(fixture.World.GetContactDiagnostics().PersistedPointCount == 5);
        // 0.2 m SIDEWAYS in one tick turns the normal by eleven degrees and leaves the spheres within the offset.
        REQUIRE(fixture.World.TrySetBodyVelocity(passing, {0.2 / DEFAULT_TICK_SECONDS, 0.0, 0.0}, {}) ==
                Status::Success);
        fixture.StepOrFail(); // Still detected at the old pose.
        REQUIRE(fixture.World.GetContactDiagnostics().PersistedPointCount == 5);
        fixture.StepOrFail();
        requireRetired(ContactRetirement::NormalChanged, 1, 4, 1);
    }
}

TEST_CASE("Contact settings are validated and a world without contact capacity detects nothing", "[rigid][contact]")
{
    collision::GeometryStore store;
    REQUIRE(store.TryInitialize({ .MaxShapes = SHAPE_CAPACITY }) == collision::Status::Success);
    const RigidWorldSettings valid
    {
        .MaxBodies = BODY_CAPACITY,
        .MaxColliders = COLLIDER_CAPACITY,
        .MaxPendingCommands = TEST_COMMAND_CAPACITY,
        .MaxContactPairs = PAIR_CAPACITY,
        .MaxContactPoints = POINT_CAPACITY,
    };
    const auto initialize = [&](const RigidWorldSettings& settings) {
        RigidWorld world;
        return world.TryInitialize(store, settings);
    };
    RigidWorldSettings settings = valid;
    settings.MaxContactPoints = 0;
    REQUIRE(initialize(settings) == Status::InvalidArgument);
    settings = valid;
    settings.MaxContactPairs = 0;
    REQUIRE(initialize(settings) == Status::InvalidArgument);
    settings = valid;
    settings.MaxContactPairs = MAX_RIGID_WORLD_CONTACT_PAIRS + 1;
    REQUIRE(initialize(settings) == Status::CapacityExceeded);
    settings = valid;
    settings.MaxContactPoints = MAX_RIGID_WORLD_CONTACT_POINTS + 1;
    REQUIRE(initialize(settings) == Status::CapacityExceeded);
    settings = valid;
    settings.ContactOffsetMeters = std::numeric_limits<float64>::infinity();
    REQUIRE(initialize(settings) == Status::NonFiniteInput);
    settings.ContactOffsetMeters = -0.001;
    REQUIRE(initialize(settings) == Status::OutOfRange);
    settings.ContactOffsetMeters = 2.0 * collision::MAX_CONTACT_OFFSET_METERS;
    REQUIRE(initialize(settings) == Status::OutOfRange);

    SECTION("zero capacity turns detection off")
    {
        WorldFixture fixture;
        fixture.CreateDynamic(fixture.Sphere, {});
        fixture.CreateDynamic(fixture.Sphere, {0.5, 0.0, 0.0});
        fixture.StepTicks(2);
        REQUIRE(fixture.World.GetContactDiagnostics().HasDetected == false);
        ContactExportResult exported;
        REQUIRE(fixture.World.TryExportContacts({}, exported) == Status::Success);
        REQUIRE(exported.PointCount == 0);
    }
    SECTION("a wider contact offset reaches further")
    {
        constexpr float64 GAP = 0.05;
        for (const float64 offset : {0.0, collision::DEFAULT_CONTACT_OFFSET_METERS, 0.1})
        {
            collision::GeometryStore offsetStore;
            REQUIRE(offsetStore.TryInitialize({ .MaxShapes = SHAPE_CAPACITY }) == collision::Status::Success);
            collision::ShapeHandle sphere;
            REQUIRE(offsetStore.TryAddPrimitive(collision::MakeSphereShape(0.5), sphere) == collision::Status::Success);
            RigidWorld world;
            settings = valid;
            settings.GravityWorld = {};
            settings.ContactOffsetMeters = offset;
            REQUIRE(world.TryInitialize(offsetStore, settings) == Status::Success);
            const BodyColliderDescription collider{ .Shape = sphere };
            BodyHandle body;
            REQUIRE(world.TryCreateBody({ .Colliders = BorrowedView<const BodyColliderDescription>(&collider, 1) },
                                        body) == Status::Success);
            REQUIRE(world.TryCreateBody(
                        {
                            .Pose = { .PositionWorld = {1.0 + GAP, 0.0, 0.0} },
                            .Colliders = BorrowedView<const BodyColliderDescription>(&collider, 1),
                        },
                        body) == Status::Success);
            REQUIRE(world.Step().Result == Status::Success);
            REQUIRE(world.GetContactDiagnostics().PointCount == (offset >= GAP ? 1U : 0U));
        }
    }
    SECTION("the export validates its buffer and reports completion")
    {
        RigidWorld uninitialized;
        ContactExportResult exported;
        exported.PointCount = 9;
        REQUIRE(uninitialized.TryExportContacts({}, exported) == Status::InvalidPhase);
        ContactFixture fixture;
        fixture.CreateFloorBox();
        fixture.Create(MotionType::Dynamic, fixture.Cube, {0.0, RESTING_HEIGHT, 0.0});
        fixture.StepOrFail();
        REQUIRE(fixture.World.TryExportContacts(BorrowedView<ContactPointState>(nullptr, 3), exported) ==
                Status::InvalidArgument);
        REQUIRE(exported.PointCount == 9);
        StaticArray<ContactPointState, 3> buffer{};
        REQUIRE(fixture.World.TryExportContacts(BorrowedView<ContactPointState>(buffer.GetData(), buffer.GetSize()),
                                                exported) == Status::Success);
        REQUIRE(exported.PointCount == 4);
        REQUIRE(exported.WrittenCount == 3);
        REQUIRE(exported.IsComplete == false);
    }
}
