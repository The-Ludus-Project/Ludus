// Contacts of convex shapes against cooked triangle meshes: flat floors made of many triangles, interior seams,
// ridges, valleys, boundaries, both sides of a surface and the patch limit.

#include "contact_test_support.h"

#include <ludus/foundation/base/core.h>

#include <ludus/foundation/containers/static_array.hpp>
#include <ludus/foundation/math/quaternion.hpp>
#include <ludus/foundation/math/scalar.hpp>
#include <ludus/foundation/math/vector.hpp>
#include <ludus/physics/collision/contacts.h>
#include <ludus/physics/collision/status.h>
#include <ludus/physics/collision/triangle_mesh.h>

#include <catch2/catch_test_macros.hpp>

using namespace ludus::physics::collision;
using namespace ludus::physics::collision::test;

namespace
{
constexpr float64 EXACT_TOLERANCE = 1.0e-9;
constexpr float64 TURNED_TOLERANCE = 1.0e-6;
constexpr uint32 GRID_CELLS = 4;
constexpr Vector3d DOWN{0.0, -1.0, 0.0};
constexpr float32 QUARTER_TURN = math::kHalfPiF;

PosedSpec At(const ShapeSpec& shape, Vector3d position, math::Quaternion orientation = {})
{
    return { .Shape = shape, .Pose = { .PositionWorld = position, .OrientationWorld = orientation } };
}

/// A ridge or valley along z: two quads meeting at x = 0, each falling (or rising) by `rise` per meter away from
/// it, 2 m long and 2 m wide on each side. Faces +y.
MeshData MakeRoof(float32 rise)
{
    MeshData mesh;
    mesh.Positions.Add({-2.0f, 2.0f * rise, -1.0f});
    mesh.Positions.Add({-2.0f, 2.0f * rise, 1.0f});
    mesh.Positions.Add({0.0f, 0.0f, -1.0f});
    mesh.Positions.Add({0.0f, 0.0f, 1.0f});
    mesh.Positions.Add({2.0f, 2.0f * rise, -1.0f});
    mesh.Positions.Add({2.0f, 2.0f * rise, 1.0f});
    const StaticArray<uint32, 2 * QUAD_INDEX_COUNT> indices{0, 1, 3, 0, 3, 2, 2, 3, 5, 2, 5, 4};
    for (const uint32 index : indices)
    {
        mesh.Indices.Add(index);
    }
    return mesh;
}

void RequireAllNormals(const ContactPairContacts& contacts, Vector3d normal, float64 tolerance)
{
    REQUIRE(contacts.PatchCount > 0);
    for (uint32 patch = 0; patch < contacts.PatchCount; ++patch)
    {
        REQUIRE(GetDistance(contacts.Patches[patch].NormalWorld, normal) <= tolerance);
    }
}
} // namespace

TEST_CASE("A sphere on a flat floor of many triangles makes one point with the face normal",
          "[collision][contact][mesh]")
{
    const MeshData grid = MakeGrid(GRID_CELLS);
    const ShapeSpec floor = MakeMeshSpec(grid);
    const ShapeSpec sphere = MakeSphereSpec(0.5);
    // Over a triangle's interior, over the diagonal seam, over a cell edge and over an interior grid vertex.
    for (const Vector3d footprint :
         {Vector3d{1.3, 0.0, 1.6}, Vector3d{1.5, 0.0, 1.5}, Vector3d{2.0, 0.0, 1.4}, Vector3d{2.0, 0.0, 2.0}})
    {
        for (const float64 height : {0.49, 0.5, 0.51})
        {
            INFO("sphere at " << footprint.X << ", " << height << ", " << footprint.Z);
            const PosedSpec ball = At(sphere, {footprint.X, height, footprint.Z});
            const ContactPairContacts contacts = GeneratePair(ball, At(floor, {}));
            REQUIRE(contacts.PatchCount == 1);
            REQUIRE(contacts.PointCount == 1);
            REQUIRE(GetDistance(contacts.Patches[0].NormalWorld, DOWN) <= EXACT_TOLERANCE);
            REQUIRE(math::Abs(contacts.Points[0].SeparationMeters - (height - 0.5)) <= EXACT_TOLERANCE);
            REQUIRE(GetDistance(contacts.Points[0].PositionWorld, {footprint.X, 0.5 * (height - 0.5), footprint.Z}) <=
                    EXACT_TOLERANCE);
            REQUIRE(contacts.Points[0].FirstFeature == MakeContactFeature(ContactFeatureKind::Vertex, 0));
            REQUIRE(contacts.Points[0].SecondFeature == MakeContactFeature(ContactFeatureKind::Face, 0));
            REQUIRE(contacts.Points[0].TriangleIndex < 2 * GRID_CELLS * GRID_CELLS);
            REQUIRE(contacts.TriangleTestCount >= 1);
            RequireMirrored(contacts, GeneratePair(ball, At(floor, {}), { .IsReversed = true }), EXACT_TOLERANCE);
        }
    }
}

TEST_CASE("An interior seam cannot push sideways, and without classification it does", "[collision][contact][mesh]")
{
    // A sphere sunk one centimeter into the floor, five centimeters from a cell edge. Its neighbour triangle's
    // nearest point is on that edge, 0.4925 m from the centre and 5.8 degrees off vertical.
    const MeshData grid = MakeGrid(GRID_CELLS);
    const ShapeSpec sphere = MakeSphereSpec(0.5);
    const PosedSpec ball = At(sphere, {1.95, 0.49, 1.3});
    const ContactPairContacts classified = GeneratePair(ball, At(MakeMeshSpec(grid), {}));
    REQUIRE(classified.PatchCount == 1);
    REQUIRE(classified.PointCount == 1);
    RequireAllNormals(classified, DOWN, EXACT_TOLERANCE);
    REQUIRE(math::Abs(classified.Points[0].SeparationMeters + 0.01) <= EXACT_TOLERANCE);

    const ContactPairContacts conservative =
        GeneratePair(ball, At(MakeMeshSpec(grid, { .EdgePolicy = InternalEdgePolicy::KeepAll }), {}));
    bool hasSidewaysNormal = false;
    for (uint32 patch = 0; patch < conservative.PatchCount; ++patch)
    {
        hasSidewaysNormal = hasSidewaysNormal == true || math::Abs(conservative.Patches[patch].NormalWorld.X) > 0.05;
    }
    REQUIRE(hasSidewaysNormal == true);
}

TEST_CASE("A box on a floor of many triangles keeps a four-point patch", "[collision][contact][mesh]")
{
    const MeshData grid = MakeGrid(GRID_CELLS);
    const ShapeSpec floor = MakeMeshSpec(grid);
    const ShapeSpec box = MakeBoxSpec({0.75, 0.25, 0.6});
    // The box spans several cells; its bottom is one centimeter below the floor.
    const PosedSpec crate = At(box, {2.1, 0.24, 1.9});
    const ContactPairContacts contacts = GeneratePair(crate, At(floor, {}));
    REQUIRE(contacts.PatchCount == 1);
    REQUIRE(contacts.PointCount == MAX_CONTACT_PATCH_POINTS);
    REQUIRE(contacts.GeneratedPointCount > MAX_CONTACT_PATCH_POINTS);
    RequireAllNormals(contacts, DOWN, EXACT_TOLERANCE);
    for (const Vector3d corner : {Vector3d{1.35, -0.005, 1.3},
                                  Vector3d{2.85, -0.005, 1.3},
                                  Vector3d{1.35, -0.005, 2.5},
                                  Vector3d{2.85, -0.005, 2.5}})
    {
        const ContactPoint& point = contacts.Points[FindPointOrFail(contacts, corner, EXACT_TOLERANCE)];
        REQUIRE(math::Abs(point.SeparationMeters + 0.01) <= EXACT_TOLERANCE);
        REQUIRE(GetContactFeatureKind(point.FirstFeature) == ContactFeatureKind::Vertex);
        REQUIRE(point.SecondFeature == MakeContactFeature(ContactFeatureKind::Face, 0));
        REQUIRE(point.TriangleIndex != INVALID_TRIANGLE_INDEX);
    }
    RequireMirrored(contacts, GeneratePair(crate, At(floor, {}), { .IsReversed = true }), EXACT_TOLERANCE);

    SECTION("a hull of the same box finds the same patch")
    {
        const ContactPairContacts general =
            GeneratePair(At(MakeBoxHullSpec({0.75f, 0.25f, 0.6f}), {2.1, 0.24, 1.9}), At(floor, {}));
        REQUIRE(general.ApproximatePatchCount == 0);
        RequireSameGeometry(general, contacts, TURNED_TOLERANCE);
    }
    SECTION("a turned floor and box give the turned patch")
    {
        // Turn the whole scene a quarter about x: +y becomes +z and +z becomes -y.
        const math::Quaternion turn = MakeTurn({1, 0, 0}, QUARTER_TURN);
        const ContactPairContacts turned = GeneratePair(At(box, {2.1, -1.9, 0.24}, turn), At(floor, {}, turn));
        REQUIRE(turned.PatchCount == 1);
        REQUIRE(turned.PointCount == MAX_CONTACT_PATCH_POINTS);
        RequireAllNormals(turned, {0.0, 0.0, -1.0}, TURNED_TOLERANCE);
        for (uint32 index = 0; index < contacts.PointCount; ++index)
        {
            const Vector3d position = contacts.Points[index].PositionWorld;
            REQUIRE(math::Abs(
                        turned.Points[FindPointOrFail(turned, {position.X, -position.Z, position.Y}, TURNED_TOLERANCE)]
                            .SeparationMeters +
                        0.01) <= TURNED_TOLERANCE);
        }
    }
}

TEST_CASE("A capsule lying across cell edges keeps its two ends", "[collision][contact][mesh]")
{
    const MeshData grid = MakeGrid(GRID_CELLS);
    const ShapeSpec capsule = MakeCapsuleSpec(1.0, 0.25);
    // Lying along x from 1.1 to 3.1, across two cell edges and two diagonals.
    const ContactPairContacts contacts =
        GeneratePair(At(capsule, {2.1, 0.24, 1.7}, MakeTurn({0, 0, 1}, QUARTER_TURN)), At(MakeMeshSpec(grid), {}));
    REQUIRE(contacts.PatchCount == 1);
    REQUIRE(contacts.PointCount == 2);
    RequireAllNormals(contacts, DOWN, TURNED_TOLERANCE);
    for (const float64 end : {1.1, 3.1})
    {
        const ContactPoint& point = contacts.Points[FindPointOrFail(contacts, {end, -0.005, 1.7}, TURNED_TOLERANCE)];
        REQUIRE(math::Abs(point.SeparationMeters + 0.01) <= TURNED_TOLERANCE);
        REQUIRE(GetContactFeatureKind(point.FirstFeature) == ContactFeatureKind::Vertex);
    }
}

TEST_CASE("Ridges and boundaries keep their own normals; a valley contacts its two faces", "[collision][contact][mesh]")
{
    const ShapeSpec sphere = MakeSphereSpec(0.5);
    SECTION("a sphere on a ridge is pushed along the line from the ridge to its centre")
    {
        const MeshData ridge = MakeRoof(-1.0f);
        const ContactPairContacts contacts = GeneratePair(At(sphere, {0.0, 0.45, 0.0}), At(MakeMeshSpec(ridge), {}));
        REQUIRE(contacts.PatchCount == 1);
        REQUIRE(contacts.PointCount == 1);
        RequireAllNormals(contacts, DOWN, EXACT_TOLERANCE);
        REQUIRE(math::Abs(contacts.Points[0].SeparationMeters + 0.05) <= EXACT_TOLERANCE);
        REQUIRE(GetContactFeatureKind(contacts.Points[0].SecondFeature) == ContactFeatureKind::Edge);
    }
    SECTION("a sphere beside a ridge is pushed diagonally by the ridge edge")
    {
        // A steep roof, falling two meters per meter: beside the ridge and level with it, the ridge is nearer than
        // either slope.
        const MeshData ridge = MakeRoof(-2.0f);
        const ContactPairContacts contacts = GeneratePair(At(sphere, {0.3, 0.3, 0.0}), At(MakeMeshSpec(ridge), {}));
        const float64 diagonal = math::Sqrt(0.5);
        REQUIRE(contacts.PatchCount == 1);
        REQUIRE(contacts.PointCount == 1);
        RequireAllNormals(contacts, {-diagonal, -diagonal, 0.0}, EXACT_TOLERANCE);
        REQUIRE(math::Abs(contacts.Points[0].SeparationMeters - (math::Sqrt(0.18) - 0.5)) <= EXACT_TOLERANCE);
    }
    SECTION("a sphere past a floor's border is pushed by the border edge")
    {
        const MeshData grid = MakeGrid(GRID_CELLS);
        // 0.3 m outside the border x = 0 and 0.3 m above the floor plane.
        const ContactPairContacts contacts = GeneratePair(At(sphere, {-0.3, 0.3, 1.5}), At(MakeMeshSpec(grid), {}));
        const float64 diagonal = math::Sqrt(0.5);
        REQUIRE(contacts.PatchCount == 1);
        REQUIRE(contacts.PointCount == 1);
        RequireAllNormals(contacts, {diagonal, -diagonal, 0.0}, EXACT_TOLERANCE);
    }
    SECTION("a sphere in a valley touches both slopes and not the seam")
    {
        const MeshData valley = MakeRoof(1.0f);
        // The slopes rise at 45 degrees; a centre at height h is h / sqrt(2) from each.
        const float64 height = 0.69;
        const ContactPairContacts contacts = GeneratePair(At(sphere, {0.0, height, 0.0}), At(MakeMeshSpec(valley), {}));
        const float64 diagonal = math::Sqrt(0.5);
        REQUIRE(contacts.PatchCount == 2);
        REQUIRE(contacts.PointCount == 2);
        for (uint32 patch = 0; patch < contacts.PatchCount; ++patch)
        {
            const Vector3d normal = contacts.Patches[patch].NormalWorld;
            REQUIRE(math::Abs(math::Abs(normal.X) - diagonal) <= TURNED_TOLERANCE);
            REQUIRE(math::Abs(normal.Y + diagonal) <= TURNED_TOLERANCE);
            REQUIRE(math::Abs(contacts.Points[patch].SeparationMeters - (height * diagonal - 0.5)) <= TURNED_TOLERANCE);
            REQUIRE(contacts.Points[patch].SecondFeature == MakeContactFeature(ContactFeatureKind::Face, 0));
        }
        REQUIRE(contacts.Patches[0].NormalWorld.X * contacts.Patches[1].NormalWorld.X < 0.0);
    }
}

TEST_CASE("A shape under a surface contacts its back side", "[collision][contact][mesh]")
{
    const MeshData grid = MakeGrid(GRID_CELLS);
    const ContactPairContacts contacts =
        GeneratePair(At(MakeSphereSpec(0.5), {1.5, -0.49, 1.6}), At(MakeMeshSpec(grid), {}));
    REQUIRE(contacts.PatchCount == 1);
    REQUIRE(contacts.PointCount == 1);
    RequireAllNormals(contacts, {0.0, 1.0, 0.0}, EXACT_TOLERANCE);
    REQUIRE(math::Abs(contacts.Points[0].SeparationMeters + 0.01) <= EXACT_TOLERANCE);
}

TEST_CASE("A pair with more normal groups than patches keeps the deepest and counts the rest",
          "[collision][contact][mesh]")
{
    // A twelve-sided pit: slopes from a rim of radius 2 m at height 2 m down to an apex at the origin. A sphere on
    // the axis touches all twelve slopes, whose normals are thirty degrees apart.
    constexpr uint32 SIDE_COUNT = 12;
    constexpr float32 RIM_RADIUS = 2.0f;
    constexpr float32 RIM_HEIGHT = 2.0f;
    MeshData pit;
    pit.Positions.Add({0.0f, 0.0f, 0.0f});
    for (uint32 side = 0; side < SIDE_COUNT; ++side)
    {
        const float32 angle = 2.0f * math::kPiF * static_cast<float32>(side) / static_cast<float32>(SIDE_COUNT);
        pit.Positions.Add({RIM_RADIUS * math::Cos(angle), RIM_HEIGHT, RIM_RADIUS * math::Sin(angle)});
    }
    for (uint32 side = 0; side < SIDE_COUNT; ++side)
    {
        pit.Indices.Add(0);
        pit.Indices.Add(1 + (side + 1) % SIDE_COUNT);
        pit.Indices.Add(1 + side);
    }
    // Each slope's plane passes through the apex; the middle of its rim chord is the apothem from the axis.
    const float64 apothem = static_cast<float64>(RIM_RADIUS) * math::Cos(math::kPi / static_cast<float64>(SIDE_COUNT));
    const float64 centerHeight = 1.0;
    const float64 slopeDistance =
        centerHeight * apothem / math::Sqrt(apothem * apothem + static_cast<float64>(RIM_HEIGHT * RIM_HEIGHT));
    const ContactPairContacts contacts =
        GeneratePair(At(MakeSphereSpec(slopeDistance + 0.005), {0.0, centerHeight, 0.0}), At(MakeMeshSpec(pit), {}));
    REQUIRE(contacts.TriangleTestCount == SIDE_COUNT);
    REQUIRE(contacts.GeneratedPointCount == SIDE_COUNT);
    REQUIRE(contacts.PatchCount == MAX_CONTACT_PAIR_PATCHES);
    REQUIRE(contacts.PointCount == MAX_CONTACT_PAIR_PATCHES);
    REQUIRE(contacts.DiscardedPatchCount == SIDE_COUNT - MAX_CONTACT_PAIR_PATCHES);
    for (uint32 index = 0; index < contacts.PointCount; ++index)
    {
        REQUIRE(math::Abs(contacts.Points[index].SeparationMeters + 0.005) <= TURNED_TOLERANCE);
        REQUIRE(contacts.Patches[index].NormalWorld.Y < 0.0);
    }
}
