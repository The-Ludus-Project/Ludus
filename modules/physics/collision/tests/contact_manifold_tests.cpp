// Contact topology and reduction: the face-to-edge transition against an independent clip, patch reduction of
// many-cornered faces, and the candidate search against an all-pairs oracle.

#include "contact_test_support.h"

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
#include <ludus/physics/collision/geometry_store.h>
#include <ludus/physics/collision/status.h>

#include <catch2/catch_test_macros.hpp>

#include <limits>

using namespace ludus::physics::collision;
using namespace ludus::physics::collision::test;

namespace
{
constexpr float64 GEOMETRY_TOLERANCE = 1.0e-6;
constexpr Vector3d FAR_TRANSLATION{-16384.25, 512.5, 8192.125};
constexpr usize CUBE_BOTTOM_CORNERS = 4;
constexpr usize MAX_ORACLE_VERTICES = 16;
constexpr uint32 FLOOR_TOP_FACE = 3;
constexpr uint32 FLOOR_TOP_POSITIVE_X_EDGE = 11;
/// The cube's bottom corner that crosses the floor's side plane: +X and +Z in the cube's own axes.
constexpr uint32 CROSSING_CORNER = 5;

/// A point in the floor's top plane.
struct PlanePoint final
{
    float64 X = 0.0;
    float64 Z = 0.0;
};

/// A convex polygon in the floor's top plane.
struct PlanePolygon final
{
    StaticArray<PlanePoint, MAX_ORACLE_VERTICES> Vertices{};
    usize Count = 0;
};

/// Keep the part of a polygon where `sign * coordinate <= limit`, for the x axis or the z axis: one half-plane of the
/// independent clip. No tolerance is applied.
PlanePolygon ClipHalfPlane(const PlanePolygon& polygon, bool isAlongX, float64 sign, float64 limit)
{
    PlanePolygon clipped;
    for (usize index = 0; index < polygon.Count; ++index)
    {
        const PlanePoint start = polygon.Vertices[index];
        const PlanePoint end = polygon.Vertices[(index + 1) % polygon.Count];
        const float64 startValue = sign * (isAlongX == true ? start.X : start.Z) - limit;
        const float64 endValue = sign * (isAlongX == true ? end.X : end.Z) - limit;
        if (startValue <= 0.0)
        {
            clipped.Vertices[clipped.Count++] = start;
        }
        if ((startValue <= 0.0) != (endValue <= 0.0))
        {
            const float64 fraction = startValue / (startValue - endValue);
            clipped.Vertices[clipped.Count++] = {start.X + (end.X - start.X) * fraction,
                                                 start.Z + (end.Z - start.Z) * fraction};
        }
    }
    return clipped;
}

float64 GetArea(const PlanePolygon& polygon)
{
    float64 doubled = 0.0;
    for (usize index = 0; index < polygon.Count; ++index)
    {
        const PlanePoint start = polygon.Vertices[index];
        const PlanePoint end = polygon.Vertices[(index + 1) % polygon.Count];
        doubled += start.X * end.Z - end.X * start.Z;
    }
    return 0.5 * math::Abs(doubled);
}

/// How far a point lies outside a convex polygon of either winding; zero or negative inside.
float64 GetOutsideDistance(const PlanePolygon& polygon, PlanePoint point)
{
    float64 winding = 0.0;
    for (usize index = 0; index < polygon.Count; ++index)
    {
        const PlanePoint start = polygon.Vertices[index];
        const PlanePoint end = polygon.Vertices[(index + 1) % polygon.Count];
        winding += start.X * end.Z - end.X * start.Z;
    }
    float64 outside = -std::numeric_limits<float64>::max();
    for (usize index = 0; index < polygon.Count; ++index)
    {
        const PlanePoint start = polygon.Vertices[index];
        const PlanePoint end = polygon.Vertices[(index + 1) % polygon.Count];
        const float64 edgeX = end.X - start.X;
        const float64 edgeZ = end.Z - start.Z;
        const float64 length = math::Sqrt(edgeX * edgeX + edgeZ * edgeZ);
        if (length <= 0.0)
        {
            continue;
        }
        const float64 side = (edgeX * (point.Z - start.Z) - edgeZ * (point.X - start.X)) / length;
        outside = math::Max(outside, winding >= 0.0 ? -side : side);
    }
    return outside;
}

/// The contact points of one patch as a convex polygon, ordered around their centroid.
PlanePolygon ToPolygon(const ContactPairContacts& contacts, Vector3d translation)
{
    PlanePolygon polygon;
    PlanePoint centroid;
    for (uint32 index = 0; index < contacts.PointCount; ++index)
    {
        const Vector3d position = contacts.Points[index].PositionWorld - translation;
        polygon.Vertices[polygon.Count++] = {position.X, position.Z};
        centroid.X += position.X / static_cast<float64>(contacts.PointCount);
        centroid.Z += position.Z / static_cast<float64>(contacts.PointCount);
    }
    for (usize first = 0; first < polygon.Count; ++first)
    {
        for (usize second = first + 1; second < polygon.Count; ++second)
        {
            const float64 firstAngle =
                math::Atan2(polygon.Vertices[first].Z - centroid.Z, polygon.Vertices[first].X - centroid.X);
            const float64 secondAngle =
                math::Atan2(polygon.Vertices[second].Z - centroid.Z, polygon.Vertices[second].X - centroid.X);
            if (secondAngle < firstAngle)
            {
                const PlanePoint saved = polygon.Vertices[first];
                polygon.Vertices[first] = polygon.Vertices[second];
                polygon.Vertices[second] = saved;
            }
        }
    }
    return polygon;
}

/// Rotation columns of a quaternion by the float64 conversion the module documents.
struct Axes final
{
    Vector3d X{};
    Vector3d Y{};
    Vector3d Z{};
};

Axes ToAxes(math::Quaternion orientation)
{
    const float64 x = static_cast<float64>(orientation.X);
    const float64 y = static_cast<float64>(orientation.Y);
    const float64 z = static_cast<float64>(orientation.Z);
    const float64 w = static_cast<float64>(orientation.W);
    const float64 scale = 2.0 / (x * x + y * y + z * z + w * w);
    return
    {
        .X = {1.0 - (y * y + z * z) * scale, (x * y + w * z) * scale, (x * z - w * y) * scale},
        .Y = {(x * y - w * z) * scale, 1.0 - (x * x + z * z) * scale, (y * z + w * x) * scale},
        .Z = {(x * z + w * y) * scale, (y * z - w * x) * scale, 1.0 - (x * x + y * y) * scale},
    };
}

bool HasFeatures(const ContactPairContacts& contacts, ContactFeature first, ContactFeature second)
{
    for (uint32 index = 0; index < contacts.PointCount; ++index)
    {
        if (contacts.Points[index].FirstFeature == first && contacts.Points[index].SecondFeature == second)
        {
            return true;
        }
    }
    return false;
}

bool HasSecondFeature(const ContactPairContacts& contacts, ContactFeature second)
{
    for (uint32 index = 0; index < contacts.PointCount; ++index)
    {
        if (contacts.Points[index].SecondFeature == second)
        {
            return true;
        }
    }
    return false;
}

PosedSpec At(const ShapeSpec& shape, Vector3d position, math::Quaternion orientation = {})
{
    return { .Shape = shape, .Pose = { .PositionWorld = position, .OrientationWorld = orientation } };
}
} // namespace

TEST_CASE("A corner crossing a reference side plane changes one feature and nothing else",
          "[collision][contact][manifold]")
{
    // A unit cube, turned thirty degrees about y, overlaps a floor box by one centimeter. One of its bottom corners
    // sits `crossing` meters beyond the floor's +x side plane. The contact patch must match the cube's bottom face
    // clipped to the floor's top face by a clip written here, in the floor's own plane and without tolerances.
    constexpr float64 OVERLAP = 0.01;
    constexpr float64 CUBE_HALF = 0.5;
    constexpr float32 THIRTY_DEGREES = math::kPiF / 6.0f;
    const math::Quaternion turn = MakeTurn({0, 1, 0}, THIRTY_DEGREES);
    const Axes axes = ToAxes(turn);
    const Vector3d crossingOffset = (axes.X - axes.Y + axes.Z) * CUBE_HALF;
    const ShapeSpec floorBox = MakeBoxSpec({1.0, 0.5, 1.0});
    const ShapeSpec cubeBox = MakeBoxSpec({CUBE_HALF, CUBE_HALF, CUBE_HALF});
    const ShapeSpec floorHull = MakeBoxHullSpec({1.0f, 0.5f, 1.0f});
    const ShapeSpec cubeHull = MakeBoxHullSpec({0.5f, 0.5f, 0.5f});
    const ContactFeature topFace = MakeContactFeature(ContactFeatureKind::Face, FLOOR_TOP_FACE);
    const ContactFeature sideEdge = MakeContactFeature(ContactFeatureKind::Edge, FLOOR_TOP_POSITIVE_X_EDGE);

    for (const float64 crossing : {-1.0e-3, -1.0e-6, 0.0, 1.0e-6, 1.0e-3})
    {
        INFO("the corner is " << crossing << " m beyond the side plane");
        const Vector3d cubePosition{1.0 + crossing - crossingOffset.X, 1.0 - OVERLAP, 0.0};
        const PosedSpec floor = At(floorBox, {});
        const PosedSpec cube = At(cubeBox, cubePosition, turn);

        // The oracle: the cube's bottom corners, in order around the face, clipped by the floor's four side planes.
        PlanePolygon oracle;
        const StaticArray<Vector3d, CUBE_BOTTOM_CORNERS> signs{Vector3d{-1, -1, -1},
                                                               Vector3d{1, -1, -1},
                                                               Vector3d{1, -1, 1},
                                                               Vector3d{-1, -1, 1}};
        for (const Vector3d& sign : signs)
        {
            const Vector3d corner = cubePosition + (axes.X * sign.X + axes.Y * sign.Y + axes.Z * sign.Z) * CUBE_HALF;
            oracle.Vertices[oracle.Count++] = {corner.X, corner.Z};
        }
        for (const bool isAlongX : {true, false})
        {
            for (const float64 sign : {1.0, -1.0})
            {
                oracle = ClipHalfPlane(oracle, isAlongX, sign, 1.0);
            }
        }
        REQUIRE(oracle.Count >= CUBE_BOTTOM_CORNERS);
        const float64 oracleArea = GetArea(oracle);
        // Four points cannot follow a five-cornered outline exactly: the patch may fall short by the sliver that
        // dropping one of the two crossing points removes, which is below two meters times the crossing.
        const float64 areaTolerance = 2.0 * math::Abs(crossing) + GEOMETRY_TOLERANCE;

        const ContactPairContacts contacts = GeneratePair(floor, cube);
        REQUIRE(contacts.PatchCount == 1);
        REQUIRE(contacts.PointCount == MAX_CONTACT_PATCH_POINTS);
        REQUIRE(contacts.ApproximatePatchCount == 0);
        REQUIRE(GetDistance(contacts.Patches[0].NormalWorld, {0.0, 1.0, 0.0}) <= GEOMETRY_TOLERANCE);
        for (uint32 index = 0; index < contacts.PointCount; ++index)
        {
            const ContactPoint& point = contacts.Points[index];
            REQUIRE(math::IsFinite(point.PositionWorld) == true);
            REQUIRE(math::Abs(point.SeparationMeters + OVERLAP) <= GEOMETRY_TOLERANCE);
            REQUIRE(math::Abs(point.PositionWorld.Y - (0.5 - 0.5 * OVERLAP)) <= GEOMETRY_TOLERANCE);
            REQUIRE(GetOutsideDistance(oracle, {point.PositionWorld.X, point.PositionWorld.Z}) <= GEOMETRY_TOLERANCE);
            for (uint32 other = index + 1; other < contacts.PointCount; ++other)
            {
                REQUIRE(GetDistance(point.PositionWorld, contacts.Points[other].PositionWorld) > GEOMETRY_TOLERANCE);
            }
        }
        const float64 patchArea = GetArea(ToPolygon(contacts, {}));
        REQUIRE(patchArea <= oracleArea + GEOMETRY_TOLERANCE);
        REQUIRE(patchArea >= oracleArea - areaTolerance);

        // Features that did not change keep their identity through the whole sweep: no churn for a cache to see.
        for (const uint32 corner : {0U, 1U, 4U})
        {
            REQUIRE(HasFeatures(contacts, topFace, MakeContactFeature(ContactFeatureKind::Vertex, corner)) == true);
        }
        // The intended change: the corner is a face contact up to and on the boundary, and an edge crossing beyond.
        const bool isInside = crossing <= 0.0;
        REQUIRE(HasFeatures(contacts, topFace, MakeContactFeature(ContactFeatureKind::Vertex, CROSSING_CORNER)) ==
                isInside);
        bool hasCrossing = false;
        for (uint32 index = 0; index < contacts.PointCount; ++index)
        {
            hasCrossing = hasCrossing == true ||
                          (contacts.Points[index].FirstFeature == sideEdge &&
                           GetContactFeatureKind(contacts.Points[index].SecondFeature) == ContactFeatureKind::Edge);
        }
        REQUIRE(hasCrossing == (isInside == false));

        // Reversed order and a far shared translation give the same patch.
        RequireMirrored(contacts, GeneratePair(floor, cube, { .IsReversed = true }), GEOMETRY_TOLERANCE);
        const ContactPairContacts moved = GeneratePair(floor, cube, { .TranslationWorld = FAR_TRANSLATION });
        RequireSameGeometry(moved, contacts, GEOMETRY_TOLERANCE, FAR_TRANSLATION);
        for (uint32 index = 0; index < contacts.PointCount; ++index)
        {
            REQUIRE(HasFeatures(moved, contacts.Points[index].FirstFeature, contacts.Points[index].SecondFeature) ==
                    true);
        }

        // The general convex path on the same solids: the same patch and the same corner features.
        for (const bool isFloorHull : {false, true})
        {
            INFO("hull cube against a " << (isFloorHull == true ? "hull" : "box") << " floor");
            const PosedSpec generalFloor = At(isFloorHull == true ? floorHull : floorBox, {});
            const PosedSpec generalCube = At(cubeHull, cubePosition, turn);
            const ContactPairContacts general = GeneratePair(generalFloor, generalCube);
            REQUIRE(general.ApproximatePatchCount == 0);
            RequireSameGeometry(general, contacts, GEOMETRY_TOLERANCE);
            RequireMirrored(general,
                            GeneratePair(generalFloor,
                                         generalCube,
                                         {
                                             .IsReversed = true,
                                         }),
                            GEOMETRY_TOLERANCE);
            RequireSameGeometry(GeneratePair(generalFloor, generalCube, { .TranslationWorld = FAR_TRANSLATION }),
                                general,
                                GEOMETRY_TOLERANCE,
                                FAR_TRANSLATION);
            for (const uint32 corner : {0U, 1U, 4U})
            {
                REQUIRE(HasSecondFeature(general, MakeContactFeature(ContactFeatureKind::Vertex, corner)) == true);
            }
            REQUIRE(HasSecondFeature(general, MakeContactFeature(ContactFeatureKind::Vertex, CROSSING_CORNER)) ==
                    isInside);
        }
    }
}

TEST_CASE("A many-cornered face is reduced to four points that keep its depth and area",
          "[collision][contact][manifold]")
{
    // A twelve-sided prism of radius 1 m standing on a floor box.
    constexpr uint32 SIDE_COUNT = 12;
    constexpr float32 HALF_HEIGHT = 0.25f;
    Array<Vector3> points;
    for (uint32 side = 0; side < SIDE_COUNT; ++side)
    {
        const float32 angle = 2.0f * math::kPiF * static_cast<float32>(side) / static_cast<float32>(SIDE_COUNT);
        points.Add({math::Cos(angle), -HALF_HEIGHT, math::Sin(angle)});
        points.Add({math::Cos(angle), HALF_HEIGHT, math::Sin(angle)});
    }
    const ShapeSpec prism = MakeHullSpec(points);
    const ShapeSpec floor = MakeBoxSpec({2.0, 0.5, 2.0});
    SECTION("resting flat")
    {
        const ContactPairContacts contacts = GeneratePair(At(floor, {}), At(prism, {0.0, 0.74, 0.0}));
        REQUIRE(contacts.PatchCount == 1);
        REQUIRE(contacts.GeneratedPointCount == SIDE_COUNT);
        REQUIRE(contacts.PointCount == MAX_CONTACT_PATCH_POINTS);
        for (uint32 index = 0; index < contacts.PointCount; ++index)
        {
            REQUIRE(math::Abs(contacts.Points[index].SeparationMeters + 0.01) <= GEOMETRY_TOLERANCE);
            REQUIRE(GetContactFeatureKind(contacts.Points[index].SecondFeature) == ContactFeatureKind::Vertex);
        }
        // The largest quadrilateral in a regular twelve-sided polygon of radius 1 is a square of area 2.
        REQUIRE(GetArea(ToPolygon(contacts, {})) >= 2.0 - GEOMETRY_TOLERANCE);
    }
    SECTION("tilted, the deepest corner is kept")
    {
        // Tilted about z, the rim point at -x dips lowest.
        constexpr float32 TILT = 0.01f;
        const ContactPairContacts contacts =
            GeneratePair(At(floor, {}), At(prism, {0.0, 0.75, 0.0}, MakeTurn({0, 0, 1}, TILT)));
        REQUIRE(contacts.PatchCount == 1);
        REQUIRE(contacts.PointCount == MAX_CONTACT_PATCH_POINTS);
        const float64 lowest =
            0.75 - static_cast<float64>(math::Sin(TILT)) - static_cast<float64>(HALF_HEIGHT * math::Cos(TILT));
        float64 deepest = 0.0;
        for (uint32 index = 0; index < contacts.PointCount; ++index)
        {
            deepest = math::Min(deepest, contacts.Points[index].SeparationMeters);
        }
        REQUIRE(math::Abs(deepest - (lowest - 0.5)) <= GEOMETRY_TOLERANCE);
        REQUIRE(GetArea(ToPolygon(contacts, {})) >= 1.5);
    }
}

TEST_CASE("The candidate search agrees with an all-pairs oracle", "[collision][contact][candidates]")
{
    constexpr uint32 COLLIDER_COUNT = 96;
    constexpr float32 EXTENT = 2.5f;
    constexpr float64 MARGIN = 0.05;
    constexpr uint32 CATEGORY_COUNT = 3;
    GeometryStore store;
    CollisionWorld world;
    REQUIRE(store.TryInitialize({ .MaxShapes = COLLIDER_COUNT }) == Status::Success);
    REQUIRE(world.TryInitialize(store, { .MaxColliders = COLLIDER_COUNT }) == Status::Success);
    math::RandomStream random;
    REQUIRE(random.TryReseed(20270, 0) == true);
    Array<ColliderHandle> colliders;
    Array<ColliderDescription> descriptions;
    Array<float64> radii;
    for (uint32 index = 0; index < COLLIDER_COUNT; ++index)
    {
        const float64 radius = static_cast<float64>(NextInRange(random, 0.2f, 0.9f));
        ShapeHandle shape;
        REQUIRE(store.TryAddPrimitive(MakeSphereShape(radius), shape) == Status::Success);
        const uint32 category =
            static_cast<uint32>(NextInRange(random, 0.0f, static_cast<float32>(CATEGORY_COUNT))) % CATEGORY_COUNT;
        const uint32 rejected =
            static_cast<uint32>(NextInRange(random, 0.0f, static_cast<float32>(CATEGORY_COUNT + 1)));
        const bool isSensor = random.NextFloat01() < 0.1f;
        const ColliderDescription description
        {
            .Shape = shape,
            .Pose = { .PositionWorld = Widen(NextPoint(random, EXTENT)) },
            .Participation = { .IsSolid = isSensor == false, .IsSensor = isSensor },
            // Each collider rejects at most one category; some reject none.
            .Filter =
            {
                .CategoryBits = 1U << category,
                .CollisionMask = rejected < CATEGORY_COUNT ? ~(1U << rejected) : ALL_COLLISION_CATEGORIES,
            },
            .IsEnabled = random.NextFloat01() >= 0.1f,
        };
        ColliderHandle collider;
        REQUIRE(world.TryCreateCollider(description, collider) == Status::Success);
        colliders.Add(collider);
        descriptions.Add(description);
        radii.Add(radius);
    }
    // Move a third of the colliders so the hierarchy has been updated, not only built.
    for (uint32 index = 0; index < COLLIDER_COUNT; index += 3)
    {
        descriptions[index].Pose.PositionWorld = Widen(NextPoint(random, EXTENT));
        REQUIRE(world.TrySetColliderPose(colliders[index], descriptions[index].Pose) == Status::Success);
    }
    CollisionReadView view;
    REQUIRE(world.TryAcquireReadView(view) == Status::Success);
    usize pairCount = 0;
    StaticArray<ColliderHandle, COLLIDER_COUNT> found{};
    StaticArray<ColliderHandle, COLLIDER_COUNT> swept{};
    for (uint32 subject = 0; subject < COLLIDER_COUNT; ++subject)
    {
        // The oracle: every other collider, by the documented rule, with a sphere's exact bounds.
        Array<uint32> expected;
        for (uint32 other = 0; other < COLLIDER_COUNT; ++other)
        {
            const ColliderDescription& first = descriptions[subject];
            const ColliderDescription& second = descriptions[other];
            const Vector3d gap = second.Pose.PositionWorld - first.Pose.PositionWorld;
            const float64 reach = radii[subject] + radii[other] + MARGIN;
            if (other != subject && first.IsEnabled == true && second.IsEnabled == true &&
                first.Participation.IsSolid == true && second.Participation.IsSolid == true &&
                ShouldCollide(first.Filter, second.Filter) == true && math::Abs(gap.X) <= reach &&
                math::Abs(gap.Y) <= reach && math::Abs(gap.Z) <= reach)
            {
                expected.Add(other);
            }
        }
        ContactCandidateResult hierarchy;
        REQUIRE(view.FindContactCandidates({ .Collider = colliders[subject], .MarginMeters = MARGIN },
                                           BorrowedView<ColliderHandle>(found.GetData(), found.GetSize()),
                                           hierarchy) == Status::Success);
        ContactCandidateResult bruteForce;
        REQUIRE(view.FindContactCandidates(
                    {
                        .Collider = colliders[subject],
                        .MarginMeters = MARGIN,
                        .Traversal = QueryTraversal::AllTriangles,
                    },
                    BorrowedView<ColliderHandle>(swept.GetData(), swept.GetSize()),
                    bruteForce) == Status::Success);
        REQUIRE(hierarchy.IsComplete == true);
        REQUIRE(hierarchy.CandidateCount == expected.GetSize());
        REQUIRE(bruteForce.CandidateCount == expected.GetSize());
        for (usize index = 0; index < expected.GetSize(); ++index)
        {
            REQUIRE(found[index] == colliders[expected[index]]);
            REQUIRE(swept[index] == colliders[expected[index]]);
        }
        pairCount += expected.GetSize();
    }
    // The scene is dense enough to exercise the search and sparse enough to prune.
    REQUIRE(pairCount > COLLIDER_COUNT);
    REQUIRE(pairCount < static_cast<usize>(COLLIDER_COUNT) * COLLIDER_COUNT / 4);
}

TEST_CASE("The candidate search reports completion and validates its inputs", "[collision][contact][candidates]")
{
    ContactScene scene;
    const ShapeSpec sphere = MakeSphereSpec(0.5);
    const ColliderHandle subject = scene.AddCollider(At(sphere, {}));
    const ColliderHandle near = scene.AddCollider(At(sphere, {0.9, 0.0, 0.0}));
    const ColliderHandle reached = scene.AddCollider(At(sphere, {0.0, 1.04, 0.0}));
    const ColliderHandle far = scene.AddCollider(At(sphere, {0.0, 0.0, 3.0}));
    StaticArray<ColliderHandle, 4> buffer{};
    ContactCandidateResult result;
    result.CandidateCount = 9;
    CollisionReadView inactive;
    REQUIRE(inactive.FindContactCandidates({ .Collider = subject },
                                           BorrowedView<ColliderHandle>(buffer.GetData(), buffer.GetSize()),
                                           result) == Status::InvalidPhase);
    {
        CollisionReadView view;
        REQUIRE(scene.World.TryAcquireReadView(view) == Status::Success);
        const BorrowedView<ColliderHandle> all(buffer.GetData(), buffer.GetSize());
        REQUIRE(view.FindContactCandidates({}, all, result) == Status::InvalidHandle);
        REQUIRE(view.FindContactCandidates({ .Collider = subject, .MarginMeters = -1.0 }, all, result) ==
                Status::OutOfRange);
        REQUIRE(view.FindContactCandidates({ .Collider = subject, .Traversal = static_cast<QueryTraversal>(9) },
                                           all,
                                           result) == Status::InvalidArgument);
        REQUIRE(result.CandidateCount == 9);
        // Bounds 0.04 m apart are candidates with a 0.05 m MARGIN and not with none.
        REQUIRE(view.FindContactCandidates({ .Collider = subject, .MarginMeters = 0.05 }, all, result) ==
                Status::Success);
        REQUIRE(result.CandidateCount == 2);
        REQUIRE(result.IsComplete == true);
        REQUIRE(buffer[0] == near);
        REQUIRE(buffer[1] == reached);
        REQUIRE(view.FindContactCandidates(
                    {
                        .Collider = subject,
                        .MarginMeters = 0.0,
                    },
                    all,
                    result) == Status::Success);
        REQUIRE(result.CandidateCount == 1);
        // A buffer that is too small reports how many candidates there are.
        REQUIRE(view.FindContactCandidates({ .Collider = subject, .MarginMeters = 0.05 },
                                           BorrowedView<ColliderHandle>(buffer.GetData(), 1),
                                           result) == Status::Success);
        REQUIRE(result.CandidateCount == 2);
        REQUIRE(result.WrittenCount == 1);
        REQUIRE(result.IsComplete == false);
        REQUIRE(view.FindContactCandidates({ .Collider = far }, all, result) == Status::Success);
        REQUIRE(result.CandidateCount == 0);
    }
    // A disabled collider is no one's candidate and has none.
    REQUIRE(scene.World.TrySetColliderEnabled(near, false) == Status::Success);
    CollisionReadView view;
    REQUIRE(scene.World.TryAcquireReadView(view) == Status::Success);
    const BorrowedView<ColliderHandle> all(buffer.GetData(), buffer.GetSize());
    REQUIRE(view.FindContactCandidates({ .Collider = subject, .MarginMeters = 0.05 }, all, result) == Status::Success);
    REQUIRE(result.CandidateCount == 1);
    REQUIRE(buffer[0] == reached);
    REQUIRE(view.FindContactCandidates({ .Collider = near, .MarginMeters = 0.05 }, all, result) == Status::Success);
    REQUIRE(result.CandidateCount == 0);
}
