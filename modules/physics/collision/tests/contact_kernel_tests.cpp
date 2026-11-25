// Analytic contact points, normals and depths for every kernel pair: spheres, capsules, boxes and hulls, with
// reversed pairs, tangent, edge and deep-overlap cases, and a shared world translation.

#include "contact_test_support.h"

#include <ludus/foundation/base/core.h>

#include <ludus/foundation/containers/static_array.hpp>
#include <ludus/foundation/math/quaternion.hpp>
#include <ludus/foundation/math/scalar.hpp>
#include <ludus/foundation/math/vector.hpp>
#include <ludus/physics/collision/collision_world.h>
#include <ludus/physics/collision/contacts.h>
#include <ludus/physics/collision/status.h>

#include <catch2/catch_test_macros.hpp>

#include <limits>

using namespace ludus::physics::collision;
using namespace ludus::physics::collision::test;

namespace
{
/// Closed-form kernels on axis-aligned shapes answer to rounding.
constexpr float64 EXACT_TOLERANCE = 1.0e-9;
/// Poses with a float32 orientation carry about 1e-7 of rotation error.
constexpr float64 TURNED_TOLERANCE = 1.0e-6;
/// Exactly representable, so translated poses keep their relative positions bit for bit.
constexpr Vector3d FAR_TRANSLATION{8192.5, -4096.25, 2048.125};
constexpr float32 QUARTER_TURN = math::kHalfPiF;
constexpr float32 EIGHTH_TURN = 0.5f * math::kHalfPiF;

ContactFeature Vertex(uint32 index)
{
    return MakeContactFeature(ContactFeatureKind::Vertex, index);
}

ContactFeature Edge(uint32 index)
{
    return MakeContactFeature(ContactFeatureKind::Edge, index);
}

ContactFeature Face(uint32 index)
{
    return MakeContactFeature(ContactFeatureKind::Face, index);
}

/// Generate a pair's contacts, and require that the reversed pair mirrors them and that a far translation of both
/// shapes moves the points without changing them.
ContactPairContacts GenerateChecked(const PosedSpec& first,
                                    const PosedSpec& second,
                                    float64 tolerance = EXACT_TOLERANCE,
                                    float64 contactOffset = DEFAULT_CONTACT_OFFSET_METERS)
{
    const ContactPairContacts contacts = GeneratePair(first, second, { .ContactOffsetMeters = contactOffset });
    RequireMirrored(contacts,
                    GeneratePair(first, second, { .ContactOffsetMeters = contactOffset, .IsReversed = true }),
                    tolerance);
    RequireSameGeometry(
        GeneratePair(first, second, { .ContactOffsetMeters = contactOffset, .TranslationWorld = FAR_TRANSLATION }),
        contacts,
        tolerance,
        FAR_TRANSLATION);
    REQUIRE(contacts.ApproximatePatchCount == 0);
    REQUIRE(contacts.DiscardedPatchCount == 0);
    return contacts;
}

/// What one contact point is expected to be.
struct ExpectedPoint final
{
    Vector3d Position{};
    float64 Separation = 0.0;
    ContactFeature FirstFeature = 0;
    ContactFeature SecondFeature = 0;
};

void RequirePoint(const ContactPairContacts& contacts, const ExpectedPoint& expected, float64 tolerance)
{
    const ContactPoint& point = contacts.Points[FindPointOrFail(contacts, expected.Position, tolerance)];
    REQUIRE(math::Abs(point.SeparationMeters - expected.Separation) <= tolerance);
    REQUIRE(point.FirstFeature == expected.FirstFeature);
    REQUIRE(point.SecondFeature == expected.SecondFeature);
    REQUIRE(point.TriangleIndex == INVALID_TRIANGLE_INDEX);
}

/// What the single patch of a pair is expected to be.
struct ExpectedPatch final
{
    Vector3d Normal{};
    uint32 PointCount = 0;
};

void RequireSinglePatch(const ContactPairContacts& contacts, const ExpectedPatch& expected, float64 tolerance)
{
    REQUIRE(contacts.PatchCount == 1);
    REQUIRE(contacts.PointCount == expected.PointCount);
    REQUIRE(contacts.Patches[0].FirstPoint == 0);
    REQUIRE(contacts.Patches[0].PointCount == expected.PointCount);
    REQUIRE(contacts.Patches[0].IsApproximate == false);
    REQUIRE(GetDistance(contacts.Patches[0].NormalWorld, expected.Normal) <= tolerance);
    REQUIRE(math::Abs(math::Length(contacts.Patches[0].NormalWorld) - 1.0) <= tolerance);
}

PosedSpec At(const ShapeSpec& shape, Vector3d position, math::Quaternion orientation = {})
{
    return { .Shape = shape, .Pose = { .PositionWorld = position, .OrientationWorld = orientation } };
}

const Vector3d FLOOR_BOX_HALF_EXTENTS{1.0, 0.5, 1.0};
constexpr float64 CUBE_HALF_EXTENT = 0.5;
constexpr uint32 BOX_TOP_FACE = 3;
} // namespace

TEST_CASE("Two spheres touch along their line of centres", "[collision][contact][kernel]")
{
    const ShapeSpec large = MakeSphereSpec(0.5);
    const ShapeSpec small = MakeSphereSpec(0.25);
    SECTION("overlapping")
    {
        const ContactPairContacts contacts = GenerateChecked(At(large, {}), At(small, {0.7, 0.0, 0.0}));
        RequireSinglePatch(contacts, {{1.0, 0.0, 0.0}, 1}, EXACT_TOLERANCE);
        RequirePoint(contacts, {{0.475, 0.0, 0.0}, -0.05, Vertex(0), Vertex(0)}, EXACT_TOLERANCE);
        REQUIRE(contacts.GeneratedPointCount == 1);
        REQUIRE(contacts.TriangleTestCount == 0);
    }
    SECTION("apart but within the contact offset")
    {
        const ContactPairContacts contacts = GenerateChecked(At(large, {}), At(small, {0.0, 0.76, 0.0}));
        RequireSinglePatch(contacts, {{0.0, 1.0, 0.0}, 1}, EXACT_TOLERANCE);
        RequirePoint(contacts, {{0.0, 0.505, 0.0}, 0.01, Vertex(0), Vertex(0)}, EXACT_TOLERANCE);
    }
    SECTION("tangent")
    {
        const ContactPairContacts contacts = GenerateChecked(At(large, {}), At(small, {0.0, 0.0, -0.75}));
        RequireSinglePatch(contacts, {{0.0, 0.0, -1.0}, 1}, EXACT_TOLERANCE);
        RequirePoint(contacts, {{0.0, 0.0, -0.5}, 0.0, Vertex(0), Vertex(0)}, EXACT_TOLERANCE);
    }
    SECTION("beyond the contact offset")
    {
        const ContactPairContacts contacts = GenerateChecked(At(large, {}), At(small, {0.8, 0.0, 0.0}));
        REQUIRE(contacts.PatchCount == 0);
        REQUIRE(contacts.PointCount == 0);
    }
    SECTION("a zero offset reports touching and overlapping pairs only")
    {
        REQUIRE(GenerateChecked(At(large, {}), At(small, {0.76, 0.0, 0.0}), EXACT_TOLERANCE, 0.0).PointCount == 0);
        REQUIRE(GenerateChecked(At(large, {}), At(small, {0.74, 0.0, 0.0}), EXACT_TOLERANCE, 0.0).PointCount == 1);
    }
    SECTION("concentric spheres still report their full depth along a unit normal")
    {
        const ContactPairContacts contacts = GeneratePair(At(large, {}), At(small, {}));
        REQUIRE(contacts.PointCount == 1);
        REQUIRE(math::Abs(math::Length(contacts.Patches[0].NormalWorld) - 1.0) <= EXACT_TOLERANCE);
        REQUIRE(math::Abs(contacts.Points[0].SeparationMeters + 0.75) <= EXACT_TOLERANCE);
    }
}

TEST_CASE("A sphere touches a capsule on its side and on its cap", "[collision][contact][kernel]")
{
    const ShapeSpec capsule = MakeCapsuleSpec(1.0, 0.25);
    const ShapeSpec sphere = MakeSphereSpec(0.5);
    SECTION("side")
    {
        const ContactPairContacts contacts = GenerateChecked(At(capsule, {}), At(sphere, {0.7, 0.3, 0.0}));
        RequireSinglePatch(contacts, {{1.0, 0.0, 0.0}, 1}, EXACT_TOLERANCE);
        RequirePoint(contacts, {{0.225, 0.3, 0.0}, -0.05, Edge(0), Vertex(0)}, EXACT_TOLERANCE);
    }
    SECTION("cap")
    {
        const ContactPairContacts contacts = GenerateChecked(At(capsule, {}), At(sphere, {0.0, 1.6, 0.0}));
        RequireSinglePatch(contacts, {{0.0, 1.0, 0.0}, 1}, EXACT_TOLERANCE);
        RequirePoint(contacts, {{0.0, 1.175, 0.0}, -0.15, Vertex(1), Vertex(0)}, EXACT_TOLERANCE);
    }
    SECTION("diagonally off the lower cap")
    {
        const ContactPairContacts contacts = GenerateChecked(At(capsule, {}), At(sphere, {0.4, -1.3, 0.0}));
        // The lower ball centre is at y = -1; the centres are 0.5 m apart along (0.8, -0.6, 0).
        RequireSinglePatch(contacts, {{0.8, -0.6, 0.0}, 1}, EXACT_TOLERANCE);
        RequirePoint(contacts, {{0.1, -1.075, 0.0}, -0.25, Vertex(0), Vertex(0)}, EXACT_TOLERANCE);
    }
}

TEST_CASE("Two capsules touch at one point, or at two when they are parallel", "[collision][contact][kernel]")
{
    const ShapeSpec capsule = MakeCapsuleSpec(1.0, 0.25);
    SECTION("parallel capsules share the covered part of their segments")
    {
        const ContactPairContacts contacts = GenerateChecked(At(capsule, {}), At(capsule, {0.45, 0.5, 0.0}));
        RequireSinglePatch(contacts, {{1.0, 0.0, 0.0}, 2}, EXACT_TOLERANCE);
        RequirePoint(contacts, {{0.225, -0.5, 0.0}, -0.05, Edge(0), Vertex(0)}, EXACT_TOLERANCE);
        RequirePoint(contacts, {{0.225, 1.0, 0.0}, -0.05, Vertex(1), Edge(0)}, EXACT_TOLERANCE);
    }
    SECTION("crossing capsules touch where their segments are closest")
    {
        const ContactPairContacts contacts =
            GenerateChecked(At(capsule, {}),
                            At(capsule, {0.0, 0.0, 0.4}, MakeTurn({0, 0, 1}, QUARTER_TURN)),
                            TURNED_TOLERANCE);
        RequireSinglePatch(contacts, {{0.0, 0.0, 1.0}, 1}, TURNED_TOLERANCE);
        RequirePoint(contacts, {{0.0, 0.0, 0.2}, -0.1, Edge(0), Edge(0)}, TURNED_TOLERANCE);
    }
    SECTION("capsules whose segments cross separate along the common perpendicular")
    {
        const ContactPairContacts contacts =
            GeneratePair(At(capsule, {}), At(capsule, {}, MakeTurn({0, 0, 1}, QUARTER_TURN)));
        REQUIRE(contacts.PointCount == 1);
        REQUIRE(math::Abs(math::Abs(contacts.Patches[0].NormalWorld.Z) - 1.0) <= TURNED_TOLERANCE);
        REQUIRE(math::Abs(contacts.Points[0].SeparationMeters + 0.5) <= TURNED_TOLERANCE);
    }
    SECTION("end to end")
    {
        const ContactPairContacts contacts = GenerateChecked(At(capsule, {}), At(capsule, {0.0, 2.4, 0.0}));
        RequireSinglePatch(contacts, {{0.0, 1.0, 0.0}, 1}, EXACT_TOLERANCE);
        RequirePoint(contacts, {{0.0, 1.2, 0.0}, -0.1, Vertex(1), Vertex(0)}, EXACT_TOLERANCE);
    }
}

TEST_CASE("A sphere touches a box on a face, an edge and a corner, and from inside", "[collision][contact][kernel]")
{
    const ShapeSpec sphere = MakeSphereSpec(0.5);
    const ShapeSpec box = MakeBoxSpec({1.0, 0.5, 0.25});
    SECTION("face")
    {
        const ContactPairContacts contacts = GenerateChecked(At(sphere, {1.4, 0.0, 0.0}), At(box, {}));
        RequireSinglePatch(contacts, {{-1.0, 0.0, 0.0}, 1}, EXACT_TOLERANCE);
        RequirePoint(contacts, {{0.95, 0.0, 0.0}, -0.1, Vertex(0), Face(1)}, EXACT_TOLERANCE);
    }
    SECTION("edge")
    {
        const ContactPairContacts contacts = GenerateChecked(At(sphere, {1.3, 0.8, 0.0}), At(box, {}));
        const float64 distance = math::Sqrt(0.18);
        const float64 diagonal = math::Sqrt(0.5);
        RequireSinglePatch(contacts, {{-diagonal, -diagonal, 0.0}, 1}, EXACT_TOLERANCE);
        const float64 surface = 0.5 * diagonal; // The sphere's surface point lies its radius along the normal.
        RequirePoint(
            contacts,
            {{0.5 * (1.3 - surface + 1.0), 0.5 * (0.8 - surface + 0.5), 0.0}, distance - 0.5, Vertex(0), Edge(11)},
            EXACT_TOLERANCE);
    }
    SECTION("corner")
    {
        const ContactPairContacts contacts = GenerateChecked(At(sphere, {1.25, 0.75, 0.5}), At(box, {}));
        const float64 distance = math::Sqrt(0.1875);
        REQUIRE(contacts.PointCount == 1);
        REQUIRE(math::Abs(contacts.Points[0].SeparationMeters - (distance - 0.5)) <= EXACT_TOLERANCE);
        REQUIRE(contacts.Points[0].FirstFeature == Vertex(0));
        REQUIRE(contacts.Points[0].SecondFeature == Vertex(7));
    }
    SECTION("a centre inside the box leaves through the nearest face")
    {
        const ContactPairContacts contacts = GenerateChecked(At(sphere, {0.9, 0.0, 0.0}), At(box, {}));
        RequireSinglePatch(contacts, {{-1.0, 0.0, 0.0}, 1}, EXACT_TOLERANCE);
        RequirePoint(contacts, {{0.7, 0.0, 0.0}, -0.6, Vertex(0), Face(1)}, EXACT_TOLERANCE);
    }
    SECTION("a turned box")
    {
        // Turned a quarter about z, the box's +x face looks along +y and is 1 m from its centre.
        const ContactPairContacts contacts = GenerateChecked(At(sphere, {0.0, 1.4, 0.0}),
                                                             At(box, {}, MakeTurn({0, 0, 1}, QUARTER_TURN)),
                                                             TURNED_TOLERANCE);
        RequireSinglePatch(contacts, {{0.0, -1.0, 0.0}, 1}, TURNED_TOLERANCE);
        RequirePoint(contacts, {{0.0, 0.95, 0.0}, -0.1, Vertex(0), Face(1)}, TURNED_TOLERANCE);
    }
}

TEST_CASE("A capsule rests on a box with one or two points", "[collision][contact][kernel]")
{
    const ShapeSpec box = MakeBoxSpec(FLOOR_BOX_HALF_EXTENTS);
    const ShapeSpec capsule = MakeCapsuleSpec(0.5, 0.25);
    // Turned a quarter about z, the capsule lies along x with its +Y end at -x.
    const math::Quaternion lying = MakeTurn({0, 0, 1}, QUARTER_TURN);
    SECTION("lying flat")
    {
        const ContactPairContacts contacts =
            GenerateChecked(At(box, {}), At(capsule, {0.0, 0.7, 0.0}, lying), TURNED_TOLERANCE);
        RequireSinglePatch(contacts, {{0.0, 1.0, 0.0}, 2}, TURNED_TOLERANCE);
        RequirePoint(contacts, {{0.5, 0.475, 0.0}, -0.05, Face(BOX_TOP_FACE), Vertex(0)}, TURNED_TOLERANCE);
        RequirePoint(contacts, {{-0.5, 0.475, 0.0}, -0.05, Face(BOX_TOP_FACE), Vertex(1)}, TURNED_TOLERANCE);
    }
    SECTION("standing")
    {
        const ContactPairContacts contacts = GenerateChecked(At(box, {}), At(capsule, {0.0, 1.2, 0.0}));
        RequireSinglePatch(contacts, {{0.0, 1.0, 0.0}, 1}, EXACT_TOLERANCE);
        RequirePoint(contacts, {{0.0, 0.475, 0.0}, -0.05, Face(BOX_TOP_FACE), Vertex(0)}, EXACT_TOLERANCE);
    }
    SECTION("hanging over the box's edge keeps the part above the face")
    {
        const ContactPairContacts contacts =
            GenerateChecked(At(box, {}), At(capsule, {0.8, 0.7, 0.0}, lying), TURNED_TOLERANCE);
        RequireSinglePatch(contacts, {{0.0, 1.0, 0.0}, 2}, TURNED_TOLERANCE);
        RequirePoint(contacts, {{0.3, 0.475, 0.0}, -0.05, Face(BOX_TOP_FACE), Vertex(1)}, TURNED_TOLERANCE);
        RequirePoint(contacts, {{1.0, 0.475, 0.0}, -0.05, Edge(11), Edge(0)}, TURNED_TOLERANCE);
    }
    SECTION("along the box's edge")
    {
        // Lying along z beside the top +x edge, 0.15 m out and 0.15 m up.
        const ContactPairContacts contacts =
            GenerateChecked(At(box, {}),
                            At(capsule, {1.15, 0.65, 0.0}, MakeTurn({1, 0, 0}, QUARTER_TURN)),
                            TURNED_TOLERANCE);
        const float64 diagonal = math::Sqrt(0.5);
        const float64 separation = math::Sqrt(0.045) - 0.25;
        RequireSinglePatch(contacts, {{diagonal, diagonal, 0.0}, 2}, TURNED_TOLERANCE);
        // Midway between the edge and the capsule's surface, which lies its radius back along the normal.
        const float64 out = 0.5 * (1.0 + 1.15 - 0.25 * diagonal);
        const float64 up = 0.5 * (0.5 + 0.65 - 0.25 * diagonal);
        REQUIRE(math::Abs(contacts.Points[0].SeparationMeters - separation) <= TURNED_TOLERANCE);
        REQUIRE(contacts.Points[FindPointOrFail(contacts, {out, up, 0.5}, TURNED_TOLERANCE)].FirstFeature == Edge(11));
        REQUIRE(contacts.Points[FindPointOrFail(contacts, {out, up, -0.5}, TURNED_TOLERANCE)].FirstFeature == Edge(11));
    }
    SECTION("a segment inside the box leaves through the nearest face")
    {
        const ContactPairContacts contacts =
            GenerateChecked(At(box, {}), At(capsule, {0.0, 0.3, 0.0}, lying), TURNED_TOLERANCE);
        RequireSinglePatch(contacts, {{0.0, 1.0, 0.0}, 2}, TURNED_TOLERANCE);
        RequirePoint(contacts, {{0.5, 0.275, 0.0}, -0.45, Face(BOX_TOP_FACE), Vertex(0)}, TURNED_TOLERANCE);
        RequirePoint(contacts, {{-0.5, 0.275, 0.0}, -0.45, Face(BOX_TOP_FACE), Vertex(1)}, TURNED_TOLERANCE);
    }
}

TEST_CASE("Two boxes touch with a face patch, an edge crossing or a corner", "[collision][contact][kernel]")
{
    const ShapeSpec floor = MakeBoxSpec(FLOOR_BOX_HALF_EXTENTS);
    const ShapeSpec cube = MakeBoxSpec({CUBE_HALF_EXTENT, CUBE_HALF_EXTENT, CUBE_HALF_EXTENT});
    SECTION("a cube resting on a face keeps its four corners")
    {
        const ContactPairContacts contacts = GenerateChecked(At(floor, {}), At(cube, {0.0, 0.99, 0.0}));
        RequireSinglePatch(contacts, {{0.0, 1.0, 0.0}, 4}, EXACT_TOLERANCE);
        RequirePoint(contacts, {{-0.5, 0.495, -0.5}, -0.01, Face(BOX_TOP_FACE), Vertex(0)}, EXACT_TOLERANCE);
        RequirePoint(contacts, {{0.5, 0.495, -0.5}, -0.01, Face(BOX_TOP_FACE), Vertex(1)}, EXACT_TOLERANCE);
        RequirePoint(contacts, {{-0.5, 0.495, 0.5}, -0.01, Face(BOX_TOP_FACE), Vertex(4)}, EXACT_TOLERANCE);
        RequirePoint(contacts, {{0.5, 0.495, 0.5}, -0.01, Face(BOX_TOP_FACE), Vertex(5)}, EXACT_TOLERANCE);
        REQUIRE(contacts.GeneratedPointCount == 4);
    }
    SECTION("apart within the contact offset, and beyond it")
    {
        const ContactPairContacts near = GenerateChecked(At(floor, {}), At(cube, {0.0, 1.01, 0.0}));
        RequireSinglePatch(near, {{0.0, 1.0, 0.0}, 4}, EXACT_TOLERANCE);
        RequirePoint(near, {{0.5, 0.505, 0.5}, 0.01, Face(BOX_TOP_FACE), Vertex(5)}, EXACT_TOLERANCE);
        REQUIRE(GenerateChecked(At(floor, {}), At(cube, {0.0, 1.05, 0.0})).PointCount == 0);
    }
    SECTION("a turned cube keeps its turned corners")
    {
        const ContactPairContacts contacts =
            GenerateChecked(At(floor, {}),
                            At(cube, {0.0, 0.99, 0.0}, MakeTurn({0, 1, 0}, EIGHTH_TURN)),
                            TURNED_TOLERANCE);
        const float64 reach = math::Sqrt(0.5);
        RequireSinglePatch(contacts, {{0.0, 1.0, 0.0}, 4}, TURNED_TOLERANCE);
        for (const Vector3d corner : {Vector3d{reach, 0.495, 0.0},
                                      Vector3d{-reach, 0.495, 0.0},
                                      Vector3d{0.0, 0.495, reach},
                                      Vector3d{0.0, 0.495, -reach}})
        {
            const ContactPoint& point = contacts.Points[FindPointOrFail(contacts, corner, TURNED_TOLERANCE)];
            REQUIRE(math::Abs(point.SeparationMeters + 0.01) <= TURNED_TOLERANCE);
            REQUIRE(point.FirstFeature == Face(BOX_TOP_FACE));
            REQUIRE(GetContactFeatureKind(point.SecondFeature) == ContactFeatureKind::Vertex);
        }
    }
    SECTION("a cube hanging over the edge is clipped to the face under it")
    {
        const ContactPairContacts contacts = GenerateChecked(At(floor, {}), At(cube, {0.8, 0.99, 0.0}));
        RequireSinglePatch(contacts, {{0.0, 1.0, 0.0}, 4}, EXACT_TOLERANCE);
        RequirePoint(contacts, {{0.3, 0.495, -0.5}, -0.01, Face(BOX_TOP_FACE), Vertex(0)}, EXACT_TOLERANCE);
        RequirePoint(contacts, {{0.3, 0.495, 0.5}, -0.01, Face(BOX_TOP_FACE), Vertex(4)}, EXACT_TOLERANCE);
        // The floor's top +x edge cuts the cube's two bottom edges along x.
        RequirePoint(contacts, {{1.0, 0.495, -0.5}, -0.01, Edge(11), Edge(0)}, EXACT_TOLERANCE);
        RequirePoint(contacts, {{1.0, 0.495, 0.5}, -0.01, Edge(11), Edge(2)}, EXACT_TOLERANCE);
    }
    SECTION("crossed edges make one point")
    {
        // One cube turned an eighth about z has an edge along z on top; the other, turned about x, an edge along x
        // underneath. Each edge is half a diagonal from its cube's centre.
        const float64 reach = math::Sqrt(0.5);
        const ContactPairContacts contacts =
            GenerateChecked(At(cube, {}, MakeTurn({0, 0, 1}, EIGHTH_TURN)),
                            At(cube, {0.0, 2.0 * reach - 0.01, 0.0}, MakeTurn({1, 0, 0}, EIGHTH_TURN)),
                            TURNED_TOLERANCE);
        RequireSinglePatch(contacts, {{0.0, 1.0, 0.0}, 1}, TURNED_TOLERANCE);
        const ContactPoint& point =
            contacts.Points[FindPointOrFail(contacts, {0.0, reach - 0.005, 0.0}, TURNED_TOLERANCE)];
        REQUIRE(math::Abs(point.SeparationMeters + 0.01) <= TURNED_TOLERANCE);
        REQUIRE(GetContactFeatureKind(point.FirstFeature) == ContactFeatureKind::Edge);
        REQUIRE(GetContactFeatureKind(point.SecondFeature) == ContactFeatureKind::Edge);
    }
    SECTION("a corner on a face makes one point")
    {
        math::Quaternion onCorner;
        const float32 third = 0.57735026f;
        REQUIRE(math::TryRotationBetween({third, third, third}, {0.0f, 1.0f, 0.0f}, onCorner) ==
                math::MathStatus::Success);
        const float64 reach = math::Sqrt(0.75);
        const ContactPairContacts contacts =
            GenerateChecked(At(floor, {}), At(cube, {0.0, 0.5 + reach - 0.01, 0.0}, onCorner), TURNED_TOLERANCE);
        RequireSinglePatch(contacts, {{0.0, 1.0, 0.0}, 1}, TURNED_TOLERANCE);
        RequirePoint(contacts, {{0.0, 0.495, 0.0}, -0.01, Face(BOX_TOP_FACE), Vertex(0)}, TURNED_TOLERANCE);
    }
    SECTION("a deep overlap separates along the shallowest axis")
    {
        const ContactPairContacts contacts = GenerateChecked(At(floor, {}), At(cube, {0.0, 0.6, 0.0}));
        RequireSinglePatch(contacts, {{0.0, 1.0, 0.0}, 4}, EXACT_TOLERANCE);
        RequirePoint(contacts, {{0.5, 0.3, 0.5}, -0.4, Face(BOX_TOP_FACE), Vertex(5)}, EXACT_TOLERANCE);
    }
}

TEST_CASE("The general convex path agrees with the specialized kernels", "[collision][contact][kernel][general]")
{
    // A hull with a box's corners is the same solid as the box, so the general path must find the same contacts.
    const Vector3 floorHalf{1.0f, 0.5f, 1.0f};
    const Vector3 cubeHalf{0.5f, 0.5f, 0.5f};
    const ShapeSpec floorBox = MakeBoxSpec(FLOOR_BOX_HALF_EXTENTS);
    const ShapeSpec cubeBox = MakeBoxSpec({CUBE_HALF_EXTENT, CUBE_HALF_EXTENT, CUBE_HALF_EXTENT});
    const ShapeSpec floorHull = MakeBoxHullSpec(floorHalf);
    const ShapeSpec cubeHull = MakeBoxHullSpec(cubeHalf);
    math::Quaternion onCorner;
    const float32 third = 0.57735026f;
    REQUIRE(math::TryRotationBetween({third, third, third}, {0.0f, 1.0f, 0.0f}, onCorner) == math::MathStatus::Success);
    const float64 edgeReach = math::Sqrt(0.5);
    const float64 cornerReach = math::Sqrt(0.75);

    /// Two posed solids, each available as a box and as a hull.
    struct Case final
    {
        const char* Name;
        Vector3d FirstPosition;
        math::Quaternion FirstTurn;
        bool IsFirstCube;
        Vector3d SecondPosition;
        math::Quaternion SecondTurn;
        uint32 PointCount;
    };
    const StaticArray<Case, 8> cases{
        Case{"resting", {}, {}, false, {0.0, 0.99, 0.0}, {}, 4},
        Case{"apart within the offset", {}, {}, false, {0.0, 1.01, 0.0}, {}, 4},
        Case{"turned", {}, {}, false, {0.0, 0.99, 0.0}, MakeTurn({0, 1, 0}, EIGHTH_TURN), 4},
        Case{"overhang", {}, {}, false, {0.8, 0.99, 0.0}, {}, 4},
        Case{"crossed edges",
             {},
             MakeTurn({0, 0, 1}, EIGHTH_TURN),
             true,
             {0.0, 2.0 * edgeReach - 0.01, 0.0},
             MakeTurn({1, 0, 0}, EIGHTH_TURN),
             1},
        Case{"corner", {}, {}, false, {0.0, 0.5 + cornerReach - 0.01, 0.0}, onCorner, 1},
        Case{"deep", {}, {}, false, {0.0, 0.6, 0.0}, {}, 4},
        Case{"tilted", {}, {}, false, {0.0, 1.0, 0.0}, MakeTurn({0, 0, 1}, 0.01f), 4},
    };
    for (const Case& entry : cases)
    {
        INFO("case: " << entry.Name);
        const ShapeSpec& firstBox = entry.IsFirstCube == true ? cubeBox : floorBox;
        const ShapeSpec& firstHull = entry.IsFirstCube == true ? cubeHull : floorHull;
        const ContactPairContacts expected = GeneratePair(At(firstBox, entry.FirstPosition, entry.FirstTurn),
                                                          At(cubeBox, entry.SecondPosition, entry.SecondTurn));
        REQUIRE(expected.PointCount == entry.PointCount);
        for (const bool isFirstHull : {false, true})
        {
            for (const bool isSecondHull : {false, true})
            {
                if (isFirstHull == false && isSecondHull == false)
                {
                    continue;
                }
                INFO("first is a hull: " << isFirstHull << ", second is a hull: " << isSecondHull);
                const ContactPairContacts contacts = GenerateChecked(
                    At(isFirstHull == true ? firstHull : firstBox, entry.FirstPosition, entry.FirstTurn),
                    At(isSecondHull == true ? cubeHull : cubeBox, entry.SecondPosition, entry.SecondTurn),
                    TURNED_TOLERANCE);
                RequireSameGeometry(contacts, expected, TURNED_TOLERANCE);
            }
        }
    }
}

TEST_CASE("Rounded shapes against a hull match the same shapes against a box", "[collision][contact][kernel][general]")
{
    const ShapeSpec box = MakeBoxSpec({1.0, 0.5, 0.25});
    const ShapeSpec hull = MakeBoxHullSpec({1.0f, 0.5f, 0.25f});
    const ShapeSpec sphere = MakeSphereSpec(0.5);
    const ShapeSpec capsule = MakeCapsuleSpec(0.5, 0.25);
    const math::Quaternion lying = MakeTurn({0, 0, 1}, QUARTER_TURN);
    SECTION("sphere")
    {
        for (const Vector3d center : {Vector3d{1.4, 0.0, 0.0},   // Face.
                                      Vector3d{1.3, 0.8, 0.0},   // Edge.
                                      Vector3d{1.25, 0.75, 0.5}, // Corner.
                                      Vector3d{0.9, 0.0, 0.0},   // Centre inside: expanding-polytope depth.
                                      Vector3d{0.2, 0.45, 0.0}}) // Centre inside, near the top face.
        {
            INFO("sphere centre: " << center.X << ", " << center.Y << ", " << center.Z);
            const ContactPairContacts expected = GeneratePair(At(sphere, center), At(box, {}));
            REQUIRE(expected.PointCount == 1);
            RequireSameGeometry(GenerateChecked(At(sphere, center), At(hull, {}), TURNED_TOLERANCE),
                                expected,
                                TURNED_TOLERANCE);
        }
    }
    SECTION("capsule")
    {
        for (const Vector3d center : {Vector3d{0.0, 0.7, 0.0},  // Lying on the top face.
                                      Vector3d{0.9, 0.7, 0.0},  // Hanging over the edge.
                                      Vector3d{0.0, 0.3, 0.0}}) // Segment inside: expanding-polytope depth.
        {
            INFO("capsule centre: " << center.X << ", " << center.Y << ", " << center.Z);
            const ContactPairContacts expected = GeneratePair(At(box, {}), At(capsule, center, lying));
            REQUIRE(expected.PointCount == 2);
            RequireSameGeometry(GenerateChecked(At(hull, {}), At(capsule, center, lying), TURNED_TOLERANCE),
                                expected,
                                TURNED_TOLERANCE);
        }
    }
}

TEST_CASE("Contact queries validate their inputs and leave outputs unchanged on failure", "[collision][contact]")
{
    ContactScene scene;
    const ColliderHandle first = scene.AddCollider(At(MakeSphereSpec(0.5), {}));
    const ColliderHandle second = scene.AddCollider(At(MakeSphereSpec(0.5), {0.9, 0.0, 0.0}));
    MeshData floor;
    AddFloorQuad(floor);
    const ColliderHandle firstMesh = scene.AddCollider(At(MakeMeshSpec(floor), {}));
    const ColliderHandle secondMesh = scene.AddCollider(At(MakeMeshSpec(floor), {0.0, 1.0, 0.0}));
    ContactPairContacts contacts;
    contacts.PointCount = 7;
    CollisionReadView inactive;
    REQUIRE(inactive.GenerateContacts(first, second, {}, contacts) == Status::InvalidPhase);
    CollisionReadView view;
    REQUIRE(scene.World.TryAcquireReadView(view) == Status::Success);
    REQUIRE(view.GenerateContacts(first, {}, {}, contacts) == Status::InvalidHandle);
    REQUIRE(view.GenerateContacts(first, first, {}, contacts) == Status::InvalidArgument);
    REQUIRE(view.GenerateContacts(first, second, { .ContactOffsetMeters = -0.01 }, contacts) == Status::OutOfRange);
    REQUIRE(view.GenerateContacts(first,
                                  second,
                                  {
                                      .ContactOffsetMeters = 2.0 * MAX_CONTACT_OFFSET_METERS,
                                  },
                                  contacts) == Status::OutOfRange);
    REQUIRE(view.GenerateContacts(first,
                                  second,
                                  { .ContactOffsetMeters = std::numeric_limits<float64>::quiet_NaN() },
                                  contacts) == Status::NonFiniteInput);
    REQUIRE(view.GenerateContacts(firstMesh, secondMesh, {}, contacts) == Status::Unsupported);
    REQUIRE(contacts.PointCount == 7);
    REQUIRE(view.GenerateContacts(first, second, {}, contacts) == Status::Success);
    REQUIRE(contacts.PointCount == 1);
}
